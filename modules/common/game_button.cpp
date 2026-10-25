// The game's popup button as a reusable runtime uGUI button (see game_button.h). SR2 1.3.0, all by name.

#include "modules/common/game_button.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <format>
#include <initializer_list>
#include <mutex>
#include <utility>

#include "core/detour_guard.h"
#include "core/log.h"
#include "core/main_thread.h"
#include "core/resolver.h"
#include "core/unity.h"

namespace sr2::modules::game_button {

namespace {

namespace unity = sr2::core::unity;
namespace main_thread = sr2::core::main_thread;
using sr2::il2cpp::api;
using sr2::il2cpp::Resolver;

constexpr const char* kImage = "Assembly-CSharp";
constexpr const char* kCore = "UnityEngine.CoreModule";
constexpr const char* kUi = "UnityEngine.UI";
constexpr const char* kTmp = "Unity.TextMeshPro";
constexpr int kReapplyFrames = 30;     // key / label / placement re-asserted this many frames after a change
constexpr int kAlignCenter = 514;      // TMP TextAlignmentOptions.Center

struct Vec2 {
    float x, y;
};

// Everything resolved once (worker thread), read-only afterwards.
struct G {
    bool done = false, ok = false;
    std::string why;
    using ObjectNewFn = Il2CppObject* (*)(Il2CppClass*);
    ObjectNewFn objectNew = nullptr;
    Il2CppClass* goClass = nullptr, *rtClass = nullptr, *tmpTextClass = nullptr, *popupClass = nullptr;
    Il2CppClass* ieButtonClass = nullptr, *moreViewClass = nullptr, *uiButtonClass = nullptr, *animatorClass = nullptr;
    Il2CppClass* layoutMgrClass = nullptr, *displayClass = nullptr, *iconClass = nullptr, *lseClass = nullptr;
    Il2CppClass* colorClass = nullptr, *resizeClass = nullptr;
    const MethodInfo* gcInstance = nullptr, *gcGetTemplates = nullptr;
    const MethodInfo* goCtor = nullptr, *goAddComponent = nullptr, *objDestroy = nullptr;
    const MethodInfo* trSetParent = nullptr, *trFind = nullptr, *trSetLocalScale = nullptr, *trChildCount = nullptr,
                    *trGetChild = nullptr;
    const MethodInfo* rtSetAnchorMin = nullptr, *rtSetAnchorMax = nullptr, *rtSetPivot = nullptr, *rtSetSize = nullptr,
                    *rtSetPos = nullptr, *rtGetRect = nullptr;
    const MethodInfo* behSetEnabled = nullptr, *selSetInteractable = nullptr, *selIsPressed = nullptr,
                    *selPointerInside = nullptr, *selSetNav = nullptr;
    const MethodInfo* tmpSetText = nullptr, *tmpGetText = nullptr, *tmpSetAlign = nullptr;
    const MethodInfo* animInteractableChanged = nullptr, *soCreate = nullptr;
    size_t offTplPopup = 0, offPopupPositive = 0, offPopupNegative = 0, offMoreLabel = 0;
    size_t offAnimNormal = 0, offAnimSelected = 0, offColor = 0;
    size_t offIconButton = 0, offIconKey = 0, offIconKeyText = 0, offIconKeyImage = 0, offIconKeyLse = 0,
           offIconDouble = 0;
    size_t offResizeMinW = 0;  // ResizeToPreferred._minWidth (Optional<float>: bool hasValue, float value)
    bool glyphOk = false, tintOk = false, minWidthOk = false;
};
G g;
std::mutex g_mutex;

// ScriptableColor instances we made for tints (one per colour, kept for the session).
struct Tint {
    Color c;
    Il2CppObject* obj;
    Il2CppGCHandle h;
};
std::vector<Tint> g_tints;

template <class T>
T Unbox(Il2CppObject* boxed, T def = T{}) {
    return boxed ? *reinterpret_cast<T*>(reinterpret_cast<char*>(boxed) + 2 * sizeof(void*)) : def;
}

bool IsA(Il2CppObject* obj, Il2CppClass* k) {
    const auto& a = api();
    if (!obj || !k || !a.il2cpp_object_get_class) return false;
    Il2CppClass* c = a.il2cpp_object_get_class(obj);
    if (c == k) return true;
    return c && a.il2cpp_class_is_assignable_from && a.il2cpp_class_is_assignable_from(k, c);
}

// A guarded call; a native exception marks `module` faulted, a managed one is logged.
Il2CppObject* Call(const std::string& module, const MethodInfo* m, void* obj, std::initializer_list<void*> args,
                   const char* what, bool* ok = nullptr) {
    if (!m) {
        if (ok) *ok = false;
        return nullptr;
    }
    void* params[8] = {};
    size_t n = 0;
    for (void* a : args)
        if (n < 8) params[n++] = a;
    std::string err;
    Il2CppObject* r = Resolver::Get().Invoke(m, obj, n ? params : nullptr, &err);
    const bool good = err.empty();
    if (ok) *ok = good;
    if (!good) {
        if (err.rfind("native exception", 0) == 0) sr2::core::MarkModuleFaulted(module, std::format("game button: {}: {}", what, err));
        else SR2_WARN("[{}] game button: {} failed: {}", module, what, err);
    }
    return r;
}

void FreeLater(std::vector<Il2CppGCHandle>& hs) {
    std::vector<Il2CppGCHandle> doomed;
    doomed.swap(hs);
    if (!doomed.empty()) main_thread::Post([doomed] {
        for (Il2CppGCHandle h : doomed) unity::FreeGCHandle(h);
    });
}

void Keep(std::vector<Il2CppGCHandle>& into, Il2CppObject* o) {
    if (Il2CppGCHandle h = o ? unity::NewGCHandle(o) : nullptr) into.push_back(h);
}

bool SameColor(const Color& a, const Color& b) {
    return std::fabs(a.r - b.r) < 1e-4f && std::fabs(a.g - b.g) < 1e-4f && std::fabs(a.b - b.b) < 1e-4f &&
           std::fabs(a.a - b.a) < 1e-4f;
}

// A ScriptableColor with colour c (created once per colour).
Il2CppObject* TintObject(const std::string& module, const Color& c) {
    for (const Tint& t : g_tints)
        if (SameColor(t.c, c) && unity::IsAlive(t.obj)) return t.obj;
    if (!g.tintOk) return nullptr;
    bool ok = false;
    Il2CppObject* so = Call(module, g.soCreate, nullptr, {unity::TypeOf(g.colorClass)}, "ScriptableObject.CreateInstance", &ok);
    if (!ok || !unity::IsAlive(so) || !IsA(so, g.colorClass)) return nullptr;
    unity::Field<Color>(so, g.offColor) = c;
    Il2CppGCHandle h = unity::NewGCHandle(so);
    if (!h) return nullptr;
    unity::SetName(so, "sr2mods_ButtonTint");
    g_tints.push_back({c, so, h});
    return so;
}

}  // namespace

// ---------------------------------------------------------------- resolve --

bool Resolve(Resolver& R, std::string& why) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g.done) {
        why = g.why;
        return g.ok;
    }
    g.done = true;
    std::string err, report;
    auto parentOf = [](Il2CppClass* c) { return api().il2cpp_class_get_parent ? api().il2cpp_class_get_parent(c) : nullptr; };
    auto cls = [&](const char* image, const char* ns, const char* name) { return R.Class(image, ns, name, &err); };
    auto method = [&](Il2CppClass* k, const char* name, int argc) -> const MethodInfo* {
        for (Il2CppClass* c = k; c; c = parentOf(c))
            if (const MethodInfo* m = R.Method(c, name, argc, &err)) return m;
        return nullptr;
    };
    auto off = [&](Il2CppClass* k, const char* field, size_t& out) {
        for (Il2CppClass* c = k; c; c = parentOf(c))
            if (auto o = R.FieldOffset(c, field, &err)) {
                out = *o;
                return true;
            }
        return false;
    };

    HMODULE ga = api().module;
    g.objectNew = reinterpret_cast<G::ObjectNewFn>(ga ? GetProcAddress(ga, "il2cpp_object_new") : nullptr);
    g.goClass = cls(kCore, "UnityEngine", "GameObject");
    Il2CppClass* transform = cls(kCore, "UnityEngine", "Transform");
    Il2CppClass* uobject = cls(kCore, "UnityEngine", "Object");
    Il2CppClass* behaviour = cls(kCore, "UnityEngine", "Behaviour");
    Il2CppClass* scriptableObject = cls(kCore, "UnityEngine", "ScriptableObject");
    g.rtClass = cls(kCore, "UnityEngine", "RectTransform");
    Il2CppClass* selectable = cls(kUi, "UnityEngine.UI", "Selectable");
    g.uiButtonClass = cls(kUi, "UnityEngine.UI", "Button");
    g.tmpTextClass = cls(kTmp, "TMPro", "TMP_Text");
    Il2CppClass* gameCtx = cls(kImage, "", "GameContext");
    Il2CppClass* templates = cls(kImage, "MonomiPark.SlimeRancher.UI", "UITemplates");
    g.popupClass = cls(kImage, "MonomiPark.SlimeRancher.UI.Popup", "PopupPrompt");
    g.ieButtonClass = cls(kImage, "MonomiPark.SlimeRancher.Input", "InputEventButton");
    g.moreViewClass = cls(kImage, "MonomiPark.SlimeRancher.UI.Adapter", "DetailsMoreButtonView");
    g.animatorClass = cls(kImage, "MonomiPark.SlimeRancher.UI", "ButtonAnimator");
    g.layoutMgrClass = cls(kImage, "MonomiPark.SlimeRancher.UI.Framework.Layout", "LayoutManager");
    g.resizeClass = cls(kImage, "MonomiPark.SlimeRancher.UI.Framework.Layout", "ResizeToPreferred");
    g.displayClass = cls(kImage, "MonomiPark.SlimeRancher.UI.Framework.CommonControls", "InputEventDisplay");
    g.iconClass = cls(kImage, "MonomiPark.SlimeRancher.UI.Framework.CommonControls", "InputIcon");
    g.colorClass = cls(kImage, "MonomiPark.SlimeRancher.UI", "ScriptableColor");
    g.lseClass = cls("Unity.Localization", "UnityEngine.Localization.Components", "LocalizeStringEvent");

    if (gameCtx)
        if (Il2CppClass* single = parentOf(gameCtx)) g.gcInstance = R.Method(single, "get_Instance", 0, &err);
    g.gcGetTemplates = method(gameCtx, "get_UITemplates", 0);
    g.goCtor = g.goClass ? R.MethodByParams(g.goClass, ".ctor", {"System.String"}, &err) : nullptr;
    g.goAddComponent = g.goClass ? R.MethodByParams(g.goClass, "AddComponent", {"System.Type"}, &err) : nullptr;
    g.objDestroy = uobject ? R.MethodByParams(uobject, "Destroy", {"UnityEngine.Object"}, &err) : nullptr;
    g.trSetParent = transform ? R.MethodByParams(transform, "SetParent", {"UnityEngine.Transform", "System.Boolean"}, &err) : nullptr;
    g.trFind = transform ? R.MethodByParams(transform, "Find", {"System.String"}, &err) : nullptr;
    g.trSetLocalScale = method(transform, "set_localScale", 1);
    g.trChildCount = method(transform, "get_childCount", 0);
    g.trGetChild = method(transform, "GetChild", 1);
    g.rtSetAnchorMin = method(g.rtClass, "set_anchorMin", 1);
    g.rtSetAnchorMax = method(g.rtClass, "set_anchorMax", 1);
    g.rtSetPivot = method(g.rtClass, "set_pivot", 1);
    g.rtSetSize = method(g.rtClass, "set_sizeDelta", 1);
    g.rtSetPos = method(g.rtClass, "set_anchoredPosition", 1);
    g.rtGetRect = method(g.rtClass, "get_rect", 0);
    g.behSetEnabled = method(behaviour, "set_enabled", 1);
    g.selSetInteractable = method(selectable, "set_interactable", 1);
    g.selIsPressed = method(selectable, "IsPressed", 0);
    g.selPointerInside = method(selectable, "get_isPointerInside", 0);
    g.selSetNav = method(selectable, "set_navigation", 1);
    g.tmpSetText = method(g.tmpTextClass, "set_text", 1);
    g.tmpGetText = method(g.tmpTextClass, "get_text", 0);
    g.tmpSetAlign = method(g.tmpTextClass, "set_alignment", 1);
    g.animInteractableChanged = method(g.animatorClass, "OnInteractableChanged", 0);
    g.soCreate = scriptableObject ? R.MethodByParams(scriptableObject, "CreateInstance", {"System.Type"}, &err) : nullptr;

    g.ok = g.objectNew && g.goClass && transform && g.rtClass && selectable && g.uiButtonClass && g.tmpTextClass &&
           g.popupClass && g.ieButtonClass && g.moreViewClass && g.gcInstance && g.gcGetTemplates && g.goCtor &&
           g.goAddComponent && g.objDestroy && g.trSetParent && g.trFind && g.trSetLocalScale && g.rtSetAnchorMin &&
           g.rtSetAnchorMax && g.rtSetPivot && g.rtSetSize && g.rtSetPos && g.rtGetRect && g.behSetEnabled &&
           g.selSetInteractable && g.selIsPressed && g.selPointerInside && g.tmpSetText && templates &&
           off(templates, "popupPrompt", g.offTplPopup) && off(g.popupClass, "_positiveActionButton", g.offPopupPositive) &&
           off(g.popupClass, "_negativeActionButton", g.offPopupNegative) && off(g.moreViewClass, "label", g.offMoreLabel);
    if (!g.ok) {
        g.why = "the popup button's classes / members are missing" + (err.empty() ? std::string() : " (" + err + ")");
        why = g.why;
        return false;
    }
    if (!g.layoutMgrClass) report += " layout: NOT available (prefab layout);";
    if (!g.selSetNav) report += " navigation: NOT set;";
    g.glyphOk = g.displayClass && g.iconClass && g.trChildCount && g.trGetChild && g.tmpGetText &&
                off(g.iconClass, "_buttonRoot", g.offIconButton) && off(g.iconClass, "_keyRoot", g.offIconKey) &&
                off(g.iconClass, "_keyText", g.offIconKeyText) && off(g.iconClass, "_keyImage", g.offIconKeyImage) &&
                off(g.iconClass, "_keyLocalizeStringEvent", g.offIconKeyLse) && off(g.iconClass, "_doubleTapRoot", g.offIconDouble);
    if (!g.glyphOk) report += " own key glyphs: NOT available (the popup's binding glyph);";
    g.tintOk = g.animatorClass && g.colorClass && g.soCreate && g.animInteractableChanged &&
               off(g.animatorClass, "ColorNormal", g.offAnimNormal) && off(g.animatorClass, "ColorSelected", g.offAnimSelected) &&
               off(g.colorClass, "Color", g.offColor);
    if (!g.tintOk) report += " tints / disabled grey: NOT available;";
    if (size_t maxW = 0; g.resizeClass && g.tmpSetAlign && off(g.resizeClass, "_minWidth", g.offResizeMinW) &&
                         off(g.resizeClass, "_maxWidth", maxW) && maxW == g.offResizeMinW + 8)
        g.minWidthOk = true;  // Optional<float> = { bool _hasValue; float _value; } (8 bytes, dump)
    if (!g.minWidthOk) report += " minimum width: NOT available;";
    g.why = report;
    why = report;
    return true;
}

bool Available() { return g.done && g.ok; }

// ---------------------------------------------------------------- button --

Button::~Button() { Release(); }

Button::Button(Button&& o) noexcept { *this = std::move(o); }

Button& Button::operator=(Button&& o) noexcept {
    if (this == &o) return *this;
    Release();
    module_ = std::move(o.module_);
    slot_ = std::exchange(o.slot_, nullptr);
    go_ = std::exchange(o.go_, nullptr);
    button_ = std::exchange(o.button_, nullptr);
    animator_ = std::exchange(o.animator_, nullptr);
    label_ = std::exchange(o.label_, nullptr);
    origNormal_ = std::exchange(o.origNormal_, nullptr);
    origSelected_ = std::exchange(o.origSelected_, nullptr);
    iconBg_ = std::exchange(o.iconBg_, nullptr);
    display_ = std::exchange(o.display_, nullptr);
    icons_ = std::move(o.icons_);
    o.icons_.clear();
    padKeyText_ = std::exchange(o.padKeyText_, nullptr);
    padKeyRoot_ = std::exchange(o.padKeyRoot_, nullptr);
    handles_ = std::move(o.handles_);
    o.handles_.clear();
    label_text_ = std::move(o.label_text_);
    key_ = std::move(o.key_);
    padGlyph_ = o.padGlyph_;
    popupGlyph_ = o.popupGlyph_;
    interactable_ = o.interactable_;
    colorDirty_ = o.colorDirty_;
    wasPressed_ = o.wasPressed_;
    tinted_ = o.tinted_;
    tint_ = o.tint_;
    std::copy(std::begin(o.anchor_), std::end(o.anchor_), anchor_);
    std::copy(std::begin(o.pos_), std::end(o.pos_), pos_);
    std::copy(std::begin(o.pivot_), std::end(o.pivot_), pivot_);
    scale_ = o.scale_;
    reapply_ = o.reapply_;
    return *this;
}

void Button::Release() {
    slot_ = go_ = button_ = animator_ = label_ = iconBg_ = display_ = padKeyText_ = padKeyRoot_ = nullptr;
    origNormal_ = origSelected_ = nullptr;
    icons_.clear();
    FreeLater(handles_);
}

bool Button::Alive() const { return slot_ && unity::IsAlive(slot_) && unity::IsAlive(go_); }

bool Button::Create(Il2CppObject* parentTr, const Spec& spec, std::string& why) {
    Release();
    module_ = spec.module ? spec.module : "game_button";
    if (!Available()) {
        why = "game button not available";
        return false;
    }
    if (sr2::core::ModuleFaulted(module_)) {
        why = "module faulted";
        return false;
    }
    const std::string& M = module_;
    bool ok = false;
    Il2CppObject* gc = Call(M, g.gcInstance, nullptr, {}, "GameContext.Instance", &ok);
    if (!ok || !unity::IsAlive(gc)) return why = "no GameContext", false;
    Il2CppObject* tpl = Call(M, g.gcGetTemplates, gc, {}, "GameContext.get_UITemplates", &ok);
    if (!ok || !unity::IsAlive(tpl)) return why = "no UITemplates", false;
    Il2CppObject* popupGo = unity::Field<Il2CppObject*>(tpl, g.offTplPopup);
    Il2CppObject* popup = unity::IsAlive(popupGo) ? unity::GetComponent(popupGo, g.popupClass) : nullptr;
    if (!unity::IsAlive(popup)) return why = "no PopupPrompt template", false;
    Il2CppObject* src = unity::Field<Il2CppObject*>(popup, spec.source == Source::kPositive ? g.offPopupPositive : g.offPopupNegative);
    if (!unity::IsAlive(src) || !IsA(src, g.goClass)) return why = "the popup button is not a GameObject", false;

    // The slot: a zero-size RectTransform that carries the scale and the placement.
    Il2CppObject* slot = g.objectNew(g.goClass);
    if (!slot) return why = "il2cpp_object_new failed", false;
    Call(M, g.goCtor, slot, {unity::NewString(spec.name + "Slot")}, "GameObject..ctor", &ok);
    if (!ok || !unity::IsAlive(slot)) return why = "slot GameObject failed", false;
    Il2CppObject* rtc = Call(M, g.goAddComponent, slot, {unity::TypeOf(g.rtClass)}, "AddComponent(RectTransform)", &ok);
    bool stays = false;
    if (ok && unity::IsAlive(rtc)) Call(M, g.trSetParent, unity::TransformOf(slot), {parentTr, &stays}, "Transform.SetParent", &ok);
    if (!ok || !unity::IsAlive(rtc)) {
        Call(M, g.objDestroy, nullptr, {slot}, "Object.Destroy");
        return why = "slot RectTransform failed", false;
    }
    slot_ = slot;
    Keep(handles_, slot);
    Il2CppObject* slotRt = unity::TransformOf(slot);
    // Layer of the parent's GameObject (a dialog may sit on a UI layer); Instantiate keeps the prefab's.
    scale_ = spec.scale > 0.0f ? spec.scale : 1.0f;
    float s3[3] = {scale_, scale_, 1.0f};
    Call(M, g.trSetLocalScale, slotRt, {s3}, "Transform.set_localScale");
    Vec2 zero{0, 0};
    Call(M, g.rtSetSize, slotRt, {&zero}, "RectTransform.set_sizeDelta");

    Il2CppObject* go = unity::Instantiate(src, slotRt);
    if (!unity::IsAlive(go)) {
        Destroy();
        return why = "Instantiate failed", false;
    }
    go_ = go;
    Keep(handles_, go);
    unity::SetName(go, spec.name);
    button_ = unity::GetComponent(go, g.uiButtonClass);
    if (!unity::IsAlive(button_)) {
        Destroy();
        return why = "no Button on the clone", false;
    }
    Keep(handles_, button_);
    animator_ = g.animatorClass ? unity::GetComponent(go, g.animatorClass) : nullptr;
    if (!unity::IsAlive(animator_)) animator_ = nullptr;
    Keep(handles_, animator_);
    if (animator_ && g.tintOk) {
        origNormal_ = unity::Field<Il2CppObject*>(animator_, g.offAnimNormal);
        origSelected_ = unity::Field<Il2CppObject*>(animator_, g.offAnimSelected);
    }
    // Its pointer down / up would perform the popup's InputEvent (a menu submit / cancel elsewhere).
    if (Il2CppObject* ieb = unity::GetComponent(go, g.ieButtonClass); unity::IsAlive(ieb)) {
        bool off = false;
        Call(M, g.behSetEnabled, ieb, {&off}, "Behaviour.set_enabled(InputEventButton)");
    }
    // Navigation None: a press must not select it (ButtonAnimator would then stop showing the hover).
    if (g.selSetNav) {
        alignas(8) unsigned char nav[64] = {};  // Navigation.Mode.None, no explicit targets
        Call(M, g.selSetNav, button_, {nav}, "Selectable.set_navigation");
    }
    // Label: the LocalizeStringEvent off (it would write "Confirm" / "Cancel" back), our text in.
    Il2CppObject* view = unity::GetComponent(go, g.moreViewClass);
    Il2CppObject* lse = unity::IsAlive(view) ? unity::Field<Il2CppObject*>(view, g.offMoreLabel) : nullptr;
    if (unity::IsAlive(lse)) {
        bool off = false;
        Call(M, g.behSetEnabled, lse, {&off}, "Behaviour.set_enabled(LocalizeStringEvent)");
        label_ = unity::GetComponent(lse, g.tmpTextClass);
    }
    if (!unity::IsAlive(label_)) {
        Destroy();
        return why = "no label text on the clone", false;
    }
    Keep(handles_, label_);
    // Minimum label width: the label's ResizeToPreferred gets a minimum, the text is centred in it.
    if (spec.minWidth > 0.0f && g.minWidthOk)
        if (Il2CppObject* rz = unity::GetComponent(label_, g.resizeClass); unity::IsAlive(rz)) {
            auto* p = reinterpret_cast<char*>(rz) + g.offResizeMinW;
            *reinterpret_cast<bool*>(p) = true;
            *reinterpret_cast<float*>(p + 4) = spec.minWidth / scale_;
            int32_t center = kAlignCenter;
            Call(M, g.tmpSetAlign, label_, {&center}, "TMP_Text.set_alignment");
        }
    // The glyph: "Icon Background" / InputEventDisplay / Icon Group / InputIcons.
    Il2CppObject* goTr = unity::TransformOf(go);
    bool found = false;
    Il2CppObject* bg = Call(M, g.trFind, goTr, {unity::NewString("Icon Background")}, "Transform.Find", &found);
    if (found && unity::IsAlive(bg)) {
        iconBg_ = unity::GameObjectOf(bg);
        Keep(handles_, iconBg_);
        Il2CppObject* ied = Call(M, g.trFind, bg, {unity::NewString("InputEventDisplay")}, "Transform.Find", &found);
        if (found && unity::IsAlive(ied) && g.displayClass) {
            display_ = unity::GetComponent(ied, g.displayClass);
            if (!unity::IsAlive(display_)) display_ = nullptr;
            Keep(handles_, display_);
        }
        Il2CppObject* group = Call(M, g.trFind, bg, {unity::NewString("InputEventDisplay/Icon Group")}, "Transform.Find", &found);
        if (found && unity::IsAlive(group) && g.glyphOk) {
            const int n = Unbox<int32_t>(Call(M, g.trChildCount, group, {}, "Transform.get_childCount", &ok));
            for (int32_t i = 0; ok && i < n && i < 8; ++i) {
                Il2CppObject* c = Call(M, g.trGetChild, group, {&i}, "Transform.GetChild");
                Il2CppObject* icon = unity::IsAlive(c) ? unity::GetComponent(c, g.iconClass) : nullptr;
                if (!unity::IsAlive(icon)) continue;
                icons_.push_back(icon);
                Keep(handles_, icon);
            }
        }
    }
    // Laid out like in the popup: the game's layout behaviours run under a LayoutManager only.
    if (g.layoutMgrClass) Call(M, g.goAddComponent, go, {unity::TypeOf(g.layoutMgrClass)}, "AddComponent(LayoutManager)");

    label_text_ = spec.label;
    key_ = spec.key;
    padGlyph_ = spec.padGlyph;
    popupGlyph_ = spec.popupGlyph;
    interactable_ = true;
    tinted_ = false;
    wasPressed_ = false;
    colorDirty_ = true;
    // Default placement: centred in the parent's top-left corner (callers Place it).
    PlaceAnchored(0.0f, 1.0f, 0.0f, 0.0f, 0.5f, 0.5f);
    Apply();
    reapply_ = kReapplyFrames;
    if (sr2::core::ModuleFaulted(module_)) {
        Forget();
        return why = "module faulted", false;
    }
    return true;
}

void Button::PlaceAnchored(float anchorX, float anchorY, float posX, float posY, float pivotX, float pivotY) {
    anchor_[0] = anchorX;
    anchor_[1] = anchorY;
    pos_[0] = posX;
    pos_[1] = posY;
    pivot_[0] = pivotX;
    pivot_[1] = pivotY;
    if (!Alive()) return;
    Il2CppObject* srt = unity::TransformOf(slot_);
    Vec2 a{anchorX, anchorY}, p{posX, posY};
    Call(module_, g.rtSetAnchorMin, srt, {&a}, "RectTransform.set_anchorMin");
    Call(module_, g.rtSetAnchorMax, srt, {&a}, "RectTransform.set_anchorMax");
    Call(module_, g.rtSetPos, srt, {&p}, "RectTransform.set_anchoredPosition");
    // The button hangs off its pivot at the slot's centre; its layout sizes it around that pivot.
    Il2CppObject* brt = unity::TransformOf(go_);
    Vec2 mid{0.5f, 0.5f}, piv{pivotX, pivotY}, zero{0, 0};
    Call(module_, g.rtSetAnchorMin, brt, {&mid}, "RectTransform.set_anchorMin");
    Call(module_, g.rtSetAnchorMax, brt, {&mid}, "RectTransform.set_anchorMax");
    Call(module_, g.rtSetPivot, brt, {&piv}, "RectTransform.set_pivot");
    Call(module_, g.rtSetPos, brt, {&zero}, "RectTransform.set_anchoredPosition");
    reapply_ = std::max(reapply_, kReapplyFrames);
}

void Button::Place(float x, float y, float pivotX, float pivotY) { PlaceAnchored(0.0f, 1.0f, x, -y, pivotX, pivotY); }

void Button::SetLabel(std::string_view text) {
    if (text == label_text_) return;
    label_text_ = std::string(text);
    if (Alive()) Call(module_, g.tmpSetText, label_, {unity::NewString(label_text_)}, "TMP_Text.set_text");
    reapply_ = std::max(reapply_, 4);
}

void Button::SetKey(std::string_view key) {
    if (key == key_) return;
    key_ = std::string(key);
    ApplyGlyph();
    reapply_ = std::max(reapply_, 4);
}

void Button::SetInteractable(bool on) {
    if (on == interactable_) return;
    interactable_ = on;
    if (Alive()) Call(module_, g.selSetInteractable, button_, {&on}, "Selectable.set_interactable");
    if (!on) wasPressed_ = false;
    colorDirty_ = true;
}

void Button::SetTint(const Color* normal) {
    const bool want = normal != nullptr;
    if (want == tinted_ && (!want || SameColor(*normal, tint_))) return;
    tinted_ = want;
    if (want) tint_ = *normal;
    if (!Alive() || !animator_ || !g.tintOk) return;
    // The prefab's pink asset is both ColorNormal and ColorSelected; a tint replaces both, untinted puts the
    // clone's own assets back. (Managed field writes: the animator is a live MonoBehaviour, its fields are
    // GC roots for the tint objects, which we also keep for the session.)
    Il2CppObject* useNormal = origNormal_, *useSelected = origSelected_;
    if (want) {
        Il2CppObject* c = TintObject(module_, tint_);
        if (!c) return;
        useNormal = useSelected = c;
    }
    unity::Field<Il2CppObject*>(animator_, g.offAnimNormal) = useNormal;
    unity::Field<Il2CppObject*>(animator_, g.offAnimSelected) = useSelected;
    colorDirty_ = true;
}

void Button::SetActive(bool on) {
    if (Alive()) unity::SetActive(slot_, on);
    if (on) {
        colorDirty_ = true;
        reapply_ = std::max(reapply_, 4);
    }
}

void Button::Apply() {
    if (!Alive()) return;
    Call(module_, g.tmpSetText, label_, {unity::NewString(label_text_)}, "TMP_Text.set_text");
    ApplyGlyph();
}

void Button::ApplyGlyph() {
    if (!Alive() || !unity::IsAlive(iconBg_)) return;
    const bool show = !key_.empty() || popupGlyph_;
    unity::SetActive(iconBg_, show);
    if (!show) return;
    if (key_.empty() || padGlyph_ || !g.glyphOk || icons_.empty()) {
        // The source binding's own display (padGlyph: the key text is replaced in PadGlyphTick).
        if (display_) {
            bool on = true;
            Call(module_, g.behSetEnabled, display_, {&on}, "Behaviour.set_enabled(InputEventDisplay)");
        }
        if (padGlyph_ && !key_.empty() && !padKeyText_ && !icons_.empty()) {
            // First icon's key text: its LocalizeStringEvent would write the bound key's name back.
            Il2CppObject* icon = icons_[0];
            Il2CppObject* text = unity::Field<Il2CppObject*>(icon, g.offIconKeyText);
            Il2CppObject* root = unity::Field<Il2CppObject*>(icon, g.offIconKey);
            if (unity::IsAlive(text) && unity::IsAlive(root)) {
                if (Il2CppObject* tlse = unity::Field<Il2CppObject*>(icon, g.offIconKeyLse); unity::IsAlive(tlse)) {
                    bool off = false;
                    Call(module_, g.behSetEnabled, tlse, {&off}, "Behaviour.set_enabled(LocalizeStringEvent)");
                }
                padKeyText_ = text;
                padKeyRoot_ = root;
                Keep(handles_, text);
                Keep(handles_, root);
            }
        }
        return;
    }
    // Our key cap: the display off (it follows the device / binding), the first InputIcon set as
    // InputIcon.SetKey does (button root off, key root on, key image off, key text on), the others hidden.
    if (display_) {
        bool off = false;
        Call(module_, g.behSetEnabled, display_, {&off}, "Behaviour.set_enabled(InputEventDisplay)");
    }
    for (size_t i = 0; i < icons_.size(); ++i) {
        Il2CppObject* icon = icons_[i];
        if (!unity::IsAlive(icon)) continue;
        Il2CppObject* iconGo = unity::GameObjectOf(icon);
        if (i > 0) {
            unity::SetActive(iconGo, false);
            continue;
        }
        unity::SetActive(iconGo, true);
        Il2CppObject* btn = unity::Field<Il2CppObject*>(icon, g.offIconButton);
        Il2CppObject* keyRoot = unity::Field<Il2CppObject*>(icon, g.offIconKey);
        Il2CppObject* dbl = unity::Field<Il2CppObject*>(icon, g.offIconDouble);
        Il2CppObject* text = unity::Field<Il2CppObject*>(icon, g.offIconKeyText);
        if (unity::IsAlive(btn)) unity::SetActive(btn, false);
        if (unity::IsAlive(dbl)) unity::SetActive(dbl, false);
        if (unity::IsAlive(keyRoot)) unity::SetActive(keyRoot, true);
        bool off = false, on = true;
        if (Il2CppObject* img = unity::Field<Il2CppObject*>(icon, g.offIconKeyImage); unity::IsAlive(img))
            Call(module_, g.behSetEnabled, img, {&off}, "Behaviour.set_enabled(key image)");
        if (Il2CppObject* tlse = unity::Field<Il2CppObject*>(icon, g.offIconKeyLse); unity::IsAlive(tlse))
            Call(module_, g.behSetEnabled, tlse, {&off}, "Behaviour.set_enabled(key LocalizeStringEvent)");
        if (unity::IsAlive(text)) {
            Call(module_, g.behSetEnabled, text, {&on}, "Behaviour.set_enabled(key text)");
            Call(module_, g.tmpSetText, text, {unity::NewString(key_)}, "TMP_Text.set_text");
        }
    }
}

// padGlyph: while the display shows a keyboard key cap, it reads our key and the other icons are hidden.
void Button::PadGlyphTick() {
    if (!padGlyph_ || key_.empty() || !unity::IsAlive(padKeyText_) || !unity::IsAlive(padKeyRoot_) ||
        !unity::ActiveInHierarchy(padKeyRoot_))
        return;  // a pad glyph (or none): the display's own
    bool ok = false;
    Il2CppObject* s = Call(module_, g.tmpGetText, padKeyText_, {}, "TMP_Text.get_text", &ok);
    if (ok && unity::ToUtf8(reinterpret_cast<Il2CppString*>(s)) != key_)
        Call(module_, g.tmpSetText, padKeyText_, {unity::NewString(key_)}, "TMP_Text.set_text");
    for (size_t i = 1; i < icons_.size(); ++i)
        if (Il2CppObject* o = unity::IsAlive(icons_[i]) ? unity::GameObjectOf(icons_[i]) : nullptr;
            o && unity::ActiveInHierarchy(o))
            unity::SetActive(o, false);
}

bool Button::Tick() {
    if (!Alive() || sr2::core::ModuleFaulted(module_)) return false;
    if (!unity::ActiveInHierarchy(go_)) {
        wasPressed_ = false;
        return false;
    }
    if (reapply_ > 0) {
        --reapply_;
        Il2CppObject* brt = unity::TransformOf(go_);
        Vec2 piv{pivot_[0], pivot_[1]}, zero{0, 0};
        Call(module_, g.rtSetPivot, brt, {&piv}, "RectTransform.set_pivot");
        Call(module_, g.rtSetPos, brt, {&zero}, "RectTransform.set_anchoredPosition");
        if (reapply_ % 4 == 0) Apply();  // the display / layout may still be configuring itself
    }
    PadGlyphTick();
    // Colours: the animator applies Normal / Disabled when told (its own copy of `interactable` was taken in
    // Awake, before or after our change). Only once active: DOTween animates live graphics.
    if (colorDirty_ && animator_ && g.animInteractableChanged) {
        colorDirty_ = false;
        Call(module_, g.animInteractableChanged, animator_, {}, "ButtonAnimator.OnInteractableChanged");
    }
    bool ok = false, ok2 = false;
    const bool pressed = Unbox<bool>(Call(module_, g.selIsPressed, button_, {}, "Selectable.IsPressed", &ok)) && ok;
    const bool inside = Unbox<bool>(Call(module_, g.selPointerInside, button_, {}, "Selectable.get_isPointerInside", &ok2)) && ok2;
    const bool click = wasPressed_ && !pressed && inside && interactable_;
    wasPressed_ = pressed;
    return click;
}

bool Button::Hovered() const {
    if (!Alive()) return false;
    bool ok = false;
    return Unbox<bool>(Call(module_, g.selPointerInside, button_, {}, "Selectable.get_isPointerInside", &ok)) && ok;
}

void Button::Size(float& w, float& h) const {
    w = h = 0.0f;
    if (!Alive()) return;
    bool ok = false;
    Il2CppObject* boxed = Call(module_, g.rtGetRect, unity::TransformOf(go_), {}, "RectTransform.get_rect", &ok);
    if (!ok || !boxed) return;
    const float* r = reinterpret_cast<const float*>(reinterpret_cast<char*>(boxed) + 2 * sizeof(void*));
    w = r[2] * scale_;
    h = r[3] * scale_;
}

void Button::Destroy() {
    if (slot_ && unity::IsAlive(slot_) && !sr2::core::ModuleFaulted(module_)) {
        unity::SetActive(slot_, false);
        Call(module_, g.objDestroy, nullptr, {slot_}, "Object.Destroy");
    }
    Release();
}

void Button::Forget() { Release(); }

}  // namespace sr2::modules::game_button
