// Teleport onto the ground below a world (x, z). See ground_teleport.h and modules/map_tools/NOTES.md
// ("Finding the ground").
//
// Every game call is either Resolver::Invoke (managed exceptions and native faults captured) or, for
// TeleportablePlayer.TeleportTo (its Nullable<Vector3> argument), a direct call under GuardedCallEx. A native
// fault marks the owning module faulted and switches this helper off for the session.

#include "modules/map_tools/ground_teleport.h"

#include <windows.h>

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
constexpr float kRayTop = 2000.0f;     // ground ray from this height straight down (SR2's worlds stay well below)
constexpr float kRayLength = 4000.0f;
constexpr float kLandOffset = 1.0f;    // land this far above the hit, then fall
constexpr float kNearDist = 40.0f;     // closer than this (horizontally): the area is loaded, raycast at once
constexpr float kParkSide = 4.0f;      // parked this far beside the target column (never in the ray)
constexpr float kRepark = 0.5f;        // re-park this often (the player falls while the area loads)
constexpr float kSettle = 0.25f;       // first accepted hit this long after parking
constexpr float kStableGap = 0.2f;     // a hit counts when it is the same for two checks this far apart
constexpr float kStableDy = 0.5f;
constexpr float kWaitMax = 6.0f;       // give up after this long parked
constexpr int kSelfSkips = 4;          // hits on the player's own colliders skipped per ray

struct NullableVec3 {  // System.Nullable<Vector3>: hasValue false = keep the rotation
    bool hasValue = false;
    Vec3f value;
};
using TeleportToFn = void (*)(Il2CppObject*, Vec3f, Il2CppObject*, NullableVec3, bool, const MethodInfo*);

struct Syms {
    const MethodInfo* tryGetSceneContext = nullptr;  // SceneContextUtility.TryGetSceneContext(out SceneContext)
    const MethodInfo* scPlayer = nullptr;            // SceneContext.get_Player (GameObject)
    Il2CppClass* teleportable = nullptr;             // TeleportablePlayer
    const MethodInfo* teleportTo = nullptr;          // TeleportablePlayer.TeleportTo(Vector3, SceneGroup, Vector3?, bool)
    TeleportToFn teleportToFn = nullptr;
    const MethodInfo* sceneGroup = nullptr;          // TeleportablePlayer.get_SceneGroup
    const MethodInfo* raycast = nullptr;             // Physics.Raycast(Vector3, Vector3, out RaycastHit, float, int, QTI)
    size_t hitPoint = 0, hitDistance = 0, hitCollider = 0;  // RaycastHit fields (unboxed struct offsets)
    const MethodInfo* idToObject = nullptr;          // Resources.InstanceIDToObject(int) (optional: hit names)
    const MethodInfo* goLayer = nullptr;             // GameObject.get_layer
    const MethodInfo* trPosition = nullptr;          // Transform.get_position
    const MethodInfo* trParent = nullptr;            // Transform.get_parent
    Il2CppClass* regionLoader = nullptr;             // RegionLoader (optional: its sizes for the log)
    size_t rlLoadSize = 0, rlWakeSize = 0;
    bool haveLoaderSizes = false;
};

Syms g_s;
std::mutex g_resolveMutex;
std::atomic<int> g_state{0};  // 0 not tried, 1 ok, -1 failed
std::string g_err;
bool g_dead = false;          // a native fault: off for the session (main thread)

struct Request {
    bool active = false;
    std::string owner;
    Done done;
    float x = 0, z = 0;
    Il2CppGCHandle player = nullptr;  // TeleportablePlayer (pinned for the request)
    Il2CppGCHandle group = nullptr;   // its SceneGroup at the start
    Vec3f from, park;
    int mask = 0, playerLayer = -1;
    float age = 0, parkedAt = -1, lastPark = -1;
    bool havePrev = false;
    float prevY = 0, prevT = 0;
    int rays = 0, selfHits = 0;
    std::string lastHit;  // the last hit seen (log)
};
Request g_req;
int g_frameCb = 0;

// ------------------------------------------------------------ game calls ---
struct Fault {};  // a native fault inside a game call: abort the request, owner faulted

Il2CppObject* Call(const MethodInfo* m, void* obj, std::initializer_list<void*> args, const char* what, bool* ok = nullptr) {
    if (ok) *ok = false;
    if (!m) return nullptr;
    std::vector<void*> params(args);
    std::string err;
    Il2CppObject* r = Resolver::Get().Invoke(m, obj, params.empty() ? nullptr : params.data(), &err);
    if (!err.empty()) {
        if (err.rfind("native exception", 0) == 0) {
            SR2_ERROR("ground_teleport[{}]: {}: {}", g_req.owner, what, err);
            throw Fault{};
        }
        SR2_WARN("ground_teleport[{}]: {} failed: {}", g_req.owner, what, err);
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

// TeleportablePlayer.TeleportTo(position, sceneGroup, keep rotation, overlayEnabled).
bool TeleportTo(Il2CppObject* player, Il2CppObject* group, Vec3f pos, bool overlay) {
    struct Ctx {
        Il2CppObject *player, *group;
        Vec3f pos;
        bool overlay;
    } ctx{player, group, pos, overlay};
    std::string err;
    core::FaultInfo fi;
    bool ok = core::GuardedCallEx(
        [](void* p) {
            auto* c = static_cast<Ctx*>(p);
            g_s.teleportToFn(c->player, c->pos, c->group, NullableVec3{}, c->overlay, g_s.teleportTo);
        },
        &ctx, &err, &fi);
    if (ok) return true;
    if (fi.code != 0) {
        SR2_ERROR("ground_teleport[{}]: TeleportablePlayer.TeleportTo: native fault {}", g_req.owner, err);
        throw Fault{};
    }
    SR2_WARN("ground_teleport[{}]: TeleportablePlayer.TeleportTo threw: {}", g_req.owner, err);
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

// The ground below (x, z): the first collider from above that is not the player's. `hitY` = its height.
bool Ray(float x, float z, Il2CppObject* player, float& hitY, std::string* name) {
    Il2CppObject* playerT = unity::TransformOf(player);
    float top = kRayTop;
    for (int skip = 0; skip <= kSelfSkips; ++skip) {
        alignas(16) unsigned char hit[64] = {};
        Vec3f origin{x, top, z}, down{0, -1, 0};
        float len = kRayLength - (kRayTop - top);
        int mask = g_req.mask, triggers = 1;  // QueryTriggerInteraction.Ignore
        bool any = false;
        ++g_req.rays;
        if (!CallValue(g_s.raycast, nullptr, {&origin, &down, hit, &len, &mask, &triggers}, "Physics.Raycast", any) || !any)
            return false;
        Vec3f p = *reinterpret_cast<Vec3f*>(hit + g_s.hitPoint);
        float d = *reinterpret_cast<float*>(hit + g_s.hitDistance);
        if (!Finite(p) || !std::isfinite(d) || std::fabs(p.x - x) > 1.0f || std::fabs(p.z - z) > 1.0f) return false;
        Il2CppObject* col = nullptr;
        if (g_s.idToObject) {
            int id = *reinterpret_cast<int*>(hit + g_s.hitCollider);
            col = Call(g_s.idToObject, nullptr, {&id}, "Resources.InstanceIDToObject");
            if (col && !unity::IsAlive(col)) col = nullptr;
        }
        std::string colName = col ? unity::GetName(unity::GameObjectOf(col)) : std::string("?");
        g_req.lastHit = std::format("'{}' at y {:.1f} ({:.1f} m down)", colName, p.y, d);
        if (col && playerT && UnderPlayer(unity::TransformOf(col), playerT)) {  // the player's own collider
            ++g_req.selfHits;
            top = p.y - 0.05f;
            continue;
        }
        hitY = p.y;
        if (name) *name = colName;
        return true;
    }
    return false;
}

void StopFrame() {
    if (g_frameCb) main_thread::RemoveFrameCallback(g_frameCb);
    g_frameCb = 0;
}

void Finish(Result r) {
    r.seconds = g_req.age;
    r.from = g_req.from;
    r.parked = g_req.parkedAt >= 0;
    Done done = std::move(g_req.done);
    for (Il2CppGCHandle h : {g_req.player, g_req.group})
        if (h) unity::FreeGCHandle(h);
    g_req = Request{};
    StopFrame();
    if (done) done(r);
}

std::string RayParams() {
    return std::format("Physics.Raycast(origin y {:.0f}, down, {:.0f} m, layer mask 0x{:08X} (all but Ignore Raycast "
                       "and the player's layer {}), triggers ignored)",
                       kRayTop, kRayLength, static_cast<unsigned>(g_req.mask), g_req.playerLayer);
}

std::string LoaderSizes(Il2CppObject* player) {
    if (!g_s.haveLoaderSizes) return "region loader sizes unknown";
    Il2CppObject* rl = unity::GetComponent(player, g_s.regionLoader);
    if (!rl) return "no RegionLoader on the player";
    Vec3f load = unity::Field<Vec3f>(rl, g_s.rlLoadSize), wake = unity::Field<Vec3f>(rl, g_s.rlWakeSize);
    return std::format("RegionLoader LoadSize ({:.0f}, {:.0f}, {:.0f}), WakeSize ({:.0f}, {:.0f}, {:.0f})", load.x, load.y,
                       load.z, wake.x, wake.y, wake.z);
}

void Land(Il2CppObject* player, Il2CppObject* group, float groundY, const std::string& name) {
    Vec3f dest{g_req.x, groundY + kLandOffset, g_req.z};
    Result r;
    if (!TeleportTo(player, group, dest, true)) {
        r.why = "the game's teleport call failed";
        Finish(r);
        return;
    }
    r.ok = true;
    r.to = dest;
    SR2_INFO("ground_teleport[{}]: landed at ({:.1f}, {:.1f}, {:.1f}) on '{}' from ({:.1f}, {:.1f}, {:.1f}), {:.0f} m, {}{:.1f} s, "
             "{} ray(s){}",
             g_req.owner, dest.x, dest.y, dest.z, name, g_req.from.x, g_req.from.y, g_req.from.z,
             std::hypot(dest.x - g_req.from.x, dest.z - g_req.from.z),
             g_req.parkedAt >= 0 ? "after loading the area, " : "", g_req.age, g_req.rays,
             g_req.selfHits ? std::format(", {} hit(s) on the player skipped", g_req.selfHits) : std::string());
    Finish(r);
}

void Refuse(const std::string& why, Il2CppObject* player, Il2CppObject* group, bool goBack) {
    if (goBack && player && group && g_req.parkedAt >= 0) TeleportTo(player, group, g_req.from, true);
    SR2_WARN("ground_teleport[{}]: no ground below ({:.1f}, {:.1f}): {}. {}; {} ray(s), last hit {}; player from "
             "({:.1f}, {:.1f}, {:.1f}){}; {}",
             g_req.owner, g_req.x, g_req.z, why, RayParams(), g_req.rays, g_req.lastHit.empty() ? "none" : g_req.lastHit,
             g_req.from.x, g_req.from.y, g_req.from.z,
             g_req.parkedAt >= 0 ? std::format(", parked at ({:.1f}, {:.1f}, {:.1f}) for {:.1f} s and sent back", g_req.park.x,
                                               g_req.park.y, g_req.park.z, g_req.age - g_req.parkedAt)
                                 : std::string(),
             player ? LoaderSizes(player) : std::string("no player"));
    Result r;
    r.why = why;
    Finish(r);
}

void Park(Il2CppObject* player, Il2CppObject* group) {
    if (!TeleportTo(player, group, g_req.park, false)) {
        Refuse("the game's teleport call failed", player, group, true);
        return;
    }
    if (g_req.parkedAt < 0) {
        g_req.parkedAt = g_req.age;
        SR2_INFO("ground_teleport[{}]: target ({:.1f}, {:.1f}) is {:.0f} m away: player parked at ({:.1f}, {:.1f}, {:.1f}) "
                 "while the area loads; {}",
                 g_req.owner, g_req.x, g_req.z, std::hypot(g_req.x - g_req.from.x, g_req.z - g_req.from.z), g_req.park.x,
                 g_req.park.y, g_req.park.z, LoaderSizes(player));
    }
    g_req.lastPark = g_req.age;
}

// One step of a parked request (every frame).
void Step(float dt) {
    g_req.age += dt;
    Il2CppObject* player = Target(g_req.player);
    Il2CppObject* group = Target(g_req.group);
    if (!player || !unity::IsAlive(player) || !group) {
        Refuse("the player went away (scene change?)", nullptr, nullptr, false);
        return;
    }
    Il2CppObject* now = Call(g_s.sceneGroup, player, {}, "TeleportablePlayer.get_SceneGroup");
    if (now != group) {
        Refuse("the scene changed", nullptr, nullptr, false);
        return;
    }
    float sinceParked = g_req.age - g_req.parkedAt;
    if (sinceParked >= kSettle) {
        float y = 0;
        std::string name;
        if (Ray(g_req.x, g_req.z, player, y, &name)) {
            if (g_req.havePrev && std::fabs(y - g_req.prevY) < kStableDy) {
                if (g_req.age - g_req.prevT >= kStableGap) {
                    Land(player, group, y, name);
                    return;
                }
            } else {
                g_req.havePrev = true;
                g_req.prevY = y;
                g_req.prevT = g_req.age;
            }
        } else {
            g_req.havePrev = false;
        }
    }
    if (sinceParked >= kWaitMax) {
        Refuse(std::format("nothing to stand on after {:.0f} s", kWaitMax), player, group, true);
        return;
    }
    if (g_req.age - g_req.lastPark >= kRepark) Park(player, group);
}

template <class F>
void Guarded(F&& f) {
    try {
        f();
    } catch (const Fault&) {
        g_dead = true;
        std::string owner = g_req.owner;
        core::MarkModuleFaulted(owner, "native fault in the ground teleport (see the line above)");
        g_req.done = nullptr;  // the owner is faulted: no callback into it
        Result r;
        r.why = "internal error";
        Finish(r);
        SR2_ERROR("ground_teleport: off for the rest of this session after a native fault ({})", owner);
    }
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
    field(hit, "m_Distance", s.hitDistance, true);
    field(hit, "m_Collider", s.hitCollider, true);
    if (s.teleportTo) {
        s.teleportToFn = reinterpret_cast<TeleportToFn>(R.MethodPointer(s.teleportTo, &e));
        if (!s.teleportToFn && err.empty()) err = "TeleportablePlayer.TeleportTo: no native body";
    }
    if (!err.empty()) return false;
    // optional
    std::string opt;
    if (Il2CppClass* res = R.Class("UnityEngine.CoreModule", "UnityEngine", "Resources", &opt))
        s.idToObject = R.Method(res, "InstanceIDToObject", 1, &opt);
    s.regionLoader = R.Class(kImage, "MonomiPark.SlimeRancher.Regions", "RegionLoader", &opt);
    if (s.regionLoader) {
        auto a = R.FieldOffset(s.regionLoader, "LoadSize", &opt), b = R.FieldOffset(s.regionLoader, "WakeSize", &opt);
        if (a && b) {
            s.rlLoadSize = *a;
            s.rlWakeSize = *b;
            s.haveLoaderSizes = true;
        }
    }
    g_s = s;
    SR2_INFO("ground_teleport: resolved (hit names {}, region loader sizes {})", s.idToObject ? "yes" : "no",
             s.haveLoaderSizes ? "yes" : "no");
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

bool Start(const char* owner, float x, float z, Done done, std::string* why) {
    auto fail = [&](const char* w) {
        if (why) *why = w;
        return false;
    };
    if (!Resolved()) return fail("the ground teleport is not available (see the log)");
    if (g_req.active) return fail("a teleport is still running");
    if (!std::isfinite(x) || !std::isfinite(z)) return fail("bad position");
    if (core::ModuleFaulted(owner)) return fail("module faulted");
    g_req = Request{};
    g_req.owner = owner;
    bool started = false;
    Guarded([&] {
        Il2CppObject* sc = nullptr;
        bool found = false;
        if (!CallValue(g_s.tryGetSceneContext, nullptr, {&sc}, "SceneContextUtility.TryGetSceneContext", found) || !found ||
            !sc || !unity::IsAlive(sc))
            return;
        Il2CppObject* go = Call(g_s.scPlayer, sc, {}, "SceneContext.get_Player");
        if (!go || !unity::IsAlive(go)) return;
        Il2CppObject* player = unity::GetComponent(go, g_s.teleportable);
        if (!player) player = unity::FindObjectOfType(g_s.teleportable);
        if (!player) return;
        Il2CppObject* group = Call(g_s.sceneGroup, player, {}, "TeleportablePlayer.get_SceneGroup");
        Vec3f from;
        if (!group || !Position(player, from)) return;
        int layer = -1;
        CallValue(g_s.goLayer, unity::GameObjectOf(player), {}, "GameObject.get_layer", layer);
        g_req.mask = ~(1 << 2);  // Ignore Raycast
        if (layer >= 0 && layer < 32 && layer != 0) g_req.mask &= ~(1 << layer);  // never drop Default
        g_req.playerLayer = layer;
        g_req.x = x;
        g_req.z = z;
        g_req.from = from;
        g_req.done = std::move(done);
        g_req.active = true;
        started = true;
        // Near: the area is loaded, one ray.
        float y = 0;
        std::string name;
        if (std::hypot(x - from.x, z - from.z) <= kNearDist && Ray(x, z, player, y, &name)) {
            Land(player, group, y, name);
            return;
        }
        g_req.player = unity::NewGCHandle(player);
        g_req.group = unity::NewGCHandle(group);
        if (!g_req.player || !g_req.group) {
            Refuse("no GC handle", player, group, false);
            return;
        }
        // Parked beside the column (away from the player's start), at the current height.
        float dx = from.x - x, dz = from.z - z, len = std::hypot(dx, dz);
        if (len < 1e-3f) dx = 1, dz = 0, len = 1;
        g_req.park = {x + dx / len * kParkSide, from.y, z + dz / len * kParkSide};
        Park(player, group);
        if (g_req.active && !g_frameCb)
            g_frameCb = main_thread::AddFrameCallback("ground_teleport", [](float dt) {
                if (!g_req.active) {
                    StopFrame();
                    return;
                }
                Guarded([&] { Step(dt); });
            });
        if (g_req.active && !g_frameCb) Refuse("no frame callback", player, group, true);
    });
    if (!started) {
        g_req = Request{};
        return fail(g_dead ? "internal error" : "no player (load a save first)");
    }
    return true;
}

}  // namespace sr2::modules::ground_teleport
