// drone_price_limit: Quantum Drone station price-limit action + dialog (see station_dialog.h).
// SR2 1.3.0, everything by name; RVAs for reference only.
//
// Research (Cpp2IL / Il2CppInspector dump, IDA, the menu scene bundle
// uiscenes_..._menulayer_dronestationmenu.unity read with UnityPy):
//   * The station menu is the scene MenuLayer_DroneStationMenu: DroneUIDisplay (1470 x 980, centred)
//     > DroneMenuRoot (DroneMenuUI) > ContentContainer > TopViewSwitcher > ProgrammingPage
//     (DroneProgramMenu) > PageContainer (RectMask2D) > ProgramViewSwitcher > OptionSubPage >
//     ProgramSettings > ButtonRow > Target / Source / DestinationProgramOption
//     (DroneProgramOptionButton, 445 x 380). DroneProgramMenu.SetViewModel(DroneProgramViewModel)
//     (0x81BE10) binds the page whenever the menu opens for a station.
//   * The "Change" action: each option has PromptContainer (= DroneProgramOptionButton._promptContainer,
//     inactive in the prefab): a pill (SDFRoundedRectangle, cyan (0.32, 0.78, 0.84), radius 50, anchored
//     bottom-left at (132, 0), 264 x 56, EncapsulateChildren minimum width 264, HorizontalLayout) with
//     InputEventDisplay (InputIconGroup with 4 InputIcons: Button image / Key (Border, Background, Text,
//     Image) / DoubleTap) + InputLabel (TMP + LocalizeStringEvent "Change", UI table key
//     263830960522432512). The OptionButton's FocusEventTriggers SetActive(true / false) the pill on
//     focus. InputIcon.SetKey (0x7C6FE0) = buttonRoot off, keyRoot on, keyText enabled / keyImage
//     disabled, keyLocalizeStringEvent disabled, keyText.text = HumanReadableControl.
//     We clone the destination's pill next to it, disable the clone's InputEventDisplay and
//     LocalizeStringEvent, and draw the C key and our label the same way.
//   * The page's legend (DroneProgramInputLegendConfiguration): Start Program, Clear All, Close. In the
//     PlayControls "Paused" map the C key is only MapZoomIn (map screen), so C is free here; the pad
//     right stick click only marks Pedia entries as seen.
//   * Destination: DroneProgramViewModel._destinationForTarget._currentSelection
//     (DroneDestinationOptionEntry).get_Type() (0x815070) = DroneTaskSinkType, PLORT_MARKET = 5. The
//     station: DroneProgramViewModel._station (DroneStationGadgetModel) .actorId (IdentifiableModel,
//     saved as PlacedGadgetV06.ActorId -> stable per save).
//   * Input while the dialog is open: menu navigation / submit / cancel all go through the static
//     FocusManager (FocusManagerBehaviour's move / submit / cancel actions call
//     NavigateInDirection(MoveDirection, InteractionCause, AxisEventData) (0x7926D0),
//     SendSubmitActionToFocus(InputAction) (0x793BE0), SendCancelActionToFocus(InputAction) (0x7939A0)).
//     We detour the three: while the dialog is open they drive the dialog and report "handled";
//     otherwise one atomic load and the original. The page's own hotkeys (Start Program / Clear All)
//     come from InputEvents bound to DroneProgramMenu.HandleActivateDrone / HandleClearAllOptions:
//     detoured to do nothing while the dialog is open.
//   * Cheat sheet: DroneProgramMenu._marketConfig (MarketUIConfiguration._plorts, PlortEntry.IdentType)
//     is the market terminal's plort order (MarketUI uses the same asset type); the ranch terminal's
//     PriceEntry shows item icon + coin icon + amount, so a cell is icon, name, coin + base price. Coin =
//     LookupDirector._currencyList._currencies[0]._icon (the default currency, Newbucks).
//
// Pointer input without managed delegates: every clickable part (our prompt, mode buttons, Close, the
// dimmed backdrop) carries a UnityEngine.UI.Toggle (navigation None, no transition) that we poll and
// reset. The dialog root has its own Canvas (sorted above the menu) + GraphicRaycaster.
//
// Lifetime: the prompt and the dialog are children of the menu, so they die with it; we hold GC handles
// to everything we touch across frames and free them only from posted main-thread jobs.

#include "modules/drone_price_limit/station_dialog.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <format>
#include <initializer_list>
#include <string_view>
#include <vector>

#include "core/detour_guard.h"
#include "core/il2cpp_api.h"
#include "core/log.h"
#include "core/main_thread.h"
#include "core/module.h"
#include "core/resolver.h"
#include "core/unity.h"
#include "modules/custom_upgrades/plot_menu_slider.h"

namespace sr2::modules::drone_price_limit {

std::string Describe(const StationLimit& l) {
    if (!l.Active()) return "no limit";
    if (l.mode == LimitMode::kFraction) return std::format("{}% of the base price", l.value);
    return std::format("{} newbucks", l.value);
}

std::string Serialize(const StationLimit& l) {
    if (!l.Active()) return {};
    return std::format("{}:{}", l.mode == LimitMode::kFraction ? "fraction" : "price", l.value);
}

bool Parse(const std::string& s, StationLimit& out) {
    out = {};
    if (s.empty()) return true;
    const size_t c = s.find(':');
    if (c == std::string::npos) return false;
    const std::string kind = s.substr(0, c);
    int v = 0;
    try {
        v = std::stoi(s.substr(c + 1));
    } catch (...) {
        return false;
    }
    if (kind == "price") out.mode = LimitMode::kPrice;
    else if (kind == "fraction") out.mode = LimitMode::kFraction;
    else return false;
    out.value = std::clamp(v, 0, 1000000);
    return true;
}

namespace {

namespace unity = sr2::core::unity;
namespace main_thread = sr2::core::main_thread;
namespace sui = sr2::modules::custom_upgrades::ui;
using sr2::il2cpp::api;
using sr2::il2cpp::Resolver;

constexpr const char* kImage = "Assembly-CSharp";
constexpr const char* kCore = "UnityEngine.CoreModule";
constexpr const char* kUi = "UnityEngine.UI";
constexpr const char* kTmp = "Unity.TextMeshPro";
constexpr const char* kInput = "Unity.InputSystem";
constexpr const char* kDroneUiNs = "MonomiPark.SlimeRancher.UI.Drone";
constexpr const char* kDroneDataNs = "MonomiPark.SlimeRancher.UI.Drone.Data";

constexpr int kSinkPlortMarket = 5;   // DroneTaskSinkType.PLORT_MARKET
constexpr int kMoveLeft = 0, kMoveUp = 1, kMoveRight = 2, kMoveDown = 3;  // EventSystems.MoveDirection
constexpr int kAlignLeft = 513, kAlignCenter = 514, kAlignRight = 516, kAlignTopLeft = 257;  // TMP
constexpr int kStateFrames = 10;      // menu state (destination / station) re-read every N frames
constexpr int kReapplyFrames = 4;     // re-assert the cloned prompt's key / label for a few frames

struct Vec2 {
    float x, y;
};
struct Color {
    float r, g, b, a;
};

// The drone menu's palette (scene data).
constexpr Color kNavy{0.267f, 0.325f, 0.478f, 1.0f};      // text
constexpr Color kWhite{1.0f, 1.0f, 1.0f, 1.0f};           // content panel
constexpr Color kLavender{0.737f, 0.686f, 0.824f, 1.0f};  // content border
constexpr Color kBacker{0.875f, 0.902f, 0.945f, 1.0f};    // inner boxes
constexpr Color kTeal{0.063f, 0.675f, 0.757f, 1.0f};      // highlight / selected
constexpr Color kCyan{0.322f, 0.776f, 0.839f, 1.0f};      // prompt pill

// Dialog layout (DroneUIDisplay units).
constexpr float kPad = 40.0f;
constexpr float kPanelW = 1300.0f;
constexpr float kRowH = 56.0f;
constexpr float kCellH = 62.0f;
constexpr float kCellGap = 6.0f;

std::atomic<bool> g_open{false};  // dialog open: the FocusManager detours route input to it

}  // namespace

struct StationUi::Impl {
    const char* module = "drone_price_limit";
    DialogHost host;

    // ---- classes ----
    Il2CppClass* goClass = nullptr, *rtClass = nullptr, *imageClass = nullptr, *toggleClass = nullptr;
    Il2CppClass* canvasClass = nullptr, *raycasterClass = nullptr, *tmpTextClass = nullptr, *tmpUguiClass = nullptr;
    Il2CppClass* sdfRectClass = nullptr, *lseClass = nullptr, *lsClass = nullptr;
    Il2CppClass* menuClass = nullptr, *optionButtonClass = nullptr, *displayClass = nullptr, *entryClass = nullptr;
    Il2CppClass* inputIconClass = nullptr, *inputEventDisplayClass = nullptr, *encapsulateClass = nullptr;
    Il2CppClass* identClass = nullptr, *vmClass = nullptr, *destVmClass = nullptr, *stationModelClass = nullptr;

    // ---- offsets ----
    size_t offMenuVm = 0, offMenuDest = 0, offMenuPicker = 0, offMenuHeader = 0, offMenuDesc = 0, offMenuMarket = 0;
    size_t offOptPrompt = 0, offVmDest = 0, offVmStation = 0, offDestSel = 0, offActorId = 0;
    size_t offMarketPlorts = 0, offPlortEntryType = 0, offIdentIcon = 0, offIdentName = 0, offIdentIsPlort = 0;
    size_t offIconButton = 0, offIconKey = 0, offIconKeyText = 0, offIconKeyImage = 0, offIconKeyLse = 0, offIconDouble = 0;
    size_t offEncMinW = 0;  // optional
    size_t offGcLookup = 0, offLookupCurrencies = 0, offCurrencyArr = 0, offCurrencyIcon = 0;  // optional (coin)

    // ---- methods ----
    const MethodInfo* goCtor = nullptr, *goAddComponent = nullptr, *goGetLayer = nullptr, *goSetLayer = nullptr;
    const MethodInfo* trSetParent = nullptr, *trFind = nullptr, *trGetChild = nullptr, *trChildCount = nullptr;
    const MethodInfo* rtSetAnchorMin = nullptr, *rtSetAnchorMax = nullptr, *rtSetPivot = nullptr, *rtSetSize = nullptr,
                    *rtSetPos = nullptr, *rtGetSize = nullptr, *rtGetPos = nullptr;
    const MethodInfo* objDestroy = nullptr, *behSetEnabled = nullptr, *compInParent = nullptr;
    const MethodInfo* gfxSetColor = nullptr, *gfxSetRaycast = nullptr, *imgSetSprite = nullptr, *imgSetPreserve = nullptr;
    const MethodInfo* tmpSetText = nullptr, *tmpGetFont = nullptr, *tmpSetFont = nullptr, *tmpGetMat = nullptr,
                    *tmpSetMat = nullptr, *tmpSetSize = nullptr, *tmpSetColor = nullptr, *tmpSetAlign = nullptr,
                    *tmpSetWrap = nullptr, *tmpSetOverflow = nullptr;
    const MethodInfo* cvGetOrder = nullptr, *cvSetOrder = nullptr, *cvGetLayer = nullptr, *cvSetLayer = nullptr,
                    *cvSetOverride = nullptr;
    const MethodInfo* sdfSetRadius = nullptr, *sdfSetBorder = nullptr, *sdfSetBorderColor = nullptr;
    const MethodInfo* tglGetOn = nullptr, *tglSetOnQuiet = nullptr, *selSetTransition = nullptr, *selSetNav = nullptr;
    const MethodInfo* lsGet = nullptr, *entryGetType = nullptr, *gcInstance = nullptr;
    const MethodInfo* kbCurrent = nullptr, *kbC = nullptr, *padCurrent = nullptr, *padR3 = nullptr, *btnPressedThisFrame = nullptr;
    using ObjectNewFn = Il2CppObject* (*)(Il2CppClass*);
    ObjectNewFn objectNew = nullptr;

    // ---- live state (main thread) ----
    struct Held {  // object + GC handle (freed by a posted job)
        Il2CppObject* p = nullptr;
        Il2CppGCHandle h = nullptr;
    };
    Held menu;                 // DroneProgramMenu last bound
    Il2CppObject* boundMenu = nullptr;  // raw pointer from the SetViewModel hook (identity, set inside the hook)
    std::vector<Il2CppGCHandle> pillHandles, dialogHandles;
    Il2CppObject* pill = nullptr, *pillToggle = nullptr, *pillLabel = nullptr, *pillKeyText = nullptr;
    Il2CppObject* pillOrig = nullptr;  // the destination's PromptContainer (RectTransform) we cloned
    std::vector<Il2CppObject*> pillIcons;  // InputIcon transforms of the clone
    Il2CppObject* pillIed = nullptr, *pillGroup = nullptr;  // their parents (Transforms)
    int pillReapply = 0;
    bool pillShown = false;
    int stateCountdown = 0;
    bool marketSelected = false, pageActive = false;
    int64_t station = 0, labelStation = 0;
    bool haveStation = false;
    bool loggedPill = false, loggedPillFail = false, loggedOpen = false, loggedOpenFail = false;

    // dialog
    Il2CppObject* root = nullptr, *panelRt = nullptr, *blockerToggle = nullptr, *closeToggle = nullptr;
    Il2CppObject* modeToggle[2] = {}, *modeBg[2] = {}, *modeText[2] = {};
    Il2CppObject* focusFrame = nullptr, *noteText = nullptr;
    struct Cell {
        Il2CppObject* price = nullptr;  // TMP
        int base = 0;
    };
    std::vector<Cell> cells;
    Il2CppObject* titleSrc = nullptr, *bodySrc = nullptr;  // TMP style sources (menu labels), this open only
    sui::PlotMenuSlider slider;
    bool sliderReady = false;
    float sliderY = 0.0f, modeRowY = 0.0f;
    int64_t openStation = 0;
    LimitMode mode = LimitMode::kPrice;
    int values[2] = {0, 0};
    int focusRow = 1;  // 0 = mode, 1 = value
    int maxPrice = 100;

    // ---- helpers ----
    bool Faulted() const { return sr2::core::ModuleFaulted(module); }
    Il2CppObject* Call(const MethodInfo* m, void* obj, std::initializer_list<void*> args, const char* what,
                       bool* ok = nullptr) {
        if (!m) {
            if (ok) *ok = false;
            return nullptr;
        }
        std::vector<void*> params(args);
        std::string err;
        Il2CppObject* r = Resolver::Get().Invoke(m, obj, params.empty() ? nullptr : params.data(), &err);
        const bool good = err.empty();
        if (ok) *ok = good;
        if (!good) {
            if (err.rfind("native exception", 0) == 0) sr2::core::MarkModuleFaulted(module, std::format("{}: {}", what, err));
            else SR2_WARN("[{}] station ui: {} failed: {}", module, what, err);
        }
        return r;
    }
    template <class T>
    static T Unbox(Il2CppObject* boxed, T def = T{}) {
        return boxed ? *reinterpret_cast<T*>(reinterpret_cast<char*>(boxed) + 2 * sizeof(void*)) : def;
    }
    bool IsA(Il2CppObject* obj, Il2CppClass* k) const {
        const auto& a = api();
        if (!obj || !k || !a.il2cpp_object_get_class) return false;
        Il2CppClass* c = a.il2cpp_object_get_class(obj);
        if (c == k) return true;
        return c && a.il2cpp_class_is_assignable_from && a.il2cpp_class_is_assignable_from(k, c);
    }
    static void Keep(std::vector<Il2CppGCHandle>& into, Il2CppObject* o) {
        if (Il2CppGCHandle h = o ? unity::NewGCHandle(o) : nullptr) into.push_back(h);
    }
    static void FreeLater(std::vector<Il2CppGCHandle>& handles) {
        std::vector<Il2CppGCHandle> doomed;
        doomed.swap(handles);
        if (!doomed.empty()) main_thread::Post([doomed] {
            for (Il2CppGCHandle h : doomed) unity::FreeGCHandle(h);
        });
    }
    static void Release(Held& h) {
        h.p = nullptr;
        if (Il2CppGCHandle g = h.h) {
            h.h = nullptr;
            main_thread::Post([g] { unity::FreeGCHandle(g); });
        }
    }
    Il2CppObject* AddComponent(Il2CppObject* go, Il2CppClass* k, const char* what) {
        if (!unity::IsAlive(go) || !k) return nullptr;
        bool ok = false;
        Il2CppObject* c = Call(goAddComponent, go, {unity::TypeOf(k)}, what, &ok);
        return ok && unity::IsAlive(c) && IsA(c, k) ? c : nullptr;
    }
    void SetVec(const MethodInfo* m, Il2CppObject* rt, Vec2 v, const char* what) { Call(m, rt, {&v}, what); }
    Vec2 GetVec(const MethodInfo* m, Il2CppObject* rt, const char* what) {
        bool ok = false;
        Il2CppObject* b = Call(m, rt, {}, what, &ok);
        return ok ? Unbox<Vec2>(b) : Vec2{0, 0};
    }
    void Rect(Il2CppObject* rt, Vec2 amin, Vec2 amax, Vec2 pivot, Vec2 pos, Vec2 size) {
        SetVec(rtSetAnchorMin, rt, amin, "RectTransform.set_anchorMin");
        SetVec(rtSetAnchorMax, rt, amax, "RectTransform.set_anchorMax");
        SetVec(rtSetPivot, rt, pivot, "RectTransform.set_pivot");
        SetVec(rtSetSize, rt, size, "RectTransform.set_sizeDelta");
        SetVec(rtSetPos, rt, pos, "RectTransform.set_anchoredPosition");
    }
    // Top-left anchored rect inside the parent (y grows downwards).
    void At(Il2CppObject* rt, float x, float y, float w, float h) {
        Rect(rt, {0, 1}, {0, 1}, {0, 1}, {x, -y}, {w, h});
    }
    Il2CppObject* NewUi(const std::string& name, Il2CppObject* parentTr, int layer) {
        Il2CppObject* go = objectNew ? objectNew(goClass) : nullptr;
        if (!go) return nullptr;
        bool ok = false;
        Call(goCtor, go, {unity::NewString(name)}, "GameObject..ctor", &ok);
        if (!ok || !unity::IsAlive(go)) return nullptr;
        if (!AddComponent(go, rtClass, "AddComponent(RectTransform)")) {
            Destroy(go);
            return nullptr;
        }
        bool stays = false;
        Call(trSetParent, unity::TransformOf(go), {parentTr, &stays}, "Transform.SetParent", &ok);
        if (!ok) {
            Destroy(go);
            return nullptr;
        }
        if (layer >= 0) Call(goSetLayer, go, {&layer}, "GameObject.set_layer");
        return go;
    }
    void Destroy(Il2CppObject* go) {
        if (unity::IsAlive(go)) Call(objDestroy, nullptr, {go}, "Object.Destroy");
    }
    void SetEnabled(Il2CppObject* behaviour, bool on) {
        if (unity::IsAlive(behaviour)) Call(behSetEnabled, behaviour, {&on}, "Behaviour.set_enabled");
    }
    void SetColor(Il2CppObject* g, Color c) {
        if (unity::IsAlive(g)) Call(gfxSetColor, g, {&c}, "Graphic.set_color");
    }
    void SetRaycast(Il2CppObject* g, bool on) {
        if (unity::IsAlive(g)) Call(gfxSetRaycast, g, {&on}, "Graphic.set_raycastTarget");
    }
    void SetText(Il2CppObject* tmp, const std::string& s) {
        if (unity::IsAlive(tmp)) Call(tmpSetText, tmp, {unity::NewString(s)}, "TMP_Text.set_text");
    }
    // Rounded shape (the game's SDFRoundedRectangle, else a plain Image).
    Il2CppObject* Shape(Il2CppObject* go, Color c, float radius, float border = 0.0f, Color borderColor = {}) {
        if (sdfRectClass) {
            if (Il2CppObject* g = AddComponent(go, sdfRectClass, "AddComponent(SDFRoundedRectangle)")) {
                SetColor(g, c);
                Call(sdfSetRadius, g, {&radius}, "SDFRoundedRectangle.SetCornerRadius");
                if (border > 0.0f && sdfSetBorder && sdfSetBorderColor) {
                    Call(sdfSetBorder, g, {&border}, "SDFGraphic.set_BorderSize");
                    Call(sdfSetBorderColor, g, {&borderColor}, "SDFGraphic.set_BorderColor");
                }
                return g;
            }
        }
        Il2CppObject* img = AddComponent(go, imageClass, "AddComponent(Image)");
        SetColor(img, c);
        return img;
    }
    Il2CppObject* Box(Il2CppObject* parentRt, const char* name, float x, float y, float w, float h, Color c, float radius,
                      float border = 0.0f, Color borderColor = {}) {
        Il2CppObject* go = NewUi(name, parentRt, -1);
        if (!go) return nullptr;
        At(unity::TransformOf(go), x, y, w, h);
        Il2CppObject* g = Shape(go, c, radius, border, borderColor);
        SetRaycast(g, false);
        return go;
    }
    // TMP text; style (font / material) copied from `src` when given.
    Il2CppObject* Text(Il2CppObject* parentRt, const char* name, float x, float y, float w, float h, const std::string& s,
                       float size, Color col, int align, Il2CppObject* src, bool wrap = false, bool ellipsis = false) {
        Il2CppObject* go = NewUi(name, parentRt, -1);
        if (!go) return nullptr;
        At(unity::TransformOf(go), x, y, w, h);
        Il2CppObject* tmp = AddComponent(go, tmpUguiClass, "AddComponent(TextMeshProUGUI)");
        if (!tmp) return nullptr;
        if (unity::IsAlive(src)) {
            if (Il2CppObject* font = Call(tmpGetFont, src, {}, "TMP_Text.get_font"); unity::IsAlive(font))
                Call(tmpSetFont, tmp, {font}, "TMP_Text.set_font");
            if (Il2CppObject* mat = Call(tmpGetMat, src, {}, "TMP_Text.get_fontSharedMaterial"); unity::IsAlive(mat))
                Call(tmpSetMat, tmp, {mat}, "TMP_Text.set_fontSharedMaterial");
        }
        Call(tmpSetSize, tmp, {&size}, "TMP_Text.set_fontSize");
        Call(tmpSetColor, tmp, {&col}, "TMP_Text.set_color");
        int32_t a = align, wrapMode = wrap ? 1 : 0, overflow = 1;  // TextWrappingModes.Normal / NoWrap; Ellipsis
        Call(tmpSetAlign, tmp, {&a}, "TMP_Text.set_alignment");
        if (tmpSetWrap) Call(tmpSetWrap, tmp, {&wrapMode}, "TMP_Text.set_textWrappingMode");
        if (ellipsis && tmpSetOverflow) Call(tmpSetOverflow, tmp, {&overflow}, "TMP_Text.set_overflowMode");
        SetRaycast(tmp, false);
        SetText(tmp, s);
        return tmp;
    }
    Il2CppObject* Sprite(Il2CppObject* parentRt, const char* name, float x, float y, float w, float h, Il2CppObject* sprite) {
        if (!unity::IsAlive(sprite)) return nullptr;
        Il2CppObject* go = NewUi(name, parentRt, -1);
        if (!go) return nullptr;
        At(unity::TransformOf(go), x, y, w, h);
        Il2CppObject* img = AddComponent(go, imageClass, "AddComponent(Image)");
        if (!img) return nullptr;
        Call(imgSetSprite, img, {sprite}, "Image.set_sprite");
        bool keep = true;
        if (imgSetPreserve) Call(imgSetPreserve, img, {&keep}, "Image.set_preserveAspect");
        SetRaycast(img, false);
        return img;
    }
    // A pollable click target on `go` (whose graphics must be raycast targets).
    Il2CppObject* Clickable(Il2CppObject* go) {
        Il2CppObject* t = AddComponent(go, toggleClass, "AddComponent(Toggle)");
        if (!t) return nullptr;
        int32_t none = 0;  // Selectable.Transition.None
        alignas(8) unsigned char nav[64] = {};  // Navigation.Mode.None
        Call(selSetTransition, t, {&none}, "Selectable.set_transition");
        Call(selSetNav, t, {nav}, "Selectable.set_navigation");
        bool off = false;
        Call(tglSetOnQuiet, t, {&off}, "Toggle.SetIsOnWithoutNotify");
        return t;
    }
    // true once per click.
    bool Clicked(Il2CppObject* toggle) {
        if (!unity::IsAlive(toggle)) return false;
        bool ok = false;
        const bool on = Unbox<bool>(Call(tglGetOn, toggle, {}, "Toggle.get_isOn", &ok));
        if (!ok || !on) return false;
        bool off = false;
        Call(tglSetOnQuiet, toggle, {&off}, "Toggle.SetIsOnWithoutNotify");
        return true;
    }
    Il2CppObject* Find(Il2CppObject* tr, const char* path) {
        if (!unity::IsAlive(tr)) return nullptr;
        bool ok = false;
        Il2CppObject* r = Call(trFind, tr, {unity::NewString(path)}, "Transform.Find", &ok);
        return ok && unity::IsAlive(r) ? r : nullptr;
    }
    Il2CppObject* TmpOf(Il2CppObject* goOrComp) {
        if (!unity::IsAlive(goOrComp)) return nullptr;
        Il2CppObject* t = unity::GetComponent(goOrComp, tmpTextClass);
        return unity::IsAlive(t) ? t : nullptr;
    }
    bool KeyPressed(const MethodInfo* current, const MethodInfo* control) {
        if (!current || !control || !btnPressedThisFrame) return false;
        bool ok = false;
        Il2CppObject* dev = Call(current, nullptr, {}, "InputDevice.get_current", &ok);
        if (!ok || !dev) return false;
        Il2CppObject* ctl = Call(control, dev, {}, "InputDevice control getter", &ok);
        if (!ok || !ctl) return false;
        return Unbox<bool>(Call(btnPressedThisFrame, ctl, {}, "ButtonControl.get_wasPressedThisFrame", &ok)) && ok;
    }
    std::string Localized(Il2CppObject* ls) {
        if (!ls || !lsGet || !IsA(ls, lsClass)) return {};
        bool ok = false;
        Il2CppObject* r = Call(lsGet, ls, {}, "LocalizedString.GetLocalizedString", &ok);
        return ok && r ? unity::ToUtf8(reinterpret_cast<Il2CppString*>(r)) : std::string();
    }
    Il2CppObject* CoinSprite() {
        if (!gcInstance || !offGcLookup || !offLookupCurrencies || !offCurrencyArr || !offCurrencyIcon) return nullptr;
        bool ok = false;
        Il2CppObject* gc = Call(gcInstance, nullptr, {}, "GameContext.Instance", &ok);
        if (!ok || !unity::IsAlive(gc)) return nullptr;
        Il2CppObject* lookup = unity::Field<Il2CppObject*>(gc, offGcLookup);
        if (!unity::IsAlive(lookup)) return nullptr;
        Il2CppObject* list = unity::Field<Il2CppObject*>(lookup, offLookupCurrencies);
        if (!unity::IsAlive(list)) return nullptr;
        void* arr = unity::Field<void*>(list, offCurrencyArr);
        auto* first = arr && unity::ArrayLength(arr) > 0 ? static_cast<Il2CppObject*>(unity::ArrayAt(arr, 0)) : nullptr;
        Il2CppObject* icon = unity::IsAlive(first) ? unity::Field<Il2CppObject*>(first, offCurrencyIcon) : nullptr;
        return unity::IsAlive(icon) ? icon : nullptr;
    }

    // ---- menu state ----
    void ForgetMenu();
    void ReadState();
    // ---- prompt ----
    void EnsurePill();
    void ApplyPillLook();
    void PlacePill();
    void UpdatePillLabel();
    void DestroyPill();
    // ---- dialog ----
    bool Open();
    void Close(const char* why);
    void BuildSlider();
    void SetMode(LimitMode m);
    void SetFocus(int row);
    void StyleModes();
    void UpdateSheet();
    void Changed();
    void Navigate(int dir);
    void Submit();
    void Tick(float dt);
};

namespace {

StationUi::Impl* g_ui = nullptr;
sr2::core::DetourGuard g_bindGuard{"drone_price_limit", "DroneProgramMenu.SetViewModel"};
sr2::core::DetourGuard g_navGuard{"drone_price_limit", "FocusManager (dialog input)"};
sr2::core::DetourGuard g_hotkeyGuard{"drone_price_limit", "DroneProgramMenu hotkeys"};

using BindFn = void (*)(Il2CppObject*, Il2CppObject*, const MethodInfo*);
using NavFn = bool (*)(int32_t, int32_t, Il2CppObject*, const MethodInfo*);
using ActionFn = bool (*)(Il2CppObject*, const MethodInfo*);
using HotkeyFn = void (*)(Il2CppObject*, void*, const MethodInfo*);
BindFn g_origBind = nullptr;
NavFn g_origNav = nullptr;
ActionFn g_origSubmit = nullptr, g_origCancel = nullptr;
HotkeyFn g_origClearAll = nullptr, g_origActivate = nullptr;

void BindDetour(Il2CppObject* self, Il2CppObject* vm, const MethodInfo* mi) {
    g_origBind(self, vm, mi);
    if (!g_ui) return;
    g_bindGuard.Run([&] {
        if (main_thread::IsMainThread()) g_ui->boundMenu = self;  // identity only; pinned in Tick
    });
}

bool NavDetour(int32_t dir, int32_t cause, Il2CppObject* evt, const MethodInfo* mi) {
    if (!g_open.load(std::memory_order_relaxed) || !g_ui) return g_origNav(dir, cause, evt, mi);
    bool handled = false;
    g_navGuard.Run([&] {
        if (!main_thread::IsMainThread()) return;
        g_ui->Navigate(dir);
        handled = true;
    });
    return handled ? true : g_origNav(dir, cause, evt, mi);
}

bool SubmitDetour(Il2CppObject* action, const MethodInfo* mi) {
    if (!g_open.load(std::memory_order_relaxed) || !g_ui) return g_origSubmit(action, mi);
    bool handled = false;
    g_navGuard.Run([&] {
        if (!main_thread::IsMainThread()) return;
        g_ui->Submit();
        handled = true;
    });
    return handled ? true : g_origSubmit(action, mi);
}

bool CancelDetour(Il2CppObject* action, const MethodInfo* mi) {
    if (!g_open.load(std::memory_order_relaxed) || !g_ui) return g_origCancel(action, mi);
    bool handled = false;
    g_navGuard.Run([&] {
        if (!main_thread::IsMainThread()) return;
        g_ui->Close("cancel");
        handled = true;
    });
    return handled ? true : g_origCancel(action, mi);
}

// The page's own hotkeys do nothing while the dialog covers the page.
void ClearAllDetour(Il2CppObject* self, void* evt, const MethodInfo* mi) {
    if (g_open.load(std::memory_order_relaxed) && g_hotkeyGuard.Active()) return;
    g_origClearAll(self, evt, mi);
}

void ActivateDetour(Il2CppObject* self, void* evt, const MethodInfo* mi) {
    if (g_open.load(std::memory_order_relaxed) && g_hotkeyGuard.Active()) return;
    g_origActivate(self, evt, mi);
}

}  // namespace

// ---------------------------------------------------------------- setup --

bool StationUi::Setup(sr2::core::ModuleContext& ctx, const char* module, DialogHost host, std::string& why) {
    Resolver& R = ctx.resolver();
    auto* p = new Impl();
    p->module = module;
    p->host = std::move(host);
    auto parentOf = [](Il2CppClass* c) { return api().il2cpp_class_get_parent ? api().il2cpp_class_get_parent(c) : nullptr; };
    auto cls = [&](const char* image, const char* ns, const char* name) -> Il2CppClass* {
        std::string err;
        Il2CppClass* k = R.Class(image, ns, name, &err);
        if (!k && why.empty()) why = err.empty() ? std::format("class {}.{} not found", ns, name) : err;
        return k;
    };
    auto method = [&](Il2CppClass* k, const char* name, int argc, bool required = true) -> const MethodInfo* {
        std::string err;
        for (Il2CppClass* c = k; c; c = parentOf(c))
            if (const MethodInfo* m = R.Method(c, name, argc, &err)) return m;
        if (required && why.empty()) why = std::format("method {}({}) not found", name, argc);
        return nullptr;
    };
    auto byParams = [&](Il2CppClass* k, const char* name, std::initializer_list<std::string_view> params,
                        bool required = true) -> const MethodInfo* {
        std::string err;
        const MethodInfo* m = k ? R.MethodByParams(k, name, params, &err) : nullptr;
        if (!m && required && why.empty()) why = std::format("{}: {}", name, err);
        return m;
    };
    auto off = [&](Il2CppClass* k, const char* field, size_t& out, bool required = true) {
        std::string err;
        for (Il2CppClass* c = k; c; c = parentOf(c))
            if (auto o = R.FieldOffset(c, field, &err)) {
                out = *o;
                return;
            }
        if (required && why.empty()) why = std::format("field {} not found", field);
    };
    auto fail = [&]() {
        delete p;
        return false;
    };

    HMODULE ga = api().module;
    p->objectNew = reinterpret_cast<Impl::ObjectNewFn>(ga ? GetProcAddress(ga, "il2cpp_object_new") : nullptr);
    if (!p->objectNew) why = "il2cpp_object_new export missing";

    p->goClass = cls(kCore, "UnityEngine", "GameObject");
    Il2CppClass* transform = cls(kCore, "UnityEngine", "Transform");
    Il2CppClass* component = cls(kCore, "UnityEngine", "Component");
    Il2CppClass* behaviour = cls(kCore, "UnityEngine", "Behaviour");
    Il2CppClass* uobject = cls(kCore, "UnityEngine", "Object");
    p->rtClass = cls(kCore, "UnityEngine", "RectTransform");
    p->canvasClass = cls("UnityEngine.UIModule", "UnityEngine", "Canvas");
    p->imageClass = cls(kUi, "UnityEngine.UI", "Image");
    Il2CppClass* graphic = cls(kUi, "UnityEngine.UI", "Graphic");
    Il2CppClass* selectable = cls(kUi, "UnityEngine.UI", "Selectable");
    p->toggleClass = cls(kUi, "UnityEngine.UI", "Toggle");
    p->raycasterClass = cls(kUi, "UnityEngine.UI", "GraphicRaycaster");
    p->tmpTextClass = cls(kTmp, "TMPro", "TMP_Text");
    p->tmpUguiClass = cls(kTmp, "TMPro", "TextMeshProUGUI");
    p->lseClass = cls("Unity.Localization", "UnityEngine.Localization.Components", "LocalizeStringEvent");
    p->lsClass = cls("Unity.Localization", "UnityEngine.Localization", "LocalizedString");
    p->menuClass = cls(kImage, kDroneUiNs, "DroneProgramMenu");
    p->optionButtonClass = cls(kImage, kDroneUiNs, "DroneProgramOptionButton");
    p->displayClass = cls(kImage, kDroneUiNs, "DroneUIDisplay");
    p->vmClass = cls(kImage, kDroneDataNs, "DroneProgramViewModel");
    p->destVmClass = cls(kImage, kDroneDataNs, "DroneProgramReadOnlyDestinationViewModel");
    p->stationModelClass = cls(kImage, "MonomiPark.SlimeRancher.DataModel", "DroneStationGadgetModel");
    p->entryClass = cls(kImage, kDroneDataNs, "DroneDestinationOptionEntry");
    Il2CppClass* identModel = cls(kImage, "MonomiPark.SlimeRancher.DataModel", "IdentifiableModel");
    Il2CppClass* marketCfg = cls(kImage, "MonomiPark.SlimeRancher.UI", "MarketUIConfiguration");
    Il2CppClass* plortEntry = cls(kImage, "MonomiPark.SlimeRancher.UI", "PlortEntry");
    p->identClass = cls(kImage, "", "IdentifiableType");
    p->inputIconClass = cls(kImage, "MonomiPark.SlimeRancher.UI.Framework.CommonControls", "InputIcon");
    p->inputEventDisplayClass = cls(kImage, "MonomiPark.SlimeRancher.UI.Framework.CommonControls", "InputEventDisplay");
    Il2CppClass* focusMgr = cls(kImage, "MonomiPark.SlimeRancher.UI.Framework.Interaction.Focus", "FocusManager");
    if (!why.empty()) return fail();

    off(p->menuClass, "_viewModel", p->offMenuVm);
    off(p->menuClass, "_destinationButton", p->offMenuDest);
    off(p->menuClass, "_optionPicker", p->offMenuPicker);
    off(p->menuClass, "_headerLabel", p->offMenuHeader);
    off(p->menuClass, "_descriptionLabel", p->offMenuDesc);
    off(p->menuClass, "_marketConfig", p->offMenuMarket);
    off(p->optionButtonClass, "_promptContainer", p->offOptPrompt);
    off(p->vmClass, "_destinationForTarget", p->offVmDest);
    off(p->vmClass, "_station", p->offVmStation);
    off(p->destVmClass, "_currentSelection", p->offDestSel);
    off(identModel, "actorId", p->offActorId);
    off(marketCfg, "_plorts", p->offMarketPlorts);
    off(plortEntry, "IdentType", p->offPlortEntryType);
    off(p->identClass, "icon", p->offIdentIcon);
    off(p->identClass, "localizedName", p->offIdentName);
    off(p->inputIconClass, "_buttonRoot", p->offIconButton);
    off(p->inputIconClass, "_keyRoot", p->offIconKey);
    off(p->inputIconClass, "_keyText", p->offIconKeyText);
    off(p->inputIconClass, "_keyImage", p->offIconKeyImage);
    off(p->inputIconClass, "_keyLocalizeStringEvent", p->offIconKeyLse);
    off(p->inputIconClass, "_doubleTapRoot", p->offIconDouble);
    if (!why.empty()) return fail();

    p->goCtor = byParams(p->goClass, ".ctor", {"System.String"});
    p->goAddComponent = byParams(p->goClass, "AddComponent", {"System.Type"});
    p->goGetLayer = method(p->goClass, "get_layer", 0);
    p->goSetLayer = method(p->goClass, "set_layer", 1);
    p->trSetParent = byParams(transform, "SetParent", {"UnityEngine.Transform", "System.Boolean"});
    p->trFind = byParams(transform, "Find", {"System.String"});
    p->trGetChild = method(transform, "GetChild", 1);
    p->trChildCount = method(transform, "get_childCount", 0);
    p->rtSetAnchorMin = method(p->rtClass, "set_anchorMin", 1);
    p->rtSetAnchorMax = method(p->rtClass, "set_anchorMax", 1);
    p->rtSetPivot = method(p->rtClass, "set_pivot", 1);
    p->rtSetSize = method(p->rtClass, "set_sizeDelta", 1);
    p->rtSetPos = method(p->rtClass, "set_anchoredPosition", 1);
    p->rtGetSize = method(p->rtClass, "get_sizeDelta", 0);
    p->rtGetPos = method(p->rtClass, "get_anchoredPosition", 0);
    p->objDestroy = byParams(uobject, "Destroy", {"UnityEngine.Object"});
    p->behSetEnabled = method(behaviour, "set_enabled", 1);
    p->compInParent = byParams(component, "GetComponentInParent", {"System.Type", "System.Boolean"});
    p->gfxSetColor = method(graphic, "set_color", 1);
    p->gfxSetRaycast = method(graphic, "set_raycastTarget", 1);
    p->imgSetSprite = method(p->imageClass, "set_sprite", 1);
    p->tmpSetText = method(p->tmpTextClass, "set_text", 1);
    p->tmpGetFont = method(p->tmpTextClass, "get_font", 0);
    p->tmpSetFont = method(p->tmpTextClass, "set_font", 1);
    p->tmpGetMat = method(p->tmpTextClass, "get_fontSharedMaterial", 0);
    p->tmpSetMat = method(p->tmpTextClass, "set_fontSharedMaterial", 1);
    p->tmpSetSize = method(p->tmpTextClass, "set_fontSize", 1);
    p->tmpSetColor = method(p->tmpTextClass, "set_color", 1);
    p->tmpSetAlign = method(p->tmpTextClass, "set_alignment", 1);
    p->cvGetOrder = method(p->canvasClass, "get_sortingOrder", 0);
    p->cvSetOrder = method(p->canvasClass, "set_sortingOrder", 1);
    p->cvGetLayer = method(p->canvasClass, "get_sortingLayerID", 0);
    p->cvSetLayer = method(p->canvasClass, "set_sortingLayerID", 1);
    p->cvSetOverride = method(p->canvasClass, "set_overrideSorting", 1);
    p->tglGetOn = method(p->toggleClass, "get_isOn", 0);
    p->tglSetOnQuiet = method(p->toggleClass, "SetIsOnWithoutNotify", 1);
    p->selSetTransition = method(selectable, "set_transition", 1);
    p->selSetNav = method(selectable, "set_navigation", 1);
    p->entryGetType = method(p->entryClass, "get_Type", 0);
    const MethodInfo* bindMi = method(p->menuClass, "SetViewModel", 1);
    const MethodInfo* navMi = method(focusMgr, "NavigateInDirection", 3);
    const MethodInfo* submitMi = method(focusMgr, "SendSubmitActionToFocus", 1);
    const MethodInfo* cancelMi = method(focusMgr, "SendCancelActionToFocus", 1);
    const MethodInfo* clearAllMi = method(p->menuClass, "HandleClearAllOptions", 1);
    const MethodInfo* activateMi = method(p->menuClass, "HandleActivateDrone", 1);
    if (!why.empty()) return fail();

    // ---- optional parts ----
    std::string report;
    {
        std::string err;
        p->tmpSetWrap = R.Method(p->tmpTextClass, "set_textWrappingMode", 1, &err);
        p->tmpSetOverflow = R.Method(p->tmpTextClass, "set_overflowMode", 1, &err);
        p->imgSetPreserve = R.Method(p->imageClass, "set_preserveAspect", 1, &err);
        p->lsGet = R.Method(p->lsClass, "GetLocalizedString", 0, &err);
        p->sdfRectClass = R.Class(kImage, "MonomiPark.SlimeRancher.UI.SDFUI", "SDFRoundedRectangle", &err);
        if (p->sdfRectClass) {
            p->sdfSetRadius = R.MethodByParams(p->sdfRectClass, "SetCornerRadius", {"System.Single"}, &err);
            Il2CppClass* sdf = parentOf(p->sdfRectClass);  // SDFGraphic
            p->sdfSetBorder = sdf ? R.Method(sdf, "set_BorderSize", 1, &err) : nullptr;
            p->sdfSetBorderColor = sdf ? R.Method(sdf, "set_BorderColor", 1, &err) : nullptr;
            if (!p->sdfSetRadius) p->sdfRectClass = nullptr;
        }
        if (!p->sdfRectClass) report += " SDF shapes: NOT available (plain images);";
        p->encapsulateClass = R.Class(kImage, "MonomiPark.SlimeRancher.UI.Framework.Layout", "EncapsulateChildren", &err);
        if (p->encapsulateClass)
            if (auto o = R.FieldOffset(p->encapsulateClass, "_minimumWidth", &err)) p->offEncMinW = *o;
        // Coin icon: GameContext.Instance.LookupDirector._currencyList._currencies[0]._icon.
        Il2CppClass* gameCtx = R.Class(kImage, "", "GameContext", &err);
        Il2CppClass* lookup = R.Class(kImage, "", "LookupDirector", &err);
        Il2CppClass* curList = R.Class(kImage, "MonomiPark.SlimeRancher.Economy", "CurrencyList", &err);
        Il2CppClass* curDef = R.Class(kImage, "MonomiPark.SlimeRancher.Economy", "CurrencyDefinition", &err);
        if (gameCtx && lookup && curList && curDef) {
            if (Il2CppClass* single = parentOf(gameCtx)) p->gcInstance = R.Method(single, "get_Instance", 0, &err);
            if (auto o = R.FieldOffset(gameCtx, "<LookupDirector>k__BackingField", &err)) p->offGcLookup = *o;
            if (auto o = R.FieldOffset(lookup, "_currencyList", &err)) p->offLookupCurrencies = *o;
            if (auto o = R.FieldOffset(curList, "_currencies", &err)) p->offCurrencyArr = *o;
            if (auto o = R.FieldOffset(curDef, "_icon", &err)) p->offCurrencyIcon = *o;
        }
        if (!p->gcInstance || !p->offGcLookup || !p->offLookupCurrencies || !p->offCurrencyArr || !p->offCurrencyIcon)
            report += " coin icon: NOT available;";
        // Hotkeys: C and the pad's right stick click.
        Il2CppClass* kb = R.Class(kInput, "UnityEngine.InputSystem", "Keyboard", &err);
        Il2CppClass* pad = R.Class(kInput, "UnityEngine.InputSystem", "Gamepad", &err);
        Il2CppClass* button = R.Class(kInput, "UnityEngine.InputSystem.Controls", "ButtonControl", &err);
        p->kbCurrent = kb ? R.Method(kb, "get_current", 0, &err) : nullptr;
        p->kbC = kb ? R.Method(kb, "get_cKey", 0, &err) : nullptr;
        p->padCurrent = pad ? R.Method(pad, "get_current", 0, &err) : nullptr;
        p->padR3 = pad ? R.Method(pad, "get_rightStickButton", 0, &err) : nullptr;
        p->btnPressedThisFrame = button ? R.Method(button, "get_wasPressedThisFrame", 0, &err) : nullptr;
        if (!p->btnPressedThisFrame || !p->kbCurrent || !p->kbC) report += " C key: NOT available (click only);";
    }

    // The slider: our own instance of the plot menu slider, built into the dialog.
    {
        std::string serr;
        p->sliderReady = p->slider.Setup(R, module, serr);
        if (!p->sliderReady) {
            why = "slider: " + serr;
            return fail();
        }
    }

    struct H {
        const char* name;
        const MethodInfo* mi;
        void* detour;
        void** orig;
    };
    const H hooks[] = {
        {"DroneProgramMenu.SetViewModel", bindMi, reinterpret_cast<void*>(&BindDetour), reinterpret_cast<void**>(&g_origBind)},
        {"FocusManager.NavigateInDirection", navMi, reinterpret_cast<void*>(&NavDetour), reinterpret_cast<void**>(&g_origNav)},
        {"FocusManager.SendSubmitActionToFocus", submitMi, reinterpret_cast<void*>(&SubmitDetour),
         reinterpret_cast<void**>(&g_origSubmit)},
        {"FocusManager.SendCancelActionToFocus", cancelMi, reinterpret_cast<void*>(&CancelDetour),
         reinterpret_cast<void**>(&g_origCancel)},
        {"DroneProgramMenu.HandleClearAllOptions", clearAllMi, reinterpret_cast<void*>(&ClearAllDetour),
         reinterpret_cast<void**>(&g_origClearAll)},
        {"DroneProgramMenu.HandleActivateDrone", activateMi, reinterpret_cast<void*>(&ActivateDetour),
         reinterpret_cast<void**>(&g_origActivate)},
    };
    std::string hookReport;
    for (const H& h : hooks) {
        std::string err;
        void* ptr = R.MethodPointer(h.mi, &err);
        if (!ptr) {
            why = std::format("{}: no native body ({})", h.name, err);
            return fail();
        }
        if (!ctx.HookAddress(h.name, ptr, h.detour, h.orig)) {
            why = std::format("hook {} failed", h.name);
            return fail();
        }
        hookReport += std::format(" {}@0x{:X}", h.name, Resolver::Rva(ptr));
    }

    impl_ = p;
    g_ui = p;
    ready_ = true;
    SR2_INFO("[{}] station ui ready:{}{}", module, hookReport, report.empty() ? "" : std::string(";") + report);
    return true;
}

// ---------------------------------------------------------------- menu state --

void StationUi::Impl::ForgetMenu() {
    if (g_open.load()) Close("menu gone");
    DestroyPill();
    Release(menu);
    marketSelected = pageActive = haveStation = false;
    station = 0;
}

void StationUi::Impl::ReadState() {
    marketSelected = false;
    haveStation = false;
    Il2CppObject* m = menu.p;
    pageActive = unity::ActiveInHierarchy(unity::GameObjectOf(m));
    if (pageActive) {  // the option picker sub page covers the program page
        Il2CppObject* picker = unity::Field<Il2CppObject*>(m, offMenuPicker);
        if (unity::IsAlive(picker) && unity::ActiveInHierarchy(unity::GameObjectOf(picker))) pageActive = false;
    }
    Il2CppObject* vm = unity::Field<Il2CppObject*>(m, offMenuVm);
    if (!IsA(vm, vmClass)) return;
    if (Il2CppObject* st = unity::Field<Il2CppObject*>(vm, offVmStation); IsA(st, stationModelClass)) {
        station = unity::Field<int64_t>(st, offActorId);
        haveStation = station != 0;
    }
    Il2CppObject* dest = unity::Field<Il2CppObject*>(vm, offVmDest);
    Il2CppObject* sel = IsA(dest, destVmClass) ? unity::Field<Il2CppObject*>(dest, offDestSel) : nullptr;
    if (!IsA(sel, entryClass)) return;
    bool ok = false;
    const int type = Unbox<int32_t>(Call(entryGetType, sel, {}, "DroneDestinationOptionEntry.get_Type", &ok));
    marketSelected = ok && type == kSinkPlortMarket;
}

// ---------------------------------------------------------------- prompt --

void StationUi::Impl::DestroyPill() {
    if (pill && !Faulted()) Destroy(pill);
    pill = pillToggle = pillLabel = pillKeyText = pillOrig = pillIed = pillGroup = nullptr;
    pillIcons.clear();
    pillShown = false;
    FreeLater(pillHandles);
}

void StationUi::Impl::EnsurePill() {
    if (pill && unity::IsAlive(pill)) return;
    if (pill) DestroyPill();  // died with the menu
    Il2CppObject* button = unity::Field<Il2CppObject*>(menu.p, offMenuDest);
    if (!unity::IsAlive(button) || !IsA(button, optionButtonClass)) return;
    Il2CppObject* orig = unity::Field<Il2CppObject*>(button, offOptPrompt);
    if (!unity::IsAlive(orig) || !IsA(orig, rtClass)) {
        if (!loggedPillFail) SR2_WARN("[{}] station ui: the destination has no prompt container - no price-limit action", module);
        loggedPillFail = true;
        return;
    }
    Il2CppObject* clone = unity::Instantiate(unity::GameObjectOf(orig), unity::TransformOf(button));
    if (!unity::IsAlive(clone)) {
        if (!loggedPillFail) SR2_WARN("[{}] station ui: cloning the Change prompt failed", module);
        loggedPillFail = true;
        return;
    }
    Il2CppObject* go = IsA(clone, goClass) ? clone : unity::GameObjectOf(clone);
    pill = go;
    pillOrig = orig;
    Keep(pillHandles, go);
    Keep(pillHandles, orig);
    unity::SetName(go, "sr2mods_PriceLimitPrompt");
    Il2CppObject* tr = unity::TransformOf(go);
    // Our own key and label: the clone must not follow the input device / localisation.
    if (Il2CppObject* ied = Find(tr, "InputEventDisplay")) {
        SetEnabled(unity::GetComponent(ied, inputEventDisplayClass), false);
        pillIed = ied;
        Keep(pillHandles, ied);
    }
    if (Il2CppObject* group = Find(tr, "InputEventDisplay/Icon Group")) {
        pillGroup = group;
        Keep(pillHandles, group);
        bool ok = false;
        const int n = Unbox<int32_t>(Call(trChildCount, group, {}, "Transform.get_childCount", &ok));
        for (int i = 0; ok && i < n && i < 8; ++i) {
            int32_t idx = i;
            Il2CppObject* child = Call(trGetChild, group, {&idx}, "Transform.GetChild");
            if (unity::IsAlive(child)) {
                Keep(pillHandles, child);
                pillIcons.push_back(child);
            }
        }
    }
    if (Il2CppObject* labelTr = Find(tr, "InputLabel")) {
        SetEnabled(unity::GetComponent(labelTr, lseClass), false);
        pillLabel = TmpOf(labelTr);
        Keep(pillHandles, pillLabel);
    }
    // Clickable: the pill background catches the pointer, a Toggle reports the click.
    if (sdfRectClass) SetRaycast(unity::GetComponent(go, sdfRectClass), true);
    pillToggle = Clickable(go);
    Keep(pillHandles, pillToggle);
    if (offEncMinW && encapsulateClass)
        if (Il2CppObject* enc = unity::GetComponent(go, encapsulateClass)) unity::Field<float>(enc, offEncMinW) = 0.0f;
    unity::SetActive(go, false);
    pillShown = false;
    pillReapply = kReapplyFrames;
    if (!loggedPill) {
        loggedPill = true;
        SR2_INFO("[{}] station ui: price-limit action cloned from the destination's Change prompt ({} key icon(s), "
                 "label {}, click {})",
                 module, pillIcons.size(), pillLabel ? "ok" : "MISSING", pillToggle ? "ok" : "MISSING");
    }
}

void StationUi::Impl::ApplyPillLook() {
    // InputIcon.SetKey by hand: the first icon shows the C key, the others are hidden.
    if (unity::IsAlive(pillIed)) unity::SetActive(unity::GameObjectOf(pillIed), true);
    if (unity::IsAlive(pillGroup)) unity::SetActive(unity::GameObjectOf(pillGroup), true);
    for (size_t i = 0; i < pillIcons.size(); ++i) {
        Il2CppObject* t = pillIcons[i];
        if (!unity::IsAlive(t)) continue;
        if (i > 0) {
            unity::SetActive(unity::GameObjectOf(t), false);
            continue;
        }
        unity::SetActive(unity::GameObjectOf(t), true);
        Il2CppObject* icon = unity::GetComponent(t, inputIconClass);
        if (!unity::IsAlive(icon)) continue;
        Il2CppObject* btn = unity::Field<Il2CppObject*>(icon, offIconButton);
        Il2CppObject* key = unity::Field<Il2CppObject*>(icon, offIconKey);
        Il2CppObject* dbl = unity::Field<Il2CppObject*>(icon, offIconDouble);
        Il2CppObject* text = unity::Field<Il2CppObject*>(icon, offIconKeyText);
        if (unity::IsAlive(btn)) unity::SetActive(btn, false);
        if (unity::IsAlive(dbl)) unity::SetActive(dbl, false);
        if (unity::IsAlive(key)) unity::SetActive(key, true);
        SetEnabled(unity::Field<Il2CppObject*>(icon, offIconKeyImage), false);
        SetEnabled(unity::Field<Il2CppObject*>(icon, offIconKeyLse), false);
        if (unity::IsAlive(text)) {
            SetEnabled(text, true);
            SetText(text, "C");
            pillKeyText = text;
        }
    }
    UpdatePillLabel();
}

void StationUi::Impl::UpdatePillLabel() {
    if (!pillLabel || !haveStation || !host.get) return;
    const StationLimit l = host.get(station);
    std::string s = "Price limit";
    if (l.Active()) s += l.mode == LimitMode::kFraction ? std::format(": {}%", l.value) : std::format(": {}", l.value);
    SetText(pillLabel, s);
}

void StationUi::Impl::PlacePill() {
    if (!unity::IsAlive(pill) || !unity::IsAlive(pillOrig)) return;
    Il2CppObject* rt = unity::TransformOf(pill);
    const Vec2 opos = GetVec(rtGetPos, pillOrig, "RectTransform.get_anchoredPosition");
    const Vec2 osize = GetVec(rtGetSize, pillOrig, "RectTransform.get_sizeDelta");
    const Vec2 size = GetVec(rtGetSize, rt, "RectTransform.get_sizeDelta");
    // Same anchors / pivot as the original (pivot x 0.5): to its right, 12 apart.
    const Vec2 want{opos.x + osize.x * 0.5f + 12.0f + size.x * 0.5f, opos.y};
    const Vec2 cur = GetVec(rtGetPos, rt, "RectTransform.get_anchoredPosition");
    if (std::fabs(cur.x - want.x) > 0.5f || std::fabs(cur.y - want.y) > 0.5f)
        SetVec(rtSetPos, rt, want, "RectTransform.set_anchoredPosition");
}

// ---------------------------------------------------------------- dialog --

bool StationUi::Impl::Open() {
    if (g_open.load() || !haveStation || !host.get || !host.set) return false;
    Il2CppObject* m = menu.p;
    auto failOpen = [&](const char* what) {
        if (!loggedOpenFail) SR2_WARN("[{}] station ui: dialog not opened: {}", module, what);
        loggedOpenFail = true;
        Close("open failed");  // removes what was built, frees the handles
        return false;
    };
    bool ok = false;
    bool inactive = true;
    Il2CppObject* display = Call(compInParent, m, {unity::TypeOf(displayClass), &inactive}, "GetComponentInParent(DroneUIDisplay)", &ok);
    if (!ok || !unity::IsAlive(display)) return failOpen("DroneUIDisplay not found above the program page");
    Il2CppObject* displayTr = unity::TransformOf(display);
    const int layer = Unbox<int32_t>(Call(goGetLayer, unity::GameObjectOf(display), {}, "GameObject.get_layer"), -1);
    Il2CppObject* hdr = unity::Field<Il2CppObject*>(m, offMenuHeader);
    Il2CppObject* desc = unity::Field<Il2CppObject*>(m, offMenuDesc);
    titleSrc = TmpOf(hdr);
    bodySrc = TmpOf(desc);
    if (!bodySrc) bodySrc = titleSrc;
    Keep(dialogHandles, titleSrc);
    Keep(dialogHandles, bodySrc);

    // Plorts in the market terminal's order, with their base price.
    struct Row {
        Il2CppObject* icon;
        std::string name;
        int base;
    };
    std::vector<Row> rows;
    float maxBase = 0.0f;
    if (Il2CppObject* cfg = unity::Field<Il2CppObject*>(m, offMenuMarket); unity::IsAlive(cfg)) {
        void* arr = unity::Field<void*>(cfg, offMarketPlorts);
        const size_t n = arr ? unity::ArrayLength(arr) : 0;
        for (size_t i = 0; i < n && i < 128; ++i) {
            auto* e = static_cast<Il2CppObject*>(unity::ArrayAt(arr, i));
            Il2CppObject* id = e ? unity::Field<Il2CppObject*>(e, offPlortEntryType) : nullptr;
            if (!IsA(id, identClass) || !unity::IsAlive(id)) continue;
            const float base = host.baseValue ? host.baseValue(id) : 0.0f;
            std::string name = Localized(unity::Field<Il2CppObject*>(id, offIdentName));
            if (name.empty()) name = unity::GetName(id);
            rows.push_back({unity::Field<Il2CppObject*>(id, offIdentIcon), name, static_cast<int>(std::lround(base))});
            maxBase = std::max(maxBase, base);
        }
    }
    maxPrice = std::max(100, static_cast<int>(std::ceil(maxBase * 2.0f / 10.0f)) * 10);

    // Current limit of this station.
    openStation = station;
    const StationLimit cur = host.get(station);
    mode = cur.mode;
    values[0] = values[1] = 0;
    values[static_cast<int>(mode)] = cur.value;
    focusRow = 1;

    // ---- layout ----
    const int n = static_cast<int>(rows.size());
    const int cols = n > 30 ? 6 : n > 20 ? 5 : 4;
    const int nrows = (n + cols - 1) / cols;
    const float sheetX = kPad, sheetW = kPanelW - 2 * kPad;
    const float cellW = (sheetW - 32.0f - (cols - 1) * kCellGap) / cols;
    const float titleY = 28.0f, subY = 84.0f;
    modeRowY = 156.0f;
    sliderY = modeRowY + kRowH + 22.0f;
    const float sheetY = sliderY + sui::PlotMenuSlider::Height() + 30.0f;
    const float sheetH = 56.0f + nrows * (kCellH + kCellGap) + 10.0f;
    const float footY = sheetY + sheetH + 18.0f;
    const float panelH = footY + kRowH + 26.0f;

    // ---- root: own canvas above the menu, a dimmed backdrop and the panel ----
    Il2CppObject* r = NewUi("sr2mods_DronePriceLimitDialog", displayTr, layer);
    if (!r) return failOpen("GameObject creation failed");
    root = r;
    Keep(dialogHandles, r);
    unity::SetActive(r, false);
    Il2CppObject* rootRt = unity::TransformOf(r);
    Rect(rootRt, {0, 0}, {1, 1}, {0.5f, 0.5f}, {0, 0}, {0, 0});
    {
        Il2CppObject* parentCanvas = Call(compInParent, displayTr, {unity::TypeOf(canvasClass), &inactive}, "GetComponentInParent(Canvas)", &ok);
        if (!ok || !unity::IsAlive(parentCanvas)) parentCanvas = nullptr;
        Il2CppObject* canvas = AddComponent(r, canvasClass, "AddComponent(Canvas)");
        if (!canvas) return failOpen("Canvas failed");
        int32_t order = parentCanvas ? Unbox<int32_t>(Call(cvGetOrder, parentCanvas, {}, "Canvas.get_sortingOrder")) : 0;
        int32_t sortLayer = parentCanvas ? Unbox<int32_t>(Call(cvGetLayer, parentCanvas, {}, "Canvas.get_sortingLayerID")) : 0;
        bool over = true;
        order += 20;
        Call(cvSetOverride, canvas, {&over}, "Canvas.set_overrideSorting");
        Call(cvSetLayer, canvas, {&sortLayer}, "Canvas.set_sortingLayerID");
        Call(cvSetOrder, canvas, {&order}, "Canvas.set_sortingOrder");
        if (!AddComponent(r, raycasterClass, "AddComponent(GraphicRaycaster)")) return failOpen("GraphicRaycaster failed");
    }
    // Backdrop: far beyond the display's rect (it is smaller than the screen); a click on it closes.
    if (Il2CppObject* bd = NewUi("Backdrop", rootRt, layer)) {
        Rect(unity::TransformOf(bd), {0, 0}, {1, 1}, {0.5f, 0.5f}, {0, 0}, {4000, 4000});
        Il2CppObject* img = AddComponent(bd, imageClass, "AddComponent(Image)");
        SetColor(img, Color{0.05f, 0.07f, 0.12f, 0.55f});
        blockerToggle = Clickable(bd);
        Keep(dialogHandles, blockerToggle);
    }
    Il2CppObject* panel = NewUi("Panel", rootRt, layer);
    if (!panel) return failOpen("panel creation failed");
    Il2CppObject* prt = unity::TransformOf(panel);
    panelRt = prt;
    Keep(dialogHandles, prt);
    Rect(prt, {0.5f, 0.5f}, {0.5f, 0.5f}, {0.5f, 0.5f}, {0, 0}, {kPanelW, panelH});
    // Border + white content (the menu's ContentBorder / ContentContainer); the content catches clicks.
    Box(prt, "Border", -8, -8, kPanelW + 16, panelH + 16, kLavender, 34.0f);
    if (Il2CppObject* content = Box(prt, "Content", 0, 0, kPanelW, panelH, kWhite, 30.0f)) {
        SetRaycast(sdfRectClass ? unity::GetComponent(content, sdfRectClass) : unity::GetComponent(content, imageClass), true);
        Clickable(content);  // never polled: it only stops clicks on the panel from reaching the menu behind it
    }

    Text(prt, "Title", kPad, titleY, kPanelW - 2 * kPad, 50, "Plort Market price limit", 40.0f, kNavy, kAlignLeft, titleSrc);
    const bool persistent = host.persistent && host.persistent();
    noteText = Text(prt, "Subtitle", kPad, subY, kPanelW - 2 * kPad, 60,
                    std::string("This station's drones only sell a plort while its market price is at or above the "
                                "limit. Below it they keep the plorts and sell once the price recovers.") +
                        (persistent ? "" : " (Not saved: custom_upgrades is off, the limit lasts until the game is closed.)"),
                    22.0f, kNavy, kAlignTopLeft, bodySrc, true);

    // Focus frame (behind the focused row).
    focusFrame = Box(prt, "Focus", kPad - 14, 0, kPanelW - 2 * kPad + 28, kRowH + 16, Color{0, 0, 0, 0}, 20.0f, 4.0f, kTeal);
    Keep(dialogHandles, focusFrame);

    // Mode row: label + two buttons.
    Text(prt, "ModeLabel", kPad, modeRowY, 230, kRowH, "Limit by", 26.0f, kNavy, kAlignLeft, bodySrc);
    const char* modeNames[2] = {"Minimum price", "Fraction of base price"};
    const float modeX[2] = {kPad + 240.0f, kPad + 240.0f + 360.0f};
    for (int i = 0; i < 2; ++i) {
        Il2CppObject* b = NewUi(i == 0 ? "ModePrice" : "ModeFraction", prt, layer);
        if (!b) return failOpen("mode button failed");
        At(unity::TransformOf(b), modeX[i], modeRowY, 340, kRowH);
        modeBg[i] = Shape(b, kBacker, 28.0f);
        SetRaycast(modeBg[i], true);
        modeText[i] = Text(unity::TransformOf(b), "Label", 0, 0, 340, kRowH, modeNames[i], 24.0f, kNavy, kAlignCenter, bodySrc);
        modeToggle[i] = Clickable(b);
        Keep(dialogHandles, modeBg[i]);
        Keep(dialogHandles, modeText[i]);
        Keep(dialogHandles, modeToggle[i]);
    }

    // Base-price cheat sheet.
    if (Il2CppObject* sheet = Box(prt, "BasePrices", sheetX, sheetY, sheetW, sheetH, kBacker, 10.0f)) {
        Il2CppObject* srt = unity::TransformOf(sheet);
        Text(srt, "Header", 16, 10, sheetW - 32, 36, "Base prices (Plort Market)", 26.0f, kNavy, kAlignLeft, titleSrc);
        Il2CppObject* coin = CoinSprite();
        cells.clear();
        for (int i = 0; i < n; ++i) {
            const float cx = 16.0f + (i % cols) * (cellW + kCellGap);
            const float cy = 56.0f + (i / cols) * (kCellH + kCellGap);
            Il2CppObject* cell = Box(srt, "Cell", cx, cy, cellW, kCellH, kWhite, 10.0f);
            if (!cell) continue;
            Il2CppObject* crt = unity::TransformOf(cell);
            Sprite(crt, "Icon", 6, 5, 52, 52, rows[i].icon);
            Text(crt, "Name", 64, 3, cellW - 70, 28, rows[i].name, 20.0f, kNavy, kAlignLeft, bodySrc, false, true);
            float px = 64.0f;
            if (Sprite(crt, "Coin", 64, 33, 24, 24, coin)) px = 92.0f;
            Il2CppObject* price = Text(crt, "Price", px, 30, cellW - px - 6, 30, rows[i].base > 0 ? std::to_string(rows[i].base) : "?",
                                       24.0f, kNavy, kAlignLeft, titleSrc, false, true);
            Keep(dialogHandles, price);
            cells.push_back({price, rows[i].base});
        }
    }

    // Footer: input help + Close.
    Text(prt, "Help", kPad, footY, kPanelW - 2 * kPad - 240, kRowH,
         "Up / Down: choose    Left / Right: change    C or Esc: close", 22.0f, kNavy, kAlignLeft, bodySrc);
    if (Il2CppObject* cb = NewUi("Close", prt, layer)) {
        At(unity::TransformOf(cb), kPanelW - kPad - 220, footY, 220, kRowH);
        SetRaycast(Shape(cb, kCyan, 28.0f), true);
        Text(unity::TransformOf(cb), "Label", 0, 0, 220, kRowH, "Close", 26.0f, kNavy, kAlignCenter, titleSrc);
        closeToggle = Clickable(cb);
        Keep(dialogHandles, closeToggle);
    }
    if (Faulted()) return failOpen("faulted");

    unity::SetActive(r, true);
    g_open.store(true);
    BuildSlider();
    StyleModes();
    SetFocus(focusRow);
    UpdateSheet();
    if (!loggedOpen) {
        loggedOpen = true;
        SR2_INFO("[{}] station ui: dialog opened for station {} ({} plorts, {} columns, price range 0..{}, limit {})",
                 module, station, n, cols, maxPrice, Describe(cur));
    }
    return true;
}

void StationUi::Impl::Close(const char* why) {
    const bool was = g_open.exchange(false);
    if (sliderReady) slider.Destroy();
    if (root && !Faulted()) Destroy(root);
    root = panelRt = blockerToggle = closeToggle = focusFrame = noteText = nullptr;
    modeToggle[0] = modeToggle[1] = modeBg[0] = modeBg[1] = modeText[0] = modeText[1] = nullptr;
    titleSrc = bodySrc = nullptr;
    cells.clear();
    FreeLater(dialogHandles);
    if (was) SR2_INFO("[{}] station ui: dialog closed ({}), station {}: {}", module, why, openStation,
                      host.get ? Describe(host.get(openStation)) : std::string("?"));
}

void StationUi::Impl::BuildSlider() {
    if (!sliderReady || !panelRt) return;
    const bool fraction = mode == LimitMode::kFraction;
    sui::SliderSpec spec;
    spec.name = "DronePriceLimit";
    spec.min = 0;
    spec.max = fraction ? 200 : maxPrice;
    spec.step = fraction ? 5 : (maxPrice <= 200 ? 1 : maxPrice <= 1000 ? 5 : 10);
    spec.value = values[fraction ? 1 : 0];
    spec.label = [fraction](int v) {
        if (v <= 0) return std::string(fraction ? "Minimum fraction of the base price: no limit" : "Minimum price: no limit");
        return fraction ? std::format("Minimum: {}% of the base price", v) : std::format("Minimum price: {} newbucks", v);
    };
    spec.onChange = [this](int v) {
        values[static_cast<int>(mode)] = v;
        Changed();
    };
    sui::SliderHost h;
    h.parent = panelRt;
    h.x = kPad;
    h.y = sliderY;
    h.width = kPanelW - 2 * kPad;
    h.textStyle = bodySrc;
    h.fontSize = 26.0f;
    slider.CreateIn(h, spec);
    slider.SetPolling(focusRow == 1);
}

void StationUi::Impl::Changed() {
    StationLimit l;
    l.mode = mode;
    l.value = values[static_cast<int>(mode)];
    host.set(openStation, l);
    UpdateSheet();
    UpdatePillLabel();
}

void StationUi::Impl::SetMode(LimitMode m) {
    if (m == mode) return;
    if (slider.Active()) values[static_cast<int>(mode)] = slider.Value();
    mode = m;
    BuildSlider();
    StyleModes();
    Changed();
}

void StationUi::Impl::StyleModes() {
    for (int i = 0; i < 2; ++i) {
        const bool sel = static_cast<int>(mode) == i;
        SetColor(modeBg[i], sel ? kTeal : kBacker);
        if (unity::IsAlive(modeText[i])) {
            Color c = sel ? kWhite : kNavy;
            Call(tmpSetColor, modeText[i], {&c}, "TMP_Text.set_color");
        }
    }
}

void StationUi::Impl::SetFocus(int row) {
    focusRow = std::clamp(row, 0, 1);
    if (unity::IsAlive(focusFrame)) {
        const float y = focusRow == 0 ? modeRowY - 8.0f : sliderY - 6.0f;
        const float h = focusRow == 0 ? kRowH + 16.0f : sui::PlotMenuSlider::Height() + 12.0f;
        At(unity::TransformOf(focusFrame), kPad - 14, y, kPanelW - 2 * kPad + 28, h);
    }
    if (sliderReady) slider.SetPolling(focusRow == 1);
}

void StationUi::Impl::UpdateSheet() {
    const int v = values[static_cast<int>(mode)];
    for (const Cell& c : cells) {
        if (!unity::IsAlive(c.price)) continue;
        std::string s = c.base > 0 ? std::to_string(c.base) : "?";
        if (mode == LimitMode::kFraction && v > 0 && c.base > 0) {
            const int lim = static_cast<int>(std::ceil(c.base * (v / 100.0) - 1e-4));
            s += std::format("  <size=75%>min {}</size>", lim);
        }
        SetText(c.price, s);
    }
}

void StationUi::Impl::Navigate(int dir) {
    if (!g_open.load()) return;
    if (dir == kMoveUp) SetFocus(focusRow - 1);
    else if (dir == kMoveDown) SetFocus(focusRow + 1);
    else if (dir == kMoveLeft || dir == kMoveRight) {
        if (focusRow == 0) SetMode(dir == kMoveLeft ? LimitMode::kPrice : LimitMode::kFraction);
        else if (sliderReady) slider.Step(dir == kMoveRight ? 1 : -1);
    }
}

void StationUi::Impl::Submit() {
    if (!g_open.load()) return;
    if (focusRow == 0) SetMode(mode == LimitMode::kPrice ? LimitMode::kFraction : LimitMode::kPrice);
}

// ---------------------------------------------------------------- per frame --

void StationUi::Impl::Tick(float dt) {
    // A menu bound since the last frame: pin it (GC handle) and start over.
    if (Il2CppObject* b = boundMenu) {
        boundMenu = nullptr;
        if (b != menu.p && IsA(b, menuClass) && unity::IsAlive(b)) {
            ForgetMenu();
            menu.p = b;
            menu.h = unity::NewGCHandle(b);
            if (!menu.h) menu.p = nullptr;
        }
        stateCountdown = 0;
    }
    if (!menu.p) return;
    if (!unity::IsAlive(menu.p)) {
        ForgetMenu();
        return;
    }
    if (--stateCountdown <= 0 || g_open.load()) {
        stateCountdown = kStateFrames;
        ReadState();
        const bool want = pageActive && marketSelected && haveStation;
        if (want) EnsurePill();
        if (pill && !unity::IsAlive(pill)) DestroyPill();
        if (pill && want != pillShown) {
            unity::SetActive(pill, want);
            pillShown = want;
            if (want) pillReapply = kReapplyFrames;
        }
        if (pillShown) PlacePill();
        if (pillShown && station != labelStation) {  // the same menu re-bound for another station
            labelStation = station;
            UpdatePillLabel();
        }
        // The page went away (tab switch, picker, destination changed, other station): close.
        if (g_open.load() && (!want || station != openStation || !unity::IsAlive(root))) Close("page changed");
    }
    if (pillShown && pillReapply > 0) {
        --pillReapply;
        ApplyPillLook();
    }

    // Hotkey: C (or the pad's right stick click) toggles the dialog; a click on our prompt opens it.
    const bool hot = (pillShown || g_open.load()) &&
                     (KeyPressed(kbCurrent, kbC) || KeyPressed(padCurrent, padR3));
    const bool clicked = pillShown && Clicked(pillToggle);
    if (g_open.load()) {
        if (hot || Clicked(closeToggle) || Clicked(blockerToggle)) {
            Close(hot ? "C" : "click");
            return;
        }
        for (int i = 0; i < 2; ++i)
            if (Clicked(modeToggle[i])) {
                SetFocus(0);
                SetMode(static_cast<LimitMode>(i));
            }
        if (sliderReady) slider.Tick(dt);
    } else if ((hot || clicked) && pillShown) {
        Open();
    }
}

// ---------------------------------------------------------------- public --

void StationUi::Tick(float dt) {
    if (!impl_ || !main_thread::IsMainThread()) return;
    if (sr2::core::ModuleFaulted(impl_->module)) return;
    impl_->Tick(dt);
}

void StationUi::Shutdown() {
    if (!impl_ || !main_thread::IsMainThread()) return;
    Impl& p = *impl_;
    if (sr2::core::ModuleFaulted(p.module)) {  // no game calls after a fault: forget, leak nothing we can't free
        g_open.store(false);
        p.root = p.pill = nullptr;
        Impl::FreeLater(p.dialogHandles);
        Impl::FreeLater(p.pillHandles);
        Impl::Release(p.menu);
        return;
    }
    p.ForgetMenu();
    p.boundMenu = nullptr;
}

}  // namespace sr2::modules::drone_price_limit
