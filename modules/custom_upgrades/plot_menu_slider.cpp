// custom_upgrades: reusable plot-menu slider (see plot_menu_slider.h). SR2 1.3.0, all lookups by name.
//
// Research (Cpp2IL dump + capstone, RVAs for reference only):
//   * PurchaseItemDetailsContentView.BindData(vm) (0x6D9EA0): _detailsText (LSE, +0x58) gets
//     vm._description; _craftCostView (+0x60) is SetActive(vm._showCost) and bound. _showCost is
//     IsAvailable && NewbuckCost > 0, so the cost row is hidden for OWNED items: its slot is free.
//   * No UnityEngine.UI.Slider prefab is referenced by the plot UI or the options menu (the options
//     ranges are preset steppers); the map's zoom bar is MapZoomSlider (a custom drag handler bound to
//     the MapViewModel, not reusable). The game styles its uGUI sliders with SliderStyler (+0x28
//     _style : SliderStyle, +0x30 _slider): OnEnable (0x6CB430) = _slider = GetComponent<Slider>();
//     ApplyStyle() (0x6CAC90): transform.Find("Background").Image <- bgSprite / bgColor,
//     slider.fillRect.Image <- fillSprite / fillColor, slider.handleRect.Image <- handleSprite /
//     handleColor, slider.colors <- normal / highlighted / pressed / disabled tints (each setting
//     only when its override flag is set). So a standard uGUI slider hierarchy + SliderStyler with a
//     loaded SliderStyle asset gets the game's own slider look.
//   * Selectable navigation: Navigation {m_Mode +0, m_WrapAround +4, 4 x Selectable} - all zero =
//     Mode.None. Slider.OnMove still steps left / right itself while it is the EventSystem selection.

#include "modules/custom_upgrades/plot_menu_slider.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <format>
#include <initializer_list>
#include <string_view>
#include <vector>

#include "core/detour_guard.h"
#include "core/il2cpp_api.h"
#include "core/log.h"
#include "core/main_thread.h"
#include "core/resolver.h"
#include "core/unity.h"

namespace sr2::modules::custom_upgrades::ui {
namespace {

namespace unity = sr2::core::unity;
namespace main_thread = sr2::core::main_thread;
using sr2::il2cpp::api;
using sr2::il2cpp::Resolver;

constexpr const char* kImage = "Assembly-CSharp";
constexpr const char* kCore = "UnityEngine.CoreModule";
constexpr const char* kUi = "UnityEngine.UI";
constexpr const char* kTmp = "Unity.TextMeshPro";
constexpr const char* kInput = "Unity.InputSystem";

constexpr float kLabelHeight = 34.0f;
constexpr float kBarHeight = 28.0f;
constexpr float kGap = 6.0f;
constexpr float kTotalHeight = kLabelHeight + kGap + kBarHeight;
constexpr float kColumnGap = 28.0f;    // between sliders side by side
constexpr float kColumnFontScale = 0.85f;  // label size in a shared row (half the width)
constexpr float kRepeatDelay = 0.40f;  // seconds before a held direction repeats
constexpr float kRepeatRate = 0.07f;   // seconds between repeats
constexpr uint64_t kSamePressMs = 150; // a navigation event and our poll this close belong to one press
constexpr int kImageSliced = 1;
constexpr int kTmpAlignLeft = 513;     // TMPro.TextAlignmentOptions.Left
constexpr int kFindExcludeInactive = 0;

struct Vec2 {
    float x, y;
};
struct Color {
    float r, g, b, a;
};

}  // namespace

struct PlotMenuSlider::Impl {
    const char* module = "custom_upgrades";

    // classes
    Il2CppClass* goClass = nullptr;
    Il2CppClass* rtClass = nullptr;
    Il2CppClass* imageClass = nullptr;
    Il2CppClass* sliderClass = nullptr;
    Il2CppClass* layoutElemClass = nullptr;
    Il2CppClass* layoutGroupClass = nullptr;
    Il2CppClass* tmpTextClass = nullptr;
    Il2CppClass* tmpUguiClass = nullptr;
    Il2CppClass* contentViewClass = nullptr;
    Il2CppClass* styleClass = nullptr;   // SliderStyle (optional)
    Il2CppClass* stylerClass = nullptr;  // SliderStyler (optional)
    Il2CppClass* popupClass = nullptr;   // PopupPrompt (optional: input blocker)
    Il2CppClass* canvasClass = nullptr;  // UnityEngine.Canvas
    Il2CppClass* raycasterClass = nullptr;  // UnityEngine.UI.GraphicRaycaster
    Il2CppClass* sdfRectClass = nullptr;    // SDFRoundedRectangle (optional: the game's UI shapes)
    size_t offDetailsText = 0, offCraftCost = 0, offStylerStyle = 0;

    // methods (all through Resolver::Invoke)
    const MethodInfo* goCtor = nullptr;          // GameObject..ctor(string)
    const MethodInfo* goAddComponent = nullptr;  // GameObject.AddComponent(Type)
    const MethodInfo* goGetLayer = nullptr;
    const MethodInfo* goSetLayer = nullptr;
    const MethodInfo* trSetParent = nullptr;     // Transform.SetParent(Transform, bool)
    const MethodInfo* trGetParent = nullptr;
    const MethodInfo* trGetSibling = nullptr;
    const MethodInfo* trSetSibling = nullptr;
    const MethodInfo* rtSetAnchorMin = nullptr, *rtSetAnchorMax = nullptr, *rtSetPivot = nullptr,
                    *rtSetSize = nullptr, *rtSetPos = nullptr;
    const MethodInfo* rtGetAnchorMin = nullptr, *rtGetAnchorMax = nullptr, *rtGetPivot = nullptr,
                    *rtGetSize = nullptr, *rtGetPos = nullptr;
    const MethodInfo* objDestroy = nullptr;       // Object.Destroy(Object)
    const MethodInfo* imgSetType = nullptr;       // Image.set_type
    const MethodInfo* gfxSetColor = nullptr;      // Graphic.set_color
    const MethodInfo* gfxSetRaycast = nullptr;    // Graphic.set_raycastTarget
    const MethodInfo* sldSetFill = nullptr, *sldSetHandle = nullptr, *sldSetDir = nullptr, *sldSetMin = nullptr,
                    *sldSetMax = nullptr, *sldSetWhole = nullptr, *sldGetValue = nullptr, *sldSetValueQuiet = nullptr;
    const MethodInfo* selSetTarget = nullptr;     // Selectable.set_targetGraphic
    const MethodInfo* selSetNav = nullptr;        // Selectable.set_navigation
    const MethodInfo* selInteractable = nullptr;  // Selectable.IsInteractable()
    const MethodInfo* leSetPrefH = nullptr, *leSetMinH = nullptr, *leSetFlexW = nullptr;
    const MethodInfo* tmpSetText = nullptr, *tmpGetFont = nullptr, *tmpSetFont = nullptr, *tmpGetMat = nullptr,
                    *tmpSetMat = nullptr, *tmpGetSize = nullptr, *tmpSetSize = nullptr, *tmpGetColor = nullptr,
                    *tmpSetColor = nullptr, *tmpSetAlign = nullptr, *tmpSetWrap = nullptr;
    const MethodInfo* findAny = nullptr;          // Object.FindAnyObjectByType(Type, FindObjectsInactive)
    const MethodInfo* compInParent = nullptr;     // Component.GetComponentInParent(Type, bool)
    const MethodInfo* cvGetOrder = nullptr, *cvSetOrder = nullptr, *cvGetLayer = nullptr, *cvSetLayer = nullptr,
                    *cvSetOverride = nullptr;
    const MethodInfo* sdfSetRadius = nullptr, *sdfSetBorder = nullptr, *sdfSetBorderColor = nullptr;
    // EventSystem (optional: selection hand-back)
    const MethodInfo* esCurrent = nullptr, *esGetSelected = nullptr, *esSetSelected = nullptr;
    // Input System (optional: gamepad / keyboard steps)
    const MethodInfo* padCurrent = nullptr, *padDpad = nullptr, *padStick = nullptr, *dpadLeft = nullptr,
                    *dpadRight = nullptr, *stickLeft = nullptr, *stickRight = nullptr, *btnPressed = nullptr;
    const MethodInfo* kbCurrent = nullptr, *kbLeft = nullptr, *kbRight = nullptr;
    const MethodInfo* padRStick = nullptr, *kbShift = nullptr;  // SliderInput::kSecondary
    const MethodInfo* mouseCurrent = nullptr, *mouseLeft = nullptr;
    // spec.selectToStep: Selectable.IsPressed() / get_isPointerInside (the mouse on the slider), a press
    // anywhere (ButtonControl.get_wasPressedThisFrame)
    const MethodInfo* selIsPressed = nullptr, *selPointerInside = nullptr, *btnPressedThisFrame = nullptr;
    bool inputOk = false, selectionOk = false;

    using WBarrierFn = void (*)(Il2CppObject*, void**, void*);
    using ObjectNewFn = Il2CppObject* (*)(Il2CppClass*);
    WBarrierFn wbarrier = nullptr;
    ObjectNewFn objectNew = nullptr;

    // session assets
    bool styleTried = false;
    Il2CppObject* style = nullptr;  // pinned (never freed)

    // the live slider (main thread only)
    Il2CppObject* row = nullptr;      // GameObject of the shared row this slider built (ownsRow), else null
    bool ownsRow = false;
    Il2CppObject* root = nullptr;     // GameObject
    Il2CppObject* slider = nullptr;   // UnityEngine.UI.Slider
    Il2CppObject* sliderGo = nullptr; // its GameObject
    Il2CppObject* label = nullptr;    // TMP_Text (may be null)
    Color labelColor{0.259f, 0.310f, 0.451f, 1.0f};  // the panel's description colour (game default)
    bool externalSteps = false;       // left / right come from the menu's navigation (Step), not polling
    bool polling = true;              // pad / keyboard polled in Tick (SetPolling)
    bool styled = false;              // the last build got a SliderStyler
    std::vector<Il2CppGCHandle> handles;
    SliderSpec spec;
    int value = 0;
    // input repeat
    int heldDir = 0;
    int blockedDir = 0;  // direction held since a press was refused (popup / not interactable)
    float heldTime = 0.0f, nextRepeat = 0.0f;
    // steps from the menu's navigation events (Step): the polled press they belong to is not stepped twice
    bool navDriven = false;            // the held press is stepped by navigation events (no poll repeats)
    int lastNavDir = 0;
    uint64_t lastNavMs = 0, lastPollStepMs = 0;
    bool loggedNav = false, loggedBlocked = false;
    // spec.selectToStep
    bool focused = false;         // selected: keys step it
    bool focusFromMouse = false;  // selected by a mouse press: released when the mouse leaves it
    bool focusGained = false;     // TakeFocusGained
    int pendingClick = 0;         // frames until a mouse press elsewhere is judged (EventSystem order)
    bool loggedFocus = false;
    // EventSystem selection to hand back after a mouse drag
    Il2CppObject* prevSel = nullptr;
    Il2CppGCHandle prevSelHandle = nullptr;
    bool loggedBuild = false, loggedFail = false;

    // ---- helpers ----
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
            else SR2_WARN("[{}] slider: {} failed: {}", module, what, err);
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
    bool Faulted() const { return sr2::core::ModuleFaulted(module); }
    void Keep(Il2CppObject* o) {
        if (Il2CppGCHandle h = o ? unity::NewGCHandle(o) : nullptr) handles.push_back(h);
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
    // New UI GameObject (RectTransform) under `parentTr`, on the parent's layer. Returns the GameObject.
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
    Il2CppObject* AddImage(Il2CppObject* go, Color c, bool sliced) {
        Il2CppObject* img = AddComponent(go, imageClass, "AddComponent(Image)");
        if (!img) return nullptr;
        Call(gfxSetColor, img, {&c}, "Graphic.set_color");
        if (sliced) {
            int32_t t = kImageSliced;
            Call(imgSetType, img, {&t}, "Image.set_type");
        }
        return img;
    }
    // The game's own UI shape (SDFRoundedRectangle: makes its SDF material from Shader.Find("SDFUI/...")),
    // else a plain Image.
    Il2CppObject* AddShape(Il2CppObject* go, Color c, float radius, float border = 0.0f, Color borderColor = {}) {
        if (sdfRectClass) {
            if (Il2CppObject* g = AddComponent(go, sdfRectClass, "AddComponent(SDFRoundedRectangle)")) {
                Call(gfxSetColor, g, {&c}, "Graphic.set_color");
                if (sdfSetRadius) Call(sdfSetRadius, g, {&radius}, "SDFRoundedRectangle.SetCornerRadius");
                if (border > 0.0f && sdfSetBorder && sdfSetBorderColor) {
                    Call(sdfSetBorder, g, {&border}, "SDFGraphic.set_BorderSize");
                    Call(sdfSetBorderColor, g, {&borderColor}, "SDFGraphic.set_BorderColor");
                }
                return g;
            }
        }
        return AddImage(go, c, false);
    }
    static uint64_t NowMs() { return GetTickCount64(); }
    bool ShiftHeld() {
        if (!kbCurrent || !kbShift) return false;
        bool ok = false;
        Il2CppObject* kb = Call(kbCurrent, nullptr, {}, "Keyboard.get_current", &ok);
        return ok && kb && Pressed(kbShift, kb);
    }
    // This press / event is for this slider's keys (SliderInput): Shift held <=> kSecondary. A selected
    // slider (selectToStep) takes the keys whatever Shift says.
    bool OwnInput() {
        if (spec.selectToStep) return focused;
        return (spec.input == SliderInput::kSecondary) == ShiftHeld();
    }
    bool PrimaryKeys() const { return spec.input == SliderInput::kPrimary || spec.selectToStep; }
    void FocusTick();
    void SetFocus(bool on, bool fromMouse, const char* why);
    void PlaceInSlot(Il2CppObject* go, Il2CppObject* craftTr, bool layout);
    bool AddRaycaster(Il2CppObject* go, Il2CppObject* parentTr);
    bool InputBlocked();
    bool Pressed(const MethodInfo* getControl, Il2CppObject* device) {
        if (!device || !getControl) return false;
        bool ok = false;
        Il2CppObject* ctl = Call(getControl, device, {}, "InputControl getter", &ok);
        if (!ok || !ctl) return false;
        return Unbox<bool>(Call(btnPressed, ctl, {}, "ButtonControl.get_isPressed", &ok)) && ok;
    }
    Il2CppObject* SliderStyle();
    const char* BuildBody(Il2CppObject* root, Il2CppObject* rootRt, int layer, Il2CppObject* labelSrc, float fontSize,
                          float fontScale);
    std::string LabelText(int v) const {
        std::string s = spec.label ? spec.label(v) : std::to_string(v);
        return spec.selectToStep && focused ? "< " + s + " >" : s;  // selected: the arrows step it
    }
    int Snap(int v) const {
        const int step = std::max(spec.step, 1);
        v = std::clamp(v, spec.min, spec.max);
        v = spec.min + static_cast<int>(std::lround(static_cast<double>(v - spec.min) / step)) * step;
        return std::clamp(v, spec.min, spec.max);
    }
    void SetLabel(int v) {
        if (unity::IsAlive(label)) Call(tmpSetText, label, {unity::NewString(LabelText(v))}, "TMP_Text.set_text");
    }
    void SetSliderValue(int v) {
        float f = static_cast<float>(v);
        if (unity::IsAlive(slider)) Call(sldSetValueQuiet, slider, {&f}, "Slider.SetValueWithoutNotify");
    }
    void Apply(int v, bool notify) {
        if (v == value) return;
        value = v;
        SetLabel(v);
        if (notify && spec.onChange) spec.onChange(v);
    }
    int InputDirection();
    void HandBackSelection();
    void ForgetSelection() {
        prevSel = nullptr;
        if (Il2CppGCHandle h = prevSelHandle) {
            prevSelHandle = nullptr;
            main_thread::Post([h] { unity::FreeGCHandle(h); });
        }
    }
    void Clear() {
        root = slider = sliderGo = label = row = nullptr;
        ownsRow = false;
        std::vector<Il2CppGCHandle> doomed;
        doomed.swap(handles);
        if (!doomed.empty()) main_thread::Post([doomed] {
            for (Il2CppGCHandle h : doomed) unity::FreeGCHandle(h);
        });
        ForgetSelection();
        heldDir = blockedDir = 0;
        navDriven = false;
        focused = focusFromMouse = focusGained = false;
        pendingClick = 0;
        spec = {};
    }
};

PlotMenuSlider::PlotMenuSlider() = default;
PlotMenuSlider::~PlotMenuSlider() = default;  // Impl lives for the process (handles freed by jobs)

// ---------------------------------------------------------------- setup --

bool PlotMenuSlider::Setup(Resolver& R, const char* module, std::string& why) {
    auto* p = new Impl();
    p->module = module;
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
    auto off = [&](Il2CppClass* k, const char* field, size_t& out) {
        std::string err;
        for (Il2CppClass* c = k; c; c = parentOf(c))
            if (auto o = R.FieldOffset(c, field, &err)) {
                out = *o;
                return;
            }
        if (why.empty()) why = std::format("field {} not found", field);
    };

    HMODULE ga = api().module;
    p->wbarrier = reinterpret_cast<Impl::WBarrierFn>(ga ? GetProcAddress(ga, "il2cpp_gc_wbarrier_set_field") : nullptr);
    p->objectNew = reinterpret_cast<Impl::ObjectNewFn>(ga ? GetProcAddress(ga, "il2cpp_object_new") : nullptr);
    if (!p->objectNew) why = "il2cpp_object_new export missing";

    p->goClass = cls(kCore, "UnityEngine", "GameObject");
    Il2CppClass* transform = cls(kCore, "UnityEngine", "Transform");
    Il2CppClass* component = cls(kCore, "UnityEngine", "Component");
    p->rtClass = cls(kCore, "UnityEngine", "RectTransform");
    Il2CppClass* uobject = cls(kCore, "UnityEngine", "Object");
    p->imageClass = cls(kUi, "UnityEngine.UI", "Image");
    Il2CppClass* graphic = cls(kUi, "UnityEngine.UI", "Graphic");
    p->sliderClass = cls(kUi, "UnityEngine.UI", "Slider");
    Il2CppClass* selectable = cls(kUi, "UnityEngine.UI", "Selectable");
    p->layoutElemClass = cls(kUi, "UnityEngine.UI", "LayoutElement");
    p->layoutGroupClass = cls(kUi, "UnityEngine.UI", "LayoutGroup");
    p->tmpTextClass = cls(kTmp, "TMPro", "TMP_Text");
    p->tmpUguiClass = cls(kTmp, "TMPro", "TextMeshProUGUI");
    p->contentViewClass = cls(kImage, "MonomiPark.SlimeRancher.UI.Purchase", "PurchaseItemDetailsContentView");
    if (!why.empty()) {
        delete p;
        return false;
    }
    off(p->contentViewClass, "_detailsText", p->offDetailsText);
    off(p->contentViewClass, "_craftCostView", p->offCraftCost);
    p->goCtor = byParams(p->goClass, ".ctor", {"System.String"});
    p->goAddComponent = byParams(p->goClass, "AddComponent", {"System.Type"});
    p->goGetLayer = method(p->goClass, "get_layer", 0);
    p->goSetLayer = method(p->goClass, "set_layer", 1);
    p->trSetParent = byParams(transform, "SetParent", {"UnityEngine.Transform", "System.Boolean"});
    p->trGetParent = method(transform, "get_parent", 0);
    p->trGetSibling = method(transform, "GetSiblingIndex", 0);
    p->trSetSibling = method(transform, "SetSiblingIndex", 1);
    p->rtSetAnchorMin = method(p->rtClass, "set_anchorMin", 1);
    p->rtSetAnchorMax = method(p->rtClass, "set_anchorMax", 1);
    p->rtSetPivot = method(p->rtClass, "set_pivot", 1);
    p->rtSetSize = method(p->rtClass, "set_sizeDelta", 1);
    p->rtSetPos = method(p->rtClass, "set_anchoredPosition", 1);
    p->rtGetAnchorMin = method(p->rtClass, "get_anchorMin", 0);
    p->rtGetAnchorMax = method(p->rtClass, "get_anchorMax", 0);
    p->rtGetPivot = method(p->rtClass, "get_pivot", 0);
    p->rtGetSize = method(p->rtClass, "get_sizeDelta", 0);
    p->rtGetPos = method(p->rtClass, "get_anchoredPosition", 0);
    p->objDestroy = byParams(uobject, "Destroy", {"UnityEngine.Object"});
    p->imgSetType = method(p->imageClass, "set_type", 1);
    p->gfxSetColor = method(graphic, "set_color", 1);
    p->gfxSetRaycast = method(graphic, "set_raycastTarget", 1);
    p->sldSetFill = method(p->sliderClass, "set_fillRect", 1);
    p->sldSetHandle = method(p->sliderClass, "set_handleRect", 1);
    p->sldSetDir = method(p->sliderClass, "set_direction", 1);
    p->sldSetMin = method(p->sliderClass, "set_minValue", 1);
    p->sldSetMax = method(p->sliderClass, "set_maxValue", 1);
    p->sldSetWhole = method(p->sliderClass, "set_wholeNumbers", 1);
    p->sldGetValue = method(p->sliderClass, "get_value", 0);
    p->sldSetValueQuiet = method(p->sliderClass, "SetValueWithoutNotify", 1);
    p->selSetTarget = method(selectable, "set_targetGraphic", 1);
    p->selSetNav = method(selectable, "set_navigation", 1);
    p->selInteractable = method(selectable, "IsInteractable", 0);
    p->leSetPrefH = method(p->layoutElemClass, "set_preferredHeight", 1);
    p->leSetMinH = method(p->layoutElemClass, "set_minHeight", 1);
    p->leSetFlexW = method(p->layoutElemClass, "set_flexibleWidth", 1);
    p->tmpSetText = method(p->tmpTextClass, "set_text", 1);
    p->tmpGetFont = method(p->tmpTextClass, "get_font", 0);
    p->tmpSetFont = method(p->tmpTextClass, "set_font", 1);
    p->tmpGetMat = method(p->tmpTextClass, "get_fontSharedMaterial", 0);
    p->tmpSetMat = method(p->tmpTextClass, "set_fontSharedMaterial", 1);
    p->tmpGetSize = method(p->tmpTextClass, "get_fontSize", 0);
    p->tmpSetSize = method(p->tmpTextClass, "set_fontSize", 1);
    p->tmpGetColor = method(p->tmpTextClass, "get_color", 0);
    p->tmpSetColor = method(p->tmpTextClass, "set_color", 1);
    p->tmpSetAlign = method(p->tmpTextClass, "set_alignment", 1);
    if (!why.empty()) {
        delete p;
        return false;
    }

    // ---- optional parts ----
    std::string report;
    {
        std::string err;
        p->tmpSetWrap = R.Method(p->tmpTextClass, "set_textWrappingMode", 1, &err);
        p->findAny = byParams(uobject, "FindAnyObjectByType", {"System.Type", "UnityEngine.FindObjectsInactive"}, false);
        p->popupClass = R.Class(kImage, "MonomiPark.SlimeRancher.UI.Popup", "PopupPrompt", &err);
        // Pointer input: the details panel (PurchaseItemDetailsContent) is a nested Canvas WITHOUT a
        // GraphicRaycaster, so the menu's raycaster never sees graphics below it. Our root gets its
        // own Canvas (sorted just above the panel's) + GraphicRaycaster.
        p->canvasClass = R.Class("UnityEngine.UIModule", "UnityEngine", "Canvas", &err);
        p->raycasterClass = R.Class(kUi, "UnityEngine.UI", "GraphicRaycaster", &err);
        p->compInParent = byParams(component, "GetComponentInParent", {"System.Type", "System.Boolean"}, false);
        if (p->canvasClass) {
            p->cvGetOrder = R.Method(p->canvasClass, "get_sortingOrder", 0, &err);
            p->cvSetOrder = R.Method(p->canvasClass, "set_sortingOrder", 1, &err);
            p->cvGetLayer = R.Method(p->canvasClass, "get_sortingLayerID", 0, &err);
            p->cvSetLayer = R.Method(p->canvasClass, "set_sortingLayerID", 1, &err);
            p->cvSetOverride = R.Method(p->canvasClass, "set_overrideSorting", 1, &err);
        }
        if (!p->canvasClass || !p->raycasterClass || !p->compInParent || !p->cvGetOrder || !p->cvSetOrder ||
            !p->cvGetLayer || !p->cvSetLayer || !p->cvSetOverride) {
            p->raycasterClass = nullptr;
            report += " own raycaster: NOT available (mouse may not reach the slider);";
        }
        // The game's UI shapes (rounded rectangles drawn with an SDF shader).
        p->sdfRectClass = R.Class(kImage, "MonomiPark.SlimeRancher.UI.SDFUI", "SDFRoundedRectangle", &err);
        if (p->sdfRectClass) {
            p->sdfSetRadius = R.MethodByParams(p->sdfRectClass, "SetCornerRadius", {"System.Single"}, &err);
            Il2CppClass* sdf = parentOf(p->sdfRectClass);  // SDFGraphic
            p->sdfSetBorder = sdf ? R.Method(sdf, "set_BorderSize", 1, &err) : nullptr;
            p->sdfSetBorderColor = sdf ? R.Method(sdf, "set_BorderColor", 1, &err) : nullptr;
            if (!p->sdfSetRadius) p->sdfRectClass = nullptr;
        }
        if (!p->sdfRectClass) report += " SDF shapes: NOT available (plain images);";
        p->styleClass = R.Class(kImage, "MonomiPark.SlimeRancher.UI.UIStyling", "SliderStyle", &err);
        p->stylerClass = R.Class(kImage, "MonomiPark.SlimeRancher.UI.UIStyling", "SliderStyler", &err);
        if (p->stylerClass) {
            if (auto o = R.FieldOffset(p->stylerClass, "_style", &err)) p->offStylerStyle = *o;
        }
        if (!p->styleClass || !p->stylerClass || !p->offStylerStyle) {
            p->styleClass = p->stylerClass = nullptr;
            report += " game slider style: NOT available (plain colours);";
        }
        Il2CppClass* es = R.Class(kUi, "UnityEngine.EventSystems", "EventSystem", &err);
        p->esCurrent = es ? R.Method(es, "get_current", 0, &err) : nullptr;
        p->esGetSelected = es ? R.Method(es, "get_currentSelectedGameObject", 0, &err) : nullptr;
        p->esSetSelected = es ? R.MethodByParams(es, "SetSelectedGameObject", {"UnityEngine.GameObject"}, &err) : nullptr;
        Il2CppClass* mouse = R.Class(kInput, "UnityEngine.InputSystem", "Mouse", &err);
        p->mouseCurrent = mouse ? R.Method(mouse, "get_current", 0, &err) : nullptr;
        p->mouseLeft = mouse ? R.Method(mouse, "get_leftButton", 0, &err) : nullptr;
        Il2CppClass* button = R.Class(kInput, "UnityEngine.InputSystem.Controls", "ButtonControl", &err);
        p->btnPressed = button ? R.Method(button, "get_isPressed", 0, &err) : nullptr;
        p->btnPressedThisFrame = button ? R.Method(button, "get_wasPressedThisFrame", 0, &err) : nullptr;
        if (Il2CppClass* sel = R.Class(kUi, "UnityEngine.UI", "Selectable", &err)) {
            p->selIsPressed = R.Method(sel, "IsPressed", 0, &err);              // protected
            p->selPointerInside = R.Method(sel, "get_isPointerInside", 0, &err);  // private
        }
        if (!p->selIsPressed || !p->selPointerInside || !p->btnPressedThisFrame)
            report += " select by mouse: NOT available (Submit selects);";
        p->selectionOk = p->esCurrent && p->esGetSelected && p->esSetSelected && p->mouseCurrent && p->mouseLeft &&
                         p->btnPressed;
        if (!p->selectionOk) report += " selection hand-back: NOT available;";

        Il2CppClass* pad = R.Class(kInput, "UnityEngine.InputSystem", "Gamepad", &err);
        Il2CppClass* dpad = R.Class(kInput, "UnityEngine.InputSystem.Controls", "DpadControl", &err);
        Il2CppClass* stick = R.Class(kInput, "UnityEngine.InputSystem.Controls", "StickControl", &err);
        Il2CppClass* kb = R.Class(kInput, "UnityEngine.InputSystem", "Keyboard", &err);
        p->padCurrent = pad ? R.Method(pad, "get_current", 0, &err) : nullptr;
        p->padDpad = pad ? R.Method(pad, "get_dpad", 0, &err) : nullptr;
        p->padStick = pad ? R.Method(pad, "get_leftStick", 0, &err) : nullptr;
        p->dpadLeft = dpad ? R.Method(dpad, "get_left", 0, &err) : nullptr;
        p->dpadRight = dpad ? R.Method(dpad, "get_right", 0, &err) : nullptr;
        p->stickLeft = stick ? R.Method(stick, "get_left", 0, &err) : nullptr;
        p->stickRight = stick ? R.Method(stick, "get_right", 0, &err) : nullptr;
        p->kbCurrent = kb ? R.Method(kb, "get_current", 0, &err) : nullptr;
        p->kbLeft = kb ? R.Method(kb, "get_leftArrowKey", 0, &err) : nullptr;
        p->kbRight = kb ? R.Method(kb, "get_rightArrowKey", 0, &err) : nullptr;
        p->padRStick = pad ? R.Method(pad, "get_rightStick", 0, &err) : nullptr;
        p->kbShift = kb ? R.Method(kb, "get_shiftKey", 0, &err) : nullptr;
        if (!p->padRStick || !p->kbShift) report += " second slider keys (right stick / Shift + arrows): partly NOT available;";
        p->inputOk = p->btnPressed && ((p->padCurrent && p->padDpad && p->dpadLeft && p->dpadRight) ||
                                       (p->kbCurrent && p->kbLeft && p->kbRight));
        if (!p->inputOk) report += " gamepad / keyboard steps: NOT available;";
    }
    impl_ = p;
    ready_ = true;
    SR2_INFO("[{}] plot menu slider ready (details view +0x{:X} / +0x{:X}){}", module, p->offDetailsText, p->offCraftCost,
             report.empty() ? "" : std::string(";") + report);
    return true;
}

// ---------------------------------------------------------------- building --

Il2CppObject* PlotMenuSlider::Impl::SliderStyle() {
    if (styleTried) return unity::IsAlive(style) ? style : nullptr;
    styleTried = true;
    if (!styleClass) return nullptr;
    // One-time scan (first slider of the session): the loaded SliderStyle assets.
    std::vector<Il2CppObject*> all = unity::FindObjectsOfTypeAll(styleClass);
    std::string names;
    for (Il2CppObject* s : all) {
        if (!unity::IsAlive(s) || !IsA(s, styleClass)) continue;
        const std::string n = unity::GetName(s);
        names += (names.empty() ? "" : ", ") + n;
        if (!style) style = s;
    }
    if (style) {
        unity::NewGCHandle(style);  // pinned for the session (never freed)
        SR2_INFO("[{}] slider: game SliderStyle '{}' used (loaded: {})", module, unity::GetName(style), names);
    } else {
        SR2_INFO("[{}] slider: no SliderStyle asset loaded - plain colours", module);
    }
    return style;
}

// The cost row's slot for `go` (a slider root, or the row of sliders side by side): with a LayoutGroup
// a LayoutElement right after the cost row, else the cost row's RectTransform, else the panel bottom.
void PlotMenuSlider::Impl::PlaceInSlot(Il2CppObject* go, Il2CppObject* craftTr, bool layout) {
    Il2CppObject* rt = unity::TransformOf(go);
    bool ok = false;
    if (layout) {
        Il2CppObject* le = AddComponent(go, layoutElemClass, "AddComponent(LayoutElement)");
        float h = kTotalHeight, flex = 1.0f;
        if (le) {
            Call(leSetPrefH, le, {&h}, "LayoutElement.set_preferredHeight");
            Call(leSetMinH, le, {&h}, "LayoutElement.set_minHeight");
            Call(leSetFlexW, le, {&flex}, "LayoutElement.set_flexibleWidth");
        }
        if (craftTr) {
            int32_t idx = Unbox<int32_t>(Call(trGetSibling, craftTr, {}, "Transform.GetSiblingIndex", &ok), -1);
            if (ok && idx >= 0) {
                ++idx;
                Call(trSetSibling, rt, {&idx}, "Transform.SetSiblingIndex");
            }
        }
    } else if (craftTr && IsA(craftTr, rtClass)) {
        const Vec2 amin = GetVec(rtGetAnchorMin, craftTr, "get_anchorMin");
        const Vec2 amax = GetVec(rtGetAnchorMax, craftTr, "get_anchorMax");
        const Vec2 piv = GetVec(rtGetPivot, craftTr, "get_pivot");
        const Vec2 pos = GetVec(rtGetPos, craftTr, "get_anchoredPosition");
        Vec2 size = GetVec(rtGetSize, craftTr, "get_sizeDelta");
        if (amin.y == amax.y) size.y = std::max(size.y, kTotalHeight);  // fixed height: at least ours
        Rect(rt, amin, amax, piv, pos, size);
    } else {
        Rect(rt, {0, 0}, {1, 0}, {0.5f, 0}, {0, 16}, {-32, kTotalHeight});
    }
}

// Pointer input: own Canvas + GraphicRaycaster on `go` (the panel's nested Canvas has no raycaster,
// so nothing below it is ever hit). Sorted one above the panel's canvas so our hits win over the
// menu's own graphics behind it.
bool PlotMenuSlider::Impl::AddRaycaster(Il2CppObject* go, Il2CppObject* parentTr) {
    if (!raycasterClass) return false;
    bool inactive = true, ok = false;
    Il2CppObject* parentCanvas =
        Call(compInParent, parentTr, {unity::TypeOf(canvasClass), &inactive}, "GetComponentInParent(Canvas)", &ok);
    if (!ok || !unity::IsAlive(parentCanvas) || !IsA(parentCanvas, canvasClass)) parentCanvas = nullptr;
    Il2CppObject* canvas = AddComponent(go, canvasClass, "AddComponent(Canvas)");
    if (!canvas) return false;
    int32_t order = parentCanvas ? Unbox<int32_t>(Call(cvGetOrder, parentCanvas, {}, "Canvas.get_sortingOrder")) : 0;
    int32_t sortLayer = parentCanvas ? Unbox<int32_t>(Call(cvGetLayer, parentCanvas, {}, "Canvas.get_sortingLayerID")) : 0;
    bool over = true;
    ++order;
    Call(cvSetOverride, canvas, {&over}, "Canvas.set_overrideSorting");
    Call(cvSetLayer, canvas, {&sortLayer}, "Canvas.set_sortingLayerID");
    Call(cvSetOrder, canvas, {&order}, "Canvas.set_sortingOrder");
    return AddComponent(go, raycasterClass, "AddComponent(GraphicRaycaster)") != nullptr;
}

// Label + uGUI slider under the (inactive) root; activates it. nullptr = ok, else what failed.
const char* PlotMenuSlider::Impl::BuildBody(Il2CppObject* r, Il2CppObject* rootRt, int layer, Il2CppObject* labelSrc,
                                            float fontSize, float fontScale) {
    // ---- label ----
    Il2CppObject* labelGo = NewUi("Label", rootRt, layer);
    if (labelGo) {
        Rect(unity::TransformOf(labelGo), {0, 1}, {1, 1}, {0.5f, 1}, {0, 0}, {0, kLabelHeight});
        Il2CppObject* tmp = AddComponent(labelGo, tmpUguiClass, "AddComponent(TextMeshProUGUI)");
        if (tmp) {
            Keep(tmp);
            label = tmp;
            Il2CppObject* src = unity::IsAlive(labelSrc) ? labelSrc : nullptr;
            float size = 26.0f;
            Color col = labelColor;
            if (src) {
                if (Il2CppObject* font = Call(tmpGetFont, src, {}, "TMP_Text.get_font"); unity::IsAlive(font))
                    Call(tmpSetFont, tmp, {font}, "TMP_Text.set_font");
                if (Il2CppObject* mat = Call(tmpGetMat, src, {}, "TMP_Text.get_fontSharedMaterial"); unity::IsAlive(mat))
                    Call(tmpSetMat, tmp, {mat}, "TMP_Text.set_fontSharedMaterial");
                const float s = Unbox<float>(Call(tmpGetSize, src, {}, "TMP_Text.get_fontSize"));
                if (std::isfinite(s) && s >= 10.0f && s <= 80.0f) size = s;
                col = Unbox<Color>(Call(tmpGetColor, src, {}, "TMP_Text.get_color"), col);
            }
            labelColor = col;
            if (fontSize > 0.0f) size = fontSize;
            size *= fontScale;
            Call(tmpSetSize, tmp, {&size}, "TMP_Text.set_fontSize");
            Call(tmpSetColor, tmp, {&col}, "TMP_Text.set_color");
            int32_t align = kTmpAlignLeft, noWrap = 0;
            Call(tmpSetAlign, tmp, {&align}, "TMP_Text.set_alignment");
            if (tmpSetWrap) Call(tmpSetWrap, tmp, {&noWrap}, "TMP_Text.set_textWrappingMode");
            bool off = false;
            Call(gfxSetRaycast, tmp, {&off}, "Graphic.set_raycastTarget");
            SetLabel(value);
        }
    }

    // ---- slider: standard uGUI hierarchy (names are what SliderStyler looks for) ----
    Il2CppObject* sGo = NewUi("Slider", rootRt, layer);
    if (!sGo) return "slider GameObject creation failed";
    Il2CppObject* sRt = unity::TransformOf(sGo);
    Rect(sRt, {0, 0}, {1, 0}, {0.5f, 0}, {0, 0}, {-8, kBarHeight});
    Il2CppObject* bgGo = NewUi("Background", sRt, layer);
    Il2CppObject* fillArea = NewUi("Fill Area", sRt, layer);
    Il2CppObject* fillGo = fillArea ? NewUi("Fill", unity::TransformOf(fillArea), layer) : nullptr;
    Il2CppObject* handleArea = NewUi("Handle Slide Area", sRt, layer);
    Il2CppObject* handleGo = handleArea ? NewUi("Handle", unity::TransformOf(handleArea), layer) : nullptr;
    if (!bgGo || !fillGo || !handleGo) return "slider parts creation failed";
    // Unity's DefaultControls.CreateSlider geometry, scaled to our bar.
    Rect(unity::TransformOf(bgGo), {0, 0.25f}, {1, 0.75f}, {0.5f, 0.5f}, {0, 0}, {0, 0});
    Rect(unity::TransformOf(fillArea), {0, 0.25f}, {1, 0.75f}, {0.5f, 0.5f}, {-6, 0}, {-28, 0});
    Rect(unity::TransformOf(fillGo), {0, 0}, {0, 1}, {0.5f, 0.5f}, {0, 0}, {12, 0});
    Rect(unity::TransformOf(handleArea), {0, 0}, {1, 1}, {0.5f, 0.5f}, {0, 0}, {-28, 0});
    Rect(unity::TransformOf(handleGo), {0, 0}, {0, 1}, {0.5f, 0.5f}, {0, 0}, {28, 0});
    // Hit area: the whole bar (the track is only half its height); alpha 1/255, so it is drawn (a
    // CanvasRenderer may cull fully transparent meshes, and undrawn graphics are never raycast).
    if (Il2CppObject* hit = AddImage(sGo, Color{1.0f, 1.0f, 1.0f, 1.0f / 255.0f}, false); !hit)
        return "hit area Image failed";
    // The game's look: SDF rounded shapes in the menu's colours (the panel text colour for the track,
    // the game's slider pink (scroll bars) for fill and handle border); a SliderStyle, when one is
    // loaded and the shapes are plain Images, overrides what it sets.
    const Color text = labelColor;
    const Color pink{0.871f, 0.310f, 0.522f, 1.0f};
    const bool sdf = sdfRectClass != nullptr;
    Il2CppObject* bgImg = AddShape(bgGo, Color{text.r, text.g, text.b, 0.22f}, kBarHeight * 0.25f);
    Il2CppObject* fillImg = AddShape(fillGo, pink, kBarHeight * 0.25f);
    Il2CppObject* handleImg = AddShape(handleGo, Color{1.0f, 1.0f, 1.0f, 1.0f}, kBarHeight * 0.5f, 3.0f, pink);
    if (!bgImg || !fillImg || !handleImg) return "Image components failed";

    Il2CppObject* sl = AddComponent(sGo, sliderClass, "AddComponent(Slider)");
    if (!sl) return "Slider component failed";
    Keep(sl);
    slider = sl;
    sliderGo = sGo;
    {
        int32_t dir = 0;  // LeftToRight
        float mn = static_cast<float>(spec.min), mx = static_cast<float>(spec.max);
        bool whole = true;
        alignas(8) unsigned char nav[64] = {};  // Navigation, all zero = Mode.None, no explicit targets
        Call(sldSetFill, sl, {unity::TransformOf(fillGo)}, "Slider.set_fillRect");
        Call(sldSetHandle, sl, {unity::TransformOf(handleGo)}, "Slider.set_handleRect");
        Call(selSetTarget, sl, {handleImg}, "Selectable.set_targetGraphic");
        Call(sldSetDir, sl, {&dir}, "Slider.set_direction");
        Call(sldSetWhole, sl, {&whole}, "Slider.set_wholeNumbers");
        Call(sldSetMin, sl, {&mn}, "Slider.set_minValue");
        Call(sldSetMax, sl, {&mx}, "Slider.set_maxValue");
        Call(selSetNav, sl, {nav}, "Selectable.set_navigation");
        SetSliderValue(value);
    }
    // Game look: SliderStyler + a loaded SliderStyle (applied by its OnEnable when we activate).
    styled = false;
    if (Il2CppObject* st = sdf ? nullptr : SliderStyle()) {  // SliderStyler expects Images
        if (Il2CppObject* styler = AddComponent(sGo, stylerClass, "AddComponent(SliderStyler)")) {
            void** slot = reinterpret_cast<void**>(reinterpret_cast<char*>(styler) + offStylerStyle);
            if (wbarrier) wbarrier(styler, slot, st);
            else *slot = st;
            styled = true;
        }
    }
    if (Faulted()) return "faulted";
    unity::SetActive(r, true);
    SetSliderValue(value);  // after OnEnable (handle / fill placement)
    return nullptr;
}

bool PlotMenuSlider::Create(Il2CppObject* contentView, const SliderSpec& specIn, const PlotMenuSlider* rowOf) {
    if (!ready_ || !impl_ || !main_thread::IsMainThread()) return false;
    Impl& p = *impl_;
    Destroy();
    if (p.Faulted() || !unity::IsAlive(contentView) || !p.IsA(contentView, p.contentViewClass)) return false;
    auto fail = [&](const char* what) {
        if (!p.loggedFail) SR2_WARN("[{}] slider '{}' not built: {}", p.module, specIn.name, what);
        p.loggedFail = true;
        if (p.ownsRow && p.row) p.Destroy(p.row);
        else if (p.root) p.Destroy(p.root);
        p.Clear();
        return false;
    };

    // Where: the cost row's slot (hidden for owned items), else the bottom of the content view.
    Il2CppObject* craft = unity::Field<Il2CppObject*>(contentView, p.offCraftCost);
    Il2CppObject* craftTr = unity::IsAlive(craft) ? unity::TransformOf(craft) : nullptr;
    bool ok = false;
    Il2CppObject* parentTr = craftTr ? p.Call(p.trGetParent, craftTr, {}, "Transform.get_parent", &ok) : nullptr;
    if (!unity::IsAlive(parentTr)) {
        parentTr = unity::TransformOf(contentView);
        craftTr = nullptr;
    }
    if (!unity::IsAlive(parentTr) || !p.IsA(parentTr, p.rtClass)) return fail("details panel has no RectTransform");
    const int layer = p.Unbox<int32_t>(p.Call(p.goGetLayer, unity::GameObjectOf(parentTr), {}, "GameObject.get_layer"), -1);
    const bool layout = unity::GetComponent(parentTr, p.layoutGroupClass) != nullptr;

    p.spec = specIn;
    p.spec.step = std::max(p.spec.step, 1);
    if (p.spec.max < p.spec.min) std::swap(p.spec.max, p.spec.min);
    p.value = p.Snap(p.spec.value);

    // ---- shared row (sliders side by side): built by the first, joined by the others ----
    Il2CppObject* rowTr = nullptr;
    bool raycaster = false;
    const bool shared = rowOf != nullptr || p.spec.columns > 1;
    if (rowOf) {
        const Impl* o = rowOf->impl_;
        if (!o || !o->ownsRow || !unity::IsAlive(o->row)) return fail("the row to join is not built");
        rowTr = unity::TransformOf(o->row);
        raycaster = o->raycasterClass != nullptr;  // the row's Canvas + raycaster cover this root too
    } else if (shared) {
        Il2CppObject* rowGo = p.NewUi("sr2mods_SliderRow_" + p.spec.name, parentTr, layer);
        if (!rowGo) return fail("row GameObject creation failed");
        p.row = rowGo;
        p.ownsRow = true;
        p.Keep(rowGo);
        p.PlaceInSlot(rowGo, craftTr, layout);
        raycaster = p.AddRaycaster(rowGo, parentTr);
        rowTr = unity::TransformOf(rowGo);
    }
    if (p.Faulted()) return fail("faulted");

    // ---- root (inactive while being built: SliderStyler.OnEnable must see its style) ----
    Il2CppObject* root = p.NewUi("sr2mods_Slider_" + p.spec.name, rowTr ? rowTr : parentTr, layer);
    if (!root) return fail("GameObject creation failed");
    p.root = root;
    p.Keep(root);
    unity::SetActive(root, false);
    Il2CppObject* rootRt = unity::TransformOf(root);
    if (rowTr) {
        const int cols = std::max(p.spec.columns, 1);
        const int col = std::clamp(p.spec.column, 0, cols - 1);
        p.Rect(rootRt, {static_cast<float>(col) / cols, 0}, {static_cast<float>(col + 1) / cols, 1}, {0.5f, 0.5f}, {0, 0},
               {-kColumnGap, 0});
    } else {
        p.PlaceInSlot(root, craftTr, layout);
        if (p.Faulted()) return fail("faulted");
        raycaster = p.AddRaycaster(root, parentTr);
    }
    if (p.Faulted()) return fail("faulted");

    Il2CppObject* lse = unity::Field<Il2CppObject*>(contentView, p.offDetailsText);
    Il2CppObject* labelSrc = unity::IsAlive(lse) ? unity::GetComponent(lse, p.tmpTextClass) : nullptr;
    if (const char* err = p.BuildBody(root, rootRt, layer, labelSrc, 0.0f, shared ? kColumnFontScale : 1.0f)) return fail(err);
    const bool sdf = p.sdfRectClass != nullptr;
    const bool styled = p.styled;
    if (!p.loggedBuild) {
        p.loggedBuild = true;
        SR2_INFO("[{}] slider '{}' built under '{}' ({}; {}; {}; {}), range {}..{} step {}, value {}", p.module,
                 p.spec.name, unity::GetName(unity::GameObjectOf(parentTr)),
                 rowOf ? "joined the row" : layout ? "LayoutGroup: inserted after the cost row" : craftTr ? "cost row slot copied" : "panel bottom",
                 sdf ? "game SDF shapes" : styled ? "game SliderStyle" : "plain colours",
                 raycaster ? "own Canvas + GraphicRaycaster" : "NO own raycaster",
                 p.externalSteps ? "left / right from the menu's navigation" : "left / right polled", p.spec.min,
                 p.spec.max, p.spec.step, p.value);
    }
    return true;
}

float PlotMenuSlider::Height() { return kTotalHeight; }

bool PlotMenuSlider::CreateIn(const SliderHost& host, const SliderSpec& specIn) {
    if (!ready_ || !impl_ || !main_thread::IsMainThread()) return false;
    Impl& p = *impl_;
    Destroy();
    if (p.Faulted()) return false;
    auto fail = [&](const char* what) {
        if (!p.loggedFail) SR2_WARN("[{}] slider '{}' not built: {}", p.module, specIn.name, what);
        p.loggedFail = true;
        if (p.root) p.Destroy(p.root);
        p.Clear();
        return false;
    };
    if (!unity::IsAlive(host.parent) || !p.IsA(host.parent, p.rtClass)) return fail("host has no RectTransform");
    Il2CppObject* labelSrc = unity::IsAlive(host.textStyle) && p.IsA(host.textStyle, p.tmpTextClass) ? host.textStyle : nullptr;
    const int layer = p.Unbox<int32_t>(p.Call(p.goGetLayer, unity::GameObjectOf(host.parent), {}, "GameObject.get_layer"), -1);
    p.spec = specIn;
    p.spec.step = std::max(p.spec.step, 1);
    if (p.spec.max < p.spec.min) std::swap(p.spec.max, p.spec.min);
    p.value = p.Snap(p.spec.value);
    Il2CppObject* root = p.NewUi("sr2mods_Slider_" + p.spec.name, host.parent, layer);
    if (!root) return fail("GameObject creation failed");
    p.root = root;
    p.Keep(root);
    unity::SetActive(root, false);
    Il2CppObject* rootRt = unity::TransformOf(root);
    p.Rect(rootRt, {0, 1}, {0, 1}, {0, 1}, {host.x, -host.y}, {host.width, kTotalHeight});
    if (p.Faulted()) return fail("faulted");
    if (const char* err = p.BuildBody(root, rootRt, layer, labelSrc, host.fontSize, 1.0f)) return fail(err);
    SR2_INFO("[{}] slider '{}' built in '{}' at ({:.0f}, {:.0f}) width {:.0f} ({}), range {}..{} step {}, value {}",
             p.module, p.spec.name, unity::GetName(unity::GameObjectOf(host.parent)), host.x, host.y, host.width,
             p.sdfRectClass ? "game SDF shapes" : p.styled ? "game SliderStyle" : "plain colours", p.spec.min,
             p.spec.max, p.spec.step, p.value);
    return true;
}

void PlotMenuSlider::Destroy() {
    if (!impl_ || !main_thread::IsMainThread()) return;
    Impl& p = *impl_;
    if (!p.root) return;
    if (!p.Faulted()) {
        p.HandBackSelection();  // never leave the EventSystem on a destroyed slider
        // The row's builder removes the whole row (the other sliders' roots with it).
        p.Destroy(p.ownsRow && unity::IsAlive(p.row) ? p.row : p.root);
    }
    p.Clear();
}

bool PlotMenuSlider::Active() const { return impl_ && impl_->root; }
int PlotMenuSlider::Value() const { return impl_ ? impl_->value : 0; }
Il2CppObject* PlotMenuSlider::Root() const { return impl_ ? impl_->root : nullptr; }

void PlotMenuSlider::SetPolling(bool on) {
    if (!impl_) return;
    impl_->polling = on;
    if (!on) impl_->heldDir = impl_->blockedDir = 0;
}

void PlotMenuSlider::SetValue(int v) {
    if (!impl_ || !impl_->root || !main_thread::IsMainThread() || impl_->Faulted()) return;
    Impl& p = *impl_;
    v = p.Snap(v);
    p.SetSliderValue(v);
    p.Apply(v, false);
}

void PlotMenuSlider::SetLabel(std::function<std::string(int)> label) {
    if (!impl_ || !main_thread::IsMainThread()) return;
    Impl& p = *impl_;
    p.spec.label = std::move(label);
    if (p.root && !p.Faulted()) p.SetLabel(p.value);
}

// ---------------------------------------------------------------- per frame --

void PlotMenuSlider::Impl::HandBackSelection() {
    if (!selectionOk || !sliderGo) return;
    bool ok = false;
    Il2CppObject* es = Call(esCurrent, nullptr, {}, "EventSystem.get_current", &ok);
    if (!ok || !unity::IsAlive(es)) return;
    Il2CppObject* sel = Call(esGetSelected, es, {}, "EventSystem.get_currentSelectedGameObject", &ok);
    if (!ok || sel != sliderGo) return;
    Il2CppObject* back = unity::IsAlive(prevSel) ? prevSel : nullptr;
    Call(esSetSelected, es, {back}, "EventSystem.SetSelectedGameObject");
}

int PlotMenuSlider::Impl::InputDirection() {
    if (!inputOk) return 0;
    bool ok = false;
    int dir = 0;
    const bool primary = PrimaryKeys();
    if (padCurrent) {
        Il2CppObject* pad = Call(padCurrent, nullptr, {}, "Gamepad.get_current", &ok);
        if (ok && pad && primary) {
            Il2CppObject* dpad = Call(padDpad, pad, {}, "Gamepad.get_dpad", &ok);
            if (ok && dpad) dir += (Pressed(dpadRight, dpad) ? 1 : 0) - (Pressed(dpadLeft, dpad) ? 1 : 0);
            if (dir == 0 && padStick && stickLeft && stickRight) {
                Il2CppObject* stick = Call(padStick, pad, {}, "Gamepad.get_leftStick", &ok);
                if (ok && stick) dir += (Pressed(stickRight, stick) ? 1 : 0) - (Pressed(stickLeft, stick) ? 1 : 0);
            }
        } else if (ok && pad && padRStick && stickLeft && stickRight) {
            Il2CppObject* stick = Call(padRStick, pad, {}, "Gamepad.get_rightStick", &ok);
            if (ok && stick) dir += (Pressed(stickRight, stick) ? 1 : 0) - (Pressed(stickLeft, stick) ? 1 : 0);
        }
    }
    if (dir == 0 && kbCurrent && OwnInput()) {  // arrows: Shift + arrows for the secondary slider
        Il2CppObject* kb = Call(kbCurrent, nullptr, {}, "Keyboard.get_current", &ok);
        if (ok && kb) dir += (Pressed(kbRight, kb) ? 1 : 0) - (Pressed(kbLeft, kb) ? 1 : 0);
    }
    return dir;
}

bool PlotMenuSlider::Impl::InputBlocked() {
    bool ok = false;
    const bool interactable = Unbox<bool>(Call(selInteractable, slider, {}, "Selectable.IsInteractable", &ok), true);
    Il2CppObject* found = nullptr;
    if (findAny && popupClass) {
        int32_t mode = kFindExcludeInactive;
        found = Call(findAny, nullptr, {unity::TypeOf(popupClass), &mode}, "FindAnyObjectByType", &ok);
        if (!ok || !unity::IsAlive(found)) found = nullptr;
    }
    if (interactable && !found) return false;
    if (!loggedBlocked) {
        loggedBlocked = true;
        SR2_INFO("[{}] slider '{}': left / right ignored ({})", module, spec.name,
                 !interactable ? std::string("slider not interactable")
                               : "PopupPrompt '" + unity::GetName(unity::GameObjectOf(found)) + "' is active");
    }
    return true;
}

void PlotMenuSlider::Impl::SetFocus(bool on, bool fromMouse, const char* why) {
    focusFromMouse = on && fromMouse;
    if (focused == on) return;
    focused = on;
    heldDir = blockedDir = 0;  // a press held across the change starts fresh
    navDriven = false;
    SetLabel(value);
    if (!loggedFocus) {
        loggedFocus = true;
        SR2_INFO("[{}] slider '{}': {} ({}) - the arrows step it only while selected", module, spec.name,
                 on ? "selected" : "released", why ? why : "");
    } else {
        SR2_DEBUG("[{}] slider '{}': {} ({})", module, spec.name, on ? "selected" : "released", why ? why : "");
    }
}

// spec.selectToStep, every frame while shown: a mouse press on the slider selects it; a slider selected
// that way is released when the mouse leaves it (not while the button is held: a drag may leave the
// bar); a press anywhere else releases it, judged two frames later (the EventSystem may process the
// press after us, so the slider's own pressed flag is not set yet in the frame of the press).
void PlotMenuSlider::Impl::FocusTick() {
    if (!selIsPressed || !selPointerInside) return;
    bool ok = false;
    const bool pressed = Unbox<bool>(Call(selIsPressed, slider, {}, "Selectable.IsPressed", &ok)) && ok;
    const bool inside = Unbox<bool>(Call(selPointerInside, slider, {}, "Selectable.get_isPointerInside", &ok)) && ok;
    if (pressed) {
        if (!focused) focusGained = true;
        SetFocus(true, true, "mouse press");
    } else if (focused && focusFromMouse && !inside) {
        SetFocus(false, false, "mouse left the slider");
    }
    if (pendingClick > 0 && --pendingClick == 0 && focused && !pressed && !inside)
        SetFocus(false, false, "mouse press elsewhere");
    if (btnPressedThisFrame && mouseCurrent && mouseLeft) {
        Il2CppObject* mouse = Call(mouseCurrent, nullptr, {}, "Mouse.get_current", &ok);
        Il2CppObject* left = ok && mouse ? Call(mouseLeft, mouse, {}, "Mouse.get_leftButton", &ok) : nullptr;
        if (ok && left && Unbox<bool>(Call(btnPressedThisFrame, left, {}, "ButtonControl.get_wasPressedThisFrame", &ok)) && ok)
            pendingClick = 2;
    }
}

bool PlotMenuSlider::Focused() const { return impl_ && impl_->root && impl_->focused; }

void PlotMenuSlider::SetFocused(bool on, const char* why) {
    if (!impl_ || !impl_->root || !main_thread::IsMainThread() || impl_->Faulted() || !impl_->spec.selectToStep) return;
    impl_->SetFocus(on, false, why);
}

bool PlotMenuSlider::TakeFocusGained() {
    if (!impl_ || !impl_->focusGained) return false;
    impl_->focusGained = false;
    return true;
}

bool PlotMenuSlider::Blocked() {
    if (!impl_ || !impl_->root || !main_thread::IsMainThread() || impl_->Faulted()) return true;
    if (!unity::IsAlive(impl_->root) || !unity::IsAlive(impl_->slider)) return true;
    return impl_->InputBlocked();
}

bool PlotMenuSlider::Step(int dir) {
    if (!impl_ || !impl_->root || dir == 0 || !main_thread::IsMainThread()) return false;
    Impl& p = *impl_;
    if (p.Faulted() || !unity::IsAlive(p.root) || !unity::IsAlive(p.slider) || !unity::ActiveInHierarchy(p.root))
        return false;
    if (!p.OwnInput()) return false;  // the other slider's keys (Shift held or not)
    if (p.InputBlocked()) return false;
    dir = dir > 0 ? 1 : -1;
    const uint64_t now = Impl::NowMs();
    const bool samePress = p.heldDir == dir;
    const bool pollStepped = samePress && !p.navDriven && now - p.lastPollStepMs < kSamePressMs;
    if (samePress) p.navDriven = true;  // the input module repeats it from now on, not our poll
    p.lastNavDir = dir;
    p.lastNavMs = now;
    if (!p.loggedNav) {
        p.loggedNav = true;
        SR2_INFO("[{}] slider '{}': left / right taken from the menu's grid navigation", p.module, p.spec.name);
    }
    if (!pollStepped) {
        const int v = p.Snap(p.value + dir * p.spec.step);
        p.SetSliderValue(v);
        p.Apply(v, true);
    }
    return true;
}

void PlotMenuSlider::SetNavigationSteps(bool on) {
    if (impl_) impl_->externalSteps = on;
}

void PlotMenuSlider::Tick(float dt) {
    if (!impl_ || !impl_->root || !main_thread::IsMainThread()) return;
    Impl& p = *impl_;
    if (p.Faulted()) {
        p.Clear();  // no game calls after a fault (handles freed by the posted job)
        return;
    }
    if (!unity::IsAlive(p.root) || !unity::IsAlive(p.slider)) {
        p.Clear();  // died with the menu
        return;
    }
    if (!unity::ActiveInHierarchy(p.root)) {
        p.heldDir = 0;
        return;
    }
    if (!std::isfinite(dt) || dt < 0.0f) dt = 0.0f;
    dt = std::min(dt, 0.25f);

    // EventSystem selection: remember the menu's own; after a mouse drag on the slider, hand it back.
    bool sliderSelected = false;
    if (p.selectionOk) {
        bool ok = false;
        Il2CppObject* es = p.Call(p.esCurrent, nullptr, {}, "EventSystem.get_current", &ok);
        Il2CppObject* sel = (ok && unity::IsAlive(es)) ? p.Call(p.esGetSelected, es, {}, "EventSystem.get_currentSelectedGameObject", &ok) : nullptr;
        if (ok && sel == p.sliderGo) {
            sliderSelected = true;
            Il2CppObject* mouse = p.Call(p.mouseCurrent, nullptr, {}, "Mouse.get_current", &ok);
            if (!(ok && mouse && p.Pressed(p.mouseLeft, mouse))) {
                p.HandBackSelection();
                sliderSelected = false;
            }
        } else if (ok && sel && sel != p.prevSel && unity::IsAlive(sel)) {
            p.ForgetSelection();
            if (Il2CppGCHandle h = unity::NewGCHandle(sel)) {
                p.prevSel = sel;
                p.prevSelHandle = h;
            }
        }
    }

    if (p.spec.selectToStep) p.FocusTick();

    // Gamepad / keyboard steps (not while Unity's own Slider.OnMove has it, not under a popup; with
    // selectToStep only while selected).
    const bool keysOff = sliderSelected || !p.polling || (p.spec.selectToStep && !p.focused);
    int dir = keysOff ? 0 : p.InputDirection();
    if (dir == 0 || dir != p.blockedDir) p.blockedDir = 0;
    if (dir != 0 && dir == p.blockedDir) dir = 0;  // held since it was refused: no re-check per frame
    if (dir != 0 && dir != p.heldDir && p.InputBlocked()) {  // checked once per new press, not per frame
        p.blockedDir = dir;
        dir = 0;
    }
    int steps = 0;
    const uint64_t now = Impl::NowMs();
    if (dir == 0) {
        p.heldDir = 0;
        p.navDriven = false;
    } else if (dir != p.heldDir) {
        p.heldDir = dir;
        p.heldTime = 0.0f;
        p.nextRepeat = kRepeatDelay;
        // Already stepped by the menu's navigation event for this press (Step)? Then it drives it.
        p.navDriven = p.lastNavDir == dir && now - p.lastNavMs < kSamePressMs;
        steps = p.navDriven ? 0 : 1;
        if (steps) p.lastPollStepMs = now;
    } else if (!p.navDriven) {
        p.heldTime += dt;
        while (p.heldTime >= p.nextRepeat && steps < 8) {
            ++steps;
            p.nextRepeat += kRepeatRate;
        }
    }

    // Value: poll the slider (mouse drag / Unity's own move), apply our steps.
    bool ok = false;
    const float f = p.Unbox<float>(p.Call(p.sldGetValue, p.slider, {}, "Slider.get_value", &ok));
    if (!ok || !std::isfinite(f)) return;
    int v = p.Snap(static_cast<int>(std::lround(f)));
    if (steps) v = p.Snap(v + dir * steps * p.spec.step);
    if (static_cast<float>(v) != f) p.SetSliderValue(v);
    p.Apply(v, true);
}

}  // namespace sr2::modules::custom_upgrades::ui
