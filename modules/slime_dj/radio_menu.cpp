// slime_dj: the Corral Radio playlist menu (see radio_menu.h). SR2 1.3.0, all lookups by name.
//
// Layout (CanvasScaler reference 1920 x 1080, the panel centred, 1400 wide), round 12 ("the playlist could
// use more slime ranchification, we dont need keybinds when we put the list items as slime rancher's
// buttons ... remove the bottom hint, DONE PLAY UP DOWN REMOVE buttons"):
//   title "Corral Radio"                            [R Radio: on] [F Shuffle: off] [C Clear]
//   the corral's name                 [ warning pill, only when the radio can't play ]
//   Songs (N)            1-11 of N        Playlist (M)          1-11 of M
//   [ song buttons, virtual ] |scroll|  > [ entry buttons  (^) (v) (x) ] |scroll|
//   feedback note (green / orange, a few seconds)
// No key hint bar and no action button row: every list row is a button in the game's button colours
// (game_button.h: pink, DeepBlue on hover, white label), a song row adds the song, a playlist row plays
// that entry, and each playlist row has its own small move up / move down / remove buttons. The playing
// entry is teal with a pink '>' left of its row. The header buttons are the game's popup button through
// the shared modules/common/game_button (round 13: "Use game's button ... they have support to show what
// icon to press"), with their key caps R / F / C; our own SDF pills if that fails. Round 13 also dropped
// the status line ("Remove the top status row 'Now playing: ...'"): only a warning shows, when the radio
// can't play (no Music Box: the stored playlist greyed, not empty). Other keys of round 11 still work
// unprinted (radio_menu.h). The row count follows the canvas height (as the receipt): 5..11 rows of 44 px.
// The parts are the receipt's (custom_upgrades silo_receipt.cpp: SDF shapes with the game's menu palette,
// TMP texts with the HUD's font, Toggles as pollable click targets, UIInputLocker); the helpers are
// repeated here because they are private to that file.

#include "modules/slime_dj/radio_menu.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <format>
#include <initializer_list>
#include <string_view>

#include "core/detour_guard.h"
#include "core/il2cpp_api.h"
#include "core/log.h"
#include "core/main_thread.h"
#include "core/module.h"
#include "core/resolver.h"
#include "core/unity.h"
#include "modules/common/game_button.h"

namespace sr2::modules::slime_dj {
namespace {

namespace unity = sr2::core::unity;
namespace gbn = sr2::modules::game_button;
namespace main_thread = sr2::core::main_thread;
using sr2::il2cpp::api;
using sr2::il2cpp::Resolver;

constexpr const char* kModule = "slime_dj";
constexpr const char* kImage = "Assembly-CSharp";
constexpr const char* kCore = "UnityEngine.CoreModule";
constexpr const char* kUi = "UnityEngine.UI";
constexpr const char* kTmp = "Unity.TextMeshPro";
constexpr const char* kInput = "Unity.InputSystem";

constexpr int kAlignLeft = 513, kAlignCenter = 514, kAlignRight = 516;  // TMP TextAlignmentOptions
constexpr float kGraceSeconds = 0.3f;  // input ignored right after opening
constexpr int kSortingOrder = 1000;    // above the HUD
constexpr float kRepeatDelay = 0.35f, kRepeatEvery = 0.06f;  // held Up / Down
constexpr float kNoteSeconds = 3.0f;
constexpr int kWheelRows = 3;

struct Vec2 {
    float x, y;
};
struct Color {
    float r, g, b, a;
    bool operator==(const Color&) const = default;
};

// The game's menu palette (drone station / plot menu scene data), as in the Sell-All receipt.
constexpr Color kNavy{0.267f, 0.325f, 0.478f, 1.0f};
constexpr Color kNavyDim{0.267f, 0.325f, 0.478f, 0.60f};
constexpr Color kWhite{1.0f, 1.0f, 1.0f, 1.0f};
constexpr Color kLavender{0.737f, 0.686f, 0.824f, 1.0f};
constexpr Color kBacker{0.875f, 0.902f, 0.945f, 1.0f};
constexpr Color kSelected{0.80f, 0.86f, 0.96f, 1.0f};  // the playlist entry selected while another area has focus
constexpr Color kTeal{0.063f, 0.675f, 0.757f, 1.0f};
constexpr Color kPink{0.871f, 0.310f, 0.522f, 1.0f};
constexpr Color kGrey{0.78f, 0.80f, 0.84f, 1.0f};
constexpr Color kGreyRow{gbn::kGrey.r, gbn::kGrey.g, gbn::kGrey.b, 1.0f};  // the game's disabled button grey
constexpr Color kPlaying{gbn::kTeal.r, gbn::kTeal.g, gbn::kTeal.b, 1.0f};  // the playing entry's row
constexpr Color kGreen{0.20f, 0.62f, 0.33f, 1.0f};
constexpr Color kOrange{0.90f, 0.45f, 0.12f, 1.0f};
constexpr Color kClear{0.0f, 0.0f, 0.0f, 0.0f};
constexpr Color kUnset{-1, -1, -1, -1};

constexpr float kPanelW = 1400.0f;
constexpr float kPad = 40.0f;
constexpr float kColGap = 40.0f;
constexpr float kColW = (kPanelW - 2 * kPad - kColGap) / 2;  // 640
constexpr float kRowH = 44.0f, kRowGap = 6.0f;
constexpr int kMaxRows = 11, kMinRows = 5;
constexpr float kRowW = kColW - 22;  // a row inside its list box (scroll thumb on the right)
constexpr float kInlD = 34.0f, kInlGap = 6.0f;  // the playlist rows' small buttons
constexpr float kInlW = 3 * kInlD + 2 * kInlGap + 6.0f;
constexpr float kHdrBtnW = 250.0f, kHdrBtnH = 52.0f, kHdrY = 26.0f;  // header buttons (slot width; own pills)
constexpr float kGameButtonScale = 0.72f;  // the popup button (59 high, 42 pt label), as in the receipt
constexpr float kMarkerW = 22.0f;           // the playing entry's '>' left of its row

// Header buttons: the radio's switches, right of the title, with their keys.
enum Hdr { kHdrToggle, kHdrShuffle, kHdrClear, kHdrCount };
constexpr const char* kHdrNames[kHdrCount] = {"Radio", "Shuffle", "Clear"};
constexpr const char* kHdrKeys[kHdrCount] = {"R", "F", "C"};
// A playlist row's small buttons.
enum Inl { kInlUp, kInlDown, kInlRemove, kInlCount };

std::string Clock(double s) {
    if (!(s > 0)) return {};
    const long long t = std::llround(s);
    if (t >= 3600) return std::format("{}:{:02}:{:02}", t / 3600, t / 60 % 60, t % 60);
    return std::format("{}:{:02}", t / 60, t % 60);
}

}  // namespace

struct RadioMenu::Impl {
    // ---- classes ----
    Il2CppClass* goClass = nullptr, *rtClass = nullptr, *imageClass = nullptr, *toggleClass = nullptr;
    Il2CppClass* canvasClass = nullptr, *scalerClass = nullptr, *raycasterClass = nullptr;
    Il2CppClass* tmpTextClass = nullptr, *tmpUguiClass = nullptr, *sdfRectClass = nullptr, *lockerClass = nullptr;

    // ---- methods ----
    const MethodInfo* goCtor = nullptr, *goAddComponent = nullptr, *trSetParent = nullptr;
    const MethodInfo* rtSetAnchorMin = nullptr, *rtSetAnchorMax = nullptr, *rtSetPivot = nullptr, *rtSetSize = nullptr,
                    *rtSetPos = nullptr;
    const MethodInfo* objDestroy = nullptr, *gfxSetColor = nullptr, *gfxSetRaycast = nullptr;
    const MethodInfo* tmpSetText = nullptr, *tmpGetFont = nullptr, *tmpSetFont = nullptr, *tmpSetSize = nullptr,
                    *tmpSetColor = nullptr, *tmpSetAlign = nullptr, *tmpSetWrap = nullptr, *tmpSetOverflow = nullptr;
    const MethodInfo* cvSetMode = nullptr, *cvSetOrder = nullptr;
    const MethodInfo* scSetMode = nullptr, *scSetRef = nullptr, *scSetMatch = nullptr;
    const MethodInfo* sdfSetRadius = nullptr, *sdfSetBorder = nullptr, *sdfSetBorderColor = nullptr;
    const MethodInfo* tglGetOn = nullptr, *tglSetOnQuiet = nullptr, *selSetTransition = nullptr, *selSetNav = nullptr;
    const MethodInfo* selPointerInside = nullptr;  // optional: hover (rows, the wheel's list)
    const MethodInfo* trSetLocalRotation = nullptr;  // optional
    const MethodInfo* screenW = nullptr, *screenH = nullptr;
    // input (InputSystem)
    const MethodInfo* kbCurrent = nullptr, *padCurrent = nullptr, *mouseCurrent = nullptr;
    const MethodInfo* btnPressed = nullptr, *btnIsPressed = nullptr;
    const MethodInfo* kbEsc = nullptr, *kbEnter = nullptr, *kbNumEnter = nullptr, *kbSpace = nullptr, *kbUp = nullptr,
                    *kbDown = nullptr, *kbLeft = nullptr, *kbRight = nullptr, *kbW = nullptr, *kbS = nullptr, *kbA = nullptr,
                    *kbD = nullptr, *kbTab = nullptr, *kbDelete = nullptr, *kbBackspace = nullptr, *kbR = nullptr,
                    *kbF = nullptr, *kbPgUp = nullptr, *kbPgDn = nullptr, *kbHome = nullptr, *kbEnd = nullptr,
                    *kbShift = nullptr, *kbC = nullptr;
    const MethodInfo* padSouth = nullptr, *padEast = nullptr, *padWest = nullptr, *padNorth = nullptr, *padDpad = nullptr,
                    *padStick = nullptr, *padLB = nullptr, *padRB = nullptr, *padLT = nullptr, *padRT = nullptr;
    const MethodInfo* dirUp = nullptr, *dirDown = nullptr, *dirLeft = nullptr, *dirRight = nullptr;
    const MethodInfo* stickUp = nullptr, *stickDown = nullptr, *stickLeft = nullptr, *stickRight = nullptr;
    const MethodInfo* mouseScroll = nullptr, *vec2Y = nullptr, *axisRead = nullptr;
    using ObjectNewFn = Il2CppObject* (*)(Il2CppClass*);
    ObjectNewFn objectNew = nullptr;
    // optional: the game's popup button (modules/common/game_button) for the header buttons and the rows' colours
    bool gameButtonsOk = false;

    // ---- live state (main thread) ----
    struct Small {  // a playlist row's small button: a round pill with a glyph of two bars
        Il2CppObject* bg = nullptr, *bar[2] = {}, *toggle = nullptr;
        Color fill = kUnset, glyph = kUnset;
    };
    struct Slot {
        Il2CppObject* bg = nullptr, *name = nullptr, *time = nullptr, *toggle = nullptr, *inlRoot = nullptr;
        Il2CppObject* marker = nullptr;  // playlist: the '>' left of the row (shown on the playing entry)
        int markerShown = -1;
        std::string nameText, timeText;
        Color bgCol = kUnset, textCol = kUnset, timeCol = kUnset;
        Small inl[kInlCount];
        int inlShown = -1;
    };
    struct Header {
        gbn::Button game;                    // the game's button (game.Exists(); else our own pill below)
        Il2CppObject* button = nullptr;      // own pill: its Toggle
        Il2CppObject* bg = nullptr, *text = nullptr;
        std::string label;
        Color fill = kUnset, textCol = kUnset;
    };
    // Row colours: the game's popup button (game_button.h: pink, DeepBlue on mouse-over, white label) when
    // it is available, else the menu palette.
    struct Style {
        Color normal = kBacker, hover = kTeal, text = kNavy, hoverText = kWhite, border = kLavender;
        bool fromGame = false;
    };
    bool open = false;
    float grace = 0.0f;
    std::vector<Il2CppGCHandle> handles;
    Il2CppObject* root = nullptr, *rootRt = nullptr, *backdropToggle = nullptr, *font = nullptr;
    Il2CppObject* warnBox = nullptr, *warnText = nullptr;  // the warning pill (inactive while there is none)
    Il2CppObject* noteText = nullptr, *head[2] = {}, *range[2] = {}, *listToggle[2] = {}, *thumb[2] = {};
    std::string warnStr, noteStr, headStr[2], rangeStr[2];
    int warnShown = -1;
    Color noteCol = kUnset, thumbCol[2] = {kUnset, kUnset};
    float listY = 0.0f, listH = 0.0f;
    std::vector<Slot> slots[2];
    Header hdr[kHdrCount];
    Style style;
    int rows = kMaxRows;
    int area = 0;              // 0 = Songs, 1 = Playlist, 2 = header buttons
    int cursor[2] = {0, 0}, top[2] = {0, 0}, hdrFocus = kHdrToggle;
    int hoverList = -1, hoverRow = -1, hoverInl = -1, hoverHdr = -1;  // under the mouse (last frame)
    int repeatDir = 0;
    float repeatTimer = 0.0f;
    float noteLeft = 0.0f;
    float clearArmed = 0.0f;   // Clear pressed once: a second press within this time empties the playlist
    std::shared_ptr<const std::vector<MenuSong>> songs;
    MenuState st;
    bool loggedOpen = false, loggedFail = false, loggedButtonMiss = false;

    // ---- helpers ----
    bool Faulted() const { return sr2::core::ModuleFaulted(kModule); }
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
            if (err.rfind("native exception", 0) == 0) sr2::core::MarkModuleFaulted(kModule, std::format("radio menu: {}: {}", what, err));
            else SR2_WARN("slime_dj: radio menu: {} failed: {}", what, err);
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
    void Keep(Il2CppObject* o) {
        if (Il2CppGCHandle h = o ? unity::NewGCHandle(o) : nullptr) handles.push_back(h);
    }
    void FreeLater() {
        std::vector<Il2CppGCHandle> doomed;
        doomed.swap(handles);
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
    // Centred in the parent, offset (x right, y up), turned `deg` about z.
    void Centred(Il2CppObject* rt, float x, float y, float w, float h, float deg = 0.0f) {
        Rect(rt, {0.5f, 0.5f}, {0.5f, 0.5f}, {0.5f, 0.5f}, {x, y}, {w, h});
        if (deg != 0.0f && trSetLocalRotation) {
            const float half = deg * 3.14159265f / 360.0f;
            float q[4] = {0.0f, 0.0f, std::sin(half), std::cos(half)};
            Call(trSetLocalRotation, rt, {q}, "Transform.set_localRotation");
        }
    }
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
    void SetColorCached(Il2CppObject* g, Color c, Color& was) {
        if (c == was) return;
        was = c;
        SetColor(g, c);
    }
    void SetRaycast(Il2CppObject* g, bool on) {
        if (unity::IsAlive(g)) Call(gfxSetRaycast, g, {&on}, "Graphic.set_raycastTarget");
    }
    void SetText(Il2CppObject* tmp, const std::string& s) {
        if (unity::IsAlive(tmp)) Call(tmpSetText, tmp, {unity::NewString(s)}, "TMP_Text.set_text");
    }
    void SetTextCached(Il2CppObject* tmp, const std::string& s, std::string& was) {
        if (s == was) return;
        was = s;
        SetText(tmp, s);
    }
    void SetTextColor(Il2CppObject* tmp, Color c, Color& was) {
        if (c == was || !unity::IsAlive(tmp)) return;
        was = c;
        Call(tmpSetColor, tmp, {&c}, "TMP_Text.set_color");
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
    // A box; `graphic` gets its shape (raycast off unless `clickable`).
    Il2CppObject* Box(Il2CppObject* parentRt, const char* name, float x, float y, float w, float h, Color c, float radius,
                      Il2CppObject** graphic = nullptr, float border = 0.0f, Color borderColor = {}) {
        Il2CppObject* go = NewUi(name, parentRt);
        if (!go) return nullptr;
        At(unity::TransformOf(go), x, y, w, h);
        Il2CppObject* g = Shape(go, c, radius, border, borderColor);
        SetRaycast(g, false);
        if (graphic) *graphic = g;
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
    bool Clicked(Il2CppObject* toggle) {  // true once per click
        if (!unity::IsAlive(toggle)) return false;
        bool ok = false;
        const bool on = Unbox<bool>(Call(tglGetOn, toggle, {}, "Toggle.get_isOn", &ok));
        if (!ok || !on) return false;
        bool off = false;
        Call(tglSetOnQuiet, toggle, {&off}, "Toggle.SetIsOnWithoutNotify");
        return true;
    }
    bool PointerInside(Il2CppObject* selectable) {
        if (!selPointerInside || !unity::IsAlive(selectable)) return false;
        bool ok = false;
        return Unbox<bool>(Call(selPointerInside, selectable, {}, "Selectable.get_isPointerInside", &ok)) && ok;
    }
    Il2CppObject* Device(const MethodInfo* current) {
        if (!current) return nullptr;
        bool ok = false;
        Il2CppObject* dev = Call(current, nullptr, {}, "InputDevice.get_current", &ok);
        return ok ? dev : nullptr;
    }
    bool Control(Il2CppObject* dev, const MethodInfo* control, const MethodInfo* sub, const MethodInfo* state) {
        if (!dev || !control || !state) return false;
        bool ok = false;
        Il2CppObject* ctl = Call(control, dev, {}, "input control getter", &ok);
        if (ok && ctl && sub) ctl = Call(sub, ctl, {}, "input sub-control getter", &ok);
        if (!ok || !ctl) return false;
        return Unbox<bool>(Call(state, ctl, {}, "ButtonControl state", &ok)) && ok;
    }
    bool Pressed(Il2CppObject* dev, const MethodInfo* control, const MethodInfo* sub = nullptr) {
        return Control(dev, control, sub, btnPressed);
    }
    bool Held(Il2CppObject* dev, const MethodInfo* control, const MethodInfo* sub = nullptr) {
        return Control(dev, control, sub, btnIsPressed);
    }
    float CanvasHeight() {
        if (!screenW || !screenH) return 1080.0f;
        bool ok = false, ok2 = false;
        const int w = Unbox<int32_t>(Call(screenW, nullptr, {}, "Screen.get_width", &ok));
        const int h = Unbox<int32_t>(Call(screenH, nullptr, {}, "Screen.get_height", &ok2));
        if (!ok || !ok2 || w <= 0 || h <= 0) return 1080.0f;
        const float scale = std::sqrt(static_cast<float>(w) / 1920.0f * static_cast<float>(h) / 1080.0f);
        if (!std::isfinite(scale) || scale <= 0.0f) return 1080.0f;
        return std::clamp(static_cast<float>(h) / scale, 400.0f, 4000.0f);
    }

    int Count(int list) const {
        if (list == 0) return songs ? static_cast<int>(songs->size()) : 0;
        return static_cast<int>(st.playlist.size());
    }
    // Keeps the cursor inside the list and in view.
    void Clamp(int list) {
        const int n = Count(list);
        cursor[list] = n ? std::clamp(cursor[list], 0, n - 1) : 0;
        if (cursor[list] < top[list]) top[list] = cursor[list];
        if (cursor[list] >= top[list] + rows) top[list] = cursor[list] - rows + 1;
        top[list] = std::clamp(top[list], 0, std::max(0, n - rows));
    }
    void ShowNote(const std::string& text, bool good, float seconds = kNoteSeconds) {
        SetTextCached(noteText, text, noteStr);
        SetTextColor(noteText, good ? kGreen : kOrange, noteCol);
        noteLeft = seconds;
    }

    void ReadGameStyle();
    bool BuildHeader(Il2CppObject* prt);
    bool BuildSmall(Il2CppObject* parentRt, Small& s, int kind, float cx);
    bool Build();
    void Redraw();
    void Teardown();
};

// ------------------------------------------------------------- game style --

// The rows' colours: the game's popup button (game_button.h kPink / kDeepBlue with a white label: the
// colours of its ButtonAnimator, re/ui.md) when the button can be cloned, else the menu palette.
void RadioMenu::Impl::ReadGameStyle() {
    style = Style{};
    if (!gameButtonsOk) return;
    style.normal = {gbn::kPink.r, gbn::kPink.g, gbn::kPink.b, 1.0f};
    style.hover = {gbn::kDeepBlue.r, gbn::kDeepBlue.g, gbn::kDeepBlue.b, 1.0f};
    style.text = style.hoverText = kWhite;
    style.border = kLavender;
    style.fromGame = true;
}

// ---------------------------------------------------------------- setup --

RadioMenu::RadioMenu() = default;
RadioMenu::~RadioMenu() = default;  // impl_ lives for the process (the module is never destroyed)

bool RadioMenu::Setup(Resolver& R, std::string& why) {
    auto* p = new Impl();
    auto parentOf = [](Il2CppClass* c) { return api().il2cpp_class_get_parent ? api().il2cpp_class_get_parent(c) : nullptr; };
    auto cls = [&](const char* image, const char* ns, const char* name) -> Il2CppClass* {
        std::string err;
        Il2CppClass* k = R.Class(image, ns, name, &err);
        if (!k && why.empty()) why = err.empty() ? std::format("class {}.{} not found", ns, name) : err;
        return k;
    };
    auto method = [&](Il2CppClass* k, const char* name, int argc) -> const MethodInfo* {
        std::string err;
        for (Il2CppClass* c = k; c; c = parentOf(c))
            if (const MethodInfo* m = R.Method(c, name, argc, &err)) return m;
        if (why.empty()) why = std::format("method {}({}) not found", name, argc);
        return nullptr;
    };
    auto opt = [&](Il2CppClass* k, const char* name, int argc) -> const MethodInfo* {
        std::string err;
        for (Il2CppClass* c = k; c; c = parentOf(c))
            if (const MethodInfo* m = R.Method(c, name, argc, &err)) return m;
        return nullptr;
    };
    auto byParams = [&](Il2CppClass* k, const char* name, std::initializer_list<std::string_view> params) {
        std::string err;
        const MethodInfo* m = k ? R.MethodByParams(k, name, params, &err) : nullptr;
        if (!m && why.empty()) why = std::format("{}: {}", name, err);
        return m;
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
    p->lockerClass = cls(kImage, "MonomiPark.SlimeRancher.UI", "UIInputLocker");
    Il2CppClass* kb = cls(kInput, "UnityEngine.InputSystem", "Keyboard");
    Il2CppClass* pad = cls(kInput, "UnityEngine.InputSystem", "Gamepad");
    Il2CppClass* button = cls(kInput, "UnityEngine.InputSystem.Controls", "ButtonControl");
    Il2CppClass* dpad = cls(kInput, "UnityEngine.InputSystem.Controls", "DpadControl");
    Il2CppClass* stick = cls(kInput, "UnityEngine.InputSystem.Controls", "StickControl");
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
    p->kbCurrent = method(kb, "get_current", 0);
    p->padCurrent = method(pad, "get_current", 0);
    p->btnPressed = method(button, "get_wasPressedThisFrame", 0);
    p->btnIsPressed = method(button, "get_isPressed", 0);
    p->kbEsc = method(kb, "get_escapeKey", 0);
    p->kbEnter = method(kb, "get_enterKey", 0);
    p->kbSpace = method(kb, "get_spaceKey", 0);
    p->kbUp = method(kb, "get_upArrowKey", 0);
    p->kbDown = method(kb, "get_downArrowKey", 0);
    p->kbLeft = method(kb, "get_leftArrowKey", 0);
    p->kbRight = method(kb, "get_rightArrowKey", 0);
    p->padSouth = method(pad, "get_buttonSouth", 0);
    p->padEast = method(pad, "get_buttonEast", 0);
    p->padDpad = method(pad, "get_dpad", 0);
    p->dirUp = method(dpad, "get_up", 0);
    p->dirDown = method(dpad, "get_down", 0);
    p->dirLeft = method(dpad, "get_left", 0);
    p->dirRight = method(dpad, "get_right", 0);
    if (!why.empty()) return fail();

    // Optional parts: each missing one only drops its key / feature.
    std::string err;
    p->tmpSetWrap = opt(p->tmpTextClass, "set_textWrappingMode", 1);
    p->tmpSetOverflow = opt(p->tmpTextClass, "set_overflowMode", 1);
    p->selPointerInside = opt(selectable, "get_isPointerInside", 0);
    p->trSetLocalRotation = opt(transform, "set_localRotation", 1);
    p->sdfRectClass = R.Class(kImage, "MonomiPark.SlimeRancher.UI.SDFUI", "SDFRoundedRectangle", &err);
    if (p->sdfRectClass) {
        p->sdfSetRadius = R.MethodByParams(p->sdfRectClass, "SetCornerRadius", {"System.Single"}, &err);
        Il2CppClass* sdf = parentOf(p->sdfRectClass);  // SDFGraphic
        p->sdfSetBorder = sdf ? R.Method(sdf, "set_BorderSize", 1, &err) : nullptr;
        p->sdfSetBorderColor = sdf ? R.Method(sdf, "set_BorderColor", 1, &err) : nullptr;
        if (!p->sdfSetRadius) p->sdfRectClass = nullptr;
    }
    Il2CppClass* screen = R.Class(kCore, "UnityEngine", "Screen", &err);
    p->screenW = opt(screen, "get_width", 0);
    p->screenH = opt(screen, "get_height", 0);
    p->kbNumEnter = opt(kb, "get_numpadEnterKey", 0);
    p->kbW = opt(kb, "get_wKey", 0);
    p->kbS = opt(kb, "get_sKey", 0);
    p->kbA = opt(kb, "get_aKey", 0);
    p->kbD = opt(kb, "get_dKey", 0);
    p->kbTab = opt(kb, "get_tabKey", 0);
    p->kbDelete = opt(kb, "get_deleteKey", 0);
    p->kbBackspace = opt(kb, "get_backspaceKey", 0);
    p->kbR = opt(kb, "get_rKey", 0);
    p->kbF = opt(kb, "get_fKey", 0);
    p->kbPgUp = opt(kb, "get_pageUpKey", 0);
    p->kbPgDn = opt(kb, "get_pageDownKey", 0);
    p->kbHome = opt(kb, "get_homeKey", 0);
    p->kbEnd = opt(kb, "get_endKey", 0);
    p->kbShift = opt(kb, "get_shiftKey", 0);
    p->kbC = opt(kb, "get_cKey", 0);
    p->padWest = opt(pad, "get_buttonWest", 0);
    p->padNorth = opt(pad, "get_buttonNorth", 0);
    p->padStick = opt(pad, "get_leftStick", 0);
    p->padLB = opt(pad, "get_leftShoulder", 0);
    p->padRB = opt(pad, "get_rightShoulder", 0);
    p->padLT = opt(pad, "get_leftTrigger", 0);
    p->padRT = opt(pad, "get_rightTrigger", 0);
    p->stickUp = opt(stick, "get_up", 0);
    p->stickDown = opt(stick, "get_down", 0);
    p->stickLeft = opt(stick, "get_left", 0);
    p->stickRight = opt(stick, "get_right", 0);
    Il2CppClass* mouse = R.Class(kInput, "UnityEngine.InputSystem", "Mouse", &err);
    Il2CppClass* vec2 = R.Class(kInput, "UnityEngine.InputSystem.Controls", "Vector2Control", &err);
    Il2CppClass* axis = R.Class(kInput, "UnityEngine.InputSystem.Controls", "AxisControl", &err);
    p->mouseCurrent = opt(mouse, "get_current", 0);
    p->mouseScroll = opt(mouse, "get_scroll", 0);
    p->vec2Y = opt(vec2, "get_y", 0);
    p->axisRead = opt(axis, "ReadValue", 0);

    // The game's popup button (shared helper; header buttons) and its colours (rows).
    {
        std::string gerr;
        p->gameButtonsOk = p->selPointerInside && gbn::Resolve(R, gerr);
        if (!p->gameButtonsOk) SR2_INFO("slime_dj: radio menu: game buttons not available ({})", gerr.empty() ? "no hover" : gerr);
    }

    std::string missing;
    if (!p->sdfRectClass) missing += " SDF shapes (plain images);";
    if (!p->mouseScroll || !p->vec2Y || !p->axisRead) missing += " mouse wheel;";
    if (!p->selPointerInside) missing += " hover (rows light up only from the keys);";
    if (!p->btnIsPressed || !p->kbShift) missing += " Shift + Up / Down;";
    if (!p->gameButtonsOk) missing += " game buttons (own pills);";
    if (!p->trSetLocalRotation) missing += " row arrows (letters instead);";
    impl_ = p;
    ready_ = true;
    SR2_INFO("slime_dj: radio menu ready{}", missing.empty() ? "" : "; NOT available:" + missing);
    return true;
}

// ---------------------------------------------------------------- build --

// Header buttons: the game's popup button through game_button (round 13), scaled like the receipt's and
// centred on fixed points, with the key cap of its key (R / F / C); else our own pills in the row style.
bool RadioMenu::Impl::BuildHeader(Il2CppObject* prt) {
    const bool haveKey[kHdrCount] = {kbR != nullptr, kbF != nullptr, kbC != nullptr};
    for (int i = 0; i < kHdrCount; ++i) {
        Header& h = hdr[i];
        h.game.Forget();
        h = Header{};
        const float cx = kPanelW - kPad - kHdrBtnW * 0.5f - (kHdrCount - 1 - i) * (kHdrBtnW + 14.0f);
        if (gameButtonsOk) {
            gbn::Spec spec;
            spec.name = std::string("Radio") + kHdrNames[i];
            spec.label = kHdrNames[i];
            spec.key = haveKey[i] ? kHdrKeys[i] : "";
            spec.scale = kGameButtonScale;
            spec.minWidth = 150.0f;
            spec.module = kModule;
            std::string why;
            if (h.game.Create(prt, spec, why)) {
                h.game.Place(cx, kHdrY + kHdrBtnH * 0.5f, 0.5f);
                h.label = kHdrNames[i];
                continue;
            }
            if (!loggedButtonMiss) SR2_WARN("slime_dj: radio menu: game button not cloned ({}) - own buttons", why);
            loggedButtonMiss = true;
            h.game.Forget();
        }
        Il2CppObject* b = NewUi(kHdrNames[i], prt);
        if (!b) return false;
        At(unity::TransformOf(b), cx - kHdrBtnW * 0.5f, kHdrY, kHdrBtnW, kHdrBtnH);
        h.bg = Shape(b, style.normal, kHdrBtnH * 0.5f, 3.0f, style.border);
        SetRaycast(h.bg, true);
        h.text = Text(unity::TransformOf(b), "Label", 0, 0, kHdrBtnW, kHdrBtnH, kHdrNames[i], 24.0f, style.text, kAlignCenter);
        h.label = kHdrNames[i];
        h.button = Clickable(b);
        Keep(h.bg);
        Keep(h.text);
        Keep(h.button);
        if (!h.bg || !h.text || !h.button) return false;
    }
    return true;
}

// One small round button of a playlist row (`cx` = its centre from the row's left edge): move up / move down
// (a chevron of two bars) or remove (a cross of two bars). Bars are rotated SDF pills, so no glyph depends on
// the font; without Transform.set_localRotation the glyph is a letter.
bool RadioMenu::Impl::BuildSmall(Il2CppObject* parentRt, Small& s, int kind, float cx) {
    static const char* const kNames[kInlCount] = {"MoveUp", "MoveDown", "Remove"};
    Il2CppObject* go = NewUi(kNames[kind], parentRt);
    if (!go) return false;
    At(unity::TransformOf(go), cx - kInlD * 0.5f, (kRowH - kInlD) * 0.5f, kInlD, kInlD);
    s.bg = Shape(go, style.normal, kInlD * 0.5f, 2.0f, style.border);
    SetRaycast(s.bg, true);
    s.toggle = Clickable(go);
    Keep(s.bg);
    Keep(s.toggle);
    if (!s.bg || !s.toggle) return false;
    Il2CppObject* rt = unity::TransformOf(go);
    if (!trSetLocalRotation) {
        static const char* const kLetters[kInlCount] = {"^", "v", "x"};
        s.bar[0] = Text(rt, "Glyph", 0, 0, kInlD, kInlD, kLetters[kind], 22.0f, style.text, kAlignCenter);
        Keep(s.bar[0]);
        return s.bar[0] != nullptr;
    }
    // Chevron: apex 4 px above (up) / below (down) the centre, arms 8 px out; cross: two 16 px bars.
    struct Bar {
        float x, y, len, deg;
    };
    const float t = 4.0f;
    Bar bars[2];
    if (kind == kInlRemove) {
        bars[0] = {0, 0, 16.0f, 45.0f};
        bars[1] = {0, 0, 16.0f, -45.0f};
    } else {
        const float sgn = kind == kInlUp ? 1.0f : -1.0f;
        bars[0] = {-4.0f, 0.0f, 12.5f, 45.0f * sgn};
        bars[1] = {4.0f, 0.0f, 12.5f, -45.0f * sgn};
    }
    for (int b = 0; b < 2; ++b) {
        Il2CppObject* bar = NewUi("Bar", rt);
        if (!bar) return false;
        Centred(unity::TransformOf(bar), bars[b].x, bars[b].y, bars[b].len, t, bars[b].deg);
        s.bar[b] = Shape(bar, style.text, t * 0.5f);
        SetRaycast(s.bar[b], false);
        Keep(s.bar[b]);
        if (!s.bar[b]) return false;
    }
    return true;
}

bool RadioMenu::Impl::Build() {
    ReadGameStyle();
    // Rows that fit the canvas (border + margin 36 px each side). Round 13: no status line any more.
    listY = 154.0f;
    const float fixedH = listY + 14.0f + 36.0f + 24.0f;  // everything but the rows
    rows = std::clamp(static_cast<int>((CanvasHeight() - 72.0f - fixedH + kRowGap) / (kRowH + kRowGap)), kMinRows, kMaxRows);
    listH = rows * (kRowH + kRowGap) - kRowGap;
    const float noteY = listY + listH + 14.0f;
    const float panelH = noteY + 36.0f + 24.0f;

    Il2CppObject* panel = NewUi("Panel", rootRt);
    if (!panel) return false;
    Il2CppObject* prt = unity::TransformOf(panel);
    Rect(prt, {0.5f, 0.5f}, {0.5f, 0.5f}, {0.5f, 0.5f}, {0, 0}, {kPanelW, panelH});
    Box(prt, "Border", -8, -8, kPanelW + 16, panelH + 16, kLavender, 34.0f);
    Il2CppObject* contentG = nullptr;
    if (Il2CppObject* content = Box(prt, "Content", 0, 0, kPanelW, panelH, kWhite, 30.0f, &contentG)) {
        SetRaycast(contentG, true);
        Clickable(content);  // never polled: a click on the panel must not reach the backdrop (= close)
    }
    Text(prt, "Title", kPad, 26, 560, 52, "Corral Radio", 40.0f, kNavy, kAlignLeft);
    Text(prt, "Corral", kPad, 80, 300, 32, st.title, 24.0f, kNavyDim, kAlignLeft);
    // The warning pill right of the corral's name (only while the radio can't play; round 13: "make it
    // grayed out with an error warning on top of the dialog").
    {
        constexpr float wx = kPad + 260.0f, ww = kPanelW - kPad - wx, wh = 34.0f;
        warnBox = Box(prt, "Warning", wx, 79, ww, wh, kOrange, wh * 0.5f);
        if (!warnBox) return false;
        warnText = Text(unity::TransformOf(warnBox), "Text", 16, 0, ww - 32, wh, "", 21.0f, kWhite, kAlignCenter);
        Keep(warnBox);
        Keep(warnText);
        unity::SetActive(warnBox, false);
        warnShown = 0;
    }
    if (!BuildHeader(prt)) return false;

    for (int l = 0; l < 2; ++l) {
        const float x = kPad + l * (kColW + kColGap);
        head[l] = Text(prt, l == 0 ? "SongsHead" : "PlaylistHead", x + 6, 118, 400, 34, "", 26.0f, kNavy, kAlignLeft);
        range[l] = Text(prt, l == 0 ? "SongsRange" : "PlaylistRange", x + kColW - 260, 122, 240, 30, "", 19.0f, kNavyDim,
                        kAlignRight);
        Keep(head[l]);
        Keep(range[l]);
        // The list's area: an invisible raycast target with a Toggle (never polled for clicks) whose
        // pointer-inside state tells which list the mouse is over (wheel, hover); the rows are its children.
        Il2CppObject* listG = nullptr;
        Il2CppObject* list = Box(prt, l == 0 ? "Songs" : "Playlist", x - 6, listY - 6, kColW + 12, listH + 12,
                                 Color{1, 1, 1, 0.0f}, 18.0f, &listG);
        if (!list) return false;
        SetRaycast(listG, true);
        listToggle[l] = Clickable(list);
        Keep(listToggle[l]);
        Il2CppObject* lrt = unity::TransformOf(list);
        slots[l].assign(static_cast<size_t>(rows), Slot{});
        for (int i = 0; i < rows; ++i) {
            Slot& s = slots[l][static_cast<size_t>(i)];
            // The row is a button in the game's button look: a pill, mouse-over colour on hover.
            Il2CppObject* rowG = nullptr;
            Il2CppObject* row = Box(lrt, "Row", 6, 6 + i * (kRowH + kRowGap), kRowW, kRowH, style.normal, kRowH * 0.5f,
                                    &rowG, 3.0f, style.border);
            if (!row || !rowG) return false;
            SetRaycast(rowG, true);
            s.bg = rowG;
            s.toggle = Clickable(row);
            Il2CppObject* rrt = unity::TransformOf(row);
            const float inl = l == 1 ? kInlW : 0.0f;
            s.name = Text(rrt, "Name", 22, 0, kRowW - 22 - 104 - inl, kRowH, "", 22.0f, style.text, kAlignLeft);
            s.time = Text(rrt, "Time", kRowW - 104 - inl, 0, 88, kRowH, "", 20.0f, style.text, kAlignRight);
            Keep(s.bg);
            Keep(s.toggle);
            Keep(s.name);
            Keep(s.time);
            if (!s.toggle || !s.name || !s.time) return false;
            if (l == 1) {
                // The playing entry's '>' outside the row, in the list's left margin (round 13: "move the '>'
                // outside before the row currently playing").
                s.marker = Text(lrt, "Playing", 2 - kMarkerW, 6 + i * (kRowH + kRowGap), kMarkerW, kRowH, ">", 28.0f, kPink,
                                kAlignCenter);
                Keep(s.marker);
                if (s.marker) {
                    unity::SetActive(unity::GameObjectOf(s.marker), false);
                    s.markerShown = 0;
                }
                // The entry's own small buttons (round 12: no Up / Down / Remove row any more).
                Il2CppObject* box = NewUi("Controls", rrt);
                if (!box) return false;
                At(unity::TransformOf(box), kRowW - kInlW, 0, kInlW, kRowH);
                s.inlRoot = box;
                Keep(s.inlRoot);
                for (int k = 0; k < kInlCount; ++k)
                    if (!BuildSmall(unity::TransformOf(box), s.inl[k], k, kInlD * 0.5f + k * (kInlD + kInlGap))) return false;
            }
        }
        // Scroll position: a thumb in the list's right margin (display only; the wheel / keys scroll).
        Il2CppObject* th = Box(lrt, "Thumb", kColW - 8, 6, 8, 40, kLavender, 4.0f, &thumb[l]);
        if (!th) return false;
        Keep(thumb[l]);
    }

    noteText = Text(prt, "Note", kPad, noteY, kPanelW - 2 * kPad, 30, "", 21.0f, kGreen, kAlignCenter);
    Keep(noteText);
    return !Faulted();
}

// Writes what changed: row texts and colours, headers, scroll thumbs, status, note, buttons.
void RadioMenu::Impl::Redraw() {
    const Style& sy = style;
    for (int l = 0; l < 2; ++l) {
        Clamp(l);
        const int n = Count(l);
        SetTextCached(head[l], l == 0 ? std::format("Songs ({})", n) : std::format("Playlist ({})", n), headStr[l]);
        SetTextCached(range[l], n > rows ? std::format("{}-{} of {}", top[l] + 1, std::min(n, top[l] + rows), n) : "",
                      rangeStr[l]);
        for (int i = 0; i < rows && i < static_cast<int>(slots[l].size()); ++i) {
            Slot& s = slots[l][static_cast<size_t>(i)];
            const int idx = top[l] + i;
            std::string name, time;
            Color bg = kClear, text = sy.text, tcol = sy.text;
            bool controls = false, marker = false;
            if (idx < n) {
                bool dim = false, playing = false;
                // Without a channel (no Music Box) the stored playlist is shown greyed and does not react.
                const bool greyed = l == 1 && !st.available;
                const bool focused = area == l && idx == cursor[l] && !greyed;
                if (l == 0) {
                    const MenuSong& song = (*songs)[static_cast<size_t>(idx)];
                    name = song.name;
                    time = Clock(song.seconds);
                    dim = !song.playable;
                    if (dim) time = "can't play";
                    else if (focused) time = "+ Add";
                } else {
                    const MenuEntry& e = st.playlist[static_cast<size_t>(idx)];
                    playing = idx == st.playing;
                    marker = playing;
                    name = std::format("{}. {}", idx + 1, e.name);
                    time = e.missing ? "missing" : focused && !playing ? "Play" : Clock(e.seconds);
                    dim = e.missing;
                    controls = st.available;
                }
                // Round 13: the playing entry was pink text on the pink row ("the same color as background");
                // now its row is the game's selected teal with a white label (mouse-over still wins).
                const bool selected = l == 1 && idx == cursor[1] && !greyed;
                if (greyed) {
                    bg = kGreyRow;
                    text = kWhite;
                } else if (focused) {
                    bg = sy.hover;
                    text = sy.hoverText;
                } else if (playing) {
                    bg = kPlaying;
                    text = kWhite;
                } else if (selected) {
                    bg = kSelected;
                    text = kNavy;
                } else {
                    bg = sy.normal;
                    text = sy.text;
                }
                if (dim && !focused) text.a = 0.55f;
                tcol = text;
                tcol.a *= 0.8f;
            } else if (l == 1 && n == 0 && i == 0) {
                name = st.available ? "Empty: click songs on the left to add them." : "The playlist is empty.";
                text = kNavyDim;
            } else if (l == 0 && n == 0 && i == 0) {
                name = songs ? "No songs: put music files into the sr2mods_music folder." : "Reading the music folder...";
                text = kNavyDim;
            }
            SetColorCached(s.bg, bg, s.bgCol);
            SetTextCached(s.name, name, s.nameText);
            SetTextColor(s.name, text, s.textCol);
            SetTextCached(s.time, time, s.timeText);
            SetTextColor(s.time, tcol, s.timeCol);
            if (l == 1 && unity::IsAlive(s.marker) && static_cast<int>(marker) != s.markerShown) {
                s.markerShown = marker ? 1 : 0;
                unity::SetActive(unity::GameObjectOf(s.marker), marker);
            }
            if (l == 1 && unity::IsAlive(s.inlRoot)) {
                if (static_cast<int>(controls) != s.inlShown) {
                    s.inlShown = controls ? 1 : 0;
                    unity::SetActive(s.inlRoot, controls);
                }
                if (controls) {
                    const bool can[kInlCount] = {idx > 0, idx + 1 < n, true};
                    const bool rowFocused = area == 1 && idx == cursor[1];
                    for (int k = 0; k < kInlCount; ++k) {
                        Small& c = s.inl[k];
                        const bool hot = can[k] && hoverList == 1 && hoverRow == i && hoverInl == k;
                        Color fill = !can[k] ? kGrey : hot ? (rowFocused ? kWhite : sy.hover) : sy.normal;
                        Color glyph = !can[k] ? kWhite : hot ? (rowFocused ? sy.hover : sy.hoverText) : sy.text;
                        SetColorCached(c.bg, fill, c.fill);
                        if (glyph == c.glyph) continue;
                        c.glyph = glyph;
                        for (Il2CppObject* b : c.bar) {
                            if (!b) continue;
                            if (IsA(b, tmpTextClass)) Call(tmpSetColor, b, {&glyph}, "TMP_Text.set_color");
                            else SetColor(b, glyph);
                        }
                    }
                }
            }
        }
        // Thumb: its size is the visible share, its position the scroll offset.
        if (unity::IsAlive(thumb[l])) {
            const bool show = n > rows;
            const float h = show ? std::max(24.0f, listH * rows / n) : listH;
            const float y = show ? 6.0f + (listH - h) * top[l] / std::max(1, n - rows) : 6.0f;
            if (show) At(unity::TransformOf(thumb[l]), kColW - 8, y, 8, h);
            SetColorCached(thumb[l], show ? kLavender : kClear, thumbCol[l]);
        }
    }
    // The warning pill: shown only while there is something wrong.
    if (unity::IsAlive(warnBox)) {
        const bool show = !st.warning.empty();
        if (static_cast<int>(show) != warnShown) {
            warnShown = show ? 1 : 0;
            unity::SetActive(warnBox, show);
        }
        if (show) SetTextCached(warnText, st.warning, warnStr);
    }
    // Header buttons: labels follow the radio; Clear needs entries; keys light the focus.
    const bool have = st.available && !st.playlist.empty();
    const bool enabled[kHdrCount] = {st.available && (st.on || have), st.available, have};
    for (int i = 0; i < kHdrCount; ++i) {
        Header& h = hdr[i];
        std::string label = kHdrNames[i];
        if (i == kHdrToggle) label = st.on ? "Radio: on" : "Radio: off";
        else if (i == kHdrShuffle) label = st.shuffle ? "Shuffle: on" : "Shuffle: off";
        else if (i == kHdrClear && clearArmed > 0.0f) label = "Clear?";
        const bool focused = area == 2 && hdrFocus == i;
        if (h.game.Exists()) {
            // The game's button animates hover / press / disabled itself; the key focus tints it teal.
            h.label = label;
            h.game.SetLabel(label);
            h.game.SetInteractable(enabled[i]);
            h.game.SetSelected(focused && enabled[i]);
            continue;
        }
        if (label != h.label) {
            h.label = label;
            SetText(h.text, label);
        }
        const bool hot = focused || hoverHdr == i;
        const bool lit = (i == kHdrToggle && st.on) || (i == kHdrShuffle && st.shuffle);
        const Color fill = !enabled[i] ? kGrey : hot ? sy.hover : lit ? kPink : sy.normal;
        SetColorCached(h.bg, fill, h.fill);
        SetTextColor(h.text, !enabled[i] || lit ? kWhite : hot ? sy.hoverText : sy.text, h.textCol);
    }
}

void RadioMenu::Impl::Teardown() {
    // Deactivate first: UIInputLocker.OnDisable unpauses (input back) right now, Destroy is deferred.
    // Done even after a fault - the game must never stay paused because of us.
    if (unity::IsAlive(root)) {
        unity::SetActive(root, false);
        Destroy(root);
    }
    open = false;
    root = rootRt = backdropToggle = font = warnBox = warnText = noteText = nullptr;
    warnShown = -1;
    for (int l = 0; l < 2; ++l) {
        head[l] = range[l] = listToggle[l] = thumb[l] = nullptr;
        headStr[l].clear();
        rangeStr[l].clear();
        thumbCol[l] = kUnset;
        slots[l].clear();
    }
    for (Header& h : hdr) {
        h.game.Forget();  // destroyed with the root
        h = Header{};
    }
    warnStr.clear();
    noteStr.clear();
    noteCol = kUnset;
    noteLeft = clearArmed = 0.0f;
    hoverList = hoverRow = hoverInl = hoverHdr = -1;
    FreeLater();
}

// ---------------------------------------------------------------- api --

bool RadioMenu::Open(const MenuState& state, std::shared_ptr<const std::vector<MenuSong>> list) {
    if (!ready_ || !impl_ || impl_->open || !main_thread::IsMainThread()) return false;
    Impl& p = *impl_;
    auto failOpen = [&](const char* what) {
        if (!p.loggedFail) SR2_WARN("slime_dj: radio menu not opened: {}", what);
        p.loggedFail = true;
        p.Teardown();
        return false;
    };
    p.st = state;
    p.songs = std::move(list);
    p.area = p.songs && !p.songs->empty() ? 0 : 2;
    p.cursor[0] = p.top[0] = 0;
    p.cursor[1] = std::max(0, state.playing);
    p.top[1] = 0;
    p.hdrFocus = kHdrToggle;
    p.repeatDir = 0;
    p.hoverList = p.hoverRow = p.hoverInl = p.hoverHdr = -1;
    // Font: the HUD's (any active TextMeshProUGUI in the scene; one lookup per open).
    if (Il2CppObject* hudText = unity::FindObjectOfType(p.tmpUguiClass); unity::IsAlive(hudText)) {
        Il2CppObject* f = p.Call(p.tmpGetFont, hudText, {}, "TMP_Text.get_font");
        if (unity::IsAlive(f)) {
            p.font = f;
            p.Keep(f);
        }
    }
    Il2CppObject* go = p.NewGo("sr2mods_RadioMenu");
    if (!go) return failOpen("GameObject creation failed");
    p.root = go;
    p.Keep(go);
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
    // Dimmed backdrop over the whole screen; a click on it closes.
    if (Il2CppObject* bd = p.NewUi("Backdrop", p.rootRt)) {
        p.Rect(unity::TransformOf(bd), {0, 0}, {1, 1}, {0.5f, 0.5f}, {0, 0}, {0, 0});
        Il2CppObject* img = p.AddComponent(bd, p.imageClass, "AddComponent(Image)");
        p.SetColor(img, Color{0.05f, 0.07f, 0.12f, 0.55f});
        p.backdropToggle = p.Clickable(bd);
        p.Keep(p.backdropToggle);
    }
    if (!p.Build()) return failOpen("panel failed");
    p.Redraw();
    // The game's own lock: pauses time, swaps to the UI action map, frees the cursor.
    if (!p.AddComponent(go, p.lockerClass, "AddComponent(UIInputLocker)")) return failOpen("UIInputLocker failed");
    if (p.Faulted()) return failOpen("module faulted");
    unity::SetActive(go, true);
    p.open = true;
    p.grace = kGraceSeconds;
    if (!p.loggedOpen) {
        p.loggedOpen = true;
        SR2_INFO("slime_dj: radio menu opened ({} rows per list, {} song(s), font {}, header buttons {}, rows {}, input "
                 "locked by UIInputLocker)",
                 p.rows, p.Count(0), p.font ? "from the HUD" : "TMP default",
                 p.hdr[0].game.Exists() ? "the game's popup button (game_button)" : "own pills",
                 p.style.fromGame ? "in the game button's colours" : "in the menu palette");
    }
    return true;
}

void RadioMenu::Update(const MenuState& state) {
    if (!ready_ || !impl_ || !impl_->open || state == impl_->st) return;
    impl_->st = state;
    impl_->Redraw();
}

void RadioMenu::SetSongs(std::shared_ptr<const std::vector<MenuSong>> list) {
    if (!ready_ || !impl_ || !impl_->open || list == impl_->songs) return;
    impl_->songs = std::move(list);
    impl_->Redraw();
}

void RadioMenu::Note(const std::string& text, bool good) {
    if (!ready_ || !impl_ || !impl_->open) return;
    impl_->ShowNote(text, good);
}

bool RadioMenu::IsOpen() const { return ready_ && impl_ && impl_->open; }

std::vector<MenuAction> RadioMenu::Tick(float unscaledDt) {
    std::vector<MenuAction> out;
    if (!ready_ || !impl_ || !impl_->open) return out;
    Impl& p = *impl_;
    if (!unity::IsAlive(p.root)) {  // scene change destroyed it (its locker unpaused on the way)
        p.Teardown();
        out.push_back({MenuOp::kClose});
        return out;
    }
    const float dt = std::isfinite(unscaledDt) && unscaledDt > 0.0f ? std::min(unscaledDt, 0.25f) : 0.0f;
    if (p.noteLeft > 0.0f && (p.noteLeft -= dt) <= 0.0f) p.SetTextCached(p.noteText, "", p.noteStr);
    bool dirty = false;
    if (p.clearArmed > 0.0f && (p.clearArmed -= dt) <= 0.0f) dirty = true;
    // Clicks first (reset every frame, also during the grace time).
    const bool clickBackdrop = p.Clicked(p.backdropToggle);
    int clickHdr = -1, clickRow[2] = {-1, -1}, clickInl = -1, clickInlRow = -1;
    for (int i = 0; i < kHdrCount; ++i) {
        Impl::Header& h = p.hdr[i];
        if (h.game.Exists() ? h.game.Tick() : p.Clicked(h.button)) clickHdr = i;
    }
    for (int l = 0; l < 2; ++l)
        for (int i = 0; i < static_cast<int>(p.slots[l].size()); ++i) {
            Impl::Slot& s = p.slots[l][static_cast<size_t>(i)];
            if (p.Clicked(s.toggle)) clickRow[l] = i;
            if (l == 1)
                for (int k = 0; k < kInlCount; ++k)
                    if (p.Clicked(s.inl[k].toggle)) {
                        clickInl = k;
                        clickInlRow = i;
                    }
        }
    if (p.grace > 0.0f) {
        p.grace -= dt;
        return out;
    }
    const int nSongs = p.Count(0), nList = p.Count(1);

    // Hover (mouse): the list under the pointer, its row, the playlist row's small button, the header
    // button. Moving onto another row moves the cursor there (one highlight for mouse and keys).
    {
        int hl = -1, hr = -1, hi = -1, hh = -1;
        if (p.selPointerInside) {
            for (int l = 0; l < 2 && hl < 0; ++l)
                if (p.PointerInside(p.listToggle[l])) hl = l;
            if (hl >= 0) {
                const int n = p.Count(hl);
                for (int i = 0; i < static_cast<int>(p.slots[hl].size()) && p.top[hl] + i < n; ++i)
                    if (p.PointerInside(p.slots[hl][static_cast<size_t>(i)].toggle)) {
                        hr = i;
                        break;
                    }
                if (hl == 1 && hr >= 0)
                    for (int k = 0; k < kInlCount; ++k)
                        if (p.PointerInside(p.slots[1][static_cast<size_t>(hr)].inl[k].toggle)) hi = k;
            } else {
                for (int i = 0; i < kHdrCount; ++i)
                    if (!p.hdr[i].game.Exists() && p.PointerInside(p.hdr[i].button)) hh = i;
            }
        }
        if (hl != p.hoverList || hr != p.hoverRow) {
            if (hl >= 0 && hr >= 0) {
                p.area = hl;
                p.cursor[hl] = p.top[hl] + hr;
            }
            dirty = true;
        }
        if (hi != p.hoverInl || hh != p.hoverHdr) dirty = true;
        p.hoverList = hl;
        p.hoverRow = hr;
        p.hoverInl = hi;
        p.hoverHdr = hh;
    }

    Il2CppObject* kb = p.Device(p.kbCurrent);
    Il2CppObject* pad = p.Device(p.padCurrent);

    // Actions on a playlist entry (its row's small buttons, or the keys on the selected entry).
    auto entry = [&](MenuOp op, int idx) {
        if (!p.st.available || idx < 0 || idx >= nList) return;
        if (op == MenuOp::kUp && idx == 0) return;
        if (op == MenuOp::kDown && idx + 1 >= nList) return;
        out.push_back({op, 0, idx});
        if (op == MenuOp::kUp) p.cursor[1] = idx - 1;
        if (op == MenuOp::kDown) p.cursor[1] = idx + 1;
        dirty = true;
    };
    auto press = [&](int h) {
        switch (h) {
            case kHdrToggle:
                if (p.st.available) out.push_back({MenuOp::kToggle});
                break;
            case kHdrShuffle:
                if (p.st.available) out.push_back({MenuOp::kShuffle});
                break;
            case kHdrClear:
                if (!p.st.available || nList == 0) break;
                if (p.clearArmed > 0.0f) {
                    p.clearArmed = 0.0f;
                    out.push_back({MenuOp::kClear});
                } else {
                    p.clearArmed = 3.0f;
                    p.ShowNote("Press Clear again to empty the whole playlist.", false, 3.0f);
                }
                break;
            default: break;
        }
        dirty = true;
    };
    auto addSong = [&](int idx) {
        if (!p.songs || idx < 0 || idx >= nSongs) return;
        if (!p.st.available) {
            p.ShowNote(p.st.warning.empty() ? "This corral's radio is not ready yet." : p.st.warning, false);
            return;
        }
        out.push_back({MenuOp::kAdd, (*p.songs)[static_cast<size_t>(idx)].id});
    };

    if (clickBackdrop) {
        out.push_back({MenuOp::kClose});
        return out;
    }
    if (clickHdr >= 0) {
        p.area = 2;
        p.hdrFocus = clickHdr;
        press(clickHdr);
    } else if (clickInl >= 0 && p.top[1] + clickInlRow < nList) {
        const int idx = p.top[1] + clickInlRow;
        p.area = 1;
        p.cursor[1] = idx;
        entry(clickInl == kInlUp ? MenuOp::kUp : clickInl == kInlDown ? MenuOp::kDown : MenuOp::kRemove, idx);
    } else if (clickRow[0] >= 0 && p.top[0] + clickRow[0] < nSongs) {
        p.area = 0;
        p.cursor[0] = p.top[0] + clickRow[0];
        addSong(p.cursor[0]);
        dirty = true;
    } else if (clickRow[1] >= 0 && p.top[1] + clickRow[1] < nList) {
        p.area = 1;
        p.cursor[1] = p.top[1] + clickRow[1];
        entry(MenuOp::kPlayAt, p.cursor[1]);  // round 12: a click on an entry plays it
    }

    if (p.Pressed(kb, p.kbEsc) || p.Pressed(pad, p.padEast)) {
        out.push_back({MenuOp::kClose});
        return out;
    }
    const bool shift = p.Held(kb, p.kbShift);
    // Up / Down with auto-repeat while held (Shift: move the playlist entry instead).
    const bool upHeld = p.Held(kb, p.kbUp) || p.Held(kb, p.kbW) || p.Held(pad, p.padDpad, p.dirUp) ||
                        p.Held(pad, p.padStick, p.stickUp);
    const bool downHeld = p.Held(kb, p.kbDown) || p.Held(kb, p.kbS) || p.Held(pad, p.padDpad, p.dirDown) ||
                          p.Held(pad, p.padStick, p.stickDown);
    const int dir = (downHeld ? 1 : 0) - (upHeld ? 1 : 0);
    int step = 0;
    if (dir != p.repeatDir) {
        p.repeatDir = dir;
        step = dir;
        p.repeatTimer = kRepeatDelay;
    } else if (dir != 0 && (p.repeatTimer -= dt) <= 0.0f) {
        step = dir;
        p.repeatTimer = kRepeatEvery;
    }
    const bool left = p.Pressed(kb, p.kbLeft) || p.Pressed(kb, p.kbA) || p.Pressed(pad, p.padDpad, p.dirLeft) ||
                      p.Pressed(pad, p.padStick, p.stickLeft);
    const bool right = p.Pressed(kb, p.kbRight) || p.Pressed(kb, p.kbD) || p.Pressed(pad, p.padDpad, p.dirRight) ||
                       p.Pressed(pad, p.padStick, p.stickRight);
    const bool submit = p.Pressed(kb, p.kbEnter) || p.Pressed(kb, p.kbNumEnter) || p.Pressed(kb, p.kbSpace) ||
                        p.Pressed(pad, p.padSouth);
    int page = 0;
    if (p.Pressed(kb, p.kbPgUp) || p.Pressed(pad, p.padLT)) page = -1;
    if (p.Pressed(kb, p.kbPgDn) || p.Pressed(pad, p.padRT)) page = 1;
    // Mouse wheel: the list under the pointer, else the focused list.
    int wheel = 0;
    if (Il2CppObject* mouse = p.Device(p.mouseCurrent); mouse && p.mouseScroll && p.vec2Y && p.axisRead) {
        bool ok = false;
        Il2CppObject* scroll = p.Call(p.mouseScroll, mouse, {}, "Mouse.get_scroll", &ok);
        Il2CppObject* y = ok && scroll ? p.Call(p.vec2Y, scroll, {}, "Vector2Control.get_y", &ok) : nullptr;
        const float dy = ok && y ? Impl::Unbox<float>(p.Call(p.axisRead, y, {}, "AxisControl.ReadValue", &ok)) : 0.0f;
        if (ok && std::isfinite(dy) && dy != 0.0f) wheel = dy > 0.0f ? -kWheelRows : kWheelRows;
    }
    if (wheel) {
        const int l = p.hoverList >= 0 ? p.hoverList : (p.area == 1 ? 1 : 0);
        const int n = p.Count(l);
        p.top[l] = std::clamp(p.top[l] + wheel, 0, std::max(0, n - p.rows));
        if (p.cursor[l] < p.top[l]) p.cursor[l] = p.top[l];
        if (p.cursor[l] >= p.top[l] + p.rows) p.cursor[l] = p.top[l] + p.rows - 1;
        p.hoverRow = -1;  // the row under the pointer changed: it takes the cursor next frame
        dirty = true;
    }

    if (out.empty()) {
        if (p.Pressed(kb, p.kbTab)) {
            p.area = (p.area + 1) % 3;
            dirty = true;
        } else if (p.Pressed(kb, p.kbR) || p.Pressed(pad, p.padNorth)) {
            press(kHdrToggle);
        } else if (p.Pressed(kb, p.kbF)) {
            press(kHdrShuffle);
        } else if (p.Pressed(kb, p.kbC)) {
            press(kHdrClear);
        } else if (p.area == 2) {
            if (left || right) {
                p.hdrFocus = std::clamp(p.hdrFocus + (right ? 1 : -1), 0, kHdrCount - 1);
                dirty = true;
            } else if (step > 0) {
                p.area = p.hdrFocus == kHdrToggle || !nList ? 0 : 1;  // down into the lists
                dirty = true;
            } else if (submit) {
                press(p.hdrFocus);
            }
        } else {
            const int l = p.area;
            const int n = l == 0 ? nSongs : nList;
            if (left || right) {
                p.area = right ? 1 : 0;
                dirty = true;
            } else if (l == 1 && (shift && step != 0)) {
                entry(step < 0 ? MenuOp::kUp : MenuOp::kDown, p.cursor[1]);
            } else if (l == 1 && (p.Pressed(pad, p.padLB) || p.Pressed(pad, p.padRB))) {
                entry(p.Pressed(pad, p.padLB) ? MenuOp::kUp : MenuOp::kDown, p.cursor[1]);
            } else if (step != 0) {
                if (step < 0 && p.cursor[l] <= 0) {
                    p.area = 2;  // past the top: up into the header buttons
                    p.hdrFocus = l == 1 ? kHdrClear : kHdrToggle;
                } else {
                    p.cursor[l] = std::clamp(p.cursor[l] + step, 0, std::max(0, n - 1));
                }
                dirty = true;
            } else if (page != 0) {
                p.cursor[l] = std::clamp(p.cursor[l] + page * p.rows, 0, std::max(0, n - 1));
                p.top[l] = std::clamp(p.top[l] + page * p.rows, 0, std::max(0, n - p.rows));
                dirty = true;
            } else if (p.Pressed(kb, p.kbHome) || p.Pressed(kb, p.kbEnd)) {
                p.cursor[l] = p.Pressed(kb, p.kbHome) ? 0 : std::max(0, n - 1);
                dirty = true;
            } else if (l == 1 && (p.Pressed(kb, p.kbDelete) || p.Pressed(kb, p.kbBackspace) || p.Pressed(pad, p.padWest))) {
                entry(MenuOp::kRemove, p.cursor[1]);
            } else if (submit) {
                if (l == 0) addSong(p.cursor[0]);
                else entry(MenuOp::kPlayAt, p.cursor[1]);
            }
        }
    }
    if (dirty || !out.empty()) p.Redraw();
    return out;
}

void RadioMenu::Close(const char* why) {
    if (!ready_ || !impl_ || !impl_->open) return;
    SR2_INFO("slime_dj: radio menu closed ({})", why ? why : "");
    impl_->Teardown();
}

}  // namespace sr2::modules::slime_dj
