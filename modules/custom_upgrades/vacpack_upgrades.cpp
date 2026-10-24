// custom_upgrades: vacpack upgrades in the Fabricator (see vacpack_upgrades.h). SR2 1.3.0, all
// lookups by name; RVAs for reference only.
//
// Vanilla (asset bundles read with UnityPy, disassembly of 1.3.0):
//   * UpgradeDefinition "AmmoCapacity" (_uniqueId 20, Tank Booster I-VIII): 8 levels in
//     List<UpgradeLevelDefinition> _levels, each ONE FloatModifier ADD on PropertyDefinition
//     "AmmoLimit" (default 20): +10 ... +80 (levels are independent, not summed: 30 ... 100 per
//     slot). Costs 450 / 1000 / 3000 / 10000 / 15000 / 20000 / 30000 / 50000 Newbucks plus refinery
//     items, levels II+ also the TankBoosterComponent. LevelCount = _levels.Count, everywhere.
//   * "AmmoSlots" (_uniqueId 40, Extra Tank I-II): ADD +1 / +2 on "AmmoSlotsAvailable" (default 4).
//     1500 / 5000 Newbucks + items (II also the ExtraTankComponent). "LiquidSlot" (80): Water Tank.
//   * PlayerState._ammoSlotDefinitions (corescenes bundle): AmmoSlot1-4 (always unlocked),
//     "ExpansionAmmoSlot 1" / "2" (AmmoSlotsAvailable >= 5 / 6) and "LiquidAmmoSlot"
//     (LiquidSlotEnabled >= 1), all _maxAmount 30 with _maxAmountProperty = AmmoLimit. So vanilla
//     has 6 regular slots + the water slot, every definition used.
//   * UpgradeModel (DataModel): upgradeLevels Dictionary<int,int> keyed by _uniqueId, filled with
//     -1 for every definition of its list in the ctor (0xAD9FA0). SetUpgradeLevel (0xAD9C20) throws
//     for level >= LevelCount, stores the level, sets the TinkerTailorSlimes achievement progress to
//     CountUniqueUpgrades() and raises OnUpgradeChanged -> ActorUpgradeHandler.ApplyUpgrade(def,
//     level): the level's effects replace that upgrade's previous ones. IncrementUpgradeLevel
//     (0xAD9510) = SetUpgradeLevel(level + 1) while UpgradeLevelExist. Pull (0xAD99C0) hands the save
//     a NEW Dictionary copy; Push (0xAD9A40) writes the saved levels straight into the dictionary (no
//     SetUpgradeLevel), only for the keys the model has. A saved level past LevelCount throws later
//     in UpgradeDefinition.GetUpgradeLevel (ActorUpgradeHandler) - so the save must never hold one.
//   * Fabricator: PlayerUpgradeFabricateCategory._upgradeDefinitions; GetItems (0x82C000) maps it
//     to PlayerUpgradeFabricatableItems each time the menu opens. The card of an upgrade shows its
//     next level (GetUpgradeLevel + 1, clamped to LevelCount - 1): title, costs, "fully upgraded"
//     when no next level exists; the details panel's UpgradeChain draws one circle per level and the
//     grid button's pips likewise (fabricator_levels.cpp makes room for 12).
//   * AmmoSlotManager is built in PlayerState.InitModel (0x87B4F0) / Reset (0x87BC70) from
//     PlayerState._ammoSlotDefinitions; InitModel -> AmmoModel.Reset(defs) (one AmmoSlot per
//     definition, Slot.Definition = defs[i]). A load then runs PlayerModel.Push -> AmmoModel.Push
//     (0xA6E530): the saved slots are copied BY INDEX into the existing ones (count, id, emotions,
//     appearance, radiant). SetModel -> ValidateAndAdjustSlots clears a slot whose saved id its
//     definition does not allow. AmmoModel.Pull (0xA6E510, body shared with TrackedActorsModel.Pull)
//     returns the live array.
//   * The vacpack's number keys: VacuumItem._selectSlot (one InputEvent for MainGame/Slot1..Slot7)
//     -> VacuumItem.SelectSlot(InputEventData) (0xA69640): SetSelectedSlot(round(value)), value =
//     the slot index 0..6 (Optional<float> at +4 of the data). No Slot8 / Slot9 actions exist.
//   * HUD (corescenes, HudUI/UIContainer/"Ammo Slots"): 900 x 189, anchored bottom centre, a
//     HorizontalLayoutGroup (spacing 10, LowerCenter, no width control) over 108 px slots. Unity
//     centres children only while their total width is below the rect's; 9 slots (1052 px) overflow
//     900 to the right, so the row was off centre.

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
#include "core/settings.h"
#include "core/text_override.h"
#include "core/unity.h"
#include "modules/custom_upgrades/api.h"
#include "modules/custom_upgrades/fabricator_levels.h"
#include "ui/overlay.h"
#include "ui/settings_view.h"

namespace sr2::modules::custom_upgrades::vacpack {
namespace {

namespace unity = sr2::core::unity;
namespace main_thread = sr2::core::main_thread;
namespace text_override = sr2::core::text_override;
namespace settings = sr2::core::settings;
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
constexpr int kChainCircles = 8;            // level circles in the Fabricator prefabs (fabricator_levels.h)
// Extra slots' contents: "k=refId*count;..." (k = 0 / 1: Extra Tank III / IV's slot).
constexpr const char* kStashKey = "custom_upgrades.vacpack.extra_slot_items";
// Older builds: "index=refId*count;..." with the slots after the water slot (index 7 / 8).
constexpr const char* kOldStashKey = "custom_upgrades.vacpack.extra_slots";
constexpr size_t kIdentCostSize = 16;  // IdentCostEntry {int Amount @0; IdentifiableType IdentType @8}
constexpr float kHudSlotPitch = 118.0f;  // HUD: 108 px slot + 10 px spacing
constexpr float kHudMargin = 24.0f;
constexpr float kHudFindSeconds = 5.0f;  // FindObjectOfType(AmmoSlotUI) at most this often

struct LevelSpec {
    float value;   // capacity / slots: added to the vanilla max level's modifier value; battery: factor
    int newbucks;  // first currency cost
    const char* title;
};
enum class Kind {
    kCapacity,      // more levels of Tank Booster (ADD on AmmoLimit)
    kSlots,         // more levels of Extra Tank (ADD on AmmoSlotsAvailable)
    kDroneBattery,  // own card, no vacpack effect (ADD 0); drone_battery.cpp reads the level
};
struct Spec {
    int32_t id;         // own card: our _uniqueId (custom range); tiers: 0 (they are vanilla levels)
    int32_t parentId;   // tiers: the vanilla upgrade they continue; own card: 0
    const char* key;    // per-save value. Tiers: the vanilla upgrade's level (0-based), only past its
                        // vanilla max; own card: its owned level (0-based)
    const char* oldKey; // tiers: older builds' separate card level (0-based) - migrated once
    const char* name;   // asset name suffix / log
    int firstTemplate;  // AmmoCapacity level whose costs (refinery items) our first level copies
    Kind kind;
    int levels;
    LevelSpec level[4];
};

// Costs: our levels copy the refinery items of vanilla Tank Booster V-VIII (IX-XII) / VII-VIII
// (Extra Tank III-IV). Tank Booster IX-XII double the amounts (+2 for the rare items vanilla asks
// 1-3 of) and cost 60k-120k Newbucks (vanilla VIII: 50k); Extra Tank III-IV keep the amounts and cost
// 25k / 50k (vanilla Extra Tank II: 5k, but a slot is worth more than +25 per slot this late). No
// upgrade components (see CreateDefs).
// Drone Battery Saver I-IV (global, every station): half of Tank Booster V-VIII, both the Newbucks
// (7.5k / 10k / 15k / 25k) and the refinery items (rounded up). Old builds sold it per station for
// 1.5k-7.5k Newbucks (17k for one station's IV); one global purchase now covers every station, so it
// costs about as much as three stations did, and the items put it in the late game next to the drone
// stations themselves. Less fuel is its only gain, so it stays well below the vac upgrades.
const Spec kSpecs[] = {
    {0, kVanillaCapacityId, "custom_upgrades.vacpack.tank_booster_level", "custom_upgrades.vacpack.tank_booster",
     "TankBoosterTiers", 4, Kind::kCapacity, 4,
     {{25.0f, 60000, "Tank Booster IX"},
      {50.0f, 75000, "Tank Booster X"},
      {75.0f, 90000, "Tank Booster XI"},
      {100.0f, 120000, "Tank Booster XII"}}},
    {0, kVanillaSlotsId, "custom_upgrades.vacpack.extra_tank_level", "custom_upgrades.vacpack.extra_tank",
     "ExtraTankTiers", 6, Kind::kSlots, 2, {{1.0f, 25000, "Extra Tank III"}, {2.0f, 50000, "Extra Tank IV"}, {}, {}}},
    {custom_upgrades::kIdBase + 0x0103, 0, kDroneBatteryKey, "", "DroneBatterySaver", 4, Kind::kDroneBattery, 4,
     {{1.25f, 7500, "Drone Battery Saver I"},
      {1.5f, 10000, "Drone Battery Saver II"},
      {1.75f, 15000, "Drone Battery Saver III"},
      {2.0f, 25000, "Drone Battery Saver IV"}}},
};
constexpr int kSpecCount = static_cast<int>(sizeof(kSpecs) / sizeof(kSpecs[0]));

// Number keys of the extra slots (settings, Win32 virtual-key codes).
constexpr const char* kSlotKeySetting[kExtraSlots] = {"extra_slot_key_1", "extra_slot_key_2"};
// The game binds Slot1..Slot6 to 1-6 and the water slot (Slot7) to R: 7 and 8 are free.
constexpr int kSlotKeyDefault[kExtraSlots] = {'7', '8'};
constexpr int kSlotKeyOld[kExtraSlots] = {'8', '9'};  // the first builds' defaults (migrated once)
constexpr const char* kSlotKeyMigrated = "extra_slot_keys_v2";  // free-text marker: the migration ran

int ScaleAmount(const Spec& s, int amount) {
    switch (s.kind) {
        case Kind::kSlots: return amount;
        case Kind::kDroneBattery: return std::max(1, (amount + 1) / 2);
        default: return amount <= 5 ? amount + 2 : amount * 2;
    }
}
bool IsTier(const Spec& s) { return s.parentId != 0; }

using WBarrierFn = void (*)(Il2CppObject*, void**, void*);
using ArrayNewFn = Il2CppArray* (*)(Il2CppClass*, uintptr_t);

using SetLevelFn = void (*)(Il2CppObject*, Il2CppObject*, int32_t, const MethodInfo*);
using CountFn = int32_t (*)(Il2CppObject*, const MethodInfo*);
using PullDictFn = void (*)(Il2CppObject*, Il2CppObject**, const MethodInfo*);
using GetItemsFn = Il2CppObject* (*)(Il2CppObject*, Il2CppObject*, const MethodInfo*);
using InitModelFn = void (*)(Il2CppObject*, Il2CppObject*, const MethodInfo*);
using PullAmmoFn = void (*)(Il2CppObject*, Il2CppArray**, const MethodInfo*);
using PushAmmoFn = void (*)(Il2CppObject*, Il2CppArray*, const MethodInfo*);
using BindKeyFn = void (*)(Il2CppObject*, Il2CppObject*, const MethodInfo*);
using SelectSlotFn = void (*)(Il2CppObject*, void*, const MethodInfo*);

struct Vec2 {
    float x, y;
};

struct Ours {
    const Spec* spec = nullptr;
    Il2CppObject* def = nullptr;  // our UpgradeDefinition clone (GC handle held); tiers: holds the level objects
    // tiers only
    Il2CppObject* parent = nullptr;     // the vanilla definition our levels are appended to
    int vanillaLevels = 0;              // its level count without ours
    Il2CppObject* levelObjs[4] = {};    // our UpgradeLevelDefinitions (deep copies, kept alive by def)
    bool extended = false;              // our levels are in parent._levels
    bool loggedSkip = false;
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
    bool chainOk = false;  // the Fabricator details panel can show more than 8 circles
    WBarrierFn wbarrier = nullptr;
    ArrayNewFn arrayNew = nullptr;

    Il2CppClass* upgradeDefClass = nullptr;
    Il2CppClass* playerStateClass = nullptr;
    Il2CppClass* playerModelClass = nullptr;
    Il2CppClass* ammoMgrClass = nullptr;
    Il2CppClass* ammoSlotClass = nullptr;
    Il2CppClass* slotDefClass = nullptr;
    Il2CppClass* fabItemClass = nullptr;  // PlayerUpgradeFabricatableItem
    Il2CppClass* ammoUiClass = nullptr;   // MonomiPark.SlimeRancher.UI.AmmoSlotUI (HUD row)
    Il2CppClass* vacItemClass = nullptr;  // VacuumItem

    size_t offUmDefs = 0, offUmLevels = 0;
    size_t offUdIcon = 0, offUdId = 0, offUdPedia = 0, offUdLevels = 0;
    size_t offUdNeedsComponent = 0;  // _requireComponentToBeVisibleToPlayer (optional)
    size_t offPsDefs = 0, offPsModel = 0, offPsAmmo = 0;
    size_t offAmModel = 0, offAmodelSlots = 0, offAmSelected = 0;
    size_t offSlotId = 0, offSlotCount = 0, offSlotDef = 0;
    size_t offSdProp = 0, offSdThreshold = 0, offSdHotkey = 0;
    size_t offCatDefs = 0, offFabDef = 0;
    size_t offPcCurrency = 0, offPcIdent = 0, offPcComponent = 0;  // PurchaseCost (value-type offsets)
    size_t offCostAmount = 0;                                      // CurrencyCostEntry._amount
    size_t offPropDefault = 0;                                     // PropertyDefinition.DefaultValue
    // AmmoSlotViewHolder (key label; optional)
    size_t offVhData = 0, offVhKeyText = 0, offVhKeyLoc = 0, offVhKeyBox = 0;
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
    const MethodInfo* rtGetSize = nullptr;      // RectTransform.get_sizeDelta / set_sizeDelta
    const MethodInfo* rtSetSize = nullptr;
    const MethodInfo* tmpSetText = nullptr;     // TMP_Text.set_text / get_text
    const MethodInfo* tmpGetText = nullptr;
    const MethodInfo* setEnabled = nullptr;     // Behaviour.set_enabled
    const MethodInfo* activeEnabled = nullptr;  // Behaviour.get_isActiveAndEnabled
    const MethodInfo* selectSlotInt = nullptr;  // VacuumItem.SelectSlot(int) (private)
    const MethodInfo* lockState = nullptr;      // Cursor.get_lockState
    const MethodInfo* timeScale = nullptr;      // Time.get_timeScale

    SetLevelFn origSetLevel = nullptr;
    CountFn origCount = nullptr;
    PullDictFn origPullDict = nullptr;
    GetItemsFn origGetItems = nullptr;
    InitModelFn origInitModel = nullptr;
    InitModelFn origReset = nullptr;
    PullAmmoFn origPullAmmo = nullptr;
    PushAmmoFn origPushAmmo = nullptr;
    BindKeyFn origBindKey = nullptr;
    SelectSlotFn origSelect = nullptr;

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
    int vanillaSlotCount = 0;  // 7
    int insertAt = 0;          // index of our first slot = right after "ExpansionAmmoSlot 2", before the water slot
    Il2CppObject* restoredAmmo = nullptr;  // AmmoModel whose extra slots were refilled (identity only)
    bool applying = false;                 // our own SetUpgradeLevel calls: not a purchase
    bool loggedNoSlots = false, loggedNoBlueprint = false, loggedRemap = false, loggedKeys = false;
    bool batteryArt = false;  // the battery saver card has the Drone Station's icon / pedia entry
    bool migrated = false;    // old per-station battery tiers checked for this save
    bool tiersMigrated = false;  // old separate Tank Booster / Extra Tank cards checked for this save
    // HUD row
    Il2CppGCHandle hudHandle = nullptr;  // AmmoSlotUI
    float hudWidth = 0.0f;               // its width before we widened it (0 = untouched)
    float hudFindTimer = kHudFindSeconds;
    // number keys
    Il2CppGCHandle vacHandle = nullptr;  // VacuumItem
    bool keyDown[kExtraSlots] = {};
};

State g;

sr2::core::DetourGuard g_levelGuard{kModule, "UpgradeModel.SetUpgradeLevel"};
sr2::core::DetourGuard g_countGuard{kModule, "UpgradeModel.CountUniqueUpgrades"};
sr2::core::DetourGuard g_pullGuard{kModule, "UpgradeModel.Pull"};
sr2::core::DetourGuard g_itemsGuard{kModule, "PlayerUpgradeFabricateCategory.GetItems"};
sr2::core::DetourGuard g_slotsGuard{kModule, "PlayerState.InitModel/Reset"};
sr2::core::DetourGuard g_ammoGuard{kModule, "AmmoModel.Pull/Push"};
sr2::core::DetourGuard g_keyGuard{kModule, "AmmoSlotViewHolder.BindInputKey"};
sr2::core::DetourGuard g_selectGuard{kModule, "VacuumItem.SelectSlot"};
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

std::string RomanTitle(const Spec& s, int ourLevel) {
    return ourLevel >= 0 && ourLevel < s.levels ? s.level[ourLevel].title : "none";
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
bool ListAdd(Il2CppObject* list, Il2CppObject* item) {
    bool ok = false;
    Invoke(MethodOf(list, "Add", 1), list, {item}, "List.Add", &ok);
    return ok;
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

// The definition with this _uniqueId in an UpgradeDefinitionList's items.
Il2CppObject* FindDefById(Il2CppObject* items, int32_t id) {
    for (size_t i = 0, n = unity::ListCount(items); i < n; ++i) {
        auto* d = static_cast<Il2CppObject*>(unity::ListAt(items, i));
        if (IsA(d, g.upgradeDefClass) && unity::IsAlive(d) && unity::Field<int32_t>(d, g.offUdId) == id) return d;
    }
    return nullptr;
}

// ---- our levels / definitions (once per session) ----
bool CreateDefs(Il2CppObject* items) {
    Il2CppObject* cap = FindDefById(items, kVanillaCapacityId);
    Il2CppObject* slots = FindDefById(items, kVanillaSlotsId);
    const int capLevels = LevelCount(cap), slotLevels = LevelCount(slots);
    if (!cap || !slots || capLevels < 8 || slotLevels < 1 || !ResolveLevelFields(LevelAt(cap, 0))) {
        SR2_WARN("[{}] vacpack: vanilla Tank Booster / Extra Tank upgrades not found (capacity {} level(s)) - no vacpack "
                 "upgrades", kModule, capLevels);
        return false;
    }
    Il2CppObject* capMaxMod = FirstEffect(LevelAt(cap, capLevels - 1));
    Il2CppObject* slotMaxMod = FirstEffect(LevelAt(slots, slotLevels - 1));
    if (!capMaxMod || !slotMaxMod) return false;
    Il2CppObject* limitProp = unity::Field<Il2CppObject*>(capMaxMod, g.offModTarget);
    Il2CppObject* slotProp = unity::Field<Il2CppObject*>(slotMaxMod, g.offModTarget);
    const float capMax = unity::Field<float>(capMaxMod, g.offModValue);
    const float slotMax = unity::Field<float>(slotMaxMod, g.offModValue);
    const float base = unity::IsAlive(limitProp) && g.offPropDefault ? unity::Field<float>(limitProp, g.offPropDefault) : 20.0f;
    const float slotBase = unity::IsAlive(slotProp) && g.offPropDefault ? unity::Field<float>(slotProp, g.offPropDefault) : 4.0f;

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
        if (s.id) unity::Field<int32_t>(def, g.offUdId) = s.id;  // tiers: never registered, the id stays unused
        // Ours cost no upgrade component (below), so a component must not be needed to see the card.
        if (g.offUdNeedsComponent && s.id) unity::Field<bool>(def, g.offUdNeedsComponent) = false;
        std::string costLog;
        for (int k = 0; k < s.levels; ++k) {
            Il2CppObject* lvl = LevelAt(def, k);
            Il2CppObject* mod = FirstEffect(lvl);
            if (!mod) return false;
            Il2CppObject* effects = unity::Field<Il2CppObject*>(lvl, g.offUlEffects);
            ListRemoveRange(effects, 1, static_cast<int32_t>(unity::ListCount(effects)) - 1);  // vanilla has one
            // Levels replace each other (vanilla), so a tier's value is absolute: the vanilla max
            // level's value plus ours. The battery saver changes nothing on the vacpack: ADD 0 on
            // AmmoLimit (the level's effect list stays as the game expects it, one modifier).
            float value = 0.0f;
            if (s.kind == Kind::kCapacity) value = capMax + s.level[k].value;
            if (s.kind == Kind::kSlots) {
                SetRef(mod, g.offModTarget, slotProp);
                value = slotMax + s.level[k].value;
            }
            unity::Field<float>(mod, g.offModValue) = value;
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
            if (s.kind == Kind::kDroneBattery) {
                const float f = s.level[k].value;
                desc = std::format("Quantum power cells for your ranch drones: every drone station's drone uses {}% of its "
                                   "normal battery, so a full charge lasts {:g}x as long.",
                                   static_cast<int>(std::lround(100.0f / f)), f);
            } else if (s.kind == Kind::kSlots) {
                const int slot = static_cast<int>(std::lround(slotBase + value));
                desc = std::format("Squeezes a {}th tank onto your vacpack, right before the water tank. Select it with "
                                   "number key {} (changeable in the mod's custom upgrades options) or by scrolling.",
                                   slot, k + 8);
            } else {
                const int total = static_cast<int>(std::lround(base + value));
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
        Ours& o = g.ours[si];
        o = {};
        o.spec = &s;
        o.def = def;
        if (IsTier(s)) {
            o.parent = s.parentId == kVanillaCapacityId ? cap : slots;
            o.vanillaLevels = LevelCount(o.parent);
            for (int k = 0; k < s.levels; ++k) o.levelObjs[k] = LevelAt(def, k);
            SR2_INFO("[{}] vacpack: {} ready ({} level(s) after the vanilla {}){}", kModule, s.name, s.levels,
                     o.vanillaLevels, costLog);
        } else {
            SR2_INFO("[{}] vacpack: {} ready (id 0x{:X}, {} level(s)){}", kModule, s.name, s.id, s.levels, costLog);
        }
    }
    SR2_INFO("[{}] vacpack: Tank Booster VIII = {} per slot (AmmoLimit default {} + {}); IX-XII = {} / {} / {} / {}; "
             "Extra Tank II = {} slots, III / IV = {} / {}", kModule, std::lround(base + capMax), base, capMax,
             std::lround(base + capMax + 25), std::lround(base + capMax + 50), std::lround(base + capMax + 75),
             std::lround(base + capMax + 100), std::lround(slotBase + slotMax), std::lround(slotBase + slotMax + 1),
             std::lround(slotBase + slotMax + 2));
    return true;
}

const Ours* FindOurs(Il2CppObject* def) {  // own cards only
    if (!def || !g.created) return nullptr;
    for (const Ours& o : g.ours)
        if (o.def == def && o.spec && !IsTier(*o.spec)) return &o;
    return nullptr;
}
Ours* FindTier(Il2CppObject* def) {  // tiers whose vanilla parent is `def`
    if (!def || !g.created) return nullptr;
    for (Ours& o : g.ours)
        if (o.spec && IsTier(*o.spec) && o.parent == def) return &o;
    return nullptr;
}

// Insert our own cards into an UpgradeDefinitionList's items (at the end: no vanilla parent).
int InsertOurs(Il2CppObject* items) {
    int added = 0;
    for (const Ours& o : g.ours) {
        if (!o.def || !o.spec || IsTier(*o.spec) || ListIndexOf(items, o.def) >= 0) continue;
        if (ListInsert(items, static_cast<int32_t>(unity::ListCount(items)), o.def)) ++added;
    }
    return added;
}
int RemoveOurs(Il2CppObject* items) {
    int removed = 0;
    for (const Ours& o : g.ours) {
        if (!o.def || !o.spec || IsTier(*o.spec)) continue;
        const int32_t at = ListIndexOf(items, o.def);
        if (at >= 0 && ListRemoveAt(items, at)) ++removed;
    }
    return removed;
}

// ---- tiers: our levels appended to the vanilla definition ----

// `items`: the player's upgrade list (the vanilla asset is looked up by id each time: a reloaded
// asset gets our levels again).
void Extend(Ours& o, Il2CppObject* items) {
    Il2CppObject* parent = FindDefById(items, o.spec->parentId);
    if (!parent) return;
    if (parent != o.parent) {
        o.parent = parent;
        o.extended = false;
    }
    Il2CppObject* list = unity::Field<Il2CppObject*>(parent, g.offUdLevels);
    if (!list) return;
    if (ListIndexOf(list, o.levelObjs[0]) >= 0) {
        o.extended = true;
        return;
    }
    o.extended = false;
    const int have = LevelCount(parent);
    const bool tooMany = have + o.spec->levels > kChainCircles && !g.chainOk;
    if (have != o.vanillaLevels || tooMany) {
        if (!o.loggedSkip)
            SR2_WARN("[{}] vacpack: {} not added to '{}' ({}) - not offered", kModule, o.spec->name, unity::GetName(parent),
                     tooMany ? "the Fabricator panel cannot show more than 8 levels" :
                               std::format("it has {} levels, expected {}", have, o.vanillaLevels));
        o.loggedSkip = true;
        return;
    }
    for (int k = 0; k < o.spec->levels; ++k)
        if (!ListAdd(list, o.levelObjs[k])) return;
    o.extended = true;
    SR2_INFO("[{}] vacpack: '{}' now has {} levels ({} vanilla + {})", kModule, unity::GetName(parent), LevelCount(parent),
             o.vanillaLevels, o.spec->levels);
}

void Unextend(Ours& o) {
    if (!o.parent || !unity::IsAlive(o.parent)) return;
    Il2CppObject* list = unity::Field<Il2CppObject*>(o.parent, g.offUdLevels);
    if (list && ListIndexOf(list, o.levelObjs[0]) == o.vanillaLevels)
        ListRemoveRange(list, o.vanillaLevels, LevelCount(o.parent) - o.vanillaLevels);
    o.extended = false;
}

int VanillaMax(const Ours& o) { return o.vanillaLevels - 1; }

int SavedInt(const char* key, int def = -1) {
    const std::string v = g.host.value && key && *key ? g.host.value(key) : std::string();
    int out = def;
    if (!v.empty()) std::from_chars(v.data(), v.data() + v.size(), out);
    return out;
}

// The owned level of a tier upgrade per the sidecar (absolute, 0-based), -1 = nothing beyond vanilla.
int TierLevel(const Ours& o) {
    const int v = SavedInt(o.spec->key);
    return v > VanillaMax(o) && v < o.vanillaLevels + o.spec->levels ? v : -1;
}

// Own card: its owned level (0-based) per the sidecar.
int DesiredLevel(const Ours& o) { return std::clamp(SavedInt(o.spec->key), -1, o.spec->levels - 1); }

// Older builds sold Tank Booster IX-XII / Extra Tank III-IV as separate cards (own ids, own level).
// Such a level carries over when the vanilla upgrade is maxed in the save; otherwise it was bought
// out of order (the old card could show early) and is dropped.
void MigrateTiers(Il2CppObject* dict) {
    if (g.tiersMigrated) return;
    g.tiersMigrated = true;
    for (Ours& o : g.ours) {
        if (!o.spec || !IsTier(*o.spec) || !*o.spec->oldKey) continue;
        const int old = SavedInt(o.spec->oldKey);
        if (old < 0 || !o.extended) continue;
        const int have = DictGet(dict, o.spec->parentId);
        const int ourLevel = std::clamp(old, 0, o.spec->levels - 1);
        if (SavedInt(o.spec->key) >= 0) {  // the new value wins
            g.host.setValue(o.spec->oldKey, std::string());
        } else if (have == VanillaMax(o)) {
            g.host.setValue(o.spec->oldKey, std::string());
            g.host.setValue(o.spec->key, std::to_string(VanillaMax(o) + 1 + ourLevel));
            SR2_INFO("[{}] vacpack: {} of an older build (separate card) carried over to the vanilla card", kModule,
                     RomanTitle(*o.spec, ourLevel));
        } else {
            // Bought out of order (the old card could show early): kept until the vanilla upgrade is
            // maxed (checked again after every load), not applied before.
            SR2_WARN("[{}] vacpack: {} of an older build not applied yet: the vanilla upgrade is at level {} of {} in this "
                     "save (carried over once it is maxed and the save is loaded again)", kModule,
                     RomanTitle(*o.spec, ourLevel), have + 1, o.vanillaLevels);
        }
    }
}

// The Fabricator shows the Drone Battery Saver card once the Drone Station blueprint is owned
// (unknown = shown) or a level is owned.
bool Unlocked(const Ours& o, Il2CppObject* upgrades) {
    Il2CppObject* dict = upgrades ? unity::Field<Il2CppObject*>(upgrades, g.offUmLevels) : nullptr;
    if (!dict) return false;
    if (DictGet(dict, o.spec->id) >= 0) return true;
    if (o.spec->kind == Kind::kDroneBattery) {
        const int bp = g.host.droneBlueprint ? g.host.droneBlueprint() : -1;
        if (bp < 0 && !g.loggedNoBlueprint) {
            g.loggedNoBlueprint = true;
            SR2_INFO("[{}] vacpack: Drone Station blueprint unknown - the Drone Battery Saver card is shown", kModule);
        }
        return bp != 0;
    }
    return true;
}

// ---- Drone Battery Saver ----

// A field of the object's class or one of its parents (0 = not found).
size_t FieldOff(Il2CppObject* obj, const char* name) {
    const auto& a = api();
    for (Il2CppClass* c = unity::ClassOf(obj); c; c = a.il2cpp_class_get_parent ? a.il2cpp_class_get_parent(c) : nullptr)
        if (auto o = Resolver::Get().FieldOffset(c, name)) return *o;
    return 0;
}

// The card takes the Drone Station gadget's icon and pedia entry (once the scene has its definition).
void DroneArt() {
    if (g.batteryArt || !g.host.droneStation) return;
    const Ours* o = nullptr;
    for (const Ours& x : g.ours)
        if (x.spec && x.spec->kind == Kind::kDroneBattery && x.def) o = &x;
    Il2CppObject* gadget = o ? g.host.droneStation() : nullptr;
    if (!o || !unity::IsAlive(gadget)) return;
    g.batteryArt = true;
    const size_t offIcon = FieldOff(gadget, "icon"), offPedia = FieldOff(gadget, "_pediaLink");
    Il2CppObject* icon = offIcon ? unity::Field<Il2CppObject*>(gadget, offIcon) : nullptr;
    Il2CppObject* pedia = offPedia ? unity::Field<Il2CppObject*>(gadget, offPedia) : nullptr;
    if (unity::IsAlive(icon)) SetRef(o->def, g.offUdIcon, icon);
    if (unity::IsAlive(pedia)) SetRef(o->def, g.offUdPedia, pedia);
    SR2_INFO("[{}] vacpack: Drone Battery Saver card uses the '{}' icon ({}) and pedia entry ({})", kModule,
             unity::GetName(gadget), unity::IsAlive(icon) ? unity::GetName(icon) : std::string("not found, Tank Booster's"),
             unity::IsAlive(pedia) ? unity::GetName(pedia) : std::string("not found, Tank Booster's"));
}

// Old builds sold Battery Saver I-IV per drone station ("drone_battery.station.<id>" = 1..4). The
// highest tier any station owned becomes the global level; the per-station values are erased.
void MigrateBattery() {
    if (g.migrated || !g.host.valuesWithPrefix || !g.host.setValue) return;
    g.migrated = true;
    const auto old = g.host.valuesWithPrefix(kOldBatteryPrefix);
    if (old.empty()) return;
    const Spec* spec = nullptr;
    for (const Spec& sp : kSpecs)
        if (sp.kind == Kind::kDroneBattery) spec = &sp;
    if (!spec) return;
    int best = 0;
    for (const auto& [key, value] : old) {
        int t = 0;
        std::from_chars(value.data(), value.data() + value.size(), t);
        best = std::max(best, std::clamp(t, 0, spec->levels));
        g.host.setValue(key, std::string());
    }
    const int have = SavedInt(spec->key);
    const int want = std::max(have, best - 1);  // 0-based
    if (want != have) g.host.setValue(spec->key, std::to_string(want));
    SR2_INFO("[{}] vacpack: migrated the per-station Battery Saver of an older build: {} station(s), highest tier {} -> "
             "global {} (saved with the next game save; the other stations' tiers are not refunded)",
             kModule, old.size(), best, want >= 0 ? spec->level[want].title : "none");
}

// ---- extra slots ----
// Layout with the module on: vanilla slots before "ExpansionAmmoSlot 2"'s successor, our two, then
// the rest (the water slot): AmmoSlot1-4, Expansion 1-2, Extra Tank III, IV, water.
int ExtIndexOfVanilla(int v) { return v < g.insertAt ? v : v + kExtraSlots; }
int ExtraIndex(int k) { return g.insertAt + k; }
bool IsOurSlotDef(Il2CppObject* def) {
    for (Il2CppObject* d : g.extraSlotDefs)
        if (d && d == def) return true;
    return false;
}
// The player's live slot array is in our layout (our definitions at insertAt..).
bool Extended(Il2CppArray* slots) {
    if (!g.vanillaSlotCount || static_cast<int>(unity::ArrayLength(slots)) != g.vanillaSlotCount + kExtraSlots) return false;
    auto* s = static_cast<Il2CppObject*>(unity::ArrayAt(slots, ExtraIndex(0)));
    return s && unity::Field<Il2CppObject*>(s, g.offSlotDef) == g.extraSlotDefs[0];
}

bool CreateSlotDefs(Il2CppArray* vanilla) {
    Il2CppObject* tpl = nullptr;
    int best = -1, at = -1;
    for (size_t i = 0, n = unity::ArrayLength(vanilla); i < n; ++i) {
        auto* d = static_cast<Il2CppObject*>(unity::ArrayAt(vanilla, i));
        if (!IsA(d, g.slotDefClass) || !unity::Field<Il2CppObject*>(d, g.offSdProp)) continue;
        const int t = unity::Field<int32_t>(d, g.offSdThreshold);
        if (t > best) {
            best = t;
            tpl = d;
            at = static_cast<int>(i);
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
        SetRef(c, g.offSdHotkey, nullptr);  // no input action past Slot7; our own keys (Frame) and label (BindKeyDetour)
        g.extraSlotDefs[k] = c;
    }
    g.insertAt = at + 1;
    SR2_INFO("[{}] vacpack: extra slot definitions ready (copies of '{}', unlocked at {} / {} slots, placed after slot {})",
             kModule, unity::GetName(tpl), best + 1, best + 2, at + 1);
    return true;
}

// PlayerState.InitModel / Reset (pre): the player's slot definitions with ours before the water
// slot, before the game builds the AmmoSlotManager from them.
void ExtendSlots(Il2CppObject* ps) {
    if (!IsA(ps, g.playerStateClass) || !g.arrayNew) return;
    auto* arr = unity::Field<Il2CppArray*>(ps, g.offPsDefs);
    const size_t n = unity::ArrayLength(arr);
    if (!arr || n == 0) return;
    for (size_t i = 0; i < n; ++i)
        if (IsOurSlotDef(static_cast<Il2CppObject*>(unity::ArrayAt(arr, i)))) return;  // already extended
    if (!g.extraSlotDefs[0] && !g.slotDefsFailed && !CreateSlotDefs(arr)) g.slotDefsFailed = true;
    if (g.slotDefsFailed || g.insertAt <= 0 || g.insertAt > static_cast<int>(n)) return;
    Il2CppArray* ext = g.arrayNew(g.slotDefClass, n + kExtraSlots);
    if (!ext) return;
    g.vanillaSlotCount = static_cast<int>(n);
    for (size_t i = 0; i < n; ++i)
        SetArrayRef(ext, static_cast<size_t>(ExtIndexOfVanilla(static_cast<int>(i))), static_cast<Il2CppObject*>(unity::ArrayAt(arr, i)));
    for (int k = 0; k < kExtraSlots; ++k) SetArrayRef(ext, static_cast<size_t>(ExtraIndex(k)), g.extraSlotDefs[k]);
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
    SetRef(ps, g.offPsDefs, reinterpret_cast<Il2CppObject*>(ext));
    SR2_INFO("[{}] vacpack: player slot definitions {} -> {} (extra slots {} / {} before the water slot, locked until "
             "Extra Tank III / IV)", kModule, n, n + kExtraSlots, ExtraIndex(0) + 1, ExtraIndex(1) + 1);
}

std::string RefIdOf(Il2CppObject* type) {
    bool ok = false;
    Il2CppObject* s = Invoke(g.refId, type, {}, "IdentifiableType.get_ReferenceId", &ok);
    return ok ? unity::ToUtf8(reinterpret_cast<Il2CppString*>(s)) : std::string();
}

// "k=refId*count;..." of the player's extra slots.
std::string StashOf(Il2CppArray* slots) {
    std::string out;
    for (int k = 0; k < kExtraSlots; ++k) {
        auto* slot = static_cast<Il2CppObject*>(unity::ArrayAt(slots, ExtraIndex(k)));
        if (!slot) continue;
        Il2CppObject* id = unity::Field<Il2CppObject*>(slot, g.offSlotId);
        const int count = unity::Field<int32_t>(slot, g.offSlotCount);
        if (!id || count <= 0) continue;
        const std::string ref = RefIdOf(id);
        if (ref.empty() || ref.find_first_of(";=*") != std::string::npos) continue;
        out += std::format("{}{}={}*{}", out.empty() ? "" : ";", k, ref, count);
    }
    return out;
}

void RestoreStash(const Player& p) {
    auto* slots = unity::Field<Il2CppArray*>(p.ammoModel, g.offAmodelSlots);
    if (!Extended(slots)) return;  // not extended (yet)
    g.restoredAmmo = p.ammoModel;
    std::string stash = g.host.value ? g.host.value(kStashKey) : std::string();
    bool old = false;
    if (stash.empty() && g.host.value) {  // an older build's value: slots after the water slot
        stash = g.host.value(kOldStashKey);
        old = !stash.empty();
    }
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
        int k = -1, count = 0;
        if (eq == std::string_view::npos || star == std::string_view::npos || star < eq) {
            ++skipped;
            continue;
        }
        std::from_chars(e.data(), e.data() + eq, k);
        if (old) k -= g.vanillaSlotCount;  // old index 7 / 8 -> 0 / 1
        std::from_chars(e.data() + star + 1, e.data() + e.size(), count);
        const std::string ref(e.substr(eq + 1, star - eq - 1));
        if (k < 0 || k >= kExtraSlots || count <= 0 || !lookup) {
            ++skipped;
            continue;
        }
        const int idx = ExtraIndex(k);
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
    if (old) {  // carried over: the next save writes the new value
        g.host.setValue(kStashKey, stash.empty() ? std::string() : StashOf(slots));
        g.host.setValue(kOldStashKey, std::string());
    }
    SR2_INFO("[{}] vacpack: extra slots refilled from the sidecar{} ({} put back, {} skipped): {}", kModule,
             old ? " (older build's value)" : "", put, skipped, stash);
}

// Module off / faulted: the live slots back in the vanilla order with ours at the end, so a save
// made without our hooks keeps the water slot where vanilla expects it (ours are then dropped by
// a mod-free load, their contents stay in the sidecar).
void VanillaOrder(const Player& p) {
    auto* slots = p.ammoModel ? unity::Field<Il2CppArray*>(p.ammoModel, g.offAmodelSlots) : nullptr;
    if (!Extended(slots) || !g.arrayNew) return;
    const int n = g.vanillaSlotCount + kExtraSlots;
    Il2CppArray* re = g.arrayNew(g.ammoSlotClass, static_cast<uintptr_t>(n));
    if (!re) return;
    for (int v = 0; v < g.vanillaSlotCount; ++v)
        SetArrayRef(re, static_cast<size_t>(v), static_cast<Il2CppObject*>(unity::ArrayAt(slots, ExtIndexOfVanilla(v))));
    for (int k = 0; k < kExtraSlots; ++k)
        SetArrayRef(re, static_cast<size_t>(g.vanillaSlotCount + k), static_cast<Il2CppObject*>(unity::ArrayAt(slots, ExtraIndex(k))));
    SetRef(p.ammoModel, g.offAmodelSlots, reinterpret_cast<Il2CppObject*>(re));
    if (g.offAmSelected) {
        int32_t& sel = unity::Field<int32_t>(p.ammo, g.offAmSelected);
        if (sel >= ExtraIndex(0) && sel < ExtraIndex(0) + kExtraSlots) sel = 0;
        else if (sel >= ExtraIndex(0) + kExtraSlots) sel -= kExtraSlots;
    }
    SR2_INFO("[{}] vacpack: vacpack slots back in the vanilla order (water slot {}), extra slots last", kModule,
             g.vanillaSlotCount);
}

// ---- HUD row ----
Il2CppObject* HudRow() {
    Il2CppObject* ui = g.hudHandle ? unity::GCHandleTarget(g.hudHandle) : nullptr;
    if (unity::IsAlive(ui)) return ui;
    if (g.hudHandle) {
        unity::FreeGCHandle(g.hudHandle);
        g.hudHandle = nullptr;
        g.hudWidth = 0.0f;  // a new HUD (scene load): its own width
    }
    if (g.hudFindTimer < kHudFindSeconds || !g.ammoUiClass) return nullptr;
    g.hudFindTimer = 0.0f;
    ui = unity::FindObjectOfType(g.ammoUiClass);
    if (!unity::IsAlive(ui)) return nullptr;
    g.hudHandle = unity::NewGCHandle(ui);
    return ui;
}

// Wide enough that the HorizontalLayoutGroup can centre 9 slots (it centres only while they fit).
void WidenHud() {
    if (!g.rtGetSize || !g.rtSetSize) return;
    Il2CppObject* ui = HudRow();
    Il2CppObject* rt = ui ? unity::TransformOf(ui) : nullptr;
    if (!rt) return;
    bool ok = false;
    Vec2 size = Unbox<Vec2>(Invoke(g.rtGetSize, rt, {}, "RectTransform.get_sizeDelta", &ok));
    if (!ok) return;
    const float base = g.hudWidth > 0.0f ? g.hudWidth : size.x;
    const float want = base + kExtraSlots * kHudSlotPitch + kHudMargin;
    if (size.x >= want - 0.5f) return;
    if (g.hudWidth <= 0.0f) g.hudWidth = size.x;
    Vec2 v{want, size.y};
    Invoke(g.rtSetSize, rt, {&v}, "RectTransform.set_sizeDelta", &ok);
    if (ok) SR2_INFO("[{}] vacpack: HUD slot row {} -> {} px wide, so it stays centred with the extra slots", kModule,
                     std::lround(size.x), std::lround(want));
}

void RestoreHud() {
    Il2CppObject* ui = g.hudHandle ? unity::GCHandleTarget(g.hudHandle) : nullptr;
    Il2CppObject* rt = unity::IsAlive(ui) ? unity::TransformOf(ui) : nullptr;
    if (rt && g.hudWidth > 0.0f && g.rtGetSize && g.rtSetSize) {
        Vec2 size = Unbox<Vec2>(Invoke(g.rtGetSize, rt, {}, "RectTransform.get_sizeDelta"));
        Vec2 v{g.hudWidth, size.y};
        Invoke(g.rtSetSize, rt, {&v}, "RectTransform.set_sizeDelta");
    }
    g.hudWidth = 0.0f;
}

// ---- number keys ----
int SlotKey(int k) {
    const int vk = settings::GetInt(kModule, kSlotKeySetting[k]);
    return vk > 0 && vk < 256 ? vk : 0;
}

bool GameplayInput() {
    bool ok = false;
    const int lock = Unbox<int32_t>(Invoke(g.lockState, nullptr, {}, "Cursor.get_lockState", &ok), -1);
    if (!ok || lock != 1) return false;  // Locked = playing; menus free the cursor
    const float ts = Unbox<float>(Invoke(g.timeScale, nullptr, {}, "Time.get_timeScale", &ok), 0.0f);
    return ok && ts > 0.0f;
}

Il2CppObject* Vacuum() {
    Il2CppObject* v = g.vacHandle ? unity::GCHandleTarget(g.vacHandle) : nullptr;
    if (unity::IsAlive(v)) return v;
    if (g.vacHandle) {
        unity::FreeGCHandle(g.vacHandle);
        g.vacHandle = nullptr;
    }
    v = g.vacItemClass ? unity::FindObjectOfType(g.vacItemClass) : nullptr;  // on a key press only
    if (!unity::IsAlive(v)) return nullptr;
    g.vacHandle = unity::NewGCHandle(v);
    return v;
}

void SelectExtra(int k) {
    Player p;
    if (!GetPlayer(p) || !Extended(unity::Field<Il2CppArray*>(p.ammoModel, g.offAmodelSlots))) return;
    Il2CppObject* vac = Vacuum();
    bool ok = false;
    if (!vac || !Unbox<bool>(Invoke(g.activeEnabled, vac, {}, "Behaviour.get_isActiveAndEnabled", &ok)) || !ok)
        return;  // gadget mode / no vacpack
    int32_t idx = ExtraIndex(k);
    Invoke(g.selectSlotInt, vac, {&idx}, "VacuumItem.SelectSlot");
    if (!g.loggedKeys) {
        g.loggedKeys = true;
        SR2_INFO("[{}] vacpack: number key {} selects vacpack slot {} (Extra Tank {})", kModule,
                 sr2::ui::KeyName(SlotKey(k)), idx + 1, k == 0 ? "III" : "IV");
    }
}

// ---------------------------------------------------------------- detours --

// Every level change of every upgrade. Not our own restore: an own card's purchase goes to the
// sidecar; a tier upgrade's level past its vanilla max too (the save only gets the vanilla max).
void SetLevelDetour(Il2CppObject* self, Il2CppObject* def, int32_t level, const MethodInfo* mi) {
    g.origSetLevel(self, def, level, mi);
    if (g.applying || !def || !On()) return;
    g_levelGuard.Run([&] {
        if (!main_thread::IsMainThread() || !g.host.saveReady || !g.host.saveReady()) return;
        if (const Ours* o = FindOurs(def)) {
            g.host.setValue(o->spec->key, level >= 0 ? std::to_string(level) : std::string());
            SR2_INFO("[{}] vacpack: {} -> level {} ({}), saved with the next game save", kModule, o->spec->name, level + 1,
                     RomanTitle(*o->spec, level));
            return;
        }
        Ours* t = FindTier(def);
        if (!t || !t->extended) return;
        const int vmax = VanillaMax(*t);
        if (level > vmax) {
            g.host.setValue(t->spec->key, std::to_string(level));
            SR2_INFO("[{}] vacpack: '{}' -> {} (level {}), kept in the sidecar (the save gets level {})", kModule,
                     unity::GetName(def), RomanTitle(*t->spec, level - vmax - 1), level + 1, vmax + 1);
        } else if (level < vmax && SavedInt(t->spec->key) >= 0) {
            g.host.setValue(t->spec->key, std::string());  // reset below the vanilla max: nothing of ours owned
        }
    });
}

// TinkerTailorSlimes counts the upgrades with a level >= 0: our own cards never count.
int32_t CountDetour(Il2CppObject* self, const MethodInfo* mi) {
    int32_t r = g.origCount(self, mi);
    if (!g.created || !self) return r;
    g_countGuard.Run([&] {
        Il2CppObject* dict = unity::Field<Il2CppObject*>(self, g.offUmLevels);
        for (const Ours& o : g.ours)
            if (o.def && o.spec && !IsTier(*o.spec) && DictGet(dict, o.spec->id) >= 0) --r;
    });
    return std::max(r, 0);
}

// The save's copy of the upgrade levels: our own ids out, tier upgrades at most at their vanilla
// max (the rest lives in the sidecar). A mod-free load sees a valid vanilla dictionary.
void PullDictDetour(Il2CppObject* self, Il2CppObject** saved, const MethodInfo* mi) {
    g.origPullDict(self, saved, mi);
    if (!g.created || !saved || !*saved) return;
    g_pullGuard.Run([&] {
        for (const Ours& o : g.ours) {
            if (!o.def || !o.spec) continue;
            if (!IsTier(*o.spec)) {
                DictRemove(*saved, o.spec->id);
                continue;
            }
            const int v = DictGet(*saved, o.spec->parentId);
            if (o.vanillaLevels > 0 && v > VanillaMax(o)) DictSet(*saved, o.spec->parentId, VanillaMax(o));
        }
    });
}

// Fabricator upgrade category (each time the menu opens): our own cards in its list (pre), out of
// the result while not unlocked (post). Tier levels need nothing here: they are the vanilla card's.
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

// The player's ammo for the save: the vanilla slots in the vanilla order (the water slot back at
// its index), the extra slots' contents to the sidecar. A mod-free load sees an untouched vanilla
// vacpack.
void PullAmmoDetour(Il2CppObject* self, Il2CppArray** slots, const MethodInfo* mi) {
    g.origPullAmmo(self, slots, mi);
    if (!slots || !*slots || !g.vanillaSlotCount || !On() || !g.arrayNew) return;
    g_ammoGuard.Run([&] {
        // The body is shared with TrackedActorsModel.Pull (out HashSet): the owner check comes first.
        Player p;
        if (!GetPlayer(p) || p.ammoModel != self) return;  // a silo, a drone, TrackedActorsModel...
        Il2CppArray* arr = *slots;
        if (!Extended(arr)) return;
        if (self == g.restoredAmmo) {
            const std::string stash = StashOf(arr);
            g.host.setValue(kStashKey, stash);
            if (!stash.empty()) SR2_INFO("[{}] vacpack: extra slots kept in the sidecar: {}", kModule, stash);
        } else {
            SR2_WARN("[{}] vacpack: saved before the extra slots were refilled - their sidecar contents are kept", kModule);
        }
        Il2CppArray* cut = g.arrayNew(g.ammoSlotClass, static_cast<uintptr_t>(g.vanillaSlotCount));
        if (!cut) return;
        for (int v = 0; v < g.vanillaSlotCount; ++v)
            SetArrayRef(cut, static_cast<size_t>(v), static_cast<Il2CppObject*>(unity::ArrayAt(arr, ExtIndexOfVanilla(v))));
        void** out = reinterpret_cast<void**>(slots);
        if (g.wbarrier) g.wbarrier(nullptr, out, cut);
        else *out = cut;
    });
}

// A load copies the saved slots by index into the player's slots (AmmoModel.Push): a vanilla save
// (7 slots, water last) is spread into our layout first, so the water stays in the water slot and
// the extra slots stay empty (refilled from the sidecar by the tick).
void PushAmmoDetour(Il2CppObject* self, Il2CppArray* saved, const MethodInfo* mi) {
    Il2CppArray* use = saved;
    if (saved && self && g.vanillaSlotCount && g.arrayNew && On())
        g_ammoGuard.Run([&] {
            auto* live = unity::Field<Il2CppArray*>(self, g.offAmodelSlots);
            if (!Extended(live) || static_cast<int>(unity::ArrayLength(saved)) != g.vanillaSlotCount) return;
            const int n = g.vanillaSlotCount + kExtraSlots;
            Il2CppArray* spread = g.arrayNew(g.ammoSlotClass, static_cast<uintptr_t>(n));
            if (!spread) return;
            for (int v = 0; v < g.vanillaSlotCount; ++v)
                SetArrayRef(spread, static_cast<size_t>(ExtIndexOfVanilla(v)), static_cast<Il2CppObject*>(unity::ArrayAt(saved, v)));
            // Our slots get their own (empty) live slot: Push copies it onto itself.
            for (int k = 0; k < kExtraSlots; ++k)
                SetArrayRef(spread, static_cast<size_t>(ExtraIndex(k)), static_cast<Il2CppObject*>(unity::ArrayAt(live, ExtraIndex(k))));
            for (int i = 0; i < n; ++i)
                if (!unity::ArrayAt(spread, static_cast<size_t>(i))) return;  // Push would throw on a null slot
            use = spread;
            if (!g.loggedRemap) {
                g.loggedRemap = true;
                SR2_INFO("[{}] vacpack: loaded vacpack spread into the extended layout (saved slot {} = water -> slot {})",
                         kModule, g.vanillaSlotCount, ExtIndexOfVanilla(g.vanillaSlotCount - 1) + 1);
            }
        });
    g.origPushAmmo(self, use, mi);
}

// The HUD's key label of a slot (AmmoSlotViewHolder.BindInputKey(definition.Hotkey)). Ours have no
// input action: our own key's name instead of the game's "No binding set for ammo slot!" warning.
void BindKeyDetour(Il2CppObject* self, Il2CppObject* actionRef, const MethodInfo* mi) {
    bool handled = false;
    if (self && !actionRef && g.offVhData && On())
        g_keyGuard.Run([&] {
            Il2CppObject* slot = unity::Field<Il2CppObject*>(self, g.offVhData);
            Il2CppObject* def = slot ? unity::Field<Il2CppObject*>(slot, g.offSlotDef) : nullptr;
            int k = -1;
            for (int i = 0; i < kExtraSlots; ++i)
                if (def && def == g.extraSlotDefs[i]) k = i;
            if (k < 0) return;
            handled = true;
            Il2CppObject* box = unity::Field<Il2CppObject*>(self, g.offVhKeyBox);
            Il2CppObject* text = unity::Field<Il2CppObject*>(self, g.offVhKeyText);
            Il2CppObject* loc = unity::Field<Il2CppObject*>(self, g.offVhKeyLoc);
            const int vk = SlotKey(k);
            if (unity::IsAlive(box)) unity::SetActive(box, vk != 0);
            if (!vk) return;
            bool off = false;
            if (unity::IsAlive(loc)) Invoke(g.setEnabled, loc, {&off}, "Behaviour.set_enabled");
            if (unity::IsAlive(text)) {
                const std::string want = sr2::ui::KeyName(vk);
                bool ok = false;
                Il2CppObject* cur = Invoke(g.tmpGetText, text, {}, "TMP_Text.get_text", &ok);
                if (!ok || unity::ToUtf8(reinterpret_cast<Il2CppString*>(cur)) != want)
                    Invoke(g.tmpSetText, text, {unity::NewString(want)}, "TMP_Text.set_text");
            }
        });
    if (!handled) g.origBindKey(self, actionRef, mi);
}

// The game's slot keys (VacuumItem.SelectSlot(InputEventData), value = vanilla slot index): a
// vanilla index after our slots (the water slot, Slot7 = R by default) selects its slot in our layout.
void SelectSlotDetour(Il2CppObject* self, void* data, const MethodInfo* mi) {
    bool handled = false;
    if (self && data && g.vanillaSlotCount && g.insertAt > 0 && On())
        g_selectGuard.Run([&] {
            const float value = *reinterpret_cast<const float*>(static_cast<const char*>(data) + 4);
            if (!std::isfinite(value)) return;
            const int v = static_cast<int>(std::lround(value));
            if (v < g.insertAt || v >= g.vanillaSlotCount) return;
            Player p;
            if (!GetPlayer(p) || !Extended(unity::Field<Il2CppArray*>(p.ammoModel, g.offAmodelSlots))) return;
            int32_t idx = ExtIndexOfVanilla(v);
            bool ok = false;
            Invoke(g.selectSlotInt, self, {&idx}, "VacuumItem.SelectSlot", &ok);
            handled = ok;
        });
    if (!handled) g.origSelect(self, data, mi);
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
        DroneArt();
        MigrateBattery();  // before the saved levels go onto the model
        // Own cards: the model knows our ids before the list does (ActorUpgradeHandler.
        // SetModifiersFromModel asks GetUpgradeLevel for every listed definition; a missing key
        // would throw).
        for (const Ours& o : g.ours)
            if (o.spec && !IsTier(*o.spec) && !DictHas(dict, o.spec->id)) DictSet(dict, o.spec->id, -1);
        if (const int added = InsertOurs(items)) {
            if (g.modelList != items) {
                unity::NewGCHandle(items);
                g.modelList = items;
            }
            SR2_INFO("[{}] vacpack: {} upgrade(s) added to the player's upgrade list", kModule, added);
        }
        // Tiers: our levels on the vanilla definitions, then older builds' cards carried over.
        for (Ours& o : g.ours)
            if (o.spec && IsTier(*o.spec)) Extend(o, items);
        MigrateTiers(dict);
        // Saved levels -> model (after a load, a re-enable or a sidecar change).
        for (Ours& o : g.ours) {
            if (!o.spec) continue;
            if (!IsTier(*o.spec)) {
                int32_t want = DesiredLevel(o);
                const int32_t have = DictGet(dict, o.spec->id);
                if (want == have) continue;
                g.applying = true;
                bool ok = false;
                Invoke(g.setLevel, p.upgrades, {o.def, &want}, "UpgradeModel.SetUpgradeLevel", &ok);
                g.applying = false;
                SR2_INFO("[{}] vacpack: {} level {} -> {} (from the sidecar){}", kModule, o.spec->name, have + 1, want + 1,
                         ok ? "" : " FAILED");
                continue;
            }
            if (!o.extended) continue;
            // The save holds the vanilla max; the sidecar the real level. Only on top of a maxed
            // vanilla upgrade (anything else: the save and the sidecar disagree, the save wins).
            int32_t want = TierLevel(o);
            const int32_t have = DictGet(dict, o.spec->parentId);
            if (want < 0 || have != VanillaMax(o) || want >= LevelCount(o.parent)) continue;
            g.applying = true;
            bool ok = false;
            Invoke(g.setLevel, p.upgrades, {o.parent, &want}, "UpgradeModel.SetUpgradeLevel", &ok);
            g.applying = false;
            SR2_INFO("[{}] vacpack: '{}' level {} -> {} ({}, from the sidecar){}", kModule, unity::GetName(o.parent),
                     have + 1, want + 1, RomanTitle(*o.spec, want - VanillaMax(o) - 1), ok ? "" : " FAILED");
        }
        // Extra slots' contents, once per player ammo model; the HUD row wide enough to centre them.
        auto* live = p.ammoModel ? unity::Field<Il2CppArray*>(p.ammoModel, g.offAmodelSlots) : nullptr;
        if (p.ammoModel && p.ammoModel != g.restoredAmmo) RestoreStash(p);
        g.hudFindTimer += 1.0f;  // ~1 Hz
        if (Extended(live)) WidenHud();
        int slotsLevel = -1;
        for (const Ours& o : g.ours)
            if (o.spec && o.spec->kind == Kind::kSlots && o.extended) slotsLevel = DictGet(dict, o.spec->parentId) - VanillaMax(o);
        if (!Extended(live) && !g.loggedNoSlots && slotsLevel > 0) {
            g.loggedNoSlots = true;  // custom upgrades were switched on after this save was loaded
            SR2_INFO("[{}] vacpack: Extra Tank III/IV owned but the player was built without the extra slots - they "
                     "appear after loading the save again", kModule);
        }
    });
}

void VacpackUpgrades::Frame() {
    if (!ready_ || !On() || !g.vanillaSlotCount || !main_thread::IsMainThread()) return;
    g_tickGuard.Run([&] {
        for (int k = 0; k < kExtraSlots; ++k) {
            const int vk = SlotKey(k);
            const bool down = vk && (GetAsyncKeyState(vk) & 0x8000) != 0;
            const bool edge = down && !g.keyDown[k];
            g.keyDown[k] = down;
            if (!edge || sr2::ui::overlay::IsOpen()) continue;
            DWORD pid = 0;
            GetWindowThreadProcessId(GetForegroundWindow(), &pid);
            if (pid != GetCurrentProcessId() || !GameplayInput()) continue;
            SelectExtra(k);
        }
    });
}

void VacpackUpgrades::Cleanup(const char* why) {
    if (!ready_ || !main_thread::IsMainThread()) return;
    g_tickGuard.Run([&] {
        Player p;
        const bool havePlayer = GetPlayer(p);
        Il2CppObject* dict = havePlayer ? unity::Field<Il2CppObject*>(p.upgrades, g.offUmLevels) : nullptr;
        int lowered = 0;
        if (g.created && havePlayer) {
            for (Ours& o : g.ours) {
                if (!o.spec || !dict) continue;
                if (IsTier(*o.spec)) {
                    // Levels past the vanilla max off first (SetUpgradeLevel needs level < LevelCount).
                    if (o.extended && DictGet(dict, o.spec->parentId) > VanillaMax(o)) {
                        int32_t vmax = VanillaMax(o);
                        g.applying = true;
                        Invoke(g.setLevel, p.upgrades, {o.parent, &vmax}, "UpgradeModel.SetUpgradeLevel");
                        g.applying = false;
                        ++lowered;
                    }
                    continue;
                }
                if (!DictHas(dict, o.spec->id)) continue;
                if (DictGet(dict, o.spec->id) >= 0) {
                    int32_t none = -1;
                    g.applying = true;
                    Invoke(g.setLevel, p.upgrades, {o.def, &none}, "UpgradeModel.SetUpgradeLevel");
                    g.applying = false;
                }
                DictRemove(dict, o.spec->id);
            }
        }
        int removed = 0, unextended = 0;
        if (g.created) {
            if (Il2CppObject* items = havePlayer ? AssetItems(unity::Field<Il2CppObject*>(p.upgrades, g.offUmDefs)) : nullptr)
                removed += RemoveOurs(items);
            if (g.modelList) removed += RemoveOurs(g.modelList);
            for (Il2CppObject* l : g.categoryLists) removed += RemoveOurs(l);
            for (Ours& o : g.ours)
                if (o.spec && IsTier(*o.spec) && o.extended) {
                    Unextend(o);
                    ++unextended;
                }
            g.tiersMigrated = false;
        }
        bool slotsBack = false;
        if (havePlayer && g.extendedSlotArray && unity::Field<Il2CppObject*>(p.state, g.offPsDefs) == g.extendedSlotArray) {
            SetRef(p.state, g.offPsDefs, g.vanillaSlotArray);
            slotsBack = true;
        }
        if (havePlayer) VanillaOrder(p);
        RestoreHud();
        if (removed || slotsBack || unextended)
            SR2_INFO("[{}] {}: vacpack upgrades off ({} list entr{} removed, {} vanilla upgrade(s) back to their own levels{}"
                     "{})", kModule, why, removed, removed == 1 ? "y" : "ies", unextended,
                     lowered ? std::format(", {} lowered to their vanilla max", lowered) : std::string(),
                     slotsBack ? "; extra slots stay until the next load, locked" : "");
    });
}

void VacpackUpgrades::Reset() {
    g.restoredAmmo = nullptr;
    g.migrated = false;
    g.tiersMigrated = false;
    g.loggedRemap = false;
}

int VacpackUpgrades::DroneBatteryLevel() const {
    if (!ready_ || !On() || !g.host.saveReady || !g.host.saveReady() || !g.host.value) return 0;
    return std::clamp(SavedInt(kDroneBatteryKey) + 1, 0, 4);
}

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
    off(g.ammoSlotClass, "Definition", g.offSlotDef);
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
    if (auto o = R.FieldOffset(g.upgradeDefClass, "_requireComponentToBeVisibleToPlayer")) g.offUdNeedsComponent = *o;
    if (auto o = R.FieldOffset(g.ammoMgrClass, "_selectedAmmoIdx")) g.offAmSelected = *o;

    g.tryGetPlayer = method(scu, "TryGetPlayerState", 1);
    g.getUpgradeModel = method(g.playerModelClass, "GetUpgradeModel", 0);
    g.setLevel = method(upModel, "SetUpgradeLevel", 2);
    const MethodInfo* countMi = method(upModel, "CountUniqueUpgrades", 0);
    const MethodInfo* pullMi = method(upModel, "Pull", 1);
    const MethodInfo* itemsMi = method(category, "GetItems", 1);
    const MethodInfo* initMi = method(g.playerStateClass, "InitModel", 1);
    const MethodInfo* resetMi = method(g.playerStateClass, "Reset", 1);
    const MethodInfo* ammoPullMi = method(ammoModel, "Pull", 1);
    const MethodInfo* ammoPushMi = method(ammoModel, "Push", 1);
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

    // The Fabricator's level circles (needed for Tank Booster's 12 levels; Extend checks chainOk).
    {
        std::string ferr, freport;
        fabricator::FabricatorLevels levels;
        if (levels.Setup(ctx, kModule, g.chainOk, ferr, freport)) report += freport;
        else SR2_WARN("[{}] vacpack: Fabricator level circles not extended ({}) - Tank Booster IX-XII not offered", kModule,
                      ferr);
    }

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
        {"AmmoModel.Push", ammoPushMi, reinterpret_cast<void*>(&PushAmmoDetour), reinterpret_cast<void**>(&g.origPushAmmo)},
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

    // Optional: the extra slots' number keys and HUD key labels, the HUD row's width.
    {
        std::string err;
        Il2CppClass* holder = R.Class(kImage, "MonomiPark.SlimeRancher.UI", "AmmoSlotViewHolder", &err);
        Il2CppClass* tmp = R.Class("Unity.TextMeshPro", "TMPro", "TMP_Text", &err);
        Il2CppClass* behaviour = R.Class("UnityEngine.CoreModule", "UnityEngine", "Behaviour", &err);
        Il2CppClass* rect = R.Class("UnityEngine.CoreModule", "UnityEngine", "RectTransform", &err);
        Il2CppClass* cursor = R.Class("UnityEngine.CoreModule", "UnityEngine", "Cursor", &err);
        Il2CppClass* time = R.Class("UnityEngine.CoreModule", "UnityEngine", "Time", &err);
        g.vacItemClass = R.Class(kImage, "MonomiPark.SlimeRancher.Player.PlayerItems", "VacuumItem", &err);
        g.ammoUiClass = R.Class(kImage, "MonomiPark.SlimeRancher.UI", "AmmoSlotUI", &err);
        if (rect) {
            g.rtGetSize = R.Method(rect, "get_sizeDelta", 0, &err);
            g.rtSetSize = R.Method(rect, "set_sizeDelta", 1, &err);
        }
        if (behaviour) {
            g.setEnabled = R.Method(behaviour, "set_enabled", 1, &err);
            g.activeEnabled = R.Method(behaviour, "get_isActiveAndEnabled", 0, &err);
        }
        if (tmp) {
            g.tmpSetText = R.Method(tmp, "set_text", 1, &err);
            g.tmpGetText = R.Method(tmp, "get_text", 0, &err);
        }
        if (cursor) g.lockState = R.Method(cursor, "get_lockState", 0, &err);
        if (time) g.timeScale = R.Method(time, "get_timeScale", 0, &err);
        if (g.vacItemClass) g.selectSlotInt = R.MethodByParams(g.vacItemClass, "SelectSlot", {"System.Int32"}, &err);
        // Label: AmmoSlotViewHolder.BindInputKey(InputActionReference).
        if (holder) {
            auto o = [&](const char* f, size_t& out) {
                if (auto x = R.FieldOffset(holder, f, &err)) out = *x;
            };
            o("_data", g.offVhData);
            o("KeybindingText", g.offVhKeyText);
            o("KeybindingLocalizedString", g.offVhKeyLoc);
            o("_keyBindingContainer", g.offVhKeyBox);
        }
        const MethodInfo* bindMi = holder ? R.Method(holder, "BindInputKey", 1, &err) : nullptr;
        void* bindP = bindMi && g.offVhData && g.offVhKeyText && g.offVhKeyLoc && g.offVhKeyBox && g.tmpSetText &&
                              g.tmpGetText && g.setEnabled
                          ? R.MethodPointer(bindMi, &err)
                          : nullptr;
        if (bindP && ctx.HookAddress("AmmoSlotViewHolder.BindInputKey", bindP, reinterpret_cast<void*>(&BindKeyDetour),
                                     reinterpret_cast<void**>(&g.origBindKey)))
            report += std::format(" AmmoSlotViewHolder.BindInputKey@0x{:X}", Resolver::Rva(bindP));
        else
            SR2_WARN("[{}] vacpack: extra slots get no key label on the HUD ({})", kModule, err);
        // The water slot's key: VacuumItem.SelectSlot(InputEventData).
        const MethodInfo* selMi = g.vacItemClass && g.selectSlotInt
                                      ? R.MethodByParams(g.vacItemClass, "SelectSlot",
                                                         {"MonomiPark.SlimeRancher.Input.InputEventData"}, &err)
                                      : nullptr;
        void* selP = selMi ? R.MethodPointer(selMi, &err) : nullptr;
        if (selP && ctx.HookAddress("VacuumItem.SelectSlot", selP, reinterpret_cast<void*>(&SelectSlotDetour),
                                    reinterpret_cast<void**>(&g.origSelect)))
            report += std::format(" VacuumItem.SelectSlot@0x{:X}", Resolver::Rva(selP));
        else
            SR2_WARN("[{}] vacpack: the water slot's key is not remapped ({}) - it selects Extra Tank III's slot",
                     kModule, err);
        if (!g.selectSlotInt || !g.activeEnabled || !g.lockState || !g.timeScale || !g.vacItemClass)
            SR2_WARN("[{}] vacpack: extra slot number keys unavailable ({})", kModule, err);
        if (!g.rtGetSize || !g.ammoUiClass)
            SR2_WARN("[{}] vacpack: HUD slot row not widened ({}) - 9 slots sit off centre", kModule, err);
    }
    for (int k = 0; k < kExtraSlots; ++k) {
        settings::Def d;
        d.module = kModule;
        d.key = kSlotKeySetting[k];
        d.label = k == 0 ? "Extra Tank III slot key" : "Extra Tank IV slot key";
        d.group = "Vacpack";
        d.kind = settings::Kind::Key;
        d.def = kSlotKeyDefault[k];
        d.min = 0;
        d.max = 255;
        d.step = 1;
        d.tooltip = std::format("Selects the Extra Tank {} slot. The game binds slots 1-6 to the keys 1-6 and the "
                                "water slot to R, so 7 and 8 are free.",
                                k == 0 ? "III" : "IV");
        settings::Register(d);
    }
    // One-time: the first builds defaulted to 8 / 9 (thinking the game's key 7 was the water slot's;
    // it is R). A key still on its old default moves to the new one.
    if (settings::GetString(kModule, kSlotKeyMigrated).empty()) {
        std::string moved;
        for (int k = 0; k < kExtraSlots; ++k)
            if (settings::Saved(kModule, kSlotKeySetting[k]) && settings::GetInt(kModule, kSlotKeySetting[k]) == kSlotKeyOld[k]) {
                settings::Set(kModule, kSlotKeySetting[k], kSlotKeyDefault[k]);
                moved += std::format("{}{} {} -> {}", moved.empty() ? "" : ", ", k == 0 ? "Extra Tank III" : "Extra Tank IV",
                                     static_cast<char>(kSlotKeyOld[k]), static_cast<char>(kSlotKeyDefault[k]));
            }
        settings::SetString(kModule, kSlotKeyMigrated, "1");
        if (!moved.empty()) SR2_INFO("[{}] vacpack: slot keys moved to the new defaults: {}", kModule, moved);
    }
    g.ready = ready_ = true;
    return true;
}

}  // namespace sr2::modules::custom_upgrades::vacpack
