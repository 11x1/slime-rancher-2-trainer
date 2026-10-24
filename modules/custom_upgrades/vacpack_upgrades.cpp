// custom_upgrades: vacpack upgrades in the Fabricator (see vacpack_upgrades.h). SR2 1.3.0, all
// lookups by name; RVAs for reference only.
//
// Vanilla (asset bundles read with UnityPy, disassembly of 1.3.0):
//   * UpgradeDefinition "AmmoCapacity" (_uniqueId 20, Tank Booster I-VIII): 8 levels, each ONE
//     FloatModifier ADD on PropertyDefinition "AmmoLimit" (default 20): +10 ... +80 (levels are
//     independent, not summed: 30 ... 100 per slot). Costs 450 / 1000 / 3000 / 10000 / 15000 /
//     20000 / 30000 / 50000 Newbucks plus refinery items, levels II+ also the TankBoosterComponent.
//   * "AmmoSlots" (_uniqueId 40, Extra Tank I-II): ADD +1 / +2 on "AmmoSlotsAvailable" (default 4).
//     1500 / 5000 Newbucks + items (II also the ExtraTankComponent). "LiquidSlot" (80): Water Tank.
//   * PlayerState._ammoSlotDefinitions (corescenes bundle): AmmoSlot1-4 (always unlocked),
//     "ExpansionAmmoSlot 1" / "2" (AmmoSlotsAvailable >= 5 / 6) and "LiquidAmmoSlot"
//     (LiquidSlotEnabled >= 1), all _maxAmount 30 with _maxAmountProperty = AmmoLimit. So vanilla
//     has 6 regular slots + the water slot, every definition used. Input actions exist for
//     MainGame/Slot1..Slot7 only (the 7th is the water slot): extra slots get no number key.
//   * UpgradeModel (DataModel): upgradeLevels Dictionary<int,int> keyed by _uniqueId, filled with
//     -1 for every definition of its list in the ctor (0xAD9FA0). Get/Increment/SetUpgradeLevel throw
//     for a definition not in upgradeDefinitions; SetUpgradeLevel (0xAD9C20) stores the level, sets
//     the TinkerTailorSlimes achievement progress to CountUniqueUpgrades() (every value >= 0) and
//     raises OnUpgradeChanged -> ActorUpgradeHandler.ApplyUpgrade(def, level): removes the
//     modifiers it added for that _uniqueId, adds the level's effects to the player's
//     UnitPropertyTracker (ModifiableFloat.AddModifier; ADD modifiers of different upgrades stack).
//     Pull (0xAD99C0) copies the whole dictionary into the save; Push (0xAD9A40) only reads the keys
//     the model has (an unknown saved id is ignored, a missing one becomes -1).
//   * Fabricator: PlayerUpgradeFabricateCategory._upgradeDefinitions (an UpgradeDefinitionList);
//     GetItems(recipeFactory) (0x82C000) maps it to PlayerUpgradeFabricatableItems each time the
//     menu opens (FabricatorCategoryList.FindCategoriesToShow). FabricateAndSpendCost (0x82AFA0):
//     CheckCanFabricate (blueprint / fully upgraded / costs), SpendableDirector.Spend(next level's
//     PurchaseCost), UpgradeModel.IncrementUpgradeLevel. Its texts go through
//     LocalizeStringEvent.set_StringReference, so core/text_override covers our cloned strings.
//   * AmmoSlotManager is built in PlayerState.InitModel (0x87B4F0) / Reset (0x87BC70) from
//     PlayerState._ammoSlotDefinitions; SetModel -> AmmoModel.Reset(defs) + ValidateAndAdjustSlots
//     (a saved array of another length: warning "Unserializing ammo slot length different...",
//     copied by index, the rest cleared/padded - never an exception). The HUD (AmmoSlotUI.Start)
//     binds every slot; AmmoSlotAdapter.PrefilterData hides locked ones (AmmoSlot.IsUnlocked).
//     AmmoModel.Pull (0xA6E510, body shared with TrackedActorsModel.Pull) returns the live array.
//     AmmoSlot.MaxCount is the live property value (no clamp: contents above it stay).

#include "modules/custom_upgrades/vacpack_upgrades.h"

#include <windows.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <format>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

#include "core/detour_guard.h"
#include "core/il2cpp_api.h"
#include "core/log.h"
#include "core/main_thread.h"
#include "core/module.h"
#include "core/resolver.h"
#include "core/text_override.h"
#include "core/unity.h"
#include "modules/custom_upgrades/api.h"

namespace sr2::modules::custom_upgrades::vacpack {
namespace {

namespace unity = sr2::core::unity;
namespace main_thread = sr2::core::main_thread;
namespace text_override = sr2::core::text_override;
using sr2::il2cpp::api;
using sr2::il2cpp::Resolver;

constexpr const char* kModule = "custom_upgrades";
constexpr const char* kImage = "Assembly-CSharp";
constexpr const char* kDataNs = "MonomiPark.SlimeRancher.DataModel";
constexpr const char* kPlayerNs = "MonomiPark.SlimeRancher.Player";
constexpr int32_t kVanillaCapacityId = 20;  // "AmmoCapacity" (Tank Booster I-VIII)
constexpr int32_t kVanillaSlotsId = 40;     // "AmmoSlots" (Extra Tank I-II)
constexpr int kExtraSlots = 2;              // Extra Tank III / IV
constexpr int kHideFlagsDontUnloadUnusedAsset = 32;
constexpr const char* kStashKey = "custom_upgrades.vacpack.extra_slots";
constexpr size_t kIdentCostSize = 16;  // IdentCostEntry {int Amount @0; IdentifiableType IdentType @8}

struct LevelSpec {
    float value;   // FloatModifier.Value (ADD)
    int newbucks;  // first currency cost
    const char* title;
};
struct Spec {
    int32_t id;         // our _uniqueId (custom range, never a vanilla id)
    const char* key;    // per-save value: the owned level (0-based), "" = none
    const char* name;   // asset name suffix / log
    int32_t parentId;   // vanilla upgrade this one continues (shown once that is maxed)
    int firstTemplate;  // AmmoCapacity level whose costs (refinery items) our first level copies
    bool slots;         // targets AmmoSlotsAvailable (else AmmoLimit, as cloned)
    int levels;
    LevelSpec level[4];
};

// Costs: our levels copy the refinery items of vanilla Tank Booster V-VIII (IX-XII) / VII-VIII
// (Extra Tank III-IV). Tank Booster IX-XII double the amounts (+2 for the rare items vanilla asks
// 1-3 of) and cost 60k-120k Newbucks (vanilla VIII: 50k); Extra Tank III-IV keep the amounts and cost
// 25k / 50k (vanilla Extra Tank II: 5k, but a slot is worth more than +25 per slot this late). No
// upgrade components (see CreateDefs).
const Spec kSpecs[] = {
    {custom_upgrades::kIdBase + 0x0101, "custom_upgrades.vacpack.tank_booster", "TankBoosterPlus", kVanillaCapacityId, 4,
     false, 4,
     {{25.0f, 60000, "Tank Booster IX"},
      {50.0f, 75000, "Tank Booster X"},
      {75.0f, 90000, "Tank Booster XI"},
      {100.0f, 120000, "Tank Booster XII"}}},
    {custom_upgrades::kIdBase + 0x0102, "custom_upgrades.vacpack.extra_tank", "ExtraTankPlus", kVanillaSlotsId, 6, true, 2,
     {{1.0f, 25000, "Extra Tank III"}, {2.0f, 50000, "Extra Tank IV"}, {}, {}}},
};
constexpr int kSpecCount = static_cast<int>(sizeof(kSpecs) / sizeof(kSpecs[0]));

int ScaleAmount(const Spec& s, int amount) {
    if (s.slots) return amount;
    return amount <= 5 ? amount + 2 : amount * 2;
}

using WBarrierFn = void (*)(Il2CppObject*, void**, void*);
using ArrayNewFn = Il2CppArray* (*)(Il2CppClass*, uintptr_t);

using SetLevelFn = void (*)(Il2CppObject*, Il2CppObject*, int32_t, const MethodInfo*);
using CountFn = int32_t (*)(Il2CppObject*, const MethodInfo*);
using PullDictFn = void (*)(Il2CppObject*, Il2CppObject**, const MethodInfo*);
using GetItemsFn = Il2CppObject* (*)(Il2CppObject*, Il2CppObject*, const MethodInfo*);
using InitModelFn = void (*)(Il2CppObject*, Il2CppObject*, const MethodInfo*);
using PullAmmoFn = void (*)(Il2CppObject*, Il2CppArray**, const MethodInfo*);

struct Ours {
    const Spec* spec = nullptr;
    Il2CppObject* def = nullptr;     // our UpgradeDefinition clone (GC handle held)
    Il2CppObject* parent = nullptr;  // the vanilla definition it continues
    int parentLevels = 0;
};

struct Player {
    Il2CppObject* state = nullptr;     // PlayerState
    Il2CppObject* model = nullptr;     // PlayerModel
    Il2CppObject* upgrades = nullptr;  // UpgradeModel
    Il2CppObject* ammo = nullptr;      // AmmoSlotManager
    Il2CppObject* ammoModel = nullptr; // AmmoModel
};

struct State {
    Host host;
    bool ready = false;
    WBarrierFn wbarrier = nullptr;
    ArrayNewFn arrayNew = nullptr;

    Il2CppClass* upgradeDefClass = nullptr;
    Il2CppClass* playerStateClass = nullptr;
    Il2CppClass* playerModelClass = nullptr;
    Il2CppClass* ammoMgrClass = nullptr;
    Il2CppClass* ammoSlotClass = nullptr;
    Il2CppClass* slotDefClass = nullptr;
    Il2CppClass* fabItemClass = nullptr;  // PlayerUpgradeFabricatableItem

    size_t offUmDefs = 0, offUmLevels = 0;
    size_t offUdIcon = 0, offUdId = 0, offUdPedia = 0, offUdLevels = 0;
    size_t offPsDefs = 0, offPsModel = 0, offPsAmmo = 0;
    size_t offAmModel = 0, offAmodelSlots = 0;
    size_t offSlotId = 0, offSlotCount = 0;
    size_t offSdProp = 0, offSdThreshold = 0, offSdHotkey = 0;
    size_t offCatDefs = 0, offFabDef = 0;
    size_t offPcCurrency = 0, offPcIdent = 0, offPcComponent = 0;  // PurchaseCost (value-type offsets)
    size_t offCostAmount = 0;                                      // CurrencyCostEntry._amount
    size_t offPropDefault = 0;                                     // PropertyDefinition.DefaultValue
    // UpgradeLevelDefinition (nested) and FloatModifier: read on the live objects
    size_t offUlEffects = 0, offUlCosts = 0, offUlTitle = 0, offUlDesc = 0;
    size_t offModTarget = 0, offModValue = 0;

    const MethodInfo* tryGetPlayer = nullptr;   // SceneContextUtility.TryGetPlayerState(out PlayerState)
    const MethodInfo* getUpgradeModel = nullptr;
    const MethodInfo* setLevel = nullptr;       // UpgradeModel.SetUpgradeLevel (invoked: goes through our detour)
    const MethodInfo* maybeAdd = nullptr;       // AmmoSlotManager.MaybeAddResource(type, slot, count, overflow)
    const MethodInfo* refId = nullptr;          // IdentifiableType.get_ReferenceId
    const MethodInfo* findByRef = nullptr;      // LookupDirector.TryFindIdentifiableTypeByReferenceId
    const MethodInfo* setHideFlags = nullptr;

    SetLevelFn origSetLevel = nullptr;
    CountFn origCount = nullptr;
    PullDictFn origPullDict = nullptr;
    GetItemsFn origGetItems = nullptr;
    InitModelFn origInitModel = nullptr;
    InitModelFn origReset = nullptr;
    PullAmmoFn origPullAmmo = nullptr;

    // runtime (main thread)
    Ours ours[kSpecCount];
    bool created = false, createFailed = false;
    Il2CppObject* modelList = nullptr;              // the player's UpgradeDefinitionList (handle held)
    std::vector<Il2CppObject*> categoryLists;       // Fabricator category lists we inserted into (handles held)
    Il2CppObject* extraSlotDefs[kExtraSlots] = {};  // our AmmoSlotDefinition clones (handles held)
    bool slotDefsFailed = false;
    Il2CppObject* vanillaSlotArray = nullptr;  // PlayerState's own array (handle) - put back on cleanup
    Il2CppObject* extendedSlotArray = nullptr; // the array we gave it (handle)
    Il2CppGCHandle vanillaSlotHandle = nullptr, extendedSlotHandle = nullptr;
    int vanillaSlotCount = 0;
    Il2CppObject* restoredAmmo = nullptr;  // AmmoModel whose extra slots were refilled (identity only)
    bool applying = false;                 // our own SetUpgradeLevel calls: not a purchase
    bool loggedNoSlots = false;
};

State g;

sr2::core::DetourGuard g_levelGuard{kModule, "UpgradeModel.SetUpgradeLevel"};
sr2::core::DetourGuard g_countGuard{kModule, "UpgradeModel.CountUniqueUpgrades"};
sr2::core::DetourGuard g_pullGuard{kModule, "UpgradeModel.Pull"};
sr2::core::DetourGuard g_itemsGuard{kModule, "PlayerUpgradeFabricateCategory.GetItems"};
sr2::core::DetourGuard g_slotsGuard{kModule, "PlayerState.InitModel/Reset"};
sr2::core::DetourGuard g_ammoGuard{kModule, "AmmoModel.Pull"};
sr2::core::DetourGuard g_tickGuard{kModule, "custom_upgrades.Vacpack"};

bool On() { return g.ready && g.host.on && g.host.on(); }

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
        else SR2_WARN("[{}] vacpack: {} failed: {}", kModule, what, err);
        return nullptr;
    }
    if (ok) *ok = true;
    return r;
}

// A method of the object's own (generic instance) class: List<T> / Dictionary<,> members.
const MethodInfo* MethodOf(Il2CppObject* obj, const char* name, int argc) {
    const auto& a = api();
    if (!obj || !a.il2cpp_object_get_class) return nullptr;
    for (Il2CppClass* c = a.il2cpp_object_get_class(obj); c; c = a.il2cpp_class_get_parent ? a.il2cpp_class_get_parent(c) : nullptr)
        if (const MethodInfo* m = a.il2cpp_class_get_method_from_name(c, name, argc)) return m;
    return nullptr;
}

void SetRef(Il2CppObject* obj, size_t offset, Il2CppObject* value) {
    void** slot = reinterpret_cast<void**>(reinterpret_cast<char*>(obj) + offset);
    if (g.wbarrier) g.wbarrier(obj, slot, value);
    else *slot = value;
}

void SetArrayRef(Il2CppArray* arr, size_t i, Il2CppObject* value) {
    void** slot = unity::ArrayData<void*>(arr) + i;
    if (g.wbarrier) g.wbarrier(reinterpret_cast<Il2CppObject*>(arr), slot, value);
    else *slot = value;
}

// ---- Dictionary<int,int> (UpgradeModel.upgradeLevels and its Pull copy) ----
bool DictHas(Il2CppObject* dict, int32_t key) {
    bool ok = false;
    Il2CppObject* r = Invoke(MethodOf(dict, "ContainsKey", 1), dict, {&key}, "Dictionary.ContainsKey", &ok);
    return ok && Unbox<bool>(r);
}
int32_t DictGet(Il2CppObject* dict, int32_t key, int32_t def = -1) {
    if (!DictHas(dict, key)) return def;
    bool ok = false;
    Il2CppObject* r = Invoke(MethodOf(dict, "get_Item", 1), dict, {&key}, "Dictionary.get_Item", &ok);
    return ok ? Unbox<int32_t>(r, def) : def;
}
bool DictSet(Il2CppObject* dict, int32_t key, int32_t value) {
    bool ok = false;
    Invoke(MethodOf(dict, "set_Item", 2), dict, {&key, &value}, "Dictionary.set_Item", &ok);
    return ok;
}
bool DictRemove(Il2CppObject* dict, int32_t key) {
    bool ok = false;
    Il2CppObject* r = Invoke(MethodOf(dict, "Remove", 1), dict, {&key}, "Dictionary.Remove", &ok);
    return ok && Unbox<bool>(r);
}

// ---- List<T> ----
int32_t ListIndexOf(Il2CppObject* list, Il2CppObject* item) {
    bool ok = false;
    Il2CppObject* r = Invoke(MethodOf(list, "IndexOf", 1), list, {item}, "List.IndexOf", &ok);
    return ok ? Unbox<int32_t>(r, -1) : -1;
}
bool ListInsert(Il2CppObject* list, int32_t index, Il2CppObject* item) {
    bool ok = false;
    Invoke(MethodOf(list, "Insert", 2), list, {&index, item}, "List.Insert", &ok);
    return ok;
}
bool ListRemoveAt(Il2CppObject* list, int32_t index) {
    bool ok = false;
    Invoke(MethodOf(list, "RemoveAt", 1), list, {&index}, "List.RemoveAt", &ok);
    return ok;
}
bool ListRemoveRange(Il2CppObject* list, int32_t index, int32_t count) {
    if (count <= 0) return true;
    bool ok = false;
    Invoke(MethodOf(list, "RemoveRange", 2), list, {&index, &count}, "List.RemoveRange", &ok);
    return ok;
}

// ListAsset<T>.items (the List<T> inside an UpgradeDefinitionList).
Il2CppObject* AssetItems(Il2CppObject* listAsset) {
    if (!unity::IsAlive(listAsset)) return nullptr;
    auto o = Resolver::Get().FieldOffset(unity::ClassOf(listAsset), "items");
    return o ? unity::Field<Il2CppObject*>(listAsset, *o) : nullptr;
}

bool GetPlayer(Player& p) {
    Il2CppObject* ps = nullptr;
    bool ok = false;
    Il2CppObject* r = Invoke(g.tryGetPlayer, nullptr, {&ps}, "SceneContextUtility.TryGetPlayerState", &ok);
    if (!ok || !Unbox<bool>(r) || !IsA(ps, g.playerStateClass) || !unity::IsAlive(ps)) return false;
    p.state = ps;
    p.model = unity::Field<Il2CppObject*>(ps, g.offPsModel);
    if (!IsA(p.model, g.playerModelClass)) return false;
    p.upgrades = Invoke(g.getUpgradeModel, p.model, {}, "PlayerModel.GetUpgradeModel", &ok);
    if (!ok || !p.upgrades) return false;
    p.ammo = unity::Field<Il2CppObject*>(ps, g.offPsAmmo);
    p.ammoModel = IsA(p.ammo, g.ammoMgrClass) ? unity::Field<Il2CppObject*>(p.ammo, g.offAmModel) : nullptr;
    return true;
}

int LevelCount(Il2CppObject* def) {
    return def ? static_cast<int>(unity::ListCount(unity::Field<void*>(def, g.offUdLevels))) : 0;
}
Il2CppObject* LevelAt(Il2CppObject* def, int i) {
    return def ? static_cast<Il2CppObject*>(unity::ListAt(unity::Field<void*>(def, g.offUdLevels), i)) : nullptr;
}

bool ResolveLevelFields(Il2CppObject* lvl) {
    if (g.offUlEffects) return true;
    Resolver& R = Resolver::Get();
    Il2CppClass* k = unity::ClassOf(lvl);
    auto e = R.FieldOffset(k, "Effects"), c = R.FieldOffset(k, "_costs"), t = R.FieldOffset(k, "Title"),
         d = R.FieldOffset(k, "Description");
    if (!e || !c || !t || !d) return false;
    g.offUlEffects = *e;
    g.offUlCosts = *c;
    g.offUlTitle = *t;
    g.offUlDesc = *d;
    return true;
}
Il2CppObject* FirstEffect(Il2CppObject* lvl) {
    void* effects = lvl ? unity::Field<void*>(lvl, g.offUlEffects) : nullptr;
    auto* mod = static_cast<Il2CppObject*>(unity::ListAt(effects, 0));
    if (mod && !g.offModTarget) {
        Resolver& R = Resolver::Get();
        Il2CppClass* mk = unity::ClassOf(mod);
        auto t = R.FieldOffset(mk, "TargetProperty"), v = R.FieldOffset(mk, "Value");
        if (!t || !v) return nullptr;
        g.offModTarget = *t;
        g.offModValue = *v;
    }
    return mod;
}

Il2CppObject* Clone(Il2CppObject* tpl, const std::string& name) {
    Il2CppObject* c = unity::Instantiate(tpl, nullptr);
    if (!unity::IsAlive(c)) return nullptr;
    unity::NewGCHandle(c);  // never freed: lives for the session
    unity::SetName(c, name);
    if (g.setHideFlags) {
        int32_t flags = kHideFlagsDontUnloadUnusedAsset;
        Invoke(g.setHideFlags, c, {&flags}, "Object.set_hideFlags");
    }
    return c;
}

// ---- our UpgradeDefinitions (once per session) ----
bool CreateDefs(Il2CppObject* items) {
    Il2CppObject* cap = nullptr;
    Il2CppObject* slots = nullptr;
    for (size_t i = 0, n = unity::ListCount(items); i < n; ++i) {
        auto* d = static_cast<Il2CppObject*>(unity::ListAt(items, i));
        if (!IsA(d, g.upgradeDefClass) || !unity::IsAlive(d)) continue;
        const int32_t id = unity::Field<int32_t>(d, g.offUdId);
        if (id == kVanillaCapacityId) cap = d;
        if (id == kVanillaSlotsId) slots = d;
    }
    const int capLevels = LevelCount(cap);
    if (!cap || !slots || capLevels < 8 || LevelCount(slots) < 1 || !ResolveLevelFields(LevelAt(cap, 0))) {
        SR2_WARN("[{}] vacpack: vanilla Tank Booster / Extra Tank upgrades not found (capacity {} level(s)) - no vacpack "
                 "upgrades", kModule, capLevels);
        return false;
    }
    Il2CppObject* capMaxMod = FirstEffect(LevelAt(cap, capLevels - 1));
    Il2CppObject* slotMod = FirstEffect(LevelAt(slots, 0));
    if (!capMaxMod || !slotMod) return false;
    Il2CppObject* limitProp = unity::Field<Il2CppObject*>(capMaxMod, g.offModTarget);
    Il2CppObject* slotProp = unity::Field<Il2CppObject*>(slotMod, g.offModTarget);
    const float capMax = unity::Field<float>(capMaxMod, g.offModValue);
    const float base = unity::IsAlive(limitProp) && g.offPropDefault ? unity::Field<float>(limitProp, g.offPropDefault) : 20.0f;

    for (int si = 0; si < kSpecCount; ++si) {
        const Spec& s = kSpecs[si];
        Il2CppObject* def = Clone(cap, std::string("sr2mods_") + s.name);
        if (!IsA(def, g.upgradeDefClass)) return false;
        Il2CppObject* levels = unity::Field<Il2CppObject*>(def, g.offUdLevels);
        // Deep copy check: own level list, own level objects, own texts - else overriding them would
        // change the vanilla upgrade.
        Il2CppObject* tplLevels = unity::Field<Il2CppObject*>(cap, g.offUdLevels);
        Il2CppObject* l0 = LevelAt(def, 0);
        Il2CppObject* v0 = LevelAt(cap, 0);
        auto currencyList = [](Il2CppObject* lvl) {
            return lvl ? *reinterpret_cast<void**>(reinterpret_cast<char*>(lvl) + g.offUlCosts + g.offPcCurrency) : nullptr;
        };
        if (!levels || levels == tplLevels || LevelCount(def) != capLevels || !l0 || l0 == v0 ||
            unity::Field<Il2CppObject*>(l0, g.offUlTitle) == unity::Field<Il2CppObject*>(v0, g.offUlTitle) ||
            FirstEffect(l0) == FirstEffect(v0) || currencyList(l0) == currencyList(v0)) {
            SR2_WARN("[{}] vacpack: the cloned upgrade shares its levels with Tank Booster - no vacpack upgrades", kModule);
            return false;
        }
        const int first = std::clamp(s.firstTemplate, 0, capLevels - s.levels);
        if (!ListRemoveRange(levels, 0, first) || !ListRemoveRange(levels, s.levels, LevelCount(def) - s.levels) ||
            LevelCount(def) != s.levels)
            return false;
        unity::Field<int32_t>(def, g.offUdId) = s.id;
        if (s.slots) {
            SetRef(def, g.offUdIcon, unity::Field<Il2CppObject*>(slots, g.offUdIcon));
            SetRef(def, g.offUdPedia, unity::Field<Il2CppObject*>(slots, g.offUdPedia));
        }
        std::string costLog;
        for (int k = 0; k < s.levels; ++k) {
            Il2CppObject* lvl = LevelAt(def, k);
            Il2CppObject* mod = FirstEffect(lvl);
            if (!mod) return false;
            Il2CppObject* effects = unity::Field<Il2CppObject*>(lvl, g.offUlEffects);
            ListRemoveRange(effects, 1, static_cast<int32_t>(unity::ListCount(effects)) - 1);  // vanilla has one
            if (s.slots) SetRef(mod, g.offModTarget, slotProp);
            unity::Field<float>(mod, g.offModValue) = s.level[k].value;
            // costs: Newbucks, refinery items, upgrade component
            char* pc = reinterpret_cast<char*>(lvl) + g.offUlCosts;
            void* currency = *reinterpret_cast<void**>(pc + g.offPcCurrency);
            for (size_t c = 0, n = unity::ListCount(currency); c < n; ++c)
                if (auto* e = static_cast<Il2CppObject*>(unity::ListAt(currency, c)))
                    unity::Field<int32_t>(e, g.offCostAmount) = c == 0 ? s.level[k].newbucks : 0;
            void* ident = *reinterpret_cast<void**>(pc + g.offPcIdent);
            auto identItemsOff = ident ? Resolver::Get().FieldOffset(unity::ClassOf(static_cast<Il2CppObject*>(ident)), "_items")
                                       : std::nullopt;
            void* identArr = identItemsOff ? unity::Field<void*>(ident, *identItemsOff) : nullptr;
            std::string paid;
            for (size_t c = 0, n = unity::ListCount(ident); c < n && identArr; ++c) {
                char* e = unity::ArrayData<char>(identArr) + c * kIdentCostSize;
                int32_t& amount = *reinterpret_cast<int32_t*>(e);
                amount = ScaleAmount(s, amount);
                auto* type = *reinterpret_cast<Il2CppObject**>(e + 8);
                paid += std::format("{}{} {}", paid.empty() ? "" : ", ", amount, unity::GetName(type));
            }
            // No upgrade component: vanilla consumes one Tank Booster / Extra Tank component per level and
            // the world may hold no more than vanilla needs, so ours cost Newbucks + refinery items only.
            if (auto* comps = *reinterpret_cast<Il2CppObject**>(pc + g.offPcComponent))
                Invoke(MethodOf(comps, "Clear", 0), comps, {}, "List.Clear");
            // texts
            std::string title = s.level[k].title;
            std::string desc;
            if (s.slots) {
                const int slot = 6 + static_cast<int>(s.level[k].value);
                desc = std::format("Squeezes a {}th tank onto your vacpack. It has no number key: scroll to it or use the "
                                   "next / previous slot buttons.",
                                   slot);
            } else {
                const int total = static_cast<int>(std::lround(base + capMax + s.level[k].value));
                desc = std::format("Hyper-compressed nano-storage pushes slime science even further: vac tank capacity "
                                   "goes up to {}.",
                                   total);
            }
            Il2CppObject* t = unity::Field<Il2CppObject*>(lvl, g.offUlTitle);
            Il2CppObject* d = unity::Field<Il2CppObject*>(lvl, g.offUlDesc);
            if (t) text_override::Set(t, title);
            if (d) text_override::Set(d, desc);
            costLog += std::format(" | {}: {} Newbucks + {}", title, s.level[k].newbucks, paid.empty() ? "nothing" : paid);
        }
        g.ours[si] = {&s, def, s.parentId == kVanillaCapacityId ? cap : slots, LevelCount(s.parentId == kVanillaCapacityId ? cap : slots)};
        SR2_INFO("[{}] vacpack: {} ready (id 0x{:X}, {} level(s)){}", kModule, s.name, s.id, s.levels, costLog);
    }
    SR2_INFO("[{}] vacpack: Tank Booster VIII = {} per slot (AmmoLimit default {} + {}); ours add 25 per level", kModule,
             std::lround(base + capMax), base, capMax);
    return true;
}

const Ours* FindOurs(Il2CppObject* def) {
    if (!def || !g.created) return nullptr;
    for (const Ours& o : g.ours)
        if (o.def == def) return &o;
    return nullptr;
}

// Insert our definitions into an UpgradeDefinitionList's items, right behind their vanilla parent.
int InsertOurs(Il2CppObject* items) {
    int added = 0;
    for (const Ours& o : g.ours) {
        if (!o.def || ListIndexOf(items, o.def) >= 0) continue;
        const int32_t at = ListIndexOf(items, o.parent);
        const int32_t n = static_cast<int32_t>(unity::ListCount(items));
        if (ListInsert(items, at >= 0 ? at + 1 : n, o.def)) ++added;
    }
    return added;
}
int RemoveOurs(Il2CppObject* items) {
    int removed = 0;
    for (const Ours& o : g.ours) {
        if (!o.def) continue;
        const int32_t at = ListIndexOf(items, o.def);
        if (at >= 0 && ListRemoveAt(items, at)) ++removed;
    }
    return removed;
}

// The Fabricator shows our card once the vanilla upgrade it continues is maxed (or ours is owned).
bool Unlocked(const Ours& o, Il2CppObject* upgrades) {
    Il2CppObject* dict = upgrades ? unity::Field<Il2CppObject*>(upgrades, g.offUmLevels) : nullptr;
    if (!dict) return false;
    if (DictGet(dict, o.spec->id) >= 0) return true;
    return o.parentLevels > 0 && DictGet(dict, o.spec->parentId) >= o.parentLevels - 1;
}

int DesiredLevel(const Ours& o) {
    const std::string v = g.host.value ? g.host.value(o.spec->key) : std::string();
    int lvl = -1;
    if (!v.empty()) std::from_chars(v.data(), v.data() + v.size(), lvl);
    return std::clamp(lvl, -1, o.spec->levels - 1);
}

// ---- extra slots ----
bool CreateSlotDefs(Il2CppArray* vanilla) {
    Il2CppObject* tpl = nullptr;
    int best = -1;
    for (size_t i = 0, n = unity::ArrayLength(vanilla); i < n; ++i) {
        auto* d = static_cast<Il2CppObject*>(unity::ArrayAt(vanilla, i));
        if (!IsA(d, g.slotDefClass) || !unity::Field<Il2CppObject*>(d, g.offSdProp)) continue;
        const int t = unity::Field<int32_t>(d, g.offSdThreshold);
        if (t > best) {
            best = t;
            tpl = d;
        }
    }
    if (!tpl || best < 1) {
        SR2_WARN("[{}] vacpack: no expansion slot definition to copy - no extra slots", kModule);
        return false;
    }
    for (int k = 0; k < kExtraSlots; ++k) {
        Il2CppObject* c = Clone(tpl, std::format("sr2mods_ExtraAmmoSlot{}", best + 1 + k));
        if (!IsA(c, g.slotDefClass)) return false;
        unity::Field<int32_t>(c, g.offSdThreshold) = best + 1 + k;
        SetRef(c, g.offSdHotkey, nullptr);  // no input action past Slot7 (the water slot)
        g.extraSlotDefs[k] = c;
    }
    SR2_INFO("[{}] vacpack: extra slot definitions ready (copies of '{}', unlocked at {} / {} slots)", kModule,
             unity::GetName(tpl), best + 1, best + 2);
    return true;
}

// PlayerState.InitModel / Reset (pre): the player's slot definitions + ours, before the game builds
// the AmmoSlotManager from them.
void ExtendSlots(Il2CppObject* ps) {
    if (!IsA(ps, g.playerStateClass) || !g.arrayNew) return;
    auto* arr = unity::Field<Il2CppArray*>(ps, g.offPsDefs);
    const size_t n = unity::ArrayLength(arr);
    if (!arr || n == 0) return;
    for (size_t i = 0; i < n; ++i)
        for (Il2CppObject* d : g.extraSlotDefs)
            if (d && unity::ArrayAt(arr, i) == d) return;  // already extended
    if (!g.extraSlotDefs[0] && !g.slotDefsFailed && !CreateSlotDefs(arr)) g.slotDefsFailed = true;
    if (g.slotDefsFailed) return;
    Il2CppArray* ext = g.arrayNew(g.slotDefClass, n + kExtraSlots);
    if (!ext) return;
    for (size_t i = 0; i < n; ++i) SetArrayRef(ext, i, static_cast<Il2CppObject*>(unity::ArrayAt(arr, i)));
    for (int k = 0; k < kExtraSlots; ++k) SetArrayRef(ext, n + k, g.extraSlotDefs[k]);
    // Keep both arrays alive (the vanilla one goes back on cleanup); the previous pair is released
    // later on the main thread (no handle frees inside a hook).
    Il2CppGCHandle oldV = g.vanillaSlotHandle, oldE = g.extendedSlotHandle;
    g.vanillaSlotHandle = unity::NewGCHandle(reinterpret_cast<Il2CppObject*>(arr));
    g.extendedSlotHandle = unity::NewGCHandle(reinterpret_cast<Il2CppObject*>(ext));
    if (oldV || oldE)
        main_thread::Post([oldV, oldE] {
            if (oldV) unity::FreeGCHandle(oldV);
            if (oldE) unity::FreeGCHandle(oldE);
        });
    g.vanillaSlotArray = reinterpret_cast<Il2CppObject*>(arr);
    g.extendedSlotArray = reinterpret_cast<Il2CppObject*>(ext);
    g.vanillaSlotCount = static_cast<int>(n);
    SetRef(ps, g.offPsDefs, reinterpret_cast<Il2CppObject*>(ext));
    SR2_INFO("[{}] vacpack: player slot definitions {} -> {} (extra slots stay locked until Extra Tank III / IV)",
             kModule, n, n + kExtraSlots);
}

std::string RefIdOf(Il2CppObject* type) {
    bool ok = false;
    Il2CppObject* s = Invoke(g.refId, type, {}, "IdentifiableType.get_ReferenceId", &ok);
    return ok ? unity::ToUtf8(reinterpret_cast<Il2CppString*>(s)) : std::string();
}

// "idx=refId*count;..." of the player's slots past the vanilla count.
std::string StashOf(Il2CppArray* slots) {
    std::string out;
    for (size_t i = static_cast<size_t>(g.vanillaSlotCount), n = unity::ArrayLength(slots); i < n; ++i) {
        auto* slot = static_cast<Il2CppObject*>(unity::ArrayAt(slots, i));
        if (!slot) continue;
        Il2CppObject* id = unity::Field<Il2CppObject*>(slot, g.offSlotId);
        const int count = unity::Field<int32_t>(slot, g.offSlotCount);
        if (!id || count <= 0) continue;
        const std::string ref = RefIdOf(id);
        if (ref.empty() || ref.find_first_of(";=*") != std::string::npos) continue;
        out += std::format("{}{}={}*{}", out.empty() ? "" : ";", i, ref, count);
    }
    return out;
}

void RestoreStash(const Player& p) {
    auto* slots = unity::Field<Il2CppArray*>(p.ammoModel, g.offAmodelSlots);
    const size_t n = unity::ArrayLength(slots);
    if (!g.vanillaSlotCount || n <= static_cast<size_t>(g.vanillaSlotCount)) return;  // not extended (yet)
    g.restoredAmmo = p.ammoModel;
    const std::string stash = g.host.value ? g.host.value(kStashKey) : std::string();
    if (stash.empty()) return;
    Il2CppObject* lookup = g.host.lookup ? g.host.lookup() : nullptr;
    int put = 0, skipped = 0;
    size_t pos = 0;
    while (pos < stash.size()) {
        size_t end = stash.find(';', pos);
        if (end == std::string::npos) end = stash.size();
        const std::string_view e(stash.data() + pos, end - pos);
        pos = end + 1;
        const size_t eq = e.find('='), star = e.rfind('*');
        int idx = -1, count = 0;
        if (eq == std::string_view::npos || star == std::string_view::npos || star < eq) {
            ++skipped;
            continue;
        }
        std::from_chars(e.data(), e.data() + eq, idx);
        std::from_chars(e.data() + star + 1, e.data() + e.size(), count);
        const std::string ref(e.substr(eq + 1, star - eq - 1));
        if (idx < g.vanillaSlotCount || idx >= static_cast<int>(n) || count <= 0 || !lookup) {
            ++skipped;
            continue;
        }
        auto* slot = static_cast<Il2CppObject*>(unity::ArrayAt(slots, idx));
        if (!slot || unity::Field<int32_t>(slot, g.offSlotCount) > 0) {
            ++skipped;
            continue;
        }
        Il2CppObject* type = nullptr;
        bool ok = false;
        Il2CppString* refStr = unity::NewString(ref);
        Il2CppObject* r = Invoke(g.findByRef, lookup, {refStr, &type}, "LookupDirector.TryFindIdentifiableTypeByReferenceId", &ok);
        if (!ok || !Unbox<bool>(r) || !type) {
            SR2_WARN("[{}] vacpack: slot {} item '{}' x{} not found - left in the sidecar only", kModule, idx + 1, ref, count);
            ++skipped;
            continue;
        }
        int32_t slotIdx = idx, cnt = count;
        bool overflow = true;  // never lose items to a lower maximum
        r = Invoke(g.maybeAdd, p.ammo, {type, &slotIdx, &cnt, &overflow}, "AmmoSlotManager.MaybeAddResource", &ok);
        if (ok && Unbox<bool>(r)) ++put;
        else ++skipped;
    }
    SR2_INFO("[{}] vacpack: extra slots refilled from the sidecar ({} put back, {} skipped): {}", kModule, put, skipped, stash);
}

// ---------------------------------------------------------------- detours --

// Every level change of every upgrade. Ours (not our own restore): the purchase goes to the sidecar.
void SetLevelDetour(Il2CppObject* self, Il2CppObject* def, int32_t level, const MethodInfo* mi) {
    g.origSetLevel(self, def, level, mi);
    if (g.applying || !def || !On()) return;
    g_levelGuard.Run([&] {
        const Ours* o = FindOurs(def);
        if (!o || !main_thread::IsMainThread() || !g.host.saveReady || !g.host.saveReady()) return;
        g.host.setValue(o->spec->key, level >= 0 ? std::to_string(level) : std::string());
        SR2_INFO("[{}] vacpack: {} -> level {} ({}), saved with the next game save", kModule, o->spec->name, level + 1,
                 level >= 0 ? o->spec->level[std::min(level, o->spec->levels - 1)].title : "none");
    });
}

// TinkerTailorSlimes counts the upgrades with a level >= 0: ours never count.
int32_t CountDetour(Il2CppObject* self, const MethodInfo* mi) {
    int32_t r = g.origCount(self, mi);
    if (!g.created || !self) return r;
    g_countGuard.Run([&] {
        Il2CppObject* dict = unity::Field<Il2CppObject*>(self, g.offUmLevels);
        for (const Ours& o : g.ours)
            if (o.def && DictGet(dict, o.spec->id) >= 0) --r;
    });
    return std::max(r, 0);
}

// The save's copy of the upgrade levels: our ids out (they live in the sidecar).
void PullDictDetour(Il2CppObject* self, Il2CppObject** saved, const MethodInfo* mi) {
    g.origPullDict(self, saved, mi);
    if (!g.created || !saved || !*saved) return;
    g_pullGuard.Run([&] {
        for (const Ours& o : g.ours)
            if (o.def) DictRemove(*saved, o.spec->id);
    });
}

// Fabricator upgrade category (each time the menu opens): our definitions in its list (pre), our
// cards out of the result while the vanilla upgrade is not maxed (post).
Il2CppObject* GetItemsDetour(Il2CppObject* self, Il2CppObject* recipeFactory, const MethodInfo* mi) {
    const bool on = On() && g.created;
    if (on && self)
        g_itemsGuard.Run([&] {
            Il2CppObject* items = AssetItems(unity::Field<Il2CppObject*>(self, g.offCatDefs));
            if (!items) return;
            if (InsertOurs(items) > 0) {
                if (std::find(g.categoryLists.begin(), g.categoryLists.end(), items) == g.categoryLists.end()) {
                    unity::NewGCHandle(items);  // a handful per session: kept for cleanup
                    g.categoryLists.push_back(items);
                }
                SR2_INFO("[{}] vacpack: upgrades added to the Fabricator category list", kModule);
            }
        });
    Il2CppObject* r = g.origGetItems(self, recipeFactory, mi);
    if (r && g.created)
        g_itemsGuard.Run([&] {
            Player p;
            const bool havePlayer = on && GetPlayer(p);
            for (int32_t i = static_cast<int32_t>(unity::ListCount(r)) - 1; i >= 0; --i) {
                auto* item = static_cast<Il2CppObject*>(unity::ListAt(r, static_cast<size_t>(i)));
                if (!IsA(item, g.fabItemClass)) continue;
                const Ours* o = FindOurs(unity::Field<Il2CppObject*>(item, g.offFabDef));
                if (!o) continue;
                if (!havePlayer || !Unlocked(*o, p.upgrades)) ListRemoveAt(r, i);
            }
        });
    return r;
}

void InitModelDetour(Il2CppObject* self, Il2CppObject* model, const MethodInfo* mi) {
    if (On() && self) g_slotsGuard.Run([&] { ExtendSlots(self); });
    g.origInitModel(self, model, mi);
}
void ResetDetour(Il2CppObject* self, Il2CppObject* model, const MethodInfo* mi) {
    if (On() && self) g_slotsGuard.Run([&] { ExtendSlots(self); });
    g.origReset(self, model, mi);
}

// The player's ammo for the save: slots past the vanilla count go to the sidecar, the save gets the
// vanilla count (a mod-free load sees an untouched vanilla vacpack).
void PullAmmoDetour(Il2CppObject* self, Il2CppArray** slots, const MethodInfo* mi) {
    g.origPullAmmo(self, slots, mi);
    if (!slots || !*slots || !g.vanillaSlotCount || !On() || !g.arrayNew) return;
    g_ammoGuard.Run([&] {
        // The body is shared with TrackedActorsModel.Pull (out HashSet): the owner check comes first.
        Player p;
        if (!GetPlayer(p) || p.ammoModel != self) return;  // a silo, a drone, TrackedActorsModel...
        Il2CppArray* arr = *slots;
        const size_t n = unity::ArrayLength(arr);
        if (n <= static_cast<size_t>(g.vanillaSlotCount)) return;
        if (self == g.restoredAmmo) {
            const std::string stash = StashOf(arr);
            g.host.setValue(kStashKey, stash);
            if (!stash.empty()) SR2_INFO("[{}] vacpack: extra slots kept in the sidecar: {}", kModule, stash);
        } else {
            SR2_WARN("[{}] vacpack: saved before the extra slots were refilled - their sidecar contents are kept", kModule);
        }
        Il2CppArray* cut = g.arrayNew(g.ammoSlotClass, static_cast<uintptr_t>(g.vanillaSlotCount));
        if (!cut) return;
        for (int i = 0; i < g.vanillaSlotCount; ++i) SetArrayRef(cut, i, static_cast<Il2CppObject*>(unity::ArrayAt(arr, i)));
        void** out = reinterpret_cast<void**>(slots);
        if (g.wbarrier) g.wbarrier(nullptr, out, cut);
        else *out = cut;
    });
}

}  // namespace

// ---------------------------------------------------------------- public --

void VacpackUpgrades::Tick() {
    if (!ready_ || !On() || !main_thread::IsMainThread()) return;
    g_tickGuard.Run([&] {
        if (!g.host.saveReady || !g.host.saveReady()) return;
        Player p;
        if (!GetPlayer(p)) return;
        Il2CppObject* defsAsset = unity::Field<Il2CppObject*>(p.upgrades, g.offUmDefs);
        Il2CppObject* items = AssetItems(defsAsset);
        Il2CppObject* dict = unity::Field<Il2CppObject*>(p.upgrades, g.offUmLevels);
        if (!items || !dict) return;
        if (!g.created && !g.createFailed) {
            g.created = CreateDefs(items);
            g.createFailed = !g.created;
        }
        if (!g.created) return;
        // The model knows our ids before the list does (ActorUpgradeHandler.SetModifiersFromModel
        // asks GetUpgradeLevel for every listed definition; a missing key would throw).
        for (const Ours& o : g.ours)
            if (!DictHas(dict, o.spec->id)) DictSet(dict, o.spec->id, -1);
        if (const int added = InsertOurs(items)) {
            if (g.modelList != items) {
                unity::NewGCHandle(items);
                g.modelList = items;
            }
            SR2_INFO("[{}] vacpack: {} upgrade(s) added to the player's upgrade list", kModule, added);
        }
        // Saved levels -> model (after a load, a re-enable or a sidecar change).
        for (const Ours& o : g.ours) {
            int32_t want = DesiredLevel(o);
            const int32_t have = DictGet(dict, o.spec->id);
            if (want == have) continue;
            g.applying = true;
            bool ok = false;
            Invoke(g.setLevel, p.upgrades, {o.def, &want}, "UpgradeModel.SetUpgradeLevel", &ok);
            g.applying = false;
            SR2_INFO("[{}] vacpack: {} level {} -> {} (from the sidecar){}", kModule, o.spec->name, have + 1, want + 1,
                     ok ? "" : " FAILED");
        }
        // Extra slots' contents, once per player ammo model.
        if (p.ammoModel && p.ammoModel != g.restoredAmmo) RestoreStash(p);
        if (!g.vanillaSlotCount && !g.loggedNoSlots && DictGet(dict, kSpecs[1].id) >= 0) {
            g.loggedNoSlots = true;  // custom upgrades were switched on after this save was loaded
            SR2_INFO("[{}] vacpack: Extra Tank III/IV owned but the player was built without the extra slots - they "
                     "appear after loading the save again", kModule);
        }
    });
}

void VacpackUpgrades::Cleanup(const char* why) {
    if (!ready_ || !main_thread::IsMainThread()) return;
    g_tickGuard.Run([&] {
        Player p;
        const bool havePlayer = GetPlayer(p);
        if (g.created && havePlayer) {
            Il2CppObject* dict = unity::Field<Il2CppObject*>(p.upgrades, g.offUmLevels);
            for (const Ours& o : g.ours) {
                if (!dict || !DictHas(dict, o.spec->id)) continue;
                if (DictGet(dict, o.spec->id) >= 0) {
                    int32_t none = -1;
                    g.applying = true;
                    Invoke(g.setLevel, p.upgrades, {o.def, &none}, "UpgradeModel.SetUpgradeLevel");
                    g.applying = false;
                }
                DictRemove(dict, o.spec->id);
            }
        }
        int removed = 0;
        if (g.created) {
            if (Il2CppObject* items = havePlayer ? AssetItems(unity::Field<Il2CppObject*>(p.upgrades, g.offUmDefs)) : nullptr)
                removed += RemoveOurs(items);
            if (g.modelList) removed += RemoveOurs(g.modelList);
            for (Il2CppObject* l : g.categoryLists) removed += RemoveOurs(l);
        }
        bool slotsBack = false;
        if (havePlayer && g.extendedSlotArray && unity::Field<Il2CppObject*>(p.state, g.offPsDefs) == g.extendedSlotArray) {
            SetRef(p.state, g.offPsDefs, g.vanillaSlotArray);
            slotsBack = true;
        }
        if (removed || slotsBack)
            SR2_INFO("[{}] {}: vacpack upgrades off ({} list entr{} removed{})", kModule, why, removed,
                     removed == 1 ? "y" : "ies",
                     slotsBack ? "; extra slots stay until the next load, locked" : "");
    });
}

void VacpackUpgrades::Reset() { g.restoredAmmo = nullptr; }

bool VacpackUpgrades::Setup(sr2::core::ModuleContext& ctx, Host host, std::string& why, std::string& report) {
    Resolver& R = ctx.resolver();
    auto parentOf = [](Il2CppClass* c) { return api().il2cpp_class_get_parent ? api().il2cpp_class_get_parent(c) : nullptr; };
    auto cls = [&](const char* ns, const char* name) -> Il2CppClass* {
        std::string err;
        Il2CppClass* k = R.Class(kImage, ns, name, &err);
        if (!k && why.empty()) why = err.empty() ? std::format("class {}.{} not found", ns, name) : err;
        return k;
    };
    auto off = [&](Il2CppClass* k, const char* field, size_t& out) {
        std::string err;
        for (Il2CppClass* c = k; c; c = parentOf(c))
            if (auto o = R.FieldOffset(c, field, &err)) {
                out = *o;
                return;
            }
        if (why.empty()) why = std::format("field {} not found", field);
    };
    auto method = [&](Il2CppClass* k, const char* name, int argc) -> const MethodInfo* {
        std::string err;
        for (Il2CppClass* c = k; c; c = parentOf(c))
            if (const MethodInfo* m = R.Method(c, name, argc, &err)) return m;
        if (why.empty()) why = std::format("method {}({}) not found", name, argc);
        return nullptr;
    };

    g.host = std::move(host);
    g.wbarrier = reinterpret_cast<WBarrierFn>(GetProcAddress(api().module, "il2cpp_gc_wbarrier_set_field"));
    g.arrayNew = reinterpret_cast<ArrayNewFn>(GetProcAddress(api().module, "il2cpp_array_new"));
    if (!g.arrayNew) why = "il2cpp_array_new export missing";

    Il2CppClass* upModel = cls(kDataNs, "UpgradeModel");
    g.upgradeDefClass = cls("", "UpgradeDefinition");
    g.playerStateClass = cls("", "PlayerState");
    g.playerModelClass = cls(kDataNs, "PlayerModel");
    g.ammoMgrClass = cls(kPlayerNs, "AmmoSlotManager");
    g.ammoSlotClass = cls(kPlayerNs, "AmmoSlot");
    Il2CppClass* ammoModel = cls(kDataNs, "AmmoModel");
    g.slotDefClass = cls("", "AmmoSlotDefinition");
    g.fabItemClass = cls("MonomiPark.SlimeRancher.UI.Fabricator", "PlayerUpgradeFabricatableItem");
    Il2CppClass* category = cls("MonomiPark.SlimeRancher.UI.Fabricator", "PlayerUpgradeFabricateCategory");
    Il2CppClass* purchaseCost = cls("MonomiPark.SlimeRancher.Economy", "PurchaseCost");
    Il2CppClass* costEntry = cls("MonomiPark.SlimeRancher.Economy", "CurrencyCostEntry");
    Il2CppClass* propDef = R.Class("Assembly-CSharp", "MonomiPark.UnitPropertySystem", "PropertyDefinition");
    Il2CppClass* ident = cls("", "IdentifiableType");
    Il2CppClass* lookup = cls("", "LookupDirector");
    Il2CppClass* scu = cls("MonomiPark.SlimeRancher.Util", "SceneContextUtility");
    Il2CppClass* uobject = R.Class("UnityEngine.CoreModule", "UnityEngine", "Object");
    if (!why.empty()) return false;

    off(upModel, "upgradeDefinitions", g.offUmDefs);
    off(upModel, "upgradeLevels", g.offUmLevels);
    off(g.upgradeDefClass, "_icon", g.offUdIcon);
    off(g.upgradeDefClass, "_uniqueId", g.offUdId);
    off(g.upgradeDefClass, "_pediaEntry", g.offUdPedia);
    off(g.upgradeDefClass, "_levels", g.offUdLevels);
    off(g.playerStateClass, "_ammoSlotDefinitions", g.offPsDefs);
    off(g.playerStateClass, "_model", g.offPsModel);
    off(g.playerStateClass, "<Ammo>k__BackingField", g.offPsAmmo);
    off(g.ammoMgrClass, "_ammoModel", g.offAmModel);
    off(ammoModel, "Slots", g.offAmodelSlots);
    off(g.ammoSlotClass, "_id", g.offSlotId);
    off(g.ammoSlotClass, "_count", g.offSlotCount);
    off(g.slotDefClass, "_isUnlockedProperty", g.offSdProp);
    off(g.slotDefClass, "_isUnlockedThreshold", g.offSdThreshold);
    off(g.slotDefClass, "_hotkey", g.offSdHotkey);
    off(category, "_upgradeDefinitions", g.offCatDefs);
    off(g.fabItemClass, "<UpgradeDefinition>k__BackingField", g.offFabDef);
    off(purchaseCost, "_currencyCosts", g.offPcCurrency);
    off(purchaseCost, "IdentCosts", g.offPcIdent);
    off(purchaseCost, "ComponentCosts", g.offPcComponent);
    off(costEntry, "_amount", g.offCostAmount);
    if (!why.empty()) return false;
    for (size_t* o : {&g.offPcCurrency, &g.offPcIdent, &g.offPcComponent})  // value-type field offsets
        *o = *o >= 2 * sizeof(void*) ? *o - 2 * sizeof(void*) : *o;
    if (propDef)
        if (auto o = R.FieldOffset(propDef, "DefaultValue")) g.offPropDefault = *o;  // texts only (fallback 20)

    g.tryGetPlayer = method(scu, "TryGetPlayerState", 1);
    g.getUpgradeModel = method(g.playerModelClass, "GetUpgradeModel", 0);
    g.setLevel = method(upModel, "SetUpgradeLevel", 2);
    const MethodInfo* countMi = method(upModel, "CountUniqueUpgrades", 0);
    const MethodInfo* pullMi = method(upModel, "Pull", 1);
    const MethodInfo* itemsMi = method(category, "GetItems", 1);
    const MethodInfo* initMi = method(g.playerStateClass, "InitModel", 1);
    const MethodInfo* resetMi = method(g.playerStateClass, "Reset", 1);
    const MethodInfo* ammoPullMi = method(ammoModel, "Pull", 1);
    {
        std::string err;
        g.maybeAdd = R.MethodByParams(g.ammoMgrClass, "MaybeAddResource",
                                      {"IdentifiableType", "System.Int32", "System.Int32", "System.Boolean"}, &err);
        if (!g.maybeAdd && why.empty()) why = "AmmoSlotManager.MaybeAddResource: " + err;
    }
    g.refId = method(ident, "get_ReferenceId", 0);
    g.findByRef = method(lookup, "TryFindIdentifiableTypeByReferenceId", 2);
    if (uobject) {
        std::string err;
        g.setHideFlags = R.Method(uobject, "set_hideFlags", 1, &err);
    }
    if (!why.empty()) return false;

    struct H {
        const char* name;
        const MethodInfo* mi;
        void* detour;
        void** orig;
    };
    const H hooks[] = {
        {"UpgradeModel.SetUpgradeLevel", g.setLevel, reinterpret_cast<void*>(&SetLevelDetour),
         reinterpret_cast<void**>(&g.origSetLevel)},
        {"UpgradeModel.CountUniqueUpgrades", countMi, reinterpret_cast<void*>(&CountDetour),
         reinterpret_cast<void**>(&g.origCount)},
        {"UpgradeModel.Pull", pullMi, reinterpret_cast<void*>(&PullDictDetour), reinterpret_cast<void**>(&g.origPullDict)},
        {"PlayerUpgradeFabricateCategory.GetItems", itemsMi, reinterpret_cast<void*>(&GetItemsDetour),
         reinterpret_cast<void**>(&g.origGetItems)},
        {"PlayerState.InitModel", initMi, reinterpret_cast<void*>(&InitModelDetour),
         reinterpret_cast<void**>(&g.origInitModel)},
        {"PlayerState.Reset", resetMi, reinterpret_cast<void*>(&ResetDetour), reinterpret_cast<void**>(&g.origReset)},
        {"AmmoModel.Pull", ammoPullMi, reinterpret_cast<void*>(&PullAmmoDetour), reinterpret_cast<void**>(&g.origPullAmmo)},
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
            return false;  // hooks made so far pass through: ready_ stays false
        }
        report += std::format(" {}@0x{:X}", hooks[i].name, Resolver::Rva(bodies[i]));
    }
    g.ready = ready_ = true;
    return true;
}

}  // namespace sr2::modules::custom_upgrades::vacpack
