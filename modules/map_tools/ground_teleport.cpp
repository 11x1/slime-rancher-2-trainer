// Teleport onto the ground below a world (x, z). See ground_teleport.h and modules/map_tools/NOTES.md
// ("Finding the ground").
//
// Every game call is either Resolver::Invoke (managed exceptions and native faults captured) or, for
// TeleportablePlayer.TeleportTo (its Nullable<Vector3> argument), a direct call under GuardedCallEx. A native
// fault marks the owning module faulted and switches this helper off for the session.

#include "modules/map_tools/ground_teleport.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <format>
#include <initializer_list>
#include <mutex>
#include <vector>

#include "core/detour_guard.h"
#include "core/guard.h"
#include "core/il2cpp_api.h"
#include "core/log.h"
#include "core/main_thread.h"
#include "core/resolver.h"
#include "core/unity.h"

namespace sr2::modules::ground_teleport {
namespace {

namespace unity = sr2::core::unity;
namespace main_thread = sr2::core::main_thread;
using sr2::il2cpp::Resolver;

constexpr const char* kImage = "Assembly-CSharp";
constexpr float kRayTop = 2000.0f;     // ground rays from this height straight down (SR2's worlds stay well below)
constexpr float kRayLength = 4000.0f;
constexpr float kLandOffset = 0.6f;    // land this far above the floor, then fall
constexpr float kHeadroom = 2.6f;      // free space above a floor (landing offset + the player)
constexpr float kMinNormalY = 0.7f;    // walkable: at most ~45 degrees
constexpr float kRoofThick = 6.0f;     // a surface at most this thick with a floor below it is a roof
constexpr float kOpenRay = 4.0f;       // sealed-in test: 8 horizontal rays this long, one must be free
constexpr float kHazardRadius = 1.0f;  // hazard trigger test: sphere this big, 1 m above the floor
constexpr float kPinMargin = 24.0f;    // regions whose bounds come this close to the column are loaded
constexpr int kMaxPins = 64;
constexpr int kMaxSurfaces = 16;       // surfaces below the column looked at
constexpr float kSettle = 0.3f;        // first floor check this long after loading regions
constexpr float kStableGap = 0.2f;     // a floor counts when it is the same for two checks this far apart
constexpr float kStableDy = 0.5f;
constexpr float kWaitMax = 6.0f;       // give up after this long
constexpr float kReleaseDelay = 1.5f;  // our region references are dropped this long after landing
constexpr int kSelfSkips = 4;          // hits on the player's own colliders skipped per ray
constexpr float kLoaderRetry = 5.0f;   // SceneLoader looked up at most this often while missing

struct NullableVec3 {  // System.Nullable<Vector3>: hasValue false = keep the rotation
    bool hasValue = false;
    Vec3f value;
};
using TeleportToFn = void (*)(Il2CppObject*, Vec3f, Il2CppObject*, NullableVec3, bool, const MethodInfo*);

struct BoundsF {
    Vec3f center, extents;
};

struct Syms {
    const MethodInfo* tryGetSceneContext = nullptr;  // SceneContextUtility.TryGetSceneContext(out SceneContext)
    const MethodInfo* scPlayer = nullptr;            // SceneContext.get_Player (GameObject)
    Il2CppClass* teleportable = nullptr;             // TeleportablePlayer
    const MethodInfo* teleportTo = nullptr;          // TeleportablePlayer.TeleportTo(Vector3, SceneGroup, Vector3?, bool)
    TeleportToFn teleportToFn = nullptr;
    const MethodInfo* sceneGroup = nullptr;          // TeleportablePlayer.get_SceneGroup
    const MethodInfo* raycast = nullptr;             // Physics.Raycast(Vector3, Vector3, out RaycastHit, float, int, QTI)
    size_t hitPoint = 0, hitNormal = 0, hitDistance = 0, hitCollider = 0;  // RaycastHit (unboxed struct offsets)
    const MethodInfo* overlapSphere = nullptr;       // Physics.OverlapSphere(Vector3, float, int, QTI) -> Collider[]
    const MethodInfo* idToObject = nullptr;          // Resources.InstanceIDToObject(int) (optional: hit names)
    const MethodInfo* goLayer = nullptr;             // GameObject.get_layer
    const MethodInfo* trPosition = nullptr;          // Transform.get_position
    const MethodInfo* trEuler = nullptr;             // Transform.get_eulerAngles (optional: rotation kept)
    const MethodInfo* trParent = nullptr;            // Transform.get_parent
    const MethodInfo* layersCollided = nullptr;      // Layers.GetLayersCollidedWith(int) (optional: ray mask)
    // regions
    Il2CppClass* regionLoader = nullptr;             // RegionLoader (on the player)
    size_t rlRegistry = 0, rlLoadSize = 0, rlWakeSize = 0;
    const MethodInfo* getRegions = nullptr;          // RegionRegistry.GetRegions(SceneGroup) -> new List<Region>
    Il2CppClass* region = nullptr;
    size_t regBounds = 0, regRefs = 0;               // Region.Bounds, Region._nonProxyRefCount
    const MethodInfo* regAddRef = nullptr;           // Region.AddNonProxiedReference
    const MethodInfo* regRemoveRef = nullptr;        // Region.RemoveNonProxiedReference
    // player state (optional)
    Il2CppClass* deathHandler = nullptr;             // PlayerDeathHandler
    size_t dhIsDead = 0;                             // ._isPlayerDead (ScriptedBool)
    const MethodInfo* scriptedBoolValue = nullptr;   // ScriptedValue<bool>.get_Value
    Il2CppClass* sceneLoader = nullptr;              // SceneLoader
    const MethodInfo* loadInProgress = nullptr;      // SceneLoader.get_IsSceneLoadInProgress
    // hazard triggers (optional each)
    std::vector<std::pair<Il2CppClass*, const char*>> hazards;
};

Syms g_s;
std::mutex g_resolveMutex;
std::atomic<int> g_state{0};  // 0 not tried, 1 ok, -1 failed
std::string g_err;
bool g_dead = false;          // a native fault: off for the session (main thread)
float g_clock = 0;            // seconds, advanced by the frame callback

struct Request {
    bool active = false;
    std::string owner;
    Done done;
    float x = 0, z = 0;
    Il2CppGCHandle player = nullptr;  // TeleportablePlayer (pinned for the request)
    Il2CppGCHandle group = nullptr;   // its SceneGroup at the start
    Vec3f from, euler;
    bool haveEuler = false;
    int mask = 0, playerLayer = -1;
    bool matrixMask = false;          // mask from the game's collision matrix
    float age = 0;
    bool loadedFirst = false;
    int regionsPinned = 0, regionsLoaded = 0;
    bool havePrev = false;
    float prevY = 0, prevT = 0;
    int rays = 0, selfHits = 0, picks = 0;
    std::string lastHit, lastWhy;     // the last surface hit / the last pick refusal (log)
};
Request g_req;
std::vector<Il2CppGCHandle> g_pins;  // regions with our reference (request, then kReleaseDelay after landing)
float g_releaseAt = -1;              // >= 0: drop g_pins at this g_clock
int g_frameCb = 0;
Il2CppGCHandle g_loader = nullptr;   // SceneLoader (session cache)
float g_loaderRetry = 0;
bool g_checksLogged = false;         // the player state checks logged once (first Start)

// ------------------------------------------------------------ game calls ---
struct Fault {};  // a native fault inside a game call: abort the request, owner faulted

const char* Owner() { return g_req.owner.empty() ? "-" : g_req.owner.c_str(); }

Il2CppObject* Call(const MethodInfo* m, void* obj, std::initializer_list<void*> args, const char* what, bool* ok = nullptr) {
    if (ok) *ok = false;
    if (!m) return nullptr;
    std::vector<void*> params(args);
    std::string err;
    Il2CppObject* r = Resolver::Get().Invoke(m, obj, params.empty() ? nullptr : params.data(), &err);
    if (!err.empty()) {
        if (err.rfind("native exception", 0) == 0) {
            SR2_ERROR("ground_teleport[{}]: {}: {}", Owner(), what, err);
            throw Fault{};
        }
        SR2_WARN("ground_teleport[{}]: {} failed: {}", Owner(), what, err);
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

Il2CppObject* Target(Il2CppGCHandle h) { return h ? unity::GCHandleTarget(h) : nullptr; }

bool Finite(const Vec3f& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

bool Position(Il2CppObject* comp, Vec3f& out) {
    Il2CppObject* t = unity::TransformOf(comp);
    return t && CallValue(g_s.trPosition, t, {}, "Transform.get_position", out) && Finite(out);
}

// TeleportablePlayer.TeleportTo(position, sceneGroup, rotation, overlayEnabled). With a rotation the game also
// zeroes the controller's velocity (IPlayerController.BaseVelocity), so the player does not keep falling speed.
bool TeleportTo(Il2CppObject* player, Il2CppObject* group, Vec3f pos, bool overlay) {
    struct Ctx {
        Il2CppObject *player, *group;
        Vec3f pos;
        NullableVec3 rot;
        bool overlay;
    } ctx{player, group, pos, {g_req.haveEuler, g_req.euler}, overlay};
    std::string err;
    core::FaultInfo fi;
    bool ok = core::GuardedCallEx(
        [](void* p) {
            auto* c = static_cast<Ctx*>(p);
            g_s.teleportToFn(c->player, c->pos, c->group, c->rot, c->overlay, g_s.teleportTo);
        },
        &ctx, &err, &fi);
    if (ok) return true;
    if (fi.code != 0) {
        SR2_ERROR("ground_teleport[{}]: TeleportablePlayer.TeleportTo: native fault {}", Owner(), err);
        throw Fault{};
    }
    SR2_WARN("ground_teleport[{}]: TeleportablePlayer.TeleportTo threw: {}", Owner(), err);
    return false;
}

// Is `t` the player's transform or below it?
bool UnderPlayer(Il2CppObject* t, Il2CppObject* playerT) {
    for (int i = 0; t && i < 24; ++i) {
        if (t == playerT) return true;
        t = Call(g_s.trParent, t, {}, "Transform.get_parent");
    }
    return false;
}

// ------------------------------------------------------------ rays ---------
struct Hit {
    Vec3f p, n;
    std::string name;
    int layer = -1;
};

Il2CppObject* HitCollider(const unsigned char* hit) {
    if (!g_s.idToObject) return nullptr;
    int id = *reinterpret_cast<const int*>(hit + g_s.hitCollider);
    Il2CppObject* col = Call(g_s.idToObject, nullptr, {&id}, "Resources.InstanceIDToObject");
    return col && unity::IsAlive(col) ? col : nullptr;
}

// The first collider along the ray (layers the player collides with, triggers ignored) that is not the
// player's own. `named`: also the collider's name / layer (costs two calls).
bool Cast(Vec3f origin, Vec3f dir, float len, Il2CppObject* playerT, Hit& out, bool named) {
    for (int skip = 0; skip <= kSelfSkips && len > 0.01f; ++skip) {
        alignas(16) unsigned char hit[64] = {};
        int mask = g_req.mask, triggers = 1;  // QueryTriggerInteraction.Ignore
        bool any = false;
        ++g_req.rays;
        if (!CallValue(g_s.raycast, nullptr, {&origin, &dir, hit, &len, &mask, &triggers}, "Physics.Raycast", any) || !any)
            return false;
        Vec3f p = *reinterpret_cast<Vec3f*>(hit + g_s.hitPoint);
        Vec3f n = *reinterpret_cast<Vec3f*>(hit + g_s.hitNormal);
        float d = *reinterpret_cast<float*>(hit + g_s.hitDistance);
        if (!Finite(p) || !Finite(n) || !std::isfinite(d)) return false;
        Il2CppObject* col = (named || playerT) ? HitCollider(hit) : nullptr;
        if (col && playerT && UnderPlayer(unity::TransformOf(col), playerT)) {  // the player's own collider
            ++g_req.selfHits;
            float step = d + 0.05f;
            origin = {origin.x + dir.x * step, origin.y + dir.y * step, origin.z + dir.z * step};
            len -= step;
            continue;
        }
        out.p = p;
        out.n = n;
        if (named) {
            Il2CppObject* go = col ? unity::GameObjectOf(col) : nullptr;
            out.name = go ? unity::GetName(go) : std::string("?");
            out.layer = -1;
            if (go) CallValue(g_s.goLayer, go, {}, "GameObject.get_layer", out.layer);
        }
        return true;
    }
    return false;
}

// A hazard trigger around `c` (any layer, triggers included): the class name, else "".
std::string HazardAt(Vec3f c) {
    if (!g_s.overlapSphere || g_s.hazards.empty()) return {};
    float r = kHazardRadius;
    int mask = -1, triggers = 2;  // QueryTriggerInteraction.Collide
    Il2CppObject* arr = Call(g_s.overlapSphere, nullptr, {&c, &r, &mask, &triggers}, "Physics.OverlapSphere");
    if (!arr) return {};
    Il2CppGCHandle pin = unity::NewGCHandle(arr);  // our calls below may allocate
    std::string found;
    for (size_t i = 0, n = std::min<size_t>(unity::ArrayLength(arr), 128); i < n && found.empty(); ++i) {
        auto* col = static_cast<Il2CppObject*>(unity::ArrayAt(arr, i));
        if (!col || !unity::IsAlive(col)) continue;
        Il2CppObject* go = unity::GameObjectOf(col);
        for (int up = 0; go && up < 2 && found.empty(); ++up) {
            for (const auto& [k, name] : g_s.hazards)
                if (unity::GetComponent(go, k)) {
                    found = std::format("{} on '{}'", name, unity::GetName(go));
                    break;
                }
            Il2CppObject* t = Call(g_s.trParent, unity::TransformOf(go), {}, "Transform.get_parent");
            go = t ? unity::GameObjectOf(t) : nullptr;
        }
    }
    if (pin) unity::FreeGCHandle(pin);
    return found;
}

struct Surface {
    Hit h;
    bool ok = false;
    std::string why;  // not a valid floor: why
};

void Judge(Surface& s, Il2CppObject* playerT) {
    const Vec3f p = s.h.p;
    Hit tmp;
    if (s.h.n.y < kMinNormalY) {
        s.why = std::format("too steep at y {:.1f}", p.y);
        return;
    }
    if (Cast({p.x, p.y + 0.05f, p.z}, {0, 1, 0}, kHeadroom, playerT, tmp, false)) {
        s.why = std::format("no headroom at y {:.1f} ({:.1f} m)", p.y, tmp.p.y - p.y);
        return;
    }
    std::string hazard = HazardAt({p.x, p.y + 1.0f, p.z});
    if (!hazard.empty()) {
        s.why = std::format("hazard at y {:.1f}: {}", p.y, hazard);
        return;
    }
    int free = 0;
    for (int i = 0; i < 8 && !free; ++i) {
        float a = static_cast<float>(i) * 0.785398f;
        if (!Cast({p.x, p.y + 1.0f, p.z}, {std::cos(a), 0, std::sin(a)}, kOpenRay, playerT, tmp, false)) ++free;
    }
    if (!free) {
        s.why = std::format("sealed in at y {:.1f}", p.y);
        return;
    }
    s.ok = true;
}

// Is surface `a` a thin slab (a roof) over the floor `b` below it? The underside above `b` (an upward ray only
// hits downward-facing faces) must be within kRoofThick of `a`'s top, with headroom above `b`.
bool RoofOver(const Surface& a, const Surface& b, Il2CppObject* playerT) {
    Hit up;
    float ceiling = a.h.p.y;
    if (Cast({b.h.p.x, b.h.p.y + 0.05f, b.h.p.z}, {0, 1, 0}, a.h.p.y - b.h.p.y + 1.0f, playerT, up, false)) ceiling = up.p.y;
    return a.h.p.y - ceiling <= kRoofThick && ceiling - b.h.p.y >= kHeadroom;
}

struct Pick {
    bool ok = false;
    float y = 0;
    std::string name, why, note;
    int layer = -1;
};

// The landing floor below (x, z); see ground_teleport.h.
Pick PickFloor(float x, float z, Il2CppObject* player) {
    ++g_req.picks;
    Pick r;
    Il2CppObject* playerT = unity::TransformOf(player);
    std::vector<Surface> s;
    float top = kRayTop;
    for (int i = 0; i < kMaxSurfaces; ++i) {
        Surface cur;
        if (!Cast({x, top, z}, {0, -1, 0}, kRayLength - (kRayTop - top), playerT, cur.h, true)) break;
        if (std::fabs(cur.h.p.x - x) > 1.0f || std::fabs(cur.h.p.z - z) > 1.0f) break;
        if (i == 0) g_req.lastHit = std::format("'{}' at y {:.1f} (layer {})", cur.h.name, cur.h.p.y, cur.h.layer);
        top = cur.h.p.y - 0.05f;
        s.push_back(std::move(cur));
    }
    if (s.empty()) {
        r.why = "no collider below the spot (area not loaded, or outside the world)";
        return r;
    }
    size_t best = s.size();
    Judge(s[0], playerT);
    if (s[0].ok) best = 0;
    // Down through roofs: from the chosen surface (or a refused top surface) to a valid floor right below a thin slab.
    size_t from = 0;
    for (size_t j = 1; j < s.size(); ++j) {
        Judge(s[j], playerT);
        if (!s[j].ok) continue;
        if (!RoofOver(s[from], s[j], playerT)) break;
        r.note += std::format("{}'{}' at y {:.1f} is a roof over '{}' at y {:.1f}", r.note.empty() ? "" : "; ", s[from].h.name,
                              s[from].h.p.y, s[j].h.name, s[j].h.p.y);
        best = j;
        from = j;
    }
    if (best == s.size()) {
        r.why = std::format("the spot is not safe to stand on: '{}' {}", s[0].h.name, s[0].why);
        return r;
    }
    r.ok = true;
    r.y = s[best].h.p.y;
    r.name = s[best].h.name;
    r.layer = s[best].h.layer;
    return r;
}

// ------------------------------------------------------------ player state -
Il2CppObject* SceneLoader() {
    if (!g_s.sceneLoader || !g_s.loadInProgress) return nullptr;
    Il2CppObject* l = Target(g_loader);
    if (l && unity::IsAlive(l)) return l;
    if (g_loader) unity::FreeGCHandle(g_loader);
    g_loader = nullptr;
    if (g_clock < g_loaderRetry) return nullptr;
    g_loaderRetry = g_clock + kLoaderRetry;
    l = unity::FindObjectOfType(g_s.sceneLoader);
    if (l) g_loader = unity::NewGCHandle(l);
    return l;
}

// Knocked out / respawning (`knockedOut`), or a scene load: the reason, else "". `playerGo` may be null (looked
// up). The knock-out comes first: a respawn into another zone is also a scene load.
std::string Unavailable(Il2CppObject* playerGo, bool* knockedOut = nullptr) {
    if (knockedOut) *knockedOut = false;
    if (g_s.deathHandler && g_s.scriptedBoolValue) {
        if (!playerGo) {
            Il2CppObject* sc = nullptr;
            bool found = false;
            if (CallValue(g_s.tryGetSceneContext, nullptr, {&sc}, "SceneContextUtility.TryGetSceneContext", found) && found &&
                sc && unity::IsAlive(sc))
                playerGo = Call(g_s.scPlayer, sc, {}, "SceneContext.get_Player");
        }
        Il2CppObject* dh = playerGo && unity::IsAlive(playerGo) ? unity::GetComponent(playerGo, g_s.deathHandler) : nullptr;
        Il2CppObject* flag = dh ? unity::Field<Il2CppObject*>(dh, g_s.dhIsDead) : nullptr;
        bool dead = false;
        if (flag && unity::IsAlive(flag) && CallValue(g_s.scriptedBoolValue, flag, {}, "ScriptedBool.get_Value", dead) && dead) {
            if (knockedOut) *knockedOut = true;
            return "the player is knocked out";
        }
    }
    if (Il2CppObject* l = SceneLoader()) {
        bool loading = false;
        if (CallValue(g_s.loadInProgress, l, {}, "SceneLoader.get_IsSceneLoadInProgress", loading) && loading)
            return "a scene is loading";
    }
    return {};
}

// ------------------------------------------------------------ regions ------
// Load (reference) every region of `group` whose bounds come close to the column; returns how many of them
// were proxied (not loaded) before.
int PinRegions(Il2CppObject* player, Il2CppObject* group, float x, float z) {
    if (!g_s.getRegions || !g_s.regAddRef || !g_s.regRemoveRef) return 0;
    Il2CppObject* loader = unity::GetComponent(player, g_s.regionLoader);
    Il2CppObject* reg = loader ? unity::Field<Il2CppObject*>(loader, g_s.rlRegistry) : nullptr;
    if (!reg || !unity::IsAlive(reg)) return 0;
    Il2CppObject* list = Call(g_s.getRegions, reg, {group}, "RegionRegistry.GetRegions");
    if (!list) return 0;
    Il2CppGCHandle pin = unity::NewGCHandle(list);
    int loaded = 0;
    for (size_t i = 0, n = std::min<size_t>(unity::ListCount(list), 4096); i < n && g_pins.size() < kMaxPins; ++i) {
        auto* r = static_cast<Il2CppObject*>(unity::ListAt(list, i));
        if (!r || !unity::IsAlive(r)) continue;
        BoundsF b = unity::Field<BoundsF>(r, g_s.regBounds);
        if (std::fabs(x - b.center.x) > b.extents.x + kPinMargin || std::fabs(z - b.center.z) > b.extents.z + kPinMargin)
            continue;
        Il2CppGCHandle h = unity::NewGCHandle(r);
        if (!h) continue;
        bool proxied = unity::Field<int>(r, g_s.regRefs) <= 0;
        bool ok = false;
        Call(g_s.regAddRef, r, {}, "Region.AddNonProxiedReference", &ok);
        if (!ok) {
            unity::FreeGCHandle(h);
            continue;
        }
        g_pins.push_back(h);
        if (proxied) ++loaded;
    }
    if (pin) unity::FreeGCHandle(pin);
    return loaded;
}

// Our references back (callGame false after a native fault: handles only).
void ReleasePins(bool callGame) {
    std::vector<Il2CppGCHandle> pins;
    pins.swap(g_pins);
    g_releaseAt = -1;
    for (Il2CppGCHandle h : pins) {
        Il2CppObject* r = callGame ? Target(h) : nullptr;
        if (r && unity::IsAlive(r)) {
            try {
                Call(g_s.regRemoveRef, r, {}, "Region.RemoveNonProxiedReference");
            } catch (const Fault&) {
                callGame = false;
                g_dead = true;
            }
        }
        unity::FreeGCHandle(h);
    }
}

// ------------------------------------------------------------ request ------
void StopFrame() {
    if (g_frameCb) main_thread::RemoveFrameCallback(g_frameCb);
    g_frameCb = 0;
}

void Finish(Result r, bool landed) {
    r.seconds = g_req.age;
    r.from = g_req.from;
    r.loadedFirst = g_req.loadedFirst;
    Done done = std::move(g_req.done);
    for (Il2CppGCHandle h : {g_req.player, g_req.group})
        if (h) unity::FreeGCHandle(h);
    g_req = Request{};
    if (landed && !g_pins.empty()) g_releaseAt = g_clock + kReleaseDelay;  // the loader takes over first
    else ReleasePins(!g_dead);
    if (g_pins.empty()) StopFrame();
    if (done) done(r);
}

std::string RayParams() {
    return std::format("rays from y {:.0f} down {:.0f} m, layer mask 0x{:08X} ({}), triggers ignored", kRayTop, kRayLength,
                       static_cast<unsigned>(g_req.mask),
                       g_req.matrixMask ? std::format("what the player's layer {} collides with", g_req.playerLayer)
                                        : std::format("all but Ignore Raycast and the player's layer {}", g_req.playerLayer));
}

std::string LoaderSizes(Il2CppObject* player) {
    if (!g_s.regionLoader) return "region loader unknown";
    Il2CppObject* rl = unity::GetComponent(player, g_s.regionLoader);
    if (!rl) return "no RegionLoader on the player";
    Vec3f load = unity::Field<Vec3f>(rl, g_s.rlLoadSize), wake = unity::Field<Vec3f>(rl, g_s.rlWakeSize);
    return std::format("RegionLoader LoadSize ({:.0f}, {:.0f}, {:.0f}), WakeSize ({:.0f}, {:.0f}, {:.0f})", load.x, load.y,
                       load.z, wake.x, wake.y, wake.z);
}

void Land(Il2CppObject* player, Il2CppObject* group, const Pick& pick) {
    Vec3f dest{g_req.x, pick.y + kLandOffset, g_req.z};
    Result r;
    std::string busy = Unavailable(unity::GameObjectOf(player));
    if (!busy.empty()) {
        r.why = busy;
        SR2_INFO("ground_teleport[{}]: not teleported to ({:.1f}, {:.1f}): {}", Owner(), g_req.x, g_req.z, busy);
        Finish(r, false);
        return;
    }
    if (!TeleportTo(player, group, dest, true)) {
        r.why = "the game's teleport call failed";
        Finish(r, false);
        return;
    }
    r.ok = true;
    r.to = dest;
    SR2_INFO("ground_teleport[{}]: landed at ({:.1f}, {:.1f}, {:.1f}) on '{}' (layer {}) from ({:.1f}, {:.1f}, {:.1f}), {:.0f} m, "
             "{}{:.1f} s, {} floor check(s), {} ray(s){}{}",
             Owner(), dest.x, dest.y, dest.z, pick.name, pick.layer, g_req.from.x, g_req.from.y, g_req.from.z,
             std::hypot(dest.x - g_req.from.x, dest.z - g_req.from.z),
             g_req.loadedFirst ? std::format("{} of {} region(s) loaded first, ", g_req.regionsLoaded, g_req.regionsPinned)
                               : std::string(),
             g_req.age, g_req.picks, g_req.rays,
             g_req.selfHits ? std::format(", {} hit(s) on the player skipped", g_req.selfHits) : std::string(),
             pick.note.empty() ? std::string() : "; " + pick.note);
    Finish(r, true);
}

void Refuse(const std::string& why, Il2CppObject* player) {
    SR2_WARN("ground_teleport[{}]: not teleported to ({:.1f}, {:.1f}): {}. {}; {} floor check(s), {} ray(s), first hit {}; "
             "player stays at ({:.1f}, {:.1f}, {:.1f}); {} region(s) referenced ({} were not loaded); {}",
             Owner(), g_req.x, g_req.z, why, RayParams(), g_req.picks, g_req.rays,
             g_req.lastHit.empty() ? "none" : g_req.lastHit, g_req.from.x, g_req.from.y, g_req.from.z, g_req.regionsPinned,
             g_req.regionsLoaded, player ? LoaderSizes(player) : std::string("no player"));
    Result r;
    r.why = why;
    Finish(r, false);
}

// Dropped mid-way (knocked out, scene change): no teleport, one log line.
void Abort(const std::string& why) {
    SR2_INFO("ground_teleport[{}]: teleport to ({:.1f}, {:.1f}) dropped after {:.1f} s: {} - the player was not moved", Owner(),
             g_req.x, g_req.z, g_req.age, why);
    Result r;
    r.why = why;
    Finish(r, false);
}

// One step of a waiting request (every frame).
void Step(float dt) {
    g_req.age += dt;
    Il2CppObject* player = Target(g_req.player);
    Il2CppObject* group = Target(g_req.group);
    if (!player || !unity::IsAlive(player) || !group) {
        Abort("the player went away (scene change?)");
        return;
    }
    // We never teleport before landing, so the player's scene group is the game's own.
    Il2CppObject* now = Call(g_s.sceneGroup, player, {}, "TeleportablePlayer.get_SceneGroup");
    if (now != group) {
        Abort("the scene changed");
        return;
    }
    std::string busy = Unavailable(unity::GameObjectOf(player));
    if (!busy.empty()) {
        Abort(busy);
        return;
    }
    if (g_req.age >= kSettle) {
        Pick p = PickFloor(g_req.x, g_req.z, player);
        if (p.ok) {
            if (g_req.havePrev && std::fabs(p.y - g_req.prevY) < kStableDy) {
                if (g_req.age - g_req.prevT >= kStableGap) {
                    Land(player, group, p);
                    return;
                }
            } else {
                g_req.havePrev = true;
                g_req.prevY = p.y;
                g_req.prevT = g_req.age;
            }
        } else {
            g_req.havePrev = false;
            g_req.lastWhy = p.why;
        }
    }
    if (g_req.age >= kWaitMax)
        Refuse(g_req.lastWhy.empty() ? std::format("nothing to stand on after {:.0f} s", kWaitMax) : g_req.lastWhy, player);
}

template <class F>
void Guarded(F&& f) {
    try {
        f();
    } catch (const Fault&) {
        g_dead = true;
        std::string owner = g_req.owner;
        if (!owner.empty()) core::MarkModuleFaulted(owner, "native fault in the ground teleport (see the line above)");
        g_req.done = nullptr;  // the owner is faulted: no callback into it
        if (g_req.active) {
            Result r;
            r.why = "internal error";
            Finish(r, false);
        } else {
            ReleasePins(false);
            StopFrame();
        }
        SR2_ERROR("ground_teleport: off for the rest of this session after a native fault ({})", owner.empty() ? "-" : owner);
    }
}

void Frame(float dt) {
    g_clock += dt;
    if (g_req.active) {
        Guarded([&] { Step(dt); });
        return;
    }
    if (g_releaseAt >= 0 && g_clock >= g_releaseAt) Guarded([] { ReleasePins(!g_dead); });
    if (g_pins.empty()) StopFrame();
}

bool EnsureFrame() {
    if (!g_frameCb) g_frameCb = main_thread::AddFrameCallback("ground_teleport", [](float dt) { Frame(dt); });
    return g_frameCb != 0;
}

bool ResolveImpl(std::string& err) {
    Resolver& R = Resolver::Get();
    Syms s;
    std::string e;
    auto cls = [&](const char* image, const char* ns, const char* name) -> Il2CppClass* {
        Il2CppClass* k = R.Class(image, ns, name, &e);
        if (!k && err.empty()) err = e.empty() ? std::format("class {}.{} not found", ns, name) : e;
        return k;
    };
    auto method = [&](Il2CppClass* k, const char* name, int argc) -> const MethodInfo* {
        if (!k) return nullptr;
        const MethodInfo* m = R.Method(k, name, argc, &e);
        if (!m && err.empty()) err = std::format("method {}({}) not found", name, argc);
        return m;
    };
    auto field = [&](Il2CppClass* k, const char* name, size_t& out, bool structField) {
        if (!k) return false;
        auto o = R.FieldOffset(k, name, &e);
        size_t hdr = structField ? 2 * sizeof(void*) : 0;  // value type field offsets include the object header
        if (!o || *o < hdr) {
            if (err.empty()) err = std::format("field {} not found", name);
            return false;
        }
        out = *o - hdr;
        return true;
    };
    Il2CppClass* scu = cls(kImage, "MonomiPark.SlimeRancher.Util", "SceneContextUtility");
    Il2CppClass* sceneCtx = cls(kImage, "", "SceneContext");
    s.teleportable = cls(kImage, "", "TeleportablePlayer");
    Il2CppClass* physics = cls("UnityEngine.PhysicsModule", "UnityEngine", "Physics");
    Il2CppClass* hit = cls("UnityEngine.PhysicsModule", "UnityEngine", "RaycastHit");
    Il2CppClass* go = cls("UnityEngine.CoreModule", "UnityEngine", "GameObject");
    Il2CppClass* tr = cls("UnityEngine.CoreModule", "UnityEngine", "Transform");
    s.tryGetSceneContext = method(scu, "TryGetSceneContext", 1);
    s.scPlayer = method(sceneCtx, "get_Player", 0);
    s.teleportTo = method(s.teleportable, "TeleportTo", 4);
    s.sceneGroup = method(s.teleportable, "get_SceneGroup", 0);
    s.goLayer = method(go, "get_layer", 0);
    s.trPosition = method(tr, "get_position", 0);
    s.trParent = method(tr, "get_parent", 0);
    if (physics) {
        s.raycast = R.MethodByParams(physics, "Raycast",
                                     {"UnityEngine.Vector3", "UnityEngine.Vector3", "UnityEngine.RaycastHit&", "System.Single",
                                      "System.Int32", "UnityEngine.QueryTriggerInteraction"},
                                     &e);
        if (!s.raycast && err.empty()) err = "Physics.Raycast(6): " + e;
    }
    field(hit, "m_Point", s.hitPoint, true);
    field(hit, "m_Normal", s.hitNormal, true);
    field(hit, "m_Distance", s.hitDistance, true);
    field(hit, "m_Collider", s.hitCollider, true);
    if (s.teleportTo) {
        s.teleportToFn = reinterpret_cast<TeleportToFn>(R.MethodPointer(s.teleportTo, &e));
        if (!s.teleportToFn && err.empty()) err = "TeleportablePlayer.TeleportTo: no native body";
    }
    // regions: needed to load a far target without moving the player
    const char* nsReg = "MonomiPark.SlimeRancher.Regions";
    s.regionLoader = cls(kImage, nsReg, "RegionLoader");
    Il2CppClass* registry = cls(kImage, nsReg, "RegionRegistry");
    s.region = cls(kImage, nsReg, "Region");
    field(s.regionLoader, "_regionReg", s.rlRegistry, false);
    field(s.regionLoader, "LoadSize", s.rlLoadSize, false);
    field(s.regionLoader, "WakeSize", s.rlWakeSize, false);
    s.getRegions = method(registry, "GetRegions", 1);
    field(s.region, "Bounds", s.regBounds, false);
    field(s.region, "_nonProxyRefCount", s.regRefs, false);
    s.regAddRef = method(s.region, "AddNonProxiedReference", 0);
    s.regRemoveRef = method(s.region, "RemoveNonProxiedReference", 0);
    if (!err.empty()) return false;

    // optional
    std::vector<std::string> missing;
    std::string opt;
    auto optClass = [&](const char* image, const char* ns, const char* name) -> Il2CppClass* {
        Il2CppClass* k = R.Class(image, ns, name, &opt);
        if (!k) missing.push_back(std::format("{}{}{}", ns, *ns ? "." : "", name));
        return k;
    };
    auto optMethod = [&](Il2CppClass* k, const char* name, int argc) -> const MethodInfo* {
        const MethodInfo* m = k ? R.Method(k, name, argc, &opt) : nullptr;
        if (k && !m) missing.push_back(name);
        return m;
    };
    if (Il2CppClass* res = optClass("UnityEngine.CoreModule", "UnityEngine", "Resources"))
        s.idToObject = optMethod(res, "InstanceIDToObject", 1);
    s.trEuler = optMethod(tr, "get_eulerAngles", 0);
    s.overlapSphere = physics ? R.MethodByParams(physics, "OverlapSphere",
                                                 {"UnityEngine.Vector3", "System.Single", "System.Int32",
                                                  "UnityEngine.QueryTriggerInteraction"},
                                                 &opt)
                              : nullptr;
    if (!s.overlapSphere) missing.push_back("Physics.OverlapSphere(4)");
    s.layersCollided = optMethod(optClass(kImage, "", "Layers"), "GetLayersCollidedWith", 1);
    s.deathHandler = optClass(kImage, "", "PlayerDeathHandler");
    if (s.deathHandler) {
        auto o = R.FieldOffset(s.deathHandler, "_isPlayerDead", &opt);
        if (o) {
            s.dhIsDead = *o;
        } else {
            s.deathHandler = nullptr;
            missing.push_back("PlayerDeathHandler._isPlayerDead");
        }
    }
    s.scriptedBoolValue = optMethod(optClass(kImage, "MonomiPark.ScriptedValue", "ScriptedBool"), "get_Value", 0);
    s.sceneLoader = optClass(kImage, "MonomiPark.SlimeRancher.SceneManagement", "SceneLoader");
    s.loadInProgress = optMethod(s.sceneLoader, "get_IsSceneLoadInProgress", 0);
    for (auto [ns, name] : {std::pair{"", "KillOnTrigger"}, std::pair{"", "KillOnTriggerCloudOcean"},
                            std::pair{"MonomiPark.SlimeRancher.Damage", "LavaVolume"},
                            std::pair{"MonomiPark.SlimeRancher.Damage", "DamagePlayerOnEnterTrigger"}})
        if (Il2CppClass* k = optClass(kImage, ns, name)) s.hazards.emplace_back(k, name);
    g_s = s;
    std::string miss;
    for (const std::string& m : missing) miss += (miss.empty() ? "" : ", ") + m;
    SR2_INFO("ground_teleport: resolved (hit names {}, collision matrix mask {}, hazard triggers {} of 4{}, knock-out check {}, "
             "scene load check {}, rotation kept {}){}{}",
             s.idToObject ? "yes" : "no", s.layersCollided ? "yes" : "no", s.hazards.size(),
             s.overlapSphere ? "" : " (no OverlapSphere: unchecked)", s.deathHandler && s.scriptedBoolValue ? "yes" : "no",
             s.loadInProgress ? "yes" : "no", s.trEuler ? "yes" : "no", miss.empty() ? "" : "; missing: ", miss);
    return true;
}

}  // namespace

bool Resolve(std::string& err) {
    std::lock_guard lk(g_resolveMutex);
    if (g_state.load() == 0) g_state.store(ResolveImpl(g_err) ? 1 : -1);
    err = g_err;
    return g_state.load() == 1;
}

bool Resolved() { return g_state.load() == 1 && !g_dead; }

bool Busy() { return g_req.active; }

bool PlayerUnavailable(std::string* why, bool* knockedOut) {
    if (knockedOut) *knockedOut = false;
    if (!Resolved() || !main_thread::IsMainThread()) return false;
    std::string r;
    try {
        r = Unavailable(nullptr, knockedOut);
    } catch (const Fault&) {
        g_dead = true;
        SR2_ERROR("ground_teleport: off for the rest of this session after a native fault (player state check)");
        return false;
    }
    if (why) *why = r;
    return !r.empty();
}

bool Start(const char* owner, float x, float z, Done done, std::string* why) {
    auto fail = [&](const std::string& w) {
        if (why) *why = w;
        return false;
    };
    if (!Resolved()) return fail("the ground teleport is not available (see the log)");
    if (g_req.active) return fail("a teleport is still running");
    if (!std::isfinite(x) || !std::isfinite(z)) return fail("bad position");
    if (core::ModuleFaulted(owner)) return fail("module faulted");
    if (!EnsureFrame()) return fail("no frame callback");
    if (g_releaseAt >= 0) Guarded([] { ReleasePins(true); });  // the last landing's references, early
    g_req = Request{};
    g_req.owner = owner;
    bool started = false;
    std::string busy;
    Guarded([&] {
        Il2CppObject* sc = nullptr;
        bool found = false;
        if (!CallValue(g_s.tryGetSceneContext, nullptr, {&sc}, "SceneContextUtility.TryGetSceneContext", found) || !found ||
            !sc || !unity::IsAlive(sc))
            return;
        Il2CppObject* go = Call(g_s.scPlayer, sc, {}, "SceneContext.get_Player");
        if (!go || !unity::IsAlive(go)) return;
        if (!g_checksLogged) {
            g_checksLogged = true;
            SR2_INFO("ground_teleport: knock-out check {}, scene load check {}",
                     !g_s.deathHandler || !g_s.scriptedBoolValue ? "unavailable (symbols)"
                     : unity::GetComponent(go, g_s.deathHandler) ? "on (PlayerDeathHandler on the player)"
                                                                 : "OFF (no PlayerDeathHandler on the player object)",
                     SceneLoader() ? "on" : "OFF (no SceneLoader found)");
        }
        busy = Unavailable(go);
        if (!busy.empty()) return;
        Il2CppObject* player = unity::GetComponent(go, g_s.teleportable);
        if (!player) player = unity::FindObjectOfType(g_s.teleportable);
        if (!player) return;
        Il2CppObject* group = Call(g_s.sceneGroup, player, {}, "TeleportablePlayer.get_SceneGroup");
        Vec3f from;
        if (!group || !Position(player, from)) return;
        if (Il2CppObject* t = unity::TransformOf(player))
            g_req.haveEuler = CallValue(g_s.trEuler, t, {}, "Transform.get_eulerAngles", g_req.euler) && Finite(g_req.euler);
        int layer = -1;
        CallValue(g_s.goLayer, unity::GameObjectOf(player), {}, "GameObject.get_layer", layer);
        g_req.playerLayer = layer;
        int matrix = 0;
        if (layer >= 0 && layer < 32 && CallValue(g_s.layersCollided, nullptr, {&layer}, "Layers.GetLayersCollidedWith", matrix) &&
            matrix != 0) {
            g_req.mask = matrix & ~(1 << layer);  // what the player stands on; never himself
            g_req.matrixMask = true;
        } else {
            g_req.mask = ~(1 << 2);  // Ignore Raycast
            if (layer > 0 && layer < 32) g_req.mask &= ~(1 << layer);
        }
        g_req.x = x;
        g_req.z = z;
        g_req.from = from;
        g_req.done = std::move(done);
        g_req.active = true;
        started = true;
        g_req.player = unity::NewGCHandle(player);
        g_req.group = unity::NewGCHandle(group);
        if (!g_req.player || !g_req.group) {
            Refuse("no GC handle", player);
            return;
        }
        g_req.regionsLoaded = PinRegions(player, group, x, z);
        g_req.regionsPinned = static_cast<int>(g_pins.size());
        g_req.loadedFirst = g_req.regionsLoaded > 0;
        if (!g_req.loadedFirst) {  // everything there is loaded already: decide now
            Pick p = PickFloor(x, z, player);
            if (p.ok) Land(player, group, p);
            else Refuse(p.why, player);
            return;
        }
        SR2_INFO("ground_teleport[{}]: target ({:.1f}, {:.1f}) is {:.0f} m away: {} of {} region(s) there loaded on the spot, "
                 "the player stays at ({:.1f}, {:.1f}, {:.1f}) meanwhile; {}",
                 Owner(), x, z, std::hypot(x - from.x, z - from.z), g_req.regionsLoaded, g_req.regionsPinned, from.x, from.y,
                 from.z, LoaderSizes(player));
    });
    if (!started) {
        g_req = Request{};
        if (g_pins.empty()) StopFrame();
        if (!busy.empty()) return fail(busy);
        return fail(g_dead ? "internal error" : "no player (load a save first)");
    }
    return true;
}

}  // namespace sr2::modules::ground_teleport
