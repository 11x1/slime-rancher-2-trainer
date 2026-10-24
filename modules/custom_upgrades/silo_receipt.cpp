// custom_upgrades: the silo Sell-All receipt dialog (see silo_receipt.h). SR2 1.3.0, all lookups by name.
//
// Layout (CanvasScaler reference 1920 x 1080, the panel centred):
//   title "Sell-All receipt" + one line of explanation
//   column labels   Plort | Count | Price | Total
//   one row per plort type: inner box (kBacker), icon, localized name, "x N", unit price, line total
//   separator, "Total" + "N plorts" + grand total with the Newbucks coin
//   note line (orange; empty unless the receipt was refreshed / the market closed)
//   [ (Esc) Cancel ]                                             [ (Enter) Sell ]
// The panel never gets taller than the canvas (Screen size through the CanvasScaler formula): rows
// shrink (56 -> 40 px) when there are many types, then the row list scrolls (uGUI ScrollRect in a
// RectMask2D viewport, a thin Scrollbar on the right; mouse wheel / Up / Down / W / S / d-pad / left
// stick up / down step one row). A silo has 12 slots, so 12 rows at most.
// Buttons: clones of the game's own popup buttons (GameContext.UITemplates.popupPrompt ->
// PopupPrompt._positiveActionButton / _negativeActionButton, the prefab TextButton_NoneSelectable,
// gamecore_scenes bundle read with UnityPy: Button + ButtonAnimator (hover animation and colours) +
// DetailsMoreButtonView (label = LocalizeStringEvent) + InputEventButton (performs the popup's
// InputEvent on pointer down / up: disabled on the clone) + InputEventDisplay under "Icon Background"
// (the key icon of the popup's positive / negative InputEvent, kept), self-sizing through
// EncapsulateChildren). Our label goes into the TMP text (its LocalizeStringEvent disabled); a polled
// Toggle on the clone's root takes the click; the keyboard focus is a coloured ring behind the button.
// Without the prefab: our own SDF buttons with the keys in the label, as before.
// The palette is the game's menu one (drone station / plot menu scene data): navy text, white
// content, lavender border, light blue inner boxes, teal focus, pink accent (the plot menu's scroll
// handle, used by the Hen Collector sliders too).

#include "modules/custom_upgrades/silo_receipt.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <format>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

#include "core/detour_guard.h"
#include "core/il2cpp_api.h"
#include "core/log.h"
#include "core/main_thread.h"
#include "core/module.h"
#include "core/resolver.h"
#include "core/unity.h"

namespace sr2::modules::custom_upgrades::ui {

long long Receipt::Total() const {
    long long t = 0;
    for (const ReceiptLine& l : lines) t += l.Total();
    return t;
}

int Receipt::Count() const {
    int n = 0;
    for (const ReceiptLine& l : lines) n += l.count;
    return n;
}

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

constexpr int kAlignLeft = 513, kAlignCenter = 514, kAlignRight = 516;  // TMP TextAlignmentOptions
constexpr float kGraceSeconds = 0.3f;  // input ignored right after opening
constexpr int kSortingOrder = 1000;    // above the HUD

struct Vec2 {
    float x, y;
};
struct Color {
    float r, g, b, a;
};

constexpr Color kNavy{0.267f, 0.325f, 0.478f, 1.0f};
constexpr Color kNavyDim{0.267f, 0.325f, 0.478f, 0.70f};
constexpr Color kWhite{1.0f, 1.0f, 1.0f, 1.0f};
constexpr Color kLavender{0.737f, 0.686f, 0.824f, 1.0f};
constexpr Color kBacker{0.875f, 0.902f, 0.945f, 1.0f};
constexpr Color kTeal{0.063f, 0.675f, 0.757f, 1.0f};
constexpr Color kPink{0.871f, 0.310f, 0.522f, 1.0f};
constexpr Color kGrey{0.78f, 0.80f, 0.84f, 1.0f};
constexpr Color kOrange{0.90f, 0.45f, 0.12f, 1.0f};

constexpr float kPanelW = 980.0f;
constexpr float kPad = 40.0f;
constexpr float kInnerW = kPanelW - 2 * kPad;
constexpr float kButtonW = 320.0f, kButtonH = 64.0f;

// "12,345"
std::string Group(long long v) {
    std::string s = std::to_string(v < 0 ? -v : v);
    for (int i = static_cast<int>(s.size()) - 3; i > 0; i -= 3) s.insert(static_cast<size_t>(i), ",");
    return v < 0 ? "-" + s : s;
}

}  // namespace

struct SiloReceipt::Impl {
    const char* module = "custom_upgrades";

    // ---- classes ----
    Il2CppClass* goClass = nullptr, *rtClass = nullptr, *imageClass = nullptr, *toggleClass = nullptr;
    Il2CppClass* canvasClass = nullptr, *scalerClass = nullptr, *raycasterClass = nullptr;
    Il2CppClass* tmpTextClass = nullptr, *tmpUguiClass = nullptr, *sdfRectClass = nullptr;
    Il2CppClass* lockerClass = nullptr, *identClass = nullptr, *lsClass = nullptr;

    // ---- offsets ----
    size_t offIdentIcon = 0, offIdentName = 0;
    size_t offGcLookup = 0, offLookupCurrencies = 0, offCurrencyArr = 0, offCurrencyIcon = 0;  // optional (coin)

    // ---- methods ----
    const MethodInfo* goCtor = nullptr, *goAddComponent = nullptr;
    const MethodInfo* trSetParent = nullptr;
    const MethodInfo* rtSetAnchorMin = nullptr, *rtSetAnchorMax = nullptr, *rtSetPivot = nullptr, *rtSetSize = nullptr,
                    *rtSetPos = nullptr;
    const MethodInfo* objDestroy = nullptr;
    const MethodInfo* gfxSetColor = nullptr, *gfxSetRaycast = nullptr, *imgSetSprite = nullptr, *imgSetPreserve = nullptr;
    const MethodInfo* tmpSetText = nullptr, *tmpGetFont = nullptr, *tmpSetFont = nullptr, *tmpSetSize = nullptr,
                    *tmpSetColor = nullptr, *tmpSetAlign = nullptr, *tmpSetWrap = nullptr, *tmpSetOverflow = nullptr;
    const MethodInfo* cvSetMode = nullptr, *cvSetOrder = nullptr;
    const MethodInfo* scSetMode = nullptr, *scSetRef = nullptr, *scSetMatch = nullptr;
    const MethodInfo* sdfSetRadius = nullptr, *sdfSetBorder = nullptr, *sdfSetBorderColor = nullptr;
    const MethodInfo* tglGetOn = nullptr, *tglSetOnQuiet = nullptr, *selSetTransition = nullptr, *selSetNav = nullptr;
    const MethodInfo* lsGet = nullptr, *gcInstance = nullptr;
    // input (InputSystem)
    const MethodInfo* kbCurrent = nullptr, *padCurrent = nullptr, *btnPressed = nullptr;
    const MethodInfo* kbEsc = nullptr, *kbEnter = nullptr, *kbNumEnter = nullptr, *kbSpace = nullptr, *kbLeft = nullptr,
                    *kbRight = nullptr, *kbA = nullptr, *kbD = nullptr, *kbTab = nullptr;
    const MethodInfo* padSouth = nullptr, *padEast = nullptr, *padDpad = nullptr, *padStick = nullptr;
    const MethodInfo* dirLeft = nullptr, *dirRight = nullptr;      // DpadControl.get_left / get_right
    const MethodInfo* stickLeft = nullptr, *stickRight = nullptr;  // StickControl.get_left / get_right
    // optional: scrolling rows (keys + mouse wheel) and the canvas height
    const MethodInfo* kbUp = nullptr, *kbDown = nullptr, *kbW = nullptr, *kbS = nullptr;
    const MethodInfo* dirUp = nullptr, *dirDown = nullptr, *stickUp = nullptr, *stickDown = nullptr;
    const MethodInfo* mouseCurrent = nullptr, *mouseScroll = nullptr, *vec2Y = nullptr, *axisRead = nullptr;
    const MethodInfo* screenW = nullptr, *screenH = nullptr;
    Il2CppClass* scrollRectClass = nullptr, *scrollbarClass = nullptr, *rectMaskClass = nullptr;
    const MethodInfo* srSetContent = nullptr, *srSetViewport = nullptr, *srSetHorizontal = nullptr,
                    *srSetVertical = nullptr, *srSetMovement = nullptr, *srSetSensitivity = nullptr,
                    *srGetNormY = nullptr, *srSetNormY = nullptr, *srSetScrollbar = nullptr,
                    *srSetScrollbarVis = nullptr;
    const MethodInfo* sbSetHandle = nullptr, *sbSetDirection = nullptr, *selSetTarget = nullptr;
    bool scrollOk = false;
    // optional: the game's popup buttons
    const MethodInfo* gcGetTemplates = nullptr, *rtGetRect = nullptr, *behSetEnabled = nullptr,
                    *selSetInteractable = nullptr;
    // The clone already has a Button (a Selectable: DisallowMultipleComponent), so no Toggle can go on it
    // (Player.log: "Can't add 'Toggle' to Cancel because a 'Button' is already added"). Its click is
    // polled from the Button's own state: Selectable.IsPressed() (protected), get_isPointerInside (private).
    const MethodInfo* selIsPressed = nullptr, *selPointerInside = nullptr;
    Il2CppClass* popupClass = nullptr, *ieButtonClass = nullptr, *moreViewClass = nullptr, *uiButtonClass = nullptr;
    size_t offTplPopup = 0, offPopupPositive = 0, offPopupNegative = 0, offMoreLabel = 0;
    bool gameButtonsOk = false;
    using ObjectNewFn = Il2CppObject* (*)(Il2CppClass*);
    ObjectNewFn objectNew = nullptr;

    // ---- live state (main thread) ----
    bool open = false;
    float grace = 0.0f;
    int focus = 1;  // 0 = Cancel, 1 = Sell
    bool canSell = true;
    std::vector<Il2CppGCHandle> handles;       // root-level objects (whole open)
    std::vector<Il2CppGCHandle> panelHandles;  // current panel (rebuilt by Refresh)
    Il2CppObject* root = nullptr, *rootRt = nullptr, *backdropToggle = nullptr;
    Il2CppObject* panel = nullptr;
    Il2CppObject* btnBg[2] = {}, *btnText[2] = {}, *btnToggle[2] = {};
    Il2CppObject* btnGame[2] = {};  // the cloned game button's root (nullptr: our own button)
    Il2CppObject* btnRing[2] = {};  // focus ring behind a game button (its SDF graphic)
    bool btnWasPressed[2] = {};     // a game button's IsPressed() in the last frame
    Vec2 btnCentre[2] = {};         // centre of a game button inside the panel (y down)
    int ringFrames = 0;             // frames left to fit the rings to the buttons' self-sized rects
    Il2CppObject* scrollRect = nullptr;
    float scrollStep = 0.0f;        // one row in normalized scroll units (0 = no scrolling)
    Il2CppObject* font = nullptr;  // TMP_FontAsset of a HUD text (this open only)
    bool loggedOpen = false, loggedFail = false, loggedButtons = false, loggedButtonMiss = false;

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
            else SR2_WARN("[{}] silo receipt: {} failed: {}", module, what, err);
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
    static void FreeLater(std::vector<Il2CppGCHandle>& hs) {
        std::vector<Il2CppGCHandle> doomed;
        doomed.swap(hs);
        if (!doomed.empty()) main_thread::Post([doomed] {
            for (Il2CppGCHandle h : doomed) unity::FreeGCHandle(h);
        });
    }
    Il2CppObject* AddComponent(Il2CppObject* go, Il2CppClass* k, const char* what) {
        if (!unity::IsAlive(go) || !k) return nullptr;
        bool ok = false;
        Il2CppObject* c = Call(goAddComponent, go, {unity::TypeOf(k)}, what, &ok);
        return ok && unity::IsAlive(c) && IsA(c, k) ? c : nullptr;
    }
    void SetVec(const MethodInfo* m, Il2CppObject* rt, Vec2 v, const char* what) { Call(m, rt, {&v}, what); }
    void Rect(Il2CppObject* rt, Vec2 amin, Vec2 amax, Vec2 pivot, Vec2 pos, Vec2 size) {
        SetVec(rtSetAnchorMin, rt, amin, "RectTransform.set_anchorMin");
        SetVec(rtSetAnchorMax, rt, amax, "RectTransform.set_anchorMax");
        SetVec(rtSetPivot, rt, pivot, "RectTransform.set_pivot");
        SetVec(rtSetSize, rt, size, "RectTransform.set_sizeDelta");
        SetVec(rtSetPos, rt, pos, "RectTransform.set_anchoredPosition");
    }
    // Top-left anchored rect inside the parent (y grows downwards).
    void At(Il2CppObject* rt, float x, float y, float w, float h) { Rect(rt, {0, 1}, {0, 1}, {0, 1}, {x, -y}, {w, h}); }
    Il2CppObject* NewGo(const std::string& name) {
        Il2CppObject* go = objectNew ? objectNew(goClass) : nullptr;
        if (!go) return nullptr;
        bool ok = false;
        Call(goCtor, go, {unity::NewString(name)}, "GameObject..ctor", &ok);
        return ok && unity::IsAlive(go) ? go : nullptr;
    }
    Il2CppObject* NewUi(const std::string& name, Il2CppObject* parentTr) {
        Il2CppObject* go = NewGo(name);
        if (!go) return nullptr;
        if (!AddComponent(go, rtClass, "AddComponent(RectTransform)")) {
            Destroy(go);
            return nullptr;
        }
        bool stays = false, ok = false;
        Call(trSetParent, unity::TransformOf(go), {parentTr, &stays}, "Transform.SetParent", &ok);
        if (!ok) {
            Destroy(go);
            return nullptr;
        }
        return go;
    }
    void Destroy(Il2CppObject* go) {
        if (unity::IsAlive(go)) Call(objDestroy, nullptr, {go}, "Object.Destroy");
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
        Il2CppObject* go = NewUi(name, parentRt);
        if (!go) return nullptr;
        At(unity::TransformOf(go), x, y, w, h);
        Il2CppObject* g = Shape(go, c, radius, border, borderColor);
        SetRaycast(g, false);
        return go;
    }
    Il2CppObject* Text(Il2CppObject* parentRt, const char* name, float x, float y, float w, float h, const std::string& s,
                       float size, Color col, int align, bool wrap = false) {
        Il2CppObject* go = NewUi(name, parentRt);
        if (!go) return nullptr;
        At(unity::TransformOf(go), x, y, w, h);
        Il2CppObject* tmp = AddComponent(go, tmpUguiClass, "AddComponent(TextMeshProUGUI)");
        if (!tmp) return nullptr;
        if (unity::IsAlive(font)) Call(tmpSetFont, tmp, {font}, "TMP_Text.set_font");
        Call(tmpSetSize, tmp, {&size}, "TMP_Text.set_fontSize");
        Call(tmpSetColor, tmp, {&col}, "TMP_Text.set_color");
        int32_t a = align, wrapMode = wrap ? 1 : 0, overflow = 1;  // TextWrappingModes Normal / NoWrap; Ellipsis
        Call(tmpSetAlign, tmp, {&a}, "TMP_Text.set_alignment");
        if (tmpSetWrap) Call(tmpSetWrap, tmp, {&wrapMode}, "TMP_Text.set_textWrappingMode");
        if (!wrap && tmpSetOverflow) Call(tmpSetOverflow, tmp, {&overflow}, "TMP_Text.set_overflowMode");
        SetRaycast(tmp, false);
        SetText(tmp, s);
        return tmp;
    }
    Il2CppObject* Sprite(Il2CppObject* parentRt, const char* name, float x, float y, float w, float h, Il2CppObject* sprite) {
        if (!unity::IsAlive(sprite)) return nullptr;
        Il2CppObject* go = NewUi(name, parentRt);
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
    // A pollable click target on `go` (whose graphic must be a raycast target).
    Il2CppObject* Clickable(Il2CppObject* go) {
        Il2CppObject* t = AddComponent(go, toggleClass, "AddComponent(Toggle)");
        if (!t) return nullptr;
        int32_t none = 0;                       // Selectable.Transition.None
        alignas(8) unsigned char nav[64] = {};  // Navigation.Mode.None
        Call(selSetTransition, t, {&none}, "Selectable.set_transition");
        Call(selSetNav, t, {nav}, "Selectable.set_navigation");
        bool off = false;
        Call(tglSetOnQuiet, t, {&off}, "Toggle.SetIsOnWithoutNotify");
        return t;
    }
    // A game button's click: pressed in the last frame, released now with the pointer still on it.
    bool GameClicked(int i) {
        if (!btnGame[i] || !unity::IsAlive(btnBg[i])) return false;
        bool ok = false, ok2 = false;
        const bool pressed = Unbox<bool>(Call(selIsPressed, btnBg[i], {}, "Selectable.IsPressed", &ok)) && ok;
        const bool inside = Unbox<bool>(Call(selPointerInside, btnBg[i], {}, "Selectable.get_isPointerInside", &ok2)) && ok2;
        const bool click = btnWasPressed[i] && !pressed && inside;
        btnWasPressed[i] = pressed;
        return click;
    }
    bool Clicked(Il2CppObject* toggle) {  // true once per click
        if (!unity::IsAlive(toggle)) return false;
        bool ok = false;
        const bool on = Unbox<bool>(Call(tglGetOn, toggle, {}, "Toggle.get_isOn", &ok));
        if (!ok || !on) return false;
        bool off = false;
        Call(tglSetOnQuiet, toggle, {&off}, "Toggle.SetIsOnWithoutNotify");
        return true;
    }
    Il2CppObject* Device(const MethodInfo* current) {
        if (!current) return nullptr;
        bool ok = false;
        Il2CppObject* dev = Call(current, nullptr, {}, "InputDevice.get_current", &ok);
        return ok ? dev : nullptr;
    }
    bool Pressed(Il2CppObject* dev, const MethodInfo* control, const MethodInfo* sub = nullptr) {
        if (!dev || !control || !btnPressed) return false;
        bool ok = false;
        Il2CppObject* ctl = Call(control, dev, {}, "input control getter", &ok);
        if (ok && ctl && sub) ctl = Call(sub, ctl, {}, "input sub-control getter", &ok);
        if (!ok || !ctl) return false;
        return Unbox<bool>(Call(btnPressed, ctl, {}, "ButtonControl.get_wasPressedThisFrame", &ok)) && ok;
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

    float CanvasHeight();
    Il2CppObject* GameButton(Il2CppObject* parentRt, bool positive, const char* label);
    void FitRings();
    void ScrollBy(int rows);
    bool BuildPanel(const Receipt& r, const std::string& note);
    bool Build(const Receipt& r, const std::string& note);
    void ClearPanel();
    std::string buildFail;  // the step BuildPanel stopped at (logged)
    bool BuildFail(const char* what) {
        if (buildFail.empty()) buildFail = what;
        return false;
    }
    void StyleButtons();
    void Teardown();
};

// ---------------------------------------------------------------- setup --

SiloReceipt::SiloReceipt() = default;
SiloReceipt::~SiloReceipt() = default;  // impl_ lives for the process (detour-free, but cheap to keep)

bool SiloReceipt::Setup(Resolver& R, const char* module, std::string& why) {
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
    auto byParams = [&](Il2CppClass* k, const char* name, std::initializer_list<std::string_view> params) {
        std::string err;
        const MethodInfo* m = k ? R.MethodByParams(k, name, params, &err) : nullptr;
        if (!m && why.empty()) why = std::format("{}: {}", name, err);
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
    auto fail = [&]() {
        delete p;
        return false;
    };

    HMODULE ga = api().module;
    p->objectNew = reinterpret_cast<Impl::ObjectNewFn>(ga ? GetProcAddress(ga, "il2cpp_object_new") : nullptr);
    if (!p->objectNew) why = "il2cpp_object_new export missing";

    p->goClass = cls(kCore, "UnityEngine", "GameObject");
    Il2CppClass* transform = cls(kCore, "UnityEngine", "Transform");
    Il2CppClass* uobject = cls(kCore, "UnityEngine", "Object");
    p->rtClass = cls(kCore, "UnityEngine", "RectTransform");
    p->canvasClass = cls("UnityEngine.UIModule", "UnityEngine", "Canvas");
    p->scalerClass = cls(kUi, "UnityEngine.UI", "CanvasScaler");
    p->raycasterClass = cls(kUi, "UnityEngine.UI", "GraphicRaycaster");
    p->imageClass = cls(kUi, "UnityEngine.UI", "Image");
    Il2CppClass* graphic = cls(kUi, "UnityEngine.UI", "Graphic");
    Il2CppClass* selectable = cls(kUi, "UnityEngine.UI", "Selectable");
    p->toggleClass = cls(kUi, "UnityEngine.UI", "Toggle");
    p->tmpTextClass = cls(kTmp, "TMPro", "TMP_Text");
    p->tmpUguiClass = cls(kTmp, "TMPro", "TextMeshProUGUI");
    p->lsClass = cls("Unity.Localization", "UnityEngine.Localization", "LocalizedString");
    p->lockerClass = cls(kImage, "MonomiPark.SlimeRancher.UI", "UIInputLocker");
    p->identClass = cls(kImage, "", "IdentifiableType");
    Il2CppClass* kb = cls(kInput, "UnityEngine.InputSystem", "Keyboard");
    Il2CppClass* pad = cls(kInput, "UnityEngine.InputSystem", "Gamepad");
    Il2CppClass* button = cls(kInput, "UnityEngine.InputSystem.Controls", "ButtonControl");
    Il2CppClass* dpad = cls(kInput, "UnityEngine.InputSystem.Controls", "DpadControl");
    Il2CppClass* stick = cls(kInput, "UnityEngine.InputSystem.Controls", "StickControl");
    if (!why.empty()) return fail();

    off(p->identClass, "icon", p->offIdentIcon);
    off(p->identClass, "localizedName", p->offIdentName);
    if (!why.empty()) return fail();

    p->goCtor = byParams(p->goClass, ".ctor", {"System.String"});
    p->goAddComponent = byParams(p->goClass, "AddComponent", {"System.Type"});
    p->trSetParent = byParams(transform, "SetParent", {"UnityEngine.Transform", "System.Boolean"});
    p->rtSetAnchorMin = method(p->rtClass, "set_anchorMin", 1);
    p->rtSetAnchorMax = method(p->rtClass, "set_anchorMax", 1);
    p->rtSetPivot = method(p->rtClass, "set_pivot", 1);
    p->rtSetSize = method(p->rtClass, "set_sizeDelta", 1);
    p->rtSetPos = method(p->rtClass, "set_anchoredPosition", 1);
    p->objDestroy = byParams(uobject, "Destroy", {"UnityEngine.Object"});
    p->gfxSetColor = method(graphic, "set_color", 1);
    p->gfxSetRaycast = method(graphic, "set_raycastTarget", 1);
    p->imgSetSprite = method(p->imageClass, "set_sprite", 1);
    p->tmpSetText = method(p->tmpTextClass, "set_text", 1);
    p->tmpGetFont = method(p->tmpTextClass, "get_font", 0);
    p->tmpSetFont = method(p->tmpTextClass, "set_font", 1);
    p->tmpSetSize = method(p->tmpTextClass, "set_fontSize", 1);
    p->tmpSetColor = method(p->tmpTextClass, "set_color", 1);
    p->tmpSetAlign = method(p->tmpTextClass, "set_alignment", 1);
    p->cvSetMode = method(p->canvasClass, "set_renderMode", 1);
    p->cvSetOrder = method(p->canvasClass, "set_sortingOrder", 1);
    p->scSetMode = method(p->scalerClass, "set_uiScaleMode", 1);
    p->scSetRef = method(p->scalerClass, "set_referenceResolution", 1);
    p->scSetMatch = method(p->scalerClass, "set_matchWidthOrHeight", 1);
    p->tglGetOn = method(p->toggleClass, "get_isOn", 0);
    p->tglSetOnQuiet = method(p->toggleClass, "SetIsOnWithoutNotify", 1);
    p->selSetTransition = method(selectable, "set_transition", 1);
    p->selSetNav = method(selectable, "set_navigation", 1);
    p->lsGet = method(p->lsClass, "GetLocalizedString", 0);
    p->kbCurrent = method(kb, "get_current", 0);
    p->padCurrent = method(pad, "get_current", 0);
    p->btnPressed = method(button, "get_wasPressedThisFrame", 0);
    p->kbEsc = method(kb, "get_escapeKey", 0);
    p->kbEnter = method(kb, "get_enterKey", 0);
    p->kbNumEnter = method(kb, "get_numpadEnterKey", 0);
    p->kbSpace = method(kb, "get_spaceKey", 0);
    p->kbLeft = method(kb, "get_leftArrowKey", 0);
    p->kbRight = method(kb, "get_rightArrowKey", 0);
    p->kbA = method(kb, "get_aKey", 0);
    p->kbD = method(kb, "get_dKey", 0);
    p->kbTab = method(kb, "get_tabKey", 0);
    p->padSouth = method(pad, "get_buttonSouth", 0);
    p->padEast = method(pad, "get_buttonEast", 0);
    p->padDpad = method(pad, "get_dpad", 0);
    p->padStick = method(pad, "get_leftStick", 0);
    p->dirLeft = method(dpad, "get_left", 0);
    p->dirRight = method(dpad, "get_right", 0);
    p->stickLeft = method(stick, "get_left", 0);
    p->stickRight = method(stick, "get_right", 0);
    if (!why.empty()) return fail();

    // ---- optional parts ----
    std::string report;
    {
        std::string err;
        p->tmpSetWrap = R.Method(p->tmpTextClass, "set_textWrappingMode", 1, &err);
        p->tmpSetOverflow = R.Method(p->tmpTextClass, "set_overflowMode", 1, &err);
        p->imgSetPreserve = R.Method(p->imageClass, "set_preserveAspect", 1, &err);
        p->sdfRectClass = R.Class(kImage, "MonomiPark.SlimeRancher.UI.SDFUI", "SDFRoundedRectangle", &err);
        if (p->sdfRectClass) {
            p->sdfSetRadius = R.MethodByParams(p->sdfRectClass, "SetCornerRadius", {"System.Single"}, &err);
            Il2CppClass* sdf = parentOf(p->sdfRectClass);  // SDFGraphic
            p->sdfSetBorder = sdf ? R.Method(sdf, "set_BorderSize", 1, &err) : nullptr;
            p->sdfSetBorderColor = sdf ? R.Method(sdf, "set_BorderColor", 1, &err) : nullptr;
            if (!p->sdfSetRadius) p->sdfRectClass = nullptr;
        }
        if (!p->sdfRectClass) report += " SDF shapes: NOT available (plain images);";
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

        // Optional lookups below never fail the setup; `opt` walks the parents like `method`.
        auto opt = [&](Il2CppClass* k, const char* name, int argc) -> const MethodInfo* {
            for (Il2CppClass* c = k; c; c = parentOf(c))
                if (const MethodInfo* m = R.Method(c, name, argc, &err)) return m;
            return nullptr;
        };
        auto optOff = [&](Il2CppClass* k, const char* field, size_t& out) {
            for (Il2CppClass* c = k; c; c = parentOf(c))
                if (auto o = R.FieldOffset(c, field, &err)) {
                    out = *o;
                    return true;
                }
            return false;
        };

        // Scrolling rows + the canvas height.
        p->kbUp = opt(kb, "get_upArrowKey", 0);
        p->kbDown = opt(kb, "get_downArrowKey", 0);
        p->kbW = opt(kb, "get_wKey", 0);
        p->kbS = opt(kb, "get_sKey", 0);
        p->dirUp = opt(dpad, "get_up", 0);
        p->dirDown = opt(dpad, "get_down", 0);
        p->stickUp = opt(stick, "get_up", 0);
        p->stickDown = opt(stick, "get_down", 0);
        Il2CppClass* mouse = R.Class(kInput, "UnityEngine.InputSystem", "Mouse", &err);
        Il2CppClass* vec2 = R.Class(kInput, "UnityEngine.InputSystem.Controls", "Vector2Control", &err);
        Il2CppClass* axis = R.Class(kInput, "UnityEngine.InputSystem.Controls", "AxisControl", &err);
        p->mouseCurrent = opt(mouse, "get_current", 0);
        p->mouseScroll = opt(mouse, "get_scroll", 0);
        p->vec2Y = opt(vec2, "get_y", 0);
        p->axisRead = opt(axis, "ReadValue", 0);  // InputControl<float>.ReadValue (inflated parent)
        Il2CppClass* screen = R.Class(kCore, "UnityEngine", "Screen", &err);
        p->screenW = opt(screen, "get_width", 0);
        p->screenH = opt(screen, "get_height", 0);
        p->scrollRectClass = R.Class(kUi, "UnityEngine.UI", "ScrollRect", &err);
        p->scrollbarClass = R.Class(kUi, "UnityEngine.UI", "Scrollbar", &err);
        p->rectMaskClass = R.Class(kUi, "UnityEngine.UI", "RectMask2D", &err);
        p->srSetContent = opt(p->scrollRectClass, "set_content", 1);
        p->srSetViewport = opt(p->scrollRectClass, "set_viewport", 1);
        p->srSetHorizontal = opt(p->scrollRectClass, "set_horizontal", 1);
        p->srSetVertical = opt(p->scrollRectClass, "set_vertical", 1);
        p->srSetMovement = opt(p->scrollRectClass, "set_movementType", 1);
        p->srSetSensitivity = opt(p->scrollRectClass, "set_scrollSensitivity", 1);
        p->srGetNormY = opt(p->scrollRectClass, "get_verticalNormalizedPosition", 0);
        p->srSetNormY = opt(p->scrollRectClass, "set_verticalNormalizedPosition", 1);
        p->srSetScrollbar = opt(p->scrollRectClass, "set_verticalScrollbar", 1);
        p->srSetScrollbarVis = opt(p->scrollRectClass, "set_verticalScrollbarVisibility", 1);
        p->sbSetHandle = opt(p->scrollbarClass, "set_handleRect", 1);
        p->sbSetDirection = opt(p->scrollbarClass, "set_direction", 1);
        p->selSetTarget = opt(selectable, "set_targetGraphic", 1);
        p->scrollOk = p->scrollRectClass && p->rectMaskClass && p->srSetContent && p->srSetViewport &&
                      p->srSetHorizontal && p->srSetVertical && p->srSetMovement && p->srSetSensitivity &&
                      p->srGetNormY && p->srSetNormY;
        if (!p->scrollOk) report += " scrolling rows: NOT available (rows only shrink);";
        if (!p->screenW || !p->screenH) report += " screen size: NOT available (1080 assumed);";

        // The game's popup buttons.
        Il2CppClass* templates = R.Class(kImage, "MonomiPark.SlimeRancher.UI", "UITemplates", &err);
        p->popupClass = R.Class(kImage, "MonomiPark.SlimeRancher.UI.Popup", "PopupPrompt", &err);
        p->ieButtonClass = R.Class(kImage, "MonomiPark.SlimeRancher.Input", "InputEventButton", &err);
        p->moreViewClass = R.Class(kImage, "MonomiPark.SlimeRancher.UI.Adapter", "DetailsMoreButtonView", &err);
        p->uiButtonClass = R.Class(kUi, "UnityEngine.UI", "Button", &err);
        Il2CppClass* behaviour = R.Class(kCore, "UnityEngine", "Behaviour", &err);
        p->gcGetTemplates = opt(gameCtx, "get_UITemplates", 0);
        p->rtGetRect = opt(p->rtClass, "get_rect", 0);
        p->behSetEnabled = opt(behaviour, "set_enabled", 1);
        p->selSetInteractable = opt(selectable, "set_interactable", 1);
        p->selIsPressed = opt(selectable, "IsPressed", 0);
        p->selPointerInside = opt(selectable, "get_isPointerInside", 0);
        p->gameButtonsOk = p->gcInstance && templates && p->popupClass && p->ieButtonClass && p->moreViewClass &&
                           p->uiButtonClass && p->gcGetTemplates && p->rtGetRect && p->behSetEnabled &&
                           p->selIsPressed && p->selPointerInside &&
                           optOff(templates, "popupPrompt", p->offTplPopup) &&
                           optOff(p->popupClass, "_positiveActionButton", p->offPopupPositive) &&
                           optOff(p->popupClass, "_negativeActionButton", p->offPopupNegative) &&
                           optOff(p->moreViewClass, "label", p->offMoreLabel);
        if (!p->gameButtonsOk) report += " game buttons: NOT available (own buttons);";
    }
    impl_ = p;
    ready_ = true;
    SR2_INFO("[{}] silo receipt ready{}", module, report.empty() ? "" : ":" + report);
    return true;
}

// ---------------------------------------------------------------- build --

// Canvas height in reference units: the CanvasScaler (ScaleWithScreenSize, 1920 x 1080, match 0.5)
// scales by sqrt(w / 1920 * h / 1080), so 16:9 is always 1080 high, ultrawide less, 4:3 more.
float SiloReceipt::Impl::CanvasHeight() {
    if (!screenW || !screenH) return 1080.0f;
    bool ok = false, ok2 = false;
    const int w = Unbox<int32_t>(Call(screenW, nullptr, {}, "Screen.get_width", &ok));
    const int h = Unbox<int32_t>(Call(screenH, nullptr, {}, "Screen.get_height", &ok2));
    if (!ok || !ok2 || w <= 0 || h <= 0) return 1080.0f;
    const float scale = std::sqrt(static_cast<float>(w) / 1920.0f * static_cast<float>(h) / 1080.0f);
    if (!std::isfinite(scale) || scale <= 0.0f) return 1080.0f;
    return std::clamp(static_cast<float>(h) / scale, 400.0f, 4000.0f);
}

// A clone of the game's popup button (positive = Accept, else Cancel) under `parentRt`, labelled
// `label`; its centre is set by the caller. nullptr: not available (the caller builds its own).
Il2CppObject* SiloReceipt::Impl::GameButton(Il2CppObject* parentRt, bool positive, const char* label) {
    if (!gameButtonsOk) return nullptr;
    auto miss = [&](const char* what) -> Il2CppObject* {
        if (!loggedButtonMiss) SR2_WARN("[{}] silo receipt: game button not cloned ({}) - own buttons", module, what);
        loggedButtonMiss = true;
        return nullptr;
    };
    bool ok = false;
    Il2CppObject* gc = Call(gcInstance, nullptr, {}, "GameContext.Instance", &ok);
    if (!ok || !unity::IsAlive(gc)) return miss("no GameContext");
    Il2CppObject* tpl = Call(gcGetTemplates, gc, {}, "GameContext.get_UITemplates", &ok);
    if (!ok || !unity::IsAlive(tpl)) return miss("no UITemplates");
    Il2CppObject* popupGo = unity::Field<Il2CppObject*>(tpl, offTplPopup);
    Il2CppObject* popup = unity::IsAlive(popupGo) ? unity::GetComponent(popupGo, popupClass) : nullptr;
    if (!unity::IsAlive(popup)) return miss("no PopupPrompt template");
    Il2CppObject* src = unity::Field<Il2CppObject*>(popup, positive ? offPopupPositive : offPopupNegative);
    if (!unity::IsAlive(src) || !IsA(src, goClass)) return miss("popup button is not a GameObject");
    Il2CppObject* go = unity::Instantiate(src, parentRt);
    if (!unity::IsAlive(go)) return miss("Instantiate failed");
    // The click is polled from its Button (GameClicked): without one there is nothing to poll.
    if (!unity::IsAlive(unity::GetComponent(go, uiButtonClass))) {
        Destroy(go);
        return miss("no Button on the clone");
    }
    unity::SetName(go, positive ? "Sell" : "Cancel");
    // Its pointer down / up would perform the popup's InputEvent (a menu submit / cancel elsewhere).
    if (Il2CppObject* ieb = unity::GetComponent(go, ieButtonClass); unity::IsAlive(ieb)) {
        bool off = false;
        Call(behSetEnabled, ieb, {&off}, "Behaviour.set_enabled(InputEventButton)");
    }
    // Label: the LocalizeStringEvent off (it would write "Confirm" / "Cancel" back), our text in.
    Il2CppObject* view = unity::GetComponent(go, moreViewClass);
    Il2CppObject* lse = unity::IsAlive(view) ? unity::Field<Il2CppObject*>(view, offMoreLabel) : nullptr;
    Il2CppObject* tmp = nullptr;
    if (unity::IsAlive(lse)) {
        bool off = false;
        Call(behSetEnabled, lse, {&off}, "Behaviour.set_enabled(LocalizeStringEvent)");
        tmp = unity::GetComponent(lse, tmpTextClass);
    }
    if (!unity::IsAlive(tmp)) {
        Destroy(go);
        return miss("no label text on the clone");
    }
    SetText(tmp, label);
    Keep(panelHandles, go);
    Keep(panelHandles, tmp);
    return go;
}

// Rings behind the game buttons, fitted to the size the buttons give themselves (EncapsulateChildren
// runs in LateUpdate, so for a few frames after a build).
void SiloReceipt::Impl::FitRings() {
    for (int i = 0; i < 2; ++i) {
        if (!unity::IsAlive(btnGame[i]) || !unity::IsAlive(btnRing[i])) continue;
        bool ok = false;
        Il2CppObject* boxed = Call(rtGetRect, unity::TransformOf(btnGame[i]), {}, "RectTransform.get_rect", &ok);
        if (!ok || !boxed) continue;
        const float* r = reinterpret_cast<const float*>(reinterpret_cast<char*>(boxed) + 2 * sizeof(void*));  // x y w h
        const float w = r[2], h = r[3];
        if (!std::isfinite(w) || !std::isfinite(h) || w <= 1.0f || h <= 1.0f || w > 2000.0f || h > 500.0f) continue;
        constexpr float kRing = 6.0f;
        Rect(unity::TransformOf(unity::GameObjectOf(btnRing[i])), {0, 1}, {0, 1}, {0.5f, 0.5f},
             {btnCentre[i].x, -btnCentre[i].y}, {w + 2 * kRing, h + 2 * kRing});
        if (sdfRectClass && IsA(btnRing[i], sdfRectClass)) {
            float radius = h * 0.5f + kRing;
            Call(sdfSetRadius, btnRing[i], {&radius}, "SDFRoundedRectangle.SetCornerRadius");
        }
    }
}

void SiloReceipt::Impl::ScrollBy(int rows) {
    if (!unity::IsAlive(scrollRect) || scrollStep <= 0.0f || rows == 0) return;
    bool ok = false;
    const float y = Unbox<float>(Call(srGetNormY, scrollRect, {}, "ScrollRect.get_verticalNormalizedPosition", &ok));
    if (!ok || !std::isfinite(y)) return;
    // Normalized 1 = top: a row down lowers it.
    float v = std::clamp(y - static_cast<float>(rows) * scrollStep, 0.0f, 1.0f);
    Call(srSetNormY, scrollRect, {&v}, "ScrollRect.set_verticalNormalizedPosition");
}

bool SiloReceipt::Impl::BuildPanel(const Receipt& r, const std::string& note) {
    // Rows sorted by the caller. The panel fits the canvas: rows shrink to kMinRow, then scroll.
    const int n = static_cast<int>(r.lines.size());
    const float titleY = 28.0f, subY = 84.0f, headY = 136.0f, rowsY = 172.0f, gap = 6.0f;
    const float bottomH = 8.0f + 16.0f + 58.0f + 44.0f + kButtonH + 30.0f;  // separator .. below the buttons
    const float kMinRow = scrollOk ? 40.0f : 34.0f;
    const float maxPanel = std::max(rowsY + 56.0f + bottomH, CanvasHeight() - 2 * 36.0f);  // 36: border + margin
    const float availRows = maxPanel - rowsY - bottomH;
    const float rowH = std::clamp(n > 0 ? availRows / n - gap : 56.0f, kMinRow, 56.0f);
    const float contentH = n > 0 ? n * (rowH + gap) - gap : 56.0f;
    const bool scrolls = scrollOk && contentH > availRows + 0.5f;
    const float viewH = scrolls ? std::max(rowH, std::floor((availRows + gap) / (rowH + gap)) * (rowH + gap) - gap)
                                : contentH;
    const float rowsEnd = rowsY + viewH + gap;
    const float sepY = rowsEnd + 8.0f, totalY = sepY + 16.0f, noteY = totalY + 58.0f, btnY = noteY + 44.0f;
    const float panelH = btnY + kButtonH + 30.0f;
    // Columns (x inside the content, left edge = kPad).
    const float cIcon = kPad + 12.0f, cName = kPad + 70.0f, cCount = kPad + 460.0f, cPrice = kPad + 590.0f,
                cTotal = kPad + 720.0f, cCoin = kPad + 866.0f;
    const float nameW = 380.0f, colW = 120.0f, totalW = 140.0f, coin = 28.0f;

    Il2CppObject* p = NewUi("Panel", rootRt);
    if (!p) return BuildFail("panel object");
    panel = p;
    Keep(panelHandles, p);
    Il2CppObject* prt = unity::TransformOf(p);
    Rect(prt, {0.5f, 0.5f}, {0.5f, 0.5f}, {0.5f, 0.5f}, {0, 0}, {kPanelW, panelH});
    Box(prt, "Border", -8, -8, kPanelW + 16, panelH + 16, kLavender, 34.0f);
    if (Il2CppObject* content = Box(prt, "Content", 0, 0, kPanelW, panelH, kWhite, 30.0f)) {
        SetRaycast(sdfRectClass ? unity::GetComponent(content, sdfRectClass) : unity::GetComponent(content, imageClass), true);
        Clickable(content);  // never polled: a click on the panel must not reach the backdrop (= Cancel)
    }
    Text(prt, "Title", kPad, titleY, kInnerW, 50, "Sell-All receipt", 40.0f, kNavy, kAlignLeft);
    Text(prt, "Subtitle", kPad, subY, kInnerW, 34, "Every plort in this silo at today's Plort Market prices.", 22.0f,
         kNavyDim, kAlignLeft);
    Text(prt, "HeadPlort", cName, headY, nameW, 30, "Plort", 20.0f, kNavyDim, kAlignLeft);
    Text(prt, "HeadCount", cCount, headY, colW, 30, "Count", 20.0f, kNavyDim, kAlignRight);
    Text(prt, "HeadPrice", cPrice, headY, colW, 30, "Price", 20.0f, kNavyDim, kAlignRight);
    Text(prt, "HeadTotal", cTotal, headY, totalW, 30, "Total", 20.0f, kNavyDim, kAlignRight);

    // Rows: straight into the panel, or (too many for the screen) into a scrolling list.
    Il2CppObject* rowsRt = prt;
    float ox = 0.0f, oy = 0.0f;  // panel position of rowsRt's top-left
    scrollRect = nullptr;
    scrollStep = 0.0f;
    if (scrolls) {
        Il2CppObject* vp = NewUi("RowsViewport", prt);
        Il2CppObject* vpRt = vp ? unity::TransformOf(vp) : nullptr;
        Il2CppObject* content = vpRt ? NewUi("Rows", vpRt) : nullptr;
        Il2CppObject* contentRt = content ? unity::TransformOf(content) : nullptr;
        Il2CppObject* img = contentRt ? AddComponent(vp, imageClass, "AddComponent(Image)") : nullptr;
        Il2CppObject* mask = img ? AddComponent(vp, rectMaskClass, "AddComponent(RectMask2D)") : nullptr;
        Il2CppObject* sr = mask ? AddComponent(vp, scrollRectClass, "AddComponent(ScrollRect)") : nullptr;
        if (sr) {
            At(vpRt, kPad, rowsY, kInnerW, viewH);
            SetColor(img, Color{0, 0, 0, 0});  // invisible, but a raycast target: drag / wheel land here
            At(contentRt, 0, 0, kInnerW, contentH);
            bool no = false, yes = true;
            int32_t clamped = 2;   // MovementType.Clamped
            float sensitivity = 0;  // the wheel is ours (one row per notch, Tick), not ScrollRect's
            Call(srSetContent, sr, {contentRt}, "ScrollRect.set_content");
            Call(srSetViewport, sr, {vpRt}, "ScrollRect.set_viewport");
            Call(srSetHorizontal, sr, {&no}, "ScrollRect.set_horizontal");
            Call(srSetVertical, sr, {&yes}, "ScrollRect.set_vertical");
            Call(srSetMovement, sr, {&clamped}, "ScrollRect.set_movementType");
            Call(srSetSensitivity, sr, {&sensitivity}, "ScrollRect.set_scrollSensitivity");
            // A thin scrollbar in the right margin (draggable; ScrollRect keeps it in sync).
            if (scrollbarClass && sbSetHandle && sbSetDirection && srSetScrollbar) {
                Il2CppObject* track = Box(prt, "Scrollbar", kPanelW - kPad + 12.0f, rowsY, 10.0f, viewH, kBacker, 5.0f);
                Il2CppObject* trackRt = track ? unity::TransformOf(track) : nullptr;
                Il2CppObject* area = trackRt ? NewUi("Sliding Area", trackRt) : nullptr;
                Il2CppObject* areaRt = area ? unity::TransformOf(area) : nullptr;
                Il2CppObject* handle = areaRt ? NewUi("Handle", areaRt) : nullptr;
                Il2CppObject* handleRt = handle ? unity::TransformOf(handle) : nullptr;
                if (handleRt) {
                    Rect(areaRt, {0, 0}, {1, 1}, {0.5f, 0.5f}, {0, 0}, {0, 0});
                    Rect(handleRt, {0, 0}, {1, 1}, {0.5f, 0.5f}, {0, 0}, {0, 0});
                    Il2CppObject* hg = Shape(handle, kLavender, 5.0f);
                    SetRaycast(hg, true);
                    SetRaycast(sdfRectClass ? unity::GetComponent(track, sdfRectClass) : unity::GetComponent(track, imageClass), true);
                    if (Il2CppObject* sb = AddComponent(track, scrollbarClass, "AddComponent(Scrollbar)")) {
                        int32_t none = 0, bottomToTop = 2;  // Transition.None, Direction.BottomToTop
                        alignas(8) unsigned char nav[64] = {};  // Navigation.Mode.None
                        Call(selSetTransition, sb, {&none}, "Selectable.set_transition");
                        Call(selSetNav, sb, {nav}, "Selectable.set_navigation");
                        if (selSetTarget && hg) Call(selSetTarget, sb, {hg}, "Selectable.set_targetGraphic");
                        Call(sbSetHandle, sb, {handleRt}, "Scrollbar.set_handleRect");
                        Call(sbSetDirection, sb, {&bottomToTop}, "Scrollbar.set_direction");
                        Call(srSetScrollbar, sr, {sb}, "ScrollRect.set_verticalScrollbar");
                    }
                }
            }
            float top = 1.0f;
            Call(srSetNormY, sr, {&top}, "ScrollRect.set_verticalNormalizedPosition");
            scrollRect = sr;
            Keep(panelHandles, sr);
            scrollStep = (rowH + gap) / std::max(1.0f, contentH - viewH);
            rowsRt = contentRt;
            ox = kPad;
            oy = rowsY;
        } else if (vp) {
            Destroy(vp);  // half built: rows go into the panel (they shrink as far as they can)
        }
    }

    Il2CppObject* coinSprite = CoinSprite();
    const float textSize = rowH >= 48.0f ? 26.0f : 22.0f;
    const float iconSize = std::min(44.0f, rowH - 8.0f);
    for (int i = 0; i < n; ++i) {
        const ReceiptLine& l = r.lines[static_cast<size_t>(i)];
        const float y = rowsY + i * (rowH + gap) - oy;
        Box(rowsRt, "Row", kPad - ox, y, kInnerW, rowH, kBacker, 18.0f);
        std::string name;
        Il2CppObject* icon = nullptr;
        if (unity::IsAlive(l.type) && IsA(l.type, identClass)) {
            name = Localized(unity::Field<Il2CppObject*>(l.type, offIdentName));
            if (name.empty()) name = unity::GetName(l.type);
            icon = unity::Field<Il2CppObject*>(l.type, offIdentIcon);
        }
        Sprite(rowsRt, "Icon", cIcon - ox, y + (rowH - iconSize) * 0.5f, iconSize, iconSize, icon);
        Text(rowsRt, "Name", cName - ox, y, nameW, rowH, name, textSize, kNavy, kAlignLeft);
        Text(rowsRt, "Count", cCount - ox, y, colW, rowH, std::format("x {}", Group(l.count)), textSize, kNavy, kAlignRight);
        Text(rowsRt, "Price", cPrice - ox, y, colW, rowH, Group(l.price), textSize, kNavyDim, kAlignRight);
        Text(rowsRt, "Total", cTotal - ox, y, totalW, rowH, Group(l.Total()), textSize, kNavy, kAlignRight);
        Sprite(rowsRt, "Coin", cCoin - ox, y + (rowH - coin) * 0.5f, coin, coin, coinSprite);
    }
    if (n == 0) Text(prt, "Empty", cName, rowsY, kInnerW - 70, 56, "No plorts to sell.", 26.0f, kNavyDim, kAlignLeft);

    Box(prt, "Separator", kPad, sepY, kInnerW, 3, kLavender, 1.5f);
    Text(prt, "TotalLabel", kPad + 12, totalY, 300, 50, "Total", 32.0f, kNavy, kAlignLeft);
    const int count = r.Count();
    Text(prt, "TotalCount", cCount - 140, totalY, colW + 140, 50, std::format("{} plort{}", Group(count), count == 1 ? "" : "s"),
         26.0f, kNavyDim, kAlignRight);
    Text(prt, "TotalValue", cTotal - 60, totalY, totalW + 60, 50, Group(r.Total()), 32.0f, kNavy, kAlignRight);
    Sprite(prt, "TotalCoin", cCoin - 2, totalY + 9, 32, 32, coinSprite);
    if (!note.empty()) Text(prt, "Note", kPad, noteY, kInnerW, 40, note, 22.0f, kOrange, kAlignCenter, true);

    // Buttons: the game's own (popup Accept / Cancel), else ours with the keys in the label.
    const float bx[2] = {kPad, kPanelW - kPad - kButtonW};
    for (int i = 0; i < 2; ++i) {
        btnCentre[i] = Vec2{bx[i] + kButtonW * 0.5f, btnY + kButtonH * 0.5f};
        // The ring first (drawn behind the button); transparent until the button has the focus.
        Il2CppObject* ringGo = gameButtonsOk ? Box(prt, "FocusRing", bx[i], btnY, kButtonW, kButtonH, Color{0, 0, 0, 0}, 36.0f)
                                             : nullptr;
        Il2CppObject* game = GameButton(prt, i == 1, i == 0 ? "Cancel" : "Sell");
        if (game) {
            Rect(unity::TransformOf(game), {0, 1}, {0, 1}, {0.5f, 0.5f}, {btnCentre[i].x, -btnCentre[i].y},
                 {kButtonW, 59.0f});
            btnGame[i] = game;
            btnRing[i] = ringGo ? (sdfRectClass ? unity::GetComponent(ringGo, sdfRectClass) : unity::GetComponent(ringGo, imageClass))
                                : nullptr;
            btnBg[i] = unity::GetComponent(game, uiButtonClass);  // its Selectable: the click, interactable
            btnToggle[i] = nullptr;
            btnWasPressed[i] = false;
            Keep(panelHandles, btnRing[i]);
            Keep(panelHandles, btnBg[i]);
            continue;
        }
        if (ringGo) Destroy(ringGo);
        const char* labels[2] = {"Cancel  (Esc / B)", "Sell  (Enter / A)"};
        Il2CppObject* b = NewUi(i == 0 ? "Cancel" : "Sell", prt);
        if (!b) return BuildFail("own button object");
        At(unity::TransformOf(b), bx[i], btnY, kButtonW, kButtonH);
        btnBg[i] = Shape(b, kBacker, 32.0f, 4.0f, kLavender);
        SetRaycast(btnBg[i], true);
        btnText[i] = Text(unity::TransformOf(b), "Label", 0, 0, kButtonW, kButtonH, labels[i], 26.0f, kNavy, kAlignCenter);
        btnToggle[i] = Clickable(b);
        Keep(panelHandles, btnBg[i]);
        Keep(panelHandles, btnText[i]);
        Keep(panelHandles, btnToggle[i]);
        if (!btnBg[i] || !btnToggle[i]) return BuildFail(!btnBg[i] ? "own button shape" : "own button Toggle");
    }
    ringFrames = 30;
    if (!loggedButtons) {
        loggedButtons = true;
        SR2_INFO("[{}] silo receipt: buttons {}, rows {} ({} px, canvas {:.0f} high)", module,
                 btnGame[0] && btnGame[1] ? "cloned from the game's popup" : btnGame[0] || btnGame[1] ? "mixed" : "own",
                 scrolls ? "scroll" : "fit", static_cast<int>(rowH), CanvasHeight());
    }
    StyleButtons();
    return Faulted() ? BuildFail("module faulted") : true;
}

// BuildPanel; when it fails with the game's buttons, once more with our own (and own buttons from then on).
bool SiloReceipt::Impl::Build(const Receipt& r, const std::string& note) {
    buildFail.clear();
    if (BuildPanel(r, note)) return true;
    SR2_WARN("[{}] silo receipt: panel build failed at: {}", module, buildFail);
    if (Faulted() || !gameButtonsOk) return false;
    ClearPanel();
    gameButtonsOk = false;
    loggedButtons = false;
    SR2_WARN("[{}] silo receipt: retrying with our own buttons (game buttons off for this session)", module);
    buildFail.clear();
    if (BuildPanel(r, note)) return true;
    SR2_WARN("[{}] silo receipt: panel build failed again at: {}", module, buildFail);
    return false;
}

void SiloReceipt::Impl::ClearPanel() {
    if (unity::IsAlive(panel)) Destroy(panel);
    panel = nullptr;
    for (int i = 0; i < 2; ++i) {
        btnBg[i] = btnText[i] = btnToggle[i] = btnGame[i] = btnRing[i] = nullptr;
        btnWasPressed[i] = false;
    }
    scrollRect = nullptr;
    scrollStep = 0.0f;
    FreeLater(panelHandles);
}

void SiloReceipt::Impl::StyleButtons() {
    if (!canSell && focus == 1) focus = 0;
    for (int i = 0; i < 2; ++i) {
        const bool focused = focus == i;
        const bool disabled = i == 1 && !canSell;
        if (btnGame[i]) {
            // The game's button keeps its own look (hover / disabled colours); the focus is the ring.
            if (selSetInteractable && unity::IsAlive(btnBg[i])) {
                bool on = !disabled;
                Call(selSetInteractable, btnBg[i], {&on}, "Selectable.set_interactable");
            }
            SetColor(btnRing[i], focused && !disabled ? (i == 1 ? kPink : kTeal) : Color{0, 0, 0, 0});
            continue;
        }
        const Color fill = disabled ? kGrey : focused ? (i == 1 ? kPink : kTeal) : kBacker;
        SetColor(btnBg[i], fill);
        if (sdfSetBorderColor && sdfRectClass && IsA(btnBg[i], sdfRectClass)) {
            Color border = focused && !disabled ? kWhite : kLavender;
            Call(sdfSetBorderColor, btnBg[i], {&border}, "SDFGraphic.set_BorderColor");
        }
        SetColor(btnText[i], disabled ? kWhite : focused ? kWhite : kNavy);
    }
}

void SiloReceipt::Impl::Teardown() {
    // Deactivate first: UIInputLocker.OnDisable unpauses (input back) right now, Destroy is deferred.
    // Done even after a fault - the game must never stay paused because of us.
    if (unity::IsAlive(root)) {
        unity::SetActive(root, false);
        Destroy(root);
    }
    open = false;
    root = rootRt = backdropToggle = panel = scrollRect = nullptr;
    for (int i = 0; i < 2; ++i) btnBg[i] = btnText[i] = btnToggle[i] = btnGame[i] = btnRing[i] = nullptr;
    scrollStep = 0.0f;
    ringFrames = 0;
    font = nullptr;
    FreeLater(panelHandles);
    FreeLater(handles);
}

// ---------------------------------------------------------------- api --

bool SiloReceipt::Open(const Receipt& r) {
    if (!ready_ || !impl_ || impl_->open || !main_thread::IsMainThread()) return false;
    Impl& p = *impl_;
    auto failOpen = [&](const char* what) {
        if (!p.loggedFail) SR2_WARN("[{}] silo receipt: not opened: {}", p.module, what);
        p.loggedFail = true;
        p.Teardown();
        return false;
    };
    // Font: the HUD's (any active TextMeshProUGUI in the scene; one lookup per open).
    if (Il2CppObject* hudText = unity::FindObjectOfType(p.tmpUguiClass); unity::IsAlive(hudText)) {
        Il2CppObject* f = p.Call(p.tmpGetFont, hudText, {}, "TMP_Text.get_font");
        if (unity::IsAlive(f)) {
            p.font = f;
            Impl::Keep(p.handles, f);
        }
    }
    Il2CppObject* go = p.NewGo("sr2mods_SiloSellReceipt");
    if (!go) return failOpen("GameObject creation failed");
    p.root = go;
    Impl::Keep(p.handles, go);
    unity::SetActive(go, false);  // the locker pauses only once everything is built
    Il2CppObject* canvas = p.AddComponent(go, p.canvasClass, "AddComponent(Canvas)");
    if (!canvas) return failOpen("Canvas failed");
    int32_t overlay = 0, order = kSortingOrder;  // RenderMode.ScreenSpaceOverlay
    p.Call(p.cvSetMode, canvas, {&overlay}, "Canvas.set_renderMode");
    p.Call(p.cvSetOrder, canvas, {&order}, "Canvas.set_sortingOrder");
    if (Il2CppObject* scaler = p.AddComponent(go, p.scalerClass, "AddComponent(CanvasScaler)")) {
        int32_t scaleWithScreen = 1;
        Vec2 ref{1920.0f, 1080.0f};
        float match = 0.5f;
        p.Call(p.scSetMode, scaler, {&scaleWithScreen}, "CanvasScaler.set_uiScaleMode");
        p.Call(p.scSetRef, scaler, {&ref}, "CanvasScaler.set_referenceResolution");
        p.Call(p.scSetMatch, scaler, {&match}, "CanvasScaler.set_matchWidthOrHeight");
    }
    if (!p.AddComponent(go, p.raycasterClass, "AddComponent(GraphicRaycaster)")) return failOpen("GraphicRaycaster failed");
    p.rootRt = unity::TransformOf(go);
    if (!unity::IsAlive(p.rootRt) || !p.IsA(p.rootRt, p.rtClass)) return failOpen("no RectTransform on the canvas");
    // Dimmed backdrop over the whole screen; a click on it cancels.
    if (Il2CppObject* bd = p.NewUi("Backdrop", p.rootRt)) {
        p.Rect(unity::TransformOf(bd), {0, 0}, {1, 1}, {0.5f, 0.5f}, {0, 0}, {0, 0});
        Il2CppObject* img = p.AddComponent(bd, p.imageClass, "AddComponent(Image)");
        p.SetColor(img, Color{0.05f, 0.07f, 0.12f, 0.55f});
        p.backdropToggle = p.Clickable(bd);
        Impl::Keep(p.handles, p.backdropToggle);
    }
    p.canSell = r.marketAvailable && !r.marketClosed && !r.lines.empty();
    p.focus = p.canSell ? 1 : 0;
    if (!p.Build(r, "")) return failOpen("panel failed");
    // The game's own lock: pauses time, swaps to the UI action map, frees the cursor.
    if (!p.AddComponent(go, p.lockerClass, "AddComponent(UIInputLocker)")) return failOpen("UIInputLocker failed");
    if (p.Faulted()) return failOpen("module faulted");
    unity::SetActive(go, true);
    p.open = true;
    p.grace = kGraceSeconds;
    if (!p.loggedOpen) {
        p.loggedOpen = true;
        SR2_INFO("[{}] silo receipt opened ({} row(s), font {}, input locked by UIInputLocker)", p.module, r.lines.size(),
                 p.font ? "from the HUD" : "TMP default");
    }
    return true;
}

void SiloReceipt::Refresh(const Receipt& r, const std::string& note, bool canSell) {
    if (!ready_ || !impl_ || !impl_->open) return;
    Impl& p = *impl_;
    p.ClearPanel();
    p.canSell = canSell;
    if (!p.Build(r, note)) {
        SR2_WARN("[{}] silo receipt: refresh failed - closed", p.module);
        p.Teardown();
    }
    p.grace = kGraceSeconds;  // the same press must not confirm the new receipt
}

bool SiloReceipt::IsOpen() const { return ready_ && impl_ && impl_->open; }

SiloReceipt::Result SiloReceipt::Tick(float unscaledDt) {
    if (!ready_ || !impl_ || !impl_->open) return Result::kNone;
    Impl& p = *impl_;
    if (!unity::IsAlive(p.root)) {  // scene change destroyed it (its locker unpaused on the way)
        p.Teardown();
        return Result::kCancel;
    }
    // Clicks first (reset every frame, also during the grace time).
    const bool clickBackdrop = p.Clicked(p.backdropToggle);
    const bool clickCancel = p.btnGame[0] ? p.GameClicked(0) : p.Clicked(p.btnToggle[0]);
    const bool clickSell = p.btnGame[1] ? p.GameClicked(1) : p.Clicked(p.btnToggle[1]);
    if (p.ringFrames > 0) {
        --p.ringFrames;
        p.FitRings();
    }
    if (std::isfinite(unscaledDt) && unscaledDt > 0.0f) p.grace -= std::min(unscaledDt, 0.25f);
    if (p.grace > 0.0f) return Result::kNone;
    if (clickCancel || clickBackdrop) return Result::kCancel;
    if (clickSell) {
        if (p.canSell) return Result::kConfirm;
        p.focus = 0;
        p.StyleButtons();
        return Result::kNone;
    }
    Il2CppObject* kb = p.Device(p.kbCurrent);
    Il2CppObject* pad = p.Device(p.padCurrent);
    if (p.Pressed(kb, p.kbEsc) || p.Pressed(pad, p.padEast)) return Result::kCancel;
    const bool left = p.Pressed(kb, p.kbLeft) || p.Pressed(kb, p.kbA) || p.Pressed(pad, p.padDpad, p.dirLeft) ||
                      p.Pressed(pad, p.padStick, p.stickLeft);
    const bool right = p.Pressed(kb, p.kbRight) || p.Pressed(kb, p.kbD) || p.Pressed(pad, p.padDpad, p.dirRight) ||
                       p.Pressed(pad, p.padStick, p.stickRight);
    const bool tab = p.Pressed(kb, p.kbTab);
    if (p.scrollStep > 0.0f) {  // the row list scrolls: one row per press / wheel notch
        int rows = 0;
        if (p.Pressed(kb, p.kbUp) || p.Pressed(kb, p.kbW) || p.Pressed(pad, p.padDpad, p.dirUp) ||
            p.Pressed(pad, p.padStick, p.stickUp))
            --rows;
        if (p.Pressed(kb, p.kbDown) || p.Pressed(kb, p.kbS) || p.Pressed(pad, p.padDpad, p.dirDown) ||
            p.Pressed(pad, p.padStick, p.stickDown))
            ++rows;
        if (Il2CppObject* mouse = p.Device(p.mouseCurrent); mouse && p.mouseScroll && p.vec2Y && p.axisRead) {
            bool ok = false;
            Il2CppObject* scroll = p.Call(p.mouseScroll, mouse, {}, "Mouse.get_scroll", &ok);
            Il2CppObject* y = ok && scroll ? p.Call(p.vec2Y, scroll, {}, "Vector2Control.get_y", &ok) : nullptr;
            const float dy = ok && y ? Impl::Unbox<float>(p.Call(p.axisRead, y, {}, "AxisControl.ReadValue", &ok)) : 0.0f;
            if (ok && std::isfinite(dy) && dy != 0.0f) rows += dy > 0.0f ? -1 : 1;  // wheel up = list up
        }
        p.ScrollBy(rows);
    }
    if (left || right || tab) {
        const int want = tab ? 1 - p.focus : left ? 0 : 1;
        if (want != p.focus && (want == 0 || p.canSell)) {
            p.focus = want;
            p.StyleButtons();
        }
    }
    const bool submit = p.Pressed(kb, p.kbEnter) || p.Pressed(kb, p.kbNumEnter) || p.Pressed(kb, p.kbSpace) ||
                        p.Pressed(pad, p.padSouth);
    if (submit) {
        if (p.focus == 0) return Result::kCancel;
        if (p.canSell) return Result::kConfirm;
    }
    return Result::kNone;
}

void SiloReceipt::Close(const char* why) {
    if (!ready_ || !impl_ || !impl_->open) return;
    SR2_DEBUG("[{}] silo receipt closed ({})", impl_->module, why ? why : "");
    impl_->Teardown();
}

}  // namespace sr2::modules::custom_upgrades::ui
