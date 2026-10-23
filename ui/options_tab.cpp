#include "options_tab.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <exception>
#include <format>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "core/hook_manager.h"
#include "core/log.h"
#include "core/main_thread.h"
#include "core/resolver.h"
#include "core/settings.h"
#include "core/text_override.h"
#include "core/unity.h"
#include "core/version_guard.h"
#include "il2cpp_util.h"
#include "mod_icons.h"
#include "settings_view.h"

namespace sr2::ui {
namespace {

namespace unity = ::sr2::core::unity;
using core::HookManager;

constexpr const char* kAsm = "Assembly-CSharp";
constexpr const char* kNsOptions = "MonomiPark.SlimeRancher.Options";
constexpr const char* kNsUiOptions = "MonomiPark.SlimeRancher.UI.Options";
constexpr const char* kNsDataModel = "MonomiPark.SlimeRancher.DataModel";
constexpr const char* kNsAdapter = "MonomiPark.SlimeRancher.UI.Adapter";
constexpr int kHideFlagsDontUnloadUnusedAsset = 32;  // UnityEngine.HideFlags.DontUnloadUnusedAsset
constexpr const char* kTabTitle = "Mods";
constexpr int kMaxStepperPresets = 201;  // coarser steps beyond this (keeps the stepper usable)

// ------------------------------------------------------------- symbols ----
struct Sym {
    // classes
    Il2CppClass* uiRoot = nullptr;
    Il2CppClass* viewModel = nullptr;
    Il2CppClass* config = nullptr;
    Il2CppClass* category = nullptr;
    Il2CppClass* itemDef = nullptr;
    Il2CppClass* presetDef = nullptr;
    Il2CppClass* toggleDef = nullptr;
    Il2CppClass* rangeDef = nullptr;
    Il2CppClass* itemModel = nullptr;
    Il2CppClass* presetModel = nullptr;
    Il2CppClass* presetSet = nullptr;
    Il2CppClass* rowHolder = nullptr;
    Il2CppClass* detailsView = nullptr;
    Il2CppClass* presetSelector = nullptr;
    Il2CppClass* presetSelectorBase = nullptr;
    Il2CppClass* tabHolder = nullptr;
    Il2CppClass* categoryVm = nullptr;
    Il2CppClass* tmpText = nullptr;
    Il2CppClass* behaviour = nullptr;
    Il2CppClass* unityObject = nullptr;
    Il2CppClass* localizedString = nullptr;

    // field offsets (all by name)
    size_t cfgItems = 0, catItems = 0;
    size_t catTitle = 0, catIcon = 0, catActiveCondition = 0, catShowRebind = 0, aboutIcon = 0;
    size_t defReferenceId = 0, defApplyImmediately = 0, defRequireConfirmation = 0, defShowTutorial = 0, defIndent = 0,
           defShowCondition = 0;
    size_t presetWrap = 0, presetModels = 0, presetDevices = 0, presetPlatforms = 0;
    size_t togTarget = 0, togDefault = 0, togPositive = 0, togPositiveRef = 0;
    size_t rngTarget = 0, rngMin = 0, rngMax = 0, rngStep = 0, rngNormalize = 0, rngDefault = 0;
    size_t modelDef = 0, presetModelPresets = 0, presetSetList = 0;
    size_t rowLabel = 0, detailsHeader = 0, detailsDetails = 0, selectorText = 0, selectorModel = 0;
    size_t tabTitleEvent = 0, tabTitleText = 0, vmTitle = 0;
    size_t rootItemModels = 0, rootDetailsView = 0;  // optional (empty-tab details)
    size_t rootCategories = 0, rootCategoryIndex = 0;  // optional (live refresh: which tab is shown)
    const MethodInfo* bindItemCategoryMi = nullptr;    // optional (live refresh)
    // optional (live refresh: keep the selected row and the scroll position)
    size_t rootAdapterView = 0;                        // OptionsUIRoot.optionsAdapterView
    Il2CppClass* adapterView = nullptr;                // OptionsAdapterView
    Il2CppClass* scrollArea = nullptr;                 // game's CommonControls.ScrollArea
    size_t scrollAreaPos = 0;                          // ScrollArea._scrollPosition (world units)
    const MethodInfo* scrollAreaScrollTo = nullptr;    // ScrollArea.ScrollTo(float, ScrollTransition)
    Il2CppClass* scrollRect = nullptr;                 // UnityEngine.UI.ScrollRect (fallback)
    const MethodInfo* scrollRectContent = nullptr;     // ScrollRect.get_content()
    const MethodInfo* scrollRectStop = nullptr;        // ScrollRect.StopMovement()
    const MethodInfo* rtGetAnchored = nullptr;         // RectTransform.get_anchoredPosition()
    const MethodInfo* rtSetAnchored = nullptr;         // RectTransform.set_anchoredPosition(Vector2)
    const MethodInfo* transformParent = nullptr;       // Transform.get_parent()
    // optional (live refresh: keep the game's UI focus on a live row)
    const MethodInfo* fmGetFocus = nullptr;  // FocusManager.get_CurrentFocus() (static)
    const MethodInfo* fmSetFocus = nullptr;  // FocusManager.SetFocus(GameObject, InteractionCause) (static)
    size_t rowModel = 0;                     // OptionsItemViewHolder._model
    size_t rowButton = 0;                    // ButtonViewHolder<T>.button

    // invoked methods
    const MethodInfo* modelCtor = nullptr;        // PresetOptionsItemModel..ctor(PresetOptionsItemDefinition)
    const MethodInfo* rebuildOptions = nullptr;   // PresetOptionsItemModel.RebuildOptions()
    const MethodInfo* getValueForIndex = nullptr; // ScriptedValueRangeOptionDefinition.GetValueForIndex(int)
    const MethodInfo* toggleGetPresets = nullptr; // ToggleOptionDefinition.GetOptionsPresets()
    const MethodInfo* setEnabled = nullptr;       // Behaviour.set_enabled(bool) (unused, kept resolved)
    const MethodInfo* lseUpdateString = nullptr;  // LocalizeStringEvent.UpdateString(string)
    Il2CppClass* lseClass = nullptr;
    size_t objCachedPtr = 0;                      // UnityEngine.Object.m_CachedPtr
    size_t lseStringRef = 0;                      // LocalizeStringEvent.m_StringReference (optional)
    const MethodInfo* tmpSetText = nullptr;       // TMP_Text.set_text(string)
    const MethodInfo* setHideFlags = nullptr;     // Object.set_hideFlags(HideFlags)
    const MethodInfo* lsCtor = nullptr;           // LocalizedString..ctor()

    // hook targets
    void* bindCategories = nullptr;
    void* getItems = nullptr;
    void* toggleApply = nullptr;
    void* rangeApply = nullptr;
    void* rowBind = nullptr;
    void* detailsBind = nullptr;
    void* selectorChanged = nullptr;
    void* updateString = nullptr;
    void* tabBind = nullptr;
    void* bindItemCategory = nullptr;  // optional
};
Sym S;

// ---------------------------------------------------------------- hooks ----
using BindCategoriesFn = void (*)(Il2CppObject*, Il2CppObject*, Il2CppObject*, const MethodInfo*);
using GetItemsFn = Il2CppObject* (*)(Il2CppObject*, Il2CppObject*, Il2CppObject*, bool, const MethodInfo*);
using ApplyFn = void (*)(Il2CppObject*, int32_t, const MethodInfo*);
using BindFn = void (*)(Il2CppObject*, Il2CppObject*, const MethodInfo*);

BindCategoriesFn o_bindCategories = nullptr;
GetItemsFn o_getItems = nullptr;
ApplyFn o_toggleApply = nullptr;
ApplyFn o_rangeApply = nullptr;
BindFn o_rowBind = nullptr;
BindFn o_detailsBind = nullptr;
ApplyFn o_selectorChanged = nullptr;  // (self, int selection)
using UpdateStringFn = void (*)(Il2CppObject*, Il2CppString*, const MethodInfo*);
UpdateStringFn o_updateString = nullptr;
BindFn o_tabBind = nullptr;
BindFn o_bindItemCategory = nullptr;  // (OptionsUIRoot*, OptionsItemCategory*)

std::atomic<bool> g_active{false};

// ---------------------------------------------------------- tab state ----
// Everything below is touched on the Unity main thread only (all hooked
// methods are UI code).
struct Row {
    settings::Def def;
    std::string label;
    bool toggle = false;
    bool indented = false;
    bool isParent = false;           // other rows' visibility depends on it (Def::parent, or the module's "enabled")
    bool shown = true;               // in the model list built last (settings::Visible at that time)
    Il2CppObject* defObj = nullptr;  // our cloned definition (GC handle held)
    Il2CppObject* target = nullptr;  // private ScriptedBool / ScriptedFloat (may be null)
    int trueIndex = -1;              // toggles: preset index meaning "on" (lazy)
    double rmin = 0, rmax = 1, rstep = 1;  // steppers
};

struct CachedDef {
    Il2CppObject* obj = nullptr;
    Il2CppObject* target = nullptr;
    bool toggle = false;
};

Il2CppObject* g_tplToggle = nullptr;
Il2CppObject* g_tplRange = nullptr;
Il2CppObject* g_tplCategory = nullptr;
Il2CppObject* g_category = nullptr;       // our "Mods" OptionsItemCategory
Il2CppObject* g_categoryTitle = nullptr;  // its _title LocalizedString (identity marker)
std::vector<Row> g_rows;
std::unordered_map<Il2CppObject*, size_t> g_rowByDef;
std::unordered_map<std::string, CachedDef> g_defCache;  // "module\x1fkey"
int g_textWarned = 0;
// Last successfully built, NON-EMPTY model array for the Mods tab. The game
// indexes optionsItemModels[0] right after GetOptionsItemsForCategory
// (BindOptionsDetailsView(0)), so an empty list for a bound tab throws
// ArgumentOutOfRange. The tab is only injected once this exists, and in fault
// mode GetItems returns it as-is (no game calls).
Il2CppObject* g_cachedModels = nullptr;
void* g_cachedModelsHandle = nullptr;
bool g_loggedActive = false;

// -------------------------------------------------------------- guards ----
// Last step entered (plain pointers so __try frames can set them). Reported
// with every error, so the log names the exact step that failed.
const char* g_stage = "-";
const Row* g_stageRow = nullptr;
std::string g_disabledReason;

inline void Stage(const char* s, const Row* row = nullptr) {
    g_stage = s;
    g_stageRow = row;
}

std::string StageText() {
    std::string s = std::string("step '") + (g_stage ? g_stage : "?") + "'";
    if (g_stageRow) s += " (row " + g_stageRow->def.module + "." + g_stageRow->def.key + ")";
    return s;
}

void Disable(const char* where, const std::string& why) {
    if (g_active.exchange(false)) {
        g_disabledReason = std::format("{} at {} in {}", why, StageText(), where);
        SR2_ERROR("ui: options tab DISABLED for this session: {} - the Mods tab will not be injected again "
                  "(the overlay still works)", g_disabledReason);
        SetOptionsTabStatus("disabled after error: " + g_disabledReason);
    }
}

// Fault mode: after ANY native exception (in a hook here, or inside game code
// reached through il::Invoke) we never run mod logic or call game methods
// again; every hook passes straight through to the original.
inline bool Faulted() { return il::AnyFault(); }

__declspec(noinline) void OnSeh(const char* where, DWORD code) {
    il::MarkFault(where, code);
    Disable(where, std::format("native exception 0x{:08X} (all options-tab hooks now pass through)", code));
}

template <class F>
__declspec(noinline) void RunCpp(const char* where, F& f) {
    try {
        f();
    } catch (const std::exception& e) {
        Disable(where, e.what());
    } catch (...) {
        Disable(where, "unknown C++ exception");
    }
}

template <class F>
void Guarded(const char* where, F&& f) {
    if (!g_active.load(std::memory_order_relaxed) || Faulted()) return;
    Stage("entry");
    __try {
        RunCpp(where, f);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        OnSeh(where, GetExceptionCode());
    }
}

// ------------------------------------------------------------- helpers ----
template <class T>
T& F(Il2CppObject* o, size_t off) {
    return unity::Field<T>(o, off);
}

void SetRef(Il2CppObject* obj, size_t off, void* value) {
    using Fn = void (*)(Il2CppObject*, void**, void*);
    static Fn wb = reinterpret_cast<Fn>(il::Export("il2cpp_gc_wbarrier_set_field"));
    void** slot = reinterpret_cast<void**>(reinterpret_cast<char*>(obj) + off);
    if (wb)
        wb(obj, slot, value);
    else
        *slot = value;
}

Il2CppObject* NewArray(Il2CppClass* elem, size_t n) {
    using Fn = Il2CppObject* (*)(Il2CppClass*, uintptr_t);
    static Fn fn = reinterpret_cast<Fn>(il::Export("il2cpp_array_new"));
    return fn ? fn(elem, n) : nullptr;
}

void KeepAsset(Il2CppObject* o, const std::string& name) {
    if (!o) return;
    il::Keep(o);
    int flags = kHideFlagsDontUnloadUnusedAsset;
    il::Invoke(S.setHideFlags, o, {&flags});
    unity::SetName(o, name);
}

bool ListInvoke(Il2CppObject* list, const char* method, int argc, void* arg) {
    if (!list) return false;
    const MethodInfo* m = il::Method(il::ClassOf(list), method, argc);
    if (!m) return false;
    std::string err;
    if (argc == 0)
        il::Invoke(m, list, {}, &err);
    else
        il::Invoke(m, list, {arg}, &err);
    if (!err.empty()) {
        SR2_WARN("ui: options tab: List.{} failed: {}", method, err);
        return false;
    }
    return true;
}

bool ListContains(Il2CppObject* list, Il2CppObject* item) {
    size_t n = unity::ListCount(list);
    for (size_t i = 0; i < n; ++i)
        if (unity::ListAt(list, i) == item) return true;
    return false;
}

std::string RowKey(const settings::Def& d) { return d.module + '\x1f' + d.key; }

bool SuitableTemplate(Il2CppObject* d) {
    if (!unity::IsAlive(d)) return false;
    if (unity::ListCount(F<void*>(d, S.presetDevices)) != 0) return false;   // device-specific row
    if (unity::ListCount(F<void*>(d, S.presetPlatforms)) != 0) return false; // platform-specific row
    if (F<void*>(d, S.defShowCondition) != nullptr) return false;           // conditional row
    std::string id = unity::ToUtf8(F<Il2CppString*>(d, S.defReferenceId));
    return id.rfind("sr2mods.", 0) != 0;
}

bool EnsureTemplates(Il2CppObject* cfg) {
    if (unity::IsAlive(g_tplCategory) && (unity::IsAlive(g_tplToggle) || unity::IsAlive(g_tplRange))) return true;
    g_tplToggle = g_tplRange = g_tplCategory = nullptr;
    Il2CppObject* cats = F<Il2CppObject*>(cfg, S.cfgItems);
    size_t nc = unity::ListCount(cats);
    for (size_t c = 0; c < nc; ++c) {
        auto* cat = static_cast<Il2CppObject*>(unity::ListAt(cats, c));
        if (!unity::IsAlive(cat) || cat == g_category) continue;
        if (!g_tplCategory) g_tplCategory = cat;
        Il2CppObject* defs = F<Il2CppObject*>(cat, S.catItems);
        size_t nd = unity::ListCount(defs);
        for (size_t i = 0; i < nd; ++i) {
            auto* d = static_cast<Il2CppObject*>(unity::ListAt(defs, i));
            Il2CppClass* k = il::ClassOf(d);
            if (!g_tplToggle && k == S.toggleDef && SuitableTemplate(d)) g_tplToggle = d;
            if (!g_tplRange && k == S.rangeDef && SuitableTemplate(d)) g_tplRange = d;
        }
    }
    if (!g_tplCategory || (!g_tplToggle && !g_tplRange)) {
        SR2_WARN("ui: options tab: no usable template rows in OptionsConfiguration ({} categories, toggle {}, range {})",
                 nc, static_cast<void*>(g_tplToggle), static_cast<void*>(g_tplRange));
        return false;
    }
    SR2_INFO("ui: options tab templates: category '{}', toggle '{}', range '{}'", unity::GetName(g_tplCategory),
             unity::GetName(g_tplToggle), unity::GetName(g_tplRange));
    return true;
}

Il2CppObject* NewEmptyLocalizedString() {
    if (!S.localizedString || !S.lsCtor) return nullptr;
    Il2CppObject* ls = il::NewObject(S.localizedString);
    if (!ls) return nullptr;
    std::string err;
    il::Invoke(S.lsCtor, ls, {}, &err);
    return err.empty() ? ls : nullptr;
}

// Stepper range for a Float/Int/Enum def.
void StepperRange(const settings::Def& d, double& lo, double& hi, double& step) {
    if (d.kind == settings::Kind::Enum) {
        lo = 0;
        hi = d.options.empty() ? std::max(0.0, d.max) : static_cast<double>(d.options.size() - 1);
        step = 1;
        return;
    }
    lo = std::min(d.min, d.max);
    hi = std::max(d.min, d.max);
    step = d.step;
    if (d.kind == settings::Kind::Int) step = std::max(1.0, std::round(step));
    if (step <= 0) step = (hi - lo) / 20.0;
    if (step <= 0) step = 1;
    double n = std::floor((hi - lo) / step + 1e-6) + 1;
    if (n > kMaxStepperPresets) step *= std::ceil(n / kMaxStepperPresets);  // stays on the Def's grid
}

Il2CppObject* CreateDefObject(const Row& r, Il2CppObject*& targetOut) {
    targetOut = nullptr;
    Il2CppObject* tpl = r.toggle ? g_tplToggle : g_tplRange;
    if (!unity::IsAlive(tpl)) return nullptr;
    Il2CppObject* obj = unity::Instantiate(tpl, nullptr);
    if (!obj) {
        SR2_WARN("ui: options tab: Object.Instantiate of the template definition failed");
        return nullptr;
    }
    const std::string id = "sr2mods." + r.def.module + "." + r.def.key;
    KeepAsset(obj, id);
    SetRef(obj, S.defReferenceId, unity::NewString(id));  // unique id: never matches a game option
    SetRef(obj, S.defShowCondition, nullptr);
    F<bool>(obj, S.defApplyImmediately) = true;
    F<bool>(obj, S.defRequireConfirmation) = false;
    F<bool>(obj, S.defShowTutorial) = false;
    // _label / _detailsText keep the template's LocalizedString; the texts are
    // replaced at bind time (see the BindData hooks).
    // Private ScriptedValue so nothing can ever write the template's real setting.
    const size_t tOff = r.toggle ? S.togTarget : S.rngTarget;
    Il2CppObject* tplTarget = F<Il2CppObject*>(tpl, tOff);
    if (unity::IsAlive(tplTarget)) {
        if (Il2CppObject* t = unity::Instantiate(tplTarget, nullptr)) {
            KeepAsset(t, id + ".value");
            SetRef(obj, tOff, t);
            targetOut = t;
        }
    }
    if (!targetOut) SetRef(obj, tOff, nullptr);
    if (!r.toggle) F<bool>(obj, S.rngNormalize) = false;
    return obj;
}

// Refreshes the per-bind fields of a row's definition from the current value.
void UpdateDefObject(Row& r) {
    const double cur = settings::Get(r.def.module, r.def.key);
    F<uint32_t>(r.defObj, S.defIndent) = r.indented ? 1u : 0u;
    if (r.toggle) {
        F<bool>(r.defObj, S.togDefault) = cur >= 0.5;
    } else {
        StepperRange(r.def, r.rmin, r.rmax, r.rstep);
        F<float>(r.defObj, S.rngMin) = static_cast<float>(r.rmin);
        F<float>(r.defObj, S.rngMax) = static_cast<float>(r.rmax);
        F<float>(r.defObj, S.rngStep) = static_cast<float>(r.rstep);
        F<float>(r.defObj, S.rngDefault) = static_cast<float>(Snap(r.def, cur));
        F<bool>(r.defObj, S.rngNormalize) = false;
        F<bool>(r.defObj, S.presetWrap) = r.def.kind == settings::Kind::Enum;
    }
}

void RebuildRows() {
    g_rows.clear();
    g_rowByDef.clear();
    for (const auto& mv : BuildModuleViews()) {
        auto add = [&](const settings::Def& d, std::string label, bool indented) {
            if (d.kind == settings::Kind::Key) return;  // overlay-only
            const bool toggle = d.kind == settings::Kind::Bool;
            if (toggle ? !g_tplToggle : !g_tplRange) return;
            Row r;
            r.def = d;
            r.label = std::move(label);
            r.toggle = toggle;
            r.indented = indented;
            CachedDef& c = g_defCache[RowKey(d)];
            if (!unity::IsAlive(c.obj) || c.toggle != toggle) {
                c.obj = CreateDefObject(r, c.target);
                c.toggle = toggle;
            }
            if (!c.obj) return;
            r.defObj = c.obj;
            r.target = c.target;
            UpdateDefObject(r);
            g_rowByDef[r.defObj] = g_rows.size();
            g_rows.push_back(std::move(r));
        };
        const bool hasMaster = mv.hasEnabled && mv.module != kUiModule;
        if (hasMaster) add(mv.enabled, mv.title, false);
        for (const auto& g : mv.groups)
            for (const auto& d : g.defs) {
                std::string label = d.label.empty() ? d.key : d.label;
                if (!hasMaster) label = mv.title + ": " + label;
                if (!g.name.empty()) label = g.name + " - " + label;
                add(d, std::move(label), hasMaster);
            }
    }
    // Rows whose value decides whether other rows are listed: a change to one
    // rebinds the tab (ScheduleRefresh). Only rows actually present count, so
    // e.g. Autobhop does not refresh for its overlay-only key row.
    for (Row& p : g_rows) {
        if (p.def.key == kKeyEnabled) {
            p.isParent = true;
            continue;
        }
        for (const Row& c : g_rows)
            if (c.def.module == p.def.module && c.def.parent == p.def.key) {
                p.isParent = true;
                break;
            }
    }
}

bool EnsureCategory() {
    if (!unity::IsAlive(g_category)) {
        g_category = unity::Instantiate(g_tplCategory, nullptr);
        if (!g_category) {
            SR2_WARN("ui: options tab: cloning the template OptionsItemCategory failed");
            return false;
        }
        KeepAsset(g_category, "sr2mods.ModsCategory");
        SetRef(g_category, S.catActiveCondition, nullptr);
        F<bool>(g_category, S.catShowRebind) = false;
        // Fresh LocalizedString as the title: a unique identity marker for the
        // tab view holder hook (the text itself is set directly).
        if (Il2CppObject* ls = NewEmptyLocalizedString()) SetRef(g_category, S.catTitle, ls);
        g_categoryTitle = F<Il2CppObject*>(g_category, S.catTitle);
        if (g_categoryTitle && g_categoryTitle == F<Il2CppObject*>(g_tplCategory, S.catTitle)) {
            SR2_WARN("ui: options tab: could not give the Mods tab its own title object; it keeps the template's name");
            g_categoryTitle = nullptr;
        }
    }
    // Rows of the category asset (informational for the game; our models come
    // from the GetOptionsItemsForCategory hook).
    Il2CppObject* items = F<Il2CppObject*>(g_category, S.catItems);
    if (items) {
        ListInvoke(items, "Clear", 0, nullptr);
        for (const auto& r : g_rows) ListInvoke(items, "Add", 1, r.defObj);
    }
    return true;
}

const Row* RowForDef(Il2CppObject* defObj) {
    auto it = g_rowByDef.find(defObj);
    return it == g_rowByDef.end() ? nullptr : &g_rows[it->second];
}

const Row* RowForModel(Il2CppObject* model) {
    if (!model || g_rows.empty() || !il::IsA(model, S.itemModel)) return nullptr;
    return RowForDef(F<Il2CppObject*>(model, S.modelDef));
}

int ToggleTrueIndex(Row& r) {
    if (r.trueIndex >= 0) return r.trueIndex;
    r.trueIndex = 1;  // fallback: [negative, positive]
    Il2CppObject* set = il::Invoke(S.toggleGetPresets, r.defObj, {});
    Il2CppObject* list = set ? F<Il2CppObject*>(set, S.presetSetList) : nullptr;
    Il2CppObject* positive = F<Il2CppObject*>(r.defObj, S.togPositive);
    std::string posRef = unity::ToUtf8(F<Il2CppString*>(r.defObj, S.togPositiveRef));
    size_t n = unity::ListCount(list);
    for (size_t i = 0; i < n; ++i) {
        auto* p = static_cast<Il2CppObject*>(unity::ListAt(list, i));
        Il2CppClass* pk = il::ClassOf(p);
        size_t labelOff = 0, refOff = 0;
        if (positive && il::FieldOffset(pk, "<Label>k__BackingField", labelOff) && F<Il2CppObject*>(p, labelOff) == positive) {
            r.trueIndex = static_cast<int>(i);
            break;
        }
        if (!posRef.empty() && il::FieldOffset(pk, "<ReferenceId>k__BackingField", refOff) &&
            unity::ToUtf8(F<Il2CppString*>(p, refOff)) == posRef) {
            r.trueIndex = static_cast<int>(i);
            break;
        }
    }
    return r.trueIndex;
}

bool RangeValue(const Row& r, int index, double& out) {
    float v = 0;
    if (il::UnboxTo(il::Invoke(S.getValueForIndex, r.defObj, {&index}), v)) {
        out = v;
        return true;
    }
    out = r.rmin + r.rstep * index;  // same formula, if the call failed
    return false;
}

std::string ValueText(Row& r, int index) {
    if (r.toggle) return index == ToggleTrueIndex(r) ? "On" : "Off";
    double v = 0;
    RangeValue(r, index, v);
    return FormatValue(r.def, Snap(r.def, v));
}

std::string DetailsText(const Row& r) {
    std::string s = r.def.tooltip;
    if (r.def.key == kKeyEnabled) {
        bool failed = false;
        std::string st = ModuleStatus(r.def.module, &failed);
        if (!st.empty()) s += (s.empty() ? "" : "\n") + std::string("Status: ") + st;
    }
    if (core::version_guard::Mismatch())
        s += (s.empty() ? "" : "\n\n") + std::string("Version warning: ") + core::version_guard::Status();
    if (s.empty()) s = "Mod setting (" + PrettyModuleName(r.def.module) + ").";
    s += "\nSaved automatically to sr2mods.json. Key bindings: use the overlay (" +
         KeyName(static_cast<int>(settings::Get(kUiModule, kKeyOverlayHotkey))) + ").";
    return s;
}

// LocalizeStringEvent override. The component stays enabled; instead its
// single text sink, LocalizeStringEvent.UpdateString(string), is hooked and
// substitutes our text for the events we own. Every refresh path (rebinding
// of recycled/paged row views, deferred async localization, OnEnable,
// locale change) then shows our label. Game LSEs are not in the map and pass
// through untouched.
struct OwnedText {
    void* cachedPtr = nullptr;  // UnityEngine.Object.m_CachedPtr when registered (detects reuse)
    std::string text;
};
std::unordered_map<Il2CppObject*, OwnedText> g_ownedText;

void* CachedPtr(Il2CppObject* o) { return o ? F<void*>(o, S.objCachedPtr) : nullptr; }

void OverrideText(Il2CppObject* lse, const std::string& text) {
    if (!unity::IsAlive(lse) || !il::IsA(lse, S.lseClass)) return;
    g_ownedText[lse] = OwnedText{CachedPtr(lse), text};
    // Push it now through the game's own path (goes through HkUpdateString).
    std::string err;
    il::Invoke(S.lseUpdateString, lse, {unity::NewString(text)}, &err);
    if (err.empty()) return;
    if (g_textWarned++ < 3) SR2_WARN("ui: options tab: LocalizeStringEvent.UpdateString failed ({}); setting TMP text", err);
    if (Il2CppObject* tmp = unity::GetComponent(lse, S.tmpText)) il::Invoke(S.tmpSetText, tmp, {unity::NewString(text)});
}

// The view is about to be rebound (possibly to a game row): release it so the
// game's localization applies again.
void RestoreText(Il2CppObject* lse) {
    if (lse) g_ownedText.erase(lse);
}

// --------------------------------------------------------------- hooks ----
// Icon for the Mods tab: a fitting stock category's sprite (controls /
// gameplay / accessibility), else the About tab's, else the template's.
Il2CppObject* PickIcon(Il2CppObject* cfg, Il2CppObject* about) {
    static const char* const kPreferred[] = {"control", "input", "gameplay", "game", "accessib", "other"};
    Il2CppObject* cats = F<Il2CppObject*>(cfg, S.cfgItems);
    const size_t n = unity::ListCount(cats);
    for (const char* want : kPreferred) {
        for (size_t i = 0; i < n; ++i) {
            auto* c = static_cast<Il2CppObject*>(unity::ListAt(cats, i));
            if (!unity::IsAlive(c) || c == g_category || c == g_tplCategory) continue;
            std::string name = unity::GetName(c);
            for (auto& ch : name) ch = static_cast<char>(tolower(static_cast<unsigned char>(ch)));
            Il2CppObject* icon = F<Il2CppObject*>(c, S.catIcon);
            if (name.find(want) != std::string::npos && unity::IsAlive(icon)) {
                static bool logged = false;
                if (!logged) SR2_INFO("ui: options tab: Mods tab uses the icon of category '{}'", unity::GetName(c));
                logged = true;
                return icon;
            }
        }
    }
    if (S.aboutIcon && unity::IsAlive(about)) {
        Il2CppObject* icon = F<Il2CppObject*>(about, S.aboutIcon);
        if (unity::IsAlive(icon)) {
            static bool logged = false;
            if (!logged) SR2_INFO("ui: options tab: Mods tab uses the About tab's icon");
            logged = true;
            return icon;
        }
    }
    return nullptr;
}

Il2CppObject* BuildModels();

void CacheModels(Il2CppObject* arr) {
    if (!arr || unity::ArrayLength(arr) == 0 || arr == g_cachedModels) return;
    void* h = il::Keep(arr);
    if (!h) return;
    void* old = g_cachedModelsHandle;
    g_cachedModels = arr;
    g_cachedModelsHandle = h;
    il::Release(old);
}

Il2CppObject* InjectCategory(Il2CppObject* cfg, Il2CppObject* about) {
    Stage("BindCategories: check configuration");
    if (!unity::IsAlive(cfg)) {
        SR2_WARN("ui: options tab: BindCategories got no OptionsConfiguration; Mods tab skipped");
        return nullptr;
    }
    Stage("BindCategories: find templates");
    if (!EnsureTemplates(cfg)) return nullptr;
    Stage("BindCategories: build rows");
    RebuildRows();
    if (g_rows.empty()) {
        SR2_INFO("ui: options tab: no settings to show; Mods tab skipped");
        return nullptr;
    }
    Stage("BindCategories: create category");
    if (!EnsureCategory()) return nullptr;
    // Build the row models now: the tab is only added if it can show rows, so
    // the game's category / model / details lists can never disagree.
    Stage("BindCategories: build row models");
    Il2CppObject* models = BuildModels();
    if (Faulted()) return nullptr;
    if (!models || unity::ArrayLength(models) == 0) {
        SR2_WARN("ui: options tab: no row model could be built; Mods tab not injected");
        return nullptr;
    }
    CacheModels(models);
    Stage("BindCategories: set icon");
    {
        // Our own icon (ui/mod_icons.h); the stock one picked above is the fallback and its size reference.
        Il2CppObject* icon = PickIcon(cfg, about);
        if (Il2CppObject* own = mod_icons::Get("mods_tab", icon)) icon = own;
        if (icon) SetRef(g_category, S.catIcon, icon);
    }
    Stage("BindCategories: append to configuration");
    Il2CppObject* list = F<Il2CppObject*>(cfg, S.cfgItems);
    if (!list || ListContains(list, g_category)) return nullptr;
    if (!ListInvoke(list, "Add", 1, g_category)) return nullptr;
    SR2_INFO("ui: options tab: 'Mods' category injected ({} rows) into OptionsUIRoot binding", g_rows.size());
    SetOptionsTabStatus(std::format("active ({} rows)", g_rows.size()));
    Stage("-");
    return list;
}

struct RemoveOnExit {
    Il2CppObject* list = nullptr;
    ~RemoveOnExit() {
        // Runs even if the tab was disabled meanwhile, but never after a fault
        // (no game calls then; GetItems keeps our category empty instead).
        if (!list || Faulted()) return;
        Il2CppObject* l = list;
        __try {
            ListInvoke(l, "Remove", 1, g_category);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            il::MarkFault("List.Remove(Mods category)", GetExceptionCode());
        }
    }
};

void HkBindCategories(Il2CppObject* self, Il2CppObject* cfg, Il2CppObject* about, const MethodInfo* mi) {
    if (Faulted()) {
        SR2_WARN("ui: options tab is in fault mode ({}); Mods tab not injected into this Options menu",
                 g_disabledReason.empty() ? std::string("native exception elsewhere in the UI") : g_disabledReason);
        return o_bindCategories(self, cfg, about, mi);
    }
    RemoveOnExit cleanup;
    if (!g_active.load()) {
        // Logged on every Options open so a missing tab is easy to explain.
        SR2_WARN("ui: options tab is disabled ({}); Mods tab not injected into this Options menu",
                 g_disabledReason.empty() ? std::string("not started") : g_disabledReason);
    }
    // Re-injected for every OptionsUIRoot instance (main menu, pause menu, after
    // scene changes); cached objects are validated with IsAlive and rebuilt.
    Guarded("OptionsUIRoot.BindCategories", [&] { cleanup.list = InjectCategory(cfg, about); });
    o_bindCategories(self, cfg, about, mi);
}

// One row's model, built with the PresetOptionsItemModel(definition)
// constructor (what CreateOptionItemModel does, minus registering it in the
// definition's _optionsItemModels list, which a runtime clone may not have).
// Kept in its own SEH frame so a bad row is skipped and logged with its step.
Il2CppObject* CreateModelCpp(Row& r, const char*& stage) {
    stage = "IsAlive(definition)";
    if (!unity::IsAlive(r.defObj)) return nullptr;
    stage = "check definition class";
    Il2CppClass* k = il::ClassOf(r.defObj);
    if (k != (r.toggle ? S.toggleDef : S.rangeDef)) {
        SR2_WARN("ui: options tab: row {}.{}: definition has unexpected class {}", r.def.module, r.def.key,
                 unity::ClassName(r.defObj));
        return nullptr;
    }
    stage = "refresh definition fields";
    UpdateDefObject(r);  // values may have changed in the overlay since binding
    stage = "il2cpp_object_new(PresetOptionsItemModel)";
    Il2CppObject* m = il::NewObject(S.presetModel);
    if (!m) {
        SR2_WARN("ui: options tab: il2cpp_object_new(PresetOptionsItemModel) failed");
        return nullptr;
    }
    stage = "PresetOptionsItemModel..ctor(definition)";
    std::string err;
    il::Invoke(S.modelCtor, m, {r.defObj}, &err);
    if (!err.empty()) {
        SR2_WARN("ui: options tab: row {}.{}: PresetOptionsItemModel ctor failed: {}", r.def.module, r.def.key, err);
        return nullptr;
    }
    stage = "check presets";
    if (!F<Il2CppObject*>(m, S.presetModelPresets)) {
        stage = "RebuildOptions";
        il::Invoke(S.rebuildOptions, m, {}, &err);
        if (!F<Il2CppObject*>(m, S.presetModelPresets)) {
            SR2_WARN("ui: options tab: row {}.{}: model has no presets ({})", r.def.module, r.def.key, err);
            return nullptr;
        }
    }
    return m;
}

__declspec(noinline) void LogRowSeh(const Row& r, const char* stage, DWORD code) {
    SR2_ERROR("ui: options tab: row {}.{} skipped: native exception 0x{:08X} at step '{}'", r.def.module, r.def.key,
              code, stage ? stage : "?");
}

template <class F>
__declspec(noinline) Il2CppObject* RowCpp(F& f) {
    try {
        return f();
    } catch (const std::exception& e) {
        SR2_ERROR("ui: options tab: row skipped: {}", e.what());
    } catch (...) {
        SR2_ERROR("ui: options tab: row skipped: unknown C++ exception");
    }
    return nullptr;
}

Il2CppObject* CreateModelGuarded(Row& r) {
    const char* stage = "start";
    auto f = [&] { return CreateModelCpp(r, stage); };
    __try {
        return RowCpp(f);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        il::MarkFault("options tab row model", GetExceptionCode());
        LogRowSeh(r, stage, GetExceptionCode());
        return nullptr;
    }
}

Il2CppObject* BuildModels() {
    // No GC handles in this path: the models are stored into a managed array
    // as soon as they exist. The array pointer lives in a volatile stack slot,
    // which the conservative GC scans, so models stay alive until returned.
    // Child rows (settings::Def::parent, module "enabled") are left out while
    // their parent does not allow them. Hidden rows keep their definition and
    // their g_rowByDef entry, so an Apply or a recycled view that still refers
    // to one is recognised as ours. Never an empty list: if nothing would be
    // shown (cannot happen while master rows exist), everything is.
    size_t want = 0;
    for (Row& r : g_rows) {
        r.shown = settings::Visible(r.def.module, r.def.key);
        if (r.shown) ++want;
    }
    if (want == 0) {
        for (Row& r : g_rows) r.shown = true;
        want = g_rows.size();
    }
    Stage("GetItems: il2cpp_array_new(OptionsItemModel, rows)");
    Il2CppObject* full = NewArray(S.itemModel, want);
    if (!full) {
        SR2_WARN("ui: options tab: il2cpp_array_new({}) failed", want);
        return nullptr;
    }
    Il2CppObject* volatile keepAlive = full;
    std::vector<Il2CppObject*> models;
    models.reserve(want);
    size_t failed = 0;
    for (auto& r : g_rows) {
        if (!r.shown) continue;
        Il2CppObject* m = CreateModelGuarded(r);
        if (Faulted()) return nullptr;  // stop immediately; caller passes through
        if (!m) {
            ++failed;
            continue;
        }
        Stage("GetItems: store model", &r);
        SetRef(full, unity::kArrayDataOffset + models.size() * sizeof(void*), m);
        models.push_back(m);
    }
    Il2CppObject* arr = full;
    if (models.size() != want) {  // some rows failed: exact-size array, no null entries
        Stage("GetItems: il2cpp_array_new(OptionsItemModel, built)");
        arr = NewArray(S.itemModel, models.size());
        if (arr) {
            Stage("GetItems: copy models");
            for (size_t i = 0; i < models.size(); ++i)
                SetRef(arr, unity::kArrayDataOffset + i * sizeof(void*), models[i]);
        }
    }
    (void)keepAlive;
    static bool logged = false;
    if (!logged || failed) SR2_INFO("ui: options tab: {} row models built, {} skipped", models.size(), failed);
    logged = true;
    if (failed && models.empty()) Disable("GetOptionsItemsForCategory", "no row model could be created");
    Stage("-");
    return arr;
}

Il2CppObject* HkGetItems(Il2CppObject* self, Il2CppObject* cfg, Il2CppObject* cat, bool profileFirst, const MethodInfo* mi) {
    if (cat && g_category && cat == g_category) {
        Il2CppObject* arr = nullptr;
        // Healthy: fresh models (values may have changed in the overlay).
        Guarded("OptionsViewModel.GetOptionsItemsForCategory", [&] {
            Il2CppObject* fresh = BuildModels();
            if (!Faulted() && fresh && unity::ArrayLength(fresh) > 0) {
                CacheModels(fresh);
                arr = fresh;
            }
        });
        // Disabled / faulted / rebuild failed: the last good non-empty array
        // (returning a pointer we already hold; no game calls). Our category
        // never reaches the game's own lookup, and never yields an empty list.
        if (!arr) arr = g_cachedModels;
        if (arr) return arr;
    }
    return o_getItems(self, cfg, cat, profileFirst, mi);
}

// Empty Mods tab (no rows could be built): replace the stale details text.
void HkBindItemCategory(Il2CppObject* self, Il2CppObject* category, const MethodInfo* mi) {
    o_bindItemCategory(self, category, mi);
    if (Faulted()) return;
    Guarded("OptionsUIRoot.BindItemCategory", [&] {
        Stage("BindItemCategory: check category");
        if (!category || category != g_category) return;
        Stage("BindItemCategory: count models");
        if (unity::ListCount(F<void*>(self, S.rootItemModels)) != 0) return;  // rows: details follow the selection
        Stage("BindItemCategory: details view");
        Il2CppObject* dv = F<Il2CppObject*>(self, S.rootDetailsView);
        if (!unity::IsAlive(dv)) return;
        OverrideText(F<Il2CppObject*>(dv, S.detailsHeader), kTabTitle);
        OverrideText(F<Il2CppObject*>(dv, S.detailsDetails),
                     "Mods: settings for sr2mods.\nNo settings could be shown here; use the overlay (" +
                         KeyName(static_cast<int>(settings::Get(kUiModule, kKeyOverlayHotkey))) + ").");
        Stage("-");
    });
}

// ---- live show/hide of child rows ----
// A row other rows depend on (Row::isParent) changed: rebind the Mods tab so
// GetOptionsItemsForCategory (HkGetItems) returns the new set of rows. Runs
// as a posted main-thread job, i.e. after the selector / Apply call stack
// that changed the value has returned, and only if the Mods tab is still the
// one the open OptionsUIRoot shows (otherwise the next swap to it rebinds).
// BindItemCategory is the game's own tab-swap path: it rebuilds the model
// list, rebinds the paged adapter (row views are recycled through
// HkRowBind, which re-registers our labels) and resets the details view.
//
// The rebind selects row 0 and the focus change scrolls the list to it. To
// keep the user's place, the job saves the scroll offset of the list's scroll
// container first, then re-selects the changed row (same setting key) with
// AdapterView.SelectItem(index), the adapter's own selection path (gamepad
// navigator position, row highlight, OnItemSelect -> details view), and puts
// the offset back. Rows above the changed one never change (children follow
// their parent), so the absolute offset is the right one to keep. The game's
// focus handling may still scroll to row 0 a frame later, so the offset is
// re-applied for a few ticks while the changed row stays selected. Nothing is
// held across ticks except plain values: every tick looks the root up again.
bool g_refreshQueued = false;  // main thread only
std::string g_refreshKey;      // RowKey of the row whose change queued the refresh

struct Vec2 {
    float x = 0, y = 0;
};

struct ScrollSave {
    bool valid = false;
    bool area = false;  // ScrollArea (pos) or ScrollRect (content.anchoredPosition)
    float pos = 0;
    Vec2 anchored;
};

struct ModsView {
    Il2CppObject* root = nullptr;
    Il2CppObject* adapter = nullptr;  // OptionsAdapterView or nullptr
};

constexpr int kSettleTicks = 6;
int g_settleTicks = 0;  // main thread only
int g_settleIndex = -1;
ScrollSave g_settleScroll;
bool g_settleFocus = false;  // keep the UI focus on a live row while settling

// True if this OptionsUIRoot shows the Mods tab: its current category is ours,
// or (fallback) the row models it bound last are ours, i.e. the last
// BindItemCategory it ran was for the Mods category. The fallback matters
// because a refresh is only ever queued by a change made in one of our rows,
// so the root that holds our models is the one on screen.
bool ShowsMods(Il2CppObject* root) {
    if (!unity::IsAlive(root) || !il::IsA(root, S.uiRoot)) return false;
    Il2CppObject* cats = F<Il2CppObject*>(root, S.rootCategories);
    const int32_t idx = F<int32_t>(root, S.rootCategoryIndex);
    if (cats && idx >= 0 && static_cast<size_t>(idx) < unity::ListCount(cats) &&
        unity::ListAt(cats, static_cast<size_t>(idx)) == g_category)
        return true;
    if (!S.rootItemModels) return false;
    Il2CppObject* models = F<Il2CppObject*>(root, S.rootItemModels);
    return unity::ListCount(models) > 0 && RowForModel(static_cast<Il2CppObject*>(unity::ListAt(models, 0))) != nullptr;
}

// One line per loaded OptionsUIRoot, for the log when no root shows the Mods tab.
std::string DescribeRoots() {
    std::string s;
    const auto all = unity::FindObjectsOfTypeAll(S.uiRoot);
    for (Il2CppObject* r : all) {
        if (!unity::IsAlive(r) || !il::IsA(r, S.uiRoot)) continue;
        Il2CppObject* go = unity::GameObjectOf(r);
        Il2CppObject* cats = F<Il2CppObject*>(r, S.rootCategories);
        const int32_t idx = F<int32_t>(r, S.rootCategoryIndex);
        const size_t n = unity::ListCount(cats);
        auto* cur = (idx >= 0 && static_cast<size_t>(idx) < n) ? static_cast<Il2CppObject*>(unity::ListAt(cats, idx)) : nullptr;
        Il2CppObject* models = S.rootItemModels ? F<Il2CppObject*>(r, S.rootItemModels) : nullptr;
        const size_t nm = unity::ListCount(models);
        const bool oursModels = nm > 0 && RowForModel(static_cast<Il2CppObject*>(unity::ListAt(models, 0))) != nullptr;
        s += std::format("{}['{}' active {}, tab {}/{} {}, {} models{}]", s.empty() ? "" : ", ", unity::GetName(go),
                         go && unity::ActiveInHierarchy(go), idx, n,
                         cur == g_category ? "= Mods" : (cur ? "'" + unity::GetName(cur) + "'" : std::string("-")), nm,
                         oursModels ? " (ours)" : "");
    }
    return s.empty() ? "no OptionsUIRoot loaded" : s;
}

// The OptionsUIRoot that shows the Mods tab (see ShowsMods). The first active
// instance is tried first; if that is another one (the game can keep more
// than one Options root loaded), every loaded instance whose GameObject is
// active is checked. Nothing is kept: each call looks the root up again.
bool FindModsView(ModsView& v) {
    if (!S.rootCategories || !S.rootCategoryIndex || !unity::IsAlive(g_category)) return false;
    Il2CppObject* first = unity::FindObjectOfType(S.uiRoot);  // active scene instance only
    Il2CppObject* root = ShowsMods(first) ? first : nullptr;
    if (!root) {
        for (Il2CppObject* r : unity::FindObjectsOfTypeAll(S.uiRoot)) {  // also prefab assets: active check
            if (r == first || !ShowsMods(r)) continue;
            Il2CppObject* go = unity::GameObjectOf(r);
            if (go && unity::ActiveInHierarchy(go)) {
                root = r;
                break;
            }
        }
    }
    if (!root) return false;
    v.root = root;
    v.adapter = nullptr;
    if (S.rootAdapterView) {
        Il2CppObject* a = F<Il2CppObject*>(root, S.rootAdapterView);
        if (unity::IsAlive(a) && il::IsA(a, S.adapterView)) v.adapter = a;
    }
    return true;
}

// Per adapter class (the generic base's fields and methods, looked up on the
// object's own class): AdapterView.container, .currentlySelectedIndex and
// SelectItem(int).
struct AdapterInfo {
    Il2CppClass* klass = nullptr;
    size_t container = 0, selected = 0, viewHolders = 0;
    const MethodInfo* selectItem = nullptr;
};
AdapterInfo g_adapterInfo;

const AdapterInfo* AdapterInfoFor(Il2CppObject* adapter) {
    Il2CppClass* k = il::ClassOf(adapter);
    if (!k) return nullptr;
    if (g_adapterInfo.klass != k) {
        AdapterInfo a;
        a.klass = k;
        std::string e;
        if (!il::FieldOffsetInHierarchy(k, "container", a.container, &e)) a.container = 0;
        if (!il::FieldOffsetInHierarchy(k, "currentlySelectedIndex", a.selected, &e)) a.selected = 0;
        if (!il::FieldOffsetInHierarchy(k, "viewHolders", a.viewHolders, &e)) a.viewHolders = 0;
        a.selectItem = il::MethodInHierarchy(k, "SelectItem", 1, &e);
        if (!a.selected || !a.selectItem)
            SR2_WARN("ui: options tab: OptionsAdapterView selection API not found ({}); the selected row is not kept", e);
        g_adapterInfo = a;
    }
    return &g_adapterInfo;
}

int SelectedIndex(Il2CppObject* adapter) {
    const AdapterInfo* a = AdapterInfoFor(adapter);
    return a && a->selected ? F<int32_t>(adapter, a->selected) : -1;
}

// Nearest ScrollArea / ScrollRect above the adapter's row container.
Il2CppObject* FindScroller(Il2CppObject* adapter, bool& area) {
    if (!S.transformParent || (!S.scrollArea && !S.scrollRect)) return nullptr;
    const AdapterInfo* a = AdapterInfoFor(adapter);
    Il2CppObject* t = a && a->container ? F<Il2CppObject*>(adapter, a->container) : nullptr;
    if (!unity::IsAlive(t)) t = unity::TransformOf(adapter);
    for (int depth = 0; depth < 12 && unity::IsAlive(t); ++depth) {
        if (S.scrollArea) {
            Il2CppObject* c = unity::GetComponent(t, S.scrollArea);
            if (unity::IsAlive(c) && il::IsA(c, S.scrollArea)) {
                area = true;
                return c;
            }
        }
        if (S.scrollRect) {
            Il2CppObject* c = unity::GetComponent(t, S.scrollRect);
            if (unity::IsAlive(c) && il::IsA(c, S.scrollRect)) {
                area = false;
                return c;
            }
        }
        t = il::Invoke(S.transformParent, t, {});
    }
    return nullptr;
}

Il2CppObject* ScrollRectContent(Il2CppObject* scroller) {
    Il2CppObject* content = il::Invoke(S.scrollRectContent, scroller, {});
    return unity::IsAlive(content) ? content : nullptr;
}

ScrollSave SaveScroll(Il2CppObject* adapter) {
    ScrollSave s;
    Il2CppObject* sc = FindScroller(adapter, s.area);
    if (!sc) return s;
    if (s.area) {
        s.pos = F<float>(sc, S.scrollAreaPos);
        s.valid = std::isfinite(s.pos);
    } else if (Il2CppObject* content = ScrollRectContent(sc)) {
        s.valid = il::UnboxTo(il::Invoke(S.rtGetAnchored, content, {}), s.anchored);
    }
    static bool logged = false;
    if (!logged && s.valid)
        SR2_INFO("ui: options tab: Mods list scrolls with {} '{}'", s.area ? "ScrollArea" : "ScrollRect", unity::GetName(sc));
    logged = logged || s.valid;
    return s;
}

void RestoreScroll(Il2CppObject* adapter, const ScrollSave& s) {
    if (!s.valid) return;
    bool area = false;
    Il2CppObject* sc = FindScroller(adapter, area);
    if (!sc || area != s.area) return;
    if (area) {
        if (std::fabs(F<float>(sc, S.scrollAreaPos) - s.pos) < 0.5f) return;
        float pos = s.pos;
        int32_t instant = 0;  // ScrollTransition 0: jump (no animation); stops a running one
        il::Invoke(S.scrollAreaScrollTo, sc, {&pos, &instant});
        return;
    }
    Il2CppObject* content = ScrollRectContent(sc);
    if (!content) return;
    if (S.scrollRectStop) il::Invoke(S.scrollRectStop, sc, {});
    Vec2 cur;
    if (il::UnboxTo(il::Invoke(S.rtGetAnchored, content, {}), cur) && std::fabs(cur.x - s.anchored.x) < 0.5f &&
        std::fabs(cur.y - s.anchored.y) < 0.5f)
        return;
    Vec2 v = s.anchored;
    il::Invoke(S.rtSetAnchored, content, {&v});
}

// Index of the row with this setting key in the root's freshly bound models.
int ModelIndexForKey(Il2CppObject* root, const std::string& key) {
    if (!S.rootItemModels || key.empty()) return -1;
    Il2CppObject* list = F<Il2CppObject*>(root, S.rootItemModels);
    const size_t n = unity::ListCount(list);
    for (size_t i = 0; i < n; ++i) {
        const Row* r = RowForModel(static_cast<Il2CppObject*>(unity::ListAt(list, i)));
        if (r && RowKey(r->def) == key) return static_cast<int>(i);
    }
    return -1;
}

// ---- UI focus across the rebind ----
// The game sends Cancel (Escape, gamepad B) to the GameObject that
// FocusManager has focused and on up its parents
// (FocusManager.SendCancelActionToFocus -> SRExecuteEvents.ExecuteCancelHierarchy),
// and only while that object is alive: OptionsUIRoot.OnCancel -> Close is
// reached only while the focus is somewhere inside the Options screen.
// Clicking a row's value arrows focuses that row's control. The rebind
// rebinds every row view, and OptionsItemViewHolder.BindData destroys the
// old control (Destroyer.Destroy) and creates a new one, so the focus was
// left on a destroyed object and Back did nothing until the game restarted.
// A vanilla tab swap never hits this because the focus is then on the tab
// strip. So when the focus is inside the row list (or already dead), the
// refresh moves it to the changed row's own button, which is only rebound,
// never destroyed, and checks it again while settling.
Il2CppObject* CurrentFocus() {
    return S.fmGetFocus ? il::Invoke(S.fmGetFocus, nullptr, {}) : nullptr;  // may be a destroyed (Unity-null) object
}

// True if `obj` (GameObject or Component) is `ancestor` or below it.
// Compared by native object (m_CachedPtr), not by managed wrapper.
bool IsUnder(Il2CppObject* obj, Il2CppObject* ancestor) {
    void* want = unity::IsAlive(ancestor) ? CachedPtr(unity::TransformOf(ancestor)) : nullptr;
    if (!want || !unity::IsAlive(obj)) return false;
    Il2CppObject* t = unity::TransformOf(obj);
    for (int depth = 0; depth < 32 && unity::IsAlive(t); ++depth) {
        if (CachedPtr(t) == want) return true;
        t = il::Invoke(S.transformParent, t, {});
    }
    return false;
}

// The live GameObjects that show `model` in the adapter: the row's button
// (`button`, may be null) and the row view itself (`row`).
bool RowFocusTargets(Il2CppObject* adapter, Il2CppObject* model, Il2CppObject*& button, Il2CppObject*& row) {
    button = row = nullptr;
    const AdapterInfo* a = AdapterInfoFor(adapter);
    if (!a || !a->viewHolders || !S.rowModel || !model) return false;
    Il2CppObject* list = F<Il2CppObject*>(adapter, a->viewHolders);
    const size_t n = unity::ListCount(list);
    for (size_t i = 0; i < n; ++i) {
        auto* vh = static_cast<Il2CppObject*>(unity::ListAt(list, i));
        if (!unity::IsAlive(vh) || !il::IsA(vh, S.rowHolder) || F<Il2CppObject*>(vh, S.rowModel) != model) continue;
        Il2CppObject* go = unity::GameObjectOf(vh);
        if (!go || !unity::ActiveInHierarchy(go)) continue;
        row = go;
        Il2CppObject* b = S.rowButton ? F<Il2CppObject*>(vh, S.rowButton) : nullptr;
        if (unity::IsAlive(b)) {
            Il2CppObject* bgo = unity::GameObjectOf(b);
            if (bgo && bgo != go && unity::ActiveInHierarchy(bgo)) button = bgo;
        }
        return true;
    }
    return false;
}

// The focus cannot take Cancel: destroyed, or a deactivated object inside
// this Options screen (a recycled row view).
bool FocusLost(const ModsView& v) {
    Il2CppObject* f = CurrentFocus();
    if (!unity::IsAlive(f)) return true;
    return !unity::ActiveInHierarchy(f) && IsUnder(f, v.root);
}

// Focus the selected row (FocusManager.SetFocus(GameObject, PROGRAMMATIC)).
bool FocusSelectedRow(const ModsView& v) {
    if (!S.fmSetFocus || !v.adapter || !S.rootItemModels) return false;
    const int idx = SelectedIndex(v.adapter);
    Il2CppObject* models = F<Il2CppObject*>(v.root, S.rootItemModels);
    if (idx < 0 || static_cast<size_t>(idx) >= unity::ListCount(models)) return false;
    Il2CppObject *button = nullptr, *row = nullptr;
    if (!RowFocusTargets(v.adapter, static_cast<Il2CppObject*>(unity::ListAt(models, idx)), button, row)) return false;
    // SetFocus may refuse an object that is not focusable (it returns what
    // it focused): the button first, then the row view.
    for (Il2CppObject* target : {button, row}) {
        if (!target) continue;
        int32_t cause = 0;  // InteractionCause.PROGRAMMATIC
        std::string err;
        il::Invoke(S.fmSetFocus, nullptr, {target, &cause}, &err);
        if (Faulted()) return false;
        if (!err.empty()) {
            static int warned = 0;
            if (warned++ < 3) SR2_WARN("ui: options tab: moving the UI focus to row {} failed: {}", idx, err);
            continue;
        }
        Il2CppObject* now = CurrentFocus();
        if (!unity::IsAlive(now) || !unity::ActiveInHierarchy(now) || !IsUnder(now, v.root)) continue;
        static bool logged = false;
        if (!logged)
            SR2_INFO("ui: options tab: UI focus put on the changed row ('{}'); its old value control is rebuilt",
                     unity::GetName(now));
        logged = true;
        return true;
    }
    static int warned = 0;
    if (warned++ < 3) SR2_WARN("ui: options tab: the UI focus could not be put on row {}", idx);
    return false;
}

// After a refresh: for a few ticks re-apply the scroll offset (while the
// changed row stays selected) and keep the UI focus usable. Every tick looks
// the root up again; nothing is held across ticks but plain values.
void SettleAfterRefresh() {
    if (Faulted() || g_settleTicks <= 0) return;
    Guarded("Mods tab refresh (settle)", [&] {
        Stage("refresh: settle");
        ModsView v;
        if (!FindModsView(v) || !v.adapter) {  // the tab or the menu is gone
            g_settleTicks = 0;
            return;
        }
        if (SelectedIndex(v.adapter) != g_settleIndex) g_settleScroll.valid = false;  // the user moved on
        if (g_settleScroll.valid) RestoreScroll(v.adapter, g_settleScroll);
        if (g_settleFocus && FocusLost(v)) {
            Stage("refresh: settle (UI focus)");
            if (FocusSelectedRow(v)) SR2_INFO("ui: options tab: UI focus was lost after the Mods tab refresh; restored");
        }
        Stage("-");
    });
    if (--g_settleTicks > 0 && !Faulted()) core::main_thread::Post([] { SettleAfterRefresh(); });
}

void RefreshModsTab() {
    g_refreshQueued = false;
    const std::string key = std::move(g_refreshKey);
    g_refreshKey.clear();
    if (Faulted()) return;
    Guarded("Mods tab refresh", [&] {
        Stage("refresh: find OptionsUIRoot");
        if (!S.bindItemCategoryMi) return;
        ModsView v;
        std::string shown = key;
        std::replace(shown.begin(), shown.end(), '\x1f', '.');
        if (!FindModsView(v)) {
            static int warned = 0;
            if (warned++ < 5)
                SR2_WARN("ui: options tab: {} changed but no Options menu shows the Mods tab; rows update when the tab "
                         "is reopened ({})",
                         shown, DescribeRoots());
            return;
        }
        Stage("refresh: save scroll position");
        const ScrollSave scroll = v.adapter ? SaveScroll(v.adapter) : ScrollSave{};
        // Focus inside the row list (on a control the rebind destroys) or
        // already gone: it is put on the changed row after the rebind.
        Stage("refresh: check UI focus");
        bool keepFocus = false;
        if (S.fmGetFocus && v.adapter) {
            Il2CppObject* f = CurrentFocus();
            const AdapterInfo* a = AdapterInfoFor(v.adapter);
            Il2CppObject* container = a && a->container ? F<Il2CppObject*>(v.adapter, a->container) : nullptr;
            keepFocus = !unity::IsAlive(f) || IsUnder(f, v.adapter) || IsUnder(f, container);
        }
        Stage("refresh: OptionsUIRoot.BindItemCategory(Mods)");
        std::string err;
        il::Invoke(S.bindItemCategoryMi, v.root, {g_category}, &err);
        if (!err.empty()) {
            SR2_WARN("ui: options tab: refreshing the Mods tab failed: {}", err);
            return;
        }
        {
            static int logged = 0;
            size_t shownRows = 0;
            for (const Row& r : g_rows) shownRows += r.shown ? 1 : 0;
            if (logged++ < 10)
                SR2_INFO("ui: options tab: Mods tab rebound after {} changed ({} of {} rows listed)", shown, shownRows,
                         g_rows.size());
        }
        if (Faulted() || !v.adapter || !unity::IsAlive(v.adapter)) return;
        Stage("refresh: re-select the changed row");
        const int index = ModelIndexForKey(v.root, key);
        const AdapterInfo* a = AdapterInfoFor(v.adapter);
        if (index > 0 && a && a->selectItem && SelectedIndex(v.adapter) != index) {
            int32_t i = index;
            il::Invoke(a->selectItem, v.adapter, {&i}, &err);
            if (!err.empty()) SR2_WARN("ui: options tab: re-selecting row {} failed: {}", index, err);
        }
        if (Faulted()) return;
        if (keepFocus) {
            Stage("refresh: move UI focus to the changed row");
            FocusSelectedRow(v);
            if (Faulted()) return;
        }
        Stage("refresh: restore scroll position");
        RestoreScroll(v.adapter, scroll);
        // The old row controls are destroyed at the end of this frame and the
        // focus change may scroll a frame later: settle for a few ticks.
        if (scroll.valid || keepFocus) {
            g_settleScroll = scroll;
            g_settleIndex = SelectedIndex(v.adapter);
            g_settleFocus = keepFocus;
            if (g_settleTicks <= 0) core::main_thread::Post([] { SettleAfterRefresh(); });
            g_settleTicks = kSettleTicks;
        }
        Stage("-");
    });
}

void ScheduleRefresh(const Row& changed) {
    if (!S.bindItemCategoryMi || !S.rootCategories || !S.rootCategoryIndex) return;
    g_refreshKey = RowKey(changed.def);  // the latest change wins if one is already queued
    if (g_refreshQueued || !core::main_thread::Ready()) return;
    g_refreshQueued = true;
    core::main_thread::Post([] { RefreshModsTab(); });
}

void HkToggleApply(Il2CppObject* self, int32_t index, const MethodInfo* mi) {
    bool ours = false;
    Guarded("Toggle.ApplyPresetSelection", [&] {
        auto it = g_rowByDef.find(self);
        if (it == g_rowByDef.end()) return;
        ours = true;
        Row& r = g_rows[it->second];
        const bool on = index == ToggleTrueIndex(r);
        F<bool>(r.defObj, S.togDefault) = on;
        const bool was = settings::GetBool(r.def.module, r.def.key);
        Write(r.def, on ? 1.0 : 0.0);
        if (r.isParent && was != on) ScheduleRefresh(r);
    });
    // Game definitions always reach the original. Ours never do (even in fault
    // mode): the original would write our private, possibly null, target.
    if (!ours && g_rowByDef.find(self) == g_rowByDef.end()) o_toggleApply(self, index, mi);
}

void HkRangeApply(Il2CppObject* self, int32_t index, const MethodInfo* mi) {
    bool ours = false;
    Guarded("Range.ApplyPresetSelection", [&] {
        auto it = g_rowByDef.find(self);
        if (it == g_rowByDef.end()) return;
        ours = true;
        Row& r = g_rows[it->second];
        double v = 0;
        RangeValue(r, index, v);
        v = Snap(r.def, v);
        F<float>(r.defObj, S.rngDefault) = static_cast<float>(v);
        const double was = settings::Get(r.def.module, r.def.key);
        Write(r.def, v);
        if (r.isParent && std::fabs(settings::Get(r.def.module, r.def.key) - was) > 1e-9) ScheduleRefresh(r);  // Enum parents
    });
    // Game definitions always reach the original. Ours never do (even in fault
    // mode): the original would write our private, possibly null, target.
    if (!ours && g_rowByDef.find(self) == g_rowByDef.end()) o_rangeApply(self, index, mi);
}

void HkRowBind(Il2CppObject* self, Il2CppObject* data, const MethodInfo* mi) {
    if (Faulted()) return o_rowBind(self, data, mi);
    Guarded("OptionsItemViewHolder.BindData/pre", [&] { RestoreText(F<Il2CppObject*>(self, S.rowLabel)); });
    o_rowBind(self, data, mi);
    Guarded("OptionsItemViewHolder.BindData", [&] {
        if (const Row* r = RowForModel(data)) OverrideText(F<Il2CppObject*>(self, S.rowLabel), r->label);
    });
}

void HkDetailsBind(Il2CppObject* self, Il2CppObject* data, const MethodInfo* mi) {
    if (Faulted()) return o_detailsBind(self, data, mi);
    Guarded("OptionsDetailsView.BindData/pre", [&] {
        RestoreText(F<Il2CppObject*>(self, S.detailsHeader));
        RestoreText(F<Il2CppObject*>(self, S.detailsDetails));
    });
    o_detailsBind(self, data, mi);
    Guarded("OptionsDetailsView.BindData", [&] {
        if (const Row* r = RowForModel(data)) {
            OverrideText(F<Il2CppObject*>(self, S.detailsHeader), r->label);
            OverrideText(F<Il2CppObject*>(self, S.detailsDetails), DetailsText(*r));
        }
    });
}

void HkSelectorChanged(Il2CppObject* self, int32_t selection, const MethodInfo* mi) {
    if (Faulted()) return o_selectorChanged(self, selection, mi);
    Guarded("OptionsPresetSelector.OnSelectionChanged/pre", [&] { RestoreText(F<Il2CppObject*>(self, S.selectorText)); });
    o_selectorChanged(self, selection, mi);
    Guarded("OptionsPresetSelector.OnSelectionChanged", [&] {
        Il2CppObject* model = F<Il2CppObject*>(self, S.selectorModel);
        const Row* cr = RowForModel(model);
        if (!cr) return;
        Row& r = g_rows[g_rowByDef[cr->defObj]];
        OverrideText(F<Il2CppObject*>(self, S.selectorText), ValueText(r, selection));
    });
}

void HkTabBind(Il2CppObject* self, Il2CppObject* data, const MethodInfo* mi) {
    if (Faulted()) return o_tabBind(self, data, mi);
    Guarded("CategoryTabViewHolder.BindData/pre", [&] { RestoreText(F<Il2CppObject*>(self, S.tabTitleEvent)); });
    o_tabBind(self, data, mi);
    Guarded("CategoryTabViewHolder.BindData", [&] {
        if (!data || !g_categoryTitle || F<Il2CppObject*>(data, S.vmTitle) != g_categoryTitle) return;
        OverrideText(F<Il2CppObject*>(self, S.tabTitleEvent), kTabTitle);
    });
}

// Hot path for every localized text in the game: a map lookup, nothing else
// for game LSEs. No game calls here except the original and il2cpp_string_new.
Il2CppString* OwnedTextFor(Il2CppObject* self) {
    auto it = g_ownedText.find(self);
    if (it == g_ownedText.end()) {
        // Module-registered LocalizedString instances (core/text_override.h),
        // matched by the event's m_StringReference identity.
        if (S.lseStringRef && core::text_override::Any()) {
            std::string text;
            if (core::text_override::Lookup(F<Il2CppObject*>(self, S.lseStringRef), text))
                return unity::NewString(text);
        }
        return nullptr;
    }
    if (!it->second.cachedPtr || it->second.cachedPtr != CachedPtr(self)) {
        g_ownedText.erase(it);  // destroyed / address reused: not ours any more
        return nullptr;
    }
    return unity::NewString(it->second.text);
}

template <class F>
__declspec(noinline) Il2CppString* LookupCpp(F& f) noexcept {
    try {
        return f();
    } catch (...) {
        return nullptr;
    }
}

Il2CppString* OwnedTextGuarded(Il2CppObject* self) {
    auto f = [&] { return OwnedTextFor(self); };
    __try {
        return LookupCpp(f);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        OnSeh("LocalizeStringEvent.UpdateString", GetExceptionCode());
        return nullptr;
    }
}

void HkUpdateString(Il2CppObject* self, Il2CppString* value, const MethodInfo* mi) {
    if (!Faulted() && (!g_ownedText.empty() || core::text_override::Any())) {
        if (Il2CppString* ours = OwnedTextGuarded(self)) value = ours;
    }
    o_updateString(self, value, mi);  // game code: always runs, outside the guard
}

// ----------------------------------------------------------- resolving ----
struct Resolve {
    std::string err;
    bool ok = true;

    Il2CppClass* C(const char* image, const char* ns, const char* name) {
        if (!ok) return nullptr;
        std::string e;
        Il2CppClass* k = il::Class(image, ns, name, &e);
        if (!k) Fail(std::format("class {}.{} not found ({})", ns, name, e));
        return k;
    }
    void Fo(Il2CppClass* k, const char* field, size_t& out) {
        if (!ok) return;
        std::string e;
        if (!il::FieldOffsetInHierarchy(k, field, out, &e)) Fail(std::format("field {} not found ({})", field, e));
    }
    const MethodInfo* M(Il2CppClass* k, const char* method, int argc) {
        if (!ok) return nullptr;
        std::string e;
        const MethodInfo* m = il::MethodInHierarchy(k, method, argc, &e);
        if (!m) Fail(std::format("method {}({}) not found ({})", method, argc, e));
        return m;
    }
    // Hook target: must be declared on `k` itself (not inherited / folded).
    void* P(Il2CppClass* k, const char* method, int argc) {
        if (!ok) return nullptr;
        std::string e;
        const MethodInfo* m = il::Method(k, method, argc, &e);
        void* p = m ? il::MethodPointer(m, &e) : nullptr;
        if (!p) Fail(std::format("hook target {}({}) not found ({})", method, argc, e));
        return p;
    }
    void Fail(std::string why) {
        if (ok) err = std::move(why);
        ok = false;
    }
};

bool ResolveAll(std::string& err) {
    Resolve r;
    S.uiRoot = r.C(kAsm, kNsUiOptions, "OptionsUIRoot");
    S.viewModel = r.C(kAsm, kNsUiOptions, "OptionsViewModel");
    S.config = r.C(kAsm, kNsOptions, "OptionsConfiguration");
    S.category = r.C(kAsm, kNsOptions, "OptionsItemCategory");
    S.itemDef = r.C(kAsm, kNsOptions, "OptionsItemDefinition");
    S.presetDef = r.C(kAsm, kNsOptions, "PresetOptionsItemDefinition");
    S.toggleDef = r.C(kAsm, kNsOptions, "ToggleOptionDefinition");
    S.rangeDef = r.C(kAsm, kNsOptions, "ScriptedValueRangeOptionDefinition");
    S.presetSet = r.C(kAsm, kNsOptions, "OptionPresetSet");
    S.itemModel = r.C(kAsm, kNsDataModel, "OptionsItemModel");
    S.presetModel = r.C(kAsm, kNsDataModel, "PresetOptionsItemModel");
    S.rowHolder = r.C(kAsm, kNsUiOptions, "OptionsItemViewHolder");
    S.detailsView = r.C(kAsm, kNsUiOptions, "OptionsDetailsView");
    S.presetSelector = r.C(kAsm, kNsUiOptions, "OptionsPresetSelector");
    S.presetSelectorBase = r.C(kAsm, kNsUiOptions, "OptionsPresetSelectorBase");
    S.tabHolder = r.C(kAsm, kNsAdapter, "CategoryTabViewHolder");
    S.categoryVm = r.C(kAsm, kNsAdapter, "CategoryViewModel");
    S.tmpText = r.C("Unity.TextMeshPro", "TMPro", "TMP_Text");
    S.behaviour = r.C("UnityEngine.CoreModule", "UnityEngine", "Behaviour");
    S.lseClass = r.C("Unity.Localization", "UnityEngine.Localization.Components", "LocalizeStringEvent");
    S.unityObject = r.C("UnityEngine.CoreModule", "UnityEngine", "Object");
    // Optional: a fresh LocalizedString for the tab title.
    {
        std::string e;
        S.localizedString = il::Class("Unity.Localization", "UnityEngine.Localization", "LocalizedString", &e);
        if (S.localizedString) S.lsCtor = il::Method(S.localizedString, ".ctor", 0, &e);
    }

    r.Fo(S.config, "items", S.cfgItems);
    r.Fo(S.category, "items", S.catItems);
    r.Fo(S.category, "_title", S.catTitle);
    r.Fo(S.category, "_activeCondition", S.catActiveCondition);
    r.Fo(S.category, "_showRebindButton", S.catShowRebind);
    r.Fo(S.itemDef, "_referenceId", S.defReferenceId);
    r.Fo(S.itemDef, "_applyImmediately", S.defApplyImmediately);
    r.Fo(S.itemDef, "_requireConfirmation", S.defRequireConfirmation);
    r.Fo(S.itemDef, "_showTutorialDisclaimer", S.defShowTutorial);
    r.Fo(S.itemDef, "_indent", S.defIndent);
    r.Fo(S.itemDef, "_showCondition", S.defShowCondition);
    r.Fo(S.presetDef, "_wrapAround", S.presetWrap);
    r.Fo(S.presetDef, "_optionsItemModels", S.presetModels);
    r.Fo(S.presetDef, "SupportedInputDeviceAssets", S.presetDevices);
    r.Fo(S.presetDef, "SupportedPlatforms", S.presetPlatforms);
    r.Fo(S.toggleDef, "TargetBool", S.togTarget);
    r.Fo(S.toggleDef, "_defaultValue", S.togDefault);
    r.Fo(S.toggleDef, "_positiveOption", S.togPositive);
    r.Fo(S.toggleDef, "_positiveReferenceId", S.togPositiveRef);
    r.Fo(S.rangeDef, "_targetValue", S.rngTarget);
    r.Fo(S.rangeDef, "_minValue", S.rngMin);
    r.Fo(S.rangeDef, "_maxValue", S.rngMax);
    r.Fo(S.rangeDef, "_valueStep", S.rngStep);
    r.Fo(S.rangeDef, "_normalizeOutputValue", S.rngNormalize);
    r.Fo(S.rangeDef, "_defaultValue", S.rngDefault);
    r.Fo(S.itemModel, "optionsItemDefinition", S.modelDef);
    r.Fo(S.presetModel, "_presets", S.presetModelPresets);
    r.Fo(S.presetSet, "_optionPresets", S.presetSetList);
    r.Fo(S.rowHolder, "label", S.rowLabel);
    r.Fo(S.detailsView, "_header", S.detailsHeader);
    r.Fo(S.detailsView, "_details", S.detailsDetails);
    r.Fo(S.presetSelector, "_currentSelectionText", S.selectorText);
    r.Fo(S.presetSelectorBase, "optionsItemModel", S.selectorModel);
    r.Fo(S.tabHolder, "titleTextString", S.tabTitleEvent);
    r.Fo(S.tabHolder, "titleText", S.tabTitleText);
    r.Fo(S.categoryVm, "title", S.vmTitle);
    r.Fo(S.category, "_icon", S.catIcon);
    {  // optional extras: never fatal
        std::string e;
        if (Il2CppClass* about = il::Class(kAsm, kNsOptions, "OptionsAboutCategory", &e))
            il::FieldOffset(about, "_icon", S.aboutIcon, &e);
        if (S.uiRoot && il::FieldOffset(S.uiRoot, "optionsItemModels", S.rootItemModels, &e) &&
            il::FieldOffset(S.uiRoot, "optionsDetailsView", S.rootDetailsView, &e)) {
            if (const MethodInfo* m = il::Method(S.uiRoot, "BindItemCategory", 1, &e)) {
                S.bindItemCategory = il::MethodPointer(m, &e);
                if (S.bindItemCategory) S.bindItemCategoryMi = m;
            }
        }
        if (!S.bindItemCategory) SR2_WARN("ui: options tab: empty-tab details text unavailable ({})", e);
        // Live refresh of child rows (Def::parent): which tab the root shows.
        if (!S.uiRoot || !il::FieldOffset(S.uiRoot, "categories", S.rootCategories, &e) ||
            !il::FieldOffset(S.uiRoot, "currentCategoryIndex", S.rootCategoryIndex, &e)) {
            S.rootCategories = S.rootCategoryIndex = 0;
            SR2_WARN("ui: options tab: live show/hide of child rows unavailable ({}); they update when the tab is "
                     "reopened", e);
        }
        // Keeping the selected row and the scroll position across that refresh.
        if (S.uiRoot && il::FieldOffset(S.uiRoot, "optionsAdapterView", S.rootAdapterView, &e))
            S.adapterView = il::Class(kAsm, kNsUiOptions, "OptionsAdapterView", &e);
        if (!S.adapterView) {
            S.rootAdapterView = 0;
            SR2_WARN("ui: options tab: row selection is not kept across a child-row refresh ({})", e);
        }
        S.scrollArea = il::Class(kAsm, "MonomiPark.SlimeRancher.UI.Framework.CommonControls", "ScrollArea", &e);
        if (S.scrollArea && (!il::FieldOffset(S.scrollArea, "_scrollPosition", S.scrollAreaPos, &e) ||
                             !(S.scrollAreaScrollTo = il::Method(S.scrollArea, "ScrollTo", 2, &e))))
            S.scrollArea = nullptr;
        S.scrollRect = il::Class("UnityEngine.UI", "UnityEngine.UI", "ScrollRect", &e);
        if (S.scrollRect) {
            S.scrollRectContent = il::Method(S.scrollRect, "get_content", 0, &e);
            S.scrollRectStop = il::Method(S.scrollRect, "StopMovement", 0, &e);
            Il2CppClass* rt = il::Class("UnityEngine.CoreModule", "UnityEngine", "RectTransform", &e);
            if (rt) {
                S.rtGetAnchored = il::Method(rt, "get_anchoredPosition", 0, &e);
                S.rtSetAnchored = il::Method(rt, "set_anchoredPosition", 1, &e);
            }
            if (!S.scrollRectContent || !S.rtGetAnchored || !S.rtSetAnchored) S.scrollRect = nullptr;
        }
        if (Il2CppClass* tr = unity::TransformClass()) S.transformParent = il::Method(tr, "get_parent", 0, &e);
        if (!S.transformParent || (!S.scrollArea && !S.scrollRect)) {
            S.scrollArea = S.scrollRect = nullptr;
            SR2_WARN("ui: options tab: scroll position is not kept across a child-row refresh ({})", e);
        }
        // The game's UI focus (Back / Escape / gamepad B go to the focused
        // object) must not be left on a row control the rebind destroys.
        if (Il2CppClass* fm = il::Class(kAsm, "MonomiPark.SlimeRancher.UI.Framework.Interaction.Focus", "FocusManager", &e)) {
            auto& res = il2cpp::Resolver::Get();
            S.fmGetFocus = res.MethodByParams(fm, "get_CurrentFocus", {}, &e);
            S.fmSetFocus = res.MethodByParams(
                fm, "SetFocus", {"UnityEngine.GameObject", "MonomiPark.SlimeRancher.UI.Framework.Interaction.InteractionCause"},
                &e);
        }
        if (!S.rowHolder || !il::FieldOffsetInHierarchy(S.rowHolder, "_model", S.rowModel, &e) ||
            !il::FieldOffsetInHierarchy(S.rowHolder, "button", S.rowButton, &e))
            S.rowModel = S.rowButton = 0;
        if (!S.fmGetFocus || !S.fmSetFocus || !S.rowModel || !S.transformParent) {
            S.fmGetFocus = S.fmSetFocus = nullptr;
            SR2_WARN("ui: options tab: UI focus is not moved off rebuilt row controls on a child-row refresh ({})", e);
        }
    }

    S.modelCtor = r.M(S.presetModel, ".ctor", 1);
    S.rebuildOptions = r.M(S.presetModel, "RebuildOptions", 0);
    S.getValueForIndex = r.M(S.rangeDef, "GetValueForIndex", 1);
    S.toggleGetPresets = r.M(S.toggleDef, "GetOptionsPresets", 0);
    S.setEnabled = r.M(S.behaviour, "set_enabled", 1);
    S.tmpSetText = r.M(S.tmpText, "set_text", 1);
    S.lseUpdateString = r.M(S.lseClass, "UpdateString", 1);
    r.Fo(S.unityObject, "m_CachedPtr", S.objCachedPtr);
    S.setHideFlags = r.M(S.unityObject, "set_hideFlags", 1);

    S.bindCategories = r.P(S.uiRoot, "BindCategories", 2);
    S.getItems = r.P(S.viewModel, "GetOptionsItemsForCategory", 3);
    S.toggleApply = r.P(S.toggleDef, "ApplyPresetSelection", 1);
    S.rangeApply = r.P(S.rangeDef, "ApplyPresetSelection", 1);
    S.rowBind = r.P(S.rowHolder, "BindData", 1);
    S.detailsBind = r.P(S.detailsView, "BindData", 1);
    S.selectorChanged = r.P(S.presetSelector, "OnSelectionChanged", 1);
    S.updateString = r.P(S.lseClass, "UpdateString", 1);
    {  // optional: module LocalizedString overrides (core/text_override.h)
        std::string e;
        if (!il::FieldOffsetInHierarchy(S.lseClass, "m_StringReference", S.lseStringRef, &e)) {
            S.lseStringRef = 0;
            SR2_WARN("ui: LocalizeStringEvent.m_StringReference not found ({}); module text overrides off", e);
        }
    }
    S.tabBind = r.P(S.tabHolder, "BindData", 1);

    if (r.ok) {
        // IL2CPP folds identical method bodies; two hooks on one address would
        // mean the dump assumptions no longer hold for this build.
        void* targets[] = {S.bindCategories, S.getItems, S.toggleApply, S.rangeApply,
                           S.rowBind, S.detailsBind, S.selectorChanged, S.tabBind, S.updateString};
        for (size_t i = 0; i < std::size(targets) && r.ok; ++i)
            for (size_t j = i + 1; j < std::size(targets); ++j)
                if (targets[i] == targets[j]) {
                    r.Fail(std::format("hook targets #{} and #{} share one address (folded method)", i, j));
                    break;
                }
    }
    if (r.ok && !il::Export("il2cpp_array_new")) r.Fail("il2cpp_array_new not exported");
    if (r.ok && !il::Export("il2cpp_object_unbox")) r.Fail("il2cpp_object_unbox not exported");
    err = r.err;
    return r.ok;
}

struct HookSpec {
    const char* name;
    void* target;
    void* detour;
    void** orig;
};

}  // namespace

bool StartOptionsTab() {
    static bool started = false;
    if (started) return true;
    RegisterUiSettings();
    SetOptionsTabStatus("starting");

    std::string err;
    if (!ResolveAll(err)) {
        SR2_ERROR("ui: options tab unavailable: {}", err);
        SetOptionsTabStatus("unavailable: " + err);
        return false;
    }

    const HookSpec hooks[] = {
        {"ui.options_bind_categories", S.bindCategories, reinterpret_cast<void*>(&HkBindCategories), reinterpret_cast<void**>(&o_bindCategories)},
        {"ui.options_get_items", S.getItems, reinterpret_cast<void*>(&HkGetItems), reinterpret_cast<void**>(&o_getItems)},
        {"ui.options_toggle_apply", S.toggleApply, reinterpret_cast<void*>(&HkToggleApply), reinterpret_cast<void**>(&o_toggleApply)},
        {"ui.options_range_apply", S.rangeApply, reinterpret_cast<void*>(&HkRangeApply), reinterpret_cast<void**>(&o_rangeApply)},
        {"ui.options_row_bind", S.rowBind, reinterpret_cast<void*>(&HkRowBind), reinterpret_cast<void**>(&o_rowBind)},
        {"ui.options_details_bind", S.detailsBind, reinterpret_cast<void*>(&HkDetailsBind), reinterpret_cast<void**>(&o_detailsBind)},
        {"ui.options_selector_changed", S.selectorChanged, reinterpret_cast<void*>(&HkSelectorChanged), reinterpret_cast<void**>(&o_selectorChanged)},
        {"ui.options_lse_update_string", S.updateString, reinterpret_cast<void*>(&HkUpdateString), reinterpret_cast<void**>(&o_updateString)},
        {"ui.options_tab_bind", S.tabBind, reinterpret_cast<void*>(&HkTabBind), reinterpret_cast<void**>(&o_tabBind)},
    };

    auto& hm = HookManager::Get();
    if (!hm.Initialize()) {
        SR2_ERROR("ui: options tab unavailable: hook manager not initialised");
        SetOptionsTabStatus("unavailable: hook manager");
        return false;
    }
    // Create all (disabled) first; enable only if every one was created.
    size_t created = 0;
    for (const auto& h : hooks) {
        if (!hm.Create(kUiModule, h.name, h.target, h.detour, h.orig, &err)) break;
        ++created;
    }
    bool ok = created == std::size(hooks);
    if (ok) g_active = true;  // detours must be live before the first call can reach them
    for (size_t i = 0; ok && i < std::size(hooks); ++i) {
        if (!hm.Enable(hooks[i].name, &err)) ok = false;
    }
    if (!ok) {
        g_active = false;
        for (size_t i = 0; i < created; ++i) hm.Remove(hooks[i].name);
        SR2_ERROR("ui: options tab unavailable: hook installation failed: {}", err);
        SetOptionsTabStatus("unavailable: hook installation failed");
        return false;
    }
    for (const auto& h : hooks)
        SR2_DEBUG("ui: hook {} at RVA 0x{:X}", h.name, il2cpp::Resolver::Rva(h.target));

    if (S.bindItemCategory) {
        std::string e;
        if (!hm.Create(kUiModule, "ui.options_bind_item_category", S.bindItemCategory,
                       reinterpret_cast<void*>(&HkBindItemCategory), reinterpret_cast<void**>(&o_bindItemCategory), &e) ||
            !hm.Enable("ui.options_bind_item_category", &e)) {
            hm.Remove("ui.options_bind_item_category");
            SR2_WARN("ui: options tab: optional hook BindItemCategory failed: {}", e);
        }
    }

    started = true;
    if (S.lseStringRef) core::text_override::SetSinkActive(true);
    SetOptionsTabStatus("waiting for the Options menu");
    SR2_INFO("ui: options tab hooks installed ({}); the 'Mods' tab appears when the Options menu opens", std::size(hooks));
    return true;
}

}  // namespace sr2::ui
