#include "hud_edit.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

#include "core/log.h"
#include "core/settings.h"
#include "settings_view.h"

#if defined(SR2_HAVE_IMGUI)
#include "imgui.h"
#include "momentum_hud.h"
#include "theme.h"
#endif

namespace sr2::ui::hud_edit {
namespace {

// The settings each element is placed with (registered by its module).
struct Desc {
    const char* name;
    const char* title;  // help bar heading
    const char* module;
    const char* keyX;
    const char* keyY;
    const char* keySize;
    float minSize, maxSize;  // % of the screen height
};

constexpr Desc kDesc[] = {
    {"minimap", "Minimap", "minimap", "pos_x", "pos_y", "size", 10.0f, 50.0f},
    {"Speed Panel", "Speed Panel", "hl2_movement", "momentum_hud_x", "momentum_hud_y", "momentum_hud_size", 0.8f, 8.0f},
};
constexpr int kCount = static_cast<int>(sizeof(kDesc) / sizeof(kDesc[0]));

const Desc& D(Element e) { return kDesc[static_cast<int>(e)]; }

constexpr unsigned long long kCommitWaitMs = 1500;  // commit preview kept until the settings have the values

// Shared with the main thread (Preview).
std::mutex g_mutex;
bool g_previewOn = false;
Element g_previewElem = Element::Minimap;
Placement g_preview;
bool g_pendingOn = false;  // committed, waiting for the posted writes to land
Element g_pendingElem = Element::Minimap;
Placement g_pending;
unsigned long long g_pendingUntil = 0;

std::atomic<bool> g_active{false};
std::atomic<int> g_activeElem{0};
std::atomic<bool> g_finished{false};
std::atomic<bool> g_hoverWants{false};  // Hover: element under the mouse or pressed (render thread writes)

bool Near(double a, double b) { return std::fabs(a - b) < 0.02; }

// The stored values (not pending UI writes) match `p`.
bool Stored(Element e, const Placement& p) {
    const Desc& d = D(e);
    namespace settings = ::sr2::core::settings;
    return Near(settings::Get(d.module, d.keyX), p.x) && Near(settings::Get(d.module, d.keyY), p.y) &&
           Near(settings::Get(d.module, d.keySize), p.size);
}

void SetPreview(bool on, Element e, const Placement& p) {
    std::lock_guard lk(g_mutex);
    g_previewOn = on;
    g_previewElem = e;
    g_preview = p;
}

}  // namespace

bool Preview(Element e, Placement& out) {
    Placement pending;
    unsigned long long until = 0;
    {
        std::lock_guard lk(g_mutex);
        if (g_previewOn && g_previewElem == e) {
            out = g_preview;
            return true;
        }
        if (!g_pendingOn || g_pendingElem != e) return false;
        pending = g_pending;
        until = g_pendingUntil;
    }
    if (GetTickCount64() < until && !Stored(e, pending)) {
        out = pending;
        return true;
    }
    std::lock_guard lk(g_mutex);
    if (g_pendingElem == e) g_pendingOn = false;
    return false;
}

bool Active() { return g_active.load(std::memory_order_acquire); }

bool Editing(Element e) { return Active() && g_activeElem.load(std::memory_order_relaxed) == static_cast<int>(e); }

bool MoveRow(const char* module, const char* key, Element& out) {
    for (int i = 0; i < kCount; ++i)
        if (!std::strcmp(module, kDesc[i].module) && !std::strcmp(key, kDesc[i].keyX)) {
            out = static_cast<Element>(i);
            return true;
        }
    return false;
}

bool Hidden(const char* module, const char* key) {
    for (const Desc& d : kDesc)
        if (!std::strcmp(module, d.module) && (!std::strcmp(key, d.keyY) || !std::strcmp(key, d.keySize))) return true;
    return false;
}

const char* Name(Element e) { return D(e).name; }

bool TakeFinished() { return g_finished.exchange(false); }

bool WantsMouse() { return g_hoverWants.load(std::memory_order_relaxed); }

#if defined(SR2_HAVE_IMGUI)

namespace {

namespace settings = ::sr2::core::settings;

enum class Drag { None, Move, Resize };

// Render thread only.
struct State {
    Element elem = Element::Minimap;
    Placement orig, cur;
    Drag drag = Drag::None;
    int corner = 0;            // 0 TL, 1 TR, 2 BL, 3 BR (Resize)
    ImVec2 startMouse{}, startTl{}, startBox{}, grab{};
    bool centred = false;      // snapped to the horizontal screen centre (guide line)
} g_st;

const settings::Def* FindDef(const std::vector<settings::Def>& all, const char* module, const char* key) {
    for (const auto& d : all)
        if (d.module == module && d.key == key) return &d;
    return nullptr;
}

// Element box in px for `size` (% of the screen height).
ImVec2 Box(Element e, float size, float H) {
    if (e == Element::SpeedPanel) {
        float w = 0, h = 0;
        momentum_hud::EditBox(size, H, w, h);
        return ImVec2(w, h);
    }
    const float s = H * size / 100.0f;
    return ImVec2(s, s);
}

void Finish(bool keep) {
    const Element e = g_st.elem;
    if (keep) {
        const Desc& d = D(e);
        const auto all = settings::All();
        const char* keys[3] = {d.keyX, d.keyY, d.keySize};
        const float vals[3] = {g_st.cur.x, g_st.cur.y, g_st.cur.size};
        for (int i = 0; i < 3; ++i)
            if (const settings::Def* def = FindDef(all, d.module, keys[i])) Write(*def, vals[i]);
        {
            std::lock_guard lk(g_mutex);
            g_pendingOn = true;
            g_pendingElem = e;
            g_pending = g_st.cur;
            g_pendingUntil = GetTickCount64() + kCommitWaitMs;
        }
        SR2_INFO("ui: {} placed at {:.1f}% / {:.1f}%, size {:.2f}%", D(e).name, g_st.cur.x, g_st.cur.y, g_st.cur.size);
    }
    SetPreview(false, e, {});
    g_st.drag = Drag::None;
    g_active.store(false, std::memory_order_release);
    g_finished.store(true);
}

// One edge as long dashes, starting with a dash at `a`.
void DashedLine(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 col, float thick, float dash, float gap) {
    const float dx = b.x - a.x, dy = b.y - a.y;
    const float len = std::sqrt(dx * dx + dy * dy);
    if (len < 1.0f) return;
    const float ux = dx / len, uy = dy / len;
    for (float t = 0; t < len; t += dash + gap) {
        const float t1 = std::min(len, t + dash);
        dl->AddLine(ImVec2(a.x + ux * t, a.y + uy * t), ImVec2(a.x + ux * t1, a.y + uy * t1), col, thick);
    }
}

void DashedRect(ImDrawList* dl, ImVec2 mn, ImVec2 mx, ImU32 col, float thick, float dash, float gap) {
    DashedLine(dl, mn, ImVec2(mx.x, mn.y), col, thick, dash, gap);
    DashedLine(dl, ImVec2(mx.x, mn.y), mx, col, thick, dash, gap);
    DashedLine(dl, mx, ImVec2(mn.x, mx.y), col, thick, dash, gap);
    DashedLine(dl, ImVec2(mn.x, mx.y), mn, col, thick, dash, gap);
}

ImVec2 CornerOf(ImVec2 mn, ImVec2 mx, int c) { return ImVec2((c & 1) ? mx.x : mn.x, (c & 2) ? mx.y : mn.y); }

bool KeyDown(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

ImU32 Fade(ImU32 c, float a) {
    const float al = static_cast<float>((c >> IM_COL32_A_SHIFT) & 0xFFu) * a;
    return (c & ~IM_COL32_A_MASK) | (static_cast<ImU32>(al + 0.5f) << IM_COL32_A_SHIFT);
}

ImU32 Darker(ImU32 c, float k) {
    ImVec4 v = ImGui::ColorConvertU32ToFloat4(c);
    v.x *= k;
    v.y *= k;
    v.z *= k;
    return ImGui::ColorConvertFloat4ToU32(v);
}

ImVec2 TextSz(ImFont* f, float fs, const char* t) { return f->CalcTextSizeA(fs, 1e9f, 0.0f, t); }

// Soft drop shadow below a rounded rect (as the overlay window's floating surfaces, menu.cpp).
void SoftShadow(ImDrawList* dl, ImVec2 a, ImVec2 b, float rounding, float dy, float spread, ImU32 col) {
    constexpr int kLayers = 10;
    const ImU32 layer = Fade(col, 0.18f);
    const float top = a.y + rounding + spread;  // hidden behind the rect
    for (int i = 0; i < kLayers; ++i) {
        const float e = spread * (1.0f - static_cast<float>(i) / kLayers);
        const float ex = e - spread;  // never wider than the rect: the shadow only shows below it
        dl->AddRectFilled(ImVec2(a.x - ex, top), ImVec2(b.x + ex, b.y + dy + e), layer, std::max(0.0f, rounding + ex));
    }
}

// ---- help bar: a slightly rounded bar at the top centre in the overlay's theme;
// key hints as keycap chips, then the cancel ([Esc] X) and keep ([Enter] check) buttons ----

struct Hint {
    const char* key;
    const char* text;
    int prio;  // the lowest go first when the bar is wider than the screen
};
constexpr Hint kHints[] = {
    {"Drag", "move", 3},
    {"Corner", "resize", 3},
    {"Alt", "from the centre", 1},
    {"Shift", "equal gaps", 1},
};  // Enter / Esc are shown on the buttons themselves
constexpr const char* kCancelKey = "Esc";
constexpr const char* kKeepKey = "Enter";

// Bar metrics in px for the overlay scale `s` (1 = 1080p).
struct Metrics {
    float u, h, padL, gap, hintGap, keyGap, chipX, chipY, btnH, btnPadL, btnPadR, glyph, btnGap, edge, top, barR,
        btnRound;
    float fsTitle, fsText, fsKey;
    explicit Metrics(float s)
        : u(s), h(44 * s), padL(20 * s), gap(14 * s), hintGap(16 * s), keyGap(6 * s), chipX(7 * s), chipY(3 * s),
          btnH(32 * s), btnPadL(5 * s), btnPadR(9 * s), glyph(14 * s), btnGap(6 * s), edge(6 * s), top(16 * s),
          barR(8 * s), btnRound(6 * s), fsTitle(theme::kDisplaySize * s), fsText(theme::kBodySize * s),
          fsKey((theme::kSmallSize + 0.5f) * s) {}
    float Sep() const { return std::max(1.0f, std::round(2 * u)); }
};

struct Bar {
    ImVec2 mn, mx;              // the bar
    ImVec2 cancelMn, cancelMx;  // the cancel button
    ImVec2 keepMn, keepMx;      // the keep button
    int minPrio = 0;            // hints with prio >= minPrio are shown
};

// A bar button: its key as a keycap, then the glyph (X or check).
float ButtonW(const Metrics& bm, const char* key) {
    const float chip = TextSz(theme::Mono(), bm.fsKey, key).x + bm.chipX * 2;
    return std::round(bm.btnPadL + std::round(chip) + bm.keyGap + bm.glyph + bm.btnPadR);
}

enum class BarHit { None, Body, Cancel, Keep };

float HintW(const Metrics& bm, const Hint& h) {
    return TextSz(theme::Mono(), bm.fsKey, h.key).x + bm.chipX * 2 + bm.keyGap +
           TextSz(theme::Body(), bm.fsText, h.text).x;
}

Bar LayoutBar(Element e, float W, const Metrics& bm) {
    const float titleW = TextSz(theme::Display(), bm.fsTitle, D(e).title).x;
    const float cancelW = ButtonW(bm, kCancelKey), keepW = ButtonW(bm, kKeepKey);
    const float fixed = bm.padL + titleW + bm.gap + bm.Sep() + bm.gap + bm.gap + cancelW + keepW + bm.btnGap + bm.edge;
    const float maxW = W - bm.top * 2;
    Bar b;
    float w = fixed;
    for (b.minPrio = 0; b.minPrio <= 4; ++b.minPrio) {  // 4: no hints at all
        float hw = 0;
        int n = 0;
        for (const Hint& h : kHints)
            if (h.prio >= b.minPrio) {
                hw += HintW(bm, h);
                ++n;
            }
        if (n > 1) hw += bm.hintGap * static_cast<float>(n - 1);
        w = fixed + hw;
        if (w <= maxW) break;
    }
    b.minPrio = std::min(b.minPrio, 4);
    b.mn = ImVec2(std::round((W - w) * 0.5f), std::round(bm.top));
    b.mx = ImVec2(b.mn.x + std::round(w), b.mn.y + std::round(bm.h));
    const float y0 = std::round((b.mn.y + b.mx.y - bm.btnH) * 0.5f), y1 = y0 + std::round(bm.btnH);
    b.keepMx = ImVec2(b.mx.x - std::round(bm.edge), y1);
    b.keepMn = ImVec2(b.keepMx.x - keepW, y0);
    b.cancelMx = ImVec2(b.keepMn.x - std::round(bm.btnGap), y1);
    b.cancelMn = ImVec2(b.cancelMx.x - cancelW, y0);
    return b;
}

BarHit HitBar(const Bar& b, ImVec2 p) {
    auto in = [&](ImVec2 mn, ImVec2 mx) { return p.x >= mn.x && p.x <= mx.x && p.y >= mn.y && p.y <= mx.y; };
    if (in(b.cancelMn, b.cancelMx)) return BarHit::Cancel;
    if (in(b.keepMn, b.keepMx)) return BarHit::Keep;
    if (p.x >= b.mn.x && p.x <= b.mx.x && p.y >= b.mn.y && p.y <= b.mx.y) return BarHit::Body;
    return BarHit::None;
}

// A key as a keycap: monospace text on a rounded chip in the frame colour (like the
// overlay's console-name chips) with a darker edge below. `at` = left end, vertical centre.
float KeyChip(ImDrawList* dl, ImVec2 at, const char* key, const Metrics& bm, float alpha) {
    const theme::Palette& P = theme::Colors();
    const ImVec2 ts = TextSz(theme::Mono(), bm.fsKey, key);
    const ImVec2 sz(std::round(ts.x + bm.chipX * 2), std::round(ts.y + bm.chipY * 2));
    const ImVec2 a(std::round(at.x), std::round(at.y - sz.y * 0.5f - bm.u)), b(a.x + sz.x, a.y + sz.y);
    const float r = 6 * bm.u, edge = std::max(1.0f, std::round(2 * bm.u));
    dl->AddRectFilled(ImVec2(a.x, a.y + edge), ImVec2(b.x, b.y + edge), Fade(P.line, alpha), r);
    dl->AddRectFilled(a, b, Fade(P.frame, alpha), r);
    dl->AddText(theme::Mono(), bm.fsKey, ImVec2(a.x + bm.chipX, a.y + bm.chipY), Fade(P.text, alpha), key);
    return sz.x;
}

// Bar button: the key as a keycap, then a drawn glyph (no font dependency): X = cancel,
// check = keep.
void BarButton(ImDrawList* dl, ImVec2 mn, ImVec2 mx, const char* key, bool keep, bool hot, const Metrics& bm,
               float alpha) {
    const theme::Palette& P = theme::Colors();
    const ImU32 fill = keep ? (hot ? Darker(P.accent, 0.88f) : P.accent) : (hot ? P.btnHover : P.btn);
    const ImU32 ink = keep ? P.accentInk : P.text;
    const float ring = 2 * bm.u;
    if (hot)
        dl->AddRect(ImVec2(mn.x - ring, mn.y - ring), ImVec2(mx.x + ring, mx.y + ring), Fade(P.tabSel, alpha),
                    bm.btnRound + ring, 0, std::max(1.0f, 2 * bm.u));
    dl->AddRectFilled(mn, mx, Fade(fill, alpha), bm.btnRound);
    const float cy = (mn.y + mx.y) * 0.5f;
    const float chipW = KeyChip(dl, ImVec2(mn.x + bm.btnPadL, cy), key, bm, alpha);
    const ImVec2 c(std::round(mn.x + bm.btnPadL + chipW + bm.keyGap + bm.glyph * 0.5f), cy);
    const float r = 16 * bm.u;  // the glyphs' size as on the old 32 px round buttons
    const float t = std::max(1.5f, 2.4f * bm.u);
    if (keep) {
        const ImVec2 pts[3] = {ImVec2(c.x - r * 0.40f, c.y + r * 0.02f), ImVec2(c.x - r * 0.12f, c.y + r * 0.30f),
                               ImVec2(c.x + r * 0.42f, c.y - r * 0.28f)};
        dl->AddPolyline(pts, 3, Fade(ink, alpha), ImDrawFlags_None, t);
    } else {
        const float k = r * 0.32f;
        dl->AddLine(ImVec2(c.x - k, c.y - k), ImVec2(c.x + k, c.y + k), Fade(ink, alpha), t);
        dl->AddLine(ImVec2(c.x - k, c.y + k), ImVec2(c.x + k, c.y - k), Fade(ink, alpha), t);
    }
}

void DrawBar(ImDrawList* dl, Element e, const Bar& b, const Metrics& bm, float alpha, BarHit hot) {
    const theme::Palette& P = theme::Colors();
    const float r = bm.barR;
    SoftShadow(dl, b.mn, b.mx, r, 2 * bm.u, 6 * bm.u, Fade(P.shadow, alpha));
    dl->AddRectFilled(b.mn, b.mx, Fade(P.win, alpha), r);
    dl->AddRect(b.mn, b.mx, Fade(P.line, alpha), r, 0, bm.Sep());
    const float cy = (b.mn.y + b.mx.y) * 0.5f;
    float x = b.mn.x + bm.padL;
    const char* title = D(e).title;
    const ImVec2 ts = TextSz(theme::Display(), bm.fsTitle, title);
    dl->AddText(theme::Display(), bm.fsTitle, ImVec2(x, std::round(cy - ts.y * 0.5f)), Fade(P.head, alpha), title);
    x += ts.x + bm.gap;
    const float sep = bm.Sep(), half = std::round(bm.h * 0.25f);
    dl->AddRectFilled(ImVec2(std::round(x), cy - half), ImVec2(std::round(x) + sep, cy + half), Fade(P.line, alpha),
                      sep * 0.5f);
    x += sep + bm.gap;
    bool first = true;
    for (const Hint& h : kHints) {
        if (h.prio < b.minPrio) continue;
        if (!first) x += bm.hintGap;
        first = false;
        x += KeyChip(dl, ImVec2(x, cy), h.key, bm, alpha) + bm.keyGap;
        const ImVec2 hs = TextSz(theme::Body(), bm.fsText, h.text);
        dl->AddText(theme::Body(), bm.fsText, ImVec2(std::round(x), std::round(cy - hs.y * 0.5f)), Fade(P.soft, alpha),
                    h.text);
        x += hs.x;
    }
    BarButton(dl, b.cancelMn, b.cancelMx, kCancelKey, false, hot == BarHit::Cancel, bm, alpha);
    BarButton(dl, b.keepMn, b.keepMx, kKeepKey, true, hot == BarHit::Keep, bm, alpha);
}

// Enters edit mode for `e` with the stored placement. False if its settings are missing.
bool Start(Element e) {
    if (Active()) return false;
    const Desc& d = D(e);
    const auto all = settings::All();
    const settings::Def* dx = FindDef(all, d.module, d.keyX);
    const settings::Def* dy = FindDef(all, d.module, d.keyY);
    const settings::Def* ds = FindDef(all, d.module, d.keySize);
    if (!dx || !dy || !ds) {
        SR2_WARN("ui: edit mode: {} has no position settings registered", d.name);
        return false;
    }
    g_st = State{};
    g_st.elem = e;
    g_st.orig = {static_cast<float>(Read(*dx)), static_cast<float>(Read(*dy)), static_cast<float>(Read(*ds))};
    g_st.cur = g_st.orig;
    SetPreview(true, e, g_st.cur);
    g_activeElem.store(static_cast<int>(e), std::memory_order_relaxed);
    g_active.store(true, std::memory_order_release);
    SR2_INFO("ui: edit mode for the {}", d.name);
    return true;
}

// Hover (render thread): the element pressed and not yet dragged far enough.
struct HoverState {
    int elem = -1;
    bool armed = false;
    ImVec2 press{};
} g_hv;

// The placement on screen now: the stored one, or a commit that has not landed yet.
Placement Shown(Element e) {
    const Desc& d = D(e);
    Placement p{static_cast<float>(settings::Get(d.module, d.keyX)), static_cast<float>(settings::Get(d.module, d.keyY)),
                static_cast<float>(settings::Get(d.module, d.keySize))};
    Preview(e, p);
    return p;
}

}  // namespace

float DrawKeycap(ImDrawList* dl, float x, float cy, const char* key, float uiScale, float alpha) {
    return KeyChip(dl, ImVec2(x, cy), key, Metrics(uiScale), alpha);
}

float KeycapWidth(const char* key, float uiScale) {
    const Metrics bm(uiScale);
    return std::round(TextSz(theme::Mono(), bm.fsKey, key).x + bm.chipX * 2);
}

namespace {
// "Ctrl+LMB" -> {"Ctrl", "LMB"}; a '+' at the end is a key of its own.
template <class F>
void ForEachKey(const char* keys, F&& f) {
    std::string part;
    for (const char* c = keys; *c; ++c) {
        if (*c == '+' && c[1] && !part.empty()) {
            f(part);
            part.clear();
        } else {
            part += *c;
        }
    }
    if (!part.empty()) f(part);
}

float PlusW(float uiScale) { return std::round(TextSz(theme::Body(), (theme::kSmallSize - 1) * uiScale, "+").x + 4 * uiScale); }
}  // namespace

float DrawKeyCombo(ImDrawList* dl, float x, float cy, const char* keys, float uiScale, float alpha) {
    const theme::Palette& P = theme::Colors();
    const float x0 = x, fs = (theme::kSmallSize - 1) * uiScale;
    bool first = true;
    ForEachKey(keys, [&](const std::string& k) {
        if (!first) {
            const ImVec2 ts = TextSz(theme::Body(), fs, "+");
            dl->AddText(theme::Body(), fs, ImVec2(std::round(x + 2 * uiScale), std::round(cy - ts.y * 0.5f - uiScale)),
                        Fade(P.dim, alpha), "+");
            x += PlusW(uiScale);
        }
        first = false;
        x += DrawKeycap(dl, x, cy, k.c_str(), uiScale, alpha);
    });
    return x - x0;
}

float KeyComboWidth(const char* keys, float uiScale) {
    float w = 0;
    bool first = true;
    ForEachKey(keys, [&](const std::string& k) {
        w += KeycapWidth(k.c_str(), uiScale) + (first ? 0.0f : PlusW(uiScale));
        first = false;
    });
    return w;
}

void Begin(Element e) { Start(e); }

void Cancel() {
    if (!Active()) return;
    g_st.cur = g_st.orig;
    Finish(false);
}

void Draw(float uiScale) {
    if (!Active()) return;
    const ImGuiIO& io = ImGui::GetIO();
    const float W = io.DisplaySize.x, H = io.DisplaySize.y;
    if (W <= 0 || H <= 0) return;
    const float u = uiScale > 0 ? uiScale : 1.0f;
    const Desc& d = D(g_st.elem);

    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        Cancel();
        return;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)) {
        Finish(true);
        return;
    }

    const float pad = 6 * u;       // outline distance from the element
    const float handle = 10 * u;   // corner square edge
    const float hit = 12 * u;      // corner hit half-size
    const float snapPx = 12 * u;   // centre snap distance
    const bool shift = KeyDown(VK_SHIFT), alt = KeyDown(VK_MENU);
    const ImVec2 m = io.MousePos;

    ImVec2 box = Box(g_st.elem, g_st.cur.size, H);
    ImVec2 tl(ToPx(g_st.cur.x, W, box.x), ToPx(g_st.cur.y, H, box.y));
    ImVec2 omn(tl.x - pad, tl.y - pad), omx(tl.x + box.x + pad, tl.y + box.y + pad);

    auto cornerAt = [&](ImVec2 p) {
        for (int c = 0; c < 4; ++c) {
            const ImVec2 k = CornerOf(omn, omx, c);
            if (std::fabs(p.x - k.x) <= hit && std::fabs(p.y - k.y) <= hit) return c;
        }
        return -1;
    };
    const bool mouseOk = ImGui::IsMousePosValid(&m);
    const Metrics bm(u);
    const Bar bar = LayoutBar(g_st.elem, W, bm);
    const BarHit barHit = mouseOk && g_st.drag == Drag::None ? HitBar(bar, m) : BarHit::None;

    if (g_st.drag == Drag::None && mouseOk && ImGui::IsMouseClicked(ImGuiMouseButton_Left, false)) {
        // The bar's buttons first, then the element's corners and inside, then the
        // bar itself (does nothing), then outside (keep).
        if (barHit == BarHit::Cancel) {
            Cancel();
            return;
        }
        if (barHit == BarHit::Keep) {
            Finish(true);
            return;
        }
        const int c = cornerAt(m);
        if (c >= 0) {
            g_st.drag = Drag::Resize;
            g_st.corner = c;
            g_st.grab = ImVec2(m.x - CornerOf(tl, ImVec2(tl.x + box.x, tl.y + box.y), c).x,
                               m.y - CornerOf(tl, ImVec2(tl.x + box.x, tl.y + box.y), c).y);
        } else if (m.x >= omn.x && m.x <= omx.x && m.y >= omn.y && m.y <= omx.y) {
            g_st.drag = Drag::Move;
        } else if (barHit == BarHit::None) {
            Finish(true);  // click outside: keep
            return;
        }
        g_st.startMouse = m;
        g_st.startTl = tl;
        g_st.startBox = box;
    }
    if (g_st.drag != Drag::None && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) g_st.drag = Drag::None;

    if (g_st.drag == Drag::Move && mouseOk) {
        ImVec2 p(g_st.startTl.x + m.x - g_st.startMouse.x, g_st.startTl.y + m.y - g_st.startMouse.y);
        const bool horizontal = std::fabs(m.x - g_st.startMouse.x) >= std::fabs(m.y - g_st.startMouse.y);
        // Nearest corner (by the element's centre) and its two gaps.
        const bool left = p.x + box.x * 0.5f < W * 0.5f, top = p.y + box.y * 0.5f < H * 0.5f;
        auto gapX = [&] { return left ? p.x : W - box.x - p.x; };
        auto gapY = [&] { return top ? p.y : H - box.y - p.y; };
        if (shift && horizontal) {
            const float g = gapY();
            p.x = left ? g : W - box.x - g;
        } else if (std::fabs(p.x + box.x * 0.5f - W * 0.5f) <= snapPx) {
            p.x = W * 0.5f - box.x * 0.5f;
        }
        if (shift && !horizontal) {
            const float g = gapX();
            p.y = top ? g : H - box.y - g;
        }
        p.x = std::clamp(p.x, 0.0f, std::max(0.0f, W - box.x));
        p.y = std::clamp(p.y, 0.0f, std::max(0.0f, H - box.y));
        g_st.cur.x = ToPct(p.x, W, box.x);
        g_st.cur.y = ToPct(p.y, H, box.y);
    } else if (g_st.drag == Drag::Resize && mouseOk) {
        const ImVec2 unit = Box(g_st.elem, 1.0f, H);  // box at 1 % (linear in the size)
        const ImVec2 smn = g_st.startTl, smx(g_st.startTl.x + g_st.startBox.x, g_st.startTl.y + g_st.startBox.y);
        const ImVec2 p(m.x - g_st.grab.x, m.y - g_st.grab.y);  // where the dragged corner goes
        const int c = g_st.corner;
        ImVec2 anchor = alt ? ImVec2((smn.x + smx.x) * 0.5f, (smn.y + smx.y) * 0.5f) : CornerOf(smn, smx, 3 - c);
        const float k = alt ? 2.0f : 1.0f;
        float size = std::max(k * std::fabs(p.x - anchor.x) / std::max(0.01f, unit.x),
                              k * std::fabs(p.y - anchor.y) / std::max(0.01f, unit.y));
        const float fit = std::min(W / std::max(0.01f, unit.x), H / std::max(0.01f, unit.y));
        size = std::clamp(size, d.minSize, std::max(d.minSize, std::min(d.maxSize, fit)));
        const ImVec2 nb(unit.x * size, unit.y * size);
        ImVec2 q;
        if (alt) {
            q = ImVec2(anchor.x - nb.x * 0.5f, anchor.y - nb.y * 0.5f);
        } else {
            q.x = (c & 1) ? anchor.x : anchor.x - nb.x;
            q.y = (c & 2) ? anchor.y : anchor.y - nb.y;
        }
        q.x = std::clamp(q.x, 0.0f, std::max(0.0f, W - nb.x));
        q.y = std::clamp(q.y, 0.0f, std::max(0.0f, H - nb.y));
        g_st.cur.size = size;
        g_st.cur.x = ToPct(q.x, W, nb.x);
        g_st.cur.y = ToPct(q.y, H, nb.y);
    }
    SetPreview(true, g_st.elem, g_st.cur);

    // Draw with the updated placement.
    box = Box(g_st.elem, g_st.cur.size, H);
    tl = ImVec2(ToPx(g_st.cur.x, W, box.x), ToPx(g_st.cur.y, H, box.y));
    omn = ImVec2(tl.x - pad, tl.y - pad);
    omx = ImVec2(tl.x + box.x + pad, tl.y + box.y + pad);
    g_st.centred = std::fabs(tl.x + box.x * 0.5f - W * 0.5f) < 0.75f;

    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const float dash = 14 * u, gap = 8 * u, thick = std::max(1.0f, std::round(2 * u));
    const ImU32 ink = IM_COL32(15, 10, 30, 150), line = IM_COL32(255, 255, 255, 235);
    const ImU32 accent = theme::Colors().accent;
    if (g_st.centred) {
        const float x = std::round(W * 0.5f) + 0.5f;
        DashedLine(dl, ImVec2(x + 1, 0), ImVec2(x + 1, H), ink, thick, dash, gap);
        DashedLine(dl, ImVec2(x, 0), ImVec2(x, H), accent, thick, dash, gap);
    }
    DashedRect(dl, ImVec2(omn.x + 1, omn.y + 1), ImVec2(omx.x + 1, omx.y + 1), ink, thick + 1, dash, gap);
    DashedRect(dl, omn, omx, line, thick, dash, gap);
    const bool onButton = barHit == BarHit::Cancel || barHit == BarHit::Keep;
    const int hot = g_st.drag == Drag::Resize ? g_st.corner : (mouseOk && !onButton ? cornerAt(m) : -1);
    for (int c = 0; c < 4; ++c) {
        const ImVec2 k = CornerOf(omn, omx, c);
        const float h = handle * 0.5f;
        dl->AddRectFilled(ImVec2(k.x - h - 1, k.y - h - 1), ImVec2(k.x + h + 1, k.y + h + 1), ink);
        dl->AddRectFilled(ImVec2(k.x - h, k.y - h), ImVec2(k.x + h, k.y + h), c == hot ? accent : line);
    }
    // The bar stays at the top; while the element is dragged under it, it fades so the element shows through.
    const bool under = omx.x > bar.mn.x && omn.x < bar.mx.x && omx.y > bar.mn.y && omn.y < bar.mx.y;
    DrawBar(dl, g_st.elem, bar, bm, g_st.drag != Drag::None && under ? 0.35f : 1.0f, barHit);

    if (onButton) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    else if (hot >= 0) ImGui::SetMouseCursor((hot == 0 || hot == 3) ? ImGuiMouseCursor_ResizeNWSE : ImGuiMouseCursor_ResizeNESW);
    else if (g_st.drag == Drag::Move || (mouseOk && m.x >= omn.x && m.x <= omx.x && m.y >= omn.y && m.y <= omx.y))
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
}

void Hover(float uiScale, bool allowed) {
    const ImGuiIO& io = ImGui::GetIO();
    const float W = io.DisplaySize.x, H = io.DisplaySize.y;
    const ImVec2 m = io.MousePos;
    if (g_hv.armed && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) g_hv = HoverState{};  // released without a drag
    // Only outside the overlay's windows, with no popup open (a press that started on an element keeps it).
    const bool free = allowed && !Active() && W > 0 && H > 0 && ImGui::IsMousePosValid(&m) &&
                      (g_hv.armed || (!io.WantCaptureMouse && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId)));
    if (!free) {
        g_hv = HoverState{};
        g_hoverWants.store(false, std::memory_order_relaxed);
        return;
    }
    const float u = uiScale > 0 ? uiScale : 1.0f;
    const float pad = 6 * u;  // as the edit outline

    // The element under the mouse (the Speed Panel first: it is the smaller one), or the one pressed.
    // Hidden ones count too: the minimap hides while a menu (this window) frees the cursor.
    int hit = -1;
    ImVec2 mn, mx;
    for (int i = kCount - 1; i >= 0 && hit < 0; --i) {
        if (g_hv.armed && i != g_hv.elem) continue;
        const Desc& dd = kDesc[i];
        if (!settings::Visible(dd.module, dd.keyX)) continue;  // module off (or the Speed Panel turned off)
        const Element e = static_cast<Element>(i);
        const Placement p = Shown(e);
        const ImVec2 box = Box(e, p.size, H);
        const ImVec2 tl(ToPx(p.x, W, box.x), ToPx(p.y, H, box.y));
        mn = ImVec2(tl.x - pad, tl.y - pad);
        mx = ImVec2(tl.x + box.x + pad, tl.y + box.y + pad);
        if (g_hv.armed || (m.x >= mn.x && m.x <= mx.x && m.y >= mn.y && m.y <= mx.y)) hit = i;
    }
    if (g_hv.armed && hit < 0) g_hv = HoverState{};  // its module was turned off meanwhile

    if (!g_hv.armed && hit >= 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Left, false)) {
        g_hv.armed = true;
        g_hv.elem = hit;
        g_hv.press = m;
    }
    if (g_hv.armed) {
        const float dx = m.x - g_hv.press.x, dy = m.y - g_hv.press.y;
        const float thr = std::max(io.MouseDragThreshold, 4 * u);
        if (dx * dx + dy * dy >= thr * thr) {
            // Dragged: edit mode, continuing this drag as a move from the press.
            const Element e = static_cast<Element>(g_hv.elem);
            const ImVec2 press = g_hv.press;
            g_hv = HoverState{};
            g_hoverWants.store(false, std::memory_order_relaxed);
            if (Start(e)) {
                g_st.drag = Drag::Move;
                g_st.startMouse = press;
                g_st.startBox = Box(e, g_st.cur.size, H);
                g_st.startTl = ImVec2(ToPx(g_st.cur.x, W, g_st.startBox.x), ToPx(g_st.cur.y, H, g_st.startBox.y));
            }
            return;
        }
    }
    g_hoverWants.store(hit >= 0, std::memory_order_relaxed);
    if (hit < 0) return;

    // Hover outline behind the overlay's windows, and a label pill below it (above near the bottom).
    const theme::Palette& P = theme::Colors();
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    const float dash = 14 * u, gap = 8 * u, thick = std::max(1.0f, std::round(2 * u));
    DashedRect(dl, ImVec2(mn.x + 1, mn.y + 1), ImVec2(mx.x + 1, mx.y + 1), IM_COL32(15, 10, 30, 110), thick + 1, dash, gap);
    DashedRect(dl, mn, mx, IM_COL32(255, 255, 255, 190), thick, dash, gap);
    char label[64];
    std::snprintf(label, sizeof label, "Drag to move the %s", D(static_cast<Element>(hit)).name);
    const float fs = theme::kSmallSize * u, px = 10 * u, py = 4 * u, off = 6 * u;
    const ImVec2 ts = TextSz(theme::Body(), fs, label);
    const ImVec2 sz(std::round(ts.x + px * 2), std::round(ts.y + py * 2));
    const bool below = mx.y + off + sz.y <= H;
    ImVec2 a(std::round((mn.x + mx.x - sz.x) * 0.5f), std::round(below ? mx.y + off : mn.y - off - sz.y));
    a.x = std::clamp(a.x, 0.0f, std::max(0.0f, W - sz.x));
    a.y = std::clamp(a.y, 0.0f, std::max(0.0f, H - sz.y));
    const ImVec2 b(a.x + sz.x, a.y + sz.y);
    SoftShadow(dl, a, b, sz.y * 0.5f, u, 3 * u, P.shadow);
    dl->AddRectFilled(a, b, P.win, sz.y * 0.5f);
    dl->AddRect(a, b, P.line, sz.y * 0.5f, 0, std::max(1.0f, std::round(u)));
    dl->AddText(theme::Body(), fs, ImVec2(a.x + px, a.y + py), P.text, label);
    ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
}

#else

void Begin(Element) {}
void Cancel() {}
void Draw(float) {}
void Hover(float, bool) {}

#endif

}  // namespace sr2::ui::hud_edit
