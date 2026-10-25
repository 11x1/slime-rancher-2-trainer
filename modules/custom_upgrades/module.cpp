// custom_upgrades: framework for NEW land-plot upgrades bought in the game's own plot
// purchase menu, with save-safe sidecar persistence (SR2 1.3.0). Upgrades: the silo
// "Sell-All Button" (sell every plort stored in that silo at the current Plort Market
// price, market saturation applied by the game; a press button on the silo, cloned from the
// corral collector button - silo_world.cpp) and the per-plot upgrade TIERS whose
// effects plot_upgrades applies (api.h Multiplier):
//
//   tier                 plot         needs                 price   effect
//   Music Box II        corral       Music Box             2000    2x calming bonus
//   (not a tier: Corral Radio, corral, needs Music Box, 1500: the corral plays its own Slime DJ
//   playlist instead of the global queue; slime_dj reads ownership through api.h Owns and keeps
//   the playlist as a per-plot value "slime_dj.radio")
//   Faster Hens I        coop         -                     2000    hens lay 1.5x as often
//   Faster Hens II       coop         Faster Hens I         4000    hens lay 2x as often
//   Sprinkler II         garden       Sprinkler             2000    crops grow 1.5x as fast
//   Sprinkler III        garden       Sprinkler II          4000    crops grow 2x as fast
//   (save ids garden_growth_1 / _2: the tiers were called Fast Growth I / II until round 8)
//   Ash Booster          incinerator  Ash Trough            1500    2x ash per incinerated food
//   Silo Capacity II     silo         Storage Capacity      3000    300 items per silo slot
//   Silo Capacity III    silo         Silo Capacity II      4500    400 items per silo slot
//   Silo Capacity IV     silo         Silo Capacity III     6500    500 items per silo slot
//   Silo Capacity V      silo         Silo Capacity IV      9000    600 items per silo slot
//
//   Not plot upgrades: the Fabricator vacpack upgrades Tank Booster IX-XII (125 / 150 / 175 / 200
//   per slot, more levels of the vanilla Tank Booster card), Extra Tank III-IV (a 7th / 8th slot,
//   more levels of the vanilla Extra Tank card) and Drone Battery Saver I-IV (own card; every drone
//   station uses 1/1.25 .. 1/2 of the battery; effect in drone_battery.cpp) and Shadow Catcher (own
//   card; the vacpack holds shadow slimes, they stay and eat on the ranch; effect in
//   shadow_catcher.cpp) and Drone Program Scheduler (own card; unlocks several programs per drone
//   station, drone_programs.cpp), in vacpack_upgrades.cpp
//   (own hooks, levels and extra slot contents in this module's sidecar as per-save values).
//
//   Silo capacity (vanilla, prefab + disassembly): the silo's SiloStorage has 12 slot definitions
//   ("SiloNonSlimes", _maxAmount 100); STORAGE2/3/4 only swap slot GameObjects, the one vanilla
//   capacity upgrade STORAGE_CAPACITY_INCREASE (1800) runs StorageUpgrader.Apply ->
//   AmmoSlotManager.UpdateMaxCount(StorageCapacityUpgradeAmount = 200) -> every AmmoSlot's
//   MaxCount. So vanilla is 100 -> 200 per slot; our four tiers continue the chain on the same
//   card to 300 / 400 / 500 / 600. The three vanilla "Additional Storage" cards (STORAGE2 / 3 / 4,
//   one more storage section each) are merged into one card the same way (MergeStorageCards). The max count is runtime only (AmmoDataV02 saves id, count,
//   emotions, appearance, radiant - no max), load restores counts without clamping them
//   (AmmoModel.Push -> AmmoSlot.set_Count), and AmmoSlotManager.ValidateAndAdjustSlots never
//   trims counts. So the ~1 Hz world tick re-applies the owned tier's capacity (vanilla
//   ApplyUpgrades writes 200 again on load and after every purchase on the plot), and taking a
//   tier away (module off / fault) only lowers the max: contents above it stay in the silo,
//   which takes no more until it is below. Like the vanilla silo storage upgrades they cannot be
//   sold (sell_upgrades shows no Sell button for them).
//
//   Each chain has ONE menu card (Shown): the first tier not owned yet, or the last one once
//   all are owned (greyed, checkmark). Faster Hens I -> II and Sprinkler II -> III share a
//   card; a tier on a vanilla upgrade (Music Box II, Sprinkler II / III, Ash Booster) takes over that
//   upgrade's card once it is owned (the vanilla item leaves the list while the tier card stands at its
//   index, and comes back for every other plot / when the module stops). A card that shows a tier not owned yet gets Buy (the
//   vanilla Purchase button) and, next to it, sell_upgrades' Sell button for the tier below
//   (api.h ChainSellTarget). Prerequisites: custom `needs` or a vanilla upgrade via the
//   original LandPlot.HasUpgrade. Selling is refused while a higher tier - or, for a vanilla
//   prerequisite, any tier built on it - is still owned. Only the highest owned tier of a
//   family counts (they do not stack).
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
//                                                adult hens / roosters: vacuums the hens and the
//                                                roosters over the coop's two limits (defaults
//                                                18 + 2) every 6 game hours into 2 x 100 slots;
//                                                chicks never count (deluxe: 1300)
//
//   Hen Collector limit sliders (plot_menu_slider.cpp, reusable): while the owned Hen Collector is
//   selected in the coop's purchase menu, two sliders side by side, "Keep hens: N" (0..58, default
//   18) and "Keep roostros: N" (0..20, default 2), sit in the details panel where the price row is
//   for unowned items. Mouse only for now (no pad / keyboard steps: the arrows / d-pad navigate the
//   menu as usual; gamepad support comes later). Built from a PurchaseItemDetailsContentView.BindData detour (post:
//   our item = the view model's _description is our clone's LocalizedString instance), replaced on
//   every bind (selection change, RebuildUI after a buy / sell), removed with the menu. The values
//   are per coop in the sidecar ("hen_collector_keep_hens" / "_roosters"; an old single
//   "hen_collector_keep" n reads as min(2, n) roosters + the rest hens), saved with the next game
//   save like ownership, and read live by the collector (CoopRequest.keepHens / keepRoosters).
//
//   Auto-Feeder "Leave last food item in" (feeder_lock.cpp, round 16): while the owned vanilla Auto-Feeder
//   card is selected in a corral's purchase menu, an on/off switch there (mouse, or Submit = pad A /
//   Enter) keeps the feeder's last food item in, so it keeps that food and drones / shots add only more of
//   it. Per plot value "custom_upgrades.feeder_lock" in the sidecar (non-empty = on).
//   Auto-Feeder fill lights (feeder_lights.cpp, round 17): the feeder's port LEDs red / yellow / green by
//   fill (setting FeederFillLights, world tick part 13).
//
//   Hen Collector save safety: its storage is a LOCAL AmmoSlotManager (never linked to the
//   plot's LandPlotModel, so never in the vanilla save; the vanilla elder slots would not
//   accept hens on a mod-free load anyway); its contents are kept in the sidecar
//   ("hen_collector": [{type, count}] per slot), copied from the live storage every tick and
//   before every save. Selling is refused while it holds chickens (vac them out first).
//
//   Tier props (plot_props.cpp): owned tiers show on their plot as pure decoration cloned from
//   vanilla models - Faster Hens I / II: two small hay bales (decorHayBales), II adds two nests
//   (the coop's own), a flower pillow and a stuffed chicken toy; Sprinkler II / III: a third
//   (III: and a fourth) sprinkler head (garden prefab); Ash Booster: glow + embers in the incinerator's fire. Built /
//   removed by the same ~1 Hz world tick from the sidecar ownership.
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
//   Receipt (silo_receipt.cpp): the world button first quotes the silo (one line per plort type:
//   count, current price) and shows it in a game-styled dialog that locks input like the game's
//   menus (UIInputLocker: time paused, UI action map, cursor). Sell re-quotes: an identical quote is
//   sold with the quote as the plan (at most the shown count per type); a different one (prices,
//   contents, market closed) refreshes the receipt and nothing is sold. Cancel / Esc / B: nothing.
//
// ---------------------------------------------------------------- gate --------------
//   The module is off by default: no custom item in any purchase menu until the switch is
//   turned on. Once it is on with a save loaded, that save is marked (per-save value
//   "custom_upgrades.on", written with the next game save) and the switch is pinned on
//   (settings::Lock, the overlay shows it locked with kLockReason) while the save stays
//   loaded. A save whose sidecar has the mark or any owned custom upgrade counts the same.
//   Setup also runs while the module is off (SetupWhileDisabled, hooks stay disabled), so
//   the frame callback can watch the loaded save (GateTick, 2 Hz: SceneContext.GameModel
//   + AutoSaveDirector._currentGameMetadata, one read-only sidecar lookup per new save):
//   loading such a save with the switch off turns the module on (posted settings::Set) and
//   remembers that; leaving that save again (title screen, or a save without custom
//   upgrades) turns it back off. Purchases made in an unsaved session vanish with it, and
//   so does the mark: a save never saved with the module on is not locked.
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
#include "core/perf.h"
#include "core/settings.h"
#include "core/text_override.h"
#include "core/unity.h"
#include "modules/custom_upgrades/api.h"
#include "modules/custom_upgrades/coop_world.h"
#include "modules/custom_upgrades/drone_battery.h"
#include "modules/custom_upgrades/drone_programs.h"
#include "modules/custom_upgrades/drone_source.h"
#include "modules/custom_upgrades/plot_menu_slider.h"
#include "modules/custom_upgrades/plot_props.h"
#include "modules/custom_upgrades/gadget_look.h"
#include "modules/custom_upgrades/quantum_link.h"
#include "modules/custom_upgrades/dj_buildables.h"
#include "modules/custom_upgrades/radio_shop.h"
#include "modules/custom_upgrades/sale_notify.h"
#include "modules/custom_upgrades/silo_receipt.h"
#include "modules/custom_upgrades/silo_world.h"
#include "modules/custom_upgrades/feeder_lights.h"
#include "modules/custom_upgrades/feeder_lock.h"
#include "modules/custom_upgrades/shadow_catcher.h"
#include "modules/custom_upgrades/slime_launcher.h"
#include "modules/custom_upgrades/vacpack_upgrades.h"
#include "modules/slime_dj/api.h"
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
constexpr int kSidecarFormat = 2;  // 1 = one file per game (legacy), 2 = one file per .sav
constexpr const char* kRotationSuffix = ".sav.json";  // sidecar of <save>.sav = sr2mods_saves/<save>.sav.json
// ReadSidecarFile's error for a sidecar that holds only another module's data (sandbox "cheated" mark).
constexpr const char* kForeignSidecar = "no custom upgrade data in it";
// Gate: per-save value "the module was on with this save loaded" (see the Gate section above).
constexpr const char* kOnKey = "custom_upgrades.on";
constexpr float kGateTickSeconds = 0.5f;
constexpr const char* kLockReason =
    "Locked on: this save uses custom upgrades. Load a save without them, or go to the title screen, to turn it off.";
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
constexpr int32_t kUpgAshTrough = 14;
constexpr int32_t kUpgDeluxeCoop = 17;
constexpr int32_t kUpgStorageCapacity = 20;  // template preference
// STORAGE2 / STORAGE3 / STORAGE4: the silo's "Additional Storage" cards (one more section each).
constexpr int32_t kUpgStorageSections[] = {3, 4, 5};
constexpr unsigned long long kTierCacheFrames = 900;
constexpr float kWorldTickSeconds = 1.0f;  // coop world objects: create / fit / remove
constexpr int kWorldSteps = 14;            // WorldStep parts, one per frame (round 13: all in one frame took 2-3.5 ms)

using custom_upgrades::Family;
constexpr int kFamilies = static_cast<int>(Family::kCount);
constexpr int kNoFamily = -1;
// Tier chains that are not a multiplier family of api.h (no Multiplier() entry): the silo capacity
// chain. `mult` is the capacity per slot there.
constexpr int kSiloCapFamily = kFamilies;
constexpr int kSiloSlotDefault = 100;  // vanilla AmmoSlotDefinition "SiloNonSlimes" _maxAmount (fallback)

// ---- custom upgrade definitions -------------------------------------------------------
enum CustomId : int32_t {
    SILO_SELL_ALL = custom_upgrades::kIdBase + 0x0001,  // owned upgrade (silo world button)
    SILO_CAPACITY_2 = custom_upgrades::kIdBase + 0x0060,  // silo capacity tiers (300 / 400 / 500 / 600 per slot)
    SILO_CAPACITY_3 = custom_upgrades::kIdBase + 0x0061,
    SILO_CAPACITY_4 = custom_upgrades::kIdBase + 0x0062,
    SILO_CAPACITY_5 = custom_upgrades::kIdBase + 0x0063,
    CORRAL_MUSIC_2 = custom_upgrades::kIdBase + 0x0013,
    CORRAL_RADIO = custom_upgrades::kCorralRadio,  // 0x0014: own Slime DJ playlist (modules/slime_dj)
    COOP_HENS_1 = custom_upgrades::kIdBase + 0x0020,
    COOP_HENS_2 = custom_upgrades::kIdBase + 0x0021,
    GARDEN_GROWTH_1 = custom_upgrades::kIdBase + 0x0030,
    GARDEN_GROWTH_2 = custom_upgrades::kIdBase + 0x0031,
    INCIN_ASH_BOOST = custom_upgrades::kIdBase + 0x0040,
    COOP_AIR_NET = custom_upgrades::kIdBase + 0x0050,        // world object (coop_world.cpp)
    COOP_HEN_COLLECTOR = custom_upgrades::kIdBase + 0x0051,  // world object (coop_world.cpp)
};
// Retired (never reuse these ids or sidecar keys): 0x0002 "silo_sell_all_now" (the menu action),
// 0x0010 "corral_collector_2", 0x0011 "corral_collector_3", 0x0012 "corral_feeder_2". A sidecar
// that still lists one loads fine: the key is ignored (logged once per plot, no refund) and is
// gone from that save's sidecar after the next game save.
const char* const kRetiredKeys[] = {"silo_sell_all_now", "corral_collector_2", "corral_collector_3",
                                    "corral_feeder_2"};

// Upgrades that are a world object (only offered while the world-object code is ready).
bool IsWorldUpgrade(int32_t id) { return id == COOP_AIR_NET || id == COOP_HEN_COLLECTOR; }

struct UpgradeDef {
    int32_t id;
    const char* key;     // sidecar key (stable)
    int32_t plotType;    // LandPlot.Id
    int32_t needs;       // listed only while this upgrade is owned: custom id, vanilla id or 0
    int price;           // Newbucks
    int family;          // Family (kNoFamily = not a tier)
    float mult;          // tier multiplier
    int32_t iconFrom;    // vanilla upgrade whose menu icon is reused (0 = Market Link icon)
    int32_t after;       // vanilla upgrade whose menu entry this one follows (0 = end of the list)
    const char* title;
    const char* description;
    const char* prompt;
};

constexpr int F(Family f) { return static_cast<int>(f); }

const UpgradeDef kDefs[] = {
    {SILO_SELL_ALL, "silo_sell_all", kPlotSilo, 0, 10000, kNoFamily, 1.0f, 0, 0, "Sell-All Button",
     "A big red button for this silo that sells every plort inside at today's prices. Food and slimes stay put.",
     "Buy the Sell-All Button for this silo?"},
    // ---- silo capacity chain on the vanilla Storage Capacity card (100 -> 200 vanilla) ----
    {SILO_CAPACITY_2, "silo_capacity_2", kPlotSilo, kUpgStorageCapacity, 3000, kSiloCapFamily, 300.0f,
     kUpgStorageCapacity, 0, "Silo Capacity II",
     "Bigger tanks: every slot of this silo holds 300 items instead of 200. Like all silo storage, it can't be sold.",
     "Buy Silo Capacity II for this silo?"},
    {SILO_CAPACITY_3, "silo_capacity_3", kPlotSilo, SILO_CAPACITY_2, 4500, kSiloCapFamily, 400.0f,
     kUpgStorageCapacity, 0, "Silo Capacity III",
     "Even bigger tanks: every slot of this silo holds 400 items. Like all silo storage, it can't be sold.",
     "Buy Silo Capacity III for this silo?"},
    {SILO_CAPACITY_4, "silo_capacity_4", kPlotSilo, SILO_CAPACITY_3, 6500, kSiloCapFamily, 500.0f,
     kUpgStorageCapacity, 0, "Silo Capacity IV",
     "Huge tanks: every slot of this silo holds 500 items. Like all silo storage, it can't be sold.",
     "Buy Silo Capacity IV for this silo?"},
    {SILO_CAPACITY_5, "silo_capacity_5", kPlotSilo, SILO_CAPACITY_4, 9000, kSiloCapFamily, 600.0f,
     kUpgStorageCapacity, 0, "Silo Capacity V",
     "The biggest tanks around: every slot of this silo holds 600 items. Like all silo storage, it can't be sold.",
     "Buy Silo Capacity V for this silo?"},
    // ---- tiers (one menu card per chain, see Shown) ----
    {CORRAL_MUSIC_2, "corral_music_2", kPlotCorral, kUpgMusicBox, 2000, F(Family::kMusicBox), 2.0f, kUpgMusicBox, 0,
     "Music Box II", "A catchier tune that calms this corral's slimes twice as well.",
     "Buy Music Box II for this corral?"},
    // ---- Slime DJ (no effect of its own here: slime_dj asks Owns(plot, kCorralRadio)) ----
    {CORRAL_RADIO, "corral_radio", kPlotCorral, kUpgMusicBox, 1500, kNoFamily, 1.0f, kUpgMusicBox, 0, "Corral Radio",
     "Tunes this corral's music box to its own station. Press the blue button at the corral's corner to pick its "
     "songs (or use the mod menu: Ranch > Slime DJ > Corral radios).",
     "Buy the Corral Radio for this corral?"},
    {COOP_HENS_1, "coop_hens_1", kPlotCoop, 0, 2000, F(Family::kCoop), 1.5f, kUpgDeluxeCoop, 0, "Faster Hens I",
     "Cozier nests help this coop's hens lay eggs 1.5x as fast.", "Buy Faster Hens I for this coop?"},
    {COOP_HENS_2, "coop_hens_2", kPlotCoop, COOP_HENS_1, 4000, F(Family::kCoop), 2.0f, kUpgDeluxeCoop, 0,
     "Faster Hens II", "The coziest nests around: hens lay eggs twice as fast. Replaces Faster Hens I.",
     "Buy Faster Hens II for this coop?"},
    // Tiers of the vanilla Sprinkler card (round 8: "not extending sprinkler I like I wanted to"): needs the
    // Sprinkler, so ChainVanillaRoot puts the whole chain on its card (Sprinkler -> II -> III).
    {GARDEN_GROWTH_1, "garden_growth_1", kPlotGarden, kUpgSprinkler, 2000, F(Family::kGarden), 1.5f, kUpgSprinkler, 0,
     "Sprinkler II",
     "Far, Far Range ranchers swear by a third sprinkler head. Well-watered crops grow 1.5x as fast!",
     "Buy Sprinkler II for this garden?"},
    {GARDEN_GROWTH_2, "garden_growth_2", kPlotGarden, GARDEN_GROWTH_1, 4000, F(Family::kGarden), 2.0f, kUpgSprinkler,
     0, "Sprinkler III",
     "A fourth head soaks every last row. Ogden would approve: crops grow twice as fast! Replaces Sprinkler II.",
     "Buy Sprinkler III for this garden?"},
    {INCIN_ASH_BOOST, "incinerator_ash_boost", kPlotIncinerator, kUpgAshTrough, 1500, F(Family::kAsh), 2.0f,
     kUpgAshTrough, 0, "Ash Booster",
     "Squeezes twice the ash out of everything you burn. Fire slimes approve!",
     "Buy the Ash Booster for this incinerator?"},
    // ---- world objects ----
    {COOP_AIR_NET, "coop_air_net", kPlotCoop, 0, 425, kNoFamily, 1.0f, kUpgAirNet, 0, "Coop Air Net",
     "An air net for your coop. Launched chickens stay in, and nothing drops in from above.",
     "Buy the Coop Air Net for this coop?"},
    {COOP_HEN_COLLECTOR, "coop_hen_collector", kPlotCoop, 0, 1000, kNoFamily, 1.0f, kUpgDeluxeCoop, 0,
     "Hen Collector",
     "A collector for your coop that gathers spare grown chickens every few hours and stores them for you or your "
     "drones to pick up.",
     "Buy the Hen Collector for this coop?"},
};

const UpgradeDef* FindDef(int32_t id) {
    for (const UpgradeDef& d : kDefs)
        if (d.id == id) return &d;
    return nullptr;
}

// The vanilla upgrade a tier chain starts on (Music Box for Music Box II, Storage Capacity for every
// Silo Capacity tier), or 0. Every tier of such a chain shows on that upgrade's card.
int32_t ChainVanillaRoot(const UpgradeDef& d) {
    const UpgradeDef* t = &d;
    for (int guard = 0; t && t->family != kNoFamily && guard < 16; ++guard) {
        if (!t->needs) return 0;
        if (!custom_upgrades::IsCustomUpgrade(t->needs)) return t->needs;
        const UpgradeDef* r = FindDef(t->needs);
        if (!r || r->family != d.family) return 0;
        t = r;
    }
    return 0;
}

bool IsSiloCapacity(const UpgradeDef& d) { return d.family == kSiloCapFamily; }

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
    int henKeepHens = custom_upgrades::world::kHenKeepHensDefault;          // Hen Collector keep-limits (sliders)
    int henKeepRoosters = custom_upgrades::world::kHenKeepRoostersDefault;
    std::map<std::string, std::string> values;          // api.h SetPlotValue ("<module>.<name>")
    bool Empty() const { return owned.empty() && values.empty(); }
};

int ClampKeepHens(int64_t v) {
    return static_cast<int>(
        std::clamp<int64_t>(v, custom_upgrades::world::kHenKeepHensMin, custom_upgrades::world::kHenKeepHensMax));
}
int ClampKeepRoosters(int64_t v) {
    return static_cast<int>(
        std::clamp<int64_t>(v, custom_upgrades::world::kHenKeepRoostersMin, custom_upgrades::world::kHenKeepRoostersMax));
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
    Il2CppObject* base = nullptr;      // tier on a vanilla upgrade: that upgrade's item, whose card it takes over
    Il2CppObject* anchor = nullptr;    // vanilla item of def->after (listed right after it)
    bool inList = false;
};
// A vanilla item taken out of the list while a tier card stands in its place.
struct HiddenVanilla {
    Il2CppObject* item = nullptr;  // GC handle held (Injected::base)
    int32_t index = 0;             // its index in the vanilla list
};
struct MenuInjection {
    Il2CppObject* config = nullptr;    // MenuConfiguration (GC handle held)
    Il2CppObject* list = nullptr;      // the category's items list (GC handle held)
    std::vector<Injected> items;
    std::vector<HiddenVanilla> hidden;
    std::vector<Il2CppObject*> storageTitles;  // vanilla "Additional Storage" _title objects we relabel
    std::string storageReport;                 // last logged Additional Storage card state (logged on change)
    bool failed = false;               // clone/verification failed: never retried for this config
};

struct SiloSummary {
    int plorts = 0;        // sellable plort count
    long long value = 0;   // at current market values
    int types = 0;
    bool marketClosed = false;
    bool marketAvailable = false;
    std::vector<std::pair<Il2CppObject*, int>> sold;  // sale only: IdentifiableType -> count sold
    bool noticed = false;  // per-type HUD notices queued (SaleNotices) - no text feedback needed
};

using HasUpgradeFn = bool (*)(Il2CppObject*, int32_t, const MethodInfo*);
HasUpgradeFn g_origHas = nullptr;  // original LandPlot.HasUpgrade (vanilla ids only)

class CustomUpgrades final : public sr2::core::Module {
public:
    const char* Name() const override { return kModule; }
    bool DefaultEnabled() const override { return false; }  // gate: nothing in the menus until switched on
    bool SetupWhileDisabled() const override { return true; }  // GateTick watches the loaded save while off
    bool Setup(sr2::core::ModuleContext& ctx) override;
    void OnEnable() override {
        // The Load / NewGame hooks were off while disabled: the loaded save may be another one
        // than the in-memory state follows. Re-read on the next main-thread EnsureLoaded.
        resync_.store(true);
        enabled_.store(true);
        cleaned_ = false;
        drones_.ResetSeen();
        programs_.ResetSeen();
    }
    void OnDisable() override {
        enabled_.store(false);
        // Before the hooks go off (the Pull detour keeps our source type out of the save): every
        // program with the Hen Collector source says Coop again. Field writes only; Cleanup gives
        // the running drones the Coop program on the main thread.
        programs_.Revert(false, "custom upgrades off");  // first: program 1 with its source already vanilla
        drones_.Revert(false, "custom upgrades off");
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
    void RegisterTexts(Il2CppObject* plot, const MenuInjection& inj);
    bool IsPlotMenu(Il2CppObject* root) const { return IsA(root, rootClass_); }
    void BeforeSave();
    void AfterSave(Il2CppObject* result);
    void OnStoreGameData(Il2CppObject* provider, Il2CppString* fileName);
    void OnLoad(Il2CppObject* asd, const void* identifier);
    void AfterLoad(Il2CppObject* asd);
    void OnNewGame();
    bool BeforePlotReplaced(Il2CppObject* location, Il2CppObject* oldPlot, std::string& id, int32_t& oldType);
    void OnPlotReplaced(const std::string& id, int32_t oldType, Il2CppObject* newObj);
    void ResetState(const char* why);
    void FrameCheck(float dt);
    void GateTick();
    bool WorldStep(int step);  // one part of the ~1 Hz world tick; false: that part is not there (no frame spent)
    bool OnSiloButton(Il2CppObject* activator);
    bool OnHenButton(Il2CppObject* activator);
    bool OnRadioButton(Il2CppObject* activator);
    Il2CppObject* CardIcon(int32_t id);
    void KeepLoaded(Il2CppObject* asset);
    bool Notify(Il2CppObject* icon, const std::string& text);
    SiloSummary SellAllNow(Il2CppObject* plot, const std::string& id, const char* via, bool queueNotices,
                           const custom_upgrades::ui::Receipt* plan = nullptr);
    bool OpenReceipt(Il2CppObject* plot, Il2CppObject* activator, const std::string& id,
                     const custom_upgrades::ui::Receipt& quote, const char* note = nullptr);
    void ReceiptTick(float dt);
    void CloseReceipt(const char* why);
    bool WorldQuery(Il2CppObject* plot, custom_upgrades::world::CoopRequest& req);
    void AfterDetailsBind(Il2CppObject* view, Il2CppObject* vm);
    void SliderTick(float dt);
    bool SliderActive() const { return sliderHens_.Active() || sliderRoosters_.Active() || sliderFeeder_.Active(); }
    void DestroySliders() {
        sliderHens_.Destroy();  // the row's builder first (removes the row with both)
        sliderRoosters_.Destroy();
        sliderFeeder_.Destroy();
    }
    void FeederCardBind(Il2CppObject* view, Il2CppObject* desc, Il2CppObject* plot, Il2CppObject* config);
    void FeederLockTick();
    bool FeederSwitchShown() const { return sliderFeeder_.Active(); }
    bool OnFeederSubmit();

    // api.h
    std::string CanSellCustom(Il2CppObject* plot, int32_t upg, std::string* brief);
    bool RemoveCustom(Il2CppObject* plot, int32_t upg, std::string& notes);
    std::string VanillaDependentOf(Il2CppObject* plot, int32_t vanillaUpg);
    bool ChainSellTargetOf(Il2CppObject* plot, Il2CppObject* item, int32_t& upgrade, Il2CppObject*& priceItem,
                           std::string* why = nullptr);
    Il2CppObject* HiddenMenuItemOf(size_t fieldOffset, Il2CppObject* value, Il2CppObject*& standIn);
    float TierMultiplier(Il2CppObject* obj, int family);
    std::string PlotValueOf(Il2CppObject* plot, const std::string& key);
    std::string PlotKeyOf(Il2CppObject* plot) {
        return unity::IsAlive(plot) && IsA(plot, plotClass_) ? PlotId(plot) : std::string();
    }
    bool SetPlotValueOf(Il2CppObject* plot, const std::string& key, const std::string& value);
    std::string SaveValueOf(const std::string& key);
    bool SetSaveValueOf(const std::string& key, const std::string& value);
    std::vector<std::pair<std::string, std::string>> SaveValuesWithPrefix(const std::string& prefix);

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
    bool ListMethods(Il2CppObject* list);
    bool ListInsert(Il2CppObject* list, int32_t index, Il2CppObject* item);
    int32_t ListIndexOf(Il2CppObject* list, Il2CppObject* item);
    void RestoreVanilla(MenuInjection& inj);
    void MergeStorageCards(Il2CppObject* plot, MenuInjection& inj, const std::vector<std::pair<Il2CppObject*, int32_t>>& cards);
    void RefreshAvailability(Il2CppObject* plot, const MenuInjection& inj);
    void ReportStorageCards(Il2CppObject* plot, MenuInjection& inj);
    void NoteVanillaIcon(Il2CppObject* item);
    void ApplyVanillaIcons();
    void RestoreVanillaIcons(const char* why);
    Il2CppObject* ItemIcon(Il2CppObject* item);
    void GameSpriteIcons(MenuInjection& inj);
    bool Shown(Il2CppObject* plot, const UpgradeDef& d);
    Il2CppObject* MarketIcon();
    Il2CppObject* FindGadgetByName(Il2CppObject* lookup, std::string_view name);
    void SetTexts(const Injected& in, const std::string& title, const std::string& desc, const std::string& prompt);
    void Cleanup(const char* why);

    Il2CppObject* EconDirector();
    SiloSummary Summarize(Il2CppObject* plot, bool sell, std::string* log, custom_upgrades::ui::Receipt* quote = nullptr,
                          const custom_upgrades::ui::Receipt* plan = nullptr);

    std::atomic<bool> enabled_{false};
    std::atomic<bool> resync_{false};  // (re-)enabled: state must be re-read for the loaded save
    bool cleaned_ = false;
    // Vanilla plot menu cards showing one of the user's drawings while the module is on (VanillaIconName):
    // item (GC handle held) -> its own vanilla _icon sprite, put back by Cleanup.
    std::unordered_map<Il2CppObject*, Il2CppObject*> vanillaIcons_;
    // Cards showing a game sprite found by name (GameSpriteIcons): Sprite class (optional), scans so far.
    Il2CppClass* spriteClass_ = nullptr;
    int spriteScans_ = 0;
    unsigned long long lastSpriteScan_ = 0;
    std::unordered_map<int32_t, Il2CppObject*> spriteIcons_;  // our id -> the sprite set (GC handle held)
    // gate (main thread)
    float gateAccum_ = 0.0f;
    bool locked_ = false;                 // "enabled" pinned on (settings::Lock) for the loaded save
    bool autoOn_ = false;                 // switched on by GateTick for such a save: off again once it is left
    std::string polledGame_;              // while off: the loaded save last checked
    unsigned long long polledNumber_ = 0;
    static bool UsesCustom(const SaveState& s);
    bool InGame();
    bool SaveLocked(const std::string& game, unsigned long long number);
    void SetLocked(bool on);
    void SwitchModule(bool on, const char* why);
    float worldAccum_ = 0.0f;
    int worldStep_ = -1;  // next WorldStep of the running world tick (-1: none running)
    Il2CppObject* LookupDirector();
    std::map<int32_t, Il2CppObject*> globalUpgItems_;  // vanilla upgrade -> menu item of any plot menu (icons)
    bool globalUpgItemsTried_ = false;
    Il2CppObject* GlobalUpgradeItem(int32_t upgrade);
    custom_upgrades::world::CoopWorld world_;
    custom_upgrades::drones::HenSource drones_;          // drone sources Hen / Elder Collector, Quantum Storage Link
    custom_upgrades::drone_programs::Rotation programs_;  // several programs per drone station (drone_programs.h)
    custom_upgrades::quantum::QuantumLink quantum_;      // Quantum Storage Link gadget (quantum_link.h)
    custom_upgrades::dj::DjBuildables dj_;               // Slime DJ booth / speakers / radio (dj_buildables.h)
    custom_upgrades::radio_shop::RadioShop radioShop_;   // the Slime Radio in the shop (radio_shop.h)
    custom_upgrades::world::SiloButtons silo_;  // Sell-All Button world button
    custom_upgrades::world::SiloButtons radioButtons_;  // Corral Radio world button (blue; opens slime_dj's menu)
    custom_upgrades::world::PlotProps props_;   // decorative props of owned tiers (plot_props.h)
    custom_upgrades::vacpack::VacpackUpgrades vacpack_;  // Fabricator vacpack upgrades (vacpack_upgrades.h)
    custom_upgrades::drones::BatterySaver battery_;      // Drone Battery Saver effect (drone_battery.h)
    custom_upgrades::shadow::ShadowCatcher shadow_;      // Shadow Catcher effect (shadow_catcher.h)
    custom_upgrades::launcher::SlimeLauncher launcher_;  // Slime Launcher effect (slime_launcher.h)
    std::vector<custom_upgrades::world::PropRequest> WantedProps() const;
    custom_upgrades::ui::PlotMenuSlider sliderHens_;      // Hen Collector limits, side by side (one pair
    custom_upgrades::ui::PlotMenuSlider sliderRoosters_;  // at a time; the hens slider builds the row)
    custom_upgrades::ui::PlotMenuSlider sliderFeeder_;    // Auto-Feeder "Leave last food item in" switch
    custom_upgrades::feeder::FeederLock feeder_;          // its effect (feeder_lock.h)
    custom_upgrades::feeder::FeederLights feederLights_;  // Auto-Feeder fill lights (feeder_lights.h)
    unsigned long long feederSubmitTick_ = ~0ull;         // frame of the last Submit the switch took
    custom_upgrades::ui::SaleNotices notices_;    // Sell-All: per-plort HUD notifications
    custom_upgrades::ui::SiloReceipt receipt_;    // Sell-All: confirmation dialog (silo_receipt.h)
    // While the receipt is open: the silo, the pressed button (both pinned; handles freed by a posted
    // job on close), the silo's plot id and the quote the dialog shows (what Sell may sell).
    Il2CppObject* receiptPlot_ = nullptr;
    Il2CppObject* receiptButton_ = nullptr;
    std::vector<Il2CppGCHandle> receiptHandles_;
    std::string receiptPlotId_;
    custom_upgrades::ui::Receipt receiptQuote_;
    // Silo capacity tiers (world tick): capacity per slot we set, by plot id (to put vanilla back).
    void CapacityTick();
    void RestoreCapacities(const char* why);
    Il2CppObject* LivePlot(Il2CppObject* gameModel, const std::string& id);
    int ApplyCapacity(Il2CppObject* plot, int target, const std::string& id, const char* why);
    int VanillaCapacity(Il2CppObject* plot, Il2CppObject* upgrader, Il2CppObject* silo);
    std::map<std::string, int> raisedCap_;
    bool capacityOk_ = false;
    Il2CppClass* storageUpgraderClass_ = nullptr;
    size_t offUpgSilo_ = 0, offUpgAmount_ = 0, offSiloDefs_ = 0, offDefMax_ = 0, offAmmoModel_ = 0, offPlotModelObj_ = 0;
    const MethodInfo* capUpdateMi_ = nullptr;      // AmmoSlotManager.UpdateMaxCount(int)
    const MethodInfo* capSlotMaxMi_ = nullptr;     // AmmoSlotManager.GetSlotMaxCount(int)
    const MethodInfo* getPlotModelMi_ = nullptr;   // GameModel.GetLandPlotModel(string)
    const MethodInfo* compInChildrenMi_ = nullptr; // Component.GetComponentInChildren(Type, bool)
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
    const MethodInfo* listInsert_ = nullptr;
    const MethodInfo* listIndexOf_ = nullptr;
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
    size_t offScGameModel_ = 0;  // SceneContext.GameModel (optional; 0 = the gate trusts the game name alone)
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
    // Market Link by name in LookupDirector._gadgetDefinitions (optional; the property is null in 1.3.0)
    Il2CppClass* groupClass_ = nullptr;
    size_t offLookupGadgets_ = 0, offGroupMembers_ = 0, offGroupSubs_ = 0;
    const MethodInfo* itemUpdAvailMi_ = nullptr;   // PlotUpgradePurchaseItemModel.UpdateAvailability (optional)
    const MethodInfo* itemUpdHiddenMi_ = nullptr;  // PlotUpgradePurchaseItemModel.UpdateHidden (optional)
    const MethodInfo* setHideFlagsMi_ = nullptr;
    const MethodInfo* getHideFlagsMi_ = nullptr;  // optional (KeepLoaded ORs the flag in)

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
sr2::core::DetourGuard g_gateGuard{kModule, "custom_upgrades.Gate"};
sr2::core::DetourGuard g_batteryGuard{kModule, "custom_upgrades.DroneBatterySaver"};
sr2::core::DetourGuard g_siloGuard{kModule, "PlortCollectorActivator.Activate"};
using ActivateFn = void (*)(Il2CppObject*, const MethodInfo*);
ActivateFn g_origActivate = nullptr;
size_t g_offActCollector = 0;  // PlortCollectorActivator.Collector

// Our button clones (the silo's Sell-All button, silo_world.cpp, and the Hen Collector's Activate
// button, coop_world.cpp) have Collector == null: the original would only NRE on them, so it never
// runs for them - also not while the module is off / faulted. Vanilla buttons (Collector set)
// always go to the original.
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

// ListUI.Submit(InputEventData) (0x83F450) is the list menus' submit listener (pad A / Enter: `if (the list
// has focus) onSubmitActionPerformed()`, the selected card's OnAction, which does nothing for an owned card).
// While the Auto-Feeder's "Leave last food item in" switch is shown (only for the owned Feeder card), Submit
// flips it and the original is not run. Every list menu shares the body: one flag check for the others.
using ListSubmitFn = void (*)(Il2CppObject*, void*, const MethodInfo*);
ListSubmitFn g_origListSubmit = nullptr;
sr2::core::DetourGuard g_submitGuard{kModule, "ListUI.Submit (Auto-Feeder switch)"};

void ListSubmitDetour(Il2CppObject* self, void* data, const MethodInfo* mi) {
    bool taken = false;
    if (g_self && self && g_self->On() && g_self->FeederSwitchShown())
        g_submitGuard.Run([&] { taken = g_self->OnFeederSubmit(); });
    if (!taken) g_origListSubmit(self, data, mi);
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

// ListUIRoot`4.RebuildUI (the shared generic body: every list menu comes through here). With the
// menu already open it does NOT run CreateUI / UpdateMenuItemAvailabilityAndHidden: it only asks
// the ListUI to RefreshAndAdvance, which re-reads the category item lists. Vanilla items only need
// that (their availability is a live Func); ours are added / removed / swapped in those lists, so
// the list is brought up to date first - after a purchase (Upgrade -> AddUpgrade -> RebuildUI) and
// after a sale (sell_upgrades -> RebuildUI). Other menus: one class check.
using RebuildFn = void (*)(Il2CppObject*, const MethodInfo*);
RebuildFn g_origRebuild = nullptr;
sr2::core::DetourGuard g_rebuildGuard{kModule, "ListUIRoot.RebuildUI"};

void RebuildDetour(Il2CppObject* root, const MethodInfo* mi) {
    const bool run = g_self && root && g_self->On() && g_self->IsPlotMenu(root);
    if (run) g_rebuildGuard.Run([&] { g_self->BeforeMenuUpdate(root); });
    g_origRebuild(root, mi);
    if (run && g_self->On()) g_rebuildGuard.Run([&] { g_self->AfterMenuUpdate(root); });
}

// LandPlotLocation.Replace(oldPlot, prefab) (0x871E60): the only path that swaps the plot at a
// location - buying a plot on an empty one (LandPlotUIRoot.BuyPlot) and demolishing (the demolish
// closure buys the empty plot through BuyPlot). The old LandPlot is destroyed, its model unregistered
// and a NEW LandPlotModel registered under the same LandPlotLocation._id, so every custom upgrade of
// the old plot goes with it (the vanilla upgrades are gone with the old model). Before this hook only
// a rebuild as another TYPE dropped them (OwnsKey); a silo demolished and rebuilt as a silo kept its
// Silo Capacity tiers, a corral its Music Box II.
using ReplaceFn = Il2CppObject* (*)(Il2CppObject*, Il2CppObject*, Il2CppObject*, const MethodInfo*);
ReplaceFn g_origReplace = nullptr;
sr2::core::DetourGuard g_replaceGuard{kModule, "LandPlotLocation.Replace"};

Il2CppObject* ReplaceDetour(Il2CppObject* location, Il2CppObject* oldPlot, Il2CppObject* prefab, const MethodInfo* mi) {
    // id and old type are read before the original: it detaches the old plot and destroys it.
    std::string id;
    int32_t oldType = -1;
    bool known = false;
    if (g_self && location && g_self->On())
        g_replaceGuard.Run([&] { known = g_self->BeforePlotReplaced(location, oldPlot, id, oldType); });
    Il2CppObject* r = g_origReplace(location, oldPlot, prefab, mi);
    if (known && g_self->On()) g_replaceGuard.Run([&] { g_self->OnPlotReplaced(id, oldType, r); });
    return r;
}

std::atomic<custom_upgrades::SaveVeto> g_saveVeto{nullptr};

Il2CppObject* SaveImplDetour(Il2CppObject* self, bool ignoreDebug, Il2CppObject* modify, const MethodInfo* mi) {
    if (auto veto = g_saveVeto.load(); veto && veto(self)) return nullptr;  // autotest save guard (guards itself)
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

int CustomUpgrades::Price(const UpgradeDef& d) const { return std::clamp(d.price, 0, 10000000); }

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
    quantum_.Reset();  // links of the previous game go with its scene; the loaded save's are built at the next tick
    dj_.Reset();       // the same for the Slime DJ buildables
    radioShop_.Reset();  // and the Slime Radio's shop entry (made again in the loaded save's shop)
    drones_.ResetSeen();  // the loaded save's Hen Collector programs are mapped back at the next tick
    programs_.ResetSeen();  // and its stations' extra programs come from its sidecar
    silo_.Reset();
    radioButtons_.Reset();
    props_.Reset();
    feeder_.Cleanup(why);  // the loaded save's locks come from its sidecar at the next tick
    feederLights_.Reset();  // its feeders are found again at the next tick
    notices_.Clear();
    DestroySliders();  // their value callbacks belong to the old state
    if (receipt_.IsOpen() && main_thread::IsMainThread()) CloseReceipt(why);  // its silo belongs to the old game
    raisedCap_.clear();  // those silos are replaced by the loaded game's (vanilla ApplyUpgrades sets them up)
}

void CustomUpgrades::OnLoad(Il2CppObject* asd, const void* identifier) {
    resync_.store(false);  // this load names the file exactly
    ResetState("save game load");
    vacpack_.Reset();  // the loaded player gets its extra slots' contents back once
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

bool CustomUpgrades::BeforePlotReplaced(Il2CppObject* location, Il2CppObject* oldPlot, std::string& id,
                                        int32_t& oldType) {
    if (!main_thread::IsMainThread() || !unity::IsAlive(location) || !IsA(location, locationClass_)) return false;
    id = unity::ToUtf8(unity::Field<Il2CppString*>(location, offLocationId_));
    oldType = unity::IsAlive(oldPlot) && IsA(oldPlot, plotClass_) ? PlotType(oldPlot) : -1;
    return !id.empty();
}

// A plot was demolished (or an empty plot built on): drop everything stored for that location id -
// owned tiers / world upgrades, Hen Collector contents and limits, per-plot values of other modules
// (sell_upgrades' remembered Ash Trough fill). Props, the Sell-All button and the Hen Collector clone
// were children of the old plot and are destroyed with it; the next ~1 Hz tick sees a bare plot. In
// memory only, like the vanilla demolish: the sidecar of the loaded .sav is written with the next game
// save, so reloading without saving brings back the plot AND its custom upgrades together.
void CustomUpgrades::OnPlotReplaced(const std::string& id, int32_t oldType, Il2CppObject* newObj) {
    if (!EnsureLoaded()) return;
    raisedCap_.erase(id);  // the new silo (if any) is a fresh object with the vanilla capacity
    auto it = state_.plots.find(id);
    if (it == state_.plots.end()) {
        SR2_INFO("[{}] plot '{}' replaced (old type {}): no custom upgrade state to drop", kModule, id, oldType);
        return;
    }
    std::string owned, values;
    for (const std::string& k : it->second.owned) owned += (owned.empty() ? "" : ", ") + k;
    for (const auto& [k, v] : it->second.values) values += (values.empty() ? "" : ", ") + k;
    const int hens = HenCount(it->second);
    SR2_INFO("[{}] plot '{}' demolished / replaced (old type {}, stored type {}{}): dropped owned [{}], values [{}]{}",
             kModule, id, oldType, it->second.type, unity::IsAlive(newObj) ? "" : ", no new plot object", owned, values,
             hens > 0 ? std::format(", {} chicken(s) in the Hen Collector lost with it", hens) : std::string());
    state_.plots.erase(it);
    Bump();
    tierCache_.clear();
}

void CustomUpgrades::OnNewGame() {
    resync_.store(false);
    ResetState("new game");
    vacpack_.Reset();
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
    if (!root.find("format") && !root.find("plots") && !root.find("values")) {
        // Only another module's per-save data (the sandbox's "cheated" mark, written while custom
        // upgrades never saved this file): not a custom-upgrade state, so the older-sidecar fallback applies.
        err = kForeignSidecar;
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
            bool known = false, retired = false;
            for (const UpgradeDef& d : kDefs) known |= key == d.key;
            for (const char* r : kRetiredKeys) retired |= key == r;
            if (known) ps.owned.insert(key);
            else if (retired)
                SR2_INFO("[{}] sidecar: retired upgrade '{}' on plot '{}' ignored (no longer offered, no refund)",
                         kModule, key, id);
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
        // Hen Collector keep-limits (plot menu sliders); absent -> defaults. Older sidecars have one
        // limit for all adults ("hen_collector_keep"): min(2, n) roosters, the rest hens.
        if (p["hen_collector_keep_hens"].isNumber() || p["hen_collector_keep_roosters"].isNumber()) {
            ps.henKeepHens = ClampKeepHens(p["hen_collector_keep_hens"].asInt(custom_upgrades::world::kHenKeepHensDefault));
            ps.henKeepRoosters =
                ClampKeepRoosters(p["hen_collector_keep_roosters"].asInt(custom_upgrades::world::kHenKeepRoostersDefault));
        } else if (p["hen_collector_keep"].isNumber()) {
            const int64_t n = std::max<int64_t>(p["hen_collector_keep"].asInt(20), 0);
            ps.henKeepRoosters = ClampKeepRoosters(std::min<int64_t>(n, custom_upgrades::world::kHenKeepRoostersDefault));
            ps.henKeepHens = ClampKeepHens(n - ps.henKeepRoosters);
        }
        if (!ps.owned.count("coop_hen_collector")) {
            ps.hens.clear();
            ps.henKeepHens = custom_upgrades::world::kHenKeepHensDefault;
            ps.henKeepRoosters = custom_upgrades::world::kHenKeepRoostersDefault;
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
    radioButtons_.Reset();
    props_.Reset();

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
                if (err == kForeignSidecar)
                    SR2_INFO("[{}] sidecar {}: {} - looking for another one", kModule, p.string(), err);
                else
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
        if (ps.owned.count("coop_hen_collector")) {
            p.set("hen_collector_keep_hens", sr2::json::Value(static_cast<double>(ClampKeepHens(ps.henKeepHens))));
            p.set("hen_collector_keep_roosters", sr2::json::Value(static_cast<double>(ClampKeepRoosters(ps.henKeepRoosters))));
        }
        if (!ps.values.empty()) {
            sr2::json::Value vals = sr2::json::Value::Object();
            for (const auto& [k, v] : ps.values) vals.set(k, sr2::json::Value(v));
            p.set("values", std::move(vals));
        }
        plots.set(id, std::move(p));
    }
    if (rotation && std::filesystem::exists(path, ec)) {
        // The sandbox marks this file "cheated" from inside the same game save, just before this
        // write (modules/sandbox/cheated_save.cpp): keep its entry if it was made for this file.
        std::ifstream f(path, std::ios::binary);
        std::stringstream ss;
        ss << f.rdbuf();
        sr2::json::Value old;
        std::string perr;
        if (sr2::json::Parse(ss.str(), old, perr) && old.isObject()) {
            const sr2::json::Value& sb = old["sandbox"];
            const double n = sb["save_number"].asNumber(0);
            if (sb.isObject() && (n <= 0 || static_cast<unsigned long long>(n) == saveNumber)) root.set("sandbox", sb);
        }
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
    if (d.id == SILO_SELL_ALL && !silo_.Ready()) return false;  // its world button is all it does
    if (IsSiloCapacity(d) && !capacityOk_) return false;        // nothing could apply the capacity
    if (!d.needs) return true;
    // An owned upgrade that is not a tier stays listed (sellable) without its prerequisite: the Corral Radio
    // after its Music Box was sold (VanillaDependentOf).
    if (d.family == kNoFamily && OwnsKey(plot, d)) return true;
    if (!custom_upgrades::IsCustomUpgrade(d.needs)) return VanillaOwned(plot, d.needs);  // vanilla prerequisite
    // A higher tier of a chain on a vanilla card needs that vanilla upgrade too: a save that owns Sprinkler II
    // without the Sprinkler (bought before round 9, when Sprinkler II needed nothing) keeps the vanilla Sprinkler
    // card buyable instead of covering it with Sprinkler III.
    if (const int32_t root = ChainVanillaRoot(d); root && !VanillaOwned(plot, root)) return false;
    const UpgradeDef* r = FindDef(d.needs);
    return r && OwnsKey(plot, *r);
}

// A tier chain has ONE menu card (vanilla lists each silo storage tier as its own card, the next
// one appearing once the previous is owned; ours chain on the same card instead): the first tier not owned yet, or the last one once all are owned. An
// owned tier with a next tier gives its place to that one (which is listed now: its `needs` is
// owned); the card then offers Buy (next tier) and Sell (the owned one, sell_upgrades via
// ChainSellTarget). A chain that starts on a vanilla upgrade (Music Box -> Music Box II) takes over
// the vanilla card once that upgrade is owned (BeforeMenuUpdate).
bool CustomUpgrades::Shown(Il2CppObject* plot, const UpgradeDef& d) {
    if (!Visible(plot, d)) return false;
    if (d.family == kNoFamily) return true;
    for (const UpgradeDef& o : kDefs)
        if (o.needs == d.id && o.family == d.family) return !OwnsKey(plot, d);
    return true;
}

// ---------------------------------------------------------------- menu --

bool CustomUpgrades::ListMethods(Il2CppObject* list) {
    Il2CppClass* k = list ? api().il2cpp_object_get_class(list) : nullptr;
    if (!k) return false;
    if (k == listClass_) return true;
    std::string err;
    const MethodInfo* add = Resolver::Get().Method(k, "Add", 1, &err);
    const MethodInfo* rem = Resolver::Get().Method(k, "Remove", 1, &err);
    const MethodInfo* has = Resolver::Get().Method(k, "Contains", 1, &err);
    const MethodInfo* ins = Resolver::Get().Method(k, "Insert", 2, &err);
    const MethodInfo* idx = Resolver::Get().Method(k, "IndexOf", 1, &err);
    if (!add || !rem || !has || !ins || !idx) {
        SR2_WARN("[{}] List methods not found: {}", kModule, err);
        return false;
    }
    listClass_ = k;
    listAdd_ = add;
    listRemove_ = rem;
    listContains_ = has;
    listInsert_ = ins;
    listIndexOf_ = idx;
    return true;
}

bool CustomUpgrades::ListInsert(Il2CppObject* list, int32_t index, Il2CppObject* item) {
    if (!ListMethods(list)) return false;
    index = std::clamp<int32_t>(index, 0, static_cast<int32_t>(unity::ListCount(list)));
    bool ok = false;
    Invoke(listInsert_, list, {&index, item}, "List.Insert", &ok);
    return ok;
}

int32_t CustomUpgrades::ListIndexOf(Il2CppObject* list, Il2CppObject* item) {
    if (!item || !ListMethods(list)) return -1;
    bool ok = false;
    Il2CppObject* boxed = Invoke(listIndexOf_, list, {item}, "List.IndexOf", &ok);
    return ok ? Unbox<int32_t>(boxed, -1) : -1;
}

bool CustomUpgrades::ListCall(Il2CppObject* list, const char* method, Il2CppObject* item, bool* result) {
    if (!ListMethods(list)) return false;
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
    bool ok = false;
    Il2CppObject* gc = Invoke(gameCtxInstMi_, nullptr, {}, "GameContext.Instance", &ok);
    Il2CppObject* lookup = (ok && unity::IsAlive(gc)) ? unity::Field<Il2CppObject*>(gc, offGcLookup_) : nullptr;
    if (!unity::IsAlive(lookup)) return nullptr;  // not loaded yet: try again next time
    marketIconTried_ = true;
    Il2CppObject* ml = marketLinkMi_ ? Invoke(marketLinkMi_, lookup, {}, "LookupDirector.get_MarketLink", &ok) : nullptr;
    const char* from = "LookupDirector.MarketLink";
    if (!ml || !ok || !unity::IsAlive(ml) || !IsA(ml, identClass_)) {
        // Round 11: the property is null in 1.3.0 (as RefineryLink, quantum_link.cpp), so the card kept the
        // cloned template's icon, Storage Capacity's. The definition is in the gadget groups by name.
        ml = FindGadgetByName(lookup, "MarketLink");
        from = "the gadget groups";
    }
    Il2CppObject* icon = ml ? unity::Field<Il2CppObject*>(ml, offIdentIcon_) : nullptr;
    if (unity::IsAlive(icon)) {
        marketIcon_ = icon;
        unity::NewGCHandle(icon);  // pinned for the session (never freed)
        SR2_INFO("[{}] using the Market Link icon '{}' for the sell-all items (definition from {})", kModule,
                 unity::GetName(icon), from);
    } else {
        SR2_WARN("[{}] no Market Link definition (LookupDirector.MarketLink null, none named 'MarketLink' in the gadget "
                 "groups) - the Sell-All Button card keeps the template's icon", kModule);
    }
    return marketIcon_;
}

// The gadget definition with this name in LookupDirector._gadgetDefinitions and its member groups.
// Once per session (MarketIcon).
Il2CppObject* CustomUpgrades::FindGadgetByName(Il2CppObject* lookup, std::string_view name) {
    if (!groupClass_ || !offLookupGadgets_ || !offGroupMembers_) return nullptr;
    std::vector<Il2CppObject*> groups{unity::Field<Il2CppObject*>(lookup, offLookupGadgets_)};
    for (size_t gi = 0; gi < groups.size() && gi < 64; ++gi) {
        Il2CppObject* grp = groups[gi];
        if (!unity::IsAlive(grp) || !IsA(grp, groupClass_)) continue;
        void* members = unity::Field<void*>(grp, offGroupMembers_);
        for (size_t i = 0, n = unity::ListCount(members); i < n && i < 1000; ++i) {
            auto* t = static_cast<Il2CppObject*>(unity::ListAt(members, i));
            if (IsA(t, identClass_) && unity::IsAlive(t) && unity::GetName(t) == name) return t;
        }
        if (!offGroupSubs_) continue;
        void* subs = unity::Field<void*>(grp, offGroupSubs_);
        for (size_t i = 0, n = unity::ListCount(subs); i < n && i < 64; ++i)
            groups.push_back(static_cast<Il2CppObject*>(unity::ListAt(subs, i)));
    }
    return nullptr;
}

// The user's drawing for a custom item (assets/icons/<name>.png, ui/mod_icons.h), or nullptr when
// none is embedded: the card then shows the vanilla icon of iconFrom (Market Link for the Sell-All
// Button). Round 11: no script-drawn card icons ("trying to move away from AI art"); a drawing
// dropped in under one of these names takes over at the next build (.claude/notes/custom-icons.md).
const char* ModIconName(int32_t id) {
    const char* name = nullptr;
    switch (id) {
        case SILO_SELL_ALL: name = "sell_all"; break;          // vanilla: Market Link
        case CORRAL_MUSIC_2: name = "music_2"; break;          // vanilla: Music Box
        case CORRAL_RADIO: name = "corral_radio"; break;       // vanilla: Music Box
        case COOP_HENS_1: name = "hens_1"; break;              // vanilla: Deluxe Coop
        case COOP_HENS_2: name = "hens_2"; break;              // vanilla: Deluxe Coop
        case GARDEN_GROWTH_1: name = "growth_1"; break;        // vanilla: Sprinkler
        case GARDEN_GROWTH_2: name = "growth_2"; break;        // vanilla: Sprinkler
        case INCIN_ASH_BOOST: name = "ash_boost"; break;       // vanilla: Ash Trough
        case COOP_AIR_NET: name = "coop_air_net"; break;       // vanilla: the corral's Air Net
        // The user's drawing (the Plort Collector with a hen coming out), also the drone source's icon.
        case COOP_HEN_COLLECTOR: name = "hen_collector_source"; break;  // vanilla: Deluxe Coop
        default: return nullptr;
    }
    return sr2::ui::mod_icons::Has(name) ? name : nullptr;
}

// The user's drawing for a VANILLA plot upgrade card (the game's own item), or nullptr. Shown only while
// the module is on (ApplyVanillaIcons from the menu pre-hook, Cleanup puts the vanilla sprite back), so a
// vanilla setup keeps vanilla looks. Our tiers that borrow that card's icon (iconFrom) still get the
// vanilla sprite (ItemIcon). Add a line here and a drawing assets/icons/<name>.png for another card.
const char* VanillaIconName(int32_t upgrade) {
    const char* name = nullptr;
    switch (upgrade) {
        case kUpgSprinkler: name = "sprinkler_1"; break;  // Sprinkler (garden T1; T2 = growth_1, T3 vanilla)
        case kUpgMusicBox: name = "music_1"; break;       // Music Box (corral T1; Music Box II stays vanilla)
        default: return nullptr;
    }
    return sr2::ui::mod_icons::Has(name) ? name : nullptr;
}

// A vanilla card of an injected menu that has a drawing: remembered with its own sprite (once per item).
void CustomUpgrades::NoteVanillaIcon(Il2CppObject* item) {
    if (vanillaIcons_.count(item) || !VanillaIconName(unity::Field<int32_t>(item, offItemUpgrade_))) return;
    Il2CppObject* icon = unity::Field<Il2CppObject*>(item, offItemIcon_);
    if (!unity::IsAlive(icon) || !unity::NewGCHandle(item)) return;  // item pinned (a few, never freed)
    vanillaIcons_.emplace(item, icon);
}

// The drawings onto their vanilla cards (main thread, module on). Cheap when already set.
void CustomUpgrades::ApplyVanillaIcons() {
    for (const auto& [item, vanilla] : vanillaIcons_) {
        if (!unity::IsAlive(item)) continue;
        const char* name = VanillaIconName(unity::Field<int32_t>(item, offItemUpgrade_));
        Il2CppObject* mine = name ? sr2::ui::mod_icons::Get(name, vanilla) : nullptr;
        if (!mine || unity::Field<Il2CppObject*>(item, offItemIcon_) == mine) continue;
        SetRef(item, offItemIcon_, mine);
        SR2_INFO("[{}] vanilla card '{}' shows the drawing '{}' (vanilla icon '{}')", kModule, unity::GetName(item), name,
                 unity::IsAlive(vanilla) ? unity::GetName(vanilla) : std::string("?"));
    }
}

// The vanilla sprites back (Cleanup; a field write, also after a fault).
void CustomUpgrades::RestoreVanillaIcons(const char* why) {
    int restored = 0;
    for (const auto& [item, vanilla] : vanillaIcons_) {
        if (!unity::IsAlive(item) || unity::Field<Il2CppObject*>(item, offItemIcon_) == vanilla) continue;
        SetRef(item, offItemIcon_, vanilla);
        ++restored;
    }
    if (restored) SR2_INFO("[{}] {}: {} vanilla card icon(s) back to the game's", kModule, why, restored);
}

// A game sprite (by asset name) for one of our cards, the user's pick, when no drawing is embedded under
// the card's ModIconName: the Corral Radio shows the Slimepedia's music player icon. Until it is found
// the card keeps its iconFrom icon (the Music Box's). Only sprites loaded at that moment are found (the
// Slimepedia's may load later), so up to 3 Resources.FindObjectsOfTypeAll(Sprite) scans per session, at
// least 20 s apart, from the plot menu pre-hook (menu opens / rebuilds, never per frame).
const char* GameSpriteName(int32_t id) {
    switch (id) {
        case CORRAL_RADIO: return "iconPediaHomeMusicPlayer";
        default: return nullptr;
    }
}

void CustomUpgrades::GameSpriteIcons(MenuInjection& inj) {
    std::vector<Injected*> want;
    for (Injected& in : inj.items)
        if (GameSpriteName(in.def->id) && !ModIconName(in.def->id) && !spriteIcons_.count(in.def->id) &&
            unity::IsAlive(in.item))
            want.push_back(&in);
    if (want.empty() || !spriteClass_ || spriteScans_ >= 3) return;
    const unsigned long long now = GetTickCount64();
    if (spriteScans_ > 0 && now - lastSpriteScan_ < 20000) return;
    ++spriteScans_;
    lastSpriteScan_ = now;
    std::unordered_map<std::string, Il2CppObject*> byName;
    for (Injected* in : want) byName.emplace(GameSpriteName(in->def->id), nullptr);
    size_t seen = 0;
    for (Il2CppObject* spr : unity::FindObjectsOfTypeAll(spriteClass_)) {
        if (!unity::IsAlive(spr) || !IsA(spr, spriteClass_)) continue;
        ++seen;
        auto it = byName.find(unity::GetName(spr));
        if (it != byName.end() && !it->second) it->second = spr;
    }
    for (Injected* in : want) {
        const char* name = GameSpriteName(in->def->id);
        Il2CppObject* spr = byName[name];
        if (!spr || !unity::NewGCHandle(spr)) {
            SR2_INFO("[{}] {}: game sprite '{}' not loaded (scan {} of 3, {} sprites) - keeps the icon '{}'", kModule,
                     in->def->title, name, spriteScans_, seen,
                     unity::GetName(unity::Field<Il2CppObject*>(in->item, offItemIcon_)));
            continue;
        }
        KeepLoaded(spr);  // a Slimepedia sprite must outlive the pedia's assets (Resources.UnloadUnusedAssets)
        SetRef(in->item, offItemIcon_, spr);
        spriteIcons_.emplace(in->def->id, spr);
        SR2_INFO("[{}] {}: card icon = the game sprite '{}' (scan {})", kModule, in->def->title, name, spriteScans_);
    }
}

// A menu item's icon, the vanilla sprite for a card that shows a drawing right now.
Il2CppObject* CustomUpgrades::ItemIcon(Il2CppObject* item) {
    auto it = vanillaIcons_.find(item);
    return it != vanillaIcons_.end() ? it->second : unity::Field<Il2CppObject*>(item, offItemIcon_);
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
    Il2CppObject* icon = iconSrc ? ItemIcon(iconSrc) : nullptr;  // vanilla, not the card's drawing
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
            if (vanilla.emplace(u, item).second) {
                KeepLoaded(item);
                NoteVanillaIcon(item);
            }
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
        // Menu position: a tier on a vanilla upgrade takes over that upgrade's card; `after` lists
        // the item right behind a vanilla one. The vanilla item may be out of the list for a while,
        // so it is pinned (a few per session, never freed).
        if (const int32_t root = ChainVanillaRoot(d)) {
            auto vit = vanilla.find(root);
            if (vit != vanilla.end() && unity::NewGCHandle(vit->second)) in.base = vit->second;
        }
        if (d.after) {
            auto vit = vanilla.find(d.after);
            if (vit != vanilla.end()) in.anchor = vit->second;
        }
        inj.items.push_back(in);
    }
    inj.list = bestList;
    unity::NewGCHandle(bestList);
    SR2_INFO("[{}] plot type {}: {} custom item(s) cloned from '{}' (upgrade {})", kModule, plotType, inj.items.size(),
             unity::GetName(tpl), unity::Field<int32_t>(tpl, offItemUpgrade_));
    return !inj.items.empty();
}

// Round 12 ("cant sell speakers I again, same bug as before with there being no sell & only buy"; log: "Music
// Box II: vanilla menu item of upgrade 2 not found" after selling Music Box II): a vanilla card we take out
// of the shared list (a tier card stands in for it, merged storage cards) is referenced only by our GC
// handle while it is out, and a GC handle does not keep an asset through Resources.UnloadUnusedAssets
// (the same as slime_dj's song clip, re/audio.md): the asset was destroyed (m_CachedPtr 0), the chain card
// lost its Sell button and RestoreVanilla would put a dead item back. HideFlags.DontUnloadUnusedAsset on
// every vanilla upgrade item of an injected menu keeps them (they are part of the menu config anyway).
void CustomUpgrades::KeepLoaded(Il2CppObject* asset) {
    if (!setHideFlagsMi_ || !unity::IsAlive(asset)) return;
    int32_t flags = 0;
    if (getHideFlagsMi_) {
        bool ok = false;
        Il2CppObject* boxed = Invoke(getHideFlagsMi_, asset, {}, "Object.get_hideFlags", &ok);
        if (!ok) return;
        flags = Unbox<int32_t>(boxed);
    }
    if (flags & kHideFlagsDontUnloadUnusedAsset) return;
    flags |= kHideFlagsDontUnloadUnusedAsset;
    Invoke(setHideFlagsMi_, asset, {&flags}, "Object.set_hideFlags");
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
    // The list is shared by every plot of this type: start from the vanilla list each time, then
    // place this plot's cards. Cards that take over a vanilla card first (same list length, so the
    // recorded indices are vanilla ones), then the rest (right after their anchor, else at the end).
    RestoreVanilla(*inj);
    if (sink) {
        ApplyVanillaIcons();    // the user's drawings on vanilla cards (Sprinkler, Music Box)
        GameSpriteIcons(*inj);  // game sprites picked by the user (Corral Radio)
    }
    // Silo "Additional Storage" cards with their VANILLA indices (the list is vanilla right now).
    std::vector<std::pair<Il2CppObject*, int32_t>> storageCards;
    if (type == kPlotSilo && sink)
        for (size_t i = 0, n = unity::ListCount(inj->list); i < n && i < 64; ++i) {
            auto* item = static_cast<Il2CppObject*>(unity::ListAt(inj->list, i));
            if (!IsA(item, upgItemClass_) || !unity::IsAlive(item)) continue;
            const int32_t u = unity::Field<int32_t>(item, offItemUpgrade_);
            if (std::find(std::begin(kUpgStorageSections), std::end(kUpgStorageSections), u) != std::end(kUpgStorageSections))
                storageCards.emplace_back(item, static_cast<int32_t>(i));
        }
    for (int pass = 0; pass < 2; ++pass) {
        for (Injected& in : inj->items) {
            if ((pass == 0) != (in.base != nullptr)) continue;
            if (in.costEntry) unity::Field<int32_t>(in.costEntry, offCostAmount_) = Price(*in.def);
            if (!sink || !Shown(plot, *in.def)) continue;
            if (in.base) {
                const int32_t at = ListIndexOf(inj->list, in.base);
                bool removed = false;
                if (at >= 0 && ListCall(inj->list, "Remove", in.base, &removed) && removed) {
                    if (ListInsert(inj->list, at, in.item)) {
                        inj->hidden.push_back({in.base, at});
                        in.inList = true;
                    } else {
                        ListInsert(inj->list, at, in.base);  // the vanilla card stays
                    }
                    continue;
                }
                // The vanilla item is not in this list: listed like any other card below.
            }
            const int32_t anchorAt = in.anchor ? ListIndexOf(inj->list, in.anchor) : -1;
            in.inList = anchorAt >= 0 ? ListInsert(inj->list, anchorAt + 1, in.item)
                                      : ListCall(inj->list, "Add", in.item);
        }
    }
    if (storageCards.size() > 1) MergeStorageCards(plot, *inj, storageCards);
    RefreshAvailability(plot, *inj);
    // Texts now, before the original runs: with the menu open, RebuildUI's original rebinds the
    // cards (RefreshAndAdvance) and so runs LocalizeStringEvent.UpdateString for a card that was
    // just placed (a tier taking over a vanilla card after its purchase). With the texts only
    // registered in the post hook, a clone shown for the first time this session showed its
    // template's text ("High Walls" on the Music Box card, test of 506a4f60) until the next bind.
    if (sink) RegisterTexts(plot, *inj);
}

void CustomUpgrades::RegisterTexts(Il2CppObject* /*plot*/, const MenuInjection& inj) {
    for (const Injected& in : inj.items) {
        if (!in.inList) continue;
        const UpgradeDef& d = *in.def;
        SetTexts(in, d.title, d.description, d.prompt);  // Hen Collector too: plain text, the limits are on its sliders
    }
}

// User request ("merge additional storage to 1 buy menu option, I don't want to scroll to upgrade
// it"): vanilla lists STORAGE2 / 3 / 4 as three "Additional Storage" cards (STORAGE3 / 4 hidden until
// the one before is owned, owned ones stay greyed with the checkmark). Only one stays in the list:
// the first section not owned yet, or STORAGE4 once all are owned. The others go out as hidden
// vanilla cards at their vanilla index, so RestoreVanilla puts them back for the next plot / when the
// module stops. Runs after our cards are placed: their recorded indices are vanilla ones too.
void CustomUpgrades::MergeStorageCards(Il2CppObject* plot, MenuInjection& inj,
                                       const std::vector<std::pair<Il2CppObject*, int32_t>>& cards) {
    // Tier labels like our other chains (user request "add additional storage tier labels"): the three
    // vanilla cards share the title "Additional Storage"; each gets the section it adds (STORAGE2 ->
    // "Additional Storage II", ...). Their own _title objects, registered once; Cleanup removes them.
    if (inj.storageTitles.empty() && text_override::SinkActive()) {
        for (const auto& [item, index] : cards) {
            const int32_t u = unity::Field<int32_t>(item, offItemUpgrade_);
            const char* tier = u == kUpgStorageSections[0] ? "II" : u == kUpgStorageSections[1] ? "III"
                               : u == kUpgStorageSections[2] ? "IV" : nullptr;
            Il2CppObject* title = unity::Field<Il2CppObject*>(item, offItemTitle_);
            if (!tier || !IsA(title, lsClass_) || !unity::NewGCHandle(item)) continue;  // item pinned (3, never freed)
            text_override::Set(title, std::string("Additional Storage ") + tier);
            inj.storageTitles.push_back(title);
        }
        SR2_INFO("[{}] silo: {} Additional Storage card(s) labelled with their section (II / III / IV)", kModule,
                 inj.storageTitles.size());
    }
    Il2CppObject* keep = cards.back().first;
    for (const auto& [item, index] : cards)
        if (!VanillaOwned(plot, unity::Field<int32_t>(item, offItemUpgrade_))) {
            keep = item;
            break;
        }
    for (const auto& [item, index] : cards) {
        if (item == keep) continue;
        bool removed = false;
        if (ListCall(inj.list, "Remove", item, &removed) && removed) inj.hidden.push_back({item, index});
    }
}

// The availability / hidden Funcs of the cards in the list, for this plot - what vanilla's
// UpdateMenuItemAvailabilityAndHidden does on every menu open (IsAvailable = () => !plot.HasUpgrade(
// _upgrade), the greyed icon with the checkmark and no Purchase button when false). RebuildUI (a
// purchase or a sale with the menu open) does not run it, so a card we put in the list there kept the
// Funcs of its last plot or, never bound this session, the constructor's "always available": a maxed
// chain (Faster Hens II, Silo Capacity V, ...) showed Purchase and no checkmark until the menu was
// opened again (user report). Runs before the original, so the rebind already sees the new Funcs; on
// a menu open the original then does the same once more.
void CustomUpgrades::RefreshAvailability(Il2CppObject* plot, const MenuInjection& inj) {
    if (!itemUpdAvailMi_ || !itemUpdHiddenMi_ || !inj.list) return;
    for (size_t i = 0, n = unity::ListCount(inj.list); i < n && i < 64; ++i) {
        auto* item = static_cast<Il2CppObject*>(unity::ListAt(inj.list, i));
        if (!IsA(item, upgItemClass_) || !unity::IsAlive(item)) continue;
        bool ok = false;
        Invoke(itemUpdAvailMi_, item, {plot}, "PlotUpgradePurchaseItemModel.UpdateAvailability", &ok);
        if (!ok) return;
        Invoke(itemUpdHiddenMi_, item, {plot}, "PlotUpgradePurchaseItemModel.UpdateHidden", &ok);
        if (!ok) return;
    }
}

void CustomUpgrades::RestoreVanilla(MenuInjection& inj) {
    // Our cards out, the vanilla cards they stood in for back at their own index (ascending, so
    // each index is valid when it is used).
    if (!inj.list) return;
    for (Injected& in : inj.items) {
        bool has = false, removed = false;
        if (ListCall(inj.list, "Contains", in.item, &has) && has) ListCall(inj.list, "Remove", in.item, &removed);
        in.inList = false;
    }
    std::sort(inj.hidden.begin(), inj.hidden.end(),
              [](const HiddenVanilla& a, const HiddenVanilla& b) { return a.index < b.index; });
    for (const HiddenVanilla& h : inj.hidden) {
        if (!unity::IsAlive(h.item)) {  // see KeepLoaded: a destroyed asset must not go back into the menu
            SR2_WARN("[{}] a vanilla menu card taken out of the list was destroyed meanwhile - not put back", kModule);
            continue;
        }
        bool has = false;
        if (ListCall(inj.list, "Contains", h.item, &has) && !has) ListInsert(inj.list, h.index, h.item);
    }
    inj.hidden.clear();
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
    RegisterTexts(plot, *inj);  // again: the original may have changed what the pre hook read
    if (PlotType(plot) == kPlotSilo) ReportStorageCards(plot, *inj);
}

// Round 8: "silo additional storage is separate upgrades again" (no code path found that undoes the merge).
// After the game built / refreshed the menu: which Additional Storage cards are in the list now, logged when
// it changes. More than one means something put them back after BeforeMenuUpdate (logged as a warning).
void CustomUpgrades::ReportStorageCards(Il2CppObject* plot, MenuInjection& inj) {
    if (!inj.list) return;
    std::string listed;
    int count = 0;
    for (size_t i = 0, n = unity::ListCount(inj.list); i < n && i < 64; ++i) {
        auto* item = static_cast<Il2CppObject*>(unity::ListAt(inj.list, i));
        if (!IsA(item, upgItemClass_) || !unity::IsAlive(item)) continue;
        const int32_t u = unity::Field<int32_t>(item, offItemUpgrade_);
        const auto at = std::find(std::begin(kUpgStorageSections), std::end(kUpgStorageSections), u);
        if (at == std::end(kUpgStorageSections)) continue;
        ++count;
        listed += std::format("{}STORAGE{} at {}{}", listed.empty() ? "" : ", ", 2 + (at - std::begin(kUpgStorageSections)), i,
                              VanillaOwned(plot, u) ? " (owned)" : "");
    }
    std::string report = std::format("{} card(s): {}; {} vanilla card(s) out of the list", count,
                                     listed.empty() ? "none" : listed, inj.hidden.size());
    if (report == inj.storageReport) return;
    inj.storageReport = report;
    if (count > 1) {
        SR2_WARN("[{}] silo menu: Additional Storage NOT merged after the game's update - {}", kModule, report);
    } else {
        SR2_INFO("[{}] silo menu: Additional Storage {}", kModule, report);
    }
}

void CustomUpgrades::Cleanup(const char* why) {
    // Always (also after a fault, also when cleaned already): the receipt's input lock (paused
    // time) must never outlive the module.
    if (receipt_.IsOpen() && main_thread::IsMainThread()) CloseReceipt(why);
    // The drone programs and sources too: also after a fault (field writes only then).
    programs_.Revert(main_thread::IsMainThread() && !sr2::core::ModuleFaulted(kModule), why);
    drones_.Revert(main_thread::IsMainThread() && !sr2::core::ModuleFaulted(kModule), why);
    if (cleaned_) return;
    cleaned_ = true;
    DestroySliders();  // main thread only (no-op otherwise / without game calls after a fault)
    notices_.Clear();
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
    if (radioButtons_.Ready() && main_thread::IsMainThread()) {
        radioButtons_.RemoveAll();
        SR2_INFO("[{}] {}: corral radio buttons removed", kModule, why);
    }
    if (props_.Ready() && main_thread::IsMainThread()) {
        props_.RemoveAll();
        SR2_INFO("[{}] {}: plot props removed", kModule, why);
    }
    // Quantum Storage Links back to plain-looking Refinery Links, our variant out of the favourite slots
    // and the variant list (also after a fault: the favourites are saved).
    quantum_.Cleanup(why);
    dj_.Cleanup(why);  // Slime DJ buildables undressed, our variants out of the favourites and variant lists
    radioShop_.Cleanup(why);  // the Slime Radio out of the shop's item list
    RestoreCapacities(why);  // silos back to their vanilla capacity per slot (contents stay)
    vacpack_.Cleanup(why);   // vacpack upgrade levels off, our definitions out of the game's lists
    shadow_.Cleanup(why);    // Shadow diet back to vanilla, kept shadow slimes behave as in vanilla again
    feeder_.Cleanup(why);    // locked auto-feeders take any food again
    feederLights_.Cleanup(why);  // feeder lights back to the material's green
    int removed = 0, restored = 0;
    for (auto& inj : injections_) {
        for (Injected& in : inj->items) {
            // Lock the price first: an already open menu must not be able to buy the item
            // through vanilla AddUpgrade (our detour may be gone).
            if (in.costEntry) unity::Field<int32_t>(in.costEntry, offCostAmount_) = INT_MAX;
            removed += in.inList ? 1 : 0;
        }
        restored += static_cast<int>(inj->hidden.size());
        RestoreVanilla(*inj);  // our items out, vanilla cards back
        for (Il2CppObject* title : inj->storageTitles) text_override::Remove(title);  // vanilla titles again
        inj->storageTitles.clear();
    }
    RestoreVanillaIcons(why);  // vanilla cards with a drawing (the Sprinkler) show the game's icon again
    if (removed || restored)
        SR2_INFO("[{}] {}: {} custom menu item(s) removed, {} vanilla item(s) put back", kModule, why, removed,
                 restored);
}

void CustomUpgrades::FrameCheck(float dt) {
    if (sr2::core::ModuleFaulted(kModule)) {
        if (!cleaned_ || receipt_.IsOpen()) Cleanup("module faulted");
        SetLocked(false);
        return;
    }
    if (!On() && receipt_.IsOpen()) CloseReceipt("module off");
    if (std::isfinite(dt) && dt > 0.0f) gateAccum_ += std::min(dt, 1.0f);
    // Round 13: the parts are timed on their own (perf "custom_upgrades.*" lines in the 10 s summary and the
    // > 2 ms warnings), so the log names the slow one.
    if (gateAccum_ >= kGateTickSeconds) {
        SR2_PERF_DETAIL("custom_upgrades.gate (0.5 s)");
        gateAccum_ = 0.0f;
        g_gateGuard.Run([&] { GateTick(); });
    }
    if (On() && SliderActive()) {
        SR2_PERF_DETAIL("custom_upgrades.plot menu slider");
        g_sliderGuard.Run([&] { SliderTick(dt); });
    }
    if (On()) {
        SR2_PERF_DETAIL("custom_upgrades.silo / radio / notices / receipt");
        if (silo_.Ready()) g_siloGuard.Run([&] { silo_.Frame(); });  // button message (cheap when none)
        if (radioButtons_.Ready()) g_siloGuard.Run([&] { radioButtons_.Frame(); });
        if (notices_.Ready()) g_siloGuard.Run([&] { notices_.Frame(dt); });  // queued sale notices
        if (receipt_.IsOpen()) g_siloGuard.Run([&] { ReceiptTick(dt); });   // Sell-All receipt input
    }
    if (On()) {
        SR2_PERF_DETAIL("custom_upgrades.vacpack / quantum / dj frame");
        if (vacpack_.Ready()) vacpack_.Frame();  // extra vacpack slots' number keys
        if (quantum_.Ready()) quantum_.Frame();  // the Quantum Storage Links' glow glitch
        if (dj_.Ready()) dj_.Frame();  // Slime Radios held in the vacuum: rotation free while held
    }
    if (battery_.Ready()) {  // drone battery refunds (every ~2 s); off: forget the last looks
        SR2_PERF_DETAIL("custom_upgrades.battery saver");
        if (On()) g_batteryGuard.Run([&] { battery_.Tick(dt); });
        else battery_.Reset();
    }
    if (!On() || (!world_.Ready() && !silo_.Ready() && !props_.Ready() && !capacityOk_ && !vacpack_.Ready() &&
                  !quantum_.Ready() && !drones_.Ready())) {
        worldStep_ = -1;
        return;
    }
    if (std::isfinite(dt) && dt > 0.0f) worldAccum_ += std::min(dt, 1.0f);
    // The ~1 Hz world tick, spread over frames: one part per frame, in the old order, so no frame carries all
    // of it. A tick asked for early (worldAccum_ = kWorldTickSeconds) starts once the running one is done.
    if (worldStep_ < 0) {
        if (worldAccum_ < kWorldTickSeconds) return;
        worldAccum_ = 0.0f;
        worldStep_ = 0;
    }
    g_worldGuard.Run([&] {
        while (worldStep_ >= 0 && worldStep_ < kWorldSteps)
            if (WorldStep(worldStep_++)) break;
    });
    if (worldStep_ >= kWorldSteps || sr2::core::ModuleFaulted(kModule)) worldStep_ = -1;
}

// ---------------------------------------------------------------- gate ------

bool CustomUpgrades::UsesCustom(const SaveState& s) {
    if (s.values.count(kOnKey)) return true;
    return std::any_of(s.plots.begin(), s.plots.end(), [](const auto& p) { return !p.second.owned.empty(); });
}

// A save game is running (not the title screen, where _currentGameMetadata keeps the last game).
bool CustomUpgrades::InGame() {
    if (offScGameModel_) {
        bool ok = false;
        Il2CppObject* sc = Invoke(sceneCtxInstMi_, nullptr, {}, "SceneContext.Instance", &ok);
        if (!ok || !unity::IsAlive(sc) || !unity::Field<Il2CppObject*>(sc, offScGameModel_)) return false;
    }
    return !CurrentGameName().empty();
}

// Read-only: does the sidecar the loaded save would use (same order as LoadSidecar) say custom
// upgrades are on for it? Never touches state_.
bool CustomUpgrades::SaveLocked(const std::string& game, unsigned long long number) {
    if (Il2CppObject* asd = AutoSave(); asd && IsA(asd, autoSaveClass_) && offAsdProvider_) {
        auto p = ProviderSavePath(unity::Field<Il2CppObject*>(asd, offAsdProvider_));
        if (!p.empty()) savePath_ = p;
    }
    auto locked = [&](const std::filesystem::path& p) {
        SaveState tmp;
        SidecarMeta m;
        std::string err;
        return ReadSidecarFile(p, tmp, m, err) && UsesCustom(tmp);
    };
    std::error_code ec;
    std::string save;
    if (number && FindExactRotation(game, number, save)) return locked(RotationPath(save));
    if (const auto p = LegacyPath(game); std::filesystem::exists(p, ec)) return locked(p);
    std::filesystem::path p;
    SidecarMeta m;
    return FindOlderRotation(game, number, save, p, m) && locked(p);
}

void CustomUpgrades::SetLocked(bool on) {
    if (on == locked_) return;
    locked_ = on;
    if (on) settings::Lock(kModule, "enabled", 1.0, kLockReason);
    else settings::Unlock(kModule, "enabled");
}

// Through the setting, like the switch: the module manager does the switch in a posted job,
// never inside this frame callback.
void CustomUpgrades::SwitchModule(bool on, const char* why) {
    SR2_INFO("[{}] {} - turning custom upgrades {}", kModule, why, on ? "on" : "off");
    main_thread::Post([on] { settings::Set(kModule, "enabled", on ? 1.0 : 0.0); });
}

void CustomUpgrades::GateTick() {
    if (!main_thread::IsMainThread()) return;
    const bool inGame = InGame();
    if (!On()) {
        SetLocked(false);
        autoOn_ = false;
        if (!inGame) {
            polledGame_.clear();
            polledNumber_ = 0;
            return;
        }
        std::string game;
        unsigned long long number = 0;
        if (!CurrentMetadata(AutoSave(), game, number)) game = CurrentGameName();
        if (game.empty() || (game == polledGame_ && number == polledNumber_)) return;
        polledGame_ = game;
        polledNumber_ = number;
        if (!SaveLocked(game, number)) return;
        autoOn_ = true;
        SwitchModule(true, std::format("save game '{}' (save #{}) has custom upgrades", game, number).c_str());
        return;
    }
    polledGame_.clear();
    polledNumber_ = 0;
    if (!inGame || !EnsureLoaded()) {
        SetLocked(false);
        if (autoOn_) {
            autoOn_ = false;
            SwitchModule(false, "left the save game that has custom upgrades");
        }
        return;
    }
    if (!UsesCustom(state_)) {
        if (autoOn_) {  // on only for the save before; this one has none
            autoOn_ = false;
            SetLocked(false);
            SwitchModule(false, std::format("save game '{}' has no custom upgrades", state_.game).c_str());
            return;
        }
        state_.values[kOnKey] = "1";
        SR2_INFO("[{}] custom upgrades are on with save game '{}' loaded: they stay on for it (saved with the next "
                 "game save)", kModule, state_.game);
    }
    SetLocked(true);
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
    if (req.henCollector && it != state_.plots.end()) {
        req.keepHens = ClampKeepHens(it->second.henKeepHens);
        req.keepRoosters = ClampKeepRoosters(it->second.henKeepRoosters);
    }
    return true;
}

// Part `step` (0 .. kWorldSteps-1) of the world tick; FrameCheck runs one part per frame, in this order.
// Returns false (and spends no frame) when the part is not there. No save game: the tick ends at once.
bool CustomUpgrades::WorldStep(int step) {
    if (!main_thread::IsMainThread() || !EnsureLoaded()) {  // no save game: nothing to build
        worldStep_ = kWorldSteps;
        return true;
    }
    if (sr2::core::ModuleFaulted(kModule)) return false;
    cleaned_ = false;
    switch (step) {
    case 0:
        if (!world_.Ready()) return false;
        {
            SR2_PERF_DETAIL("custom_upgrades.world: coops");
            world_.Tick([&](Il2CppObject* plot, custom_upgrades::world::CoopRequest& req) { return WorldQuery(plot, req); },
                        [&]() { return LookupDirector(); });
        }
        return true;
    case 1:
        if (!quantum_.Ready()) return false;
        {
            SR2_PERF_DETAIL("custom_upgrades.world: quantum links");
            quantum_.Tick();
        }
        return true;
    case 2:
        if (!dj_.Ready()) return false;
        {
            SR2_PERF_DETAIL("custom_upgrades.world: dj buildables");
            dj_.Tick();
        }
        return true;
    case 3:
        if (!radioShop_.Ready()) return false;
        {
            SR2_PERF_DETAIL("custom_upgrades.world: radio shop");
            radioShop_.Tick();
        }
        return true;
    case 4:
        if (!drones_.Ready()) return false;
        {
            SR2_PERF_DETAIL("custom_upgrades.world: drone sources");
            drones_.Tick(world_.Ready() ? world_.HenStorages() : std::vector<Il2CppObject*>{},
                         custom_upgrades::quantum::Available(), custom_upgrades::quantum::Placed());
        }
        return true;
    case 5:
        if (!programs_.Ready()) return false;
        {
            SR2_PERF_DETAIL("custom_upgrades.world: drone programs");
            programs_.Tick();  // after the sources
        }
        return true;
    case 6: {
        if (!silo_.Ready()) return false;
        SR2_PERF_DETAIL("custom_upgrades.world: silo buttons");
        // Silos that own the Sell-All Button (the button checks the plot type itself).
        const UpgradeDef* d = FindDef(SILO_SELL_ALL);
        std::vector<std::string> wanted;
        for (const auto& [id, ps] : state_.plots)
            if (d && ps.type == kPlotSilo && ps.owned.count(d->key)) wanted.push_back(id);
        silo_.Tick(wanted, [&]() { return LookupDirector(); });
        return true;
    }
    case 7: {
        if (!radioButtons_.Ready()) return false;
        SR2_PERF_DETAIL("custom_upgrades.world: radio buttons");
        // Corrals that own the Corral Radio (round 11: a blue button on the free corner opens its playlist).
        const UpgradeDef* d = FindDef(CORRAL_RADIO);
        std::vector<std::string> wanted;
        for (const auto& [id, ps] : state_.plots)
            if (d && ps.type == kPlotCorral && ps.owned.count(d->key)) wanted.push_back(id);
        radioButtons_.Tick(wanted, [&]() { return LookupDirector(); });
        return true;
    }
    case 8:
        if (!props_.Ready()) return false;
        {
            SR2_PERF_DETAIL("custom_upgrades.world: plot props");
            props_.Tick(WantedProps(), [&]() { return LookupDirector(); });
        }
        return true;
    case 9:
        if (!capacityOk_) return false;
        {
            SR2_PERF_DETAIL("custom_upgrades.world: silo capacity");
            CapacityTick();
        }
        return true;
    case 10:  // Shadow Catcher before the vacpack: it finds the Shadow slime the vacpack tick's card and stash need.
        if (!shadow_.Ready()) return false;
        {
            SR2_PERF_DETAIL("custom_upgrades.world: shadow catcher");
            shadow_.Tick();
        }
        return true;
    case 11:
        if (!vacpack_.Ready()) return false;
        {
            SR2_PERF_DETAIL("custom_upgrades.world: vacpack");
            vacpack_.Tick();
        }
        return true;
    case 12:
        if (!feeder_.Ready()) return false;
        {
            SR2_PERF_DETAIL("custom_upgrades.world: feeder locks");
            FeederLockTick();
        }
        return true;
    case 13:
        if (!feederLights_.Ready()) return false;
        {
            SR2_PERF_DETAIL("custom_upgrades.world: feeder lights");
            // The save's plot list (GameModel.landPlots) instead of a scene-wide search (round 18: 22-32 ms hitches).
            Il2CppObject* gm = nullptr;
            if (offScGameModel_) {
                bool ok = false;
                Il2CppObject* sc = Invoke(sceneCtxInstMi_, nullptr, {}, "SceneContext.Instance", &ok);
                gm = ok && unity::IsAlive(sc) ? unity::Field<Il2CppObject*>(sc, offScGameModel_) : nullptr;
            }
            feederLights_.Tick(unity::IsAlive(gm) ? gm : nullptr);
        }
        return true;
    default:
        return false;
    }
}

// Plots whose owned tiers have props: Faster Hens I / II (hay; II adds nests), Sprinkler II / III
// (sprinkler heads), Ash Booster (fire glow). The plot type is checked again on the live plot.
std::vector<custom_upgrades::world::PropRequest> CustomUpgrades::WantedProps() const {
    namespace w = custom_upgrades::world;
    auto owns = [](const PlotState& ps, int32_t id) {
        const UpgradeDef* d = FindDef(id);
        return d && ps.owned.count(d->key) > 0;
    };
    std::vector<w::PropRequest> out;
    for (const auto& [id, ps] : state_.plots) {
        uint32_t kinds = 0;
        if (owns(ps, COOP_HENS_1) || owns(ps, COOP_HENS_2)) kinds |= w::kPropCoopHay;
        if (owns(ps, COOP_HENS_2)) kinds |= w::kPropCoopNests;
        if (owns(ps, GARDEN_GROWTH_1) || owns(ps, GARDEN_GROWTH_2)) kinds |= w::kPropGardenSprinkler;
        if (owns(ps, GARDEN_GROWTH_2)) kinds |= w::kPropGardenSprinkler2;
        if (owns(ps, INCIN_ASH_BOOST)) kinds |= w::kPropIncinGlow;
        if (owns(ps, CORRAL_MUSIC_2)) kinds |= w::kPropCorralMusic;
        // Silo Capacity II-V: one more ground tank per tier (a higher tier implies the lower ones).
        const bool cap5 = owns(ps, SILO_CAPACITY_5), cap4 = cap5 || owns(ps, SILO_CAPACITY_4),
                   cap3 = cap4 || owns(ps, SILO_CAPACITY_3);
        if (cap3 || owns(ps, SILO_CAPACITY_2)) kinds |= w::kPropSiloTank2;
        if (cap3) kinds |= w::kPropSiloTank3;
        if (cap4) kinds |= w::kPropSiloTank4;
        if (cap5) kinds |= w::kPropSiloTank5;
        if (kinds) out.push_back({id, kinds});
    }
    return out;
}

// ---------------------------------------------------------------- silo capacity --

// The live LandPlot of a plot id: GameModel.GetLandPlotModel(id) (null if unknown) -> gameObj (the
// LandPlotLocation object) -> GetComponentInChildren<LandPlot>.
Il2CppObject* CustomUpgrades::LivePlot(Il2CppObject* gameModel, const std::string& id) {
    bool ok = false;
    Il2CppObject* model = Invoke(getPlotModelMi_, gameModel, {unity::NewString(id)}, "GameModel.GetLandPlotModel", &ok);
    if (!ok || !model) return nullptr;
    Il2CppObject* loc = unity::Field<Il2CppObject*>(model, offPlotModelObj_);
    if (!unity::IsAlive(loc)) return nullptr;
    Il2CppObject* tr = unity::TransformOf(loc);
    if (!unity::IsAlive(tr)) return nullptr;
    bool inactive = true;
    Il2CppObject* plot = Invoke(compInChildrenMi_, tr, {unity::TypeOf(plotClass_), &inactive},
                                "GetComponentInChildren(LandPlot)", &ok);
    return ok && unity::IsAlive(plot) && IsA(plot, plotClass_) ? plot : nullptr;
}

// What the silo would hold per slot without our tiers: the vanilla capacity upgrade's amount (200)
// when owned, else the slot definition's _maxAmount (100).
int CustomUpgrades::VanillaCapacity(Il2CppObject* plot, Il2CppObject* upgrader, Il2CppObject* silo) {
    if (VanillaOwned(plot, kUpgStorageCapacity)) {
        const int amount = unity::Field<int32_t>(upgrader, offUpgAmount_);
        if (amount > 0) return amount;
    }
    void* defs = unity::Field<void*>(silo, offSiloDefs_);
    auto* def = defs && unity::ArrayLength(defs) > 0 ? static_cast<Il2CppObject*>(unity::ArrayAt(defs, 0)) : nullptr;
    const int base = unity::IsAlive(def) ? unity::Field<int32_t>(def, offDefMax_) : 0;
    return base > 0 ? base : kSiloSlotDefault;
}

// Sets every slot of the silo's storage to `target` per slot (target <= 0: its vanilla capacity).
// Returns the capacity now in place, 0 if the silo can't be reached (no model yet, not a silo).
// Lowering never touches the contents: counts above the new max stay (logged).
int CustomUpgrades::ApplyCapacity(Il2CppObject* plot, int target, const std::string& id, const char* why) {
    if (!unity::IsAlive(plot) || !IsA(plot, plotClass_) || PlotType(plot) != kPlotSilo) return 0;
    Il2CppObject* upgrader = getComponent_(plot, unity::TypeOf(storageUpgraderClass_));
    if (!unity::IsAlive(upgrader) || !IsA(upgrader, storageUpgraderClass_)) return 0;
    Il2CppObject* silo = unity::Field<Il2CppObject*>(upgrader, offUpgSilo_);
    if (!unity::IsAlive(silo) || !IsA(silo, siloStorageClass_)) return 0;
    if (target <= 0) target = VanillaCapacity(plot, upgrader, silo);
    bool ok = false;
    Il2CppObject* ammo = Invoke(localAmmoMi_, silo, {}, "SiloStorage.get_LocalAmmo", &ok);
    if (!ok || !ammo || !unity::Field<Il2CppObject*>(ammo, offAmmoModel_)) return 0;  // not set up yet (load)
    Il2CppObject* slots = Invoke(slotsMi_, ammo, {}, "AmmoSlotManager.get_Slots", &ok);
    const int32_t n = ok ? static_cast<int32_t>(unity::ArrayLength(slots)) : 0;
    if (n <= 0) return 0;
    int32_t first = 0;
    const int cur = Unbox<int32_t>(Invoke(capSlotMaxMi_, ammo, {&first}, "AmmoSlotManager.GetSlotMaxCount", &ok));
    if (!ok) return 0;
    if (cur == target) return cur;
    int32_t value = target;
    Invoke(capUpdateMi_, ammo, {&value}, "AmmoSlotManager.UpdateMaxCount", &ok);
    if (!ok) return 0;
    int over = 0;
    if (target < cur)
        for (int32_t i = 0; i < n && i < 64; ++i) {
            const int c = Unbox<int32_t>(Invoke(slotCountMi_, ammo, {&i}, "AmmoSlotManager.GetSlotCount", &ok));
            if (ok && c > target) over += c - target;
        }
    if (over > 0)
        SR2_WARN("[{}] silo capacity on plot '{}': {} -> {} per slot ({}); {} item(s) above it stay in the silo (nothing is "
                 "removed), it takes no more until they are taken out",
                 kModule, id, cur, target, why, over);
    else SR2_INFO("[{}] silo capacity on plot '{}': {} -> {} per slot ({})", kModule, id, cur, target, why);
    return target;
}

// ~1 Hz (world tick): every silo with a Silo Capacity tier gets that tier's capacity; one that had it
// and lost it (sold is impossible, but a plot rebuilt as another type / state reset) goes back to
// vanilla. Vanilla ApplyUpgrades (load, every purchase on the plot) writes 200 again: re-applied here
// within a second, and no item is ever removed by a lower max in between.
void CustomUpgrades::CapacityTick() {
    std::map<std::string, int> wanted;
    for (const auto& [id, ps] : state_.plots) {
        if (ps.type != kPlotSilo) continue;
        int cap = 0;
        for (const UpgradeDef& d : kDefs)
            if (IsSiloCapacity(d) && ps.owned.count(d.key)) cap = std::max(cap, static_cast<int>(d.mult));
        if (cap > 0) wanted[id] = cap;
    }
    if (wanted.empty() && raisedCap_.empty()) return;
    bool ok = false;
    Il2CppObject* sc = Invoke(sceneCtxInstMi_, nullptr, {}, "SceneContext.Instance", &ok);
    Il2CppObject* gm = ok && unity::IsAlive(sc) ? unity::Field<Il2CppObject*>(sc, offScGameModel_) : nullptr;
    if (!gm) return;
    for (const auto& [id, cap] : wanted) {
        Il2CppObject* plot = LivePlot(gm, id);
        if (!plot) continue;
        if (ApplyCapacity(plot, cap, id, "Silo Capacity tier") > 0) raisedCap_[id] = cap;
        if (sr2::core::ModuleFaulted(kModule)) return;
    }
    for (auto it = raisedCap_.begin(); it != raisedCap_.end();) {
        if (wanted.count(it->first)) {
            ++it;
            continue;
        }
        if (Il2CppObject* plot = LivePlot(gm, it->first)) ApplyCapacity(plot, 0, it->first, "tier gone");
        it = raisedCap_.erase(it);
    }
}

// Module off / faulted: every silo we raised goes back to its vanilla capacity (contents stay).
void CustomUpgrades::RestoreCapacities(const char* why) {
    if (raisedCap_.empty() || !capacityOk_ || !main_thread::IsMainThread()) {
        raisedCap_.clear();
        return;
    }
    bool ok = false;
    Il2CppObject* sc = Invoke(sceneCtxInstMi_, nullptr, {}, "SceneContext.Instance", &ok);
    Il2CppObject* gm = ok && unity::IsAlive(sc) ? unity::Field<Il2CppObject*>(sc, offScGameModel_) : nullptr;
    if (gm)
        for (const auto& [id, cap] : raisedCap_)
            if (Il2CppObject* plot = LivePlot(gm, id)) ApplyCapacity(plot, 0, id, why);
    raisedCap_.clear();
}

// ---------------------------------------------------------------- silo button --

SiloSummary CustomUpgrades::SellAllNow(Il2CppObject* plot, const std::string& id, const char* via, bool queueNotices,
                                       const custom_upgrades::ui::Receipt* plan) {
    std::string log;
    SiloSummary s = Summarize(plot, true, &log, nullptr, plan);
    if (!s.marketAvailable) SR2_INFO("[{}] sell all ({}, plot '{}'): Plort Market not available", kModule, via, id);
    else if (s.marketClosed) SR2_INFO("[{}] sell all ({}, plot '{}'): Plort Market closed, nothing sold", kModule, via, id);
    else SR2_INFO("[{}] sell all ({}, plot '{}'): sold {} plort(s) for {} Newbucks{}{}", kModule, via, id, s.plorts, s.value,
                  log.empty() ? "" : ": ", log);
    if (plan && s.marketAvailable && !s.marketClosed && (s.plorts != plan->Count() || s.value != plan->Total()))
        SR2_WARN("[{}] sell all ({}, plot '{}'): the receipt said {} plort(s) for {} Newbucks, sold {} for {}", kModule, via,
                 id, plan->Count(), plan->Total(), s.plorts, s.value);
    // One HUD notice per plort type (icon, name, count) like the drones' "plorts sold" ones. The HUD
    // drops notices while it is suppressed (a menu, our receipt just closing), so `queueNotices`
    // queues them anyway (shown once it allows); otherwise only if the HUD list is there now, and
    // the caller shows its own text when nothing was queued.
    if (!s.sold.empty() && notices_.Ready() && (queueNotices || notices_.Available())) {
        notices_.Queue(s.sold);
        s.noticed = true;
    }
    return s;
}

bool CustomUpgrades::OnSiloButton(Il2CppObject* activator) {
    if (!main_thread::IsMainThread()) return false;
    if (OnHenButton(activator)) return true;
    if (OnRadioButton(activator)) return true;
    if (!silo_.IsOurs(activator)) return false;
    if (!On()) return true;  // swallowed (the button is removed with the next frame)
    if (receipt_.IsOpen()) return true;  // a receipt is up (input is locked; never expected)
    Il2CppObject* plot = silo_.OwnerPlot(activator);
    if (!plot || !silo_.Press(activator)) return true;  // no silo / pressed again within 0.4 s
    const UpgradeDef* d = FindDef(SILO_SELL_ALL);
    std::string id;
    if (!d || !OwnsKey(plot, *d, &id)) {
        SR2_INFO("[{}] sell all: pressed on a silo without the Sell-All Button - button removed", kModule);
        worldAccum_ = kWorldTickSeconds;  // remove the stale button
        return true;
    }
    // Round 10 ("remove the overlay on the button ... when no plorts to sell still open the dialog"): no
    // text drawn over the button any more; the receipt always opens, an empty / closed-market one with a
    // note and Sell disabled. Nothing is sold until the player confirms it.
    if (receipt_.Ready()) {
        custom_upgrades::ui::Receipt quote;
        Summarize(plot, false, nullptr, &quote);
        const char* note = !quote.marketAvailable ? "The Plort Market can't be reached right now."
                           : quote.marketClosed   ? "The Plort Market is closed - nothing can be sold now."
                           : quote.lines.empty()  ? "There are no plorts in this silo."
                                                  : "";
        if (OpenReceipt(plot, activator, id, quote, note)) return true;
        SR2_WARN("[{}] sell all (plot '{}'): the receipt could not be opened - selling at once", kModule, id);
        if (!quote.marketAvailable || quote.marketClosed || quote.lines.empty()) return true;
    }
    const SiloSummary s = SellAllNow(plot, id, "world button", false);
    if (s.plorts > 0 && !s.noticed)  // no HUD notices: the currency popup (TrySell showUi) is the feedback
        SR2_INFO("[{}] sell all (plot '{}'): sold without HUD notices", kModule, id);
    return true;
}

// ---------------------------------------------------------------- sell-all receipt --

bool CustomUpgrades::OpenReceipt(Il2CppObject* plot, Il2CppObject* activator, const std::string& id,
                                 const custom_upgrades::ui::Receipt& quote, const char* note) {
    if (!receipt_.Open(quote, note ? note : "")) return false;
    receiptPlot_ = plot;
    receiptButton_ = activator;
    if (Il2CppGCHandle h = unity::NewGCHandle(plot)) receiptHandles_.push_back(h);
    if (Il2CppGCHandle h = unity::NewGCHandle(activator)) receiptHandles_.push_back(h);
    receiptPlotId_ = id;
    receiptQuote_ = quote;
    SR2_INFO("[{}] sell all (plot '{}'): receipt shown, {} plort(s) in {} type(s) for {} Newbucks", kModule, id,
             quote.Count(), quote.lines.size(), quote.Total());
    return true;
}

void CustomUpgrades::CloseReceipt(const char* why) {
    receipt_.Close(why);
    receiptPlot_ = receiptButton_ = nullptr;
    receiptPlotId_.clear();
    receiptQuote_ = {};
    std::vector<Il2CppGCHandle> doomed;
    doomed.swap(receiptHandles_);
    if (!doomed.empty()) main_thread::Post([doomed] {  // freed by a posted job, never inline
        for (Il2CppGCHandle h : doomed) unity::FreeGCHandle(h);
    });
}

// Every frame while the receipt is open. Sell re-quotes the silo: only the very receipt the player
// saw is sold (exact counts per type at the shown prices); anything else (prices, contents, the
// market) refreshes the dialog instead and waits for a new confirmation.
void CustomUpgrades::ReceiptTick(float dt) {
    using Result = custom_upgrades::ui::SiloReceipt::Result;
    const Result r = receipt_.Tick(dt);
    if (r == Result::kNone) return;
    const std::string id = receiptPlotId_;
    if (r == Result::kCancel) {
        SR2_INFO("[{}] sell all (plot '{}'): receipt cancelled, nothing sold", kModule, id);
        CloseReceipt("cancelled");
        return;
    }
    Il2CppObject* plot = receiptPlot_;
    const UpgradeDef* d = FindDef(SILO_SELL_ALL);
    std::string ownedId;
    if (!unity::IsAlive(plot) || !IsA(plot, plotClass_) || !d || !OwnsKey(plot, *d, &ownedId) || ownedId != id) {
        SR2_WARN("[{}] sell all (plot '{}'): the silo or its Sell-All Button is gone - nothing sold", kModule, id);
        CloseReceipt("silo gone");
        return;
    }
    custom_upgrades::ui::Receipt now;
    Summarize(plot, false, nullptr, &now);
    if (!now.Same(receiptQuote_)) {
        const bool canSell = now.marketAvailable && !now.marketClosed && !now.lines.empty();
        const char* note = !now.marketAvailable ? "The Plort Market can't be reached right now."
                           : now.marketClosed   ? "The Plort Market just closed - nothing can be sold now."
                           : now.lines.empty()  ? "There are no plorts left in this silo."
                                                : "Prices or contents changed - this is the new receipt. Sell again to confirm.";
        SR2_INFO("[{}] sell all (plot '{}'): receipt changed before the sale ({} plort(s) for {} -> {} for {}) - refreshed, "
                 "nothing sold", kModule, id, receiptQuote_.Count(), receiptQuote_.Total(), now.Count(), now.Total());
        receiptQuote_ = now;
        receipt_.Refresh(now, note, canSell);
        if (!receipt_.IsOpen()) CloseReceipt("refresh failed");
        return;
    }
    // Close first: the input lock (paused time) goes, then the sale; its HUD notices are queued
    // and show as soon as the HUD takes them.
    // No text over the button afterwards (round 10): the HUD notices and the currency popup show the sale.
    const custom_upgrades::ui::Receipt plan = receiptQuote_;
    CloseReceipt("confirmed");
    SellAllNow(plot, id, "receipt", true, &plan);
}

// The Hen Collector's own button (the elder collector's, kept on the clone with a null Collector,
// coop_world.cpp): collects the excess chickens now, like the vanilla collector buttons. Button
// animation, press cue and the 0.4 s rate limit as on the silo button (SiloButtons::Press works on
// any PlortCollectorActivator). Round 15 (user: "Remove the popup when manually clicking the
// collector, just start sucking"): no notification and no text; the cyclone spins at once and the
// result goes to the log only.
bool CustomUpgrades::OnHenButton(Il2CppObject* activator) {
    if (!world_.Ready()) return false;
    Il2CppObject* plot = world_.HenButtonPlot(activator);
    if (!plot) return false;
    if (!On()) return true;  // swallowed (the clone is removed with the next world tick)
    if (silo_.Ready() && !silo_.Press(activator)) return true;  // pressed again within 0.4 s
    const custom_upgrades::world::QueryFn query = [&](Il2CppObject* p, custom_upgrades::world::CoopRequest& req) {
        return state_.loaded && WorldQuery(p, req);
    };
    const std::string text = world_.Activate(plot, query);
    SR2_INFO("[{}] hen collector button: {}", kModule, text);
    return true;
}

// The menu icon of a custom card (the clone's _icon: the user's drawing or the borrowed vanilla icon), from
// the plot menus built this session; nullptr before that menu was opened (the notice then has no icon).
Il2CppObject* CustomUpgrades::CardIcon(int32_t id) {
    for (auto& inj : injections_) {
        if (inj->failed) continue;
        for (const Injected& in : inj->items)
            if (in.def && in.def->id == id && unity::IsAlive(in.item)) {
                Il2CppObject* icon = unity::Field<Il2CppObject*>(in.item, offItemIcon_);
                return unity::IsAlive(icon) ? icon : nullptr;
            }
    }
    return nullptr;
}

// One of the game's notification pop-ups (the drone-style entry with our text, sale_notify.h QueueText).
// false = the HUD can't show it now (no list, no text sink): the caller falls back to its own feedback.
bool CustomUpgrades::Notify(Il2CppObject* icon, const std::string& text) {
    if (text.empty() || !notices_.TextReady() || !notices_.Available()) return false;
    return notices_.QueueText(icon, text);
}

// The Corral Radio's button (round 11, user: "Add a button in the last empty corner (color it blue; like
// you did for silo's button) and let the button open a menu to build a queue from"): a second
// SiloButtons with the blue look on the corral's free corner. A press opens slime_dj's playlist menu for
// the corral (slime_dj/api.h OpenRadioMenu); when Slime DJ can't, the reason is a game notification
// (round 12; the text above the button only when the HUD can't take one). Round 14 ("Just show 'Slime DJ
// is off'"): the reason alone, no "Corral Radio: " in front (the card's icon says which).
bool CustomUpgrades::OnRadioButton(Il2CppObject* activator) {
    if (!radioButtons_.Ready() || !radioButtons_.IsOurs(activator)) return false;
    if (!On()) return true;  // swallowed (the button is removed with the next frame)
    Il2CppObject* plot = radioButtons_.OwnerPlot(activator);
    if (!plot || !radioButtons_.Press(activator)) return true;  // no corral / pressed again within 0.4 s
    const UpgradeDef* d = FindDef(CORRAL_RADIO);
    std::string id;
    if (!d || !OwnsKey(plot, *d, &id)) {
        SR2_INFO("[{}] corral radio: pressed on a corral without the Corral Radio - button removed", kModule);
        worldAccum_ = kWorldTickSeconds;
        return true;
    }
    std::string why;
    if (sr2::modules::slime_dj::OpenRadioMenu(plot, &why)) {
        SR2_INFO("[{}] corral radio (plot '{}'): playlist menu requested", kModule, id);
    } else {
        SR2_INFO("[{}] corral radio (plot '{}'): no playlist menu ({})", kModule, id, why);
        if (!Notify(CardIcon(CORRAL_RADIO), why)) radioButtons_.Message(activator, why, false);
    }
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
    if (!main_thread::IsMainThread() || !sliderHens_.Ready() || !sliderRoosters_.Ready()) return;
    DestroySliders();  // every bind (selection change, RebuildUI) replaces the sliders
    if (!vm || !offVmDescription_ || !menuRoot_ || !unity::IsAlive(menuRoot_) || !IsA(menuRoot_, rootClass_)) return;
    Il2CppObject* desc = unity::Field<Il2CppObject*>(vm, offVmDescription_);
    if (!desc) return;
    Il2CppObject* plot = unity::Field<Il2CppObject*>(menuRoot_, offActivator_);
    Il2CppObject* config = unity::Field<Il2CppObject*>(menuRoot_, offMenuConfig_);
    if (unity::IsAlive(plot) && IsA(plot, plotClass_) && PlotType(plot) == kPlotCorral) {
        FeederCardBind(view, desc, plot, config);
        return;
    }
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
    if (!d || !OwnsKey(plot, *d, &id)) return;  // not bought (yet): no sliders
    auto it = state_.plots.find(id);
    if (it == state_.plots.end()) return;

    // Two sliders side by side: hens (left, builds the row) and roosters (right), mouse only (user:
    // "remove the icons and arrow key functionality for now, will deal with gamepads later"). By key
    // (never a pointer into the map): the state may be reloaded while the menu is open.
    auto onChange = [this, id, game = state_.game](bool roosters) {
        return [this, id, game, roosters](int v) {
            if (!state_.loaded || state_.game != game) return;
            auto p = state_.plots.find(id);
            if (p == state_.plots.end() || !p->second.owned.count("coop_hen_collector")) return;
            if (roosters) p->second.henKeepRoosters = ClampKeepRoosters(v);
            else p->second.henKeepHens = ClampKeepHens(v);
            SR2_DEBUG("[{}] hen collector on plot '{}': keep {} {} (saved with the next game save)", kModule, id, v,
                      roosters ? "roosters" : "hens");
        };
    };
    custom_upgrades::ui::SliderSpec hens;
    hens.name = "HenCollectorHens";
    hens.min = custom_upgrades::world::kHenKeepHensMin;
    hens.max = custom_upgrades::world::kHenKeepHensMax;
    hens.value = ClampKeepHens(it->second.henKeepHens);
    hens.label = [](int v) { return std::format("Keep hens: {}", v); };
    hens.onChange = onChange(false);
    hens.column = 0;
    hens.columns = 2;
    hens.keys = false;
    custom_upgrades::ui::SliderSpec roosters = hens;
    roosters.name = "HenCollectorRoosters";
    roosters.min = custom_upgrades::world::kHenKeepRoostersMin;
    roosters.max = custom_upgrades::world::kHenKeepRoostersMax;
    roosters.value = ClampKeepRoosters(it->second.henKeepRoosters);
    roosters.label = [](int v) { return std::format("Keep roostros: {}", v); };
    roosters.onChange = onChange(true);
    roosters.column = 1;
    if (sliderHens_.Create(view, hens)) sliderRoosters_.Create(view, roosters, &sliderHens_);
}

void CustomUpgrades::SliderTick(float dt) {
    if (!main_thread::IsMainThread()) return;
    sliderHens_.Tick(dt);
    sliderRoosters_.Tick(dt);
    sliderFeeder_.Tick(dt);
}

// ---------------------------------------------------------------- auto-feeder lock --

// Round 16 (user on the round 15 food picker: "No no no no no, it should be just a toggle, 'Leave last food
// item in'"): while the corral's OWNED vanilla Auto-Feeder card is selected, an on/off switch sits in the
// details panel (the cost row's slot, like the Hen Collector limits): a two-position PlotMenuSlider, half
// the panel wide (off = handle left, on = handle right with the pink fill). Mouse: click / drag it; pad A /
// Enter (the menu's Submit, which does nothing on an owned card) flips it. Saved per plot in the sidecar
// (PlotValue feeder::kKeepKey, "1" = on, "" = off; round 15's food names read as on), applied at once
// (FeederLock::SetNow) and by the world tick.
void CustomUpgrades::FeederCardBind(Il2CppObject* view, Il2CppObject* desc, Il2CppObject* plot, Il2CppObject* config) {
    namespace fl = custom_upgrades::feeder;
    if (!feeder_.Ready() || !sliderFeeder_.Ready() || !VanillaOwned(plot, kUpgFeeder)) return;
    // The vanilla Feeder card of this menu: its own _description instance (pointer identity).
    bool feederCard = false;
    for (auto& inj : injections_) {
        if (inj->config != config || !inj->list) continue;
        for (size_t i = 0, n = unity::ListCount(inj->list); i < n && i < 64 && !feederCard; ++i) {
            auto* item = static_cast<Il2CppObject*>(unity::ListAt(inj->list, i));
            if (!unity::IsAlive(item) || !IsA(item, upgItemClass_)) continue;
            feederCard = unity::Field<int32_t>(item, offItemUpgrade_) == kUpgFeeder &&
                         unity::Field<Il2CppObject*>(item, offItemDesc_) == desc;
        }
    }
    if (!feederCard) return;
    const std::string id = PlotId(plot);
    if (id.empty() || !EnsureLoaded() || !feeder_.HasFeeder(plot)) return;

    custom_upgrades::ui::SliderSpec spec;
    spec.name = "FeederKeepLast";
    spec.min = 0;
    spec.max = 1;
    spec.value = PlotValueOf(plot, fl::kKeepKey).empty() ? 0 : 1;
    spec.keys = false;  // left / right belong to the menu's grid; Submit flips it (OnFeederSubmit)
    spec.column = 0;
    spec.columns = 2;
    spec.label = [](int v) { return std::string(v ? "Leave last food item in: On" : "Leave last food item in: Off"); };
    // By plot id (never a pointer into the state): the save may be reloaded while the menu is open.
    spec.onChange = [this, id, game = state_.game](int v) {
        if (!state_.loaded || state_.game != game) return;
        bool ok = false;
        Il2CppObject* sc = Invoke(sceneCtxInstMi_, nullptr, {}, "SceneContext.Instance", &ok);
        Il2CppObject* gm = ok && offScGameModel_ && unity::IsAlive(sc) ? unity::Field<Il2CppObject*>(sc, offScGameModel_) : nullptr;
        Il2CppObject* live = gm ? LivePlot(gm, id) : nullptr;
        if (!live || PlotType(live) != kPlotCorral) return;
        SetPlotValueOf(live, fl::kKeepKey, v ? "1" : "");
        feeder_.SetNow(live, v != 0, id);
    };
    sliderFeeder_.Create(view, spec);
}

// ListUI.Submit while the switch is shown: flip it (one flip per frame; a popup keeps the key).
bool CustomUpgrades::OnFeederSubmit() {
    if (!main_thread::IsMainThread() || !sliderFeeder_.Active()) return false;
    const unsigned long long tick = main_thread::TickCount();
    if (tick == feederSubmitTick_) return true;  // the same press again
    if (!sliderFeeder_.Flip()) return false;
    feederSubmitTick_ = tick;
    SR2_DEBUG("[{}] auto-feeder switch flipped by Submit (pad A / Enter): {}", kModule, sliderFeeder_.Value() ? "on" : "off");
    return true;
}

// World tick part: the corrals with the switch on (sidecar values) -> their live plots.
void CustomUpgrades::FeederLockTick() {
    namespace fl = custom_upgrades::feeder;
    std::vector<fl::KeepRequest> keep;
    Il2CppObject* gm = nullptr;
    bool tried = false;
    for (const auto& [id, ps] : state_.plots) {
        if (ps.type != kPlotCorral) continue;
        auto v = ps.values.find(fl::kKeepKey);
        if (v == ps.values.end() || v->second.empty()) continue;
        if (!tried) {
            tried = true;
            bool ok = false;
            Il2CppObject* sc = Invoke(sceneCtxInstMi_, nullptr, {}, "SceneContext.Instance", &ok);
            gm = ok && offScGameModel_ && unity::IsAlive(sc) ? unity::Field<Il2CppObject*>(sc, offScGameModel_) : nullptr;
        }
        if (!gm) break;
        Il2CppObject* plot = LivePlot(gm, id);
        if (plot && PlotType(plot) == kPlotCorral) keep.push_back({plot, id});
        if (sr2::core::ModuleFaulted(kModule)) return;
    }
    feeder_.Tick(keep);
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
    PlotState& ps = state_.plots[id];
    ps.type = d->plotType;
    ps.owned.insert(d->key);
    Bump();
    worldAccum_ = kWorldTickSeconds;  // world object / button / props: built at the next frame
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

// sell = false: count only (`quote`, optional, gets one line per plort type: count, current price;
// sorted by type name). sell = true: TrySell + Decrement per slot; with `plan` (the receipt the
// player confirmed) at most the planned count per type is sold, nothing else.
SiloSummary CustomUpgrades::Summarize(Il2CppObject* plot, bool sell, std::string* log, custom_upgrades::ui::Receipt* quote,
                                      const custom_upgrades::ui::Receipt* plan) {
    SiloSummary s;
    if (quote) *quote = {};
    std::vector<std::pair<Il2CppObject*, int>> remaining;  // plan: type -> count still to sell
    if (plan)
        for (const auto& l : plan->lines) remaining.emplace_back(l.type, l.count);
    Il2CppObject* econ = EconDirector();
    if (!econ) return s;
    s.marketAvailable = true;
    bool ok = false;
    s.marketClosed = Unbox<bool>(Invoke(shutdownMi_, econ, {}, "PlortEconomyDirector.IsMarketShutdown", &ok));
    if (!ok) {
        s.marketAvailable = false;
        return s;
    }
    if (quote) {
        quote->marketAvailable = true;
        quote->marketClosed = s.marketClosed;
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
                if (quote) {
                    auto it = std::find_if(quote->lines.begin(), quote->lines.end(),
                                           [id](const custom_upgrades::ui::ReceiptLine& l) { return l.type == id; });
                    if (it != quote->lines.end()) it->count += count;  // same type in several slots: same price
                    else quote->lines.push_back({id, count, cur});
                }
                continue;
            }
            if (s.marketClosed) continue;
            std::pair<Il2CppObject*, int>* left = nullptr;
            if (plan) {
                auto it = std::find_if(remaining.begin(), remaining.end(), [id](const auto& e) { return e.first == id; });
                if (it == remaining.end() || it->second <= 0) continue;  // not on the receipt (or done)
                left = &*it;
                count = std::min(count, it->second);
            }
            bool ignoreShutdown = false, showUi = true;
            const bool sold = Unbox<bool>(Invoke(trySellMi_, econ, {id, &count, &ignoreShutdown, &showUi, nullptr},
                                                 "PlortEconomyDirector.TrySell", &ok));
            if (!ok || !sold) continue;
            if (left) left->second -= count;
            // Paid: now take the plorts out of the slot.
            Invoke(decrementMi_, ammo, {&i, &count}, "AmmoSlotManager.Decrement", &ok);
            if (!ok) SR2_WARN("[{}] sold {} x {} but could not empty the silo slot", kModule, count, unity::GetName(id));
            s.plorts += count;
            s.value += static_cast<long long>(cur) * count;
            types.insert(id);
            {
                auto it = std::find_if(s.sold.begin(), s.sold.end(), [id](const auto& e) { return e.first == id; });
                if (it != s.sold.end()) it->second += count;
                else s.sold.emplace_back(id, count);
            }
            if (log) *log += std::format("{}{} x {} @ {}", log->empty() ? "" : ", ", count, unity::GetName(id), cur);
        }
    }
    s.types = static_cast<int>(types.size());
    if (quote)
        std::sort(quote->lines.begin(), quote->lines.end(),
                  [](const custom_upgrades::ui::ReceiptLine& a, const custom_upgrades::ui::ReceiptLine& b) {
                      const std::string na = unity::GetName(a.type), nb = unity::GetName(b.type);
                      return na != nb ? na < nb : a.type < b.type;
                  });
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

std::string CustomUpgrades::CanSellCustom(Il2CppObject* plot, int32_t upg, std::string* brief) {
    auto no = [brief](std::string why, std::string shortWhy) {
        if (brief) *brief = std::move(shortWhy);
        return why;
    };
    const UpgradeDef* d = FindDef(upg);
    if (!d) return no("This can't be sold.", "not sellable");
    // Same rule and brief as sell_upgrades' vanilla silo storage upgrades (a permanent reason: no button).
    if (IsSiloCapacity(*d))
        return no("Silo storage upgrades can't be sold: they change the silo's slots and capacity, and the items "
                  "stored there could be lost.",
                  "silo storage");
    std::string id;
    if (!OwnsKey(plot, *d, &id)) return no("This upgrade is not owned.", "not owned");
    if (d->id == COOP_HEN_COLLECTOR) {
        // Stored chickens are never thrown away: the player vacs them out first.
        int stored = world_.Ready() ? world_.StoredHens(plot) : -1;
        if (stored < 0) {
            auto it = state_.plots.find(id);
            stored = it != state_.plots.end() ? HenCount(it->second) : 0;
        }
        if (stored > 0)
            return no(std::format("Empty the Hen Collector first: {} chicken{} stored. Vac them out of its ports.",
                                  stored, stored == 1 ? " is" : "s are"),
                      "holds chickens");
    }
    // Tiers: an owned upgrade that needs this one must be sold first.
    for (const UpgradeDef& o : kDefs)
        if (o.needs == d->id && OwnsKey(plot, o))
            return no(std::format("Sell '{}' first - it needs this upgrade.", o.title),
                      std::format("sell {} first", o.title));
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
        it->second.henKeepHens = custom_upgrades::world::kHenKeepHensDefault;
        it->second.henKeepRoosters = custom_upgrades::world::kHenKeepRoostersDefault;
    }
    if (it->second.Empty()) state_.plots.erase(it);
    Bump();
    if (IsWorldUpgrade(d->id) && world_.Ready())
        world_.Remove(plot, d->id == COOP_AIR_NET, d->id == COOP_HEN_COLLECTOR);
    if (d->id == SILO_SELL_ALL && silo_.Ready()) silo_.Remove(plot);
    if (d->id == CORRAL_RADIO && radioButtons_.Ready()) radioButtons_.Remove(plot);
    worldAccum_ = kWorldTickSeconds;  // props of a sold tier go at the next frame
    notes += "custom upgrade removed (saved with the next game save)";
    SR2_INFO("[{}] {} removed from plot '{}'", kModule, d->title, id);
    return true;
}

// Only a tier chain on the vanilla upgrade blocks its sale (Music Box II on the Music Box card). Round 12
// ("bought radio and i cant sell speakers I again"): the Corral Radio (not a tier, only bought on top of the
// Music Box) no longer does; it stays owned and listed (Visible), and plays again once the Music Box is back.
std::string CustomUpgrades::VanillaDependentOf(Il2CppObject* plot, int32_t vanillaUpg) {
    if (custom_upgrades::IsCustomUpgrade(vanillaUpg) || !vanillaUpg) return {};
    for (const UpgradeDef& o : kDefs)
        if (o.needs == vanillaUpg && o.family != kNoFamily && OwnsKey(plot, o)) return o.title;
    return {};
}

bool CustomUpgrades::ChainSellTargetOf(Il2CppObject* plot, Il2CppObject* item, int32_t& upgrade,
                                       Il2CppObject*& priceItem, std::string* why) {
    auto no = [why](std::string r) {
        if (why) *why = std::move(r);
        return false;
    };
    if (!main_thread::IsMainThread() || !item || !unity::IsAlive(plot) || !IsA(plot, plotClass_)) return no("no plot");
    for (auto& inj : injections_) {
        if (inj->failed) continue;
        for (const Injected& in : inj->items) {
            if (in.item != item) continue;
            const UpgradeDef& d = *in.def;
            // Only the card of a chain showing a tier that is not owned yet; an owned card sells itself.
            if (!in.inList) return no(std::format("{}: card not in the list", d.title));
            if (d.family == kNoFamily || !d.needs) return no(std::format("{}: not a tier above another", d.title));
            if (PlotType(plot) != d.plotType) return no(std::format("{}: other plot type", d.title));
            if (OwnsKey(plot, d)) return no(std::format("{}: owned (sells itself)", d.title));
            if (custom_upgrades::IsCustomUpgrade(d.needs)) {
                const UpgradeDef* r = FindDef(d.needs);
                if (!r || r->family != d.family || !OwnsKey(plot, *r))
                    return no(std::format("{}: the tier below is not owned", d.title));
                for (const Injected& o : inj->items)
                    if (o.def == r && unity::IsAlive(o.item)) {
                        upgrade = r->id;
                        priceItem = o.item;
                        return true;
                    }
                return no(std::format("{}: no menu item for {}", d.title, r->title));
            }
            // Chain on a vanilla upgrade (Music Box, Sprinkler, Ash Trough): the vanilla upgrade below, priced by
            // its own menu item. The card stands at the vanilla card's place whenever it is shown (Shown needs
            // the vanilla upgrade), so ownership is what counts; the item is the one BuildInjection pinned.
            if (!in.base) return no(std::format("{}: vanilla menu item of upgrade {} not found", d.title, d.needs));
            if (!unity::IsAlive(in.base))
                return no(std::format("{}: vanilla menu item of upgrade {} was destroyed", d.title, d.needs));
            if (!VanillaOwned(plot, d.needs)) return no(std::format("{}: vanilla upgrade {} not owned", d.title, d.needs));
            upgrade = d.needs;
            priceItem = in.base;
            return true;
        }
    }
    return no("not a custom card");
}

Il2CppObject* CustomUpgrades::HiddenMenuItemOf(size_t fieldOffset, Il2CppObject* value, Il2CppObject*& standIn) {
    standIn = nullptr;
    for (auto& inj : injections_) {
        if (inj->failed) continue;
        for (const HiddenVanilla& h : inj->hidden) {
            if (!h.item || !unity::IsAlive(h.item) || unity::Field<Il2CppObject*>(h.item, fieldOffset) != value) continue;
            for (const Injected& in : inj->items)
                if (in.base == h.item && in.inList) standIn = in.item;
            return h.item;
        }
    }
    return nullptr;
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

std::vector<std::pair<std::string, std::string>> CustomUpgrades::SaveValuesWithPrefix(const std::string& prefix) {
    std::vector<std::pair<std::string, std::string>> out;
    if (prefix.empty() || !EnsureLoaded()) return out;
    for (const auto& [k, v] : state_.values)
        if (k.compare(0, prefix.size(), prefix) == 0) out.emplace_back(k, v);
    return out;
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
                        if (d.family != kNoFamily && d.family < kFamilies && d.plotType == type &&
                            ps->second.owned.count(d.key))
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
    spriteClass_ = R.Class(kCore, "UnityEngine", "Sprite");        // optional (GameSpriteIcons)

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
    // Optional: without them a card placed while the menu is open shows its availability at the next open.
    // No subclass of PlotUpgradePurchaseItemModel exists in 1.3.0, so the virtuals need no dispatch.
    itemUpdAvailMi_ = R.Method(upgItemClass_, "UpdateAvailability", 1);
    itemUpdHiddenMi_ = R.Method(upgItemClass_, "UpdateHidden", 1);
    if (!itemUpdAvailMi_ || !itemUpdHiddenMi_)
        SR2_WARN("[{}] PlotUpgradePurchaseItemModel.UpdateAvailability / UpdateHidden not found - a maxed tier shows "
                 "Purchase until the menu is opened again", kModule);
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
    if (auto gm = R.FieldOffset(sceneCtx, "<GameModel>k__BackingField")) offScGameModel_ = *gm;  // optional (gate)
    else SR2_WARN("[{}] SceneContext.GameModel not found - the gate tells the title screen by the game name only", kModule);

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
    getHideFlagsMi_ = method(uobject, "get_hideFlags", 0, false);
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
        groupClass_ = R.Class(kImage, "", "IdentifiableTypeGroup", &err);
        if (lookup && groupClass_) {
            auto lg = R.FieldOffset(lookup, "_gadgetDefinitions");
            auto gm = R.FieldOffset(groupClass_, "_memberTypes");
            auto gs = R.FieldOffset(groupClass_, "_memberGroups");
            if (lg && gm) {
                offLookupGadgets_ = *lg;
                offGroupMembers_ = *gm;
                offGroupSubs_ = gs ? *gs : 0;
            }
        }
    }

    {
        std::string werr;
        if (!world_.Setup(R, kModule, werr))
            SR2_WARN("[{}] coop world objects unavailable ({}) - Coop Air Net and Hen Collector are not offered", kModule, werr);
    }
    // Game-UI looks of the mod's gadget definitions (label, preview, description; optional: missing ->
    // the base gadget's label / preview / description).
    {
        std::string gerr, greport;
        if (custom_upgrades::gadget_look::Setup(ctx, kModule, gerr, greport))
            SR2_INFO("[{}] mod gadget looks ready:{}", kModule, greport);
        else SR2_WARN("[{}] mod gadget looks unavailable ({}) - mod gadgets show their base gadget's label and preview",
                      kModule, gerr);
    }
    // Quantum Storage Link gadget (optional; missing -> not offered, nothing else changes).
    {
        custom_upgrades::quantum::Host qhost;
        qhost.on = [this] { return On(); };
        qhost.saveReady = [this] { return InGame() && EnsureLoaded(); };
        qhost.value = [this](const std::string& k) { return SaveValueOf(k); };
        qhost.setValue = [this](const std::string& k, const std::string& v) { return SetSaveValueOf(k, v); };
        qhost.lookup = [this] { return LookupDirector(); };
        qhost.notify = [this](const std::vector<std::pair<Il2CppObject*, std::string>>& lines) {
            if (!notices_.TextReady() || !notices_.Available()) return false;
            bool any = false;
            for (const auto& [icon, text] : lines) any = notices_.QueueText(icon, text, false) || any;
            if (any) notices_.FlushText();
            return any;
        };
        std::string qerr, qreport;
        if (quantum_.Setup(ctx, std::move(qhost), qerr, qreport)) SR2_INFO("[{}] Quantum Storage Link ready:{}", kModule, qreport);
        else SR2_WARN("[{}] Quantum Storage Link unavailable ({}) - not offered", kModule, qerr);
    }
    // Slime DJ buildables (DJ Booth, DJ Speakers, Slime Radio): on the Quantum Storage Link's hooks
    // (variant_ext.h), so only with it.
    if (quantum_.Ready()) {
        custom_upgrades::dj::Host dhost;
        dhost.saveReady = [this] { return InGame() && EnsureLoaded(); };
        dhost.value = [this](const std::string& k) { return SaveValueOf(k); };
        dhost.setValue = [this](const std::string& k, const std::string& v) { return SetSaveValueOf(k, v); };
        dhost.lookup = [this] { return LookupDirector(); };
        std::string derr;
        if (dj_.Setup(ctx, std::move(dhost), derr)) SR2_INFO("[{}] Slime DJ buildables ready", kModule);
        else SR2_WARN("[{}] Slime DJ buildables unavailable ({}) - not offered", kModule, derr);
    } else {
        SR2_WARN("[{}] Slime DJ buildables unavailable (they need the Quantum Storage Link's hooks) - not offered", kModule);
    }
    // The Slime Radio in the shop's Daily Deals (optional; missing -> the radio is only fabricated).
    if (dj_.Ready()) {
        custom_upgrades::radio_shop::Host rhost;
        rhost.on = [this] { return On(); };
        rhost.saveReady = [this] { return InGame() && EnsureLoaded(); };
        rhost.value = [this](const std::string& k) { return SaveValueOf(k); };
        rhost.setValue = [this](const std::string& k, const std::string& v) { return SetSaveValueOf(k, v); };
        std::string rerr;
        if (radioShop_.Setup(ctx, std::move(rhost), rerr)) SR2_INFO("[{}] Slime Radio shop entry ready", kModule);
        else SR2_WARN("[{}] Slime Radio shop entry unavailable ({}) - the radio is only fabricated", kModule, rerr);
    }
    // Drone program sources "Hen Collector" / "Elder Collector" / "Quantum Storage Link" (optional;
    // missing -> only the vanilla sources).
    {
        std::string derr, dreport;
        if (drones_.Setup(ctx, kModule, derr, dreport))
            SR2_INFO("[{}] drone sources Hen / Elder Collector, Quantum Storage Link ready:{}", kModule, dreport);
        else SR2_WARN("[{}] drone sources Hen / Elder Collector, Quantum Storage Link unavailable ({}) - drones reach the "
                      "Hen Collector through Coop", kModule, derr);
    }
    // Several programs per drone station (optional; needs the drone sources' Pull detour and helpers).
    // Locked behind the Drone Program Scheduler card (vacpack upgrades, set up below); without the
    // vacpack upgrades the card can't be bought, so the feature is not locked then.
    if (drones_.Ready()) {
        std::string perr;
        auto owned = [this] { return vacpack_.Ready() ? vacpack_.DroneProgramsOwned() : 1; };
        if (programs_.Setup(R, kModule, owned, perr))
            SR2_INFO("[{}] drone programs ready (up to {} per station, with the {})", kModule,
                     custom_upgrades::drone_programs::kMaxPrograms, custom_upgrades::drone_programs::kUpgradeName);
        else SR2_WARN("[{}] drone programs unavailable ({}) - one program per station", kModule, perr);
    } else {
        SR2_WARN("[{}] drone programs unavailable (they need the drone sources) - one program per station", kModule);
    }

    // Silo capacity tiers (optional: missing -> Silo Capacity II-V are not offered).
    {
        std::string cerr;
        auto fo = [&](Il2CppClass* k, const char* field, size_t& out) {
            for (Il2CppClass* c = k; c && !out; c = parentOf(c))
                if (auto o = R.FieldOffset(c, field, &cerr)) out = *o;
            if (!out && cerr.empty()) cerr = std::format("field {} not found", field);
        };
        storageUpgraderClass_ = R.Class(kImage, "", "StorageUpgrader", &cerr);
        Il2CppClass* slotDef = R.Class(kImage, "", "AmmoSlotDefinition", &cerr);
        Il2CppClass* gameModel = R.Class(kImage, "MonomiPark.SlimeRancher.DataModel", "GameModel", &cerr);
        Il2CppClass* plotModel = R.Class(kImage, "MonomiPark.SlimeRancher.DataModel", "LandPlotModel", &cerr);
        if (storageUpgraderClass_ && slotDef && gameModel && plotModel) {
            fo(storageUpgraderClass_, "StorageSilo", offUpgSilo_);
            fo(storageUpgraderClass_, "StorageCapacityUpgradeAmount", offUpgAmount_);
            fo(siloStorageClass_, "AmmoSlotDefinitions", offSiloDefs_);
            fo(slotDef, "_maxAmount", offDefMax_);
            fo(ammoMgr, "_ammoModel", offAmmoModel_);
            fo(plotModel, "gameObj", offPlotModelObj_);
            capUpdateMi_ = R.MethodByParams(ammoMgr, "UpdateMaxCount", {"System.Int32"}, &cerr);
            capSlotMaxMi_ = R.MethodByParams(ammoMgr, "GetSlotMaxCount", {"System.Int32"}, &cerr);
            getPlotModelMi_ = R.Method(gameModel, "GetLandPlotModel", 1, &cerr);
            compInChildrenMi_ = R.MethodByParams(component, "GetComponentInChildren", {"System.Type", "System.Boolean"}, &cerr);
        }
        capacityOk_ = offUpgSilo_ && offUpgAmount_ && offSiloDefs_ && offDefMax_ && offAmmoModel_ && offPlotModelObj_ &&
                      capUpdateMi_ && capSlotMaxMi_ && getPlotModelMi_ && compInChildrenMi_ && offScGameModel_;
        if (!capacityOk_)
            SR2_WARN("[{}] silo capacity tiers unavailable ({}) - Silo Capacity II-V are not offered", kModule,
                     cerr.empty() ? "SceneContext.GameModel not found" : cerr);
    }

    // Hen Collector limit sliders (optional: without them the limits stay at their saved / default values).
    const MethodInfo* detailsBindMi = nullptr;
    if (world_.Ready()) {
        std::string serr;
        Il2CppClass* view = R.Class(kImage, "MonomiPark.SlimeRancher.UI.Purchase", "PurchaseItemDetailsContentView", &serr);
        Il2CppClass* vm = R.Class(kImage, "MonomiPark.SlimeRancher.UI.Purchase", "PurchaseItemDetailsContentViewModel", &serr);
        auto o = vm ? R.FieldOffset(vm, "_description", &serr) : std::nullopt;
        detailsBindMi = view ? R.Method(view, "BindData", 1, &serr) : nullptr;  // the class's own override
        if (o && detailsBindMi && sliderHens_.Setup(R, kModule, serr) && sliderRoosters_.Setup(R, kModule, serr) &&
            sliderFeeder_.Setup(R, kModule, serr)) {
            offVmDescription_ = *o;
        } else {
            detailsBindMi = nullptr;
            SR2_WARN("[{}] Hen Collector limit sliders unavailable ({}) - the limits stay at their saved values", kModule,
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
    // Menu refresh after a purchase / sale with the menu open (optional: without it the menu shows
    // the new tier only when it is opened again).
    {
        std::string err;
        const MethodInfo* rebuildMi = method(rootClass_, "RebuildUI", 0, false);
        void* p = rebuildMi ? R.MethodPointer(rebuildMi, &err) : nullptr;
        if (p && ctx.HookAddress("ListUIRoot.RebuildUI", p, reinterpret_cast<void*>(&RebuildDetour),
                                 reinterpret_cast<void**>(&g_origRebuild)))
            report += std::format(" ListUIRoot.RebuildUI@0x{:X}", Resolver::Rva(p));
        else
            SR2_WARN("[{}] ListUIRoot.RebuildUI not hooked ({}) - the plot menu updates its custom items only when "
                     "it is opened again", kModule, err.empty() ? "not found" : err);
    }
    // Demolish / plot swap (optional: without it a plot rebuilt as the same type keeps its tiers).
    {
        std::string err;
        const MethodInfo* replaceMi = R.Method(locationClass_, "Replace", 2, &err);
        void* p = replaceMi ? R.MethodPointer(replaceMi, &err) : nullptr;
        if (p && ctx.HookAddress("LandPlotLocation.Replace", p, reinterpret_cast<void*>(&ReplaceDetour),
                                 reinterpret_cast<void**>(&g_origReplace)))
            report += std::format(" LandPlotLocation.Replace@0x{:X}", Resolver::Rva(p));
        else
            SR2_WARN("[{}] LandPlotLocation.Replace not hooked ({}) - a plot demolished and rebuilt as the same type "
                     "keeps its custom upgrades", kModule, err.empty() ? "not found" : err);
    }
    if (detailsBindMi) {
        std::string err;
        void* p = R.MethodPointer(detailsBindMi, &err);
        if (p && ctx.HookAddress("PurchaseItemDetailsContentView.BindData", p, reinterpret_cast<void*>(&DetailsBindDetour),
                                 reinterpret_cast<void**>(&g_origDetailsBind)))
            report += std::format(" PurchaseItemDetailsContentView.BindData@0x{:X}", Resolver::Rva(p));
        else SR2_WARN("[{}] hook PurchaseItemDetailsContentView.BindData failed ({}) - no limit slider", kModule, err);
        // The Hen Collector sliders are mouse only for now (no navigation / Submit / Cancel hooks).
    }

    // Sell-All HUD notices (optional; missing -> the world button's text says what was sold).
    {
        std::string nerr;
        if (!notices_.Setup(R, kModule, nerr))
            SR2_WARN("[{}] sell-all HUD notices unavailable ({}) - text feedback only", kModule, nerr);
        else if (std::string terr; !notices_.SetupText(R, terr))  // free-text notices: Use on a Quantum Storage Link
            SR2_WARN("[{}] text HUD notices unavailable ({}) - Use on a Quantum Storage Link opens the refinery screen",
                     kModule, terr);
        std::string rerr;
        if (!receipt_.Setup(R, kModule, rerr))
            SR2_WARN("[{}] sell-all receipt unavailable ({}) - the Sell-All button sells at once", kModule, rerr);
    }

    // Silo Sell-All world button (optional; missing -> the Sell-All Button is not offered).
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
        // The Hen Collector keeps its button only with this detour (a null Collector would NRE in vanilla).
        world_.KeepHenButton(ok);
        // The Corral Radio's button shares the detour (optional: the Slime DJ tab edits playlists too).
        if (std::string berr; ok && !radioButtons_.Setup(R, kModule, berr, custom_upgrades::world::RadioButtonLook())) {
            radioButtons_ = custom_upgrades::world::SiloButtons{};
            SR2_WARN("[{}] corral radio button unavailable ({}) - playlists only in the Slime DJ tab", kModule, berr);
        }
        if (!ok) {
            silo_ = custom_upgrades::world::SiloButtons{};  // not Ready(): no buttons are built
            g_offActCollector = 0;
            SR2_WARN("[{}] silo sell button unavailable ({}) - the Sell-All Button is not offered", kModule, serr);
        }
    }

    // Decorative props of owned tiers (optional; missing -> the tiers work without them).
    {
        std::string perr;
        if (!props_.Setup(R, kModule, perr)) {
            props_ = custom_upgrades::world::PlotProps{};
            SR2_WARN("[{}] plot props unavailable ({}) - tiers show no props", kModule, perr);
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

    // Drone Battery Saver effect (optional; missing -> the Fabricator card works but changes nothing,
    // logged). Its level is the vacpack upgrade's.
    {
        std::string berr;
        if (!battery_.Setup(R, kModule, [this] { return vacpack_.DroneBatteryLevel(); }, berr))
            SR2_WARN("[{}] drone battery saver unavailable ({}) - owning the upgrade changes nothing", kModule, berr);
    }
    // Vacpack upgrades in the Fabricator (optional; missing -> not offered, nothing else changes).
    {
        custom_upgrades::vacpack::Host host;
        host.on = [this] { return On(); };
        host.saveReady = [this] { return InGame() && EnsureLoaded(); };
        host.value = [this](const std::string& k) { return SaveValueOf(k); };
        host.setValue = [this](const std::string& k, const std::string& v) { return SetSaveValueOf(k, v); };
        host.lookup = [this] { return LookupDirector(); };
        host.valuesWithPrefix = [this](const std::string& p) { return SaveValuesWithPrefix(p); };
        if (battery_.Ready()) {
            host.droneStation = [this] { return battery_.StationDefinition(); };
            host.droneBlueprint = [this] { return battery_.HasStationBlueprint(); };
        }
        std::string verr, vreport;
        if (vacpack_.Setup(ctx, std::move(host), verr, vreport)) report += vreport;
        else SR2_WARN("[{}] vacpack upgrades unavailable ({}) - Tank Booster IX-XII, Extra Tank III-IV, Drone Battery "
                      "Saver I-IV, Shadow Catcher and Drone Program Scheduler are not offered (several programs per drone "
                      "station unlocked)", kModule, verr);
    }
    // Shadow Catcher effect (optional; needs the vacpack upgrades, which sell it and publish its state).
    if (vacpack_.Ready()) {
        std::string serr, sreport;
        if (shadow_.Setup(ctx, serr, sreport)) report += sreport;
        else SR2_WARN("[{}] shadow catcher unavailable ({}) - owning the upgrade changes nothing", kModule, serr);
    }
    // Auto-Feeder "Leave last food item in" (optional: without it feeders stay vanilla and the Feeder card has
    // no switch).
    {
        std::string ferr, freport;
        if (feeder_.Setup(ctx, ferr, freport)) report += freport;
        else SR2_WARN("[{}] auto-feeder 'leave last food item in' unavailable ({}) - feeders stay vanilla", kModule, ferr);
    }
    // Its pad / keyboard key: the menu's Submit flips the switch (optional: without it the switch is mouse only).
    if (feeder_.Ready() && sliderFeeder_.Ready()) {
        std::string kerr;
        Il2CppClass* listUi = R.Class(kImage, "MonomiPark.SlimeRancher.UI.Adapter", "ListUI", &kerr);
        const MethodInfo* submitMi = listUi ? R.Method(listUi, "Submit", 1, &kerr) : nullptr;
        void* sp = submitMi ? R.MethodPointer(submitMi, &kerr) : nullptr;
        if (sp && ctx.HookAddress("ListUI.Submit", sp, reinterpret_cast<void*>(&ListSubmitDetour),
                                  reinterpret_cast<void**>(&g_origListSubmit)))
            report += std::format(" ListUI.Submit@0x{:X}", Resolver::Rva(sp));
        else
            SR2_WARN("[{}] ListUI.Submit not hooked ({}) - the auto-feeder switch is mouse only", kModule,
                     kerr.empty() ? "hook failed" : kerr);
    }
    // Auto-Feeder fill lights (optional: without them the feeder lights stay vanilla green).
    {
        std::string lerr;
        if (!feederLights_.Setup(R, kModule, lerr))
            SR2_WARN("[{}] auto-feeder fill lights unavailable ({}) - feeder lights stay green", kModule, lerr);
    }
    // Slime Launcher effect (optional; the vacpack upgrades sell it and publish its level).
    if (vacpack_.Ready()) {
        std::string lerr, lreport;
        if (launcher_.Setup(ctx, lerr, lreport)) report += lreport;
        else SR2_WARN("[{}] slime launcher unavailable ({}) - owning the upgrade changes nothing", kModule, lerr);
    }

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

bool IsModGadget(Il2CppObject* def) { return quantum::IsOurs(def) || dj::Owns(def); }

std::string Name(int32_t upgrade) {
    const UpgradeDef* d = FindDef(upgrade);
    return d ? d->title : "custom upgrade";
}

bool Owns(Il2CppObject* landPlot, int32_t upgrade) {
    if (!Active() || !main_thread::IsMainThread() || !IsCustomUpgrade(upgrade)) return false;
    bool r = false;
    g_hasGuard.Run([&] { r = g_self->Owns(landPlot, upgrade); });
    return r;
}

void SetSaveVeto(SaveVeto fn) { g_saveVeto.store(fn); }

std::string PlotKey(Il2CppObject* landPlot) {
    if (!Active() || !main_thread::IsMainThread()) return {};
    std::string r;
    g_hasGuard.Run([&] { r = g_self->PlotKeyOf(landPlot); });
    return r;
}

std::string CanSell(Il2CppObject* landPlot, int32_t upgrade, std::string* brief) {
    if (brief) brief->clear();
    if (!Active() || !main_thread::IsMainThread()) {
        if (brief) *brief = "unavailable";
        return "Custom upgrades are not available right now.";
    }
    std::string r;
    if (!g_hasGuard.Run([&] { r = g_self->CanSellCustom(landPlot, upgrade, brief); })) {
        if (brief) *brief = "unavailable";
        return "Custom upgrades failed.";
    }
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

bool ChainSellTarget(Il2CppObject* landPlot, Il2CppObject* item, int32_t& upgrade, Il2CppObject*& priceItem,
                     std::string* why) {
    upgrade = 0;
    priceItem = nullptr;
    if (why) why->clear();
    if (!Active() || !main_thread::IsMainThread()) {
        if (why) *why = "custom upgrades not active";
        return false;
    }
    bool ok = false;
    g_hasGuard.Run([&] { ok = g_self->ChainSellTargetOf(landPlot, item, upgrade, priceItem, why); });
    return ok;
}

Il2CppObject* HiddenMenuItem(size_t fieldOffset, Il2CppObject* value, Il2CppObject*& standIn) {
    standIn = nullptr;
    if (!Active() || !main_thread::IsMainThread() || !value || fieldOffset < 2 * sizeof(void*)) return nullptr;
    Il2CppObject* r = nullptr;
    g_hasGuard.Run([&] { r = g_self->HiddenMenuItemOf(fieldOffset, value, standIn); });
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
