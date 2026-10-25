// sandbox: noclip (see noclip.h and NOTES.md, "Noclip"). SR2 1.3.0.
//
// Game facts (re/player.md "Noclip"):
//   * The player: SceneContext.player (GameObject) carries the SRCharacterController
//     (: KFCCharacterController); its _motor is the KinematicCharacterMotor that KCC
//     ticks at 40 Hz (UpdatePhase1 / UpdatePhase2 -> UpdateVelocity(ref BaseVelocity)).
//   * KinematicCharacterMotor._solveMovementCollisions / _solveGrounding are what
//     SetMovementCollisionsSolvingActivation / SetGroundSolvingActivation write (16-byte
//     setters, nothing else). With both off a tick only does transient += BaseVelocity * dt
//     and the grounding report stays empty (airborne branch every tick).
//   * KFCCharacterController.HandleGravity does nothing while <BypassGravity> is set (the
//     jetpack uses it); <GravityAmount> is the accumulated gravity.
//   * KinematicCharacterMotor.SetPosition(pos, true) sets the transform, the transient and
//     initial simulation positions and InitialTickPosition, so the KCC interpolation
//     starts from there.
//
// Per frame while on: BaseVelocity = 0, gravity bypassed, both solvers off, then the
// motor's transient position + this frame's move goes through SetPosition. What a KCC
// tick adds in between (one tick of air control from the movement input) is tiny and
// zeroed again next frame. Rotation and mouse look are untouched (the motor still runs).
//
// Kill volumes (the slime sea and fall-through volumes: KillOnTrigger; the Labyrinth cloud ocean:
// KillOnTriggerCloudOcean) still see the player's capsule, so flying into them knocked the player out.
// Both end in the static DeathHandler.Kill(GameObject, Damage) -> IDeathHandler (PlayerDeathHandler.OnDeath);
// while noclip is on, a Kill of the player is dropped there, and KillOnTrigger.OnTriggerEnter for the
// player's main collider is skipped too (no "Fallthrough destroying" log line, no splash FX).

#include "modules/sandbox/noclip.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <format>
#include <initializer_list>
#include <string>
#include <vector>

#include "core/detour_guard.h"
#include "core/il2cpp_api.h"
#include "core/log.h"
#include "core/module.h"
#include "core/resolver.h"
#include "core/settings.h"
#include "core/unity.h"
#include "modules/common/noclip_state.h"
#include "modules/sandbox/player_extras.h"
#include "ui/overlay.h"
#include "ui/sandbox_bridge.h"

namespace sr2::modules::sandbox {
namespace {

namespace unity = sr2::core::unity;
namespace settings = sr2::core::settings;
using sr2::il2cpp::api;
using sr2::il2cpp::Resolver;

constexpr const char* kModule = "sandbox";
constexpr const char* kImage = "Assembly-CSharp";
constexpr const char* kKeyToggle = "noclip_key";
constexpr const char* kKeyUp = "noclip_up_key";
constexpr const char* kKeyDown = "noclip_down_key";
constexpr const char* kKeySpeed = "noclip_speed";
constexpr const char* kKeyFast = "noclip_fast";
constexpr unsigned long long kSceneCheckMs = 250;  // scene group / player checks while on
constexpr int kCursorLocked = 1;                     // UnityEngine.CursorLockMode.Locked

struct V2 {
    float x, y;
};
struct V3 {
    float x, y, z;
};

std::string g_why;  // "" = available (SetupNoclip)

// ---- resolved in SetupNoclip ----
Il2CppClass* g_srccClass = nullptr;               // SRCharacterController
const MethodInfo* g_tryGetSceneContext = nullptr; // SceneContextUtility.TryGetSceneContext(out SceneContext)
size_t g_offScPlayer = 0;                         // SceneContext.player (GameObject)
size_t g_offScRegions = 0;                        // SceneContext.<RegionRegistry>k__BackingField
const MethodInfo* g_currentGroup = nullptr;       // RegionRegistry.get_CurrentSceneGroup
const MethodInfo* g_inChildren = nullptr;         // GameObject.GetComponentInChildren(Type) (optional)
size_t g_offMotor = 0;                            // KFCCharacterController._motor
size_t g_offBypass = 0;                           // KFCCharacterController.<BypassGravity>k__BackingField
size_t g_offGravity = 0;                          // KFCCharacterController.<GravityAmount>k__BackingField
size_t g_offInput = 0;                            // KFCCharacterController.<InputVector>k__BackingField (optional)
size_t g_offTransient = 0;                        // KinematicCharacterMotor._transientPosition
size_t g_offSolveMove = 0;                        // KinematicCharacterMotor._solveMovementCollisions
size_t g_offSolveGround = 0;                      // KinematicCharacterMotor._solveGrounding
size_t g_offBaseVel = 0;                          // KinematicCharacterMotor.BaseVelocity
const MethodInfo* g_setPosition = nullptr;        // KinematicCharacterMotor.SetPosition(Vector3, bool)
const MethodInfo* g_trForward = nullptr;          // Transform.get_forward
const MethodInfo* g_trRight = nullptr;            // Transform.get_right
const MethodInfo* g_lockState = nullptr;          // Cursor.get_lockState (optional)
const MethodInfo* g_isPlayerCollider = nullptr;   // PhysicsUtil.IsPlayerMainCollider(Collider) (optional)

// ---- kill volume hooks (optional; toggle with the module) ----
using KillFn = void (*)(Il2CppObject*, Il2CppObject*, const MethodInfo*);
using TriggerFn = void (*)(Il2CppObject*, Il2CppObject*, const MethodInfo*);
KillFn g_origKill = nullptr;
TriggerFn g_origKillTrigger = nullptr;
sr2::core::DetourGuard g_killGuard{kModule, "DeathHandler.Kill"};
sr2::core::DetourGuard g_killTriggerGuard{kModule, "KillOnTrigger.OnTriggerEnter"};
bool g_loggedKillSkip = false;  // once per noclip session

// ---- state (main thread) ----
bool g_on = false;
Il2CppGCHandle g_ctrl = nullptr;      // the noclipping SRCharacterController
Il2CppObject* g_group = nullptr;      // the scene group it was switched on in (compared only, an asset)
bool g_savedMove = true, g_savedGround = true, g_savedBypass = false;
bool g_keyWas = false;                // toggle key held last frame
unsigned long long g_lastCheck = 0;
bool g_loggedMoveFail = false;

Il2CppObject* Call(const MethodInfo* m, void* obj, std::initializer_list<void*> args, const char* what, bool* ok = nullptr) {
    if (ok) *ok = false;
    if (!m || sr2::core::ModuleFaulted(kModule)) return nullptr;
    std::vector<void*> params(args);
    std::string err;
    Il2CppObject* r = Resolver::Get().Invoke(m, obj, params.empty() ? nullptr : params.data(), &err);
    if (!err.empty()) {
        if (err.rfind("native exception", 0) == 0) sr2::core::MarkModuleFaulted(kModule, std::format("{}: {}", what, err));
        else SR2_WARN("[{}] noclip: {} failed: {}", kModule, what, err);
        return nullptr;
    }
    if (ok) *ok = true;
    return r;
}

template <class T>
bool CallValue(const MethodInfo* m, void* obj, std::initializer_list<void*> args, const char* what, T& out) {
    bool ok = false;
    Il2CppObject* r = Call(m, obj, args, what, &ok);
    if (!ok || !r) return false;
    out = *reinterpret_cast<T*>(reinterpret_cast<char*>(r) + 2 * sizeof(void*));
    return true;
}

bool IsA(Il2CppObject* obj, Il2CppClass* k) {
    const auto& a = api();
    if (!obj || !k || !a.il2cpp_object_get_class) return false;
    Il2CppClass* c = a.il2cpp_object_get_class(obj);
    return c == k || (c && a.il2cpp_class_is_assignable_from && a.il2cpp_class_is_assignable_from(k, c));
}

Il2CppObject* SceneContext() {
    Il2CppObject* sc = nullptr;
    bool found = false;
    if (!CallValue(g_tryGetSceneContext, nullptr, {&sc}, "SceneContextUtility.TryGetSceneContext", found) || !found ||
        !sc || !unity::IsAlive(sc))
        return nullptr;
    return sc;
}

Il2CppObject* CurrentGroup(Il2CppObject* sc) {
    Il2CppObject* regions = sc ? unity::Field<Il2CppObject*>(sc, g_offScRegions) : nullptr;
    return regions && unity::IsAlive(regions) ? Call(g_currentGroup, regions, {}, "RegionRegistry.CurrentSceneGroup") : nullptr;
}

// The player's SRCharacterController (on SceneContext.player, or below it), or nullptr with `why`.
Il2CppObject* FindController(Il2CppObject* sc, std::string& why) {
    Il2CppObject* go = sc ? unity::Field<Il2CppObject*>(sc, g_offScPlayer) : nullptr;
    if (!go || !unity::IsAlive(go)) {
        why = "No player right now (load a save first).";
        return nullptr;
    }
    Il2CppObject* ctrl = unity::GetComponent(go, g_srccClass);
    if (!ctrl && g_inChildren) {
        Il2CppObject* type = unity::TypeOf(g_srccClass);
        if (type) ctrl = Call(g_inChildren, go, {type}, "GameObject.GetComponentInChildren");
    }
    if (!ctrl || !IsA(ctrl, g_srccClass) || !unity::IsAlive(ctrl)) {
        why = "The player's character controller was not found.";
        return nullptr;
    }
    Il2CppObject* motor = unity::Field<Il2CppObject*>(ctrl, g_offMotor);
    if (!motor || !unity::IsAlive(motor)) {
        why = "The player's character motor was not found.";
        return nullptr;
    }
    return ctrl;
}

bool GameHasFocus() {
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}

bool KeyDown(int vk) { return vk > 0 && vk < 256 && (GetAsyncKeyState(vk) & 0x8000) != 0; }

// Gameplay has the keyboard: the game window in front, the overlay closed, and (when readable) the cursor locked,
// i.e. no game menu, map or text field open.
bool Playing() {
    if (sr2::ui::overlay::IsOpen() || !GameHasFocus()) return false;
    int mode = kCursorLocked;
    if (g_lockState && !CallValue(g_lockState, nullptr, {}, "Cursor.get_lockState", mode)) return false;
    return mode == kCursorLocked;
}

void Notice(const std::string& text) { PlayerExtrasNotice(text); }

// Holds the motor in noclip mode: what a KCC tick may have changed is put back every frame.
void Hold(Il2CppObject* ctrl, Il2CppObject* motor) {
    unity::Field<bool>(motor, g_offSolveMove) = false;
    unity::Field<bool>(motor, g_offSolveGround) = false;
    unity::Field<V3>(motor, g_offBaseVel) = V3{0, 0, 0};
    unity::Field<bool>(ctrl, g_offBypass) = true;
    unity::Field<float>(ctrl, g_offGravity) = 0.0f;
}

void Stop(const char* reason, bool notice) {
    if (!g_on) return;
    g_on = false;
    sr2::modules::common::NoclipFlag().store(false);
    Il2CppObject* ctrl = g_ctrl ? unity::GCHandleTarget(g_ctrl) : nullptr;
    Il2CppObject* motor = ctrl && unity::IsAlive(ctrl) ? unity::Field<Il2CppObject*>(ctrl, g_offMotor) : nullptr;
    const bool restored = motor && unity::IsAlive(motor);
    if (restored) {
        // Field writes only (no game call), so this also works with the module faulted.
        unity::Field<bool>(motor, g_offSolveMove) = g_savedMove;
        unity::Field<bool>(motor, g_offSolveGround) = g_savedGround;
        unity::Field<V3>(motor, g_offBaseVel) = V3{0, 0, 0};
        unity::Field<bool>(ctrl, g_offBypass) = g_savedBypass;
        unity::Field<float>(ctrl, g_offGravity) = 0.0f;
    }
    if (g_ctrl) unity::FreeGCHandle(g_ctrl);
    g_ctrl = nullptr;
    g_group = nullptr;
    SR2_INFO("[{}] noclip off ({}){}", kModule, reason,
             restored ? ": collision and gravity back" : ": the player object is gone, nothing to restore");
    if (notice) Notice("Noclip off");
}

void Start() {
    std::string why = NoclipWhy(true);
    Il2CppObject* sc = why.empty() ? SceneContext() : nullptr;
    if (why.empty() && !sc) why = "No player right now (load a save first).";
    Il2CppObject* ctrl = why.empty() ? FindController(sc, why) : nullptr;
    Il2CppGCHandle h = ctrl ? unity::NewGCHandle(ctrl) : nullptr;
    if (ctrl && !h) why = "Could not keep a handle on the player.";
    if (!h) {
        SR2_WARN("[{}] noclip not switched on: {}", kModule, why);
        sr2::ui::sandbox::SetStatus("Noclip: " + why, true);
        return;
    }
    Il2CppObject* motor = unity::Field<Il2CppObject*>(ctrl, g_offMotor);
    g_savedMove = unity::Field<bool>(motor, g_offSolveMove);
    g_savedGround = unity::Field<bool>(motor, g_offSolveGround);
    g_savedBypass = unity::Field<bool>(ctrl, g_offBypass);
    g_ctrl = h;
    g_group = CurrentGroup(sc);
    g_on = true;
    g_lastCheck = GetTickCount64();
    g_loggedMoveFail = false;
    g_loggedKillSkip = false;
    Hold(ctrl, motor);
    sr2::modules::common::NoclipFlag().store(true);
    const V3 p = unity::Field<V3>(motor, g_offTransient);
    SR2_INFO("[{}] noclip on at ({:.1f}, {:.1f}, {:.1f}): collision and ground solving off, gravity bypassed (were: "
             "collisions {}, grounding {}, bypass {}), {:.0f} m/s",
             kModule, p.x, p.y, p.z, g_savedMove, g_savedGround, g_savedBypass, settings::GetFloat(kModule, kKeySpeed));
    Notice("Noclip on");
}

V3 TransformDir(const MethodInfo* m, Il2CppObject* tr, const char* what, bool& ok) {
    V3 v{0, 0, 0};
    ok = CallValue(m, tr, {}, what, v) && std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
    return v;
}

// One frame of flight: the movement input along the camera's view (W towards where you look, also up / down),
// the up / down keys straight up / down.
void Move(Il2CppObject* ctrl, Il2CppObject* motor) {
    const float dt = unity::DeltaTime();  // scaled: 0 while the game is paused
    if (!(dt > 0.0f) || dt > 0.25f || !Playing()) return;
    V2 in{0, 0};
    if (g_offInput) in = unity::Field<V2>(ctrl, g_offInput);
    if (!std::isfinite(in.x) || !std::isfinite(in.y)) in = V2{0, 0};
    if (std::fabs(in.x) + std::fabs(in.y) < 0.05f) {  // no movement input read: the WASD keys themselves
        in.x = (KeyDown('D') ? 1.0f : 0.0f) - (KeyDown('A') ? 1.0f : 0.0f);
        in.y = (KeyDown('W') ? 1.0f : 0.0f) - (KeyDown('S') ? 1.0f : 0.0f);
    }
    const float len = std::sqrt(in.x * in.x + in.y * in.y);
    if (len > 1.0f) in.x /= len, in.y /= len;
    const float vert = (KeyDown(settings::GetInt(kModule, kKeyUp)) ? 1.0f : 0.0f) -
                       (KeyDown(settings::GetInt(kModule, kKeyDown)) ? 1.0f : 0.0f);
    if (len < 0.01f && vert == 0.0f) return;
    V3 fwd{0, 0, 1}, right{1, 0, 0};
    if (len >= 0.01f) {
        Il2CppObject* cam = unity::Camera_main();
        Il2CppObject* tr = cam ? unity::TransformOf(cam) : nullptr;
        bool okF = false, okR = false;
        if (tr) {
            fwd = TransformDir(g_trForward, tr, "Transform.get_forward", okF);
            right = TransformDir(g_trRight, tr, "Transform.get_right", okR);
        }
        if (!okF || !okR) {
            if (!g_loggedMoveFail) SR2_WARN("[{}] noclip: could not read the camera's direction", kModule);
            g_loggedMoveFail = true;
            return;
        }
        right.y = 0;  // strafing stays level
        const float rl = std::sqrt(right.x * right.x + right.z * right.z);
        if (rl > 1e-4f) right.x /= rl, right.z /= rl;
    }
    float speed = std::clamp(settings::GetFloat(kModule, kKeySpeed), 1.0f, 500.0f);
    if (KeyDown(VK_SHIFT)) speed *= std::clamp(settings::GetFloat(kModule, kKeyFast), 1.0f, 10.0f);
    const float step = speed * dt;
    V3 pos = unity::Field<V3>(motor, g_offTransient);
    pos.x += (fwd.x * in.y + right.x * in.x) * step;
    pos.y += (fwd.y * in.y) * step + vert * step;
    pos.z += (fwd.z * in.y + right.z * in.x) * step;
    if (!std::isfinite(pos.x) || !std::isfinite(pos.y) || !std::isfinite(pos.z)) return;
    bool bypassInterpolation = true;
    Call(g_setPosition, motor, {&pos, &bypassInterpolation}, "KinematicCharacterMotor.SetPosition");
}

// While noclip is on: `go` is the noclipping player's GameObject (the controller's own, or SceneContext.player).
bool IsNoclipPlayer(Il2CppObject* go) {
    if (!g_on || !go || !unity::IsAlive(go)) return false;
    Il2CppObject* ctrl = g_ctrl ? unity::GCHandleTarget(g_ctrl) : nullptr;
    if (!ctrl || !unity::IsAlive(ctrl)) return false;
    if (unity::GameObjectOf(ctrl) == go || unity::GetComponent(go, g_srccClass) == ctrl) return true;
    Il2CppObject* sc = SceneContext();
    return sc && unity::Field<Il2CppObject*>(sc, g_offScPlayer) == go;
}

void LogKillSkip(const char* where) {
    if (g_loggedKillSkip) return;
    g_loggedKillSkip = true;
    SR2_INFO("[{}] noclip: kept the player out of a kill volume ({}); logged once per noclip", kModule, where);
}

// DeathHandler.Kill(GameObject, Damage), static: the end of every kill volume. Dropped for the noclipping player.
void KillDetour(Il2CppObject* go, Il2CppObject* damage, const MethodInfo* mi) {
    bool skip = false;
    if (g_on && g_killGuard.Run([&] { skip = IsNoclipPlayer(go); }) && skip) {
        LogKillSkip("DeathHandler.Kill");
        return;
    }
    g_origKill(go, damage, mi);
}

// KillOnTrigger.OnTriggerEnter(Collider): the slime sea. Skipped for the noclipping player's main collider.
void KillTriggerDetour(Il2CppObject* self, Il2CppObject* collider, const MethodInfo* mi) {
    bool skip = false;
    if (g_on && g_killTriggerGuard.Run([&] {
            if (!collider || !unity::IsAlive(collider)) return;
            // The game's own test (also what KillOnTrigger uses to pick the player FX); one player, so it is ours.
            bool main = false;
            skip = CallValue(g_isPlayerCollider, nullptr, {collider}, "PhysicsUtil.IsPlayerMainCollider", main) && main;
        }) &&
        skip) {
        LogKillSkip("KillOnTrigger");
        return;
    }
    g_origKillTrigger(self, collider, mi);
}

}  // namespace

void SetupNoclip(sr2::core::ModuleContext& ctx) {
    auto& R = ctx.resolver();
    using settings::Kind;
    auto def = [](const char* key, const char* label, Kind kind, double d, double mn, double mx, double step,
                  const char* tip) {
        settings::Def x;
        x.module = kModule;
        x.key = key;
        x.label = label;
        x.group = "Noclip";
        x.kind = kind;
        x.def = d;
        x.min = mn;
        x.max = mx;
        x.step = step;
        x.tooltip = tip;
        settings::Register(x);
    };
    def(kKeyToggle, "Noclip key", Kind::Key, 0, 0, 255, 1,
        "Switches noclip on and off while Sandbox is on (not while a menu or the overlay is open). Unbound by "
        "default.");
    def(kKeyUp, "Fly up", Kind::Key, VK_SPACE, 0, 255, 1, "Hold to rise while noclipping.");
    def(kKeyDown, "Fly down", Kind::Key, VK_CONTROL, 0, 255, 1, "Hold to sink while noclipping.");
    def(kKeySpeed, "Noclip speed", Kind::Float, 15, 1, 200, 1, "How fast noclip moves you.");
    def(kKeyFast, "Shift speed multiplier", Kind::Float, 3, 1, 10, 0.5, "Noclip speed times this while Shift is held.");
    settings::SetDisplay(kModule, kKeySpeed, " m/s");
    settings::SetDisplay(kModule, kKeyFast, "x");

    std::string& w = g_why;
    auto cls = [&](const char* image, const char* ns, const char* name) -> Il2CppClass* {
        std::string err;
        Il2CppClass* k = R.Class(image, ns, name, &err);
        if (!k && w.empty()) w = err.empty() ? std::format("class {}.{} not found", ns, name) : err;
        return k;
    };
    auto off = [&](Il2CppClass* k, const char* field, size_t& out, bool required = true) {
        if (!k) return;
        std::string err;
        if (auto o = R.FieldOffset(k, field, &err)) out = *o;
        else if (required && w.empty()) w = err.empty() ? std::format("field {} not found", field) : err;
    };
    auto method = [&](Il2CppClass* k, const char* name, int argc, bool required = true) -> const MethodInfo* {
        std::string err;
        for (Il2CppClass* c = k; c; c = api().il2cpp_class_get_parent ? api().il2cpp_class_get_parent(c) : nullptr)
            if (const MethodInfo* m = R.Method(c, name, argc, &err)) return m;
        if (required && k && w.empty()) w = std::format("method {}({}) not found", name, argc);
        return nullptr;
    };
    g_srccClass = cls(kImage, "MonomiPark.SlimeRancher.Player.CharacterController", "SRCharacterController");
    Il2CppClass* kfc = cls(kImage, "MonomiPark.KFC", "KFCCharacterController");
    Il2CppClass* motor = cls(kImage, "KinematicCharacterController", "KinematicCharacterMotor");
    Il2CppClass* scu = cls(kImage, "MonomiPark.SlimeRancher.Util", "SceneContextUtility");
    Il2CppClass* sceneCtx = cls(kImage, "", "SceneContext");
    Il2CppClass* regions = cls(kImage, "MonomiPark.SlimeRancher.Regions", "RegionRegistry");
    Il2CppClass* transform = cls("UnityEngine.CoreModule", "UnityEngine", "Transform");
    g_tryGetSceneContext = method(scu, "TryGetSceneContext", 1);
    off(sceneCtx, "player", g_offScPlayer);
    off(sceneCtx, "<RegionRegistry>k__BackingField", g_offScRegions);
    g_currentGroup = method(regions, "get_CurrentSceneGroup", 0);
    off(kfc, "_motor", g_offMotor);
    off(kfc, "<BypassGravity>k__BackingField", g_offBypass);
    off(kfc, "<GravityAmount>k__BackingField", g_offGravity);
    off(kfc, "<InputVector>k__BackingField", g_offInput, false);
    off(motor, "_transientPosition", g_offTransient);
    off(motor, "_solveMovementCollisions", g_offSolveMove);
    off(motor, "_solveGrounding", g_offSolveGround);
    off(motor, "BaseVelocity", g_offBaseVel);
    if (motor) {
        std::string err;
        g_setPosition = R.MethodByParams(motor, "SetPosition", {"UnityEngine.Vector3", "System.Boolean"}, &err);
        if (!g_setPosition && w.empty()) w = "KinematicCharacterMotor.SetPosition(Vector3, bool): " + err;
    }
    g_trForward = method(transform, "get_forward", 0);
    g_trRight = method(transform, "get_right", 0);
    {
        std::string err;
        if (Il2CppClass* go = unity::GameObjectClass())
            g_inChildren = R.MethodByParams(go, "GetComponentInChildren", {"System.Type"}, &err);
        if (Il2CppClass* cursor = R.Class("UnityEngine.CoreModule", "UnityEngine", "Cursor", &err))
            g_lockState = R.Method(cursor, "get_lockState", 0, &err);
        if (Il2CppClass* pu = R.Class(kImage, "", "PhysicsUtil", &err))
            g_isPlayerCollider = R.Method(pu, "IsPlayerMainCollider", 1, &err);
    }
    // Kill volumes: optional hooks; without them noclip still works, but the slime sea knocks you out.
    if (w.empty()) {
        auto hook = [&](const char* ns, const char* klass, const char* name, int argc, const char* hookName,
                        void* detour, void** orig) {
            std::string err;
            Il2CppClass* k = R.Class(kImage, ns, klass, &err);
            const MethodInfo* m = k ? R.Method(k, name, argc, &err) : nullptr;
            void* target = m ? R.MethodPointer(m, &err) : nullptr;
            if (target && ctx.HookAddress(hookName, target, detour, orig))
                SR2_INFO("[{}] noclip: {} hook ready (+0x{:X})", kModule, hookName, Resolver::Rva(target));
            else
                SR2_WARN("[{}] noclip: no {} hook ({})", kModule, hookName, err.empty() ? ctx.failure() : err);
        };
        hook("MonomiPark.SlimeRancher.Damage", "DeathHandler", "Kill", 2, "DeathHandler.Kill",
             reinterpret_cast<void*>(&KillDetour), reinterpret_cast<void**>(&g_origKill));
        if (g_isPlayerCollider)
            hook("", "KillOnTrigger", "OnTriggerEnter", 1, "KillOnTrigger.OnTriggerEnter",
                 reinterpret_cast<void*>(&KillTriggerDetour), reinterpret_cast<void**>(&g_origKillTrigger));
    }
    if (w.empty())
        SR2_INFO("[{}] noclip ready (motor solvers 0x{:X} / 0x{:X}, BaseVelocity 0x{:X}, BypassGravity 0x{:X}; input vector {}, "
                 "cursor lock check {})",
                 kModule, g_offSolveMove, g_offSolveGround, g_offBaseVel, g_offBypass, g_offInput ? "yes" : "no (WASD keys)",
                 g_lockState ? "yes" : "no");
    else
        SR2_WARN("[{}] noclip unavailable: {}", kModule, w);
}

void NoclipTick(bool live) {
    if (!live) {
        g_keyWas = false;
        if (g_on) Stop(sr2::core::ModuleFaulted(kModule) ? "Sandbox faulted" : "Sandbox off", false);
        return;
    }
    if (!g_why.empty()) return;
    // The toggle key: an edge while gameplay has the keyboard.
    const int vk = settings::GetInt(kModule, kKeyToggle);
    const bool down = KeyDown(vk) && Playing();
    const bool pressed = down && !g_keyWas;
    g_keyWas = down;
    if (pressed) {
        if (g_on) Stop("its key", true);
        else Start();
    }
    if (!g_on) return;
    Il2CppObject* ctrl = unity::GCHandleTarget(g_ctrl);
    Il2CppObject* motor = ctrl && unity::IsAlive(ctrl) ? unity::Field<Il2CppObject*>(ctrl, g_offMotor) : nullptr;
    if (!motor || !unity::IsAlive(motor)) {
        Stop("the player object went away", false);
        return;
    }
    const unsigned long long now = GetTickCount64();
    if (now - g_lastCheck >= kSceneCheckMs) {
        g_lastCheck = now;
        Il2CppObject* sc = SceneContext();
        std::string why;
        Il2CppObject* cur = sc ? FindController(sc, why) : nullptr;
        if (!sc || cur != ctrl) {
            Stop(!sc ? "no scene context (loading or title screen)" : "the player changed", false);
            return;
        }
        if (CurrentGroup(sc) != g_group) {
            Stop("scene change", true);
            return;
        }
    }
    Hold(ctrl, motor);
    Move(ctrl, motor);
}

bool NoclipOn() { return g_on; }

std::string NoclipWhy(bool live) {
    if (!live) return "Sandbox is off.";
    if (!g_why.empty()) return "Noclip unavailable: " + g_why;
    return {};
}

}  // namespace sr2::modules::sandbox
