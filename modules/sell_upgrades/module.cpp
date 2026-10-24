// sell_upgrades: sell owned ranch land-plot upgrades back for half their price (fixed,
// floor(0.5 x price)), from inside the game's own plot purchase menu (SR2 1.3.0).
//
// ---------------------------------------------------------------- research --
// (Cpp2IL dump + capstone disassembly of GameAssembly.dll 1.3.0; RVAs for reference)
//
// Storage
//   LandPlotModel.upgrades : HashSet<LandPlot.Upgrade> (int enum, saved as a list by
//   Push/Pull). LandPlot.HasUpgrade(u) -> model.upgrades.Contains(u).
//   LandPlot.AddUpgrade(u) (0x988750) = model.upgrades.Add(u); ApplyUpgrades({u}, true).
//   LandPlot.ApplyUpgrades(set, isInitialPurchase) (0x9887E0): for every PlotUpgrader
//   component on the LandPlot GameObject and every upgrade in `set`: upgrader.Apply(u)
//   (+ OnInitialPurchase(u) when buying); then _trackers = GetComponentsInChildren
//   <TrackContainedIdentifiables>(false) and RanchMetadata.Deregister/Register(this).
//   On load the plot prefab is instantiated fresh and the model's set is applied, so
//   after a reload the plot always looks exactly like its saved upgrade set.
//
// Buying (MonomiPark.SlimeRancher.UI.Plot.LandPlotUIRoot, the plot purchase menu)
//   Items are PlotUpgradePurchaseItemModel (ScriptableObject): _upgrade (+0x88), the price
//   in MenuPurchaseItemModel._purchaseCost (PurchaseCost struct, List<CurrencyCostEntry>
//   _currencyCosts {CurrencyDefinition _currency, int _amount}), _title, _promptMessage.
//   IsAvailable = !plot.HasUpgrade(_upgrade); owned upgrades stay in the list (greyed).
//   Hidden: STORAGE3 needs STORAGE2, STORAGE4 needs STORAGE3, PLORT_COLLECTOR_INCINERATOR
//   needs ASH_TROUGH.
//   ListUI: selecting an item sets the submit action to onActionPerformed(item) without any
//   availability check -> LandPlotUIRoot.OnAction(item) (0x6E6F50) runs for owned items
//   too; vanilla returns silently there when !IsAvailable.
//   OnAction -> PromptForUpgrade -> ListUIRoot.OpenPopupPrompt(title, message, warning,
//   positiveLabel, negativeLabel, Action positive, Action negative) with a compiler closure
//   LandPlotUIRoot.<>c__DisplayClass6_0 {<>4__this, upgrade, cost, item}; its
//   <PromptForUpgrade>b__0 (0x702E70) calls LandPlotUIRoot.Upgrade(upgrade, cost):
//   HasUpgrade -> false (abort) / ISpendableSpender.Spend(cost) / AddUpgrade / cue /
//   RebuildUI() / swapper.PlayPurchaseFX(). Labels from GameContext.UITemplates
//   (popupPromptPositive/NegativeActionLabel). PopupPrompt shows the strings through
//   LocalizeStringEvent components (_messageText, _positiveActionButtonText, ...).
//
// Upgraders (each Apply(u) only acts on its own upgrade value):
//   WallUpgrader       WALLS:   StandardWalls off, UpgradeWalls on
//   MusicBoxUpgrader   MUSIC_BOX: MusicBox on (MusicBoxRegion.OnDisable releases slimes)
//   MineralSoil/Sprinkler/Scareslime/Feeder/Vitamizer/AshTrough: one GameObject on
//   AirNetUpgrader / SolarShieldUpgrader: GameObject[] on
//   PlortCollectorUpgrader PLORT_COLLECTOR / _POND / _INCINERATOR: Collector on
//   MiracleMixUpgrader MIRACLE_MIX: MiracleMix on, NormSoil off
//   DeluxeCoopUpgrader DELUXE_COOP: DeluxeStuff on, CoopRegions[i]._isDeluxe = true
//   DeluxeGardenUpgrader DELUXE_GARDEN: DeluxeStuff on; if a crop is planted: destroy it,
//     GardenCatcher.Plant(cropId, true) (picks DeluxePlantedPrefab because the model has
//     DELUXE_GARDEN), new.InitAsReplacement(old) (keeps the growth state).
//   StorageUpgrader STORAGE2/3/4: order-dependent slot GameObject swaps;
//     STORAGE_CAPACITY_INCREASE: AmmoSlotManager.UpdateMaxCount(+amount) on the silo.
//   OnInitialPurchase side effects: FeederUpgrader -> SlimeFeeder model.nextFeedingTime
//   reset; AshTroughUpgrader -> FillableAshSource fills the trough (model.ashUnits = max).
//   The vanilla demolish / replace path (LandPlotLocation.Replace -> GameModel.
//   ReplaceLandPlot) builds a NEW model, so it cannot be reused to drop one upgrade.
//
// ---------------------------------------------------------------- behaviour --
//   * Selling is the Sell button in the details panel (below), with NO confirm popup (user
//     feedback): one press sells. Detour LandPlotUIRoot.OnAction: confirming an OWNED upgrade
//     item (A / submit / clicking the selected card) does nothing, as in vanilla - a double
//     click must not sell. Not owned -> vanilla purchase flow. If the upgrade cannot be sold
//     there is no popup either: a disabled button with a short reason, or no button.
//   * Detour on LandPlotUIRoot.<PromptForUpgrade>b__0 (the vanilla purchase confirm): the Ash
//     Trough re-purchase below, and no purchase in the frame of a sale or the next (a Sell
//     click also bought the card's tier, see sellTick_).
//   * Sale (main thread): re-validate, refund = floor(0.5 x each currency cost) via
//     PlayerState.AddCurrency, model.upgrades.Remove(u), revert the upgrader exactly
//     (table above, inverted), ApplyUpgrades(empty set, false) to refresh trackers +
//     RanchMetadata like vanilla does after a change, RebuildUI() to refresh the menu.
//     The upgrade can be bought again at full price through the vanilla flow.
//   * Side state: AUTO-FEEDER is refused while it still holds food (no item loss). ASH
//     TROUGH: refused while the incinerator plort collector (which needs it) is owned;
//     selling empties the trough (model.ashUnits = 0). DELUXE GARDEN: the planted crop is
//     replanted as the normal variant with the game's own replant calls (growth kept).
//   * Ash Trough re-purchase (sell + rebuy exploit): vanilla OnInitialPurchase sets
//     model.ashUnits = FillableAshSource.StartingUnits (a full trough), which made a
//     sell + rebuy a half-price refill. At the sale the trough's ash is remembered per
//     LandPlotModel (in memory; strong GC handle on the model so the key can't be reused,
//     freed by a posted job). The ConfirmDetour (vanilla purchase closures) notes an Ash
//     Trough purchase on a plot with a record, lets the original run, then sets
//     model.ashUnits = min(remembered, MaxUnits) and calls FillableAshSource.AddAsh(0)
//     (clamps + moves the ash mesh). The ash is also stored per plot in custom_upgrades'
//     per-save sidecar (api.h SetPlotValue "sell_upgrades.ash_trough", written with the next
//     game save into that .sav file's sidecar), so a re-purchase after a reload of a save
//     made after the sale keeps it too; the value is cleared when it is used. No record
//     and no sidecar value (never sold, sold but not saved, custom_upgrades off) or module
//     off: vanilla fill. A demolished plot loses the value (custom_upgrades drops
//     every per-plot value in its LandPlotLocation.Replace detour). The ash stays a vanilla-producible amount.
//     The FillableAshSource sits on the trough's child "Ash Level" (GetComponentInChildren, as
//     vanilla does; a plain GetComponent on AshTrough never found it, so before this fix every
//     re-purchase kept the vanilla full trough). After every completed Ash Trough purchase the
//     trough model (AshTroughUpgrader.AshTrough, hidden by the sale) is checked and switched
//     on if vanilla Apply did not (log "trough model shown" / "was still hidden - shown now").
//   * EXCLUDED: silo STORAGE2/3/4 and STORAGE_CAPACITY_INCREASE (they change the silo's
//     slot layout / per-slot capacity of stored items; reverting means rewriting slot
//     data and could lose or over-fill stored items).
//   * Save: only the vanilla HashSet changes (and ashUnits); every resulting state is one
//     vanilla can produce (the plot simply never had that upgrade). No new save data.
//
// ---------------------------------------------------------------- sell button --
//   The details panel's action area (ListUI, shared by every list menu) is built per
//   selection by ListUI.<ShowMenu>b__4 -> CreateDetailsMoreButton(item, category, vm):
//   available item -> Instantiate(category.detailsButton) (the "Purchase" button), not
//   available (= owned upgrade) -> Instantiate(category.detailsInfo) (a label-only twin:
//   the "PURCHASED" text), both into ListUI.mainActionButtonContainer, then
//   DetailsMoreButtonView.BindData(vm) (label = vm.actionLabel if the view has a Button,
//   else vm.infoLabel = the item's own _infoButtonLabel instance). ListUI wires no
//   listener in code; the Purchase prefab's click performs the menu's submit input
//   event, which ends in LandPlotUIRoot.OnAction(selected item) - like gamepad A.
//   * LandPlotUIRoot.SetActivator detour (pre): remember the open plot menu (GC handle;
//     the previous handle is freed by a posted job, never inside a hook).
//   * DetailsMoreButtonView.BindData detour (post): vm.infoLabel is matched (pointer
//     identity) to an upgrade item of the open plot menu. What the Sell button sells:
//     - label-only variant (PURCHASED): the owned item itself;
//     - Purchase variant: only the card of a custom tier chain that shows the next tier
//       (custom_upgrades::ChainSellTarget): the owned tier below it (a custom tier, or the
//       vanilla Music Box / Ash Trough whose card the tier took over), priced by that tier's
//       own menu item.
//     A copy of that category's own detailsButton prefab (the game's Purchase button) is
//     instantiated as a sibling in the same container, labelled "Sell +N" (its
//     LocalizeStringEvent is disabled, the TMP text set). The container has no LayoutGroup
//     (log: "MainActionButtonContainer (LayoutGroup no)"). On an owned card the PURCHASED view
//     is hidden and Sell stands alone in its place; on a tier card Purchase and Sell stand side
//     by side (PlaceButtons: pivots / anchoredPosition only; the buttons size themselves through
//     the game's own LayoutManager, which overwrote the old half-width split - build 506a4f60
//     showed them overlapping). Without the RectTransform accessors there is no Sell button next
//     to Purchase. An upgrade that can't
//     be sold for a passing reason ("holds food", "sell Music Box II first") gets the button
//     non-interactable (Selectable.set_interactable(false): the prefab's own disabled look,
//     skipped by navigation; label alpha halved on top) labelled with that reason; permanent
//     reasons ("silo storage", "not sellable") get no button. The previous sell button is
//     destroyed on every bind (= every selection change).
//   * UnityEngine.UI.Button.Press detour: a click on OUR button (pointer compare; other
//     buttons are not touched) sells at once and never reaches the original Press (whose
//     submit would buy the selected card's tier). An OnAction in that frame or the next (the
//     prefab's own submit routing) is swallowed. The cloned prefab (TextButton_NoneSelectable) is
//     not reachable by gamepad navigation: gamepad / keyboard selling is the alt action below.
//
// ---------------------------------------------------------------- alt-action sell --
//   The list menus have a second action, the alt action ("Info": opens the Pedia). ListUI
//   subscribes AltSubmit to its `altSubmit` InputEvent while focused; every selection
//   (ListUI.<ShowMenu>b__4) sets onAltSubmitActionPerformed = onAltActionPerformed(item) for
//   EVERY item (button shown or not), i.e. ListUIRoot`4.OnAltAction(item) (0x11AF260, shared
//   generic body: Pedia for an IPediaEntryProvider, else nothing). After CreateDetailsMoreButton
//   (our BindData detour) b__4 calls CreateDetailsAltButton(item, category, showAlt): destroy the
//   previous ListUI.altActionButtonGameObject, then, if category.altDetailsButton and
//   ListUIRoot`4.ShowAltActionButton(item) (0x11AFE20: item has a Pedia entry), instantiate
//   altDetailsButton into ListUI.altActionButtonContainer. No label is set in code: the prefab is
//   an input hint (key / button icon of the altSubmit event + "Info" label).
//   * BindData detour: when the selected card has a sellable target (the same target and checks
//     as the Sell button, but only when it can be sold now), a copy of that category's own
//     altDetailsButton is instantiated into altActionButtonContainer (ListUIRoot`4.swapper = the
//     ListUI), labelled "Sell +N" (InputHint._label disabled, _labelText set; fallbacks TextLabel,
//     DetailsMoreButtonView). Its input icon is the game's own, for the current device. Destroyed
//     on every bind like the Sell button. No prompt (and no gamepad sale) when nothing is
//     sellable or the label could not be set.
//   * ShowAltActionButton detour (shared body, one class check for other menus): false for the
//     card that shows our prompt, so the vanilla Info hint is not stacked on it.
//   * OnAltAction detour (shared body, LandPlotUIRoot only): for the card that shows our prompt,
//     sells once (SellNow, one sale per frame) and never reaches the original (no Pedia for that
//     press). Other cards: vanilla. A click on the prompt performs the same InputEvent -> same path.
//   * Key on the Sell button: the cloned Purchase prefab's InputEventDisplay (which showed the
//     submit key) gets InputEventDisplay.SetInputEvent(ListUI.altSubmit, null) while the button can
//     sell now, so it shows the alt-action key (the game's own icon for the current device). Without
//     an alt prompt (prefab / label missing) that key still sells the card (btnAltCard_). Disabled
//     buttons ("Holds food") hide the icon.
//
// Fault policy: every detour runs its logic inside a DetourGuard (native fault -> module
// faulted, detours pass-through). Objects are class- and IsAlive-checked; runtime_invoke
// native faults mark the module faulted (never used as control flow). GC handles (open
// menu root, our sell button, one per remembered ash trough model) are freed only from
// posted main-thread jobs. No scans, nothing per frame.

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <format>
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
#include "modules/custom_upgrades/api.h"

namespace sr2::modules {
namespace {

namespace unity = sr2::core::unity;
namespace main_thread = sr2::core::main_thread;
using sr2::il2cpp::api;
using sr2::il2cpp::Resolver;

constexpr const char* kModule = "sell_upgrades";
constexpr const char* kImage = "Assembly-CSharp";
constexpr const char* kCore = "UnityEngine.CoreModule";
constexpr const char* kDataNs = "MonomiPark.SlimeRancher.DataModel";
constexpr const char* kPlotUiNs = "MonomiPark.SlimeRancher.UI.Plot";

// LandPlot.Upgrade
enum Upg : int32_t {
    WALLS = 1, MUSIC_BOX = 2, STORAGE2 = 3, STORAGE3 = 4, STORAGE4 = 5, SOIL = 6, SPRINKLER = 7,
    SCARESLIME = 8, FEEDER = 9, VITAMIZER = 10, AIR_NET = 11, PLORT_COLLECTOR = 12, SOLAR_SHIELD = 13,
    ASH_TROUGH = 14, MIRACLE_MIX = 15, DELUXE_GARDEN = 16, DELUXE_COOP = 17, PLORT_COLLECTOR_POND = 18,
    PLORT_COLLECTOR_INCINERATOR = 19, STORAGE_CAPACITY_INCREASE = 20,
};

std::string UpgName(int32_t u) {
    if (custom_upgrades::IsCustomUpgrade(u)) return custom_upgrades::Name(u);
    switch (u) {
        case WALLS: return "High Walls";
        case MUSIC_BOX: return "Music Box";
        case STORAGE2: return "Silo Storage 2";
        case STORAGE3: return "Silo Storage 3";
        case STORAGE4: return "Silo Storage 4";
        case SOIL: return "Rich Soil";
        case SPRINKLER: return "Sprinkler";
        case SCARESLIME: return "Scareslime";
        case FEEDER: return "Auto-Feeder";
        case VITAMIZER: return "Vitamizer";
        case AIR_NET: return "Air Net";
        case PLORT_COLLECTOR: return "Plort Collector";
        case SOLAR_SHIELD: return "Solar Shield";
        case ASH_TROUGH: return "Ash Trough";
        case MIRACLE_MIX: return "Miracle Mix";
        case DELUXE_GARDEN: return "Deluxe Garden";
        case DELUXE_COOP: return "Deluxe Coop";
        case PLORT_COLLECTOR_POND: return "Plort Collector (pond)";
        case PLORT_COLLECTOR_INCINERATOR: return "Plort Collector (incinerator)";
        case STORAGE_CAPACITY_INCREASE: return "Silo Capacity";
        default: return "unknown upgrade";
    }
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

// Native method pointer + its MethodInfo (passed as the hidden last argument).
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

struct Vec2 {
    float x, y;
};
struct Rect {
    float x, y, width, height;
};

class SellUpgrades final : public sr2::core::Module {
public:
    const char* Name() const override { return kModule; }
    bool DefaultEnabled() const override { return true; }
    bool Setup(sr2::core::ModuleContext& ctx) override;
    void OnEnable() override { enabled_.store(true); }
    void OnDisable() override {
        enabled_.store(false);
        main_thread::Post([this] {
            if (!enabled_.load()) {
                DestroySellButton();
                DestroyAltPrompt();
            }
        });
    }
    bool On() const { return enabled_.load(std::memory_order_relaxed); }

    // detour bodies (main thread, inside a DetourGuard)
    bool HandleAction(Il2CppObject* root, Il2CppObject* item);
    void TrackRoot(Il2CppObject* root);
    void AfterMoreBind(Il2CppObject* view, Il2CppObject* vm);
    bool OnSellButton(Il2CppObject* button);
    Il2CppObject* SellButton() const { return btnButton_; }  // pointer compare only
    bool HidesAltButton(Il2CppObject* root, Il2CppObject* item) const {
        if (root != root_) return false;  // pointer compare only
        return (altGo_ && item == altItem_) || (btnAltCard_ && item == btnAltCard_);
    }
    bool HandleAltAction(Il2CppObject* root, Il2CppObject* item);

    // Ash Trough re-purchase: a vanilla purchase closure about to buy ASH_TROUGH on a plot
    // whose ash was remembered at its sale (else plot == nullptr).
    struct AshRebuy {
        Il2CppObject* plot = nullptr;
        Il2CppObject* model = nullptr;
        float saved = -1.0f;   // ash from the save's sidecar (custom_upgrades::PlotValue); < 0 = none
        bool record = false;   // ash remembered (in memory or `saved`): restore it after the purchase
    };
    AshRebuy BeforeVanillaBuy(Il2CppObject* closure);
    void AfterVanillaBuy(const AshRebuy& r);
    // A vanilla purchase confirm in the frame of a sale or the next: never the player's (see
    // sellTick_), refused before Upgrade() spends anything.
    bool RefuseBuyAfterSale(Il2CppObject* closure);

private:
    struct Refund {
        Il2CppObject* currency = nullptr;
        int32_t price = 0;
        int32_t amount = 0;
    };

    std::string CanSell(Il2CppObject* plot, int32_t upg, std::string* brief = nullptr);
    bool Price(Il2CppObject* item, std::vector<Refund>& out, int32_t& price, int32_t& refund);
    bool SetLabel(Il2CppObject* lse, const std::string& text);
    bool SellNow(Il2CppObject* root, Il2CppObject* priceItem, int32_t upg, const char* via);
    void DoSell(Il2CppObject* root, Il2CppObject* plot, Il2CppObject* item, int32_t upg);
    bool PlaceButtons(Il2CppObject* vanillaView, Il2CppObject* ourView, bool alone);
    void ScheduleCentre(unsigned gen, int ticksLeft);
    void CentrePair();
    Vec2 GetVec(const MethodInfo* m, Il2CppObject* rt, const char* what, bool& ok);
    void SetVec(const MethodInfo* m, Il2CppObject* rt, Vec2 v, const char* what, bool& ok);
    int RevertVisuals(Il2CppObject* plot, int32_t upg, std::string& notes);
    bool ReplantNormalCrop(Il2CppObject* plot, std::string& notes);
    int CountUpgraders(Il2CppObject* plot, int32_t upg);
    std::string PlotId(Il2CppObject* plot);
    Il2CppObject* Invoke(const MethodInfo* m, void* obj, std::initializer_list<void*> args, const char* what,
                         bool* ok = nullptr);
    bool HashSetCall(Il2CppObject* set, const char* method, int32_t upg, bool* result);
    Il2CppObject* NewEmptySetLike(Il2CppObject* set);
    Il2CppObject* GameObjectOf(Il2CppObject* comp);
    void SetActive(Il2CppObject* go, bool on);
    bool HasUpgrade(Il2CppObject* plot, int32_t upg) { return hasUpgrade_(plot, upg); }
    Il2CppObject* Type(Il2CppClass* k) { return unity::TypeOf(k); }

    Il2CppObject* FindItemByInfoLabel(Il2CppObject* root, Il2CppObject* infoLabel, Il2CppObject*& category);
    size_t OffsetIn(Il2CppObject* obj, const char* field, std::unordered_map<Il2CppClass*, size_t>& cache);
    void DestroySellButton();
    void CreateAltPrompt(Il2CppObject* category, Il2CppObject* item, Il2CppObject* priceItem, int32_t upg,
                         int32_t refund);
    bool SetAltLabel(Il2CppObject* go, const std::string& text, const char*& via);
    void DestroyAltPrompt();

    std::atomic<bool> enabled_{false};
    // TickCount of the last sale: a click on our button can also reach OnAction through the
    // prefab's own submit routing, for the item that is selected after the sale (which may then be
    // buyable): swallowed in that frame and the next. A purchase popup that this routing opened
    // and confirmed in the same frame is refused too (RefuseBuyAfterSale): test 2 logged a sale of
    // Fast Growth I / Music Box / Ash Trough from a tier card followed 2-12 ms later by a buy of
    // that card's tier (so the plot kept the tier and its prop). LandPlotUIRoot.Upgrade has one
    // caller, <PromptForUpgrade>b__0 (IDA), and PromptForUpgrade always opens the popup.
    unsigned long long sellTick_ = ~0ull;
    bool loggedButton_ = false;
    bool loggedLayout_ = false;
    bool loggedAlt_ = false;

    // open plot menu (main thread only)
    Il2CppObject* root_ = nullptr;
    Il2CppGCHandle rootHandle_ = nullptr;
    // our sell button (main thread only)
    Il2CppObject* btnGo_ = nullptr;      // GameObject (handle held)
    Il2CppObject* btnButton_ = nullptr;  // its UnityEngine.UI.Button (may be null)
    Il2CppObject* btnItem_ = nullptr;    // the menu item whose price the refund uses (menu asset)
    int32_t btnUpgrade_ = 0;             // the owned upgrade it sells
    Il2CppObject* btnAltCard_ = nullptr; // the card whose alt action the button's key icon names (sellable only)
    Il2CppGCHandle btnHandle_ = nullptr;
    // Purchase + Sell pair (main thread only): the vanilla Purchase view's RectTransform (handle
    // held), the pair's centre and a generation that stops stale CentrePair jobs.
    static constexpr float kPairGap = 16.0f;
    Il2CppObject* pairVanilla_ = nullptr;
    Il2CppGCHandle pairVanillaHandle_ = nullptr;
    float pairCentre_ = 0.0f;
    unsigned pairGen_ = 0;
    // our alt-action "Sell +N" prompt (main thread only)
    Il2CppObject* altGo_ = nullptr;         // GameObject (handle held)
    Il2CppObject* altItem_ = nullptr;       // the selected card's menu item (pointer compare)
    Il2CppObject* altPriceItem_ = nullptr;  // the menu item whose price the refund uses
    int32_t altUpgrade_ = 0;                // the owned upgrade it sells
    Il2CppGCHandle altHandle_ = nullptr;
    std::unordered_map<Il2CppClass*, size_t> offCategories_, offItems_, offDetailsButton_, offAltButton_;
    // ash in a sold Ash Trough, per LandPlotModel (main thread only; handle keeps the key alive)
    struct AshRecord {
        Il2CppGCHandle handle = nullptr;
        float ash = 0.0f;
    };
    std::unordered_map<Il2CppObject*, AshRecord> ashRecords_;
    void RememberAsh(Il2CppObject* model, float ash);
    Il2CppObject* FindAshSource(Il2CppObject* plot);
    Il2CppObject* AshTroughObject(Il2CppObject* plot);  // AshTroughUpgrader.AshTrough (the trough model)

public:
    // ---- resolved (Setup, worker thread) ----
    Il2CppClass* rootClass_ = nullptr;        // LandPlotUIRoot
    Il2CppClass* itemClass_ = nullptr;        // PlotUpgradePurchaseItemModel
    Il2CppClass* closureClass_ = nullptr;     // LandPlotUIRoot/<>c__DisplayClass6_0
    Il2CppClass* plotClass_ = nullptr;        // LandPlot
    Il2CppClass* modelClass_ = nullptr;       // LandPlotModel
    Il2CppClass* costEntryClass_ = nullptr;   // CurrencyCostEntry
    Il2CppClass* upgraderClass_ = nullptr;    // PlotUpgrader
    Il2CppClass* tmpClass_ = nullptr;         // TMPro.TMP_Text
    Il2CppClass* lseClass_ = nullptr;         // LocalizeStringEvent
    Il2CppClass* feederClass_ = nullptr;      // SlimeFeeder
    Il2CppClass* locationClass_ = nullptr;    // LandPlotLocation
    Il2CppClass* spawnResClass_ = nullptr;    // SpawnResource
    Il2CppClass* catcherClass_ = nullptr;     // GardenCatcher
    Il2CppClass* ashSourceClass_ = nullptr;   // FillableAshSource (optional: ash restore)
    size_t offAshMax_ = 0;                    // FillableAshSource.MaxUnits
    size_t offAshModel_ = 0;                  // FillableAshSource._plotModel
    Bound<void(Il2CppObject*, float)> addAsh_;  // FillableAshSource.AddAsh(float)
    bool ashRestoreOk_ = false;

    struct UpgraderInfo {
        Il2CppClass* klass = nullptr;
        size_t off1 = 0, off2 = 0;  // GameObject / GameObject[] / CoopRegion[] fields
    };
    UpgraderInfo walls_, music_, soil_, sprinkler_, scare_, feederU_, vitamizer_, airNet_, collector_, solar_,
        ash_, miracle_, dGarden_, dCoop_;
    size_t offCoopDeluxe_ = 0;  // CoopRegion._isDeluxe

    size_t offActivator_ = 0;     // LandPlotUIRoot.Activator
    size_t offItemUpgrade_ = 0;   // PlotUpgradePurchaseItemModel._upgrade
    size_t offItemCosts_ = 0;     // MenuPurchaseItemModel._purchaseCost._currencyCosts
    size_t offCostCurrency_ = 0;  // CurrencyCostEntry._currency
    size_t offCostAmount_ = 0;    // CurrencyCostEntry._amount
    size_t offPlotModel_ = 0;     // LandPlot._model
    size_t offPlotAttached_ = 0;  // LandPlot._attached
    size_t offModelUpgrades_ = 0; // LandPlotModel.upgrades
    size_t offModelAsh_ = 0;      // LandPlotModel.ashUnits
    size_t offLocationId_ = 0;    // IdHandler._id
    size_t offMenuConfig_ = 0;    // ListUIRoot`4.menuConfig
    size_t offItemInfoLabel_ = 0; // MenuPurchaseItemModel._infoButtonLabel
    size_t offMoreLabel_ = 0;     // DetailsMoreButtonView.label (LocalizeStringEvent)
    size_t offMoreButton_ = 0;    // DetailsMoreButtonView.button (UnityEngine.UI.Button)
    size_t offVmInfoLabel_ = 0;   // DetailsMoreButtonViewModel.infoLabel
    Il2CppClass* moreViewClass_ = nullptr;     // DetailsMoreButtonView
    // alt-action sell (optional: missing -> no gamepad / keyboard selling)
    Il2CppClass* listUiClass_ = nullptr;       // ListUI
    size_t offSwapper_ = 0;                    // ListUIRoot`4.swapper (the ListUI)
    size_t offAltContainer_ = 0;               // ListUI.altActionButtonContainer
    size_t offAltSubmit_ = 0;                  // ListUI.altSubmit (InputEvent, for the log)
    Il2CppClass* hintClass_ = nullptr;         // InputHint (optional label route)
    size_t offHintLabel_ = 0, offHintText_ = 0;         // InputHint._label / _labelText
    Il2CppClass* textLabelClass_ = nullptr;    // TextLabel (optional label route)
    size_t offTextLabelLse_ = 0, offTextLabelTmp_ = 0;  // TextLabel._stringEvent / _textElement
    bool altOk_ = false;
    Il2CppClass* layoutGroupClass_ = nullptr;  // UnityEngine.UI.LayoutGroup (optional)
    Il2CppClass* inputEventButtonClass_ = nullptr;   // InputEventButton (optional)
    Il2CppClass* inputEventDisplayClass_ = nullptr;  // InputEventDisplay (optional)
    const MethodInfo* displaySetEventMi_ = nullptr;  // InputEventDisplay.SetInputEvent(InputEvent, InputEvent)
    Bound<void(Il2CppObject*)> objectDestroy_;  // UnityEngine.Object.Destroy(Object)
    // RectTransform (optional: side-by-side layout of PURCHASED / Purchase and our Sell button)
    Il2CppClass* rtClass_ = nullptr;
    const MethodInfo* rtGetRect_ = nullptr;
    const MethodInfo* rtGetAnchorMin_ = nullptr;
    const MethodInfo* rtSetAnchorMin_ = nullptr;
    const MethodInfo* rtGetAnchorMax_ = nullptr;
    const MethodInfo* rtSetAnchorMax_ = nullptr;
    const MethodInfo* rtGetPivot_ = nullptr;
    const MethodInfo* rtSetPivot_ = nullptr;
    const MethodInfo* rtGetSize_ = nullptr;
    const MethodInfo* rtSetSize_ = nullptr;
    const MethodInfo* rtGetPos_ = nullptr;
    const MethodInfo* rtSetPos_ = nullptr;
    bool layoutOk_ = false;

    FieldInfo* fClosureThis_ = nullptr;
    FieldInfo* fClosureUpgrade_ = nullptr;
    size_t offClosureThis_ = 0, offClosureUpgrade_ = 0;

    const MethodInfo* b0Mi_ = nullptr;           // <PromptForUpgrade>b__0
    const MethodInfo* rebuildMi_ = nullptr;      // ListUIRoot`4.RebuildUI (inflated)
    const MethodInfo* tmpSetTextMi_ = nullptr;   // TMP_Text.set_text
    const MethodInfo* setRemoveMi_ = nullptr;    // HashSet<Upgrade>.Remove (lazy)
    const MethodInfo* setCtorMi_ = nullptr;      // HashSet<Upgrade>..ctor() (lazy)
    Il2CppClass* setClass_ = nullptr;

    Bound<bool(Il2CppObject*, int32_t)> hasUpgrade_;
    Bound<void(Il2CppObject*, Il2CppObject*, bool)> applyUpgrades_;
    Bound<void(Il2CppObject*, Il2CppObject*, int32_t, bool, Il2CppObject*)> addCurrency_;
    Bound<bool(Il2CppObject**)> tryGetPlayerState_;
    Bound<Il2CppObject*(Il2CppObject*, Il2CppObject*)> getComponent_;
    Bound<Il2CppArray*(Il2CppObject*, Il2CppObject*)> getComponents_;
    Bound<Il2CppObject*(Il2CppObject*, Il2CppObject*, bool)> getComponentInChildren_;
    Bound<Il2CppObject*(Il2CppObject*)> getGameObject_;
    Bound<Il2CppObject*(Il2CppObject*)> getTransform_;
    Bound<Il2CppObject*(Il2CppObject*)> getParent_;
    Bound<void(Il2CppObject*, bool)> setActive_;
    Bound<void(Il2CppObject*, bool)> setEnabled_;
    Bound<void(Il2CppObject*, bool)> setInteractable_;  // Selectable.set_interactable (optional)
    Bound<void(Il2CppObject*, float)> tmpSetAlpha_;     // TMP_Text.set_alpha (optional)
    Bound<int32_t(Il2CppObject*)> foodCount_;
    // deluxe garden replant (optional: missing -> DELUXE_GARDEN not sellable)
    Bound<Il2CppObject*(Il2CppObject*)> primarySpawnId_;
    Bound<void(Il2CppObject*, Il2CppObject*)> initAsReplacement_;
    Bound<Il2CppObject*(Il2CppObject*, Il2CppObject*, bool)> plant_;
    Bound<void(Il2CppObject*, Il2CppString*)> destroy_;
    bool replantOk_ = false;

    using ObjectNewFn = Il2CppObject* (*)(Il2CppClass*);
    ObjectNewFn objectNew_ = nullptr;
};

SellUpgrades* g_self = nullptr;

sr2::core::DetourGuard g_actionGuard{kModule, "LandPlotUIRoot.OnAction"};
sr2::core::DetourGuard g_confirmGuard{kModule, "LandPlotUIRoot.<PromptForUpgrade>b__0"};

sr2::core::DetourGuard g_activatorGuard{kModule, "LandPlotUIRoot.SetActivator"};
sr2::core::DetourGuard g_bindGuard{kModule, "DetailsMoreButtonView.BindData"};
sr2::core::DetourGuard g_pressGuard{kModule, "Button.Press"};
sr2::core::DetourGuard g_showAltGuard{kModule, "ListUIRoot.ShowAltActionButton"};
sr2::core::DetourGuard g_altGuard{kModule, "ListUIRoot.OnAltAction"};

using OnActionFn = void (*)(Il2CppObject*, Il2CppObject*, const MethodInfo*);
using ConfirmFn = void (*)(Il2CppObject*, const MethodInfo*);
using SetActivatorFn = void (*)(Il2CppObject*, Il2CppObject*, const MethodInfo*);
using MoreBindFn = void (*)(Il2CppObject*, Il2CppObject*, const MethodInfo*);
using PressFn = void (*)(Il2CppObject*, const MethodInfo*);
using ShowAltFn = bool (*)(Il2CppObject*, Il2CppObject*, const MethodInfo*);
using OnAltActionFn = void (*)(Il2CppObject*, Il2CppObject*, const MethodInfo*);
OnActionFn g_origOnAction = nullptr;
ConfirmFn g_origConfirm = nullptr;
SetActivatorFn g_origSetActivator = nullptr;
MoreBindFn g_origMoreBind = nullptr;
PressFn g_origPress = nullptr;
ShowAltFn g_origShowAlt = nullptr;
OnAltActionFn g_origOnAlt = nullptr;

void SetActivatorDetour(Il2CppObject* self, Il2CppObject* plot, const MethodInfo* mi) {
    // Before the original: it ends in RebuildUI, which binds the details of the first item.
    if (g_self && self && g_self->On()) g_activatorGuard.Run([&] { g_self->TrackRoot(self); });
    g_origSetActivator(self, plot, mi);
}

void MoreBindDetour(Il2CppObject* view, Il2CppObject* vm, const MethodInfo* mi) {
    g_origMoreBind(view, vm, mi);
    if (g_self && view && g_self->On()) g_bindGuard.Run([&] { g_self->AfterMoreBind(view, vm); });
}

void PressDetour(Il2CppObject* button, const MethodInfo* mi) {
    // Every UI button of the game comes through here: one pointer compare for the others.
    if (g_self && button && button == g_self->SellButton() && g_self->On()) {
        bool handled = false;
        g_pressGuard.Run([&] { handled = g_self->OnSellButton(button); });
        if (handled) return;
    }
    g_origPress(button, mi);
}

// ListUIRoot`4.ShowAltActionButton / OnAltAction are shared generic bodies (every list menu):
// LandPlotUIRoot only, by class.
bool ShowAltDetour(Il2CppObject* self, Il2CppObject* item, const MethodInfo* mi) {
    if (g_self && self && item && g_self->On() && IsA(self, g_self->rootClass_)) {
        bool hide = false;
        g_showAltGuard.Run([&] { hide = g_self->HidesAltButton(self, item); });
        if (hide) return false;  // our Sell prompt stands in the alt slot for this card
    }
    return g_origShowAlt(self, item, mi);
}

void OnAltActionDetour(Il2CppObject* self, Il2CppObject* item, const MethodInfo* mi) {
    bool handled = false;
    if (g_self && self && item && g_self->On() && IsA(self, g_self->rootClass_))
        g_altGuard.Run([&] { handled = g_self->HandleAltAction(self, item); });
    if (!handled) g_origOnAlt(self, item, mi);
}

void OnActionDetour(Il2CppObject* self, Il2CppObject* item, const MethodInfo* mi) {
    bool handled = false;
    if (g_self && self && item && g_self->On())
        g_actionGuard.Run([&] { handled = g_self->HandleAction(self, item); });
    if (!handled) g_origOnAction(self, item, mi);
}

void ConfirmDetour(Il2CppObject* closure, const MethodInfo* mi) {
    bool refuse = false;
    if (g_self && closure && g_self->On()) g_confirmGuard.Run([&] { refuse = g_self->RefuseBuyAfterSale(closure); });
    if (refuse) return;
    SellUpgrades::AshRebuy rebuy;
    if (g_self && closure && g_self->On()) g_confirmGuard.Run([&] { rebuy = g_self->BeforeVanillaBuy(closure); });
    g_origConfirm(closure, mi);
    if (rebuy.plot && g_self && g_self->On()) g_confirmGuard.Run([&] { g_self->AfterVanillaBuy(rebuy); });
}

// ---------------------------------------------------------------- helpers --

Il2CppObject* SellUpgrades::Invoke(const MethodInfo* m, void* obj, std::initializer_list<void*> args,
                                   const char* what, bool* ok) {
    std::vector<void*> params(args);
    std::string err;
    Il2CppObject* r = Resolver::Get().Invoke(m, obj, params.empty() ? nullptr : params.data(), &err);
    const bool good = err.empty();
    if (ok) *ok = good;
    if (!good) {
        if (err.rfind("native exception", 0) == 0) {
            // A native fault inside game code: never continue as if nothing happened.
            sr2::core::MarkModuleFaulted(kModule, std::format("{}: {}", what, err));
        } else {
            SR2_WARN("[{}] {} failed: {}", kModule, what, err);
        }
    }
    return r;
}

Il2CppObject* SellUpgrades::GameObjectOf(Il2CppObject* comp) {
    if (!unity::IsAlive(comp)) return nullptr;
    Il2CppObject* go = getGameObject_(comp);
    return unity::IsAlive(go) ? go : nullptr;
}

void SellUpgrades::SetActive(Il2CppObject* go, bool on) {
    if (unity::IsAlive(go)) setActive_(go, on);
}

bool SellUpgrades::HashSetCall(Il2CppObject* set, const char* method, int32_t upg, bool* result) {
    Il2CppClass* k = api().il2cpp_object_get_class(set);
    if (!k) return false;
    if (k != setClass_) {
        std::string err;
        const MethodInfo* rm = Resolver::Get().Method(k, "Remove", 1, &err);
        const MethodInfo* ct = Resolver::Get().Method(k, ".ctor", 0, &err);
        if (!rm || !ct) {
            SR2_WARN("[{}] HashSet<Upgrade> methods not found: {}", kModule, err);
            return false;
        }
        setClass_ = k;
        setRemoveMi_ = rm;
        setCtorMi_ = ct;
    }
    (void)method;  // only Remove is used
    bool ok = false;
    Il2CppObject* boxed = Invoke(setRemoveMi_, set, {&upg}, "HashSet<Upgrade>.Remove", &ok);
    if (!ok || !boxed) return false;
    *result = *reinterpret_cast<bool*>(reinterpret_cast<char*>(boxed) + 2 * sizeof(void*));  // unboxed payload
    return true;
}

Il2CppObject* SellUpgrades::NewEmptySetLike(Il2CppObject* set) {
    Il2CppClass* k = api().il2cpp_object_get_class(set);
    if (!k || k != setClass_ || !setCtorMi_) return nullptr;
    Il2CppObject* fresh = objectNew_(k);
    if (!fresh) return nullptr;
    bool ok = false;
    Invoke(setCtorMi_, fresh, {}, "HashSet<Upgrade>..ctor", &ok);
    return ok ? fresh : nullptr;
}

std::string SellUpgrades::PlotId(Il2CppObject* plot) {
    std::string id;
    Il2CppObject* t = getTransform_(plot);
    Il2CppObject* parent = unity::IsAlive(t) ? getParent_(t) : nullptr;
    Il2CppObject* loc = unity::IsAlive(parent) ? getComponent_(parent, Type(locationClass_)) : nullptr;
    if (unity::IsAlive(loc) && IsA(loc, locationClass_))
        id = unity::ToUtf8(unity::Field<Il2CppString*>(loc, offLocationId_));
    if (id.empty()) id = unity::GetName(plot);
    return id.empty() ? std::string("?") : id;
}

// The refund is always half the price, rounded down (per currency).
bool SellUpgrades::Price(Il2CppObject* item, std::vector<Refund>& out, int32_t& price, int32_t& refund) {
    out.clear();
    price = refund = 0;
    void* list = unity::Field<void*>(item, offItemCosts_);
    if (!list) return true;  // free upgrade
    const size_t n = unity::ListCount(list);
    if (n > 16) return false;
    for (size_t i = 0; i < n; ++i) {
        auto* e = static_cast<Il2CppObject*>(unity::ListAt(list, i));
        if (!IsA(e, costEntryClass_)) continue;
        Refund r;
        r.currency = unity::Field<Il2CppObject*>(e, offCostCurrency_);
        r.price = unity::Field<int32_t>(e, offCostAmount_);
        if (!r.currency || r.price <= 0 || r.price > 100000000) continue;
        r.amount = r.price / 2;  // price > 0: integer division = floor(0.5 x price)
        price += r.price;
        refund += r.amount;
        out.push_back(r);
    }
    return true;
}

int SellUpgrades::CountUpgraders(Il2CppObject* plot, int32_t upg) {
    const UpgraderInfo* u = nullptr;
    switch (upg) {
        case WALLS: u = &walls_; break;
        case MUSIC_BOX: u = &music_; break;
        case SOIL: u = &soil_; break;
        case SPRINKLER: u = &sprinkler_; break;
        case SCARESLIME: u = &scare_; break;
        case FEEDER: u = &feederU_; break;
        case VITAMIZER: u = &vitamizer_; break;
        case AIR_NET: u = &airNet_; break;
        case PLORT_COLLECTOR:
        case PLORT_COLLECTOR_POND:
        case PLORT_COLLECTOR_INCINERATOR: u = &collector_; break;
        case SOLAR_SHIELD: u = &solar_; break;
        case ASH_TROUGH: u = &ash_; break;
        case MIRACLE_MIX: u = &miracle_; break;
        case DELUXE_GARDEN: u = &dGarden_; break;
        case DELUXE_COOP: u = &dCoop_; break;
        default: return 0;
    }
    if (!u->klass) return 0;
    Il2CppArray* arr = getComponents_(plot, Type(upgraderClass_));
    int n = 0;
    for (size_t i = 0, c = unity::ArrayLength(arr); i < c; ++i) {
        auto* comp = static_cast<Il2CppObject*>(unity::ArrayAt(arr, i));
        if (unity::IsAlive(comp) && IsA(comp, u->klass)) ++n;
    }
    return n;
}

// Empty string = can be sold. `brief` (optional): a few words for the sell button's label.
std::string SellUpgrades::CanSell(Il2CppObject* plot, int32_t upg, std::string* brief) {
    if (brief) brief->clear();
    // custom_upgrades ids (never in the vanilla set): that module decides.
    if (custom_upgrades::IsCustomUpgrade(upg)) return custom_upgrades::CanSell(plot, upg, brief);
    auto no = [brief](std::string why, std::string shortWhy) {
        if (brief) *brief = std::move(shortWhy);
        return why;
    };
    switch (upg) {
        case STORAGE2:
        case STORAGE3:
        case STORAGE4:
        case STORAGE_CAPACITY_INCREASE:
            return no("Silo storage upgrades can't be sold: they change the silo's slots and capacity, and the items "
                      "stored there could be lost.",
                      "silo storage");
        default: break;
    }
    if (upg < WALLS || upg > STORAGE_CAPACITY_INCREASE) return no("This upgrade can't be sold.", "not sellable");
    // A custom tier built on this vanilla upgrade (e.g. Music Box II) must be sold first.
    if (std::string dep = custom_upgrades::VanillaDependent(plot, upg); !dep.empty())
        return no(std::format("Sell '{}' first - it needs this upgrade.", dep), std::format("sell {} first", dep));
    if (upg == ASH_TROUGH && HasUpgrade(plot, PLORT_COLLECTOR_INCINERATOR))
        return no("The incinerator's Plort Collector needs the Ash Trough. Sell the Plort Collector first.",
                  "sell Plort Collector first");
    if (upg == DELUXE_GARDEN && !replantOk_)
        return no("This upgrade can't be sold (game functions not found).", "unavailable");
    if (CountUpgraders(plot, upg) <= 0) return no("This upgrade can't be sold on this plot.", "not on this plot");
    if (upg == FEEDER) {
        Il2CppObject* feeder = getComponentInChildren_(plot, Type(feederClass_), true);
        if (unity::IsAlive(feeder) && IsA(feeder, feederClass_)) {
            const int32_t food = foodCount_(feeder);
            if (food > 0)
                return no(std::format("The Auto-Feeder still holds {} food item{}. Let it feed them out (or wait until "
                                      "it is empty) before selling it, so no food is lost.",
                                      food, food == 1 ? "" : "s"),
                          "holds food");
        }
    }
    return {};
}

bool SellUpgrades::SetLabel(Il2CppObject* lse, const std::string& text) {
    if (!unity::IsAlive(lse) || !IsA(lse, lseClass_)) return false;
    // Our own button instance: stop its localisation from (re)writing the label, then set our text.
    setEnabled_(lse, false);
    Il2CppObject* tmp = getComponent_(lse, Type(tmpClass_));
    if (!unity::IsAlive(tmp)) return false;
    Il2CppString* s = unity::NewString(text);
    if (!s) return false;
    bool ok = false;
    Invoke(tmpSetTextMi_, tmp, {s}, "TMP_Text.set_text", &ok);
    return ok;
}

// The Sell button was pressed: sells `upg` at once (no confirm popup, user feedback), refund from
// `priceItem`'s price. DoSell re-checks everything and logs a refusal.
bool SellUpgrades::SellNow(Il2CppObject* root, Il2CppObject* priceItem, int32_t upg, const char* via) {
    if (!IsA(root, rootClass_) || !unity::IsAlive(root)) return false;
    if (!IsA(priceItem, itemClass_) || !unity::IsAlive(priceItem)) return false;
    Il2CppObject* plot = unity::Field<Il2CppObject*>(root, offActivator_);
    if (!IsA(plot, plotClass_) || !unity::IsAlive(plot) || !HasUpgrade(plot, upg)) return false;
    SR2_DEBUG("[{}] sell {} on plot '{}' ({})", kModule, UpgName(upg), PlotId(plot), via);
    sellTick_ = main_thread::TickCount();
    DoSell(root, plot, priceItem, upg);
    return true;
}

// ---------------------------------------------------------------- detours --

bool SellUpgrades::HandleAction(Il2CppObject* root, Il2CppObject* item) {
    if (!main_thread::IsMainThread()) return false;
    if (!IsA(item, itemClass_) || !unity::IsAlive(item)) return false;  // plots, demolish, clear crop: vanilla
    if (!IsA(root, rootClass_) || !unity::IsAlive(root)) return false;
    // A click on our Sell button can also arrive here through the prefab's own submit routing, for
    // the item that is selected after the sale (it may be buyable now): no purchase prompt for it.
    const unsigned long long tick = main_thread::TickCount();
    if (sellTick_ != ~0ull && tick >= sellTick_ && tick - sellTick_ <= 1) return true;
    Il2CppObject* plot = unity::Field<Il2CppObject*>(root, offActivator_);
    if (!IsA(plot, plotClass_) || !unity::IsAlive(plot)) return false;
    const int32_t upg = unity::Field<int32_t>(item, offItemUpgrade_);
    if (!HasUpgrade(plot, upg)) return false;  // not owned: vanilla purchase flow
    // Owned item confirmed (A / submit / clicking the selected card): nothing happens, as in
    // vanilla. Selling is only the Sell button (no popup, so no accidental sale on a double click).
    return true;
}

// ---------------------------------------------------------------- sell button --

void SellUpgrades::TrackRoot(Il2CppObject* root) {
    if (!main_thread::IsMainThread() || root == root_) return;
    if (!IsA(root, rootClass_) || !unity::IsAlive(root)) return;
    DestroySellButton();  // belongs to the previous menu
    DestroyAltPrompt();
    Il2CppGCHandle h = unity::NewGCHandle(root);
    if (!h) return;
    Il2CppGCHandle old = rootHandle_;
    rootHandle_ = h;
    root_ = root;
    if (old) main_thread::Post([old] { unity::FreeGCHandle(old); });
}

size_t SellUpgrades::OffsetIn(Il2CppObject* obj, const char* field, std::unordered_map<Il2CppClass*, size_t>& cache) {
    Il2CppClass* k = api().il2cpp_object_get_class(obj);
    if (!k) return 0;
    auto it = cache.find(k);
    if (it != cache.end()) return it->second;
    size_t off = 0;
    std::string err;
    for (Il2CppClass* c = k; c && api().il2cpp_class_get_parent; c = api().il2cpp_class_get_parent(c))
        if (auto o = Resolver::Get().FieldOffset(c, field, &err)) {
            off = *o;
            break;
        }
    cache.emplace(k, off);
    return off;
}

// The upgrade item of the open plot menu whose own _infoButtonLabel instance is `infoLabel`.
Il2CppObject* SellUpgrades::FindItemByInfoLabel(Il2CppObject* root, Il2CppObject* infoLabel, Il2CppObject*& category) {
    category = nullptr;
    Il2CppObject* config = unity::Field<Il2CppObject*>(root, offMenuConfig_);
    if (!unity::IsAlive(config)) return nullptr;
    const size_t offCats = OffsetIn(config, "categories", offCategories_);
    if (!offCats) return nullptr;
    void* cats = unity::Field<void*>(config, offCats);
    for (size_t ci = 0, cn = unity::ListCount(cats); ci < cn && ci < 32; ++ci) {
        auto* cat = static_cast<Il2CppObject*>(unity::ListAt(cats, ci));
        if (!unity::IsAlive(cat)) continue;
        const size_t offItems = OffsetIn(cat, "items", offItems_);
        if (!offItems) continue;
        void* items = unity::Field<void*>(cat, offItems);
        for (size_t i = 0, n = unity::ListCount(items); i < n && i < 64; ++i) {
            auto* item = static_cast<Il2CppObject*>(unity::ListAt(items, i));
            if (!IsA(item, itemClass_)) continue;
            if (unity::Field<Il2CppObject*>(item, offItemInfoLabel_) != infoLabel) continue;
            if (!unity::IsAlive(item)) return nullptr;
            category = cat;
            return item;
        }
    }
    return nullptr;
}

void SellUpgrades::DestroySellButton() {
    if (!main_thread::IsMainThread()) return;
    if (btnGo_ && unity::IsAlive(btnGo_) && objectDestroy_) objectDestroy_(btnGo_);
    btnGo_ = btnButton_ = btnItem_ = btnAltCard_ = nullptr;
    btnUpgrade_ = 0;
    if (Il2CppGCHandle h = btnHandle_) {
        btnHandle_ = nullptr;
        main_thread::Post([h] { unity::FreeGCHandle(h); });
    }
    ++pairGen_;
    pairVanilla_ = nullptr;
    if (Il2CppGCHandle h = pairVanillaHandle_) {
        pairVanillaHandle_ = nullptr;
        main_thread::Post([h] { unity::FreeGCHandle(h); });
    }
}

Vec2 SellUpgrades::GetVec(const MethodInfo* m, Il2CppObject* rt, const char* what, bool& ok) {
    bool good = false;
    Il2CppObject* boxed = ok ? Invoke(m, rt, {}, what, &good) : nullptr;
    if (!good || !boxed) {
        ok = false;
        return {};
    }
    return *reinterpret_cast<Vec2*>(reinterpret_cast<char*>(boxed) + 2 * sizeof(void*));  // unboxed payload
}

void SellUpgrades::SetVec(const MethodInfo* m, Il2CppObject* rt, Vec2 v, const char* what, bool& ok) {
    if (!ok) return;
    bool good = false;
    Invoke(m, rt, {&v}, what, &good);
    ok = good;
}

// The action container has no LayoutGroup (PURCHASED / Purchase fill it alone). The buttons size
// themselves: the game's own layout (LayoutManager.LateUpdate: HorizontalLayout +
// EncapsulateChildren on the button root, ResizeToPreferred on its label; prefab
// TextButton_NoneSelectable, read from the asset bundle) sets each root's width from its label, so
// a sizeDelta we write is overwritten at the next LateUpdate (build 506a4f60: the halves grew back
// to full width and Purchase ran under Sell). So we never size, we only place:
//   * alone (owned card, nothing left to buy): the vanilla PURCHASED view is hidden and ours takes
//     its anchors and centre;
//   * pair (tier card: Purchase + Sell): both keep the vanilla anchors; the vanilla view gets
//     pivot x 1 and ours pivot x 0, both at the old centre -/+ gap / 2, so each grows away from the
//     middle and they can't overlap whatever their widths. CentrePair then shifts the pair so it is
//     centred on the old centre once the layout has given them their real widths (posted, a few
//     ticks).
bool SellUpgrades::PlaceButtons(Il2CppObject* vanillaView, Il2CppObject* ourView, bool alone) {
    if (!layoutOk_ || !unity::IsAlive(vanillaView) || !unity::IsAlive(ourView)) return false;
    Il2CppObject* a = getTransform_(vanillaView);
    Il2CppObject* b = getTransform_(ourView);
    if (!unity::IsAlive(a) || !unity::IsAlive(b) || !IsA(a, rtClass_) || !IsA(b, rtClass_)) return false;
    bool ok = false;
    Il2CppObject* boxedRect = Invoke(rtGetRect_, a, {}, "RectTransform.get_rect", &ok);
    if (!ok || !boxedRect) return false;
    const Rect r = *reinterpret_cast<Rect*>(reinterpret_cast<char*>(boxedRect) + 2 * sizeof(void*));
    const Vec2 amin = GetVec(rtGetAnchorMin_, a, "RectTransform.get_anchorMin", ok);
    const Vec2 amax = GetVec(rtGetAnchorMax_, a, "RectTransform.get_anchorMax", ok);
    const Vec2 pivot = GetVec(rtGetPivot_, a, "RectTransform.get_pivot", ok);
    const Vec2 pos = GetVec(rtGetPos_, a, "RectTransform.get_anchoredPosition", ok);
    if (!ok || !std::isfinite(r.width) || r.width < 0.0f || r.width > 10000.0f || !std::isfinite(pivot.x) ||
        !std::isfinite(pos.x))
        return false;
    SetVec(rtSetAnchorMin_, b, amin, "RectTransform.set_anchorMin", ok);
    SetVec(rtSetAnchorMax_, b, amax, "RectTransform.set_anchorMax", ok);
    // Centre of the vanilla rect, in the anchored-position frame.
    const float centre = pos.x + (0.5f - pivot.x) * r.width;
    if (alone) {
        SetVec(rtSetPivot_, b, Vec2{0.5f, pivot.y}, "RectTransform.set_pivot", ok);
        SetVec(rtSetPos_, b, Vec2{centre, pos.y}, "RectTransform.set_anchoredPosition", ok);
        if (ok) SetActive(GameObjectOf(vanillaView), false);  // ListUI destroys it on the next bind
    } else {
        SetVec(rtSetPivot_, a, Vec2{1.0f, pivot.y}, "RectTransform.set_pivot", ok);
        SetVec(rtSetPos_, a, Vec2{centre - kPairGap * 0.5f, pos.y}, "RectTransform.set_anchoredPosition", ok);
        SetVec(rtSetPivot_, b, Vec2{0.0f, pivot.y}, "RectTransform.set_pivot", ok);
        SetVec(rtSetPos_, b, Vec2{centre + kPairGap * 0.5f, pos.y}, "RectTransform.set_anchoredPosition", ok);
        if (ok) {
            pairVanillaHandle_ = unity::NewGCHandle(a);
            if (pairVanillaHandle_) {
                pairVanilla_ = a;
                pairCentre_ = centre;
                ScheduleCentre(++pairGen_, 12);
            }
        }
    }
    if (!loggedLayout_) {
        loggedLayout_ = true;
        SR2_INFO("[{}] sell button layout: vanilla rect {:.0f} x {:.0f} (anchors {:.2f},{:.2f}-{:.2f},{:.2f}, pivot "
                 "{:.2f}, centre x {:.0f}) - {}{}",
                 kModule, r.width, r.height, amin.x, amin.y, amax.x, amax.y, pivot.x, centre,
                 alone ? "Sell alone (PURCHASED hidden)" : "Purchase left, Sell right", ok ? "" : " - FAILED");
    }
    return ok;
}

void SellUpgrades::ScheduleCentre(unsigned gen, int ticksLeft) {
    main_thread::Post([this, gen, ticksLeft] {
        if (gen != pairGen_ || !On()) return;
        CentrePair();
        if (ticksLeft > 1) ScheduleCentre(gen, ticksLeft - 1);
    });
}

// Shift Purchase (pivot x 1) and Sell (pivot x 0) so the pair is centred on the old centre, with the
// widths the game's layout has given them by now. Posted job (main thread), our pair only.
void SellUpgrades::CentrePair() {
    Il2CppObject* a = pairVanilla_;
    Il2CppObject* b = btnGo_ && unity::IsAlive(btnGo_) ? unity::TransformOf(btnGo_) : nullptr;
    if (!a || !unity::IsAlive(a) || !unity::IsAlive(b) || !IsA(a, rtClass_) || !IsA(b, rtClass_)) return;
    bool ok = false;
    Il2CppObject* ra = Invoke(rtGetRect_, a, {}, "RectTransform.get_rect", &ok);
    if (!ok || !ra) return;
    const float wa = reinterpret_cast<Rect*>(reinterpret_cast<char*>(ra) + 2 * sizeof(void*))->width;
    Il2CppObject* rb = Invoke(rtGetRect_, b, {}, "RectTransform.get_rect", &ok);
    if (!ok || !rb) return;
    const float wb = reinterpret_cast<Rect*>(reinterpret_cast<char*>(rb) + 2 * sizeof(void*))->width;
    if (!std::isfinite(wa) || !std::isfinite(wb) || wa < 0.0f || wb < 0.0f || wa > 10000.0f || wb > 10000.0f) return;
    const float shift = (wa - wb) * 0.5f;
    const Vec2 pa = GetVec(rtGetPos_, a, "RectTransform.get_anchoredPosition", ok);
    const Vec2 pb = GetVec(rtGetPos_, b, "RectTransform.get_anchoredPosition", ok);
    if (!ok) return;
    const float xa = pairCentre_ - kPairGap * 0.5f + shift;
    const float xb = pairCentre_ + kPairGap * 0.5f + shift;
    if (std::fabs(pa.x - xa) > 0.5f) SetVec(rtSetPos_, a, Vec2{xa, pa.y}, "RectTransform.set_anchoredPosition", ok);
    if (std::fabs(pb.x - xb) > 0.5f) SetVec(rtSetPos_, b, Vec2{xb, pb.y}, "RectTransform.set_anchoredPosition", ok);
}

// Short reasons that go away by themselves or by selling something else first ("holds food",
// "sell Music Box II first") get a disabled button saying so; permanent ones ("silo storage", "not
// sellable") get no button at all.
bool TemporaryReason(const std::string& brief) { return brief.rfind("holds ", 0) == 0 || brief.rfind("sell ", 0) == 0; }

void SellUpgrades::AfterMoreBind(Il2CppObject* view, Il2CppObject* vm) {
    if (!main_thread::IsMainThread()) return;
    // Any details bind (selection change, rebuild) replaces our previous button and prompt.
    if (btnGo_) DestroySellButton();
    if (altGo_ || altItem_) DestroyAltPrompt();
    if (!root_ || !unity::IsAlive(root_) || !vm) return;
    if (!IsA(view, moreViewClass_) || !unity::IsAlive(view)) return;
    Il2CppObject* infoLabel = unity::Field<Il2CppObject*>(vm, offVmInfoLabel_);
    if (!infoLabel) return;
    Il2CppObject* category = nullptr;
    Il2CppObject* item = FindItemByInfoLabel(root_, infoLabel, category);
    if (!item || !category) return;  // another menu / not an upgrade item
    Il2CppObject* plot = unity::Field<Il2CppObject*>(root_, offActivator_);
    if (!IsA(plot, plotClass_) || !unity::IsAlive(plot)) return;

    // What the button sells: an owned item (PURCHASED variant) sells itself; the card of a custom
    // tier chain that shows the next tier (Purchase variant) sells the owned tier below it.
    const bool purchaseVariant = unity::Field<Il2CppObject*>(view, offMoreButton_) != nullptr;
    int32_t upg = unity::Field<int32_t>(item, offItemUpgrade_);
    Il2CppObject* priceItem = item;
    if (purchaseVariant && !custom_upgrades::ChainSellTarget(plot, item, upg, priceItem)) return;
    if (!priceItem || !HasUpgrade(plot, upg)) return;

    std::string brief;
    const std::string reason = CanSell(plot, upg, &brief);
    std::vector<Refund> refunds;
    int32_t price = 0, refund = 0;
    const bool sellable = reason.empty() && Price(priceItem, refunds, price, refund);
    // Gamepad / keyboard: the alt action sells it (prompt only while it can be sold now).
    if (sellable && altOk_) CreateAltPrompt(category, item, priceItem, upg, refund);
    if (!sellable && !TemporaryReason(brief)) return;  // no "can't sell" button (user feedback)

    const size_t offPrefab = OffsetIn(category, "detailsButton", offDetailsButton_);
    Il2CppObject* prefab = offPrefab ? unity::Field<Il2CppObject*>(category, offPrefab) : nullptr;
    if (!unity::IsAlive(prefab)) return;
    Il2CppObject* t = getTransform_(view);
    Il2CppObject* parent = unity::IsAlive(t) ? getParent_(t) : nullptr;
    if (!unity::IsAlive(parent)) return;

    Il2CppObject* go = unity::Instantiate(prefab, parent);
    if (!unity::IsAlive(go)) return;
    btnHandle_ = unity::NewGCHandle(go);
    if (!btnHandle_) {
        if (objectDestroy_) objectDestroy_(go);
        return;
    }
    btnGo_ = go;
    btnItem_ = priceItem;
    btnUpgrade_ = upg;
    unity::SetName(go, "sr2mods_SellButton");
    Il2CppObject* btnView = unity::GetComponent(go, moreViewClass_);
    Il2CppObject* button = btnView ? unity::Field<Il2CppObject*>(btnView, offMoreButton_) : nullptr;
    btnButton_ = unity::IsAlive(button) ? button : nullptr;
    // The prefab (TextButton_NoneSelectable) carries an InputEventButton: on pointer down / up it
    // performs its InputEvent (the menu's submit) itself, before the Button's click reaches our
    // Press detour. On a tier card that submit opened the purchase prompt of the shown tier, and
    // the same click confirmed it (selling Music Box / Fast Growth I bought tier II in the same
    // frame). Disabled, it gets no pointer events (EventSystem skips disabled behaviours); the
    // Button's own click still sells. Its InputEventDisplay shows the submit key, which on this
    // button would buy, not sell: when the button can sell now and the alt action is ours, the
    // display is pointed at the menu's altSubmit InputEvent (the key that sells: the game's own icon
    // for the current device, as Purchase shows submit); otherwise it is hidden with its "Icon
    // Background".
    Il2CppObject* goT = unity::TransformOf(go);  // Component calls below go through the transform
    if (inputEventButtonClass_ && unity::IsAlive(goT)) {
        Il2CppObject* ieb = getComponent_(goT, Type(inputEventButtonClass_));
        if (unity::IsAlive(ieb) && IsA(ieb, inputEventButtonClass_)) setEnabled_(ieb, false);
    }
    const char* keyIcon = "none";
    if (inputEventDisplayClass_ && unity::IsAlive(goT)) {
        Il2CppObject* disp = getComponentInChildren_(goT, Type(inputEventDisplayClass_), true);
        if (!unity::IsAlive(disp) || !IsA(disp, inputEventDisplayClass_)) disp = nullptr;
        bool shown = false;
        if (disp && sellable && altOk_ && displaySetEventMi_) {
            Il2CppObject* list = unity::Field<Il2CppObject*>(root_, offSwapper_);
            Il2CppObject* ev = IsA(list, listUiClass_) && unity::IsAlive(list)
                                   ? unity::Field<Il2CppObject*>(list, offAltSubmit_) : nullptr;
            if (unity::IsAlive(ev)) {
                bool ok = false;
                Invoke(displaySetEventMi_, disp, {ev, nullptr}, "InputEventDisplay.SetInputEvent", &ok);
                if (ok) {
                    shown = true;
                    btnAltCard_ = item;  // the alt key sells this card's target (HandleAltAction)
                    keyIcon = "alt submit";
                }
            }
        }
        if (disp && !shown) {
            Il2CppObject* dt = getTransform_(disp);
            Il2CppObject* iconBg = unity::IsAlive(dt) ? getParent_(dt) : nullptr;
            // Only the icon's own holder (a direct child of the button root), never the root itself.
            if (unity::IsAlive(iconBg) && iconBg != goT && getParent_(iconBg) == goT) {
                SetActive(GameObjectOf(iconBg), false);
                keyIcon = "hidden";
            }
        }
    }

    // Owned card (PURCHASED variant): Sell alone, PURCHASED hidden (user feedback). Tier card
    // (Purchase variant): side by side, by the container's LayoutGroup if it has one, else by
    // PlaceButtons. A Sell button that would cover the Purchase button is not shown.
    const bool layoutGroup = layoutGroupClass_ && unity::GetComponent(parent, layoutGroupClass_) != nullptr;
    bool placed = false;
    if (layoutGroup) {
        placed = true;
        if (!purchaseVariant) SetActive(GameObjectOf(view), false);
    } else {
        placed = PlaceButtons(view, btnView, !purchaseVariant);
    }
    if (!placed && purchaseVariant) {
        DestroySellButton();
        return;
    }

    std::string text;
    if (sellable) {
        text = std::format("Sell +{}", refund);
    } else {
        text = brief;  // "holds food" -> "Holds food"
        if (!text.empty() && text[0] >= 'a' && text[0] <= 'z') text[0] = static_cast<char>(text[0] - 'a' + 'A');
    }
    Il2CppObject* labelLse = btnView ? unity::Field<Il2CppObject*>(btnView, offMoreLabel_) : nullptr;
    bool labelled = btnView && SetLabel(labelLse, text);
    Il2CppObject* labelTmp = labelled ? getComponent_(labelLse, Type(tmpClass_)) : nullptr;
    if (!labelled && btnView) {
        // Label text not on the LocalizeStringEvent's own object: first TMP_Text below the button.
        Il2CppObject* tmp = getComponentInChildren_(btnView, Type(tmpClass_), true);
        if (unity::IsAlive(tmp)) {
            bool ok = false;
            Invoke(tmpSetTextMi_, tmp, {unity::NewString(text)}, "TMP_Text.set_text", &ok);
            labelled = ok;
            if (ok) labelTmp = tmp;
        }
    }
    if (!sellable) {
        // Disabled look, no popup: the Selectable's own non-interactable state (prefab transition,
        // skipped by navigation) plus a dimmed label.
        if (btnButton_ && setInteractable_) setInteractable_(btnButton_, false);
        if (unity::IsAlive(labelTmp) && IsA(labelTmp, tmpClass_) && tmpSetAlpha_) tmpSetAlpha_(labelTmp, 0.5f);
    }
    if (!loggedButton_) {
        loggedButton_ = true;
        SR2_INFO("[{}] sell button: prefab '{}' cloned into '{}' (LayoutGroup {}, side by side {}), Button {}, label {} "
                 "- '{}', key icon {}",
                 kModule, unity::GetName(prefab), unity::GetName(parent), layoutGroup ? "yes" : "no",
                 placed ? "yes" : "NO", btnButton_ ? "found" : "MISSING", labelled ? "set" : "NOT set", text, keyIcon);
    }
}

bool SellUpgrades::OnSellButton(Il2CppObject* button) {
    if (!main_thread::IsMainThread() || button != btnButton_ || !unity::IsAlive(button)) return false;
    // Always handled: the original Press would submit the menu, i.e. buy the selected card's tier.
    if (sellTick_ == main_thread::TickCount()) return true;  // one sale per frame
    if (root_ && unity::IsAlive(root_) && btnItem_ && btnUpgrade_) SellNow(root_, btnItem_, btnUpgrade_, "sell button");
    return true;
}

// ---------------------------------------------------------------- alt-action sell --

void SellUpgrades::DestroyAltPrompt() {
    if (!main_thread::IsMainThread()) return;
    if (altGo_ && unity::IsAlive(altGo_) && objectDestroy_) objectDestroy_(altGo_);
    altGo_ = altItem_ = altPriceItem_ = nullptr;
    altUpgrade_ = 0;
    if (Il2CppGCHandle h = altHandle_) {
        altHandle_ = nullptr;
        main_thread::Post([h] { unity::FreeGCHandle(h); });
    }
}

// The prompt's label: InputHint (_label / _labelText), else TextLabel, else a DetailsMoreButtonView.
// The LocalizeStringEvent is disabled first so it can't write "Info" back (InputHint.Start ->
// Configure only sets its StringReference, which a disabled event does not apply).
bool SellUpgrades::SetAltLabel(Il2CppObject* go, const std::string& text, const char*& via) {
    via = "none";
    Il2CppObject* t = unity::TransformOf(go);
    if (!unity::IsAlive(t)) return false;
    auto set = [&](Il2CppObject* lse, Il2CppObject* tmp) -> bool {
        const bool lseOk = unity::IsAlive(lse) && IsA(lse, lseClass_);
        if (lseOk) setEnabled_(lse, false);
        if (!unity::IsAlive(tmp) && lseOk) tmp = getComponent_(lse, Type(tmpClass_));
        if (!unity::IsAlive(tmp) || !IsA(tmp, tmpClass_)) return false;
        Il2CppString* s = unity::NewString(text);
        if (!s) return false;
        bool ok = false;
        Invoke(tmpSetTextMi_, tmp, {s}, "TMP_Text.set_text", &ok);
        return ok;
    };
    if (hintClass_) {
        Il2CppObject* hint = getComponentInChildren_(t, Type(hintClass_), true);
        if (unity::IsAlive(hint) && IsA(hint, hintClass_)) {
            via = "InputHint";
            return set(unity::Field<Il2CppObject*>(hint, offHintLabel_), unity::Field<Il2CppObject*>(hint, offHintText_));
        }
    }
    if (textLabelClass_) {
        Il2CppObject* label = getComponentInChildren_(t, Type(textLabelClass_), true);
        if (unity::IsAlive(label) && IsA(label, textLabelClass_)) {
            via = "TextLabel";
            return set(unity::Field<Il2CppObject*>(label, offTextLabelLse_),
                       unity::Field<Il2CppObject*>(label, offTextLabelTmp_));
        }
    }
    Il2CppObject* view = getComponentInChildren_(t, Type(moreViewClass_), true);
    if (unity::IsAlive(view) && IsA(view, moreViewClass_)) {
        via = "DetailsMoreButtonView";
        return SetLabel(unity::Field<Il2CppObject*>(view, offMoreLabel_), text);
    }
    return false;
}

// A copy of the game's own alt-action hint (category.altDetailsButton) in the ListUI's alt slot,
// labelled "Sell +N". Called from the details bind, before ListUI.CreateDetailsAltButton (which only
// destroys its own instance, and skips this card through the ShowAltActionButton detour).
void SellUpgrades::CreateAltPrompt(Il2CppObject* category, Il2CppObject* item, Il2CppObject* priceItem, int32_t upg,
                                   int32_t refund) {
    Il2CppObject* list = unity::Field<Il2CppObject*>(root_, offSwapper_);
    if (!IsA(list, listUiClass_) || !unity::IsAlive(list)) return;
    Il2CppObject* container = unity::Field<Il2CppObject*>(list, offAltContainer_);
    if (!unity::IsAlive(container)) return;
    // The card's own category first, else any category of this menu that has the hint prefab.
    Il2CppObject* prefab = nullptr;
    if (const size_t o = OffsetIn(category, "altDetailsButton", offAltButton_))
        prefab = unity::Field<Il2CppObject*>(category, o);
    bool borrowed = false;
    if (!unity::IsAlive(prefab)) {
        prefab = nullptr;
        Il2CppObject* config = unity::Field<Il2CppObject*>(root_, offMenuConfig_);
        const size_t offCats = unity::IsAlive(config) ? OffsetIn(config, "categories", offCategories_) : 0;
        void* cats = offCats ? unity::Field<void*>(config, offCats) : nullptr;
        for (size_t ci = 0, cn = unity::ListCount(cats); ci < cn && ci < 32 && !prefab; ++ci) {
            auto* cat = static_cast<Il2CppObject*>(unity::ListAt(cats, ci));
            const size_t o = unity::IsAlive(cat) ? OffsetIn(cat, "altDetailsButton", offAltButton_) : 0;
            Il2CppObject* p = o ? unity::Field<Il2CppObject*>(cat, o) : nullptr;
            if (unity::IsAlive(p)) prefab = p;
        }
        borrowed = prefab != nullptr;
    }
    if (!prefab) {
        if (!loggedAlt_) {
            loggedAlt_ = true;
            SR2_WARN("[{}] alt-action sell: no altDetailsButton prefab in this menu - no Sell prompt", kModule);
        }
        return;
    }
    Il2CppObject* go = unity::Instantiate(prefab, container);
    if (!unity::IsAlive(go)) return;
    altHandle_ = unity::NewGCHandle(go);
    if (!altHandle_) {
        if (objectDestroy_) objectDestroy_(go);
        return;
    }
    altGo_ = go;
    unity::SetName(go, "sr2mods_SellHint");
    const char* via = "none";
    const std::string text = std::format("Sell +{}", refund);
    const bool labelled = SetAltLabel(go, text, via);
    if (!loggedAlt_) {
        loggedAlt_ = true;
        Il2CppObject* ev = unity::Field<Il2CppObject*>(list, offAltSubmit_);
        SR2_INFO("[{}] alt-action sell: prefab '{}'{} cloned into '{}', label via {} {} - '{}'; input event '{}'",
                 kModule, unity::GetName(prefab), borrowed ? " (from another category)" : "",
                 unity::GetName(container), via, labelled ? "set" : "NOT set (no prompt)", text,
                 unity::IsAlive(ev) ? unity::GetName(ev) : std::string("?"));
    }
    if (!labelled) {  // an "Info" hint that sells would mislead: no prompt, no alt-action sale
        DestroyAltPrompt();
        return;
    }
    altItem_ = item;
    altPriceItem_ = priceItem;
    altUpgrade_ = upg;
}

bool SellUpgrades::HandleAltAction(Il2CppObject* root, Il2CppObject* item) {
    if (!main_thread::IsMainThread() || root != root_ || !unity::IsAlive(root)) return false;
    if (altGo_ && item == altItem_ && unity::IsAlive(altGo_)) {
        // Handled either way: this press is the Sell prompt's, never the Pedia's. One sale per frame.
        if (sellTick_ != main_thread::TickCount() && altPriceItem_ && altUpgrade_)
            SellNow(root, altPriceItem_, altUpgrade_, "alt action");
        return true;
    }
    // No prompt (prefab / label missing), but the Sell button shows the alt key: that key sells too.
    if (btnAltCard_ && item == btnAltCard_ && btnGo_ && unity::IsAlive(btnGo_)) {
        if (sellTick_ != main_thread::TickCount() && btnItem_ && btnUpgrade_)
            SellNow(root, btnItem_, btnUpgrade_, "alt action (sell button key)");
        return true;
    }
    return false;  // vanilla Info / Pedia
}

// ---------------------------------------------------------------- ash trough re-purchase --

void SellUpgrades::RememberAsh(Il2CppObject* model, float ash) {
    if (!ashRestoreOk_ || !model) return;
    if (!std::isfinite(ash) || ash < 0.0f) ash = 0.0f;
    auto it = ashRecords_.find(model);
    if (it != ashRecords_.end()) {
        it->second.ash = ash;
        return;
    }
    Il2CppGCHandle h = unity::NewGCHandle(model);
    if (!h) return;  // no record: a re-purchase fills the trough as in vanilla
    ashRecords_.emplace(model, AshRecord{h, ash});
}

Il2CppObject* SellUpgrades::AshTroughObject(Il2CppObject* plot) {
    if (!ash_.klass) return nullptr;
    Il2CppArray* arr = getComponents_(plot, Type(upgraderClass_));
    for (size_t i = 0, c = unity::ArrayLength(arr); i < c; ++i) {
        auto* comp = static_cast<Il2CppObject*>(unity::ArrayAt(arr, i));
        if (!unity::IsAlive(comp) || !IsA(comp, ash_.klass)) continue;
        Il2CppObject* go = unity::Field<Il2CppObject*>(comp, ash_.off1);  // AshTroughUpgrader.AshTrough
        if (unity::IsAlive(go)) return go;
    }
    return nullptr;
}

// The FillableAshSource is on the trough's child "Ash Level" (patchIncinerator prefab: AshTrough ->
// Meshes (floor, walls) + Ash Level), the way vanilla OnInitialPurchase finds it
// (GetComponentInChildren). GetComponent on AshTrough itself never found it ("trough not found").
Il2CppObject* SellUpgrades::FindAshSource(Il2CppObject* plot) {
    Il2CppObject* go = AshTroughObject(plot);
    if (!go || !ashSourceClass_) return nullptr;
    Il2CppObject* src = getComponentInChildren_(unity::TransformOf(go), Type(ashSourceClass_), true);
    return unity::IsAlive(src) && IsA(src, ashSourceClass_) ? src : nullptr;
}

// Per-save copy of the remembered ash (custom_upgrades sidecar of the .sav file): survives a
// reload once the game was saved after the sale.
constexpr const char* kAshValueKey = "sell_upgrades.ash_trough";

float SavedAsh(Il2CppObject* plot) {
    const std::string v = custom_upgrades::PlotValue(plot, kAshValueKey);
    if (v.empty()) return -1.0f;
    char* end = nullptr;
    const float f = std::strtof(v.c_str(), &end);
    return (end != v.c_str() && std::isfinite(f) && f >= 0.0f) ? f : -1.0f;
}

bool SellUpgrades::RefuseBuyAfterSale(Il2CppObject* closure) {
    if (!main_thread::IsMainThread() || sellTick_ == ~0ull) return false;
    const unsigned long long tick = main_thread::TickCount();
    if (tick < sellTick_ || tick - sellTick_ > 1) return false;
    const int32_t upg =
        offClosureUpgrade_ && IsA(closure, closureClass_) ? unity::Field<int32_t>(closure, offClosureUpgrade_) : 0;
    SR2_INFO("[{}] purchase of {} right after a sale refused (same click)", kModule, upg ? UpgName(upg) : "?");
    return true;
}

SellUpgrades::AshRebuy SellUpgrades::BeforeVanillaBuy(Il2CppObject* closure) {
    if (!ash_.klass || !main_thread::IsMainThread()) return {};
    if (!IsA(closure, closureClass_)) return {};
    if (unity::Field<int32_t>(closure, offClosureUpgrade_) != ASH_TROUGH) return {};
    auto* root = unity::Field<Il2CppObject*>(closure, offClosureThis_);
    if (!IsA(root, rootClass_) || !unity::IsAlive(root)) return {};
    Il2CppObject* plot = unity::Field<Il2CppObject*>(root, offActivator_);
    if (!IsA(plot, plotClass_) || !unity::IsAlive(plot) || HasUpgrade(plot, ASH_TROUGH)) return {};
    Il2CppObject* model = unity::Field<Il2CppObject*>(plot, offPlotModel_);
    if (!IsA(model, modelClass_)) return {};
    if (!ashRestoreOk_) return {plot, model, -1.0f, false};  // the trough check only
    if (ashRecords_.count(model)) return {plot, model, -1.0f, true};
    const float saved = SavedAsh(plot);  // after a reload: the copy in the save's sidecar
    return {plot, model, saved, saved >= 0.0f};
}

void SellUpgrades::AfterVanillaBuy(const AshRebuy& r) {
    if (!main_thread::IsMainThread()) return;
    // Only after a completed purchase on the same plot / model (cancelled or failed: keep the record).
    if (!unity::IsAlive(r.plot) || !IsA(r.plot, plotClass_) || !HasUpgrade(r.plot, ASH_TROUGH)) return;
    if (unity::Field<Il2CppObject*>(r.plot, offPlotModel_) != r.model) return;
    const std::string id = PlotId(r.plot);
    // The trough model: vanilla AshTroughUpgrader.Apply (0x983930) sets AshTrough active again; the
    // sale deactivated it. Checked (and switched on if Apply did not run) so a re-purchase always shows it.
    Il2CppObject* trough = AshTroughObject(r.plot);
    if (!trough) {
        SR2_WARN("[{}] Ash Trough bought on plot '{}': AshTroughUpgrader.AshTrough not found (no trough model)", kModule, id);
    } else if (!unity::ActiveInHierarchy(trough)) {  // its parent is the plot root (active)
        SetActive(trough, true);
        SR2_WARN("[{}] Ash Trough bought on plot '{}': trough model was still hidden - shown now (active: {})", kModule,
                 id, unity::ActiveInHierarchy(trough) ? "yes" : "no");
    } else {
        SR2_INFO("[{}] Ash Trough bought on plot '{}': trough model shown", kModule, id);
    }
    if (!r.record) return;
    auto it = ashRecords_.find(r.model);
    const bool inMemory = it != ashRecords_.end();
    if (!inMemory && r.saved < 0.0f) return;
    const float remembered = inMemory ? it->second.ash : r.saved;
    if (inMemory) {
        const Il2CppGCHandle h = it->second.handle;
        ashRecords_.erase(it);
        if (h) main_thread::Post([h] { unity::FreeGCHandle(h); });
    }
    custom_upgrades::SetPlotValue(r.plot, kAshValueKey, "");  // used up (saved with the next game save)

    Il2CppObject* src = FindAshSource(r.plot);
    if (!src) {
        SR2_WARN("[{}] Ash Trough bought back on plot '{}': trough not found, vanilla fill kept", kModule, id);
        return;
    }
    const int32_t maxUnits = unity::Field<int32_t>(src, offAshMax_);
    float& ash = unity::Field<float>(r.model, offModelAsh_);
    const float vanilla = ash;
    float v = remembered;
    if (maxUnits >= 0 && v > static_cast<float>(maxUnits)) v = static_cast<float>(maxUnits);
    ash = v;
    // AddAsh works on the source's own _plotModel: refresh the mesh only if that is this model.
    if (unity::Field<Il2CppObject*>(src, offAshModel_) == r.model) addAsh_(src, 0.0f);  // clamps, moves the ash
    SR2_INFO("[{}] Ash Trough bought back on plot '{}': ash set to {:.1f} (remembered at sale{}; vanilla fill {:.1f}, "
             "max {})",
             kModule, id, ash, inMemory ? "" : ", from the save's sidecar", vanilla, maxUnits);
}

// ---------------------------------------------------------------- selling --

void SellUpgrades::DoSell(Il2CppObject* root, Il2CppObject* plot, Il2CppObject* item, int32_t upg) {
    const std::string id = PlotId(plot);
    std::string reason = CanSell(plot, upg);  // re-check (state may have changed while the popup was open)
    if (!reason.empty()) {
        SR2_INFO("[{}] sale cancelled: plot '{}', {}: {}", kModule, id, UpgName(upg), reason);
        return;
    }
    Il2CppObject* model = unity::Field<Il2CppObject*>(plot, offPlotModel_);
    if (!IsA(model, modelClass_)) return;
    Il2CppObject* set = unity::Field<Il2CppObject*>(model, offModelUpgrades_);
    if (!set) return;
    std::vector<Refund> refunds;
    int32_t price = 0, refund = 0;
    if (!Price(item, refunds, price, refund)) return;
    Il2CppObject* player = nullptr;
    if (!tryGetPlayerState_(&player) || !unity::IsAlive(player)) {
        SR2_WARN("[{}] sale cancelled: PlayerState not available", kModule);
        return;
    }

    if (custom_upgrades::IsCustomUpgrade(upg)) {
        // Custom upgrade: ownership lives in custom_upgrades' sidecar, not in the vanilla set.
        std::string notes;
        if (!custom_upgrades::Remove(plot, upg, notes)) {
            SR2_WARN("[{}] sale cancelled: could not remove {} from plot '{}'", kModule, UpgName(upg), id);
            return;
        }
        for (const Refund& r : refunds)
            if (r.amount > 0) addCurrency_(player, r.currency, r.amount, true, nullptr);
        bool rebuilt = false;
        Invoke(rebuildMi_, root, {}, "ListUIRoot.RebuildUI", &rebuilt);
        SR2_INFO("[{}] sold {} (custom) on plot '{}': refund {} of price {}; {}", kModule, UpgName(upg), id, refund,
                 price, notes);
        return;
    }

    // 1) model: the only saved change
    bool removed = false;
    if (!HashSetCall(set, "Remove", upg, &removed) || !removed || HasUpgrade(plot, upg)) {
        SR2_WARN("[{}] sale cancelled: could not remove {} from plot '{}'", kModule, UpgName(upg), id);
        return;
    }
    if (upg == ASH_TROUGH) {
        const float ash = unity::Field<float>(model, offModelAsh_);
        RememberAsh(model, ash);  // for a re-purchase on this plot
        if (ashRestoreOk_)        // and in the save's sidecar, for a re-purchase after a reload
            custom_upgrades::SetPlotValue(plot, kAshValueKey,
                                          std::format("{:.3f}", std::isfinite(ash) && ash > 0.0f ? ash : 0.0f));
        unity::Field<float>(model, offModelAsh_) = 0.0f;
    }

    // 2) refund
    for (const Refund& r : refunds)
        if (r.amount > 0) addCurrency_(player, r.currency, r.amount, true, nullptr);

    // 3) visuals / behaviour
    std::string notes;
    const int reverted = RevertVisuals(plot, upg, notes);

    // 4) like vanilla after any upgrade change: refresh trackers + ranch metadata
    if (Il2CppObject* empty = NewEmptySetLike(set)) applyUpgrades_(plot, empty, false);

    // 5) refresh the purchase menu (the item becomes buyable again)
    bool ok = false;
    Invoke(rebuildMi_, root, {}, "ListUIRoot.RebuildUI", &ok);

    SR2_INFO("[{}] sold {} (Upgrade {}) on plot '{}': refund {} of price {}, {} upgrader(s) reverted{}{}", kModule,
             UpgName(upg), upg, id, refund, price, reverted, notes.empty() ? "" : "; ", notes);
}

bool SellUpgrades::ReplantNormalCrop(Il2CppObject* plot, std::string& notes) {
    // Mirror of DeluxeGardenUpgrader.Apply with DELUXE_GARDEN already removed from the model,
    // so GardenCatcher.Plant picks the normal PlantedPrefab.
    Il2CppObject* attached = unity::Field<Il2CppObject*>(plot, offPlotAttached_);
    if (!unity::IsAlive(attached)) {
        notes += "no crop planted";
        return true;
    }
    Il2CppObject* oldSr = getComponent_(attached, Type(spawnResClass_));
    if (!unity::IsAlive(oldSr) || !IsA(oldSr, spawnResClass_)) {
        notes += "attached object is not a crop";
        return true;
    }
    Il2CppObject* cropId = primarySpawnId_(oldSr);
    if (!unity::IsAlive(cropId)) {
        notes += "crop has no id";
        return true;
    }
    Il2CppObject* catcher = getComponentInChildren_(plot, Type(catcherClass_), false);
    if (!unity::IsAlive(catcher) || !IsA(catcher, catcherClass_)) {
        notes += "GardenCatcher not found (crop fixes itself on reload)";
        return false;
    }
    Il2CppObject* oldGo = GameObjectOf(oldSr);
    if (!oldGo) return false;
    destroy_(oldGo, unity::NewString("sell_upgrades.ReplantNormalCrop"));
    Il2CppObject* newGo = plant_(catcher, cropId, true);
    Il2CppObject* newSr = unity::IsAlive(newGo) ? getComponent_(newGo, Type(spawnResClass_)) : nullptr;
    if (unity::IsAlive(newSr) && IsA(newSr, spawnResClass_)) {
        initAsReplacement_(newSr, oldSr);
        notes += "crop replanted as normal variant";
        return true;
    }
    notes += "replant returned no SpawnResource";
    return false;
}

int SellUpgrades::RevertVisuals(Il2CppObject* plot, int32_t upg, std::string& notes) {
    Il2CppArray* arr = getComponents_(plot, Type(upgraderClass_));
    int n = 0;
    auto go = [](Il2CppObject* comp, size_t off) { return unity::Field<Il2CppObject*>(comp, off); };
    auto allOff = [&](Il2CppObject* comp, size_t off) {
        void* a = unity::Field<void*>(comp, off);
        for (size_t i = 0, c = unity::ArrayLength(a); i < c; ++i)
            SetActive(static_cast<Il2CppObject*>(unity::ArrayAt(a, i)), false);
    };
    for (size_t i = 0, c = unity::ArrayLength(arr); i < c; ++i) {
        auto* comp = static_cast<Il2CppObject*>(unity::ArrayAt(arr, i));
        if (!unity::IsAlive(comp)) continue;
        switch (upg) {
            case WALLS:
                if (!IsA(comp, walls_.klass)) continue;
                SetActive(go(comp, walls_.off1), true);   // StandardWalls
                SetActive(go(comp, walls_.off2), false);  // UpgradeWalls
                break;
            case MUSIC_BOX:
                if (!IsA(comp, music_.klass)) continue;
                SetActive(go(comp, music_.off1), false);
                break;
            case SOIL:
                if (!IsA(comp, soil_.klass)) continue;
                SetActive(go(comp, soil_.off1), false);
                break;
            case SPRINKLER:
                if (!IsA(comp, sprinkler_.klass)) continue;
                SetActive(go(comp, sprinkler_.off1), false);
                break;
            case SCARESLIME:
                if (!IsA(comp, scare_.klass)) continue;
                SetActive(go(comp, scare_.off1), false);
                break;
            case FEEDER:
                if (!IsA(comp, feederU_.klass)) continue;
                SetActive(go(comp, feederU_.off1), false);
                break;
            case VITAMIZER:
                if (!IsA(comp, vitamizer_.klass)) continue;
                SetActive(go(comp, vitamizer_.off1), false);
                break;
            case AIR_NET:
                if (!IsA(comp, airNet_.klass)) continue;
                allOff(comp, airNet_.off1);
                break;
            case PLORT_COLLECTOR:
            case PLORT_COLLECTOR_POND:
            case PLORT_COLLECTOR_INCINERATOR:
                if (!IsA(comp, collector_.klass)) continue;
                SetActive(go(comp, collector_.off1), false);
                break;
            case SOLAR_SHIELD:
                if (!IsA(comp, solar_.klass)) continue;
                allOff(comp, solar_.off1);
                break;
            case ASH_TROUGH:
                if (!IsA(comp, ash_.klass)) continue;
                SetActive(go(comp, ash_.off1), false);
                break;
            case MIRACLE_MIX:
                if (!IsA(comp, miracle_.klass)) continue;
                SetActive(go(comp, miracle_.off1), false);  // MiracleMix
                SetActive(go(comp, miracle_.off2), true);   // NormSoil
                break;
            case DELUXE_GARDEN:
                if (!IsA(comp, dGarden_.klass)) continue;
                SetActive(go(comp, dGarden_.off1), false);  // DeluxeStuff
                break;
            case DELUXE_COOP: {
                if (!IsA(comp, dCoop_.klass)) continue;
                SetActive(go(comp, dCoop_.off1), false);  // DeluxeStuff
                void* regions = unity::Field<void*>(comp, dCoop_.off2);
                for (size_t r = 0, rc = unity::ArrayLength(regions); r < rc; ++r)
                    if (auto* reg = static_cast<Il2CppObject*>(unity::ArrayAt(regions, r)))
                        unity::Field<bool>(reg, offCoopDeluxe_) = false;
                break;
            }
            default: continue;
        }
        ++n;
    }
    if (upg == DELUXE_GARDEN && n > 0) ReplantNormalCrop(plot, notes);
    if (n == 0) notes += "no matching upgrader found (visuals fix themselves on reload)";
    return n;
}

// ---------------------------------------------------------------- setup --

bool SellUpgrades::Setup(sr2::core::ModuleContext& ctx) {
    Resolver& R = ctx.resolver();
    std::string why;
    auto cls = [&](const char* image, const char* ns, const char* name) -> Il2CppClass* {
        std::string err;
        Il2CppClass* k = R.Class(image, ns, name, &err);
        if (!k && why.empty()) why = err.empty() ? std::format("class {}.{} not found", ns, name) : err;
        return k;
    };
    // Field offset in `k` or its parents.
    auto off = [&](Il2CppClass* k, const char* field, size_t& out) -> bool {
        std::string err;
        for (Il2CppClass* c = k; c; c = api().il2cpp_class_get_parent ? api().il2cpp_class_get_parent(c) : nullptr) {
            if (auto o = R.FieldOffset(c, field, &err)) {
                out = *o;
                return true;
            }
        }
        if (why.empty()) why = std::format("field {} not found", field);
        return false;
    };
    auto fieldInfo = [&](Il2CppClass* k, const char* field, FieldInfo*& fi, size_t& o) -> bool {
        std::string err;
        fi = k ? R.Field(k, field, &err) : nullptr;
        if (!fi) {
            if (why.empty()) why = std::format("field {} not found", field);
            return false;
        }
        o = api().il2cpp_field_get_offset(fi);
        return true;
    };
    // MethodInfo in `k` or its parents.
    auto method = [&](Il2CppClass* k, const char* name, int argc, bool required = true) -> const MethodInfo* {
        std::string err;
        for (Il2CppClass* c = k; c; c = api().il2cpp_class_get_parent ? api().il2cpp_class_get_parent(c) : nullptr)
            if (const MethodInfo* m = R.Method(c, name, argc, &err)) return m;
        if (required && why.empty()) why = std::format("method {}({}) not found", name, argc);
        return nullptr;
    };
    auto bind = [&](auto& b, const MethodInfo* m, bool required = true) -> bool {
        if (!m) return false;
        std::string err;
        void* p = R.MethodPointer(m, &err);
        if (!p) {
            if (required && why.empty()) why = std::format("{}: no native body ({})", api().il2cpp_method_get_name ? api().il2cpp_method_get_name(m) : "?", err);
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

    objectNew_ = Export<ObjectNewFn>("il2cpp_object_new");
    if (!objectNew_) return ctx.Fail("il2cpp_object_new export missing");

    // ---- classes ----
    rootClass_ = cls(kImage, kPlotUiNs, "LandPlotUIRoot");
    itemClass_ = cls(kImage, kPlotUiNs, "PlotUpgradePurchaseItemModel");
    closureClass_ = cls(kImage, kPlotUiNs, "LandPlotUIRoot/<>c__DisplayClass6_0");
    plotClass_ = cls(kImage, "", "LandPlot");
    modelClass_ = cls(kImage, kDataNs, "LandPlotModel");
    costEntryClass_ = cls(kImage, "MonomiPark.SlimeRancher.Economy", "CurrencyCostEntry");
    Il2CppClass* purchaseCost = cls(kImage, "MonomiPark.SlimeRancher.Economy", "PurchaseCost");
    upgraderClass_ = cls(kImage, "", "PlotUpgrader");
    feederClass_ = cls(kImage, "", "SlimeFeeder");
    locationClass_ = cls(kImage, "", "LandPlotLocation");
    Il2CppClass* playerState = cls(kImage, "", "PlayerState");
    Il2CppClass* scu = cls(kImage, "MonomiPark.SlimeRancher.Util", "SceneContextUtility");
    Il2CppClass* coopRegion = cls(kImage, "", "CoopRegion");
    Il2CppClass* component = cls(kCore, "UnityEngine", "Component");
    Il2CppClass* gameObject = cls(kCore, "UnityEngine", "GameObject");
    Il2CppClass* transform = cls(kCore, "UnityEngine", "Transform");
    Il2CppClass* behaviour = cls(kCore, "UnityEngine", "Behaviour");
    tmpClass_ = cls("Unity.TextMeshPro", "TMPro", "TMP_Text");
    lseClass_ = cls("Unity.Localization", "UnityEngine.Localization.Components", "LocalizeStringEvent");
    if (!why.empty()) return ctx.Fail(why);

    struct UDef {
        UpgraderInfo* info;
        const char* name;
        const char* f1;
        const char* f2;
    };
    const UDef udefs[] = {
        {&walls_, "WallUpgrader", "StandardWalls", "UpgradeWalls"},
        {&music_, "MusicBoxUpgrader", "MusicBox", nullptr},
        {&soil_, "MineralSoilUpgrader", "Soil", nullptr},
        {&sprinkler_, "SprinklerUpgrader", "Sprinkler", nullptr},
        {&scare_, "ScareslimeUpgrader", "Scareslime", nullptr},
        {&feederU_, "FeederUpgrader", "Feeder", nullptr},
        {&vitamizer_, "VitamizerUpgrader", "Vitamizer", nullptr},
        {&airNet_, "AirNetUpgrader", "AirNets", nullptr},
        {&collector_, "PlortCollectorUpgrader", "Collector", nullptr},
        {&solar_, "SolarShieldUpgrader", "Shields", nullptr},
        {&ash_, "AshTroughUpgrader", "AshTrough", nullptr},
        {&miracle_, "MiracleMixUpgrader", "MiracleMix", "NormSoil"},
        {&dGarden_, "DeluxeGardenUpgrader", "DeluxeStuff", nullptr},
        {&dCoop_, "DeluxeCoopUpgrader", "DeluxeStuff", "CoopRegions"},
    };
    // Upgraders are optional one by one: a missing one only makes that upgrade unsellable.
    std::string upgraderReport;
    for (const UDef& d : udefs) {
        std::string err;
        Il2CppClass* k = R.Class(kImage, "", d.name, &err);
        auto o1 = k ? R.FieldOffset(k, d.f1, &err) : std::nullopt;
        auto o2 = (k && d.f2) ? R.FieldOffset(k, d.f2, &err) : std::optional<size_t>(0);
        if (k && o1 && o2) {
            d.info->klass = k;
            d.info->off1 = *o1;
            d.info->off2 = *o2;
        } else {
            upgraderReport += std::format(" {}(missing: {})", d.name, err);
        }
    }

    // ---- fields ----
    size_t offCostField = 0, offCurrencyList = 0;
    off(rootClass_, "Activator", offActivator_);
    off(itemClass_, "_upgrade", offItemUpgrade_);
    off(itemClass_, "_purchaseCost", offCostField);
    off(purchaseCost, "_currencyCosts", offCurrencyList);
    // Value-type field offsets include the object header (boxed layout).
    if (offCurrencyList >= 2 * sizeof(void*)) offCurrencyList -= 2 * sizeof(void*);
    offItemCosts_ = offCostField + offCurrencyList;
    off(costEntryClass_, "_currency", offCostCurrency_);
    off(costEntryClass_, "_amount", offCostAmount_);
    off(plotClass_, "_model", offPlotModel_);
    off(plotClass_, "_attached", offPlotAttached_);
    off(modelClass_, "upgrades", offModelUpgrades_);
    off(modelClass_, "ashUnits", offModelAsh_);
    off(locationClass_, "_id", offLocationId_);
    off(coopRegion, "_isDeluxe", offCoopDeluxe_);
    fieldInfo(closureClass_, "<>4__this", fClosureThis_, offClosureThis_);
    fieldInfo(closureClass_, "upgrade", fClosureUpgrade_, offClosureUpgrade_);
    if (!why.empty()) return ctx.Fail(why);

    // ---- methods ----
    const MethodInfo* onActionMi = method(rootClass_, "OnAction", 1);
    b0Mi_ = method(closureClass_, "<PromptForUpgrade>b__0", 0);
    rebuildMi_ = method(rootClass_, "RebuildUI", 0);
    tmpSetTextMi_ = method(tmpClass_, "set_text", 1);
    bind(hasUpgrade_, method(plotClass_, "HasUpgrade", 1));
    bind(applyUpgrades_, method(plotClass_, "ApplyUpgrades", 2));
    bind(addCurrency_, method(playerState, "AddCurrency", 4));
    bind(tryGetPlayerState_, method(scu, "TryGetPlayerState", 1));
    bind(getComponent_, byParams(component, "GetComponent", {"System.Type"}));
    bind(getComponents_, byParams(component, "GetComponents", {"System.Type"}));
    bind(getComponentInChildren_, byParams(component, "GetComponentInChildren", {"System.Type", "System.Boolean"}));
    bind(getGameObject_, method(component, "get_gameObject", 0));
    bind(getTransform_, method(component, "get_transform", 0));
    bind(getParent_, method(transform, "get_parent", 0));
    bind(setActive_, method(gameObject, "SetActive", 1));
    bind(setEnabled_, method(behaviour, "set_enabled", 1));
    bind(foodCount_, method(feederClass_, "GetFoodCount", 0));
    if (!why.empty()) return ctx.Fail(why);
    if (!b0Mi_ || !rebuildMi_ || !tmpSetTextMi_ || !onActionMi)
        return ctx.Fail("required method missing");

    // Optional: deluxe garden replant.
    {
        std::string saved = why;
        spawnResClass_ = R.Class(kImage, "", "SpawnResource");
        catcherClass_ = R.Class(kImage, "", "GardenCatcher");
        Il2CppClass* destroyer = R.Class(kImage, "", "Destroyer");
        replantOk_ = spawnResClass_ && catcherClass_ && destroyer && dGarden_.klass &&
                     bind(primarySpawnId_, method(spawnResClass_, "GetPrimarySpawnId", 0, false), false) &&
                     bind(initAsReplacement_, method(spawnResClass_, "InitAsReplacement", 1, false), false) &&
                     bind(plant_, method(catcherClass_, "Plant", 2, false), false) &&
                     bind(destroy_, method(destroyer, "Destroy", 2, false), false);
        why = saved;
    }

    // Optional: Ash Trough re-purchase keeps the ash it was sold with (missing -> vanilla refill).
    {
        std::string saved = why, err;
        ashSourceClass_ = R.Class(kImage, "", "FillableAshSource");
        auto oMax = ashSourceClass_ ? R.FieldOffset(ashSourceClass_, "MaxUnits", &err) : std::nullopt;
        auto oModel = ashSourceClass_ ? R.FieldOffset(ashSourceClass_, "_plotModel", &err) : std::nullopt;
        if (oMax) offAshMax_ = *oMax;
        if (oModel) offAshModel_ = *oModel;
        ashRestoreOk_ = ashSourceClass_ && oMax && oModel && ash_.klass && offModelAsh_ &&
                        bind(addAsh_, method(ashSourceClass_, "AddAsh", 1, false), false);
        why = saved;
    }

    // ---- sell button (optional: missing pieces only disable the button) ----
    const MethodInfo* setActivatorMi = nullptr;
    const MethodInfo* moreBindMi = nullptr;
    const MethodInfo* pressMi = nullptr;
    std::string buttonWhy;
    {
        std::string saved = why;
        why.clear();
        moreViewClass_ = cls(kImage, "MonomiPark.SlimeRancher.UI.Adapter", "DetailsMoreButtonView");
        Il2CppClass* moreVm = cls(kImage, "MonomiPark.SlimeRancher.UI.Adapter", "DetailsMoreButtonViewModel");
        Il2CppClass* uiButton = cls("UnityEngine.UI", "UnityEngine.UI", "Button");
        Il2CppClass* uobject = cls(kCore, "UnityEngine", "Object");
        if (why.empty()) {
            off(rootClass_, "menuConfig", offMenuConfig_);
            off(itemClass_, "_infoButtonLabel", offItemInfoLabel_);
            off(moreViewClass_, "label", offMoreLabel_);
            off(moreViewClass_, "button", offMoreButton_);
            off(moreVm, "infoLabel", offVmInfoLabel_);
        }
        if (why.empty()) {
            setActivatorMi = R.Method(rootClass_, "SetActivator", 1);  // the LandPlotUIRoot override itself
            moreBindMi = R.Method(moreViewClass_, "BindData", 1);
            pressMi = R.Method(uiButton, "Press", 0);
            if (!setActivatorMi || !moreBindMi || !pressMi) why = "SetActivator / BindData / Button.Press not found";
        }
        if (why.empty()) bind(objectDestroy_, byParams(uobject, "Destroy", {"UnityEngine.Object"}));
        // Optional: disabled look for an upgrade that can't be sold (missing -> the label alone says it).
        if (why.empty()) {
            bind(setInteractable_, method(uiButton, "set_interactable", 1, false), false);
            bind(tmpSetAlpha_, method(tmpClass_, "set_alpha", 1, false), false);
        }
        layoutGroupClass_ = R.Class("UnityEngine.UI", "UnityEngine.UI", "LayoutGroup");  // optional
        // Optional: the cloned Purchase prefab's submit wiring (see AfterMoreBind).
        inputEventButtonClass_ = R.Class(kImage, "MonomiPark.SlimeRancher.Input", "InputEventButton");
        inputEventDisplayClass_ =
            R.Class(kImage, "MonomiPark.SlimeRancher.UI.Framework.CommonControls", "InputEventDisplay");
        if (inputEventDisplayClass_)  // optional: the Sell button's key icon (missing -> icon hidden)
            displaySetEventMi_ = R.MethodByParams(inputEventDisplayClass_, "SetInputEvent",
                                                  {"MonomiPark.SlimeRancher.Input.InputEvent",
                                                   "MonomiPark.SlimeRancher.Input.InputEvent"});
        // Optional: side-by-side layout (missing -> no Sell button next to Purchase; on owned
        // items it covers the PURCHASED slot as before).
        if (why.empty()) {
            std::string lerr;
            rtClass_ = R.Class(kCore, "UnityEngine", "RectTransform", &lerr);
            auto m = [&](const char* name, int argc) { return rtClass_ ? R.Method(rtClass_, name, argc, &lerr) : nullptr; };
            rtGetRect_ = m("get_rect", 0);
            rtGetAnchorMin_ = m("get_anchorMin", 0);
            rtSetAnchorMin_ = m("set_anchorMin", 1);
            rtGetAnchorMax_ = m("get_anchorMax", 0);
            rtSetAnchorMax_ = m("set_anchorMax", 1);
            rtGetPivot_ = m("get_pivot", 0);
            rtSetPivot_ = m("set_pivot", 1);
            rtGetSize_ = m("get_sizeDelta", 0);
            rtSetSize_ = m("set_sizeDelta", 1);
            rtGetPos_ = m("get_anchoredPosition", 0);
            rtSetPos_ = m("set_anchoredPosition", 1);
            layoutOk_ = rtGetRect_ && rtGetAnchorMin_ && rtSetAnchorMin_ && rtGetAnchorMax_ && rtSetAnchorMax_ &&
                        rtGetPivot_ && rtSetPivot_ && rtGetSize_ && rtSetSize_ && rtGetPos_ && rtSetPos_;
            if (!layoutOk_)
                SR2_WARN("[{}] RectTransform accessors not found ({}) - no Sell button on tier cards", kModule, lerr);
        }
        buttonWhy = why;
        why = saved;
    }

    // ---- alt-action sell (optional: needs the sell button's pieces; missing -> mouse only) ----
    const MethodInfo* showAltMi = nullptr;
    const MethodInfo* onAltMi = nullptr;
    std::string altWhy = buttonWhy.empty() ? "" : "sell button unavailable";
    if (altWhy.empty()) {
        std::string saved = why;
        why.clear();
        listUiClass_ = cls(kImage, "MonomiPark.SlimeRancher.UI.Adapter", "ListUI");
        if (why.empty()) {
            off(rootClass_, "swapper", offSwapper_);
            off(listUiClass_, "altActionButtonContainer", offAltContainer_);
            off(listUiClass_, "altSubmit", offAltSubmit_);
        }
        // ListUIRoot`4 (inflated base of LandPlotUIRoot): the shared generic bodies.
        if (why.empty()) {
            showAltMi = method(rootClass_, "ShowAltActionButton", 1);
            onAltMi = method(rootClass_, "OnAltAction", 1);
        }
        altWhy = why;
        why = saved;
        // Label routes (each optional; none found -> no prompt is shown).
        constexpr const char* kControlsNs = "MonomiPark.SlimeRancher.UI.Framework.CommonControls";
        std::string lerr;
        if (Il2CppClass* k = R.Class(kImage, kControlsNs, "InputHint", &lerr)) {
            auto a = R.FieldOffset(k, "_label", &lerr);
            auto b = R.FieldOffset(k, "_labelText", &lerr);
            if (a && b) {
                hintClass_ = k;
                offHintLabel_ = *a;
                offHintText_ = *b;
            }
        }
        if (Il2CppClass* k = R.Class(kImage, kControlsNs, "TextLabel", &lerr)) {
            auto a = R.FieldOffset(k, "_stringEvent", &lerr);
            auto b = R.FieldOffset(k, "_textElement", &lerr);
            if (a && b) {
                textLabelClass_ = k;
                offTextLabelLse_ = *a;
                offTextLabelTmp_ = *b;
            }
        }
    }

    // ---- hooks ----
    std::string err;
    void* onActionPtr = R.MethodPointer(onActionMi, &err);
    void* confirmPtr = R.MethodPointer(b0Mi_, &err);
    if (!onActionPtr || !confirmPtr) return ctx.Fail("hook target without native body: " + err);
    if (!ctx.HookAddress("LandPlotUIRoot.OnAction", onActionPtr, reinterpret_cast<void*>(&OnActionDetour),
                         reinterpret_cast<void**>(&g_origOnAction)))
        return ctx.Fail("hook LandPlotUIRoot.OnAction failed");
    if (!ctx.HookAddress("LandPlotUIRoot.PromptForUpgrade_b__0", confirmPtr, reinterpret_cast<void*>(&ConfirmDetour),
                         reinterpret_cast<void**>(&g_origConfirm)))
        return ctx.Fail("hook <PromptForUpgrade>b__0 failed");
    if (buttonWhy.empty()) {
        struct H {
            const char* name;
            const MethodInfo* mi;
            void* detour;
            void** orig;
        };
        const H hooks[] = {
            {"LandPlotUIRoot.SetActivator", setActivatorMi, reinterpret_cast<void*>(&SetActivatorDetour),
             reinterpret_cast<void**>(&g_origSetActivator)},
            {"DetailsMoreButtonView.BindData", moreBindMi, reinterpret_cast<void*>(&MoreBindDetour),
             reinterpret_cast<void**>(&g_origMoreBind)},
            {"UnityEngine.UI.Button.Press", pressMi, reinterpret_cast<void*>(&PressDetour),
             reinterpret_cast<void**>(&g_origPress)},
        };
        for (const H& h : hooks) {
            void* p = R.MethodPointer(h.mi, &err);
            if (!p) return ctx.Fail(std::format("{}: no native body ({})", h.name, err));
            if (!ctx.HookAddress(h.name, p, h.detour, h.orig)) return ctx.Fail(std::format("hook {} failed", h.name));
        }
    } else {
        SR2_WARN("[{}] sell button unavailable ({}) - upgrades can't be sold this session", kModule, buttonWhy);
    }
    if (altWhy.empty()) {
        void* sp = R.MethodPointer(showAltMi, &err);
        void* ap = sp ? R.MethodPointer(onAltMi, &err) : nullptr;
        if (!sp || !ap)
            altWhy = "ShowAltActionButton / OnAltAction: no native body (" + err + ")";
        else if (!ctx.HookAddress("ListUIRoot.ShowAltActionButton", sp, reinterpret_cast<void*>(&ShowAltDetour),
                                  reinterpret_cast<void**>(&g_origShowAlt)))
            altWhy = "hook ListUIRoot.ShowAltActionButton failed";
        else if (!ctx.HookAddress("ListUIRoot.OnAltAction", ap, reinterpret_cast<void*>(&OnAltActionDetour),
                                  reinterpret_cast<void**>(&g_origOnAlt)))
            altWhy = "hook ListUIRoot.OnAltAction failed";  // ShowAlt stays pass-through (altOk_ false)
        else
            altOk_ = true;
    }
    if (!altOk_)
        SR2_WARN("[{}] alt-action sell unavailable ({}) - selling is mouse-only this session", kModule, altWhy);
    else
        SR2_INFO("[{}] alt-action sell: ShowAltActionButton RVA 0x{:X}, OnAltAction RVA 0x{:X}; label via InputHint {}, "
                 "TextLabel {}",
                 kModule, Resolver::Rva(R.MethodPointer(showAltMi)), Resolver::Rva(R.MethodPointer(onAltMi)),
                 hintClass_ ? "yes" : "no", textLabelClass_ ? "yes" : "no");

    g_self = this;
    SR2_INFO("[{}] setup ok: OnAction RVA 0x{:X}, <PromptForUpgrade>b__0 RVA 0x{:X}; offsets item._upgrade +0x{:X}, "
             "costs +0x{:X}, LandPlot._model +0x{:X}, model.upgrades +0x{:X}; deluxe-garden replant {}; ash-trough "
             "rebuy keeps ash: {}; refund fixed at 50%{}{}",
             kModule, Resolver::Rva(onActionPtr), Resolver::Rva(confirmPtr), offItemUpgrade_, offItemCosts_,
             offPlotModel_, offModelUpgrades_, replantOk_ ? "available" : "NOT available (DELUXE_GARDEN unsellable)",
             ashRestoreOk_ ? "yes" : "NO (vanilla refill)", upgraderReport.empty() ? "" : "; upgraders unavailable:", upgraderReport);
    return true;
}

}  // namespace

std::unique_ptr<sr2::core::Module> CreateSellUpgrades() { return std::make_unique<SellUpgrades>(); }

}  // namespace sr2::modules
