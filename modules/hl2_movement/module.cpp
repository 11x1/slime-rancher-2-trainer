// hl2_movement: Half-Life 2 (Source SDK 2013) player movement for SR2, with
// uncapped bunnyhopping and HL2's accelerated back hopping (jump boost mode
// "Uncapped + ABH" by default; the exact vanilla clamp is selectable).
//
// SR2's player is a Kinematic Character Controller (KCC) setup:
//   KinematicCharacterSystem.FixedUpdate -> KinematicCharacterMotor.UpdatePhase2
//     -> ICharacterController.UpdateVelocity(ref BaseVelocity, dt)
//        = KFCCharacterController.UpdateVelocity, which (confirmed by
//        disassembling 1.3.0, see NOTES.md) does:
//          if motor.GroundingStatus.IsStableOnGround:
//              HandleStablyGroundedVelocity(ref v, dt); HandleStablyGroundedDrag(ref v, dt)
//          else:
//              HandleHorizontalAirborneMovement(ref v, dt, MovementVector)
//              HandleAirborneDrag(ref v, dt, MovementVector)
//              HandleGravity(ref v, dt)
//              ClampAirborneHorizontalVelocity(ref v, dt)
//          foreach active ability: ability.UpdateVelocity(ref v, dt)   (jump, jetpack, ...)
//          UpdateVelocityWithExternalForces(ref v, dt)                (tail call)
//   KCC keeps doing collision, grounding, step handling and slope projection.
//
// What this module does (player only, i.e. `this` is an SRCharacterController):
//   * UpdateVelocity wrapper: per-tick bookkeeping (grounded state, substeps:
//     the KCC tick, 40 Hz in 1.3.0, split into ceil(dt / 0.01) equal steps,
//     wish direction/speed, sprint state, jump-held poll) and post-tick
//     safety (CheckVelocity, optional speed limit, fall-through recovery,
//     jetpack horizontal guard, fast-movement readout).
//   * HandleStablyGroundedVelocity -> Source Friction + Accelerate (WalkMove)
//     in substeps; the result is kept tangent to the ground plane so KCC
//     stays grounded, with the horizontal speed preserved. No friction on a
//     take-off tick or on the landing tick of a queued SR2 jump.
//   * HandleHorizontalAirborneMovement -> Source AirAccelerate (30 u/s cap),
//     still routed through SR2's HandleGroundOnAirborneMovement so the
//     anti-wall-climb projection keeps working.
//   * Ramp physics (setting, default on): on the first grounded tick the last
//     airborne velocity is clipped against the ground plane (Source landing,
//     ramp boost); slope re-tilts by KCC keep the horizontal speed (Source);
//     moving up > 140 u/s leaves the ground (CategorizePosition);
//     KinematicCharacterMotor.HandleVelocityProjection clips with overbounce 1
//     on every airborne hit (surf); SR2's anti-air-climb projection is
//     skipped while touching an unstandable slope.
//   * Jump height setting (metres) at the HL2 gravity in effect (x multiplier).
//   * HandleStablyGroundedDrag / ClampAirborneHorizontalVelocity -> removed.
//   * HandleAirborneDrag -> removed (HL2 gravity) / vertical-only (vanilla
//     gravity or jetpack).
//   * HandleGravity -> sv_gravity (HL2 mode) unless the jetpack is active or
//     gravity is bypassed.
//   * Jetpack: horizontal = Source AirAccelerate only; SR2 keeps the vertical
//     thrust (its UpdateVelocity/AddForce are vertical-only in 1.3.0, and any
//     horizontal change made after our air step is discarded while it runs).
//   * JumpAbilityBehavior: SR2 still decides WHEN a normal jump happens (input,
//     coyote time, sounds, animation, jetpack hand-off). On the take-off tick
//     its velocity change is replaced by HL2's CheckJumpButton (impulse +
//     jump boost per mode). Its hold force is stripped in HL2 mode.
//   * Scroll-wheel jump / autobhop: "synthetic" jumps done here on a grounded
//     tick. Autobhop re-jumps only while its own hold key (default Alt, read
//     with GetAsyncKeyState once per frame, never swallowed) is held. Holding
//     Space never auto-hops: Space jumps are SR2's press-edge jumps.
// Every detour runs its mod logic inside a core DetourGuard; the original is
// always called outside it.
//   * Sprint: HL2 rules on SR2's energy bar (12.5 % of max per second while
//     moving, cannot start below 10 %, stops when empty); SR2's own sprint
//     drain is undone. Jetpack and energy regen stay vanilla.

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <format>
#include <memory>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <vector>

#include "core/detour_guard.h"
#include "core/guard.h"
#include "core/il2cpp_api.h"
#include "core/log.h"
#include "core/main_thread.h"
#include "core/module.h"
#include "core/perf.h"
#include "core/resolver.h"
#include "core/settings.h"
#include "core/unity.h"
#include "modules/hl2_movement/source_movement.h"
#include "ui/momentum_hud.h"
#include "ui/overlay.h"

namespace sr2::modules {
namespace {

namespace mv = sr2::modules::hl2mv;
namespace settings = sr2::core::settings;
namespace unity = sr2::core::unity;
namespace main_thread = sr2::core::main_thread;
using sr2::il2cpp::Resolver;

constexpr const char* kModule = "hl2_movement";
constexpr const char* kImage = "Assembly-CSharp";
constexpr const char* kNsKfc = "MonomiPark.KFC";
constexpr const char* kNsKcc = "KinematicCharacterController";
constexpr const char* kNsCc = "MonomiPark.SlimeRancher.Player.CharacterController";
constexpr const char* kNsAbil = "MonomiPark.SlimeRancher.Player.CharacterController.Abilities";
constexpr const char* kNsInput = "MonomiPark.SlimeRancher.Input";
constexpr const char* kNsUnitProp = "MonomiPark.UnitPropertySystem";
constexpr const char* kNsItems = "MonomiPark.SlimeRancher.Player.PlayerItems";
constexpr const char* kNsGadgetUi = "MonomiPark.SlimeRancher.UI.Gadget";

constexpr float kSubstep = 0.01f;  // longest movement substep (a KCC tick is split into ceil(dt / this))
constexpr int kMaxSubsteps = 10;   // per KCC tick; the rest of a hitch (> 0.1 s) is dropped
constexpr double kCoyoteSeconds = 0.15;
constexpr double kFallRecoverAirSeconds = 3.0;
constexpr float kFallRecoverDrop = 150.0f;  // metres below the last grounded position
constexpr float kTeleportDistance = 25.0f;  // metres in one tick = game teleport, not movement

// IL2CPP reports instance field offsets of value types as if boxed, i.e.
// including the Il2CppObject header (klass + monitor). Runtime ABI constant,
// same reasoning as unity::kArrayDataOffset.
constexpr size_t kObjectHeader = 2 * sizeof(void*);

// MonomiPark.SlimeRancher.Input.InputEventSource (metadata enum constants).
constexpr int32_t kSourceUnknown = 0;
constexpr int32_t kSourcePointer = 1;

// Setting keys.
constexpr const char* kAccel = "accelerate";
constexpr const char* kAirAccel = "airaccelerate";
constexpr const char* kFriction = "friction";
constexpr const char* kStopSpeed = "stopspeed";
constexpr const char* kGravityMode = "gravity_mode";
constexpr const char* kGravity = "gravity";
constexpr const char* kMaxVelocity = "maxvelocity";
constexpr const char* kWalkSpeed = "walk_speed";
constexpr const char* kNormSpeed = "normal_speed";
constexpr const char* kSprintSpeed = "sprint_speed";
constexpr const char* kWalkKey = "walk_key";
constexpr const char* kAutobhop = "autobhop";
constexpr const char* kAutobhopKey = "autobhop_key";
constexpr const char* kScrollJump = "scroll_jump";
constexpr const char* kScrollDir = "scroll_jump_dir";
constexpr const char* kBoostMode = "jump_boost_mode";
constexpr const char* kHl2Sprint = "hl2_sprint";
constexpr const char* kSprintDrain = "sprint_drain";
constexpr const char* kSpeedLimitOn = "speed_limit_on";
constexpr const char* kSpeedLimit = "speed_limit";
constexpr const char* kFallRecovery = "fall_recovery";
constexpr const char* kUnitScale = "unit_scale";
constexpr const char* kMatchVanilla = "match_vanilla_scale";
constexpr const char* kDebugLog = "debug_log";
constexpr const char* kSpeedReadout = "speed_readout";
constexpr const char* kRampPhysics = "ramp_physics";
constexpr const char* kSurfLimit = "surf_slope_limit";
constexpr const char* kJumpHeight = "jump_height";
constexpr const char* kGravityMult = "gravity_multiplier";
constexpr const char* kMomentumHud = "momentum_hud";
constexpr const char* kMomentumDetails = "momentum_hud_details";
constexpr const char* kMomentumX = "momentum_hud_x";        // placed with the HUD edit mode (ui/hud_edit.h)
constexpr const char* kMomentumY = "momentum_hud_y";
constexpr const char* kMomentumSize = "momentum_hud_size";
constexpr const char* kOldMomentumY = "momentum_hud_y_offset";  // before the edit mode (migrated once)
constexpr const char* kOldMomentumScale = "momentum_hud_scale";
constexpr const char* kMomentumFade = "momentum_hud_fade";
constexpr const char* kMomentumBar = "momentum_hud_bar";

// acos(0.7): Source's steepest walkable plane (normal.z >= 0.7).
constexpr float kSourceMaxSlopeDeg = 45.573f;

// ------------------------------------------------------------------ types --

struct V3 {  // UnityEngine.Vector3
    float x, y, z;
};
struct V2 {  // UnityEngine.Vector2
    float x, y;
};
// MonomiPark.SlimeRancher.Input.InputEventData {InputEventSource Source; Vector2 _vecValue;}
// 12 bytes -> passed by reference on x64 (verified against the runtime in Setup).
struct InputEventDataRaw {
    int32_t source;
    float x, y;
};

// Unity (y up, metres) <-> Source (z up, units).
mv::Vec3 ToSrc(V3 u, float invScale) { return {u.x * invScale, u.z * invScale, u.y * invScale}; }
V3 ToUnity(mv::Vec3 s, float scale) { return {s.x * scale, s.z * scale, s.y * scale}; }
mv::Vec3 DirToSrc(V3 u) { return {u.x, u.z, u.y}; }

using UpdVelFn = void (*)(Il2CppObject*, V3*, float, const MethodInfo*);
using UpdVelMoveFn = void (*)(Il2CppObject*, V3*, float, V3, const MethodInfo*);
using VoidFn = void (*)(Il2CppObject*, const MethodInfo*);
using FloatArgFn = void (*)(Il2CppObject*, float, const MethodInfo*);
using InputDataFn = void (*)(Il2CppObject*, InputEventDataRaw, const MethodInfo*);
using SetPositionFn = void (*)(Il2CppObject*, V3, bool, const MethodInfo*);
using GroundOnAirFn = void (*)(Il2CppObject*, V3*, V3*, const MethodInfo*);
// KinematicCharacterMotor.HandleVelocityProjection(ref Vector3 velocity, Vector3 obstructionNormal, bool stableOnHit):
// the Vector3 by value is passed by pointer (1.3.0 disassembly: r8 = &normal, r9b = stableOnHit).
using VelProjFn = void (*)(Il2CppObject*, V3*, const V3*, bool, const MethodInfo*);

template <class Fn>
struct Call {
    Fn fn = nullptr;
    const MethodInfo* mi = nullptr;
};

// ------------------------------------------------------------- resolved --

struct Resolved {
    Il2CppClass* kfc = nullptr;
    Il2CppClass* srcc = nullptr;
    Il2CppClass* motor = nullptr;
    Il2CppClass* jumpB = nullptr;
    Il2CppClass* sprintB = nullptr;
    Il2CppClass* jetpackB = nullptr;
    Il2CppClass* pulseB = nullptr;  // optional; PulseWave never moves the player

    // KFCCharacterController
    size_t kfcParams = 0, kfcMotor = 0, kfcAbilities = 0, kfcLook = 0, kfcMovement = 0, kfcInput = 0,
           kfcBypassGravity = 0;
    // SRCharacterController
    size_t srccEnergy = 0;
    // KinematicCharacterMotor
    size_t motorGrounding = 0, motorBaseVelocity = 0, motorPos = 0;
    // Optional (ramp physics): MaxStableSlopeAngle, _mustUnground, _mustUngroundTimeCounter
    size_t motorMaxSlope = 0, motorMustUnground = 0, motorMustUngroundTime = 0;
    // JumpAbilityData, optional (vanilla jump apex estimate for the log)
    size_t jdAirDamp = 0, jdInitHold = 0, jdHoldDamp = 0, jdStopThr = 0;
    size_t cpGravAccum = 0;  // CharacterControllerParameters._gravityAccumulationSharpness, optional
    // CharacterGroundingReport (offsets inside the struct)
    size_t grFoundAny = 0, grStable = 0, grNormal = 0;
    // AbilityBehavior<T> (resolved on the concrete classes)
    size_t jumpData = 0, jumpActive = 0, sprintData = 0, sprintActive = 0, jetpackActive = 0;
    // AbilityBehavior<T>.StartAbilityEventReceived on the jump: set by a jump
    // press (StartInputEventPerformed), cleared by release and by Start().
    // Optional (0 = unknown): only used to skip friction on a landing tick.
    size_t jumpStartReq = 0;
    // MonomiPark.KFC.AbilityData
    size_t adStart = 0, adAltStart = 0, adStop = 0, adAltStop = 0;
    // InputEvent.PollValue (Func<float>), optional
    size_t iePoll = 0;
    // JumpAbilityData
    size_t jdRecurrence = 0, jdUpSpeed = 0;
    // UnitResource
    size_t urValue = 0;
    // CharacterControllerParameters (optional, logging / auto scale)
    size_t cpMaxGround = 0, cpGravity = 0;
    bool haveParams = false;

    Call<FloatArgFn> forceUnground;
    Call<SetPositionFn> setPosition;
    Call<FloatArgFn> setEnergy;
    Call<GroundOnAirFn> groundOnAir;

    bool inputHooks = false;   // jump/sprint held tracking available
    bool scrollHooks = false;  // wheel slot suppression available
    bool velProjHook = false;  // airborne ClipVelocity in KCC's collision projection (ramp physics)
};
Resolved R;

// Trampolines.
UpdVelFn o_UpdateVelocity = nullptr;
UpdVelFn o_GroundVel = nullptr;
UpdVelFn o_GroundDrag = nullptr;
UpdVelMoveFn o_AirMove = nullptr;
UpdVelMoveFn o_AirDrag = nullptr;
UpdVelFn o_Gravity = nullptr;
UpdVelFn o_AirClamp = nullptr;
UpdVelFn o_JumpUpdVel = nullptr;
VoidFn o_JumpStart = nullptr;
FloatArgFn o_SprintUpdate = nullptr;
InputDataFn o_InputStart = nullptr;
InputDataFn o_InputCancel = nullptr;
VelProjFn o_VelProj = nullptr;

// ---------------------------------------------------------------- config --

struct Cfg {
    mv::MoveVars vars;
    bool hl2Gravity = true;
    float gravity = mv::kSvGravity;
    float walk = mv::kWalkSpeed, normal = mv::kNormSpeed, sprint = mv::kSprintSpeed;
    int walkKey = 0;            // unbound by default (Alt is the autobhop hold key)
    int autobhopKey = VK_MENU;
    bool autobhop = false, scrollJump = false;
    mv::BoostMode boostMode = mv::BoostMode::UncappedAbh;
    int scrollDir = 2;  // 0 up, 1 down, 2 both
    bool hl2Sprint = true;
    float sprintDrain = mv::kSprintDrainPerSec;
    bool speedLimitOn = false;
    float speedLimit = 2000.0f;
    bool fallRecovery = true;
    float unitScale = 0.0254f;
    bool matchVanilla = true;
    bool debugLog = false;
    bool speedReadout = true;
    bool rampPhysics = true;   // Source landing clip, launch, airborne ClipVelocity, surf
    bool surfSource = false;   // surf limit: false = KCC MaxStableSlopeAngle, true = Source 45.57 degrees
    float jumpHeight = 2.0f;   // metres, HL2 gravity mode
    float gravityMult = 1.0f;  // applied to sv_gravity in HL2 gravity mode
    // Momentum HUD (ui/momentum_hud.h).
    bool mhOn = true, mhDetails = false, mhFade = true, mhBar = false;
    float mhX = 50, mhY = 57.1f, mhSize = 1.6f;
};
Cfg g_cfg;  // main thread (and the constructor before hooks exist)
std::atomic<bool> g_cfgDirty{true};
std::atomic<bool> g_active{false};

void LoadCfg() {
    auto f = [](const char* k) { return settings::GetFloat(kModule, k); };
    auto b = [](const char* k) { return settings::GetBool(kModule, k); };
    Cfg c;
    c.vars.accelerate = f(kAccel);
    c.vars.airaccelerate = f(kAirAccel);
    c.vars.friction = f(kFriction);
    c.vars.stopspeed = f(kStopSpeed);
    c.vars.maxvelocity = std::max(1.0f, f(kMaxVelocity));
    c.hl2Gravity = settings::GetInt(kModule, kGravityMode) == 0;
    c.gravity = f(kGravity);
    c.walk = f(kWalkSpeed);
    c.normal = std::max(1.0f, f(kNormSpeed));
    c.sprint = f(kSprintSpeed);
    c.walkKey = settings::GetInt(kModule, kWalkKey);
    c.autobhop = b(kAutobhop);
    c.autobhopKey = settings::GetInt(kModule, kAutobhopKey);
    // Same key bound twice: the autobhop hold key wins, walk is disabled.
    static bool s_keyClashWarned = false;
    if (c.walkKey > 0 && c.walkKey == c.autobhopKey) {
        if (!s_keyClashWarned) {
            SR2_WARN("hl2_movement: walk key and autobhop hold key are both bound to key {} - the autobhop key wins, "
                     "walk is disabled until one of them is changed",
                     c.walkKey);
            s_keyClashWarned = true;
        }
        c.walkKey = 0;
    } else {
        s_keyClashWarned = false;
    }
    c.scrollJump = b(kScrollJump);
    c.scrollDir = settings::GetInt(kModule, kScrollDir);
    c.boostMode = static_cast<mv::BoostMode>(std::clamp(settings::GetInt(kModule, kBoostMode), 0, 3));
    c.hl2Sprint = b(kHl2Sprint);
    c.sprintDrain = f(kSprintDrain);
    c.speedLimitOn = b(kSpeedLimitOn);
    c.speedLimit = f(kSpeedLimit);
    c.fallRecovery = b(kFallRecovery);
    c.unitScale = std::clamp(f(kUnitScale), 0.001f, 1.0f);
    c.matchVanilla = b(kMatchVanilla);
    c.debugLog = b(kDebugLog);
    c.speedReadout = b(kSpeedReadout);
    c.rampPhysics = b(kRampPhysics);
    c.surfSource = settings::GetInt(kModule, kSurfLimit) == 1;
    c.jumpHeight = std::clamp(f(kJumpHeight), 0.1f, 20.0f);
    c.gravityMult = std::clamp(f(kGravityMult), 0.05f, 10.0f);
    c.mhOn = b(kMomentumHud);
    c.mhDetails = b(kMomentumDetails);
    c.mhFade = b(kMomentumFade);
    c.mhBar = b(kMomentumBar);
    c.mhX = std::clamp(f(kMomentumX), 0.0f, 100.0f);
    c.mhY = std::clamp(f(kMomentumY), 0.0f, 100.0f);
    c.mhSize = std::clamp(f(kMomentumSize), 0.5f, 10.0f);
    g_cfg = c;
}

// sv_gravity in effect in HL2 gravity mode (units/s^2).
float Hl2Gravity() { return std::max(0.0f, g_cfg.gravity * g_cfg.gravityMult); }

void ReloadCfgIfDirty() {
    if (g_cfgDirty.exchange(false)) LoadCfg();
}

// ----------------------------------------------------------------- state --
// Everything below is touched only on the Unity main thread (KCC simulation,
// input callbacks, frame callback).

struct PlayerCache {
    Il2CppObject* ctrl = nullptr;
    void* abilities = nullptr;
    Il2CppObject* jump = nullptr;
    Il2CppObject* sprint = nullptr;
    Il2CppObject* jetpack = nullptr;
    Il2CppObject* jumpData = nullptr;
    Il2CppObject* jumpEv[2] = {};
    Il2CppObject* jumpStopEv[2] = {};
    Il2CppObject* sprintEv[2] = {};
    struct Other {
        Il2CppObject* obj;
        size_t activeOff;  // 0 = unknown, never treated as active
    };
    std::vector<Other> others;
    float vanillaMaxGround = 0;  // m/s, from CharacterControllerParameters
};
PlayerCache g_pc;

struct Tick {
    bool active = false;  // inside the player's UpdateVelocity
    bool ours = false;    // HL2 movement applies this tick (no unknown ability active)
    Il2CppObject* self = nullptr;
    Il2CppObject* motor = nullptr;
    bool grounded = false, foundAnyGround = false, jetpack = false, bypassGravity = false;
    V3 groundNormal{0, 1, 0};
    int substeps = 0;
    float subDt = kSubstep;  // dt / substeps
    float scale = 0.0254f, invScale = 1.0f / 0.0254f;
    mv::Vec3 wishdir{}, forwardFlat{1, 0, 0};
    float wishspeed = 0, forwardmove = 0, maxspeed = mv::kNormSpeed;
    bool sprinting = false;
    bool srJumpPending = false;  // SR2's JumpAbilityBehavior.Start ran since the last tick
    uint32_t jumpSerialAtEntry = 0;
    bool jumped = false;  // a take-off was applied this tick
    float takeoffVy = 0;  // Unity vertical velocity set at take-off
    bool jumpQueued = false;  // SR2 jump pressed, Start comes next tick (landing): no friction now
    bool haveMove = false;    // our ground/air step ran; moveX/moveZ = its horizontal result
    float moveX = 0, moveZ = 0;
    bool launched = false;    // ramp physics: left the ground this tick (moving up > NON_JUMP_VELOCITY)
    bool landingClip = false; // ramp physics: this tick's ground speed came from the landing clip
};
Tick g_tick;

// Ramp physics state (main thread). Tick numbers count the player's KCC ticks.
uint64_t g_tickNo = 0;
mv::Vec3 g_preLand{};          // final velocity (Source units) of the last airborne tick
uint64_t g_preLandTick = 0;    // tick number it was stored on (0 = none)
V3 g_lastFinal{};              // final velocity (Unity) of the last grounded tick (no take-off)
uint64_t g_lastFinalTick = 0;
// The player's motor while ramp physics applies this tick (HandleVelocityProjection hook filter).
std::atomic<Il2CppObject*> g_rampMotor{nullptr};
Il2CppObject* g_loggedSlopeMotor = nullptr;
// MaxStableSlopeAngle patch (surf limit "Source").
Il2CppObject* g_slopeObj = nullptr;
float g_slopeOrig = 0;
bool g_slopeSaved = false;
// Jump measurement (take-off to landing): apex above the take-off point and air time.
struct JumpMeasure {
    bool active = false;
    bool hl2 = true;  // gravity mode at take-off
    float y0 = 0, maxY = 0;
    double t0 = 0;
};
JumpMeasure g_jm;
bool g_jmLogged[2] = {false, false};  // first measurement per gravity mode logged at Info
struct LoggedJumpCfg {
    float scale = 0, gravity = 0, mult = 0, height = 0, walk = 0, normal = 0, sprint = 0;
    bool hl2 = true;
    bool operator==(const LoggedJumpCfg&) const = default;
};
LoggedJumpCfg g_loggedCfg;

double g_simTime = 0;  // sum of the player's KCC deltas
int g_groundTicks = 0;  // consecutive grounded ticks (1 = landing tick)
double g_lastGroundedTime = -1e9, g_lastJumpTime = -1e9;
V3 g_lastGroundPos{}, g_lastPos{};
bool g_haveGround = false, g_havePos = false, g_recoveryPending = false;
bool g_scrollLatch = false, g_jumpHeld = false, g_sprintHeld = false, g_hl2Sprinting = false;
bool g_walkHeld = false;     // sampled once per frame in Frame(), not per physics tick
bool g_autobhopHeld = false; // autobhop hold key, sampled like g_walkHeld
// Held state from InputEvent.PollValue: trusted once it has reported > 0.5
// (an unbound event polls the constant 0 of InputEvent.DefaultPoll).
bool g_jumpPollTrusted = false, g_sprintPollTrusted = false;
float g_loggedScale = 0;     // last effective unit scale written to the log
uint32_t g_jumpStartSerial = 0, g_jumpConsumedSerial = 0;
float g_maxEnergy = 100.0f;
float g_lastHSpeed = 0, g_lastVSpeed = 0;  // Source units/s, for drain + debug
// _recurrenceDelay patch (autobhop).
Il2CppObject* g_recurrenceObj = nullptr;
float g_recurrenceOrig = 0;
bool g_recurrenceSaved = false;
// Debug / fast-movement readout (per second).
float g_debugTimer = 0;
int g_debugTicks = 0, g_debugSubsteps = 0;
bool g_lastTickGrounded = false;
float g_peakHSpeed = 0;
struct Limiters {
    int safety = 0;      // safety speed limit clipped horizontal speed
    int maxvel = 0;      // sv_maxvelocity (CheckVelocity) clipped an axis
    int boostClamp = 0;  // the vanilla jump-boost formula changed the boost (clamp or ABH)
    int friction = 0;    // grounded friction ticks while above max speed (missed hop timing)
    int wall = 0;        // SR2's no-wall-climb projection cut the air acceleration
    int jetpack = 0;     // horizontal change by SR2 discarded while the jetpack ran
    int vanilla = 0;     // ticks with vanilla movement (debug fly or other unknown ability)
    int refused = 0;     // SR2 jump starts refused (not grounded, no coyote time)
    int launch = 0;      // ramp physics: left the ground moving up > 140 u/s (Source CategorizePosition)
};
Limiters g_lim;
// Momentum HUD bookkeeping (main thread: PostTick + Frame). Published to the
// render thread through ui::momentum_hud (relaxed atomics), never shared directly.
struct MomentumState {
    bool inAir = false;     // air phase started by a take-off / launch, not yet landed
    int airTicks = 0, gainTicks = 0;
    float sync = -1;        // last completed air phase (%), < 0 = none
    float pre = 0, preDelta = 0;  // horizontal speed at the last take-off, change vs the one before
    static constexpr int kRing = 64;
    double clock = 0;       // unscaled seconds (Frame)
    double sampleT[kRing] = {};
    float sampleV[kRing] = {};
    int head = 0, count = 0;
    bool published = false;
};
MomentumState g_mh;
// "A game menu is open" signals for the HUD (optional; bound in Setup).
struct StaticGetter {
    void* fn = nullptr;
    const MethodInfo* mi = nullptr;
};
StaticGetter g_cursorLockState, g_timeScale;  // UnityEngine.Cursor.get_lockState, Time.get_timeScale
// Func<float>.Invoke per delegate class (normally one): MethodInfo + pointer.
std::unordered_map<Il2CppClass*, std::pair<const MethodInfo*, void*>> g_pollInvoke;
std::unordered_map<Il2CppClass*, size_t> g_isActiveOffsets;

void ResetRuntimeState() {
    g_pc = {};
    g_tick = {};
    g_groundTicks = 0;
    g_haveGround = g_havePos = g_recoveryPending = false;
    g_jumpPollTrusted = g_sprintPollTrusted = false;
    g_lim = {};
    g_peakHSpeed = 0;
    g_scrollLatch = g_jumpHeld = g_sprintHeld = g_hl2Sprinting = g_walkHeld = g_autobhopHeld = false;
    g_loggedScale = 0;
    g_loggedCfg = {};
    g_jumpConsumedSerial = g_jumpStartSerial;
    g_lastHSpeed = g_lastVSpeed = 0;
    g_preLandTick = g_lastFinalTick = 0;
    g_rampMotor.store(nullptr);
    g_loggedSlopeMotor = nullptr;
    g_jm = {};
    g_mh = {};
    sr2::ui::momentum_hud::Clear();
}

// --------------------------------------------------------------- helpers --

template <class T>
T& F(void* obj, size_t off) {
    return unity::Field<T>(obj, off);
}

bool IsActiveAt(Il2CppObject* ability, size_t off) { return ability && off && F<bool>(ability, off); }

size_t IsActiveOffsetFor(Il2CppClass* k) {
    auto it = g_isActiveOffsets.find(k);
    if (it != g_isActiveOffsets.end()) return it->second;
    auto off = Resolver::Get().FieldOffset(k, "<IsActive>k__BackingField");
    const size_t v = off ? *off : 0;
    g_isActiveOffsets.emplace(k, v);
    return v;
}

bool ReadEnergy(Il2CppObject* ctrl, float& out) {
    if (!ctrl || !R.srccEnergy) return false;
    auto* res = F<Il2CppObject*>(ctrl, R.srccEnergy);
    if (!res) return false;
    out = F<float>(res, R.urValue);
    return std::isfinite(out);
}

void WriteEnergy(Il2CppObject* ctrl, float v) {
    if (!ctrl || !R.setEnergy.fn) return;
    auto* res = F<Il2CppObject*>(ctrl, R.srccEnergy);
    if (!res) return;
    // set_Value fires OnValueChanged so the HUD bar follows.
    sr2::core::Try([&] { R.setEnergy.fn(res, v, R.setEnergy.mi); });
}

// time = how long KCC skips ground probing/snapping (0 = only the next tick).
void ForceUnground(Il2CppObject* motor, float time = 0.1f) {
    if (!motor || !R.forceUnground.fn) return;
    sr2::core::Try([&] { R.forceUnground.fn(motor, time, R.forceUnground.mi); });
}

bool GameHasFocus() {
    HWND fg = GetForegroundWindow();
    if (!fg) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}

// All key polls go through here. While the settings overlay is open every key
// reads as released, so typing in its text fields never moves the player.
bool KeyDown(int vk) {
    return vk > 0 && vk < 256 && !sr2::ui::overlay::IsOpen() && GameHasFocus() && (GetAsyncKeyState(vk) & 0x8000) != 0;
}

float EffectiveScale() {
    if (g_cfg.matchVanilla && g_pc.vanillaMaxGround > 0.1f) {
        return std::clamp(g_pc.vanillaMaxGround / g_cfg.normal, 0.001f, 1.0f);
    }
    return g_cfg.unitScale;
}

// Logs the unit scale in use and what it means in metres (on discovery and
// whenever a setting changes it).
LoggedJumpCfg CurrentJumpCfg(float scale) {
    return {scale, g_cfg.gravity, g_cfg.gravityMult, g_cfg.jumpHeight, g_cfg.walk, g_cfg.normal, g_cfg.sprint,
            g_cfg.hl2Gravity};
}

void LogScale(float scale, float tickDt) {
    const bool scaleChanged = std::fabs(scale - g_loggedScale) > 1e-6f;
    g_loggedScale = scale;
    g_loggedCfg = CurrentJumpCfg(scale);
    const bool matched = g_cfg.matchVanilla && g_pc.vanillaMaxGround > 0.1f;
    const float gu = Hl2Gravity();
    const float v0 = mv::JumpImpulseForHeight(gu, g_cfg.jumpHeight / scale, tickDt);
    float air = 0;
    const float apex = mv::DiscreteJumpApex(v0, gu, tickDt, &air) * scale;
    SR2_INFO(
        "hl2_movement: unit scale {:.4f} m/u ({}): walk/normal/sprint {:.2f}/{:.2f}/{:.2f} m/s; HL2 gravity {:.0f} x {:.2f} "
        "u/s^2 = {:.1f} m/s^2, jump height {:.2f} m -> impulse {:.0f} u/s = {:.2f} m/s, apex {:.2f} m, {:.2f} s air "
        "time (40 Hz tick model; HL2's own jump is 160 u/s = 1.1 m / 0.53 s){}",
        scale, matched ? "matched to vanilla walk speed" : "manual", g_cfg.walk * scale, g_cfg.normal * scale,
        g_cfg.sprint * scale, g_cfg.gravity, g_cfg.gravityMult, gu * scale, g_cfg.jumpHeight, v0, v0 * scale, apex,
        air, g_cfg.hl2Gravity ? "" : " (gravity mode Vanilla SR2: gravity/jump are the game's, these are unused)");
    if (scaleChanged && matched && std::fabs(g_cfg.unitScale - 0.0254f) > 1e-5f)
        SR2_INFO("hl2_movement: the manual unit scale {:.4f} is ignored while 'Match vanilla walk speed' is on (moving "
                 "the unit scale slider turns matching off)",
                 g_cfg.unitScale);
}

void ApplyRecurrencePatch(bool want) {
    Il2CppObject* data = g_pc.jumpData;
    if (g_recurrenceSaved && (!want || data != g_recurrenceObj)) {
        if (g_recurrenceObj && unity::IsAlive(g_recurrenceObj)) F<float>(g_recurrenceObj, R.jdRecurrence) = g_recurrenceOrig;
        g_recurrenceSaved = false;
        g_recurrenceObj = nullptr;
    }
    if (want && data && !g_recurrenceSaved) {
        g_recurrenceObj = data;
        g_recurrenceOrig = F<float>(data, R.jdRecurrence);
        g_recurrenceSaved = true;
        F<float>(data, R.jdRecurrence) = 0.0f;
        SR2_DEBUG("hl2_movement: JumpAbilityData._recurrenceDelay {} -> 0 (Source has no re-jump delay)", g_recurrenceOrig);
    }
}

// Surf limit "Source": the player's motor's MaxStableSlopeAngle lowered to
// 45.57 degrees (normal.z 0.7) while ramp physics is on, restored otherwise.
void ApplySlopePatch(Il2CppObject* motor, bool want) {
    if (!R.motorMaxSlope) return;
    if (g_slopeSaved && (!want || motor != g_slopeObj)) {
        if (g_slopeObj && unity::IsAlive(g_slopeObj)) F<float>(g_slopeObj, R.motorMaxSlope) = g_slopeOrig;
        SR2_INFO("hl2_movement: KCC MaxStableSlopeAngle restored to {:.1f} deg", g_slopeOrig);
        g_slopeSaved = false;
        g_slopeObj = nullptr;
    }
    if (want && motor && !g_slopeSaved) {
        const float orig = F<float>(motor, R.motorMaxSlope);
        if (!std::isfinite(orig) || orig <= kSourceMaxSlopeDeg) return;
        g_slopeObj = motor;
        g_slopeOrig = orig;
        g_slopeSaved = true;
        F<float>(motor, R.motorMaxSlope) = kSourceMaxSlopeDeg;
        SR2_INFO("hl2_movement: surf limit Source - KCC MaxStableSlopeAngle {:.1f} -> {:.2f} deg", orig,
                 kSourceMaxSlopeDeg);
    }
}

// Vanilla SR2 jump, as JumpAbilityBehavior.UpdateVelocity (1.3.0 disassembly)
// and KFC.HandleGravity do it at the 40 Hz KCC tick:
//   take-off tick: v.y = _jumpUpSpeed (forward/no input), _holdForce = _initialHoldVelocity
//   later ticks:   gravityAmount = min(gravityAmount + g * _gravityAccumulationSharpness, g); v.y -= gravityAmount * dt
//                  while active: v.y /= 1 + dt * _airborneForceDamping;
//                  while held:   _holdForce /= 1 + dt * _holdVelocityDamping; v.y += _holdForce
//   the ability stops once v.y <= _verticalVelocityStopThreshold.
// Assumes the gravity accumulator starts at 0 at take-off (where SR2 resets
// it was not traced). Returns the apex in metres; airTime = back to take-off height.
float VanillaJumpApex(bool held, float dt, float* airTime) {
    if (airTime) *airTime = 0;
    Il2CppObject* d = g_pc.jumpData;
    if (!d || !R.jdAirDamp || !R.jdInitHold || !R.jdHoldDamp || !R.jdStopThr || !R.haveParams) return 0;
    Il2CppObject* p = F<Il2CppObject*>(g_pc.ctrl, R.kfcParams);
    if (!p) return 0;
    const float up = F<float>(d, R.jdUpSpeed), damp = F<float>(d, R.jdAirDamp), hold0 = F<float>(d, R.jdInitHold),
                holdDamp = F<float>(d, R.jdHoldDamp), stopThr = F<float>(d, R.jdStopThr);
    const float g = F<float>(p, R.cpGravity), sharp = R.cpGravAccum ? F<float>(p, R.cpGravAccum) : 1e9f;
    if (!(up > 0) || !(g > 0) || !std::isfinite(damp + hold0 + holdDamp + stopThr + sharp)) return 0;
    float vy = up, y = up * dt, apex = y, amount = 0, hold = hold0;
    bool active = true;
    int ticks = 1;
    while (y > 0 && ticks < 4000) {
        if (active && vy <= stopThr) active = false;  // ShouldStop, checked before the tick's velocity update
        amount = std::min(amount + g * sharp, g);
        vy -= amount * dt;
        if (active) {
            vy /= 1.0f + dt * damp;
            if (held && hold0 > 0) {
                hold /= 1.0f + dt * holdDamp;
                vy += hold;
            }
        }
        y += vy * dt;
        apex = std::max(apex, y);
        ++ticks;
    }
    if (airTime) *airTime = static_cast<float>(ticks) * dt;
    return apex;
}

// Find the player's ability behaviours (once per controller / ability array).
void Discover(Il2CppObject* self) {
    void* arr = F<void*>(self, R.kfcAbilities);
    if (self == g_pc.ctrl && arr == g_pc.abilities) return;
    const bool newCtrl = self != g_pc.ctrl;
    PlayerCache pc;
    pc.ctrl = self;
    pc.abilities = arr;
    const size_t n = unity::ArrayLength(arr);
    for (size_t i = 0; i < n; ++i) {
        auto* obj = static_cast<Il2CppObject*>(unity::ArrayAt(arr, i));
        if (!obj) continue;
        Il2CppClass* k = unity::ClassOf(obj);
        if (k == R.jumpB) pc.jump = obj;
        else if (k == R.sprintB) pc.sprint = obj;
        else if (k == R.jetpackB) pc.jetpack = obj;
        else if (R.pulseB && k == R.pulseB) continue;  // vac pulse wave: no movement of its own
        else pc.others.push_back({obj, IsActiveOffsetFor(k)});
    }
    if (pc.jump) {
        pc.jumpData = F<Il2CppObject*>(pc.jump, R.jumpData);
        if (pc.jumpData) {
            pc.jumpEv[0] = F<Il2CppObject*>(pc.jumpData, R.adStart);
            pc.jumpEv[1] = F<Il2CppObject*>(pc.jumpData, R.adAltStart);
            pc.jumpStopEv[0] = F<Il2CppObject*>(pc.jumpData, R.adStop);
            pc.jumpStopEv[1] = F<Il2CppObject*>(pc.jumpData, R.adAltStop);
        }
    }
    if (pc.sprint) {
        if (auto* sd = F<Il2CppObject*>(pc.sprint, R.sprintData)) {
            pc.sprintEv[0] = F<Il2CppObject*>(sd, R.adStart);
            pc.sprintEv[1] = F<Il2CppObject*>(sd, R.adAltStart);
        }
    }
    float vanillaGravity = 0;
    if (R.haveParams) {
        if (auto* p = F<Il2CppObject*>(self, R.kfcParams)) {
            pc.vanillaMaxGround = F<float>(p, R.cpMaxGround);
            vanillaGravity = F<float>(p, R.cpGravity);
        }
    }
    g_pc = std::move(pc);
    if (newCtrl) {
        g_groundTicks = 0;
        g_haveGround = g_havePos = false;
        float e = 0;
        if (ReadEnergy(self, e)) g_maxEnergy = std::max(100.0f, e);
    }
    const float jumpUp = g_pc.jumpData ? F<float>(g_pc.jumpData, R.jdUpSpeed) : 0.0f;
    const float recur = g_pc.jumpData ? F<float>(g_pc.jumpData, R.jdRecurrence) : 0.0f;
    SR2_INFO(
        "hl2_movement: player controller found ({} abilities: jump={} sprint={} jetpack={} other={}). Vanilla: max "
        "ground speed {:.2f} m/s (unit scale for 190 u/s = {:.4f}), gravity {:.2f} m/s^2, jump {:.2f} m/s, "
        "recurrence delay {:.2f} s",
        n, g_pc.jump != nullptr, g_pc.sprint != nullptr, g_pc.jetpack != nullptr, g_pc.others.size(),
        g_pc.vanillaMaxGround, g_pc.vanillaMaxGround / mv::kNormSpeed, vanillaGravity, jumpUp,
        g_recurrenceSaved && g_recurrenceObj == g_pc.jumpData ? g_recurrenceOrig : recur);
    if (g_pc.jumpData && R.jdAirDamp && R.jdInitHold && R.jdHoldDamp && R.jdStopThr) {
        float airTap = 0, airHeld = 0;
        const float tap = VanillaJumpApex(false, 0.025f, &airTap);
        const float held = VanillaJumpApex(true, 0.025f, &airHeld);
        SR2_INFO(
            "hl2_movement: vanilla JumpAbilityData: up speed {:.2f} m/s, airborne damping {:.3f}, initial hold velocity "
            "{:.3f}, hold damping {:.3f}, stop threshold {:.2f} m/s, gravity accumulation {:.3f} -> estimated vanilla "
            "apex (40 Hz): tap {:.2f} m / {:.2f} s, held {:.2f} m / {:.2f} s (the 'Jump height' setting defaults to "
            "2.0 m; gravity mode Vanilla SR2 logs a measured apex on its first jump)",
            F<float>(g_pc.jumpData, R.jdUpSpeed), F<float>(g_pc.jumpData, R.jdAirDamp),
            F<float>(g_pc.jumpData, R.jdInitHold), F<float>(g_pc.jumpData, R.jdHoldDamp),
            F<float>(g_pc.jumpData, R.jdStopThr),
            R.haveParams && R.cpGravAccum && F<Il2CppObject*>(self, R.kfcParams)
                ? F<float>(F<Il2CppObject*>(self, R.kfcParams), R.cpGravAccum)
                : -1.0f,
            tap, airTap, held, airHeld);
    }
}

bool AnyOtherAbilityActive() {
    for (const auto& o : g_pc.others)
        if (IsActiveAt(o.obj, o.activeOff)) return true;
    return false;
}

void UpdateSprint(Tick& t) {
    const bool srActive = IsActiveAt(g_pc.sprint, R.sprintActive);
    if (!g_cfg.hl2Sprint) {
        g_hl2Sprinting = srActive;
    } else {
        const bool wanted = g_sprintHeld || srActive;
        float e = 0;
        const bool haveE = ReadEnergy(t.self, e);
        if (haveE) g_maxEnergy = std::max(g_maxEnergy, e);
        if (!haveE) {
            g_hl2Sprinting = wanted;
        } else if (!g_hl2Sprinting) {
            // CHL2_Player::StartSprinting: refuses below 10 of 100 suit power.
            if (wanted && e >= mv::kSprintMinStartPower * 0.01f * g_maxEnergy) g_hl2Sprinting = true;
        } else if (!wanted || e <= 0.0f) {
            g_hl2Sprinting = false;
        }
    }
    t.sprinting = g_hl2Sprinting;
}

void BuildWishForTick(Tick& t) {
    V2 input = F<V2>(t.self, R.kfcInput);
    if (!std::isfinite(input.x) || !std::isfinite(input.y)) input = {0, 0};
    const float mag = std::sqrt(input.x * input.x + input.y * input.y);
    if (mag > 1.0f) input = {input.x / mag, input.y / mag};

    V3 look = F<V3>(t.self, R.kfcLook);
    mv::Vec3 fwd = mv::Flat(DirToSrc(look));
    if (mv::Normalize(fwd) < 1e-3f) {
        fwd = mv::Flat(DirToSrc(F<V3>(t.self, R.kfcMovement)));
        if (mv::Normalize(fwd) < 1e-3f) fwd = {1, 0, 0};
    }
    // Unity right = Cross(up, forward) = (f.z, 0, -f.x); in Source axes that
    // is (f.y, -f.x, 0) for forward (f.x, f.y, 0).
    const mv::Vec3 right{fwd.y, -fwd.x, 0};

    const bool walking = !t.sprinting && g_walkHeld;
    t.maxspeed = t.sprinting ? g_cfg.sprint : (walking ? g_cfg.walk : g_cfg.normal);
    float fmove = input.y * t.maxspeed;
    float smove = input.x * t.maxspeed;
    mv::ClampMove(fmove, smove, t.maxspeed);
    mv::BuildWish(fwd, right, fmove, smove, t.maxspeed, t.wishdir, t.wishspeed);
    t.forwardmove = fmove;
    t.forwardFlat = fwd;
}

// HL2 mode: the impulse that reaches the "Jump height (m)" setting at the HL2
// gravity in effect (sqrt(2 g h), corrected for KCC's per-tick integration so
// the apex equals the setting). Vanilla mode: SR2's jump speed.
float JumpImpulseUnits(const Tick& t) {
    if (g_cfg.hl2Gravity || !g_pc.jumpData)
        return mv::JumpImpulseForHeight(Hl2Gravity(), g_cfg.jumpHeight * t.invScale,
                                        static_cast<float>(t.substeps) * t.subDt);
    return F<float>(g_pc.jumpData, R.jdUpSpeed) * t.invScale;  // vanilla SR2 jump speed
}

bool Ours(Il2CppObject* self) { return g_tick.active && g_tick.ours && self == g_tick.self; }

// InputEvent.PollValue() for a button event: Func<float>.Invoke called
// through the delegate class's own Invoke MethodInfo (resolved by name, cached
// per class). false = no delegate / not callable / threw.
bool PollInputEvent(Il2CppObject* ev, float& out) {
    if (!ev || !R.iePoll) return false;
    auto* del = F<Il2CppObject*>(ev, R.iePoll);
    if (!del) return false;
    Il2CppClass* k = unity::ClassOf(del);
    if (!k) return false;
    auto it = g_pollInvoke.find(k);
    if (it == g_pollInvoke.end()) {
        Resolver& res = Resolver::Get();
        std::string err;
        const MethodInfo* m = res.Method(k, "Invoke", 0, &err);
        void* fp = m ? res.MethodPointer(m, &err) : nullptr;
        if (!fp) SR2_WARN("hl2_movement: PollValue delegate not callable ({}), using input events", err);
        it = g_pollInvoke.emplace(k, std::make_pair(m, fp)).first;
    }
    if (!it->second.second) return false;
    using PollFn = float (*)(Il2CppObject*, const MethodInfo*);
    auto fn = reinterpret_cast<PollFn>(it->second.second);
    const MethodInfo* mi = it->second.first;
    float v = 0;
    if (!sr2::core::Try([&] { v = fn(del, mi); })) return false;
    if (!std::isfinite(v)) return false;
    out = v;
    return true;
}

// Physical held state of a pair of start events, polled once per tick. The
// poll becomes authoritative once it has reported "down" at least once; until
// then the StartInput/Cancel tracking in `held` is kept.
void PollHeld(Il2CppObject* const (&evs)[2], bool& trusted, bool& held) {
    bool any = false, down = false;
    for (Il2CppObject* ev : evs) {
        float v = 0;
        if (PollInputEvent(ev, v)) {
            any = true;
            down |= v > 0.5f;
        }
    }
    if (!any) return;
    if (down && !trusted) {
        trusted = true;
        SR2_DEBUG("hl2_movement: input held state now read from InputEvent.PollValue");
    }
    if (trusted) held = down;
}

void ScheduleRecovery(Il2CppObject* motor, V3 target) {
    g_recoveryPending = true;
    main_thread::Post([motor, target] {
        g_recoveryPending = false;
        g_lastGroundedTime = g_simTime;  // no immediate re-trigger
        if (!g_active.load() || !unity::IsAlive(motor) || !R.setPosition.fn) return;
        std::string err;
        const bool ok = sr2::core::Try(
            [&] {
                R.setPosition.fn(motor, target, true, R.setPosition.mi);
                F<V3>(motor, R.motorBaseVelocity) = V3{0, 0, 0};
            },
            &err);
        if (ok) SR2_INFO("hl2_movement: fall-through recovery -> ({:.1f}, {:.1f}, {:.1f})", target.x, target.y, target.z);
        else SR2_WARN("hl2_movement: fall-through recovery failed: {}", err);
    });
}

// Measures each jump from take-off to landing (motor position before each
// tick's move): apex above the take-off point and air time. The first jump
// per gravity mode is logged at Info (Vanilla SR2 mode = the game's real jump
// height incl. its hold/damping logic), later ones only with the debug log.
void MeasureJump(const Tick& t, V3 pos) {
    if (t.jumped) {
        g_jm = {true, g_cfg.hl2Gravity, pos.y, pos.y, g_simTime};
        return;
    }
    if (!g_jm.active) return;
    if (t.jetpack || g_simTime - g_jm.t0 > 30.0) {  // jetpack took over / no landing: not a plain jump
        g_jm.active = false;
        return;
    }
    g_jm.maxY = std::max(g_jm.maxY, pos.y);
    if (!t.grounded) return;
    g_jm.active = false;
    const float apex = g_jm.maxY - g_jm.y0, air = static_cast<float>(g_simTime - g_jm.t0);
    const int mode = g_jm.hl2 ? 0 : 1;
    const std::string msg = std::format(
        "hl2_movement: measured jump ({}): apex {:.2f} m above take-off, air time {:.2f} s, landed {:+.2f} m{}",
        g_jm.hl2 ? "HL2 gravity" : "gravity mode Vanilla SR2 = vanilla jump height", apex, air, pos.y - g_jm.y0,
        g_jm.hl2 ? std::format(" (setting {:.2f} m)", g_cfg.jumpHeight) : std::string());
    if (!g_jmLogged[mode]) {
        g_jmLogged[mode] = true;
        SR2_INFO("{}", msg);
    } else if (g_cfg.debugLog) {
        SR2_DEBUG("{}", msg);
    }
}

// Momentum HUD: take-off ("pre") speed and strafe sync = share of the air
// phase's ticks that gained horizontal speed (air strafing only gains while
// the turn matches the strafe key, so this tracks Source-style sync).
void NoteMomentum(const Tick& t, float prevH, float h) {
    MomentumState& m = g_mh;
    if (t.jumped || t.launched) {
        if (t.jumped) {
            m.preDelta = m.pre > 0 ? h - m.pre : 0.0f;
            m.pre = h;
        }
        m.inAir = true;
        m.airTicks = m.gainTicks = 0;
        return;
    }
    if (t.jetpack) {  // jetpack flight is not a hop
        m.inAir = false;
        return;
    }
    if (!t.grounded) {
        if (m.inAir) {
            ++m.airTicks;
            if (h > prevH + 0.01f) ++m.gainTicks;
        }
        return;
    }
    if (m.inAir && m.airTicks > 0) m.sync = 100.0f * static_cast<float>(m.gainTicks) / static_cast<float>(m.airTicks);
    m.inAir = false;
}

void PostTick(Tick& t, V3* vel) {
    // An SR2 jump start that was pending at tick entry is valid for one tick
    // only (starts that happened during this tick stay pending).
    g_jumpConsumedSerial = t.jumpSerialAtEntry;

    // HL2 take-off: nothing after the jump (e.g. a force SR2 might queue in
    // JumpAbilityBehavior.Start) may add more upward speed on this tick.
    if (t.jumped && g_cfg.hl2Gravity && vel->y > t.takeoffVy) vel->y = t.takeoffVy;

    // Jetpack: horizontal movement is Source AirAccelerate only. Whatever
    // changed the horizontal velocity after our step (abilities, external
    // forces) is discarded; SR2 keeps its vertical thrust.
    if (t.jetpack && t.haveMove && (vel->x != t.moveX || vel->z != t.moveZ)) {
        vel->x = t.moveX;
        vel->z = t.moveZ;
        ++g_lim.jetpack;
    }

    mv::Vec3 v = ToSrc(*vel, t.invScale);
    bool changed = false;
    if (g_cfg.speedLimitOn && g_cfg.speedLimit > 0) {
        const float h = mv::Length2D(v);
        if (h > g_cfg.speedLimit) {
            const float k = g_cfg.speedLimit / h;
            v.x *= k;
            v.y *= k;
            changed = true;
            ++g_lim.safety;
        }
    }
    if (mv::CheckVelocity(v, g_cfg.vars.maxvelocity)) {
        changed = true;
        ++g_lim.maxvel;
    }
    if (changed) *vel = ToUnity(v, t.scale);
    const float prevHSpeed = g_lastHSpeed;
    g_lastHSpeed = mv::Length2D(v);
    NoteMomentum(t, prevHSpeed, g_lastHSpeed);
    g_lastVSpeed = v.z;
    g_peakHSpeed = std::max(g_peakHSpeed, g_lastHSpeed);
    g_lastTickGrounded = t.grounded && !t.jumped && !t.launched;

    // Ramp physics: the velocity KCC moves with on an airborne tick is the
    // pre-landing velocity if it touches ground (KCC's own landing drops its
    // vertical part); a grounded tick's final velocity tells a pure slope
    // reorientation by KCC apart from a real collision on the next tick.
    if (!t.grounded || t.launched) {
        g_preLand = v;
        g_preLandTick = g_tickNo;
    } else if (!t.jumped && !t.jetpack && t.haveMove) {
        g_lastFinal = *vel;
        g_lastFinalTick = g_tickNo;
    }

    // Fall-through recovery bookkeeping (motor position before this tick's move).
    const V3 pos = F<V3>(t.motor, R.motorPos);
    MeasureJump(t, pos);
    if (g_havePos) {
        const float dx = pos.x - g_lastPos.x, dy = pos.y - g_lastPos.y, dz = pos.z - g_lastPos.z;
        if (dx * dx + dy * dy + dz * dz > kTeleportDistance * kTeleportDistance) g_haveGround = false;  // teleported
    }
    g_lastPos = pos;
    g_havePos = true;
    if (t.grounded) {
        g_lastGroundPos = pos;
        g_haveGround = true;
        g_lastGroundedTime = g_simTime;
    } else if (g_cfg.fallRecovery && g_haveGround && !g_recoveryPending && vel->y < 0 &&
               g_simTime - g_lastGroundedTime > kFallRecoverAirSeconds && pos.y < g_lastGroundPos.y - kFallRecoverDrop) {
        ScheduleRecovery(t.motor, V3{g_lastGroundPos.x, g_lastGroundPos.y + 1.0f, g_lastGroundPos.z});
    }
}

// Remember the horizontal result of our ground/air step (jetpack guard).
void NoteMove(Tick& t, const V3& vel) {
    t.haveMove = true;
    t.moveX = vel.x;
    t.moveZ = vel.z;
}

// ---------------------------------------------------------------- hooks --

sr2::core::DetourGuard g_gUpdVel{kModule, "KFC.UpdateVelocity"};
sr2::core::DetourGuard g_gGroundVel{kModule, "KFC.HandleStablyGroundedVelocity"};
sr2::core::DetourGuard g_gGroundDrag{kModule, "KFC.HandleStablyGroundedDrag"};
sr2::core::DetourGuard g_gAirMove{kModule, "KFC.HandleHorizontalAirborneMovement"};
sr2::core::DetourGuard g_gAirDrag{kModule, "KFC.HandleAirborneDrag"};
sr2::core::DetourGuard g_gGravity{kModule, "KFC.HandleGravity"};
sr2::core::DetourGuard g_gAirClamp{kModule, "KFC.ClampAirborneHorizontalVelocity"};
sr2::core::DetourGuard g_gJumpStart{kModule, "JumpAbilityBehavior.Start"};
sr2::core::DetourGuard g_gJumpUpdVel{kModule, "JumpAbilityBehavior.UpdateVelocity"};
sr2::core::DetourGuard g_gInput{kModule, "InputEvent.StartInput/Cancel"};
sr2::core::DetourGuard g_gSprint{kModule, "SprintAbilityBehavior.Update"};
sr2::core::DetourGuard g_gSlots{kModule, "NextSlot/PreviousSlot"};
sr2::core::DetourGuard g_gVelProj{kModule, "KinematicCharacterMotor.HandleVelocityProjection"};

struct TickScope {
    ~TickScope() { g_tick.active = false; }
};

// Pre-tick bookkeeping. Returns false when the tick is not the player's (or
// cannot be handled), so the original runs untouched.
bool PreTick(Il2CppObject* self, float dt) {
    ReloadCfgIfDirty();
    g_rampMotor.store(nullptr, std::memory_order_relaxed);
    auto* motor = F<Il2CppObject*>(self, R.kfcMotor);
    if (!motor || !(dt > 0.0f) || !std::isfinite(dt)) return false;
    Discover(self);

    Tick& t = g_tick;
    t = Tick{};
    t.self = self;
    t.motor = motor;
    t.scale = EffectiveScale();
    t.invScale = 1.0f / t.scale;
    ++g_tickNo;
    t.grounded = F<bool>(motor, R.motorGrounding + R.grStable);
    t.foundAnyGround = F<bool>(motor, R.motorGrounding + R.grFoundAny);
    t.groundNormal = F<V3>(motor, R.motorGrounding + R.grNormal);
    t.jetpack = IsActiveAt(g_pc.jetpack, R.jetpackActive);
    t.bypassGravity = F<bool>(self, R.kfcBypassGravity);

    // Substeps: the KCC tick (fixed 40 Hz in 1.3.0, dt = 0.025) split into
    // equal steps of at most 0.01 s (0.025 -> 3 x 0.00833), so the simulated
    // movement time equals KCC's time on every tick (no accumulator jitter
    // between 2 and 3 steps). A hitch longer than 0.1 s is cut to 0.1 s.
    g_simTime += dt;
    const float simDt = std::min(dt, kSubstep * kMaxSubsteps);
    t.substeps = std::clamp(static_cast<int>(std::ceil(simDt / kSubstep - 1e-3f)), 1, kMaxSubsteps);
    t.subDt = simDt / static_cast<float>(t.substeps);
    g_groundTicks = t.grounded ? g_groundTicks + 1 : 0;
    if (!(CurrentJumpCfg(t.scale) == g_loggedCfg)) LogScale(t.scale, simDt);
    if (motor != g_loggedSlopeMotor) {
        g_loggedSlopeMotor = motor;
        if (R.motorMaxSlope) {
            const float a = g_slopeSaved && g_slopeObj == motor ? g_slopeOrig : F<float>(motor, R.motorMaxSlope);
            SR2_INFO("hl2_movement: KCC MaxStableSlopeAngle {:.1f} deg (Source walkable limit {:.2f} deg); ramp physics "
                     "{}, surfaces steeper than {:.1f} deg are surfed",
                     a, kSourceMaxSlopeDeg, g_cfg.rampPhysics ? "on" : "off",
                     g_cfg.surfSource ? std::min(a, kSourceMaxSlopeDeg) : a);
        }
    }
    ApplySlopePatch(motor, g_cfg.rampPhysics && g_cfg.surfSource);

    ApplyRecurrencePatch(true);
    PollHeld(g_pc.jumpEv, g_jumpPollTrusted, g_jumpHeld);
    PollHeld(g_pc.sprintEv, g_sprintPollTrusted, g_sprintHeld);
    UpdateSprint(t);
    BuildWishForTick(t);
    t.jumpSerialAtEntry = g_jumpStartSerial;
    t.srJumpPending = g_jumpStartSerial != g_jumpConsumedSerial;
    // SR2 starts a pressed jump in BeforeCharacterUpdate, which runs before
    // ground probing: on the landing tick it still sees "airborne" and starts
    // the jump one tick later. Skip friction on that landing tick (and only
    // within the first two grounded ticks, so a jump refused by SR2, e.g.
    // under a low ceiling, never disables friction for long).
    t.jumpQueued = t.grounded && !t.srJumpPending && R.jumpStartReq && g_pc.jump && g_groundTicks <= 2 &&
                   F<bool>(g_pc.jump, R.jumpStartReq) && !IsActiveAt(g_pc.jump, R.jumpActive);
    // Debug fly and any other unknown movement ability: vanilla movement.
    t.ours = !AnyOtherAbilityActive();
    // Airborne ClipVelocity in KCC's collision projection: only for the
    // player's motor on ticks where HL2 movement applies (not the jetpack).
    g_rampMotor.store(g_cfg.rampPhysics && t.ours && !t.jetpack ? motor : nullptr, std::memory_order_relaxed);
    return true;
}

void D_UpdateVelocity(Il2CppObject* self, V3* vel, float dt, const MethodInfo* mi) {
    if (!g_active.load(std::memory_order_relaxed) || !self || !vel || !R.srcc || unity::ClassOf(self) != R.srcc) {
        o_UpdateVelocity(self, vel, dt, mi);
        return;
    }
    // perf: only the mod's own work before + after the original (nested
    // detours inside it are tiny substep loops and are not timed).
    static const int perfSlot = sr2::core::perf::Slot("hook:hl2.UpdateVelocity");
    const int64_t perf0 = sr2::core::perf::Now();
    bool mine = false;
    g_gUpdVel.Run([&] { mine = PreTick(self, dt); });
    if (!mine) {
        g_tick.active = false;
        o_UpdateVelocity(self, vel, dt, mi);
        return;
    }
    const int64_t perf1 = sr2::core::perf::Now();
    {
        TickScope scope;
        g_tick.active = true;
        o_UpdateVelocity(self, vel, dt, mi);
    }
    const int64_t perf2 = sr2::core::perf::Now();
    g_gUpdVel.Run([&] {
        Tick& t = g_tick;
        if (t.ours) {
            PostTick(t, vel);
        } else {
            g_jumpConsumedSerial = t.jumpSerialAtEntry;
            ++g_lim.vanilla;
        }
        ++g_debugTicks;
        g_debugSubsteps += t.substeps;
    });
    if (perf0) sr2::core::perf::Add(perfSlot, (perf1 - perf0) + (sr2::core::perf::Now() - perf2));
}

// HL2 take-off from a grounded tick (scroll wheel / autobhop).
void SyntheticJump(Tick& t, mv::Vec3 h, V3* vel) {
    mv::Vec3 j{h.x, h.y, 0};
    if (mv::Jump(j, JumpImpulseUnits(t), t.forwardFlat, t.forwardmove, t.maxspeed, t.sprinting, g_cfg.boostMode))
        ++g_lim.boostClamp;
    *vel = ToUnity(j, t.scale);
    ForceUnground(t.motor);
    g_lastJumpTime = g_simTime;
    t.jumped = true;
    t.takeoffVy = vel->y;
}

// Ramp physics: leave the ground with velocity v3 (Source units) because it
// moves up faster than NON_JUMP_VELOCITY. KCC skips ground probing for the
// next tick only (ForceUnground(0)), so Source's re-grounding rule decides
// again on the following landing.
void RampLaunch(Tick& t, mv::Vec3 v3, V3* vel) {
    *vel = ToUnity(v3, t.scale);
    ForceUnground(t.motor, 0.0f);
    t.launched = true;
    ++g_lim.launch;
    NoteMove(t, *vel);
}

// True when `now` is `last` turned onto another slope with the same length
// and horizontal direction (KCC's GetDirectionTangentToSurface * magnitude on
// a stable hit), i.e. no collision changed it.
bool IsSlopeReorientation(V3 now, V3 last) {
    const float ln = std::sqrt(last.x * last.x + last.y * last.y + last.z * last.z);
    const float nn = std::sqrt(now.x * now.x + now.y * now.y + now.z * now.z);
    if (!(ln > 1e-3f) || std::fabs(nn - ln) > 0.005f * ln) return false;
    const float lh = std::sqrt(last.x * last.x + last.z * last.z), nh = std::sqrt(now.x * now.x + now.z * now.z);
    if (lh < 1e-3f || nh < 1e-3f) return false;
    return (now.x * last.x + now.z * last.z) / (lh * nh) >= 0.9995f;
}

void GroundVel(V3* vel) {
    Tick& t = g_tick;
    const Cfg& c = g_cfg;
    const mv::Vec3 v = ToSrc(*vel, t.invScale);

    // Jetpack on (or near) the ground: no ground movement, AirAccelerate only,
    // vertical left to SR2.
    if (t.jetpack) {
        mv::Vec3 h = mv::Flat(v);
        for (int i = 0; i < t.substeps; ++i) mv::AirMoveStep(h, t.wishdir, t.wishspeed, c.vars, t.subDt);
        const V3 out = ToUnity(h, t.scale);
        vel->x = out.x;
        vel->z = out.z;
        NoteMove(t, *vel);
        return;
    }

    mv::Vec3 n = DirToSrc(t.groundNormal);
    if (mv::Normalize(n) < 0.5f || n.z < 0.2f) n = {0, 0, 1};

    // Autobhop: only while the autobhop hold key is held (latched per frame).
    // Holding the jump input itself never auto-hops.
    const bool synthetic = !t.srJumpPending && (g_scrollLatch || (c.autobhop && g_autobhopHeld)) &&
                           g_simTime - g_lastJumpTime > 2.0 * kSubstep;
    g_scrollLatch = false;  // a wheel tick lives until the next tick
    // Source skips Friction on the tick CheckJumpButton takes off (that is
    // what makes bunnyhopping work) and runs AirMove instead of WalkMove.
    const bool noFriction = synthetic || t.srJumpPending || t.jumpQueued;

    // Horizontal ground velocity.
    mv::Vec3 h;
    if (!c.rampPhysics) {
        // Previous behaviour: horizontal part of KCC's velocity along the plane.
        h = mv::Flat(mv::ClipVelocity(v, n, 1.0f));
    } else if (g_groundTicks == 1 && g_preLandTick != 0 && g_preLandTick + 1 == g_tickNo) {
        // Landing (Source TryPlayerMove + SetGroundEntity): the full 3D
        // velocity of the last airborne tick clipped against the ground plane;
        // KCC's own landing kept only the horizontal speed. Falling onto a
        // down slope converts fall speed into ground speed (ramp boost).
        const mv::Vec3 cl = mv::LandingClip(g_preLand, n);
        if (mv::MovingUpRapidly(cl) && !noFriction) {
            // CategorizePosition: moving up faster than 140 u/s is not ground
            // (fast landing on an up slope): stay airborne along the slope.
            RampLaunch(t, cl, vel);
            return;
        }
        h = mv::Flat(cl);
        t.landingClip = true;
    } else if (g_lastFinalTick != 0 && g_lastFinalTick + 1 == g_tickNo && IsSlopeReorientation(*vel, g_lastFinal)) {
        // KCC only re-tilted last tick's velocity onto a new slope (keeping
        // its 3D length). Source keeps the horizontal velocity on slopes
        // (z = 0 before Friction/Accelerate, StepMove keeps the horizontal
        // move), so take last tick's horizontal velocity.
        h = mv::Flat(ToSrc(g_lastFinal, t.invScale));
    } else {
        h = mv::Flat(mv::ClipVelocity(v, n, 1.0f));  // collision changed it: KCC's result
    }
    if (!noFriction && mv::Length2D(h) > t.maxspeed + 1.0f) ++g_lim.friction;
    for (int i = 0; i < t.substeps; ++i) {
        if (noFriction) mv::AirMoveStep(h, t.wishdir, t.wishspeed, c.vars, t.subDt);
        else mv::GroundMoveStep(h, t.wishdir, t.wishspeed, c.vars, t.subDt);
    }

    if (synthetic) {
        SyntheticJump(t, h, vel);
        NoteMove(t, *vel);
        return;
    }
    if (t.srJumpPending) {
        // SR2's jump ability takes off later this tick (see D_JumpUpdVel).
        V3 out = ToUnity(mv::Vec3{h.x, h.y, 0}, t.scale);
        out.y = vel->y;
        *vel = out;
        NoteMove(t, *vel);
        return;
    }
    // Fast uphill: Source's StepMove takes z from the clipped slide move; if
    // that is above NON_JUMP_VELOCITY, CategorizePosition drops the ground.
    if (c.rampPhysics) {
        const float zc = mv::GroundClipZ(h, n);
        if (zc > mv::kNonJumpVelocity) {
            RampLaunch(t, mv::Vec3{h.x, h.y, zc}, vel);
            return;
        }
    }
    // Stay on the ground plane: keep the Source horizontal velocity exactly
    // and pick the vertical component so that v . n == 0.
    const mv::Vec3 out{h.x, h.y, -(h.x * n.x + h.y * n.y) / n.z};
    *vel = ToUnity(out, t.scale);
    NoteMove(t, *vel);
}

void D_GroundVel(Il2CppObject* self, V3* vel, float dt, const MethodInfo* mi) {
    if (Ours(self) && vel && g_gGroundVel.Run([&] { GroundVel(vel); })) return;
    o_GroundVel(self, vel, dt, mi);
}

void D_GroundDrag(Il2CppObject* self, V3* vel, float dt, const MethodInfo* mi) {
    // Source friction already ran in D_GroundVel.
    if (Ours(self) && g_gGroundDrag.Active()) return;
    o_GroundDrag(self, vel, dt, mi);
}

void AirMove(Il2CppObject* self, V3* vel) {
    Tick& t = g_tick;
    g_scrollLatch = false;  // wheel tick while airborne: no jump (Source semantics)
    const mv::Vec3 h0 = mv::Flat(ToSrc(*vel, t.invScale));
    mv::Vec3 h = h0;
    for (int i = 0; i < t.substeps; ++i) mv::AirMoveStep(h, t.wishdir, t.wishspeed, g_cfg.vars, t.subDt);
    V3 added = ToUnity(h - h0, t.scale);
    added.y = 0;
    if (added.x != 0.0f || added.z != 0.0f) {
        // SR2/KCC "don't air-climb steep walls" projection of the added velocity.
        // Ramp physics, touching a surface too steep to stand on: that
        // projection removes the strafe into the ramp that surfing needs;
        // Source only clips the velocity against it (HandleVelocityProjection
        // hook / KCC's own airborne projection), so it is skipped there.
        const bool surfing = g_cfg.rampPhysics && !t.jetpack && t.foundAnyGround && !t.grounded;
        if (R.groundOnAir.fn && !surfing) {
            const float before = added.x * added.x + added.z * added.z;
            R.groundOnAir.fn(self, vel, &added, R.groundOnAir.mi);
            if (added.x * added.x + added.z * added.z < 0.81f * before) ++g_lim.wall;
        }
        vel->x += added.x;
        vel->y += added.y;
        vel->z += added.z;
    }
    NoteMove(t, *vel);
}

void D_AirMove(Il2CppObject* self, V3* vel, float dt, V3 movement, const MethodInfo* mi) {
    if (Ours(self) && vel && g_gAirMove.Run([&] { AirMove(self, vel); })) return;
    o_AirMove(self, vel, dt, movement, mi);
}

void D_AirDrag(Il2CppObject* self, V3* vel, float dt, V3 movement, const MethodInfo* mi) {
    // SR2's airborne drag is horizontal-only (1.3.0 disassembly): Source has
    // none, so it is skipped whenever HL2 movement applies.
    if (Ours(self) && vel && g_gAirDrag.Active()) return;
    o_AirDrag(self, vel, dt, movement, mi);
}

void D_Gravity(Il2CppObject* self, V3* vel, float dt, const MethodInfo* mi) {
    const Tick& t = g_tick;
    if (Ours(self) && vel && g_cfg.hl2Gravity && !t.jetpack && !t.bypassGravity &&
        g_gGravity.Run([&] { vel->y -= Hl2Gravity() * t.scale * (static_cast<float>(t.substeps) * t.subDt); }))
        return;
    o_Gravity(self, vel, dt, mi);
}

// KCC collision response while airborne (or forced off the ground): Source's
// TryPlayerMove clips the velocity against every plane it hits with
// overbounce 1, whatever the slope. KCC does that only for unstable hits; a
// stable (walkable) hit while airborne drops the vertical speed and keeps the
// horizontal magnitude along the surface, which would cancel ramp launches
// and slides. Returns false = let KCC handle it (grounded, bad input).
bool VelProj(Il2CppObject* motor, V3* vel, V3 normal) {
    const bool stable = F<bool>(motor, R.motorGrounding + R.grStable);
    bool mustUnground = R.motorMustUnground && F<bool>(motor, R.motorMustUnground);
    if (R.motorMustUngroundTime && F<float>(motor, R.motorMustUngroundTime) > 0.0f) mustUnground = true;
    if (stable && !mustUnground) return false;  // grounded: KCC's ground-following projection
    mv::Vec3 n{normal.x, normal.y, normal.z};  // axis order is irrelevant for the clip
    if (!(mv::Normalize(n) > 0.5f)) return false;
    const mv::Vec3 out = mv::ClipVelocity({vel->x, vel->y, vel->z}, n, 1.0f);
    if (!std::isfinite(out.x) || !std::isfinite(out.y) || !std::isfinite(out.z)) return false;
    *vel = V3{out.x, out.y, out.z};
    return true;
}

void D_VelProj(Il2CppObject* self, V3* vel, const V3* normal, bool stableOnHit, const MethodInfo* mi) {
    bool handled = false;
    if (self && vel && normal && self == g_rampMotor.load(std::memory_order_relaxed) &&
        g_active.load(std::memory_order_relaxed) && g_gVelProj.Run([&] { handled = VelProj(self, vel, *normal); }) &&
        handled)
        return;
    o_VelProj(self, vel, normal, stableOnHit, mi);
}

void D_AirClamp(Il2CppObject* self, V3* vel, float dt, const MethodInfo* mi) {
    // SR2's air speed cap is exactly what HL2 movement must not have.
    if (Ours(self) && g_gAirClamp.Active()) return;
    o_AirClamp(self, vel, dt, mi);
}

void D_JumpStart(Il2CppObject* self, const MethodInfo* mi) {
    o_JumpStart(self, mi);
    if (g_active.load(std::memory_order_relaxed) && self)
        g_gJumpStart.Run([&] {
            if (self == g_pc.jump) ++g_jumpStartSerial;
        });
}

void JumpUpdVelPost(V3* vel, const V3& before, const V3& after) {
    Tick& t = g_tick;
    if (t.srJumpPending && !t.jumped) {
        t.srJumpPending = false;
        g_jumpConsumedSerial = t.jumpSerialAtEntry;
        const bool allowed = t.grounded || (g_simTime - g_lastGroundedTime <= kCoyoteSeconds &&
                                            g_lastJumpTime < g_lastGroundedTime);
        if (!allowed) {
            *vel = before;  // HL2 only jumps off the ground
            ++g_lim.refused;
            return;
        }
        mv::Vec3 v = mv::Flat(ToSrc(before, t.invScale));
        const float impulse = g_cfg.hl2Gravity ? JumpImpulseUnits(t) : std::max(0.0f, after.y) * t.invScale;
        if (mv::Jump(v, impulse, t.forwardFlat, t.forwardmove, t.maxspeed, t.sprinting, g_cfg.boostMode))
            ++g_lim.boostClamp;
        *vel = ToUnity(v, t.scale);
        ForceUnground(t.motor);
        g_lastJumpTime = g_simTime;
        t.jumped = true;
        t.takeoffVy = vel->y;
        NoteMove(t, *vel);
        return;
    }
    // Not a take-off tick: drop SR2's horizontal jump shaping; in HL2 gravity
    // mode also its variable-height hold force.
    vel->x = before.x;
    vel->z = before.z;
    if (g_cfg.hl2Gravity) vel->y = before.y;
}

void D_JumpUpdVel(Il2CppObject* self, V3* vel, float dt, const MethodInfo* mi) {
    const Tick& t = g_tick;
    if (!t.active || !t.ours || !vel || !self || self != g_pc.jump) {
        o_JumpUpdVel(self, vel, dt, mi);
        return;
    }
    const V3 before = *vel;
    o_JumpUpdVel(self, vel, dt, mi);  // keep SR2's jump state machine running
    const V3 after = *vel;
    g_gJumpUpdVel.Run([&] { JumpUpdVelPost(vel, before, after); });
}

// Fallback held tracking (until PollValue has proven itself): start events
// go down on StartInput and up on Cancel; a stop event (jump released, when
// the game binds release separately) starting also means "released".
void NoteInput(Il2CppObject* ev, bool down) {
    if (!ev) return;
    if (!g_jumpPollTrusted) {
        if (ev == g_pc.jumpEv[0] || ev == g_pc.jumpEv[1]) g_jumpHeld = down;
        if (down && (ev == g_pc.jumpStopEv[0] || ev == g_pc.jumpStopEv[1])) g_jumpHeld = false;
    }
    if (!g_sprintPollTrusted && (ev == g_pc.sprintEv[0] || ev == g_pc.sprintEv[1])) g_sprintHeld = down;
}

void D_InputStart(Il2CppObject* self, InputEventDataRaw data, const MethodInfo* mi) {
    if (g_active.load(std::memory_order_relaxed)) g_gInput.Run([&] { NoteInput(self, true); });
    o_InputStart(self, data, mi);
}

void D_InputCancel(Il2CppObject* self, InputEventDataRaw data, const MethodInfo* mi) {
    if (g_active.load(std::memory_order_relaxed)) g_gInput.Run([&] { NoteInput(self, false); });
    o_InputCancel(self, data, mi);
}

void D_SprintUpdate(Il2CppObject* self, float dt, const MethodInfo* mi) {
    if (!g_active.load(std::memory_order_relaxed) || !g_cfg.hl2Sprint || !self || self != g_pc.sprint) {
        o_SprintUpdate(self, dt, mi);
        return;
    }
    float before = 0;
    bool haveBefore = false;
    g_gSprint.Run([&] { haveBefore = ReadEnergy(g_pc.ctrl, before); });
    o_SprintUpdate(self, dt, mi);
    // HL2-style sprint drains in Frame(); undo SR2's own sprint drain.
    if (haveBefore)
        g_gSprint.Run([&] {
            float after = 0;
            if (ReadEnergy(g_pc.ctrl, after) && after < before) WriteEnergy(g_pc.ctrl, before);
        });
}

// Wheel slot cycling: VacuumItem / GadgetFavoritesBar .NextSlot / .PreviousSlot.
// NextSlot is taken to be wheel-down and PreviousSlot wheel-up (assumption).
template <int I>
struct SlotHook {
    static inline InputDataFn orig = nullptr;
    static constexpr bool kWheelUp = (I % 2) == 1;  // odd = PreviousSlot
    static void Detour(Il2CppObject* self, InputEventDataRaw data, const MethodInfo* mi) {
        bool swallow = false;
        if (g_active.load(std::memory_order_relaxed) && g_cfg.scrollJump &&
            (data.source == kSourcePointer || data.source == kSourceUnknown)) {
            g_gSlots.Run([&] {
                const int dir = g_cfg.scrollDir;
                if (dir == 2 || (dir == 0) == kWheelUp) g_scrollLatch = true;
                swallow = true;  // the wheel never changes slots while scroll-jump is on
            });
        }
        if (!swallow) orig(self, data, mi);
    }
};

// ------------------------------------------------------------- frame tick --

std::string LimiterText() {
    std::string s;
    auto add = [&](int n, const char* what) {
        if (n <= 0) return;
        if (!s.empty()) s += ", ";
        s += std::format("{} x{}", what, n);
    };
    add(g_lim.friction, "ground friction above max speed (late hop)");
    add(g_lim.safety, "safety speed limit");
    add(g_lim.maxvel, "sv_maxvelocity");
    add(g_lim.boostClamp, "HL2 jump-boost formula (clamp/ABH)");
    add(g_lim.wall, "wall projection");
    add(g_lim.jetpack, "jetpack horizontal change discarded");
    add(g_lim.vanilla, "vanilla movement ticks (other ability active)");
    add(g_lim.refused, "SR2 jump refused (airborne)");
    add(g_lim.launch, "ramp launch (left ground moving up > 140 u/s)");
    return s.empty() ? "none" : s;
}

void HideMomentumHud() {
    if (g_mh.published) sr2::ui::momentum_hud::Clear();
    g_mh.published = false;
}

bool GameMenuOpen() {
    if (sr2::ui::overlay::IsOpen()) return true;
    if (g_cursorLockState.fn &&
        reinterpret_cast<int32_t (*)(const MethodInfo*)>(g_cursorLockState.fn)(g_cursorLockState.mi) !=
            1 /* CursorLockMode.Locked */)
        return true;
    if (g_timeScale.fn && reinterpret_cast<float (*)(const MethodInfo*)>(g_timeScale.fn)(g_timeScale.mi) == 0.0f)
        return true;
    return false;
}

// Speed trend over the last ~0.25 s from a ring of per-frame samples (at most
// one per 1/60 s, so 64 samples cover about a second at any frame rate).
float MomentumTrend(float speed) {
    MomentumState& m = g_mh;
    constexpr int N = MomentumState::kRing;
    const int newest = (m.head + N - 1) % N;
    if (m.count == 0 || m.clock - m.sampleT[newest] >= 1.0 / 60.0) {
        m.sampleT[m.head] = m.clock;
        m.sampleV[m.head] = speed;
        m.head = (m.head + 1) % N;
        m.count = std::min(m.count + 1, N);
    }
    float ref = speed;
    for (int i = 1; i <= m.count; ++i) {  // newest -> oldest; first sample at least 0.25 s old
        const int k = (m.head + N - i) % N;
        ref = m.sampleV[k];
        if (m.clock - m.sampleT[k] >= 0.25) break;
    }
    return speed - ref;
}

void UpdateMomentumHud(float unscaledDt) {
    if (!g_cfg.mhOn || GameMenuOpen()) {
        HideMomentumHud();
        return;
    }
    MomentumState& m = g_mh;
    m.clock += std::clamp(unscaledDt, 0.0f, 0.5f);
    const float speed = g_lastHSpeed;
    sr2::ui::momentum_hud::State st;
    st.speed = speed;
    st.trend = MomentumTrend(speed);
    st.preSpeed = m.pre;
    st.preDelta = m.preDelta;
    st.sync = m.inAir && m.airTicks > 0 ? 100.0f * static_cast<float>(m.gainTicks) / static_cast<float>(m.airTicks)
                                        : m.sync;
    st.details = g_cfg.mhDetails;
    st.bar = g_cfg.mhBar;
    st.fade = g_cfg.mhFade;
    st.x = g_cfg.mhX;
    st.y = g_cfg.mhY;
    st.size = g_cfg.mhSize;
    st.barFull = std::max(1.0f, 2.0f * g_cfg.sprint);
    sr2::ui::momentum_hud::Publish(st);
    if (!m.published) {
        static bool s_logged = false;
        if (!s_logged) {
            s_logged = true;
            SR2_INFO("hl2_movement: momentum HUD shown (speed {:.0f} u/s; menu check: cursor {}, time scale {})", speed,
                     g_cursorLockState.fn ? "yes" : "no", g_timeScale.fn ? "yes" : "no");
        }
    }
    m.published = true;
}

void Frame(float unscaledDt) {
    ReloadCfgIfDirty();
    if (!g_active.load(std::memory_order_relaxed)) {
        HideMomentumHud();
        return;
    }
    if (g_pc.ctrl && !unity::IsAlive(g_pc.ctrl)) {
        ResetRuntimeState();
        return;
    }
    if (!g_pc.ctrl) {
        HideMomentumHud();
        return;
    }
    UpdateMomentumHud(unscaledDt);
    // Read-only polls (GetAsyncKeyState, only while the game is foreground and
    // the overlay is closed);
    // nothing is swallowed, so Alt+Tab / Alt+F4 keep working.
    g_walkHeld = KeyDown(g_cfg.walkKey);
    g_autobhopHeld = g_cfg.autobhop && KeyDown(g_cfg.autobhopKey);

    // HL2 sprint drain: SuitDeviceSprint, only while actually moving
    // (CHL2_Player::SuitPower_Update skips the drain when x/y velocity is 0).
    if (g_cfg.hl2Sprint && g_hl2Sprinting && g_lastHSpeed > 1.0f) {
        const float dt = unity::DeltaTime();
        float e = 0;
        if (dt > 0 && ReadEnergy(g_pc.ctrl, e) && e > 0) {
            const float drain = g_cfg.sprintDrain * 0.01f * g_maxEnergy * dt;
            WriteEnergy(g_pc.ctrl, std::max(0.0f, e - drain));
        }
    }

    g_debugTimer += unscaledDt;
    if (g_debugTimer < 1.0f) return;
    const float scale = EffectiveScale();
    // Fast-movement readout (Info): once per second while above 2x normal speed.
    if (g_cfg.speedReadout && g_peakHSpeed > 2.0f * g_cfg.normal) {
        SR2_INFO(
            "hl2_movement: speed {:.0f} u/s = {:.1f} m/s (peak {:.0f} u/s = {:.1f} m/s), grounded={}, jetpack={}, "
            "{} ticks; limiters this second: {}",
            g_lastHSpeed, g_lastHSpeed * scale, g_peakHSpeed, g_peakHSpeed * scale, g_lastTickGrounded,
            IsActiveAt(g_pc.jetpack, R.jetpackActive), g_debugTicks, LimiterText());
    }
    if (g_cfg.debugLog) {
        float e = 0;
        ReadEnergy(g_pc.ctrl, e);
        SR2_DEBUG(
            "hl2_movement: speed {:.0f} u/s ({:.2f} m/s), vz {:.0f} u/s, grounded={}, sprint={}, jump held={} (poll {}), "
            "autobhop key held={}, "
            "energy {:.1f}/{:.0f}, {} ticks / {} substeps in {:.2f} s",
            g_lastHSpeed, g_lastHSpeed * scale, g_lastVSpeed, g_lastTickGrounded, g_hl2Sprinting, g_jumpHeld,
            g_jumpPollTrusted ? "on" : "off", g_autobhopHeld, e, g_maxEnergy, g_debugTicks, g_debugSubsteps, g_debugTimer);
    }
    g_debugTimer = 0;
    g_debugTicks = g_debugSubsteps = 0;
    g_peakHSpeed = 0;
    g_lim = {};
}

// --------------------------------------------------------------- settings --

// One-time migration of the old Speed Panel placement (vertical offset from the
// screen centre as a fraction of the height, size multiplier of the overlay's
// 15 px body font x 1.15 at 1080p) to the edit mode's percent placement, so
// the panel stays where it was: horizontally centred, same top edge and size.
void MigrateMomentumPlacement() {
    const float oldY = std::clamp(settings::GetFloat(kModule, kOldMomentumY), -0.45f, 0.45f);
    float oldScale = settings::GetFloat(kModule, kOldMomentumScale);
    if (!settings::Saved(kModule, kOldMomentumScale)) oldScale = 1.0f;
    oldScale = std::clamp(oldScale, 0.5f, 3.0f);
    const float size = 15.0f * 1.15f / 1080.0f * 100.0f * oldScale;  // % of the screen height
    const float boxH = size * sr2::ui::momentum_hud::BoxHeightUnits(settings::GetBool(kModule, kMomentumBar),
                                                                    settings::GetBool(kModule, kMomentumDetails));
    const float top = (0.5f + (settings::Saved(kModule, kOldMomentumY) ? oldY : 0.06f)) * 100.0f;
    const float y = std::clamp(top / std::max(1.0f, 100.0f - boxH) * 100.0f, 0.0f, 100.0f);
    settings::Set(kModule, kMomentumX, 50);
    settings::Set(kModule, kMomentumY, y);
    settings::Set(kModule, kMomentumSize, size);
    SR2_INFO("hl2_movement: Speed Panel placement migrated (offset {:.3f}, scale {:.2f} -> y {:.1f}%, size {:.2f}%)", oldY,
             oldScale, y, size);
}

void RegisterSettings() {
    using settings::Kind;
    // Same definition the module manager registers later; registering it
    // first keeps "Enabled" at the top of this module's settings list.
    {
        settings::Def d;
        d.module = kModule;
        d.key = "enabled";
        d.label = "Enabled";
        d.kind = Kind::Bool;
        d.def = 0;
        d.tooltip = "Turn this module on or off (applies immediately).";
        settings::Register(d);
    }
    auto add = [](const char* key, const char* label, const char* group, Kind kind, double def, double mn, double mx,
                  double step, const char* tip, std::vector<std::string> options = {}) {
        settings::Def d;
        d.module = kModule;
        d.key = key;
        d.label = label;
        d.group = group;
        d.kind = kind;
        d.def = def;
        d.min = mn;
        d.max = mx;
        d.step = step;
        d.options = std::move(options);
        d.tooltip = tip;
        settings::Register(d);
    };
    add(kAccel, "sv_accelerate", "Physics", Kind::Float, mv::kSvAccelerate, 0, 100, 0.5,
        "Ground acceleration (HL2: 10).");
    add(kAirAccel, "sv_airaccelerate", "Physics", Kind::Float, mv::kSvAirAccelerate, 0, 1000, 1,
        "Air acceleration (HL2: 10). Air-strafe gain comes from the 30 u/s air speed cap.");
    add(kFriction, "sv_friction", "Physics", Kind::Float, mv::kSvFriction, 0, 20, 0.1, "Ground friction (HL2: 4).");
    add(kStopSpeed, "sv_stopspeed", "Physics", Kind::Float, mv::kSvStopSpeed, 0, 500, 5,
        "Below this speed friction acts as if moving at this speed (HL2: 100).");
    add(kGravityMode, "Gravity and jump", "Physics", Kind::Enum, 0, 0, 1, 1,
        "HL2: sv_gravity (x multiplier) and a fixed jump to 'Jump height (m)', no variable jump height. Vanilla SR2: "
        "the game's gravity and "
        "jump height (horizontal movement stays HL2). The jetpack always uses vanilla gravity.",
        {"HL2", "Vanilla SR2"});
    add(kGravity, "sv_gravity", "Physics", Kind::Float, mv::kSvGravity, 0, 2000, 10,
        "Gravity in HL2 mode, units/s^2 (HL2: 600). The jump impulse follows it so the jump height stays at the "
        "'Jump height (m)' setting.");
    add(kGravityMult, "Gravity multiplier", "Physics", Kind::Float, 1.0, 0.25, 3.0, 0.05,
        "Multiplies sv_gravity in HL2 mode (1 = HL2). Lower = floatier, longer jumps at the same height; the jump "
        "height setting is kept.");
    add(kRampPhysics, "Ramp physics (Source)", "Physics", Kind::Bool, 1, 0, 1, 1,
        "Source ramp behaviour: landing clips your full velocity against the slope (falling onto a down slope turns "
        "fall speed into speed = ramp boost, an up slope costs speed), ground speed is kept on slopes like Source, "
        "moving up faster than 140 u/s leaves the ground (launch / slide up ramps), surfaces too steep to stand on "
        "are surfed: you slide along them in the air, strafing into the ramp holds you on it. Off = previous "
        "behaviour.");
    add(kSurfLimit, "Surf slope limit", "Physics", Kind::Enum, 0, 0, 1, 1,
        "Which slopes are too steep to stand on (surfed) while ramp physics is on. SR2: KCC's MaxStableSlopeAngle "
        "(logged), the game's walkable slopes stay walkable. Source: 45.6 degrees (normal.z < 0.7) like HL2; steeper "
        "SR2 slopes that were walkable become slides.",
        {"SR2 (KCC max stable slope)", "Source (45.6 degrees)"});
    add(kMaxVelocity, "sv_maxvelocity", "Physics", Kind::Float, mv::kSvMaxVelocity, 100, 100000, 100,
        "Per-axis velocity clamp, units/s (HL2: 3500, CGameMovement::CheckVelocity).");

    add(kWalkSpeed, "Walk speed", "Speeds", Kind::Float, mv::kWalkSpeed, 10, 2000, 5,
        "hl2_walkspeed (150), used while the walk key is held.");
    add(kNormSpeed, "Normal speed", "Speeds", Kind::Float, mv::kNormSpeed, 10, 2000, 5, "hl2_normspeed (190).");
    add(kSprintSpeed, "Sprint speed", "Speeds", Kind::Float, mv::kSprintSpeed, 10, 4000, 5,
        "hl2_sprintspeed (320), while sprinting.");
    add(kWalkKey, "Walk key", "Speeds", Kind::Key, 0, 0, 255, 1,
        "Hold to move at walk speed (hl2_walkspeed). Unbound by default because Alt (HL2's walk key) is the "
        "autobhop hold key; if both are bound to the same key, autobhop wins.");

    add(kJumpHeight, "Jump height (m)", "Jumping", Kind::Float, 2.0, 0.5, 6.0, 0.05,
        "Jump apex in metres in HL2 gravity mode; the impulse is sqrt(2 g h) at the HL2 gravity in effect (HL2's own "
        "jump is 1.1 m at the default scale). Air time grows with the height. Gravity mode Vanilla SR2 ignores it; "
        "its first jump logs the measured vanilla apex.");
    add(kAutobhop, "Autobhop", "Jumping", Kind::Bool, 0, 0, 1, 1,
        "While the autobhop hold key is held, you jump on the first grounded tick of every landing (no friction, "
        "jump boost per 'Jump boost'). Space always does normal single jumps and needs a release between jumps, "
        "as in Source.");
    add(kAutobhopKey, "Autobhop hold key", "Jumping", Kind::Key, VK_MENU, 0, 255, 1,
        "Hold to bunnyhop automatically while Autobhop is on (default Alt). Only read, never blocked, so Alt+Tab "
        "and Alt+F4 still work. Set to none to disable.");
    add(kScrollJump, "Scroll-wheel jump", "Jumping", Kind::Bool, 0, 0, 1, 1,
        "A mouse wheel tick jumps (like binding +jump to the wheel). While on, the wheel no longer cycles vacpack or "
        "gadget slots.");
    add(kScrollDir, "Scroll-wheel jump direction", "Jumping", Kind::Enum, 2, 0, 2, 1,
        "Which wheel direction jumps.", {"Wheel up", "Wheel down", "Both"});
    add(kBoostMode, "Jump boost", "Jumping", Kind::Enum, 2, 0, 3, 1,
        "HL2 adds 50% of your forward input (10% sprinting) along the view on every jump. Uncapped forward boost: "
        "never limited. Vanilla HL2: the exact Source SDK 2013 clamp (forward hops capped near 1.5x max speed), "
        "which makes hopping backwards above that speed accelerate (ABH). Uncapped + ABH: uncapped while moving "
        "forward relative to the view, vanilla formula (ABH) while moving backwards. Accelerated forward + back: "
        "the ABH formula mirrored for forward travel, so hopping forward (facing where you go, no key or W held) "
        "gains speed each hop exactly like a back hop, up to sv_maxvelocity; back hops are vanilla ABH.",
        {"Uncapped forward boost", "Vanilla HL2 (clamp, with ABH)", "Uncapped + ABH",
         "Accelerated forward + back (ABH both ways)"});

    add(kHl2Sprint, "HL2-style sprint", "Sprint", Kind::Bool, 1, 0, 1, 1,
        "Hold sprint for sprint speed; drains the energy bar like HL2 suit power (only while moving), cannot start "
        "below 10%, stops when empty. Off = SR2 decides when you sprint and drains as vanilla.");
    add(kSprintDrain, "Sprint energy drain", "Sprint", Kind::Float, mv::kSprintDrainPerSec, 0, 100, 0.5,
        "Percent of max energy per second while sprinting (HL2: 12.5 = empty in 8 s).");

    add(kSpeedLimitOn, "Safety speed limit", "Safety", Kind::Bool, 0, 0, 1, 1,
        "Caps horizontal speed so you cannot outrun SR2's world streaming.");
    add(kSpeedLimit, "Safety speed limit (u/s)", "Safety", Kind::Float, 2000, 100, 100000, 50,
        "Horizontal speed cap in Source units/s when the safety limit is on.");
    add(kFallRecovery, "Fall-through recovery", "Safety", Kind::Bool, 1, 0, 1, 1,
        "If you fall far below your last grounded position for several seconds, teleport back to it.");

    add(kMatchVanilla, "Match vanilla walk speed", "Advanced", Kind::Bool, 1, 0, 1, 1,
        "Pick the unit scale so normal speed (190 u/s) equals SR2's vanilla max ground speed (10 m/s in 1.3.0 -> "
        "0.0526 m/u). Speeds, gravity and jump height all scale together, so the movement is HL2's at SR2 scale. "
        "While on, the manual unit scale below is ignored; moving that slider turns this off.");
    add(kUnitScale, "Unit scale (m per unit)", "Advanced", Kind::Float, 0.0254, 0.005, 0.2, 0.0001,
        "Metres per Source unit (0.0254 = 1 inch, HL2's real size: 190 u/s = 4.8 m/s). Scales all speeds, gravity "
        "and jump height together. Changing it turns 'Match vanilla walk speed' off.");
    add(kMomentumHud, "Momentum HUD", "HUD", Kind::Bool, 1, 0, 1, 1,
        "Small horizontal speed readout (Source u/s) just below the crosshair: neutral while steady, green-tinted while "
        "gaining speed, red-tinted while losing. Hidden in menus, the map and the mod settings.");
    add(kMomentumDetails, "Momentum HUD: details", "HUD", Kind::Bool, 0, 0, 1, 1,
        "Second, smaller line: speed at your last take-off (pre-speed, tinted vs the hop before) and strafe sync % "
        "(share of air ticks that gained speed; live while airborne).");
    add(kMomentumFade, "Momentum HUD: fade when still", "HUD", Kind::Bool, 1, 0, 1, 1,
        "Fade the readout out after standing still for a moment; it fades back in when you move.");
    add(kMomentumBar, "Momentum HUD: underline bar", "HUD", Kind::Bool, 0, 0, 1, 1,
        "Thin bar under the number, full at 2x sprint speed.");
    // Placement (the overlay shows a "Move" button for these, ui/hud_edit.h).
    const bool migrate = !settings::Saved(kModule, kMomentumY) &&
                         (settings::Saved(kModule, kOldMomentumY) || settings::Saved(kModule, kOldMomentumScale));
    add(kMomentumX, "Momentum HUD: horizontal position (% of free space)", "HUD", Kind::Float, 50, 0, 100, 0.01,
        "0 = left edge, 100 = right edge. Set with Move.");
    add(kMomentumY, "Momentum HUD: vertical position (% of free space)", "HUD", Kind::Float, 57.1, 0, 100, 0.01,
        "0 = top edge, 100 = bottom edge. Set with Move.");
    add(kMomentumSize, "Momentum HUD: size (% of screen height)", "HUD", Kind::Float, 1.6, 0.8, 8, 0.01,
        "Height of the speed number. Set with Move (drag a corner).");
    if (migrate) MigrateMomentumPlacement();

    add(kSpeedReadout, "Fast-movement log", "Advanced", Kind::Bool, 1, 0, 1, 1,
        "Once per second while faster than 2x normal speed, log (Info) the speed in u/s and m/s, grounded state and "
        "which limiter (friction, speed limit, sv_maxvelocity, boost clamp, ...) touched the velocity.");
    add(kDebugLog, "Debug speed log", "Advanced", Kind::Bool, 0, 0, 1, 1,
        "Log speed, input and tick counts once per second (needs log_level debug).");

    // Visibility parents (settings::Def::parent): both settings UIs hide these
    // while their parent is off (or, with a value, while it differs from it).
    settings::SetParent(kModule, kGravity, kGravityMode, 0);  // HL2 gravity mode only
    settings::SetParent(kModule, kGravityMult, kGravityMode, 0);
    settings::SetParent(kModule, kJumpHeight, kGravityMode, 0);  // Vanilla SR2 ignores it
    settings::SetParent(kModule, kSurfLimit, kRampPhysics);
    settings::SetParent(kModule, kAutobhopKey, kAutobhop);
    settings::SetParent(kModule, kScrollDir, kScrollJump);
    settings::SetParent(kModule, kSprintDrain, kHl2Sprint);
    settings::SetParent(kModule, kSpeedLimit, kSpeedLimitOn);
    settings::SetParent(kModule, kUnitScale, kMatchVanilla, 0);  // ignored while matching
    for (const char* k : {kMomentumDetails, kMomentumFade, kMomentumBar, kMomentumX, kMomentumY, kMomentumSize})
        settings::SetParent(kModule, k, kMomentumHud);
}

// ----------------------------------------------------------------- module --

class Hl2Movement final : public sr2::core::Module {
public:
    Hl2Movement() {
        RegisterSettings();
        LoadCfg();
        g_cfgDirty.store(false);
        settings::OnChange(kModule, [](const std::string& key) {
            g_cfgDirty.store(true);
            // Moving the unit scale slider means "use this scale": matching
            // would silently ignore it, so turn matching off.
            if (key == kUnitScale && settings::GetBool(kModule, kMatchVanilla)) {
                SR2_INFO("hl2_movement: unit scale changed - turning 'Match vanilla walk speed' off so it applies");
                settings::Set(kModule, kMatchVanilla, 0);
            }
        });
    }

    const char* Name() const override { return kModule; }
    bool DefaultEnabled() const override { return false; }
    bool Setup(sr2::core::ModuleContext& ctx) override;
    void OnEnable() override;
    void OnDisable() override;

private:
    int frameId_ = 0;
};

bool Hl2Movement::Setup(sr2::core::ModuleContext& ctx) {
#ifndef NDEBUG
    {
        std::string report;
        const bool ok = mv::SelfTest(&report);
        if (ok) SR2_INFO("hl2_movement: self-test: {}", report);
        else SR2_WARN("hl2_movement: self-test: {}", report);
    }
#endif
    auto& api = sr2::il2cpp::api();
    if (!api.il2cpp_object_get_class) return ctx.Fail("il2cpp_object_get_class export missing");
    Resolver& res = ctx.resolver();

    // Classes.
    R.kfc = ctx.Class(kImage, kNsKfc, "KFCCharacterController");
    R.srcc = ctx.Class(kImage, kNsCc, "SRCharacterController");
    R.motor = ctx.Class(kImage, kNsKcc, "KinematicCharacterMotor");
    Il2CppClass* grounding = ctx.Class(kImage, kNsKcc, "CharacterGroundingReport");
    R.jumpB = ctx.Class(kImage, kNsAbil, "JumpAbilityBehavior");
    R.sprintB = ctx.Class(kImage, kNsAbil, "SprintAbilityBehavior");
    R.jetpackB = ctx.Class(kImage, kNsAbil, "JetpackAbilityBehavior");
    R.pulseB = res.Class(kImage, kNsAbil, "PulseWaveAbilityBehavior");  // optional
    Il2CppClass* jumpData = ctx.Class(kImage, kNsAbil, "JumpAbilityData");
    Il2CppClass* abilityData = ctx.Class(kImage, kNsKfc, "AbilityData");
    Il2CppClass* inputEvent = ctx.Class(kImage, kNsInput, "InputEvent");
    Il2CppClass* inputEventData = ctx.Class(kImage, kNsInput, "InputEventData");
    Il2CppClass* unitResource = ctx.Class(kImage, kNsUnitProp, "UnitResource");
    if (!R.kfc || !R.srcc || !R.motor || !grounding || !R.jumpB || !R.sprintB || !R.jetpackB || !jumpData ||
        !abilityData || !inputEvent || !inputEventData || !unitResource)
        return false;

    // Fields.
    bool ok = ctx.FieldOffset(R.kfc, "_motor", R.kfcMotor) && ctx.FieldOffset(R.kfc, "AbilityBehaviors", R.kfcAbilities) &&
              ctx.FieldOffset(R.kfc, "LookVector", R.kfcLook) && ctx.FieldOffset(R.kfc, "MovementVector", R.kfcMovement) &&
              ctx.FieldOffset(R.kfc, "<InputVector>k__BackingField", R.kfcInput) &&
              ctx.FieldOffset(R.kfc, "<BypassGravity>k__BackingField", R.kfcBypassGravity) &&
              ctx.FieldOffset(R.srcc, "_currentEnergy", R.srccEnergy) &&
              ctx.FieldOffset(R.motor, "GroundingStatus", R.motorGrounding) &&
              ctx.FieldOffset(R.motor, "BaseVelocity", R.motorBaseVelocity) &&
              ctx.FieldOffset(R.motor, "_transientPosition", R.motorPos) &&
              ctx.FieldOffset(grounding, "FoundAnyGround", R.grFoundAny) &&
              ctx.FieldOffset(grounding, "IsStableOnGround", R.grStable) &&
              ctx.FieldOffset(grounding, "GroundNormal", R.grNormal) &&
              ctx.FieldOffset(R.jumpB, "AbilityData", R.jumpData) &&
              ctx.FieldOffset(R.jumpB, "<IsActive>k__BackingField", R.jumpActive) &&
              ctx.FieldOffset(R.sprintB, "AbilityData", R.sprintData) &&
              ctx.FieldOffset(R.sprintB, "<IsActive>k__BackingField", R.sprintActive) &&
              ctx.FieldOffset(R.jetpackB, "<IsActive>k__BackingField", R.jetpackActive) &&
              ctx.FieldOffset(abilityData, "_startInputEvent", R.adStart) &&
              ctx.FieldOffset(abilityData, "_alternativeStartInputEvent", R.adAltStart) &&
              ctx.FieldOffset(abilityData, "_stopInputEvent", R.adStop) &&
              ctx.FieldOffset(abilityData, "_alternativeStopInputEvent", R.adAltStop) &&
              ctx.FieldOffset(jumpData, "_recurrenceDelay", R.jdRecurrence) &&
              ctx.FieldOffset(jumpData, "_jumpUpSpeed", R.jdUpSpeed) &&
              ctx.FieldOffset(unitResource, "_unitResourceValue", R.urValue);
    if (!ok) return false;
    // Value-type field offsets come back boxed (header included).
    if (R.grFoundAny < kObjectHeader || R.grStable < kObjectHeader || R.grNormal < kObjectHeader)
        return ctx.Fail("CharacterGroundingReport field offsets look wrong");
    R.grFoundAny -= kObjectHeader;
    R.grStable -= kObjectHeader;
    R.grNormal -= kObjectHeader;

    // Optional: the jump's "press received" flag (landing-tick friction skip)
    // and InputEvent.PollValue (physical held state for sprint / jump).
    if (auto o = res.FieldOffset(R.jumpB, "StartAbilityEventReceived")) R.jumpStartReq = *o;
    else SR2_WARN("hl2_movement: StartAbilityEventReceived not found - landing ticks keep friction before SR2 jumps");
    if (auto o = res.FieldOffset(inputEvent, "PollValue")) R.iePoll = *o;
    else SR2_WARN("hl2_movement: InputEvent.PollValue not found - jump/sprint held state from input events only");

    // Optional: ramp physics (slope limit, forced-unground state) and the
    // vanilla jump data for the apex estimate in the log.
    auto opt = [&](Il2CppClass* k, const char* name, size_t& out) {
        if (auto o = res.FieldOffset(k, name)) out = *o;
        else SR2_WARN("hl2_movement: optional field {} not found", name);
    };
    opt(R.motor, "MaxStableSlopeAngle", R.motorMaxSlope);
    opt(R.motor, "_mustUnground", R.motorMustUnground);
    opt(R.motor, "_mustUngroundTimeCounter", R.motorMustUngroundTime);
    opt(jumpData, "_airborneForceDamping", R.jdAirDamp);
    opt(jumpData, "_initialHoldVelocity", R.jdInitHold);
    opt(jumpData, "_holdVelocityDamping", R.jdHoldDamp);
    opt(jumpData, "_verticalVelocityStopThreshold", R.jdStopThr);

    // Optional: vanilla tuning values for the log / "match vanilla walk speed".
    if (Il2CppClass* params = res.Class(kImage, kNsKfc, "CharacterControllerParameters")) {
        auto a = res.FieldOffset(R.kfc, "_parameters");
        auto b = res.FieldOffset(params, "_maxGroundedMoveSpeed");
        auto c = res.FieldOffset(params, "_gravityMagnitude");
        if (a && b && c) {
            R.kfcParams = *a;
            R.cpMaxGround = *b;
            R.cpGravity = *c;
            R.haveParams = true;
            if (auto d = res.FieldOffset(params, "_gravityAccumulationSharpness")) R.cpGravAccum = *d;
        }
    }

    // Optional: "game menu open" signals for the momentum HUD (static icalls).
    {
        const char* core = "UnityEngine.CoreModule";
        auto getter = [&](const char* cls, const char* name, StaticGetter& out) {
            std::string err;
            Il2CppClass* k = res.Class(core, "UnityEngine", cls, &err);
            const MethodInfo* m = k ? res.Method(k, name, 0, &err) : nullptr;
            void* p = m ? res.MethodPointer(m, &err) : nullptr;
            if (!p) {
                SR2_WARN("hl2_movement: optional {}.{} unavailable ({}) - momentum HUD menu check without it", cls, name,
                         err);
                return;
            }
            out.fn = p;
            out.mi = m;
        };
        getter("Cursor", "get_lockState", g_cursorLockState);
        getter("Time", "get_timeScale", g_timeScale);
    }

    // Methods we call.
    auto bind = [&](auto& call, Il2CppClass* k, const char* name, int argc, bool required) -> bool {
        std::string err;
        const MethodInfo* m = res.Method(k, name, argc, &err);
        void* p = m ? res.MethodPointer(m, &err) : nullptr;
        if (!p) {
            if (required) return ctx.Fail(err);
            SR2_WARN("hl2_movement: optional {} not available: {}", name, err);
            return false;
        }
        call.fn = reinterpret_cast<std::remove_reference_t<decltype(call.fn)>>(p);
        call.mi = m;
        return true;
    };
    if (!bind(R.forceUnground, R.motor, "ForceUnground", 1, true)) return false;
    if (!bind(R.setEnergy, unitResource, "set_Value", 1, true)) return false;
    bind(R.setPosition, R.motor, "SetPosition", 2, false);  // fall-through recovery only
    if (!bind(R.groundOnAir, R.srcc, "HandleGroundOnAirborneMovement", 2, false))
        bind(R.groundOnAir, R.kfc, "HandleGroundOnAirborneMovement", 2, false);

    // Hooks.
    auto hook = [&](const char* name, Il2CppClass* k, const char* method, int argc, void* detour, void** orig,
                    bool required) -> bool {
        std::string err;
        const MethodInfo* m = res.Method(k, method, argc, &err);
        void* p = m ? res.MethodPointer(m, &err) : nullptr;
        if (!p) {
            if (required) return ctx.Fail(err);
            SR2_WARN("hl2_movement: optional hook {} unavailable: {}", name, err);
            return false;
        }
        SR2_DEBUG("hl2_movement: hook {} -> GameAssembly+0x{:X}", name, Resolver::Rva(p));
        return ctx.HookAddress(name, p, detour, orig);
    };
    auto V = [](auto& o) { return reinterpret_cast<void**>(&o); };
    ok = hook("UpdateVelocity", R.kfc, "UpdateVelocity", 2, reinterpret_cast<void*>(&D_UpdateVelocity),
              V(o_UpdateVelocity), true) &&
         hook("HandleStablyGroundedVelocity", R.kfc, "HandleStablyGroundedVelocity", 2,
              reinterpret_cast<void*>(&D_GroundVel), V(o_GroundVel), true) &&
         hook("HandleStablyGroundedDrag", R.kfc, "HandleStablyGroundedDrag", 2, reinterpret_cast<void*>(&D_GroundDrag),
              V(o_GroundDrag), true) &&
         hook("HandleHorizontalAirborneMovement", R.kfc, "HandleHorizontalAirborneMovement", 3,
              reinterpret_cast<void*>(&D_AirMove), V(o_AirMove), true) &&
         hook("HandleAirborneDrag", R.kfc, "HandleAirborneDrag", 3, reinterpret_cast<void*>(&D_AirDrag), V(o_AirDrag),
              true) &&
         hook("HandleGravity", R.kfc, "HandleGravity", 2, reinterpret_cast<void*>(&D_Gravity), V(o_Gravity), true) &&
         hook("ClampAirborneHorizontalVelocity", R.kfc, "ClampAirborneHorizontalVelocity", 2,
              reinterpret_cast<void*>(&D_AirClamp), V(o_AirClamp), true) &&
         hook("JumpAbilityBehavior.UpdateVelocity", R.jumpB, "UpdateVelocity", 2,
              reinterpret_cast<void*>(&D_JumpUpdVel), V(o_JumpUpdVel), true) &&
         hook("JumpAbilityBehavior.Start", R.jumpB, "Start", 0, reinterpret_cast<void*>(&D_JumpStart), V(o_JumpStart),
              true) &&
         hook("SprintAbilityBehavior.Update", R.sprintB, "Update", 1, reinterpret_cast<void*>(&D_SprintUpdate),
              V(o_SprintUpdate), true);
    if (!ok) return false;

    // Optional: airborne ClipVelocity in KCC's collision projection (ramp
    // physics). Every KCC motor's hits pass through it; the detour is a
    // pointer compare for anything but the player's motor.
    R.velProjHook = hook("KinematicCharacterMotor.HandleVelocityProjection", R.motor, "HandleVelocityProjection", 3,
                         reinterpret_cast<void*>(&D_VelProj), V(o_VelProj), false);

    // Hooks that take InputEventData by value: only if its layout is the
    // 12-byte {int Source; Vector2 _vecValue} the detours are declared with.
    bool dataOk = false;
    {
        auto src = res.FieldOffset(inputEventData, "Source");
        auto vec = res.FieldOffset(inputEventData, "_vecValue");
        using ValueSizeFn = int32_t (*)(Il2CppClass*, uint32_t*);
        auto valueSize = reinterpret_cast<ValueSizeFn>(GetProcAddress(api.module, "il2cpp_class_value_size"));
        const int32_t size = valueSize ? valueSize(inputEventData, nullptr) : -1;
        dataOk = src && vec && *src == kObjectHeader && *vec == kObjectHeader + 4 &&
                 (size == static_cast<int32_t>(sizeof(InputEventDataRaw)) || size == -1);
        if (!dataOk)
            SR2_WARN("hl2_movement: InputEventData layout changed (size {}), jump/sprint hold fallback / scroll-wheel jump disabled",
                     size);
    }
    if (dataOk) {
        R.inputHooks = hook("InputEvent.StartInput", inputEvent, "StartInput", 1,
                            reinterpret_cast<void*>(&D_InputStart), V(o_InputStart), false) &&
                       hook("InputEvent.Cancel", inputEvent, "Cancel", 1, reinterpret_cast<void*>(&D_InputCancel),
                            V(o_InputCancel), false);

        int slots = 0;
        if (Il2CppClass* vac = res.Class(kImage, kNsItems, "VacuumItem")) {
            slots += hook("VacuumItem.NextSlot", vac, "NextSlot", 1, reinterpret_cast<void*>(&SlotHook<0>::Detour),
                          V(SlotHook<0>::orig), false);
            slots += hook("VacuumItem.PreviousSlot", vac, "PreviousSlot", 1,
                          reinterpret_cast<void*>(&SlotHook<1>::Detour), V(SlotHook<1>::orig), false);
        }
        if (Il2CppClass* bar = res.Class(kImage, kNsGadgetUi, "GadgetFavoritesBar")) {
            slots += hook("GadgetFavoritesBar.NextSlot", bar, "NextSlot", 1,
                          reinterpret_cast<void*>(&SlotHook<2>::Detour), V(SlotHook<2>::orig), false);
            slots += hook("GadgetFavoritesBar.PreviousSlot", bar, "PreviousSlot", 1,
                          reinterpret_cast<void*>(&SlotHook<3>::Detour), V(SlotHook<3>::orig), false);
        }
        R.scrollHooks = slots >= 2;
        if (slots < 4) SR2_WARN("hl2_movement: only {}/4 wheel slot hooks installed", slots);
    }
    if (!R.inputHooks && !R.iePoll) SR2_WARN("hl2_movement: jump/sprint hold tracking unavailable (HL2 sprint hold needs it)");

    SR2_INFO(
        "hl2_movement: set up (input events {}, held poll {}, landing jump queue {}, scroll-wheel jump {}, recovery {}, "
        "wall projection {}, airborne clip {}, slope limit {})",
        R.inputHooks ? "on" : "off", R.iePoll ? "on" : "off", R.jumpStartReq ? "on" : "off",
        R.scrollHooks ? "on" : "off", R.setPosition.fn ? "on" : "off", R.groundOnAir.fn ? "on" : "off",
        R.velProjHook ? "on" : "off", R.motorMaxSlope ? "on" : "off");
    return true;
}

void Hl2Movement::OnEnable() {
    auto start = [] {
        ResetRuntimeState();
        g_cfgDirty.store(true);
        g_active.store(true);
    };
    if (main_thread::Ready() && !main_thread::IsMainThread()) main_thread::Post(start);
    else start();
    if (frameId_ == 0) frameId_ = main_thread::AddFrameCallback(kModule, [](float dt) { Frame(dt); });
    if (frameId_ == 0) SR2_WARN("hl2_movement: frame callback unavailable (HL2 sprint drain / debug log off)");
}

void Hl2Movement::OnDisable() {
    g_active.store(false);
    sr2::ui::momentum_hud::Clear();
    if (frameId_) {
        main_thread::RemoveFrameCallback(frameId_);
        frameId_ = 0;
    }
    auto stop = [] {
        ApplyRecurrencePatch(false);
        ApplySlopePatch(nullptr, false);
        ResetRuntimeState();
    };
    if (main_thread::Ready() && !main_thread::IsMainThread()) main_thread::Post(stop);
    else stop();
}

}  // namespace

std::unique_ptr<sr2::core::Module> CreateHl2Movement() { return std::make_unique<Hl2Movement>(); }

}  // namespace sr2::modules
