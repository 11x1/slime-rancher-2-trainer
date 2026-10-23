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
constexpr float kRepeatDelay = 0.40f;  // seconds before a held direction repeats
constexpr float kRepeatRate = 0.07f;   // seconds between repeats
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
    // EventSystem (optional: selection hand-back)
    const MethodInfo* esCurrent = nullptr, *esGetSelected = nullptr, *esSetSelected = nullptr;
    // Input System (optional: gamepad / keyboard steps)
    const MethodInfo* padCurrent = nullptr, *padDpad = nullptr, *padStick = nullptr, *dpadLeft = nullptr,
                    *dpadRight = nullptr, *stickLeft = nullptr, *stickRight = nullptr, *btnPressed = nullptr;
    const MethodInfo* kbCurrent = nullptr, *kbLeft = nullptr, *kbRight = nullptr;
    const MethodInfo* mouseCurrent = nullptr, *mouseLeft = nullptr;
    bool inputOk = false, selectionOk = false;

    using WBarrierFn = void (*)(Il2CppObject*, void**, void*);
    using ObjectNewFn = Il2CppObject* (*)(Il2CppClass*);
    WBarrierFn wbarrier = nullptr;
    ObjectNewFn objectNew = nullptr;

    // session assets
    bool styleTried = false;
    Il2CppObject* style = nullptr;  // pinned (never freed)

    // the live slider (main thread only)
    Il2CppObject* root = nullptr;     // GameObject
    Il2CppObject* slider = nullptr;   // UnityEngine.UI.Slider
    Il2CppObject* sliderGo = nullptr; // its GameObject
    Il2CppObject* label = nullptr;    // TMP_Text (may be null)
    std::vector<Il2CppGCHandle> handles;
    SliderSpec spec;
    int value = 0;
    // input repeat
    int heldDir = 0;
    int blockedDir = 0;  // direction held since a press was refused (popup / not interactable)
    float heldTime = 0.0f, nextRepeat = 0.0f;
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
    bool Pressed(const MethodInfo* getControl, Il2CppObject* device) {
        if (!device || !getControl) return false;
        bool ok = false;
        Il2CppObject* ctl = Call(getControl, device, {}, "InputControl getter", &ok);
        if (!ok || !ctl) return false;
        return Unbox<bool>(Call(btnPressed, ctl, {}, "ButtonControl.get_isPressed", &ok)) && ok;
    }
    Il2CppObject* SliderStyle();
    std::string LabelText(int v) const { return spec.label ? spec.label(v) : std::to_string(v); }
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
        root = slider = sliderGo = label = nullptr;
        std::vector<Il2CppGCHandle> doomed;
        doomed.swap(handles);
        if (!doomed.empty()) main_thread::Post([doomed] {
            for (Il2CppGCHandle h : doomed) unity::FreeGCHandle(h);
        });
        ForgetSelection();
        heldDir = blockedDir = 0;
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

bool PlotMenuSlider::Create(Il2CppObject* contentView, const SliderSpec& specIn) {
    if (!ready_ || !impl_ || !main_thread::IsMainThread()) return false;
    Impl& p = *impl_;
    Destroy();
    if (p.Faulted() || !unity::IsAlive(contentView) || !p.IsA(contentView, p.contentViewClass)) return false;
    auto fail = [&](const char* what) {
        if (!p.loggedFail) SR2_WARN("[{}] slider '{}' not built: {}", p.module, specIn.name, what);
        p.loggedFail = true;
        if (p.root) p.Destroy(p.root);
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

    // ---- root (inactive while being built: SliderStyler.OnEnable must see its style) ----
    Il2CppObject* root = p.NewUi("sr2mods_Slider_" + p.spec.name, parentTr, layer);
    if (!root) return fail("GameObject creation failed");
    p.root = root;
    p.Keep(root);
    unity::SetActive(root, false);
    Il2CppObject* rootRt = unity::TransformOf(root);
    if (layout) {
        Il2CppObject* le = p.AddComponent(root, p.layoutElemClass, "AddComponent(LayoutElement)");
        float h = kTotalHeight, flex = 1.0f;
        if (le) {
            p.Call(p.leSetPrefH, le, {&h}, "LayoutElement.set_preferredHeight");
            p.Call(p.leSetMinH, le, {&h}, "LayoutElement.set_minHeight");
            p.Call(p.leSetFlexW, le, {&flex}, "LayoutElement.set_flexibleWidth");
        }
        if (craftTr) {
            int32_t idx = p.Unbox<int32_t>(p.Call(p.trGetSibling, craftTr, {}, "Transform.GetSiblingIndex", &ok), -1);
            if (ok && idx >= 0) {
                ++idx;
                p.Call(p.trSetSibling, rootRt, {&idx}, "Transform.SetSiblingIndex");
            }
        }
    } else if (craftTr && p.IsA(craftTr, p.rtClass)) {
        const Vec2 amin = p.GetVec(p.rtGetAnchorMin, craftTr, "get_anchorMin");
        const Vec2 amax = p.GetVec(p.rtGetAnchorMax, craftTr, "get_anchorMax");
        const Vec2 piv = p.GetVec(p.rtGetPivot, craftTr, "get_pivot");
        const Vec2 pos = p.GetVec(p.rtGetPos, craftTr, "get_anchoredPosition");
        Vec2 size = p.GetVec(p.rtGetSize, craftTr, "get_sizeDelta");
        if (amin.y == amax.y) size.y = std::max(size.y, kTotalHeight);  // fixed height: at least ours
        p.Rect(rootRt, amin, amax, piv, pos, size);
    } else {
        p.Rect(rootRt, {0, 0}, {1, 0}, {0.5f, 0}, {0, 16}, {-32, kTotalHeight});
    }
    if (p.Faulted()) return fail("faulted");

    // ---- label ----
    Il2CppObject* labelGo = p.NewUi("Label", rootRt, layer);
    if (labelGo) {
        p.Rect(unity::TransformOf(labelGo), {0, 1}, {1, 1}, {0.5f, 1}, {0, 0}, {0, kLabelHeight});
        Il2CppObject* tmp = p.AddComponent(labelGo, p.tmpUguiClass, "AddComponent(TextMeshProUGUI)");
        if (tmp) {
            p.Keep(tmp);
            p.label = tmp;
            Il2CppObject* lse = unity::Field<Il2CppObject*>(contentView, p.offDetailsText);
            Il2CppObject* src = unity::IsAlive(lse) ? unity::GetComponent(lse, p.tmpTextClass) : nullptr;
            float size = 26.0f;
            Color col{1, 1, 1, 1};
            if (src) {
                if (Il2CppObject* font = p.Call(p.tmpGetFont, src, {}, "TMP_Text.get_font"); unity::IsAlive(font))
                    p.Call(p.tmpSetFont, tmp, {font}, "TMP_Text.set_font");
                if (Il2CppObject* mat = p.Call(p.tmpGetMat, src, {}, "TMP_Text.get_fontSharedMaterial"); unity::IsAlive(mat))
                    p.Call(p.tmpSetMat, tmp, {mat}, "TMP_Text.set_fontSharedMaterial");
                const float s = p.Unbox<float>(p.Call(p.tmpGetSize, src, {}, "TMP_Text.get_fontSize"));
                if (std::isfinite(s) && s >= 10.0f && s <= 80.0f) size = s;
                col = p.Unbox<Color>(p.Call(p.tmpGetColor, src, {}, "TMP_Text.get_color"), col);
            }
            p.Call(p.tmpSetSize, tmp, {&size}, "TMP_Text.set_fontSize");
            p.Call(p.tmpSetColor, tmp, {&col}, "TMP_Text.set_color");
            int32_t align = kTmpAlignLeft, noWrap = 0;
            p.Call(p.tmpSetAlign, tmp, {&align}, "TMP_Text.set_alignment");
            if (p.tmpSetWrap) p.Call(p.tmpSetWrap, tmp, {&noWrap}, "TMP_Text.set_textWrappingMode");
            bool off = false;
            p.Call(p.gfxSetRaycast, tmp, {&off}, "Graphic.set_raycastTarget");
            p.SetLabel(p.value);
        }
    }

    // ---- slider: standard uGUI hierarchy (names are what SliderStyler looks for) ----
    Il2CppObject* sGo = p.NewUi("Slider", rootRt, layer);
    if (!sGo) return fail("slider GameObject creation failed");
    Il2CppObject* sRt = unity::TransformOf(sGo);
    p.Rect(sRt, {0, 0}, {1, 0}, {0.5f, 0}, {0, 0}, {-8, kBarHeight});
    Il2CppObject* bgGo = p.NewUi("Background", sRt, layer);
    Il2CppObject* fillArea = p.NewUi("Fill Area", sRt, layer);
    Il2CppObject* fillGo = fillArea ? p.NewUi("Fill", unity::TransformOf(fillArea), layer) : nullptr;
    Il2CppObject* handleArea = p.NewUi("Handle Slide Area", sRt, layer);
    Il2CppObject* handleGo = handleArea ? p.NewUi("Handle", unity::TransformOf(handleArea), layer) : nullptr;
    if (!bgGo || !fillGo || !handleGo) return fail("slider parts creation failed");
    // Unity's DefaultControls.CreateSlider geometry, scaled to our bar.
    p.Rect(unity::TransformOf(bgGo), {0, 0.25f}, {1, 0.75f}, {0.5f, 0.5f}, {0, 0}, {0, 0});
    p.Rect(unity::TransformOf(fillArea), {0, 0.25f}, {1, 0.75f}, {0.5f, 0.5f}, {-6, 0}, {-28, 0});
    p.Rect(unity::TransformOf(fillGo), {0, 0}, {0, 1}, {0.5f, 0.5f}, {0, 0}, {12, 0});
    p.Rect(unity::TransformOf(handleArea), {0, 0}, {1, 1}, {0.5f, 0.5f}, {0, 0}, {-28, 0});
    p.Rect(unity::TransformOf(handleGo), {0, 0}, {0, 1}, {0.5f, 0.5f}, {0, 0}, {28, 0});
    // Plain game-like colours first (a SliderStyle overrides what it sets).
    Il2CppObject* bgImg = p.AddImage(bgGo, Color{0.10f, 0.07f, 0.16f, 0.85f}, true);
    Il2CppObject* fillImg = p.AddImage(fillGo, Color{0.98f, 0.45f, 0.64f, 1.0f}, true);
    Il2CppObject* handleImg = p.AddImage(handleGo, Color{1.0f, 1.0f, 1.0f, 1.0f}, false);
    if (!bgImg || !fillImg || !handleImg) return fail("Image components failed");

    Il2CppObject* slider = p.AddComponent(sGo, p.sliderClass, "AddComponent(Slider)");
    if (!slider) return fail("Slider component failed");
    p.Keep(slider);
    p.slider = slider;
    p.sliderGo = sGo;
    {
        int32_t dir = 0;  // LeftToRight
        float mn = static_cast<float>(p.spec.min), mx = static_cast<float>(p.spec.max);
        bool whole = true;
        alignas(8) unsigned char nav[64] = {};  // Navigation, all zero = Mode.None, no explicit targets
        p.Call(p.sldSetFill, slider, {unity::TransformOf(fillGo)}, "Slider.set_fillRect");
        p.Call(p.sldSetHandle, slider, {unity::TransformOf(handleGo)}, "Slider.set_handleRect");
        p.Call(p.selSetTarget, slider, {handleImg}, "Selectable.set_targetGraphic");
        p.Call(p.sldSetDir, slider, {&dir}, "Slider.set_direction");
        p.Call(p.sldSetWhole, slider, {&whole}, "Slider.set_wholeNumbers");
        p.Call(p.sldSetMin, slider, {&mn}, "Slider.set_minValue");
        p.Call(p.sldSetMax, slider, {&mx}, "Slider.set_maxValue");
        p.Call(p.selSetNav, slider, {nav}, "Selectable.set_navigation");
        p.SetSliderValue(p.value);
    }
    // Game look: SliderStyler + a loaded SliderStyle (applied by its OnEnable when we activate).
    bool styled = false;
    if (Il2CppObject* style = p.SliderStyle()) {
        if (Il2CppObject* styler = p.AddComponent(sGo, p.stylerClass, "AddComponent(SliderStyler)")) {
            void** slot = reinterpret_cast<void**>(reinterpret_cast<char*>(styler) + p.offStylerStyle);
            if (p.wbarrier) p.wbarrier(styler, slot, style);
            else *slot = style;
            styled = true;
        }
    }
    if (p.Faulted()) return fail("faulted");
    unity::SetActive(root, true);
    p.SetSliderValue(p.value);  // after OnEnable (handle / fill placement)
    if (!p.loggedBuild) {
        p.loggedBuild = true;
        SR2_INFO("[{}] slider '{}' built under '{}' ({}; {}), range {}..{} step {}, value {}", p.module, p.spec.name,
                 unity::GetName(unity::GameObjectOf(parentTr)),
                 layout ? "LayoutGroup: inserted after the cost row" : craftTr ? "cost row slot copied" : "panel bottom",
                 styled ? "game SliderStyle" : "plain colours", p.spec.min, p.spec.max, p.spec.step, p.value);
    }
    return true;
}

void PlotMenuSlider::Destroy() {
    if (!impl_ || !main_thread::IsMainThread()) return;
    Impl& p = *impl_;
    if (!p.root) return;
    if (!p.Faulted()) {
        p.HandBackSelection();  // never leave the EventSystem on a destroyed slider
        p.Destroy(p.root);
    }
    p.Clear();
}

bool PlotMenuSlider::Active() const { return impl_ && impl_->root; }
int PlotMenuSlider::Value() const { return impl_ ? impl_->value : 0; }

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
    if (padCurrent) {
        Il2CppObject* pad = Call(padCurrent, nullptr, {}, "Gamepad.get_current", &ok);
        if (ok && pad) {
            Il2CppObject* dpad = Call(padDpad, pad, {}, "Gamepad.get_dpad", &ok);
            if (ok && dpad) dir += (Pressed(dpadRight, dpad) ? 1 : 0) - (Pressed(dpadLeft, dpad) ? 1 : 0);
            if (dir == 0 && padStick && stickLeft && stickRight) {
                Il2CppObject* stick = Call(padStick, pad, {}, "Gamepad.get_leftStick", &ok);
                if (ok && stick) dir += (Pressed(stickRight, stick) ? 1 : 0) - (Pressed(stickLeft, stick) ? 1 : 0);
            }
        }
    }
    if (dir == 0 && kbCurrent) {
        Il2CppObject* kb = Call(kbCurrent, nullptr, {}, "Keyboard.get_current", &ok);
        if (ok && kb) dir += (Pressed(kbRight, kb) ? 1 : 0) - (Pressed(kbLeft, kb) ? 1 : 0);
    }
    return dir;
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

    // Gamepad / keyboard steps (not while Unity's own Slider.OnMove has it, not under a popup).
    int dir = sliderSelected ? 0 : p.InputDirection();
    if (dir == 0 || dir != p.blockedDir) p.blockedDir = 0;
    if (dir != 0 && dir == p.blockedDir) dir = 0;  // held since it was refused: no re-check per frame
    if (dir != 0 && dir != p.heldDir) {            // checked once per new press, not per frame
        bool ok = false;
        const bool interactable = p.Unbox<bool>(p.Call(p.selInteractable, p.slider, {}, "Selectable.IsInteractable", &ok), true);
        bool popup = false;
        if (p.findAny && p.popupClass) {
            int32_t mode = kFindExcludeInactive;
            Il2CppObject* found = p.Call(p.findAny, nullptr, {unity::TypeOf(p.popupClass), &mode}, "FindAnyObjectByType", &ok);
            popup = ok && unity::IsAlive(found);
        }
        if (!interactable || popup) {
            p.blockedDir = dir;
            dir = 0;
        }
    }
    int steps = 0;
    if (dir == 0) {
        p.heldDir = 0;
    } else if (dir != p.heldDir) {
        p.heldDir = dir;
        p.heldTime = 0.0f;
        p.nextRepeat = kRepeatDelay;
        steps = 1;
    } else {
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
