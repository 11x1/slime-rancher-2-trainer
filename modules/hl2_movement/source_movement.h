#pragma once
// Pure port of the Half-Life 2 player movement math from Source SDK 2013
// (src/game/shared/gamemovement.cpp, CGameMovement::Friction / Accelerate /
// AirAccelerate / CheckJumpButton / ClipVelocity / CheckVelocity, and the HL2
// forward jump boost from the HL2_DLL block of CheckJumpButton).
//
// No game dependencies. Everything is in Source units (1 unit ~ 1 inch) and
// Source axes: x/y horizontal, z up. The module converts to Unity metres.

#include <string>

namespace sr2::modules::hl2mv {

struct Vec3 {
    float x = 0, y = 0, z = 0;
};

inline Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3 operator*(Vec3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline float Dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
float Length(Vec3 v);
float Length2D(Vec3 v);
// Normalizes in place, returns the old length (VectorNormalize semantics).
float Normalize(Vec3& v);
inline Vec3 Flat(Vec3 v) { return {v.x, v.y, 0.0f}; }

// Source SDK 2013 / HL2 defaults (movevars_shared.cpp, gamemovement.h, hl2_player.cpp).
inline constexpr float kSvAccelerate = 10.0f;
inline constexpr float kSvAirAccelerate = 10.0f;
inline constexpr float kSvFriction = 4.0f;
inline constexpr float kSvStopSpeed = 100.0f;
inline constexpr float kSvMaxVelocity = 3500.0f;
inline constexpr float kSvGravity = 600.0f;           // HL2 (movevars_shared.cpp)
inline constexpr float kJumpHeight = 21.0f;           // GAMEMOVEMENT_JUMP_HEIGHT
inline constexpr float kJumpImpulse = 160.0f;         // g_bMovementOptimizations path (~sqrt(2*600*21))
inline constexpr float kAirSpeedCap = 30.0f;          // CGameMovement::GetAirSpeedCap()
inline constexpr float kWalkSpeed = 150.0f;           // hl2_walkspeed
inline constexpr float kNormSpeed = 190.0f;           // hl2_normspeed
inline constexpr float kSprintSpeed = 320.0f;         // hl2_sprintspeed
inline constexpr float kSprintDrainPerSec = 12.5f;    // SuitDeviceSprint drain, of 100 units
inline constexpr float kSprintMinStartPower = 10.0f;  // CHL2_Player::StartSprinting refuses below 10
inline constexpr float kNonJumpVelocity = 140.0f;     // NON_JUMP_VELOCITY: faster upwards = not on ground
inline constexpr float kMinWalkNormal = 0.7f;         // CategorizePosition: plane.normal[2] < 0.7 is not ground

struct MoveVars {
    float accelerate = kSvAccelerate;
    float airaccelerate = kSvAirAccelerate;
    float friction = kSvFriction;
    float stopspeed = kSvStopSpeed;
    float maxvelocity = kSvMaxVelocity;
    float surfaceFriction = 1.0f;  // player->m_surfaceFriction
    float airSpeedCap = kAirSpeedCap;
};

// CGameMovement::CheckParameters: scale forward/side move down so their
// combined length does not exceed maxspeed.
void ClampMove(float& forwardmove, float& sidemove, float maxspeed);

// WalkMove / AirMove wish vector: forward/right are flattened + normalized,
// wishspeed is clamped to maxspeed. wishdir is normalized (or zero).
void BuildWish(Vec3 forward, Vec3 right, float forwardmove, float sidemove, float maxspeed, Vec3& wishdir,
               float& wishspeed);

// CGameMovement::Friction (grounded). Uses the full velocity length like Source.
void Friction(Vec3& vel, const MoveVars& mv, float dt);
// CGameMovement::Accelerate.
void Accelerate(Vec3& vel, Vec3 wishdir, float wishspeed, float accel, float dt, float surfaceFriction);
// CGameMovement::AirAccelerate: `addspeed` uses min(wishspeed, airSpeedCap),
// the step size uses the full wishspeed. This is what makes air-strafing gain speed.
void AirAccelerate(Vec3& vel, Vec3 wishdir, float wishspeed, float accel, float dt, float surfaceFriction,
                   float airSpeedCap);
// CGameMovement::ClipVelocity (overbounce 1.0 = slide along the plane).
Vec3 ClipVelocity(Vec3 in, Vec3 normal, float overbounce);
// CGameMovement::CheckVelocity: per-axis clamp to +-maxvelocity, NaN -> 0.
// Returns true if anything was changed.
bool CheckVelocity(Vec3& vel, float maxvelocity);

// One grounded tick, the friction + WalkMove part of FullWalkMove:
// z = 0, Friction, Accelerate, z = 0, and the "spd < 1 -> stop" rule.
void GroundMoveStep(Vec3& vel, Vec3 wishdir, float wishspeed, const MoveVars& mv, float dt);
// One airborne tick, the AirMove part (horizontal only, gravity is separate).
void AirMoveStep(Vec3& vel, Vec3 wishdir, float wishspeed, const MoveVars& mv, float dt);

// HL2 jump impulse for a given sv_gravity (160 u/s at 600, scaled with sqrt(g)).
float JumpImpulse(float gravity);

// Jump impulse that reaches exactly `height` (units) at `gravity` (u/s^2) when
// simulated the way KCC does it: the take-off tick moves with the full
// impulse, every later tick first subtracts gravity * tickDt and then moves
// (semi-implicit Euler). Its apex is (v0 + g*dt/2)^2 / (2g), so
// v0 = sqrt(2 g h) - g dt / 2. tickDt = 0 gives the continuous sqrt(2 g h).
float JumpImpulseForHeight(float gravity, float height, float tickDt);
// Apex height of that discrete simulation for a given impulse (self-test, log).
float DiscreteJumpApex(float impulse, float gravity, float tickDt, float* airTime = nullptr);

// ---- ramps (CGameMovement::TryPlayerMove / CategorizePosition / SetGroundEntity) --
// Source walkable plane: normal.z >= 0.7 (45.57 degrees).
inline bool IsWalkable(Vec3 n) { return n.z >= kMinWalkNormal; }
// CategorizePosition: moving up faster than NON_JUMP_VELOCITY = not on ground.
inline bool MovingUpRapidly(Vec3 v) { return v.z > kNonJumpVelocity; }
// Landing: the air tick that hits the ground clips the full 3D velocity
// against the plane (overbounce 1). SetGroundEntity then zeroes z, so the
// ground speed is the horizontal part of the result. Falling onto a down
// slope turns vertical speed into horizontal speed (ramp boost).
inline Vec3 LandingClip(Vec3 preLanding, Vec3 n) { return ClipVelocity(preLanding, n, 1.0f); }
// Vertical component Source gives a horizontal ground velocity on a plane
// (StepMove keeps the horizontal velocity of the step-up move and takes z
// from the clipped slide move): -(h . n) * n.z. If that exceeds
// NON_JUMP_VELOCITY the player leaves the ground (fast uphill running).
inline float GroundClipZ(Vec3 h, Vec3 n) { return -(h.x * n.x + h.y * n.y) * n.z; }

// Jump boost variants (CheckJumpButton, HL2_DLL block).
enum class BoostMode : int {
    // |forwardmove| * perc added along the view forward (sign flipped when
    // forwardmove < 0), never clamped: uncapped forward bunnyhopping.
    Uncapped = 0,
    // Exact Source SDK 2013 code including the clamp
    //   flMaxSpeed = maxspeed + maxspeed * perc
    //   if (flNewSpeed > flMaxSpeed) flSpeedAddition -= flNewSpeed - flMaxSpeed;
    // Above flMaxSpeed the "boost" turns negative along the view forward, so
    // hopping backwards (velocity behind the view) speeds you up: ABH.
    VanillaClamp = 1,
    // Uncapped while the horizontal velocity points forward relative to the
    // view (dot >= 0), the exact vanilla (clamped) formula when it points
    // backwards: uncapped forward hops plus HL2's accelerated back hopping.
    UncappedAbh = 2,
    // ABH both ways: the exact vanilla (clamped) formula, mirrored while the
    // horizontal velocity points forward relative to the view (dot >= 0).
    // Above flMaxSpeed the vanilla addition is negative (speed - flMaxSpeed
    // against the view); here it is applied along the view instead, so a
    // forward hop gains speed - flMaxSpeed exactly like a back hop does
    // (exponential up to sv_maxvelocity), with no key or W held. Below
    // flMaxSpeed, and while travelling backwards, it is the vanilla formula.
    AbhBothWays = 3,
};

// HL2 jump boost. forwardFlat is the view forward (z is dropped and it is
// normalized here). forwardmove is the clamped forward input in units/s,
// maxspeed the current mv->m_flMaxSpeed. Returns true if the vanilla clamp
// changed the addition (for the limiter readout).
bool ApplyJumpBoost(Vec3& vel, Vec3 forwardFlat, float forwardmove, float maxspeed, bool sprinting, BoostMode mode);

// Full jump as CheckJumpButton does it from the ground: z += impulse (z is 0
// on the ground), then the boost. Returns ApplyJumpBoost's result.
bool Jump(Vec3& vel, float impulse, Vec3 forwardFlat, float forwardmove, float maxspeed, bool sprinting,
          BoostMode mode);

// Self-test: strafe-jump, back-hop and forward-hop simulations for all boost
// modes (strafe-jumping gains speed uncapped and stays bounded with the clamp;
// back-hopping gains speed only in the ABH modes; forward-hopping with no keys
// gains speed only in AbhBothWays, exactly like the back hop), ramp landing
// (30 degree down slope: ramp boost), surfing (60 degree ramp keeps speed),
// jump apex for a height setting, plus helper checks.
// Returns true on success; `report` gets a one-line summary.
bool SelfTest(std::string* report);

}  // namespace sr2::modules::hl2mv
