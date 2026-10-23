// Built only with Dear ImGui (third_party/imgui), like overlay.cpp.
#if defined(SR2_HAVE_IMGUI)

#include "item_grid.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "item_icons.h"
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

}  // namespace

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
    if (t.accent) {
        // Glow rising from the bottom edge, then the accent bar itself.
        const float gy = a.y + (b.y - a.y) * 0.45f;
        const ImU32 glow = WithAlpha(t.accent, t.dimmed ? 0.06f : (hot ? 0.30f : 0.18f));
        dl->AddRectFilledMultiColor(ImVec2(a.x + inset, gy), ImVec2(b.x - inset, b.y - 3.0f * scale), WithAlpha(glow, 0.0f),
                                    WithAlpha(glow, 0.0f), glow, glow);
        dl->AddRectFilled(ImVec2(a.x, b.y - 3.0f * scale), b, t.dimmed ? WithAlpha(t.accent, 0.35f) : t.accent, r,
                          ImDrawFlags_RoundCornersBottom);
    }
    const float pad = 7.0f * scale;
    const ImVec2 ia(a.x + pad, a.y + pad), ib(b.x - pad, b.y - pad - 2.0f * scale);
    const ImU32 tint = t.dimmed ? IM_COL32(255, 255, 255, 90) : IM_COL32_WHITE;
    if (!t.icon || !item_icons::Draw(dl, t.icon, ia, ib, tint)) {
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
    if (t.badge >= 0) {
        ImFont* f = theme::Body();
        const float size = theme::kSmallSize * scale * 0.85f;
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%d", t.badge);
        const ImVec2 ts = f->CalcTextSizeA(size, FLT_MAX, 0.0f, buf);
        const ImVec2 p(b.x - ts.x - 5.0f * scale, a.y + 3.0f * scale);
        dl->AddText(f, size, ImVec2(p.x + 1, p.y + 1), IM_COL32(0, 0, 0, 140), buf);
        dl->AddText(f, size, p, t.badge > 0 ? P.text : P.dim, buf);
    }
    if (selected) dl->AddRect(a, b, Dark() ? P.grab : P.accent, r, 0, 2.0f * scale);
    else if (hot) dl->AddRect(a, b, WithAlpha(Dark() ? P.tabSel : P.accent, 0.55f), r, 0, std::max(1.0f, scale));
}

Result Draw(const char* id, const std::vector<Tile>& tiles, State& st, const Options& o) {
    const theme::Palette& P = theme::Colors();
    const float s = o.scale;
    Result res;
    ImGui::PushID(id);
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##search", o.hint, st.search, sizeof(st.search));

    std::vector<int> shown;
    shown.reserve(tiles.size());
    for (int i = 0; i < static_cast<int>(tiles.size()); ++i)
        if (!st.search[0] || (tiles[i].name && Matches(*tiles[i].name, st.search))) shown.push_back(i);
    res.shown = static_cast<int>(shown.size());

    ImGui::PushStyleColor(ImGuiCol_ChildBg, Dark() ? Mix(P.card, IM_COL32_BLACK, 0.18f) : P.frame);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6.0f * s, 6.0f * s));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 8.0f * s);
    ImGui::BeginChild("##grid", ImVec2(0, std::floor(o.height * s)), ImGuiChildFlags_AlwaysUseWindowPadding);
    const float gap = 6.0f * s;
    const float avail = ImGui::GetContentRegionAvail().x;
    const int cols = std::max(1, static_cast<int>((avail + gap) / (o.tile * s + gap)));
    const float tw = std::max(8.0f * s, std::floor((avail - gap * static_cast<float>(cols - 1)) / static_cast<float>(cols)));
    const int rows = (res.shown + cols - 1) / cols;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (res.shown == 0) {
        ImGui::PushStyleColor(ImGuiCol_Text, P.dim);
        ImGui::TextUnformatted(o.empty);
        ImGui::PopStyleColor();
    }
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(gap, gap));
    ImGuiListClipper clip;
    clip.Begin(rows, tw + gap);
    while (clip.Step()) {
        for (int row = clip.DisplayStart; row < clip.DisplayEnd; ++row) {
            for (int c = 0; c < cols; ++c) {
                const int k = row * cols + c;
                if (k >= res.shown) break;
                const int i = shown[static_cast<size_t>(k)];
                const Tile& t = tiles[static_cast<size_t>(i)];
                if (c > 0) ImGui::SameLine(0, gap);
                ImGui::PushID(reinterpret_cast<const void*>(t.id));
                if (ImGui::InvisibleButton("##tile", ImVec2(tw, tw))) res.clicked = i;
                const bool hot = ImGui::IsItemHovered();
                if (hot) res.hovered = i;
                DrawTile(dl, ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), t, hot || ImGui::IsItemActive(),
                         o.selected != 0 && t.id == o.selected, s);
                if (o.drag && ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceNoPreviewTooltip)) {
                    ImGui::SetDragDropPayload(kDragType, &t.id, sizeof(t.id));
                    ImGui::EndDragDropSource();
                }
                ImGui::PopID();
            }
        }
    }
    clip.End();
    ImGui::PopStyleVar();
    ImGui::EndChild();
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
