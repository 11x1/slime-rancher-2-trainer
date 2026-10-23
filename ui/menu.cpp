// Built only with Dear ImGui (third_party/imgui), like overlay.cpp.
#if defined(SR2_HAVE_IMGUI)

#include "menu.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <format>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "core/build_info.h"
#include "core/profiles.h"
#include "hud_edit.h"
#include "menu_layout.h"
#include "item_grid.h"
#include "sandbox_bridge.h"
#include "slime_icons.h"
#include "theme.h"

namespace sr2::ui::menu {
namespace {

namespace profiles = ::sr2::core::profiles;
using layout::Category;
using layout::Tab;
using layout::TabKind;
using theme::Palette;

// Sizes at 1080p; U() applies the scale.
constexpr float kWinW = 900, kWinH = 600, kSideW = 200, kTopH = 64, kNavH = 42, kCtlW = 176, kGap = 12;
constexpr float kSwitchW = 42, kSwitchH = 24, kPeelSeconds = 0.85f;
constexpr float kPi = 3.14159265358979f;

float g_scale = 1.0f;
float U(float v) { return v * g_scale; }

// ---------------------------------------------------------------- state ----
std::string g_cat = "ranch";
std::map<std::string, int> g_tab;  // selected tab per category
std::string g_lastPage;            // category/tab drawn last frame ("" after the overlay opens)
std::map<std::string, float> g_pageScroll;  // scroll position per page, kept for the session
int g_scrollRestore = 0;           // frames left to re-apply a page's saved scroll (clamped while content grows)
ImVec2 g_winMin, g_winMax;         // window rect of the last frame

struct Peel {
    int state = 0;  // 0 idle, 1 waiting for the snapshot, 2 running
    double start = 0;
    ImVec2 min, max;
    ImTextureID tex = ImTextureID_Invalid;
} g_peel;

// Profiles page.
std::string g_profSel;
char g_profName[64] = {};
char g_profDesc[256] = {};
std::string g_profPopupMsg;

// -------------------------------------------------------------- helpers ----
ImU32 WithAlpha(ImU32 c, float a) {
    const unsigned ca = (c >> IM_COL32_A_SHIFT) & 0xFF;
    return (c & ~IM_COL32_A_MASK) | (static_cast<ImU32>(static_cast<float>(ca) * a + 0.5f) << IM_COL32_A_SHIFT);
}

ImU32 Mix(ImU32 a, ImU32 b, float t) {
    const ImVec4 x = ImGui::ColorConvertU32ToFloat4(a), y = ImGui::ColorConvertU32ToFloat4(b);
    return ImGui::ColorConvertFloat4ToU32(ImVec4(x.x + (y.x - x.x) * t, x.y + (y.y - x.y) * t, x.z + (y.z - x.z) * t,
                                                 x.w + (y.w - x.w) * t));
}

float Approach(float cur, float target, float perSecond) {
    const float step = ImGui::GetIO().DeltaTime * perSecond;
    return cur < target ? std::min(target, cur + step) : std::max(target, cur - step);
}

float Smooth(float t) { return t * t * (3.0f - 2.0f * t); }

ImVec2 Add(ImVec2 a, ImVec2 b) { return ImVec2(a.x + b.x, a.y + b.y); }

ImVec2 TextSize(ImFont* f, float size, const char* text, float wrap = 0.0f) {
    return f->CalcTextSizeA(U(size), FLT_MAX, wrap, text);
}

void Text(ImDrawList* dl, ImFont* f, float size, ImVec2 pos, ImU32 col, const char* text, float wrap = 0.0f) {
    dl->AddText(f, U(size), pos, col, text, nullptr, wrap);
}

struct FontScope {
    FontScope(ImFont* f, float size) { ImGui::PushFont(f, size); }
    ~FontScope() { ImGui::PopFont(); }
};

// Soft drop shadow below a rounded rect, nothing above it. ImGui has no blur, so
// it is stacked faint rounded layers, widest first: the overlap darkens towards
// the rect and the edge fades out over `spread` below it (dy + spread at most).
// The layers start inside the rect, so its top edge never shows a shadow.
void SoftShadow(ImDrawList* dl, ImVec2 a, ImVec2 b, float rounding, float dy, float spread, ImU32 col) {
    constexpr int kLayers = 10;
    const ImU32 layer = WithAlpha(col, 0.18f);
    const float top = a.y + rounding + spread;  // hidden behind the rect
    for (int i = 0; i < kLayers; ++i) {
        const float e = spread * (1.0f - static_cast<float>(i) / kLayers);
        const float ex = e - spread;  // never wider than the rect: the shadow only shows below it
        dl->AddRectFilled(ImVec2(a.x - ex, top), ImVec2(b.x + ex, b.y + dy + e), layer, std::max(0.0f, rounding + ex));
    }
}

// Background, optional border and soft drop shadow of a floating window (tooltip, open
// dropdown), drawn by the window itself: its own ImGui background is turned off
// (PopupBg transparent, no border) so the shadow can sit underneath.
void FloatingSurface(bool outline) {
    const Palette& P = theme::Colors();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 a = ImGui::GetWindowPos(), sz = ImGui::GetWindowSize(), b(a.x + sz.x, a.y + sz.y);
    const float r = ImGui::GetStyle().PopupRounding;
    dl->PushClipRectFullScreen();
    SoftShadow(dl, a, b, r, U(1), U(4), P.shadow);
    dl->AddRectFilled(a, b, P.popBg, r);
    if (outline) dl->AddRect(a, b, P.popBorder, r, 0, std::max(1.0f, U(1)));
    dl->PopClipRect();
}

void PushFloatingStyle() {
    ImGui::PushStyleColor(ImGuiCol_PopupBg, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 0.0f);
}

void PopFloatingStyle() {
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

// ---------------------------------------------------------- inline code ----
// Console names (HL2 Movement's sv_gravity, hl2_walkspeed, ...) are shown as
// inline code: monospace text on a small rounded chip.
bool IsConsoleName(std::string_view s) {
    const size_t us = s.find('_');
    if (us == std::string_view::npos || us == 0 || us + 1 >= s.size()) return false;
    const std::string_view prefix = s.substr(0, us);
    if (prefix != "sv" && prefix != "hl2" && prefix != "cl" && prefix != "mp") return false;
    return std::all_of(s.begin(), s.end(), [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'; });
}

constexpr float kCodeSize = theme::kSmallSize + 0.5f;

ImVec2 CodeChipSize(const char* text, float size) {
    const ImVec2 ts = TextSize(theme::Mono(), size, text);
    return ImVec2(ts.x + U(10), ts.y + U(4));
}

// Draws the chip with its top-left at `a`; returns its size.
ImVec2 DrawCodeChip(ImDrawList* dl, ImVec2 a, const char* text, float size, ImU32 ink) {
    const Palette& P = theme::Colors();
    const ImVec2 cs = CodeChipSize(text, size);
    a = ImVec2(std::round(a.x), std::round(a.y));
    dl->AddRectFilled(a, ImVec2(a.x + cs.x, a.y + cs.y), ImGui::GetColorU32(P.frame), U(5));
    dl->AddText(theme::Mono(), U(size), ImVec2(a.x + U(5), a.y + U(2)), ImGui::GetColorU32(ink), text);
    return cs;
}

// A chip as a layout item, vertically centred on a frame-height line (like
// AlignTextToFramePadding text).
void CodeChipItem(const char* text, ImU32 ink) {
    const ImVec2 cs = CodeChipSize(text, kCodeSize);
    const ImVec2 cur = ImGui::GetCursorScreenPos();
    const float fh = ImGui::GetFrameHeight();
    ImGui::Dummy(ImVec2(cs.x, fh));
    DrawCodeChip(ImGui::GetWindowDrawList(), ImVec2(cur.x, cur.y + (fh - cs.y) * 0.5f), text, kCodeSize, ink);
}

// Wrapped text whose console names are chips (word by word, wrapping at `wrap`
// px from the line start). Plain text otherwise keeps ImGui's own wrapping.
void RichText(const std::string& text, float wrap) {
    ImFont* f = ImGui::GetFont();
    const float fs = ImGui::GetFontSize();
    const float space = f->CalcTextSizeA(fs, FLT_MAX, 0, " ").x;
    const float lineH = std::max(fs, CodeChipSize("x", kCodeSize).y);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 ink = ImGui::GetColorU32(ImGuiCol_Text);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    float x = 0, y = 0, maxX = 0;
    size_t i = 0;
    while (i <= text.size()) {
        const size_t nl = text.find('\n', i);
        const std::string line = text.substr(i, nl == std::string::npos ? std::string::npos : nl - i);
        size_t p = 0;
        bool first = true;
        while (p < line.size()) {
            const size_t e = std::min(line.find(' ', p), line.size());
            const std::string word = line.substr(p, e - p);
            p = e + 1;
            if (word.empty()) continue;
            // "(sv_gravity)." -> "(", "sv_gravity", ")."
            size_t a = 0, b = word.size();
            while (a < b && std::strchr("(\"'", word[a])) ++a;
            while (b > a && std::strchr(".,;:)\"'", word[b - 1])) --b;
            const std::string pre = word.substr(0, a), core = word.substr(a, b - a), post = word.substr(b);
            const bool code = IsConsoleName(core);
            const float w = code ? f->CalcTextSizeA(fs, FLT_MAX, 0, pre.c_str()).x + CodeChipSize(core.c_str(), kCodeSize).x +
                                       f->CalcTextSizeA(fs, FLT_MAX, 0, post.c_str()).x
                                 : f->CalcTextSizeA(fs, FLT_MAX, 0, word.c_str()).x;
            if (!first && x + space + w > wrap) {
                x = 0;
                y += lineH;
            } else if (!first) {
                x += space;
            }
            first = false;
            const float ty = origin.y + y + (lineH - fs) * 0.5f;
            if (code) {
                dl->AddText(f, fs, ImVec2(origin.x + x, ty), ink, pre.c_str());
                x += f->CalcTextSizeA(fs, FLT_MAX, 0, pre.c_str()).x;
                const ImVec2 cs = CodeChipSize(core.c_str(), kCodeSize);
                DrawCodeChip(dl, ImVec2(origin.x + x, origin.y + y + (lineH - cs.y) * 0.5f), core.c_str(), kCodeSize, ink);
                x += cs.x;
                dl->AddText(f, fs, ImVec2(origin.x + x, ty), ink, post.c_str());
                x += f->CalcTextSizeA(fs, FLT_MAX, 0, post.c_str()).x;
            } else {
                dl->AddText(f, fs, ImVec2(origin.x + x, ty), ink, word.c_str());
                x += w;
            }
            maxX = std::max(maxX, x);
        }
        y += lineH;
        if (nl == std::string::npos) break;
        i = nl + 1;
    }
    ImGui::Dummy(ImVec2(maxX, y));
}

bool HasConsoleName(const std::string& text) {
    size_t p = 0;
    while (p < text.size()) {
        size_t e = text.find_first_of(" \n", p);
        if (e == std::string::npos) e = text.size();
        std::string_view w(text.data() + p, e - p);
        while (!w.empty() && std::strchr("(\"'", w.front())) w.remove_prefix(1);
        while (!w.empty() && std::strchr(".,;:)\"'", w.back())) w.remove_suffix(1);
        if (IsConsoleName(w)) return true;
        p = e + 1;
    }
    return false;
}

void Tooltip(const std::string& text) {
    if (text.empty()) return;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(U(12), U(9)));
    PushFloatingStyle();
    if (ImGui::BeginTooltip()) {
        FloatingSurface(false);
        {
            FontScope f(theme::Body(), theme::kSmallSize + 0.5f);  // popped before EndTooltip
            if (HasConsoleName(text)) {
                RichText(text, U(300) - ImGui::GetStyle().WindowPadding.x);
            } else {
                ImGui::PushTextWrapPos(U(300));
                ImGui::TextUnformatted(text.c_str());
                ImGui::PopTextWrapPos();
            }
        }
        ImGui::EndTooltip();
    }
    PopFloatingStyle();
    ImGui::PopStyleVar();
}

// The logo's Slimepedia entry: the slime's name as a heading, then each section's
// title and text as the Slimepedia page shows them (the intro has no title).
void EntryTooltip(const slime_icons::Entry& e) {
    const Palette& P = theme::Colors();
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(U(14), U(11)));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(U(6), U(2)));
    PushFloatingStyle();
    if (ImGui::BeginTooltip()) {
        FloatingSurface(false);
        ImGui::PushTextWrapPos(U(360));
        {
            FontScope f(theme::Display(), theme::kDisplaySize + 2);
            ImGui::PushStyleColor(ImGuiCol_Text, theme::Current() == theme::Mode::Dark ? P.grab : P.accent);
            ImGui::TextUnformatted(e.name.c_str());
            ImGui::PopStyleColor();
        }
        for (const auto& s : e.sections) {
            ImGui::Dummy(ImVec2(0, U(5)));
            if (!s.title.empty()) {
                FontScope f(theme::Display(), theme::kSmallSize + 1);
                ImGui::PushStyleColor(ImGuiCol_Text, P.soft);
                ImGui::TextUnformatted(s.title.c_str());
                ImGui::PopStyleColor();
            }
            FontScope f(theme::Body(), theme::kSmallSize);
            ImGui::TextUnformatted(s.text.c_str());
        }
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
    PopFloatingStyle();
    ImGui::PopStyleVar(2);
}

void HoverTip(const std::string& text) {
    if (!text.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort | ImGuiHoveredFlags_AllowWhenDisabled))
        Tooltip(text);
}

const ModuleView* FindView(const std::vector<ModuleView>& views, std::string_view m) {
    for (const auto& v : views)
        if (v.module == m) return &v;
    return nullptr;
}

bool IsOn(const settings::Def& d) { return Read(d) >= 0.5; }
bool ModuleOn(const ModuleView& mv) { return !mv.hasEnabled || IsOn(mv.enabled); }

// ---------------------------------------------------------------- icons ----
void DrawIcon(ImDrawList* dl, layout::Icon icon, ImVec2 o, ImU32 col) {
    const float k = g_scale, th = 2.0f * g_scale;
    auto P = [&](float x, float y) { return ImVec2(o.x + x * k, o.y + y * k); };
    switch (icon) {
        case layout::Icon::Movement:
            dl->PathLineTo(P(3, 13)), dl->PathLineTo(P(8, 8)), dl->PathLineTo(P(11, 11)), dl->PathLineTo(P(15, 5));
            dl->PathStroke(col, 0, th);
            dl->PathLineTo(P(11, 5)), dl->PathLineTo(P(15, 5)), dl->PathLineTo(P(15, 9));
            dl->PathStroke(col, 0, th);
            break;
        case layout::Icon::World:
            dl->AddCircle(P(9, 9), 7 * k, col, 0, th);
            dl->AddLine(P(2, 9), P(16, 9), col, th);
            dl->AddEllipse(P(9, 9), ImVec2(3.2f * k, 7 * k), col, 0, 0, th);
            break;
        case layout::Icon::Hud:
            dl->PathArcTo(P(9, 13.5f), 6.5f * k, kPi, 2 * kPi);
            dl->PathStroke(col, 0, th);
            dl->AddLine(P(9, 13.5f), P(12.2f, 8.2f), col, th);
            dl->AddCircleFilled(P(9, 13.5f), 1.7f * k, col);
            break;
        case layout::Icon::Ranch:
            dl->PathLineTo(P(2, 8)), dl->PathLineTo(P(9, 3)), dl->PathLineTo(P(16, 8)), dl->PathLineTo(P(16, 16)),
                dl->PathLineTo(P(2, 16));
            dl->PathStroke(col, ImDrawFlags_Closed, th);
            dl->PathLineTo(P(7, 16)), dl->PathLineTo(P(7, 11)), dl->PathLineTo(P(11, 11)), dl->PathLineTo(P(11, 16));
            dl->PathStroke(col, 0, th);
            break;
        case layout::Icon::Misc:
            for (float y : {2.5f, 10.5f})
                for (float x : {2.5f, 10.5f}) dl->AddRect(P(x, y), P(x + 5, y + 5), col, 1.5f * k, 0, th * 0.9f);
            break;
        case layout::Icon::Profiles:
            dl->AddLine(P(3, 5), P(15, 5), col, th);
            dl->AddLine(P(3, 9), P(15, 9), col, th);
            dl->AddLine(P(3, 13), P(11, 13), col, th);
            break;
        case layout::Icon::Settings:
            dl->AddCircle(P(9, 9), 2.6f * k, col, 0, th);
            for (int i = 0; i < 8; ++i) {
                const float a = static_cast<float>(i) * kPi / 4;
                dl->AddLine(P(9 + std::cos(a) * 4.8f, 9 + std::sin(a) * 4.8f), P(9 + std::cos(a) * 7.2f, 9 + std::sin(a) * 7.2f),
                            col, th);
            }
            break;
    }
}

void DrawResetIcon(ImDrawList* dl, ImVec2 c, ImU32 col) {
    const float r = U(6.5f), th = U(2);
    dl->PathArcTo(c, r, kPi * 0.95f, kPi * 2.55f);
    dl->PathStroke(col, 0, th);
    const ImVec2 tip(c.x + std::cos(kPi * 0.95f) * r, c.y + std::sin(kPi * 0.95f) * r);
    dl->AddTriangleFilled(ImVec2(tip.x - U(3.5f), tip.y - U(1)), ImVec2(tip.x + U(3.5f), tip.y - U(1)),
                          ImVec2(tip.x, tip.y + U(4)), col);
}

void DrawCross(ImDrawList* dl, ImVec2 c, ImU32 col) {
    const float r = U(5), th = U(2.4f);
    dl->AddLine(ImVec2(c.x - r, c.y - r), ImVec2(c.x + r, c.y + r), col, th);
    dl->AddLine(ImVec2(c.x + r, c.y - r), ImVec2(c.x - r, c.y + r), col, th);
}

void DrawChevron(ImDrawList* dl, ImVec2 c, ImU32 col) {
    dl->PathLineTo(ImVec2(c.x - U(4), c.y - U(2)));
    dl->PathLineTo(ImVec2(c.x, c.y + U(2.5f)));
    dl->PathLineTo(ImVec2(c.x + U(4), c.y - U(2)));
    dl->PathStroke(col, 0, U(2.4f));
}

// The menu's own pink slime (48x44 design units), for when the game's icons
// are not available.
void DrawSlimeFallback(ImDrawList* dl, ImVec2 min, ImVec2 max) {
    const float k = std::min((max.x - min.x) / 48.0f, (max.y - min.y) / 44.0f);
    const ImVec2 o((min.x + max.x) * 0.5f - 24 * k, (min.y + max.y) * 0.5f - 22 * k);
    auto P = [&](float x, float y) { return ImVec2(o.x + x * k, o.y + y * k); };
    const ImU32 body = IM_COL32(0xFF, 0x8F, 0xC0, 0xFF), base = IM_COL32(0xE5, 0x67, 0x9F, 0xFF);
    const ImU32 ink = IM_COL32(0x24, 0x1F, 0x3D, 0xFF), white = IM_COL32(0xFF, 0xFF, 0xFF, 0xFF);
    dl->PathLineTo(P(24, 10));
    dl->PathBezierCubicCurveTo(P(35, 10), P(42, 20), P(42, 31));
    dl->PathBezierCubicCurveTo(P(42, 39), P(35, 42), P(24, 42));
    dl->PathBezierCubicCurveTo(P(13, 42), P(6, 39), P(6, 31));
    dl->PathBezierCubicCurveTo(P(6, 20), P(13, 10), P(24, 10));
    dl->_Path.pop_back();  // the closing point repeats the first
    dl->PathFillConvex(body);
    dl->PathLineTo(P(8, 35));
    dl->PathBezierCubicCurveTo(P(12, 40), P(36, 40), P(40, 35));
    dl->PathBezierCubicCurveTo(P(39, 40), P(33, 42), P(24, 42));
    dl->PathBezierCubicCurveTo(P(15, 42), P(9, 40), P(8, 35));
    dl->_Path.pop_back();
    dl->PathFillConcave(base);
    dl->AddEllipseFilled(P(15.5f, 18), ImVec2(4.4f * k, 2.6f * k), WithAlpha(white, 0.72f), -kPi / 6);
    dl->AddCircleFilled(P(11.5f, 23), 1.2f * k, WithAlpha(white, 0.55f));
    for (float x : {18.5f, 29.5f}) {
        dl->AddEllipseFilled(P(x, 27), ImVec2(2.5f * k, 4 * k), ink);
        dl->AddCircleFilled(P(x + 0.8f, 25.4f), 1.0f * k, white);
    }
    dl->PathLineTo(P(20.5f, 33));
    dl->PathBezierQuadraticCurveTo(P(24, 36.5f), P(27.5f, 33));
    dl->PathStroke(ink, 0, 1.9f * k);
}

// -------------------------------------------------------------- widgets ----
// "(?)" with a tooltip, `size` square at the cursor. Crisp at any scale: the
// centre is snapped to the pixel grid, the ring is a soft filled disc with a
// 1.5 px outline (many segments), and the "?" is the bold display font at a
// whole-pixel position with its ink (not its line box) centred.
void HelpMarker(const char* id, float size, const std::string& tip) {
    const Palette& P = theme::Colors();
    ImGui::InvisibleButton(id, ImVec2(size, size));
    const bool hov = ImGui::IsItemHovered() || ImGui::IsItemFocused();
    const ImVec2 a = ImGui::GetItemRectMin();
    const ImVec2 c(std::floor(a.x + size * 0.5f) + 0.5f, std::floor(a.y + size * 0.5f) + 0.5f);
    const float r = std::floor(size * 0.5f) - 0.5f;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddCircleFilled(c, r, ImGui::GetColorU32(hov ? P.frameHover : P.frame), 48);
    dl->AddCircle(c, r - U(0.75f), ImGui::GetColorU32(hov ? P.tabSel : P.line), 48, U(1.5f));
    ImFont* qf = theme::Display();
    const float qs = std::round(size * 0.62f);
    const ImFontGlyph* g = qf->GetFontBaked(qs)->FindGlyph('?');
    ImVec2 qp = g ? ImVec2(c.x - (g->X0 + g->X1) * 0.5f, c.y - (g->Y0 + g->Y1) * 0.5f) : c;
    qp = ImVec2(std::round(qp.x), std::round(qp.y));
    dl->AddText(qf, qs, qp, ImGui::GetColorU32(hov ? P.text : P.soft), "?");
    if (hov) Tooltip(tip);
}

// On/off switch, vertically centred in a frame-height row. True when clicked.
bool Switch(const char* id, bool on) {
    const Palette& P = theme::Colors();
    const ImVec2 size(U(kSwitchW), U(kSwitchH));
    const ImVec2 cur = ImGui::GetCursorScreenPos();
    ImGui::SetCursorScreenPos(ImVec2(cur.x, cur.y + (ImGui::GetFrameHeight() - size.y) * 0.5f));
    const bool clicked = ImGui::InvisibleButton(id, size);
    const bool hovered = ImGui::IsItemHovered();
    float* t = ImGui::GetStateStorage()->GetFloatRef(ImGui::GetItemID(), on ? 1.0f : 0.0f);
    *t = Approach(*t, on ? 1.0f : 0.0f, 9.0f);
    const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImU32 bg = Mix(P.frame, P.accent, Smooth(*t));
    if (hovered) bg = Mix(bg, on ? IM_COL32_WHITE : P.frameHover, on ? 0.12f : 0.8f);
    // GetColorU32 applies the style alpha, so BeginDisabled() fades the switch like ImGui's own widgets.
    dl->AddRectFilled(a, b, ImGui::GetColorU32(bg), size.y * 0.5f);
    const float r = U(8), x = a.x + U(4) + r + Smooth(*t) * (size.x - U(8) - 2 * r);
    dl->AddCircleFilled(ImVec2(x, (a.y + b.y) * 0.5f), r, ImGui::GetColorU32(Mix(P.dim, IM_COL32_WHITE, Smooth(*t))));
    return clicked;
}

// ImGui format text: '%' doubled so a unit or zero text is shown literally.
std::string FormatLiteral(const std::string& s) {
    std::string out;
    for (char c : s) {
        out.push_back(c);
        if (c == '%') out.push_back('%');
    }
    return out;
}

// Slider on the displayed value (Def::displayScale) with Def::unit after the
// number, or Def::zeroText instead of it at 0. Ctrl+click types the number.
void SliderDef(const settings::Def& d, double cur, float w) {
    ImGui::SetNextItemWidth(w);
    const double k = d.displayScale > 0 ? d.displayScale : 1.0;
    std::string fmt;
    if (!d.zeroText.empty() && std::fabs(cur) < 1e-9 && !ImGui::GetIO().KeyCtrl)
        fmt = FormatLiteral(d.zeroText);
    else if (d.kind == settings::Kind::Int && k == 1.0)
        fmt = "%d" + FormatLiteral(d.unit);
    else
        fmt = std::format("%.{}f", DisplayDecimals(d)) + FormatLiteral(d.unit);
    if (d.kind == settings::Kind::Int && k == 1.0) {
        int v = static_cast<int>(std::lround(cur));
        if (ImGui::SliderInt("##v", &v, static_cast<int>(std::lround(d.min)), static_cast<int>(std::lround(d.max)),
                             fmt.c_str(), ImGuiSliderFlags_AlwaysClamp)) {
            const double s = Snap(d, v);
            if (std::fabs(s - cur) > 1e-9) Write(d, s);
        }
    } else {
        float v = static_cast<float>(cur * k);
        if (ImGui::SliderFloat("##v", &v, static_cast<float>(d.min * k), static_cast<float>(d.max * k), fmt.c_str(),
                               ImGuiSliderFlags_AlwaysClamp)) {
            const double s = Snap(d, v / k);
            if (std::fabs(s - cur) > 1e-9) Write(d, s);
        }
    }
}

// Dropdown: short label when closed; each option shows its description when open.
void ComboDef(const settings::Def& d, double cur, float w) {
    const Palette& P = theme::Colors();
    const auto texts = layout::EnumTexts(d);
    if (texts.empty()) return;
    const int idx = std::clamp(static_cast<int>(std::lround(cur)), 0, static_cast<int>(texts.size()) - 1);
    ImFont* body = theme::Body();
    bool anyDesc = false;
    float popW = w;
    for (const auto& t : texts) {
        popW = std::max(popW, TextSize(body, theme::kBodySize, t.label.c_str()).x + U(40));
        anyDesc |= !t.desc.empty();
    }
    if (anyDesc) popW = std::max(popW, U(240));
    popW = std::min(popW, U(280));

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const float fh = ImGui::GetFrameHeight();
    ImGui::SetNextItemWidth(w);
    ImGui::SetNextWindowSizeConstraints(ImVec2(popW, 0), ImVec2(popW, FLT_MAX));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(U(6), U(6)));
    PushFloatingStyle();
    const bool open = ImGui::BeginCombo("##e", texts[static_cast<size_t>(idx)].label.c_str(),
                                        ImGuiComboFlags_NoArrowButton | ImGuiComboFlags_HeightLarge);
    PopFloatingStyle();
    ImGui::PopStyleVar();
    if (open) FloatingSurface(true);
    DrawChevron(dl, ImVec2(pos.x + w - U(17), pos.y + fh * 0.5f), ImGui::GetColorU32(P.grab));
    if (!open) return;
    // Options sit flush against each other inside the list's padding.
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_SelectableTextAlign, ImVec2(0, 0));
    // Rows touch (no dead space between them); their fill is drawn inset below, so
    // neighbouring highlights keep a gap. Selectable's own fill is off.
    ImGui::PushStyleColor(ImGuiCol_Header, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, IM_COL32(0, 0, 0, 0));
    for (int i = 0; i < static_cast<int>(texts.size()); ++i) {
        const auto& t = texts[static_cast<size_t>(i)];
        const float innerW = ImGui::GetContentRegionAvail().x;
        const float lineH = ImGui::GetFontSize();
        const ImVec2 ds = t.desc.empty() ? ImVec2(0, 0) : TextSize(body, theme::kSmallSize, t.desc.c_str(), innerW - U(20));
        const float h = U(20) + lineH + (t.desc.empty() ? 0.0f : U(3) + ds.y);
        const bool sel = i == idx;
        ImGui::PushID(i);
        if (ImGui::Selectable("##opt", sel, 0, ImVec2(0, h)) && !sel) Write(d, i);
        if (sel) ImGui::SetItemDefaultFocus();
        const ImVec2 mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax();
        ImDrawList* pdl = ImGui::GetWindowDrawList();
        const bool hov = ImGui::IsItemHovered() || ImGui::IsItemFocused();
        if (hov || sel)
            pdl->AddRectFilled(ImVec2(mn.x, mn.y + U(2)), ImVec2(mx.x, mx.y - U(2)), hov ? P.optHover : P.optSel,
                               ImGui::GetStyle().SelectableRounding);
        const ImU32 labelCol = sel ? (theme::Current() == theme::Mode::Dark ? P.grab : P.accent) : P.text;
        Text(pdl, body, theme::kBodySize, ImVec2(mn.x + U(10), mn.y + U(10)), labelCol, t.label.c_str());
        if (!t.desc.empty())
            Text(pdl, body, theme::kSmallSize, ImVec2(mn.x + U(10), mn.y + U(10) + lineH + U(3)), P.dim, t.desc.c_str(),
                 innerW - U(20));
        ImGui::PopID();
    }
    ImGui::PopStyleColor(3);
    ImGui::PopStyleVar(2);
    ImGui::EndCombo();
}

void KeyDef(const Host& h, const settings::Def& d, double cur, float w) {
    const Palette& P = theme::Colors();
    const bool capturing = h.isCapturing && h.isCapturing(d);
    std::string text = capturing ? std::string("Press a key...") : KeyName(static_cast<int>(std::lround(cur)));
    text += "###key";
    if (capturing) {
        const float pulse = 0.5f + 0.5f * std::sin(static_cast<float>(ImGui::GetTime()) * 6.0f);
        ImGui::PushStyleColor(ImGuiCol_Button, Mix(P.accent, P.btnActive, pulse));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, P.accent);
        ImGui::PushStyleColor(ImGuiCol_Text, P.accentInk);
    }
    if (ImGui::Button(text.c_str(), ImVec2(w, 0)) && !capturing && h.startCapture) h.startCapture(d);
    if (capturing) ImGui::PopStyleColor(3);
    HoverTip(capturing ? "Esc cancels, Backspace/Delete clears the bind." : "Click, then press a key or mouse button 3/4/5.");
}

// One setting: label on the left (dim while its switch is off), control on the right.
void DrawRow(const Host& h, const settings::Def& d, const std::string& label, bool nested) {
    const Palette& P = theme::Colors();
    ImGui::PushID(d.key.c_str());
    const double cur = Read(d);
    const float avail = ImGui::GetContentRegionAvail().x;
    const bool isBool = d.kind == settings::Kind::Bool;
    const float ctlW = isBool ? U(kSwitchW) : std::min(U(kCtlW), std::floor(avail * 0.5f));
    const float x0 = ImGui::GetCursorPosX();

    // A switch row is clickable as a whole (label and the gap included): an
    // invisible button under it, sized like the row last frame. AllowOverlap
    // lets the switch drawn on top keep its own hover and click.
    bool rowClicked = false, rowHovered = false;
    const ImVec2 rowStart = ImGui::GetCursorScreenPos();
    ImGuiStorage* st = ImGui::GetStateStorage();
    const ImGuiID rowHId = ImGui::GetID("##rowh");
    if (isBool) {
        const float rowH = std::max(ImGui::GetFrameHeight(), st->GetFloat(rowHId, 0.0f));
        ImGui::SetNextItemAllowOverlap();
        rowClicked = ImGui::InvisibleButton("##row", ImVec2(std::max(1.0f, avail), rowH));
        rowHovered = ImGui::IsItemHovered();
        ImGui::SetCursorScreenPos(rowStart);
    }

    ImU32 col = nested ? P.soft : P.text;
    if (isBool && cur < 0.5) col = rowHovered ? P.soft : P.dim;
    if (IsConsoleName(label)) {  // HL2 console names ("sv_gravity") as inline code
        CodeChipItem(label.c_str(), col);
    } else {
        ImGui::AlignTextToFramePadding();
        ImGui::PushTextWrapPos(x0 + avail - ctlW - U(12));
        ImGui::PushStyleColor(ImGuiCol_Text, col);
        ImGui::TextUnformatted(label.c_str());
        ImGui::PopStyleColor();
        ImGui::PopTextWrapPos();
    }
    HoverTip(d.tooltip);
    const float labelBottom = ImGui::GetItemRectMax().y;
    ImGui::SameLine(x0 + avail - ctlW);
    switch (d.kind) {
        case settings::Kind::Bool:
            if (Switch("##b", cur >= 0.5) || rowClicked) Write(d, cur >= 0.5 ? 0.0 : 1.0);
            HoverTip(d.tooltip);
            st->SetFloat(rowHId, std::max(labelBottom, ImGui::GetItemRectMax().y) - rowStart.y);
            break;
        case settings::Kind::Float:
        case settings::Kind::Int: SliderDef(d, cur, ctlW); break;
        case settings::Kind::Enum: ComboDef(d, cur, ctlW); break;
        case settings::Kind::Key: KeyDef(h, d, cur, ctlW); break;
    }
    ImGui::PopID();
}

// A HUD element's position and size settings as one row: a "Move" button that
// starts the HUD edit mode (hud_edit.h; the overlay then closes the window).
void DrawMoveRow(const settings::Def& d, hud_edit::Element e) {
    ImGui::PushID(d.key.c_str());
    const float avail = ImGui::GetContentRegionAvail().x;
    const float ctlW = std::min(U(kCtlW), std::floor(avail * 0.5f));
    const float x0 = ImGui::GetCursorPosX();
    const std::string tip = std::string("Closes this window. Drag the ") + hud_edit::Name(e) +
                            " to move it (it snaps to the screen centre), drag a corner to resize it (Alt: from the "
                            "centre). Shift: the same gap to both edges of its corner. Enter or a click outside keeps "
                            "it, Esc cancels.";
    ImGui::AlignTextToFramePadding();
    ImGui::PushStyleColor(ImGuiCol_Text, theme::Colors().text);
    ImGui::TextUnformatted("Position and size");
    ImGui::PopStyleColor();
    HoverTip(tip);
    ImGui::SameLine(x0 + avail - ctlW);
    if (ImGui::Button("Move", ImVec2(ctlW, 0))) hud_edit::Begin(e);
    HoverTip(tip);
    ImGui::PopID();
}

// Options that depend on `parent`: slide open below it, indented on a thread line.
// Drawn in the card itself (no child window), so the block is measured in the
// frame it is drawn: while animating, the rows are shifted up by the hidden part
// and clipped to the visible part, and the layout advances by exactly that part
// (trailing item spacing included), so open and close slide without a jump at
// either end. Hidden rows fade with the slide instead of snapping to the
// disabled look, and take no input while closing.
void DrawKids(const Host& h, const settings::Def& parent, const std::vector<settings::Def>& kids, bool visible,
              std::string_view masterLabel) {
    constexpr float kSlideSec = 0.22f;
    const Palette& P = theme::Colors();
    ImGuiStorage* st = ImGui::GetStateStorage();
    const std::string key = "kids/" + parent.key + "/" + std::to_string(kids.front().parentValue);
    const ImGuiID tId = ImGui::GetID(key.c_str()), hId = ImGui::GetID((key + "/h").c_str());
    const float target = visible ? 1.0f : 0.0f;
    const float t = Approach(st->GetFloat(tId, target), target, 1.0f / kSlideSec);
    st->SetFloat(tId, t);
    if (t <= 0.0f) return;
    // Last frame's block height; 0 = never measured (first open of a block that
    // started closed): that frame draws it parked far above, fully clipped, to measure it.
    const float fullH = st->GetFloat(hId, 0.0f);
    const bool sliding = t < 1.0f;
    const float e = Smooth(t);
    const float shownH = sliding ? fullH * e : fullH;
    const float shift = sliding ? (fullH > 0.0f ? fullH - shownH : U(100000)) : 0.0f;
    const float sp = ImGui::GetStyle().ItemSpacing.y;

    const ImVec2 start = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (sliding) {
        ImGui::PushClipRect(ImVec2(dl->GetClipRectMin().x, start.y), ImVec2(dl->GetClipRectMax().x, start.y + shownH), true);
        ImGui::SetCursorScreenPos(ImVec2(start.x, start.y - shift));
    }
    const float alpha = ImGui::GetStyle().Alpha;
    ImGui::BeginDisabled(!visible);
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, sliding ? alpha * e : alpha);  // instead of the disabled dim
    ImGui::Indent(U(22));
    const float top = ImGui::GetCursorScreenPos().y;
    for (const auto& k : kids) DrawRow(h, k, layout::ShortLabel(k, &parent, "", masterLabel), true);
    const float measured = ImGui::GetCursorScreenPos().y - top;  // rows + the spacing after the last one
    ImGui::Unindent(U(22));
    ImGui::PopStyleVar();
    ImGui::EndDisabled();
    if (sliding) ImGui::PopClipRect();
    st->SetFloat(hId, measured);

    // Continue below the visible part; Dummy(0, 0) adds back the spacing taken off here.
    const float advance = sliding ? shownH : measured;
    ImGui::SetCursorScreenPos(ImVec2(start.x, start.y + advance - sp));
    ImGui::Dummy(ImVec2(0, 0));
    if (shownH > U(10))
        dl->AddLine(ImVec2(start.x + U(9), start.y), ImVec2(start.x + U(9), start.y + shownH - U(10)), ImGui::GetColorU32(P.line), U(2));
}

// Rows of one group, with nested options under their parent (settings::Def::parent).
// A setting is nested when its parent is a top-level row of the same group;
// any other setting with a parent (parent in another card, or nested itself)
// stays in place and is simply hidden while its parent chain does not allow it.
struct Nesting {
    std::map<std::string, std::vector<settings::Def>> kidsOf;  // parent key -> kids, registration order
    std::set<std::string> isKid;
};

Nesting BuildNesting(const std::vector<const GroupView*>& groups, std::string_view skipKey) {
    Nesting n;
    for (const auto* g : groups) {
        auto inGroup = [&](const std::string& key) -> const settings::Def* {
            for (const auto& d : g->defs)
                if (d.key == key) return &d;
            return nullptr;
        };
        for (const auto& d : g->defs) {
            if (d.parent.empty() || d.parent == skipKey) continue;
            const settings::Def* p = inGroup(d.parent);
            if (!p || (!p->parent.empty() && p->parent != skipKey && inGroup(p->parent))) continue;  // one level only
            n.kidsOf[d.parent].push_back(d);
            n.isKid.insert(d.key);
        }
    }
    return n;
}

// Rows the group shows right now (top-level rows whose parent chain allows them).
bool RowShown(const ModuleView& mv, const settings::Def& d, const Nesting& n, std::string_view skipKey) {
    return d.key != skipKey && !n.isKid.count(d.key) && !hud_edit::Hidden(mv.module.c_str(), d.key.c_str()) &&
           ParentChainAllows(mv, d);
}

void DrawGroupRows(const Host& h, const ModuleView& mv, const GroupView& g, const Nesting& n, std::string_view skipKey,
                   std::string_view masterLabel) {
    for (const auto& d : g.defs) {
        if (!RowShown(mv, d, n, skipKey)) continue;
        if (hud_edit::Element e; hud_edit::MoveRow(mv.module.c_str(), d.key.c_str(), e)) {
            DrawMoveRow(d, e);  // position + size: the HUD edit mode instead of sliders
            continue;
        }
        DrawRow(h, d, layout::ShortLabel(d, nullptr, g.name, masterLabel), false);
        auto it = n.kidsOf.find(d.key);
        if (it == n.kidsOf.end()) continue;
        // One sliding block per distinct parentValue (e.g. "while on" and "while off" kids of one switch).
        std::map<int, std::vector<settings::Def>> byValue;
        for (const auto& k : it->second) byValue[k.parentValue].push_back(k);
        const double pv = Read(d);
        for (const auto& [value, kids] : byValue) DrawKids(h, d, kids, settings::ParentAllows(kids.front(), pv), masterLabel);
    }
}

// Shown instead of a module's settings while its switch is off.
void OffNote(const std::string& text) {
    ImGui::Dummy(ImVec2(0, U(4)));
    ImGui::PushTextWrapPos(0.0f);
    ImGui::PushStyleColor(ImGuiCol_Text, theme::Colors().dim);
    ImGui::TextUnformatted(text.c_str());
    ImGui::PopStyleColor();
    ImGui::PopTextWrapPos();
}

void BeginCard(const char* id, float w) {
    const Palette& P = theme::Colors();
    ImGui::PushStyleColor(ImGuiCol_ChildBg, P.card);
    ImGui::PushStyleColor(ImGuiCol_Border, P.cardBorder);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(U(16), U(14)));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(U(10), U(6)));
    ImGui::BeginChild(id, ImVec2(w, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY, ImGuiWindowFlags_NoScrollbar);
}

void EndCard() {
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);
}

void CardHeading(const char* text, ImU32 col) {
    FontScope f(theme::Display(), theme::kDisplaySize - 1);
    ImGui::PushStyleColor(ImGuiCol_Text, col);
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
    ImGui::Dummy(ImVec2(0, U(1)));
}

float ColumnWidth() { return std::floor((ImGui::GetContentRegionAvail().x - U(kGap)) * 0.5f); }

// A module's groups as cards in two columns (shorter column first).
void DrawModuleCards(const Host& h, const ModuleView& mv, const Tab& tab, std::string_view skipKey,
                     std::string_view masterLabel) {
    std::vector<const GroupView*> groups;
    for (const auto& g : mv.groups) {
        if (!tab.onlyGroups.empty() && std::find(tab.onlyGroups.begin(), tab.onlyGroups.end(), g.name) == tab.onlyGroups.end())
            continue;
        if (std::find(tab.skipGroups.begin(), tab.skipGroups.end(), g.name) != tab.skipGroups.end()) continue;
        groups.push_back(&g);
    }
    const Nesting n = BuildNesting(groups, skipKey);
    std::vector<const GroupView*> cols[2];
    float heights[2] = {0, 0};
    for (const auto* g : groups) {
        float rows = 0.0f;
        for (const auto& d : g->defs)
            if (RowShown(mv, d, n, skipKey)) rows += 1.0f;
        if (rows <= 0.0f) continue;  // every row hidden behind its parent: no empty card
        if (!g->name.empty()) rows += 1.3f;
        const int c = heights[0] <= heights[1] ? 0 : 1;
        cols[c].push_back(g);
        heights[c] += rows + 1.2f;
    }
    if (cols[0].empty()) {
        ImGui::TextDisabled("Nothing to set here.");
        return;
    }
    const float w = ColumnWidth();
    ImGui::PushID(mv.module.c_str());
    for (int c = 0; c < 2; ++c) {
        if (c == 1) ImGui::SameLine(0, U(kGap));
        ImGui::BeginGroup();
        for (const auto* g : cols[c]) {
            BeginCard(("card/" + g->name).c_str(), w);
            // A tab made of one group (Speed Panel) is already named by the tab.
            if (!g->name.empty() && tab.onlyGroups.size() != 1) CardHeading(g->name.c_str(), theme::Colors().head);
            DrawGroupRows(h, mv, *g, n, skipKey, masterLabel);
            EndCard();
            ImGui::Dummy(ImVec2(0, U(kGap) - ImGui::GetStyle().ItemSpacing.y));
        }
        if (cols[c].empty()) ImGui::Dummy(ImVec2(w, 1));
        ImGui::EndGroup();
    }
    ImGui::PopID();
}

void Banner(ImU32 col, const std::string& text) {
    const Palette& P = theme::Colors();
    const float w = ImGui::GetContentRegionAvail().x;
    ImFont* body = theme::Body();
    const ImVec2 ts = TextSize(body, theme::kBodySize, text.c_str(), w - U(32));
    const ImVec2 a = ImGui::GetCursorScreenPos(), b(a.x + w, a.y + ts.y + U(20));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(a, b, P.card, U(12));
    dl->AddRectFilled(a, ImVec2(a.x + U(5), b.y), col, U(12), ImDrawFlags_RoundCornersLeft);
    Text(dl, body, theme::kBodySize, ImVec2(a.x + U(18), a.y + U(10)), col, text.c_str(), w - U(32));
    ImGui::Dummy(ImVec2(w, b.y - a.y));
    ImGui::Dummy(ImVec2(0, U(4)));
}

void FailedBanner(const ModuleView& mv) {
    bool failed = false;
    const std::string status = ModuleStatus(mv.module, &failed);
    if (failed) Banner(theme::Colors().err, mv.title + " failed to start: " + status);
}

// Sub-section of a tab for a module drawn inside another (Collectibles under Minimap).
void DrawNested(const Host& h, const ModuleView& nmv) {
    const Palette& P = theme::Colors();
    ImGui::Dummy(ImVec2(0, U(10)));
    const float avail = ImGui::GetContentRegionAvail().x;
    const ImVec2 a = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddLine(a, ImVec2(a.x + avail, a.y), P.line, U(2));
    ImGui::Dummy(ImVec2(0, U(12)));
    const bool on = ModuleOn(nmv);
    const float x0 = ImGui::GetCursorPosX();
    {
        FontScope f(theme::Display(), theme::kDisplaySize + 2);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(nmv.title.c_str());
    }
    if (nmv.hasEnabled) {
        ImGui::SameLine(x0 + avail - U(kSwitchW));
        ImGui::PushID(nmv.module.c_str());
        if (Switch("##enabled", on)) Write(nmv.enabled, on ? 0.0 : 1.0);
        ImGui::PopID();
        HoverTip(nmv.enabled.tooltip);
    }
    ImGui::Dummy(ImVec2(0, U(4)));
    FailedBanner(nmv);
    Tab all;
    ImGui::BeginDisabled(!on);  // grayed out and read-only while the module is off (its switch stays usable)
    DrawModuleCards(h, nmv, all, {}, {});
    ImGui::EndDisabled();
}

// Misc > General: one card per module, the module's switch in the card header.
void DrawCardsPage(const Host& h, const Tab& tab, const std::vector<ModuleView>& views) {
    const Palette& P = theme::Colors();
    const float w = ColumnWidth();
    for (size_t c = 0; c < tab.columns.size() && c < 2; ++c) {
        if (c == 1) ImGui::SameLine(0, U(kGap));
        ImGui::BeginGroup();
        for (const auto& m : tab.columns[c]) {
            const ModuleView* mv = FindView(views, m);
            if (!mv) continue;
            ImGui::PushID(m.c_str());
            const bool on = ModuleOn(*mv);
            BeginCard("card", w);
            const float avail = ImGui::GetContentRegionAvail().x, x0 = ImGui::GetCursorPosX();
            {
                FontScope f(theme::Display(), theme::kDisplaySize);
                ImGui::AlignTextToFramePadding();
                ImGui::PushStyleColor(ImGuiCol_Text, on ? P.text : P.dim);
                ImGui::TextUnformatted(mv->title.c_str());
                ImGui::PopStyleColor();
            }
            if (mv->hasEnabled) {
                ImGui::SameLine(x0 + avail - U(kSwitchW));
                const std::string lock = settings::LockReason(m, "enabled");  // pinned by the module (settings::Lock)
                ImGui::BeginDisabled(!lock.empty());
                if (Switch("##enabled", on)) Write(mv->enabled, on ? 0.0 : 1.0);
                ImGui::EndDisabled();
                const auto help = tab.cardHelp.find(m);
                HoverTip(!lock.empty() ? lock : help != tab.cardHelp.end() ? help->second : mv->enabled.tooltip);
            }
            bool failed = false;
            const std::string status = ModuleStatus(mv->module, &failed);
            if (failed) {
                ImGui::PushTextWrapPos(0.0f);
                ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(P.err), "Failed: %s", status.c_str());
                ImGui::PopTextWrapPos();
            }
            std::vector<const GroupView*> groups;
            for (const auto& g : mv->groups) groups.push_back(&g);
            const Nesting n = BuildNesting(groups, {});
            ImGui::BeginDisabled(!on);  // grayed out and read-only while the module is off
            for (const auto* g : groups) {
                const bool any = std::any_of(g->defs.begin(), g->defs.end(),
                                             [&](const settings::Def& d) { return RowShown(*mv, d, n, {}); });
                if (!any) continue;
                if (!g->name.empty() && groups.size() > 1) CardHeading(g->name.c_str(), P.head);
                DrawGroupRows(h, *mv, *g, n, {}, {});
            }
            ImGui::EndDisabled();
            EndCard();
            ImGui::Dummy(ImVec2(0, U(kGap) - ImGui::GetStyle().ItemSpacing.y));
            ImGui::PopID();
        }
        ImGui::EndGroup();
    }
}

// ------------------------------------------------------------- profiles ----
bool SameName(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        const char x = static_cast<char>((a[i] >= 'A' && a[i] <= 'Z') ? a[i] - 'A' + 'a' : a[i]);
        const char y = static_cast<char>((b[i] >= 'A' && b[i] <= 'Z') ? b[i] - 'A' + 'a' : b[i]);
        if (x != y) return false;
    }
    return true;
}

const profiles::Info* FindProfile(const std::vector<profiles::Info>& list, const std::string& name) {
    for (const auto& i : list)
        if (SameName(i.name, name)) return &i;
    return nullptr;
}

int ProfileNameFilter(ImGuiInputTextCallbackData* data) {
    const ImWchar c = data->EventChar;
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == ' ' || c == '-' ||
                    c == '_';
    return ok ? 0 : 1;
}

void CopyToBuf(char* buf, size_t size, const std::string& s) {
    const size_t n = std::min(s.size(), size - 1);
    std::memcpy(buf, s.data(), n);
    buf[n] = 0;
}

bool PopupCancelled() { return ImGui::IsKeyPressed(ImGuiKey_Escape, false); }

void Badge(ImDrawList* dl, ImVec2 rightCenter, const char* text, ImU32 bg, ImU32 ink) {
    ImFont* body = theme::Body();
    const ImVec2 ts = TextSize(body, 11.5f, text);
    const ImVec2 a(rightCenter.x - ts.x - U(14), rightCenter.y - ts.y * 0.5f - U(3));
    const ImVec2 b(rightCenter.x, rightCenter.y + ts.y * 0.5f + U(3));
    dl->AddRectFilled(a, b, bg, U(6));
    Text(dl, body, 11.5f, ImVec2(a.x + U(7), a.y + U(3)), ink, text);
}

void DrawProfilesPage(const Host& h) {
    const Palette& P = theme::Colors();
    const auto list = profiles::List();
    const std::string active = profiles::ActiveProfile();
    const profiles::Info* activeInfo = active.empty() ? nullptr : FindProfile(list, active);
    const bool modified = activeInfo && profiles::IsModified();
    if (const profiles::Info* found = FindProfile(list, g_profSel))
        g_profSel = found->name;
    else
        g_profSel = activeInfo ? activeInfo->name : (list.empty() ? std::string() : list.front().name);
    const profiles::Info* sel = FindProfile(list, g_profSel);

    const float w = ColumnWidth();
    // Left: the profiles.
    BeginCard("list", w);
    if (list.empty()) ImGui::TextDisabled("No profiles yet.");
    ImDrawList* dl = ImGui::GetWindowDrawList();
    for (const auto& i : list) {
        ImGui::PushID(i.name.c_str());
        const bool isSel = sel && SameName(i.name, sel->name);
        const ImVec2 size(ImGui::GetContentRegionAvail().x, U(38));
        if (ImGui::InvisibleButton("##p", size)) g_profSel = i.name;
        const bool hovered = ImGui::IsItemHovered();
        if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && i.valid) profiles::Load(i.name);
        const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
        if (isSel || hovered) dl->AddRectFilled(a, b, isSel ? P.sel : P.hover, U(10));
        Text(dl, theme::Body(), theme::kBodySize, ImVec2(a.x + U(12), (a.y + b.y) * 0.5f - ImGui::GetFontSize() * 0.5f),
             i.valid ? P.text : P.err, i.name.c_str());
        const ImVec2 rc(b.x - U(10), (a.y + b.y) * 0.5f);
        if (!i.valid)
            Badge(dl, rc, "BROKEN", P.err, P.accentInk);
        else if (activeInfo && SameName(i.name, activeInfo->name))
            Badge(dl, rc, modified ? "MODIFIED" : "ACTIVE", modified ? IM_COL32(0xFF, 0xCF, 0x5A, 0xFF) : P.tabSel,
                  modified ? IM_COL32(0x2B, 0x23, 0x50, 0xFF) : P.tabInk);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
            if (!i.valid)
                Tooltip("Broken file: " + i.error);
            else
                Tooltip(i.description.empty() ? "Double-click to load." : i.description + "\nDouble-click to load.");
        }
        ImGui::PopID();
    }
    if (!active.empty() && !activeInfo)
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(P.warn), "Active profile '%s' has no file.", active.c_str());
    EndCard();

    // Right: what to do with it.
    ImGui::SameLine(0, U(kGap));
    BeginCard("actions", w);
    const float bw = std::floor((ImGui::GetContentRegionAvail().x - U(8)) * 0.5f);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(U(8), U(8)));
    const ImVec2 bs(bw, U(32));
    ImGui::BeginDisabled(!sel || !sel->valid);
    if (ImGui::Button("Load", bs) && sel) profiles::Load(sel->name);
    ImGui::EndDisabled();
    HoverTip("Apply every value of the selected profile (values it lacks reset to their defaults).");
    ImGui::SameLine();
    ImGui::BeginDisabled(active.empty());
    if (ImGui::Button("Save", bs)) ImGui::OpenPopup("Save profile?");
    ImGui::EndDisabled();
    HoverTip("Overwrite the active profile with the current settings.");
    if (ImGui::Button("Save as...", bs)) {
        g_profName[0] = 0;
        g_profDesc[0] = 0;
        g_profPopupMsg.clear();
        ImGui::OpenPopup("Save profile as");
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!sel);
    if (ImGui::Button("Rename", bs) && sel) {
        CopyToBuf(g_profName, sizeof(g_profName), profiles::SanitizeName(sel->name));
        g_profPopupMsg.clear();
        ImGui::OpenPopup("Rename profile");
    }
    if (ImGui::Button("Delete", bs) && sel) ImGui::OpenPopup("Delete profile?");
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Open folder", bs) && h.openProfilesFolder) h.openProfilesFolder();
    HoverTip("Profiles are .json files: share one by copying its file. Put shared files in this folder;\n"
             "the list refreshes every time the overlay opens.");
    ImGui::PopStyleVar();

    unsigned long long tick = 0;
    const std::string status = profiles::Status(&tick);
    if (!status.empty()) {
        ImGui::Dummy(ImVec2(0, U(2)));
        ImGui::PushTextWrapPos(0.0f);
        const bool fresh = h.nowMs - tick < 4000;
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(fresh ? P.head : P.dim), "%s", status.c_str());
        ImGui::PopTextWrapPos();
    }

    // ---- popups (same ID stack as the OpenPopup calls above) ----
    const ImGuiWindowFlags popupFlags = ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings;
    const float fieldW = U(320);
    const ImVec4 warn = ImGui::ColorConvertU32ToFloat4(P.warn), err = ImGui::ColorConvertU32ToFloat4(P.err);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(U(18), U(16)));
    if (ImGui::BeginPopupModal("Save profile?", nullptr, popupFlags)) {
        ImGui::Text("Overwrite profile '%s' with the current settings?", active.c_str());
        if (ImGui::Button("Save")) {
            if (!active.empty()) profiles::SaveCurrentAs(active, activeInfo ? activeInfo->description : std::string(), true);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel") || PopupCancelled()) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopupModal("Save profile as", nullptr, popupFlags)) {
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        ImGui::SetNextItemWidth(fieldW);
        const bool enter = ImGui::InputText("Name", g_profName, sizeof(g_profName),
                                            ImGuiInputTextFlags_CallbackCharFilter | ImGuiInputTextFlags_EnterReturnsTrue,
                                            &ProfileNameFilter);
        ImGui::SetNextItemWidth(fieldW);
        ImGui::InputText("Description (optional)", g_profDesc, sizeof(g_profDesc));
        const std::string name = profiles::SanitizeName(g_profName);
        const profiles::Info* existing = name.empty() ? nullptr : FindProfile(list, name);
        if (name.empty())
            ImGui::TextDisabled("Letters, digits, space, - and _ (max 40).");
        else if (existing)
            ImGui::TextColored(warn, "'%s' exists and will be overwritten.", existing->name.c_str());
        else
            ImGui::TextDisabled("File: profiles\\%s.json", name.c_str());
        if (!g_profPopupMsg.empty()) ImGui::TextColored(err, "%s", g_profPopupMsg.c_str());
        ImGui::BeginDisabled(name.empty());
        // Replacing a file needs the explicit "Overwrite" click; Enter never confirms it.
        const bool doSave = ImGui::Button(existing ? "Overwrite" : "Save") || (enter && !existing && !name.empty());
        ImGui::EndDisabled();
        if (doSave) {
            auto r = profiles::SaveCurrentAs(name, g_profDesc, existing != nullptr);
            if (r.ok()) {
                g_profSel = r.name;
                ImGui::CloseCurrentPopup();
            } else {
                g_profPopupMsg = r.message;
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel") || PopupCancelled()) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopupModal("Rename profile", nullptr, popupFlags)) {
        ImGui::Text("Rename '%s' to:", g_profSel.c_str());
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        ImGui::SetNextItemWidth(fieldW);
        const bool enter = ImGui::InputText("##newname", g_profName, sizeof(g_profName),
                                            ImGuiInputTextFlags_CallbackCharFilter | ImGuiInputTextFlags_EnterReturnsTrue,
                                            &ProfileNameFilter);
        const std::string name = profiles::SanitizeName(g_profName);
        const profiles::Info* existing = name.empty() ? nullptr : FindProfile(list, name);
        const bool clash = existing && !SameName(existing->name, g_profSel);
        if (name.empty())
            ImGui::TextDisabled("Letters, digits, space, - and _ (max 40).");
        else if (clash)
            ImGui::TextColored(warn, "'%s' exists and will be overwritten.", existing->name.c_str());
        if (!g_profPopupMsg.empty()) ImGui::TextColored(err, "%s", g_profPopupMsg.c_str());
        ImGui::BeginDisabled(name.empty() || g_profSel.empty());
        const bool doRename = ImGui::Button(clash ? "Overwrite" : "Rename") || (enter && !clash && !name.empty());
        ImGui::EndDisabled();
        if (doRename && !g_profSel.empty()) {
            auto r = profiles::Rename(g_profSel, name, clash);
            if (r.ok()) {
                g_profSel = r.name;
                ImGui::CloseCurrentPopup();
            } else {
                g_profPopupMsg = r.message;
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel") || PopupCancelled()) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopupModal("Delete profile?", nullptr, popupFlags)) {
        ImGui::Text("Delete profile '%s'? Its file is removed.", g_profSel.c_str());
        if (ImGui::Button("Delete")) {
            if (!g_profSel.empty()) profiles::Delete(g_profSel);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel") || PopupCancelled()) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar();
    EndCard();
}

// -------------------------------------------------------------- sandbox ----
// Misc > Sandbox (module "sandbox"). Reads only the snapshot the module copies on
// the main thread (sandbox_bridge.h); every change is a posted request.
namespace sbx = ::sr2::ui::sandbox;

constexpr const char* kSbxPopup = "##sandbox_confirm";
bool g_sbxConfirm = false;   // open the "Enable sandbox?" dialog (set by the button or the tab switch)
int g_sbxSlot = 0;           // selected vacpack slot
int g_sbxAmount = 0;         // amount slider
int g_sbxAmountSlot = -1;    // slot the amount slider belongs to
long long g_sbxBucks = 1000;
item_grid::State g_vacGrid, g_spawnGrid, g_craftGrid;  // search text per grid
int g_spawnKind = -1;        // spawner filter (sbx::Kind as int, -1 = all)
int g_spawnCount = 1;
int g_craftCat = -1;         // crafter filter (index into recipeCategories, -1 = all)
int g_craftCount = 1;

std::string Thousands(long long v) {
    std::string s = std::to_string(v < 0 ? -v : v);
    for (int i = static_cast<int>(s.size()) - 3; i > 0; i -= 3) s.insert(static_cast<size_t>(i), ",");
    return v < 0 ? "-" + s : s;
}

bool AccentButton(const char* label, ImVec2 size) {
    const Palette& P = theme::Colors();
    ImGui::PushStyleColor(ImGuiCol_Button, P.accent);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Mix(P.accent, IM_COL32_WHITE, 0.14f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, Mix(P.accent, IM_COL32_BLACK, 0.12f));
    ImGui::PushStyleColor(ImGuiCol_Text, P.accentInk);
    const bool r = ImGui::Button(label, size);
    ImGui::PopStyleColor(4);
    return r;
}

void SbxRequest(sbx::Op op, long long value = 0, int slot = -1, uintptr_t item = 0) {
    sbx::Action a;
    a.op = op;
    a.value = value;
    a.slot = slot;
    a.item = item;
    sbx::Request(a);
}

void DimText(const std::string& text, float size = theme::kSmallSize) {
    FontScope f(theme::Body(), size);
    ImGui::PushStyleColor(ImGuiCol_Text, theme::Colors().dim);
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(text.c_str());
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
}

// "Enable sandbox?": the only way the overlay turns the module on. Cancel (or Esc) does nothing.
void SandboxConfirm(const ModuleView& mv) {
    const Palette& P = theme::Colors();
    if (g_sbxConfirm) {
        ImGui::OpenPopup(kSbxPopup);
        g_sbxConfirm = false;
    }
    const float w = U(430);
    ImGui::SetNextWindowPos(ImVec2((g_winMin.x + g_winMax.x) * 0.5f, (g_winMin.y + g_winMax.y) * 0.5f), ImGuiCond_Appearing,
                            ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(w, 0), ImVec2(w, FLT_MAX));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(U(22), U(18)));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(U(10), U(8)));
    ImGui::PushStyleColor(ImGuiCol_Border, P.cardBorder);
    const ImGuiWindowFlags f = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                               ImGuiWindowFlags_AlwaysAutoResize;
    if (ImGui::BeginPopupModal(kSbxPopup, nullptr, f)) {
        {
            FontScope fs(theme::Display(), theme::kDisplaySize + 4);
            ImGui::PushStyleColor(ImGuiCol_Text, theme::Current() == theme::Mode::Dark ? P.grab : P.accent);
            ImGui::TextUnformatted("Enable sandbox?");
            ImGui::PopStyleColor();
        }
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted("Sandbox edits change your save directly. Enable?");
        ImGui::PopTextWrapPos();
        DimText("Every edit goes through the game's own functions and is kept when the game next saves. "
                "There is no undo, so back up your save if you want to keep it as it is.");
        ImGui::Dummy(ImVec2(0, U(4)));
        const ImVec2 bs(U(112), U(34));
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - 2 * bs.x -
                             ImGui::GetStyle().ItemSpacing.x);
        if (ImGui::Button("Cancel", bs) || PopupCancelled()) ImGui::CloseCurrentPopup();
        ImGui::SameLine();
        if (AccentButton("Enable", bs)) {
            if (mv.hasEnabled) Write(mv.enabled, 1.0);
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
}

void SandboxNewbucks(const sbx::Snapshot& s, float w) {
    const Palette& P = theme::Colors();
    BeginCard("sbx_bucks", w);
    CardHeading("Newbucks", P.head);
    if (!s.haveNewbucks) {
        DimText("The Newbucks currency was not found.");
        EndCard();
        return;
    }
    {
        FontScope f(theme::Display(), theme::kDisplaySize + 12);
        ImGui::TextUnformatted(Thousands(s.newbucks).c_str());
    }
    DimText("Maximum " + Thousands(s.maxNewbucks));
    const long long max = std::max(0, s.maxNewbucks);
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputScalar("##bucks", ImGuiDataType_S64, &g_sbxBucks, nullptr, nullptr, "%lld");
    HoverTip("Amount for the buttons below.");
    g_sbxBucks = std::clamp(g_sbxBucks, 0LL, max);
    const float sp = ImGui::GetStyle().ItemSpacing.x;
    const ImVec2 bs(std::floor((ImGui::GetContentRegionAvail().x - 2 * sp) / 3), U(32));
    if (ImGui::Button("Set", bs)) SbxRequest(sbx::Op::SetNewbucks, g_sbxBucks);
    HoverTip("Set your Newbucks to this amount.");
    ImGui::SameLine();
    if (ImGui::Button("Add", bs)) SbxRequest(sbx::Op::AddNewbucks, g_sbxBucks);
    HoverTip("Add this amount (up to the maximum).");
    ImGui::SameLine();
    if (ImGui::Button("Subtract", bs)) SbxRequest(sbx::Op::AddNewbucks, -g_sbxBucks);
    HoverTip("Take this amount away (down to 0).");
    EndCard();
}

// Label, value and a control at the right edge (button or switch), on one row.
float SbxRowStart(const char* label) {
    const float x0 = ImGui::GetCursorPosX();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    return x0;
}

void SandboxPlayer(const sbx::Snapshot& s, float w) {
    const Palette& P = theme::Colors();
    BeginCard("sbx_player", w);
    CardHeading("Player", P.head);
    const float avail = ImGui::GetContentRegionAvail().x, bw = U(84);
    auto row = [&](const char* label, const std::string& value, const char* button, sbx::Op op, const char* tip) {
        ImGui::PushID(label);
        const float x0 = SbxRowStart(label);
        ImGui::SameLine(x0 + std::floor(avail * 0.40f));
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(P.soft), "%s", value.c_str());
        ImGui::SameLine(x0 + avail - bw);
        if (ImGui::Button(button, ImVec2(bw, 0))) SbxRequest(op);
        HoverTip(tip);
        ImGui::PopID();
    };
    row("Health", std::format("{} / {}", s.health, s.maxHealth), "Refill", sbx::Op::RefillHealth,
        "Set health to its maximum (PlayerState.SetHealth).");
    row("Energy", std::format("{} / {}", s.energy, s.maxEnergy), "Refill", sbx::Op::RefillEnergy,
        "Set energy to its maximum (PlayerState.SetEnergy).");
    row("Radiation", std::to_string(s.rads), "Clear", sbx::Op::ClearRads, "Set radiation to 0 (PlayerState.SetRad).");
    const float x0 = SbxRowStart("Infinite energy");
    ImGui::SameLine(x0 + avail - U(kSwitchW));
    if (Switch("##infenergy", s.infiniteEnergy)) SbxRequest(sbx::Op::InfiniteEnergy, s.infiniteEnergy ? 0 : 1);
    HoverTip("Keeps energy at its maximum. For this session only: off again after a restart or when Sandbox is "
             "turned off.");
    EndCard();
}

// Tile accent per item kind: a quiet "rarity" colour along the tile's bottom edge.
ImU32 KindAccent(sbx::Kind k) {
    switch (k) {
        case sbx::Kind::Slime: return IM_COL32(0xEC, 0x5A, 0xA0, 0xFF);
        case sbx::Kind::Largo: return IM_COL32(0xC8, 0x3C, 0xE6, 0xFF);
        case sbx::Kind::Plort: return IM_COL32(0xF0, 0xC2, 0x4B, 0xFF);
        case sbx::Kind::Food: return IM_COL32(0x7A, 0xCC, 0x5A, 0xFF);
        case sbx::Kind::Animal: return IM_COL32(0xE8, 0x89, 0x4A, 0xFF);
        case sbx::Kind::Toy: return IM_COL32(0x88, 0x47, 0xFF, 0xFF);
        case sbx::Kind::Resource: return IM_COL32(0x4B, 0x8B, 0xFF, 0xFF);
        case sbx::Kind::Liquid: return IM_COL32(0x5E, 0xC8, 0xF2, 0xFF);
        case sbx::Kind::Gadget: return IM_COL32(0x4F, 0xD1, 0xB4, 0xFF);
        case sbx::Kind::Other: break;
    }
    return IM_COL32(0xA8, 0xB4, 0xC8, 0xFF);
}

const char* KindLabel(sbx::Kind k) {
    switch (k) {
        case sbx::Kind::Slime: return "Slimes";
        case sbx::Kind::Largo: return "Largos";
        case sbx::Kind::Plort: return "Plorts";
        case sbx::Kind::Food: return "Food";
        case sbx::Kind::Animal: return "Animals";
        case sbx::Kind::Toy: return "Toys";
        case sbx::Kind::Resource: return "Resources";
        case sbx::Kind::Liquid: return "Liquids";
        case sbx::Kind::Gadget: return "Gadgets";
        case sbx::Kind::Other: break;
    }
    return "Other";
}

// Small toggle pill for a filter row; true when clicked.
bool Chip(const char* label, bool on) {
    const Palette& P = theme::Colors();
    FontScope f(theme::Body(), theme::kSmallSize);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(U(9), U(3)));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, U(10));
    ImGui::PushStyleColor(ImGuiCol_Button, on ? P.sel : P.frame);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, on ? P.sel : P.frameHover);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, P.sel);
    ImGui::PushStyleColor(ImGuiCol_Text, on ? P.text : P.soft);
    const bool r = ImGui::Button(label);
    ImGui::PopStyleColor(4);
    ImGui::PopStyleVar(2);
    return r;
}

// A wrapping row of chips: "All" (-1) and one per label. Returns the new selection.
int ChipRow(const char* id, const std::vector<std::pair<int, std::string>>& chips, int sel) {
    ImGui::PushID(id);
    const float avail = ImGui::GetContentRegionAvail().x, sp = U(6);
    float x = 0;
    auto place = [&](const char* label) {
        FontScope f(theme::Body(), theme::kSmallSize);
        const float w = ImGui::CalcTextSize(label).x + U(18);
        if (x > 0 && x + sp + w <= avail) ImGui::SameLine(0, sp), x += sp;
        else x = 0;
        x += w;
    };
    place("All");
    if (Chip("All", sel == -1)) sel = -1;
    for (const auto& [value, label] : chips) {
        ImGui::PushID(value);
        place(label.c_str());
        if (Chip(label.c_str(), sel == value)) sel = value;
        ImGui::PopID();
    }
    ImGui::PopID();
    return sel;
}

void SmallHeading(const char* text) {
    FontScope f(theme::Display(), theme::kSmallSize + 1);
    ImGui::PushStyleColor(ImGuiCol_Text, theme::Colors().soft);
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
}

// "How many" slider on one row with its label.
void CountRow(const char* label, const char* id, int& value, int max, const char* tip) {
    const float avail = ImGui::GetContentRegionAvail().x;
    const float x0 = SbxRowStart(label);
    const float sw = std::floor(avail * 0.58f);
    ImGui::SameLine(x0 + avail - sw);
    ImGui::SetNextItemWidth(sw);
    ImGui::SliderInt(id, &value, 1, max, "%d", ImGuiSliderFlags_AlwaysClamp);
    HoverTip(tip);
}

item_grid::Options GridOptions(float height) {
    item_grid::Options o;
    o.scale = g_scale;
    o.height = height;
    return o;
}

// Vacpack: the slots in a row across the top, the selected slot's amount
// controls, then a grid of what that slot accepts. Clicking an item fills the
// selected slot with it; dragging an item onto any slot fills that slot.
void SandboxVacpack(const sbx::Snapshot& s, float w) {
    const Palette& P = theme::Colors();
    BeginCard("sbx_vac", w);
    CardHeading("Vacpack", P.head);
    if (s.slots.empty()) {
        DimText("No vacpack slots found.");
        EndCard();
        return;
    }
    const int n = static_cast<int>(s.slots.size());
    g_sbxSlot = std::clamp(g_sbxSlot, 0, n - 1);
    if (!s.slots[static_cast<size_t>(g_sbxSlot)].unlocked)  // move off a locked slot
        for (int i = 0; i < n; ++i)
            if (s.slots[static_cast<size_t>(i)].unlocked) {
                g_sbxSlot = i;
                break;
            }

    // ---- slot row ----
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImFont* body = theme::Body();
    const float gap = U(8), avail = ImGui::GetContentRegionAvail().x;
    const float e = std::max(U(24), std::floor(std::min(U(92), (avail - gap * static_cast<float>(n - 1)) / static_cast<float>(n))));
    for (int i = 0; i < n; ++i) {
        const sbx::Slot& sl = s.slots[static_cast<size_t>(i)];
        ImGui::PushID(i);
        if (i > 0) ImGui::SameLine(0, gap);
        if (ImGui::InvisibleButton("##slot", ImVec2(e, e)) && sl.unlocked) g_sbxSlot = i;
        const bool hot = ImGui::IsItemHovered() && sl.unlocked;
        const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
        if (sl.unlocked && ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(item_grid::kDragType);
                p && p->DataSize == sizeof(uintptr_t)) {
                g_sbxSlot = i;
                SbxRequest(sbx::Op::PutItem, sl.max > 0 ? sl.max : 1, i, *static_cast<const uintptr_t*>(p->Data));
            }
            ImGui::EndDragDropTarget();
        }
        item_grid::Tile t;
        t.id = sl.item;
        t.name = sl.item ? &sl.name : nullptr;
        t.icon = sl.item ? sl.icon : 0;
        t.accent = sl.item ? KindAccent(sl.kind) : 0;
        t.dimmed = !sl.unlocked;
        item_grid::DrawTile(dl, a, b, t, hot, sl.unlocked && i == g_sbxSlot, g_scale);
        const float fs = theme::kSmallSize;
        Text(dl, body, fs, ImVec2(a.x + U(6), a.y + U(4)), P.dim, std::to_string(i + 1).c_str());
        const char* mid = !sl.unlocked ? "Locked" : !sl.item ? "Empty" : nullptr;
        if (mid) {
            const ImVec2 ts = TextSize(body, fs, mid);
            Text(dl, body, fs, ImVec2((a.x + b.x - ts.x) * 0.5f, (a.y + b.y - ts.y) * 0.5f), P.dim, mid);
        }
        if (sl.unlocked && sl.item) {
            const std::string c = std::format("{}/{}", sl.count, sl.max);
            const ImVec2 cs = TextSize(body, fs, c.c_str());
            const ImVec2 cp(b.x - U(6) - cs.x, b.y - U(6) - cs.y);
            Text(dl, body, fs, ImVec2(cp.x + 1, cp.y + 1), IM_COL32(0, 0, 0, 150), c.c_str());
            Text(dl, body, fs, cp, P.text, c.c_str());
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
            Tooltip(!sl.unlocked ? std::format("Slot {} is locked. Vacpack upgrades unlock it.", i + 1)
                    : sl.item    ? std::format("Slot {}: {} ({} / {})", i + 1, sl.name, sl.count, sl.max)
                                 : std::format("Slot {} is empty. Click an item below to fill it.", i + 1));
        ImGui::PopID();
    }

    const sbx::Slot& sl = s.slots[static_cast<size_t>(g_sbxSlot)];
    if (!sl.unlocked) {
        DimText("Every slot is locked. Vacpack upgrades unlock them.");
        EndCard();
        return;
    }

    // ---- selected slot: amount ----
    ImGui::Dummy(ImVec2(0, U(2)));
    if (g_sbxAmountSlot != g_sbxSlot) {
        g_sbxAmountSlot = g_sbxSlot;
        g_sbxAmount = sl.item ? sl.count : sl.max;
    }
    const int max = std::max(0, sl.max);
    g_sbxAmount = std::clamp(g_sbxAmount, 0, max);
    {
        const float row = ImGui::GetContentRegionAvail().x, sp = ImGui::GetStyle().ItemSpacing.x;
        const float bw = std::floor(std::min(U(104), (row * 0.5f - 2 * sp) / 3));
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(std::format("Slot {}", g_sbxSlot + 1).c_str());
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(sl.item ? P.soft : P.dim), "%s", sl.item ? sl.name.c_str() : "Empty");
        ImGui::SameLine(0, U(14));
        const float sw = std::max(U(80), ImGui::GetContentRegionAvail().x - 3 * bw - 3 * sp);
        ImGui::SetNextItemWidth(sw);
        ImGui::SliderInt("##amount", &g_sbxAmount, 0, max, "%d", ImGuiSliderFlags_AlwaysClamp);
        HoverTip("Ctrl+click to type. Used by Set.");
        ImGui::BeginDisabled(!sl.item);
        ImGui::SameLine();
        if (ImGui::Button("Set", ImVec2(bw, 0))) SbxRequest(sbx::Op::SetSlotCount, g_sbxAmount, g_sbxSlot);
        HoverTip("Change how many of the current item the slot holds (0 empties it).");
        ImGui::SameLine();
        ImGui::BeginDisabled(sl.count >= max);
        if (ImGui::Button("Fill", ImVec2(bw, 0))) SbxRequest(sbx::Op::SetSlotCount, max, g_sbxSlot);
        ImGui::EndDisabled();
        HoverTip("Fill the slot to its maximum.");
        ImGui::SameLine();
        if (ImGui::Button("Empty", ImVec2(bw, 0))) SbxRequest(sbx::Op::SetSlotCount, 0, g_sbxSlot);
        HoverTip("Remove everything from this slot.");
        ImGui::EndDisabled();
    }

    // ---- items the selected slot accepts ----
    ImGui::Dummy(ImVec2(0, U(2)));
    SmallHeading("Click an item to fill the selected slot, or drag it onto any slot");
    const uint32_t bit = g_sbxSlot < 32 ? (1u << g_sbxSlot) : 0u;
    std::vector<item_grid::Tile> tiles;
    if (s.catalog) {
        tiles.reserve(s.catalog->size());
        for (const sbx::Item& it : *s.catalog) {
            if (!(it.slotMask & bit)) continue;
            item_grid::Tile t;
            t.id = it.id;
            t.name = &it.name;
            t.icon = it.icon;
            t.accent = KindAccent(it.kind);
            tiles.push_back(t);
        }
    }
    item_grid::Options o = GridOptions(212);
    o.selected = sl.item;
    o.drag = true;
    o.empty = s.catalog ? "No matching items." : "Reading the item list...";
    const item_grid::Result r = item_grid::Draw("vac", tiles, g_vacGrid, o);
    if (r.hovered >= 0 && !ImGui::GetDragDropPayload())
        Tooltip(std::format("{}\nClick: fill slot {} ({} max). Drag onto a slot to fill that one.",
                            *tiles[static_cast<size_t>(r.hovered)].name, g_sbxSlot + 1, max));
    if (r.clicked >= 0) SbxRequest(sbx::Op::PutItem, max > 0 ? max : 1, g_sbxSlot, tiles[static_cast<size_t>(r.clicked)].id);
    EndCard();
}

// Spawner: places any actor the game can spawn (toys, slimes, food, ...) in front of you.
void SandboxSpawner(const sbx::Snapshot& s, float w) {
    const Palette& P = theme::Colors();
    BeginCard("sbx_spawn", w);
    CardHeading("Spawner", P.head);
    if (!s.spawnWhy.empty()) {
        DimText("Unavailable: " + s.spawnWhy);
        EndCard();
        return;
    }
    if (!s.spawnables) {
        DimText("Reading the spawn list...");
        EndCard();
        return;
    }
    // Filter chips: the kinds present in the list, in enum order.
    std::vector<std::pair<int, std::string>> chips;
    {
        bool have[16] = {};
        for (const sbx::Item& it : *s.spawnables) have[static_cast<int>(it.kind) & 15] = true;
        for (int k = 1; k < 16; ++k)
            if (have[k]) chips.emplace_back(k, KindLabel(static_cast<sbx::Kind>(k)));
        if (have[0]) chips.emplace_back(0, KindLabel(sbx::Kind::Other));
    }
    g_spawnKind = ChipRow("kinds", chips, g_spawnKind);
    CountRow("How many", "##spawncount", g_spawnCount, 20, "How many to spawn per click.");
    std::vector<item_grid::Tile> tiles;
    tiles.reserve(s.spawnables->size());
    std::vector<const sbx::Item*> src;
    for (const sbx::Item& it : *s.spawnables) {
        if (g_spawnKind >= 0 && static_cast<int>(it.kind) != g_spawnKind) continue;
        item_grid::Tile t;
        t.id = it.id;
        t.name = &it.name;
        t.icon = it.icon;
        t.accent = KindAccent(it.kind);
        t.dimmed = it.tarr && s.tarrsOff;
        tiles.push_back(t);
        src.push_back(&it);
    }
    const item_grid::Result r = item_grid::Draw("spawn", tiles, g_spawnGrid, GridOptions(250));
    if (r.hovered >= 0) {
        const sbx::Item& it = *src[static_cast<size_t>(r.hovered)];
        if (it.tarr && s.tarrsOff)
            Tooltip(std::format("{}\nTarrs are disabled in this save's game settings.", it.name));
        else
            Tooltip(std::format("{}\n{}. Click to spawn {} in front of you.", it.name, KindLabel(it.kind), g_spawnCount));
    }
    if (r.clicked >= 0) {
        const sbx::Item& it = *src[static_cast<size_t>(r.clicked)];
        if (it.tarr && s.tarrsOff) sbx::SetStatus(it.name + " not spawned: Tarrs are disabled in this save's game settings", true);
        else SbxRequest(sbx::Op::Spawn, g_spawnCount, -1, it.id);
    }
    EndCard();
}

// Crafter: the fabricator's gadget recipes, added to your gadget inventory at no cost.
void SandboxCrafter(const sbx::Snapshot& s, float w) {
    const Palette& P = theme::Colors();
    BeginCard("sbx_craft", w);
    CardHeading("Crafter", P.head);
    if (!s.craftWhy.empty()) {
        DimText("Unavailable: " + s.craftWhy);
        EndCard();
        return;
    }
    if (!s.recipes) {
        DimText("Reading the fabricator recipes...");
        EndCard();
        return;
    }
    std::vector<std::pair<int, std::string>> chips;
    if (s.recipeCategories) {
        std::vector<bool> used(s.recipeCategories->size(), false);
        for (const sbx::Recipe& r : *s.recipes)
            if (r.category >= 0 && static_cast<size_t>(r.category) < used.size()) used[static_cast<size_t>(r.category)] = true;
        for (size_t i = 0; i < used.size(); ++i)
            if (used[i]) chips.emplace_back(static_cast<int>(i), (*s.recipeCategories)[i]);
    }
    if (!chips.empty()) g_craftCat = ChipRow("cats", chips, g_craftCat);
    else g_craftCat = -1;
    CountRow("How many", "##craftcount", g_craftCount, 20, "How many to craft per click.");
    std::vector<item_grid::Tile> tiles;
    std::vector<const sbx::Recipe*> src;
    tiles.reserve(s.recipes->size());
    for (const sbx::Recipe& rc : *s.recipes) {
        if (g_craftCat >= 0 && rc.category != g_craftCat) continue;
        item_grid::Tile t;
        t.id = rc.id;
        t.name = &rc.name;
        t.icon = rc.icon;
        t.accent = KindAccent(sbx::Kind::Gadget);
        t.dimmed = !rc.unlocked;
        t.badge = rc.owned;
        tiles.push_back(t);
        src.push_back(&rc);
    }
    item_grid::Options o = GridOptions(250);
    o.hint = "Search gadgets...";
    const item_grid::Result r = item_grid::Draw("craft", tiles, g_craftGrid, o);
    if (r.hovered >= 0) {
        const sbx::Recipe& rc = *src[static_cast<size_t>(r.hovered)];
        std::string tip = rc.name;
        if (!rc.cost.empty()) tip += "\nFabricator cost: " + rc.cost;
        tip += std::format("\nIn your inventory: {}", rc.owned);
        if (!rc.unlocked) tip += "\nBlueprint not unlocked yet.";
        else
            tip += std::format("\nClick to craft {} for free{}.", g_craftCount * rc.perCraft,
                               rc.perCraft > 1 ? " (made in pairs)" : "");
        Tooltip(tip);
    }
    if (r.clicked >= 0) {
        const sbx::Recipe& rc = *src[static_cast<size_t>(r.clicked)];
        if (rc.unlocked) SbxRequest(sbx::Op::Craft, g_craftCount, -1, rc.id);
        else sbx::SetStatus(rc.name + " not crafted: its blueprint is not unlocked", true);
    }
    EndCard();
}

void SandboxStatus(const Host& h) {
    const sbx::Status st = sbx::GetStatus();
    if (st.text.empty()) return;
    const Palette& P = theme::Colors();
    const bool fresh = h.nowMs - st.tick < 6000;
    ImGui::Dummy(ImVec2(0, U(2)));
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(st.error ? P.err : fresh ? P.head : P.dim), "%s", st.text.c_str());
    ImGui::PopTextWrapPos();
}

void DrawSandboxPage(const Host& h, const ModuleView& mv) {
    const Palette& P = theme::Colors();
    FailedBanner(mv);
    SandboxConfirm(mv);
    if (!ModuleOn(mv)) {
        BeginCard("sbx_off", ImGui::GetContentRegionAvail().x);
        CardHeading("Sandbox", P.head);
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted("Edit your character's resources: set your Newbucks, refill health and energy, "
                               "choose what each vacpack slot holds, spawn items in front of you and craft "
                               "fabricator gadgets for free.");
        ImGui::PopTextWrapPos();
        DimText("Edits use the game's own functions, so the save stays valid, but they change your save directly and "
                "can't be undone. Sandbox stays off until you enable it and confirm.");
        DimText("Achievements are paused while Sandbox is on.");
        ImGui::Dummy(ImVec2(0, U(4)));
        if (AccentButton("Enable sandbox", ImVec2(U(170), U(34)))) g_sbxConfirm = true;
        EndCard();
        return;
    }
    sbx::MarkViewed();
    const auto snap = sbx::Get();
    if (!snap || h.nowMs - snap->tick > 3000) {
        OffNote("Reading your character...");
        return;
    }
    if (!snap->inGame) {
        OffNote(snap->why.empty() ? std::string("Load a save to use the sandbox.") : snap->why);
        SandboxStatus(h);
        return;
    }
    // Newbucks | Player, the vacpack across both columns, then Spawner | Crafter.
    const float full = ImGui::GetContentRegionAvail().x, w = ColumnWidth();
    const float gapY = U(kGap) - ImGui::GetStyle().ItemSpacing.y;
    SandboxNewbucks(*snap, w);
    ImGui::SameLine(0, U(kGap));
    SandboxPlayer(*snap, w);
    ImGui::Dummy(ImVec2(0, gapY));
    SandboxVacpack(*snap, full);
    ImGui::Dummy(ImVec2(0, gapY));
    SandboxSpawner(*snap, w);
    ImGui::SameLine(0, U(kGap));
    SandboxCrafter(*snap, w);
    DimText("Achievements are paused while Sandbox is on.");
    SandboxStatus(h);
}

// ------------------------------------------------------------- settings ----
// Key on the left, value right-aligned; a long value wraps, each line right-aligned.
void KeyValue(const char* k, const std::string& v, ImU32 vcol, const std::string& tip = {}) {
    const float avail = ImGui::GetContentRegionAvail().x, x0 = ImGui::GetCursorPosX();
    const float keyW = std::min(U(130), avail * 0.45f);
    const float valW = std::max(avail - keyW, U(40));
    ImFont* f = ImGui::GetFont();
    const float fs = ImGui::GetFontSize();
    ImGui::BeginGroup();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(k);
    ImGui::PushStyleColor(ImGuiCol_Text, vcol);
    const char* s = v.c_str();
    const char* const end = s + v.size();
    for (bool first = true; s < end; first = false) {
        const char* e = f->CalcWordWrapPosition(fs, s, end, valW);
        if (e <= s) {  // a single glyph wider than the column: take it anyway
            e = s + 1;
            while (e < end && (static_cast<unsigned char>(*e) & 0xC0) == 0x80) ++e;
        }
        const char* le = e;
        while (le > s && (le[-1] == ' ' || le[-1] == '\n')) --le;
        const float tw = ImGui::CalcTextSize(s, le).x;
        if (first) {
            ImGui::SameLine(x0 + avail - tw);
            ImGui::AlignTextToFramePadding();
        } else {
            ImGui::SetCursorPosX(x0 + avail - tw);
        }
        ImGui::TextUnformatted(s, le);
        s = e;
        while (s < end && (*s == ' ' || *s == '\n')) ++s;
    }
    ImGui::PopStyleColor();
    ImGui::EndGroup();
    HoverTip(tip);
}

void DrawSettingsPage(const Host& h, const ModuleView* ui) {
    const Palette& P = theme::Colors();
    const float w = ColumnWidth();
    BeginCard("overlay", w);
    CardHeading("Overlay", P.head);
    if (ui)
        for (const auto& g : ui->groups)
            for (const auto& d : g.defs)
                if (d.key != kKeyOverlayTheme) DrawRow(h, d, d.key == kKeyOverlayHint ? "Show hint at startup" : d.label, false);
    EndCard();
    ImGui::SameLine(0, U(kGap));
    BeginCard("status", w);
    CardHeading("Status", P.head);
    KeyValue("Options menu tab", OptionsTabStatus(), P.dim);
    // A known build is one green line (details on hover); anything else shows the full status,
    // a mismatch as a warning.
    if (h.versionKnown)
        KeyValue("Game build", "matches known build", P.ok, h.versionStatus);
    else
        KeyValue("Game build", h.versionStatus, h.versionMismatch ? P.warn : P.dim);
    EndCard();
}

// -------------------------------------------------------------- sidebar ----
const settings::Def* ThemeDef(const ModuleView* ui) { return ui ? FindDef(*ui, kKeyOverlayTheme) : nullptr; }

void SetTheme(const ModuleView* ui, theme::Mode m) {
    const settings::Def* d = ThemeDef(ui);
    if (!d || static_cast<int>(std::lround(Read(*d))) == static_cast<int>(m)) return;
    Write(*d, static_cast<double>(m));
    g_peel.state = 1;  // the overlay copies this frame (old theme) after rendering it
    g_peel.min = g_winMin;
    g_peel.max = g_winMax;
}

bool NavItem(const Category& c, float w) {
    const Palette& P = theme::Colors();
    const bool selected = g_cat == c.id;
    const bool clicked = ImGui::InvisibleButton(c.id.c_str(), ImVec2(w, U(kNavH)));
    const bool hovered = ImGui::IsItemHovered();
    const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (selected || hovered) dl->AddRectFilled(a, b, selected ? P.accent : P.hover, U(12));
    const ImU32 col = selected ? P.accentInk : (hovered ? P.text : P.soft);
    DrawIcon(dl, c.icon, ImVec2(a.x + U(12), (a.y + b.y) * 0.5f - U(9)), col);
    ImFont* f = theme::Display();
    const float fs = U(theme::kDisplaySize);
    Text(dl, f, theme::kDisplaySize, ImVec2(a.x + U(42), (a.y + b.y) * 0.5f - fs * 0.55f), col, c.title.c_str());
    return clicked;
}

void ThemeTabs(const ModuleView* ui, float w) {
    const Palette& P = theme::Colors();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 a = ImGui::GetCursorScreenPos(), b(a.x + w, a.y + U(36));
    dl->AddRectFilled(a, b, P.frame, U(12));
    const float half = (w - U(9)) * 0.5f;
    const theme::Mode cur = theme::Current();
    for (int i = 0; i < 2; ++i) {
        const theme::Mode m = static_cast<theme::Mode>(i);
        const ImVec2 p(a.x + U(3) + static_cast<float>(i) * (half + U(3)), a.y + U(3));
        ImGui::SetCursorScreenPos(p);
        ImGui::PushID(i);
        if (ImGui::InvisibleButton("##theme", ImVec2(half, U(30)))) SetTheme(ui, m);
        const bool hovered = ImGui::IsItemHovered();
        ImGui::PopID();
        const bool sel = cur == m;
        const ImU32 bg = sel ? P.tabSel : P.frame;
        if (sel) dl->AddRectFilled(p, ImVec2(p.x + half, p.y + U(30)), bg, U(9));
        const ImU32 col = sel ? P.tabInk : (hovered ? P.text : P.soft);
        const char* label = i == 0 ? "Dark" : "Light";
        ImFont* f = theme::Display();
        const ImVec2 ts = TextSize(f, 14, label);
        const float iconW = U(14), gap = U(6);
        const float x = p.x + (half - (iconW + gap + ts.x)) * 0.5f, cy = p.y + U(15);
        if (i == 0) {
            dl->AddCircleFilled(ImVec2(x + U(7), cy), U(6), col);
            dl->AddCircleFilled(ImVec2(x + U(10), cy - U(2.5f)), U(5), bg);
        } else {
            dl->AddCircleFilled(ImVec2(x + U(7), cy), U(3), col);
            for (int r = 0; r < 8; ++r) {
                const float an = static_cast<float>(r) * kPi / 4;
                dl->AddLine(ImVec2(x + U(7) + std::cos(an) * U(4.6f), cy + std::sin(an) * U(4.6f)),
                            ImVec2(x + U(7) + std::cos(an) * U(6.8f), cy + std::sin(an) * U(6.8f)), col, U(1.6f));
            }
        }
        Text(dl, f, 14, ImVec2(x + iconW + gap, cy - ts.y * 0.5f), col, label);
    }
    ImGui::SetCursorScreenPos(a);
    ImGui::Dummy(ImVec2(w, U(36)));
}

void DrawSidebar(const Host& h, const std::vector<Category>& cats, const ModuleView* ui, float height) {
    const Palette& P = theme::Colors();
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(U(12), U(14)));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(U(8), U(4)));
    ImGui::BeginChild("##side", ImVec2(U(kSideW), height), ImGuiChildFlags_AlwaysUseWindowPadding,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoBackground);
    const float w = ImGui::GetContentRegionAvail().x;
    ImDrawList* dl = ImGui::GetWindowDrawList();

    const ImVec2 la = ImGui::GetCursorScreenPos();
    const ImVec2 lmin(la.x + w * 0.5f - U(66), la.y + U(2)), lmax(la.x + w * 0.5f + U(66), la.y + U(112));
    if (!slime_icons::Draw(dl, lmin, lmax)) DrawSlimeFallback(dl, lmin, lmax);
    ImGui::Dummy(ImVec2(w, U(124)));
    if (const slime_icons::Entry* slime = slime_icons::CurrentEntry(); slime && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
        EntryTooltip(*slime);

    size_t bottomCount = 0;
    for (const auto& c : cats) {
        if (c.bottom) {
            ++bottomCount;
            continue;
        }
        if (NavItem(c, w)) g_cat = c.id;
    }
    const float bottomH = static_cast<float>(bottomCount) * (U(kNavH) + U(4)) + U(8) + U(36) + U(30);
    const float y = ImGui::GetWindowHeight() - U(14) - bottomH;
    if (ImGui::GetCursorPosY() < y) ImGui::SetCursorPosY(y);
    for (const auto& c : cats)
        if (c.bottom && NavItem(c, w)) g_cat = c.id;
    ImGui::Dummy(ImVec2(0, U(4)));
    ThemeTabs(ui, w);

    // Save state as a dot (words in its tooltip), then the DLL's build stamp.
    const unsigned long long last = LastWriteTick();
    const bool busy = last != 0 && h.nowMs - last < 1500;
    const std::string saveErr = busy ? std::string() : settings::LastSaveError();
    const ImU32 col = busy ? P.warn : !saveErr.empty() ? P.err : P.ok;
    const ImVec2 sp = Add(ImGui::GetCursorScreenPos(), ImVec2(U(12), U(14)));  // sp.y = text centre line
    ImGui::SetCursorScreenPos(ImVec2(sp.x - U(4), sp.y - U(8)));
    ImGui::InvisibleButton("##savestate", ImVec2(U(16), U(16)));
    HoverTip(busy ? "Saving changes to sr2mods.json..."
             : !saveErr.empty() ? "Saving sr2mods.json failed: " + saveErr
                                : "All changes saved to sr2mods.json");
    dl->AddCircleFilled(ImVec2(sp.x + U(4), sp.y), U(4), col);

    // "dd/MM HH:mm · abc1234[-dirty]", both parts' digit ink centred on the dot.
    // The commit is a size smaller and shrinks further (then "-dirty" -> "*")
    // until the line fits the sidebar.
    ImFont* bf = theme::Body();
    const char* stamp = ::sr2::core::build_info::Time();
    std::string commit = ::sr2::core::build_info::Commit();
    const float x0 = sp.x + U(14), right = ImGui::GetWindowPos().x + ImGui::GetWindowWidth() - U(12);
    auto inkMid = [&](float size) {  // y of the digits' ink centre below the line top
        const ImFontGlyph* g = bf->GetFontBaked(U(size))->FindGlyph('0');
        return g ? (g->Y0 + g->Y1) * 0.5f : U(size) * 0.5f;
    };
    const ImVec2 ts = TextSize(bf, theme::kSmallSize, stamp);
    std::string tail;
    float tailSize = theme::kSmallSize - 1;
    if (!commit.empty()) {
        for (;;) {
            tail = std::string(" \xC2\xB7 ") + commit;
            if (x0 + ts.x + TextSize(bf, tailSize, tail.c_str()).x <= right) break;
            if (tailSize > theme::kSmallSize - 3) tailSize -= 1;
            else if (const size_t d = commit.find("-dirty"); d != std::string::npos) commit.replace(d, 6, "*");
            else break;
        }
    }
    const ImVec2 tp(x0, sp.y - inkMid(theme::kSmallSize));
    const ImU32 stampCol = Mix(P.dim, P.side, 0.4f);  // grayer than other dim text: only a reference
    Text(dl, bf, theme::kSmallSize, tp, stampCol, stamp);
    float lineW = ts.x;
    if (!tail.empty()) {
        Text(dl, bf, tailSize, ImVec2(x0 + ts.x, sp.y - inkMid(tailSize)), stampCol, tail.c_str());
        lineW += TextSize(bf, tailSize, tail.c_str()).x;
    }
    if (lineW > 0) {
        // Click copies the full commit ("abc1234" or "abc1234-dirty", never the
        // shortened "*" form) to the Windows clipboard (ImGui's Win32 default
        // handler); without a commit, the ISO build time. "copied!" shows ~1.5 s.
        static unsigned long long copiedUntil = 0;
        ImGui::SetCursorScreenPos(ImVec2(x0, sp.y - U(8)));
        if (ImGui::InvisibleButton("##buildstamp", ImVec2(lineW, U(16)))) {
            const char* full = ::sr2::core::build_info::Commit();
            ImGui::SetClipboardText(*full ? full : ::sr2::core::build_info::TimeIso());
            copiedUntil = h.nowMs + 1500;
        }
        if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        if (copiedUntil != 0 && h.nowMs < copiedUntil) {
            Tooltip("copied!");
        } else {
            copiedUntil = 0;
            HoverTip(*::sr2::core::build_info::Commit() ? "Click to copy the commit" : "Click to copy the build time");
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
}

// --------------------------------------------------------------- top bar ----
bool IconButton(const char* id, float size) {
    const bool clicked = ImGui::InvisibleButton(id, ImVec2(size, size));
    if (ImGui::IsItemHovered())
        ImGui::GetWindowDrawList()->AddRectFilled(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), theme::Colors().frameHover,
                                                  U(10));
    return clicked;
}

struct TabState {
    bool on = true;            // module (and master) enabled
    const settings::Def* sw = nullptr;  // the tab's switch
    bool locked = false;       // switch unusable (master tab whose module is off, or pinned by its module)
    std::string lock;          // why the module pinned its switch (settings::LockReason)
};

TabState StateOf(const Tab& t, const std::vector<ModuleView>& views) {
    TabState s;
    if (t.kind != TabKind::Module && t.kind != TabKind::Sandbox) return s;
    const ModuleView* mv = FindView(views, t.module);
    if (!mv) return s;
    const bool moduleOn = ModuleOn(*mv);
    if (t.masterKey.empty()) {
        s.sw = mv->hasEnabled ? &mv->enabled : nullptr;
        s.on = moduleOn;
        s.lock = settings::LockReason(t.module, "enabled");
        s.locked = !s.lock.empty();
    } else {
        s.sw = FindDef(*mv, t.masterKey);
        s.locked = !moduleOn;
        s.on = moduleOn && (!s.sw || IsOn(*s.sw));
    }
    return s;
}

// Defaults for everything the tab shows (switches stay as they are).
void ResetTab(const Tab& t, const std::vector<ModuleView>& views) {
    auto resetModule = [&](const ModuleView& mv, const Tab& filter) {
        for (const auto& g : mv.groups) {
            if (!filter.onlyGroups.empty() &&
                std::find(filter.onlyGroups.begin(), filter.onlyGroups.end(), g.name) == filter.onlyGroups.end())
                continue;
            if (std::find(filter.skipGroups.begin(), filter.skipGroups.end(), g.name) != filter.skipGroups.end()) continue;
            for (const auto& d : g.defs)
                if (d.key != filter.masterKey && std::fabs(Read(d) - d.def) > 1e-9) Write(d, d.def);
        }
    };
    if (const ModuleView* mv = FindView(views, t.module)) resetModule(*mv, t);
    for (const auto& n : t.nested)
        if (const ModuleView* nmv = FindView(views, n)) resetModule(*nmv, Tab{});
}

// Returns true when the close button was clicked.
bool DrawTopBar(const Category& cat, int& ti, const std::vector<ModuleView>& views) {
    const Palette& P = theme::Colors();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float barW = ImGui::GetContentRegionAvail().x;
    ImFont* f = theme::Display();

    // Tabs.
    const float stripH = U(40), tabH = U(32), cy = origin.y + U(kTopH) * 0.5f;
    float stripW = U(8) + static_cast<float>(cat.tabs.size() - 1) * U(4);
    for (const auto& t : cat.tabs) stripW += TextSize(f, 15, t.title.c_str()).x + U(26);
    const ImVec2 sa(origin.x + U(18), cy - stripH * 0.5f);
    dl->AddRectFilled(sa, ImVec2(sa.x + stripW, sa.y + stripH), P.side, U(13));
    float x = sa.x + U(4);
    for (int i = 0; i < static_cast<int>(cat.tabs.size()); ++i) {
        const Tab& t = cat.tabs[static_cast<size_t>(i)];
        const float tw = TextSize(f, 15, t.title.c_str()).x + U(26);
        ImGui::SetCursorScreenPos(ImVec2(x, cy - tabH * 0.5f));
        ImGui::PushID(i);
        if (ImGui::InvisibleButton("##tab", ImVec2(tw, tabH))) ti = i;
        const bool hovered = ImGui::IsItemHovered();
        ImGui::PopID();
        const bool sel = i == ti;
        const bool on = StateOf(t, views).on;
        ImU32 bg = 0, ink = P.soft;
        if (sel) bg = on ? P.tabSel : P.offBg, ink = on ? P.tabInk : P.offInk;
        else if (!on) ink = P.offInk;
        else if (hovered) bg = P.hover, ink = P.text;
        const ImVec2 a(x, cy - tabH * 0.5f);
        if (bg) dl->AddRectFilled(a, ImVec2(a.x + tw, a.y + tabH), bg, U(10));
        const ImVec2 ts = TextSize(f, 15, t.title.c_str());
        Text(dl, f, 15, ImVec2(a.x + U(13), cy - ts.y * 0.5f), ink, t.title.c_str());
        x += tw + U(4);
    }

    // Right side, from the right edge: close, (?), switch, reset/refresh.
    const Tab& tab = cat.tabs[static_cast<size_t>(ti)];
    float rx = origin.x + barW - U(14);
    const float ib = U(34);
    rx -= ib;
    ImGui::SetCursorScreenPos(ImVec2(rx, cy - ib * 0.5f));
    const bool closeClicked = IconButton("##close", ib);
    DrawCross(dl, ImVec2(rx + ib * 0.5f, cy), ImGui::IsItemHovered() ? P.text : P.soft);

    const TabState st = StateOf(tab, views);
    if (st.sw) {
        if (!tab.help.empty()) {
            const float hs = U(22);
            rx -= hs + U(10);
            ImGui::SetCursorScreenPos(ImVec2(rx, cy - hs * 0.5f));
            HelpMarker("##help", hs, tab.help);
        }
        rx -= U(kSwitchW) + U(8);
        const float fh = ImGui::GetFrameHeight();
        ImGui::SetCursorScreenPos(ImVec2(rx, cy - fh * 0.5f));
        ImGui::BeginDisabled(st.locked);
        const bool on = IsOn(*st.sw);
        if (Switch("##enable", on)) {
            // Sandbox: turning it on goes through the confirmation dialog (drawn by its page).
            if (tab.kind == TabKind::Sandbox && !on) g_sbxConfirm = true;
            else Write(*st.sw, on ? 0.0 : 1.0);
        }
        ImGui::EndDisabled();
        HoverTip(!st.lock.empty() ? st.lock : st.locked ? tab.help : st.sw->tooltip);
        const ImVec2 ls = TextSize(f, 15, "Enabled");
        rx -= ls.x + U(9);
        Text(dl, f, 15, ImVec2(rx, cy - ls.y * 0.5f), WithAlpha(P.soft, st.locked ? 0.45f : 1.0f), "Enabled");
    }
    if (tab.kind == TabKind::Module || tab.kind == TabKind::Profiles) {
        rx -= ib + U(10);
        ImGui::SetCursorScreenPos(ImVec2(rx, cy - ib * 0.5f));
        if (IconButton("##reset", ib)) {
            if (tab.kind == TabKind::Profiles) profiles::Refresh();
            else ResetTab(tab, views);
        }
        DrawResetIcon(dl, ImVec2(rx + ib * 0.5f, cy), ImGui::IsItemHovered() ? P.text : P.soft);
        HoverTip(tab.kind == TabKind::Profiles ? "Rescan the profiles folder" : "Reset this tab to defaults");
    }
    ImGui::SetCursorScreenPos(origin);
    ImGui::Dummy(ImVec2(barW, U(kTopH)));
    return closeClicked;
}

// ----------------------------------------------------------------- peel ----
using Poly = std::vector<ImVec2>;

// Keeps the part of `in` where (x-o.x)/s.x + (y-o.y)/s.y is <= limit (below) or >= limit.
Poly ClipDiagonal(const Poly& in, ImVec2 o, ImVec2 s, float limit, bool below) {
    auto f = [&](ImVec2 v) {
        const float d = (v.x - o.x) / s.x + (v.y - o.y) / s.y - limit;
        return below ? d : -d;
    };
    Poly out;
    for (size_t i = 0; i < in.size(); ++i) {
        const ImVec2 a = in[i], b = in[(i + 1) % in.size()];
        const float fa = f(a), fb = f(b);
        if (fa <= 0) out.push_back(a);
        if ((fa <= 0) != (fb <= 0)) {
            const float t = fa / (fa - fb);
            out.push_back(ImVec2(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t));
        }
    }
    return out;
}

// The window's outline: a rectangle with rounded corners (convex, so the
// diagonal clip and the fan fill below stay valid). The snapshot outside it is
// the game behind the window, which must not show as square corners.
Poly RoundedRect(ImVec2 a, ImVec2 b, float r) {
    r = std::clamp(r, 0.0f, std::min(b.x - a.x, b.y - a.y) * 0.5f);
    if (r < 0.5f) return {a, ImVec2(b.x, a.y), b, ImVec2(a.x, b.y)};
    constexpr int kSeg = 8;
    const ImVec2 centres[4] = {ImVec2(a.x + r, a.y + r), ImVec2(b.x - r, a.y + r), ImVec2(b.x - r, b.y - r),
                               ImVec2(a.x + r, b.y - r)};
    Poly p;
    p.reserve(4 * (kSeg + 1));
    for (int c = 0; c < 4; ++c) {
        const float a0 = kPi * (1.0f + 0.5f * static_cast<float>(c));  // top-left corner starts at 180 degrees
        for (int i = 0; i <= kSeg; ++i) {
            const float an = a0 + 0.5f * kPi * static_cast<float>(i) / kSeg;
            p.push_back(ImVec2(centres[c].x + std::cos(an) * r, centres[c].y + std::sin(an) * r));
        }
    }
    return p;
}

// `uvOff`: where the window was when the snapshot was taken, relative to now
// (the texture is sampled there, so the old look follows a moved window).
void FillPoly(ImDrawList* dl, const Poly& p, ImVec2 o, ImVec2 s, float lo, float hi, ImU32 clo, ImU32 chi, bool textured,
              ImVec2 uvOff = ImVec2(0, 0)) {
    if (p.size() < 3) return;
    const ImVec2 white = ImGui::GetFontTexUvWhitePixel();
    const ImVec2 disp = ImGui::GetIO().DisplaySize;
    const int n = static_cast<int>(p.size());
    dl->PrimReserve((n - 2) * 3, n);
    const ImDrawIdx base = static_cast<ImDrawIdx>(dl->_VtxCurrentIdx);
    for (int i = 1; i + 1 < n; ++i) {
        dl->PrimWriteIdx(base);
        dl->PrimWriteIdx(static_cast<ImDrawIdx>(base + i));
        dl->PrimWriteIdx(static_cast<ImDrawIdx>(base + i + 1));
    }
    for (const ImVec2& v : p) {
        const float k = (v.x - o.x) / s.x + (v.y - o.y) / s.y;
        const float t = hi > lo ? std::clamp((k - lo) / (hi - lo), 0.0f, 1.0f) : 0.0f;
        dl->PrimWriteVtx(v, textured ? ImVec2((v.x + uvOff.x) / disp.x, (v.y + uvOff.y) / disp.y) : white,
                         textured ? IM_COL32_WHITE : Mix(clo, chi, t));
    }
}

void DrawPeel() {
    if (g_peel.state != 2) return;
    const float u = static_cast<float>((ImGui::GetTime() - g_peel.start) / kPeelSeconds);
    if (u >= 1.0f || g_peel.tex == ImTextureID_Invalid) {
        g_peel = Peel{};
        return;
    }
    // cubic-bezier-ish ease in-out; the edge runs from beyond the bottom-right corner to past the top-left one
    const float e = u < 0.5f ? 4 * u * u * u : 1 - std::pow(-2 * u + 2, 3.0f) / 2;
    const float p = 2.0f - 2.16f * e;
    // Drawn where the window is now (it may have been dragged since the click),
    // sampled where it was when the snapshot was taken.
    const ImVec2 s(g_peel.max.x - g_peel.min.x, g_peel.max.y - g_peel.min.y);
    const ImVec2 o = g_winMin, uvOff(g_peel.min.x - g_winMin.x, g_peel.min.y - g_winMin.y);
    const Poly rect = RoundedRect(o, ImVec2(o.x + s.x, o.y + s.y), ImGui::GetStyle().WindowRounding);
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    // The old look, still on the top-left side of the edge.
    dl->PushTexture(ImTextureRef(g_peel.tex));
    FillPoly(dl, ClipDiagonal(rect, o, s, p, true), o, s, 0, 0, 0, 0, true, uvOff);
    dl->PopTexture();
    // The lifted flap and its shadow, just past the edge.
    const Palette& P = theme::Colors();
    struct Stop {
        float at;
        ImU32 col;
    };
    const ImU32 shadow = IM_COL32(16, 12, 34, 77);
    const Stop stops[] = {{0.0f, P.flap1}, {0.068f, P.flap2}, {0.072f, shadow}, {0.14f, WithAlpha(shadow, 0.0f)}};
    for (size_t i = 0; i + 1 < std::size(stops); ++i) {
        const float lo = p + stops[i].at, hi = p + stops[i + 1].at;
        const Poly band = ClipDiagonal(ClipDiagonal(rect, o, s, lo, false), o, s, hi, true);
        FillPoly(dl, band, o, s, lo, hi, stops[i].col, stops[i + 1].col, false);
    }
}

}  // namespace

// ------------------------------------------------------------------ API ----
void SyncTheme(float scale) {
    settings::Def d;
    d.module = kUiModule;
    d.key = kKeyOverlayTheme;
    const double v = settings::Has(kUiModule, kKeyOverlayTheme) ? Read(d) : 0.0;
    theme::Sync(std::lround(v) == 1 ? theme::Mode::Light : theme::Mode::Dark, scale);
}

void OnOpened() {
    slime_icons::OnOpen();
    g_lastPage.clear();  // restore the page's saved scroll position
}

bool Draw(const Host& h) {
    g_scale = h.scale;
    const Palette& P = theme::Colors();
    const auto views = BuildModuleViews();
    const auto cats = layout::Build(views);
    const ModuleView* ui = FindView(views, kUiModule);

    auto catIt = std::find_if(cats.begin(), cats.end(), [](const Category& c) { return c.id == g_cat; });
    if (catIt == cats.end()) catIt = cats.begin();
    if (catIt == cats.end()) return true;
    const Category& cat = *catIt;
    g_cat = cat.id;
    int& ti = g_tab[cat.id];
    ti = std::clamp(ti, 0, static_cast<int>(cat.tabs.size()) - 1);

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const ImVec2 size(std::min(U(kWinW), vp->WorkSize.x - U(24)), std::min(U(kWinH), vp->WorkSize.y - U(24)));
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * 0.5f),
                            ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(size, ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
    const ImGuiWindowFlags wf = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
                                ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
                                ImGuiWindowFlags_NoSavedSettings;
    bool closed = false;
    if (ImGui::Begin("sr2mods###sr2mods", nullptr, wf)) {
        ImGui::PopStyleVar(2);
        const ImVec2 pos = ImGui::GetWindowPos(), ws = ImGui::GetWindowSize();
        g_winMin = pos;
        g_winMax = ImVec2(pos.x + ws.x, pos.y + ws.y);
        const float border = ImGui::GetStyle().WindowBorderSize, rounding = ImGui::GetStyle().WindowRounding;
        SoftShadow(ImGui::GetBackgroundDrawList(), pos, g_winMax, rounding, U(1), U(7), P.shadow);
        ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(pos.x + border, pos.y + border),
                                                  ImVec2(pos.x + U(kSideW), g_winMax.y - border), P.side,
                                                  std::max(0.0f, rounding - border), ImDrawFlags_RoundCornersLeft);

        DrawSidebar(h, cats, ui, ws.y);
        ImGui::SetCursorPos(ImVec2(U(kSideW), 0));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(U(10), U(8)));
        ImGui::BeginChild("##main", ImVec2(ws.x - U(kSideW), ws.y), ImGuiChildFlags_None,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoBackground);
        closed = DrawTopBar(cat, ti, views);
        const Tab& tab = cat.tabs[static_cast<size_t>(ti)];
        const std::string page = cat.id + "/" + std::to_string(ti);

        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(U(18), 0));
        ImGui::BeginChild("##body", ImVec2(0, 0), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoBackground);
        // Each page keeps its scroll position across page switches and closing / reopening.
        // SetScrollY lands next frame, clamped to the previous frame's content height (the
        // other page's), so a restore is re-applied for a few frames.
        if (page != g_lastPage) {
            g_lastPage = page;
            g_scrollRestore = 4;
        }
        if (g_scrollRestore > 0) {
            const auto it = g_pageScroll.find(page);
            const float want = it != g_pageScroll.end() ? it->second : 0.0f;
            if (std::fabs(ImGui::GetScrollY() - want) > 0.5f) {
                ImGui::SetScrollY(want);
                --g_scrollRestore;
            } else {
                g_scrollRestore = 0;
            }
        } else {
            g_pageScroll[page] = ImGui::GetScrollY();
        }
        if (h.versionMismatch)
            Banner(P.warn, "This game build differs from the one the mod was made for; some modules may be disabled. " +
                               h.versionStatus);
        switch (tab.kind) {
            case TabKind::Module: {
                const ModuleView* mv = FindView(views, tab.module);
                if (!mv) break;
                FailedBanner(*mv);
                const TabState st = StateOf(tab, views);
                const std::string masterLabel = !tab.masterKey.empty() && st.sw ? st.sw->label : std::string();
                // While the tab's switch (or its module) is off, its settings stay visible but grayed
                // out and read-only; the switch in the tab bar stays usable.
                if (st.locked) OffNote(tab.title + " is part of " + mv->title + ". Turn on " + mv->title + " to use it.");
                ImGui::BeginDisabled(!st.on);
                DrawModuleCards(h, *mv, tab, tab.masterKey, masterLabel);
                ImGui::EndDisabled();
                for (const auto& n : tab.nested)
                    if (const ModuleView* nmv = FindView(views, n)) DrawNested(h, *nmv);
                break;
            }
            case TabKind::Cards: DrawCardsPage(h, tab, views); break;
            case TabKind::Profiles: DrawProfilesPage(h); break;
            case TabKind::Settings: DrawSettingsPage(h, ui); break;
            case TabKind::Sandbox:
                if (const ModuleView* mv = FindView(views, tab.module)) DrawSandboxPage(h, *mv);
                break;
        }
        ImGui::Dummy(ImVec2(0, U(18)));
        ImGui::EndChild();
        ImGui::PopStyleVar();
        ImGui::EndChild();
        ImGui::PopStyleVar();
    } else {
        ImGui::PopStyleVar(2);
    }
    ImGui::End();
    DrawPeel();
    return !closed;
}

void DrawHint(const Host& h) {
    g_scale = h.scale;
    const Palette& P = theme::Colors();
    ImGui::PushStyleColor(ImGuiCol_WindowBg, P.pill);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, U(12));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(U(14), U(9)));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, U(4)));
    const ImGuiWindowFlags f = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
                               ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
    const ImVec4 key = ImGui::ColorConvertU32ToFloat4(theme::Current() == theme::Mode::Dark ? P.grab : P.accent);
    float y = U(16);  // pills stack downwards
    auto next = [&y] {
        y = ImGui::GetWindowPos().y + ImGui::GetWindowSize().y + U(8);
    };
    if (h.menuHint) {
        ImGui::SetNextWindowPos(ImVec2(U(16), y), ImGuiCond_Always);
        if (ImGui::Begin("##sr2mods_hint", nullptr, f)) {
            ImGui::TextUnformatted("Press ");
            ImGui::SameLine(0, 0);
            ImGui::TextColored(key, "%s", h.hotkeyName.c_str());
            ImGui::SameLine(0, 0);
            ImGui::TextUnformatted(" for mod settings");
            if (h.versionMismatch)
                ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(P.warn), "Version warning: %s", h.versionStatus.c_str());
            next();
        }
        ImGui::End();
    }
    if (h.quickHint != 0) {
        ImGui::SetNextWindowPos(ImVec2(U(16), y), ImGuiCond_Always);
        if (ImGui::Begin("##sr2mods_quick_hint", nullptr, f)) {
            if (h.quickHint == 2) {
                ImGui::TextUnformatted("Staying in the main menu");
            } else {
                ImGui::TextUnformatted("Hold ");
                ImGui::SameLine(0, 0);
                ImGui::TextColored(key, "Shift");
                ImGui::SameLine(0, 0);
                ImGui::TextUnformatted(" to stay in the main menu");
                const ImVec4 dim = ImGui::ColorConvertU32ToFloat4(P.soft);
                if (h.quickSeconds >= 0)
                    ImGui::TextColored(dim, "Continuing in %.1f s", h.quickSeconds);
                else
                    ImGui::TextColored(dim, "Continuing to your last save");
            }
        }
        ImGui::End();
    }
    ImGui::PopStyleVar(4);
    ImGui::PopStyleColor();
}

bool WantSnapshot() { return g_peel.state == 1; }

void StartPeel(ImTextureID oldFrame) {
    if (g_peel.state != 1) return;
    if (oldFrame == ImTextureID_Invalid || g_peel.max.x <= g_peel.min.x) {
        g_peel = Peel{};
        return;
    }
    g_peel.tex = oldFrame;
    g_peel.start = ImGui::GetTime();
    g_peel.state = 2;
}

}  // namespace sr2::ui::menu

#endif  // SR2_HAVE_IMGUI
