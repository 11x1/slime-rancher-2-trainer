// sandbox: edit the character's resources from the overlay (Misc > Sandbox):
// Newbucks, health / energy / radiation, the vacpack's ammo slots, spawning
// items in front of the player and crafting fabricator gadgets at no cost.
// Default OFF; the overlay only turns it on after a confirmation dialog.
// Design and the game methods used: modules/sandbox/NOTES.md.
//
// Save safety: every change goes through the game's own methods (the same ones
// the game uses for pickups, purchases and vacuuming), with their validation,
// and values are clamped to what a vanilla game can hold. Nothing is written
// into the save directly; the game saves the result the next time it saves.
//
// Achievements are paused while the module is enabled (achievements.cpp: four
// hooks, switched on and off with the module).
//
// Threading: no hooks besides the achievement pause. A main-thread frame callback copies a Snapshot for the
// tab (4 Hz while it is open) and tops up energy while "infinite energy" is on;
// the tab's requests arrive as posted main-thread jobs (ui/sandbox_bridge.h).
// Every game call goes through Resolver::Invoke (runtime_invoke, managed
// exceptions captured); a native fault there marks the module faulted and it
// stops calling into the game.

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <format>
#include <initializer_list>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "core/detour_guard.h"
#include "core/il2cpp_api.h"
#include "core/log.h"
#include "core/main_thread.h"
#include "core/module.h"
#include "core/unity.h"
#include "modules/sandbox/achievements.h"
#include "ui/item_icons.h"
#include "ui/sandbox_bridge.h"

namespace sr2::modules {
namespace {

namespace unity = sr2::core::unity;
namespace main_thread = sr2::core::main_thread;
namespace bridge = sr2::ui::sandbox;
using sr2::il2cpp::api;
using sr2::il2cpp::Resolver;

constexpr const char* kModule = "sandbox";
constexpr const char* kImage = "Assembly-CSharp";
constexpr int kNewbuckPersistenceId = 1;  // CurrencyUtility.NEWBUCK_PERSISTENCE_ID
constexpr unsigned long long kSnapshotMs = 250;
constexpr unsigned long long kEnergyMs = 100;
constexpr int kMaxSlots = 32;  // slot mask width
constexpr unsigned long long kRecipeStateMs = 2000;  // blueprint / owned counts in the crafter
constexpr int kMaxSpawn = 20;   // per click
constexpr int kMaxCraft = 99;   // per click
constexpr float kSpawnDist = 2.5f;  // metres in front of the camera

struct Vec3 {
    float x, y, z;
};
struct Quat {
    float x, y, z, w;
};

bool IsA(Il2CppObject* obj, Il2CppClass* k) {
    const auto& a = api();
    if (!obj || !k || !a.il2cpp_object_get_class) return false;
    Il2CppClass* c = a.il2cpp_object_get_class(obj);
    if (c == k) return true;
    return c && a.il2cpp_class_is_assignable_from && a.il2cpp_class_is_assignable_from(k, c);
}

// Value of a boxed value type returned by runtime_invoke (data after the object header).
template <class T>
bool Unbox(Il2CppObject* boxed, T& out) {
    if (!boxed) return false;
    out = *reinterpret_cast<T*>(reinterpret_cast<char*>(boxed) + 2 * sizeof(void*));
    return true;
}

std::string Thousands(int64_t v) {
    std::string s = std::to_string(v < 0 ? -v : v);
    for (int i = static_cast<int>(s.size()) - 3; i > 0; i -= 3) s.insert(static_cast<size_t>(i), ",");
    return v < 0 ? "-" + s : s;
}

std::string CleanText(const std::string& s) {
    std::string out;
    bool tag = false;
    for (char c : s) {
        if (c == '<') tag = true;
        else if (c == '>' && tag) tag = false;
        else if (!tag && c != '\r' && c != '\n') out += c;
    }
    const size_t a = out.find_first_not_of(" \t");
    if (a == std::string::npos) return {};
    return out.substr(a, out.find_last_not_of(" \t") - a + 1);
}

class Sandbox final : public sr2::core::Module {
public:
    const char* Name() const override { return kModule; }
    bool DefaultEnabled() const override { return false; }
    bool Setup(sr2::core::ModuleContext& ctx) override;
    void OnEnable() override;
    void OnDisable() override;

    void Tick();                         // main thread, every frame
    void Handle(const bridge::Action&);  // main thread, posted job

private:
    struct Ctx {
        Il2CppObject* player = nullptr;    // PlayerState
        Il2CppObject* model = nullptr;     // PlayerModel
        Il2CppObject* currency = nullptr;  // Newbucks CurrencyDefinition (nullptr if not found)
        Il2CppObject* ammo = nullptr;      // AmmoSlotManager
        void* slots = nullptr;             // AmmoSlot[]
    };
    struct CatalogEntry {
        Il2CppGCHandle handle = nullptr;
        Il2CppObject* obj = nullptr;  // identity; re-read through the handle before use
    };

    bool Live() const { return enabled_.load() && !sr2::core::ModuleFaulted(kModule); }
    Il2CppObject* Call(const MethodInfo* m, void* obj, std::initializer_list<void*> args, const char* what,
                       bool* ok = nullptr);
    template <class T>
    bool CallValue(const MethodInfo* m, void* obj, std::initializer_list<void*> args, const char* what, T& out) {
        bool ok = false;
        Il2CppObject* r = Call(m, obj, args, what, &ok);
        return ok && Unbox(r, out);
    }
    bool Context(Ctx& c, std::string& why);
    Il2CppObject* SlotAt(const Ctx& c, int idx);
    std::string Localized(Il2CppObject* ls);
    std::string ItemName(Il2CppObject* ident);
    uint32_t ItemIcon(Il2CppObject* ident, uint32_t* second = nullptr);
    uint32_t SpriteIcon(Il2CppObject* sprite);
    Il2CppObject* VirtualIcon(Il2CppObject* obj);
    uint32_t GroupIconFor(Il2CppObject* ident);
    void EnsureIconGroups();
    void EnsureGroupIcons();
    void EnsureRelations();
    bridge::Kind ItemKind(Il2CppObject* ident);
    bool EnsureCatalog(const Ctx& c);
    Il2CppObject* CatalogItem(uintptr_t id, uint32_t* mask);
    Il2CppObject* Lookup(const std::unordered_map<uintptr_t, CatalogEntry>& map, uintptr_t id, Il2CppClass* k);
    Il2CppObject* GameContextObj();
    Il2CppObject* SaveIndex();
    bool Saveable(Il2CppObject* index, Il2CppObject* ident);
    static void LogLeftOut(const char* list, const std::vector<std::string>& names);
    bool TarrsOff();
    Il2CppObject* TarrGroup();
    bool EnsureSpawnables();
    bool EnsureRecipes();
    bool EnsureMaterials();
    void RefreshRecipeState();
    Il2CppObject* SlotUnlockUpgrade(const Ctx& c, Il2CppObject* slot, Il2CppObject** upgradeModel, int* level = nullptr);
    std::string RecipeCost(Il2CppObject* def);
    Il2CppObject* GadgetDirector();
    void Refresh();
    void TopUpEnergy();
    void Done(bool ok, const std::string& text);

    void DoNewbucks(const Ctx& c, int64_t target, const char* how);
    void DoSlotCount(const Ctx& c, int idx, int64_t value);
    void DoPutItem(const Ctx& c, int idx, uintptr_t item, int64_t value);
    void DoSpawn(const Ctx& c, uintptr_t item, int64_t count);
    void DoCraft(uintptr_t item, int64_t count);
    void DoAddMaterial(uintptr_t item, int64_t count);
    void DoMoveSlot(const Ctx& c, int from, int to);
    void DoUnlockSlot(const Ctx& c, int idx);
    bool SpawnPose(const Ctx& c, Vec3& pos, Vec3& fwd);
    bool GetVec3(const MethodInfo* m, Il2CppObject* obj, Vec3& out);
    void SetupExtras(sr2::il2cpp::Resolver& R);

    std::atomic<bool> enabled_{false};
    std::atomic<bool> infinite_{false};
    int frameCb_ = 0;
    unsigned long long lastSnap_ = 0, lastEnergy_ = 0;

    // catalog (main thread)
    std::shared_ptr<const std::vector<bridge::Item>> catalog_;
    std::unordered_map<uintptr_t, CatalogEntry> catalogById_;
    std::vector<Il2CppObject*> catalogDefs_;  // slot definitions it was built for (identity only)
    std::unordered_map<uintptr_t, std::string> nameCache_;  // identity -> name (never dereferenced)
    std::unordered_map<uintptr_t, uint32_t> iconCache_;     // identity -> item_icons id
    std::unordered_map<uintptr_t, uint32_t> icon2Cache_;    // identity -> second icon of a composed one (largos)
    std::unordered_map<uintptr_t, bridge::Kind> kindCache_; // identity -> kind
    std::unordered_map<Il2CppClass*, const MethodInfo*> iconGetter_;  // class -> its (overriding) get_Icon, or nullptr
    std::vector<CatalogEntry> iconGroups_;  // IdentifiableTypeGroup assets with an icon (fallback icons), leaf groups first
    bool iconGroupsBuilt_ = false;
    uint32_t groupIcons_[bridge::kGroupCount] = {};  // icons of the group tabs
    bool groupIconsBuilt_ = false;
    std::shared_ptr<const bridge::Relations> relations_;  // slime diets, built once after the spawn list

    // spawner and crafter lists (main thread), built once per enable; entries kept alive by GC handles
    std::shared_ptr<const std::vector<bridge::Item>> spawnables_;
    std::unordered_map<uintptr_t, CatalogEntry> spawnById_;
    bool spawnTried_ = false;
    std::string spawnWhy_;  // "" = spawner available
    std::shared_ptr<const std::vector<bridge::Recipe>> recipes_;
    std::shared_ptr<const std::vector<std::string>> recipeCats_;
    std::shared_ptr<const std::vector<uint32_t>> recipeCatIcons_;
    std::unordered_map<uintptr_t, CatalogEntry> recipeById_;
    bool recipesTried_ = false;
    std::shared_ptr<const std::vector<bridge::Recipe>> materials_;  // refinery materials
    std::unordered_map<uintptr_t, CatalogEntry> materialById_;
    bool materialsTried_ = false;
    std::string craftWhy_;  // "" = crafter available
    unsigned long long lastRecipeState_ = 0;

    // ---- resolved in Setup ----
    Il2CppClass* playerStateClass_ = nullptr;
    Il2CppClass* playerModelClass_ = nullptr;
    Il2CppClass* currencyClass_ = nullptr;
    Il2CppClass* ammoMgrClass_ = nullptr;
    Il2CppClass* ammoSlotClass_ = nullptr;
    Il2CppClass* slotDefClass_ = nullptr;
    Il2CppClass* identClass_ = nullptr;
    Il2CppClass* vacClass_ = nullptr;
    Il2CppClass* locStringClass_ = nullptr;
    size_t offPlayerModel_ = 0;     // PlayerState._model
    size_t offMaxCurrency_ = 0;     // PlayerModel.maxCurrency
    size_t offPersistenceId_ = 0;   // CurrencyDefinition._persistenceId
    size_t offSlotId_ = 0;          // AmmoSlot._id
    size_t offSlotCount_ = 0;       // AmmoSlot._count
    size_t offSlotDef_ = 0;         // AmmoSlot.Definition
    size_t offIdentPrefab_ = 0;     // IdentifiableType.prefab
    size_t offIdentLocName_ = 0;    // IdentifiableType.localizedName
    size_t offIdentIsPlayer_ = 0;   // IdentifiableType.IsPlayer
    size_t offIdentIsScene_ = 0;    // IdentifiableType.IsSceneObject
    size_t offVacSize_ = 0;         // Vacuumable.Size (VacuumableSize: NORMAL 0, LARGE 1, GIANT 2)
    const MethodInfo* tryGetPlayerState_ = nullptr;  // SceneContextUtility.TryGetPlayerState(out PlayerState)
    const MethodInfo* defaultCurrency_ = nullptr;    // CurrencyUtility.get_DefaultCurrency
    const MethodInfo* getCurrency_ = nullptr;        // PlayerState.GetCurrency(ICurrency)
    const MethodInfo* addCurrency_ = nullptr;        // PlayerState.AddCurrency(ICurrency, int, bool, IUIDisplayData)
    const MethodInfo* spendCurrency_ = nullptr;      // PlayerState.SpendCurrency(ICurrency, int, IUIDisplayData)
    const MethodInfo* getHealth_ = nullptr;          // PlayerState.GetCurrHealth
    const MethodInfo* getMaxHealth_ = nullptr;       // PlayerState.GetMaxHealth
    const MethodInfo* setHealth_ = nullptr;          // PlayerState.SetHealth(int)
    const MethodInfo* getEnergy_ = nullptr;          // PlayerState.GetCurrEnergy
    const MethodInfo* getMaxEnergy_ = nullptr;       // PlayerState.GetMaxEnergy
    const MethodInfo* setEnergy_ = nullptr;          // PlayerState.SetEnergy(int)
    const MethodInfo* getRad_ = nullptr;             // PlayerState.GetCurrRad
    const MethodInfo* setRad_ = nullptr;             // PlayerState.SetRad(int)
    const MethodInfo* getAmmo_ = nullptr;            // PlayerState.get_Ammo
    const MethodInfo* getSlots_ = nullptr;           // AmmoSlotManager.get_Slots
    const MethodInfo* slotMax_ = nullptr;            // AmmoSlotManager.GetSlotMaxCount(int)
    const MethodInfo* addResource_ = nullptr;        // AmmoSlotManager.MaybeAddResource(IdentifiableType, int, int, bool)
    const MethodInfo* decrement_ = nullptr;          // AmmoSlotManager.Decrement(int, int)
    const MethodInfo* slotUnlocked_ = nullptr;       // AmmoSlot.get_IsUnlocked
    const MethodInfo* isAllowed_ = nullptr;          // AmmoSlotDefinition.IsAllowed(IdentifiableType)
    const MethodInfo* locGet_ = nullptr;             // LocalizedString.GetLocalizedString()
    const MethodInfo* locIsEmpty_ = nullptr;         // LocalizedReference.get_IsEmpty (optional)
    size_t offIdentIcon_ = 0;       // IdentifiableType.icon (Sprite)
    size_t offIdentCategories_ = 0; // IdentifiableType.Categories (List<IdentifiableCategory>)
    size_t offIdentIsPlort_ = 0;    // IdentifiableType.IsPlort
    size_t offIdentIsAnimal_ = 0;   // IdentifiableType.IsAnimal
    size_t offSlimeIsLargo_ = 0;    // SlimeDefinition.IsLargo
    Il2CppClass* slimeDefClass_ = nullptr;
    Il2CppClass* toyDefClass_ = nullptr;
    Il2CppClass* liquidDefClass_ = nullptr;
    Il2CppClass* gadgetDefClass_ = nullptr;

    // ---- save check: every list only offers types the save code can store ----
    std::string saveWhy_;                            // "" = save check available (else nothing is offered)
    const MethodInfo* gameCtxInstance_ = nullptr;    // GameContext.get_Instance (SRSingleton)
    size_t offGcAutoSave_ = 0;                       // GameContext.<AutoSaveDirector>k__BackingField
    size_t offAsdTranslation_ = 0;                   // AutoSaveDirector._saveReferenceTranslation
    Il2CppClass* saveRefClass_ = nullptr;            // SaveReferenceTranslation
    size_t offSrtIdentTable_ = 0;                    // SaveReferenceTranslation._identifiableTypeToPersistenceId
    const MethodInfo* referenceId_ = nullptr;        // IdentifiableType.get_ReferenceId
    Il2CppClass* tableClass_ = nullptr;              // PersistenceIdLookupTable<IdentifiableType> (from the live object)
    size_t offTableReverse_ = 0;                     // its _reverseIndex (Dictionary<string, int>)
    Il2CppClass* dictClass_ = nullptr;               // Dictionary<string, int> (from the live object)
    const MethodInfo* containsKey_ = nullptr;        // its ContainsKey(string)

    // ---- Tarr setting (optional: without it no entry is grayed out; spawning stays save-checked) ----
    size_t offGcSettings_ = 0;                       // GameContext.<GameSettingsDirector>k__BackingField
    const MethodInfo* tarrEnabled_ = nullptr;        // GameSettingsDirector.get_TarrEnabled
    size_t offScGameModel_ = 0;                      // SceneContext.<GameModel>k__BackingField
    size_t offGmTarrGroup_ = 0;                      // GameModel.tarrTypeGroup
    Il2CppClass* groupClass_ = nullptr;              // IdentifiableTypeGroup
    const MethodInfo* groupIsMember_ = nullptr;      // IdentifiableTypeGroup.IsMember(IdentifiableType)

    // ---- spawner (optional: missing symbols only disable the spawner) ----
    Il2CppClass* actorClass_ = nullptr;              // IdentifiableActor (prefab component of spawnable actors)
    const MethodInfo* instantiateActor_ = nullptr;   // InstantiationHelpers.InstantiateActor(10 args)
    const MethodInfo* tryGetSceneContext_ = nullptr; // SceneContextUtility.TryGetSceneContext(out SceneContext)
    size_t offScRegionRegistry_ = 0;                 // SceneContext.<RegionRegistry>k__BackingField
    const MethodInfo* currentSceneGroup_ = nullptr;  // RegionRegistry.get_CurrentSceneGroup
    const MethodInfo* trPosition_ = nullptr;         // Transform.get_position
    const MethodInfo* trForward_ = nullptr;          // Transform.get_forward
    const MethodInfo* raycast_ = nullptr;            // Physics.Raycast(Vector3, Vector3, out RaycastHit, float, int, QueryTriggerInteraction) (optional)
    size_t offHitDistance_ = 0;                      // RaycastHit.m_Distance (in the unboxed struct)

    // ---- crafter (optional) ----
    const MethodInfo* tryGetGadgetDir_ = nullptr;    // SceneContextUtility.TryGetGadgetDirector(out GadgetDirector)
    const MethodInfo* gadgetAddItem_ = nullptr;      // GadgetDirector.AddItem(IdentifiableType, int)
    const MethodInfo* canAddGadget_ = nullptr;       // GadgetDirector.CanAddGadget(GadgetDefinition, int)
    const MethodInfo* hasBlueprint_ = nullptr;       // GadgetDirector.HasBlueprint(GadgetDefinition)
    const MethodInfo* gadgetCount_ = nullptr;        // GadgetDirector.GetItemCount(IdentifiableType)
    size_t offCraftCosts_ = 0;                       // GadgetDefinition.CraftingCosts (PurchaseCost, inline)
    size_t offCostCurrency_ = 0, offCostIdent_ = 0;  // PurchaseCost._currencyCosts / IdentCosts (in the struct)
    size_t offIdentCostAmount_ = 0, offIdentCostType_ = 0, identCostStride_ = 0;  // IdentCostEntry (in the struct)
    size_t offCurCostCurrency_ = 0, offCurCostAmount_ = 0;  // CurrencyCostEntry._currency / _amount
    size_t offBuyInPairs_ = 0;                       // GadgetDefinition.BuyInPairs
    Il2CppClass* fabMenuClass_ = nullptr;            // FabricateMenuConfiguration (optional, for categories)
    Il2CppClass* fabGadgetCatClass_ = nullptr;       // GadgetFabricateCategory
    size_t offFabCategories_ = 0, offFabCatTitle_ = 0;
    size_t offFabCatIcon_ = 0;                       // FabricateCategory._icon (optional)
    const MethodInfo* fabCatContains_ = nullptr;     // GadgetFabricateCategory.Contains(GadgetDefinition)
    const MethodInfo* isStorable_ = nullptr;         // GadgetDirector.IsStorable(IdentifiableType) (refinery materials)
    const MethodInfo* refinerySpace_ = nullptr;      // GadgetDirector.GetRefinerySpaceAvailable(IdentifiableType)

    // ---- icons, group tabs and diets (optional, cosmetic) ----
    size_t offSlimeBases_ = 0;      // SlimeDefinition.BaseSlimes (SlimeDefinition[])
    size_t offSlimeDiet_ = 0;       // SlimeDefinition.Diet (SlimeDiet)
    size_t offDietFavs_ = 0;        // SlimeDiet.FavoriteIdents (IdentifiableType[])
    size_t offDietProduce_ = 0;     // SlimeDiet.ProduceIdents (IdentifiableType[])
    size_t offIdentGroupType_ = 0;  // IdentifiableType.groupType (IdentifiableTypeGroup)
    size_t offGroupIcon_ = 0;       // IdentifiableTypeGroup._icon
    size_t offGroupSubGroups_ = 0;  // IdentifiableTypeGroup._memberGroups
    size_t offGroupIsFood_ = 0;     // IdentifiableTypeGroup._isFood
    const MethodInfo* groupContains_ = nullptr;  // IdentifiableTypeGroup.IsMember (for the icon fallback)
    size_t offModTarget_ = 0;       // FloatModifier.TargetProperty (from the live object's class)
    Il2CppClass* modClass_ = nullptr;
    Il2CppClass* droneTargetsClass_ = nullptr;  // DroneTargetDefinitions (ListAsset of DroneUIProgramDefinition<DroneTaskTargetType>)

    // ---- vacpack slot unlock (optional): the upgrade levels the fabricator sells ----
    const MethodInfo* getUpgradeModel_ = nullptr;    // PlayerModel.GetUpgradeModel()
    const MethodInfo* incUpgrade_ = nullptr;         // UpgradeModel.IncrementUpgradeLevel(UpgradeDefinition)
    const MethodInfo* getUpgradeLevel_ = nullptr;    // UpgradeModel.GetUpgradeLevel(UpgradeDefinition)
    size_t offUmDefs_ = 0;                           // UpgradeModel.upgradeDefinitions (UpgradeDefinitionList)
    size_t offUdLevels_ = 0;                         // UpgradeDefinition._levels (List<UpgradeLevelDefinition>)
    size_t offUlEffects_ = 0;                        // UpgradeLevelDefinition.Effects (List<FloatModifier>)
    size_t offSlotDefUnlockProp_ = 0;                // AmmoSlotDefinition._isUnlockedProperty
    size_t offSlotDefThreshold_ = 0;                 // AmmoSlotDefinition._isUnlockedThreshold
    size_t offModValue_ = 0;                         // FloatModifier.Value (from the live object's class)
    Il2CppClass* upgradeDefClass_ = nullptr;
};

Sandbox* g_self = nullptr;

void HandleThunk(const bridge::Action& a) {
    if (g_self) g_self->Handle(a);
}

// ------------------------------------------------------------------ setup ----
bool Sandbox::Setup(sr2::core::ModuleContext& ctx) {
    auto& R = ctx.resolver();
    std::string why;
    auto note = [&](const std::string& s) {
        if (why.empty()) why = s;
    };
    auto cls = [&](const char* image, const char* ns, const char* name) -> Il2CppClass* {
        std::string err;
        Il2CppClass* k = R.Class(image, ns, name, &err);
        if (!k) note(err.empty() ? std::format("class {}.{} not found", ns, name) : err);
        return k;
    };
    auto off = [&](Il2CppClass* k, const char* field, size_t& out) {
        if (!k) return;
        std::string err;
        auto o = R.FieldOffset(k, field, &err);
        if (o) out = *o;
        else note(err.empty() ? std::format("field {} not found", field) : err);
    };
    // Searches the class and its parents (inherited / interface-implementing methods).
    auto method = [&](Il2CppClass* k, const char* name, int argc, bool required = true) -> const MethodInfo* {
        std::string err;
        for (Il2CppClass* c = k; c; c = api().il2cpp_class_get_parent ? api().il2cpp_class_get_parent(c) : nullptr)
            if (const MethodInfo* m = R.Method(c, name, argc, &err)) return m;
        if (required && k) note(std::format("method {}({}) not found", name, argc));
        return nullptr;
    };

    playerStateClass_ = cls(kImage, "", "PlayerState");
    playerModelClass_ = cls(kImage, "MonomiPark.SlimeRancher.DataModel", "PlayerModel");
    currencyClass_ = cls(kImage, "MonomiPark.SlimeRancher.Economy", "CurrencyDefinition");
    Il2CppClass* currencyUtil = cls(kImage, "MonomiPark.SlimeRancher.Economy", "CurrencyUtility");
    Il2CppClass* scu = cls(kImage, "MonomiPark.SlimeRancher.Util", "SceneContextUtility");
    ammoMgrClass_ = cls(kImage, "MonomiPark.SlimeRancher.Player", "AmmoSlotManager");
    ammoSlotClass_ = cls(kImage, "MonomiPark.SlimeRancher.Player", "AmmoSlot");
    slotDefClass_ = cls(kImage, "", "AmmoSlotDefinition");
    identClass_ = cls(kImage, "", "IdentifiableType");
    vacClass_ = cls(kImage, "MonomiPark.SlimeRancher", "Vacuumable");
    locStringClass_ = cls("Unity.Localization", "UnityEngine.Localization", "LocalizedString");
    if (!why.empty()) return ctx.Fail(why);

    off(playerStateClass_, "_model", offPlayerModel_);
    off(playerModelClass_, "maxCurrency", offMaxCurrency_);
    off(currencyClass_, "_persistenceId", offPersistenceId_);
    off(ammoSlotClass_, "_id", offSlotId_);
    off(ammoSlotClass_, "_count", offSlotCount_);
    off(ammoSlotClass_, "Definition", offSlotDef_);
    off(identClass_, "prefab", offIdentPrefab_);
    off(identClass_, "localizedName", offIdentLocName_);
    off(identClass_, "IsPlayer", offIdentIsPlayer_);
    off(identClass_, "IsSceneObject", offIdentIsScene_);
    off(vacClass_, "Size", offVacSize_);
    if (!why.empty()) return ctx.Fail(why);

    tryGetPlayerState_ = method(scu, "TryGetPlayerState", 1);
    defaultCurrency_ = method(currencyUtil, "get_DefaultCurrency", 0);
    getCurrency_ = method(playerStateClass_, "GetCurrency", 1);
    addCurrency_ = method(playerStateClass_, "AddCurrency", 4);
    spendCurrency_ = method(playerStateClass_, "SpendCurrency", 3);
    getHealth_ = method(playerStateClass_, "GetCurrHealth", 0);
    getMaxHealth_ = method(playerStateClass_, "GetMaxHealth", 0);
    setHealth_ = method(playerStateClass_, "SetHealth", 1);
    getEnergy_ = method(playerStateClass_, "GetCurrEnergy", 0);
    getMaxEnergy_ = method(playerStateClass_, "GetMaxEnergy", 0);
    setEnergy_ = method(playerStateClass_, "SetEnergy", 1);
    getRad_ = method(playerStateClass_, "GetCurrRad", 0);
    setRad_ = method(playerStateClass_, "SetRad", 1);
    getAmmo_ = method(playerStateClass_, "get_Ammo", 0);
    getSlots_ = method(ammoMgrClass_, "get_Slots", 0);
    slotMax_ = method(ammoMgrClass_, "GetSlotMaxCount", 1);
    addResource_ = method(ammoMgrClass_, "MaybeAddResource", 4);
    {
        std::string err;  // two Decrement overloads take 2 arguments: (int, int) and (IdentifiableType, int)
        decrement_ = R.MethodByParams(ammoMgrClass_, "Decrement", {"System.Int32", "System.Int32"}, &err);
        if (!decrement_) note("Decrement(int, int): " + err);
    }
    slotUnlocked_ = method(ammoSlotClass_, "get_IsUnlocked", 0);
    isAllowed_ = method(slotDefClass_, "IsAllowed", 1);
    locGet_ = method(locStringClass_, "GetLocalizedString", 0);
    locIsEmpty_ = method(locStringClass_, "get_IsEmpty", 0, false);
    if (!why.empty()) return ctx.Fail(why);
    // Required: without the pause the module does not run (fail closed).
    if (!sandbox::SetupAchievementPause(ctx)) return false;
    SetupExtras(R);

    g_self = this;
    if (!frameCb_) frameCb_ = main_thread::AddFrameCallback(kModule, [this](float) { Tick(); });
    if (!frameCb_) return ctx.Fail("could not add the main-thread frame callback");
    SR2_INFO("[{}] setup ok: PlayerState currency/health/energy methods, AmmoSlotManager.MaybeAddResource / "
             "Decrement, AmmoSlotDefinition.IsAllowed resolved; offsets PlayerState._model 0x{:X}, "
             "PlayerModel.maxCurrency 0x{:X}, AmmoSlot._id 0x{:X} _count 0x{:X} Definition 0x{:X}",
             kModule, offPlayerModel_, offMaxCurrency_, offSlotId_, offSlotCount_, offSlotDef_);
    return true;
}

// Icons, item kinds, the spawner and the crafter. Nothing here is required: a
// missing symbol only turns that part off (with the reason in the tab and log).
void Sandbox::SetupExtras(sr2::il2cpp::Resolver& R) {
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
    auto method = [&](Il2CppClass* k, const char* name, int argc, std::string& why) -> const MethodInfo* {
        std::string err;
        for (Il2CppClass* c = k; c; c = api().il2cpp_class_get_parent ? api().il2cpp_class_get_parent(c) : nullptr)
            if (const MethodInfo* m = R.Method(c, name, argc, &err)) return m;
        if (k && why.empty()) why = std::format("method {}({}) not found", name, argc);
        return nullptr;
    };
    // Field offset inside an unboxed struct (il2cpp reports it with the object header).
    auto structOff = [&](Il2CppClass* k, const char* field, size_t& out, std::string& why) {
        size_t o = 0;
        off(k, field, o, why);
        if (o >= 2 * sizeof(void*)) out = o - 2 * sizeof(void*);
        else if (k && why.empty()) why = std::format("field {} has no struct offset", field);
    };

    std::string optional;  // icons and kinds: only cosmetic
    off(identClass_, "icon", offIdentIcon_, optional);
    off(identClass_, "Categories", offIdentCategories_, optional);
    off(identClass_, "IsPlort", offIdentIsPlort_, optional);
    off(identClass_, "IsAnimal", offIdentIsAnimal_, optional);
    slimeDefClass_ = cls(kImage, "", "SlimeDefinition", optional);
    off(slimeDefClass_, "IsLargo", offSlimeIsLargo_, optional);
    toyDefClass_ = cls(kImage, "", "ToyDefinition", optional);
    liquidDefClass_ = cls(kImage, "", "LiquidDefinition", optional);
    if (!optional.empty()) SR2_WARN("[{}] item icons / kinds partly unavailable: {}", kModule, optional);
    {
        // Fallback icons (largos from their base slimes, group icons for types without
        // their own), the group tabs' icons and slime diets. All cosmetic / optional.
        std::string iw;
        groupClass_ = cls(kImage, "", "IdentifiableTypeGroup", iw);
        off(groupClass_, "_icon", offGroupIcon_, iw);
        off(groupClass_, "_memberGroups", offGroupSubGroups_, iw);
        off(groupClass_, "_isFood", offGroupIsFood_, iw);
        off(identClass_, "groupType", offIdentGroupType_, iw);
        groupContains_ = method(groupClass_, "IsMember", 1, iw);
        if (!iw.empty()) {
            offGroupIcon_ = 0;
            SR2_WARN("[{}] no group icons ({})", kModule, iw);
        }
        std::string bw;
        off(slimeDefClass_, "BaseSlimes", offSlimeBases_, bw);
        if (!bw.empty()) SR2_WARN("[{}] no composed largo icons ({})", kModule, bw);
        std::string dw;
        Il2CppClass* diet = cls(kImage, "", "SlimeDiet", dw);
        off(slimeDefClass_, "Diet", offSlimeDiet_, dw);
        off(diet, "FavoriteIdents", offDietFavs_, dw);
        off(diet, "ProduceIdents", offDietProduce_, dw);
        if (!dw.empty() || !slimeDefClass_) {
            offSlimeDiet_ = 0;
            SR2_WARN("[{}] no favourites / plorts in the tab ({})", kModule, dw);
        }
        std::string tw;
        droneTargetsClass_ = cls(kImage, "MonomiPark.SlimeRancher.UI.Drone.Data", "DroneTargetDefinitions", tw);
        if (!tw.empty()) SR2_WARN("[{}] group tabs: no drone target icons ({})", kModule, tw);
    }
    {
        // Optional: unlocking a locked vacpack slot through the upgrade the fabricator sells
        // (PlayerUpgradeFabricatableItem.FabricateAndSpendCost: UpgradeModel.IncrementUpgradeLevel).
        std::string uw;
        Il2CppClass* upModel = cls(kImage, "MonomiPark.SlimeRancher.DataModel", "UpgradeModel", uw);
        upgradeDefClass_ = cls(kImage, "", "UpgradeDefinition", uw);
        getUpgradeModel_ = method(playerModelClass_, "GetUpgradeModel", 0, uw);
        incUpgrade_ = method(upModel, "IncrementUpgradeLevel", 1, uw);
        getUpgradeLevel_ = method(upModel, "GetUpgradeLevel", 1, uw);
        off(upModel, "upgradeDefinitions", offUmDefs_, uw);
        off(upgradeDefClass_, "_levels", offUdLevels_, uw);
        off(slotDefClass_, "_isUnlockedProperty", offSlotDefUnlockProp_, uw);
        off(slotDefClass_, "_isUnlockedThreshold", offSlotDefThreshold_, uw);
        // UpgradeLevelDefinition (nested) and FloatModifier (generic base): their fields are read
        // on the live objects (offUlEffects_ and the modifier's TargetProperty, resolved once).
        if (!uw.empty()) {
            incUpgrade_ = nullptr;
            SR2_WARN("[{}] vacpack: locked slots can't be unlocked ({})", kModule, uw);
        }
    }

    // Save check: the dictionary the save code looks identifiable types up in
    // (GameModelPullHelpers.BuildActorData -> SaveReferenceTranslation.GetPersistenceId).
    // Without it nothing is offered: the vacpack list stays empty, spawner and crafter are off.
    {
        std::string& vw = saveWhy_;
        Il2CppClass* gameCtx = cls(kImage, "", "GameContext", vw);
        Il2CppClass* autoSave = cls(kImage, "MonomiPark.SlimeRancher", "AutoSaveDirector", vw);
        saveRefClass_ = cls(kImage, "MonomiPark.SlimeRancher", "SaveReferenceTranslation", vw);
        gameCtxInstance_ = method(gameCtx, "get_Instance", 0, vw);
        off(gameCtx, "<AutoSaveDirector>k__BackingField", offGcAutoSave_, vw);
        off(autoSave, "_saveReferenceTranslation", offAsdTranslation_, vw);
        off(saveRefClass_, "_identifiableTypeToPersistenceId", offSrtIdentTable_, vw);
        referenceId_ = method(identClass_, "get_ReferenceId", 0, vw);
        if (!vw.empty()) {
            vw = "save check unavailable (" + vw + ")";
            SR2_WARN("[{}] {}: the sandbox offers no items", kModule, vw);
        }
    }

    // Spawner: the same InstantiationHelpers.InstantiateActor the vacpack uses to shoot items out
    // (VacuumItem.Expel), with the region registry's current scene group.
    std::string& sw = spawnWhy_;
    sw = saveWhy_;
    gadgetDefClass_ = cls(kImage, "", "GadgetDefinition", sw);
    actorClass_ = cls(kImage, "", "IdentifiableActor", sw);
    Il2CppClass* helpers = cls(kImage, "", "InstantiationHelpers", sw);
    Il2CppClass* scu = cls(kImage, "MonomiPark.SlimeRancher.Util", "SceneContextUtility", sw);
    Il2CppClass* sceneCtx = cls(kImage, "", "SceneContext", sw);
    Il2CppClass* regions = cls(kImage, "MonomiPark.SlimeRancher.Regions", "RegionRegistry", sw);
    Il2CppClass* transform = cls("UnityEngine.CoreModule", "UnityEngine", "Transform", sw);
    instantiateActor_ = method(helpers, "InstantiateActor", 10, sw);
    tryGetSceneContext_ = method(scu, "TryGetSceneContext", 1, sw);
    off(sceneCtx, "<RegionRegistry>k__BackingField", offScRegionRegistry_, sw);
    currentSceneGroup_ = method(regions, "get_CurrentSceneGroup", 0, sw);
    trPosition_ = method(transform, "get_position", 0, sw);
    trForward_ = method(transform, "get_forward", 0, sw);
    {
        // Optional: keeps spawns in front of walls. Triggers are ignored, the default layers hit.
        std::string rw;
        Il2CppClass* physics = cls("UnityEngine.PhysicsModule", "UnityEngine", "Physics", rw);
        Il2CppClass* hit = cls("UnityEngine.PhysicsModule", "UnityEngine", "RaycastHit", rw);
        if (physics)
            raycast_ = R.MethodByParams(physics, "Raycast",
                                        {"UnityEngine.Vector3", "UnityEngine.Vector3", "UnityEngine.RaycastHit&", "System.Single",
                                         "System.Int32", "UnityEngine.QueryTriggerInteraction"},
                                        &rw);
        structOff(hit, "m_Distance", offHitDistance_, rw);
        if (!rw.empty() || !offHitDistance_) {
            raycast_ = nullptr;
            SR2_WARN("[{}] spawner: no wall check ({})", kModule, rw);
        }
    }
    if (!sw.empty()) SR2_WARN("[{}] spawner unavailable: {}", kModule, sw);
    {
        // Optional: gray out the Tarr entries while the save's game settings have Tarrs off.
        std::string tw;
        Il2CppClass* gameCtx = cls(kImage, "", "GameContext", tw);
        Il2CppClass* settings = cls(kImage, "MonomiPark.SlimeRancher", "GameSettingsDirector", tw);
        Il2CppClass* gameModel = cls(kImage, "MonomiPark.SlimeRancher.DataModel", "GameModel", tw);
        groupClass_ = cls(kImage, "", "IdentifiableTypeGroup", tw);
        off(gameCtx, "<GameSettingsDirector>k__BackingField", offGcSettings_, tw);
        tarrEnabled_ = method(settings, "get_TarrEnabled", 0, tw);
        off(sceneCtx, "<GameModel>k__BackingField", offScGameModel_, tw);
        off(gameModel, "tarrTypeGroup", offGmTarrGroup_, tw);
        groupIsMember_ = method(groupClass_, "IsMember", 1, tw);
        if (!tw.empty() || !sceneCtx || !gameCtxInstance_ || !tryGetSceneContext_) {
            tarrEnabled_ = groupIsMember_ = nullptr;
            SR2_WARN("[{}] spawner: Tarr entries are not grayed out when Tarrs are off ({})", kModule, tw);
        }
    }

    // Crafter: the fabricator's GadgetFabricatableItem.FabricateAndSpendCost minus the spending:
    // GadgetDirector.AddItem(definition, count, x2 for BuyInPairs), within CanAddGadget.
    std::string& cw = craftWhy_;
    cw = saveWhy_;
    if (!gadgetDefClass_ && cw.empty()) cw = "class GadgetDefinition not found";
    Il2CppClass* gadgetDir = cls(kImage, "", "GadgetDirector", cw);
    Il2CppClass* cost = cls(kImage, "MonomiPark.SlimeRancher.Economy", "PurchaseCost", cw);
    Il2CppClass* identCost = cls(kImage, "MonomiPark.SlimeRancher.Economy", "IdentCostEntry", cw);
    Il2CppClass* curCost = cls(kImage, "MonomiPark.SlimeRancher.Economy", "CurrencyCostEntry", cw);
    if (scu) tryGetGadgetDir_ = method(scu, "TryGetGadgetDirector", 1, cw);
    if (gadgetDir) {
        std::string err;
        gadgetAddItem_ = R.MethodByParams(gadgetDir, "AddItem", {"IdentifiableType", "System.Int32"}, &err);
        if (!gadgetAddItem_ && cw.empty()) cw = "GadgetDirector.AddItem(IdentifiableType, int): " + err;
    }
    canAddGadget_ = method(gadgetDir, "CanAddGadget", 2, cw);
    hasBlueprint_ = method(gadgetDir, "HasBlueprint", 1, cw);
    gadgetCount_ = method(gadgetDir, "GetItemCount", 1, cw);
    off(gadgetDefClass_, "CraftingCosts", offCraftCosts_, cw);
    off(gadgetDefClass_, "BuyInPairs", offBuyInPairs_, cw);
    structOff(cost, "_currencyCosts", offCostCurrency_, cw);
    structOff(cost, "IdentCosts", offCostIdent_, cw);
    structOff(identCost, "Amount", offIdentCostAmount_, cw);
    structOff(identCost, "IdentType", offIdentCostType_, cw);
    off(curCost, "_currency", offCurCostCurrency_, cw);
    off(curCost, "_amount", offCurCostAmount_, cw);
    // IdentCostEntry is {int Amount; IdentifiableType IdentType}: the stride is the end of the
    // later field, rounded up to pointer alignment.
    identCostStride_ = (std::max(offIdentCostAmount_ + sizeof(int32_t), offIdentCostType_ + sizeof(void*)) + sizeof(void*) - 1) &
                       ~(sizeof(void*) - 1);
    if (!cw.empty()) SR2_WARN("[{}] crafter unavailable: {}", kModule, cw);
    {
        // Optional: the fabricator's own category list, for grouping.
        std::string fw;
        constexpr const char* kFab = "MonomiPark.SlimeRancher.UI.Fabricator";
        fabMenuClass_ = cls(kImage, kFab, "FabricateMenuConfiguration", fw);
        fabGadgetCatClass_ = cls(kImage, kFab, "GadgetFabricateCategory", fw);
        Il2CppClass* fabCat = cls(kImage, kFab, "FabricateCategory", fw);
        off(fabMenuClass_, "_categories", offFabCategories_, fw);
        off(fabCat, "_title", offFabCatTitle_, fw);
        fabCatContains_ = method(fabGadgetCatClass_, "Contains", 1, fw);
        if (!fw.empty()) {
            fabMenuClass_ = nullptr;
            SR2_WARN("[{}] crafter: no fabricator categories ({})", kModule, fw);
        }
        std::string iw;
        off(fabCat, "_icon", offFabCatIcon_, iw);
    }
    {
        // Optional: crafting materials straight into the refinery (GadgetDirector.AddItem
        // stores any IsStorable type there, up to GetRefinerySpaceAvailable).
        std::string mw;
        isStorable_ = method(gadgetDir, "IsStorable", 1, mw);
        refinerySpace_ = method(gadgetDir, "GetRefinerySpaceAvailable", 1, mw);
        if (!mw.empty()) {
            isStorable_ = refinerySpace_ = nullptr;
            SR2_WARN("[{}] crafter: no crafting materials ({})", kModule, mw);
        }
    }
}

void Sandbox::OnEnable() {
    enabled_.store(true);
    sandbox::SetAchievementsPaused(true);
    bridge::SetHandler(&HandleThunk);
    lastSnap_ = 0;
    // A list that came out empty (built before the assets were loaded) is tried again.
    if (!spawnables_ || spawnables_->empty()) spawnTried_ = false;
    if (!recipes_ || recipes_->empty()) recipesTried_ = false;
    if (!materials_ || materials_->empty()) materialsTried_ = false;
    SR2_INFO("[{}] enabled: edits from the overlay's Sandbox tab now change the loaded game", kModule);
}

void Sandbox::OnDisable() {
    enabled_.store(false);
    sandbox::SetAchievementsPaused(false);
    if (infinite_.exchange(false)) SR2_INFO("[{}] infinite energy off (module disabled)", kModule);
    bridge::SetHandler(nullptr);
    bridge::Snapshot s;
    s.tick = GetTickCount64();
    s.why = "Sandbox is off.";
    bridge::Publish(std::move(s));
    SR2_INFO("[{}] disabled", kModule);
}

// ---------------------------------------------------------------- helpers ----
Il2CppObject* Sandbox::Call(const MethodInfo* m, void* obj, std::initializer_list<void*> args, const char* what,
                            bool* ok) {
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

bool Sandbox::Context(Ctx& c, std::string& why) {
    Il2CppObject* player = nullptr;
    bool found = false;
    if (!CallValue(tryGetPlayerState_, nullptr, {&player}, "SceneContextUtility.TryGetPlayerState", found) || !found ||
        !player) {
        why = "Load a save to use the sandbox.";
        return false;
    }
    if (!IsA(player, playerStateClass_) || !unity::IsAlive(player)) {
        why = "The player is not available right now.";
        return false;
    }
    Il2CppObject* model = unity::Field<Il2CppObject*>(player, offPlayerModel_);
    if (!IsA(model, playerModelClass_)) {
        why = "The player has no data model yet.";
        return false;
    }
    c.player = player;
    c.model = model;

    bool ok = false;
    Il2CppObject* cur = Call(defaultCurrency_, nullptr, {}, "CurrencyUtility.DefaultCurrency", &ok);
    // Newbucks only: a CurrencyDefinition asset with the Newbucks persistence id.
    if (ok && IsA(cur, currencyClass_) && unity::IsAlive(cur) &&
        unity::Field<int32_t>(cur, offPersistenceId_) == kNewbuckPersistenceId)
        c.currency = cur;

    Il2CppObject* ammo = Call(getAmmo_, player, {}, "PlayerState.Ammo", &ok);
    if (ok && IsA(ammo, ammoMgrClass_)) {
        Il2CppObject* arr = Call(getSlots_, ammo, {}, "AmmoSlotManager.Slots", &ok);
        if (ok && arr) {
            c.ammo = ammo;
            c.slots = arr;
        }
    }
    return true;
}

Il2CppObject* Sandbox::SlotAt(const Ctx& c, int idx) {
    if (!c.slots || idx < 0 || static_cast<size_t>(idx) >= unity::ArrayLength(c.slots)) return nullptr;
    auto* s = static_cast<Il2CppObject*>(unity::ArrayAt(c.slots, static_cast<size_t>(idx)));
    return IsA(s, ammoSlotClass_) ? s : nullptr;
}

std::string Sandbox::Localized(Il2CppObject* ls) {
    if (!ls || !IsA(ls, locStringClass_)) return {};
    bool empty = false;
    if (locIsEmpty_ && CallValue(locIsEmpty_, ls, {}, "LocalizedString.IsEmpty", empty) && empty) return {};
    Il2CppObject* r = Call(locGet_, ls, {}, "LocalizedString.GetLocalizedString");
    return r ? CleanText(unity::ToUtf8(reinterpret_cast<Il2CppString*>(r))) : std::string();
}

std::string Sandbox::ItemName(Il2CppObject* ident) {
    if (!ident) return {};
    const auto key = reinterpret_cast<uintptr_t>(ident);
    if (auto it = nameCache_.find(key); it != nameCache_.end()) return it->second;
    if (!IsA(ident, identClass_) || !unity::IsAlive(ident)) return "unknown item";
    std::string n = Localized(unity::Field<Il2CppObject*>(ident, offIdentLocName_));
    if (n.empty()) n = unity::GetName(ident);
    if (n.empty()) n = "unknown item";
    nameCache_[key] = n;
    return n;
}

uint32_t Sandbox::SpriteIcon(Il2CppObject* sprite) {
    return sprite && unity::IsAlive(sprite) ? sr2::ui::item_icons::Register(sprite) : 0;
}

// The object's get_Icon, resolved on its own class (it is virtual: SlimeDefinition
// returns its default appearance's icon, GadgetDefinition has its own), cached per class.
Il2CppObject* Sandbox::VirtualIcon(Il2CppObject* obj) {
    const auto& a = api();
    Il2CppClass* k = a.il2cpp_object_get_class ? a.il2cpp_object_get_class(obj) : nullptr;
    if (!k) return nullptr;
    auto it = iconGetter_.find(k);
    if (it == iconGetter_.end()) {
        const MethodInfo* m = nullptr;
        for (Il2CppClass* c = k; c && !m; c = a.il2cpp_class_get_parent ? a.il2cpp_class_get_parent(c) : nullptr)
            m = Resolver::Get().Method(c, "get_Icon", 0);
        it = iconGetter_.emplace(k, m).first;
    }
    if (!it->second) return nullptr;
    bool ok = false;
    Il2CppObject* spr = Call(it->second, obj, {}, "get_Icon", &ok);
    return ok ? spr : nullptr;
}

// The type's icon, registered once with ui/item_icons: IdentifiableType.icon, else
// its get_Icon (largos: their appearance's icon), else for a largo its two base
// slimes' icons (`second` gets the other half), else the icon of a group it
// belongs to (giant eggs, vine bulbs and the like have a group icon at most).
uint32_t Sandbox::ItemIcon(Il2CppObject* ident, uint32_t* second) {
    if (second) *second = 0;
    if (!ident || !offIdentIcon_) return 0;
    const auto key = reinterpret_cast<uintptr_t>(ident);
    if (auto it = iconCache_.find(key); it != iconCache_.end()) {
        if (second) *second = icon2Cache_[key];
        return it->second;
    }
    if (!IsA(ident, identClass_) || !unity::IsAlive(ident)) return 0;
    uint32_t icon = SpriteIcon(unity::Field<Il2CppObject*>(ident, offIdentIcon_)), icon2 = 0;
    if (!icon) icon = SpriteIcon(VirtualIcon(ident));
    if (!icon && offSlimeBases_ && slimeDefClass_ && IsA(ident, slimeDefClass_)) {
        void* bases = unity::Field<void*>(ident, offSlimeBases_);
        uint32_t found[2] = {};
        for (size_t i = 0, n = unity::ArrayLength(bases), k = 0; i < n && k < 2; ++i) {
            auto* b = static_cast<Il2CppObject*>(unity::ArrayAt(bases, i));
            if (b == ident || !IsA(b, slimeDefClass_) || !unity::IsAlive(b)) continue;
            if (const uint32_t bi = ItemIcon(b)) found[k++] = bi;
        }
        icon = found[0], icon2 = found[1];
    }
    if (!icon) icon = GroupIconFor(ident);
    iconCache_[key] = icon;
    icon2Cache_[key] = icon2;
    if (second) *second = icon2;
    return icon;
}

// Every IdentifiableTypeGroup asset with an icon, read once (leaf groups first, so the
// most specific group wins in GroupIconFor). The group assets stay loaded; handles keep them.
void Sandbox::EnsureIconGroups() {
    if (iconGroupsBuilt_ || !offGroupIcon_ || !groupContains_) return;
    iconGroupsBuilt_ = true;
    std::vector<CatalogEntry> leaf, parent;
    std::string names;
    for (Il2CppObject* g : unity::FindObjectsOfTypeAll(groupClass_)) {
        if (!IsA(g, groupClass_) || !unity::IsAlive(g)) continue;
        Il2CppObject* spr = unity::Field<Il2CppObject*>(g, offGroupIcon_);
        if (!spr || !unity::IsAlive(spr)) continue;
        Il2CppGCHandle h = unity::NewGCHandle(g);
        if (!h) continue;
        (unity::ListCount(unity::Field<void*>(g, offGroupSubGroups_)) ? parent : leaf).push_back(CatalogEntry{h, g});
        names += (names.empty() ? "" : ", ") + unity::GetName(g);
    }
    iconGroups_ = std::move(leaf);
    iconGroups_.insert(iconGroups_.end(), parent.begin(), parent.end());
    SR2_INFO("[{}] {} identifiable type group(s) with an icon: {}", kModule, iconGroups_.size(), names);
}

// Icon of the type's own group (IdentifiableType.groupType), else of the first group with an icon
// it belongs to. Only asked for types without any icon of their own (a few), once each.
uint32_t Sandbox::GroupIconFor(Il2CppObject* ident) {
    if (!offGroupIcon_) return 0;
    Il2CppObject* own = offIdentGroupType_ ? unity::Field<Il2CppObject*>(ident, offIdentGroupType_) : nullptr;
    if (IsA(own, groupClass_) && unity::IsAlive(own))
        if (const uint32_t i = SpriteIcon(unity::Field<Il2CppObject*>(own, offGroupIcon_))) return i;
    EnsureIconGroups();
    for (const CatalogEntry& e : iconGroups_) {
        Il2CppObject* g = unity::GCHandleTarget(e.handle);
        if (g != e.obj || !unity::IsAlive(g)) continue;
        bool in = false;
        if (CallValue(groupContains_, g, {ident}, "IdentifiableTypeGroup.IsMember", in) && in)
            if (const uint32_t i = SpriteIcon(unity::Field<Il2CppObject*>(g, offGroupIcon_))) return i;
    }
    return 0;
}

// The game's icons for the group tabs, read once: plorts and crafting materials from the drone
// station's target menu (DroneTargetDefinitions: PLORTS, CRAFTING_MATS; food from VEGGIES), slimes
// and anything still missing from the identifiable type groups by asset name (logged).
void Sandbox::EnsureGroupIcons() {
    if (groupIconsBuilt_) return;
    groupIconsBuilt_ = true;
    const auto& a = api();
    if (droneTargetsClass_) {
        for (Il2CppObject* defs : unity::FindObjectsOfTypeAll(droneTargetsClass_)) {
            if (!IsA(defs, droneTargetsClass_) || !unity::IsAlive(defs)) continue;
            auto itemsOff = Resolver::Get().FieldOffset(a.il2cpp_object_get_class(defs), "items");
            void* list = itemsOff ? unity::Field<void*>(defs, *itemsOff) : nullptr;
            for (size_t i = 0, n = unity::ListCount(list); i < n; ++i) {
                auto* d = static_cast<Il2CppObject*>(unity::ListAt(list, i));
                if (!d) continue;
                Il2CppClass* dk = a.il2cpp_object_get_class(d);
                auto typeOff = Resolver::Get().FieldOffset(dk, "_type");
                auto iconOff = Resolver::Get().FieldOffset(dk, "_icon");
                if (!typeOff || !iconOff) break;
                const int type = unity::Field<int32_t>(d, *typeOff);  // DroneTaskTargetType
                const int group = type == 2 ? bridge::kGroupPlorts : type == 3 ? bridge::kGroupFood
                                  : type == 8                       ? bridge::kGroupResources
                                                                    : -1;
                if (group >= 0 && !groupIcons_[group]) groupIcons_[group] = SpriteIcon(unity::Field<Il2CppObject*>(d, *iconOff));
            }
            break;
        }
    }
    EnsureIconGroups();
    auto lower = [](std::string s) {
        for (char& ch : s) ch = static_cast<char>((ch >= 'A' && ch <= 'Z') ? ch - 'A' + 'a' : ch);
        return s;
    };
    struct Want {
        int group;
        const char* need;
        const char* avoid[4];
    };
    const Want wants[] = {
        {bridge::kGroupSlimes, "slime", {"largo", "tarr", "gordo", "feral"}},
        {bridge::kGroupPlorts, "plort", {"unstable", "edible", nullptr, nullptr}},
        {bridge::kGroupFood, "food", {nullptr, nullptr, nullptr, nullptr}},
        {bridge::kGroupResources, "craft", {nullptr, nullptr, nullptr, nullptr}},
    };
    for (const Want& w : wants) {
        if (groupIcons_[w.group]) continue;
        for (const CatalogEntry& e : iconGroups_) {
            Il2CppObject* g = unity::GCHandleTarget(e.handle);
            if (g != e.obj || !unity::IsAlive(g)) continue;
            const std::string n = lower(unity::GetName(g));
            if (n.find(w.need) == std::string::npos) continue;
            bool bad = false;
            for (const char* av : w.avoid) bad = bad || (av && n.find(av) != std::string::npos);
            if (bad) continue;
            if ((groupIcons_[w.group] = SpriteIcon(unity::Field<Il2CppObject*>(g, offGroupIcon_)))) break;
        }
    }
    SR2_INFO("[{}] group tab icons: plorts {}, slimes {}, food {}, resources {} (0 = drawn fallback)", kModule,
             groupIcons_[bridge::kGroupPlorts], groupIcons_[bridge::kGroupSlimes], groupIcons_[bridge::kGroupFood],
             groupIcons_[bridge::kGroupResources]);
}

// Favourite foods and plorts of every slime in the spawn list (SlimeDefinition.Diet, read once
// after the list is built), with the reverse lookups. Base slimes come before largos.
void Sandbox::EnsureRelations() {
    if (relations_ || !spawnables_) return;
    auto rel = std::make_shared<bridge::Relations>();
    if (!offSlimeDiet_) {  // no diet symbols: an empty table, so this runs once
        relations_ = std::move(rel);
        return;
    }
    auto ids = [&](void* arr, std::vector<uintptr_t>& out) {
        for (size_t i = 0, n = unity::ArrayLength(arr); i < n; ++i)
            if (auto* t = static_cast<Il2CppObject*>(unity::ArrayAt(arr, i)); t && IsA(t, identClass_))
                out.push_back(reinterpret_cast<uintptr_t>(t));
    };
    // Base slimes first, so the reverse lists start with them.
    for (int pass = 0; pass < 2; ++pass) {
        for (const bridge::Item& it : *spawnables_) {
            if ((it.kind == bridge::Kind::Largo) != (pass == 1) ||
                (it.kind != bridge::Kind::Slime && it.kind != bridge::Kind::Largo))
                continue;
            Il2CppObject* s = Lookup(spawnById_, it.id, slimeDefClass_);
            Il2CppObject* diet = s ? unity::Field<Il2CppObject*>(s, offSlimeDiet_) : nullptr;
            if (!diet) continue;
            bridge::Relation& r = (*rel)[it.id];
            ids(unity::Field<void*>(diet, offDietFavs_), r.favourites);
            ids(unity::Field<void*>(diet, offDietProduce_), r.plorts);
            for (uintptr_t f : r.favourites) (*rel)[f].favouriteOf.push_back(it.id);
            for (uintptr_t p : r.plorts) (*rel)[p].plortOf.push_back(it.id);
        }
    }
    SR2_INFO("[{}] slime diets: {} related item(s)", kModule, rel->size());
    relations_ = std::move(rel);
}

// Rough kind from the class and flags, then the names of its IdentifiableCategory assets
// (food / crafting resources have no flag of their own). Only used for looks and filters.
bridge::Kind Sandbox::ItemKind(Il2CppObject* ident) {
    if (!ident) return bridge::Kind::Other;
    const auto key = reinterpret_cast<uintptr_t>(ident);
    if (auto it = kindCache_.find(key); it != kindCache_.end()) return it->second;
    if (!IsA(ident, identClass_) || !unity::IsAlive(ident)) return bridge::Kind::Other;
    bridge::Kind k = bridge::Kind::Other;
    if (slimeDefClass_ && IsA(ident, slimeDefClass_))
        k = offSlimeIsLargo_ && unity::Field<bool>(ident, offSlimeIsLargo_) ? bridge::Kind::Largo : bridge::Kind::Slime;
    else if (gadgetDefClass_ && IsA(ident, gadgetDefClass_)) k = bridge::Kind::Gadget;
    else if (toyDefClass_ && IsA(ident, toyDefClass_)) k = bridge::Kind::Toy;
    else if (liquidDefClass_ && IsA(ident, liquidDefClass_)) k = bridge::Kind::Liquid;
    else if (offIdentIsPlort_ && unity::Field<bool>(ident, offIdentIsPlort_)) k = bridge::Kind::Plort;
    else if (offIdentIsAnimal_ && unity::Field<bool>(ident, offIdentIsAnimal_)) k = bridge::Kind::Animal;
    else if (offIdentCategories_) {
        void* cats = unity::Field<void*>(ident, offIdentCategories_);
        for (size_t i = 0, n = unity::ListCount(cats); i < n && k == bridge::Kind::Other; ++i) {
            auto* cat = static_cast<Il2CppObject*>(unity::ListAt(cats, i));
            if (!cat || !unity::IsAlive(cat)) continue;
            std::string name = unity::GetName(cat);
            for (char& ch : name) ch = static_cast<char>((ch >= 'A' && ch <= 'Z') ? ch - 'A' + 'a' : ch);
            if (name.find("food") != std::string::npos || name.find("veggie") != std::string::npos ||
                name.find("fruit") != std::string::npos || name.find("meat") != std::string::npos)
                k = bridge::Kind::Food;
            else if (name.find("craft") != std::string::npos || name.find("resource") != std::string::npos)
                k = bridge::Kind::Resource;
        }
    }
    kindCache_[key] = k;
    return k;
}

// Items a vanilla vacpack can hold: IdentifiableType assets whose prefab has a
// normal-size Vacuumable (largos and other LARGE / GIANT ones are only held,
// never stored), excluding the player and scene objects, and that at least
// one slot's AmmoSlotDefinition.IsAllowed accepts. Rebuilt only when the slot
// definitions change; the entries are kept alive with GC handles.
bool Sandbox::EnsureCatalog(const Ctx& c) {
    std::vector<Il2CppObject*> defs;
    const size_t n = std::min<size_t>(c.slots ? unity::ArrayLength(c.slots) : 0, kMaxSlots);
    for (size_t i = 0; i < n; ++i) {
        Il2CppObject* s = SlotAt(c, static_cast<int>(i));
        Il2CppObject* d = s ? unity::Field<Il2CppObject*>(s, offSlotDef_) : nullptr;
        defs.push_back(IsA(d, slotDefClass_) && unity::IsAlive(d) ? d : nullptr);
    }
    if (catalog_ && defs == catalogDefs_) return false;
    // Not readable right now: try again on a later refresh (no symbols: build it empty).
    Il2CppObject* index = SaveIndex();
    if (!index && saveWhy_.empty()) return false;

    const unsigned long long t0 = GetTickCount64();
    std::vector<Il2CppGCHandle> old;
    for (auto& [id, e] : catalogById_) old.push_back(e.handle);
    catalogById_.clear();
    if (!old.empty())
        main_thread::Post([old] {
            for (Il2CppGCHandle h : old) unity::FreeGCHandle(h);
        });

    auto items = std::make_shared<std::vector<bridge::Item>>();
    std::vector<std::string> leftOut;
    size_t scanned = 0;
    for (Il2CppObject* o : unity::FindObjectsOfTypeAll(identClass_)) {
        if (sr2::core::ModuleFaulted(kModule)) break;
        if (!IsA(o, identClass_) || !unity::IsAlive(o)) continue;
        ++scanned;
        if (unity::Field<bool>(o, offIdentIsPlayer_) || unity::Field<bool>(o, offIdentIsScene_)) continue;
        Il2CppObject* prefab = unity::Field<Il2CppObject*>(o, offIdentPrefab_);
        if (!prefab || !unity::IsAlive(prefab)) continue;
        Il2CppObject* vac = unity::GetComponent(prefab, vacClass_);
        if (!vac || !IsA(vac, vacClass_) || unity::Field<int32_t>(vac, offVacSize_) != 0) continue;
        uint32_t mask = 0;
        for (size_t i = 0; i < defs.size(); ++i) {
            bool allowed = false;
            if (defs[i] && CallValue(isAllowed_, defs[i], {o}, "AmmoSlotDefinition.IsAllowed", allowed) && allowed)
                mask |= 1u << i;
        }
        if (!mask) continue;
        if (!Saveable(index, o)) {
            leftOut.push_back(unity::GetName(o));
            continue;
        }
        Il2CppGCHandle h = unity::NewGCHandle(o);
        if (!h) continue;
        const auto id = reinterpret_cast<uintptr_t>(o);
        catalogById_[id] = CatalogEntry{h, o};
        bridge::Item item{id, ItemName(o), mask};
        item.icon = ItemIcon(o, &item.icon2);
        item.kind = ItemKind(o);
        items->push_back(std::move(item));
    }
    // Same display name twice (variants): add the asset name.
    std::map<std::string, int> seen;
    for (const auto& it : *items) ++seen[it.name];
    for (auto& it : *items)
        if (seen[it.name] > 1)
            if (Il2CppObject* o = CatalogItem(it.id, nullptr)) it.name += " (" + unity::GetName(o) + ")";
    std::sort(items->begin(), items->end(), [](const bridge::Item& a, const bridge::Item& b) { return a.name < b.name; });
    catalog_ = std::move(items);
    catalogDefs_ = std::move(defs);
    SR2_INFO("[{}] vacpack item list: {} of {} identifiable types fit {} slot(s) ({} ms)", kModule, catalog_->size(),
             scanned, catalogDefs_.size(), GetTickCount64() - t0);
    LogLeftOut("vacpack item list", leftOut);
    return true;
}

Il2CppObject* Sandbox::CatalogItem(uintptr_t id, uint32_t* mask) {
    auto it = catalogById_.find(id);
    if (it == catalogById_.end()) return nullptr;
    Il2CppObject* o = unity::GCHandleTarget(it->second.handle);
    if (o != it->second.obj || !IsA(o, identClass_) || !unity::IsAlive(o)) return nullptr;
    if (mask) {
        *mask = 0;
        if (catalog_)
            for (const auto& i : *catalog_)
                if (i.id == id) *mask = i.slotMask;
    }
    return o;
}

// An entry of the spawn or recipe list, re-read through its GC handle and checked.
Il2CppObject* Sandbox::Lookup(const std::unordered_map<uintptr_t, CatalogEntry>& map, uintptr_t id, Il2CppClass* k) {
    auto it = map.find(id);
    if (it == map.end()) return nullptr;
    Il2CppObject* o = unity::GCHandleTarget(it->second.handle);
    if (o != it->second.obj || !IsA(o, k) || !unity::IsAlive(o)) return nullptr;
    return o;
}

Il2CppObject* Sandbox::GameContextObj() {
    bool ok = false;
    Il2CppObject* gc = Call(gameCtxInstance_, nullptr, {}, "GameContext.Instance", &ok);
    return ok && gc && unity::IsAlive(gc) ? gc : nullptr;
}

// The dictionary the save code looks identifiable types up in:
// GameContext.AutoSaveDirector._saveReferenceTranslation._identifiableTypeToPersistenceId
// ._reverseIndex, a Dictionary<string, int> keyed by IdentifiableType.ReferenceId.
// GameModelPullHelpers.BuildActorData (and the other Pull* helpers) index it for
// every actor, ammo slot and gadget, so one type missing from it makes every save
// throw KeyNotFoundException (seen with a spawned Glitch Tarr). The game fills it
// from AutoSaveDirectorConfiguration._identifiableTypes. nullptr if not readable.
Il2CppObject* Sandbox::SaveIndex() {
    if (!saveWhy_.empty()) return nullptr;
    const auto& a = api();
    Il2CppObject* gc = GameContextObj();
    Il2CppObject* asd = gc ? unity::Field<Il2CppObject*>(gc, offGcAutoSave_) : nullptr;
    if (!asd || !unity::IsAlive(asd)) return nullptr;
    Il2CppObject* srt = unity::Field<Il2CppObject*>(asd, offAsdTranslation_);
    if (!IsA(srt, saveRefClass_)) return nullptr;
    Il2CppObject* table = unity::Field<Il2CppObject*>(srt, offSrtIdentTable_);
    if (!table || !a.il2cpp_object_get_class || !a.il2cpp_class_get_name) return nullptr;
    auto named = [&](Il2CppClass* k, std::string_view prefix) {
        const char* n = k ? a.il2cpp_class_get_name(k) : nullptr;
        return n && std::string_view(n).starts_with(prefix);
    };
    // Generic instances: resolved from the live objects, once per class.
    Il2CppClass* tk = a.il2cpp_object_get_class(table);
    if (tk != tableClass_) {
        if (!named(tk, "PersistenceIdLookupTable")) return nullptr;
        auto o = Resolver::Get().FieldOffset(tk, "_reverseIndex");
        if (!o) return nullptr;
        tableClass_ = tk;
        offTableReverse_ = *o;
    }
    Il2CppObject* dict = unity::Field<Il2CppObject*>(table, offTableReverse_);
    if (!dict) return nullptr;
    Il2CppClass* dk = a.il2cpp_object_get_class(dict);
    if (dk != dictClass_) {
        containsKey_ = named(dk, "Dictionary") ? Resolver::Get().Method(dk, "ContainsKey", 1) : nullptr;
        dictClass_ = containsKey_ ? dk : nullptr;
        if (!containsKey_) return nullptr;
    }
    return dict;
}

// True if the save code can store `ident`: its ReferenceId is a key of the save index.
bool Sandbox::Saveable(Il2CppObject* index, Il2CppObject* ident) {
    if (!index || !ident || !containsKey_) return false;
    bool ok = false;
    Il2CppObject* ref = Call(referenceId_, ident, {}, "IdentifiableType.ReferenceId", &ok);
    if (!ok || !ref) return false;
    bool has = false;
    return CallValue(containsKey_, index, {ref}, "save index ContainsKey", has) && has;
}

void Sandbox::LogLeftOut(const char* list, const std::vector<std::string>& names) {
    if (names.empty()) return;
    std::string s;
    for (const std::string& n : names) s += (s.empty() ? "" : ", ") + n;
    SR2_INFO("[{}] {}: left out {} type(s) the save can't store: {}", kModule, list, names.size(), s);
}

// The save's "Tarr" game setting is off (GameContext.GameSettingsDirector.TarrEnabled).
bool Sandbox::TarrsOff() {
    if (!tarrEnabled_) return false;
    Il2CppObject* gc = GameContextObj();
    Il2CppObject* settings = gc ? unity::Field<Il2CppObject*>(gc, offGcSettings_) : nullptr;
    if (!settings || !unity::IsAlive(settings)) return false;
    bool on = true;
    return CallValue(tarrEnabled_, settings, {}, "GameSettingsDirector.TarrEnabled", on) && !on;
}

// The game's own list of Tarr types (SceneContext.GameModel.tarrTypeGroup), nullptr if not loaded.
Il2CppObject* Sandbox::TarrGroup() {
    if (!groupIsMember_) return nullptr;
    Il2CppObject* scene = nullptr;
    bool found = false;
    if (!CallValue(tryGetSceneContext_, nullptr, {&scene}, "SceneContextUtility.TryGetSceneContext", found) || !found ||
        !scene || !unity::IsAlive(scene))
        return nullptr;
    Il2CppObject* model = unity::Field<Il2CppObject*>(scene, offScGameModel_);
    if (!model || !unity::IsAlive(model)) return nullptr;
    Il2CppObject* group = unity::Field<Il2CppObject*>(model, offGmTarrGroup_);
    return IsA(group, groupClass_) && unity::IsAlive(group) ? group : nullptr;
}

// What the spawner offers: IdentifiableType assets whose prefab is an actor
// (has an IdentifiableActor), except the player, scene objects (gordos and the
// like), gadgets (placed through gadget mode), liquids and anything the save
// code can't store (SaveIndex). Built once.
bool Sandbox::EnsureSpawnables() {
    if (spawnTried_ || !spawnWhy_.empty()) return false;
    Il2CppObject* index = SaveIndex();
    if (!index) return false;  // not readable right now: try again on a later refresh
    spawnTried_ = true;
    const unsigned long long t0 = GetTickCount64();
    auto items = std::make_shared<std::vector<bridge::Item>>();
    std::vector<std::string> leftOut;
    Il2CppObject* tarrs = TarrGroup();
    size_t scanned = 0;
    for (Il2CppObject* o : unity::FindObjectsOfTypeAll(identClass_)) {
        if (sr2::core::ModuleFaulted(kModule)) break;
        if (!IsA(o, identClass_) || !unity::IsAlive(o)) continue;
        ++scanned;
        if (unity::Field<bool>(o, offIdentIsPlayer_) || unity::Field<bool>(o, offIdentIsScene_)) continue;
        if ((gadgetDefClass_ && IsA(o, gadgetDefClass_)) || (liquidDefClass_ && IsA(o, liquidDefClass_))) continue;
        Il2CppObject* prefab = unity::Field<Il2CppObject*>(o, offIdentPrefab_);
        if (!prefab || !unity::IsAlive(prefab)) continue;
        Il2CppObject* actor = unity::GetComponent(prefab, actorClass_);
        if (!actor || !IsA(actor, actorClass_)) continue;
        if (!Saveable(index, o)) {
            leftOut.push_back(unity::GetName(o));
            continue;
        }
        Il2CppGCHandle h = unity::NewGCHandle(o);
        if (!h) continue;
        const auto id = reinterpret_cast<uintptr_t>(o);
        spawnById_[id] = CatalogEntry{h, o};
        bool tarr = false;
        if (tarrs) CallValue(groupIsMember_, tarrs, {o}, "IdentifiableTypeGroup.IsMember", tarr);
        bridge::Item item{id, ItemName(o)};
        item.icon = ItemIcon(o, &item.icon2);
        item.kind = ItemKind(o);
        item.tarr = tarr;
        items->push_back(std::move(item));
    }
    std::map<std::string, int> seen;
    for (const auto& it : *items) ++seen[it.name];
    for (auto& it : *items)
        if (seen[it.name] > 1)
            if (Il2CppObject* o = Lookup(spawnById_, it.id, identClass_)) it.name += " (" + unity::GetName(o) + ")";
    std::sort(items->begin(), items->end(), [](const bridge::Item& a, const bridge::Item& b) { return a.name < b.name; });
    spawnables_ = std::move(items);
    SR2_INFO("[{}] spawn list: {} of {} identifiable types are actors ({} ms)", kModule, spawnables_->size(), scanned,
             GetTickCount64() - t0);
    LogLeftOut("spawn list", leftOut);
    return true;
}

// "20 x Pink Plort, 150 Newbucks": the recipe's ident and currency costs as the fabricator charges them.
std::string Sandbox::RecipeCost(Il2CppObject* def) {
    std::string out;
    auto add = [&](const std::string& s) { out += (out.empty() ? "" : ", ") + s; };
    char* costs = reinterpret_cast<char*>(def) + offCraftCosts_;
    void* ident = *reinterpret_cast<void**>(costs + offCostIdent_);
    if (const size_t n = unity::ListCount(ident)) {
        size_t offItems = 0;
        Il2CppClass* lk = api().il2cpp_object_get_class(static_cast<Il2CppObject*>(ident));
        if (auto o = Resolver::Get().FieldOffset(lk, "_items")) offItems = *o;
        void* arr = offItems ? unity::Field<void*>(ident, offItems) : nullptr;
        char* data = arr ? unity::ArrayData<char>(arr) : nullptr;
        for (size_t i = 0; data && i < n && i < unity::ArrayLength(arr); ++i) {
            char* e = data + i * identCostStride_;
            const int amount = *reinterpret_cast<int32_t*>(e + offIdentCostAmount_);
            auto* type = *reinterpret_cast<Il2CppObject**>(e + offIdentCostType_);
            if (type && amount > 0) add(std::format("{} x {}", amount, ItemName(type)));
        }
    }
    void* cur = *reinterpret_cast<void**>(costs + offCostCurrency_);
    for (size_t i = 0, n = unity::ListCount(cur); i < n; ++i) {
        auto* e = static_cast<Il2CppObject*>(unity::ListAt(cur, i));
        if (!e) continue;
        const int amount = unity::Field<int32_t>(e, offCurCostAmount_);
        Il2CppObject* c = unity::Field<Il2CppObject*>(e, offCurCostCurrency_);
        if (amount <= 0) continue;
        const bool bucks = c && IsA(c, currencyClass_) && unity::Field<int32_t>(c, offPersistenceId_) == kNewbuckPersistenceId;
        add(std::format("{} {}", Thousands(amount), bucks ? std::string("Newbucks") : c ? unity::GetName(c) : std::string("?")));
    }
    return out;
}

Il2CppObject* Sandbox::GadgetDirector() {
    Il2CppObject* dir = nullptr;
    bool found = false;
    if (!CallValue(tryGetGadgetDir_, nullptr, {&dir}, "SceneContextUtility.TryGetGadgetDirector", found) || !found || !dir ||
        !unity::IsAlive(dir))
        return nullptr;
    return dir;
}

// The fabricator's gadget recipes: every GadgetDefinition asset with a crafting
// cost, grouped by the fabricator's own categories when its menu asset is loaded.
// Built once; blueprint state and owned counts refresh every 2 s (RefreshRecipeState).
bool Sandbox::EnsureRecipes() {
    if (recipesTried_ || !craftWhy_.empty()) return false;
    Il2CppObject* index = SaveIndex();
    if (!index) return false;  // not readable right now: try again on a later refresh
    recipesTried_ = true;
    std::vector<std::string> leftOut;
    const unsigned long long t0 = GetTickCount64();
    struct Cat {
        Il2CppObject* obj;
        int index;
    };
    std::vector<Cat> cats;
    auto titles = std::make_shared<std::vector<std::string>>();
    auto catIcons = std::make_shared<std::vector<uint32_t>>();
    if (fabMenuClass_) {
        for (Il2CppObject* menu : unity::FindObjectsOfTypeAll(fabMenuClass_)) {
            if (!IsA(menu, fabMenuClass_) || !unity::IsAlive(menu)) continue;
            void* list = unity::Field<void*>(menu, offFabCategories_);
            for (size_t i = 0, n = unity::ListCount(list); i < n; ++i) {
                auto* c = static_cast<Il2CppObject*>(unity::ListAt(list, i));
                if (!c || !IsA(c, fabGadgetCatClass_) || !unity::IsAlive(c)) continue;
                std::string t = Localized(unity::Field<Il2CppObject*>(c, offFabCatTitle_));
                if (t.empty()) t = unity::GetName(c);
                cats.push_back({c, static_cast<int>(titles->size())});
                titles->push_back(t);
                catIcons->push_back(offFabCatIcon_ ? SpriteIcon(unity::Field<Il2CppObject*>(c, offFabCatIcon_)) : 0);
            }
            if (!cats.empty()) break;  // one menu is enough
        }
    }
    auto list = std::make_shared<std::vector<bridge::Recipe>>();
    for (Il2CppObject* d : unity::FindObjectsOfTypeAll(gadgetDefClass_)) {
        if (sr2::core::ModuleFaulted(kModule)) break;
        if (!IsA(d, gadgetDefClass_) || !unity::IsAlive(d)) continue;
        char* costs = reinterpret_cast<char*>(d) + offCraftCosts_;
        const size_t nIdent = unity::ListCount(*reinterpret_cast<void**>(costs + offCostIdent_));
        const size_t nCur = unity::ListCount(*reinterpret_cast<void**>(costs + offCostCurrency_));
        if (nIdent == 0 && nCur == 0) continue;  // not crafted at the fabricator
        if (!Saveable(index, d)) {
            leftOut.push_back(unity::GetName(d));
            continue;
        }
        bridge::Recipe r;
        r.id = reinterpret_cast<uintptr_t>(d);
        r.name = ItemName(d);
        r.icon = ItemIcon(d);
        r.cost = RecipeCost(d);
        r.perCraft = unity::Field<bool>(d, offBuyInPairs_) ? 2 : 1;
        for (const Cat& c : cats) {
            bool in = false;
            if (CallValue(fabCatContains_, c.obj, {d}, "GadgetFabricateCategory.Contains", in) && in) {
                r.category = c.index;
                break;
            }
        }
        Il2CppGCHandle h = unity::NewGCHandle(d);
        if (!h) continue;
        recipeById_[r.id] = CatalogEntry{h, d};
        list->push_back(std::move(r));
    }
    std::sort(list->begin(), list->end(), [](const bridge::Recipe& a, const bridge::Recipe& b) { return a.name < b.name; });
    recipes_ = std::move(list);
    recipeCats_ = std::move(titles);
    recipeCatIcons_ = std::move(catIcons);
    lastRecipeState_ = 0;
    SR2_INFO("[{}] crafter: {} gadget recipes in {} fabricator categories ({} ms)", kModule, recipes_->size(),
             recipeCats_->size(), GetTickCount64() - t0);
    LogLeftOut("crafter", leftOut);
    return true;
}

// Blueprint unlocked and how many are in the gadget inventory, per recipe. Copies the
// list only when something changed, so the tab's shared_ptr stays the same otherwise.
void Sandbox::RefreshRecipeState() {
    Il2CppObject* dir = GadgetDirector();
    if (!dir) return;
    if (recipes_ && !recipes_->empty()) {
        auto next = std::make_shared<std::vector<bridge::Recipe>>(*recipes_);
        bool changed = false;
        for (bridge::Recipe& r : *next) {
            Il2CppObject* d = Lookup(recipeById_, r.id, gadgetDefClass_);
            if (!d) continue;
            bool unlocked = false;
            int owned = 0;
            CallValue(hasBlueprint_, dir, {d}, "GadgetDirector.HasBlueprint", unlocked);
            CallValue(gadgetCount_, dir, {d}, "GadgetDirector.GetItemCount", owned);
            if (unlocked != r.unlocked || owned != r.owned) changed = true;
            r.unlocked = unlocked;
            r.owned = owned;
        }
        if (changed) recipes_ = std::move(next);
    }
    if (materials_ && !materials_->empty()) {
        auto next = std::make_shared<std::vector<bridge::Recipe>>(*materials_);
        bool changed = false;
        for (bridge::Recipe& r : *next) {
            Il2CppObject* t = Lookup(materialById_, r.id, identClass_);
            int owned = 0;
            if (!t || !CallValue(gadgetCount_, dir, {t}, "GadgetDirector.GetItemCount", owned)) continue;
            changed = changed || owned != r.owned;
            r.owned = owned;
        }
        if (changed) materials_ = std::move(next);
    }
}

// Crafting materials for the crafter's own section: every IdentifiableType the gadget
// director stores in the refinery (GadgetDirector.IsStorable), except gadgets, that the
// save can store. Built once, in game (needs the gadget director).
bool Sandbox::EnsureMaterials() {
    if (materialsTried_ || !craftWhy_.empty() || !isStorable_ || !refinerySpace_) return false;
    Il2CppObject* index = SaveIndex();
    Il2CppObject* dir = GadgetDirector();
    if (!index || !dir) return false;  // try again on a later refresh
    materialsTried_ = true;
    const unsigned long long t0 = GetTickCount64();
    auto list = std::make_shared<std::vector<bridge::Recipe>>();
    std::vector<std::string> leftOut;
    for (Il2CppObject* o : unity::FindObjectsOfTypeAll(identClass_)) {
        if (sr2::core::ModuleFaulted(kModule)) break;
        if (!IsA(o, identClass_) || !unity::IsAlive(o) || (gadgetDefClass_ && IsA(o, gadgetDefClass_))) continue;
        if (unity::Field<bool>(o, offIdentIsPlayer_) || unity::Field<bool>(o, offIdentIsScene_)) continue;
        bool storable = false;
        if (!CallValue(isStorable_, dir, {o}, "GadgetDirector.IsStorable", storable) || !storable) continue;
        if (!Saveable(index, o)) {
            leftOut.push_back(unity::GetName(o));
            continue;
        }
        Il2CppGCHandle h = unity::NewGCHandle(o);
        if (!h) continue;
        bridge::Recipe r;
        r.id = reinterpret_cast<uintptr_t>(o);
        r.name = ItemName(o);
        r.icon = ItemIcon(o);
        r.kind = ItemKind(o);
        r.unlocked = true;
        CallValue(gadgetCount_, dir, {o}, "GadgetDirector.GetItemCount", r.owned);
        materialById_[r.id] = CatalogEntry{h, o};
        list->push_back(std::move(r));
    }
    std::sort(list->begin(), list->end(), [](const bridge::Recipe& a, const bridge::Recipe& b) { return a.name < b.name; });
    materials_ = std::move(list);
    SR2_INFO("[{}] crafter: {} crafting materials the refinery stores ({} ms)", kModule, materials_->size(),
             GetTickCount64() - t0);
    LogLeftOut("crafting materials", leftOut);
    return true;
}

// ---------------------------------------------------------------- refresh ----
void Sandbox::Refresh() {
    bridge::Snapshot s;
    s.tick = GetTickCount64();
    s.infiniteEnergy = infinite_.load();
    Ctx c;
    if (!Live()) {
        s.why = "Sandbox is off.";
    } else if (!Context(c, s.why)) {
        // s.why set
    } else {
        s.inGame = true;
        if (c.currency) {
            s.haveNewbucks = CallValue(getCurrency_, c.player, {c.currency}, "PlayerState.GetCurrency", s.newbucks);
            s.maxNewbucks = unity::Field<int32_t>(c.model, offMaxCurrency_);
        }
        CallValue(getHealth_, c.player, {}, "PlayerState.GetCurrHealth", s.health);
        CallValue(getMaxHealth_, c.player, {}, "PlayerState.GetMaxHealth", s.maxHealth);
        CallValue(getEnergy_, c.player, {}, "PlayerState.GetCurrEnergy", s.energy);
        CallValue(getMaxEnergy_, c.player, {}, "PlayerState.GetMaxEnergy", s.maxEnergy);
        CallValue(getRad_, c.player, {}, "PlayerState.GetCurrRad", s.rads);
        const size_t n = std::min<size_t>(c.slots ? unity::ArrayLength(c.slots) : 0, kMaxSlots);
        for (size_t i = 0; i < n; ++i) {
            bridge::Slot slot;
            int idx = static_cast<int>(i);
            if (Il2CppObject* a = SlotAt(c, idx)) {
                CallValue(slotUnlocked_, a, {}, "AmmoSlot.IsUnlocked", slot.unlocked);
                CallValue(slotMax_, c.ammo, {&idx}, "AmmoSlotManager.GetSlotMaxCount", slot.max);
                slot.count = unity::Field<int32_t>(a, offSlotCount_);
                Il2CppObject* id = unity::Field<Il2CppObject*>(a, offSlotId_);
                if (id && slot.count > 0) {
                    slot.item = reinterpret_cast<uintptr_t>(id);
                    slot.name = ItemName(id);
                    slot.icon = ItemIcon(id, &slot.icon2);
                    slot.kind = ItemKind(id);
                } else {
                    slot.count = 0;
                }
                if (!slot.unlocked) slot.unlockable = SlotUnlockUpgrade(c, a, nullptr) != nullptr;
            }
            s.slots.push_back(std::move(slot));
        }
        // At most one list is built per refresh, so the one-time scans don't stack into one long frame.
        bool built = c.slots && EnsureCatalog(c);
        s.catalog = catalog_;
        if (!built) built = EnsureSpawnables();
        s.spawnables = spawnables_;
        s.spawnWhy = spawnWhy_;
        s.tarrsOff = spawnables_ && TarrsOff();
        if (!built) built = EnsureRecipes();
        if (!built) built = EnsureMaterials();
        // Cheap one-time extras, each in its own refresh after the lists.
        if (!built && spawnables_ && !relations_) EnsureRelations(), built = true;
        if (!built && spawnables_ && !groupIconsBuilt_) EnsureGroupIcons();
        if ((recipes_ || materials_) && s.tick - lastRecipeState_ >= kRecipeStateMs) {
            lastRecipeState_ = s.tick;
            RefreshRecipeState();
        }
        s.recipes = recipes_;
        s.recipeCategories = recipeCats_;
        s.recipeCategoryIcons = recipeCatIcons_;
        s.materials = materials_;
        s.craftWhy = craftWhy_;
        s.relations = relations_;
        std::copy(std::begin(groupIcons_), std::end(groupIcons_), std::begin(s.groupIcons));
    }
    bridge::Publish(std::move(s));
}

void Sandbox::TopUpEnergy() {
    Ctx c;
    std::string why;
    if (!Context(c, why)) return;
    int cur = 0, max = 0;
    if (!CallValue(getEnergy_, c.player, {}, "PlayerState.GetCurrEnergy", cur) ||
        !CallValue(getMaxEnergy_, c.player, {}, "PlayerState.GetMaxEnergy", max))
        return;
    if (max > 0 && cur < max) Call(setEnergy_, c.player, {&max}, "PlayerState.SetEnergy");
}

void Sandbox::Tick() {
    if (!Live()) return;
    const unsigned long long now = GetTickCount64();
    if (infinite_.load() && now - lastEnergy_ >= kEnergyMs) {
        lastEnergy_ = now;
        TopUpEnergy();
    }
    if (bridge::WantsSnapshot() && now - lastSnap_ >= kSnapshotMs) {
        lastSnap_ = now;
        Refresh();
    }
}

void Sandbox::Done(bool ok, const std::string& text) {
    if (ok) SR2_INFO("[{}] {}", kModule, text);
    else SR2_WARN("[{}] {}", kModule, text);
    bridge::SetStatus(text, !ok);
}

// ---------------------------------------------------------------- actions ----
void Sandbox::Handle(const bridge::Action& a) {
    if (!Live()) {
        bridge::SetStatus("Sandbox is off.", true);
        return;
    }
    if (a.op == bridge::Op::InfiniteEnergy) {
        const bool on = a.value != 0;
        if (infinite_.exchange(on) != on)
            Done(true, on ? "Infinite energy on (energy is refilled to its maximum; not saved, off after a restart)"
                          : "Infinite energy off");
        Refresh();
        return;
    }
    Ctx c;
    std::string why;
    if (!Context(c, why)) {
        Done(false, "Not applied: " + why);
        Refresh();
        return;
    }
    switch (a.op) {
        case bridge::Op::SetNewbucks: DoNewbucks(c, a.value, "set"); break;
        case bridge::Op::AddNewbucks: {
            int cur = 0;
            if (c.currency && CallValue(getCurrency_, c.player, {c.currency}, "PlayerState.GetCurrency", cur))
                DoNewbucks(c, static_cast<int64_t>(cur) + a.value, a.value >= 0 ? "add" : "subtract");
            else
                Done(false, "Newbucks not changed: the Newbucks currency was not found");
            break;
        }
        case bridge::Op::RefillHealth:
        case bridge::Op::RefillEnergy: {
            const bool health = a.op == bridge::Op::RefillHealth;
            int cur = 0, max = 0;
            const char* what = health ? "Health" : "Energy";
            if (!CallValue(health ? getHealth_ : getEnergy_, c.player, {}, "PlayerState.GetCurr", cur) ||
                !CallValue(health ? getMaxHealth_ : getMaxEnergy_, c.player, {}, "PlayerState.GetMax", max) || max <= 0) {
                Done(false, std::format("{} not refilled: could not read it", what));
                break;
            }
            bool ok = false;
            Call(health ? setHealth_ : setEnergy_, c.player, {&max}, health ? "PlayerState.SetHealth" : "PlayerState.SetEnergy",
                 &ok);
            Done(ok, ok ? std::format("{} refilled: {} -> {}", what, cur, max) : std::format("{} not refilled", what));
            break;
        }
        case bridge::Op::ClearRads: {
            int cur = 0, zero = 0;
            CallValue(getRad_, c.player, {}, "PlayerState.GetCurrRad", cur);
            bool ok = false;
            Call(setRad_, c.player, {&zero}, "PlayerState.SetRad", &ok);
            Done(ok, ok ? std::format("Radiation cleared: {} -> 0", cur) : "Radiation not cleared");
            break;
        }
        case bridge::Op::SetSlotCount: DoSlotCount(c, a.slot, a.value); break;
        case bridge::Op::PutItem: DoPutItem(c, a.slot, a.item, a.value); break;
        case bridge::Op::Spawn: DoSpawn(c, a.item, a.value); break;
        case bridge::Op::Craft:
            DoCraft(a.item, a.value);
            lastRecipeState_ = 0;  // show the new owned count at once
            break;
        case bridge::Op::AddMaterial:
            DoAddMaterial(a.item, a.value);
            lastRecipeState_ = 0;
            break;
        case bridge::Op::MoveSlot: DoMoveSlot(c, a.slot, static_cast<int>(a.value)); break;
        case bridge::Op::UnlockSlot: DoUnlockSlot(c, a.slot); break;
        case bridge::Op::InfiniteEnergy: break;  // handled above
    }
    Refresh();
}

// Newbucks to `target`, clamped to [0, PlayerModel.maxCurrency]: more through
// PlayerState.AddCurrency (as a pickup or sale does; it also counts towards
// "Newbucks ever collected"), less through PlayerState.SpendCurrency (as a
// purchase does). Both show the game's own coin popup.
void Sandbox::DoNewbucks(const Ctx& c, int64_t target, const char* how) {
    if (!c.currency) {
        Done(false, "Newbucks not changed: the Newbucks currency was not found");
        return;
    }
    int cur = 0;
    if (!CallValue(getCurrency_, c.player, {c.currency}, "PlayerState.GetCurrency", cur)) {
        Done(false, "Newbucks not changed: could not read the current amount");
        return;
    }
    const int max = unity::Field<int32_t>(c.model, offMaxCurrency_);
    if (max <= 0) {
        Done(false, "Newbucks not changed: the maximum is unknown");
        return;
    }
    const int64_t clamped = std::clamp<int64_t>(target, 0, max);
    const int delta = static_cast<int>(clamped - cur);
    if (delta == 0) {
        Done(true, std::format("Newbucks unchanged ({}{})", Thousands(cur),
                               clamped != target ? std::format(", limit 0..{}", Thousands(max)) : std::string()));
        return;
    }
    bool ok = false;
    if (delta > 0) {
        int adjust = delta;
        bool popup = true;
        Call(addCurrency_, c.player, {c.currency, &adjust, &popup, nullptr}, "PlayerState.AddCurrency", &ok);
    } else {
        int adjust = -delta;
        Call(spendCurrency_, c.player, {c.currency, &adjust, nullptr}, "PlayerState.SpendCurrency", &ok);
    }
    int now = cur;
    CallValue(getCurrency_, c.player, {c.currency}, "PlayerState.GetCurrency", now);
    Done(ok, ok ? std::format("Newbucks {}: {} -> {}{}", how, Thousands(cur), Thousands(now),
                              clamped != target ? std::format(" (clamped to 0..{})", Thousands(max)) : std::string())
                : std::format("Newbucks not changed ({} failed)", delta > 0 ? "AddCurrency" : "SpendCurrency"));
}

// Count of what a slot holds, clamped to [0, GetSlotMaxCount]: up through
// AmmoSlotManager.MaybeAddResource (no overflow: it refuses more than the
// slot's maximum and anything the slot does not accept), down through
// AmmoSlotManager.Decrement (which clears the slot at 0).
void Sandbox::DoSlotCount(const Ctx& c, int idx, int64_t value) {
    Il2CppObject* slot = SlotAt(c, idx);
    if (!slot || !c.ammo) {
        Done(false, std::format("Slot {} not changed: no such slot", idx + 1));
        return;
    }
    bool unlocked = false;
    if (!CallValue(slotUnlocked_, slot, {}, "AmmoSlot.IsUnlocked", unlocked) || !unlocked) {
        Done(false, std::format("Slot {} not changed: the slot is locked", idx + 1));
        return;
    }
    Il2CppObject* id = unity::Field<Il2CppObject*>(slot, offSlotId_);
    const int cur = unity::Field<int32_t>(slot, offSlotCount_);
    if (!id || cur <= 0) {
        Done(false, std::format("Slot {} is empty: pick an item to put in it", idx + 1));
        return;
    }
    int max = 0;
    if (!CallValue(slotMax_, c.ammo, {&idx}, "AmmoSlotManager.GetSlotMaxCount", max) || max <= 0) {
        Done(false, std::format("Slot {} not changed: its maximum is unknown", idx + 1));
        return;
    }
    const int target = static_cast<int>(std::clamp<int64_t>(value, 0, max));
    const std::string name = ItemName(id);
    if (target == cur) {
        Done(true, std::format("Slot {} unchanged: {} x {}", idx + 1, cur, name));
        return;
    }
    bool ok = false;
    if (target > cur) {
        int add = target - cur;
        bool overflow = false, added = false;
        ok = CallValue(addResource_, c.ammo, {id, &idx, &add, &overflow}, "AmmoSlotManager.MaybeAddResource", added) &&
             added;
    } else {
        int sub = cur - target;
        Call(decrement_, c.ammo, {&idx, &sub}, "AmmoSlotManager.Decrement", &ok);
    }
    const int now = unity::Field<int32_t>(slot, offSlotCount_);
    Done(ok, ok ? std::format("Slot {}: {} {} -> {}", idx + 1, name, cur, target == 0 ? 0 : now)
                : std::format("Slot {} not changed: the game refused {} x {}", idx + 1, target, name));
}

// Replaces a slot's contents with `value` of a catalog item: empties it first
// if it holds something else (Decrement), then MaybeAddResource, which checks
// AmmoSlotDefinition.IsAllowed and the slot's maximum itself.
void Sandbox::DoPutItem(const Ctx& c, int idx, uintptr_t item, int64_t value) {
    uint32_t mask = 0;
    Il2CppObject* ident = CatalogItem(item, &mask);
    if (!ident) {
        Done(false, "Item not put: it is not in the vacpack item list (anymore)");
        return;
    }
    const std::string name = ItemName(ident);
    if (!Saveable(SaveIndex(), ident)) {
        Done(false, std::format("{} not put: the game can't save it", name));
        return;
    }
    Il2CppObject* slot = SlotAt(c, idx);
    if (!slot || !c.ammo || idx >= kMaxSlots) {
        Done(false, std::format("{} not put: no slot {}", name, idx + 1));
        return;
    }
    if (!(mask & (1u << idx))) {
        Done(false, std::format("{} not put: slot {} does not accept it", name, idx + 1));
        return;
    }
    bool unlocked = false;
    if (!CallValue(slotUnlocked_, slot, {}, "AmmoSlot.IsUnlocked", unlocked) || !unlocked) {
        Done(false, std::format("{} not put: slot {} is locked", name, idx + 1));
        return;
    }
    int max = 0;
    if (!CallValue(slotMax_, c.ammo, {&idx}, "AmmoSlotManager.GetSlotMaxCount", max) || max <= 0) {
        Done(false, std::format("{} not put: the slot's maximum is unknown", name));
        return;
    }
    const int target = static_cast<int>(std::clamp<int64_t>(value, 1, max));
    Il2CppObject* curId = unity::Field<Il2CppObject*>(slot, offSlotId_);
    int cur = unity::Field<int32_t>(slot, offSlotCount_);
    std::string replaced;
    if (cur > 0 && curId != ident) {
        replaced = std::format(" (replaced {} x {})", cur, ItemName(curId));
        bool ok = false;
        int sub = cur;
        Call(decrement_, c.ammo, {&idx, &sub}, "AmmoSlotManager.Decrement", &ok);
        cur = unity::Field<int32_t>(slot, offSlotCount_);
        if (!ok || cur > 0) {
            Done(false, std::format("{} not put: slot {} could not be emptied", name, idx + 1));
            return;
        }
    }
    if (cur > 0) {  // same item already there: just change the count
        DoSlotCount(c, idx, target);
        return;
    }
    int add = target;
    bool overflow = false, added = false;
    const bool ok =
        CallValue(addResource_, c.ammo, {ident, &idx, &add, &overflow}, "AmmoSlotManager.MaybeAddResource", added) && added;
    Done(ok, ok ? std::format("Slot {}: {} x {}{}", idx + 1, target, name, replaced)
                : std::format("{} not put: the game refused it for slot {}{}", name, idx + 1, replaced));
}

bool Sandbox::GetVec3(const MethodInfo* m, Il2CppObject* obj, Vec3& out) {
    return obj && unity::IsAlive(obj) && CallValue(m, obj, {}, "Transform.get_position/forward", out);
}

// Where spawns go: the camera's position and its forward direction flattened
// onto the ground plane (the player's body if there is no main camera).
bool Sandbox::SpawnPose(const Ctx& c, Vec3& pos, Vec3& fwd) {
    Il2CppObject* tr = nullptr;
    if (Il2CppObject* cam = unity::Camera_main()) tr = unity::TransformOf(cam);
    if (!tr) tr = unity::TransformOf(c.player);
    if (!GetVec3(trPosition_, tr, pos) || !GetVec3(trForward_, tr, fwd)) return false;
    fwd.y = 0;
    const float len = std::sqrt(fwd.x * fwd.x + fwd.z * fwd.z);
    if (len < 1e-3f) {  // looking straight up or down: use the body's facing
        Vec3 body{};
        if (!GetVec3(trForward_, unity::TransformOf(c.player), body)) return false;
        fwd = {body.x, 0, body.z};
        const float l2 = std::sqrt(fwd.x * fwd.x + fwd.z * fwd.z);
        if (l2 < 1e-3f) return false;
        fwd.x /= l2, fwd.z /= l2;
    } else {
        fwd.x /= len, fwd.z /= len;
    }
    return std::isfinite(pos.x) && std::isfinite(pos.y) && std::isfinite(pos.z);
}

// Spawns `count` of a spawn-list item in front of the player through
// InstantiationHelpers.InstantiateActor, the same call the vacpack makes when
// it shoots an item out (VacuumItem.Expel): the actor model is registered with
// the game model in the current scene group, so the game saves it like any
// other loose item. A short raycast keeps spawns in front of walls.
void Sandbox::DoSpawn(const Ctx& c, uintptr_t item, int64_t count) {
    if (!spawnWhy_.empty()) {
        Done(false, "Spawner unavailable: " + spawnWhy_);
        return;
    }
    Il2CppObject* ident = Lookup(spawnById_, item, identClass_);
    if (!ident) {
        Done(false, "Not spawned: the item is not in the spawn list (anymore)");
        return;
    }
    const std::string name = ItemName(ident);
    if (!Saveable(SaveIndex(), ident)) {
        Done(false, std::format("{} not spawned: the game can't save it", name));
        return;
    }
    if (spawnables_ && TarrsOff())
        for (const bridge::Item& it : *spawnables_)
            if (it.id == item && it.tarr) {
                Done(false, std::format("{} not spawned: Tarrs are disabled in this save's game settings", name));
                return;
            }
    Il2CppObject* prefab = unity::Field<Il2CppObject*>(ident, offIdentPrefab_);
    if (!prefab || !unity::IsAlive(prefab)) {
        Done(false, std::format("{} not spawned: it has no prefab", name));
        return;
    }
    Il2CppObject* scene = nullptr;
    bool found = false;
    if (!CallValue(tryGetSceneContext_, nullptr, {&scene}, "SceneContextUtility.TryGetSceneContext", found) || !found ||
        !scene || !unity::IsAlive(scene)) {
        Done(false, std::format("{} not spawned: no scene context", name));
        return;
    }
    Il2CppObject* regions = unity::Field<Il2CppObject*>(scene, offScRegionRegistry_);
    bool ok = false;
    Il2CppObject* group = regions ? Call(currentSceneGroup_, regions, {}, "RegionRegistry.CurrentSceneGroup", &ok) : nullptr;
    if (!group || !unity::IsAlive(group)) {
        Done(false, std::format("{} not spawned: the current scene group is unknown", name));
        return;
    }
    Vec3 eye{}, fwd{};
    if (!SpawnPose(c, eye, fwd)) {
        Done(false, std::format("{} not spawned: could not read where you are looking", name));
        return;
    }
    float dist = kSpawnDist;
    if (raycast_) {
        alignas(16) unsigned char hit[64] = {};
        float maxDist = kSpawnDist + 0.6f;
        int layers = -5;  // Physics.DefaultRaycastLayers (everything but Ignore Raycast)
        int triggers = 1; // QueryTriggerInteraction.Ignore
        bool any = false;
        if (CallValue(raycast_, nullptr, {&eye, &fwd, hit, &maxDist, &layers, &triggers}, "Physics.Raycast", any) && any) {
            const float d = *reinterpret_cast<float*>(hit + offHitDistance_);
            if (std::isfinite(d)) dist = std::clamp(d - 0.6f, 0.5f, kSpawnDist);
        }
    }
    const int n = static_cast<int>(std::clamp<int64_t>(count, 1, kMaxSpawn));
    const int cols = std::min(n, 5);
    const Vec3 right{fwd.z, 0, -fwd.x};
    Quat rot{0, 0, 0, 1};
    int spawned = 0;
    for (int i = 0; i < n; ++i) {
        const float side = (static_cast<float>(i % cols) - static_cast<float>(cols - 1) * 0.5f) * 0.55f;
        Vec3 pos{eye.x + fwd.x * dist + right.x * side, eye.y - 0.3f + static_cast<float>(i / cols) * 0.7f,
                 eye.z + fwd.z * dist + right.z * side};
        bool nonActorOk = false, ignoreEmotions = false, setCollected = false;
        int appearance = 0, secondAppearance = 0;  // SlimeAppearance.AppearanceSaveSet.NONE
        alignas(16) unsigned char metadata[64] = {};  // Nullable<AmmoMetadata> without a value (hasValue = false)
        Il2CppObject* go = Call(instantiateActor_, nullptr,
                                {prefab, group, &pos, &rot, &nonActorOk, &appearance, &secondAppearance, metadata,
                                 &ignoreEmotions, &setCollected},
                                "InstantiationHelpers.InstantiateActor", &ok);
        if (!ok || !go) break;
        ++spawned;
    }
    Done(spawned > 0, spawned == n   ? std::format("Spawned {} x {}", n, name)
                      : spawned > 0  ? std::format("Spawned {} of {} x {} (the game refused the rest)", spawned, n, name)
                                     : std::format("{} not spawned: the game refused it", name));
}

// Crafts `count` of a fabricator gadget at no cost: what the fabricator's
// GadgetFabricatableItem.FabricateAndSpendCost does after spending the recipe,
// GadgetDirector.AddItem(definition, count x 2 for BuyInPairs gadgets). Only
// for unlocked blueprints and within the gadget's own limit (CanAddGadget), as
// the fabricator would allow.
void Sandbox::DoCraft(uintptr_t item, int64_t count) {
    if (!craftWhy_.empty()) {
        Done(false, "Crafter unavailable: " + craftWhy_);
        return;
    }
    Il2CppObject* def = Lookup(recipeById_, item, gadgetDefClass_);
    if (!def) {
        Done(false, "Not crafted: the gadget is not in the recipe list (anymore)");
        return;
    }
    const std::string name = ItemName(def);
    if (!Saveable(SaveIndex(), def)) {
        Done(false, std::format("{} not crafted: the game can't save it", name));
        return;
    }
    Il2CppObject* dir = GadgetDirector();
    if (!dir) {
        Done(false, std::format("{} not crafted: no gadget director (load a save)", name));
        return;
    }
    // A gadget without its blueprint goes into the inventory too: GadgetDirector.AddItem
    // (1.3.0, RVA 0x867C20) has no blueprint check; it only adds the count to the
    // gadgets model, which the save stores like any crafted gadget.
    bool unlocked = false;
    CallValue(hasBlueprint_, dir, {def}, "GadgetDirector.HasBlueprint", unlocked);
    const int crafts = static_cast<int>(std::clamp<int64_t>(count, 1, kMaxCraft));
    const int per = unity::Field<bool>(def, offBuyInPairs_) ? 2 : 1;
    int amount = crafts * per;
    bool can = false;
    if (!CallValue(canAddGadget_, dir, {def, &amount}, "GadgetDirector.CanAddGadget", can) || !can) {
        Done(false, std::format("{} not crafted: {} more would go over its limit", name, amount));
        return;
    }
    int before = 0, after = 0;
    CallValue(gadgetCount_, dir, {def}, "GadgetDirector.GetItemCount", before);
    bool ok = false;
    Call(gadgetAddItem_, dir, {def, &amount}, "GadgetDirector.AddItem", &ok);
    CallValue(gadgetCount_, dir, {def}, "GadgetDirector.GetItemCount", after);
    Done(ok && after > before, ok && after > before
                                   ? std::format("Crafted {} x {} for free (inventory {} -> {}){}", after - before, name, before,
                                                 after, unlocked ? "" : "; its blueprint is still locked")
                                   : std::format("{} not crafted: the game refused it", name));
}

// Adds `count` of a crafting material to the refinery: GadgetDirector.AddItem, which
// stores IsStorable types there and clamps to GetRefinerySpaceAvailable (999 per type).
void Sandbox::DoAddMaterial(uintptr_t item, int64_t count) {
    Il2CppObject* t = Lookup(materialById_, item, identClass_);
    if (!t) {
        Done(false, "Not added: the material is not in the list (anymore)");
        return;
    }
    const std::string name = ItemName(t);
    if (!Saveable(SaveIndex(), t)) {
        Done(false, std::format("{} not added: the game can't save it", name));
        return;
    }
    Il2CppObject* dir = GadgetDirector();
    if (!dir) {
        Done(false, std::format("{} not added: no gadget director (load a save)", name));
        return;
    }
    int space = 0;
    if (!CallValue(refinerySpace_, dir, {t}, "GadgetDirector.GetRefinerySpaceAvailable", space) || space <= 0) {
        Done(false, std::format("{} not added: the refinery is full for it", name));
        return;
    }
    int amount = static_cast<int>(std::clamp<int64_t>(count, 1, space));
    int before = 0, after = 0;
    CallValue(gadgetCount_, dir, {t}, "GadgetDirector.GetItemCount", before);
    bool ok = false;
    Call(gadgetAddItem_, dir, {t, &amount}, "GadgetDirector.AddItem", &ok);
    CallValue(gadgetCount_, dir, {t}, "GadgetDirector.GetItemCount", after);
    Done(ok && after > before, ok && after > before
                                   ? std::format("Refinery: {} {} -> {}", name, before, after)
                                   : std::format("{} not added: the game refused it", name));
}

// The upgrade the fabricator sells that unlocks `slot`: an UpgradeDefinition (from the player's
// UpgradeModel.upgradeDefinitions) with a level whose effects target the slot definition's
// _isUnlockedProperty (AmmoSlot.IsUnlocked: that property >= _isUnlockedThreshold). Field reads only.
Il2CppObject* Sandbox::SlotUnlockUpgrade(const Ctx& c, Il2CppObject* slot, Il2CppObject** upgradeModel, int* level) {
    if (!incUpgrade_ || !slot) return nullptr;
    Il2CppObject* def = unity::Field<Il2CppObject*>(slot, offSlotDef_);
    if (!IsA(def, slotDefClass_) || !unity::IsAlive(def)) return nullptr;
    Il2CppObject* prop = unity::Field<Il2CppObject*>(def, offSlotDefUnlockProp_);
    if (!prop) return nullptr;
    const int threshold = unity::Field<int32_t>(def, offSlotDefThreshold_);
    bool ok = false;
    Il2CppObject* um = Call(getUpgradeModel_, c.model, {}, "PlayerModel.GetUpgradeModel", &ok);
    if (!ok || !um) return nullptr;
    if (upgradeModel) *upgradeModel = um;
    Il2CppObject* defList = unity::Field<Il2CppObject*>(um, offUmDefs_);
    if (!defList || !unity::IsAlive(defList)) return nullptr;
    const auto& a = api();
    auto itemsOff = Resolver::Get().FieldOffset(a.il2cpp_object_get_class(defList), "items");
    void* defs = itemsOff ? unity::Field<void*>(defList, *itemsOff) : nullptr;
    for (size_t i = 0, n = unity::ListCount(defs); i < n; ++i) {
        auto* up = static_cast<Il2CppObject*>(unity::ListAt(defs, i));
        if (!IsA(up, upgradeDefClass_) || !unity::IsAlive(up)) continue;
        int firstAny = -1;
        void* levels = unity::Field<void*>(up, offUdLevels_);
        for (size_t l = 0, nl = unity::ListCount(levels); l < nl; ++l) {
            auto* lvl = static_cast<Il2CppObject*>(unity::ListAt(levels, l));
            if (!lvl) continue;
            if (!offUlEffects_) {
                auto o = Resolver::Get().FieldOffset(a.il2cpp_object_get_class(lvl), "Effects");
                if (!o) return nullptr;
                offUlEffects_ = *o;
            }
            void* effects = unity::Field<void*>(lvl, offUlEffects_);
            for (size_t e = 0, ne = unity::ListCount(effects); e < ne; ++e) {
                auto* mod = static_cast<Il2CppObject*>(unity::ListAt(effects, e));
                if (!mod) continue;
                Il2CppClass* mk = a.il2cpp_object_get_class(mod);
                if (mk != modClass_) {
                    auto o = Resolver::Get().FieldOffset(mk, "TargetProperty");
                    auto v = Resolver::Get().FieldOffset(mk, "Value");
                    if (!o || !v) continue;
                    modClass_ = mk;
                    offModTarget_ = *o;
                    offModValue_ = *v;
                }
                if (unity::Field<Il2CppObject*>(mod, offModTarget_) != prop) continue;
                // The level that unlocks it: the first whose modifier on the property reaches the
                // threshold (a level may also list the property with a lower value).
                if (firstAny < 0) firstAny = static_cast<int>(l);
                if (unity::Field<float>(mod, offModValue_) >= static_cast<float>(threshold)) {
                    if (level) *level = static_cast<int>(l);
                    return up;
                }
            }
        }
        if (firstAny >= 0) {  // targets it, but no value reaches the threshold: assume the first level
            if (level) *level = firstAny;
            return up;
        }
    }
    return nullptr;
}

// Unlocks a locked slot the way buying the vacpack upgrade does
// (PlayerUpgradeFabricatableItem.FabricateAndSpendCost, 1.3.0 RVA 0x82AFA0, minus the
// spending and analytics): UpgradeModel.IncrementUpgradeLevel, one level at a time, up to
// the level that unlocks it (the model's level n applies UpgradeDefinition._levels[n]; the
// fabricator buys GetUpgradeLevel + 1). Never past that level, even if the slot does not
// report unlocked at once. Earlier slots the same levels unlock open too, as in the shop.
void Sandbox::DoUnlockSlot(const Ctx& c, int idx) {
    Il2CppObject* slot = SlotAt(c, idx);
    if (!slot) {
        Done(false, std::format("Slot {} not unlocked: no such slot", idx + 1));
        return;
    }
    bool unlocked = false;
    if (CallValue(slotUnlocked_, slot, {}, "AmmoSlot.IsUnlocked", unlocked) && unlocked) {
        Done(true, std::format("Slot {} is already unlocked", idx + 1));
        return;
    }
    Il2CppObject* um = nullptr;
    int target = -1;
    Il2CppObject* up = SlotUnlockUpgrade(c, slot, &um, &target);
    if (!up || !um || target < 0) {
        Done(false, std::format("Slot {} not unlocked: no vacpack upgrade unlocks it", idx + 1));
        return;
    }
    const std::string upName = unity::GetName(up);
    int levels = 0, cur = 0;
    for (int guard = 0; guard < 16 && !unlocked; ++guard) {
        if (!CallValue(getUpgradeLevel_, um, {up}, "UpgradeModel.GetUpgradeLevel", cur) || cur >= target) break;
        bool inc = false;
        if (!CallValue(incUpgrade_, um, {up}, "UpgradeModel.IncrementUpgradeLevel", inc) || !inc) break;
        ++levels;
        CallValue(slotUnlocked_, slot, {}, "AmmoSlot.IsUnlocked", unlocked);
    }
    Done(unlocked, unlocked ? std::format("Slot {} unlocked ({} level(s) of the {} upgrade)", idx + 1, levels, upName)
                   : levels ? std::format("Slot {}: applied {} level(s) of {}, but it is still locked", idx + 1, levels, upName)
                            : std::format("Slot {} not unlocked: the {} upgrade is at level {} already (needs {})", idx + 1,
                                          upName, cur, target));
}

// Moves a slot's contents onto another slot: onto the same item it merges (up to the
// target's maximum), onto an empty slot it moves, onto another item it swaps. Every step
// goes through Decrement / MaybeAddResource, which check what each slot accepts; if
// re-adding fails, the original contents are put back.
void Sandbox::DoMoveSlot(const Ctx& c, int from, int to) {
    Il2CppObject* a = SlotAt(c, from);
    Il2CppObject* b = SlotAt(c, to);
    if (!a || !b || from == to || !c.ammo) {
        Done(false, "Not moved: no such slot");
        return;
    }
    bool ua = false, ub = false;
    CallValue(slotUnlocked_, a, {}, "AmmoSlot.IsUnlocked", ua);
    CallValue(slotUnlocked_, b, {}, "AmmoSlot.IsUnlocked", ub);
    if (!ua || !ub) {
        Done(false, std::format("Not moved: slot {} is locked", !ua ? from + 1 : to + 1));
        return;
    }
    Il2CppObject* ia = unity::Field<Il2CppObject*>(a, offSlotId_);
    Il2CppObject* ib = unity::Field<Il2CppObject*>(b, offSlotId_);
    const int ca = unity::Field<int32_t>(a, offSlotCount_), cb = unity::Field<int32_t>(b, offSlotCount_);
    if (!ia || ca <= 0) {
        Done(false, std::format("Not moved: slot {} is empty", from + 1));
        return;
    }
    if (!Saveable(SaveIndex(), ia) || (ib && cb > 0 && !Saveable(SaveIndex(), ib))) {
        Done(false, "Not moved: the game can't save one of the items");
        return;
    }
    auto add = [&](Il2CppObject* id, int slotIdx, int n) {
        bool overflow = false, added = false;
        return n <= 0 || (CallValue(addResource_, c.ammo, {id, &slotIdx, &n, &overflow}, "AmmoSlotManager.MaybeAddResource",
                                    added) &&
                          added);
    };
    auto remove = [&](int slotIdx, int n) {
        bool ok = false;
        Call(decrement_, c.ammo, {&slotIdx, &n}, "AmmoSlotManager.Decrement", &ok);
        return ok;
    };
    const std::string na = ItemName(ia);
    int maxB = 0;
    CallValue(slotMax_, c.ammo, {&to}, "AmmoSlotManager.GetSlotMaxCount", maxB);
    if (ib == ia && cb > 0) {  // merge
        const int n = std::min(ca, std::max(0, maxB - cb));
        if (n <= 0) {
            Done(false, std::format("Not moved: slot {} is full", to + 1));
            return;
        }
        if (!add(ia, to, n)) {
            Done(false, std::format("Not moved: slot {} refused {} x {}", to + 1, n, na));
            return;
        }
        remove(from, n);
        Done(true, std::format("Moved {} x {} from slot {} to slot {}", n, na, from + 1, to + 1));
        return;
    }
    if (!(ib && cb > 0)) {  // onto an empty slot: as much as it holds; the rest stays
        const int n = std::min(ca, maxB);
        if (n <= 0 || !add(ia, to, n)) {
            Done(false, std::format("Not moved: slot {} does not accept {}", to + 1, na));
            return;
        }
        remove(from, n);
        Done(true, std::format("Moved {} x {} from slot {} to slot {}", n, na, from + 1, to + 1));
        return;
    }
    // Swap: only when each side fits into the other slot, so nothing is lost.
    const std::string nb = ItemName(ib);
    int maxA = 0;
    CallValue(slotMax_, c.ammo, {&from}, "AmmoSlotManager.GetSlotMaxCount", maxA);
    if (ca > maxB || cb > maxA) {
        Done(false, std::format("Not swapped: {} x {} or {} x {} does not fit the other slot", ca, na, cb, nb));
        return;
    }
    if (!remove(from, ca) || !remove(to, cb)) {
        Done(false, "Not swapped: a slot could not be emptied");
        add(ia, from, ca - unity::Field<int32_t>(a, offSlotCount_));
        return;
    }
    const bool okA = add(ia, to, ca);
    const bool okB = add(ib, from, cb);
    if (okA && okB) {
        Done(true, std::format("Swapped slot {} ({}) and slot {} ({})", from + 1, na, to + 1, nb));
        return;
    }
    // Put everything back where it was.
    if (okA) remove(to, unity::Field<int32_t>(b, offSlotCount_));
    if (okB) remove(from, unity::Field<int32_t>(a, offSlotCount_));
    add(ia, from, ca);
    add(ib, to, cb);
    Done(false, std::format("Not swapped: slot {} does not accept {}", okA ? from + 1 : to + 1, okA ? nb : na));
}

}  // namespace

std::unique_ptr<sr2::core::Module> CreateSandbox() { return std::make_unique<Sandbox>(); }

}  // namespace sr2::modules
