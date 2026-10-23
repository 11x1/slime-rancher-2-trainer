#include "hud_edit.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>

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
    const char* module;
    const char* keyX;
    const char* keyY;
    const char* keySize;
    float minSize, maxSize;  // % of the screen height
};

constexpr Desc kDesc[] = {
    {"minimap", "minimap", "pos_x", "pos_y", "size", 10.0f, 50.0f},
    {"Speed Panel", "hl2_movement", "momentum_hud_x", "momentum_hud_y", "momentum_hud_size", 0.8f, 8.0f},
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

// Key help, top centre; at the bottom while the element is up there (`elemTop` = outline top).
void HintBar(ImDrawList* dl, float W, float H, float u, float elemTop) {
    const char* text =
        "Drag to move   \xC2\xB7   Corners resize (Alt: from the centre)   \xC2\xB7   Shift: equal gaps to the corner   "
        "\xC2\xB7   Enter or click outside: keep   \xC2\xB7   Esc: cancel";
    ImFont* font = ImGui::GetFont();
    const float fs = ImGui::GetFontSize();
    const ImVec2 ts = font->CalcTextSizeA(fs, 1e9f, 0.0f, text);
    const float px = 14 * u, py = 7 * u, margin = 18 * u;
    const bool bottom = elemTop < margin + ts.y + py * 2 + margin;
    const ImVec2 at(std::round((W - ts.x) * 0.5f), std::round(bottom ? H - margin - ts.y : margin));
    dl->AddRectFilled(ImVec2(at.x - px, at.y - py), ImVec2(at.x + ts.x + px, at.y + ts.y + py), IM_COL32(20, 12, 34, 210),
                      ts.y);
    dl->AddText(font, fs, at, IM_COL32(255, 248, 236, 245), text);
}

}  // namespace

void Begin(Element e) {
    if (Active()) return;
    const Desc& d = D(e);
    const auto all = settings::All();
    const settings::Def* dx = FindDef(all, d.module, d.keyX);
    const settings::Def* dy = FindDef(all, d.module, d.keyY);
    const settings::Def* ds = FindDef(all, d.module, d.keySize);
    if (!dx || !dy || !ds) {
        SR2_WARN("ui: edit mode: {} has no position settings registered", d.name);
        return;
    }
    g_st = State{};
    g_st.elem = e;
    g_st.orig = {static_cast<float>(Read(*dx)), static_cast<float>(Read(*dy)), static_cast<float>(Read(*ds))};
    g_st.cur = g_st.orig;
    SetPreview(true, e, g_st.cur);
    g_activeElem.store(static_cast<int>(e), std::memory_order_relaxed);
    g_active.store(true, std::memory_order_release);
    SR2_INFO("ui: edit mode for the {}", d.name);
}

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

    if (g_st.drag == Drag::None && mouseOk && ImGui::IsMouseClicked(ImGuiMouseButton_Left, false)) {
        const int c = cornerAt(m);
        if (c >= 0) {
            g_st.drag = Drag::Resize;
            g_st.corner = c;
            g_st.grab = ImVec2(m.x - CornerOf(tl, ImVec2(tl.x + box.x, tl.y + box.y), c).x,
                               m.y - CornerOf(tl, ImVec2(tl.x + box.x, tl.y + box.y), c).y);
        } else if (m.x >= omn.x && m.x <= omx.x && m.y >= omn.y && m.y <= omx.y) {
            g_st.drag = Drag::Move;
        } else {
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
    const int hot = g_st.drag == Drag::Resize ? g_st.corner : (mouseOk ? cornerAt(m) : -1);
    for (int c = 0; c < 4; ++c) {
        const ImVec2 k = CornerOf(omn, omx, c);
        const float h = handle * 0.5f;
        dl->AddRectFilled(ImVec2(k.x - h - 1, k.y - h - 1), ImVec2(k.x + h + 1, k.y + h + 1), ink);
        dl->AddRectFilled(ImVec2(k.x - h, k.y - h), ImVec2(k.x + h, k.y + h), c == hot ? accent : line);
    }
    HintBar(dl, W, H, u, omn.y);

    if (hot >= 0) ImGui::SetMouseCursor((hot == 0 || hot == 3) ? ImGuiMouseCursor_ResizeNWSE : ImGuiMouseCursor_ResizeNESW);
    else if (g_st.drag == Drag::Move || (mouseOk && m.x >= omn.x && m.x <= omx.x && m.y >= omn.y && m.y <= omx.y))
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
}

#else

void Begin(Element) {}
void Cancel() {}
void Draw(float) {}

#endif

}  // namespace sr2::ui::hud_edit
