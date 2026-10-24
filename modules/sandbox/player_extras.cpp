// sandbox: player extras (see player_extras.h and NOTES.md, "Player extras"). SR2 1.3.0.
//
// - Infinite health: a PlayerState.Damage detour returns "not dead" without
//   applying the damage while the toggle is on, plus a 10 Hz top-up to the
//   maximum (like infinite energy) for health lost any other way.
// - Teleport to marker: MapDirector.TryGetPlayerNavigationMarkerLocation for
//   the current scene group and zone (the marker's position has y = 0), a
//   downward raycast for the ground, then TeleportNetwork.CompleteInSceneTeleport,
//   the game's own in-scene teleport (TeleportablePlayer.TeleportTo: controller
//   position, scene group, region proxies, held item, screen effects, audio).
// - Unlock ranch expansions: for every AccessDoorUIActivator whose door is
//   locked, what AccessDoorUIActivator.OnPurchaseMenuResult does after the
//   purchase, minus spending, analytics and the achievement: door OPEN, linked
//   locked doors CLOSED, the door's OnUnlockedEvent raised. The door state lives
//   in AccessDoorModel, which the save stores.
//
// Every game call is Resolver::Invoke on the main thread; a native fault marks
// the module faulted, as in module.cpp.

#include "modules/sandbox/player_extras.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <format>
#include <initializer_list>
#include <string>
#include <vector>

#include "core/detour_guard.h"
#include "core/il2cpp_api.h"
#include "core/log.h"
#include "core/module.h"
#include "core/unity.h"
#include "modules/custom_upgrades/sale_notify.h"
#include "ui/sandbox_bridge.h"
#include "ui/sandbox_extras_bridge.h"

namespace sr2::modules::sandbox {
namespace {

namespace unity = sr2::core::unity;
namespace extras = sr2::ui::sandbox::extras;
using sr2::il2cpp::api;
using sr2::il2cpp::Resolver;

constexpr const char* kModule = "sandbox";
constexpr const char* kImage = "Assembly-CSharp";
constexpr unsigned long long kHealthMs = 100;      // infinite-health top-up
constexpr unsigned long long kExpansionMs = 5000;  // rescan the expansion doors (only while the page is open)
constexpr float kRayTop = 1500.0f;                 // ground search: from this height straight down
constexpr float kRayLength = 3000.0f;
constexpr float kAboveGround = 1.0f;               // land this far above the hit, then fall
constexpr int kDoorLocked = 0, kDoorOpen = 1, kDoorClosed = 2;  // AccessDoor.State

struct Vec3 {
    float x, y, z;
};

std::atomic<bool> g_live{false};      // module enabled
std::atomic<bool> g_infinite{false};  // infinite health (session only)
bool g_hookReady = false;
unsigned long long g_lastHealth = 0, g_lastScan = 0;
int g_locked = -1, g_total = 0;  // last expansion scan

// ---- resolved in SetupPlayerExtras ----
Il2CppClass* g_playerStateClass = nullptr;
const MethodInfo* g_tryGetPlayerState = nullptr;  // SceneContextUtility.TryGetPlayerState(out PlayerState)
const MethodInfo* g_getHealth = nullptr;          // PlayerState.GetCurrHealth
const MethodInfo* g_getMaxHealth = nullptr;       // PlayerState.GetMaxHealth
const MethodInfo* g_setHealth = nullptr;          // PlayerState.SetHealth(int)
std::string g_healthWhy;                          // "" = infinite health available

using DamageFn = bool (*)(Il2CppObject*, Il2CppObject*, const MethodInfo*);
DamageFn g_origDamage = nullptr;
sr2::core::DetourGuard g_damageGuard{kModule, "PlayerState.Damage"};

std::string g_teleportWhy;                         // "" = teleport available
const MethodInfo* g_tryGetSceneContext = nullptr;  // SceneContextUtility.TryGetSceneContext(out SceneContext)
const MethodInfo* g_scRegions = nullptr;           // SceneContext.get_RegionRegistry
const MethodInfo* g_scMap = nullptr;               // SceneContext.get_MapDirector
const MethodInfo* g_scZoneTracker = nullptr;       // SceneContext.get_PlayerZoneTracker
const MethodInfo* g_scTeleport = nullptr;          // SceneContext.get_TeleportNetwork
const MethodInfo* g_scPlayer = nullptr;            // SceneContext.get_Player (GameObject)
const MethodInfo* g_currentGroup = nullptr;        // RegionRegistry.get_CurrentSceneGroup
const MethodInfo* g_currentZone = nullptr;         // PlayerZoneTracker.GetCurrentZone
const MethodInfo* g_markerLocation = nullptr;      // MapDirector.TryGetPlayerNavigationMarkerLocation(out Vector3, SceneGroup, ZoneDefinition)
const MethodInfo* g_inSceneTeleport = nullptr;     // TeleportNetwork.CompleteInSceneTeleport(SceneGroup, Vector3, Vector3)
const MethodInfo* g_raycast = nullptr;             // Physics.Raycast(Vector3, Vector3, out RaycastHit, float, int, QueryTriggerInteraction)
size_t g_offHitDistance = 0;                       // RaycastHit.m_Distance (in the unboxed struct)
const MethodInfo* g_trPosition = nullptr;          // Transform.get_position
const MethodInfo* g_trEuler = nullptr;             // Transform.get_eulerAngles

std::string g_expansionWhy;                        // "" = expansion unlock available
Il2CppClass* g_activatorClass = nullptr;           // MonomiPark.SlimeRancher.UI.AccessDoor.AccessDoorUIActivator
Il2CppClass* g_doorClass = nullptr;                // MonomiPark.World.AccessDoor
size_t g_offActDoor = 0;                           // AccessDoorUIActivator._accessDoor
size_t g_offActItem = 0;                           // AccessDoorUIActivator._purchaseMenuItem
size_t g_offActLegacy = 0;                         // AccessDoorUIActivator._legacyUIPrefab
size_t g_offDoorLinked = 0;                        // AccessDoor.LinkedDoors (AccessDoor[])
size_t g_offDoorEvent = 0;                         // AccessDoor.OnUnlockedEvent (StaticGameEvent)
size_t g_offDoorModel = 0;                         // AccessDoor._model (AccessDoorModel)
const MethodInfo* g_doorGetState = nullptr;        // AccessDoor.get_CurrState
const MethodInfo* g_doorSetState = nullptr;        // AccessDoor.set_CurrState(State)
const MethodInfo* g_raiseEvent = nullptr;          // StaticGameEvent.RaiseEvent

bool IsA(Il2CppObject* obj, Il2CppClass* k) {
    const auto& a = api();
    if (!obj || !k || !a.il2cpp_object_get_class) return false;
    Il2CppClass* c = a.il2cpp_object_get_class(obj);
    if (c == k) return true;
    return c && a.il2cpp_class_is_assignable_from && a.il2cpp_class_is_assignable_from(k, c);
}

bool Live() { return g_live.load() && !sr2::core::ModuleFaulted(kModule); }

Il2CppObject* Call(const MethodInfo* m, void* obj, std::initializer_list<void*> args, const char* what, bool* ok = nullptr) {
    if (ok) *ok = false;
    if (!m || sr2::core::ModuleFaulted(kModule)) return nullptr;
    std::vector<void*> params(args);
    std::string err;
    Il2CppObject* r = Resolver::Get().Invoke(m, obj, params.empty() ? nullptr : params.data(), &err);
    if (!err.empty()) {
        if (err.rfind("native exception", 0) == 0) sr2::core::MarkModuleFaulted(kModule, std::format("{}: {}", what, err));
        else SR2_WARN("[{}] {} failed: {}", kModule, what, err);
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

// The HUD's drone-style pop-ups (custom_upgrades/sale_notify.h), for "teleport to marker" with the teleporter icon.
sr2::modules::custom_upgrades::ui::SaleNotices g_notices;
bool g_inTeleport = false;  // Done() is reporting a teleport: also a pop-up
unsigned long long g_lastNoticeTick = 0;

void Done(bool ok, const std::string& text) {
    if (ok) SR2_INFO("[{}] {}", kModule, text);
    else SR2_WARN("[{}] {}", kModule, text);
    sr2::ui::sandbox::SetStatus(text, !ok);
    if (g_inTeleport && g_notices.TextReady())
        g_notices.QueueText(g_notices.TeleporterIcon(), ok ? std::string("Teleported to the map marker") : text);
}

// ------------------------------------------------------------ health ----
bool DamageDetour(Il2CppObject* self, Il2CppObject* damage, const MethodInfo* mi) {
    bool block = false;
    if (g_damageGuard.Run([&] {
            block = g_infinite.load(std::memory_order_relaxed) && g_live.load(std::memory_order_relaxed) &&
                    IsA(self, g_playerStateClass);
        }) &&
        block) {
        return false;  // no health lost, not dead
    }
    return g_origDamage(self, damage, mi);
}

Il2CppObject* PlayerState() {
    Il2CppObject* player = nullptr;
    bool found = false;
    if (!CallValue(g_tryGetPlayerState, nullptr, {&player}, "SceneContextUtility.TryGetPlayerState", found) || !found ||
        !IsA(player, g_playerStateClass) || !unity::IsAlive(player))
        return nullptr;
    return player;
}

void TopUpHealth() {
    Il2CppObject* player = PlayerState();
    if (!player) return;
    int cur = 0, max = 0;
    if (!CallValue(g_getHealth, player, {}, "PlayerState.GetCurrHealth", cur) ||
        !CallValue(g_getMaxHealth, player, {}, "PlayerState.GetMaxHealth", max))
        return;
    if (max > 0 && cur < max) Call(g_setHealth, player, {&max}, "PlayerState.SetHealth");
}

// ---------------------------------------------------------- teleport ----
struct Scene {
    Il2CppObject* context = nullptr;
    Il2CppObject* group = nullptr;  // current SceneGroup
};

bool GetScene(Scene& s) {
    Il2CppObject* sc = nullptr;
    bool found = false;
    if (!CallValue(g_tryGetSceneContext, nullptr, {&sc}, "SceneContextUtility.TryGetSceneContext", found) || !found ||
        !sc || !unity::IsAlive(sc))
        return false;
    s.context = sc;
    Il2CppObject* regions = Call(g_scRegions, sc, {}, "SceneContext.RegionRegistry");
    s.group = regions ? Call(g_currentGroup, regions, {}, "RegionRegistry.CurrentSceneGroup") : nullptr;
    return s.group != nullptr;
}

// The player's map marker in world space (y as stored: 0), if it is on the map of the current zone.
bool MarkerLocation(const Scene& s, Vec3& pos, std::string& why) {
    Il2CppObject* map = Call(g_scMap, s.context, {}, "SceneContext.MapDirector");
    if (!map || !unity::IsAlive(map)) {
        why = "The map is not available right now.";
        return false;
    }
    Il2CppObject* zone = nullptr;  // null is fine: only skips the "player is drawn in the zone's centre" check
    if (Il2CppObject* tracker = Call(g_scZoneTracker, s.context, {}, "SceneContext.PlayerZoneTracker"))
        if (unity::IsAlive(tracker)) zone = Call(g_currentZone, tracker, {}, "PlayerZoneTracker.GetCurrentZone");
    bool found = false;
    pos = {};
    if (!CallValue(g_markerLocation, map, {&pos, s.group, zone}, "MapDirector.TryGetPlayerNavigationMarkerLocation", found) ||
        !found) {
        why = "No map marker in this area. Place one on the map (the waypoint beam) first; markers in another zone "
              "don't count.";
        return false;
    }
    if (!std::isfinite(pos.x) || !std::isfinite(pos.z)) {
        why = "The map marker has no valid position.";
        return false;
    }
    return true;
}

void DoTeleportImpl();
void DoTeleport() {
    g_inTeleport = true;
    DoTeleportImpl();
    g_inTeleport = false;
}

void DoTeleportImpl() {
    if (!g_teleportWhy.empty()) {
        Done(false, "Not teleported: " + g_teleportWhy);
        return;
    }
    Scene s;
    std::string why;
    Vec3 marker{};
    if (!GetScene(s)) {
        Done(false, "Not teleported: load a save first.");
        return;
    }
    if (!MarkerLocation(s, marker, why)) {
        Done(false, "Not teleported: " + why);
        return;
    }
    // The marker only has x / z (the map is flat; PlaceNavigationMarker stores y = 0): find the ground below it.
    alignas(16) unsigned char hit[64] = {};
    Vec3 origin{marker.x, kRayTop, marker.z}, down{0, -1, 0};
    float maxDist = kRayLength;
    int layers = -5;   // Physics.DefaultRaycastLayers (everything but Ignore Raycast)
    int triggers = 1;  // QueryTriggerInteraction.Ignore
    bool any = false;
    if (!CallValue(g_raycast, nullptr, {&origin, &down, hit, &maxDist, &layers, &triggers}, "Physics.Raycast", any) || !any) {
        Done(false, std::format("Not teleported: no ground found below the marker ({:.0f}, {:.0f})", marker.x, marker.z));
        return;
    }
    const float d = *reinterpret_cast<float*>(hit + g_offHitDistance);
    if (!std::isfinite(d)) {
        Done(false, "Not teleported: the ground check returned no distance");
        return;
    }
    Vec3 target{marker.x, kRayTop - d + kAboveGround, marker.z};

    Il2CppObject* go = Call(g_scPlayer, s.context, {}, "SceneContext.Player");
    Il2CppObject* tr = go && unity::IsAlive(go) ? unity::TransformOf(go) : nullptr;
    Vec3 from{}, euler{};
    if (!tr || !CallValue(g_trPosition, tr, {}, "Transform.get_position", from) ||
        !CallValue(g_trEuler, tr, {}, "Transform.get_eulerAngles", euler)) {
        Done(false, "Not teleported: the player is not available right now");
        return;
    }
    euler.x = euler.z = 0;  // keep the heading, stand upright
    Il2CppObject* net = Call(g_scTeleport, s.context, {}, "SceneContext.TeleportNetwork");
    if (!net || !unity::IsAlive(net)) {
        Done(false, "Not teleported: the teleport network is not available");
        return;
    }
    bool ok = false;
    Call(g_inSceneTeleport, net, {s.group, &target, &euler}, "TeleportNetwork.CompleteInSceneTeleport", &ok);
    const float dx = target.x - from.x, dz = target.z - from.z;
    Done(ok, ok ? std::format("Teleported to the map marker: ({:.0f}, {:.0f}, {:.0f}) -> ({:.0f}, {:.0f}, {:.0f}), {:.0f} m",
                              from.x, from.y, from.z, target.x, target.y, target.z, std::sqrt(dx * dx + dz * dz))
                : "Not teleported: the game's teleport call failed");
}

// -------------------------------------------------------- expansions ----
struct Expansion {
    Il2CppObject* door;
    std::string name;
    int state;
};

// Every loaded AccessDoorUIActivator with a purchase item and a live door that has its model
// (prefabs and unloaded scenes have none). One FindObjectsOfTypeAll; only on demand or every 5 s
// while the page is open.
std::vector<Expansion> ScanExpansions() {
    std::vector<Expansion> out;
    for (Il2CppObject* act : unity::FindObjectsOfTypeAll(g_activatorClass)) {
        if (!IsA(act, g_activatorClass) || !unity::IsAlive(act)) continue;
        auto* door = unity::Field<Il2CppObject*>(act, g_offActDoor);
        if (!IsA(door, g_doorClass) || !unity::IsAlive(door) || !unity::Field<Il2CppObject*>(door, g_offDoorModel)) continue;
        auto* item = unity::Field<Il2CppObject*>(act, g_offActItem);
        if (!item && !unity::Field<Il2CppObject*>(act, g_offActLegacy)) continue;  // not a purchase (TryInitialize would fail)
        int state = -1;
        if (!CallValue(g_doorGetState, door, {}, "AccessDoor.CurrState", state)) continue;
        std::string name = item && unity::IsAlive(item) ? unity::GetName(item) : std::string();
        if (name.empty()) name = unity::GetName(door);
        out.push_back({door, std::move(name), state});
    }
    return out;
}

void Rescan() {
    g_lastScan = GetTickCount64();
    if (!g_expansionWhy.empty()) return;
    const auto list = ScanExpansions();
    g_total = static_cast<int>(list.size());
    g_locked = 0;
    for (const auto& e : list) g_locked += e.state == kDoorLocked;
}

void DoUnlockExpansions() {
    if (!g_expansionWhy.empty()) {
        Done(false, "Expansions not unlocked: " + g_expansionWhy);
        return;
    }
    if (!PlayerState()) {
        Done(false, "Expansions not unlocked: load a save first.");
        return;
    }
    const auto list = ScanExpansions();
    std::string names;
    int opened = 0, failed = 0;
    for (const auto& e : list) {
        if (e.state != kDoorLocked || !unity::IsAlive(e.door)) continue;
        int open = kDoorOpen;
        bool ok = false;
        Call(g_doorSetState, e.door, {&open}, "AccessDoor.set_CurrState(OPEN)", &ok);
        if (!ok) {
            ++failed;
            continue;
        }
        // As OnPurchaseMenuResult: linked doors that are still locked become CLOSED (openable).
        void* linked = unity::Field<void*>(e.door, g_offDoorLinked);
        for (size_t i = 0, n = unity::ArrayLength(linked); i < n; ++i) {
            auto* l = static_cast<Il2CppObject*>(unity::ArrayAt(linked, i));
            int st = -1;
            if (!IsA(l, g_doorClass) || !unity::IsAlive(l) || !unity::Field<Il2CppObject*>(l, g_offDoorModel) ||
                !CallValue(g_doorGetState, l, {}, "AccessDoor.CurrState", st) || st != kDoorLocked)
                continue;
            int closed = kDoorClosed;
            Call(g_doorSetState, l, {&closed}, "AccessDoor.set_CurrState(CLOSED)");
        }
        if (auto* ev = unity::Field<Il2CppObject*>(e.door, g_offDoorEvent); ev && unity::IsAlive(ev))
            Call(g_raiseEvent, ev, {}, "StaticGameEvent.RaiseEvent");
        ++opened;
        names += (names.empty() ? "" : ", ") + e.name;
    }
    Rescan();
    if (opened == 0 && failed == 0) {
        Done(false, list.empty() ? "No ranch expansions found here. Go to your ranch (the Conservatory) first."
                                 : "Every ranch expansion is already unlocked.");
        return;
    }
    Done(failed == 0, std::format("Unlocked {} ranch expansion(s){}{}; kept when the game next saves", opened,
                                  names.empty() ? "" : ": " + names,
                                  failed ? std::format(" ({} failed, see the log)", failed) : std::string()));
}

// ------------------------------------------------------------- state ----
void Publish() {
    extras::State st;
    st.tick = GetTickCount64();
    st.infiniteHealth = g_infinite.load();
    st.healthHook = g_hookReady;
    if (!Live()) {
        st.teleportWhy = st.expansionWhy = "Sandbox is off.";
        extras::Publish(std::move(st));
        return;
    }
    if (!g_teleportWhy.empty()) {
        st.teleportWhy = "Teleport unavailable: " + g_teleportWhy;
    } else {
        Scene s;
        Vec3 pos{};
        if (!GetScene(s)) st.teleportWhy = "Load a save first.";
        else st.canTeleport = MarkerLocation(s, pos, st.teleportWhy);
    }
    if (!g_expansionWhy.empty()) {
        st.expansionWhy = "Unavailable: " + g_expansionWhy;
    } else {
        if (st.tick - g_lastScan >= kExpansionMs) Rescan();
        st.lockedExpansions = g_locked;
        st.totalExpansions = g_total;
        if (g_total == 0) st.expansionWhy = "No ranch expansions found here. Go to your ranch (the Conservatory) first.";
        else if (g_locked == 0) st.expansionWhy = "Every ranch expansion is already unlocked.";
    }
    extras::Publish(std::move(st));
}

void Handle(extras::Op op, long long value) {
    if (!Live()) {
        sr2::ui::sandbox::SetStatus("Sandbox is off.", true);
        return;
    }
    switch (op) {
        case extras::Op::InfiniteHealth: {
            const bool on = value != 0;
            if (on && !g_healthWhy.empty()) {
                Done(false, "Infinite health unavailable: " + g_healthWhy);
                break;
            }
            if (g_infinite.exchange(on) != on)
                Done(true, on ? std::format("Infinite health on (damage {}; health refilled to its maximum; not saved, off "
                                            "after a restart)",
                                            g_hookReady ? "is ignored" : "still applies, then refills")
                              : "Infinite health off");
            if (on) TopUpHealth();
            break;
        }
        case extras::Op::TeleportToMarker: DoTeleport(); break;
        case extras::Op::UnlockExpansions: DoUnlockExpansions(); break;
    }
    Publish();
}

void HandleThunk(extras::Op op, long long value) { Handle(op, value); }

// Searches the class and its parents.
const MethodInfo* FindMethod(Resolver& R, Il2CppClass* k, const char* name, int argc, std::string& why) {
    std::string err;
    for (Il2CppClass* c = k; c; c = api().il2cpp_class_get_parent ? api().il2cpp_class_get_parent(c) : nullptr)
        if (const MethodInfo* m = R.Method(c, name, argc, &err)) return m;
    if (k && why.empty()) why = std::format("method {}({}) not found", name, argc);
    return nullptr;
}

}  // namespace

void SetupPlayerExtras(sr2::core::ModuleContext& ctx) {
    auto& R = ctx.resolver();
    auto cls = [&](const char* image, const char* ns, const char* name, std::string& why) -> Il2CppClass* {
        std::string err;
        Il2CppClass* k = R.Class(image, ns, name, &err);
        if (!k && why.empty()) why = err.empty() ? std::format("class {}.{} not found", ns, name) : err;
        return k;
    };
    auto off = [&](Il2CppClass* k, const char* field, size_t& out, std::string& why) {
        if (!k) return;
        std::string err;
        if (auto o = R.FieldOffset(k, field, &err)) out = *o;
        else if (why.empty()) why = err.empty() ? std::format("field {} not found", field) : err;
    };
    auto method = [&](Il2CppClass* k, const char* name, int argc, std::string& why) {
        return FindMethod(R, k, name, argc, why);
    };
    auto byParams = [&](Il2CppClass* k, const char* name, std::initializer_list<std::string_view> params,
                        std::string& why) -> const MethodInfo* {
        if (!k) return nullptr;
        std::string err;
        const MethodInfo* m = R.MethodByParams(k, name, params, &err);
        if (!m && why.empty()) why = std::format("{}: {}", name, err);
        return m;
    };

    Il2CppClass* scu = nullptr;
    {
        // Infinite health: the top-up needs these; the Damage hook is optional on top.
        std::string& w = g_healthWhy;
        g_playerStateClass = cls(kImage, "", "PlayerState", w);
        scu = cls(kImage, "MonomiPark.SlimeRancher.Util", "SceneContextUtility", w);
        g_tryGetPlayerState = method(scu, "TryGetPlayerState", 1, w);
        g_getHealth = method(g_playerStateClass, "GetCurrHealth", 0, w);
        g_getMaxHealth = method(g_playerStateClass, "GetMaxHealth", 0, w);
        g_setHealth = method(g_playerStateClass, "SetHealth", 1, w);
        if (w.empty()) {
            std::string hw;
            const MethodInfo* damage = method(g_playerStateClass, "Damage", 1, hw);
            void* target = damage ? R.MethodPointer(damage, &hw) : nullptr;
            // HookAddress records a failure reason on the context; Setup still returns true, so it is only logged.
            if (target && ctx.HookAddress("PlayerState.Damage", target, reinterpret_cast<void*>(&DamageDetour),
                                          reinterpret_cast<void**>(&g_origDamage))) {
                g_hookReady = true;
                SR2_INFO("[{}] infinite health: PlayerState.Damage hook ready (+0x{:X})", kModule, Resolver::Rva(target));
            } else {
                SR2_WARN("[{}] infinite health: no PlayerState.Damage hook ({}); only the 10 Hz top-up", kModule,
                         hw.empty() ? ctx.failure() : hw);
            }
        } else {
            SR2_WARN("[{}] infinite health unavailable: {}", kModule, w);
        }
    }
    {
        std::string& w = g_teleportWhy;
        if (!scu) scu = cls(kImage, "MonomiPark.SlimeRancher.Util", "SceneContextUtility", w);
        Il2CppClass* sceneCtx = cls(kImage, "", "SceneContext", w);
        Il2CppClass* regions = cls(kImage, "MonomiPark.SlimeRancher.Regions", "RegionRegistry", w);
        Il2CppClass* tracker = cls(kImage, "", "PlayerZoneTracker", w);
        Il2CppClass* map = cls(kImage, "MonomiPark.SlimeRancher.Map", "MapDirector", w);
        Il2CppClass* net = cls(kImage, "MonomiPark.SlimeRancher.World.Teleportation", "TeleportNetwork", w);
        Il2CppClass* physics = cls("UnityEngine.PhysicsModule", "UnityEngine", "Physics", w);
        Il2CppClass* hit = cls("UnityEngine.PhysicsModule", "UnityEngine", "RaycastHit", w);
        Il2CppClass* transform = cls("UnityEngine.CoreModule", "UnityEngine", "Transform", w);
        g_tryGetSceneContext = method(scu, "TryGetSceneContext", 1, w);
        g_scRegions = method(sceneCtx, "get_RegionRegistry", 0, w);
        g_scMap = method(sceneCtx, "get_MapDirector", 0, w);
        g_scZoneTracker = method(sceneCtx, "get_PlayerZoneTracker", 0, w);
        g_scTeleport = method(sceneCtx, "get_TeleportNetwork", 0, w);
        g_scPlayer = method(sceneCtx, "get_Player", 0, w);
        g_currentGroup = method(regions, "get_CurrentSceneGroup", 0, w);
        g_currentZone = method(tracker, "GetCurrentZone", 0, w);
        g_markerLocation = method(map, "TryGetPlayerNavigationMarkerLocation", 3, w);
        // Two 3-argument overloads; the public one takes (SceneGroup, Vector3 position, Vector3 euler rotation).
        g_inSceneTeleport = byParams(net, "CompleteInSceneTeleport",
                                     {"MonomiPark.SlimeRancher.SceneManagement.SceneGroup", "UnityEngine.Vector3",
                                      "UnityEngine.Vector3"},
                                     w);
        g_raycast = byParams(physics, "Raycast",
                             {"UnityEngine.Vector3", "UnityEngine.Vector3", "UnityEngine.RaycastHit&", "System.Single",
                              "System.Int32", "UnityEngine.QueryTriggerInteraction"},
                             w);
        size_t o = 0;
        off(hit, "m_Distance", o, w);
        if (o >= 2 * sizeof(void*)) g_offHitDistance = o - 2 * sizeof(void*);
        else if (w.empty()) w = "RaycastHit.m_Distance has no struct offset";
        g_trPosition = method(transform, "get_position", 0, w);
        g_trEuler = method(transform, "get_eulerAngles", 0, w);
        if (!w.empty()) SR2_WARN("[{}] teleport to map marker unavailable: {}", kModule, w);
    }
    {
        std::string& w = g_expansionWhy;
        g_activatorClass = cls(kImage, "MonomiPark.SlimeRancher.UI.AccessDoor", "AccessDoorUIActivator", w);
        g_doorClass = cls(kImage, "MonomiPark.World", "AccessDoor", w);
        Il2CppClass* ev = cls(kImage, "MonomiPark.SlimeRancher.Event", "StaticGameEvent", w);
        off(g_activatorClass, "_accessDoor", g_offActDoor, w);
        off(g_activatorClass, "_purchaseMenuItem", g_offActItem, w);
        off(g_activatorClass, "_legacyUIPrefab", g_offActLegacy, w);
        off(g_doorClass, "LinkedDoors", g_offDoorLinked, w);
        off(g_doorClass, "OnUnlockedEvent", g_offDoorEvent, w);
        off(g_doorClass, "_model", g_offDoorModel, w);
        g_doorGetState = method(g_doorClass, "get_CurrState", 0, w);
        g_doorSetState = method(g_doorClass, "set_CurrState", 1, w);
        g_raiseEvent = method(ev, "RaiseEvent", 0, w);
        if (!w.empty()) SR2_WARN("[{}] unlock ranch expansions unavailable: {}", kModule, w);
    }
    {
        std::string nerr;
        if (!g_notices.Setup(R, kModule, nerr) || !g_notices.SetupText(R, nerr))
            SR2_INFO("[{}] teleport to marker: no HUD pop-ups ({})", kModule, nerr);
    }
    SR2_INFO("[{}] player extras: infinite health {}, teleport to marker {}, unlock expansions {}", kModule,
             g_healthWhy.empty() ? (g_hookReady ? "ok (hook + top-up)" : "ok (top-up only)") : "off",
             g_teleportWhy.empty() ? "ok" : "off", g_expansionWhy.empty() ? "ok" : "off");
}

void PlayerExtrasEnabled(bool on) {
    g_live.store(on);
    if (!on && g_infinite.exchange(false)) SR2_INFO("[{}] infinite health off (module disabled)", kModule);
    extras::SetHandler(on ? &HandleThunk : nullptr);
    if (!on) {
        extras::State st;
        st.tick = GetTickCount64();
        st.teleportWhy = st.expansionWhy = "Sandbox is off.";
        extras::Publish(std::move(st));
    }
}

void PlayerExtrasTick(bool snapshot) {
    if (!Live()) return;
    const unsigned long long now = GetTickCount64();
    if (g_infinite.load() && now - g_lastHealth >= kHealthMs) {
        g_lastHealth = now;
        TopUpHealth();
    }
    if (snapshot) Publish();
    if (g_notices.Ready()) {  // queued pop-ups wait for the HUD (menus / the overlay may suppress it)
        float dt = g_lastNoticeTick ? static_cast<float>(now - g_lastNoticeTick) / 1000.0f : 0.0f;
        g_lastNoticeTick = now;
        g_notices.Frame(dt);
    }
}

}  // namespace sr2::modules::sandbox
