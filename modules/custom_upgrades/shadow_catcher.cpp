// custom_upgrades: Shadow Catcher effect (see shadow_catcher.h). SR2 1.3.0, all lookups by name; RVAs
// for reference only (IDA on GameAssembly.dll, details in re/shadow-slimes.md).
//
//   * AmmoSlotDefinition.IsAllowed(IdentifiableType) (0x866B40): false if in _slotBlockList, true if
//     _slotTypeGroup is null, else the group's runtime membership. Callers: AmmoSlotManager.TryFindSlot
//     (MaybeAddToAnySlot <- Vacuumable.TryConsume, the vac's consume coroutine), CouldAddToSlot,
//     MaybeAddResource, ValidateAndAdjustSlots (load).
//   * ShadowSlimeScatter (MonomiPark.SlimeRancher.Slime.Shadow, a Subbehaviour):
//       Relevancy (0x8A2120) = 1 when the player is within PlayerReactRadius or _latestAlertObject is
//       set, else 0; Selected (0x8A23F0) -> TriggerAlert(player);
//       ScatterIfPlayerNear (0x8A2280, called by SubbehaviourPlexer.FixedUpdateInstance) forces the
//       behaviour when the player is near and nothing alerted it yet;
//       TriggerAlert (0x8A2410) = DoAlert(self) + DoAlert on every shadow slime within
//       AlertBroadcastRadius; DoAlert (0x8A1890, also from OnDisruptionLevelChanged) sets
//       _latestAlertObject (once), _destroyTime = now + Random(TimeBeforeDisappearingMin, Max),
//       forces the behaviour, jumps, _sfAnimator._isFleeing = true;
//       Action (0x8A12D0): alerted and fixedTimeAsDouble >= _destroyTime -> DisappearFX +
//       Destroyer.DestroyActor("SlimeFlee.Action"); else jump away / spin.
//   * ProducePlortsOnHit (MonomiPark.SlimeRancher.Slime, also on Gold slimes): ProducePlort (0x885CA0)
//       returns at once when _numPlortsSpawned >= DespawnAfterPlortCount (10), else spawns and adds;
//       LateUpdateAll (0x8856B0) despawns every one whose count reached it.
//   * SlimeFeral.UpdateInstance (0xBBD5D0): when _expireAt is reached, a slime on the ranch
//       (CellDirector.IsOnRanch) or in the wilds gets another hour; only elsewhere it poofs.
//   * CellDirector.IsOnRanch(RegionMember) (0x514EA0, static): any of member.Regions whose CellDirector
//       is a ranch cell. member.Regions must not be null (NRE).
//   * SlimeEat.WillNowEat (0xBB8750) = DoesEat(id) (id in _allEats) && (feral || _keepEatingWhenFull ||
//       IsHungryFor(id)); _allEats is built from SlimeDefinition.Diet.EatMap in CalculateAllEats (InitFood,
//       SetModel). GotoConsumable.Relevancy uses KeepEatingWhenFullDrive (0.9-1) for a slime that keeps
//       eating when full, so it also goes for food. EatMapEntry ctor: FavoriteProductionCount 2,
//       ProductionCount 1; NumToProduce = IsFavorite ? FavoriteProductionCount : ProductionCount.
//       SlimeDefinitions.RefreshDefinitions rebuilds every EatMap (new lists): the tick re-adds ours.

#include "modules/custom_upgrades/shadow_catcher.h"

#include <windows.h>

#include <array>
#include <atomic>
#include <format>
#include <initializer_list>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/detour_guard.h"
#include "core/il2cpp_api.h"
#include "core/log.h"
#include "core/main_thread.h"
#include "core/module.h"
#include "core/resolver.h"
#include "core/unity.h"

namespace sr2::modules::custom_upgrades::shadow {
namespace {

namespace unity = sr2::core::unity;
namespace main_thread = sr2::core::main_thread;
using sr2::il2cpp::api;
using sr2::il2cpp::Resolver;

constexpr const char* kModule = "custom_upgrades";
constexpr const char* kImage = "Assembly-CSharp";
constexpr size_t kMaxSlots = 16;
constexpr size_t kMaxTracked = 256;
constexpr int kFindRetryTicks = 10;  // FindObjectsOfTypeAll(SlimeDefinition) at most every ~10 s until found
constexpr double kNeverDestroy = 1e300;

using IsAllowedFn = bool (*)(Il2CppObject*, Il2CppObject*, const MethodInfo*);
using RelevancyFn = float (*)(Il2CppObject*, bool, const MethodInfo*);
using VoidFn = void (*)(Il2CppObject*, const MethodInfo*);
using AlertFn = void (*)(Il2CppObject*, Il2CppObject*, const MethodInfo*);
using WBarrierFn = void (*)(Il2CppObject*, void**, void*);
using ObjectNewFn = Il2CppObject* (*)(Il2CppClass*);

struct Tracked {
    Il2CppGCHandle handle = nullptr;  // the ShadowSlimeScatter (strong: its wrapper stays valid after a destroy)
    bool kept = false;                // on the ranch while the upgrade is owned
    bool eating = false;              // its SlimeEat keeps eating when full
};

struct State {
    WBarrierFn wbarrier = nullptr;
    ObjectNewFn objectNew = nullptr;

    Il2CppClass* slimeDefClass = nullptr;
    Il2CppClass* slimeEatClass = nullptr;

    // ShadowSlimeScatter
    size_t offAlert = 0, offDestroyTime = 0, offMember = 0, offFace = 0;
    size_t offFaceFleeing = 0;  // SlimeFaceAnimator._isFleeing (optional)
    // ProducePlortsOnHit (optional hook)
    size_t offPlortType = 0, offPlortCount = 0, offPlortMember = 0;
    // RegionMember.Regions
    size_t offRegions = 0;
    // SlimeDefinition / SlimeDiet / IdentifiableType
    size_t offDiet = 0, offEatMap = 0, offProduce = 0, offIsPlort = 0;
    // SlimeDiet.EatMapEntry (read from a live entry's class)
    size_t offEmEats = 0, offEmFav = 0, offEmFavCount = 0, offEmCount = 0, offEmProduces = 0, offEmBecomes = 0,
           offEmDriver = 0, offEmExtra = 0, offEmMin = 0;
    bool emResolved = false;
    // SlimeEat
    size_t offKeepEating = 0;

    const MethodInfo* isOnRanch = nullptr;  // CellDirector.IsOnRanch(RegionMember), static
    const MethodInfo* initFood = nullptr;   // SlimeEat.InitFood()

    IsAllowedFn origIsAllowed = nullptr;
    RelevancyFn origRelevancy = nullptr;
    VoidFn origScatterNear = nullptr;
    AlertFn origDoAlert = nullptr;
    VoidFn origAction = nullptr;
    VoidFn origProducePlort = nullptr;

    // runtime (main thread)
    Il2CppObject* shadowDef = nullptr;  // GC handles held for the session
    Il2CppObject* pinkDef = nullptr;
    int findTimer = kFindRetryTicks;  // first tick looks at once
    bool loggedFind = false, loggedNoDiet = false;
    std::unordered_map<Il2CppObject*, Tracked> tracked;
    size_t loggedTracked = ~size_t{0}, loggedKept = ~size_t{0};
    // diet
    std::vector<Il2CppObject*> entries;  // our EatMapEntries (GC handles held)
    std::vector<Il2CppGCHandle> entryHandles;
    Il2CppObject* dietList = nullptr;  // the EatMap list our entries are in (identity only)
    bool dietFailed = false;
    bool wasOwned = false;
};

State g;

// Read by the detours (any thread).
std::atomic<bool> g_owned{false};
std::atomic<Il2CppObject*> g_shadowType{nullptr};
std::atomic<Il2CppObject*> g_shadowPlort{nullptr};
std::array<std::atomic<Il2CppObject*>, kMaxSlots> g_slots{};

sr2::core::DetourGuard g_allowGuard{kModule, "AmmoSlotDefinition.IsAllowed"};
sr2::core::DetourGuard g_scatterGuard{kModule, "ShadowSlimeScatter"};
sr2::core::DetourGuard g_plortGuard{kModule, "ProducePlortsOnHit.ProducePlort"};
sr2::core::DetourGuard g_tickGuard{kModule, "custom_upgrades.ShadowCatcher"};

bool IsA(Il2CppObject* obj, Il2CppClass* k) {
    const auto& a = api();
    if (!obj || !k || !a.il2cpp_object_get_class) return false;
    Il2CppClass* c = a.il2cpp_object_get_class(obj);
    if (c == k) return true;
    return c && a.il2cpp_class_is_assignable_from && a.il2cpp_class_is_assignable_from(k, c);
}

template <class T>
T Unbox(Il2CppObject* boxed, T def = T{}) {
    return boxed ? *reinterpret_cast<T*>(reinterpret_cast<char*>(boxed) + 2 * sizeof(void*)) : def;
}

Il2CppObject* Invoke(const MethodInfo* m, void* obj, std::initializer_list<void*> args, const char* what,
                     bool* ok = nullptr) {
    if (ok) *ok = false;
    if (!m) return nullptr;
    std::vector<void*> params(args);
    std::string err;
    Il2CppObject* r = Resolver::Get().Invoke(m, obj, params.empty() ? nullptr : params.data(), &err);
    if (!err.empty()) {
        if (err.rfind("native exception", 0) == 0) sr2::core::MarkModuleFaulted(kModule, std::format("{}: {}", what, err));
        else SR2_WARN("[{}] shadow catcher: {} failed: {}", kModule, what, err);
        return nullptr;
    }
    if (ok) *ok = true;
    return r;
}

const MethodInfo* MethodOf(Il2CppObject* obj, const char* name, int argc) {
    const auto& a = api();
    if (!obj || !a.il2cpp_object_get_class) return nullptr;
    for (Il2CppClass* c = a.il2cpp_object_get_class(obj); c; c = a.il2cpp_class_get_parent ? a.il2cpp_class_get_parent(c) : nullptr)
        if (const MethodInfo* m = a.il2cpp_class_get_method_from_name(c, name, argc)) return m;
    return nullptr;
}

void SetRef(Il2CppObject* obj, size_t offset, Il2CppObject* value) {
    void** slot = reinterpret_cast<void**>(reinterpret_cast<char*>(obj) + offset);
    if (g.wbarrier && value) g.wbarrier(obj, slot, value);
    else *slot = value;
}

bool IsPlayerSlot(Il2CppObject* def) {
    if (!def) return false;
    for (const auto& s : g_slots) {
        Il2CppObject* d = s.load(std::memory_order_relaxed);
        if (!d) return false;  // filled from the front
        if (d == def) return true;
    }
    return false;
}

// ---- ranch test ----
bool OnRanch(Il2CppObject* member) {
    if (!member || !g.isOnRanch || !unity::IsAlive(member)) return false;
    if (!unity::Field<Il2CppObject*>(member, g.offRegions)) return false;  // IsOnRanch would throw
    bool ok = false;
    Il2CppObject* r = Invoke(g.isOnRanch, nullptr, {member}, "CellDirector.IsOnRanch", &ok);
    return ok && Unbox<bool>(r);
}

bool Alerted(Il2CppObject* scatter) {
    Il2CppObject* a = unity::Field<Il2CppObject*>(scatter, g.offAlert);
    return a && unity::IsAlive(a);
}

// A kept slime forgets an alert from before (seen in the labyrinth, then vacuumed and shot out on the ranch
// within its 10-15 s, or alerted before the tick found it on the ranch).
void ClearAlert(Il2CppObject* scatter) {
    if (unity::Field<Il2CppObject*>(scatter, g.offAlert)) SetRef(scatter, g.offAlert, nullptr);
    unity::Field<double>(scatter, g.offDestroyTime) = kNeverDestroy;
    if (g.offFaceFleeing)
        if (Il2CppObject* face = unity::Field<Il2CppObject*>(scatter, g.offFace); face && unity::IsAlive(face))
            unity::Field<bool>(face, g.offFaceFleeing) = false;
}

// Detours: track a shadow slime the first time one of its hooks runs (a GC handle is made here; handles
// are only freed by the tick).
Tracked* Track(Il2CppObject* scatter) {
    auto it = g.tracked.find(scatter);
    if (it != g.tracked.end()) return &it->second;
    if (g.tracked.size() >= kMaxTracked) return nullptr;
    Tracked t;
    t.handle = unity::NewGCHandle(scatter);
    if (!t.handle) return nullptr;
    return &g.tracked.emplace(scatter, t).first->second;
}
Tracked* Find(Il2CppObject* scatter) {
    auto it = g.tracked.find(scatter);
    return it == g.tracked.end() ? nullptr : &it->second;
}

// ---------------------------------------------------------------- detours --

// The player's regular slots take Shadow slimes while the upgrade is owned. Pointer compares first: this
// runs for every slot on every vac tick.
bool IsAllowedDetour(Il2CppObject* self, Il2CppObject* type, const MethodInfo* mi) {
    if (type && type == g_shadowType.load(std::memory_order_relaxed) && g_owned.load(std::memory_order_relaxed)) {
        bool allow = false;
        g_allowGuard.Run([&] { allow = IsPlayerSlot(self); });
        if (allow) return true;
    }
    return g.origIsAllowed(self, type, mi);
}

// Not alerted -> the scatter behaviour is not relevant (vanilla: 1 while the player is within 10 m).
// Kept on the ranch -> never relevant.
float RelevancyDetour(Il2CppObject* self, bool grounded, const MethodInfo* mi) {
    if (self && g_owned.load(std::memory_order_relaxed) && main_thread::IsMainThread()) {
        bool calm = false;
        g_scatterGuard.Run([&] {
            Tracked* t = Track(self);
            calm = (t && t->kept) || !Alerted(self);
        });
        if (calm) return 0.0f;
    }
    return g.origRelevancy(self, grounded, mi);
}

// The player walking up no longer forces the scatter (and with it the alert).
void ScatterNearDetour(Il2CppObject* self, const MethodInfo* mi) {
    if (self && g_owned.load(std::memory_order_relaxed) && main_thread::IsMainThread()) {
        if (g_scatterGuard.Run([&] { Track(self); })) return;
    }
    g.origScatterNear(self, mi);
}

// A kept slime is never alerted (hits, neighbours' broadcasts, Grey Labyrinth disruptions).
void DoAlertDetour(Il2CppObject* self, Il2CppObject* alertObj, const MethodInfo* mi) {
    if (self && g_owned.load(std::memory_order_relaxed) && main_thread::IsMainThread()) {
        bool skip = false;
        g_scatterGuard.Run([&] {
            Tracked* t = Find(self);
            skip = t && t->kept;
        });
        if (skip) return;
    }
    g.origDoAlert(self, alertObj, mi);
}

// A kept slime's scatter does nothing: an old alert is cleared before Action could destroy it.
void ActionDetour(Il2CppObject* self, const MethodInfo* mi) {
    if (self && g_owned.load(std::memory_order_relaxed) && main_thread::IsMainThread()) {
        bool skip = false;
        g_scatterGuard.Run([&] {
            Tracked* t = Find(self);
            if (!t || !t->kept) return;
            ClearAlert(self);
            skip = true;
        });
        if (skip) return;
    }
    g.origAction(self, mi);
}

// A shadow slime on the ranch never reaches the "despawn after N hit plorts" limit.
void ProducePlortDetour(Il2CppObject* self, const MethodInfo* mi) {
    if (self && g_owned.load(std::memory_order_relaxed) && main_thread::IsMainThread()) {
        g_plortGuard.Run([&] {
            Il2CppObject* plort = g_shadowPlort.load(std::memory_order_relaxed);
            if (!plort || unity::Field<Il2CppObject*>(self, g.offPlortType) != plort) return;  // a Gold slime
            int32_t& count = unity::Field<int32_t>(self, g.offPlortCount);
            if (count > 0 && OnRanch(unity::Field<Il2CppObject*>(self, g.offPlortMember))) count = 0;
        });
    }
    g.origProducePlort(self, mi);
}

// ---------------------------------------------------------------- tick --

void FindTypes() {
    if (g.shadowDef || ++g.findTimer < kFindRetryTicks) return;
    g.findTimer = 0;
    Il2CppObject* shadow = nullptr;
    Il2CppObject* pink = nullptr;
    for (Il2CppObject* o : unity::FindObjectsOfTypeAll(g.slimeDefClass)) {
        if (!IsA(o, g.slimeDefClass) || !unity::IsAlive(o)) continue;
        const std::string n = unity::GetName(o);
        if (n == "Shadow") shadow = o;
        else if (n == "Pink") pink = o;
    }
    if (!shadow) {
        if (!g.loggedFind) SR2_INFO("[{}] shadow catcher: the 'Shadow' SlimeDefinition is not loaded yet", kModule);
        g.loggedFind = true;
        return;
    }
    Il2CppObject* diet = unity::Field<Il2CppObject*>(shadow, g.offDiet);
    void* produce = diet ? unity::Field<void*>(diet, g.offProduce) : nullptr;
    auto* plort = static_cast<Il2CppObject*>(unity::ArrayAt(produce, 0));
    unity::NewGCHandle(shadow);  // session-long
    if (pink) unity::NewGCHandle(pink);
    if (plort) unity::NewGCHandle(plort);
    g.shadowDef = shadow;
    g.pinkDef = pink;
    g_shadowPlort.store(unity::IsAlive(plort) ? plort : nullptr);
    g_shadowType.store(shadow);
    SR2_INFO("[{}] shadow catcher: found the 'Shadow' slime (plort '{}'), diet template '{}'", kModule,
             plort ? unity::GetName(plort) : std::string("none"), pink ? unity::GetName(pink) : std::string("not found"));
}

bool ResolveEntry(Il2CppObject* e) {
    if (g.emResolved) return true;
    Resolver& R = Resolver::Get();
    Il2CppClass* k = unity::ClassOf(e);
    auto f = [&](const char* name, size_t& out) {
        auto o = R.FieldOffset(k, name);
        if (o) out = *o;
        return o.has_value();
    };
    g.emResolved = f("EatsIdent", g.offEmEats) && f("IsFavorite", g.offEmFav) &&
                   f("FavoriteProductionCount", g.offEmFavCount) && f("ProductionCount", g.offEmCount) &&
                   f("ProducesIdent", g.offEmProduces) && f("BecomesIdent", g.offEmBecomes) && f("Driver", g.offEmDriver) &&
                   f("ExtraDrive", g.offEmExtra) && f("MinDrive", g.offEmMin);
    return g.emResolved;
}

Il2CppObject* EatMapOf(Il2CppObject* def) {
    Il2CppObject* diet = def ? unity::Field<Il2CppObject*>(def, g.offDiet) : nullptr;
    return diet ? unity::Field<Il2CppObject*>(diet, g.offEatMap) : nullptr;
}

// Our entries: one per food of the Pink slime's eat map (no plorts, no transformations), producing the
// Shadow plort instead of the Pink plort, never a favourite. Built once per session.
bool BuildEntries() {
    if (!g.entries.empty()) return true;
    Il2CppObject* plort = g_shadowPlort.load();
    Il2CppObject* src = EatMapOf(g.pinkDef);
    if (!plort || !src || !g.objectNew) return false;
    std::string foods;
    for (size_t i = 0, n = unity::ListCount(src); i < n; ++i) {
        auto* e = static_cast<Il2CppObject*>(unity::ListAt(src, i));
        if (!e || !ResolveEntry(e)) continue;
        Il2CppObject* eats = unity::Field<Il2CppObject*>(e, g.offEmEats);
        if (!eats || !unity::IsAlive(eats) || unity::Field<bool>(eats, g.offIsPlort)) continue;
        if (unity::Field<Il2CppObject*>(e, g.offEmBecomes) || !unity::Field<Il2CppObject*>(e, g.offEmProduces)) continue;
        Il2CppObject* c = g.objectNew(unity::ClassOf(e));
        if (!c) continue;
        Invoke(MethodOf(c, ".ctor", 0), c, {}, "EatMapEntry..ctor");
        SetRef(c, g.offEmEats, eats);
        unity::Field<bool>(c, g.offEmFav) = false;
        unity::Field<int32_t>(c, g.offEmFavCount) = unity::Field<int32_t>(e, g.offEmFavCount);
        unity::Field<int32_t>(c, g.offEmCount) = unity::Field<int32_t>(e, g.offEmCount);
        SetRef(c, g.offEmProduces, plort);
        SetRef(c, g.offEmBecomes, nullptr);
        unity::Field<int32_t>(c, g.offEmDriver) = unity::Field<int32_t>(e, g.offEmDriver);
        unity::Field<float>(c, g.offEmExtra) = unity::Field<float>(e, g.offEmExtra);
        unity::Field<float>(c, g.offEmMin) = unity::Field<float>(e, g.offEmMin);
        Il2CppGCHandle h = unity::NewGCHandle(c);
        if (!h) continue;
        g.entries.push_back(c);
        g.entryHandles.push_back(h);
        if (g.entries.size() <= 40) foods += (foods.empty() ? "" : ", ") + unity::GetName(eats);
    }
    if (g.entries.empty()) return false;
    SR2_INFO("[{}] shadow catcher: {} food entr{} for the Shadow diet (copied from '{}', producing '{}'): {}{}", kModule,
             g.entries.size(), g.entries.size() == 1 ? "y" : "ies", unity::GetName(g.pinkDef), unity::GetName(plort), foods,
             g.entries.size() > 40 ? ", ..." : "");
    return true;
}

int32_t ListIndexOf(Il2CppObject* list, Il2CppObject* item) {
    bool ok = false;
    Il2CppObject* r = Invoke(MethodOf(list, "IndexOf", 1), list, {item}, "List.IndexOf", &ok);
    return ok ? Unbox<int32_t>(r, -1) : -1;
}

// Our entries in the Shadow eat map (again after SlimeDefinitions.RefreshDefinitions made a new list).
// true = they were (re-)added just now.
bool EnsureDiet() {
    if (g.dietFailed || !g.shadowDef) return false;
    Il2CppObject* list = EatMapOf(g.shadowDef);
    if (!list) {
        if (!g.loggedNoDiet) SR2_WARN("[{}] shadow catcher: the Shadow diet has no eat map - kept shadow slimes don't eat", kModule);
        g.loggedNoDiet = true;
        return false;
    }
    if (!BuildEntries()) {
        g.dietFailed = true;
        SR2_WARN("[{}] shadow catcher: no food entries to copy (Pink slime {}) - kept shadow slimes don't eat", kModule,
                 g.pinkDef ? "has an empty eat map" : "not found");
        return false;
    }
    if (list == g.dietList && ListIndexOf(list, g.entries[0]) >= 0) return false;
    const MethodInfo* add = MethodOf(list, "Add", 1);
    int added = 0;
    for (Il2CppObject* e : g.entries) {
        if (ListIndexOf(list, e) >= 0) continue;
        bool ok = false;
        Invoke(add, list, {e}, "List.Add", &ok);
        if (ok) ++added;
    }
    g.dietList = list;
    SR2_INFO("[{}] shadow catcher: {} food entr{} added to the Shadow diet ({} in its eat map)", kModule, added,
             added == 1 ? "y" : "ies", unity::ListCount(list));
    return added > 0;
}

void RemoveDiet() {
    Il2CppObject* list = g.shadowDef ? EatMapOf(g.shadowDef) : nullptr;
    if (!list || g.entries.empty()) return;
    const MethodInfo* remove = MethodOf(list, "Remove", 1);
    int removed = 0;
    for (Il2CppObject* e : g.entries) {
        bool ok = false;
        Il2CppObject* r = Invoke(remove, list, {e}, "List.Remove", &ok);
        if (ok && Unbox<bool>(r)) ++removed;
    }
    g.dietList = nullptr;
    if (removed) SR2_INFO("[{}] shadow catcher: Shadow diet back to vanilla ({} food entries removed)", kModule, removed);
}

// A kept slime eats whenever food is offered (Shadow never gets hungry) and goes for food it sees.
void SetEating(Il2CppObject* scatter, Tracked& t, bool on) {
    if (t.eating == on || !g.slimeEatClass || !g.offKeepEating) return;
    Il2CppObject* eat = unity::GetComponent(scatter, g.slimeEatClass);
    if (!eat || !unity::IsAlive(eat)) return;
    unity::Field<bool>(eat, g.offKeepEating) = on;
    Invoke(g.initFood, eat, {}, "SlimeEat.InitFood");  // eats from the current diet
    t.eating = on;
}

void Forget(bool revert) {
    for (auto& [scatter, t] : g.tracked) {
        Il2CppObject* live = t.handle ? unity::GCHandleTarget(t.handle) : nullptr;
        if (revert && live && unity::IsAlive(live)) SetEating(live, t, false);
        if (t.handle) unity::FreeGCHandle(t.handle);
    }
    g.tracked.clear();
    g.loggedTracked = g.loggedKept = ~size_t{0};
}

}  // namespace

void SetOwned(bool owned) { g_owned.store(owned); }
bool Owned() { return g_owned.load(); }

void SetPlayerSlots(const std::vector<Il2CppObject*>& defs) {
    size_t i = 0;
    for (; i < defs.size() && i < kMaxSlots; ++i) g_slots[i].store(defs[i]);
    for (; i < kMaxSlots; ++i) g_slots[i].store(nullptr);
}

Il2CppObject* ShadowType() { return g_shadowType.load(); }
Il2CppObject* ShadowPlort() { return g_shadowPlort.load(); }

void ShadowCatcher::Tick() {
    if (!ready_ || !main_thread::IsMainThread()) return;
    g_tickGuard.Run([&] {
        FindTypes();
        const bool owned = g_owned.load();
        if (!owned) {
            if (g.wasOwned) {
                Forget(true);
                RemoveDiet();
                SR2_INFO("[{}] shadow catcher: not owned - shadow slimes behave as in vanilla", kModule);
            }
            g.wasOwned = false;
            return;
        }
        if (!g.wasOwned) SR2_INFO("[{}] shadow catcher: owned - the vacpack holds shadow slimes, they stay on the ranch", kModule);
        g.wasOwned = true;
        const bool dietNew = EnsureDiet();
        size_t kept = 0;
        for (auto it = g.tracked.begin(); it != g.tracked.end();) {
            Tracked& t = it->second;
            Il2CppObject* scatter = t.handle ? unity::GCHandleTarget(t.handle) : nullptr;
            if (!scatter || !unity::IsAlive(scatter)) {  // destroyed, vacuumed or unloaded with its scene
                if (t.handle) unity::FreeGCHandle(t.handle);
                it = g.tracked.erase(it);
                continue;
            }
            const bool now = OnRanch(unity::Field<Il2CppObject*>(scatter, g.offMember));
            if (now != t.kept) {
                t.kept = now;
                const std::string where = unity::GetName(unity::GameObjectOf(scatter));
                SR2_INFO("[{}] shadow catcher: shadow slime '{}' {}", kModule, where,
                         now ? "is on the ranch: kept (no scatter, no despawn), eats" : "left the ranch: scatters as in vanilla");
            }
            if (t.kept) {
                if (Alerted(scatter)) ClearAlert(scatter);
                if (dietNew) t.eating = false;  // recompute its eats with the new entries
                SetEating(scatter, t, !g.entries.empty());
                ++kept;
            } else {
                SetEating(scatter, t, false);
            }
            ++it;
        }
        if (g.tracked.size() != g.loggedTracked || kept != g.loggedKept) {
            g.loggedTracked = g.tracked.size();
            g.loggedKept = kept;
            SR2_INFO("[{}] shadow catcher: {} shadow slime(s) loaded nearby, {} kept on the ranch", kModule,
                     g.tracked.size(), kept);
        }
    });
}

void ShadowCatcher::Cleanup(const char* why) {
    if (!ready_ || !main_thread::IsMainThread()) return;
    g_owned.store(false);
    g_tickGuard.Run([&] {
        const bool any = !g.tracked.empty() || g.dietList;
        Forget(true);
        RemoveDiet();
        g.wasOwned = false;
        if (any) SR2_INFO("[{}] {}: shadow catcher off (shadow slimes behave as in vanilla)", kModule, why);
    });
}

bool ShadowCatcher::Setup(sr2::core::ModuleContext& ctx, std::string& why, std::string& report) {
    Resolver& R = ctx.resolver();
    auto cls = [&](const char* ns, const char* name) -> Il2CppClass* {
        std::string err;
        Il2CppClass* k = R.Class(kImage, ns, name, &err);
        if (!k && why.empty()) why = err.empty() ? std::format("class {}.{} not found", ns, name) : err;
        return k;
    };
    auto off = [&](Il2CppClass* k, const char* field, size_t& out) {
        std::string err;
        auto o = k ? R.FieldOffset(k, field, &err) : std::nullopt;
        if (o) out = *o;
        else if (why.empty()) why = std::format("field {} not found", field);
    };
    auto method = [&](Il2CppClass* k, const char* name, int argc) -> const MethodInfo* {
        std::string err;
        const MethodInfo* m = k ? R.Method(k, name, argc, &err) : nullptr;
        if (!m && why.empty()) why = std::format("method {}({}) not found", name, argc);
        return m;
    };

    HMODULE ga = api().module;
    g.wbarrier = reinterpret_cast<WBarrierFn>(GetProcAddress(ga, "il2cpp_gc_wbarrier_set_field"));
    g.objectNew = reinterpret_cast<ObjectNewFn>(GetProcAddress(ga, "il2cpp_object_new"));

    Il2CppClass* slotDef = cls("", "AmmoSlotDefinition");
    Il2CppClass* scatter = cls("MonomiPark.SlimeRancher.Slime.Shadow", "ShadowSlimeScatter");
    Il2CppClass* member = cls("MonomiPark.SlimeRancher.Regions", "RegionMember");
    Il2CppClass* cell = cls("", "CellDirector");
    Il2CppClass* diet = cls("", "SlimeDiet");
    Il2CppClass* ident = cls("", "IdentifiableType");
    g.slimeDefClass = cls("", "SlimeDefinition");
    if (!why.empty()) return false;

    off(scatter, "_latestAlertObject", g.offAlert);
    off(scatter, "_destroyTime", g.offDestroyTime);
    off(scatter, "_member", g.offMember);
    off(scatter, "_sfAnimator", g.offFace);
    off(member, "Regions", g.offRegions);
    off(g.slimeDefClass, "Diet", g.offDiet);
    off(diet, "EatMap", g.offEatMap);
    off(diet, "ProduceIdents", g.offProduce);
    off(ident, "IsPlort", g.offIsPlort);
    g.isOnRanch = method(cell, "IsOnRanch", 1);
    const MethodInfo* allowMi = method(slotDef, "IsAllowed", 1);
    const MethodInfo* relMi = method(scatter, "Relevancy", 1);
    const MethodInfo* nearMi = method(scatter, "ScatterIfPlayerNear", 0);
    const MethodInfo* alertMi = method(scatter, "DoAlert", 1);
    const MethodInfo* actionMi = method(scatter, "Action", 0);
    if (!why.empty()) return false;
    {
        std::string err;
        if (Il2CppClass* face = R.Class(kImage, "", "SlimeFaceAnimator", &err))
            if (auto o = R.FieldOffset(face, "_isFleeing", &err)) g.offFaceFleeing = *o;
    }

    struct H {
        const char* name;
        const MethodInfo* mi;
        void* detour;
        void** orig;
    };
    const H hooks[] = {
        {"AmmoSlotDefinition.IsAllowed", allowMi, reinterpret_cast<void*>(&IsAllowedDetour),
         reinterpret_cast<void**>(&g.origIsAllowed)},
        {"ShadowSlimeScatter.Relevancy", relMi, reinterpret_cast<void*>(&RelevancyDetour),
         reinterpret_cast<void**>(&g.origRelevancy)},
        {"ShadowSlimeScatter.ScatterIfPlayerNear", nearMi, reinterpret_cast<void*>(&ScatterNearDetour),
         reinterpret_cast<void**>(&g.origScatterNear)},
        {"ShadowSlimeScatter.DoAlert", alertMi, reinterpret_cast<void*>(&DoAlertDetour),
         reinterpret_cast<void**>(&g.origDoAlert)},
        {"ShadowSlimeScatter.Action", actionMi, reinterpret_cast<void*>(&ActionDetour),
         reinterpret_cast<void**>(&g.origAction)},
    };
    // All or nothing: resolve every body first, so a missing one creates no hook at all.
    void* bodies[sizeof(hooks) / sizeof(hooks[0])] = {};
    for (size_t i = 0; i < sizeof(hooks) / sizeof(hooks[0]); ++i) {
        std::string err;
        bodies[i] = R.MethodPointer(hooks[i].mi, &err);
        if (!bodies[i]) {
            why = std::format("{}: no native body ({})", hooks[i].name, err);
            return false;
        }
    }
    for (size_t i = 0; i < sizeof(hooks) / sizeof(hooks[0]); ++i) {
        if (!ctx.HookAddress(hooks[i].name, bodies[i], hooks[i].detour, hooks[i].orig)) {
            why = std::format("hook {} failed", hooks[i].name);
            return false;  // hooks made so far pass through (g_owned stays false)
        }
        report += std::format(" {}@0x{:X}", hooks[i].name, Resolver::Rva(bodies[i]));
    }

    // Optional: the hit-plort despawn limit on the ranch.
    {
        std::string err;
        Il2CppClass* poh = R.Class(kImage, "MonomiPark.SlimeRancher.Slime", "ProducePlortsOnHit", &err);
        auto o1 = poh ? R.FieldOffset(poh, "PlortIdentifiableType", &err) : std::nullopt;
        auto o2 = poh ? R.FieldOffset(poh, "_numPlortsSpawned", &err) : std::nullopt;
        auto o3 = poh ? R.FieldOffset(poh, "_regionMember", &err) : std::nullopt;
        const MethodInfo* pm = poh ? R.Method(poh, "ProducePlort", 0, &err) : nullptr;
        void* pp = o1 && o2 && o3 && pm ? R.MethodPointer(pm, &err) : nullptr;
        if (pp) {
            g.offPlortType = *o1;
            g.offPlortCount = *o2;
            g.offPlortMember = *o3;
        }
        if (pp && ctx.HookAddress("ProducePlortsOnHit.ProducePlort", pp, reinterpret_cast<void*>(&ProducePlortDetour),
                                  reinterpret_cast<void**>(&g.origProducePlort)))
            report += std::format(" ProducePlortsOnHit.ProducePlort@0x{:X}", Resolver::Rva(pp));
        else
            SR2_WARN("[{}] shadow catcher: hit-plort limit not lifted ({}) - a kept shadow slime still despawns after 10 "
                     "plorts from hits", kModule, err);
    }
    // Optional: eating.
    {
        std::string err;
        g.slimeEatClass = R.Class(kImage, "", "SlimeEat", &err);
        auto o = g.slimeEatClass ? R.FieldOffset(g.slimeEatClass, "_keepEatingWhenFull", &err) : std::nullopt;
        g.initFood = g.slimeEatClass ? R.Method(g.slimeEatClass, "InitFood", 0, &err) : nullptr;
        if (o && g.initFood && g.objectNew) g.offKeepEating = *o;
        else SR2_WARN("[{}] shadow catcher: kept shadow slimes won't eat ({})", kModule, err.empty() ? "il2cpp_object_new" : err);
    }
    ready_ = true;
    return true;
}

}  // namespace sr2::modules::custom_upgrades::shadow
