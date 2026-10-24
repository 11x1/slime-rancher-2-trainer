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
// Achievements are paused while the module is enabled (achievements.cpp), and a save
// Sandbox was used in is marked as cheated and keeps them blocked with Sandbox off
// (cheated_save.cpp). Both sets of hooks are always on, so Setup also runs while the
// module is off (SetupWhileDisabled); the rest of the module does nothing then.
//
// Threading: no hooks besides those two and infinite health. A main-thread frame callback copies a Snapshot for the
// tab (4 Hz while it is open) and tops up energy while "infinite energy" is on;
// the tab's requests arrive as posted main-thread jobs (ui/sandbox_bridge.h).
// Every game call goes through Resolver::Invoke (runtime_invoke, managed
// exceptions captured); a native fault there marks the module faulted and it
// stops calling into the game.

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <optional>
#include <cstdint>
#include <cstring>
#include <format>
#include <initializer_list>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "core/detour_guard.h"
#include "core/guard.h"
#include "core/hook_manager.h"
#include "core/il2cpp_api.h"
#include "core/log.h"
#include "core/main_thread.h"
#include "core/module.h"
#include "core/unity.h"
#include "modules/common/legend_hint.h"
#include "modules/custom_upgrades/api.h"
#include "modules/custom_upgrades/sale_notify.h"
#include "modules/minimap/ugui.h"
#include "modules/sandbox/achievements.h"
#include "modules/sandbox/cheated_save.h"
#include "modules/sandbox/player_extras.h"
#include "modules/sandbox/prefab_icons.h"
#include "modules/sandbox/upgrades.h"
#include "ui/item_icons.h"
#include "ui/overlay.h"
#include "ui/sandbox_bridge.h"

namespace sr2::modules {
namespace {

namespace unity = sr2::core::unity;
namespace main_thread = sr2::core::main_thread;
namespace bridge = sr2::ui::sandbox;
namespace legend_hint = sr2::modules::legend_hint;
namespace ugui = sr2::modules::minimap::ugui;
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
constexpr int kMaxDrop = 60;    // "Put in slots": most items dropped in front of the player per item
constexpr int kMaxCraft = 99;   // per click
constexpr float kSpawnDist = 2.5f;  // metres in front of the camera
constexpr int kFabricatorKey = 'G';  // in the open Fabricator: add the selected recipe's missing materials
constexpr const char* FabricatorKeyName() { return "G"; }

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

std::string Lower(std::string s) {
    for (char& ch : s) ch = static_cast<char>((ch >= 'A' && ch <= 'Z') ? ch - 'A' + 'a' : ch);
    return s;
}

// The area codes in the crates' asset names (identifiabletypes bundle, 1.3.0: ContainerFields01,
// ContainerGorgeGold01, ContainerLabyDreamlands01, ContainerLabyShadow01, ...), longest first. `crate`
// starts the crate's name, `zone` is the English zone name (fallback for the section when no
// ZoneDefinition lists the crate) and `zoneKey` what that zone's _nameKey is expected to contain (its
// localized name is used then). The Grey Labyrinth's areas have no localized names of their own (the
// game only uses them in dialogue: "Dream Land", "Lava Depths", "Terrarium", "Waterworks").
struct CrateCode {
    const char* code;
    const char* crate;
    const char* zone;
    const char* zoneKey;
};
constexpr CrateCode kCrateCodes[] = {
    {"LabyDreamlands", "Dream Land", "The Grey Labyrinth", "labyrinth"},
    {"LabyLavaDepths", "Lava Depths", "The Grey Labyrinth", "labyrinth"},
    {"LabyTerrarium", "Terrarium", "The Grey Labyrinth", "labyrinth"},
    {"LabyWaterworks", "Waterworks", "The Grey Labyrinth", "labyrinth"},
    {"LabyShadow", "Shadow", "The Grey Labyrinth", "labyrinth"},
    {"LabyHub", "Labyrinth Hub", "The Grey Labyrinth", "labyrinth"},
    {"Laby", "Labyrinth", "The Grey Labyrinth", "labyrinth"},
    {"Fields", "Rainbow Fields", "Rainbow Fields", "fields"},
    {"Gorge", "Ember Valley", "Ember Valley", "gorge"},
    {"Strand", "Starlight Strand", "Starlight Strand", "strand"},
    {"Bluffs", "Powderfall Bluffs", "Powderfall Bluffs", "bluffs"},
};

// The crate code of an asset name ("ContainerLabyDreamlandsGold01" -> LabyDreamlands), else nullptr.
const CrateCode* CrateCodeOf(const std::string& asset) {
    constexpr std::string_view kPrefix = "Container";
    if (!asset.starts_with(kPrefix)) return nullptr;
    const std::string_view rest = std::string_view(asset).substr(kPrefix.size());
    for (const CrateCode& c : kCrateCodes)
        if (rest.starts_with(c.code)) return &c;
    return nullptr;
}

// A readable name from an asset name, for types without a localized one (the crates show
// "!! Actor.l.crate_reef_01 !!"): "ContainerBluffsGold01" -> "Powderfall Bluffs Gold Crate",
// "ContainerLabyDreamlands01" -> "Dream Land Crate" (kCrateCodes).
std::string PrettyAssetName(const std::string& asset) {
    if (const CrateCode* c = CrateCodeOf(asset)) {
        const bool gold = asset.find("Gold", 9 + std::strlen(c->code)) != std::string::npos;
        return std::format("{}{} Crate", c->crate, gold ? " Gold" : "");
    }
    std::string s;
    for (size_t i = 0; i < asset.size(); ++i) {
        const char c = asset[i];
        if (c == '_') {
            s += ' ';
            continue;
        }
        const bool upper = c >= 'A' && c <= 'Z';
        const bool prevLower = i > 0 && asset[i - 1] >= 'a' && asset[i - 1] <= 'z';
        if (upper && prevLower) s += ' ';
        s += c;
    }
    while (!s.empty() && ((s.back() >= '0' && s.back() <= '9') || s.back() == ' ')) s.pop_back();  // "...01"
    if (s.rfind("Container ", 0) == 0) s = s.substr(10) + " Crate";
    return s.empty() ? asset : s;
}

// ---- the game's open Fabricator (FabricatorUI instances) ----
// Found through two hooks instead of a scan: the old once-a-second FindObjectOfType(FabricatorUI) cost
// 6-14 ms per call (the log's "frame:sandbox took 6 ms" every second, also on the title screen) and never
// reported the Fabricator open in the user's test (no G press reached the module, no hint was drawn).
// FabricatorUI.Start (1.3.0 RVA 0x826D30, runs once per instance, wires the content's events) adds the
// instance, OnDestroy (0x8263D0) removes it; "open" is then its GameObject being active. Both hooks stay on
// with Sandbox off (an instance started before Sandbox was turned on must be known) and only keep a list.
// HookManager module / DetourGuard fault name "sandbox_fabricator". Main thread only.
constexpr const char* kFabHooks = "sandbox_fabricator";
struct FabUi {
    Il2CppObject* ui = nullptr;
    Il2CppGCHandle handle = nullptr;
};
std::vector<FabUi> g_fabUis;
int g_fabStarts = 0;  // FabricatorUI.Start calls so far (a Fabricator UI was built: its assets are loaded)
using FabUiFn = void (*)(Il2CppObject*, const MethodInfo*);
FabUiFn g_origFabStart = nullptr, g_origFabDestroy = nullptr;
sr2::core::DetourGuard g_fabStartGuard{kFabHooks, "FabricatorUI.Start"};
sr2::core::DetourGuard g_fabDestroyGuard{kFabHooks, "FabricatorUI.OnDestroy"};

void FabStartDetour(Il2CppObject* self, const MethodInfo* mi) {
    g_origFabStart(self, mi);
    g_fabStartGuard.Run([&] {
        if (!self || !main_thread::IsMainThread()) return;
        ++g_fabStarts;
        for (const FabUi& f : g_fabUis)
            if (f.ui == self) return;
        g_fabUis.push_back({self, unity::NewGCHandle(self)});
    });
}

void FabDestroyDetour(Il2CppObject* self, const MethodInfo* mi) {
    g_fabDestroyGuard.Run([&] {
        if (!main_thread::IsMainThread()) return;
        for (auto it = g_fabUis.begin(); it != g_fabUis.end(); ++it) {
            if (it->ui != self) continue;
            if (Il2CppGCHandle h = it->handle) main_thread::Post([h] { unity::FreeGCHandle(h); });  // not inside the hook
            g_fabUis.erase(it);
            break;
        }
    });
    g_origFabDestroy(self, mi);
}

class Sandbox final : public sr2::core::Module {
public:
    const char* Name() const override { return kModule; }
    bool DefaultEnabled() const override { return false; }
    // The achievement block and the cheated-save mark (achievements.h, cheated_save.h) work while
    // Sandbox is off too: their hooks are created in Setup and never switched with the module.
    bool SetupWhileDisabled() const override { return true; }
    bool Setup(sr2::core::ModuleContext& ctx) override;
    void OnEnable() override;
    void OnDisable() override;

    void Tick(float dt);                 // main thread, every frame
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
    std::string VacpackState(bool sync);  // for the upgrades manager (log line; sync: slot maxima follow AmmoLimit)
    std::string Localized(Il2CppObject* ls);
    std::string ItemName(Il2CppObject* ident);
    uint32_t ItemIcon(Il2CppObject* ident, uint32_t* second = nullptr);
    uint32_t SpriteIcon(Il2CppObject* sprite);
    Il2CppObject* VirtualIcon(Il2CppObject* obj);
    uint32_t GroupIconFor(Il2CppObject* ident);
    bool EnsureTypeGroups();
    void CollectMembers(Il2CppObject* group, std::unordered_set<uintptr_t>& out, int depth = 0);
    Il2CppObject* NamedGroup(const char* name);
    bool EnsureNamedSprites();
    uint32_t NamedSprite(std::initializer_list<const char*> names);
    void EnsureGroupIcons();
    void EnsureRelations();
    bool RenderNextIcon();
    void QueueIconRenders(const std::vector<bridge::Item>& items);
    static void PatchIcon(std::shared_ptr<const std::vector<bridge::Item>>& list, uintptr_t id, uint32_t icon);
    void LogSpawnGroups();
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
    struct FabCat {
        Il2CppObject* obj;
        int index;  // into the category titles
    };
    void ReadFabCategories(std::vector<FabCat>& cats, std::vector<std::string>& titles, std::vector<uint32_t>& icons);
    bool EnsurePinnedIcon();
    void GroupFabCategories(std::vector<std::unordered_set<uintptr_t>>& members, std::vector<std::string>& titles,
                            std::vector<uint32_t>& icons, std::vector<std::unordered_map<uintptr_t, int>>& orders);
    int RecipeCategory(const std::vector<FabCat>& cats, Il2CppObject* def, int* order);
    uintptr_t PinnedRecipe(const Ctx& c);
    bool EnsureRecipes();
    bool EnsureMaterials();
    void RefreshRecipeState();
    bool SlotUnlockUpgrades(const Ctx& c, Il2CppObject* slot, Il2CppObject** upgradeModel,
                            std::vector<Il2CppObject*>* out = nullptr);
    std::string RecipeCost(Il2CppObject* def, std::vector<bridge::CostLine>* lines = nullptr);
    Il2CppObject* GadgetDirector();
    void Refresh();
    void TopUpEnergy();
    void Done(bool ok, const std::string& text);
    // HUD notices of spawns, crafts and refinery adds. A request that is part of a multi-item one
    // (bridge::Action::batch) collects them, and the last one shows them, one per item type (FlushNotices).
    enum class NoticeKind { Spawn, Craft, Refinery };
    struct NoticeLine {
        NoticeKind kind;
        std::string name;
        int count = 0;
        Il2CppObject* icon = nullptr;  // the type's sprite (an asset; checked again before use)
    };
    void Notify(Il2CppObject* type, NoticeKind kind, int count, const std::string& name, const std::string& text);
    Il2CppObject* NoticeIcon(Il2CppObject* type);
    static std::string NoticeText(NoticeKind kind, int count, const std::string& name);
    bool ShowNotice(Il2CppObject* icon, const std::string& notice, bool flushNow = true);
    void FlushNotices();
    void EnsureCrateAreas();

    void DoNewbucks(const Ctx& c, int64_t target, const char* how);
    void RefreshPurchaseMenus();
    void DoSlotCount(const Ctx& c, int idx, int64_t value);
    void DoPutItem(const Ctx& c, int idx, uintptr_t item, int64_t value);
    int DoSpawn(const Ctx& c, uintptr_t item, int64_t count, int limit = kMaxSpawn);
    void DoStow(const Ctx& c, int sel, uintptr_t item, int64_t amount, bool replace);
    void DoCraft(uintptr_t item, int64_t count);
    void DoAddMaterial(uintptr_t item, int64_t count);
    void DoMoveSlot(const Ctx& c, int from, int to);
    void DoUnlockSlot(const Ctx& c, int idx);
    bool SpawnPose(const Ctx& c, Vec3& pos, Vec3& fwd);
    bool GetVec3(const MethodInfo* m, Il2CppObject* obj, Vec3& out);
    void SetupExtras(sr2::il2cpp::Resolver& R);
    // "Unlock all upgrades" (Player card) and the Fabricator's "add missing materials" key.
    void SetupUnlocks(sr2::il2cpp::Resolver& R);
    bool PlayerUpgrades(const Ctx& c, Il2CppObject*& um, std::vector<Il2CppObject*>& out);
    void UpgradeState(const Ctx& c);
    void DoUnlockUpgrades(const Ctx& c);
    void FabricatorTick(unsigned long long now);
    void FabricatorHintTick(unsigned long long now);
    void DropFabricatorHint();
    void DoFabricatorMaterials();

    std::atomic<bool> enabled_{false};
    std::atomic<bool> infinite_{false};  // infinite energy (stored per save: cheated_save.h, SaveToggle)
    unsigned togglesSeen_ = 0;           // SaveTogglesVersion() applied last (0: apply again)
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
    std::unordered_map<std::string, CatalogEntry> groupsByName_;  // every IdentifiableTypeGroup asset, by asset name
    std::unordered_set<uintptr_t> foodTypes_, resourceTypes_, containerTypes_;  // members of FoodGroup, CraftGroup, StandardCrateClass
    bool typeGroupsBuilt_ = false;
    std::unordered_map<std::string, uint32_t> namedSprites_;  // the few game sprites looked up by name (tab icons)
    bool spritesScanned_ = false;
    uint32_t groupIcons_[bridge::kGroupCount] = {};  // icons of the group tabs
    bool groupIconsBuilt_ = false;
    std::shared_ptr<const bridge::Relations> relations_;  // slime diets, built once after the spawn list
    // Icons rendered from prefabs for list items without any (prefab_icons.h), one per refresh.
    sandbox::PrefabIcons prefabIcons_;
    std::vector<uintptr_t> renderQueue_;            // type identities still to render
    std::unordered_set<uintptr_t> renderQueued_;    // ever queued (each type is tried once)
    std::unordered_map<uintptr_t, CatalogEntry> renderTypes_;  // queued types, kept alive
    int rendered_ = 0, renderFailed_ = 0;
    unsigned long long renderMs_ = 0;
    // The game's HUD notices for spawns, crafts and refinery adds (custom_upgrades/sale_notify.h).
    sr2::modules::custom_upgrades::ui::SaleNotices notices_;
    std::vector<NoticeLine> batchNotices_;  // collected for the notices of a multi-item request
    int batchIdx_ = 0, batchSize_ = 0;      // the request being handled (bridge::Action::batch / batchSize)
    unsigned long long batchTick_ = 0;      // last notice collected (a batch left unfinished is shown after 2 s)
    int spawnNext_ = 0;                     // spawn grid position for the next item of a multi-item spawn
    bool spawnGroupsLogged_ = false;
    bool crateAreasBuilt_ = false;          // crate areas from the zones' crate lists (EnsureCrateAreas)

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
    const MethodInfo* fabCatIndexOf_ = nullptr;      // GadgetFabricateCategory.IndexOf(GadgetDefinition) (optional: the Fabricator's order)
    Il2CppClass* fabPinnedCatClass_ = nullptr;       // PinnedFabricateCategory (optional: the Pinned tab's icon)
    uint32_t pinnedIcon_ = 0;
    int pinnedIconTry_ = -1;                         // g_fabStarts at the last EnsurePinnedIcon search
    size_t offPinnedRecipe_ = 0;                     // PlayerModel.PinnedRecipe (IRecipe; optional)
    std::unordered_map<std::string, uintptr_t> recipeByRef_;  // gadget ReferenceId -> recipe id
    Il2CppObject* pinnedObj_ = nullptr;              // the IRecipe last resolved (identity only)
    uintptr_t pinnedId_ = 0;
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
    size_t offGroupMembers_ = 0;    // IdentifiableTypeGroup._memberTypes (List<IdentifiableType>)
    size_t offGroupLocName_ = 0;    // IdentifiableTypeGroup._localizedName (optional, crafter category titles)
    Il2CppClass* spriteClass_ = nullptr;  // UnityEngine.Sprite (tab icons by name)
    const MethodInfo* groupContains_ = nullptr;  // IdentifiableTypeGroup.IsMember (for the icon fallback)
    size_t offModTarget_ = 0;       // FloatModifier.TargetProperty (from the live object's class)
    Il2CppClass* modClass_ = nullptr;
    Il2CppClass* droneTargetsClass_ = nullptr;  // DroneTargetDefinitions (ListAsset of DroneUIProgramDefinition<DroneTaskTargetType>)
    Il2CppClass* zoneClass_ = nullptr;  // MonomiPark.SlimeRancher.World.ZoneDefinition (crate areas)
    size_t offZoneCrates_ = 0;          // ZoneDefinition._crates (SpawnableCrate[])
    size_t offZoneLocName_ = 0;         // ZoneDefinition._localizedName
    size_t offZoneNameKey_ = 0;         // ZoneDefinition._nameKey (string)
    size_t offCrateType_ = 0;           // ZoneDefinition.SpawnableCrate.Type (from the live object's class)

    // ---- vacpack slot unlock (optional): the upgrade levels the fabricator sells ----
    const MethodInfo* getUpgradeModel_ = nullptr;    // PlayerModel.GetUpgradeModel()
    const MethodInfo* incUpgrade_ = nullptr;         // UpgradeModel.IncrementUpgradeLevel(UpgradeDefinition)
    const MethodInfo* getUpgradeLevel_ = nullptr;    // UpgradeModel.GetUpgradeLevel(UpgradeDefinition)
    size_t offUmDefs_ = 0;                           // UpgradeModel.upgradeDefinitions (UpgradeDefinitionList)
    size_t offUdLevels_ = 0;                         // UpgradeDefinition._levels (List<UpgradeLevelDefinition>)
    size_t offUlEffects_ = 0;                        // UpgradeLevelDefinition.Effects (List<FloatModifier>)
    size_t offSlotDefUnlockProp_ = 0;                // AmmoSlotDefinition._isUnlockedProperty
    size_t offSlotDefThreshold_ = 0;                 // AmmoSlotDefinition._isUnlockedThreshold
    Il2CppClass* upgradeDefClass_ = nullptr;

    // ---- unlock all upgrades (optional; uses the slot-unlock symbols above) ----
    int upgCount_ = 0, upgLeft_ = -1;  // for the snapshot (refreshed every kRecipeStateMs)
    unsigned long long lastUpgState_ = 0;
    size_t offUlCosts_ = 0;            // UpgradeLevelDefinition._costs (PurchaseCost, inline; from the live class)

    // ---- Fabricator "add missing materials" key (optional) ----
    Il2CppClass* fabUiClass_ = nullptr;          // MonomiPark.SlimeRancher.UI.Fabricator.FabricatorUI
    Il2CppClass* fabGadgetItemClass_ = nullptr;  // GadgetFabricatableItem
    Il2CppClass* fabUpgradeItemClass_ = nullptr; // PlayerUpgradeFabricatableItem
    size_t offFabUiContent_ = 0, offFabUiSpend_ = 0;          // FabricatorUI._content / _spendableDirector
    size_t offFabContentGrid_ = 0, offFabContentDetails_ = 0; // FabricatorContent._itemGrid / _itemDetails
    size_t offFabGridHovered_ = 0, offFabGridItems_ = 0;      // FabricatorItemGrid._hoveredIndex / _currentPageOfItems
    size_t offFabDetailsRecipe_ = 0, offFabRecipeCount_ = 0;  // FabricatorItemDetails._recipe, FabricatorRecipe._itemCount
    size_t offFabGadgetDef_ = 0, offFabUpgradeDef_ = 0;       // the items' Definition / UpgradeDefinition backing fields
    size_t offCostComponents_ = 0;                            // PurchaseCost.ComponentCosts (in the struct)
    const MethodInfo* fabDetailsSetItem_ = nullptr;    // FabricatorItemDetails.SetItem(IFabricatableItem)
    const MethodInfo* fabRecipeSetItem_ = nullptr;     // FabricatorRecipe.SetItem(IFabricatableItem, int)
    const MethodInfo* spendIdentAmount_ = nullptr;     // SpendableDirector.GetIdentCostAmount(IdentifiableType)
    const MethodInfo* spendComponentAmount_ = nullptr; // SpendableDirector.GetUpgradeComponentAmount(UpgradeComponent)
    const MethodInfo* acquireComponent_ = nullptr;     // static AcquisitionUtility.AcquireUpgradeComponent(component, int)
    const MethodInfo* fabGridSelected_ = nullptr;      // FabricatorItemGrid.TryGetSelectedItem(out IFabricatableItem)
    const MethodInfo* fabContentSelected_ = nullptr;   // FabricatorContent.OnItemSelected(IFabricatableItem, InteractionCause)
    // The quantity popup (FabricatorUI._confirmPopup, a FabricatorConfirmPopup; optional, round 8).
    size_t offFabUiPopup_ = 0, offPopupItem_ = 0, offPopupCount_ = 0;  // _confirmPopup / its _item / _countToMake
    size_t offFabUiConfirming_ = 0, offPopupClosing_ = 0;              // FabricatorUI._confirming / popup _isClosing
    const MethodInfo* popupUpdate_ = nullptr;    // FabricatorConfirmPopup.UpdateCountAndRecipeDisplay(FabricationBlockedReason)
    const MethodInfo* checkGadget_ = nullptr;    // GadgetFabricatableItem's IFabricatableItem.CheckCanFabricate(int)
    const MethodInfo* checkUpgrade_ = nullptr;   // PlayerUpgradeFabricatableItem's IFabricatableItem.CheckCanFabricate(int)
    unsigned long long lastFabPoll_ = 0;
    bool fabOpen_ = false, fabKeyDown_ = false;
    // "[G] Add materials" in the Fabricator's own key bar (modules/common/legend_hint.h), before "[Z] Pin"
    size_t offFabUiLegend_ = 0, offFabUiPinEvent_ = 0;  // FabricatorUI._inputLegend / _pinRecipeEvent
    bool fabHintOk_ = false, fabHintLogged_ = false, fabHintFailLogged_ = false;
    unsigned long long fabHintNext_ = 0, fabHintSince_ = 0;
    legend_hint::Clone fabHint_;
    bool quiet_ = false;  // Done() only logs (the Fabricator key's results)

    // ---- "Discover all upgrades" (optional): the upgrade components that make a hidden upgrade buyable ----
    size_t offUdRequireComp_ = 0;                      // UpgradeDefinition._requireComponentToBeVisibleToPlayer
    const MethodInfo* tryGetSpendable_ = nullptr;      // SceneContextUtility.TryGetSpendableDirector(out SpendableDirector)
    int upgHidden_ = -1;                               // upgrades still to discover (snapshot; -1 = unknown)

    // ---- open plot purchase menu (optional): rebuilt after a Newbucks change ----
    Il2CppClass* landPlotUiClass_ = nullptr;           // MonomiPark.SlimeRancher.UI.Plot.LandPlotUIRoot

    Il2CppObject* OpenFabricator();                    // the open FabricatorUI, or nullptr
    Il2CppObject* FabricatorItem(Il2CppObject* ui, Il2CppObject** content, Il2CppObject** details);
    Il2CppObject* FabricatorPopup(Il2CppObject* ui);  // the quantity popup while it is shown, else nullptr
    void RefreshFabricator(bool rebuild = false);  // rebuild: also the grid, as on opening (RebuildFabricator)
    Il2CppObject* SpendableDirector();
    bool UpgradeHidden(Il2CppObject* um, Il2CppObject* up, Il2CppObject* spend, std::vector<Il2CppObject*>* missing);
    void DoDiscoverUpgrades(const Ctx& c);
};

Sandbox* g_self = nullptr;

void HandleThunk(const bridge::Action& a) {
    if (g_self) g_self->Handle(a);
}

// ------------------------------------------------------------------ setup ----
bool Sandbox::Setup(sr2::core::ModuleContext& ctx) {
    auto& R = ctx.resolver();
    // First, and required (fail closed: without them Sandbox can't be enabled). Their hooks stay
    // on even if a later step fails, so marked saves keep achievements blocked.
    if (!sandbox::SetupAchievementPause(ctx)) return false;
    if (!sandbox::SetupCheatedSave(ctx)) return false;
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
    SetupUnlocks(R);
    sandbox::SetupPlayerExtras(ctx);  // optional tools on the Extras card (player_extras.cpp)
    sandbox::SetupUpgrades(ctx, {[this](uintptr_t id) { return Lookup(recipeById_, id, gadgetDefClass_); },
                                 [this](bool rebuild) { RefreshFabricator(rebuild); },
                                 [this] { lastRecipeState_ = lastUpgState_ = lastSnap_ = 0; },
                                 [this](bool sync) { return VacpackState(sync); }});  // upgrades.cpp

    g_self = this;
    if (!frameCb_) frameCb_ = main_thread::AddFrameCallback(kModule, [this](float dt) { Tick(dt); });
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
        off(groupClass_, "_memberTypes", offGroupMembers_, iw);
        off(identClass_, "groupType", offIdentGroupType_, iw);
        groupContains_ = method(groupClass_, "IsMember", 1, iw);
        if (!iw.empty()) {
            offGroupIcon_ = offGroupMembers_ = 0;
            SR2_WARN("[{}] no group icons and no group-based item kinds ({})", kModule, iw);
        }
        std::string lw;
        off(groupClass_, "_localizedName", offGroupLocName_, lw);
        std::string sw2;
        spriteClass_ = cls("UnityEngine.CoreModule", "UnityEngine", "Sprite", sw2);
        if (!sw2.empty()) SR2_WARN("[{}] group tabs: no game sprites by name ({})", kModule, sw2);
        std::string pw;
        if (!prefabIcons_.Setup(R, kModule, pw)) SR2_WARN("[{}] no icons rendered from prefabs ({})", kModule, pw);
        std::string nw;
        // Spawn / craft / refinery feedback: free-text HUD notices with the item's icon ("Spawned 5 Pink
        // Plort"), not the drones' "sent to the Quantum Cloud" notice (Notify).
        if (!notices_.Setup(R, kModule, nw, "sandbox") || !notices_.SetupText(R, nw))
            SR2_WARN("[{}] no HUD notices for spawns / crafts ({}): the tab's status line instead", kModule, nw);
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
        std::string zw;
        zoneClass_ = cls(kImage, "MonomiPark.SlimeRancher.World", "ZoneDefinition", zw);
        off(zoneClass_, "_crates", offZoneCrates_, zw);
        off(zoneClass_, "_localizedName", offZoneLocName_, zw);
        off(zoneClass_, "_nameKey", offZoneNameKey_, zw);
        if (!zw.empty()) {
            zoneClass_ = nullptr;
            SR2_WARN("[{}] crate areas: no zone data ({}); areas from the crates' asset names", kModule, zw);
        }
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
        std::string lw2;
        landPlotUiClass_ = cls(kImage, "MonomiPark.SlimeRancher.UI.Plot", "LandPlotUIRoot", lw2);
        if (!lw2.empty()) SR2_WARN("[{}] Newbucks: an open plot menu is not refreshed ({})", kModule, lw2);
        // Optional: the Fabricator's order within a category, its Pinned category icon and the pinned recipe.
        std::string ow;
        fabCatIndexOf_ = method(fabGadgetCatClass_, "IndexOf", 1, ow);
        std::string pw2;
        fabPinnedCatClass_ = cls(kImage, kFab, "PinnedFabricateCategory", pw2);
        off(playerModelClass_, "PinnedRecipe", offPinnedRecipe_, pw2);
        if (!ow.empty() || !pw2.empty())
            SR2_WARN("[{}] crafter: {}{}", kModule, ow.empty() ? "" : "no Fabricator order (" + ow + ") ",
                     pw2.empty() ? "" : "no Pinned tab content (" + pw2 + ")");
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
    sandbox::PlayerExtrasEnabled(true);
    sandbox::UpgradesEnabled(true);
    lastSnap_ = 0;
    togglesSeen_ = 0;  // the save's toggles apply again
    // A list that came out empty (built before the assets were loaded) is tried again.
    if (!spawnables_ || spawnables_->empty()) spawnTried_ = false;
    if (!recipes_ || recipes_->empty()) recipesTried_ = false;
    if (!materials_ || materials_->empty()) materialsTried_ = false;
    SR2_INFO("[{}] enabled: edits from the overlay's Sandbox tab now change the loaded game", kModule);
}

void Sandbox::OnDisable() {
    enabled_.store(false);
    sandbox::SetAchievementsPaused(false);
    if (infinite_.exchange(false)) SR2_INFO("[{}] infinite energy off (module disabled; kept for the save)", kModule);
    notices_.Clear();
    batchNotices_.clear();
    bridge::SetHandler(nullptr);
    sandbox::PlayerExtrasEnabled(false);
    sandbox::UpgradesEnabled(false);
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

// "slot 1 holds up to 60, 6 of 9 slots unlocked": what the Tank Booster / Extra Tank levels show, read the
// way the page reads it (AmmoSlotManager.GetSlotMaxCount, AmmoSlot.IsUnlocked).
// `sync` (after a Tank Booster / Extra Tank change): when slot 1's maximum is not what the AmmoLimit property
// now says, every slot's maximum is set to it with AmmoSlotManager.UpdateMaxCount (0xA3AB70 -> AmmoModel.
// UpdateMaxCount -> AmmoSlot.set_MaxCount, the NullableFloatProperty's DefaultValue; the call the silo
// capacity upgrade uses). A backstop for "the Tank Booster max stayed at 60" (round 8), cause not seen.
std::string Sandbox::VacpackState(bool sync) {
    Ctx c;
    std::string why;
    if (!Context(c, why)) return "no player (" + why + ")";
    const size_t n = c.slots ? unity::ArrayLength(c.slots) : 0;
    int max0 = -1, unlocked = 0;
    for (size_t i = 0; i < n; ++i) {
        int idx = static_cast<int>(i);
        Il2CppObject* a = SlotAt(c, idx);
        if (!a) continue;
        bool open = false;
        CallValue(slotUnlocked_, a, {}, "AmmoSlot.IsUnlocked", open);
        unlocked += open ? 1 : 0;
        if (i == 0) CallValue(slotMax_, c.ammo, {&idx}, "AmmoSlotManager.GetSlotMaxCount", max0);
    }
    // Where slot 1's maximum comes from: AmmoSlot._maxCountValue (NullableFloatProperty: DefaultValue, or the
    // bound ModifiableFloat "AmmoLimit" that the Tank Booster's modifiers change; AmmoSlot.Setup binds it).
    std::string src;
    Il2CppObject* a0 = n ? SlotAt(c, 0) : nullptr;
    auto& R = Resolver::Get();
    static std::optional<size_t> offNullable = R.FieldOffset(ammoSlotClass_, "_maxCountValue");
    Il2CppObject* nullable = a0 && offNullable ? unity::Field<Il2CppObject*>(a0, *offNullable) : nullptr;
    if (nullable) {
        Il2CppClass* nk = api().il2cpp_object_get_class(nullable);
        auto offDef = R.FieldOffset(nk, "DefaultValue"), offProp = R.FieldOffset(nk, "Property");
        Il2CppObject* prop = offProp ? unity::Field<Il2CppObject*>(nullable, *offProp) : nullptr;
        float value = -1.0f;
        if (prop) {
            const MethodInfo* get = nullptr;
            for (Il2CppClass* k = api().il2cpp_object_get_class(prop); k && !get; k = api().il2cpp_class_get_parent(k))
                get = R.Method(k, "get_Value", 0);
            CallValue(get, prop, {}, "ModifiableFloat.get_Value", value);
        }
        src = std::format("; its default {}, AmmoLimit property {}", offDef ? unity::Field<float>(nullable, *offDef) : -1.0f,
                          prop ? std::format("bound, value {}", value) : std::string("not bound"));
        const int want = static_cast<int>(std::floor(value));
        static const MethodInfo* updateMax = R.Method(ammoMgrClass_, "UpdateMaxCount", 1);
        if (sync && prop && want > 0 && want != max0 && updateMax) {
            int32_t v = want;
            bool ok = false;
            Call(updateMax, c.ammo, {&v}, "AmmoSlotManager.UpdateMaxCount", &ok);
            int idx = 0, now = -1;
            CallValue(slotMax_, c.ammo, {&idx}, "AmmoSlotManager.GetSlotMaxCount", now);
            src += std::format("; slot maxima set to {} with UpdateMaxCount ({} -> {})", want, max0, now);
            max0 = now;
        }
    }
    return std::format("slot 1 holds up to {}{}, {} of {} slots unlocked", max0, src, unlocked, n);
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
    if (n.rfind("!!", 0) == 0) n.clear();  // missing localization entry ("!! Actor.l.crate_reef_01 !!")
    if (n.empty()) n = PrettyAssetName(unity::GetName(ident));
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

// Every IdentifiableTypeGroup asset, read once (one FindObjectsOfTypeAll): the ones with an icon
// for GroupIconFor (leaf groups first, so the most specific group wins), all of them by asset name,
// and the members of the game's own FoodGroup, CraftGroup and StandardCrateClass (crates) for the
// item kinds (field reads only). The group assets stay loaded; handles keep them. True if it ran.
bool Sandbox::EnsureTypeGroups() {
    if (typeGroupsBuilt_) return false;
    typeGroupsBuilt_ = true;
    if (!groupClass_ || !offGroupIcon_) return false;
    const unsigned long long t0 = GetTickCount64();
    std::vector<CatalogEntry> leaf, parent;
    std::string names;
    for (Il2CppObject* g : unity::FindObjectsOfTypeAll(groupClass_)) {
        if (!IsA(g, groupClass_) || !unity::IsAlive(g)) continue;
        const std::string name = unity::GetName(g);
        Il2CppObject* spr = unity::Field<Il2CppObject*>(g, offGroupIcon_);
        const bool icon = spr && unity::IsAlive(spr);
        if (!icon && (name.empty() || groupsByName_.count(name))) continue;
        Il2CppGCHandle h = unity::NewGCHandle(g);
        if (!h) continue;
        if (!name.empty() && !groupsByName_.count(name)) groupsByName_[name] = CatalogEntry{h, g};
        if (!icon) continue;
        (unity::ListCount(unity::Field<void*>(g, offGroupSubGroups_)) ? parent : leaf).push_back(CatalogEntry{h, g});
        names += (names.empty() ? "" : ", ") + name;
    }
    iconGroups_ = std::move(leaf);
    iconGroups_.insert(iconGroups_.end(), parent.begin(), parent.end());
    SR2_INFO("[{}] {} identifiable type group(s) with an icon: {}", kModule, iconGroups_.size(), names);
    if (offGroupMembers_) {
        if (Il2CppObject* g = NamedGroup("FoodGroup")) CollectMembers(g, foodTypes_);
        if (Il2CppObject* g = NamedGroup("CraftGroup")) CollectMembers(g, resourceTypes_);
        if (Il2CppObject* g = NamedGroup("StandardCrateClass")) CollectMembers(g, containerTypes_);
    }
    SR2_INFO("[{}] item kinds from the game's groups: {} food (FoodGroup), {} crafting resources (CraftGroup), {} crates "
             "(StandardCrateClass); {} groups read ({} ms)",
             kModule, foodTypes_.size(), resourceTypes_.size(), containerTypes_.size(), groupsByName_.size(),
             GetTickCount64() - t0);
    return true;
}

Il2CppObject* Sandbox::NamedGroup(const char* name) {
    auto it = groupsByName_.find(name);
    if (it == groupsByName_.end()) return nullptr;
    Il2CppObject* g = unity::GCHandleTarget(it->second.handle);
    return g == it->second.obj && IsA(g, groupClass_) && unity::IsAlive(g) ? g : nullptr;
}

// Every type in the group and its member groups (IdentifiableTypeGroup._memberTypes / _memberGroups).
void Sandbox::CollectMembers(Il2CppObject* group, std::unordered_set<uintptr_t>& out, int depth) {
    if (!group || depth > 8 || !IsA(group, groupClass_)) return;
    void* types = unity::Field<void*>(group, offGroupMembers_);
    for (size_t i = 0, n = unity::ListCount(types); i < n; ++i)
        if (auto* t = static_cast<Il2CppObject*>(unity::ListAt(types, i)); t && IsA(t, identClass_))
            out.insert(reinterpret_cast<uintptr_t>(t));
    void* subs = unity::Field<void*>(group, offGroupSubGroups_);
    for (size_t i = 0, n = unity::ListCount(subs); i < n; ++i)
        CollectMembers(static_cast<Il2CppObject*>(unity::ListAt(subs, i)), out, depth + 1);
}

// The few game sprites the tabs use that no loaded data points to directly (the Slimepedia's
// slime and largo icons, the fabricator's category icons): one FindObjectsOfTypeAll(Sprite),
// names compared once. Only sprites that are loaded at that moment are found. True if it ran.
bool Sandbox::EnsureNamedSprites() {
    if (spritesScanned_) return false;
    spritesScanned_ = true;
    if (!spriteClass_) return false;
    static const char* const kWanted[] = {"iconCategorySlimes", "pediaHomeSlimes", "iconLargoPedia_small", "iconLargoPedia",
                                          "iconCategoryUtilities", "iconCategoryWarptech", "iconCategoryDecorations",
                                          "iconCategoryCurios"};
    const unsigned long long t0 = GetTickCount64();
    size_t seen = 0;
    std::string found;
    for (Il2CppObject* s : unity::FindObjectsOfTypeAll(spriteClass_)) {
        if (sr2::core::ModuleFaulted(kModule)) break;
        if (!s || !unity::IsAlive(s)) continue;
        ++seen;
        const std::string n = unity::GetName(s);
        for (const char* w : kWanted) {
            if (n != w || namedSprites_.count(n)) continue;
            if (const uint32_t id = SpriteIcon(s)) {
                namedSprites_[n] = id;
                found += (found.empty() ? "" : ", ") + n;
            }
        }
    }
    SR2_INFO("[{}] game sprites by name: {} of {} wanted found among {} loaded sprites ({} ms): {}", kModule,
             namedSprites_.size(), std::size(kWanted), seen, GetTickCount64() - t0, found.empty() ? "none" : found);
    return true;
}

// The first of `names` that was found (EnsureNamedSprites), 0 if none.
uint32_t Sandbox::NamedSprite(std::initializer_list<const char*> names) {
    for (const char* n : names)
        if (auto it = namedSprites_.find(n); it != namedSprites_.end() && it->second) return it->second;
    return 0;
}

// Icon of the type's own group (IdentifiableType.groupType), else of the first group with an icon
// it belongs to. Only asked for types without any icon of their own (a few), once each.
uint32_t Sandbox::GroupIconFor(Il2CppObject* ident) {
    if (!offGroupIcon_) return 0;
    Il2CppObject* own = offIdentGroupType_ ? unity::Field<Il2CppObject*>(ident, offIdentGroupType_) : nullptr;
    if (IsA(own, groupClass_) && unity::IsAlive(own))
        if (const uint32_t i = SpriteIcon(unity::Field<Il2CppObject*>(own, offGroupIcon_))) return i;
    EnsureTypeGroups();
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
    EnsureTypeGroups();
    // Slimes and largos: the Slimepedia's own icons, if loaded (the slime one only while the Slimepedia
    // or a shop has been opened), else the Pink Slime's icon. The tab falls back to the first item's icon.
    groupIcons_[bridge::kGroupSlimes] = NamedSprite({"iconCategorySlimes", "pediaHomeSlimes"});
    groupIcons_[bridge::kGroupLargos] = NamedSprite({"iconLargoPedia_small", "iconLargoPedia"});
    if (!groupIcons_[bridge::kGroupSlimes] && spawnables_)
        for (const bridge::Item& it : *spawnables_)
            if (it.kind == bridge::Kind::Slime && it.icon)
                if (Il2CppObject* o = Lookup(spawnById_, it.id, identClass_); o && unity::GetName(o) == "Pink") {
                    groupIcons_[bridge::kGroupSlimes] = it.icon;
                    break;
                }
    auto lower = [](std::string s) { return Lower(std::move(s)); };
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
    SR2_INFO("[{}] group tab icons: plorts {}, slimes {}, largos {}, food {}, resources {} (0 = the first item's icon)",
             kModule, groupIcons_[bridge::kGroupPlorts], groupIcons_[bridge::kGroupSlimes], groupIcons_[bridge::kGroupLargos],
             groupIcons_[bridge::kGroupFood], groupIcons_[bridge::kGroupResources]);
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
    // Slime -> the largos it is a base slime of (the context menu's "Spawn: <slime> <other> Largo").
    for (const bridge::Item& it : *spawnables_)
        if (it.kind == bridge::Kind::Largo)
            for (uintptr_t b : {it.base, it.base2})
                if (b) (*rel)[b].largos.push_back(it.id);
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
    else if (containerTypes_.count(key)) k = bridge::Kind::Container;
    else if (offIdentIsAnimal_ && unity::Field<bool>(ident, offIdentIsAnimal_)) k = bridge::Kind::Animal;
    else if (foodTypes_.count(key)) k = bridge::Kind::Food;
    else if (resourceTypes_.count(key)) k = bridge::Kind::Resource;
    if (k == bridge::Kind::Other && isStorable_) {
        // What the refinery stores (plorts were matched above) is a crafting resource too.
        bool storable = false;
        if (Il2CppObject* dir = GadgetDirector();
            dir && CallValue(isStorable_, dir, {ident}, "GadgetDirector.IsStorable", storable) && storable)
            k = bridge::Kind::Resource;
    }
    if (k == bridge::Kind::Other && offIdentCategories_) {
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
    QueueIconRenders(*catalog_);
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
    // custom_upgrades' gadget clones have a save alias in the index but must never enter the inventory
    // (GadgetsModel.Pull would write the vanilla id twice).
    if (custom_upgrades::IsModGadget(ident)) return false;
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
    std::vector<std::string> leftOut, notActor;
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
        if (!actor || !IsA(actor, actorClass_)) {
            notActor.push_back(unity::GetName(o));
            continue;
        }
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
        if (item.kind == bridge::Kind::Largo && offSlimeBases_) {
            // Listed under the alphabetically first of its two base slimes (the Largos tab groups by it).
            void* bases = unity::Field<void*>(o, offSlimeBases_);
            std::vector<std::string> names;
            for (size_t i = 0, n = unity::ArrayLength(bases); i < n && names.size() < 2; ++i) {
                auto* b = static_cast<Il2CppObject*>(unity::ArrayAt(bases, i));
                if (!b || b == o || !IsA(b, slimeDefClass_) || !unity::IsAlive(b)) continue;
                names.push_back(ItemName(b));
                (names.size() == 1 ? item.base : item.base2) = reinterpret_cast<uintptr_t>(b);
            }
            std::sort(names.begin(), names.end());
            if (!names.empty()) item.family = names[0];
            if (names.size() > 1) item.family2 = names[1];
        }
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
    if (!notActor.empty()) {
        std::string names;
        for (const std::string& n : notActor) names += (names.empty() ? "" : ", ") + n;
        SR2_INFO("[{}] spawn list: {} type(s) with a prefab but no IdentifiableActor left out: {}", kModule, notActor.size(),
                 names);
    }
    LogSpawnGroups();
    QueueIconRenders(*spawnables_);
    crateAreasBuilt_ = false;  // areas for the new list
    return true;
}

// The area of each crate in the spawn list (bridge::Item::area, the spawner's Crates sections): the
// localized name of the zone whose crate list (ZoneDefinition._crates, SpawnableCrate.Type) has it,
// else the zone its asset name's code stands for (kCrateCodes: the zone whose _nameKey contains the
// code's key, by its localized name, else the English name). One FindObjectsOfTypeAll(ZoneDefinition).
void Sandbox::EnsureCrateAreas() {
    crateAreasBuilt_ = true;
    if (!spawnables_) return;
    std::unordered_map<uintptr_t, std::string> byType;
    std::vector<std::pair<std::string, std::string>> zones;  // _nameKey (lower case), localized name
    std::string zoneLog;
    auto& R = Resolver::Get();
    if (zoneClass_)
        for (Il2CppObject* z : unity::FindObjectsOfTypeAll(zoneClass_)) {
            if (!IsA(z, zoneClass_) || !unity::IsAlive(z)) continue;
            const std::string key =
                unity::ToUtf8(reinterpret_cast<Il2CppString*>(unity::Field<Il2CppObject*>(z, offZoneNameKey_)));
            std::string name = Localized(unity::Field<Il2CppObject*>(z, offZoneLocName_));
            if (name.rfind("!!", 0) == 0) name.clear();
            if (name.empty()) continue;
            zones.emplace_back(Lower(key), name);
            void* crates = unity::Field<void*>(z, offZoneCrates_);
            int listed = 0;
            for (size_t i = 0, n = unity::ArrayLength(crates); i < n; ++i) {
                auto* e = static_cast<Il2CppObject*>(unity::ArrayAt(crates, i));
                if (!e) continue;
                if (!offCrateType_) {
                    Il2CppClass* k = api().il2cpp_object_get_class ? api().il2cpp_object_get_class(e) : nullptr;
                    const auto o = k ? R.FieldOffset(k, "Type") : std::nullopt;
                    if (!o) break;
                    offCrateType_ = *o;
                }
                Il2CppObject* t = unity::Field<Il2CppObject*>(e, offCrateType_);
                if (!IsA(t, identClass_)) continue;
                byType.emplace(reinterpret_cast<uintptr_t>(t), name);
                ++listed;
            }
            zoneLog += std::format("{}{} \"{}\": {}", zoneLog.empty() ? "" : ", ", name, key, listed);
        }
    auto next = std::make_shared<std::vector<bridge::Item>>(*spawnables_);
    int fromZones = 0, fromCodes = 0;
    std::string unknown;
    for (bridge::Item& it : *next) {
        if (it.kind != bridge::Kind::Container) continue;
        if (auto f = byType.find(it.id); f != byType.end()) {
            it.area = f->second;
            ++fromZones;
            continue;
        }
        Il2CppObject* o = Lookup(spawnById_, it.id, identClass_);
        const std::string asset = o ? unity::GetName(o) : std::string();
        if (const CrateCode* c = CrateCodeOf(asset)) {
            it.area = c->zone;
            for (const auto& [key, name] : zones)
                if (key.find(c->zoneKey) != std::string::npos) {
                    it.area = name;
                    break;
                }
            ++fromCodes;
        } else {
            unknown += (unknown.empty() ? "" : ", ") + asset;
        }
    }
    spawnables_ = std::move(next);
    SR2_INFO("[{}] crate areas: {} from the zones' crate lists, {} from asset names{}{}; zones: {}", kModule, fromZones,
             fromCodes, unknown.empty() ? "" : "; unknown: ", unknown, zoneLog.empty() ? "none read" : zoneLog);
}

// How many spawn-list items each tab shows, logged once (the kinds come from the game's groups).
void Sandbox::LogSpawnGroups() {
    if (spawnGroupsLogged_ || !spawnables_) return;
    spawnGroupsLogged_ = true;
    int n[11] = {};
    std::string noIcon;
    int noIconCount = 0;
    for (const bridge::Item& it : *spawnables_) {
        ++n[static_cast<int>(it.kind) % 11];
        if (!it.icon && !it.icon2) {
            ++noIconCount;
            if (noIconCount <= 40) noIcon += (noIcon.empty() ? "" : ", ") + it.name;
        }
    }
    using K = bridge::Kind;
    auto c = [&](K k) { return n[static_cast<int>(k)]; };
    SR2_INFO("[{}] spawn list by tab: slimes {}, largos {}, plorts {}, food {} (meat {}), resources {}, crates {}; only under "
             "All: toys {}, other {}",
             kModule, c(K::Slime), c(K::Largo), c(K::Plort), c(K::Food) + c(K::Animal), c(K::Animal), c(K::Resource),
             c(K::Container), c(K::Toy), c(K::Other) + c(K::Liquid) + c(K::Gadget));
    if (noIconCount)
        SR2_INFO("[{}] spawn list: {} item(s) without a game icon (rendered from their prefabs next): {}{}", kModule,
                 noIconCount, noIcon, noIconCount > 40 ? ", ..." : "");
}

// Items without any icon get one rendered from their prefab later, one per refresh (RenderNextIcon).
void Sandbox::QueueIconRenders(const std::vector<bridge::Item>& items) {
    if (!prefabIcons_.Ready()) return;
    for (const bridge::Item& it : items) {
        if (it.icon || it.icon2 || renderQueued_.count(it.id)) continue;
        Il2CppObject* o = Lookup(spawnById_, it.id, identClass_);
        if (!o) o = CatalogItem(it.id, nullptr);
        if (!o) continue;
        Il2CppGCHandle h = unity::NewGCHandle(o);
        if (!h) continue;
        renderQueued_.insert(it.id);
        renderTypes_[it.id] = CatalogEntry{h, o};
        renderQueue_.push_back(it.id);
    }
}

// A copy of `list` with `icon` set on item `id` (published lists are never changed in place).
void Sandbox::PatchIcon(std::shared_ptr<const std::vector<bridge::Item>>& list, uintptr_t id, uint32_t icon) {
    if (!list) return;
    bool any = false;
    for (const bridge::Item& it : *list) any = any || it.id == id;
    if (!any) return;
    auto next = std::make_shared<std::vector<bridge::Item>>(*list);
    for (bridge::Item& it : *next)
        if (it.id == id) it.icon = icon;
    list = std::move(next);
}

// Renders the next queued item's prefab into an icon (prefab_icons.h) and puts it into the lists.
// True if it did the work this refresh.
bool Sandbox::RenderNextIcon() {
    if (renderQueue_.empty()) return false;
    const uintptr_t id = renderQueue_.front();
    renderQueue_.erase(renderQueue_.begin());
    const unsigned long long t0 = GetTickCount64();
    auto it = renderTypes_.find(id);
    Il2CppObject* o = it != renderTypes_.end() ? unity::GCHandleTarget(it->second.handle) : nullptr;
    if (!o || o != it->second.obj || !IsA(o, identClass_) || !unity::IsAlive(o)) return true;
    Il2CppObject* prefab = unity::Field<Il2CppObject*>(o, offIdentPrefab_);
    std::string why;
    Il2CppObject* spr = prefab ? prefabIcons_.Render(prefab, why) : nullptr;
    const uint32_t icon = spr ? SpriteIcon(spr) : 0;
    renderMs_ += GetTickCount64() - t0;
    if (icon) {
        ++rendered_;
        iconCache_[id] = icon;
        PatchIcon(spawnables_, id, icon);
        PatchIcon(catalog_, id, icon);
    } else {
        ++renderFailed_;
        if (renderFailed_ <= 5)
            SR2_INFO("[{}] no icon rendered for {}: {}", kModule, unity::GetName(o), why.empty() ? "no prefab" : why);
    }
    if (renderQueue_.empty())
        SR2_INFO("[{}] rendered {} icon(s) from prefabs, {} failed ({} ms in all)", kModule, rendered_, renderFailed_,
                 renderMs_);
    return true;
}

// "20 x Pink Plort, 150 Newbucks": the recipe's ident and currency costs as the fabricator charges them.
std::string Sandbox::RecipeCost(Il2CppObject* def, std::vector<bridge::CostLine>* lines) {
    std::string out;
    auto add = [&](const std::string& s) { out += (out.empty() ? "" : ", ") + s; };
    auto line = [&](const std::string& s, Il2CppObject* type) {
        if (!lines) return;
        bridge::CostLine l;
        l.text = s;
        if (type) {
            l.icon = ItemIcon(type);
            l.kind = ItemKind(type);
        } else {
            l.currency = true;
        }
        lines->push_back(std::move(l));
    };
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
            if (type && amount > 0) {
                add(std::format("{} x {}", amount, ItemName(type)));
                line(std::format("{} x {}", amount, ItemName(type)), type);
            }
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
        line(std::format("{} {}", Thousands(amount), bucks ? std::string("Newbucks") : c ? unity::GetName(c) : std::string("?")), nullptr);
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

// The fabricator's gadget categories from its FabricateMenuConfiguration asset (one
// FindObjectsOfTypeAll of that class). Nothing until the asset is loaded, which is
// when the fabricator UI is first opened.
void Sandbox::ReadFabCategories(std::vector<FabCat>& cats, std::vector<std::string>& titles, std::vector<uint32_t>& icons) {
    if (!fabMenuClass_) return;
    for (Il2CppObject* menu : unity::FindObjectsOfTypeAll(fabMenuClass_)) {
        if (!IsA(menu, fabMenuClass_) || !unity::IsAlive(menu)) continue;
        void* list = unity::Field<void*>(menu, offFabCategories_);
        for (size_t i = 0, n = unity::ListCount(list); i < n; ++i) {
            auto* c = static_cast<Il2CppObject*>(unity::ListAt(list, i));
            if (c && fabPinnedCatClass_ && offFabCatIcon_ && IsA(c, fabPinnedCatClass_) && unity::IsAlive(c))
                pinnedIcon_ = SpriteIcon(unity::Field<Il2CppObject*>(c, offFabCatIcon_));
            if (!c || !IsA(c, fabGadgetCatClass_) || !unity::IsAlive(c)) continue;
            std::string t = Localized(unity::Field<Il2CppObject*>(c, offFabCatTitle_));
            if (t.empty()) t = unity::GetName(c);
            cats.push_back({c, static_cast<int>(titles.size())});
            titles.push_back(t);
            icons.push_back(offFabCatIcon_ ? SpriteIcon(unity::Field<Il2CppObject*>(c, offFabCatIcon_)) : 0);
        }
        if (!cats.empty()) break;  // one menu is enough
    }
}

// The Fabricator's Pinned tab icon (PinnedFabricateCategory._icon). ReadFabCategories only sees it when
// the recipe list is built after the Fabricator menu asset was loaded; built earlier (from the gadget
// groups, the usual case) it was never looked at again, so the overlay's Pinned tab showed its fallback
// (round 7: "no pinned icon, opened refinery but still nothing"). Searched by its own class (one
// FindObjectsOfTypeAll) once, and again after each Fabricator UI was built (FabricatorUI.Start: the asset
// comes with it), while missing. Not on a timer: a 5 s FindObjectsOfTypeAll retry for the menu asset
// cost a 25 ms frame each time (see the Crafter notes). True if it searched.
bool Sandbox::EnsurePinnedIcon() {
    if (pinnedIcon_ || !fabPinnedCatClass_ || !offFabCatIcon_ || pinnedIconTry_ == g_fabStarts) return false;
    const bool first = pinnedIconTry_ < 0;
    pinnedIconTry_ = g_fabStarts;
    for (Il2CppObject* c : unity::FindObjectsOfTypeAll(fabPinnedCatClass_)) {
        if (!c || !IsA(c, fabPinnedCatClass_) || !unity::IsAlive(c)) continue;
        if ((pinnedIcon_ = SpriteIcon(unity::Field<Il2CppObject*>(c, offFabCatIcon_)))) {
            SR2_INFO("[{}] crafter: the Fabricator's Pinned icon found ({})", kModule, unity::GetName(c));
            return true;
        }
    }
    if (first) SR2_INFO("[{}] crafter: the Fabricator's Pinned icon is not loaded yet (the overlay draws a pin until it is)", kModule);
    return true;
}

// Index of the first category that contains the gadget, -1 if none. `order`: its position there as the
// Fabricator lists it (GadgetFabricateCategory.IndexOf), -1 if unknown.
int Sandbox::RecipeCategory(const std::vector<FabCat>& cats, Il2CppObject* def, int* order) {
    *order = -1;
    for (const FabCat& c : cats) {
        bool in = false;
        if (CallValue(fabCatContains_, c.obj, {def}, "GadgetFabricateCategory.Contains", in) && in) {
            int at = -1;
            if (fabCatIndexOf_ && CallValue(fabCatIndexOf_, c.obj, {def}, "GadgetFabricateCategory.IndexOf", at)) *order = at;
            return c.index;
        }
    }
    return -1;
}

// The fabricator's categories without its menu asset (only loaded once the fabricator UI has been
// opened): each GadgetFabricateCategory is one of the GadgetGroup's member groups (_gadgetSubGroup:
// GadgetUtilitiesGroup, GadgetWarpGroup, GadgetDecorGroup; GadgetHiddenGroup holds the variants),
// which are loaded from the start. Title from the group's localized name, icon from the
// fabricator's own category sprites by name when loaded (EnsureNamedSprites).
void Sandbox::GroupFabCategories(std::vector<std::unordered_set<uintptr_t>>& members, std::vector<std::string>& titles,
                                 std::vector<uint32_t>& icons, std::vector<std::unordered_map<uintptr_t, int>>& orders) {
    Il2CppObject* root = offGroupMembers_ ? NamedGroup("GadgetGroup") : nullptr;
    if (!root) return;
    void* subs = unity::Field<void*>(root, offGroupSubGroups_);
    for (size_t i = 0, n = unity::ListCount(subs); i < n; ++i) {
        auto* g = static_cast<Il2CppObject*>(unity::ListAt(subs, i));
        if (!g || !IsA(g, groupClass_) || !unity::IsAlive(g)) continue;
        const std::string asset = unity::GetName(g);
        const std::string low = Lower(asset);
        if (low.find("hidden") != std::string::npos || low.find("variant") != std::string::npos) continue;
        std::unordered_set<uintptr_t> set;
        CollectMembers(g, set);
        if (set.empty()) continue;
        std::string title = offGroupLocName_ ? Localized(unity::Field<Il2CppObject*>(g, offGroupLocName_)) : std::string();
        if (title.empty() || title.rfind("!!", 0) == 0) {
            title = PrettyAssetName(asset);
            for (const char* cut : {"Gadget ", " Group"})
                if (const size_t p = title.find(cut); p != std::string::npos) title.erase(p, std::strlen(cut));
        }
        uint32_t icon = 0;
        if (low.find("util") != std::string::npos) icon = NamedSprite({"iconCategoryUtilities"});
        else if (low.find("warp") != std::string::npos) icon = NamedSprite({"iconCategoryWarptech"});
        else if (low.find("decor") != std::string::npos) icon = NamedSprite({"iconCategoryDecorations"});
        else if (low.find("curio") != std::string::npos) icon = NamedSprite({"iconCategoryCurios"});
        // The Fabricator lists a category in its group's member order (GadgetFabricateCategory.IndexOf).
        std::unordered_map<uintptr_t, int> order;
        void* types = unity::Field<void*>(g, offGroupMembers_);
        for (size_t k = 0, m = unity::ListCount(types); k < m; ++k)
            if (void* t = unity::ListAt(types, k)) order.emplace(reinterpret_cast<uintptr_t>(t), static_cast<int>(k));
        members.push_back(std::move(set));
        titles.push_back(title);
        icons.push_back(icon);
        orders.push_back(std::move(order));
    }
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
    std::vector<FabCat> cats;
    auto titles = std::make_shared<std::vector<std::string>>();
    auto catIcons = std::make_shared<std::vector<uint32_t>>();
    ReadFabCategories(cats, *titles, *catIcons);
    std::vector<std::unordered_set<uintptr_t>> groupCats;  // without the fabricator's menu asset: its groups
    std::vector<std::unordered_map<uintptr_t, int>> groupOrders;
    const char* catSource = "the fabricator's menu";
    if (cats.empty()) {
        GroupFabCategories(groupCats, *titles, *catIcons, groupOrders);
        catSource = "the gadget groups";
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
        r.cost = RecipeCost(d, &r.costs);
        r.perCraft = unity::Field<bool>(d, offBuyInPairs_) ? 2 : 1;
        r.category = RecipeCategory(cats, d, &r.order);
        for (size_t i = 0; i < groupCats.size() && r.category < 0; ++i)
            if (groupCats[i].count(r.id)) {
                r.category = static_cast<int>(i);
                const auto at = groupOrders[i].find(r.id);
                r.order = at != groupOrders[i].end() ? at->second : -1;
            }
        if (referenceId_) {
            bool ok = false;
            Il2CppObject* ref = Call(referenceId_, d, {}, "IdentifiableType.ReferenceId", &ok);
            const std::string key = ok && ref ? unity::ToUtf8(reinterpret_cast<Il2CppString*>(ref)) : std::string();
            if (!key.empty()) recipeByRef_[key] = r.id;
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
    std::string catNames;
    for (size_t i = 0; i < recipeCats_->size(); ++i) {
        int count = 0;
        for (const bridge::Recipe& r : *recipes_) count += r.category == static_cast<int>(i) ? 1 : 0;
        catNames += std::format("{}{} {}{}", catNames.empty() ? "" : ", ", (*recipeCats_)[i], count,
                                (*recipeCatIcons_)[i] ? "" : " (no icon)");
    }
    SR2_INFO("[{}] crafter: {} gadget recipes, {} categories from {}: {} ({} ms)", kModule, recipes_->size(),
             recipeCats_->size(), catSource, catNames.empty() ? "none" : catNames, GetTickCount64() - t0);
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

// The recipe pinned in the game's Fabricator (PlayerModel.PinnedRecipe, an IRecipe whose ReferenceId is
// the gadget's), as a recipe id; 0 if none or not a gadget (upgrades can be pinned too). Resolved again
// only when the pinned object changes.
uintptr_t Sandbox::PinnedRecipe(const Ctx& c) {
    if (!offPinnedRecipe_ || !c.model) return 0;
    auto* pinned = unity::Field<Il2CppObject*>(c.model, offPinnedRecipe_);
    if (pinned == pinnedObj_) return pinnedId_;
    pinnedObj_ = pinned;
    pinnedId_ = 0;
    if (!pinned) return 0;
    const MethodInfo* getRef = nullptr;
    const auto& a = api();
    for (Il2CppClass* k = a.il2cpp_object_get_class ? a.il2cpp_object_get_class(pinned) : nullptr; k && !getRef;
         k = a.il2cpp_class_get_parent ? a.il2cpp_class_get_parent(k) : nullptr)
        getRef = Resolver::Get().Method(k, "get_ReferenceId", 0);
    bool ok = false;
    Il2CppObject* ref = getRef ? Call(getRef, pinned, {}, "IRecipe.ReferenceId", &ok) : nullptr;
    const std::string key = ok && ref ? unity::ToUtf8(reinterpret_cast<Il2CppString*>(ref)) : std::string();
    if (const auto it = recipeByRef_.find(key); it != recipeByRef_.end()) pinnedId_ = it->second;
    SR2_INFO("[{}] crafter: pinned recipe '{}'{}", kModule, key, pinnedId_ ? "" : " (not a gadget recipe: the Pinned tab stays empty)");
    return pinnedId_;
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
        // One-time scans, at most one per refresh so they don't stack into one long frame. The type
        // groups come first: the item kinds (also the slots' below) are read from them and cached.
        bool built = EnsureTypeGroups();
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
                if (!slot.unlocked) slot.unlockable = SlotUnlockUpgrades(c, a, nullptr);
            }
            s.slots.push_back(std::move(slot));
        }
        if (!built) built = EnsureNamedSprites();
        if (!built) built = c.slots && EnsureCatalog(c);
        s.catalog = catalog_;
        if (!built) built = EnsureSpawnables();
        s.spawnables = spawnables_;
        s.spawnWhy = spawnWhy_;
        s.tarrsOff = spawnables_ && TarrsOff();
        if (!built) built = EnsureRecipes();
        if (!built) built = EnsureMaterials();
        // Cheap one-time extras, each in its own refresh after the lists.
        if (!built && spawnables_ && !relations_) EnsureRelations(), built = true;
        if (!built && spawnables_ && !groupIconsBuilt_) EnsureGroupIcons(), built = true;
        if (!built && spawnables_ && !crateAreasBuilt_) EnsureCrateAreas(), built = true;
        if (!built && recipes_) built = EnsurePinnedIcon();
        if (!built) built = RenderNextIcon();
        s.catalog = catalog_;  // icons rendered into the lists
        s.spawnables = spawnables_;
        if ((recipes_ || materials_) && s.tick - lastRecipeState_ >= kRecipeStateMs) {
            lastRecipeState_ = s.tick;
            RefreshRecipeState();
        }
        s.recipes = recipes_;
        s.recipeCategories = recipeCats_;
        s.recipeCategoryIcons = recipeCatIcons_;
        s.materials = materials_;
        s.craftWhy = craftWhy_;
        s.pinnedRecipe = recipes_ ? PinnedRecipe(c) : 0;
        s.pinnedIcon = pinnedIcon_;
        s.relations = relations_;
        std::copy(std::begin(groupIcons_), std::end(groupIcons_), std::begin(s.groupIcons));
        if (incUpgrade_ && s.tick - lastUpgState_ >= kRecipeStateMs) {
            lastUpgState_ = s.tick;
            UpgradeState(c);
        }
        s.upgradeCount = upgCount_;
        s.upgradeLevelsLeft = upgLeft_;
        s.upgradesHidden = upgHidden_;
        if (!incUpgrade_) s.upgradeWhy = "The game's upgrade functions were not found (see the log).";
        else if (upgLeft_ < 0) s.upgradeWhy = "The player's upgrade list was not found.";
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

void Sandbox::Tick(float dt) {
    if (!Live()) {
        if (fabHint_.Exists()) DropFabricatorHint();  // Sandbox off: the key bar entry goes
        return;
    }
    if (notices_.Ready()) notices_.Frame(dt);  // queued HUD notices wait for the HUD (menus closed)
    if (const unsigned v = sandbox::SaveTogglesVersion(); v != togglesSeen_) {  // another save's toggles, or enabled
        togglesSeen_ = v;
        const bool on = sandbox::SaveToggle(sandbox::kToggleInfiniteEnergy);
        if (infinite_.exchange(on) != on) SR2_INFO("[{}] infinite energy {} (as this save has it)", kModule, on ? "on" : "off");
    }
    const unsigned long long now = GetTickCount64();
    if (infinite_.load() && now - lastEnergy_ >= kEnergyMs) {
        lastEnergy_ = now;
        TopUpEnergy();
    }
    FabricatorTick(now);
    if (!batchNotices_.empty() && now - batchTick_ > 2000) {  // a batch whose last request never came
        FlushNotices();
        batchIdx_ = batchSize_ = 0;
    }
    const bool snap = bridge::WantsSnapshot() && now - lastSnap_ >= kSnapshotMs;
    if (snap) {
        lastSnap_ = now;
        Refresh();
    }
    sandbox::PlayerExtrasTick(snap);
    sandbox::UpgradesTick(bridge::WantsSnapshot());
}

void Sandbox::Done(bool ok, const std::string& text) {
    if (ok) SR2_INFO("[{}] {}", kModule, text);
    else SR2_WARN("[{}] {}", kModule, text);
    if (!quiet_) bridge::SetStatus(text, !ok);
}

// A spawn / craft / refinery add that worked: `text` is logged, and a HUD pop-up in the game's
// notification list with the type's icon says it ("Spawned 5 Pink Plort"), instead of the tab's status
// line. It is our own text entry (SaleNotices::QueueText), not the drones' cloud-upload notice
// (AddResourceCollected reads "... sent to the Quantum Cloud"). Without the HUD list or the UI's text
// sink the result is only logged.
// Part of a multi-item request (Craft N / Spawn N / Put in slots with several items selected): the
// notices are collected and shown after the request's last item, one per item type with its own icon
// and count (FlushNotices). The game's list shows one page of entries at a time; SaleNotices hands
// the queued notices over a page at a time, when the list is idle, so every one of them is shown.
void Sandbox::Notify(Il2CppObject* type, NoticeKind kind, int count, const std::string& name, const std::string& text) {
    SR2_INFO("[{}] {}", kModule, text);
    if (batchSize_ > 1) {
        batchNotices_.push_back({kind, name, count, NoticeIcon(type)});
        batchTick_ = GetTickCount64();
        return;
    }
    ShowNotice(NoticeIcon(type), NoticeText(kind, count, name));
}

std::string Sandbox::NoticeText(NoticeKind kind, int count, const std::string& name) {
    return kind == NoticeKind::Spawn   ? std::format("Spawned {} {}", count, name)
           : kind == NoticeKind::Craft ? std::format("Crafted {} {}", count, name)
                                       : std::format("+{} {} (refinery)", count, name);
}

// The type's sprite for a notice: IdentifiableType.icon, else its get_Icon.
Il2CppObject* Sandbox::NoticeIcon(Il2CppObject* type) {
    Il2CppObject* icon = type && offIdentIcon_ ? unity::Field<Il2CppObject*>(type, offIdentIcon_) : nullptr;
    if ((!icon || !unity::IsAlive(icon)) && type) icon = VirtualIcon(type);
    if (icon && ((spriteClass_ && !IsA(icon, spriteClass_)) || !unity::IsAlive(icon))) icon = nullptr;
    return icon;
}

bool Sandbox::ShowNotice(Il2CppObject* icon, const std::string& notice, bool flushNow) {
    if (icon && ((spriteClass_ && !IsA(icon, spriteClass_)) || !unity::IsAlive(icon))) icon = nullptr;
    if (notices_.TextReady() && notices_.Available() && notices_.QueueText(icon, notice, flushNow)) {
        bridge::SetStatus("", false);  // no stale line from an earlier request
        return true;
    }
    bridge::SetStatus(notice, false);
    return false;
}

// The collected notices of a multi-item request: one per item type and kind (the same type twice, e.g.
// put in slots and the rest dropped, adds up), in the order they came.
void Sandbox::FlushNotices() {
    std::vector<NoticeLine> lines;
    lines.swap(batchNotices_);
    std::vector<NoticeLine> merged;
    for (const NoticeLine& l : lines) {
        auto it = std::find_if(merged.begin(), merged.end(),
                               [&](const NoticeLine& m) { return m.kind == l.kind && m.name == l.name; });
        if (it != merged.end()) it->count += l.count;
        else merged.push_back(l);
    }
    // All queued first, then handed over together: the first page holds as many as the HUD shows at once
    // (one by one, the first notice took a page of its own and the rest waited for it to fade out).
    bool queued = false;
    for (const NoticeLine& l : merged) queued = ShowNotice(l.icon, NoticeText(l.kind, l.count, l.name), false) || queued;
    if (queued) notices_.FlushText();
    if (merged.size() > 1) SR2_INFO("[{}] {} HUD notices queued (one per item)", kModule, merged.size());
}

// ---------------------------------------------------------------- actions ----
void Sandbox::Handle(const bridge::Action& a) {
    if (!Live()) {
        bridge::SetStatus("Sandbox is off.", true);
        return;
    }
    // A multi-item request: its items' notices are collected and shown together after the last one.
    if (a.batchSize <= 1 || a.batch <= 1) {
        if (!batchNotices_.empty()) FlushNotices();  // an earlier batch that never got its last item
        spawnNext_ = 0;
    }
    batchIdx_ = a.batch;
    batchSize_ = a.batchSize;
    struct BatchEnd {
        Sandbox* self;
        ~BatchEnd() {
            if (self->batchSize_ <= 1 || self->batchIdx_ >= self->batchSize_) {
                if (!self->batchNotices_.empty()) self->FlushNotices();
                self->batchIdx_ = self->batchSize_ = 0;
            }
        }
    } batchEnd{this};
    if (a.op == bridge::Op::InfiniteEnergy) {
        const bool on = a.value != 0;
        if (infinite_.exchange(on) != on)
            Done(true, on ? "Infinite energy on (energy is refilled to its maximum; kept for this save)"
                          : "Infinite energy off");
        sandbox::SetSaveToggle(sandbox::kToggleInfiniteEnergy, on);
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
        case bridge::Op::Spawn: DoSpawn(c, a.item, a.value, kMaxDrop); break;
        case bridge::Op::StowItem: DoStow(c, a.slot, a.item, a.value, a.replace); break;
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
        case bridge::Op::UnlockUpgrades: DoUnlockUpgrades(c); break;
        case bridge::Op::DiscoverUpgrades: DoDiscoverUpgrades(c); break;
        case bridge::Op::InfiniteEnergy: break;  // handled above
    }
    // A batch's notices go to the HUD before the refreshes below (they used to wait for them). The items
    // before a batch's last one skip the refreshes: the last one does them once.
    const bool batchMid = batchSize_ > 1 && batchIdx_ < batchSize_;
    if (!batchMid && !batchNotices_.empty()) FlushNotices();
    if (batchMid) return;
    // An open Fabricator shows the new counts at once (else only after selecting another recipe and back).
    if (a.op == bridge::Op::AddMaterial || a.op == bridge::Op::Craft || a.op == bridge::Op::SetNewbucks ||
        a.op == bridge::Op::AddNewbucks)
        RefreshFabricator();
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
    if (ok) RefreshPurchaseMenus();
    Done(ok, ok ? std::format("Newbucks {}: {} -> {}{}", how, Thousands(cur), Thousands(now),
                              clamped != target ? std::format(" (clamped to 0..{})", Thousands(max)) : std::string())
                : std::format("Newbucks not changed ({} failed)", delta > 0 ? "AddCurrency" : "SpendCurrency"));
}

// After a Newbucks change: an open plot purchase menu (LandPlotUIRoot) shows the new amount and what is
// affordable. AddCurrency / SpendCurrency already raise the game's currency events (PlayerModel.
// CurrencyChanged -> PlayerState.DisplayCurrencyChanged), which the HUD's currency lists follow, but the
// plot menu reads the amounts when it builds its list and only rebuilds after its own purchases
// (ListUIRoot.RebuildUI -> IListViewSwapper.RefreshAndAdvance). We call that rebuild, the most derived
// override, on the open menu: one FindObjectOfType per Newbucks request, nothing otherwise.
void Sandbox::RefreshPurchaseMenus() {
    if (!landPlotUiClass_) return;
    Il2CppObject* ui = unity::FindObjectOfType(landPlotUiClass_);
    if (!ui || !unity::IsAlive(ui) || !IsA(ui, landPlotUiClass_) || !unity::ActiveInHierarchy(unity::GameObjectOf(ui))) return;
    const auto& a = api();
    const MethodInfo* rebuild = nullptr;
    for (Il2CppClass* k = a.il2cpp_object_get_class ? a.il2cpp_object_get_class(ui) : nullptr; k && !rebuild;
         k = a.il2cpp_class_get_parent ? a.il2cpp_class_get_parent(k) : nullptr)
        rebuild = Resolver::Get().Method(k, "RebuildUI", 0);
    bool ok = false;
    if (rebuild) Call(rebuild, ui, {}, "LandPlotUIRoot.RebuildUI", &ok);
    SR2_INFO("[{}] Newbucks: open plot menu {}", kModule, ok ? "rebuilt (new amounts)" : "not rebuilt (RebuildUI not found or failed)");
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

// "Put in slots" for one item. `amount` (<= 0: the maximum of the first slot it goes into)
// goes into the selected slot first (emptied before only with `replace`, else used only if
// it is empty or already holds the item), then into the other unlocked slots that accept it
// and hold it or are empty, in order after the selected one. Other items are never removed
// from those. What is left is dropped in front of the player through the spawner (if the
// item is in the spawn list), at most kMaxDrop. One summary line, every step's reason logged.
void Sandbox::DoStow(const Ctx& c, int sel, uintptr_t item, int64_t amount, bool replace) {
    uint32_t mask = 0;
    Il2CppObject* ident = CatalogItem(item, &mask);
    if (!ident) {
        Done(false, "Put in slots: the item is not in the vacpack item list (anymore)");
        return;
    }
    const std::string name = ItemName(ident);
    if (!Saveable(SaveIndex(), ident)) {
        Done(false, std::format("Put in slots: {} not put, the game can't save it", name));
        return;
    }
    const int n = c.slots ? static_cast<int>(std::min<size_t>(unity::ArrayLength(c.slots), kMaxSlots)) : 0;
    if (!c.ammo || n == 0) {
        Done(false, std::format("Put in slots: {} not put, no vacpack slots", name));
        return;
    }
    if (sel < 0 || sel >= n) sel = 0;
    int left = amount > 0 ? static_cast<int>(std::min<int64_t>(amount, 1'000'000)) : -1;  // -1: the first slot's maximum
    std::string where, skipped, replaced;
    for (int k = 0; k < n && left != 0; ++k) {
        int idx = (sel + k) % n;
        Il2CppObject* slot = SlotAt(c, idx);
        if (!slot) continue;
        if (!(mask & (1u << idx))) continue;  // the slot does not take this item
        bool unlocked = false;
        if (!CallValue(slotUnlocked_, slot, {}, "AmmoSlot.IsUnlocked", unlocked) || !unlocked) continue;
        int max = 0;
        if (!CallValue(slotMax_, c.ammo, {&idx}, "AmmoSlotManager.GetSlotMaxCount", max) || max <= 0) continue;
        Il2CppObject* curId = unity::Field<Il2CppObject*>(slot, offSlotId_);
        int cur = unity::Field<int32_t>(slot, offSlotCount_);
        if (cur > 0 && curId != ident) {
            if (k != 0 || !replace) {
                skipped += std::format(" {}", idx + 1);
                continue;  // holds something else: left alone
            }
            int sub = cur;
            bool ok = false;
            Call(decrement_, c.ammo, {&idx, &sub}, "AmmoSlotManager.Decrement", &ok);
            cur = unity::Field<int32_t>(slot, offSlotCount_);
            if (!ok || cur > 0) {
                skipped += std::format(" {}", idx + 1);
                continue;
            }
            replaced = std::format(" (replaced {} x {} in slot {})", sub, ItemName(curId), idx + 1);
        }
        if (cur <= 0) cur = 0;
        if (left < 0) {  // "a slot's maximum": top this first slot up to it
            left = max - cur;
            if (left == 0) {
                Done(true, std::format("Put in slots: slot {} is already full of {}", idx + 1, name));
                return;
            }
        }
        int add = std::min(left, max - cur);
        if (add <= 0) continue;  // already full of it
        bool overflow = false, added = false;
        if (!CallValue(addResource_, c.ammo, {ident, &idx, &add, &overflow}, "AmmoSlotManager.MaybeAddResource", added) || !added) {
            SR2_WARN("[{}] put in slots: the game refused {} x {} for slot {}", kModule, add, name, idx + 1);
            continue;
        }
        left -= add;
        where += std::format("{}slot {} +{}", where.empty() ? "" : ", ", idx + 1, add);
    }
    if (left < 0) {  // no slot took any (the amount was "a slot's maximum")
        Done(false, std::format("Put in slots: no unlocked slot takes {} or has room for it{}", name,
                                skipped.empty() ? std::string() : " (slots" + skipped + " hold other items)"));
        return;
    }
    std::string dropped;
    bool ok = !where.empty();
    if (left > 0) {
        bool spawnable = false;
        if (spawnables_)
            for (const bridge::Item& it : *spawnables_) spawnable = spawnable || it.id == item;
        if (!spawnable) {
            dropped = std::format("; {} didn't fit and can't be dropped (not in the spawn list)", left);
        } else {
            const int want = std::min(left, kMaxDrop);
            const int got = DoSpawn(c, item, want, kMaxDrop);  // logs its own line
            ok = ok || got > 0;
            dropped = got > 0 ? std::format("; dropped {} in front of you", got) : std::format("; {} didn't fit and could not be dropped", left);
            if (got > 0 && left > got) dropped += std::format(" ({} more didn't fit: at most {} are dropped at once)", left - got, kMaxDrop);
        }
    }
    Done(ok, std::format("Put in slots: {} -> {}{}{}{}", name, where.empty() ? std::string("no slot") : where, replaced, dropped,
                         skipped.empty() || !where.empty() ? std::string() : " (slots" + skipped + " hold other items)"));
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
int Sandbox::DoSpawn(const Ctx& c, uintptr_t item, int64_t count, int limit) {
    if (!spawnWhy_.empty()) {
        Done(false, "Spawner unavailable: " + spawnWhy_);
        return 0;
    }
    Il2CppObject* ident = Lookup(spawnById_, item, identClass_);
    if (!ident) {
        Done(false, "Not spawned: the item is not in the spawn list (anymore)");
        return 0;
    }
    const std::string name = ItemName(ident);
    if (!Saveable(SaveIndex(), ident)) {
        Done(false, std::format("{} not spawned: the game can't save it", name));
        return 0;
    }
    if (spawnables_ && TarrsOff())
        for (const bridge::Item& it : *spawnables_)
            if (it.id == item && it.tarr) {
                Done(false, std::format("{} not spawned: Tarrs are disabled in this save's game settings", name));
                return 0;
            }
    Il2CppObject* prefab = unity::Field<Il2CppObject*>(ident, offIdentPrefab_);
    if (!prefab || !unity::IsAlive(prefab)) {
        Done(false, std::format("{} not spawned: it has no prefab", name));
        return 0;
    }
    Il2CppObject* scene = nullptr;
    bool found = false;
    if (!CallValue(tryGetSceneContext_, nullptr, {&scene}, "SceneContextUtility.TryGetSceneContext", found) || !found ||
        !scene || !unity::IsAlive(scene)) {
        Done(false, std::format("{} not spawned: no scene context", name));
        return 0;
    }
    Il2CppObject* regions = unity::Field<Il2CppObject*>(scene, offScRegionRegistry_);
    bool ok = false;
    Il2CppObject* group = regions ? Call(currentSceneGroup_, regions, {}, "RegionRegistry.CurrentSceneGroup", &ok) : nullptr;
    if (!group || !unity::IsAlive(group)) {
        Done(false, std::format("{} not spawned: the current scene group is unknown", name));
        return 0;
    }
    Vec3 eye{}, fwd{};
    if (!SpawnPose(c, eye, fwd)) {
        Done(false, std::format("{} not spawned: could not read where you are looking", name));
        return 0;
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
    const int n = static_cast<int>(std::clamp<int64_t>(count, 1, std::max(1, limit)));
    int cols = std::min(n, n > kMaxSpawn ? 10 : 5);  // more than a click's worth: wider rows, fewer of them
    // Part of a multi-item request: one grid for all its items (rows of 10), each item after the last.
    const bool batch = batchSize_ > 1;
    const int base = batch ? spawnNext_ : 0;
    if (batch) cols = 10;
    const Vec3 right{fwd.z, 0, -fwd.x};
    Quat rot{0, 0, 0, 1};
    int spawned = 0;
    for (int i = 0; i < n; ++i) {
        const int at = base + i;
        const float side = (static_cast<float>(at % cols) - static_cast<float>(cols - 1) * 0.5f) * 0.55f;
        Vec3 pos{eye.x + fwd.x * dist + right.x * side, eye.y - 0.3f + static_cast<float>(at / cols) * 0.7f,
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
    if (batch) spawnNext_ = base + spawned;
    if (spawned == n) Notify(ident, NoticeKind::Spawn, n, name, std::format("Spawned {} x {}", n, name));
    else Done(spawned > 0, spawned > 0 ? std::format("Spawned {} of {} x {} (the game refused the rest)", spawned, n, name)
                                       : std::format("{} not spawned: the game refused it", name));
    return spawned;
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
    if (ok && after > before)
        Notify(def, NoticeKind::Craft, after - before, name,
               std::format("Crafted {} x {} for free (inventory {} -> {}){}", after - before, name, before, after,
                           unlocked ? "" : "; its blueprint is still locked"));
    else
        Done(false, std::format("{} not crafted: the game refused it", name));
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
    if (ok && after > before)
        Notify(t, NoticeKind::Refinery, after - before, name, std::format("Refinery: {} {} -> {}", name, before, after));
    else Done(false, std::format("{} not added: the game refused it", name));
}

// The upgrades the fabricator sells that drive `slot`'s unlock property: every UpgradeDefinition (from the
// player's UpgradeModel.upgradeDefinitions, in list order: vanilla first, custom_upgrades' tiers behind their
// parent) with a level whose effects target the slot definition's _isUnlockedProperty (AmmoSlot.IsUnlocked: that
// property >= _isUnlockedThreshold). Field reads plus one GetUpgradeLevel per match. The effects are ADD
// modifiers relative to the property's default, and several upgrades stack (Extra Tank I-II +1 / +2 on
// AmmoSlotsAvailable, custom_upgrades' Extra Tank III-IV on top), so the level that unlocks a slot can't be read
// off one modifier's value: DoUnlockSlot buys levels in this order until the slot reports unlocked. True if any
// of them still has a level to buy.
bool Sandbox::SlotUnlockUpgrades(const Ctx& c, Il2CppObject* slot, Il2CppObject** upgradeModel,
                                 std::vector<Il2CppObject*>* out) {
    if (out) out->clear();
    if (!incUpgrade_ || !slot) return false;
    Il2CppObject* def = unity::Field<Il2CppObject*>(slot, offSlotDef_);
    if (!IsA(def, slotDefClass_) || !unity::IsAlive(def)) return false;
    Il2CppObject* prop = unity::Field<Il2CppObject*>(def, offSlotDefUnlockProp_);
    if (!prop) return false;
    Il2CppObject* um = nullptr;
    std::vector<Il2CppObject*> ups;
    if (!PlayerUpgrades(c, um, ups)) return false;
    if (upgradeModel) *upgradeModel = um;
    const auto& a = api();
    bool open = false;
    for (Il2CppObject* up : ups) {
        bool targets = false;
        void* levels = unity::Field<void*>(up, offUdLevels_);
        const size_t nl = unity::ListCount(levels);
        for (size_t l = 0; l < nl && !targets; ++l) {
            auto* lvl = static_cast<Il2CppObject*>(unity::ListAt(levels, l));
            if (!lvl) continue;
            if (!offUlEffects_) {
                auto o = Resolver::Get().FieldOffset(a.il2cpp_object_get_class(lvl), "Effects");
                if (!o) return false;
                offUlEffects_ = *o;
            }
            void* effects = unity::Field<void*>(lvl, offUlEffects_);
            for (size_t e = 0, ne = unity::ListCount(effects); e < ne && !targets; ++e) {
                auto* mod = static_cast<Il2CppObject*>(unity::ListAt(effects, e));
                if (!mod) continue;
                Il2CppClass* mk = a.il2cpp_object_get_class(mod);
                if (mk != modClass_) {
                    auto o = Resolver::Get().FieldOffset(mk, "TargetProperty");
                    if (!o) continue;
                    modClass_ = mk;
                    offModTarget_ = *o;
                }
                targets = unity::Field<Il2CppObject*>(mod, offModTarget_) == prop;
            }
        }
        if (!targets) continue;
        if (out) out->push_back(up);
        int cur = -1;
        if (CallValue(getUpgradeLevel_, um, {up}, "UpgradeModel.GetUpgradeLevel", cur) && cur < static_cast<int>(nl) - 1)
            open = true;
    }
    return open;
}

// Unlocks a locked slot the way buying the vacpack upgrades does (PlayerUpgradeFabricatableItem.
// FabricateAndSpendCost, 1.3.0 RVA 0x82AFA0, minus the spending and analytics): UpgradeModel.
// IncrementUpgradeLevel one level at a time (SetUpgradeLevel raises OnUpgradeChanged, and ActorUpgradeHandler
// applies _levels[n].Effects at once), on each upgrade that drives the slot's property in list order, until the
// slot reports unlocked. Earlier slots the same levels unlock open too, as in the shop.
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
    std::vector<Il2CppObject*> ups;
    SlotUnlockUpgrades(c, slot, &um, &ups);
    if (ups.empty() || !um) {
        Done(false, std::format("Slot {} not unlocked: no vacpack upgrade unlocks it", idx + 1));
        return;
    }
    int levels = 0;
    std::string list, names;
    for (Il2CppObject* up : ups) {
        names += (names.empty() ? "" : ", ") + unity::GetName(up);
        if (unlocked) continue;
        const int last = static_cast<int>(unity::ListCount(unity::Field<void*>(up, offUdLevels_))) - 1;
        int from = -1;
        if (!CallValue(getUpgradeLevel_, um, {up}, "UpgradeModel.GetUpgradeLevel", from)) continue;
        int cur = from;
        for (int guard = 0; guard < 16 && !unlocked && cur < last; ++guard) {
            bool inc = false;
            if (!CallValue(incUpgrade_, um, {up}, "UpgradeModel.IncrementUpgradeLevel", inc) || !inc) break;
            int now = cur;
            if (!CallValue(getUpgradeLevel_, um, {up}, "UpgradeModel.GetUpgradeLevel", now) || now <= cur) break;
            cur = now;
            CallValue(slotUnlocked_, slot, {}, "AmmoSlot.IsUnlocked", unlocked);
        }
        if (cur > from) {
            levels += cur - from;
            list += std::format("{}{} {} -> {}", list.empty() ? "" : ", ", unity::GetName(up), from, cur);
        }
    }
    if (levels) RefreshFabricator(true);
    Done(unlocked, unlocked ? std::format("Slot {} unlocked ({})", idx + 1, list)
                   : levels ? std::format("Slot {}: applied {}, but it is still locked", idx + 1, list)
                            : std::format("Slot {} not unlocked: its upgrades ({}) are at their last level already",
                                          idx + 1, names));
}

// ------------------------------------------------- unlock all upgrades ----
// Optional: everything here only turns its own tool off when a symbol is missing.
void Sandbox::SetupUnlocks(sr2::il2cpp::Resolver& R) {
    auto cls = [&](const char* ns, const char* name, std::string& why) -> Il2CppClass* {
        std::string err;
        Il2CppClass* k = R.Class(kImage, ns, name, &err);
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
        if (!k) return nullptr;
        std::string err;
        if (const MethodInfo* m = R.Method(k, name, argc, &err)) return m;
        if (why.empty()) why = std::format("method {}({}) not found", name, argc);
        return nullptr;
    };

    // Max all upgrades: the slot-unlock symbols (UpgradeModel.GetUpgradeLevel / IncrementUpgradeLevel,
    // upgradeDefinitions, UpgradeDefinition._levels) are all it needs.
    SR2_INFO("[{}] max all upgrades: {}", kModule, incUpgrade_ ? "available" : "NOT available (upgrade symbols missing)");
    {
        // Discover all upgrades: PlayerUpgradeFabricatableItem.get_ShouldShowPlaceholder (0x82B940, decompiled)
        // hides an upgrade that is not bought (level < 0) while its _requireComponentToBeVisibleToPlayer is set and
        // the player lacks one of level 0's ComponentCosts (SpendableDirector.HasUpgradeComponent(c, 1)).
        std::string dw;
        off(upgradeDefClass_, "_requireComponentToBeVisibleToPlayer", offUdRequireComp_, dw);
        Il2CppClass* scu = cls("MonomiPark.SlimeRancher.Util", "SceneContextUtility", dw);
        tryGetSpendable_ = method(scu, "TryGetSpendableDirector", 1, dw);
        if (!dw.empty()) {
            offUdRequireComp_ = 0;
            tryGetSpendable_ = nullptr;
        }
        SR2_INFO("[{}] discover all upgrades: {}", kModule, dw.empty() && incUpgrade_ ? "available" : "NOT available (" + dw + ")");
    }

    // Fabricator key: the open FabricatorUI's selected recipe and the refinery / currency / component counts.
    std::string fw;
    constexpr const char* kFabNs = "MonomiPark.SlimeRancher.UI.Fabricator";
    fabUiClass_ = cls(kFabNs, "FabricatorUI", fw);
    Il2CppClass* content = cls(kFabNs, "FabricatorContent", fw);
    Il2CppClass* grid = cls(kFabNs, "FabricatorItemGrid", fw);
    Il2CppClass* details = cls(kFabNs, "FabricatorItemDetails", fw);
    Il2CppClass* recipe = cls(kFabNs, "FabricatorRecipe", fw);
    fabGadgetItemClass_ = cls(kFabNs, "GadgetFabricatableItem", fw);
    fabUpgradeItemClass_ = cls(kFabNs, "PlayerUpgradeFabricatableItem", fw);
    Il2CppClass* spend = cls("MonomiPark.SlimeRancher.Player", "SpendableDirector", fw);
    Il2CppClass* acq = cls("MonomiPark.SlimeRancher.Economy", "AcquisitionUtility", fw);
    Il2CppClass* cost = cls("MonomiPark.SlimeRancher.Economy", "PurchaseCost", fw);
    off(fabUiClass_, "_content", offFabUiContent_, fw);
    off(fabUiClass_, "_spendableDirector", offFabUiSpend_, fw);
    off(content, "_itemGrid", offFabContentGrid_, fw);
    off(content, "_itemDetails", offFabContentDetails_, fw);
    off(grid, "_hoveredIndex", offFabGridHovered_, fw);
    off(grid, "_currentPageOfItems", offFabGridItems_, fw);
    off(details, "_recipe", offFabDetailsRecipe_, fw);
    off(recipe, "_itemCount", offFabRecipeCount_, fw);
    off(fabGadgetItemClass_, "<Definition>k__BackingField", offFabGadgetDef_, fw);
    off(fabUpgradeItemClass_, "<UpgradeDefinition>k__BackingField", offFabUpgradeDef_, fw);
    size_t compOff = 0;
    off(cost, "ComponentCosts", compOff, fw);
    if (compOff >= 2 * sizeof(void*)) offCostComponents_ = compOff - 2 * sizeof(void*);
    fabDetailsSetItem_ = method(details, "SetItem", 1, fw);
    fabRecipeSetItem_ = method(recipe, "SetItem", 2, fw);
    spendIdentAmount_ = method(spend, "GetIdentCostAmount", 1, fw);
    spendComponentAmount_ = method(spend, "GetUpgradeComponentAmount", 1, fw);
    acquireComponent_ = method(acq, "AcquireUpgradeComponent", 2, fw);
    // The selected recipe as the game reads it (_gridNavigation._selectedIndex into _currentPageOfItems; the old
    // _hoveredIndex is the pointer's hover), and the panel refresh the game runs on a selection change
    // (FabricatorContent.OnItemSelected: details SetItem, variants button, ItemSelected -> the key legend).
    fabGridSelected_ = method(grid, "TryGetSelectedItem", 1, fw);
    fabContentSelected_ = method(content, "OnItemSelected", 2, fw);
    {
        // The quantity popup that Fabricate opens for a gadget (optional): G then takes its item and count.
        std::string pw;
        Il2CppClass* popup = cls(kFabNs, "FabricatorConfirmPopup", pw);
        off(fabUiClass_, "_confirmPopup", offFabUiPopup_, pw);
        off(popup, "_item", offPopupItem_, pw);
        off(popup, "_countToMake", offPopupCount_, pw);
        off(fabUiClass_, "_confirming", offFabUiConfirming_, pw);
        off(popup, "_isClosing", offPopupClosing_, pw);
        popupUpdate_ = method(popup, "UpdateCountAndRecipeDisplay", 1, pw);
        constexpr const char* kCheck = "MonomiPark.SlimeRancher.UI.Fabricator.IFabricatableItem.CheckCanFabricate";
        checkGadget_ = method(fabGadgetItemClass_, kCheck, 1, pw);
        checkUpgrade_ = method(fabUpgradeItemClass_, kCheck, 1, pw);
        if (!pw.empty()) {
            offFabUiPopup_ = 0;
            SR2_WARN("[{}] Fabricator key: the quantity popup is not supported ({})", kModule, pw);
        }
    }
    // What it adds with: the crafter's refinery symbols and PurchaseCost layout, and the upgrade model.
    if (fw.empty() && (!gadgetAddItem_ || !refinerySpace_ || !offCraftCosts_ || !offCostIdent_ || !identCostStride_))
        fw = "the crafter's refinery / recipe-cost symbols are missing";
    std::string hooks;
    if (fw.empty()) {
        // The two always-on hooks that tell when a Fabricator exists (see FabStartDetour).
        auto& hm = sr2::core::HookManager::Get();
        struct H {
            const char* name;
            void* detour;
            void** original;
        };
        const H hs[] = {{"Start", reinterpret_cast<void*>(&FabStartDetour), reinterpret_cast<void**>(&g_origFabStart)},
                        {"OnDestroy", reinterpret_cast<void*>(&FabDestroyDetour), reinterpret_cast<void**>(&g_origFabDestroy)}};
        for (const H& h : hs) {
            std::string err;
            const MethodInfo* m = R.Method(fabUiClass_, h.name, 0, &err);
            void* target = m ? R.MethodPointer(m, &err) : nullptr;
            const std::string full = std::format("{}.FabricatorUI.{}", kFabHooks, h.name);
            if (!target || !hm.Create(kFabHooks, full, target, h.detour, h.original, &err) || !hm.Enable(full, &err)) {
                fw = std::format("hook FabricatorUI.{}: {}", h.name, err.empty() ? "method not found" : err);
                break;
            }
            hooks += std::format("{}FabricatorUI.{} +0x{:X}", hooks.empty() ? "" : ", ", h.name, Resolver::Rva(target));
        }
    }
    if (!fw.empty()) {
        if (!hooks.empty()) sr2::core::HookManager::Get().DisableModule(kFabHooks);  // Start without OnDestroy
        fabUiClass_ = nullptr;
        SR2_WARN("[{}] Fabricator key ({}): NOT available ({})", kModule, FabricatorKeyName(), fw);
    } else {
        SR2_INFO("[{}] Fabricator key ({}): available (adds the selected recipe's missing materials to the refinery; "
                 "hooks {})",
                 kModule, FabricatorKeyName(), hooks);
        // Its entry in the Fabricator's own key bar (optional): a clone of one of the bar's hints.
        std::string lw;
        if (ugui::Resolve(lw) && legend_hint::Resolve(lw)) {
            off(fabUiClass_, "_inputLegend", offFabUiLegend_, lw);
            off(fabUiClass_, "_pinRecipeEvent", offFabUiPinEvent_, lw);
        }
        fabHintOk_ = lw.empty();
        if (fabHintOk_)
            SR2_INFO("[{}] Fabricator key bar entry: available ([{}] Add materials, before the bar's Pin entry)", kModule,
                     FabricatorKeyName());
        else
            SR2_WARN("[{}] Fabricator key bar entry: NOT available ({})", kModule, lw);
    }
}

// The player's upgrades (UpgradeModel.upgradeDefinitions, the list the Fabricator's upgrade page and the
// save use; custom_upgrades adds its own tiers to it while they are registered).
bool Sandbox::PlayerUpgrades(const Ctx& c, Il2CppObject*& um, std::vector<Il2CppObject*>& out) {
    out.clear();
    um = nullptr;
    if (!incUpgrade_ || !getUpgradeModel_) return false;
    bool ok = false;
    um = Call(getUpgradeModel_, c.model, {}, "PlayerModel.GetUpgradeModel", &ok);
    if (!ok || !um || !unity::IsAlive(um)) return false;
    Il2CppObject* defList = unity::Field<Il2CppObject*>(um, offUmDefs_);
    if (!defList || !unity::IsAlive(defList)) return false;
    auto itemsOff = Resolver::Get().FieldOffset(api().il2cpp_object_get_class(defList), "items");
    void* defs = itemsOff ? unity::Field<void*>(defList, *itemsOff) : nullptr;
    for (size_t i = 0, n = unity::ListCount(defs); i < n; ++i) {
        auto* up = static_cast<Il2CppObject*>(unity::ListAt(defs, i));
        if (IsA(up, upgradeDefClass_) && unity::IsAlive(up)) out.push_back(up);
    }
    return true;
}

// For the Player card's buttons and tooltips: how many levels "Max all upgrades" would add and how many
// upgrades "Discover all upgrades" would make visible. The model's level n applies _levels[n]; -1 = not
// bought; the last level is LevelCount - 1.
void Sandbox::UpgradeState(const Ctx& c) {
    Il2CppObject* um = nullptr;
    std::vector<Il2CppObject*> ups;
    if (!PlayerUpgrades(c, um, ups)) {
        upgCount_ = 0;
        upgLeft_ = upgHidden_ = -1;
        return;
    }
    Il2CppObject* spend = SpendableDirector();
    int left = 0, hidden = 0;
    for (Il2CppObject* up : ups) {
        const int last = static_cast<int>(unity::ListCount(unity::Field<void*>(up, offUdLevels_))) - 1;
        int cur = -1;
        if (last < 0 || !CallValue(getUpgradeLevel_, um, {up}, "UpgradeModel.GetUpgradeLevel", cur)) continue;
        left += std::max(0, last - std::max(cur, -1));
        if (UpgradeHidden(um, up, spend, nullptr)) ++hidden;
    }
    upgCount_ = static_cast<int>(ups.size());
    upgLeft_ = left;
    upgHidden_ = spend && offUdRequireComp_ && acquireComponent_ && spendComponentAmount_ ? hidden : -1;
}

// Every player upgrade to its last level, the way buying them does after the cost is paid
// (PlayerUpgradeFabricatableItem.FabricateAndSpendCost: UpgradeModel.IncrementUpgradeLevel), one level
// at a time and never past the definition's last level. Mod tiers in the list (custom_upgrades) come
// after their vanilla parent and go through the same call, which custom_upgrades records for its
// sidecar (its UpgradeModel.SetUpgradeLevel detour). IncrementUpgradeLevel (0xAD9510, decompiled) returns
// false at the last level (UpgradeLevelExist), else SetUpgradeLevel(level + 1), which raises OnUpgradeChanged
// (ActorUpgradeHandler applies the level's modifiers) and the TinkerTailorSlimes progress (blocked while
// Sandbox is on).
void Sandbox::DoUnlockUpgrades(const Ctx& c) {
    if (!incUpgrade_) {
        Done(false, "Upgrades not raised: the upgrade symbols were not found");
        return;
    }
    Il2CppObject* um = nullptr;
    std::vector<Il2CppObject*> ups;
    if (!PlayerUpgrades(c, um, ups)) {
        Done(false, "Upgrades not raised: the player's upgrade list was not found");
        return;
    }
    int levels = 0, touched = 0, stuck = 0;
    std::string list;
    for (Il2CppObject* up : ups) {
        const int last = static_cast<int>(unity::ListCount(unity::Field<void*>(up, offUdLevels_))) - 1;
        int from = -1, cur = -1;
        if (last < 0 || !CallValue(getUpgradeLevel_, um, {up}, "UpgradeModel.GetUpgradeLevel", from)) continue;
        cur = from;
        for (int guard = 0; guard < 32 && cur < last; ++guard) {
            bool inc = false;
            if (!CallValue(incUpgrade_, um, {up}, "UpgradeModel.IncrementUpgradeLevel", inc) || !inc) break;
            int now = cur;
            if (!CallValue(getUpgradeLevel_, um, {up}, "UpgradeModel.GetUpgradeLevel", now) || now <= cur) break;
            cur = now;
        }
        if (cur > from) {
            levels += cur - from;
            ++touched;
            list += std::format("{}{} {} -> {}", list.empty() ? "" : ", ", unity::GetName(up), from, cur);
        }
        if (cur < last) ++stuck;
    }
    lastUpgState_ = 0;  // the button's count at once
    if (!touched) {
        Done(stuck == 0, stuck ? std::format("Upgrades not raised: the game refused {} upgrade(s)", stuck)
                               : std::string("Every upgrade is at its last level already"));
        return;
    }
    RefreshFabricator(true);
    SR2_INFO("[{}] maxed upgrades: {}", kModule, list);
    Done(true, std::format("Maxed all upgrades: {} level(s) of {} upgrade(s){}", levels, touched,
                           stuck ? std::format(" ({} could not be raised all the way)", stuck) : std::string()));
}

// SceneContext's SpendableDirector (SceneContextUtility.TryGetSpendableDirector), what the Fabricator counts with.
Il2CppObject* Sandbox::SpendableDirector() {
    if (!tryGetSpendable_) return nullptr;
    Il2CppObject* spend = nullptr;
    bool found = false;
    if (!CallValue(tryGetSpendable_, nullptr, {&spend}, "SceneContextUtility.TryGetSpendableDirector", found) || !found ||
        !spend)
        return nullptr;
    return spend;
}

// An upgrade the Fabricator shows as a placeholder until it is found, as PlayerUpgradeFabricatableItem.
// get_ShouldShowPlaceholder (0x82B940, decompiled) decides: not bought (level < 0), its
// _requireComponentToBeVisibleToPlayer set, and the player lacks one of level 0's ComponentCosts
// (SpendableDirector.HasUpgradeComponent(c, 1); here GetUpgradeComponentAmount < 1). `missing` gets those
// components, once each.
bool Sandbox::UpgradeHidden(Il2CppObject* um, Il2CppObject* up, Il2CppObject* spend, std::vector<Il2CppObject*>* missing) {
    if (missing) missing->clear();
    if (!um || !up || !spend || !offUdRequireComp_ || !offCostComponents_ || !spendComponentAmount_) return false;
    if (!unity::Field<bool>(up, offUdRequireComp_)) return false;
    int cur = 0;
    if (!CallValue(getUpgradeLevel_, um, {up}, "UpgradeModel.GetUpgradeLevel", cur) || cur >= 0) return false;
    void* levels = unity::Field<void*>(up, offUdLevels_);
    auto* first = unity::ListCount(levels) ? static_cast<Il2CppObject*>(unity::ListAt(levels, 0)) : nullptr;
    if (!first) return false;
    if (!offUlCosts_) {
        auto o = Resolver::Get().FieldOffset(api().il2cpp_object_get_class(first), "_costs");
        if (!o) return false;
        offUlCosts_ = *o;
    }
    void* comps = *reinterpret_cast<void* const*>(reinterpret_cast<const char*>(first) + offUlCosts_ + offCostComponents_);
    bool hidden = false;
    for (size_t i = 0, n = unity::ListCount(comps); i < n; ++i) {
        auto* comp = static_cast<Il2CppObject*>(unity::ListAt(comps, i));
        if (!comp || !unity::IsAlive(comp)) continue;
        int have = 0;
        if (!CallValue(spendComponentAmount_, spend, {comp}, "SpendableDirector.GetUpgradeComponentAmount", have) || have >= 1)
            continue;
        hidden = true;
        if (missing && std::find(missing->begin(), missing->end(), comp) == missing->end()) missing->push_back(comp);
    }
    return hidden;
}

// "Discover all upgrades": what finding an upgrade in the world does, without buying a level. Every upgrade
// the Fabricator hides (UpgradeHidden) gets one of each level-0 upgrade component it lacks through
// AcquisitionUtility.AcquireUpgradeComponent(component, 1), the treasure pod / pickup path; the upgrade then
// shows in the Fabricator, buyable for its normal cost (level 0 spends that component, as after finding it).
// Upgrades without that flag, or already bought, are never hidden and are left alone. Gadget blueprints are
// a different mechanism (GadgetDirector's blueprint list, GadgetFabricatableItem.get_IsMissingBlueprint) and
// are not touched.
void Sandbox::DoDiscoverUpgrades(const Ctx& c) {
    Il2CppObject* um = nullptr;
    std::vector<Il2CppObject*> ups;
    Il2CppObject* spend = SpendableDirector();
    if (!acquireComponent_ || !offUdRequireComp_ || !spend || !PlayerUpgrades(c, um, ups)) {
        Done(false, "Upgrades not discovered: the game's upgrade / component functions were not found");
        return;
    }
    int found = 0, failed = 0;
    std::string list;
    std::vector<Il2CppObject*> missing;
    for (Il2CppObject* up : ups) {
        if (!UpgradeHidden(um, up, spend, &missing)) continue;
        bool ok = true;
        for (Il2CppObject* comp : missing) {
            int one = 1;
            bool got = false;
            if (!CallValue(acquireComponent_, nullptr, {comp, &one}, "AcquisitionUtility.AcquireUpgradeComponent", got) || !got)
                ok = false;
        }
        if (ok && !UpgradeHidden(um, up, spend, nullptr)) {
            ++found;
            list += (list.empty() ? "" : ", ") + unity::GetName(up);
        } else {
            ++failed;
        }
    }
    lastUpgState_ = 0;
    if (!found) {
        Done(failed == 0, failed ? std::format("Upgrades not discovered: the game refused {} upgrade(s)", failed)
                                 : std::string("Every upgrade is discovered already"));
        return;
    }
    RefreshFabricator(true);
    SR2_INFO("[{}] discovered upgrades (one upgrade component each, as when found): {}", kModule, list);
    Done(failed == 0, std::format("Discovered {} upgrade(s){}", found,
                                  failed ? std::format(" ({} could not be discovered)", failed) : std::string()));
}

// ------------------------------------------ Fabricator "add missing materials" ----
// The open FabricatorUI: an instance the Start hook saw (still alive) whose GameObject is active.
Il2CppObject* Sandbox::OpenFabricator() {
    if (!fabUiClass_) return nullptr;
    for (const FabUi& f : g_fabUis) {
        Il2CppObject* ui = f.handle ? unity::GCHandleTarget(f.handle) : f.ui;
        if (ui != f.ui || !IsA(ui, fabUiClass_) || !unity::IsAlive(ui)) continue;
        if (unity::ActiveInHierarchy(unity::GameObjectOf(ui))) return ui;
    }
    return nullptr;
}

// The recipe the Fabricator has selected (the item its details panel shows): FabricatorItemGrid.
// TryGetSelectedItem (0x80AAD0, decompiled: _gridNavigation._selectedIndex into _currentPageOfItems), else the
// hovered index. Also hands back the content and details panel.
Il2CppObject* Sandbox::FabricatorItem(Il2CppObject* ui, Il2CppObject** contentOut, Il2CppObject** detailsOut) {
    if (contentOut) *contentOut = nullptr;
    if (detailsOut) *detailsOut = nullptr;
    if (!ui) return nullptr;
    Il2CppObject* content = unity::Field<Il2CppObject*>(ui, offFabUiContent_);
    if (!content || !unity::IsAlive(content)) return nullptr;
    if (contentOut) *contentOut = content;
    Il2CppObject* grid = unity::Field<Il2CppObject*>(content, offFabContentGrid_);
    Il2CppObject* details = unity::Field<Il2CppObject*>(content, offFabContentDetails_);
    if (detailsOut && details && unity::IsAlive(details)) *detailsOut = details;
    if (!grid || !unity::IsAlive(grid)) return nullptr;
    Il2CppObject* item = nullptr;
    bool got = false;
    if (fabGridSelected_ && CallValue(fabGridSelected_, grid, {&item}, "FabricatorItemGrid.TryGetSelectedItem", got) && got &&
        item)
        return item;
    const int idx = unity::Field<int32_t>(grid, offFabGridHovered_);
    void* items = unity::Field<void*>(grid, offFabGridItems_);
    return idx >= 0 && static_cast<size_t>(idx) < unity::ListCount(items)
               ? static_cast<Il2CppObject*>(unity::ListAt(items, static_cast<size_t>(idx)))
               : nullptr;
}

// The quantity popup (FabricatorUI._confirmPopup) while it is shown (FabricatorUI._confirming, the popup not
// closing) with an item: Fabricate on a gadget opens it
// (FabricatorConfirmPopup.Show; _countToMake is the chosen quantity, IncrementCount 0x800DB0 / DecrementCount
// change it and call UpdateCountAndRecipeDisplay(item.CheckCanFabricate(count))).
Il2CppObject* Sandbox::FabricatorPopup(Il2CppObject* ui) {
    if (!ui || !offFabUiPopup_ || !unity::Field<bool>(ui, offFabUiConfirming_)) return nullptr;  // not confirming
    Il2CppObject* popup = unity::Field<Il2CppObject*>(ui, offFabUiPopup_);
    if (!popup || !unity::IsAlive(popup) || unity::Field<bool>(popup, offPopupClosing_) ||
        !unity::ActiveInHierarchy(unity::GameObjectOf(popup)))
        return nullptr;
    Il2CppObject* item = unity::Field<Il2CppObject*>(popup, offPopupItem_);
    return item && (IsA(item, fabGadgetItemClass_) || IsA(item, fabUpgradeItemClass_)) ? popup : nullptr;
}

// After anything that changes what the open Fabricator's selected recipe needs or has (G, refinery items,
// crafts, Newbucks, upgrade levels, components): the refresh the game runs when the selection changes,
// FabricatorContent.OnItemSelected(item, PROGRAMMATIC) (0x804700, decompiled: details SetItem -> recipe counts
// and the Fabricate / Upgrade button, variants button, ItemSelected -> FabricatorUI.OnItemSelected ->
// UpdateInputLegendBasedOnItemUpgrade, which enables the legend's Fabricate / Upgrade entry). PROGRAMMATIC
// plays no selection sound. Without that method: FabricatorItemDetails.SetItem. `rebuild` (levels, discoveries,
// blueprints): first the whole Fabricator as when it opens (upgrades.cpp RebuildFabricator: grid buttons with
// their icons and tiers, hidden placeholders, the gadgets of the owned blueprints); before, it had to be reopened.
void Sandbox::RefreshFabricator(bool rebuild) {
    Il2CppObject* content = nullptr;
    Il2CppObject* details = nullptr;
    Il2CppObject* ui = OpenFabricator();
    if (rebuild && ui) sandbox::RebuildFabricator(ui);
    Il2CppObject* item = FabricatorItem(ui, &content, &details);
    if (!item) return;
    bool ok = false;
    int32_t cause = 0;  // InteractionCause.PROGRAMMATIC
    if (fabContentSelected_ && content) Call(fabContentSelected_, content, {item, &cause}, "FabricatorContent.OnItemSelected", &ok);
    if (!ok && details) Call(fabDetailsSetItem_, details, {item}, "FabricatorItemDetails.SetItem", &ok);
}

// While Sandbox is on: whether the game's Fabricator is open, 4 times a second (the overlay's hint; logged
// when it changes), and the key's edge (GetAsyncKeyState, game window in front, overlay closed). The key is
// read from the OS, so the Fabricator's own input handling does not matter.
void Sandbox::FabricatorTick(unsigned long long now) {
    if (!fabUiClass_) return;
    if (now - lastFabPoll_ >= 250) {
        lastFabPoll_ = now;
        const bool open = OpenFabricator() != nullptr;
        if (open != fabOpen_)
            SR2_INFO("[{}] Fabricator {}", kModule,
                     open ? std::format("open: {} adds the selected recipe's missing materials", FabricatorKeyName())
                          : std::string("closed"));
        fabOpen_ = open;
    }
    FabricatorHintTick(now);
    bool down = false;
    if (fabOpen_ && !sr2::ui::overlay::IsOpen()) {
        DWORD pid = 0;
        GetWindowThreadProcessId(GetForegroundWindow(), &pid);
        down = pid == GetCurrentProcessId() && (GetAsyncKeyState(kFabricatorKey) & 0x8000) != 0;
    }
    if (down && !fabKeyDown_) DoFabricatorMaterials();
    fabKeyDown_ = down;
}

// "[G] Add materials" in the Fabricator's own key bar (bottom left), right before its "[Z] Pin" entry: a clone of
// one of the bar's hints (modules/common/legend_hint.h, shared with map_tools' "[RMB] Teleport"). The bar is
// FabricatorUI._inputLegend (an InputLegend); the Pin entry is the active hint whose configuration's InputEvent
// is FabricatorUI._pinRecipeEvent. The bar is rebuilt per page (sidebar / gadgets / upgrades / pinned), so the
// clone's place is checked 4 times a second; without a Pin entry it goes last. It lives inside the Fabricator
// UI, so it hides and dies with it; Sandbox off destroys it. Runs under a native-fault guard: a fault turns the
// entry off for the session (and marks the module faulted).
void Sandbox::FabricatorHintTick(unsigned long long now) {
    if (!fabHintOk_) return;
    std::string err, fault;
    bool native = false;
    const bool ok = sr2::core::Try([&] {
        if (!ugui::GuardNative(
                [&] {
                    if (fabHint_.Alive()) fabHint_.Tick();
                    else if (fabHint_.Exists()) fabHint_.Destroy();  // died with the Fabricator UI
                    if (!fabOpen_) {
                        fabHintSince_ = 0;
                        return;
                    }
                    if (now < fabHintNext_) return;
                    fabHintNext_ = now + 250;
                    if (!fabHintSince_) fabHintSince_ = now;
                    Il2CppObject* ui = OpenFabricator();
                    if (!ui) return;
                    std::string chain;
                    Il2CppObject* legend = legend_hint::FollowLegend(unity::Field<Il2CppObject*>(ui, offFabUiLegend_), ui,
                                                                     "FabricatorUI._inputLegend", chain);
                    Il2CppObject* pin = legend_hint::HintFor(legend, unity::Field<Il2CppObject*>(ui, offFabUiPinEvent_));
                    Il2CppObject* tmpl = legend_hint::Template(legend);
                    if (fabHint_.Alive() && tmpl &&
                        ugui::GetParent(unity::TransformOf(fabHint_.GameObject())) !=
                            ugui::GetParent(unity::TransformOf(tmpl)))
                        fabHint_.Destroy();  // another Fabricator's bar
                    if (!fabHint_.Alive()) {
                        std::string why = !legend ? "no InputLegend (" + chain + ")"
                                          : !tmpl ? "none of the bar's hints is shown yet (" + chain + ")"
                                                  : "";
                        if (tmpl && !fabHint_.Create(tmpl, "SR2Mods.FabricatorKeyHint", why)) tmpl = nullptr;
                        if (!tmpl) {
                            if (!fabHintFailLogged_ && now - fabHintSince_ >= 3000) {
                                fabHintFailLogged_ = true;
                                SR2_INFO("[{}] no [{}] Add materials entry in the Fabricator's key bar 3 s after it opened: {}",
                                         kModule, FabricatorKeyName(), why);
                            }
                            return;
                        }
                        fabHint_.SetLook(FabricatorKeyName(), "Add materials");
                        if (!fabHintLogged_) {
                            fabHintLogged_ = true;
                            SR2_INFO("[{}] [{}] Add materials entry cloned from the Fabricator key bar's '{}' ({} key icon(s), "
                                     "label {}; Pin entry {}; {})",
                                     kModule, FabricatorKeyName(), unity::GetName(unity::GameObjectOf(tmpl)),
                                     fabHint_.IconCount(), fabHint_.HasLabel() ? "ok" : "MISSING",
                                     pin ? "found, placed before it" : "not shown, placed last", chain);
                        }
                    }
                    fabHint_.PlaceBefore(pin);
                },
                &fault))
            native = true;
    }, &err);
    if (native) {
        fabHintOk_ = false;
        fabHint_.Abandon();
        sr2::core::MarkModuleFaulted(kModule, "Fabricator key bar entry: " + fault);
    } else if (!ok) {
        fabHintOk_ = false;
        SR2_WARN("[{}] Fabricator key bar entry failed ({}); off for this session", kModule, err);
    }
}

// Sandbox switched off (or faulted): the key bar entry goes.
void Sandbox::DropFabricatorHint() {
    if (sr2::core::ModuleFaulted(kModule) || ugui::RuntimeFrozen()) {
        fabHint_.Abandon();
        return;
    }
    std::string fault;
    if (!sr2::core::Try([&] {
            if (!ugui::GuardNative([&] { fabHint_.Destroy(); }, &fault)) fabHint_.Abandon();
        }))
        fabHint_.Abandon();
    fabHintSince_ = 0;
}

// The selected recipe of the open Fabricator (FabricatorItem): what the player lacks of it (times the panel's
// craft count for gadgets) is added for free: refinery items through GadgetDirector.AddItem (what the refinery
// intake does, clamped to its space), upgrade components through AcquisitionUtility.AcquireUpgradeComponent
// (the treasure pod / shop path), Newbucks through PlayerState.AddCurrency. "Have" is what the Fabricator itself
// counts (SpendableDirector.GetIdentCostAmount / GetUpgradeComponentAmount). The panel is then refreshed
// (RefreshFabricator). Nothing is crafted or bought: the player still presses Fabricate.
void Sandbox::DoFabricatorMaterials() {
    // Its result is logged only: the page's status line showed it long after (the key is pressed in the game).
    struct Quiet {
        bool& q;
        ~Quiet() { q = false; }
    } quiet{quiet_ = true};
    Ctx c;
    std::string why;
    if (!Context(c, why)) {
        Done(false, "Fabricator: nothing added: " + why);
        return;
    }
    Il2CppObject* ui = OpenFabricator();
    if (!ui) {
        Done(false, "Fabricator: nothing added: the Fabricator is not open");
        return;
    }
    Il2CppObject* content = nullptr;
    Il2CppObject* details = nullptr;
    Il2CppObject* item = FabricatorItem(ui, &content, &details);
    Il2CppObject* recipe = details && unity::IsAlive(details) ? unity::Field<Il2CppObject*>(details, offFabDetailsRecipe_) : nullptr;
    int count = recipe && unity::IsAlive(recipe) ? std::clamp(unity::Field<int32_t>(recipe, offFabRecipeCount_), 1, 999) : 1;
    // On the quantity popup (round 8: "[G] should also work when choosing how much to fabricate"): its item and
    // the quantity chosen there.
    Il2CppObject* popup = FabricatorPopup(ui);
    if (popup) {
        item = unity::Field<Il2CppObject*>(popup, offPopupItem_);
        count = std::clamp(unity::Field<int32_t>(popup, offPopupCount_), 1, 999);
    }
    if (!item) {
        Done(false, "Fabricator: select a recipe first");
        return;
    }
    // The cost: a gadget's CraftingCosts, or the next level's _costs of an upgrade.
    const char* costs = nullptr;
    std::string name;
    if (IsA(item, fabGadgetItemClass_)) {
        Il2CppObject* def = unity::Field<Il2CppObject*>(item, offFabGadgetDef_);
        if (!IsA(def, gadgetDefClass_) || !unity::IsAlive(def)) {
            Done(false, "Fabricator: nothing added: the gadget of this recipe was not found");
            return;
        }
        costs = reinterpret_cast<const char*>(def) + offCraftCosts_;
        name = ItemName(def);
    } else if (IsA(item, fabUpgradeItemClass_)) {
        count = 1;
        Il2CppObject* up = unity::Field<Il2CppObject*>(item, offFabUpgradeDef_);
        Il2CppObject* um = nullptr;
        std::vector<Il2CppObject*> ups;
        if (!IsA(up, upgradeDefClass_) || !unity::IsAlive(up) || !PlayerUpgrades(c, um, ups)) {
            Done(false, "Fabricator: nothing added: the upgrade was not found");
            return;
        }
        name = unity::GetName(up);
        int cur = -1;
        CallValue(getUpgradeLevel_, um, {up}, "UpgradeModel.GetUpgradeLevel", cur);
        void* levels = unity::Field<void*>(up, offUdLevels_);
        const size_t next = static_cast<size_t>(std::max(cur, -1) + 1);
        auto* lvl = next < unity::ListCount(levels) ? static_cast<Il2CppObject*>(unity::ListAt(levels, next)) : nullptr;
        if (!lvl) {
            Done(true, std::format("Fabricator: {} is fully upgraded", name));
            return;
        }
        if (!offUlCosts_) {
            auto o = Resolver::Get().FieldOffset(api().il2cpp_object_get_class(lvl), "_costs");
            if (!o) {
                Done(false, "Fabricator: nothing added: UpgradeLevelDefinition._costs not found");
                return;
            }
            offUlCosts_ = *o;
        }
        costs = reinterpret_cast<const char*>(lvl) + offUlCosts_;
    } else {
        Done(false, "Fabricator: the selected item is not a gadget or upgrade recipe");
        return;
    }
    Il2CppObject* spend = unity::Field<Il2CppObject*>(ui, offFabUiSpend_);
    if (spend && !unity::IsAlive(spend)) spend = nullptr;
    Il2CppObject* dir = GadgetDirector();
    std::vector<std::string> added, missed;

    // Refinery items.
    void* ident = *reinterpret_cast<void* const*>(costs + offCostIdent_);
    if (const size_t n = unity::ListCount(ident)) {
        size_t offItems = 0;
        if (auto o = Resolver::Get().FieldOffset(api().il2cpp_object_get_class(static_cast<Il2CppObject*>(ident)), "_items"))
            offItems = *o;
        void* arr = offItems ? unity::Field<void*>(ident, offItems) : nullptr;
        char* data = arr ? unity::ArrayData<char>(arr) : nullptr;
        for (size_t i = 0; data && i < n && i < unity::ArrayLength(arr); ++i) {
            char* e = data + i * identCostStride_;
            const int amount = *reinterpret_cast<int32_t*>(e + offIdentCostAmount_);
            auto* type = *reinterpret_cast<Il2CppObject**>(e + offIdentCostType_);
            if (!type || amount <= 0 || !IsA(type, identClass_) || !unity::IsAlive(type)) continue;
            int have = 0;
            if (!(spend && CallValue(spendIdentAmount_, spend, {type}, "SpendableDirector.GetIdentCostAmount", have)) && dir)
                CallValue(gadgetCount_, dir, {type}, "GadgetDirector.GetItemCount", have);
            const int need = amount * count - have;
            if (need <= 0) continue;
            int space = 0;
            if (!dir || !CallValue(refinerySpace_, dir, {type}, "GadgetDirector.GetRefinerySpaceAvailable", space) || space <= 0) {
                missed.push_back(std::format("{} {} (refinery full or not stored there)", need, ItemName(type)));
                continue;
            }
            int add = std::min(need, space), before = 0, after = 0;
            CallValue(gadgetCount_, dir, {type}, "GadgetDirector.GetItemCount", before);
            bool ok = false;
            Call(gadgetAddItem_, dir, {type, &add}, "GadgetDirector.AddItem", &ok);
            CallValue(gadgetCount_, dir, {type}, "GadgetDirector.GetItemCount", after);
            if (ok && after > before) added.push_back(std::format("{} {}", after - before, ItemName(type)));
            if (!ok || after - before < need) missed.push_back(std::format("{} {}", need - std::max(0, after - before), ItemName(type)));
        }
    }
    // Upgrade components (one per list entry).
    if (offCostComponents_) {
        void* comps = *reinterpret_cast<void* const*>(costs + offCostComponents_);
        std::map<Il2CppObject*, int> needs;
        for (size_t i = 0, n = unity::ListCount(comps); i < n; ++i)
            if (auto* comp = static_cast<Il2CppObject*>(unity::ListAt(comps, i)); comp && unity::IsAlive(comp)) needs[comp] += count;
        for (auto& [comp, amount] : needs) {
            int have = 0;
            if (spend) CallValue(spendComponentAmount_, spend, {comp}, "SpendableDirector.GetUpgradeComponentAmount", have);
            int need = amount - have;
            if (need <= 0) continue;
            bool got = false;
            if (CallValue(acquireComponent_, nullptr, {comp, &need}, "AcquisitionUtility.AcquireUpgradeComponent", got) && got)
                added.push_back(std::format("{} {}", need, unity::GetName(comp)));
            else
                missed.push_back(std::format("{} {}", need, unity::GetName(comp)));
        }
    }
    // Newbucks (other currencies are left alone).
    void* cur = *reinterpret_cast<void* const*>(costs + offCostCurrency_);
    for (size_t i = 0, n = unity::ListCount(cur); i < n; ++i) {
        auto* e = static_cast<Il2CppObject*>(unity::ListAt(cur, i));
        if (!e) continue;
        Il2CppObject* currency = unity::Field<Il2CppObject*>(e, offCurCostCurrency_);
        const int64_t amount = static_cast<int64_t>(unity::Field<int32_t>(e, offCurCostAmount_)) * count;
        if (amount <= 0 || !c.currency || currency != c.currency) continue;
        int have = 0;
        if (!CallValue(getCurrency_, c.player, {c.currency}, "PlayerState.GetCurrency", have)) continue;
        const int maxBucks = unity::Field<int32_t>(c.model, offMaxCurrency_);
        int add = static_cast<int>(std::clamp<int64_t>(amount - have, 0, std::max(0, maxBucks - have)));
        if (add <= 0) continue;
        bool coinPopup = true, ok = false;
        Call(addCurrency_, c.player, {c.currency, &add, &coinPopup, nullptr}, "PlayerState.AddCurrency", &ok);
        if (ok) added.push_back(std::format("{} Newbucks", Thousands(add)));
    }
    // Show the new counts: the game's selection-change refresh (it sets the recipe to 1 craft), then the
    // recipe again with the chosen count.
    if (!added.empty()) {
        // With the popup up only the details panel behind it (no selection event: that would also switch the
        // key bar to the recipe page's while the popup has its own).
        if (!popup) RefreshFabricator();
        else if (details && unity::IsAlive(details)) Call(fabDetailsSetItem_, details, {item}, "FabricatorItemDetails.SetItem");
        bool ok = false;
        if (count > 1 && recipe && unity::IsAlive(recipe) && !popup)
            Call(fabRecipeSetItem_, recipe, {item, &count}, "FabricatorRecipe.SetItem", &ok);
        // The popup's have / need counts and its confirm button, as its own +/- does.
        if (popup && unity::IsAlive(popup)) {
            const MethodInfo* check = IsA(item, fabGadgetItemClass_) ? checkGadget_ : checkUpgrade_;
            int32_t reason = 0, n = count;
            if (CallValue(check, item, {&n}, "IFabricatableItem.CheckCanFabricate", reason))
                Call(popupUpdate_, popup, {&reason}, "FabricatorConfirmPopup.UpdateCountAndRecipeDisplay");
        }
    }
    auto join = [](const std::vector<std::string>& v) {
        std::string s;
        for (const std::string& x : v) s += (s.empty() ? "" : ", ") + x;
        return s;
    };
    const std::string what = std::format("{}{}{}", count > 1 ? std::format("{} x ", count) : "", name,
                                         popup ? " (quantity popup)" : "");
    if (added.empty() && missed.empty()) Done(true, std::format("Fabricator: you already have everything for {}", what));
    else if (missed.empty()) Done(true, std::format("Fabricator: added {} for {}", join(added), what));
    else Done(!added.empty(), std::format("Fabricator: {}{} for {}; not added: {}", added.empty() ? "nothing added" : "added ",
                                          join(added), what, join(missed)));
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
