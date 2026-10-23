// custom_upgrades: framework for NEW land-plot upgrades bought in the game's own plot
// purchase menu, with save-safe sidecar persistence (SR2 1.3.0). Upgrades: the silo
// "Sell-All Button" (sell every plort stored in that silo at the current Plort Market
// price, market saturation applied by the game; a "Sell All Plorts Now" menu action plus a
// press button on the silo, cloned from the corral collector button - silo_world.cpp) and
// the per-plot upgrade TIERS whose
// effects plot_upgrades applies (api.h Multiplier):
//
//   tier                 plot         needs                 price   effect
//   Plort Collector II   corral       Plort Collector       2500    collects 1.5x as often
//   Plort Collector III  corral       Plort Collector II    5000    collects 2x as often
//   Auto-Feeder II       corral       Auto-Feeder           2500    2x food per feeding cycle
//   Music Box II         corral       Music Box             2000    2x calming bonus
//   Faster Hens I        coop         -                     2000    hens lay 1.5x as often
//   Faster Hens II       coop         Faster Hens I         4000    hens lay 2x as often
//   Fast Growth I        garden       -                     2000    crops grow 1.5x as fast
//   Fast Growth II       garden       Fast Growth I         4000    crops grow 2x as fast
//   Ash Booster          incinerator  Ash Trough            1500    2x ash per incinerated food
//
//   Tiers are listed once the prerequisite is owned (custom `needs` or a vanilla
//   upgrade via the original LandPlot.HasUpgrade); owned ones stay listed greyed. A
//   tier reuses the icon of its vanilla upgrade (collector, feeder, music box, deluxe
//   coop, sprinkler, ash trough). Selling (sell_upgrades) is refused while a higher tier
//   - or, for a vanilla prerequisite, any tier built on it - is still owned. Only the
//   highest owned tier of a family counts (they do not stack).
//
//   Multiplier(obj, family): a per-object cache (key = the LandPlot or the component
//   the caller has; value = the multipliers of all families) invalidated by a version
//   counter (buy, sell, load, new game, sidecar read, plot type change) and expired
//   after ~900 frames (covers plot rebuilds / reused addresses). A miss resolves the
//   plot once (GetComponentInParent(LandPlot) + LandPlotLocation id), no per-call work.
//
//   world objects (coop_world.cpp)  plot  price  what
//   Coop Air Net                    coop   425   the corral's Air Net cloned onto the coop,
//                                                fitted to its walls (corral Air Net: 425)
//   Hen Collector                   coop  1000   the deluxe coop's elder collector cloned for
//                                                adult hens / roosters: collects the adults over
//                                                the coop's limit (default 20, keeps 2 roosters)
//                                                every 6 game hours into 2 x 100 slots; chicks
//                                                never count (deluxe: 1300)
//
//   Hen Collector limit slider (plot_menu_slider.cpp, reusable): while the owned Hen Collector is
//   selected in the coop's purchase menu, a slider "Collect above: N adults" (2..58, default 20)
//   sits in the details panel where the price row is for unowned items. Built from a
//   PurchaseItemDetailsContentView.BindData detour (post: our item = the view model's _description
//   is our clone's LocalizedString instance), replaced on every bind (selection change, RebuildUI
//   after a buy / sell), removed with the menu. The value is per coop in the sidecar
//   ("hen_collector_keep"), saved with the next game save like ownership, and read live by the
//   collector (CoopRequest.keepAdults).
//
//   Hen Collector save safety: its storage is a LOCAL AmmoSlotManager (never linked to the
//   plot's LandPlotModel, so never in the vanilla save; the vanilla elder slots would not
//   accept hens on a mod-free load anyway); its contents are kept in the sidecar
//   ("hen_collector": [{type, count}] per slot), copied from the live storage every tick and
//   before every save. Selling is refused while it holds chickens (vac them out first).
//
//   World objects are clones of vanilla plot-prefab parts (LookupDirector.GetPlotPrefab),
//   built / fitted / removed by a ~1 Hz tick over CoopRegion._allCoops from the sidecar
//   ownership: after a load, a plot rebuild or a re-enable they come back by themselves; a
//   sale or disabling the module removes them. They are never in the vanilla save. The icon
//   comes from the vanilla item of another plot menu when this menu has none (one-time
//   FindObjectsOfTypeAll of the menu items, the first time such an icon is needed).
//
// Design + research notes: re/custom-upgrades.md. Summary (RVAs for reference only, all
// lookups are by name):
//
// ---------------------------------------------------------------- menu --------------
//   LandPlotUIRoot (ListUIRoot`4) has `menuConfig` (MenuConfiguration<PlotPurchaseItemModel,
//   PlotPurchaseCategory>, a ScriptableObject of the plot's UI prefab): `categories` ->
//   StaticCategory.`items` (List<PlotPurchaseItemModel>). CreateUI first calls the virtual
//   UpdateMenuItemAvailabilityAndHidden (LandPlotUIRoot override 0x6E8150: for every item
//   UpdateAvailability(Activator) / UpdateHidden(Activator)), then builds the list views.
//   SetActivator / Upgrade() end with RebuildUI -> CreateUI, so that hook runs before every
//   (re)build of the menu.
//   PlotUpgradePurchaseItemModel: _upgrade (+0x88); IsAvailable = () => !plot.HasUpgrade(
//   _upgrade); UpdateHidden: only STORAGE3/4 and the incinerator collector get a
//   prerequisite, every other value (ours) gets the static "never hidden" lambda.
//   Purchase: OnAction -> PromptForUpgrade (HasUpgrade -> error "owned") -> game popup with
//   the item's _title / _promptMessage -> <PromptForUpgrade>b__0 -> Upgrade(upgrade, cost):
//   HasUpgrade -> abort; ISpendableSpender.Spend(cost) (fails = not enough money);
//   LandPlot.AddUpgrade(upgrade) (0x988750: model.upgrades.Add + ApplyUpgrades); purchase cue;
//   RebuildUI; PlayPurchaseFX. Then an analytics event with upgrade.ToString().
//
// ---------------------------------------------------------------- what we do --------
//   * UpdateMenuItemAvailabilityAndHidden detour (pre): for the activator's plot type, our
//     items are cloned ONCE per menu configuration from one of its vanilla upgrade items
//     (Object.Instantiate of the ScriptableObject: LocalizedStrings and the cost list are
//     deep-copied by Unity serialization - verified at runtime, else the clone is dropped),
//     with our _upgrade id, price, icon (Market Link icon) and texts. They are added to /
//     removed from the category list depending on the plot (type, prerequisites). (post):
//     refresh the dynamic texts. Texts: core/text_override.h (the UI's LocalizeStringEvent.
//     UpdateString hook substitutes our text for our LocalizedString instances). No sink ->
//     our items are not shown.
//   * LandPlot.HasUpgrade detour: custom ids -> sidecar ownership (vanilla ids untouched,
//     one compare). LandPlot.AddUpgrade detour: custom ids are NEVER passed to the original
//     (they would land in the vanilla save); buying marks ownership in the sidecar state,
//     an action item runs its action (sell all now).
//   * Persistence: one sidecar per .sav file, <game folder>/sr2mods_saves/<save>.sav.json
//     (<save> = "<GameName>_<i>", the rotation file the game wrote / loaded), keyed by
//     LandPlotLocation._id (+ plot type), plus api.h per-plot / per-save values. The file
//     name on save: SaveGameImpl formats "{0}_{1}" (GameName, first i whose .sav does not
//     exist yet, so indices are REUSED and are no age order) and passes it to
//     StorageProvider.StoreGameData -> detour on FileStorageProvider.StoreGameData (only
//     while inside SaveGameImpl) notes it and the provider's savePath. On load:
//     AutoSaveDirector.Load(GameSaveIdentifier {GameName, SaveName}) - SaveName is the file.
//     Each sidecar stores GameMetadata.SaveNumber (AutoSaveDirector._currentGameMetadata,
//     incremented by every save, read back from the file by Load): a sidecar whose number
//     differs from the loaded file's belongs to an older file of the same name and is
//     ignored. Read order on load: that file's sidecar -> the old per-game
//     sr2mods_saves/<GameName>.json (migration; renamed to .json.migrated after the first
//     per-save write) -> the sidecar of this game with the highest save number below the
//     loaded one -> empty (logged). Read lazily (first question after the load); written
//     after every SaveGameImpl that returned a game (manual save and autosave), so unsaved
//     purchases vanish together with the unsaved money, exactly like vanilla; sidecars of
//     this game whose .sav is gone (CleanupAutosaves keeps 5) are deleted then. Without the
//     StoreGameData hook: one per-game sidecar as before. Kept in the game folder, not in
//     the Steam Cloud save folder (see re/custom-upgrades.md 1.2). AutoSaveDirector.Load /
//     StartNewGame detours drop the in-memory state. Re-enabling the module (its Load hook was
//     off meanwhile, so another save may be loaded now) drops it too and re-reads it for the
//     loaded save, identified by _currentGameMetadata (GameName + SaveNumber -> the per-save
//     sidecar with that number, then the same order as on load; no metadata -> empty state,
//     logged). The vanilla save never contains
//     anything of ours: without the mod it loads unchanged (only the custom upgrades are
//     gone).
//   * sell_upgrades integration (modules/custom_upgrades/api.h): owned custom upgrades
//     can be sold back for half their price through sell_upgrades' popup.
//
// ---------------------------------------------------------------- sell all ----------
//   PlortEconomyDirector (SceneContext.PlortEconomyDirector).TrySell(id, count,
//   ignoreMarketShutdown, showUiNotification, source) (0x9E52A0): false if the market is
//   shut down or `id` has no market value; else PlayerState.AddCurrency(null = Newbucks,
//   value * count, showUi) and RegisterSold(id, count, total) (saturation += count, sold
//   event, MetaGame stats) - the same calls the Plort Market makes. Price = rounded current
//   value of that type (saturation lowers it at the director's next update, like a vac
//   full of plorts shot into the market).
//   Silo contents: SiloStorage (children of the silo plot).LocalAmmo -> AmmoSlotManager:
//   Slots[], GetSlotName(i) (IdentifiableType), GetSlotCount(i), Decrement(i, n) (0xA39790,
//   clears the slot at 0). Only IdentifiableType.IsPlort types are sold, everything else
//   stays. Order per slot: TrySell first (no money -> no item change), then Decrement.
//
// Fault policy: DetourGuard on every detour, objects class + IsAlive checked, game calls
// on the main thread only (runtime_invoke for everything that can throw), native faults
// mark the module faulted. Custom ids never reach vanilla AddUpgrade, also not when the
// module is off/faulted (then the menu items are removed and their price locked to
// int.MaxValue so an already open menu cannot buy them). No GC handle is freed; clones are
// pinned by never-freed handles (a handful per session). No scans; nothing per frame
// except one flag check.

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <climits>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/detour_guard.h"
#include "core/il2cpp_api.h"
#include "core/json.h"
#include "core/log.h"
#include "core/main_thread.h"
#include "core/module.h"
#include "core/paths.h"
#include "core/settings.h"
#include "core/text_override.h"
#include "core/unity.h"
#include "modules/custom_upgrades/api.h"
#include "modules/custom_upgrades/coop_world.h"
#include "modules/custom_upgrades/plot_menu_slider.h"
#include "modules/custom_upgrades/silo_world.h"
#include "ui/mod_icons.h"

namespace sr2::modules {
namespace {

namespace unity = sr2::core::unity;
namespace settings = sr2::core::settings;
namespace main_thread = sr2::core::main_thread;
namespace text_override = sr2::core::text_override;
using sr2::il2cpp::api;
using sr2::il2cpp::Resolver;

constexpr const char* kModule = "custom_upgrades";
constexpr const char* kImage = "Assembly-CSharp";
constexpr const char* kCore = "UnityEngine.CoreModule";
constexpr const char* kPlotUiNs = "MonomiPark.SlimeRancher.UI.Plot";
constexpr const char* kKeyPrice = "sell_all_price";
constexpr int kSidecarFormat = 2;  // 1 = one file per game (legacy), 2 = one file per .sav
constexpr const char* kRotationSuffix = ".sav.json";  // sidecar of <save>.sav = sr2mods_saves/<save>.sav.json
constexpr int kHideFlagsDontUnloadUnusedAsset = 32;

// LandPlot.Id
constexpr int32_t kPlotCorral = 2;
constexpr int32_t kPlotCoop = 3;
constexpr int32_t kPlotGarden = 4;
constexpr int32_t kPlotSilo = 5;
constexpr int32_t kPlotIncinerator = 7;
// LandPlot.Upgrade (vanilla)
constexpr int32_t kUpgMusicBox = 2;
constexpr int32_t kUpgSprinkler = 7;
constexpr int32_t kUpgFeeder = 9;
constexpr int32_t kUpgAirNet = 11;
constexpr int32_t kUpgPlortCollector = 12;
constexpr int32_t kUpgAshTrough = 14;
constexpr int32_t kUpgDeluxeCoop = 17;
constexpr int32_t kUpgStorageCapacity = 20;  // template preference
constexpr unsigned long long kTierCacheFrames = 900;
constexpr float kWorldTickSeconds = 1.0f;  // coop world objects: create / fit / remove

using custom_upgrades::Family;
constexpr int kFamilies = static_cast<int>(Family::kCount);
constexpr int kNoFamily = -1;

// ---- custom upgrade definitions -------------------------------------------------------
enum CustomId : int32_t {
    SILO_SELL_ALL = custom_upgrades::kIdBase + 0x0001,      // owned upgrade
    SILO_SELL_ALL_NOW = custom_upgrades::kIdBase + 0x0002,  // action item (needs SILO_SELL_ALL)
    CORRAL_COLLECTOR_2 = custom_upgrades::kIdBase + 0x0010,
    CORRAL_COLLECTOR_3 = custom_upgrades::kIdBase + 0x0011,
    CORRAL_FEEDER_2 = custom_upgrades::kIdBase + 0x0012,
    CORRAL_MUSIC_2 = custom_upgrades::kIdBase + 0x0013,
    COOP_HENS_1 = custom_upgrades::kIdBase + 0x0020,
    COOP_HENS_2 = custom_upgrades::kIdBase + 0x0021,
    GARDEN_GROWTH_1 = custom_upgrades::kIdBase + 0x0030,
    GARDEN_GROWTH_2 = custom_upgrades::kIdBase + 0x0031,
    INCIN_ASH_BOOST = custom_upgrades::kIdBase + 0x0040,
    COOP_AIR_NET = custom_upgrades::kIdBase + 0x0050,        // world object (coop_world.cpp)
    COOP_HEN_COLLECTOR = custom_upgrades::kIdBase + 0x0051,  // world object (coop_world.cpp)
};

// Upgrades that are a world object (only offered while the world-object code is ready).
bool IsWorldUpgrade(int32_t id) { return id == COOP_AIR_NET || id == COOP_HEN_COLLECTOR; }

struct UpgradeDef {
    int32_t id;
    const char* key;     // sidecar key (stable)
    int32_t plotType;    // LandPlot.Id
    bool action;         // never owned; buying runs the action
    int32_t needs;       // listed only while this upgrade is owned: custom id, vanilla id or 0
    int price;           // Newbucks (0 = setting / action)
    int family;          // Family (kNoFamily = not a tier)
    float mult;          // tier multiplier
    int32_t iconFrom;    // vanilla upgrade whose menu icon is reused (0 = Market Link icon)
    const char* title;
    const char* description;
    const char* prompt;
};

constexpr int F(Family f) { return static_cast<int>(f); }

const UpgradeDef kDefs[] = {
    {SILO_SELL_ALL, "silo_sell_all", kPlotSilo, false, 0, 0, kNoFamily, 1.0f, 0, "Sell-All Button",
     "A big red button for this silo that sells every plort inside at today's prices. Food and slimes stay put.",
     "Buy the Sell-All Button for this silo?"},
    {SILO_SELL_ALL_NOW, "silo_sell_all_now", kPlotSilo, true, SILO_SELL_ALL, 0, kNoFamily, 1.0f, 0,
     "Sell All Plorts Now", "Sells every plort in this silo at today's Plort Market prices.",
     "Sell all plorts in this silo?"},
    // ---- tiers ----
    {CORRAL_COLLECTOR_2, "corral_collector_2", kPlotCorral, false, kUpgPlortCollector, 2500, F(Family::kCollector),
     1.5f, kUpgPlortCollector, "Plort Collector II",
     "Tunes up this corral's plort collector so it vacuums up plorts 1.5x as often.",
     "Buy Plort Collector II for this corral?"},
    {CORRAL_COLLECTOR_3, "corral_collector_3", kPlotCorral, false, CORRAL_COLLECTOR_2, 5000, F(Family::kCollector),
     2.0f, kUpgPlortCollector, "Plort Collector III",
     "A fully tuned plort collector that vacuums up plorts twice as often. Replaces Plort Collector II.",
     "Buy Plort Collector III for this corral?"},
    {CORRAL_FEEDER_2, "corral_feeder_2", kPlotCorral, false, kUpgFeeder, 2500, F(Family::kFeeder), 2.0f, kUpgFeeder,
     "Auto-Feeder II",
     "A bigger hopper drops twice as much food per feeding. Just remember to keep it stocked!",
     "Buy Auto-Feeder II for this corral?"},
    {CORRAL_MUSIC_2, "corral_music_2", kPlotCorral, false, kUpgMusicBox, 2000, F(Family::kMusicBox), 2.0f,
     kUpgMusicBox, "Music Box II", "A catchier tune that calms this corral's slimes twice as well.",
     "Buy Music Box II for this corral?"},
    {COOP_HENS_1, "coop_hens_1", kPlotCoop, false, 0, 2000, F(Family::kCoop), 1.5f, kUpgDeluxeCoop, "Faster Hens I",
     "Cozier nests help this coop's hens lay eggs 1.5x as fast.", "Buy Faster Hens I for this coop?"},
    {COOP_HENS_2, "coop_hens_2", kPlotCoop, false, COOP_HENS_1, 4000, F(Family::kCoop), 2.0f, kUpgDeluxeCoop,
     "Faster Hens II", "The coziest nests around: hens lay eggs twice as fast. Replaces Faster Hens I.",
     "Buy Faster Hens II for this coop?"},
    {GARDEN_GROWTH_1, "garden_growth_1", kPlotGarden, false, 0, 2000, F(Family::kGarden), 1.5f, kUpgSprinkler,
     "Fast Growth I", "Extra-rich soil makes crops grow 1.5x as fast. Stacks with the sprinkler!",
     "Buy Fast Growth I for this garden?"},
    {GARDEN_GROWTH_2, "garden_growth_2", kPlotGarden, false, GARDEN_GROWTH_1, 4000, F(Family::kGarden), 2.0f,
     kUpgSprinkler, "Fast Growth II",
     "Crops grow twice as fast. Stacks with the sprinkler and replaces Fast Growth I.",
     "Buy Fast Growth II for this garden?"},
    {INCIN_ASH_BOOST, "incinerator_ash_boost", kPlotIncinerator, false, kUpgAshTrough, 1500, F(Family::kAsh), 2.0f,
     kUpgAshTrough, "Ash Booster",
     "Squeezes twice the ash out of everything you burn. Fire slimes approve!",
     "Buy the Ash Booster for this incinerator?"},
    // ---- world objects ----
    {COOP_AIR_NET, "coop_air_net", kPlotCoop, false, 0, 425, kNoFamily, 1.0f, kUpgAirNet, "Coop Air Net",
     "An air net for your coop. Launched chickens stay in, and nothing drops in from above.",
     "Buy the Coop Air Net for this coop?"},
    {COOP_HEN_COLLECTOR, "coop_hen_collector", kPlotCoop, false, 0, 1000, kNoFamily, 1.0f, kUpgDeluxeCoop,
     "Hen Collector",
     "Vacs up extra hens and roostros when this coop has more than 20. Chicks are safe! Set the limit with the "
     "slider.",
     "Buy the Hen Collector for this coop?"},
};

const UpgradeDef* FindDef(int32_t id) {
    for (const UpgradeDef& d : kDefs)
        if (d.id == id) return &d;
    return nullptr;
}

bool IsA(Il2CppObject* obj, Il2CppClass* k) {
    const auto& a = api();
    if (!obj || !k || !a.il2cpp_object_get_class) return false;
    Il2CppClass* c = a.il2cpp_object_get_class(obj);
    if (c == k) return true;
    return c && a.il2cpp_class_is_assignable_from && a.il2cpp_class_is_assignable_from(k, c);
}

template <class F>
F Export(const char* name) {
    HMODULE m = api().module;
    return m ? reinterpret_cast<F>(GetProcAddress(m, name)) : nullptr;
}

template <class Sig>
struct Bound;
template <class R, class... A>
struct Bound<R(A...)> {
    using Fn = R (*)(A..., const MethodInfo*);
    Fn fn = nullptr;
    const MethodInfo* mi = nullptr;
    explicit operator bool() const { return fn != nullptr; }
    R operator()(A... a) const { return fn(a..., mi); }
};

template <class T>
T Unbox(Il2CppObject* boxed, T def = T{}) {
    return boxed ? *reinterpret_cast<T*>(reinterpret_cast<char*>(boxed) + 2 * sizeof(void*)) : def;
}

std::string SanitizeFileName(const std::string& s) {
    std::string out;
    for (char c : s) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' ||
                        c == '_' || c == '.';
        out += ok ? c : '_';
        if (out.size() >= 120) break;
    }
    if (out.empty() || out == "." || out == "..") out = "_";
    return out;
}

// ---- sidecar state ----------------------------------------------------------------------
struct PlotState {
    int32_t type = 0;
    std::set<std::string> owned;  // UpgradeDef.key
    std::vector<custom_upgrades::world::HenSlot> hens;  // Hen Collector contents (per slot)
    int henKeep = custom_upgrades::world::kHenKeepDefault;  // Hen Collector keep-limit (slider)
    std::map<std::string, std::string> values;          // api.h SetPlotValue ("<module>.<name>")
    bool Empty() const { return owned.empty() && values.empty(); }
};

int ClampHenKeep(int64_t v) {
    return static_cast<int>(std::clamp<int64_t>(v, custom_upgrades::world::kHenKeepMin, custom_upgrades::world::kHenKeepMax));
}

// Short enough for the details panel (vanilla upgrade texts are 44-156 characters).
std::string HenDescription(int keep) {
    return std::format(
        "Vacs up extra hens and roostros when this coop has more than {}. Chicks are safe! Set the limit with the "
        "slider.",
        keep);
}

int HenCount(const PlotState& ps) {
    int n = 0;
    for (const auto& s : ps.hens) n += std::max(s.count, 0);
    return n;
}

struct SaveState {
    bool loaded = false;
    bool persisted = false;                    // read from / written to a sidecar file
    std::string game;                          // AutoSaveDirector.CurrentSaveGameName()
    std::string save;                          // .sav file (without extension) this state follows
    std::map<std::string, PlotState> plots;    // LandPlotLocation._id
    std::map<std::string, std::string> values; // api.h SetSaveValue
    bool Empty() const { return plots.empty() && values.empty(); }
};

// What a sidecar file says about itself (format 2).
struct SidecarMeta {
    std::string game, save;
    unsigned long long saveNumber = 0;  // GameMetadata.SaveNumber of the .sav it was written with
};

// One injected clone per (menu configuration, definition).
struct Injected {
    const UpgradeDef* def = nullptr;
    Il2CppObject* item = nullptr;      // our PlotUpgradePurchaseItemModel clone (GC handle held)
    Il2CppObject* costEntry = nullptr; // its first CurrencyCostEntry (price)
    bool inList = false;
};
struct MenuInjection {
    Il2CppObject* config = nullptr;    // MenuConfiguration (GC handle held)
    Il2CppObject* list = nullptr;      // the category's items list (GC handle held)
    std::vector<Injected> items;
    bool failed = false;               // clone/verification failed: never retried for this config
};

struct SiloSummary {
    int plorts = 0;        // sellable plort count
    long long value = 0;   // at current market values
    int types = 0;
    bool marketClosed = false;
    bool marketAvailable = false;
};

using HasUpgradeFn = bool (*)(Il2CppObject*, int32_t, const MethodInfo*);
HasUpgradeFn g_origHas = nullptr;  // original LandPlot.HasUpgrade (vanilla ids only)

class CustomUpgrades final : public sr2::core::Module {
public:
    const char* Name() const override { return kModule; }
    bool DefaultEnabled() const override { return true; }
    bool Setup(sr2::core::ModuleContext& ctx) override;
    void OnEnable() override {
        // The Load / NewGame hooks were off while disabled: the loaded save may be another one
        // than the in-memory state follows. Re-read on the next main-thread EnsureLoaded.
        resync_.store(true);
        enabled_.store(true);
        cleaned_ = false;
    }
    void OnDisable() override {
        enabled_.store(false);
        if (main_thread::IsMainThread()) Cleanup("module disabled");
        else main_thread::Post([this] { Cleanup("module disabled"); });
    }
    bool On() const {
        return enabled_.load(std::memory_order_relaxed) && !sr2::core::ModuleFaulted(kModule);
    }

    // detour bodies (main thread, inside DetourGuards)
    bool Owns(Il2CppObject* plot, int32_t upg);
    void OnAddUpgrade(Il2CppObject* plot, int32_t upg);
    void BeforeMenuUpdate(Il2CppObject* root);
    void AfterMenuUpdate(Il2CppObject* root);
    void BeforeSave();
    void AfterSave(Il2CppObject* result);
    void OnStoreGameData(Il2CppObject* provider, Il2CppString* fileName);
    void OnLoad(Il2CppObject* asd, const void* identifier);
    void AfterLoad(Il2CppObject* asd);
    void OnNewGame();
    void ResetState(const char* why);
    void FrameCheck(float dt);
    void WorldTick();
    bool OnSiloButton(Il2CppObject* activator);
    SiloSummary SellAllNow(Il2CppObject* plot, const std::string& id, const char* via);
    bool WorldQuery(Il2CppObject* plot, custom_upgrades::world::CoopRequest& req);
    void AfterDetailsBind(Il2CppObject* view, Il2CppObject* vm);
    void SliderTick(float dt);
    bool SliderActive() const { return slider_.Active(); }

    // api.h
    std::string CanSellCustom(Il2CppObject* plot, int32_t upg);
    bool RemoveCustom(Il2CppObject* plot, int32_t upg, std::string& notes);
    std::string VanillaDependentOf(Il2CppObject* plot, int32_t vanillaUpg);
    float TierMultiplier(Il2CppObject* obj, int family);
    std::string PlotValueOf(Il2CppObject* plot, const std::string& key);
    bool SetPlotValueOf(Il2CppObject* plot, const std::string& key, const std::string& value);
    std::string SaveValueOf(const std::string& key);
    bool SetSaveValueOf(const std::string& key, const std::string& value);

private:
    struct TierEntry {
        unsigned long long version = 0;
        unsigned long long tick = 0;
        float mult[kFamilies] = {};
    };
    bool VanillaOwned(Il2CppObject* plot, int32_t upg) {
        return g_origHas && unity::IsAlive(plot) && g_origHas(plot, upg, hasMi_);
    }
    void Bump() { ++version_; }

    unsigned long long version_ = 1;  // tier cache version (buy / sell / load / reset)
    std::unordered_map<Il2CppObject*, TierEntry> tierCache_;

    Il2CppObject* Invoke(const MethodInfo* m, void* obj, std::initializer_list<void*> args, const char* what,
                         bool* ok = nullptr);
    void SetRef(Il2CppObject* obj, size_t offset, Il2CppObject* value);
    bool EnsureLoaded();
    void ResyncWithLoadedSave(const std::string& game);
    bool FindExactRotation(const std::string& game, unsigned long long number, std::string& save);
    std::string CurrentGameName();
    std::filesystem::path SidecarDir() const { return sr2::paths::SelfDir() / "sr2mods_saves"; }
    std::filesystem::path LegacyPath(const std::string& game) const;
    std::filesystem::path RotationPath(const std::string& save) const;
    bool ReadSidecarFile(const std::filesystem::path& path, SaveState& into, SidecarMeta& meta, std::string& err);
    bool FindOlderRotation(const std::string& game, unsigned long long below, const std::string& skip,
                           std::filesystem::path& out, SidecarMeta& meta);
    void LoadSidecar(const std::string& game);
    bool WriteSidecar(const std::string& save, unsigned long long saveNumber);
    void PruneSidecars(const std::string& game, const std::string& justWritten);
    Il2CppObject* AutoSave();
    bool CurrentMetadata(Il2CppObject* asd, std::string& game, unsigned long long& saveNumber);
    std::filesystem::path ProviderSavePath(Il2CppObject* provider);
    std::string PlotId(Il2CppObject* plot);
    int32_t PlotType(Il2CppObject* plot) { return unity::Field<int32_t>(plot, offPlotTypeId_); }
    bool OwnsKey(Il2CppObject* plot, const UpgradeDef& d, std::string* idOut = nullptr);
    bool Visible(Il2CppObject* plot, const UpgradeDef& d);
    int Price(const UpgradeDef& d) const;

    MenuInjection* InjectionFor(Il2CppObject* config);
    bool BuildInjection(MenuInjection& inj, int32_t plotType);
    Il2CppObject* CloneItem(Il2CppObject* tpl, const UpgradeDef& d, Il2CppObject* iconSrc, Il2CppObject*& costEntry);
    bool ListCall(Il2CppObject* list, const char* method, Il2CppObject* item, bool* result = nullptr);
    Il2CppObject* MarketIcon();
    void SetTexts(const Injected& in, const std::string& title, const std::string& desc, const std::string& prompt);
    void Cleanup(const char* why);

    Il2CppObject* EconDirector();
    SiloSummary Summarize(Il2CppObject* plot, bool sell, std::string* log);

    std::atomic<bool> enabled_{false};
    std::atomic<bool> resync_{false};  // (re-)enabled: state must be re-read for the loaded save
    bool cleaned_ = false;
    float worldAccum_ = 0.0f;
    Il2CppObject* LookupDirector();
    std::map<int32_t, Il2CppObject*> globalUpgItems_;  // vanilla upgrade -> menu item of any plot menu (icons)
    bool globalUpgItemsTried_ = false;
    Il2CppObject* GlobalUpgradeItem(int32_t upgrade);
    custom_upgrades::world::CoopWorld world_;
    custom_upgrades::world::SiloButtons silo_;  // Sell-All Button world button
    custom_upgrades::ui::PlotMenuSlider slider_;  // Hen Collector limit (one at a time)
    Il2CppObject* menuRoot_ = nullptr;            // last updated LandPlotUIRoot (handle held)
    Il2CppGCHandle menuRootHandle_ = nullptr;
    void TrackMenuRoot(Il2CppObject* root);
    unsigned long long nameTick_ = ~0ull;
    std::string cachedGame_;
    SaveState state_;
    // exact .sav file: the one AutoSaveDirector.Load got (identifier.SaveName) and the one
    // SaveGameImpl handed to FileStorageProvider.StoreGameData
    std::string pendingLoadGame_, pendingLoadSave_;
    unsigned long long pendingLoadNumber_ = 0;  // GameMetadata.SaveNumber right after that Load
    bool capturing_ = false;                    // inside SaveGameImpl
    std::string capturedSave_;                  // file name StoreGameData got in this SaveGameImpl
    std::filesystem::path savePath_;            // FileStorageProvider.savePath (the .sav folder)
    bool loggedNoCapture_ = false;
    std::vector<std::unique_ptr<MenuInjection>> injections_;
    Il2CppObject* marketIcon_ = nullptr;
    bool marketIconTried_ = false;
    bool loggedSink_ = false;
    Il2CppClass* listClass_ = nullptr;
    const MethodInfo* listAdd_ = nullptr;
    const MethodInfo* listRemove_ = nullptr;
    const MethodInfo* listContains_ = nullptr;
    std::unordered_map<Il2CppClass*, size_t> offCategories_, offItems_;

public:
    // ---- resolved in Setup ----
    Il2CppClass* plotClass_ = nullptr;
    Il2CppClass* rootClass_ = nullptr;
    Il2CppClass* upgItemClass_ = nullptr;
    Il2CppClass* costEntryClass_ = nullptr;
    Il2CppClass* locationClass_ = nullptr;
    Il2CppClass* siloStorageClass_ = nullptr;
    Il2CppClass* linkedSiloClass_ = nullptr;
    Il2CppClass* identClass_ = nullptr;
    Il2CppClass* lsClass_ = nullptr;     // LocalizedString
    Il2CppClass* autoSaveClass_ = nullptr;

    size_t offPlotTypeId_ = 0, offActivator_ = 0, offMenuConfig_ = 0;
    size_t offItemUpgrade_ = 0, offItemTitle_ = 0, offItemDesc_ = 0, offItemPrompt_ = 0, offItemCosts_ = 0,
           offItemIcon_ = 0;
    size_t offCostCurrency_ = 0, offCostAmount_ = 0, offLocationId_ = 0, offIdentIsPlort_ = 0, offIdentIcon_ = 0;
    size_t offGcAutoSave_ = 0, offGcLookup_ = 0, offScEcon_ = 0;
    // save file identification (optional: missing -> one legacy sidecar per game, as before)
    Il2CppClass* fileProviderClass_ = nullptr;  // FileStorageProvider
    size_t offProviderSavePath_ = 0;            // FileStorageProvider.savePath
    size_t offAsdProvider_ = 0;                 // AutoSaveDirector._storageProvider
    size_t offAsdMetadata_ = 0;                 // AutoSaveDirector._currentGameMetadata (Nullable<GameMetadata>)
    size_t offMetaGameName_ = 0, offMetaSaveNumber_ = 0;  // GameMetadata (value-type offsets)
    bool saveIdOk_ = false;

    const MethodInfo* gameCtxInstMi_ = nullptr;
    const MethodInfo* sceneCtxInstMi_ = nullptr;
    const MethodInfo* curGameNameMi_ = nullptr;
    const MethodInfo* trySellMi_ = nullptr;
    const MethodInfo* marketValuesMi_ = nullptr;
    const MethodInfo* shutdownMi_ = nullptr;
    const MethodInfo* localAmmoMi_ = nullptr;
    const MethodInfo* slotsMi_ = nullptr;
    const MethodInfo* slotNameMi_ = nullptr;
    const MethodInfo* slotCountMi_ = nullptr;
    const MethodInfo* decrementMi_ = nullptr;
    const MethodInfo* marketLinkMi_ = nullptr;  // LookupDirector.get_MarketLink (optional)
    const MethodInfo* setHideFlagsMi_ = nullptr;

    Bound<Il2CppObject*(Il2CppObject*)> getTransform_;
    Bound<Il2CppObject*(Il2CppObject*)> getParent_;
    Bound<Il2CppObject*(Il2CppObject*, Il2CppObject*)> getComponent_;
    Bound<Il2CppArray*(Il2CppObject*, Il2CppObject*, bool)> getComponentsInChildren_;
    Bound<Il2CppObject*(Il2CppObject*, Il2CppObject*, bool)> getComponentInParent_;
    const MethodInfo* hasMi_ = nullptr;  // LandPlot.HasUpgrade (hidden arg for g_origHas)
    size_t offItemUnavailIcon_ = 0;      // MenuPurchaseItemModel._unavailableIcon
    size_t offVmDescription_ = 0;        // PurchaseItemDetailsContentViewModel._description (slider)

    using WBarrierFn = void (*)(Il2CppObject*, void**, void*);
    WBarrierFn wbarrier_ = nullptr;
};

CustomUpgrades* g_self = nullptr;
sr2::core::DetourGuard g_tierGuard{kModule, "custom_upgrades.Multiplier"};

sr2::core::DetourGuard g_hasGuard{kModule, "LandPlot.HasUpgrade"};
sr2::core::DetourGuard g_addGuard{kModule, "LandPlot.AddUpgrade"};
sr2::core::DetourGuard g_menuGuard{kModule, "LandPlotUIRoot.UpdateMenuItemAvailabilityAndHidden"};
sr2::core::DetourGuard g_saveGuard{kModule, "AutoSaveDirector.SaveGameImpl"};
sr2::core::DetourGuard g_loadGuard{kModule, "AutoSaveDirector.Load/StartNewGame"};
sr2::core::DetourGuard g_worldGuard{kModule, "custom_upgrades.CoopWorld"};
sr2::core::DetourGuard g_siloGuard{kModule, "PlortCollectorActivator.Activate"};
using ActivateFn = void (*)(Il2CppObject*, const MethodInfo*);
ActivateFn g_origActivate = nullptr;
size_t g_offActCollector = 0;  // PlortCollectorActivator.Collector

// Our silo button clones have Collector == null (silo_world.cpp): the original would only NRE on
// them, so it never runs for them - also not while the module is off / faulted. Vanilla buttons
// (Collector set) always go to the original.
void ActivateDetour(Il2CppObject* self, const MethodInfo* mi) {
    if (!self || !g_offActCollector || unity::Field<Il2CppObject*>(self, g_offActCollector) != nullptr) {
        g_origActivate(self, mi);
        return;
    }
    bool handled = false;
    if (g_self) g_siloGuard.Run([&] { handled = g_self->OnSiloButton(self); });
    if (!handled) SR2_DEBUG("[{}] plort collector button without a collector pressed - ignored", kModule);
}
sr2::core::DetourGuard g_detailsGuard{kModule, "PurchaseItemDetailsContentView.BindData"};
sr2::core::DetourGuard g_sliderGuard{kModule, "custom_upgrades.PlotMenuSlider"};

using AddUpgradeFn = void (*)(Il2CppObject*, int32_t, const MethodInfo*);
using MenuUpdateFn = void (*)(Il2CppObject*, const MethodInfo*);
using SaveImplFn = Il2CppObject* (*)(Il2CppObject*, bool, Il2CppObject*, const MethodInfo*);
using LoadFn = void (*)(Il2CppObject*, const void*, bool, Il2CppObject*, const MethodInfo*);  // struct by reference (x64)
using NewGameFn = void (*)(Il2CppObject*, int32_t, Il2CppObject*, const MethodInfo*);
using StoreGameFn = void (*)(Il2CppObject*, Il2CppString*, Il2CppString*, Il2CppObject*, const MethodInfo*);
sr2::core::DetourGuard g_storeGuard{kModule, "FileStorageProvider.StoreGameData"};
StoreGameFn g_origStore = nullptr;
AddUpgradeFn g_origAdd = nullptr;
MenuUpdateFn g_origMenu = nullptr;
SaveImplFn g_origSave = nullptr;
LoadFn g_origLoad = nullptr;
NewGameFn g_origNewGame = nullptr;
using DetailsBindFn = void (*)(Il2CppObject*, Il2CppObject*, const MethodInfo*);
DetailsBindFn g_origDetailsBind = nullptr;

void DetailsBindDetour(Il2CppObject* view, Il2CppObject* vm, const MethodInfo* mi) {
    g_origDetailsBind(view, vm, mi);
    // Every list menu's details panel binds here: one flag check for the others.
    if (g_self && view && g_self->On()) g_detailsGuard.Run([&] { g_self->AfterDetailsBind(view, vm); });
}

bool HasUpgradeDetour(Il2CppObject* plot, int32_t upg, const MethodInfo* mi) {
    if (!custom_upgrades::IsCustomUpgrade(upg)) return g_origHas(plot, upg, mi);
    // Custom ids are never in the vanilla set: never ask the original about them.
    bool owned = false;
    if (g_self && plot && g_self->On()) g_hasGuard.Run([&] { owned = g_self->Owns(plot, upg); });
    return owned;
}

void AddUpgradeDetour(Il2CppObject* plot, int32_t upg, const MethodInfo* mi) {
    if (!custom_upgrades::IsCustomUpgrade(upg)) {
        g_origAdd(plot, upg, mi);
        return;
    }
    // NEVER forward a custom id: it would end up in LandPlotModel.upgrades (the vanilla save).
    if (g_self && plot && g_self->On()) {
        g_addGuard.Run([&] { g_self->OnAddUpgrade(plot, upg); });
    } else {
        SR2_WARN("[{}] custom upgrade 0x{:X} bought while the module is off - ignored (not saved)", kModule, upg);
    }
}

void MenuUpdateDetour(Il2CppObject* root, const MethodInfo* mi) {
    const bool run = g_self && root && g_self->On();
    if (run) g_menuGuard.Run([&] { g_self->BeforeMenuUpdate(root); });
    g_origMenu(root, mi);
    if (run && g_self->On()) g_menuGuard.Run([&] { g_self->AfterMenuUpdate(root); });
}

Il2CppObject* SaveImplDetour(Il2CppObject* self, bool ignoreDebug, Il2CppObject* modify, const MethodInfo* mi) {
    const bool on = g_self && g_self->On();
    if (on) g_saveGuard.Run([&] { g_self->BeforeSave(); });
    Il2CppObject* r = g_origSave(self, ignoreDebug, modify, mi);
    if (on && g_self->On()) g_saveGuard.Run([&] { g_self->AfterSave(r); });
    return r;
}

// SaveGameImpl picks the file name ("{GameName}_{i}", the first i whose .sav does not exist)
// and hands it to StorageProvider.StoreGameData: we only note it.
void StoreGameDetour(Il2CppObject* self, Il2CppString* displayName, Il2CppString* fileName, Il2CppObject* stream,
                     const MethodInfo* mi) {
    if (g_self && g_self->On()) g_storeGuard.Run([&] { g_self->OnStoreGameData(self, fileName); });
    g_origStore(self, displayName, fileName, stream, mi);
}

void LoadDetour(Il2CppObject* self, const void* identifier, bool reloadAll, Il2CppObject* stream,
                const MethodInfo* mi) {
    if (g_self) g_loadGuard.Run([&] { g_self->OnLoad(self, identifier); });
    g_origLoad(self, identifier, reloadAll, stream, mi);
    if (g_self) g_loadGuard.Run([&] { g_self->AfterLoad(self); });
}

void NewGameDetour(Il2CppObject* self, int32_t slot, Il2CppObject* settingsModel, const MethodInfo* mi) {
    if (g_self) g_loadGuard.Run([&] { g_self->OnNewGame(); });
    g_origNewGame(self, slot, settingsModel, mi);
}

// ---------------------------------------------------------------- helpers --

Il2CppObject* CustomUpgrades::Invoke(const MethodInfo* m, void* obj, std::initializer_list<void*> args,
                                     const char* what, bool* ok) {
    std::vector<void*> params(args);
    std::string err;
    Il2CppObject* r = Resolver::Get().Invoke(m, obj, params.empty() ? nullptr : params.data(), &err);
    const bool good = err.empty();
    if (ok) *ok = good;
    if (!good) {
        if (err.rfind("native exception", 0) == 0) sr2::core::MarkModuleFaulted(kModule, std::format("{}: {}", what, err));
        else SR2_WARN("[{}] {} failed: {}", kModule, what, err);
    }
    return r;
}

void CustomUpgrades::SetRef(Il2CppObject* obj, size_t offset, Il2CppObject* value) {
    void** slot = reinterpret_cast<void**>(reinterpret_cast<char*>(obj) + offset);
    if (wbarrier_) wbarrier_(obj, slot, value);
    else *slot = value;
}

int CustomUpgrades::Price(const UpgradeDef& d) const {
    if (d.action) return 0;
    if (d.price > 0) return d.price;
    int p = settings::GetInt(kModule, kKeyPrice);
    return std::clamp(p, 0, 10000000);
}

std::string CustomUpgrades::PlotId(Il2CppObject* plot) {
    Il2CppObject* t = getTransform_(plot);
    Il2CppObject* parent = unity::IsAlive(t) ? getParent_(t) : nullptr;
    Il2CppObject* loc = unity::IsAlive(parent) ? getComponent_(parent, unity::TypeOf(locationClass_)) : nullptr;
    if (unity::IsAlive(loc) && IsA(loc, locationClass_))
        return unity::ToUtf8(unity::Field<Il2CppString*>(loc, offLocationId_));
    return {};
}

// UTF-8 (il2cpp strings) -> filesystem path without going through the ANSI code page.
std::filesystem::path Utf8Path(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return {};
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return std::filesystem::path(w);
}

std::wstring Widen(const std::string& ascii) { return std::wstring(ascii.begin(), ascii.end()); }

bool EndsWith(const std::wstring& s, const std::wstring& suffix) {
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::filesystem::path CustomUpgrades::LegacyPath(const std::string& game) const {
    return SidecarDir() / (SanitizeFileName(game) + ".json");
}

std::filesystem::path CustomUpgrades::RotationPath(const std::string& save) const {
    return SidecarDir() / (SanitizeFileName(save) + kRotationSuffix);
}

Il2CppObject* CustomUpgrades::AutoSave() {
    bool ok = false;
    Il2CppObject* gc = Invoke(gameCtxInstMi_, nullptr, {}, "GameContext.Instance", &ok);
    if (!ok || !unity::IsAlive(gc)) return nullptr;
    Il2CppObject* asd = unity::Field<Il2CppObject*>(gc, offGcAutoSave_);
    return unity::IsAlive(asd) ? asd : nullptr;
}

bool CustomUpgrades::CurrentMetadata(Il2CppObject* asd, std::string& game, unsigned long long& saveNumber) {
    if (!saveIdOk_ || !unity::IsAlive(asd) || !IsA(asd, autoSaveClass_)) return false;
    // Nullable<GameMetadata>: bool hasValue at +0, the struct at +8 (SaveGameImpl 0x5D10A0 tests
    // [asd+0x68] and writes GameName at [asd+0x70]).
    char* nullable = reinterpret_cast<char*>(asd) + offAsdMetadata_;
    if (!*reinterpret_cast<bool*>(nullable)) return false;
    char* meta = nullable + 8;
    game = unity::ToUtf8(*reinterpret_cast<Il2CppString**>(meta + offMetaGameName_));
    saveNumber = *reinterpret_cast<unsigned long long*>(meta + offMetaSaveNumber_);
    return true;
}

std::filesystem::path CustomUpgrades::ProviderSavePath(Il2CppObject* provider) {
    if (!fileProviderClass_ || !provider || !IsA(provider, fileProviderClass_)) return {};
    return Utf8Path(unity::ToUtf8(unity::Field<Il2CppString*>(provider, offProviderSavePath_)));
}

std::string CustomUpgrades::CurrentGameName() {
    Il2CppObject* asd = AutoSave();
    if (!asd) return {};
    bool ok = false;
    Il2CppObject* s = Invoke(curGameNameMi_, asd, {}, "AutoSaveDirector.CurrentSaveGameName", &ok);
    return ok ? unity::ToUtf8(reinterpret_cast<Il2CppString*>(s)) : std::string();
}

void CustomUpgrades::ResetState(const char* why) {
    nameTick_ = ~0ull;
    if (state_.loaded) SR2_INFO("[{}] {}: custom upgrade state of '{}' dropped (re-read from its sidecar)", kModule, why,
                                state_.game);
    state_ = {};
    Bump();
    tierCache_.clear();
    world_.Reset();  // clones of the previous game are destroyed (never read) at the next tick
    silo_.Reset();
    slider_.Destroy();  // its value callback belongs to the old state
}

void CustomUpgrades::OnLoad(Il2CppObject* asd, const void* identifier) {
    resync_.store(false);  // this load names the file exactly
    ResetState("save game load");
    pendingLoadGame_.clear();
    pendingLoadSave_.clear();
    pendingLoadNumber_ = 0;
    if (identifier) {
        // GameSaveIdentifier { string GameName; string SaveName; } by reference (x64, 16 bytes).
        // SaveName is the .sav file name without extension (FileStorageProvider.GetFullFilePath
        // appends ".sav" to it).
        auto* const* s = static_cast<Il2CppString* const*>(identifier);
        pendingLoadGame_ = unity::ToUtf8(s[0]);
        pendingLoadSave_ = unity::ToUtf8(s[1]);
    }
    if (unity::IsAlive(asd) && IsA(asd, autoSaveClass_) && offAsdProvider_) {
        auto p = ProviderSavePath(unity::Field<Il2CppObject*>(asd, offAsdProvider_));
        if (!p.empty()) savePath_ = p;
    }
    SR2_INFO("[{}] loading save file '{}.sav' (game '{}')", kModule, pendingLoadSave_, pendingLoadGame_);
}

void CustomUpgrades::AfterLoad(Il2CppObject* asd) {
    // Load sets _currentGameMetadata from the loaded file (GameMetadata(GameV11)) before it returns.
    std::string game;
    unsigned long long n = 0;
    if (CurrentMetadata(asd, game, n) && game == pendingLoadGame_) pendingLoadNumber_ = n;
}

void CustomUpgrades::OnNewGame() {
    resync_.store(false);
    ResetState("new game");
    pendingLoadGame_.clear();
    pendingLoadSave_.clear();
    pendingLoadNumber_ = 0;
}

void CustomUpgrades::OnStoreGameData(Il2CppObject* provider, Il2CppString* fileName) {
    if (!capturing_ || !main_thread::IsMainThread()) return;  // only the game save inside SaveGameImpl
    capturedSave_ = unity::ToUtf8(fileName);
    auto p = ProviderSavePath(provider);
    if (!p.empty()) savePath_ = p;
}

bool CustomUpgrades::ReadSidecarFile(const std::filesystem::path& path, SaveState& into, SidecarMeta& meta,
                                     std::string& err) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        err = "cannot open";
        return false;
    }
    std::stringstream ss;
    ss << f.rdbuf();
    sr2::json::Value root;
    if (!sr2::json::Parse(ss.str(), root, err) || !root.isObject()) {
        if (err.empty()) err = "not a JSON object";
        return false;
    }
    meta.game = root["game"].asString();
    meta.save = root["save"].asString();
    const double num = root["save_number"].asNumber(0);
    meta.saveNumber = num > 0 ? static_cast<unsigned long long>(num) : 0;
    auto readValues = [](const sr2::json::Value& v, std::map<std::string, std::string>& out) {
        for (const std::string& k : v.keys()) {
            std::string s = v[k].asString();
            if (!k.empty() && !s.empty()) out[k] = std::move(s);
        }
    };
    readValues(root["values"], into.values);
    const sr2::json::Value& plots = root["plots"];
    for (const std::string& id : plots.keys()) {
        const sr2::json::Value& p = plots[id];
        PlotState ps;
        ps.type = static_cast<int32_t>(p["type"].asInt(0));
        const sr2::json::Value& ups = p["upgrades"];
        for (size_t i = 0; i < ups.size(); ++i) {
            std::string key = ups.at(i).asString();
            bool known = false;
            for (const UpgradeDef& d : kDefs) known |= (key == d.key && !d.action);
            if (known) ps.owned.insert(key);
            else if (!key.empty()) SR2_WARN("[{}] sidecar: unknown upgrade '{}' on plot '{}' ignored", kModule, key, id);
        }
        // Hen Collector contents: [{"type": "<IdentifiableType name>", "count": n}, ...], one per slot.
        const sr2::json::Value& hens = p["hen_collector"];
        for (size_t i = 0; i < hens.size() && i < 8; ++i) {
            custom_upgrades::world::HenSlot s;
            s.type = hens.at(i)["type"].asString();
            s.count = static_cast<int>(std::clamp<int64_t>(hens.at(i)["count"].asInt(0), 0, 100000));
            if (s.type.empty()) s.count = 0;
            ps.hens.push_back(std::move(s));
        }
        // Hen Collector keep-limit (plot menu slider); absent -> default.
        ps.henKeep = ClampHenKeep(p["hen_collector_keep"].asInt(custom_upgrades::world::kHenKeepDefault));
        if (!ps.owned.count("coop_hen_collector")) {
            ps.hens.clear();
            ps.henKeep = custom_upgrades::world::kHenKeepDefault;
        }
        readValues(p["values"], ps.values);
        if (!ps.Empty()) into.plots[id] = std::move(ps);
    }
    return true;
}

bool CustomUpgrades::FindOlderRotation(const std::string& game, unsigned long long below, const std::string& skip,
                                       std::filesystem::path& out, SidecarMeta& meta) {
    // Newest sidecar of this game written with a lower save number than the loaded file (any,
    // when that number is unknown). The rotation index is no age order: SaveGameImpl reuses the
    // first free "{GameName}_{i}", so the save number inside each sidecar decides.
    std::error_code ec;
    const std::wstring prefix = Widen(SanitizeFileName(game) + "_");
    const std::wstring suffix = Widen(kRotationSuffix);
    bool found = false;
    std::filesystem::directory_iterator it(SidecarDir(), ec), end;
    for (int guard = 0; !ec && it != end && guard < 4096; it.increment(ec), ++guard) {
        const std::wstring name = it->path().filename().wstring();
        if (name.rfind(prefix, 0) != 0 || !EndsWith(name, suffix)) continue;
        SaveState tmp;
        SidecarMeta m;
        std::string err;
        if (!ReadSidecarFile(it->path(), tmp, m, err)) continue;
        if (m.game != game || m.save.empty() || m.save == skip || m.saveNumber == 0) continue;
        if (below && m.saveNumber >= below) continue;
        if (!found || m.saveNumber > meta.saveNumber) {
            found = true;
            meta = m;
            out = it->path();
        }
    }
    return found;
}

void CustomUpgrades::LoadSidecar(const std::string& game) {
    state_ = {};
    state_.loaded = true;
    state_.game = game;
    Bump();
    world_.Reset();  // world objects built before belong to another state: rebuilt from this one
    silo_.Reset();

    // The exact .sav this game was loaded from (AutoSaveDirector.Load), if we saw it.
    std::string save;
    unsigned long long number = 0;
    if (!pendingLoadSave_.empty() && pendingLoadGame_ == game) {
        save = pendingLoadSave_;
        number = pendingLoadNumber_;
    }
    state_.save = save;

    std::error_code ec;
    std::filesystem::path used;
    std::string source;
    auto use = [&](const std::filesystem::path& p, const std::string& what) -> bool {
        SaveState tmp;
        SidecarMeta m;
        std::string err;
        if (!ReadSidecarFile(p, tmp, m, err)) {
            SR2_WARN("[{}] sidecar {} unreadable ({}) - skipped", kModule, p.string(), err);
            return false;
        }
        state_.plots = std::move(tmp.plots);
        state_.values = std::move(tmp.values);
        state_.persisted = true;
        used = p;
        source = what;
        return true;
    };

    bool done = false;
    if (!save.empty()) {
        const auto p = RotationPath(save);
        if (std::filesystem::exists(p, ec)) {
            SaveState tmp;
            SidecarMeta m;
            std::string err;
            if (!ReadSidecarFile(p, tmp, m, err)) {
                SR2_WARN("[{}] sidecar {} unreadable ({}) - looking for another one", kModule, p.string(), err);
            } else if (m.save != save) {
                SR2_WARN("[{}] sidecar {} belongs to '{}', not '{}' - ignored", kModule, p.string(), m.save, save);
            } else if (m.saveNumber && number && m.saveNumber != number) {
                // The game reuses rotation file names: this sidecar is from an older file of the
                // same name (that file was replaced while the mod was off).
                SR2_INFO("[{}] sidecar {} was written for save #{}, the loaded file is save #{} - ignored", kModule,
                         p.string(), m.saveNumber, number);
            } else {
                done = use(p, "the sidecar of this save file");
            }
        }
    }
    if (!done) {
        const auto p = LegacyPath(game);
        if (std::filesystem::exists(p, ec)) done = use(p, "the old per-game sidecar (migration)");
    }
    if (!done) {
        std::filesystem::path p;
        SidecarMeta m;
        if (FindOlderRotation(game, number, save, p, m))
            done = use(p, std::format("the newest older save's sidecar ('{}', save #{})", m.save, m.saveNumber));
    }
    if (!done) {
        SR2_INFO("[{}] save game '{}' (file '{}', save #{}): no sidecar yet", kModule, game, save, number);
        return;
    }
    int n = 0;
    for (const auto& [id, ps] : state_.plots) n += static_cast<int>(ps.owned.size());
    SR2_INFO("[{}] save game '{}' (file '{}', save #{}): using {} ({}): {} custom upgrade(s) on {} plot(s), {} value(s)",
             kModule, game, save, number, source, used.filename().string(), n, state_.plots.size(),
             state_.values.size());
}

bool CustomUpgrades::EnsureLoaded() {
    // The game name is looked up at most once per frame (HasUpgrade can be asked often).
    const unsigned long long tick = main_thread::TickCount();
    if (tick != nameTick_ || !main_thread::Ready()) {
        cachedGame_ = CurrentGameName();
        nameTick_ = tick;
    }
    const std::string game = cachedGame_;
    if (game.empty()) return false;
    if (resync_.load()) {
        if (!main_thread::IsMainThread()) return false;  // never answer from the stale state
        resync_.store(false);
        ResyncWithLoadedSave(game);
    }
    if (!state_.loaded || state_.game != game) LoadSidecar(game);
    return true;
}

bool CustomUpgrades::FindExactRotation(const std::string& game, unsigned long long number, std::string& save) {
    // This game's per-save sidecar written with exactly this save number whose .sav still exists
    // (when the save folder is known). Two branches of a game can share a number (load an older
    // save, save again into another file): then the most recently written .sav wins.
    std::error_code ec;
    const std::wstring prefix = Widen(SanitizeFileName(game) + "_");
    const std::wstring suffix = Widen(kRotationSuffix);
    const bool checkSav = !savePath_.empty() && std::filesystem::is_directory(savePath_, ec);
    std::filesystem::file_time_type best{};
    int matches = 0;
    std::filesystem::directory_iterator it(SidecarDir(), ec), end;
    for (int guard = 0; !ec && it != end && guard < 4096; it.increment(ec), ++guard) {
        const std::wstring name = it->path().filename().wstring();
        if (name.rfind(prefix, 0) != 0 || !EndsWith(name, suffix)) continue;
        SaveState tmp;
        SidecarMeta m;
        std::string err;
        if (!ReadSidecarFile(it->path(), tmp, m, err)) continue;
        if (m.game != game || m.save.empty() || m.saveNumber != number) continue;
        if (it->path().filename() != RotationPath(m.save).filename()) continue;  // copied / renamed file
        std::filesystem::file_time_type t{};
        if (checkSav) {
            std::error_code ec2;
            const auto sav = savePath_ / Utf8Path(m.save + ".sav");
            if (!std::filesystem::exists(sav, ec2)) continue;  // its .sav is gone: not the loaded one
            t = std::filesystem::last_write_time(sav, ec2);
        }
        if (matches++ == 0 || t > best) {
            best = t;
            save = m.save;
        }
    }
    if (matches > 1)
        SR2_WARN("[{}] {} sidecars of '{}' carry save #{} - using '{}' (newest .sav)", kModule, matches, game, number,
                 save);
    return matches > 0;
}

void CustomUpgrades::ResyncWithLoadedSave(const std::string& game) {
    // Main thread. The module was (re-)enabled while a game is loaded: the in-memory state may
    // belong to another save of this game (loaded while the Load hook was off). Drop it and read
    // the sidecar of the save loaded now. Never writes a sidecar.
    ResetState("module enabled");
    pendingLoadGame_.clear();
    pendingLoadSave_.clear();
    pendingLoadNumber_ = 0;
    Il2CppObject* asd = AutoSave();
    if (asd && IsA(asd, autoSaveClass_) && offAsdProvider_) {
        auto p = ProviderSavePath(unity::Field<Il2CppObject*>(asd, offAsdProvider_));
        if (!p.empty()) savePath_ = p;
    }
    if (!saveIdOk_) {
        // No save-number support: one per-game sidecar, exactly what the Load path reads too.
        SR2_INFO("[{}] module enabled with game '{}' loaded: re-reading its per-game sidecar", kModule, game);
        LoadSidecar(game);
        return;
    }
    std::string metaGame;
    unsigned long long number = 0;
    if (!CurrentMetadata(asd, metaGame, number) || metaGame != game || number == 0) {
        SR2_WARN("[{}] module enabled with game '{}' loaded, but the loaded save cannot be identified (metadata game "
                 "'{}', save #{}) - no custom upgrades for it, its sidecars are not read",
                 kModule, game, metaGame, number);
        state_.loaded = true;
        state_.game = game;
        Bump();
        return;
    }
    std::string save;
    const bool exact = FindExactRotation(game, number, save);
    pendingLoadGame_ = game;
    pendingLoadSave_ = save;  // empty: LoadSidecar goes on with the per-game / newest older sidecar
    pendingLoadNumber_ = number;
    SR2_INFO("[{}] module enabled with game '{}' loaded: save #{} ({}) - re-reading its custom upgrades", kModule,
             game, number, exact ? std::format("file '{}'", save) : std::string("no sidecar with that number"));
    LoadSidecar(game);
}

bool CustomUpgrades::WriteSidecar(const std::string& save, unsigned long long saveNumber) {
    if (!state_.loaded || state_.game.empty()) return false;
    const bool rotation = !save.empty();
    const auto path = rotation ? RotationPath(save) : LegacyPath(state_.game);
    std::error_code ec;
    // Nothing of ours in this game so far: no file (players who never buy a custom upgrade get
    // none). Once there was one, every save gets its own, also an empty one (else loading that
    // save would fall back to an older state).
    if (state_.Empty() && !state_.persisted && !std::filesystem::exists(path, ec)) return true;
    std::filesystem::create_directories(path.parent_path(), ec);

    sr2::json::Value root = sr2::json::Value::Object();
    root.set("format", sr2::json::Value(static_cast<double>(rotation ? kSidecarFormat : 1)));
    root.set("game", sr2::json::Value(state_.game));
    if (rotation) {
        root.set("save", sr2::json::Value(save));
        root.set("save_number", sr2::json::Value(static_cast<double>(saveNumber)));
    }
    root.set("note", sr2::json::Value(std::string(
                         "sr2mods custom land-plot upgrades for this save file. Not read by the game; deleting it only "
                         "removes the custom upgrades.")));
    if (!state_.values.empty()) {
        sr2::json::Value& vals = root.child("values");
        for (const auto& [k, v] : state_.values) vals.set(k, sr2::json::Value(v));
    }
    sr2::json::Value& plots = root.child("plots");
    for (const auto& [id, ps] : state_.plots) {
        if (ps.Empty()) continue;
        sr2::json::Value p = sr2::json::Value::Object();
        p.set("type", sr2::json::Value(static_cast<double>(ps.type)));
        sr2::json::Value arr = sr2::json::Value::Array();
        for (const std::string& k : ps.owned) arr.push(sr2::json::Value(k));
        p.set("upgrades", std::move(arr));
        if (HenCount(ps) > 0) {
            sr2::json::Value hens = sr2::json::Value::Array();
            for (const auto& s : ps.hens) {
                sr2::json::Value h = sr2::json::Value::Object();
                h.set("type", sr2::json::Value(s.count > 0 ? s.type : std::string()));
                h.set("count", sr2::json::Value(static_cast<double>(std::max(s.count, 0))));
                hens.push(std::move(h));
            }
            p.set("hen_collector", std::move(hens));
        }
        if (ps.owned.count("coop_hen_collector"))
            p.set("hen_collector_keep", sr2::json::Value(static_cast<double>(ClampHenKeep(ps.henKeep))));
        if (!ps.values.empty()) {
            sr2::json::Value vals = sr2::json::Value::Object();
            for (const auto& [k, v] : ps.values) vals.set(k, sr2::json::Value(v));
            p.set("values", std::move(vals));
        }
        plots.set(id, std::move(p));
    }
    const std::string text = sr2::json::Serialize(root);
    auto tmp = path;
    tmp += L".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) {
            SR2_WARN("[{}] cannot write {}", kModule, tmp.string());
            return false;
        }
        f.write(text.data(), static_cast<std::streamsize>(text.size()));
        if (!f) return false;
    }
    if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        SR2_WARN("[{}] cannot replace {} (error {})", kModule, path.string(), GetLastError());
        return false;
    }
    state_.persisted = true;
    if (rotation) {
        // Migration: the per-game sidecar is superseded once this game has a per-save one. Kept
        // as *.json.migrated (never read again) instead of deleted.
        const auto legacy = LegacyPath(state_.game);
        if (std::filesystem::exists(legacy, ec)) {
            auto bak = legacy;
            bak += L".migrated";
            if (MoveFileExW(legacy.c_str(), bak.c_str(), MOVEFILE_REPLACE_EXISTING))
                SR2_INFO("[{}] per-game sidecar {} migrated to per-save sidecars (old file kept as {})", kModule,
                         legacy.filename().string(), bak.filename().string());
        }
    }
    return true;
}

void CustomUpgrades::PruneSidecars(const std::string& game, const std::string& justWritten) {
    // Sidecars of this game whose .sav the game has deleted (CleanupAutosaves keeps 5). Only
    // when the save folder is known and holds the file just written (sanity check).
    std::error_code ec;
    if (savePath_.empty() || !std::filesystem::is_directory(savePath_, ec)) return;
    if (!std::filesystem::exists(savePath_ / Utf8Path(justWritten + ".sav"), ec)) return;
    const std::wstring prefix = Widen(SanitizeFileName(game) + "_");
    const std::wstring suffix = Widen(kRotationSuffix);
    const std::wstring keep = Widen(SanitizeFileName(justWritten));
    std::vector<std::filesystem::path> stale;
    std::filesystem::directory_iterator it(SidecarDir(), ec), end;
    for (int guard = 0; !ec && it != end && guard < 4096; it.increment(ec), ++guard) {
        const std::wstring name = it->path().filename().wstring();
        if (name.rfind(prefix, 0) != 0 || !EndsWith(name, suffix)) continue;
        const std::wstring stem = name.substr(0, name.size() - suffix.size());  // "<save>"
        if (stem == keep) continue;
        std::error_code ec2;
        if (!std::filesystem::exists(savePath_ / (stem + L".sav"), ec2) && !ec2) stale.push_back(it->path());
    }
    for (const auto& p : stale) {
        std::error_code ec2;
        if (std::filesystem::remove(p, ec2))
            SR2_INFO("[{}] sidecar {} removed (its save file is gone)", kModule, p.filename().string());
    }
}

bool CustomUpgrades::OwnsKey(Il2CppObject* plot, const UpgradeDef& d, std::string* idOut) {
    if (d.action) return false;
    if (!unity::IsAlive(plot) || !IsA(plot, plotClass_)) return false;
    if (PlotType(plot) != d.plotType) return false;
    if (!EnsureLoaded()) return false;
    const std::string id = PlotId(plot);
    if (idOut) *idOut = id;
    if (id.empty()) return false;
    auto it = state_.plots.find(id);
    if (it == state_.plots.end()) return false;
    if (it->second.type != d.plotType) {
        // The plot was demolished and rebuilt as another type: its custom upgrades are gone.
        SR2_INFO("[{}] plot '{}' is no longer type {} - dropping its custom upgrades", kModule, id, it->second.type);
        state_.plots.erase(it);
        Bump();
        return false;
    }
    return it->second.owned.count(d.key) != 0;
}

bool CustomUpgrades::Owns(Il2CppObject* plot, int32_t upg) {
    if (!main_thread::IsMainThread()) return false;
    const UpgradeDef* d = FindDef(upg);
    return d && OwnsKey(plot, *d);
}

bool CustomUpgrades::Visible(Il2CppObject* plot, const UpgradeDef& d) {
    if (PlotType(plot) != d.plotType) return false;
    if (IsWorldUpgrade(d.id) && !world_.Ready()) return false;
    if (!d.needs) return true;
    if (!custom_upgrades::IsCustomUpgrade(d.needs)) return VanillaOwned(plot, d.needs);  // vanilla prerequisite
    const UpgradeDef* r = FindDef(d.needs);
    return r && OwnsKey(plot, *r);
}

// ---------------------------------------------------------------- menu --

bool CustomUpgrades::ListCall(Il2CppObject* list, const char* method, Il2CppObject* item, bool* result) {
    Il2CppClass* k = api().il2cpp_object_get_class(list);
    if (!k) return false;
    if (k != listClass_) {
        std::string err;
        const MethodInfo* add = Resolver::Get().Method(k, "Add", 1, &err);
        const MethodInfo* rem = Resolver::Get().Method(k, "Remove", 1, &err);
        const MethodInfo* has = Resolver::Get().Method(k, "Contains", 1, &err);
        if (!add || !rem || !has) {
            SR2_WARN("[{}] List methods not found: {}", kModule, err);
            return false;
        }
        listClass_ = k;
        listAdd_ = add;
        listRemove_ = rem;
        listContains_ = has;
    }
    const MethodInfo* m = std::string_view(method) == "Add" ? listAdd_
                          : std::string_view(method) == "Remove" ? listRemove_
                                                                 : listContains_;
    bool ok = false;
    Il2CppObject* boxed = Invoke(m, list, {item}, method, &ok);
    if (ok && result && m != listAdd_) *result = Unbox<bool>(boxed);
    return ok;
}

Il2CppObject* CustomUpgrades::MarketIcon() {
    if (marketIconTried_) return marketIcon_;
    marketIconTried_ = true;
    if (!marketLinkMi_) return nullptr;
    bool ok = false;
    Il2CppObject* gc = Invoke(gameCtxInstMi_, nullptr, {}, "GameContext.Instance", &ok);
    Il2CppObject* lookup = (ok && unity::IsAlive(gc)) ? unity::Field<Il2CppObject*>(gc, offGcLookup_) : nullptr;
    if (!unity::IsAlive(lookup)) return nullptr;
    Il2CppObject* ml = Invoke(marketLinkMi_, lookup, {}, "LookupDirector.get_MarketLink", &ok);
    if (!ok || !unity::IsAlive(ml) || !IsA(ml, identClass_)) return nullptr;
    Il2CppObject* icon = unity::Field<Il2CppObject*>(ml, offIdentIcon_);
    if (unity::IsAlive(icon)) {
        marketIcon_ = icon;
        unity::NewGCHandle(icon);  // pinned for the session (never freed)
        SR2_INFO("[{}] using the Market Link icon '{}' for the sell-all items", kModule, unity::GetName(icon));
    }
    return marketIcon_;
}

// The mod's own icon for a custom item (assets/icons, ui/mod_icons.h).
const char* ModIconName(int32_t id) {
    switch (id) {
        case SILO_SELL_ALL: return "sell_all";
        case SILO_SELL_ALL_NOW: return "sell_all_now";
        case CORRAL_COLLECTOR_2: return "collector_2";
        case CORRAL_COLLECTOR_3: return "collector_3";
        case CORRAL_FEEDER_2: return "feeder_2";
        case CORRAL_MUSIC_2: return "music_2";
        case COOP_HENS_1: return "hens_1";
        case COOP_HENS_2: return "hens_2";
        case GARDEN_GROWTH_1: return "growth_1";
        case GARDEN_GROWTH_2: return "growth_2";
        case INCIN_ASH_BOOST: return "ash_boost";
        case COOP_AIR_NET: return "coop_air_net";
        case COOP_HEN_COLLECTOR: return "hen_collector";
        default: return nullptr;
    }
}
Il2CppObject* CustomUpgrades::CloneItem(Il2CppObject* tpl, const UpgradeDef& d, Il2CppObject* iconSrc,
                                        Il2CppObject*& costEntry) {
    costEntry = nullptr;
    Il2CppObject* clone = unity::Instantiate(tpl, nullptr);
    if (!unity::IsAlive(clone) || !IsA(clone, upgItemClass_)) return nullptr;
    unity::NewGCHandle(clone);  // never freed: the clone lives for the session
    unity::SetName(clone, std::string("sr2mods_") + d.key);
    if (setHideFlagsMi_) {
        int32_t flags = kHideFlagsDontUnloadUnusedAsset;
        Invoke(setHideFlagsMi_, clone, {&flags}, "Object.set_hideFlags");
    }
    // Deep-copy check: the clone must own its texts and cost list, else overriding them would
    // change the vanilla item too.
    for (size_t off : {offItemTitle_, offItemDesc_, offItemPrompt_}) {
        Il2CppObject* a = unity::Field<Il2CppObject*>(tpl, off);
        Il2CppObject* b = unity::Field<Il2CppObject*>(clone, off);
        if (!b || a == b || !IsA(b, lsClass_)) {
            SR2_WARN("[{}] cloned menu item shares/lacks its LocalizedString (+0x{:X}) - custom items disabled for this "
                     "menu", kModule, off);
            return nullptr;
        }
    }
    void* tplList = unity::Field<void*>(tpl, offItemCosts_);
    void* list = unity::Field<void*>(clone, offItemCosts_);
    if (!list || list == tplList || unity::ListCount(list) == 0) {
        SR2_WARN("[{}] cloned menu item has no own cost list - custom items disabled for this menu", kModule);
        return nullptr;
    }
    for (size_t i = 0, n = unity::ListCount(list); i < n && i < 8; ++i) {
        auto* e = static_cast<Il2CppObject*>(unity::ListAt(list, i));
        if (!IsA(e, costEntryClass_)) continue;
        if (!costEntry) costEntry = e;
        else unity::Field<int32_t>(e, offCostAmount_) = 0;  // single price (first currency)
    }
    if (!costEntry) return nullptr;
    unity::Field<int32_t>(clone, offItemUpgrade_) = d.id;
    unity::Field<int32_t>(costEntry, offCostAmount_) = Price(d);
    // Icon: the vanilla upgrade this tier builds on, otherwise the Market Link icon (sell-all).
    // _unavailableIcon is NOT an item icon: every vanilla plot upgrade item points it at the shared
    // "checkmark" sprite, which GenericViewHolder.BindData shows as an overlay image on top of the
    // (tinted) list icon when the item is unavailable (= owned). So owned custom items keep that
    // checkmark: the template's (= iconSrc's, same shared sprite) reference, copied by Instantiate.
    Il2CppObject* icon = iconSrc ? unity::Field<Il2CppObject*>(iconSrc, offItemIcon_) : nullptr;
    Il2CppObject* check = iconSrc ? unity::Field<Il2CppObject*>(iconSrc, offItemUnavailIcon_) : nullptr;
    if (!unity::IsAlive(check)) check = unity::Field<Il2CppObject*>(tpl, offItemUnavailIcon_);
    if (!unity::IsAlive(icon)) icon = MarketIcon();
    // Our own icon replaces the borrowed one; the borrowed sprite stays the fallback and sets the
    // native size.
    if (const char* own = ModIconName(d.id)) {
        Il2CppObject* sizeRef = unity::IsAlive(icon) ? icon : unity::Field<Il2CppObject*>(tpl, offItemIcon_);
        if (Il2CppObject* mine = sr2::ui::mod_icons::Get(own, sizeRef)) icon = mine;
    }
    if (unity::IsAlive(icon)) SetRef(clone, offItemIcon_, icon);
    if (offItemUnavailIcon_) SetRef(clone, offItemUnavailIcon_, unity::IsAlive(check) ? check : nullptr);
    return clone;
}

bool CustomUpgrades::BuildInjection(MenuInjection& inj, int32_t plotType) {
    // The category holding the vanilla upgrade items, and a template upgrade item.
    void* cats = nullptr;
    {
        Il2CppClass* k = api().il2cpp_object_get_class(inj.config);
        auto it = offCategories_.find(k);
        if (it == offCategories_.end()) {
            size_t off = 0;
            std::string err;
            for (Il2CppClass* c = k; c; c = api().il2cpp_class_get_parent(c))
                if (auto o = Resolver::Get().FieldOffset(c, "categories", &err)) {
                    off = *o;
                    break;
                }
            it = offCategories_.emplace(k, off).first;
        }
        if (!it->second) return false;
        cats = unity::Field<void*>(inj.config, it->second);
    }
    Il2CppObject* bestList = nullptr;
    Il2CppObject* tpl = nullptr;
    int bestScore = -1;
    std::map<int32_t, Il2CppObject*> vanilla;  // vanilla upgrade -> its menu item (icon source)
    for (size_t ci = 0, cn = unity::ListCount(cats); ci < cn && ci < 32; ++ci) {
        auto* cat = static_cast<Il2CppObject*>(unity::ListAt(cats, ci));
        if (!cat) continue;
        Il2CppClass* k = api().il2cpp_object_get_class(cat);
        auto it = offItems_.find(k);
        if (it == offItems_.end()) {
            size_t off = 0;
            std::string err;
            for (Il2CppClass* c = k; c; c = api().il2cpp_class_get_parent(c))
                if (auto o = Resolver::Get().FieldOffset(c, "items", &err)) {
                    off = *o;
                    break;
                }
            it = offItems_.emplace(k, off).first;
        }
        if (!it->second) continue;
        auto* items = unity::Field<Il2CppObject*>(cat, it->second);
        for (size_t i = 0, n = unity::ListCount(items); i < n && i < 64; ++i) {
            auto* item = static_cast<Il2CppObject*>(unity::ListAt(items, i));
            if (!IsA(item, upgItemClass_) || !unity::IsAlive(item)) continue;
            const int32_t u = unity::Field<int32_t>(item, offItemUpgrade_);
            if (custom_upgrades::IsCustomUpgrade(u)) continue;
            vanilla.emplace(u, item);
            void* costs = unity::Field<void*>(item, offItemCosts_);
            const size_t nc = unity::ListCount(costs);
            int score = (nc == 1 ? 2 : nc > 1 ? 1 : 0) + (u == kUpgStorageCapacity ? 4 : 0);
            if (nc == 0) continue;
            if (score > bestScore) {
                bestScore = score;
                tpl = item;
                bestList = items;
            }
        }
    }
    if (!tpl || !bestList) {
        SR2_INFO("[{}] plot type {}: menu has no upgrade item to clone - no custom upgrades there", kModule, plotType);
        return false;
    }
    for (const UpgradeDef& d : kDefs) {
        if (d.plotType != plotType) continue;
        Injected in;
        in.def = &d;
        Il2CppObject* iconSrc = nullptr;
        if (d.iconFrom) {
            auto vit = vanilla.find(d.iconFrom);
            if (vit != vanilla.end()) iconSrc = vit->second;
            else if ((iconSrc = GlobalUpgradeItem(d.iconFrom)) != nullptr)
                SR2_INFO("[{}] {}: icon taken from '{}' (another plot's menu)", kModule, d.title, unity::GetName(iconSrc));
            else SR2_INFO("[{}] {}: vanilla upgrade {} not in this menu - generic icon", kModule, d.title, d.iconFrom);
        }
        in.item = CloneItem(tpl, d, iconSrc, in.costEntry);
        if (!in.item) return false;
        inj.items.push_back(in);
    }
    inj.list = bestList;
    unity::NewGCHandle(bestList);
    SR2_INFO("[{}] plot type {}: {} custom item(s) cloned from '{}' (upgrade {})", kModule, plotType, inj.items.size(),
             unity::GetName(tpl), unity::Field<int32_t>(tpl, offItemUpgrade_));
    return !inj.items.empty();
}

Il2CppObject* CustomUpgrades::GlobalUpgradeItem(int32_t upgrade) {
    // One-time scan (first menu build that needs an icon from another plot's menu, e.g. the corral
    // Air Net icon for the coop): every loaded PlotUpgradePurchaseItemModel asset.
    if (!globalUpgItemsTried_) {
        globalUpgItemsTried_ = true;
        for (Il2CppObject* item : unity::FindObjectsOfTypeAll(upgItemClass_)) {
            if (!unity::IsAlive(item) || !IsA(item, upgItemClass_)) continue;
            const int32_t u = unity::Field<int32_t>(item, offItemUpgrade_);
            if (custom_upgrades::IsCustomUpgrade(u) || unity::GetName(item).rfind("sr2mods_", 0) == 0) continue;
            if (!unity::IsAlive(unity::Field<Il2CppObject*>(item, offItemIcon_))) continue;
            if (globalUpgItems_.emplace(u, item).second) unity::NewGCHandle(item);  // pinned (a few, never freed)
        }
    }
    auto it = globalUpgItems_.find(upgrade);
    return it != globalUpgItems_.end() && unity::IsAlive(it->second) ? it->second : nullptr;
}

MenuInjection* CustomUpgrades::InjectionFor(Il2CppObject* config) {
    for (auto& p : injections_)
        if (p->config == config) return p.get();
    auto inj = std::make_unique<MenuInjection>();
    inj->config = config;
    unity::NewGCHandle(config);
    injections_.push_back(std::move(inj));
    return injections_.back().get();
}

void CustomUpgrades::SetTexts(const Injected& in, const std::string& title, const std::string& desc,
                              const std::string& prompt) {
    text_override::Set(unity::Field<Il2CppObject*>(in.item, offItemTitle_), title);
    text_override::Set(unity::Field<Il2CppObject*>(in.item, offItemDesc_), desc);
    text_override::Set(unity::Field<Il2CppObject*>(in.item, offItemPrompt_), prompt);
}

void CustomUpgrades::BeforeMenuUpdate(Il2CppObject* root) {
    if (!main_thread::IsMainThread() || !IsA(root, rootClass_) || !unity::IsAlive(root)) return;
    TrackMenuRoot(root);  // the plot menu the details panel will bind for (slider)
    Il2CppObject* plot = unity::Field<Il2CppObject*>(root, offActivator_);
    Il2CppObject* config = unity::Field<Il2CppObject*>(root, offMenuConfig_);
    if (!unity::IsAlive(plot) || !IsA(plot, plotClass_) || !unity::IsAlive(config)) return;
    const int32_t type = PlotType(plot);

    bool anyDef = false;
    for (const UpgradeDef& d : kDefs) anyDef |= d.plotType == type;
    MenuInjection* inj = nullptr;
    for (auto& p : injections_)
        if (p->config == config) inj = p.get();
    if (!anyDef && !inj) return;  // nothing of ours for this plot type / menu

    const bool sink = text_override::SinkActive();
    if (!sink && !loggedSink_) {
        loggedSink_ = true;
        SR2_WARN("[{}] menu text overrides unavailable (UI options-tab hooks not active) - custom upgrades hidden", kModule);
    }
    if (!inj) {
        if (!sink) return;
        inj = InjectionFor(config);
    }
    if (inj->items.empty() && !inj->failed && sink) {
        if (!BuildInjection(*inj, type)) inj->failed = true;
    }
    if (inj->failed) return;
    cleaned_ = false;
    for (Injected& in : inj->items) {
        const bool want = sink && Visible(plot, *in.def);
        bool has = false;
        if (!ListCall(inj->list, "Contains", in.item, &has)) continue;
        if (want && !has) ListCall(inj->list, "Add", in.item);
        else if (!want && has) {
            bool removed = false;
            ListCall(inj->list, "Remove", in.item, &removed);
        }
        in.inList = want;
        if (in.costEntry) unity::Field<int32_t>(in.costEntry, offCostAmount_) = Price(*in.def);
    }
}

void CustomUpgrades::AfterMenuUpdate(Il2CppObject* root) {
    if (!main_thread::IsMainThread() || !IsA(root, rootClass_) || !unity::IsAlive(root)) return;
    Il2CppObject* plot = unity::Field<Il2CppObject*>(root, offActivator_);
    Il2CppObject* config = unity::Field<Il2CppObject*>(root, offMenuConfig_);
    if (!unity::IsAlive(plot) || !IsA(plot, plotClass_)) return;
    MenuInjection* inj = nullptr;
    for (auto& p : injections_)
        if (p->config == config) inj = p.get();
    if (!inj || inj->failed) return;
    for (const Injected& in : inj->items) {
        if (!in.inList) continue;
        const UpgradeDef& d = *in.def;
        if (d.id == SILO_SELL_ALL_NOW) {
            const SiloSummary s = Summarize(plot, false, nullptr);
            std::string desc, prompt;
            if (!s.marketAvailable) {
                desc = "The Plort Market can't be reached right now.";
                prompt = desc;
            } else if (s.marketClosed) {
                desc = std::format("{} plort{} stored, but the Plort Market is closed. Come back later!", s.plorts,
                                   s.plorts == 1 ? "" : "s");
                prompt = "The Plort Market is closed right now. Nothing will be sold.";
            } else if (s.plorts == 0) {
                desc = "No plorts in this silo yet. Food and slimes are never sold.";
                prompt = "There are no plorts in this silo. Nothing will be sold.";
            } else {
                desc = std::format("{} plort{} ({} kind{}) worth about {} Newbucks today. Food and slimes stay put.",
                                   s.plorts, s.plorts == 1 ? "" : "s", s.types, s.types == 1 ? "" : "s", s.value);
                prompt = std::format("Sell all {} plort{} in this silo for about {} Newbucks?\nMarket prices drop as "
                                     "the market saturates.",
                                     s.plorts, s.plorts == 1 ? "" : "s", s.value);
            }
            SetTexts(in, d.title, desc, prompt);
        } else if (d.id == COOP_HEN_COLLECTOR) {
            // This coop's keep-limit (default until bought / set).
            int keep = custom_upgrades::world::kHenKeepDefault;
            const std::string id = PlotId(plot);
            if (auto it = state_.plots.find(id); !id.empty() && it != state_.plots.end())
                keep = ClampHenKeep(it->second.henKeep);
            SetTexts(in, d.title, HenDescription(keep), d.prompt);
        } else {
            SetTexts(in, d.title, d.description, d.prompt);
        }
    }
}

void CustomUpgrades::Cleanup(const char* why) {
    if (cleaned_) return;
    cleaned_ = true;
    slider_.Destroy();  // main thread only (no-op otherwise / without game calls after a fault)
    if (world_.Ready() && main_thread::IsMainThread()) {
        // Hen Collector contents go into the sidecar state first (written with the next save
        // while the module is on again), then the clones are removed.
        const custom_upgrades::world::QueryFn query = [&](Il2CppObject* plot, custom_upgrades::world::CoopRequest& req) {
            return state_.loaded && WorldQuery(plot, req);
        };
        world_.RemoveAll(&query);
        SR2_INFO("[{}] {}: coop world objects removed", kModule, why);
    }
    if (silo_.Ready() && main_thread::IsMainThread()) {
        silo_.RemoveAll();
        SR2_INFO("[{}] {}: silo sell buttons removed", kModule, why);
    }
    int removed = 0;
    for (auto& inj : injections_) {
        for (Injected& in : inj->items) {
            // Lock the price first: an already open menu must not be able to buy the item
            // through vanilla AddUpgrade (our detour may be gone).
            if (in.costEntry) unity::Field<int32_t>(in.costEntry, offCostAmount_) = INT_MAX;
            if (inj->list) {
                bool r = false;
                if (ListCall(inj->list, "Remove", in.item, &r) && r) ++removed;
            }
            in.inList = false;
        }
    }
    if (removed) SR2_INFO("[{}] {}: {} custom menu item(s) removed", kModule, why, removed);
}

void CustomUpgrades::FrameCheck(float dt) {
    if (sr2::core::ModuleFaulted(kModule)) {
        if (!cleaned_) Cleanup("module faulted");
        return;
    }
    if (On() && slider_.Active()) g_sliderGuard.Run([&] { SliderTick(dt); });
    if (On() && silo_.Ready()) g_siloGuard.Run([&] { silo_.Frame(); });  // button message (cheap when none)
    if (!On() || (!world_.Ready() && !silo_.Ready())) return;
    if (std::isfinite(dt) && dt > 0.0f) worldAccum_ += std::min(dt, 1.0f);
    if (worldAccum_ < kWorldTickSeconds) return;
    worldAccum_ = 0.0f;
    g_worldGuard.Run([&] { WorldTick(); });
}

Il2CppObject* CustomUpgrades::LookupDirector() {
    bool ok = false;
    Il2CppObject* gc = Invoke(gameCtxInstMi_, nullptr, {}, "GameContext.Instance", &ok);
    Il2CppObject* lookup = (ok && unity::IsAlive(gc)) ? unity::Field<Il2CppObject*>(gc, offGcLookup_) : nullptr;
    return unity::IsAlive(lookup) ? lookup : nullptr;
}

bool CustomUpgrades::WorldQuery(Il2CppObject* plot, custom_upgrades::world::CoopRequest& req) {
    if (!unity::IsAlive(plot) || !IsA(plot, plotClass_) || PlotType(plot) != kPlotCoop) return false;
    std::string id;
    const UpgradeDef* net = FindDef(COOP_AIR_NET);
    const UpgradeDef* hen = FindDef(COOP_HEN_COLLECTOR);
    req.airNet = net && OwnsKey(plot, *net, &id);
    req.henCollector = hen && OwnsKey(plot, *hen, &id);
    if (id.empty()) return false;  // no save game / no LandPlotLocation id (yet): leave the plot alone
    req.id = id;
    auto it = state_.plots.find(id);
    req.hens = (req.henCollector && it != state_.plots.end()) ? &it->second.hens : nullptr;
    if (req.henCollector && it != state_.plots.end()) req.keepAdults = ClampHenKeep(it->second.henKeep);
    return true;
}

void CustomUpgrades::WorldTick() {
    if (!main_thread::IsMainThread() || !EnsureLoaded()) return;  // no save game: nothing to build
    cleaned_ = false;
    if (world_.Ready())
        world_.Tick([&](Il2CppObject* plot, custom_upgrades::world::CoopRequest& req) { return WorldQuery(plot, req); },
                    [&]() { return LookupDirector(); });
    if (silo_.Ready() && !sr2::core::ModuleFaulted(kModule)) {
        // Silos that own the Sell-All Button (the button checks the plot type itself).
        const UpgradeDef* d = FindDef(SILO_SELL_ALL);
        std::vector<std::string> wanted;
        for (const auto& [id, ps] : state_.plots)
            if (d && ps.type == kPlotSilo && ps.owned.count(d->key)) wanted.push_back(id);
        silo_.Tick(wanted, [&]() { return LookupDirector(); });
    }
}

// ---------------------------------------------------------------- silo button --

SiloSummary CustomUpgrades::SellAllNow(Il2CppObject* plot, const std::string& id, const char* via) {
    std::string log;
    const SiloSummary s = Summarize(plot, true, &log);
    if (!s.marketAvailable) SR2_INFO("[{}] sell all ({}, plot '{}'): Plort Market not available", kModule, via, id);
    else if (s.marketClosed) SR2_INFO("[{}] sell all ({}, plot '{}'): Plort Market closed, nothing sold", kModule, via, id);
    else SR2_INFO("[{}] sell all ({}, plot '{}'): sold {} plort(s) for {} Newbucks{}{}", kModule, via, id, s.plorts, s.value,
                  log.empty() ? "" : ": ", log);
    return s;
}

bool CustomUpgrades::OnSiloButton(Il2CppObject* activator) {
    if (!main_thread::IsMainThread() || !silo_.IsOurs(activator)) return false;
    if (!On()) return true;  // swallowed (the button is removed with the next frame)
    Il2CppObject* plot = silo_.OwnerPlot(activator);
    if (!plot || !silo_.Press(activator)) return true;  // no silo / pressed again within 0.4 s
    const UpgradeDef* d = FindDef(SILO_SELL_ALL);
    std::string id;
    if (!d || !OwnsKey(plot, *d, &id)) {
        silo_.Message(activator, "This silo has no Sell-All Button", false);
        worldAccum_ = kWorldTickSeconds;  // remove the stale button
        return true;
    }
    const SiloSummary s = SellAllNow(plot, id, "world button");
    if (!s.marketAvailable) silo_.Message(activator, "The Plort Market can't be reached right now", false);
    else if (s.marketClosed) silo_.Message(activator, "The Plort Market is closed - nothing sold", false);
    else if (s.plorts == 0) silo_.Message(activator, "No plorts in this silo", false);
    else  // the game shows its own currency notification (TrySell showUi)
        silo_.Message(activator, std::format("Sold {} plort{} for {} Newbucks", s.plorts, s.plorts == 1 ? "" : "s", s.value),
                      true);
    return true;
}

// ---------------------------------------------------------------- limit slider --

void CustomUpgrades::TrackMenuRoot(Il2CppObject* root) {
    if (root == menuRoot_ && menuRootHandle_) return;
    Il2CppGCHandle h = unity::NewGCHandle(root);
    if (!h) return;
    Il2CppGCHandle old = menuRootHandle_;
    menuRootHandle_ = h;
    menuRoot_ = root;
    if (old) main_thread::Post([old] { unity::FreeGCHandle(old); });  // never freed inside a hook
}

void CustomUpgrades::AfterDetailsBind(Il2CppObject* view, Il2CppObject* vm) {
    if (!main_thread::IsMainThread() || !slider_.Ready()) return;
    slider_.Destroy();  // every bind (selection change, RebuildUI) replaces the slider
    if (!vm || !offVmDescription_ || !menuRoot_ || !unity::IsAlive(menuRoot_) || !IsA(menuRoot_, rootClass_)) return;
    Il2CppObject* desc = unity::Field<Il2CppObject*>(vm, offVmDescription_);
    if (!desc) return;
    Il2CppObject* plot = unity::Field<Il2CppObject*>(menuRoot_, offActivator_);
    Il2CppObject* config = unity::Field<Il2CppObject*>(menuRoot_, offMenuConfig_);
    if (!unity::IsAlive(plot) || !IsA(plot, plotClass_) || PlotType(plot) != kPlotCoop) return;
    // Our Hen Collector item of this menu: its own LocalizedString instance (pointer identity).
    bool ours = false;
    for (auto& inj : injections_) {
        if (inj->config != config || inj->failed) continue;
        for (const Injected& in : inj->items)
            if (in.def->id == COOP_HEN_COLLECTOR && in.inList && unity::IsAlive(in.item))
                ours |= unity::Field<Il2CppObject*>(in.item, offItemDesc_) == desc || in.item == desc;
    }
    if (!ours) return;
    const UpgradeDef* d = FindDef(COOP_HEN_COLLECTOR);
    std::string id;
    if (!d || !OwnsKey(plot, *d, &id)) return;  // not bought (yet): no slider
    auto it = state_.plots.find(id);
    if (it == state_.plots.end()) return;

    custom_upgrades::ui::SliderSpec spec;
    spec.name = "HenCollectorLimit";
    spec.min = custom_upgrades::world::kHenKeepMin;
    spec.max = custom_upgrades::world::kHenKeepMax;
    spec.step = 1;
    spec.value = ClampHenKeep(it->second.henKeep);
    spec.label = [](int v) { return std::format("Collect above: {} adult{}", v, v == 1 ? "" : "s"); };
    // By key (never a pointer into the map): the state may be reloaded while the menu is open.
    spec.onChange = [this, id, game = state_.game](int v) {
        if (!state_.loaded || state_.game != game) return;
        auto p = state_.plots.find(id);
        if (p == state_.plots.end() || !p->second.owned.count("coop_hen_collector")) return;
        p->second.henKeep = ClampHenKeep(v);
        SR2_DEBUG("[{}] hen collector limit on plot '{}': {} (saved with the next game save)", kModule, id, v);
    };
    slider_.Create(view, spec);
}

void CustomUpgrades::SliderTick(float dt) {
    if (!main_thread::IsMainThread()) return;
    slider_.Tick(dt);
}

// ---------------------------------------------------------------- buying --

void CustomUpgrades::OnAddUpgrade(Il2CppObject* plot, int32_t upg) {
    if (!main_thread::IsMainThread() || !unity::IsAlive(plot) || !IsA(plot, plotClass_)) return;
    const UpgradeDef* d = FindDef(upg);
    if (!d) {
        SR2_WARN("[{}] unknown custom upgrade 0x{:X} - ignored", kModule, upg);
        return;
    }
    if (PlotType(plot) != d->plotType || !EnsureLoaded()) {
        SR2_WARN("[{}] {}: wrong plot type {} or no save game - ignored", kModule, d->title, PlotType(plot));
        return;
    }
    const std::string id = PlotId(plot);
    if (id.empty()) {
        SR2_WARN("[{}] {}: plot has no LandPlotLocation id - ignored", kModule, d->title);
        return;
    }
    if (d->action) {
        if (d->id == SILO_SELL_ALL_NOW) {
            const UpgradeDef* req = FindDef(d->needs);
            if (!req || !OwnsKey(plot, *req)) {
                SR2_WARN("[{}] sell-all on plot '{}' without the Sell-All Button - ignored", kModule, id);
                return;
            }
            SellAllNow(plot, id, "menu");
        }
        return;
    }
    PlotState& ps = state_.plots[id];
    ps.type = d->plotType;
    ps.owned.insert(d->key);
    Bump();
    if (IsWorldUpgrade(d->id) || d->id == SILO_SELL_ALL) worldAccum_ = kWorldTickSeconds;  // build it at the next frame
    SR2_INFO("[{}] bought {} on plot '{}' (saved with the next game save)", kModule, d->title, id);
}

// ---------------------------------------------------------------- sell all --

Il2CppObject* CustomUpgrades::EconDirector() {
    bool ok = false;
    Il2CppObject* sc = Invoke(sceneCtxInstMi_, nullptr, {}, "SceneContext.Instance", &ok);
    if (!ok || !unity::IsAlive(sc)) return nullptr;
    Il2CppObject* econ = unity::Field<Il2CppObject*>(sc, offScEcon_);
    return unity::IsAlive(econ) ? econ : nullptr;
}

SiloSummary CustomUpgrades::Summarize(Il2CppObject* plot, bool sell, std::string* log) {
    SiloSummary s;
    Il2CppObject* econ = EconDirector();
    if (!econ) return s;
    s.marketAvailable = true;
    bool ok = false;
    s.marketClosed = Unbox<bool>(Invoke(shutdownMi_, econ, {}, "PlortEconomyDirector.IsMarketShutdown", &ok));
    if (!ok) {
        s.marketAvailable = false;
        return s;
    }
    Il2CppArray* storages = getComponentsInChildren_(plot, unity::TypeOf(siloStorageClass_), true);
    std::vector<Il2CppObject*> seen;
    std::set<Il2CppObject*> types;
    for (size_t si = 0, sn = unity::ArrayLength(storages); si < sn && si < 16; ++si) {
        auto* st = static_cast<Il2CppObject*>(unity::ArrayAt(storages, si));
        if (!unity::IsAlive(st) || !IsA(st, siloStorageClass_) || (linkedSiloClass_ && IsA(st, linkedSiloClass_)))
            continue;
        Il2CppObject* ammo = Invoke(localAmmoMi_, st, {}, "SiloStorage.get_LocalAmmo", &ok);
        if (!ok || !ammo || std::find(seen.begin(), seen.end(), ammo) != seen.end()) continue;
        seen.push_back(ammo);
        Il2CppObject* slots = Invoke(slotsMi_, ammo, {}, "AmmoSlotManager.get_Slots", &ok);
        const size_t n = ok ? unity::ArrayLength(slots) : 0;
        for (int32_t i = 0; i < static_cast<int32_t>(n) && i < 64; ++i) {
            Il2CppObject* id = Invoke(slotNameMi_, ammo, {&i}, "AmmoSlotManager.GetSlotName", &ok);
            if (!ok || !unity::IsAlive(id) || !IsA(id, identClass_)) continue;
            if (!unity::Field<bool>(id, offIdentIsPlort_)) continue;  // plorts only
            int32_t count = Unbox<int32_t>(Invoke(slotCountMi_, ammo, {&i}, "AmmoSlotManager.GetSlotCount", &ok));
            if (!ok || count <= 0) continue;
            int32_t cur = 0, delta = 0;
            const bool hasValue =
                Unbox<bool>(Invoke(marketValuesMi_, econ, {id, &cur, &delta}, "TryGetMarketValues", &ok));
            if (!ok || !hasValue) continue;  // not sellable at the market
            if (!sell) {
                s.plorts += count;
                s.value += static_cast<long long>(cur) * count;
                types.insert(id);
                continue;
            }
            if (s.marketClosed) continue;
            bool ignoreShutdown = false, showUi = true;
            const bool sold = Unbox<bool>(Invoke(trySellMi_, econ, {id, &count, &ignoreShutdown, &showUi, nullptr},
                                                 "PlortEconomyDirector.TrySell", &ok));
            if (!ok || !sold) continue;
            // Paid: now take the plorts out of the slot.
            Invoke(decrementMi_, ammo, {&i, &count}, "AmmoSlotManager.Decrement", &ok);
            if (!ok) SR2_WARN("[{}] sold {} x {} but could not empty the silo slot", kModule, count, unity::GetName(id));
            s.plorts += count;
            s.value += static_cast<long long>(cur) * count;
            types.insert(id);
            if (log) *log += std::format("{}{} x {} @ {}", log->empty() ? "" : ", ", count, unity::GetName(id), cur);
        }
    }
    s.types = static_cast<int>(types.size());
    return s;
}

// ---------------------------------------------------------------- saving --

void CustomUpgrades::BeforeSave() {
    capturing_ = true;
    capturedSave_.clear();
    // Read the sidecar of the loaded file now if nothing asked yet (SaveGameImpl is about to
    // bump the save number the stale-file check compares with).
    if (main_thread::IsMainThread()) EnsureLoaded();
}

void CustomUpgrades::AfterSave(Il2CppObject* result) {
    const std::string written = capturedSave_;
    capturing_ = false;
    capturedSave_.clear();
    if (!result || !main_thread::IsMainThread()) return;  // save skipped (scene loading, disabled...)
    if (!EnsureLoaded()) return;
    if (world_.Ready())  // live Hen Collector contents into the state (the tick copy may be ~1 s old)
        g_worldGuard.Run([&] {
            world_.Snapshot([&](Il2CppObject* plot, custom_upgrades::world::CoopRequest& req) { return WorldQuery(plot, req); });
        });
    std::string file = written;
    if (!file.empty() && file.rfind(state_.game + "_", 0) != 0) {
        SR2_WARN("[{}] saved file '{}' does not belong to game '{}' - writing the per-game sidecar", kModule, file,
                 state_.game);
        file.clear();
    }
    if (file.empty() && !loggedNoCapture_) {
        loggedNoCapture_ = true;
        SR2_WARN("[{}] the saved .sav file name was not seen - custom upgrades go to the per-game sidecar {}", kModule,
                 LegacyPath(state_.game).filename().string());
    }
    std::string game;
    unsigned long long number = 0;
    if (!file.empty()) CurrentMetadata(AutoSave(), game, number);  // already incremented: the number of this file
    if (!WriteSidecar(file, number)) return;
    if (!file.empty()) {
        state_.save = file;
        PruneSidecars(state_.game, file);
    }
    SR2_DEBUG("[{}] sidecar written for '{}' file '{}' (save #{}, {} plot(s))", kModule, state_.game, file, number,
              state_.plots.size());
}

// ---------------------------------------------------------------- api --

std::string CustomUpgrades::CanSellCustom(Il2CppObject* plot, int32_t upg) {
    const UpgradeDef* d = FindDef(upg);
    if (!d || d->action) return "This can't be sold.";
    std::string id;
    if (!OwnsKey(plot, *d, &id)) return "This upgrade is not owned.";
    if (d->id == COOP_HEN_COLLECTOR) {
        // Stored chickens are never thrown away: the player vacs them out first.
        int stored = world_.Ready() ? world_.StoredHens(plot) : -1;
        if (stored < 0) {
            auto it = state_.plots.find(id);
            stored = it != state_.plots.end() ? HenCount(it->second) : 0;
        }
        if (stored > 0)
            return std::format("Empty the Hen Collector first: {} chicken{} stored. Vac them out of its ports.", stored,
                               stored == 1 ? " is" : "s are");
    }
    // Tiers: an owned upgrade that needs this one must be sold first.
    for (const UpgradeDef& o : kDefs)
        if (!o.action && o.needs == d->id && OwnsKey(plot, o))
            return std::format("Sell '{}' first - it needs this upgrade.", o.title);
    return {};
}

bool CustomUpgrades::RemoveCustom(Il2CppObject* plot, int32_t upg, std::string& notes) {
    const UpgradeDef* d = FindDef(upg);
    std::string id;
    if (!d || !OwnsKey(plot, *d, &id)) return false;
    auto it = state_.plots.find(id);
    if (it == state_.plots.end()) return false;
    it->second.owned.erase(d->key);
    if (d->id == COOP_HEN_COLLECTOR) {
        it->second.hens.clear();  // CanSell: it was empty
        it->second.henKeep = custom_upgrades::world::kHenKeepDefault;
    }
    if (it->second.Empty()) state_.plots.erase(it);
    Bump();
    if (IsWorldUpgrade(d->id) && world_.Ready())
        world_.Remove(plot, d->id == COOP_AIR_NET, d->id == COOP_HEN_COLLECTOR);
    if (d->id == SILO_SELL_ALL && silo_.Ready()) silo_.Remove(plot);
    notes += "custom upgrade removed (saved with the next game save)";
    SR2_INFO("[{}] {} removed from plot '{}'", kModule, d->title, id);
    return true;
}

std::string CustomUpgrades::VanillaDependentOf(Il2CppObject* plot, int32_t vanillaUpg) {
    if (custom_upgrades::IsCustomUpgrade(vanillaUpg) || !vanillaUpg) return {};
    for (const UpgradeDef& o : kDefs)
        if (!o.action && o.needs == vanillaUpg && OwnsKey(plot, o)) return o.title;
    return {};
}

// ---------------------------------------------------------------- per-save values --

std::string CustomUpgrades::PlotValueOf(Il2CppObject* plot, const std::string& key) {
    if (key.empty() || !unity::IsAlive(plot) || !IsA(plot, plotClass_) || !EnsureLoaded()) return {};
    const std::string id = PlotId(plot);
    auto it = id.empty() ? state_.plots.end() : state_.plots.find(id);
    if (it == state_.plots.end() || it->second.type != PlotType(plot)) return {};  // other plot type: not this plot's
    auto v = it->second.values.find(key);
    return v != it->second.values.end() ? v->second : std::string();
}

bool CustomUpgrades::SetPlotValueOf(Il2CppObject* plot, const std::string& key, const std::string& value) {
    if (key.empty() || !unity::IsAlive(plot) || !IsA(plot, plotClass_) || !EnsureLoaded()) return false;
    const std::string id = PlotId(plot);
    if (id.empty()) return false;
    const int32_t type = PlotType(plot);
    auto it = state_.plots.find(id);
    if (it != state_.plots.end() && it->second.type != type) {
        // Rebuilt as another plot type: everything stored for the old plot is gone.
        state_.plots.erase(it);
        Bump();
        it = state_.plots.end();
    }
    if (value.empty()) {
        if (it != state_.plots.end()) {
            it->second.values.erase(key);
            if (it->second.Empty()) state_.plots.erase(it);
        }
        return true;
    }
    PlotState& ps = state_.plots[id];
    ps.type = type;
    ps.values[key] = value;
    return true;
}

std::string CustomUpgrades::SaveValueOf(const std::string& key) {
    if (key.empty() || !EnsureLoaded()) return {};
    auto it = state_.values.find(key);
    return it != state_.values.end() ? it->second : std::string();
}

bool CustomUpgrades::SetSaveValueOf(const std::string& key, const std::string& value) {
    if (key.empty() || !EnsureLoaded()) return false;
    if (value.empty()) state_.values.erase(key);
    else state_.values[key] = value;
    return true;
}

// ---------------------------------------------------------------- tiers --

float CustomUpgrades::TierMultiplier(Il2CppObject* obj, int family) {
    if (family < 0 || family >= kFamilies || !obj || !main_thread::IsMainThread()) return 1.0f;
    const unsigned long long tick = main_thread::TickCount();
    auto it = tierCache_.find(obj);
    if (it != tierCache_.end() && it->second.version == version_ && tick - it->second.tick < kTierCacheFrames)
        return it->second.mult[family];

    // Slow path (once per object per version / ~15 s): resolve the plot and its owned tiers.
    TierEntry e;
    e.version = version_;
    e.tick = tick;
    for (float& m : e.mult) m = 1.0f;
    if (unity::IsAlive(obj)) {
        Il2CppObject* plot = IsA(obj, plotClass_) ? obj : getComponentInParent_(obj, unity::TypeOf(plotClass_), true);
        if (unity::IsAlive(plot) && IsA(plot, plotClass_)) {
            const int32_t type = PlotType(plot);
            bool any = false;
            for (const UpgradeDef& d : kDefs) any |= (d.family != kNoFamily && d.plotType == type);
            if (any && EnsureLoaded()) {
                const std::string id = PlotId(plot);
                auto ps = id.empty() ? state_.plots.end() : state_.plots.find(id);
                if (ps != state_.plots.end() && ps->second.type == type) {
                    for (const UpgradeDef& d : kDefs)
                        if (d.family != kNoFamily && d.plotType == type && ps->second.owned.count(d.key))
                            e.mult[d.family] = std::max(e.mult[d.family], d.mult);
                }
            }
        }
    }
    if (tierCache_.size() > 4096) tierCache_.clear();  // bounded (stale keys of destroyed objects)
    tierCache_[obj] = e;
    return e.mult[family];
}

// ---------------------------------------------------------------- setup --

bool CustomUpgrades::Setup(sr2::core::ModuleContext& ctx) {
    Resolver& R = ctx.resolver();
    std::string why;
    auto parentOf = [](Il2CppClass* c) { return api().il2cpp_class_get_parent ? api().il2cpp_class_get_parent(c) : nullptr; };
    auto cls = [&](const char* image, const char* ns, const char* name) -> Il2CppClass* {
        std::string err;
        Il2CppClass* k = R.Class(image, ns, name, &err);
        if (!k && why.empty()) why = err.empty() ? std::format("class {}.{} not found", ns, name) : err;
        return k;
    };
    auto off = [&](Il2CppClass* k, const char* field, size_t& out) -> bool {
        std::string err;
        for (Il2CppClass* c = k; c; c = parentOf(c))
            if (auto o = R.FieldOffset(c, field, &err)) {
                out = *o;
                return true;
            }
        if (why.empty()) why = std::format("field {} not found", field);
        return false;
    };
    auto method = [&](Il2CppClass* k, const char* name, int argc, bool required = true) -> const MethodInfo* {
        std::string err;
        for (Il2CppClass* c = k; c; c = parentOf(c))
            if (const MethodInfo* m = R.Method(c, name, argc, &err)) return m;
        if (required && why.empty()) why = std::format("method {}({}) not found", name, argc);
        return nullptr;
    };
    auto bind = [&](auto& b, const MethodInfo* m) -> bool {
        if (!m) return false;
        std::string err;
        void* p = R.MethodPointer(m, &err);
        if (!p) {
            if (why.empty()) why = "no native body: " + err;
            return false;
        }
        using B = std::remove_reference_t<decltype(b)>;
        b.fn = reinterpret_cast<typename B::Fn>(p);
        b.mi = m;
        return true;
    };
    auto byParams = [&](Il2CppClass* k, const char* name, std::initializer_list<std::string_view> params) {
        std::string err;
        const MethodInfo* m = R.MethodByParams(k, name, params, &err);
        if (!m && why.empty()) why = std::format("{}: {}", name, err);
        return m;
    };

    wbarrier_ = Export<WBarrierFn>("il2cpp_gc_wbarrier_set_field");
    if (!api().il2cpp_class_get_parent) return ctx.Fail("il2cpp_class_get_parent export missing");

    plotClass_ = cls(kImage, "", "LandPlot");
    rootClass_ = cls(kImage, kPlotUiNs, "LandPlotUIRoot");
    upgItemClass_ = cls(kImage, kPlotUiNs, "PlotUpgradePurchaseItemModel");
    costEntryClass_ = cls(kImage, "MonomiPark.SlimeRancher.Economy", "CurrencyCostEntry");
    Il2CppClass* purchaseCost = cls(kImage, "MonomiPark.SlimeRancher.Economy", "PurchaseCost");
    locationClass_ = cls(kImage, "", "LandPlotLocation");
    siloStorageClass_ = cls(kImage, "", "SiloStorage");
    identClass_ = cls(kImage, "", "IdentifiableType");
    Il2CppClass* gameCtx = cls(kImage, "", "GameContext");
    Il2CppClass* sceneCtx = cls(kImage, "", "SceneContext");
    Il2CppClass* autoSave = cls(kImage, "MonomiPark.SlimeRancher", "AutoSaveDirector");
    Il2CppClass* econ = cls(kImage, "MonomiPark.SlimeRancher.Economy", "PlortEconomyDirector");
    Il2CppClass* ammoMgr = cls(kImage, "MonomiPark.SlimeRancher.Player", "AmmoSlotManager");
    lsClass_ = cls("Unity.Localization", "UnityEngine.Localization", "LocalizedString");
    Il2CppClass* component = cls(kCore, "UnityEngine", "Component");
    Il2CppClass* transform = cls(kCore, "UnityEngine", "Transform");
    Il2CppClass* uobject = cls(kCore, "UnityEngine", "Object");
    if (!why.empty()) return ctx.Fail(why);
    autoSaveClass_ = autoSave;
    linkedSiloClass_ = R.Class(kImage, "", "LinkedSiloStorage");  // optional (filter only)

    size_t offCostField = 0, offCurrencyList = 0;
    off(plotClass_, "TypeId", offPlotTypeId_);
    off(rootClass_, "Activator", offActivator_);
    off(rootClass_, "menuConfig", offMenuConfig_);
    off(upgItemClass_, "_upgrade", offItemUpgrade_);
    off(upgItemClass_, "_title", offItemTitle_);
    off(upgItemClass_, "_description", offItemDesc_);
    off(upgItemClass_, "_promptMessage", offItemPrompt_);
    off(upgItemClass_, "_icon", offItemIcon_);
    off(upgItemClass_, "_unavailableIcon", offItemUnavailIcon_);
    off(upgItemClass_, "_purchaseCost", offCostField);
    off(purchaseCost, "_currencyCosts", offCurrencyList);
    if (offCurrencyList >= 2 * sizeof(void*)) offCurrencyList -= 2 * sizeof(void*);  // value-type field
    offItemCosts_ = offCostField + offCurrencyList;
    off(costEntryClass_, "_currency", offCostCurrency_);
    off(costEntryClass_, "_amount", offCostAmount_);
    off(locationClass_, "_id", offLocationId_);
    off(identClass_, "IsPlort", offIdentIsPlort_);
    off(identClass_, "icon", offIdentIcon_);
    off(gameCtx, "<AutoSaveDirector>k__BackingField", offGcAutoSave_);
    off(gameCtx, "<LookupDirector>k__BackingField", offGcLookup_);
    off(sceneCtx, "<PlortEconomyDirector>k__BackingField", offScEcon_);
    if (!why.empty()) return ctx.Fail(why);

    const MethodInfo* hasMi = method(plotClass_, "HasUpgrade", 1);
    hasMi_ = hasMi;
    const MethodInfo* addMi = method(plotClass_, "AddUpgrade", 1);
    const MethodInfo* menuMi = R.Method(rootClass_, "UpdateMenuItemAvailabilityAndHidden", 0);  // the override itself
    if (!menuMi && why.empty()) why = "LandPlotUIRoot.UpdateMenuItemAvailabilityAndHidden not found";
    const MethodInfo* saveMi = method(autoSave, "SaveGameImpl", 2);
    const MethodInfo* loadMi = method(autoSave, "Load", 3);
    const MethodInfo* newGameMi = method(autoSave, "StartNewGame", 2);
    gameCtxInstMi_ = method(parentOf(gameCtx), "get_Instance", 0);
    sceneCtxInstMi_ = method(parentOf(sceneCtx), "get_Instance", 0);
    curGameNameMi_ = method(autoSave, "CurrentSaveGameName", 0);
    trySellMi_ = method(econ, "TrySell", 5);
    marketValuesMi_ = method(econ, "TryGetMarketValues", 3);
    shutdownMi_ = method(econ, "IsMarketShutdown", 0);
    localAmmoMi_ = method(siloStorageClass_, "get_LocalAmmo", 0);
    slotsMi_ = method(ammoMgr, "get_Slots", 0);
    slotNameMi_ = method(ammoMgr, "GetSlotName", 1);
    slotCountMi_ = method(ammoMgr, "GetSlotCount", 1);
    decrementMi_ = byParams(ammoMgr, "Decrement", {"System.Int32", "System.Int32"});
    setHideFlagsMi_ = method(uobject, "set_hideFlags", 1, false);
    bind(getTransform_, method(component, "get_transform", 0));
    bind(getParent_, method(transform, "get_parent", 0));
    bind(getComponent_, byParams(component, "GetComponent", {"System.Type"}));
    bind(getComponentsInChildren_, byParams(component, "GetComponentsInChildren", {"System.Type", "System.Boolean"}));
    bind(getComponentInParent_, byParams(component, "GetComponentInParent", {"System.Type", "System.Boolean"}));
    if (!why.empty()) return ctx.Fail(why);
    {
        Il2CppClass* lookup = R.Class(kImage, "", "LookupDirector");
        std::string err;
        marketLinkMi_ = lookup ? R.Method(lookup, "get_MarketLink", 0, &err) : nullptr;  // optional icon
    }

    {
        std::string werr;
        if (!world_.Setup(R, kModule, werr))
            SR2_WARN("[{}] coop world objects unavailable ({}) - Coop Air Net and Hen Collector are not offered", kModule, werr);
    }

    // Hen Collector limit slider (optional: without it the limit stays at its saved / default value).
    const MethodInfo* detailsBindMi = nullptr;
    if (world_.Ready()) {
        std::string serr;
        Il2CppClass* view = R.Class(kImage, "MonomiPark.SlimeRancher.UI.Purchase", "PurchaseItemDetailsContentView", &serr);
        Il2CppClass* vm = R.Class(kImage, "MonomiPark.SlimeRancher.UI.Purchase", "PurchaseItemDetailsContentViewModel", &serr);
        auto o = vm ? R.FieldOffset(vm, "_description", &serr) : std::nullopt;
        detailsBindMi = view ? R.Method(view, "BindData", 1, &serr) : nullptr;  // the class's own override
        if (o && detailsBindMi && slider_.Setup(R, kModule, serr)) {
            offVmDescription_ = *o;
        } else {
            detailsBindMi = nullptr;
            SR2_WARN("[{}] Hen Collector limit slider unavailable ({}) - the limit stays at its saved value", kModule,
                     serr.empty() ? "PurchaseItemDetailsContentView.BindData / _description not found" : serr);
        }
    }

    struct H {
        const char* name;
        const MethodInfo* mi;
        void* detour;
        void** orig;
    };
    const H hooks[] = {
        {"LandPlot.HasUpgrade", hasMi, reinterpret_cast<void*>(&HasUpgradeDetour), reinterpret_cast<void**>(&g_origHas)},
        {"LandPlot.AddUpgrade", addMi, reinterpret_cast<void*>(&AddUpgradeDetour), reinterpret_cast<void**>(&g_origAdd)},
        {"LandPlotUIRoot.UpdateMenuItemAvailabilityAndHidden", menuMi, reinterpret_cast<void*>(&MenuUpdateDetour),
         reinterpret_cast<void**>(&g_origMenu)},
        {"AutoSaveDirector.SaveGameImpl", saveMi, reinterpret_cast<void*>(&SaveImplDetour),
         reinterpret_cast<void**>(&g_origSave)},
        {"AutoSaveDirector.Load", loadMi, reinterpret_cast<void*>(&LoadDetour), reinterpret_cast<void**>(&g_origLoad)},
        {"AutoSaveDirector.StartNewGame", newGameMi, reinterpret_cast<void*>(&NewGameDetour),
         reinterpret_cast<void**>(&g_origNewGame)},
    };
    std::string report;
    for (const H& h : hooks) {
        std::string err;
        void* p = R.MethodPointer(h.mi, &err);
        if (!p) return ctx.Fail(std::format("{}: no native body ({})", h.name, err));
        if (!ctx.HookAddress(h.name, p, h.detour, h.orig)) return ctx.Fail(std::format("hook {} failed", h.name));
        report += std::format(" {}@0x{:X}", h.name, Resolver::Rva(p));
    }
    if (detailsBindMi) {
        std::string err;
        void* p = R.MethodPointer(detailsBindMi, &err);
        if (p && ctx.HookAddress("PurchaseItemDetailsContentView.BindData", p, reinterpret_cast<void*>(&DetailsBindDetour),
                                 reinterpret_cast<void**>(&g_origDetailsBind)))
            report += std::format(" PurchaseItemDetailsContentView.BindData@0x{:X}", Resolver::Rva(p));
        else SR2_WARN("[{}] hook PurchaseItemDetailsContentView.BindData failed ({}) - no limit slider", kModule, err);
    }

    // Silo Sell-All world button (optional; missing -> only the "Sell All Plorts Now" menu action).
    {
        std::string serr;
        Il2CppClass* act = R.Class(kImage, "", "PlortCollectorActivator", &serr);
        auto collector = act ? R.FieldOffset(act, "Collector", &serr) : std::nullopt;
        const MethodInfo* activateMi = act ? R.Method(act, "Activate", 0, &serr) : nullptr;
        void* p = activateMi ? R.MethodPointer(activateMi, &serr) : nullptr;
        bool ok = collector && p && silo_.Setup(R, kModule, serr);
        if (ok) {
            g_offActCollector = *collector;
            ok = ctx.HookAddress("PlortCollectorActivator.Activate", p, reinterpret_cast<void*>(&ActivateDetour),
                                 reinterpret_cast<void**>(&g_origActivate));
            if (ok) report += std::format(" PlortCollectorActivator.Activate@0x{:X}", Resolver::Rva(p));
            else serr = "hook failed";
        }
        if (!ok) {
            silo_ = custom_upgrades::world::SiloButtons{};  // not Ready(): no buttons are built
            g_offActCollector = 0;
            SR2_WARN("[{}] silo sell button unavailable ({}) - the Sell-All Button keeps only its menu action", kModule,
                     serr);
        }
    }

    // The exact .sav file (optional; missing -> one sidecar per game as before): SaveGameImpl
    // passes "{GameName}_{i}" to StorageProvider.StoreGameData (FileStorageProvider on Steam and
    // standalone: <persistentDataPath>/Steam/<id>/<name>.sav), Load gets GameSaveIdentifier
    // {GameName, SaveName}; the save number comes from AutoSaveDirector._currentGameMetadata.
    {
        std::string serr;
        auto opt = [&](Il2CppClass* k, const char* f, size_t& o) -> bool {
            if (!k) return false;
            auto r = R.FieldOffset(k, f, &serr);
            if (r) o = *r;
            return r.has_value();
        };
        Il2CppClass* meta = R.Class(kImage, "MonomiPark.SlimeRancher", "GameMetadata", &serr);
        saveIdOk_ = opt(autoSave, "_storageProvider", offAsdProvider_) &&
                    opt(autoSave, "_currentGameMetadata", offAsdMetadata_) && opt(meta, "GameName", offMetaGameName_) &&
                    opt(meta, "SaveNumber", offMetaSaveNumber_) && offMetaGameName_ >= 2 * sizeof(void*) &&
                    offMetaSaveNumber_ >= 2 * sizeof(void*);
        if (saveIdOk_) {  // value-type field offsets include the object header
            offMetaGameName_ -= 2 * sizeof(void*);
            offMetaSaveNumber_ -= 2 * sizeof(void*);
        } else {
            offAsdProvider_ = 0;
            SR2_WARN("[{}] save metadata fields not found ({}) - no save-number check for per-save sidecars", kModule,
                     serr);
        }
        fileProviderClass_ = R.Class(kImage, "", "FileStorageProvider", &serr);
        if (!opt(fileProviderClass_, "savePath", offProviderSavePath_)) fileProviderClass_ = nullptr;
        const MethodInfo* storeMi = fileProviderClass_ ? R.Method(fileProviderClass_, "StoreGameData", 3, &serr) : nullptr;
        void* p = storeMi ? R.MethodPointer(storeMi, &serr) : nullptr;
        if (p && ctx.HookAddress("FileStorageProvider.StoreGameData", p, reinterpret_cast<void*>(&StoreGameDetour),
                                 reinterpret_cast<void**>(&g_origStore)))
            report += std::format(" FileStorageProvider.StoreGameData@0x{:X}", Resolver::Rva(p));
        else
            SR2_WARN("[{}] FileStorageProvider.StoreGameData not hooked ({}) - one sidecar per game instead of per save "
                     "file", kModule, serr);
    }

    settings::Def d;
    d.module = kModule;
    d.key = kKeyPrice;
    d.label = "Silo Sell-All Button price";
    d.kind = settings::Kind::Int;
    d.def = 1000;
    d.min = 0;
    d.max = 100000;
    d.step = 50;
    d.tooltip =
        "Newbucks price of the silo 'Sell-All Button' custom upgrade (bought in the silo's own purchase menu). Once "
        "bought, the silo menu shows 'Sell All Plorts Now'. Ownership is stored next to version.dll in "
        "sr2mods_saves\\<save file>.sav.json (one per save file), never in the game's save.";
    settings::Register(d);

    main_thread::AddFrameCallback(kModule, [](float dt) {
        if (g_self) g_self->FrameCheck(dt);
    });

    g_self = this;
    SR2_INFO("[{}] setup ok:{}; item._upgrade +0x{:X}, costs +0x{:X}, menuConfig +0x{:X}; sidecars in {}", kModule, report,
             offItemUpgrade_, offItemCosts_, offMenuConfig_, (sr2::paths::SelfDir() / "sr2mods_saves").string());
    return true;
}

}  // namespace

std::unique_ptr<sr2::core::Module> CreateCustomUpgrades() { return std::make_unique<CustomUpgrades>(); }

namespace custom_upgrades {

bool Active() { return g_self && g_self->On(); }

std::string Name(int32_t upgrade) {
    const UpgradeDef* d = FindDef(upgrade);
    return d ? d->title : "custom upgrade";
}

std::string CanSell(Il2CppObject* landPlot, int32_t upgrade) {
    if (!Active() || !main_thread::IsMainThread()) return "Custom upgrades are not available right now.";
    std::string r;
    if (!g_hasGuard.Run([&] { r = g_self->CanSellCustom(landPlot, upgrade); })) return "Custom upgrades failed.";
    return r;
}

bool Remove(Il2CppObject* landPlot, int32_t upgrade, std::string& notes) {
    if (!Active() || !main_thread::IsMainThread()) return false;
    bool ok = false;
    g_addGuard.Run([&] { ok = g_self->RemoveCustom(landPlot, upgrade, notes); });
    return ok;
}

std::string VanillaDependent(Il2CppObject* landPlot, int32_t vanillaUpgrade) {
    if (!Active() || !main_thread::IsMainThread()) return {};
    std::string r;
    g_hasGuard.Run([&] { r = g_self->VanillaDependentOf(landPlot, vanillaUpgrade); });
    return r;
}

float Multiplier(Il2CppObject* plotOrComponent, Family family) {
    if (!Active()) return 1.0f;
    float m = 1.0f;
    if (!g_tierGuard.Run([&] { m = g_self->TierMultiplier(plotOrComponent, static_cast<int>(family)); })) return 1.0f;
    return std::isfinite(m) && m >= 1.0f && m <= 10.0f ? m : 1.0f;
}

std::string PlotValue(Il2CppObject* landPlot, const std::string& key) {
    if (!Active() || !main_thread::IsMainThread()) return {};
    std::string r;
    g_hasGuard.Run([&] { r = g_self->PlotValueOf(landPlot, key); });
    return r;
}

bool SetPlotValue(Il2CppObject* landPlot, const std::string& key, const std::string& value) {
    if (!Active() || !main_thread::IsMainThread()) return false;
    bool ok = false;
    g_addGuard.Run([&] { ok = g_self->SetPlotValueOf(landPlot, key, value); });
    return ok;
}

std::string SaveValue(const std::string& key) {
    if (!Active() || !main_thread::IsMainThread()) return {};
    std::string r;
    g_hasGuard.Run([&] { r = g_self->SaveValueOf(key); });
    return r;
}

bool SetSaveValue(const std::string& key, const std::string& value) {
    if (!Active() || !main_thread::IsMainThread()) return false;
    bool ok = false;
    g_addGuard.Run([&] { ok = g_self->SetSaveValueOf(key, value); });
    return ok;
}

}  // namespace custom_upgrades

}  // namespace sr2::modules
