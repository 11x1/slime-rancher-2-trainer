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
// Threading: no hooks. A main-thread frame callback copies a Snapshot for the
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
#include <unordered_map>
#include <vector>

#include "core/detour_guard.h"
#include "core/il2cpp_api.h"
#include "core/log.h"
#include "core/main_thread.h"
#include "core/module.h"
#include "core/unity.h"
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
    uint32_t ItemIcon(Il2CppObject* ident);
    bridge::Kind ItemKind(Il2CppObject* ident);
    bool EnsureCatalog(const Ctx& c);
    Il2CppObject* CatalogItem(uintptr_t id, uint32_t* mask);
    Il2CppObject* Lookup(const std::unordered_map<uintptr_t, CatalogEntry>& map, uintptr_t id, Il2CppClass* k);
    bool EnsureSpawnables();
    bool EnsureRecipes();
    void RefreshRecipeState();
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
    std::unordered_map<uintptr_t, bridge::Kind> kindCache_; // identity -> kind

    // spawner and crafter lists (main thread), built once per enable; entries kept alive by GC handles
    std::shared_ptr<const std::vector<bridge::Item>> spawnables_;
    std::unordered_map<uintptr_t, CatalogEntry> spawnById_;
    bool spawnTried_ = false;
    std::string spawnWhy_;  // "" = spawner available
    std::shared_ptr<const std::vector<bridge::Recipe>> recipes_;
    std::shared_ptr<const std::vector<std::string>> recipeCats_;
    std::unordered_map<uintptr_t, CatalogEntry> recipeById_;
    bool recipesTried_ = false;
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
    const MethodInfo* fabCatContains_ = nullptr;     // GadgetFabricateCategory.Contains(GadgetDefinition)
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

    // Spawner: the same InstantiationHelpers.InstantiateActor the vacpack uses to shoot items out
    // (VacuumItem.Expel), with the region registry's current scene group.
    std::string& sw = spawnWhy_;
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

    // Crafter: the fabricator's GadgetFabricatableItem.FabricateAndSpendCost minus the spending:
    // GadgetDirector.AddItem(definition, count, x2 for BuyInPairs), within CanAddGadget.
    std::string& cw = craftWhy_;
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
    }
}

void Sandbox::OnEnable() {
    enabled_.store(true);
    bridge::SetHandler(&HandleThunk);
    lastSnap_ = 0;
    // A list that came out empty (built before the assets were loaded) is tried again.
    if (!spawnables_ || spawnables_->empty()) spawnTried_ = false;
    if (!recipes_ || recipes_->empty()) recipesTried_ = false;
    SR2_INFO("[{}] enabled: edits from the overlay's Sandbox tab now change the loaded game", kModule);
}

void Sandbox::OnDisable() {
    enabled_.store(false);
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

// The type's own icon (IdentifiableType.icon), registered once with ui/item_icons.
uint32_t Sandbox::ItemIcon(Il2CppObject* ident) {
    if (!ident || !offIdentIcon_) return 0;
    const auto key = reinterpret_cast<uintptr_t>(ident);
    if (auto it = iconCache_.find(key); it != iconCache_.end()) return it->second;
    if (!IsA(ident, identClass_) || !unity::IsAlive(ident)) return 0;
    const uint32_t icon = sr2::ui::item_icons::Register(unity::Field<Il2CppObject*>(ident, offIdentIcon_));
    iconCache_[key] = icon;
    return icon;
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

    const unsigned long long t0 = GetTickCount64();
    std::vector<Il2CppGCHandle> old;
    for (auto& [id, e] : catalogById_) old.push_back(e.handle);
    catalogById_.clear();
    if (!old.empty())
        main_thread::Post([old] {
            for (Il2CppGCHandle h : old) unity::FreeGCHandle(h);
        });

    auto items = std::make_shared<std::vector<bridge::Item>>();
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
        Il2CppGCHandle h = unity::NewGCHandle(o);
        if (!h) continue;
        const auto id = reinterpret_cast<uintptr_t>(o);
        catalogById_[id] = CatalogEntry{h, o};
        items->push_back(bridge::Item{id, ItemName(o), mask, ItemIcon(o), ItemKind(o)});
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

// What the spawner offers: IdentifiableType assets whose prefab is an actor
// (has an IdentifiableActor), except the player, scene objects (gordos and the
// like), gadgets (placed through gadget mode) and liquids. Built once.
bool Sandbox::EnsureSpawnables() {
    if (spawnTried_ || !spawnWhy_.empty()) return false;
    spawnTried_ = true;
    const unsigned long long t0 = GetTickCount64();
    auto items = std::make_shared<std::vector<bridge::Item>>();
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
        Il2CppGCHandle h = unity::NewGCHandle(o);
        if (!h) continue;
        const auto id = reinterpret_cast<uintptr_t>(o);
        spawnById_[id] = CatalogEntry{h, o};
        items->push_back(bridge::Item{id, ItemName(o), 0, ItemIcon(o), ItemKind(o)});
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
    recipesTried_ = true;
    const unsigned long long t0 = GetTickCount64();
    struct Cat {
        Il2CppObject* obj;
        int index;
    };
    std::vector<Cat> cats;
    auto titles = std::make_shared<std::vector<std::string>>();
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
    lastRecipeState_ = 0;
    SR2_INFO("[{}] crafter: {} gadget recipes in {} fabricator categories ({} ms)", kModule, recipes_->size(),
             recipeCats_->size(), GetTickCount64() - t0);
    return true;
}

// Blueprint unlocked and how many are in the gadget inventory, per recipe. Copies the
// list only when something changed, so the tab's shared_ptr stays the same otherwise.
void Sandbox::RefreshRecipeState() {
    if (!recipes_ || recipes_->empty()) return;
    Il2CppObject* dir = GadgetDirector();
    if (!dir) return;
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
                    slot.icon = ItemIcon(id);
                    slot.kind = ItemKind(id);
                } else {
                    slot.count = 0;
                }
            }
            s.slots.push_back(std::move(slot));
        }
        // At most one list is built per refresh, so the one-time scans don't stack into one long frame.
        bool built = c.slots && EnsureCatalog(c);
        s.catalog = catalog_;
        if (!built) built = EnsureSpawnables();
        s.spawnables = spawnables_;
        s.spawnWhy = spawnWhy_;
        if (!built) EnsureRecipes();
        if (recipes_ && s.tick - lastRecipeState_ >= kRecipeStateMs) {
            lastRecipeState_ = s.tick;
            RefreshRecipeState();
        }
        s.recipes = recipes_;
        s.recipeCategories = recipeCats_;
        s.craftWhy = craftWhy_;
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
    Il2CppObject* dir = GadgetDirector();
    if (!dir) {
        Done(false, std::format("{} not crafted: no gadget director (load a save)", name));
        return;
    }
    bool unlocked = false;
    if (!CallValue(hasBlueprint_, dir, {def}, "GadgetDirector.HasBlueprint", unlocked) || !unlocked) {
        Done(false, std::format("{} not crafted: its blueprint is not unlocked", name));
        return;
    }
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
                                   ? std::format("Crafted {} x {} for free (inventory {} -> {})", after - before, name, before, after)
                                   : std::format("{} not crafted: the game refused it", name));
}

}  // namespace

std::unique_ptr<sr2::core::Module> CreateSandbox() { return std::make_unique<Sandbox>(); }

}  // namespace sr2::modules
