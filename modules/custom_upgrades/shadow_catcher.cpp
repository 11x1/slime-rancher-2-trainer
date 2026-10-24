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
//   * The Ringtail rule (Ringtails carry the KeepEatingWhenFull component, which only sets SlimeEat.
//     _keepEatingWhenFull and shows the face's _isMischievous while !IsHungryForNonPlorts): SlimeEat.
//     FinishChomp (0xBB6780) passes skipProduction = _keepEatingWhenFull && !IsHungryFor(id) to
//     EatAndProduce, so a full slime still eats but makes no plort. IsHungryFor (0xBB7380) = !isSleeping &&
//     drive >= MinDriveToEat; drive = EmotionDriveCalculator.Drive (0xAB3D70) = max(hunger, entry.MinDrive) +
//     entry.ExtraDrive. InvokeOnEat (0xBB7190) lowers the hunger by DrivePerEat after every meal; it rises
//     again at SlimeEmotions._changeRate[0] = ComputeHungerChangePerHour (0x88AE10): _hungerRecoveryRate
//     (the SlimeEmotionConfig.HungerRate property; Shadow's defaultPropertyValues make it 0) scaled by the
//     recovery factor (+ day / night extra), times _hungerDirection (Awake: RecoversTowardOne ? 1 : -1).
//   * ProducePlortsOnHit.ProcessCollisionEnter (0x8859F0): an object of a ProducePlortGroups type hitting
//     faster than _speedThresholdToProducePlort -> ProducePlort, then IFleeBehavior.StartFleeing. That is
//     the only caller of ProducePlort, so skipping it stops hit plorts (thrown food included).
//   * The Phosphor rule: Phosphor slimes carry DestroyOutsideHoursOfDay (StartHour / EndHour = the hours
//     they live in, Min/MaxEndureHoursOutsideWindow, CavesPreventShutdown). Update (0xAB3AB0) kills and
//     destroys the slime once TimeDirector.HasReached(_shutdownAt); StartShutdownClock (0xAB3960) sets it
//     to the next EndHour + Random(endure) game h; OnCaveEnter (0xAB3860) sets it to infinity while any
//     CaveTrigger holds the slime (CaveTrigger.OnTriggerEnter 0x511D60 calls IListener.OnCaveEnter on the
//     entering collider's components). The corral's Solar Shield is such a CaveTrigger: "Base / High
//     SolarShield" children of the CORRAL prefab, inactive until bought, each with a "Cave Trigger" child
//     (BoxCollider, layer 22, 10 x 5 x 10 / 10 x 8 x 10). Kept shadow slimes follow the same rule with our
//     own clock (a component added at runtime would miss the trigger-enter of a slime already inside).

#include "modules/custom_upgrades/shadow_catcher.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
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
constexpr float kDrivePerMeal = 1.0f;  // one meal empties the hunger: the meals after it make no plort
constexpr int kFullLogBudget = 12;     // "full" / "hungry again" lines per minute (all slimes together)
constexpr int kCaveRescanTicks = 10;   // FindObjectsOfTypeAll(CaveTrigger) at most every ~10 s
constexpr int kTimeDirRetryTicks = 10;
constexpr int kPediaRetryTicks = 10;   // FindObjectsOfTypeAll(StringTable) at most every ~10 s until patched
// Sunlight rule (the Phosphor's DestroyOutsideHoursOfDay values when found, else these): shadow slimes are
// safe from LiveStart to LiveEnd (wrapping midnight) and fade after Random(endure) game hours in the sun.
constexpr float kLiveStart = 18.0f, kLiveEnd = 6.0f;
constexpr float kEndureMin = 0.25f, kEndureMax = 0.5f;  // also the floor: time to get them into the shade
constexpr const char* kPediaTable = "PediaPage_en";
constexpr const char* kPediaKey = "m.risks.shadow_slime";
constexpr const char* kPediaNote =
    "\n\nRanchers who keep shadow slimes should know they can't stand the sun. A shadow slime left out in "
    "daylight will soon fade away, so give its corral a Solar Shield or keep it somewhere perpetually dark, "
    "like a cave.";

using IsAllowedFn = bool (*)(Il2CppObject*, Il2CppObject*, const MethodInfo*);
using RelevancyFn = float (*)(Il2CppObject*, bool, const MethodInfo*);
using VoidFn = void (*)(Il2CppObject*, const MethodInfo*);
using AlertFn = void (*)(Il2CppObject*, Il2CppObject*, const MethodInfo*);
using WBarrierFn = void (*)(Il2CppObject*, void**, void*);
using ObjectNewFn = Il2CppObject* (*)(Il2CppClass*);

struct Tracked {
    Il2CppGCHandle handle = nullptr;  // the ShadowSlimeScatter (strong: its wrapper stays valid after a destroy)
    bool kept = false;                // on the ranch while the upgrade is owned
    bool eating = false;              // Ringtail rule on: keeps eating when full, hunger comes back
    bool full = false;                // last seen not hungry (its meals make no plort)
    bool fullKnown = false;
    bool exposed = false;  // kept, in daylight and not in the shade: its fade clock runs
    bool fading = false;   // the sun got it: alerted on purpose, vanilla scatter makes it vanish
    double fadeAt = 0.0;   // world time it fades at while exposed
    // Vanilla values of its SlimeEat / SlimeEmotions, put back when it stops being kept.
    float origRate = 0.0f, origDir = 0.0f, origDrivePerEat = 0.0f;
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
    // Hunger rhythm (optional): SlimeEat, SlimeEmotions, SlimeEmotionConfig, PropertyDefinition, face
    size_t offDrivePerEat = 0, offMinDriveToEat = 0, offEatEmotions = 0, offEatFace = 0;
    size_t offHungerRate = 0, offHungerDir = 0, offEmoConfig = 0, offEmoValues = 0;
    size_t offCfgHungerRate = 0, offPropDefault = 0, offMischief = 0;
    const MethodInfo* computeHunger = nullptr;  // SlimeEmotions.ComputeHungerChangePerHour()
    bool hungerOk = false;

    const MethodInfo* isOnRanch = nullptr;  // CellDirector.IsOnRanch(RegionMember), static
    const MethodInfo* initFood = nullptr;   // SlimeEat.InitFood()
    const MethodInfo* doAlertMi = nullptr;  // ShadowSlimeScatter.DoAlert (for the fade)

    // Sunlight rule (optional)
    bool sunOk = false;
    Il2CppClass* caveClass = nullptr;      // CaveTrigger
    Il2CppClass* colliderClass = nullptr;  // UnityEngine.Collider
    Il2CppClass* timeDirClass = nullptr;   // TimeDirector
    Il2CppClass* dohClass = nullptr;       // DestroyOutsideHoursOfDay (Phosphor values, optional)
    const MethodInfo* colBounds = nullptr;   // Collider.get_bounds
    const MethodInfo* colEnabled = nullptr;  // Collider.get_enabled
    const MethodInfo* trPosition = nullptr;  // Transform.get_position
    const MethodInfo* currHour = nullptr;    // TimeDirector.CurrHour
    const MethodInfo* worldTime = nullptr;   // TimeDirector.WorldTime
    size_t offPrefab = 0, offModules = 0;    // IdentifiableType.prefab, SlimeDefinition.SlimeModules
    size_t offDohStart = 0, offDohEnd = 0, offDohMin = 0, offDohMax = 0;

    // Slimepedia note (optional)
    bool pediaOk = false;
    Il2CppClass* stringTableClass = nullptr;
    const MethodInfo* getEntry = nullptr;  // StringTable.GetEntry(string)
    const MethodInfo* getValue = nullptr;  // StringTableEntry.get_Value
    const MethodInfo* setValue = nullptr;  // StringTableEntry.set_Value

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
    // hunger rhythm
    float hungerRate = -1.0f;  // vanilla default hunger rate per game hour (< 0 = not read yet)
    int logTokens = kFullLogBudget, logRefill = 0;
    bool loggedMuted = false;
    // sunlight
    bool sunConfig = false;  // live window / endure read (from the Phosphor or the defaults)
    float liveStart = kLiveStart, liveEnd = kLiveEnd, endureMin = kEndureMin, endureMax = kEndureMax;
    Il2CppGCHandle timeDirHandle = nullptr;
    int timeDirTimer = kTimeDirRetryTicks;
    std::vector<Il2CppGCHandle> caves;  // CaveTrigger colliders (scene objects and prefab assets alike)
    int caveTimer = kCaveRescanTicks;
    bool wasDay = false, dayKnown = false;
    // pedia
    Il2CppGCHandle pediaTable = nullptr;  // the English PediaPage StringTable we patched
    int pediaTimer = kPediaRetryTicks;
    bool loggedPedia = false;
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
// Kept and not fading: its scatter does nothing. A fading slime runs the vanilla scatter (and vanishes).
bool Calm(const Tracked* t) { return t && t->kept && !t->fading; }

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
            calm = t && t->fading ? false : Calm(t) || !Alerted(self);
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
            skip = Calm(Find(self));
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
            if (!Calm(Find(self))) return;
            ClearAlert(self);
            skip = true;
        });
        if (skip) return;
    }
    g.origAction(self, mi);
}

// On the ranch a hit makes no plort (thrown food, shots, anything): kept shadow slimes make plorts only by
// eating while hungry (the Ringtail rule), so a full one can't be farmed by pelting it. In the wilds the
// vanilla hit plorts stay. The skipped call never counts towards the "despawn after N hit plorts" limit.
void ProducePlortDetour(Il2CppObject* self, const MethodInfo* mi) {
    if (self && g_owned.load(std::memory_order_relaxed) && main_thread::IsMainThread()) {
        bool skip = false;
        g_plortGuard.Run([&] {
            Il2CppObject* plort = g_shadowPlort.load(std::memory_order_relaxed);
            if (!plort || unity::Field<Il2CppObject*>(self, g.offPlortType) != plort) return;  // a Gold slime
            if (!OnRanch(unity::Field<Il2CppObject*>(self, g.offPlortMember))) return;
            unity::Field<int32_t>(self, g.offPlortCount) = 0;
            skip = true;
            static bool logged = false;
            if (!logged)
                SR2_INFO("[{}] shadow catcher: a hit on a shadow slime on the ranch made no plort (on the ranch only "
                         "a meal while hungry makes one; logged once)", kModule);
            logged = true;
        });
        if (skip) return;
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
        // Drive = the hunger itself (no floor, no bonus): "hungry" = hunger >= MinDriveToEat exactly.
        unity::Field<float>(c, g.offEmExtra) = 0.0f;
        unity::Field<float>(c, g.offEmMin) = 0.0f;
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

bool LogToken() {
    if (g.logTokens > 0) {
        --g.logTokens;
        return true;
    }
    if (!g.loggedMuted) SR2_INFO("[{}] shadow catcher: more full / hungry changes this minute (not logged)", kModule);
    g.loggedMuted = true;
    return false;
}

Il2CppObject* EmotionsOf(Il2CppObject* eat) {
    Il2CppObject* emo = g.hungerOk ? unity::Field<Il2CppObject*>(eat, g.offEatEmotions) : nullptr;
    return emo && unity::IsAlive(emo) ? emo : nullptr;
}

// The hunger rate every slime without an override gets (the SlimeEmotionConfig.HungerRate property's
// default; Shadow's override is 0).
float DefaultHungerRate(Il2CppObject* emo) {
    if (g.hungerRate >= 0.0f) return g.hungerRate;
    Il2CppObject* cfg = unity::Field<Il2CppObject*>(emo, g.offEmoConfig);
    Il2CppObject* prop = cfg && unity::IsAlive(cfg) ? unity::Field<Il2CppObject*>(cfg, g.offCfgHungerRate) : nullptr;
    if (!prop || !unity::IsAlive(prop)) return 0.0f;
    const float rate = unity::Field<float>(prop, g.offPropDefault);
    g.hungerRate = rate > 0.0f && rate < 100.0f ? rate : 0.0f;
    SR2_INFO("[{}] shadow catcher: kept shadow slimes get the vanilla hunger rate {:.4f}/game h (property '{}'){}",
             kModule, rate, unity::GetName(prop), g.hungerRate > 0.0f ? "" : " - not usable, their hunger won't come back");
    return g.hungerRate;
}

// The Ringtail rule, on / off for one slime: _keepEatingWhenFull (it eats whatever reaches it and goes for
// food), one meal empties its hunger (DrivePerEat), and the hunger comes back at the vanilla rate. While
// it is not hungry, FinishChomp skips the plort. All in memory: none of these fields is saved (the hunger
// value itself is, in SlimeModel.Emotions, as for every slime).
void SetEating(Il2CppObject* scatter, Tracked& t, bool on) {
    if (t.eating == on || !g.slimeEatClass || !g.offKeepEating) return;
    Il2CppObject* eat = unity::GetComponent(scatter, g.slimeEatClass);
    if (!eat || !unity::IsAlive(eat)) return;
    unity::Field<bool>(eat, g.offKeepEating) = on;
    Invoke(g.initFood, eat, {}, "SlimeEat.InitFood");  // eats from the current diet
    if (Il2CppObject* emo = EmotionsOf(eat)) {
        float& rate = unity::Field<float>(emo, g.offHungerRate);
        float& dir = unity::Field<float>(emo, g.offHungerDir);
        float& perEat = unity::Field<float>(eat, g.offDrivePerEat);
        if (on) {
            t.origRate = rate;
            t.origDir = dir;
            t.origDrivePerEat = perEat;
            rate = DefaultHungerRate(emo);
            dir = 1.0f;
            if (perEat < kDrivePerMeal) perEat = kDrivePerMeal;
        } else {
            rate = t.origRate;
            dir = t.origDir;
            perEat = t.origDrivePerEat;
            if (Il2CppObject* face = unity::Field<Il2CppObject*>(eat, g.offEatFace); face && unity::IsAlive(face))
                unity::Field<bool>(face, g.offMischief) = false;
        }
        Invoke(g.computeHunger, emo, {}, "SlimeEmotions.ComputeHungerChangePerHour");
    }
    t.eating = on;
    t.fullKnown = false;
}

// Kept and eating: keep our rate (a property change would put Shadow's 0 back), show the Ringtail's
// mischievous face while full, log "full" / "hungry again" (rate-limited).
void WatchHunger(Il2CppObject* scatter, Tracked& t) {
    if (!t.eating || !g.hungerOk) return;
    Il2CppObject* eat = unity::GetComponent(scatter, g.slimeEatClass);
    Il2CppObject* emo = eat && unity::IsAlive(eat) ? EmotionsOf(eat) : nullptr;
    if (!emo) return;
    const float want = DefaultHungerRate(emo);
    if (unity::Field<float>(emo, g.offHungerRate) != want || unity::Field<float>(emo, g.offHungerDir) != 1.0f) {
        unity::Field<float>(emo, g.offHungerRate) = want;
        unity::Field<float>(emo, g.offHungerDir) = 1.0f;
        Invoke(g.computeHunger, emo, {}, "SlimeEmotions.ComputeHungerChangePerHour");
    }
    const float hunger = unity::Field<float>(emo, g.offEmoValues);  // float4 _emotions, [0] = HUNGER
    const float minDrive = unity::Field<float>(eat, g.offMinDriveToEat);
    const bool full = !(hunger >= minDrive);  // IsHungryFor with our entries' drive (= the hunger)
    if (Il2CppObject* face = unity::Field<Il2CppObject*>(eat, g.offEatFace); face && unity::IsAlive(face))
        unity::Field<bool>(face, g.offMischief) = full;
    if (t.fullKnown && full == t.full) return;
    const bool first = !t.fullKnown;
    t.full = full;
    t.fullKnown = true;
    if (!LogToken()) return;
    const std::string where = unity::GetName(unity::GameObjectOf(scatter));
    if (full)
        SR2_INFO("[{}] shadow catcher: shadow slime '{}' {} (hunger {:.2f} < {:.2f}): it eats but makes no plort, "
                 "hungry again in ~{:.1f} game h", kModule, where, first ? "is full" : "ate and is full", hunger, minDrive,
                 g.hungerRate > 0.0f ? (minDrive - hunger) / g.hungerRate : 0.0f);
    else
        SR2_INFO("[{}] shadow catcher: shadow slime '{}' is {}hungry (hunger {:.2f} >= {:.2f}): its next meal makes a "
                 "plort", kModule, where, first ? "" : "again ", hunger, minDrive);
}

// ---- sunlight (the Phosphor rule) ----

struct Vec3 {
    float x, y, z;
};
struct Bounds {
    Vec3 center, extents;
};

// The Phosphor's DestroyOutsideHoursOfDay values (its prefab or one of its modules), once per session.
void ReadSunConfig() {
    if (g.sunConfig) return;
    g.sunConfig = true;
    Il2CppObject* doh = nullptr;
    Il2CppObject* phosphor = nullptr;
    if (g.dohClass && g.offPrefab) {
        for (Il2CppObject* o : unity::FindObjectsOfTypeAll(g.slimeDefClass))
            if (IsA(o, g.slimeDefClass) && unity::IsAlive(o) && unity::GetName(o) == "Phosphor") phosphor = o;
        if (phosphor) {
            Il2CppObject* prefab = unity::Field<Il2CppObject*>(phosphor, g.offPrefab);
            if (unity::IsAlive(prefab)) doh = unity::GetComponent(prefab, g.dohClass);
            void* mods = g.offModules ? unity::Field<void*>(phosphor, g.offModules) : nullptr;
            for (size_t i = 0, n = unity::ArrayLength(mods); !doh && i < n; ++i) {
                auto* m = static_cast<Il2CppObject*>(unity::ArrayAt(mods, i));
                if (unity::IsAlive(m)) doh = unity::GetComponent(m, g.dohClass);
            }
        }
    }
    std::string from = "defaults";
    if (doh && unity::IsAlive(doh)) {
        const float s = unity::Field<float>(doh, g.offDohStart), e = unity::Field<float>(doh, g.offDohEnd);
        const float mn = unity::Field<float>(doh, g.offDohMin), mx = unity::Field<float>(doh, g.offDohMax);
        if (s >= 0.0f && s <= 24.0f && e >= 0.0f && e <= 24.0f && s != e) {
            g.liveStart = s;
            g.liveEnd = e;
            from = std::format("the Phosphor's '{}'", unity::GetName(unity::GameObjectOf(doh)));
        }
        if (mn >= 0.0f && mx >= mn && mx < 24.0f) {
            g.endureMin = std::max(mn, kEndureMin);
            g.endureMax = std::max(mx, kEndureMax);
            from += std::format(" (Phosphor endures {:.2f}-{:.2f} game h)", mn, mx);
        }
    }
    SR2_INFO("[{}] shadow catcher: sunlight rule from {}: kept shadow slimes are safe {:.1f}:00-{:.1f}:00, in the sun "
             "they fade after {:.2f}-{:.2f} game h unless shaded (Solar Shield / cave)", kModule, from, g.liveStart,
             g.liveEnd, g.endureMin, g.endureMax);
}

Il2CppObject* TimeDir() {
    Il2CppObject* td = g.timeDirHandle ? unity::GCHandleTarget(g.timeDirHandle) : nullptr;
    if (td && unity::IsAlive(td)) return td;
    if (g.timeDirHandle) {
        unity::FreeGCHandle(g.timeDirHandle);
        g.timeDirHandle = nullptr;
    }
    if (++g.timeDirTimer < kTimeDirRetryTicks) return nullptr;
    g.timeDirTimer = 0;
    td = unity::FindObjectOfType(g.timeDirClass);
    if (!td || !unity::IsAlive(td)) return nullptr;
    g.timeDirHandle = unity::NewGCHandle(td);
    return td;
}

bool InLiveWindow(float h) {
    return g.liveStart < g.liveEnd ? h >= g.liveStart && h < g.liveEnd : h >= g.liveStart || h < g.liveEnd;
}

// The shade: every enabled, active CaveTrigger collider (natural caves and the corral Solar Shield, whose
// trigger is active only while the upgrade is bought). Rescanned every ~10 s; prefab assets have no
// world bounds and drop out at the bounds check.
void RescanCaves() {
    if (++g.caveTimer < kCaveRescanTicks) return;
    g.caveTimer = 0;
    for (Il2CppGCHandle h : g.caves) unity::FreeGCHandle(h);
    g.caves.clear();
    for (Il2CppObject* c : unity::FindObjectsOfTypeAll(g.caveClass)) {
        if (!IsA(c, g.caveClass) || !unity::IsAlive(c)) continue;
        Il2CppObject* col = unity::GetComponent(c, g.colliderClass);
        if (!col || !unity::IsAlive(col)) continue;
        if (Il2CppGCHandle h = unity::NewGCHandle(col)) g.caves.push_back(h);
        if (g.caves.size() >= 512) break;
    }
}

std::vector<Bounds> ActiveShade() {
    std::vector<Bounds> out;
    for (Il2CppGCHandle h : g.caves) {
        Il2CppObject* col = unity::GCHandleTarget(h);
        if (!col || !unity::IsAlive(col) || !unity::ActiveInHierarchy(unity::GameObjectOf(col))) continue;
        bool ok = false;
        Il2CppObject* en = Invoke(g.colEnabled, col, {}, "Collider.get_enabled", &ok);
        if (!ok || !Unbox<bool>(en)) continue;
        Il2CppObject* b = Invoke(g.colBounds, col, {}, "Collider.get_bounds", &ok);
        if (!ok || !b) continue;
        const Bounds bb = Unbox<Bounds>(b);
        if (std::isfinite(bb.center.x) && bb.extents.x > 0.01f && bb.extents.y > 0.01f && bb.extents.z > 0.01f)
            out.push_back(bb);
    }
    return out;
}

bool Shaded(Il2CppObject* scatter, const std::vector<Bounds>& shade) {
    Il2CppObject* tr = unity::TransformOf(scatter);
    if (!tr) return true;  // unknown: never punish
    bool ok = false;
    Il2CppObject* p = Invoke(g.trPosition, tr, {}, "Transform.get_position", &ok);
    if (!ok || !p) return true;
    const Vec3 v = Unbox<Vec3>(p);
    for (const Bounds& b : shade)
        if (std::fabs(v.x - b.center.x) <= b.extents.x && std::fabs(v.y - b.center.y) <= b.extents.y &&
            std::fabs(v.z - b.center.z) <= b.extents.z)
            return true;
    return false;
}

void SetScaredFace(Il2CppObject* scatter, bool on) {
    if (!g.offFaceFleeing) return;
    if (Il2CppObject* face = unity::Field<Il2CppObject*>(scatter, g.offFace); face && unity::IsAlive(face))
        unity::Field<bool>(face, g.offFaceFleeing) = on;
}

// The sun gets it: alert it on purpose (vanilla DoAlert, not our detour), the vanilla scatter then makes it
// hop about and vanish with its DisappearFX 10-15 s later (Action -> Destroyer.DestroyActor).
void Fade(Il2CppObject* scatter, Tracked& t) {
    t.fading = true;
    t.exposed = false;
    Il2CppObject* go = unity::GameObjectOf(scatter);
    SR2_INFO("[{}] shadow catcher: shadow slime '{}' fades away in the sunlight (no Solar Shield or cave over it)", kModule,
             go ? unity::GetName(go) : std::string("?"));
    if (go && g.origDoAlert) g.origDoAlert(scatter, go, g.doAlertMi);
}

// Kept slime: in daylight and outside every shade the fade clock runs (Random(endure) game hours, like the
// Phosphor's shutdown clock); shade or night stops it. The scared face shows while the clock runs.
void WatchSun(Il2CppObject* scatter, Tracked& t, bool day, double now, const std::vector<Bounds>* shade) {
    if (t.fading) return;
    const bool exposed = day && shade && !Shaded(scatter, *shade);
    if (exposed == t.exposed) {
        if (exposed && now >= t.fadeAt) Fade(scatter, t);
        else if (exposed) SetScaredFace(scatter, true);
        return;
    }
    t.exposed = exposed;
    SetScaredFace(scatter, exposed);
    const std::string where = unity::GetName(unity::GameObjectOf(scatter));
    if (exposed) {
        const float r = static_cast<float>(std::rand()) / static_cast<float>(RAND_MAX);
        const float hours = g.endureMin + (g.endureMax - g.endureMin) * r;
        t.fadeAt = now + hours * 3600.0;
        SR2_INFO("[{}] shadow catcher: shadow slime '{}' is out in the sun: it fades in ~{:.2f} game h unless it gets "
                 "into the shade (Solar Shield / cave)", kModule, where, hours);
    } else {
        SR2_INFO("[{}] shadow catcher: shadow slime '{}' is safe again ({})", kModule, where,
                 day ? "in the shade" : "night");
    }
}

// ---- Slimepedia note ----

// The English shadow slime "Rancher Risks" text gets the sunlight note while the upgrade is owned. In memory
// only (the StringTable asset); other languages keep the vanilla text.
void PatchPedia(bool on) {
    if (!g.pediaOk) return;
    Il2CppObject* table = g.pediaTable ? unity::GCHandleTarget(g.pediaTable) : nullptr;
    if (table && !unity::IsAlive(table)) table = nullptr;
    if (!table) {
        if (g.pediaTable) {
            unity::FreeGCHandle(g.pediaTable);
            g.pediaTable = nullptr;
        }
        if (!on || ++g.pediaTimer < kPediaRetryTicks) return;
        g.pediaTimer = 0;
        for (Il2CppObject* o : unity::FindObjectsOfTypeAll(g.stringTableClass))
            if (IsA(o, g.stringTableClass) && unity::IsAlive(o) && unity::GetName(o) == kPediaTable) table = o;
        if (!table) return;  // not loaded yet (the Slimepedia loads it)
        g.pediaTable = unity::NewGCHandle(table);
    }
    bool ok = false;
    Il2CppObject* entry = Invoke(g.getEntry, table, {unity::NewString(kPediaKey)}, "StringTable.GetEntry", &ok);
    if (!ok || !entry) {
        if (!g.loggedPedia) SR2_WARN("[{}] shadow catcher: no '{}' entry in '{}' - no Slimepedia note", kModule, kPediaKey, kPediaTable);
        g.loggedPedia = true;
        return;
    }
    const std::string cur = unity::ToUtf8(reinterpret_cast<Il2CppString*>(Invoke(g.getValue, entry, {}, "StringTableEntry.get_Value", &ok)));
    if (!ok) return;
    const bool has = cur.find(kPediaNote) != std::string::npos;
    if (on && !has) {
        Invoke(g.setValue, entry, {unity::NewString(cur + kPediaNote)}, "StringTableEntry.set_Value", &ok);
        if (ok && !g.loggedPedia) SR2_INFO("[{}] shadow catcher: Slimepedia shadow slime risks got the sunlight note", kModule);
        g.loggedPedia = true;
    } else if (!on && has) {
        std::string back = cur;
        back.erase(back.find(kPediaNote), std::string_view(kPediaNote).size());
        Invoke(g.setValue, entry, {unity::NewString(back)}, "StringTableEntry.set_Value", &ok);
        g.loggedPedia = false;
    }
}

void Forget(bool revert) {
    for (auto& [scatter, t] : g.tracked) {
        Il2CppObject* live = t.handle ? unity::GCHandleTarget(t.handle) : nullptr;
        if (revert && live && unity::IsAlive(live)) {
            SetEating(live, t, false);
            if (t.exposed) SetScaredFace(live, false);
        }
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
                PatchPedia(false);
                g.dayKnown = false;
                SR2_INFO("[{}] shadow catcher: not owned - shadow slimes behave as in vanilla", kModule);
            }
            g.wasOwned = false;
            return;
        }
        if (!g.wasOwned) SR2_INFO("[{}] shadow catcher: owned - the vacpack holds shadow slimes, they stay on the ranch", kModule);
        g.wasOwned = true;
        const bool dietNew = EnsureDiet();
        PatchPedia(true);
        if (++g.logRefill >= 60) {  // ~1 minute of ticks
            g.logRefill = 0;
            g.logTokens = kFullLogBudget;
            g.loggedMuted = false;
        }
        // Sunlight: is it day, and where is the shade (only looked up while it is day and slimes are kept).
        bool day = false;
        double worldNow = 0.0;
        std::vector<Bounds> shade;
        bool shadeKnown = false;
        bool anyKept = false;
        for (const auto& [s, t] : g.tracked) anyKept = anyKept || t.kept;
        if (g.sunOk && g.shadowDef && anyKept) {
            ReadSunConfig();
            if (Il2CppObject* td = TimeDir()) {
                bool ok1 = false, ok2 = false;
                const float hour = Unbox<float>(Invoke(g.currHour, td, {}, "TimeDirector.CurrHour", &ok1));
                worldNow = Unbox<double>(Invoke(g.worldTime, td, {}, "TimeDirector.WorldTime", &ok2));
                if (ok1 && ok2 && std::isfinite(hour)) {
                    day = !InLiveWindow(hour);
                    if (!g.dayKnown || day != g.wasDay)
                        SR2_INFO("[{}] shadow catcher: {} (hour {:.1f}): kept shadow slimes {}", kModule,
                                 day ? "daylight" : "night", hour,
                                 day ? "need shade (Solar Shield / cave)" : "are safe anywhere on the ranch");
                    g.dayKnown = true;
                    g.wasDay = day;
                    if (day) {
                        RescanCaves();
                        shade = ActiveShade();
                        shadeKnown = true;
                    }
                }
            }
        }
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
                         now ? "is on the ranch: kept (no scatter, no despawn; needs shade by day), eats"
                             : "left the ranch: scatters as in vanilla");
            }
            if (t.kept && t.fading) {
                // the vanilla scatter is making it vanish; nothing else to do
            } else if (t.kept) {
                if (Alerted(scatter)) ClearAlert(scatter);
                if (dietNew) SetEating(scatter, t, false);  // recompute its eats with the new entries
                SetEating(scatter, t, !g.entries.empty());
                WatchHunger(scatter, t);
                if (g.sunOk) WatchSun(scatter, t, day, worldNow, shadeKnown ? &shade : nullptr);
                ++kept;
            } else {
                SetEating(scatter, t, false);
                if (t.exposed) SetScaredFace(scatter, false);
                t.exposed = false;
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
        PatchPedia(false);
        for (Il2CppGCHandle h : g.caves) unity::FreeGCHandle(h);
        g.caves.clear();
        g.caveTimer = kCaveRescanTicks;
        g.dayKnown = false;
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
    g.doAlertMi = alertMi;
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
    // Optional: the hunger rhythm (Ringtail rule). Without it a kept slime still keeps eating when full, but
    // its hunger never comes back (Shadow's rate is 0), so it makes plorts only until it is full once.
    if (g.offKeepEating) {
        std::string err;
        Il2CppClass* emo = R.Class(kImage, "MonomiPark.SlimeRancher.Slime", "SlimeEmotions", &err);
        // SlimeEmotionConfig lives in MonomiPark.SlimeRancher.Slime (the global-namespace lookup failed in the
        // round-8 build: "no hunger rhythm", so a kept slime made plorts until it was full once).
        Il2CppClass* cfg = emo ? R.Class(kImage, "MonomiPark.SlimeRancher.Slime", "SlimeEmotionConfig", &err) : nullptr;
        Il2CppClass* prop = cfg ? R.Class(kImage, "MonomiPark.UnitPropertySystem", "PropertyDefinition", &err) : nullptr;
        Il2CppClass* face = prop ? R.Class(kImage, "", "SlimeFaceAnimator", &err) : nullptr;
        bool ok = face != nullptr;
        auto f = [&](Il2CppClass* k, const char* name, size_t& out) {
            if (!ok) return;
            auto o = R.FieldOffset(k, name, &err);
            if (o) out = *o;
            else ok = false;
        };
        f(g.slimeEatClass, "DrivePerEat", g.offDrivePerEat);
        f(g.slimeEatClass, "MinDriveToEat", g.offMinDriveToEat);
        f(g.slimeEatClass, "_emotions", g.offEatEmotions);
        f(g.slimeEatClass, "_faceAnim", g.offEatFace);
        f(emo, "_hungerRecoveryRate", g.offHungerRate);
        f(emo, "_hungerDirection", g.offHungerDir);
        f(emo, "_emotionConfig", g.offEmoConfig);
        f(emo, "_emotions", g.offEmoValues);
        f(cfg, "HungerRate", g.offCfgHungerRate);
        f(prop, "DefaultValue", g.offPropDefault);
        f(face, "_isMischievous", g.offMischief);
        g.computeHunger = ok ? R.Method(emo, "ComputeHungerChangePerHour", 0, &err) : nullptr;
        g.hungerOk = ok && g.computeHunger;
        if (!g.hungerOk)
            SR2_WARN("[{}] shadow catcher: no hunger rhythm ({}) - kept shadow slimes make plorts only until they are "
                     "full once", kModule, err);
    }
    // Optional: the sunlight rule (kept shadow slimes need shade by day, like Phosphors).
    {
        std::string err;
        g.caveClass = R.Class(kImage, "", "CaveTrigger", &err);
        g.colliderClass = g.caveClass ? R.Class("UnityEngine.PhysicsModule", "UnityEngine", "Collider", &err) : nullptr;
        g.timeDirClass = g.colliderClass ? R.Class(kImage, "", "TimeDirector", &err) : nullptr;
        Il2CppClass* tr = g.timeDirClass ? R.Class("UnityEngine.CoreModule", "UnityEngine", "Transform", &err) : nullptr;
        g.colBounds = tr ? R.Method(g.colliderClass, "get_bounds", 0, &err) : nullptr;
        g.colEnabled = g.colBounds ? R.Method(g.colliderClass, "get_enabled", 0, &err) : nullptr;
        g.trPosition = g.colEnabled ? R.Method(tr, "get_position", 0, &err) : nullptr;
        g.currHour = g.trPosition ? R.Method(g.timeDirClass, "CurrHour", 0, &err) : nullptr;
        g.worldTime = g.currHour ? R.Method(g.timeDirClass, "WorldTime", 0, &err) : nullptr;
        g.sunOk = g.worldTime && g.doAlertMi;
        if (!g.sunOk)
            SR2_WARN("[{}] shadow catcher: no sunlight rule ({}) - kept shadow slimes are safe in daylight too", kModule, err);
        // The Phosphor's own values (optional; defaults otherwise).
        std::string e2;
        g.dohClass = g.sunOk ? R.Class(kImage, "", "DestroyOutsideHoursOfDay", &e2) : nullptr;
        auto o = [&](Il2CppClass* k, const char* name, size_t& out) {
            auto v = k ? R.FieldOffset(k, name, &e2) : std::nullopt;
            out = v ? *v : 0;
            return v.has_value();
        };
        if (!(o(g.dohClass, "StartHour", g.offDohStart) && o(g.dohClass, "EndHour", g.offDohEnd) &&
              o(g.dohClass, "MinEndureHoursOutsideWindow", g.offDohMin) &&
              o(g.dohClass, "MaxEndureHoursOutsideWindow", g.offDohMax) && o(ident, "prefab", g.offPrefab)))
            g.dohClass = nullptr;
        o(g.slimeDefClass, "SlimeModules", g.offModules);
    }
    // Optional: the Slimepedia note (English "Rancher Risks" of the shadow slime).
    {
        std::string err;
        g.stringTableClass = R.Class("Unity.Localization", "UnityEngine.Localization.Tables", "StringTable", &err);
        Il2CppClass* entry = g.stringTableClass
                                 ? R.Class("Unity.Localization", "UnityEngine.Localization.Tables", "StringTableEntry", &err)
                                 : nullptr;
        g.getEntry = entry ? R.MethodByParams(g.stringTableClass, "GetEntry", {"System.String"}, &err) : nullptr;
        g.getValue = g.getEntry ? R.Method(entry, "get_Value", 0, &err) : nullptr;
        g.setValue = g.getValue ? R.Method(entry, "set_Value", 1, &err) : nullptr;
        g.pediaOk = g.setValue != nullptr;
        if (!g.pediaOk) SR2_WARN("[{}] shadow catcher: no Slimepedia note ({})", kModule, err);
    }
    ready_ = true;
    return true;
}

}  // namespace sr2::modules::custom_upgrades::shadow
