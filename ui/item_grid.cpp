// Built only with Dear ImGui (third_party/imgui), like overlay.cpp.
#if defined(SR2_HAVE_IMGUI)

#include "item_grid.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "imgui_internal.h"
#include "item_icons.h"
#include "nav.h"
#include "theme.h"

namespace sr2::ui::item_grid {
namespace {

ImU32 Mix(ImU32 a, ImU32 b, float t) {
    const ImVec4 x = ImGui::ColorConvertU32ToFloat4(a), y = ImGui::ColorConvertU32ToFloat4(b);
    return ImGui::ColorConvertFloat4ToU32(ImVec4(x.x + (y.x - x.x) * t, x.y + (y.y - x.y) * t, x.z + (y.z - x.z) * t,
                                                 x.w + (y.w - x.w) * t));
}

ImU32 WithAlpha(ImU32 c, float a) {
    const unsigned ca = (c >> IM_COL32_A_SHIFT) & 0xFF;
    return (c & ~IM_COL32_A_MASK) | (static_cast<ImU32>(static_cast<float>(ca) * a + 0.5f) << IM_COL32_A_SHIFT);
}

bool Dark() { return theme::Current() == theme::Mode::Dark; }

// Tile background: deep and dark in the dark theme (an inventory look), a
// slightly shaded frame colour in the light one.
ImU32 TileBg(const theme::Palette& P) { return Dark() ? Mix(P.win, IM_COL32_BLACK, 0.32f) : Mix(P.frame, P.text, 0.05f); }

// Small number with a drop shadow, right-aligned at `right`, top at `top`.
void Number(ImDrawList* dl, const char* text, float right, float top, ImU32 col, float scale) {
    ImFont* f = theme::Body();
    const float size = theme::kSmallSize * scale * 0.85f;
    const ImVec2 ts = f->CalcTextSizeA(size, FLT_MAX, 0.0f, text);
    const ImVec2 p(right - ts.x, top);
    dl->AddText(f, size, ImVec2(p.x + 1, p.y + 1), IM_COL32(0, 0, 0, 160), text);
    dl->AddText(f, size, p, col, text);
}

// Tab content: the game's icon, or a drawn glyph.
void DrawTabFace(ImDrawList* dl, ImVec2 a, ImVec2 b, const Tab& t, bool on, float s) {
    const theme::Palette& P = theme::Colors();
    const float pad = 4.0f * s;
    const ImVec2 ia(a.x + pad, a.y + pad), ib(b.x - pad, b.y - pad);
    const ImU32 tint = on ? IM_COL32_WHITE : IM_COL32(255, 255, 255, 170);
    if (t.icon && item_icons::Draw(dl, t.icon, ia, ib, tint)) return;
    const ImU32 col = t.tint ? t.tint : (on ? P.text : P.soft);
    if (t.glyph == 0) {  // "all": four rounded squares
        const float w = ib.x - ia.x, g = w * 0.14f, q = (w - g) * 0.5f;
        for (int i = 0; i < 4; ++i) {
            const ImVec2 qa(ia.x + (i % 2) * (q + g), ia.y + (i / 2) * (q + g));
            dl->AddRectFilled(qa, ImVec2(qa.x + q, qa.y + q), WithAlpha(col, on ? 1.0f : 0.75f), 2.0f * s);
        }
        return;
    }
    if (t.glyph == kGlyphOther) {  // "other": three dots in a row
        const float w = ib.x - ia.x, r = w * 0.11f, cy = (ia.y + ib.y) * 0.5f;
        for (int i = 0; i < 3; ++i)
            dl->AddCircleFilled(ImVec2(ia.x + w * (0.18f + 0.32f * static_cast<float>(i)), cy), r,
                                WithAlpha(col, on ? 1.0f : 0.75f), 12);
        return;
    }
    if (t.glyph == kGlyphPin) {  // "pinned": a push pin, tilted 45 degrees, the needle to the bottom left
        const float w = ib.x - ia.x;
        const ImVec2 c((ia.x + ib.x) * 0.5f + w * 0.06f, (ia.y + ib.y) * 0.5f - w * 0.06f);
        constexpr float kCos = 0.70710678f;
        auto at = [&](float x, float y) {  // pin space (unit = w, y down) -> screen, rotated
            return ImVec2(c.x + (x * kCos - y * kCos) * w, c.y + (x * kCos + y * kCos) * w);
        };
        auto box = [&](float x0, float y0, float x1, float y1) {
            dl->AddQuadFilled(at(x0, y0), at(x1, y0), at(x1, y1), at(x0, y1), WithAlpha(col, on ? 1.0f : 0.75f));
        };
        box(-0.17f, -0.44f, 0.17f, -0.32f);  // cap
        box(-0.10f, -0.33f, 0.10f, -0.06f);  // body
        box(-0.27f, -0.07f, 0.27f, 0.03f);   // collar
        dl->AddLine(at(0.0f, 0.02f), at(0.0f, 0.40f), WithAlpha(col, on ? 1.0f : 0.75f), std::max(1.0f, w * 0.07f));
        return;
    }
    char buf[2] = {static_cast<char>(t.glyph), 0};
    ImFont* f = theme::Display();
    const float size = (ib.y - ia.y) * 0.9f;
    const ImVec2 ts = f->CalcTextSizeA(size, FLT_MAX, 0.0f, buf);
    dl->AddText(f, size, ImVec2((a.x + b.x - ts.x) * 0.5f, (a.y + b.y - ts.y) * 0.5f), col, buf);
}

// The tabs in a row from the cursor. They switch on the press (PredictTab matches that) and their
// rectangles are kept for next frame's PredictTab.
void TabRow(const std::vector<Tab>& tabs, State& st, Result& res, float s) {
    const theme::Palette& P = theme::Colors();
    const float fh = ImGui::GetFrameHeight(), sp = 4.0f * s;
    const int n = static_cast<int>(tabs.size());
    st.tab = std::clamp(st.tab, 0, n - 1);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    st.tabCount = std::min(n, kMaxTabs);
    st.tabFrame = ImGui::GetFrameCount();
    st.tabWindow = ImGui::GetCurrentWindow()->ID;
    for (int i = 0; i < n; ++i) {
        ImGui::PushID(i);
        if (i > 0) ImGui::SameLine(0, sp);
        if (nav::Button("##tab", ImVec2(fh, fh), ImGuiButtonFlags_PressedOnClick) && st.tab != i)
            st.tab = i, res.tabChanged = true;
        const bool hot = ImGui::IsItemHovered(), on = st.tab == i;
        const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
        if (i < kMaxTabs) st.tabMin[i] = a, st.tabMax[i] = b;
        dl->AddRectFilled(a, b, on ? P.sel : hot ? P.frameHover : P.frame, 6.0f * s);
        if (on) dl->AddRect(a, b, Dark() ? P.grab : P.accent, 6.0f * s, 0, std::max(1.0f, 1.5f * s));
        DrawTabFace(dl, a, b, tabs[static_cast<size_t>(i)], on || hot, s);
        nav::Ring(6.0f * s);
        if (hot) res.hoveredTab = i;
        ImGui::PopID();
    }
    if (st.predicted) res.tabChanged = true;
    st.predicted = false;
}

// Options::tabsBelow: the tabs on a row of their own under the search box, right-aligned.
void TabRowBelow(const std::vector<Tab>& tabs, State& st, Result& res, float s) {
    const float fh = ImGui::GetFrameHeight(), sp = 4.0f * s;
    const int n = static_cast<int>(tabs.size());
    const float w = static_cast<float>(n) * fh + static_cast<float>(n - 1) * sp;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, ImGui::GetContentRegionAvail().x - w));
    TabRow(tabs, st, res, s);
}

// The accent bar along a tile's bottom edge, `h` high, inside the tile's rounded outline (radius `r`):
// a plain rectangle with a smaller rounding stuck out past the round corners and the selection outline.
void AccentBar(ImDrawList* dl, ImVec2 a, ImVec2 b, float r, float h, ImU32 col) {
    const float in = 1.0f;  // stays inside the outline's anti-aliased edge
    a.x += in, b.x -= in, b.y -= in, r = std::max(0.0f, r - in);
    const float top = b.y - h;
    if (r <= 0.5f || b.x - a.x < 2 * r) {
        dl->AddRectFilled(ImVec2(a.x, top), b, col);
        return;
    }
    // Left corner arc, bottom edge, right corner arc, clipped to y >= top (convex).
    constexpr int kSeg = 8;
    constexpr float kHalfPi = 1.57079633f;
    ImVec2 pts[2 * (kSeg + 1) + 2];
    int n = 0;
    const float cyc = b.y - r;  // the corner circles' centre height
    auto arcX = [&](float y) {  // horizontal distance of the arc from its centre at height y
        const float d = std::clamp(y - cyc, 0.0f, r);
        return std::sqrt(std::max(0.0f, r * r - d * d));
    };
    pts[n++] = ImVec2(a.x + r - arcX(top), top);
    for (int i = 0; i <= kSeg; ++i) {  // left corner: from the top of the bar down to the bottom
        const float an = kHalfPi * static_cast<float>(i) / kSeg;  // 0 = side, pi/2 = bottom
        const ImVec2 p(a.x + r - std::cos(an) * r, cyc + std::sin(an) * r);
        if (p.y > top) pts[n++] = p;
    }
    for (int i = kSeg; i >= 0; --i) {  // right corner: from the bottom up
        const float an = kHalfPi * static_cast<float>(i) / kSeg;
        const ImVec2 p(b.x - r + std::cos(an) * r, cyc + std::sin(an) * r);
        if (p.y > top) pts[n++] = p;
    }
    pts[n++] = ImVec2(b.x - r + arcX(top), top);
    std::reverse(pts, pts + n);  // clockwise on screen, as the anti-aliased fill wants it
    dl->AddConvexPolyFilled(pts, n, col);
}

}  // namespace

bool PredictTab(State& st) {
    st.predicted = false;
    if (st.tabCount <= 0 || st.tabFrame != ImGui::GetFrameCount() - 1 || !ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        return false;
    const ImGuiContext& g = *ImGui::GetCurrentContext();
    if (!g.HoveredWindow || g.HoveredWindow->ID != st.tabWindow) return false;
    const ImVec2 m = ImGui::GetMousePos();
    for (int i = 0; i < st.tabCount; ++i) {
        if (m.x < st.tabMin[i].x || m.y < st.tabMin[i].y || m.x >= st.tabMax[i].x || m.y >= st.tabMax[i].y) continue;
        if (st.tab == i) return false;
        st.tab = i;
        st.predicted = true;
        return true;
    }
    return false;
}

int WheelStep(float wheel) {
    const ImGuiIO& io = ImGui::GetIO();
    if (wheel == 0.0f || (!io.KeyCtrl && !io.KeyShift)) return 0;
    const int step = io.KeyCtrl ? 10 : 1;
    return wheel > 0 ? step : -step;
}

bool Matches(const std::string& hay, const char* needle) {
    auto low = [](char c) { return static_cast<char>((c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c); };
    const size_t n = needle ? std::strlen(needle) : 0;
    if (n == 0) return true;
    for (size_t i = 0; i + n <= hay.size(); ++i) {
        size_t k = 0;
        while (k < n && low(hay[i + k]) == low(needle[k])) ++k;
        if (k == n) return true;
    }
    return false;
}

bool DrawIcons(ImDrawList* dl, uint32_t icon, uint32_t icon2, ImVec2 a, ImVec2 b, ImU32 tint) {
    if (!icon2) return icon && item_icons::Draw(dl, icon, a, b, tint);
    // Composed: the first icon top left, the second bottom right, each 70 % of the box.
    const float w = (b.x - a.x) * 0.70f, h = (b.y - a.y) * 0.70f;
    bool any = icon && item_icons::Draw(dl, icon, a, ImVec2(a.x + w, a.y + h), tint);
    any = item_icons::Draw(dl, icon2, ImVec2(b.x - w, b.y - h), b, tint) || any;
    return any;
}

void DrawLock(ImDrawList* dl, ImVec2 c, float h, bool open, ImU32 col) {
    const float bw = h * 0.62f, bh = h * 0.46f;              // body
    const ImVec2 ba(c.x - bw * 0.5f, c.y + h * 0.5f - bh), bb(c.x + bw * 0.5f, c.y + h * 0.5f);
    const float r = bw * 0.30f, th = std::max(1.5f, h * 0.09f);  // shackle radius, stroke
    const float lift = open ? h * 0.16f : 0.0f;
    const float sx = open ? c.x - bw * 0.22f : c.x;            // open: shackle shifted left and up
    const float top = ba.y - h * 0.10f - lift;
    constexpr float kPi = 3.14159265358979f;
    dl->PathArcTo(ImVec2(sx, top), r, kPi, 2.0f * kPi, 12);
    dl->PathStroke(col, 0, th);
    dl->AddLine(ImVec2(sx - r, top), ImVec2(sx - r, ba.y + (open ? 0.0f : 1.0f)), col, th);
    if (!open) dl->AddLine(ImVec2(sx + r, top), ImVec2(sx + r, ba.y + 1.0f), col, th);
    else dl->AddLine(ImVec2(sx + r, top), ImVec2(sx + r, top + h * 0.10f), col, th);
    dl->AddRectFilled(ba, bb, col, h * 0.08f);
    const ImU32 hole = IM_COL32(0, 0, 0, 120);
    dl->AddCircleFilled(ImVec2(c.x, ba.y + bh * 0.42f), bh * 0.13f, hole);
    dl->AddRectFilled(ImVec2(c.x - bh * 0.05f, ba.y + bh * 0.42f), ImVec2(c.x + bh * 0.05f, ba.y + bh * 0.72f), hole);
}

void DrawTile(ImDrawList* dl, ImVec2 a, ImVec2 b, const Tile& t, bool hot, bool selected, float scale) {
    const theme::Palette& P = theme::Colors();
    const float r = 6.0f * scale;
    const ImU32 bg = TileBg(P);
    const ImU32 top = Mix(bg, IM_COL32_WHITE, hot ? 0.10f : 0.04f);
    // Vertical gradient (lighter at the top), rounded by drawing the flat colour first.
    dl->AddRectFilled(a, b, hot ? Mix(bg, IM_COL32_WHITE, 0.05f) : bg, r);
    const float inset = r * 0.5f;
    dl->AddRectFilledMultiColor(ImVec2(a.x + inset, a.y + inset), ImVec2(b.x - inset, (a.y + b.y) * 0.5f), top, top,
                                WithAlpha(top, 0.0f), WithAlpha(top, 0.0f));
    if (t.related) dl->AddRectFilled(a, b, WithAlpha(Dark() ? P.grab : P.accent, 0.16f), r);
    if (t.accent) {
        // Glow rising from the bottom edge, then the accent bar itself.
        const float gy = a.y + (b.y - a.y) * 0.45f;
        const ImU32 glow = WithAlpha(t.accent, t.dimmed ? 0.06f : (hot ? 0.30f : 0.18f));
        dl->AddRectFilledMultiColor(ImVec2(a.x + inset, gy), ImVec2(b.x - inset, b.y - 3.0f * scale), WithAlpha(glow, 0.0f),
                                    WithAlpha(glow, 0.0f), glow, glow);
        AccentBar(dl, a, b, r, 3.0f * scale, t.dimmed ? WithAlpha(t.accent, 0.35f) : t.accent);
    }
    const float pad = 7.0f * scale;
    const ImVec2 ia(a.x + pad, a.y + pad), ib(b.x - pad, b.y - pad - 2.0f * scale);
    const ImU32 tint = t.dimmed ? IM_COL32(255, 255, 255, 90) : IM_COL32_WHITE;
    if (!DrawIcons(dl, t.icon, t.icon2, ia, ib, tint)) {
        // No icon: the name, wrapped and centred.
        if (t.name && !t.name->empty()) {
            ImFont* f = theme::Body();
            const float size = theme::kSmallSize * scale * 0.85f;
            const float wrap = ib.x - ia.x;
            const ImVec2 ts = f->CalcTextSizeA(size, FLT_MAX, wrap, t.name->c_str());
            dl->PushClipRect(ia, ib, true);
            dl->AddText(f, size, ImVec2(ia.x + std::max(0.0f, (wrap - ts.x) * 0.5f), std::max(ia.y, (ia.y + ib.y - ts.y) * 0.5f)),
                        t.dimmed ? P.dim : P.soft, t.name->c_str(), nullptr, wrap);
            dl->PopClipRect();
        }
    }
    char buf[16];
    if (t.mod) {
        // Added by the mod: an "M" on a small pill in the theme's accent colour, top left (the owned count is
        // top right). Ink as on the amount pill, so it reads in both themes.
        ImFont* f = theme::Body();
        const float size = theme::kSmallSize * scale * 0.72f;
        const ImVec2 ts = f->CalcTextSizeA(size, FLT_MAX, 0.0f, "M");
        const ImVec2 pa(a.x + 3.0f * scale, a.y + 3.0f * scale), pb(pa.x + ts.x + 6.0f * scale, pa.y + ts.y + 1.0f * scale);
        const ImU32 pill = Dark() ? P.grab : P.accent;
        dl->AddRectFilled(pa, pb, t.dimmed ? WithAlpha(pill, 0.6f) : pill, 3.0f * scale);
        dl->AddText(f, size, ImVec2(pa.x + 3.0f * scale, pa.y + 0.5f * scale), Dark() ? IM_COL32(20, 20, 24, 255) : P.accentInk, "M");
    }
    if (t.badge >= 0) {
        std::snprintf(buf, sizeof(buf), "%d", t.badge);
        Number(dl, buf, b.x - 5.0f * scale, a.y + 3.0f * scale, t.badge > 0 ? P.text : P.dim, scale);
    }
    if (t.amount > 0) {
        // The chosen amount: a small pill in the bottom right corner.
        std::snprintf(buf, sizeof(buf), "x%d", t.amount);
        ImFont* f = theme::Body();
        const float size = theme::kSmallSize * scale * 0.85f;
        const ImVec2 ts = f->CalcTextSizeA(size, FLT_MAX, 0.0f, buf);
        const ImVec2 pb(b.x - 3.0f * scale, b.y - 5.0f * scale), pa(pb.x - ts.x - 8.0f * scale, pb.y - ts.y - 2.0f * scale);
        const ImU32 pill = Dark() ? P.grab : P.accent;
        dl->AddRectFilled(pa, pb, pill, (pb.y - pa.y) * 0.5f);
        dl->AddText(f, size, ImVec2(pa.x + 4.0f * scale, pa.y + 1.0f * scale), Dark() ? IM_COL32(20, 20, 24, 255) : P.accentInk,
                    buf);
    }
    if (selected || t.selected) dl->AddRect(a, b, Dark() ? P.grab : P.accent, r, 0, 2.0f * scale);
    else if (hot) dl->AddRect(a, b, WithAlpha(Dark() ? P.tabSel : P.accent, 0.55f), r, 0, std::max(1.0f, scale));
    else if (t.related) dl->AddRect(a, b, WithAlpha(Dark() ? P.grab : P.accent, 0.45f), r, 0, std::max(1.0f, scale));
}

bool SearchBox(const char* id, const char* hint, char* buf, size_t size, float width, float scale, bool* active) {
    const theme::Palette& P = theme::Colors();
    ImGui::PushID(id);
    ImGuiStorage* st = ImGui::GetStateStorage();
    const ImGuiID refocus = ImGui::GetID("##refocus");
    const float w = width > 0 ? width : ImGui::GetContentRegionAvail().x, h = ImGui::GetFrameHeight();
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    // The x goes in before the box: the first of two overlapping items keeps the hover,
    // so a click on it is the x's, not the box's. It is drawn after the box (on its frame).
    const bool hasText = buf[0] != 0;
    const float bw = std::min(h, 24.0f * scale);
    const ImVec2 xa(pos.x + w - bw - 2.0f * scale, pos.y), xb(pos.x + w - 2.0f * scale, pos.y + h);
    bool clicked = false, hov = false;
    if (hasText) {
        ImGui::SetCursorScreenPos(xa);
        ImGui::PushItemFlag(ImGuiItemFlags_NoTabStop | ImGuiItemFlags_NoNav, true);  // SetKeyboardFocusHere skips it
        clicked = ImGui::InvisibleButton("##clear", ImVec2(xb.x - xa.x, h));
        ImGui::PopItemFlag();
        hov = ImGui::IsItemHovered();
        if (hov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        ImGui::SetCursorScreenPos(pos);
    }
    bool changed = false;
    if (clicked) {
        buf[0] = 0;
        changed = true;
        st->SetBool(refocus, true);
    } else if (st->GetBool(refocus)) {  // cleared last frame: the box takes the keyboard back
        st->SetBool(refocus, false);
        ImGui::SetKeyboardFocusHere();
    }
    ImGui::SetNextItemWidth(w);
    changed |= ImGui::InputTextWithHint("##box", hint, buf, size);
    const bool isActive = ImGui::IsItemActive();
    if (hasText) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 c((xa.x + xb.x) * 0.5f, (xa.y + xb.y) * 0.5f);
        if (hov) dl->AddCircleFilled(c, bw * 0.36f, ImGui::GetColorU32(WithAlpha(P.text, 0.12f)));
        const float r = 3.5f * scale, th = std::max(1.0f, 1.6f * scale);
        const ImU32 col = ImGui::GetColorU32(hov ? P.text : P.dim);
        dl->AddLine(ImVec2(c.x - r, c.y - r), ImVec2(c.x + r, c.y + r), col, th);
        dl->AddLine(ImVec2(c.x - r, c.y + r), ImVec2(c.x + r, c.y - r), col, th);
    }
    ImGui::PopID();
    if (active) *active = isActive;
    return changed;
}

Result Draw(const char* id, const std::vector<Tile>& tiles, State& st, const Options& o) {
    const theme::Palette& P = theme::Colors();
    const float s = o.scale;
    Result res;
    ImGui::PushID(id);

    // ---- header row: tabs, search, button ----
    const float sp = 4.0f * s;
    if (!o.tabs || o.tabs->empty()) st.tabCount = 0;
    if (o.tabs && !o.tabs->empty() && !o.tabsBelow) {
        TabRow(*o.tabs, st, res, s);
        ImGui::SameLine(0, 8.0f * s);
    }
    float bw = 0;
    if (o.button) bw = ImGui::CalcTextSize(o.button).x + ImGui::GetStyle().FramePadding.x * 2 + 8.0f * s;
    SearchBox("##search", o.hint, st.search, sizeof(st.search),
              o.button ? std::max(40.0f * s, ImGui::GetContentRegionAvail().x - bw - sp) : 0.0f, s);
    if (o.button) {
        ImGui::SameLine(0, sp);
        ImGui::BeginDisabled(!o.buttonEnabled);
        ImGui::PushStyleColor(ImGuiCol_Button, P.accent);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Mix(P.accent, IM_COL32_WHITE, 0.14f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, Mix(P.accent, IM_COL32_BLACK, 0.12f));
        ImGui::PushStyleColor(ImGuiCol_Text, P.accentInk);
        res.button = ImGui::Button(o.button, ImVec2(bw, 0));
        ImGui::PopStyleColor(4);
        ImGui::EndDisabled();
        res.buttonHovered = ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort | ImGuiHoveredFlags_AllowWhenDisabled);
    }
    if (o.tabs && !o.tabs->empty() && o.tabsBelow) TabRowBelow(*o.tabs, st, res, s);

    std::vector<int> shown;
    shown.reserve(tiles.size());
    for (int i = 0; i < static_cast<int>(tiles.size()); ++i)
        if (!st.search[0] || (tiles[i].name && Matches(*tiles[i].name, st.search))) shown.push_back(i);
    res.shown = static_cast<int>(shown.size());

    ImGui::PushStyleColor(ImGuiCol_ChildBg, Dark() ? Mix(P.card, IM_COL32_BLACK, 0.18f) : P.frame);
    // Each tile sits in a padded cell: the cell (tile plus half the gap on every side) is the item, the
    // hover and the tooltip area, and the cells touch, so moving across the grid never leaves a tile
    // (round 12: the tooltip flickered over the gaps). The window padding is short by that half gap, so
    // the tiles stay where a 6 px padding and 6 px gaps put them.
    const float gap = 6.0f * s, half = gap * 0.5f;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6.0f * s - half, 6.0f * s - half));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 8.0f * s);
    // The tiles are one Tab stop (the tile focused last, else the first); the arrows move tile by tile.
    // Flattened, so Tab and Shift+Tab leave the grid for the items around it (nav.h).
    nav::TabGroup tileNav("##tilenav");
    ImGui::BeginChild("##grid", ImVec2(0, std::floor(o.height * s)),
                      ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_NavFlattened);
    const float avail = ImGui::GetContentRegionAvail().x;
    const int cols = std::max(1, static_cast<int>(avail / (o.tile * s + gap)));
    const float cell = std::max(8.0f * s + gap, std::floor(avail / static_cast<float>(cols)));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (res.shown == 0) {
        ImGui::SetCursorPos(ImVec2(ImGui::GetCursorPosX() + half, ImGui::GetCursorPosY() + half));
        ImGui::PushStyleColor(ImGuiCol_Text, P.dim);
        ImGui::TextUnformatted(o.empty);
        ImGui::PopStyleColor();
    }
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
    int col = 0, section = -1;
    const ImGuiIO& io = ImGui::GetIO();
    for (int k = 0; k < res.shown; ++k) {
        const int i = shown[static_cast<size_t>(k)];
        const Tile& t = tiles[static_cast<size_t>(i)];
        if (o.sections && t.section != section) {
            // Section break: a heading (if any) and a rule across the grid. One spacer item from the cell
            // row above to the cell row below (cells carry half a gap on each side).
            if (section >= 0 || (t.section >= 0 && static_cast<size_t>(t.section) < o.sections->size() &&
                                 !(*o.sections)[static_cast<size_t>(t.section)].empty())) {
                const char* title = (t.section >= 0 && static_cast<size_t>(t.section) < o.sections->size())
                                        ? (*o.sections)[static_cast<size_t>(t.section)].c_str()
                                        : "";
                const float pre = section >= 0 ? half + gap + 1.0f * s : half;  // above the heading
                const ImVec2 p0 = ImGui::GetCursorScreenPos();
                const ImVec2 p(p0.x + half, p0.y + pre);
                const float right = p0.x + avail - half;
                if (*title) {
                    ImFont* f = theme::Body();
                    const float size = theme::kSmallSize * s * 0.9f;
                    const ImVec2 ts = f->CalcTextSizeA(size, FLT_MAX, 0.0f, title);
                    dl->AddText(f, size, p, P.soft, title);
                    const float x = p.x + ts.x + 8.0f * s;
                    ImGui::Dummy(ImVec2(avail, pre + ts.y + half));
                    dl->AddLine(ImVec2(x, p.y + ts.y * 0.55f), ImVec2(right, p.y + ts.y * 0.55f), WithAlpha(P.soft, 0.35f),
                                std::max(1.0f, s));
                } else {
                    ImGui::Dummy(ImVec2(avail, pre + 2.0f * s + half));
                    dl->AddLine(ImVec2(p.x, p.y + 1.0f * s), ImVec2(right, p.y + 1.0f * s), WithAlpha(P.soft, 0.35f),
                                std::max(1.0f, s));
                }
            }
            section = t.section;
            col = 0;
        }
        if (col > 0) ImGui::SameLine(0, 0);
        ImGui::PushID(reinterpret_cast<const void*>(t.id));
        tileNav.Item(t.id);
        if (nav::Button("##tile", ImVec2(cell, cell), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight)) {
            if (ImGui::IsMouseReleased(ImGuiMouseButton_Right)) res.rightClicked = i, res.menuAt = ImGui::GetMousePos();
            else res.clicked = i;  // also Space / Enter on the focused tile
        }
        tileNav.Done();
        if (nav::ContextKey()) res.rightClicked = i, res.menuAt = ImVec2(ImGui::GetItemRectMin().x, ImGui::GetItemRectMax().y);
        if (res.rightClicked == i) {
            // Kept inside the visible part of the grid (a tile scrolled half out of it).
            const ImRect clip = ImGui::GetCurrentWindow()->InnerClipRect;
            res.menuAt = ImVec2(std::clamp(res.menuAt.x, clip.Min.x, clip.Max.x), std::clamp(res.menuAt.y, clip.Min.y, clip.Max.y));
        }
        // Hover also while another item (the search box being typed in) holds the
        // active id, so the tooltip and the wheel work without clicking the grid first.
        const bool hot = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
        if (hot) {
            res.hovered = i;
            if (io.KeyShift || io.KeyCtrl) {
                // Shift / Ctrl + wheel picks an amount: the grid does not scroll then (a plain wheel does).
                ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY);
                ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelX);
                const float w = io.MouseWheel != 0.0f ? io.MouseWheel : io.MouseWheelH;
                if (w != 0.0f) {
                    res.wheel = w;
                    res.wheelStep = WheelStep(w);
                }
            }
        }
        // The tile inside its cell.
        const ImVec2 ca = ImGui::GetItemRectMin(), cb = ImGui::GetItemRectMax();
        const ImVec2 a(ca.x + half, ca.y + half), b(cb.x - half, cb.y - half);
        if (ImGui::IsRectVisible(a, b))
            DrawTile(dl, a, b, t, hot || ImGui::IsItemActive(), o.selected != 0 && t.id == o.selected, s);
        nav::Ring(a, b, 6.0f * s);
        if (o.drag && !t.noDrag && ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceNoPreviewTooltip)) {
            ImGui::SetDragDropPayload(kDragType, &t.id, sizeof(t.id));
            ImGui::EndDragDropSource();
        }
        ImGui::PopID();
        col = (col + 1) % cols;
    }
    ImGui::PopStyleVar();
    // Right-click on the grid but not on a tile: the caller clears its selection.
    if (res.hovered < 0 && ImGui::IsWindowHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) res.clearSelection = true;
    const ImGuiPayload* drag = o.dropType ? ImGui::GetDragDropPayload() : nullptr;
    if (drag && drag->IsDataType(o.dropType)) {
        // Something the grid accepts is being dragged: a tint and the hint over the visible area.
        const ImVec2 wa = ImGui::GetWindowPos(), wb(wa.x + ImGui::GetWindowWidth(), wa.y + ImGui::GetWindowHeight());
        dl->AddRectFilled(wa, wb, WithAlpha(Dark() ? P.grab : P.accent, 0.10f), 8.0f * s);
        if (o.dropHint && *o.dropHint) {
            ImFont* f = theme::Body();
            const float size = theme::kSmallSize * s;
            const ImVec2 ts = f->CalcTextSizeA(size, FLT_MAX, 0.0f, o.dropHint);
            const ImVec2 c((wa.x + wb.x) * 0.5f, (wa.y + wb.y) * 0.5f);
            const ImVec2 pa(c.x - ts.x * 0.5f - 10.0f * s, c.y - ts.y * 0.5f - 5.0f * s), pb(c.x + ts.x * 0.5f + 10.0f * s, c.y + ts.y * 0.5f + 5.0f * s);
            dl->AddRectFilled(pa, pb, WithAlpha(P.win, 0.92f), (pb.y - pa.y) * 0.5f);
            dl->AddText(f, size, ImVec2(c.x - ts.x * 0.5f, c.y - ts.y * 0.5f), P.text, o.dropHint);
        }
    }
    ImGui::EndChild();
    tileNav.End();
    if (o.dropType && ImGui::BeginDragDropTarget()) {  // the child window is the last item
        if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(o.dropType)) res.dropped = p;
        ImGui::EndDragDropTarget();
    }
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();

    // While a tile is dragged: its icon follows the cursor (drawn on the foreground
    // so it is never clipped by the grid).
    if (o.drag) {
        if (const ImGuiPayload* p = ImGui::GetDragDropPayload(); p && p->IsDataType(kDragType) && p->DataSize == sizeof(uintptr_t)) {
            const uintptr_t dragged = *static_cast<const uintptr_t*>(p->Data);
            for (const Tile& t : tiles) {
                if (t.id != dragged) continue;
                const ImVec2 m = ImGui::GetMousePos();
                const float e = o.tile * s;
                const ImVec2 a(m.x + 10.0f * s, m.y + 10.0f * s), b(a.x + e, a.y + e);
                ImDrawList* fg = ImGui::GetForegroundDrawList();
                Tile ghost = t;
                ghost.badge = -1;
                ghost.amount = 0;
                ghost.selected = ghost.related = false;
                DrawTile(fg, a, b, ghost, true, false, s);
                break;
            }
        }
    }
    ImGui::PopID();
    return res;
}

}  // namespace sr2::ui::item_grid

#endif  // SR2_HAVE_IMGUI
