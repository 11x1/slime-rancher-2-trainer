// sell_upgrades: sell owned ranch land-plot upgrades back for a fraction of their price
// (default 50 %), from inside the game's own plot purchase menu (SR2 1.3.0).
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
//   * Detour LandPlotUIRoot.OnAction: for an OWNED upgrade item (module on) it opens the
//     game's own confirm popup (OpenPopupPrompt with the item's title) and replaces the
//     message / positive-button texts ("Sell this upgrade for N Newbucks?" / "Sell"). The
//     LocalizeStringEvent of those two labels on that transient popup instance is disabled
//     first so localisation cannot overwrite the text. Not owned -> vanilla purchase flow.
//     If the upgrade cannot be sold, the same popup explains why (button "OK").
//   * The popup's positive Action is a fresh <>c__DisplayClass6_0 closure (its cost stays
//     empty) bound to <PromptForUpgrade>b__0, exactly like the game builds it. Detour on
//     b__0: if it is our pending closure, the sale runs instead. If the module is off /
//     faulted, the original b__0 runs and does nothing (Upgrade() aborts: already owned).
//   * Sale (main thread): re-validate, refund = floor(fraction x each currency cost) via
//     PlayerState.AddCurrency, model.upgrades.Remove(u), revert the upgrader exactly
//     (table above, inverted), ApplyUpgrades(empty set, false) to refresh trackers +
//     RanchMetadata like vanilla does after a change, RebuildUI() to refresh the menu.
//     The upgrade can be bought again at full price through the vanilla flow.
//   * Side state: AUTO-FEEDER is refused while it still holds food (no item loss). ASH
//     TROUGH: refused while the incinerator plort collector (which needs it) is owned;
//     selling empties the trough (model.ashUnits = 0; a re-purchase refills it as in
//     vanilla). DELUXE GARDEN: the planted crop is replanted as the normal variant with
//     the game's own replant calls (growth kept).
//   * EXCLUDED: silo STORAGE2/3/4 and STORAGE_CAPACITY_INCREASE (they change the silo's
//     slot layout / per-slot capacity of stored items; reverting means rewriting slot
//     data and could lose or over-fill stored items).
//   * Save: only the vanilla HashSet changes (and ashUnits); every resulting state is one
//     vanilla can produce (the plot simply never had that upgrade). No new save data.
//
// Fault policy: both detours run their logic inside a DetourGuard (native fault -> module
// faulted, detours pass-through). Objects are class- and IsAlive-checked; runtime_invoke
// native faults mark the module faulted (never used as control flow). No GC handles
// (closure / delegate are referenced by the popup; we only compare the address), no
// scans, nothing per frame.

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <format>
#include <memory>
#include <string>
#include <vector>

#include "core/detour_guard.h"
#include "core/il2cpp_api.h"
#include "core/log.h"
#include "core/main_thread.h"
#include "core/module.h"
#include "core/settings.h"
#include "core/unity.h"
#include "modules/custom_upgrades/api.h"

namespace sr2::modules {
namespace {

namespace unity = sr2::core::unity;
namespace settings = sr2::core::settings;
namespace main_thread = sr2::core::main_thread;
using sr2::il2cpp::api;
using sr2::il2cpp::Resolver;

constexpr const char* kModule = "sell_upgrades";
constexpr const char* kImage = "Assembly-CSharp";
constexpr const char* kCore = "UnityEngine.CoreModule";
constexpr const char* kDataNs = "MonomiPark.SlimeRancher.DataModel";
constexpr const char* kPlotUiNs = "MonomiPark.SlimeRancher.UI.Plot";
constexpr const char* kKeyFraction = "refund_fraction";

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

struct PendingPrompt {
    Il2CppObject* closure = nullptr;  // identity only (kept alive by the popup's delegate)
    bool sell = false;                // false = info popup (OK does nothing)
    float fraction = 0.5f;            // the fraction shown in the popup
    int32_t upgrade = 0;
};

class SellUpgrades final : public sr2::core::Module {
public:
    const char* Name() const override { return kModule; }
    bool DefaultEnabled() const override { return true; }
    bool Setup(sr2::core::ModuleContext& ctx) override;
    void OnEnable() override { enabled_.store(true); }
    void OnDisable() override {
        enabled_.store(false);
        pending_ = {};
    }
    bool On() const { return enabled_.load(std::memory_order_relaxed); }

    // detour bodies (main thread, inside a DetourGuard)
    bool HandleAction(Il2CppObject* root, Il2CppObject* item);
    bool HandleConfirm(Il2CppObject* closure);

private:
    struct Refund {
        Il2CppObject* currency = nullptr;
        int32_t price = 0;
        int32_t amount = 0;
    };

    float Fraction() const;
    std::string CanSell(Il2CppObject* plot, int32_t upg);
    bool Price(Il2CppObject* item, float fraction, std::vector<Refund>& out, int32_t& price, int32_t& refund);
    bool OpenPrompt(Il2CppObject* root, Il2CppObject* item, int32_t upg, bool sell, const std::string& text,
                    float fraction);
    bool SetLabel(Il2CppObject* lse, const std::string& text);
    void DoSell(Il2CppObject* root, Il2CppObject* plot, Il2CppObject* item, int32_t upg, float fraction);
    int RevertVisuals(Il2CppObject* plot, int32_t upg, std::string& notes);
    bool ReplantNormalCrop(Il2CppObject* plot, std::string& notes);
    int CountUpgraders(Il2CppObject* plot, int32_t upg);
    std::string PlotId(Il2CppObject* plot);
    Il2CppObject* Invoke(const MethodInfo* m, void* obj, std::initializer_list<void*> args, const char* what,
                         bool* ok = nullptr);
    bool HashSetCall(Il2CppObject* set, const char* method, int32_t upg, bool* result);
    Il2CppObject* NewEmptySetLike(Il2CppObject* set);
    void SetRef(Il2CppObject* obj, size_t offset, Il2CppObject* value);
    Il2CppObject* GameObjectOf(Il2CppObject* comp);
    void SetActive(Il2CppObject* go, bool on);
    bool HasUpgrade(Il2CppObject* plot, int32_t upg) { return hasUpgrade_(plot, upg); }
    Il2CppObject* Type(Il2CppClass* k) { return unity::TypeOf(k); }

    std::atomic<bool> enabled_{false};
    PendingPrompt pending_;  // main thread only
    bool loggedFirstPrompt_ = false;
    bool warnedLabel_ = false;

public:
    // ---- resolved (Setup, worker thread) ----
    Il2CppClass* rootClass_ = nullptr;        // LandPlotUIRoot
    Il2CppClass* itemClass_ = nullptr;        // PlotUpgradePurchaseItemModel
    Il2CppClass* closureClass_ = nullptr;     // LandPlotUIRoot/<>c__DisplayClass6_0
    Il2CppClass* plotClass_ = nullptr;        // LandPlot
    Il2CppClass* modelClass_ = nullptr;       // LandPlotModel
    Il2CppClass* popupClass_ = nullptr;       // PopupPrompt
    Il2CppClass* actionClass_ = nullptr;      // System.Action
    Il2CppClass* costEntryClass_ = nullptr;   // CurrencyCostEntry
    Il2CppClass* upgraderClass_ = nullptr;    // PlotUpgrader
    Il2CppClass* tmpClass_ = nullptr;         // TMPro.TMP_Text
    Il2CppClass* lseClass_ = nullptr;         // LocalizeStringEvent
    Il2CppClass* feederClass_ = nullptr;      // SlimeFeeder
    Il2CppClass* locationClass_ = nullptr;    // LandPlotLocation
    Il2CppClass* spawnResClass_ = nullptr;    // SpawnResource
    Il2CppClass* catcherClass_ = nullptr;     // GardenCatcher

    struct UpgraderInfo {
        Il2CppClass* klass = nullptr;
        size_t off1 = 0, off2 = 0;  // GameObject / GameObject[] / CoopRegion[] fields
    };
    UpgraderInfo walls_, music_, soil_, sprinkler_, scare_, feederU_, vitamizer_, airNet_, collector_, solar_,
        ash_, miracle_, dGarden_, dCoop_;
    size_t offCoopDeluxe_ = 0;  // CoopRegion._isDeluxe

    size_t offActivator_ = 0;     // LandPlotUIRoot.Activator
    size_t offItemUpgrade_ = 0;   // PlotUpgradePurchaseItemModel._upgrade
    size_t offItemTitle_ = 0;     // MenuPurchaseItemModel._title
    size_t offItemPrompt_ = 0;    // MenuPurchaseItemModel._promptMessage
    size_t offItemCosts_ = 0;     // MenuPurchaseItemModel._purchaseCost._currencyCosts
    size_t offCostCurrency_ = 0;  // CurrencyCostEntry._currency
    size_t offCostAmount_ = 0;    // CurrencyCostEntry._amount
    size_t offPlotModel_ = 0;     // LandPlot._model
    size_t offPlotAttached_ = 0;  // LandPlot._attached
    size_t offModelUpgrades_ = 0; // LandPlotModel.upgrades
    size_t offModelAsh_ = 0;      // LandPlotModel.ashUnits
    size_t offUiTemplates_ = 0;   // GameContext.<UITemplates>k__BackingField
    size_t offPosLabel_ = 0;      // UITemplates.popupPromptPositiveActionLabel
    size_t offNegLabel_ = 0;      // UITemplates.popupPromptNegativeActionLabel
    size_t offPopupMsg_ = 0;      // PopupPrompt._messageText
    size_t offPopupPos_ = 0;      // PopupPrompt._positiveActionButtonText
    size_t offLocationId_ = 0;    // IdHandler._id

    FieldInfo* fClosureThis_ = nullptr;
    FieldInfo* fClosureUpgrade_ = nullptr;
    FieldInfo* fClosureItem_ = nullptr;
    size_t offClosureThis_ = 0, offClosureUpgrade_ = 0, offClosureItem_ = 0;

    const MethodInfo* b0Mi_ = nullptr;           // <PromptForUpgrade>b__0
    const MethodInfo* openPopupMi_ = nullptr;    // ListUIRoot`4.OpenPopupPrompt (inflated)
    const MethodInfo* rebuildMi_ = nullptr;      // ListUIRoot`4.RebuildUI (inflated)
    const MethodInfo* gameCtxInstMi_ = nullptr;  // SRSingleton<GameContext>.get_Instance
    const MethodInfo* tmpSetTextMi_ = nullptr;   // TMP_Text.set_text
    const MethodInfo* setRemoveMi_ = nullptr;    // HashSet<Upgrade>.Remove (lazy)
    const MethodInfo* setCtorMi_ = nullptr;      // HashSet<Upgrade>..ctor() (lazy)
    Il2CppClass* setClass_ = nullptr;

    Bound<bool(Il2CppObject*, int32_t)> hasUpgrade_;
    Bound<void(Il2CppObject*, Il2CppObject*, bool)> applyUpgrades_;
    Bound<void(Il2CppObject*, Il2CppObject*, int32_t, bool, Il2CppObject*)> addCurrency_;
    Bound<bool(Il2CppObject**)> tryGetPlayerState_;
    Bound<void(Il2CppObject*, Il2CppObject*, intptr_t)> actionCtor_;
    Bound<Il2CppObject*(Il2CppObject*, Il2CppObject*)> getComponent_;
    Bound<Il2CppArray*(Il2CppObject*, Il2CppObject*)> getComponents_;
    Bound<Il2CppObject*(Il2CppObject*, Il2CppObject*, bool)> getComponentInChildren_;
    Bound<Il2CppObject*(Il2CppObject*)> getGameObject_;
    Bound<Il2CppObject*(Il2CppObject*)> getTransform_;
    Bound<Il2CppObject*(Il2CppObject*)> getParent_;
    Bound<void(Il2CppObject*, bool)> setActive_;
    Bound<void(Il2CppObject*, bool)> setEnabled_;
    Bound<int32_t(Il2CppObject*)> foodCount_;
    Bound<void(Il2CppObject*)> closeNegative_;
    // deluxe garden replant (optional: missing -> DELUXE_GARDEN not sellable)
    Bound<Il2CppObject*(Il2CppObject*)> primarySpawnId_;
    Bound<void(Il2CppObject*, Il2CppObject*)> initAsReplacement_;
    Bound<Il2CppObject*(Il2CppObject*, Il2CppObject*, bool)> plant_;
    Bound<void(Il2CppObject*, Il2CppString*)> destroy_;
    bool replantOk_ = false;

    using ObjectNewFn = Il2CppObject* (*)(Il2CppClass*);
    using WBarrierFn = void (*)(Il2CppObject*, void**, void*);
    ObjectNewFn objectNew_ = nullptr;
    WBarrierFn wbarrier_ = nullptr;  // il2cpp_gc_wbarrier_set_field (optional)
};

SellUpgrades* g_self = nullptr;

sr2::core::DetourGuard g_actionGuard{kModule, "LandPlotUIRoot.OnAction"};
sr2::core::DetourGuard g_confirmGuard{kModule, "LandPlotUIRoot.<PromptForUpgrade>b__0"};

using OnActionFn = void (*)(Il2CppObject*, Il2CppObject*, const MethodInfo*);
using ConfirmFn = void (*)(Il2CppObject*, const MethodInfo*);
OnActionFn g_origOnAction = nullptr;
ConfirmFn g_origConfirm = nullptr;

void OnActionDetour(Il2CppObject* self, Il2CppObject* item, const MethodInfo* mi) {
    bool handled = false;
    if (g_self && self && item && g_self->On())
        g_actionGuard.Run([&] { handled = g_self->HandleAction(self, item); });
    if (!handled) g_origOnAction(self, item, mi);
}

void ConfirmDetour(Il2CppObject* closure, const MethodInfo* mi) {
    bool handled = false;
    if (g_self && closure && g_self->On()) g_confirmGuard.Run([&] { handled = g_self->HandleConfirm(closure); });
    if (!handled) g_origConfirm(closure, mi);
}

// ---------------------------------------------------------------- helpers --

float SellUpgrades::Fraction() const {
    float f = settings::GetFloat(kModule, kKeyFraction);
    if (!std::isfinite(f)) f = 0.5f;
    return f < 0.0f ? 0.0f : (f > 1.0f ? 1.0f : f);
}

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

void SellUpgrades::SetRef(Il2CppObject* obj, size_t offset, Il2CppObject* value) {
    // Reference store into a managed object: through the GC write barrier when available.
    void** slot = reinterpret_cast<void**>(reinterpret_cast<char*>(obj) + offset);
    if (wbarrier_) wbarrier_(obj, slot, value);
    else *slot = value;
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

bool SellUpgrades::Price(Il2CppObject* item, float fraction, std::vector<Refund>& out, int32_t& price,
                         int32_t& refund) {
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
        r.amount = static_cast<int32_t>(std::floor(static_cast<double>(r.price) * fraction + 1e-6));
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

// Empty string = can be sold.
std::string SellUpgrades::CanSell(Il2CppObject* plot, int32_t upg) {
    // custom_upgrades ids (never in the vanilla set): that module decides.
    if (custom_upgrades::IsCustomUpgrade(upg)) return custom_upgrades::CanSell(plot, upg);
    switch (upg) {
        case STORAGE2:
        case STORAGE3:
        case STORAGE4:
        case STORAGE_CAPACITY_INCREASE:
            return "Silo storage upgrades can't be sold: they change the silo's slots and capacity, and the items "
                   "stored there could be lost.";
        default: break;
    }
    if (upg < WALLS || upg > STORAGE_CAPACITY_INCREASE) return "This upgrade can't be sold.";
    if (upg == ASH_TROUGH && HasUpgrade(plot, PLORT_COLLECTOR_INCINERATOR))
        return "The incinerator's Plort Collector needs the Ash Trough. Sell the Plort Collector first.";
    if (upg == DELUXE_GARDEN && !replantOk_) return "This upgrade can't be sold (game functions not found).";
    if (CountUpgraders(plot, upg) <= 0) return "This upgrade can't be sold on this plot.";
    if (upg == FEEDER) {
        Il2CppObject* feeder = getComponentInChildren_(plot, Type(feederClass_), true);
        if (unity::IsAlive(feeder) && IsA(feeder, feederClass_)) {
            const int32_t food = foodCount_(feeder);
            if (food > 0)
                return std::format("The Auto-Feeder still holds {} food item{}. Let it feed them out (or wait until it "
                                   "is empty) before selling it, so no food is lost.",
                                   food, food == 1 ? "" : "s");
        }
    }
    return {};
}

bool SellUpgrades::SetLabel(Il2CppObject* lse, const std::string& text) {
    if (!unity::IsAlive(lse) || !IsA(lse, lseClass_)) return false;
    // Transient popup instance: stop its localisation from (re)writing the label, then set our text.
    setEnabled_(lse, false);
    Il2CppObject* tmp = getComponent_(lse, Type(tmpClass_));
    if (!unity::IsAlive(tmp)) return false;
    Il2CppString* s = unity::NewString(text);
    if (!s) return false;
    bool ok = false;
    Invoke(tmpSetTextMi_, tmp, {s}, "TMP_Text.set_text", &ok);
    return ok;
}

bool SellUpgrades::OpenPrompt(Il2CppObject* root, Il2CppObject* item, int32_t upg, bool sell,
                              const std::string& text, float fraction) {
    // UITemplates labels (fallback texts if our override would fail).
    bool ok = false;
    Il2CppObject* gameCtx = Invoke(gameCtxInstMi_, nullptr, {}, "GameContext.Instance", &ok);
    if (!ok || !unity::IsAlive(gameCtx)) return false;
    Il2CppObject* templates = unity::Field<Il2CppObject*>(gameCtx, offUiTemplates_);
    if (!unity::IsAlive(templates)) return false;
    Il2CppObject* posLabel = unity::Field<Il2CppObject*>(templates, offPosLabel_);
    Il2CppObject* negLabel = unity::Field<Il2CppObject*>(templates, offNegLabel_);
    Il2CppObject* title = unity::Field<Il2CppObject*>(item, offItemTitle_);
    Il2CppObject* message = unity::Field<Il2CppObject*>(item, offItemPrompt_);

    // The positive action: a closure of the game's own class bound to <PromptForUpgrade>b__0.
    Il2CppObject* closure = objectNew_(closureClass_);
    Il2CppObject* action = closure ? objectNew_(actionClass_) : nullptr;
    if (!closure || !action) return false;
    SetRef(closure, offClosureThis_, root);
    unity::Field<int32_t>(closure, offClosureUpgrade_) = upg;
    SetRef(closure, offClosureItem_, item);
    actionCtor_(action, closure, reinterpret_cast<intptr_t>(b0Mi_));

    pending_ = {};
    pending_.closure = closure;
    pending_.sell = sell;
    pending_.fraction = fraction;
    pending_.upgrade = upg;

    Il2CppObject* popup = Invoke(openPopupMi_, root, {title, message, nullptr, posLabel, negLabel, action, nullptr},
                                 "ListUIRoot.OpenPopupPrompt", &ok);
    if (!ok) {
        pending_ = {};
        return false;
    }
    bool labelled = false;
    if (unity::IsAlive(popup) && IsA(popup, popupClass_)) {
        labelled = SetLabel(unity::Field<Il2CppObject*>(popup, offPopupMsg_), text) &&
                   SetLabel(unity::Field<Il2CppObject*>(popup, offPopupPos_), sell ? "Sell" : "OK");
    }
    if (!labelled) {
        // Never leave a popup whose text could read like a purchase prompt.
        pending_ = {};
        if (unity::IsAlive(popup) && IsA(popup, popupClass_)) closeNegative_(popup);
        if (!warnedLabel_) {
            warnedLabel_ = true;
            SR2_WARN("[{}] could not set the popup texts (popup class {}), sell prompt closed again", kModule,
                     popup ? unity::ClassName(popup) : std::string("null"));
        }
        return true;  // handled (no vanilla action for an owned upgrade anyway)
    }
    if (!loggedFirstPrompt_) {
        loggedFirstPrompt_ = true;
        SR2_INFO("[{}] first sell prompt opened via the game's popup ({} / '{}')", kModule, UpgName(upg),
                 sell ? "sell" : "info");
    }
    return true;
}

// ---------------------------------------------------------------- detours --

bool SellUpgrades::HandleAction(Il2CppObject* root, Il2CppObject* item) {
    if (!main_thread::IsMainThread()) return false;
    if (!IsA(item, itemClass_) || !unity::IsAlive(item)) return false;  // plots, demolish, clear crop: vanilla
    if (!IsA(root, rootClass_) || !unity::IsAlive(root)) return false;
    Il2CppObject* plot = unity::Field<Il2CppObject*>(root, offActivator_);
    if (!IsA(plot, plotClass_) || !unity::IsAlive(plot)) return false;
    const int32_t upg = unity::Field<int32_t>(item, offItemUpgrade_);
    if (!HasUpgrade(plot, upg)) return false;  // not owned: vanilla purchase flow

    const float fraction = Fraction();
    std::string reason = CanSell(plot, upg);
    std::string text;
    if (reason.empty()) {
        std::vector<Refund> refunds;
        int32_t price = 0, refund = 0;
        if (!Price(item, fraction, refunds, price, refund)) {
            reason = "This upgrade can't be sold (unexpected price data).";
        } else {
            text = std::format("Sell this upgrade for {} Newbucks?\nYou get {}% of its {} Newbucks price back and can "
                               "buy it again later at full price.",
                               refund, static_cast<int>(std::lround(fraction * 100.0f)), price);
            if (upg == ASH_TROUGH) text += "\nThe ash in the trough is lost.";
            if (upg == DELUXE_GARDEN) text += "\nThe planted crop becomes a normal crop again (growth is kept).";
        }
    }
    if (!reason.empty()) {
        SR2_INFO("[{}] sell refused: plot '{}', {}: {}", kModule, PlotId(plot), UpgName(upg), reason);
        return OpenPrompt(root, item, upg, false, reason, fraction);
    }
    return OpenPrompt(root, item, upg, true, text, fraction);
}

bool SellUpgrades::HandleConfirm(Il2CppObject* closure) {
    if (!pending_.closure || closure != pending_.closure) return false;  // a vanilla purchase closure
    if (!main_thread::IsMainThread() || !IsA(closure, closureClass_)) return false;
    const PendingPrompt p = pending_;
    pending_ = {};
    if (!p.sell) return true;  // info popup: OK does nothing

    auto* root = unity::Field<Il2CppObject*>(closure, offClosureThis_);
    auto* item = unity::Field<Il2CppObject*>(closure, offClosureItem_);
    const int32_t upg = unity::Field<int32_t>(closure, offClosureUpgrade_);
    if (upg != p.upgrade || !IsA(root, rootClass_) || !unity::IsAlive(root) || !IsA(item, itemClass_) ||
        !unity::IsAlive(item))
        return true;
    Il2CppObject* plot = unity::Field<Il2CppObject*>(root, offActivator_);
    if (!IsA(plot, plotClass_) || !unity::IsAlive(plot)) return true;
    if (unity::Field<int32_t>(item, offItemUpgrade_) != upg || !HasUpgrade(plot, upg)) return true;
    DoSell(root, plot, item, upg, p.fraction);
    return true;
}

// ---------------------------------------------------------------- selling --

void SellUpgrades::DoSell(Il2CppObject* root, Il2CppObject* plot, Il2CppObject* item, int32_t upg, float fraction) {
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
    if (!Price(item, fraction, refunds, price, refund)) return;
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
        SR2_INFO("[{}] sold {} (custom) on plot '{}': refund {} of price {} ({:.0f}%); {}", kModule, UpgName(upg), id,
                 refund, price, fraction * 100.0f, notes);
        return;
    }

    // 1) model: the only saved change
    bool removed = false;
    if (!HashSetCall(set, "Remove", upg, &removed) || !removed || HasUpgrade(plot, upg)) {
        SR2_WARN("[{}] sale cancelled: could not remove {} from plot '{}'", kModule, UpgName(upg), id);
        return;
    }
    if (upg == ASH_TROUGH) unity::Field<float>(model, offModelAsh_) = 0.0f;

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

    SR2_INFO("[{}] sold {} (Upgrade {}) on plot '{}': refund {} of price {} ({:.0f}%), {} upgrader(s) reverted{}{}",
             kModule, UpgName(upg), upg, id, refund, price, fraction * 100.0f, reverted, notes.empty() ? "" : "; ",
             notes);
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
    wbarrier_ = Export<WBarrierFn>("il2cpp_gc_wbarrier_set_field");
    if (!objectNew_) return ctx.Fail("il2cpp_object_new export missing");

    // ---- classes ----
    rootClass_ = cls(kImage, kPlotUiNs, "LandPlotUIRoot");
    itemClass_ = cls(kImage, kPlotUiNs, "PlotUpgradePurchaseItemModel");
    closureClass_ = cls(kImage, kPlotUiNs, "LandPlotUIRoot/<>c__DisplayClass6_0");
    plotClass_ = cls(kImage, "", "LandPlot");
    modelClass_ = cls(kImage, kDataNs, "LandPlotModel");
    popupClass_ = cls(kImage, "MonomiPark.SlimeRancher.UI.Popup", "PopupPrompt");
    costEntryClass_ = cls(kImage, "MonomiPark.SlimeRancher.Economy", "CurrencyCostEntry");
    Il2CppClass* purchaseCost = cls(kImage, "MonomiPark.SlimeRancher.Economy", "PurchaseCost");
    upgraderClass_ = cls(kImage, "", "PlotUpgrader");
    feederClass_ = cls(kImage, "", "SlimeFeeder");
    locationClass_ = cls(kImage, "", "LandPlotLocation");
    Il2CppClass* gameCtx = cls(kImage, "", "GameContext");
    Il2CppClass* templates = cls(kImage, "MonomiPark.SlimeRancher.UI", "UITemplates");
    Il2CppClass* playerState = cls(kImage, "", "PlayerState");
    Il2CppClass* scu = cls(kImage, "MonomiPark.SlimeRancher.Util", "SceneContextUtility");
    Il2CppClass* coopRegion = cls(kImage, "", "CoopRegion");
    actionClass_ = cls("mscorlib", "System", "Action");
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
    off(itemClass_, "_title", offItemTitle_);
    off(itemClass_, "_promptMessage", offItemPrompt_);
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
    off(gameCtx, "<UITemplates>k__BackingField", offUiTemplates_);
    off(templates, "popupPromptPositiveActionLabel", offPosLabel_);
    off(templates, "popupPromptNegativeActionLabel", offNegLabel_);
    off(popupClass_, "_messageText", offPopupMsg_);
    off(popupClass_, "_positiveActionButtonText", offPopupPos_);
    off(locationClass_, "_id", offLocationId_);
    off(coopRegion, "_isDeluxe", offCoopDeluxe_);
    fieldInfo(closureClass_, "<>4__this", fClosureThis_, offClosureThis_);
    fieldInfo(closureClass_, "upgrade", fClosureUpgrade_, offClosureUpgrade_);
    fieldInfo(closureClass_, "plotUpgradePurchaseItemModel", fClosureItem_, offClosureItem_);
    if (!why.empty()) return ctx.Fail(why);

    // ---- methods ----
    const MethodInfo* onActionMi = method(rootClass_, "OnAction", 1);
    b0Mi_ = method(closureClass_, "<PromptForUpgrade>b__0", 0);
    openPopupMi_ = method(rootClass_, "OpenPopupPrompt", 7);
    rebuildMi_ = method(rootClass_, "RebuildUI", 0);
    gameCtxInstMi_ = method(api().il2cpp_class_get_parent ? api().il2cpp_class_get_parent(gameCtx) : nullptr,
                            "get_Instance", 0);
    tmpSetTextMi_ = method(tmpClass_, "set_text", 1);
    bind(hasUpgrade_, method(plotClass_, "HasUpgrade", 1));
    bind(applyUpgrades_, method(plotClass_, "ApplyUpgrades", 2));
    bind(addCurrency_, method(playerState, "AddCurrency", 4));
    bind(tryGetPlayerState_, method(scu, "TryGetPlayerState", 1));
    bind(actionCtor_, method(actionClass_, ".ctor", 2));
    bind(getComponent_, byParams(component, "GetComponent", {"System.Type"}));
    bind(getComponents_, byParams(component, "GetComponents", {"System.Type"}));
    bind(getComponentInChildren_, byParams(component, "GetComponentInChildren", {"System.Type", "System.Boolean"}));
    bind(getGameObject_, method(component, "get_gameObject", 0));
    bind(getTransform_, method(component, "get_transform", 0));
    bind(getParent_, method(transform, "get_parent", 0));
    bind(setActive_, method(gameObject, "SetActive", 1));
    bind(setEnabled_, method(behaviour, "set_enabled", 1));
    bind(foodCount_, method(feederClass_, "GetFoodCount", 0));
    bind(closeNegative_, method(popupClass_, "CloseWithNegativeCue", 0));
    if (!why.empty()) return ctx.Fail(why);
    if (!b0Mi_ || !openPopupMi_ || !rebuildMi_ || !gameCtxInstMi_ || !tmpSetTextMi_ || !onActionMi)
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

    // ---- settings ----
    settings::Def d;
    d.module = kModule;
    d.key = kKeyFraction;
    d.label = "Refund fraction";
    d.kind = settings::Kind::Float;
    d.def = 0.5;
    d.min = 0.0;
    d.max = 1.0;
    d.step = 0.05;
    d.tooltip =
        "Share of an upgrade's price paid back when you sell it (0.5 = half). Select an owned upgrade in a plot's "
        "purchase menu and confirm to get the sell prompt. Silo storage upgrades can't be sold.";
    settings::Register(d);

    g_self = this;
    SR2_INFO("[{}] setup ok: OnAction RVA 0x{:X}, <PromptForUpgrade>b__0 RVA 0x{:X}; offsets item._upgrade +0x{:X}, "
             "costs +0x{:X}, LandPlot._model +0x{:X}, model.upgrades +0x{:X}; deluxe-garden replant {}; refund "
             "fraction {:.2f}{}{}",
             kModule, Resolver::Rva(onActionPtr), Resolver::Rva(confirmPtr), offItemUpgrade_, offItemCosts_,
             offPlotModel_, offModelUpgrades_, replantOk_ ? "available" : "NOT available (DELUXE_GARDEN unsellable)",
             Fraction(), upgraderReport.empty() ? "" : "; upgraders unavailable:", upgraderReport);
    if (!wbarrier_) SR2_WARN("[{}] il2cpp_gc_wbarrier_set_field not exported; closure fields stored directly", kModule);
    return true;
}

}  // namespace

std::unique_ptr<sr2::core::Module> CreateSellUpgrades() { return std::make_unique<SellUpgrades>(); }

}  // namespace sr2::modules
