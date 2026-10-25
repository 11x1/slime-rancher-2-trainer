// Built only with Dear ImGui (third_party/imgui), like overlay.cpp.
#if defined(SR2_HAVE_IMGUI)

#include "menu.h"

#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <cfloat>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <format>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "core/build_info.h"
#include "core/log.h"
#include "core/profiles.h"
#include "discord_bridge.h"
#include "game_input.h"
#include "hud_edit.h"
#include "imgui_internal.h"
#include "menu_layout.h"
#include "item_grid.h"
#include "nav.h"
#include "sandbox_bridge.h"
#include "sandbox_extras_bridge.h"
#include "sandbox_upgrades.h"
#include "sandbox_weather.h"
#include "slime_dj_bridge.h"
#include "slime_icons.h"
#include "spotlight.h"
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
// Scroll position per page, kept for the session (KeepPageScroll).
struct PageScroll {
    float want = 0;      // where the user left the page
    bool pending = false;  // `want` is being re-applied (page shown again, or its content got shorter for a while)
    double since = 0;    // ImGui::GetTime() when that started (given up after 10 s)
};
std::map<std::string, PageScroll> g_pageScroll;
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

// Minimum tooltip widths (design px): plain text tips, and the Sandbox item / rich tips. Those with
// favourite rows come out about 260-300 px wide; the ones with only a name, a tag and the click rows
// (an empty or locked slot, an item with no favourites) were 230 and still looked squeezed next to
// them (round 11), so the minimum is about the width of a tip with favourite rows.
constexpr float kTipMinW = 160, kRichTipMinW = 280;

void Tooltip(const std::string& text) {
    if (text.empty()) return;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(U(12), U(9)));
    PushFloatingStyle();
    // A minimum width: a tooltip of a few words (a tile's name) looked squeezed.
    ImGui::SetNextWindowSizeConstraints(ImVec2(U(kTipMinW), 0), ImVec2(FLT_MAX, FLT_MAX));
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
        case layout::Icon::Sandbox: {
            // A four-pointed sparkle and a small filled one top right (cheats / magic), inside the
            // same 2..16 box as the other icons. Round tips (a half circle each) and curved sides
            // meeting them tangentially: a smooth outline, so the stroke has no mitre spikes (round 11:
            // the straight-edged star was too big and looked sharp next to the other icons).
            auto sparkle = [&](float cx, float cy, float r, float tip) {
                constexpr int kCap = 8;
                for (int i = 0; i < 4; ++i) {
                    const float a = static_cast<float>(i) * kPi / 2 - kPi / 2;
                    const float an = a + kPi / 2;  // the next tip
                    const ImVec2 d(std::cos(a), std::sin(a)), dn(std::cos(an), std::sin(an));
                    const ImVec2 q(cx + d.x * (r - tip), cy + d.y * (r - tip));  // this tip's cap centre
                    // The cap from a - 90 to a + 90 degrees; its first point is where the last curve ended.
                    for (int j = i == 0 ? 0 : 1; j <= kCap; ++j) {
                        const float t = a - kPi / 2 + kPi * static_cast<float>(j) / kCap;
                        dl->PathLineTo(P(q.x + std::cos(t) * tip, q.y + std::sin(t) * tip));
                    }
                    // From the cap back towards the centre and out to the next tip's cap.
                    const float l = (r - tip) * 0.75f;
                    const ImVec2 e(q.x + dn.x * tip, q.y + dn.y * tip);
                    const ImVec2 sn(cx + dn.x * (r - tip) + d.x * tip, cy + dn.y * (r - tip) + d.y * tip);
                    dl->PathBezierCubicCurveTo(P(e.x - d.x * l, e.y - d.y * l), P(sn.x - dn.x * l, sn.y - dn.y * l), P(sn.x, sn.y), 10);
                }
                dl->_Path.pop_back();  // the last curve ends on the first point
            };
            sparkle(7.5f, 10.5f, 5.6f, 1.1f);
            dl->PathStroke(col, ImDrawFlags_Closed, th * 0.9f);
            sparkle(13.6f, 4.4f, 2.7f, 0.7f);
            dl->PathFillConcave(col);
            break;
        }
        case layout::Icon::Profiles: {
            // Three list lines with round ends (round 11).
            auto line = [&](float x0, float x1, float y) {
                dl->AddLine(P(x0, y), P(x1, y), col, th);
                dl->AddCircleFilled(P(x0, y), th * 0.5f, col, 12);
                dl->AddCircleFilled(P(x1, y), th * 0.5f, col, 12);
            };
            line(3.5f, 14.5f, 5);
            line(3.5f, 14.5f, 9);
            line(3.5f, 10.5f, 13);
            break;
        }
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
    nav::Button(id, ImVec2(size, size));
    const bool hov = ImGui::IsItemHovered() || nav::Focused();
    const ImVec2 a = ImGui::GetItemRectMin();
    const ImVec2 c(std::floor(a.x + size * 0.5f) + 0.5f, std::floor(a.y + size * 0.5f) + 0.5f);
    const float r = std::floor(size * 0.5f) - 0.5f;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddCircleFilled(c, r, ImGui::GetColorU32(hov ? P.frameHover : P.frame), 48);
    dl->AddCircle(c, r - U(0.75f), ImGui::GetColorU32(hov ? P.tabSel : P.line), 48, U(1.5f));
    ImFont* qf = theme::Display();
    const float qs = std::round(size * 0.62f);
    ImFontBaked* qb = qf->GetFontBaked(qs);  // nullptr only with a locked atlas
    const ImFontGlyph* g = qb ? qb->FindGlyph('?') : nullptr;
    ImVec2 qp = g ? ImVec2(c.x - (g->X0 + g->X1) * 0.5f, c.y - (g->Y0 + g->Y1) * 0.5f) : c;
    qp = ImVec2(std::round(qp.x), std::round(qp.y));
    dl->AddText(qf, qs, qp, ImGui::GetColorU32(hov ? P.text : P.soft), "?");
    nav::Ring(size * 0.5f);
    if (hov) Tooltip(tip);
}

// On/off switch, vertically centred in a frame-height row. True when clicked.
bool Switch(const char* id, bool on) {
    const Palette& P = theme::Colors();
    const ImVec2 size(U(kSwitchW), U(kSwitchH));
    const ImVec2 cur = ImGui::GetCursorScreenPos();
    ImGui::SetCursorScreenPos(ImVec2(cur.x, cur.y + (ImGui::GetFrameHeight() - size.y) * 0.5f));
    const bool clicked = nav::Button(id, size);  // takes Tab focus (Space toggles)
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
    nav::Ring(size.y * 0.5f);
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

// ------------------------------------------------------------ highlight ----
// A search result (spotlight.cpp) that was opened: the page scrolls to its row and the row pulses.
// The row is looked for while the page is drawn (PulseRow); once it was seen on two frames (the first
// frame of a new page only measures it), the page scrolls it into view (ScrollToHighlight), then it
// pulses twice. Given up after 1 s if the page never draws it (a setting without a row).
struct Highlight {
    std::string module, key;  // empty: none
    double requested = 0;     // ImGui::GetTime() of the jump
    double start = -1;        // pulse start (< 0: still scrolling)
    int seen = 0;             // frames the row was drawn while scrolling
    float top = 0, bottom = 0;  // the row's screen rect this frame
} g_hl;
constexpr float kPulseSec = 1.6f;

void PulseRow(const settings::Def& d, ImVec2 a, ImVec2 b) {
    if (g_hl.key.empty() || d.key != g_hl.key || d.module != g_hl.module) return;
    if (g_hl.start < 0) {  // ScrollToHighlight uses it after the page
        ++g_hl.seen;
        g_hl.top = a.y;
        g_hl.bottom = b.y;
        return;
    }
    const float t = static_cast<float>(ImGui::GetTime() - g_hl.start);
    if (t < 0.0f) return;
    if (t >= kPulseSec) {
        g_hl = Highlight{};
        return;
    }
    const float k = 0.5f - 0.5f * std::cos(t * 2.0f * kPi / (kPulseSec * 0.5f));  // two pulses
    const Palette& P = theme::Colors();
    const ImU32 col = theme::Current() == theme::Mode::Dark ? P.grab : P.accent;
    const ImVec2 pa(a.x - U(6), a.y - U(3)), pb(b.x + U(6), b.y + U(3));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(pa, pb, WithAlpha(col, 0.16f * k), U(8));
    dl->AddRect(pa, pb, WithAlpha(col, 0.9f * k), U(8), 0, U(2));
}

// Called in the page's body window ("##body_<page>") after the page: scroll the found row into view (also as the page's saved
// scroll position, so KeepPageScroll keeps it), then start the pulse.
void ScrollToHighlight(const std::string& page) {
    if (g_hl.key.empty() || g_hl.start >= 0) return;
    const double now = ImGui::GetTime();
    if (g_hl.seen < 2) {
        if (now - g_hl.requested > 1.0) g_hl = Highlight{};
        return;
    }
    const float top = ImGui::GetWindowPos().y, h = ImGui::GetWindowHeight(), margin = U(24);
    if (g_hl.top < top + margin || g_hl.bottom > top + h - margin) {
        const float target =
            std::clamp(ImGui::GetScrollY() + g_hl.top - top - h * 0.3f, 0.0f, std::max(0.0f, ImGui::GetScrollMaxY()));
        ImGui::SetScrollY(target);
        PageScroll& ps = g_pageScroll[page];
        ps.want = target;
        ps.pending = true;
        ps.since = now;
    }
    g_hl.start = now + 0.1;  // after the scroll has landed
}

void JumpTo(const spotlight::Target& t) {
    g_cat = t.cat;
    g_tab[t.cat] = t.tab;
    g_hl = Highlight{};
    if (t.key.empty()) return;
    g_hl.module = t.module;
    g_hl.key = t.key;
    g_hl.requested = ImGui::GetTime();
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
    const ImVec2 rowEnd(rowStart.x + avail, std::max(labelBottom, ImGui::GetItemRectMax().y));
    // Middle click anywhere on the row: back to the default. Not while the row is disabled (locked,
    // or a nested row sliding shut), blocked by a popup, or this key bind is waiting for a key
    // (the middle button can be bound).
    const bool disabled = (GImGui->CurrentItemFlags & ImGuiItemFlags_Disabled) != 0;
    const bool capturing = d.kind == settings::Kind::Key && h.isCapturing && h.isCapturing(d);
    if (!disabled && !capturing && ImGui::IsMouseClicked(ImGuiMouseButton_Middle, false) && ImGui::IsWindowHovered() &&
        ImGui::IsMouseHoveringRect(rowStart, rowEnd) && std::fabs(cur - d.def) > 1e-9)
        Write(d, d.def);
    PulseRow(d, rowStart, rowEnd);
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
                            " to move it, or a corner to resize it. Enter keeps it, Esc cancels.";
    ImGui::AlignTextToFramePadding();
    ImGui::PushStyleColor(ImGuiCol_Text, theme::Colors().text);
    ImGui::TextUnformatted("Position and size");
    ImGui::PopStyleColor();
    HoverTip(tip);
    ImGui::SameLine(x0 + avail - ctlW);
    // Not while a game menu (map, pause menu, inventory) is open: it covers the HUD and owns the mouse.
    const bool menuOpen = game_input::GameMenuOpen();
    ImGui::BeginDisabled(menuOpen);
    if (ImGui::Button("Move", ImVec2(ctlW, 0))) hud_edit::Begin(e);
    ImGui::EndDisabled();
    HoverTip(menuOpen ? std::string("Close the game's menu first; it covers the HUD.") : tip);
    PulseRow(d, ImVec2(ImGui::GetWindowPos().x + x0 - ImGui::GetScrollX(), ImGui::GetItemRectMin().y),
             ImVec2(ImGui::GetItemRectMax().x, ImGui::GetItemRectMax().y));
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
    ImGui::BeginChild(id, ImVec2(w, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_NavFlattened,
                      ImGuiWindowFlags_NoScrollbar);
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
        PulseRow(nmv.enabled, ImVec2(ImGui::GetWindowPos().x + x0 - ImGui::GetScrollX(), ImGui::GetItemRectMin().y),
                 ImGui::GetItemRectMax());
    }
    ImGui::Dummy(ImVec2(0, U(4)));
    FailedBanner(nmv);
    Tab all;
    ImGui::BeginDisabled(!on);  // grayed out and read-only while the module is off (its switch stays usable)
    DrawModuleCards(h, nmv, all, {}, {});
    ImGui::EndDisabled();
}

// A small heading inside a card, a faint rule from its end to the card's right edge.
void SectionHeading(const char* text, bool first) {
    const Palette& P = theme::Colors();
    if (!first) ImGui::Dummy(ImVec2(0, U(6)));
    const float avail = ImGui::GetContentRegionAvail().x;
    {
        FontScope f(theme::Display(), theme::kSmallSize + 1);
        ImGui::PushStyleColor(ImGuiCol_Text, P.soft);
        ImGui::TextUnformatted(text);
        ImGui::PopStyleColor();
    }
    const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    const float y = std::round((a.y + b.y) * 0.5f), x0 = b.x + U(8), x1 = a.x + avail;
    if (x1 > x0) ImGui::GetWindowDrawList()->AddLine(ImVec2(x0, y), ImVec2(x1, y), ImGui::GetColorU32(P.line), std::max(1.0f, U(1)));
}

// A card with sections (Tab::sections; Ranch > Upgrades): each a heading, an optional note, the card module's
// groups for it (grayed while the card's module is off) and other modules, each as its switch row and then
// its own rows (grayed while that module is off, and while the card's module is off if listed in needCard).
// The card module's groups no section names follow at the end.
void DrawCardSections(const Host& h, const Tab& tab, const ModuleView& card, bool cardOn,
                      const std::vector<layout::CardSection>& secs, const std::vector<ModuleView>& views) {
    const Palette& P = theme::Colors();
    std::vector<const GroupView*> all;
    for (const auto& g : card.groups) all.push_back(&g);
    const Nesting n = BuildNesting(all, {});
    auto anyShown = [](const ModuleView& mv, const GroupView& g, const Nesting& nest) {
        return std::any_of(g.defs.begin(), g.defs.end(), [&](const settings::Def& d) { return RowShown(mv, d, nest, {}); });
    };
    std::set<std::string> named;
    bool first = true;
    for (const auto& sec : secs) {
        std::vector<const GroupView*> groups;
        for (const auto& name : sec.groups) {
            named.insert(name);
            for (const auto* g : all)
                if (g->name == name && anyShown(card, *g, n)) groups.push_back(g);
        }
        std::vector<const ModuleView*> mods;
        for (const auto& m : sec.modules)
            if (const ModuleView* mv = FindView(views, m)) mods.push_back(mv);
        if (groups.empty() && mods.empty()) continue;
        SectionHeading(sec.title.c_str(), first);
        first = false;
        if (!sec.note.empty()) {
            FontScope f(theme::Body(), theme::kSmallSize);
            ImGui::PushTextWrapPos(0.0f);
            ImGui::PushStyleColor(ImGuiCol_Text, P.dim);
            ImGui::TextUnformatted(sec.note.c_str());
            ImGui::PopStyleColor();
            ImGui::PopTextWrapPos();
        }
        ImGui::BeginDisabled(!cardOn);
        for (const auto* g : groups) DrawGroupRows(h, card, *g, n, {}, {});
        ImGui::EndDisabled();
        for (const ModuleView* mv : mods) {
            const bool needsCard = std::find(sec.needCard.begin(), sec.needCard.end(), mv->module) != sec.needCard.end();
            ImGui::PushID(mv->module.c_str());
            ImGui::BeginDisabled(needsCard && !cardOn);
            if (mv->hasEnabled) {
                settings::Def d = mv->enabled;
                const auto label = tab.switchLabel.find(mv->module);
                const auto help = tab.cardHelp.find(mv->module);
                const std::string lock = settings::LockReason(mv->module, "enabled");  // pinned by the module
                if (help != tab.cardHelp.end()) d.tooltip = help->second;
                if (!lock.empty()) d.tooltip = lock;
                ImGui::BeginDisabled(!lock.empty());
                DrawRow(h, d, label != tab.switchLabel.end() ? label->second : mv->title, false);
                ImGui::EndDisabled();
            }
            bool failed = false;
            const std::string status = ModuleStatus(mv->module, &failed);
            if (failed) {
                ImGui::PushTextWrapPos(0.0f);
                ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(P.err), "%s failed: %s", mv->title.c_str(), status.c_str());
                ImGui::PopTextWrapPos();
            }
            std::vector<const GroupView*> own;
            for (const auto& g : mv->groups) own.push_back(&g);
            const Nesting ownNest = BuildNesting(own, {});
            ImGui::BeginDisabled(!ModuleOn(*mv));
            for (const auto* g : own) DrawGroupRows(h, *mv, *g, ownNest, {}, {});
            ImGui::EndDisabled();
            ImGui::EndDisabled();
            ImGui::PopID();
        }
    }
    // Groups the layout does not place yet: after the sections, without a heading.
    ImGui::BeginDisabled(!cardOn);
    for (const auto* g : all)
        if (!named.count(g->name)) DrawGroupRows(h, card, *g, n, {}, {});
    ImGui::EndDisabled();
}

// Misc > General, Ranch > Upgrades: one card per module, the module's switch in the card header, in two
// columns. A stacked tab has its first column full width and the next two side by side under it (unused since
// round 12). A card with Tab::sections draws them (DrawCardSections).
void DrawCardsPage(const Host& h, const Tab& tab, const std::vector<ModuleView>& views) {
    const Palette& P = theme::Colors();
    const float w = ColumnWidth();
    const size_t first = tab.stacked ? 1 : 0;  // the first of the two side-by-side columns
    const size_t end = std::min(tab.columns.size(), first + 2);
    for (size_t c = 0; c < end; ++c) {
        if (c > first) ImGui::SameLine(0, U(kGap));
        const float cw = c < first ? ImGui::GetContentRegionAvail().x : w;
        ImGui::BeginGroup();
        for (const auto& m : tab.columns[c]) {
            const ModuleView* mv = FindView(views, m);
            if (!mv) continue;
            ImGui::PushID(m.c_str());
            const bool on = ModuleOn(*mv);
            BeginCard("card", cw);
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
                PulseRow(mv->enabled, ImVec2(ImGui::GetWindowPos().x + x0 - ImGui::GetScrollX(), ImGui::GetItemRectMin().y),
                         ImGui::GetItemRectMax());
            }
            bool failed = false;
            const std::string status = ModuleStatus(mv->module, &failed);
            if (failed) {
                ImGui::PushTextWrapPos(0.0f);
                ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(P.err), "Failed: %s", status.c_str());
                ImGui::PopTextWrapPos();
            }
            if (const auto secs = tab.sections.find(m); secs != tab.sections.end()) {
                DrawCardSections(h, tab, *mv, on, secs->second, views);
            } else {
                std::vector<const GroupView*> groups;
                for (const auto& g : mv->groups) groups.push_back(&g);
                const Nesting n = BuildNesting(groups, {});
                ImGui::BeginDisabled(!on);  // grayed out and read-only while the module is off
                // The card is named by its module: its groups get no headings (round 10), just one
                // list of rows in group order.
                for (const auto* g : groups) DrawGroupRows(h, *mv, *g, n, {}, {});
                ImGui::EndDisabled();
            }
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
        if (nav::Button("##p", size)) g_profSel = i.name;
        const bool hovered = ImGui::IsItemHovered();
        if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && i.valid) profiles::Load(i.name);
        const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
        if (isSel || hovered) dl->AddRectFilled(a, b, isSel ? P.sel : P.hover, U(10));
        nav::Ring(U(10));
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
    HoverTip("Share a profile by copying its .json file. Put shared files here; the list refreshes when the "
             "overlay opens.");
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
long long g_sbxBucks = 1000;
item_grid::State g_vacGrid, g_spawnGrid, g_craftGrid;  // search text and tab per grid
constexpr int kSpawnMax = 60;          // most of one item per spawn request (the module's limit)
std::map<uintptr_t, int> g_spawnPick;  // spawner multi-selection: item -> amount
std::map<uintptr_t, int> g_vacPick;    // vacpack multi-selection: item -> amount (0 = the slot's maximum)
std::map<uintptr_t, int> g_craftPick;  // crafter multi-selection: recipe / material -> amount
uintptr_t g_vacFocus = 0;    // last picked vacpack item: Shift+wheel on a slot puts this one in
// Slot counts asked for but not in a snapshot yet, so quick wheel notches add up.
struct SlotPending {
    uintptr_t item = 0;
    int count = 0;
    double until = 0;  // ImGui::GetTime() deadline
};
SlotPending g_slotPending[32];
// Right-click menu on an item tile.
enum class SbxGrid { Vacpack, Spawner };
uintptr_t g_ctxItem = 0;
SbxGrid g_ctxGrid = SbxGrid::Vacpack;
int g_ctxSlot = -1;  // the vacpack slot the menu was opened on (its Fill / Empty entries); -1: a grid tile
constexpr const char* kSbxCtx = "##sbx_item_ctx";
constexpr const char* kCraftCtx = "##sbx_craft_ctx";  // the crafter's right-click menu (variants)
uintptr_t g_craftCtx = 0;                             // the recipe it was opened on
constexpr const char* kSlotDrag = "SR2_SLOT";  // drag payload: a vacpack slot index (int)
// The grid under the mouse: the key hint bar under the page shows its controls.
enum class SbxHover { None, Slots, Picker, Spawner, Crafter, Upgrades, Blueprints };
SbxHover g_sbxHover = SbxHover::None;
double g_sbxHoverNone = 0;  // since when the mouse is over the page but no grid (0: over one)
double g_sbxShownSince = 0;  // when the Sandbox page was shown again (opened / switched to)
using sandbox_upgrades::KeyRow;
float g_vacPickerTop = FLT_MAX;  // screen y where the vacpack's item grid starts (above it: the slots)
float g_sbxRoom = 0;             // the page's visible height left for the card being drawn (SbxGridHeight); 0: unset
ImVec2 g_sbxBarMin, g_sbxBarMax;  // the key hint bar under the page, last frame (empty: not shown)

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

// A multi-item request (Craft N, Spawn N, Put in slots): each action gets its index and the size, so
// the module shows one HUD notice for all of them and spawns them in one grid (sbx::Action::batch).
void SbxBatch(std::vector<sbx::Action>& actions) {
    const int n = static_cast<int>(actions.size());
    for (int i = 0; i < n; ++i) {
        actions[static_cast<size_t>(i)].batch = n > 1 ? i + 1 : 0;
        actions[static_cast<size_t>(i)].batchSize = n > 1 ? n : 0;
        sbx::Request(actions[static_cast<size_t>(i)]);
    }
}

void DimText(const std::string& text, float size = theme::kSmallSize) {
    FontScope f(theme::Body(), size);
    ImGui::PushStyleColor(ImGuiCol_Text, theme::Colors().dim);
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(text.c_str());
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
}

// This file's card / tooltip / popup helpers for the Upgrades & Blueprints card (sandbox_upgrades.cpp).
void RichTooltip(const sandbox_upgrades::Tip& t);
void SubTabs(const char* id, const std::vector<std::string>& labels, int& sel);
ImU32 KindAccent(sbx::Kind k);

sandbox_upgrades::Helpers SbxHelpers() {
    sandbox_upgrades::Helpers h{g_scale, &BeginCard, &EndCard, &CardHeading, &Tooltip, &PushFloatingStyle, &PopFloatingStyle,
                                &FloatingSurface};
    h.tip = &RichTooltip;
    h.subTabs = &SubTabs;
    h.gadgetAccent = KindAccent(sbx::Kind::Gadget);
    h.upgradeAccent = theme::Colors().tabSel;
    return h;
}

// The same helpers for the Weather card (sandbox_weather.cpp), plus the switch and the accent button.
sandbox_weather::Helpers SbxWeatherHelpers() {
    sandbox_weather::Helpers h{g_scale, &BeginCard, &EndCard, &CardHeading, &Tooltip, &PushFloatingStyle, &PopFloatingStyle,
                               &FloatingSurface};
    h.subTabs = &SubTabs;
    h.toggle = &Switch;
    h.accentButton = &AccentButton;
    return h;
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
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(P.warn),
                           "This marks the loaded save (or, on the title screen, the next save you load) as cheated: "
                           "Steam achievements are disabled in it for good, and Sandbox can't be turned off in it again.");
        DimText("Each save keeps its own Sandbox switch: saves without the mark load with Sandbox off, a marked save "
                "always loads with Sandbox on.");
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
        case sbx::Kind::Container: return IM_COL32(0xC9, 0x9A, 0x5B, 0xFF);
        case sbx::Kind::Other: break;
    }
    return IM_COL32(0xA8, 0xB4, 0xC8, 0xFF);
}

// The tooltip's group tag (singular).
const char* KindTag(sbx::Kind k) {
    switch (k) {
        case sbx::Kind::Slime: return "Slime";
        case sbx::Kind::Largo: return "Largo";
        case sbx::Kind::Plort: return "Plort";
        case sbx::Kind::Food: return "Food";
        case sbx::Kind::Animal: return "Meat";
        case sbx::Kind::Toy: return "Toy";
        case sbx::Kind::Resource: return "Resource";
        case sbx::Kind::Liquid: return "Liquid";
        case sbx::Kind::Gadget: return "Gadget";
        case sbx::Kind::Container: return "Crate";
        case sbx::Kind::Other: break;
    }
    return "Item";
}

// Which group tab shows an item (sbx::Group; All shows everything). Largos and crates have tabs of
// their own (spawner only: the vacpack can't hold them).
bool InGroup(sbx::Kind k, int group) {
    switch (group) {
        case sbx::kGroupPlorts: return k == sbx::Kind::Plort;
        case sbx::kGroupSlimes: return k == sbx::Kind::Slime;
        case sbx::kGroupLargos: return k == sbx::Kind::Largo;
        case sbx::kGroupFood: return k == sbx::Kind::Food || k == sbx::Kind::Animal;
        case sbx::kGroupResources: return k == sbx::Kind::Resource;
        case sbx::kGroupContainers: return k == sbx::Kind::Container;
        case sbx::kGroupOther:  // fits none of the tabs above (toys, unsorted types)
            return k == sbx::Kind::Other || k == sbx::Kind::Toy || k == sbx::Kind::Liquid || k == sbx::Kind::Gadget;
        default: return true;
    }
}

// The "Mod" tab of the vacpack picker and the spawner (round 8: "add a modded filter ... for
// futureproofing"): the mod's own items (sbx::Item::mod), whatever their kind. Not an sbx::Group (no
// game icon): a tab value of the overlay's own, only listed while the grid has such items.
constexpr int kGroupMod = -10;

// Whether a group tab (an sbx::Group or kGroupMod) shows the item.
bool InTab(const sbx::Item& it, int group) { return group == kGroupMod ? it.mod : InGroup(it.kind, group); }

// The Mod tab: an "M" in the accent colour, like the tiles' [M] badge.
item_grid::Tab ModTab(const char* tip) {
    const Palette& P = theme::Colors();
    return {0, 'M', theme::Current() == theme::Mode::Dark ? P.grab : P.accent, tip};
}

// A crate's area: the zone the module found for it (sbx::Item::area, the game's zone name), else its
// name without "Crate" / "Gold" / "Shadow" ("Bluffs Gold Crate" -> "Bluffs").
std::string CrateArea(const sbx::Item& it) {
    if (!it.area.empty()) return it.area;
    const std::string& name = it.name;
    std::string out, word;
    auto flush = [&] {
        if (!word.empty() && word != "Crate" && word != "Gold" && word != "Shadow") out += (out.empty() ? "" : " ") + word;
        word.clear();
    };
    for (char c : name) {
        if (c == ' ') flush();
        else word += c;
    }
    flush();
    return out.empty() ? std::string("Other") : out;
}

// One group tab: the game's icon from the snapshot, else the first listed item's icon of that
// group, else a coloured letter.
item_grid::Tab GroupTab(const sbx::Snapshot& s, int group, const std::vector<sbx::Item>* items) {
    static const char* const kTips[sbx::kGroupCount] = {"All",    "Plorts", "Slimes", "Food",
                                                        "Resources", "Largos", "Crates", "Other"};
    static const int kGlyph[sbx::kGroupCount] = {0, 'P', 'S', 'F', 'R', 'L', 'C', item_grid::kGlyphOther};
    static const sbx::Kind kTint[sbx::kGroupCount] = {sbx::Kind::Other, sbx::Kind::Plort,    sbx::Kind::Slime,
                                                      sbx::Kind::Food,  sbx::Kind::Resource, sbx::Kind::Largo,
                                                      sbx::Kind::Container, sbx::Kind::Other};
    item_grid::Tab t;
    t.icon = s.groupIcons[group];
    // Other: always the drawn dots (its first item's icon would say "toys" or the like).
    if (!t.icon && group != sbx::kGroupAll && group != sbx::kGroupOther && items)
        for (const sbx::Item& it : *items)
            if (it.icon && !it.icon2 && InGroup(it.kind, group)) {
                t.icon = it.icon;
                break;
            }
    t.glyph = kGlyph[group];
    t.tint = group == sbx::kGroupAll || group == sbx::kGroupOther ? 0 : KindAccent(kTint[group]);
    t.tip = kTips[group];
    return t;
}

// The vacpack tabs: All, Plorts, Slimes, Food, Resources, Other while the list has items that fit
// none of them, and Mod while the selected slot (`bit`) accepts any of the mod's items. `groups` gets
// the sbx::Group (or kGroupMod) of each tab.
const std::vector<item_grid::Tab>& GroupTabs(const sbx::Snapshot& s, std::vector<int>& groups, uint32_t bit) {
    static std::vector<item_grid::Tab> tabs;
    tabs.clear();
    groups.clear();
    for (int g = sbx::kGroupAll; g <= sbx::kGroupResources; ++g) {
        tabs.push_back(GroupTab(s, g, s.catalog.get()));
        groups.push_back(g);
    }
    bool other = false;
    if (s.catalog)
        for (size_t i = 0; i < s.catalog->size() && !other; ++i) other = InGroup((*s.catalog)[i].kind, sbx::kGroupOther);
    if (other) {
        tabs.push_back(GroupTab(s, sbx::kGroupOther, s.catalog.get()));
        groups.push_back(sbx::kGroupOther);
    }
    bool mod = false;
    if (s.catalog)
        for (size_t i = 0; i < s.catalog->size() && !mod; ++i) mod = (*s.catalog)[i].mod && ((*s.catalog)[i].slotMask & bit);
    if (mod) {
        tabs.push_back(ModTab("Mod (the mod's own items)"));
        groups.push_back(kGroupMod);
    }
    return tabs;
}

// The spawner tabs: All, then each group that has items, slimes and largos side by side, Other, then
// Mod while the list has any of the mod's items. `groups` gets the sbx::Group (or kGroupMod) of each tab.
const std::vector<item_grid::Tab>& SpawnTabs(const sbx::Snapshot& s, std::vector<int>& groups) {
    static std::vector<item_grid::Tab> tabs;
    static const int kOrder[] = {sbx::kGroupAll,       sbx::kGroupPlorts,    sbx::kGroupSlimes,
                                 sbx::kGroupLargos,    sbx::kGroupFood,      sbx::kGroupResources,
                                 sbx::kGroupContainers, sbx::kGroupOther};
    tabs.clear();
    groups.clear();
    for (int g : kOrder) {
        bool any = g == sbx::kGroupAll;
        if (s.spawnables)
            for (size_t i = 0; i < s.spawnables->size() && !any; ++i) any = InGroup((*s.spawnables)[i].kind, g);
        if (!any) continue;
        tabs.push_back(GroupTab(s, g, s.spawnables.get()));
        groups.push_back(g);
    }
    bool mod = false;
    if (s.spawnables)
        for (size_t i = 0; i < s.spawnables->size() && !mod; ++i) mod = (*s.spawnables)[i].mod;
    if (mod) {
        tabs.push_back(ModTab("Mod (the mod's own items)"));
        groups.push_back(kGroupMod);
    }
    return tabs;
}

item_grid::Options GridOptions(float height) {
    item_grid::Options o;
    o.scale = g_scale;
    o.height = height;
    return o;
}

// A grid height (before scaling) that lets an item grid fill the page's visible height under it, at
// least `minH`: called in its card right before item_grid::Draw. `headerRows`: the grid's header rows
// (the search row; the tab row when the tabs are below it). Below the grid: the card's bottom padding
// and border, then the page's end spacing (Draw's 18 px dummy after the item spacing).
float SbxGridHeight(float minH, int headerRows) {
    if (g_sbxRoom <= 0) return minH;
    const float header = static_cast<float>(headerRows) * (ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.y);
    const float below = U(14) + U(2) + U(8) + U(18);
    return std::max(minH, std::floor((g_sbxRoom - ImGui::GetCursorPosY() - header - below) / g_scale));
}

// Item by id from the vacpack list or the spawn list (lookup table rebuilt when a list changes).
// The table points into the lists, so it holds them: comparing bare addresses let a rebuilt
// list allocated where a freed one had been keep the table and its dangling pointers.
const sbx::Item* FindItem(const sbx::Snapshot& s, uintptr_t id) {
    static std::shared_ptr<const std::vector<sbx::Item>> builtSpawn, builtCatalog;
    static std::map<uintptr_t, const sbx::Item*> byId;
    if (builtSpawn != s.spawnables || builtCatalog != s.catalog) {
        builtSpawn = s.spawnables;
        builtCatalog = s.catalog;
        byId.clear();
        for (const auto* list : {s.spawnables.get(), s.catalog.get()})
            if (list)
                for (const sbx::Item& it : *list) byId.emplace(it.id, &it);
    }
    const auto it = byId.find(id);
    return it == byId.end() ? nullptr : it->second;
}

const sbx::Relation* FindRelation(const sbx::Snapshot& s, uintptr_t id) {
    if (!s.relations) return nullptr;
    const auto it = s.relations->find(id);
    return it == s.relations->end() ? nullptr : &it->second;
}

// True if `other` relates to `id` through a slime diet (either direction).
bool Related(const sbx::Relation* r, uintptr_t other) {
    if (!r) return false;
    for (const auto* v : {&r->favourites, &r->plorts, &r->favouriteOf, &r->plortOf})
        if (std::find(v->begin(), v->end(), other) != v->end()) return true;
    return false;
}

// Group tag pill after the last item on the line.
void TagPill(const char* tag, ImU32 accent) {
    const Palette& P = theme::Colors();
    ImFont* f = theme::Body();
    const float size = theme::kSmallSize - 1;
    const ImVec2 ts = TextSize(f, size, tag);
    ImGui::SameLine(0, U(8));
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float h = ts.y + U(4), w = ts.x + U(12);
    const float y = p.y + (ImGui::GetTextLineHeight() - h) * 0.5f;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(ImVec2(p.x, y), ImVec2(p.x + w, y + h), Mix(P.frame, accent, 0.28f), h * 0.5f);
    Text(dl, f, size, ImVec2(p.x + U(6), y + U(2)), P.text, tag);
    ImGui::Dummy(ImVec2(w, ImGui::GetTextLineHeight()));
}

// One row of related items in a tooltip: label, then icon + name for each (at most `max`).
void RelationRow(const sbx::Snapshot& s, const char* label, const std::vector<uintptr_t>& ids, size_t max,
                 bool noLargos = false) {
    std::vector<const sbx::Item*> items;
    size_t total = 0;
    for (uintptr_t id : ids) {
        const sbx::Item* it = FindItem(s, id);
        if (!it || (noLargos && it->kind == sbx::Kind::Largo)) continue;
        ++total;
        if (items.size() < max) items.push_back(it);
    }
    if (items.empty()) return;
    const Palette& P = theme::Colors();
    FontScope f(theme::Body(), theme::kSmallSize);
    ImGui::PushStyleColor(ImGuiCol_Text, P.soft);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::PopStyleColor();
    const float e = U(22);
    for (size_t i = 0; i < items.size(); ++i) {
        ImGui::SameLine(0, i == 0 ? U(8) : U(10));
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float y = p.y + (ImGui::GetFrameHeight() - e) * 0.5f;
        if (!item_grid::DrawIcons(ImGui::GetWindowDrawList(), items[i]->icon, items[i]->icon2, ImVec2(p.x, y),
                                  ImVec2(p.x + e, y + e)))
            ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(p.x + e * 0.5f, y + e * 0.5f), e * 0.25f, KindAccent(items[i]->kind));
        ImGui::Dummy(ImVec2(e, ImGui::GetFrameHeight()));
        if (items.size() <= 2) {  // with one or two, the names fit too
            ImGui::SameLine(0, U(4));
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(items[i]->name.c_str());
        }
    }
    if (total > items.size()) {
        ImGui::SameLine(0, U(6));
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(P.dim), "+%d", static_cast<int>(total - items.size()));
    }
}

// The key hints at the bottom of a Sandbox tooltip: one row per hint, stacked at the tooltip's left
// edge (round 8: not pushed right by the widest line above), each row its keycaps (the HUD edit bar's,
// 80 %) from the left, then its own label right after them.
void TipKeys(const std::vector<KeyRow>& keys) {
    if (keys.empty()) return;
    const Palette& P = theme::Colors();
    ImFont* f = theme::Body();
    const float fs = theme::kSmallSize - 0.5f, cap = g_scale * 0.8f;
    const float gap = U(8), rowH = U(22);
    float blockW = 0;
    for (const KeyRow& k : keys)
        blockW = std::max(blockW, hud_edit::KeyComboWidth(k.keys, cap) + gap + TextSize(f, fs, k.text.c_str()).x);
    ImGui::Dummy(ImVec2(0, U(2)));
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    for (size_t i = 0; i < keys.size(); ++i) {
        const KeyRow& k = keys[i];
        const float cy = std::round(p.y + rowH * (static_cast<float>(i) + 0.5f));
        const float kw = hud_edit::DrawKeyCombo(dl, p.x, cy, k.keys, cap);
        const ImVec2 ts = TextSize(f, fs, k.text.c_str());
        Text(dl, f, fs, ImVec2(p.x + kw + gap, std::round(cy - ts.y * 0.5f)), P.soft, k.text.c_str());
    }
    ImGui::Dummy(ImVec2(blockW, rowH * static_cast<float>(keys.size())));
}

// Item tooltip: the name with its group as a small tag; for slimes, foods and plorts the
// diet links with icons (favourite food, plort, who loves / makes it; no largos under "Made
// by"); optional cost lines with icons (crafter), a dim info line, then the left / right click
// as keycap rows (TipKeys; the hint bar under the page lists the rest). `mod`: one of the mod's own items (its tile
// has the "M" badge): a second tag "Mod" in the accent colour.
void ItemTooltip(const sbx::Snapshot& s, const std::string& name, sbx::Kind kind, uintptr_t id, const char* info = nullptr,
                 const std::vector<sbx::CostLine>* costs = nullptr, const std::vector<KeyRow>& keys = {}, bool mod = false) {
    const Palette& P = theme::Colors();
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(U(14), U(10)));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(U(6), U(4)));
    PushFloatingStyle();
    // A sensible minimum width: a name with only its tag (no favourites, no key rows) looked squashed.
    ImGui::SetNextWindowSizeConstraints(ImVec2(U(kRichTipMinW), 0), ImVec2(FLT_MAX, FLT_MAX));
    if (ImGui::BeginTooltip()) {
        FloatingSurface(false);
        {
            FontScope f(theme::Body(), theme::kSmallSize + 1.5f);
            ImGui::TextUnformatted(name.c_str());
        }
        TagPill(KindTag(kind), KindAccent(kind));
        if (mod) TagPill("Mod", theme::Current() == theme::Mode::Dark ? P.grab : P.accent);
        if (const sbx::Relation* r = id ? FindRelation(s, id) : nullptr) {
            RelationRow(s, "Favourite", r->favourites, 3);
            RelationRow(s, "Plort", r->plorts, 2);
            RelationRow(s, "Favourite of", r->favouriteOf, 6);
            RelationRow(s, "Made by", r->plortOf, 6, true);
        }
        if (costs && !costs->empty()) {
            // Fabricator cost: one line per ingredient, each with its icon.
            FontScope f(theme::Body(), theme::kSmallSize);
            ImGui::Dummy(ImVec2(0, U(1)));
            ImGui::PushStyleColor(ImGuiCol_Text, P.soft);
            ImGui::TextUnformatted("Fabricator cost");
            ImGui::PopStyleColor();
            const float e = U(20);
            ImDrawList* dl = ImGui::GetWindowDrawList();
            for (const sbx::CostLine& c : *costs) {
                const ImVec2 p = ImGui::GetCursorScreenPos();
                const float y = p.y + (ImGui::GetFrameHeight() - e) * 0.5f;
                if (c.currency) {
                    // Newbucks (or another currency): a small coin.
                    const ImVec2 cc(p.x + e * 0.5f, y + e * 0.5f);
                    dl->AddCircleFilled(cc, e * 0.38f, IM_COL32(0xF0, 0xC2, 0x4B, 0xFF));
                    dl->AddCircle(cc, e * 0.38f, IM_COL32(0xA8, 0x7A, 0x1E, 0xFF), 0, std::max(1.0f, U(1.5f)));
                } else if (!item_grid::DrawIcons(dl, c.icon, 0, ImVec2(p.x, y), ImVec2(p.x + e, y + e))) {
                    dl->AddCircleFilled(ImVec2(p.x + e * 0.5f, y + e * 0.5f), e * 0.25f, KindAccent(c.kind));
                }
                ImGui::Dummy(ImVec2(e, ImGui::GetFrameHeight()));
                ImGui::SameLine(0, U(6));
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted(c.text.c_str());
            }
        }
        if (info && *info) {
            FontScope f(theme::Body(), theme::kSmallSize - 1);
            ImGui::PushStyleColor(ImGuiCol_Text, P.dim);
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + U(260));
            ImGui::TextUnformatted(info);
            ImGui::PopTextWrapPos();
            ImGui::PopStyleColor();
        }
        TipKeys(keys);
        ImGui::EndTooltip();
    }
    PopFloatingStyle();
    ImGui::PopStyleVar(2);
}

// The item tooltip's look for anything else on the Sandbox page (upgrades, blueprints, locked slots):
// name, coloured tag, lines in their colours, keycap rows.
void RichTooltip(const sandbox_upgrades::Tip& t) {
    const Palette& P = theme::Colors();
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(U(14), U(10)));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(U(6), U(4)));
    PushFloatingStyle();
    ImGui::SetNextWindowSizeConstraints(ImVec2(U(kRichTipMinW), 0), ImVec2(FLT_MAX, FLT_MAX));
    if (ImGui::BeginTooltip()) {
        FloatingSurface(false);
        {
            FontScope f(theme::Body(), theme::kSmallSize + 1.5f);
            ImGui::TextUnformatted(t.name.c_str());
        }
        if (!t.tag.empty()) TagPill(t.tag.c_str(), t.tagCol ? t.tagCol : P.soft);
        {
            FontScope f(theme::Body(), theme::kSmallSize);
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + U(260));
            for (const auto& [text, col] : t.lines) {
                ImGui::PushStyleColor(ImGuiCol_Text, col ? col : P.soft);
                ImGui::TextUnformatted(text.c_str());
                ImGui::PopStyleColor();
            }
            ImGui::PopTextWrapPos();
        }
        TipKeys(t.keys);
        ImGui::EndTooltip();
    }
    PopFloatingStyle();
    ImGui::PopStyleVar(2);
}

// The tab bar's strip (DrawTopBar) at a smaller size, for tabs inside a card (Upgrades & Blueprints):
// a rounded band in the sidebar colour, the selected tab filled, the hovered one lighter. Switches on
// the click, before the caller draws the tab's content.
void SubTabs(const char* id, const std::vector<std::string>& labels, int& sel) {
    if (labels.empty()) return;
    const Palette& P = theme::Colors();
    ImFont* f = theme::Display();
    constexpr float kFont = 13.5f;
    const float stripH = U(32), tabH = U(26), sp = U(3), avail = ImGui::GetContentRegionAvail().x;
    float textW = 0;
    for (const auto& l : labels) textW += TextSize(f, kFont, l.c_str()).x;
    const float n = static_cast<float>(labels.size());
    // Padding per side: the tab bar's 13 px scaled down, less when the labels would not fit.
    const float padX = std::clamp((avail - U(6) - sp * (n - 1) - textW) / (2 * n), U(4), U(11));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImGui::PushID(id);
    const ImVec2 o = ImGui::GetCursorScreenPos();
    const float stripW = U(6) + sp * (n - 1) + textW + 2 * padX * n, cy = o.y + stripH * 0.5f;
    dl->AddRectFilled(o, ImVec2(o.x + stripW, o.y + stripH), P.side, U(11));
    float x = o.x + U(3);
    for (size_t i = 0; i < labels.size(); ++i) {
        const ImVec2 ts = TextSize(f, kFont, labels[i].c_str());
        const float tw = ts.x + 2 * padX;
        ImGui::SetCursorScreenPos(ImVec2(x, cy - tabH * 0.5f));
        ImGui::PushID(static_cast<int>(i));
        if (nav::Button("##sub", ImVec2(tw, tabH), ImGuiButtonFlags_PressedOnClick)) sel = static_cast<int>(i);
        const bool hovered = ImGui::IsItemHovered();
        ImGui::PopID();
        const bool on = sel == static_cast<int>(i);
        ImU32 bg = 0, ink = P.soft;
        if (on) bg = P.tabSel, ink = P.tabInk;
        else if (hovered) bg = P.hover, ink = P.text;
        const ImVec2 a(x, cy - tabH * 0.5f);
        if (bg) dl->AddRectFilled(a, ImVec2(a.x + tw, a.y + tabH), bg, U(8));
        Text(dl, f, kFont, ImVec2(a.x + padX, std::round(cy - ts.y * 0.5f)), ink, labels[i].c_str());
        nav::Ring(U(8));
        x += tw + sp;
    }
    ImGui::SetCursorScreenPos(o);
    ImGui::Dummy(ImVec2(stripW, stripH));
    ImGui::PopID();
}

// The Sandbox pages' key hints: a bar pinned under the page (Draw reserves kSbxBarH under the page's
// body on the pages with grids, SandboxHintBarShown), a little wider than the two columns, in the
// floating surfaces' look (tooltips, menus: popup colour, outline, soft shadow). It shows the controls
// of the grid under the mouse (g_sbxHover), each a keycap (or several joined by '+') and a short
// label, centred; the keycaps are the HUD edit help bar's at 80 % of its size, the labels 4 px over
// the small size's 75 %. Nothing hovered: a dim line saying what the bar is for.
constexpr float kSbxBarH = 50;      // the strip under the page (bar + margins), before scaling
constexpr float kSbxBarSpill = 8;   // how far the bar reaches past the columns on each side

void SandboxHintBar(ImDrawList* dl, ImVec2 a, ImVec2 b) {
    const Palette& P = theme::Colors();
    static const KeyRow kSlots[] = {{"LMB", "select"}, {"Shift+Wheel", "\u00b11"}, {"Ctrl+Wheel", "\u00b110"},
                                    {"Drag", "move / swap"}, {"RMB", "context menu"}};
    static const KeyRow kPicker[] = {{"LMB", "put in a slot"},    {"Ctrl+LMB", "select"}, {"Shift+Wheel", "\u00b11"},
                                     {"Ctrl+Wheel", "\u00b110"}, {"Drag", "to a slot"},  {"RMB", "context menu / clear"}};
    static const KeyRow kSpawner[] = {{"LMB", "spawn"},            {"Ctrl+LMB", "select"}, {"Shift+Wheel", "\u00b11"},
                                      {"Ctrl+Wheel", "\u00b110"}, {"RMB", "context menu / clear"}};
    static const KeyRow kCrafter[] = {{"LMB", "select"}, {"Shift+Wheel", "\u00b11"}, {"Ctrl+Wheel", "\u00b110"},
                                      {"RMB", "clear"}};
    static const KeyRow kUpgrades[] = {{"Shift+Wheel", "\u00b11 level"}, {"Ctrl+Wheel", "max / none"}, {"RMB", "context menu"}};
    static const KeyRow kBlueprints[] = {{"LMB", "unlock / lock"}, {"RMB", "context menu"}};
    const KeyRow* hints = nullptr;
    size_t count = 0;
    switch (g_sbxHover) {
        case SbxHover::Slots: hints = kSlots, count = std::size(kSlots); break;
        case SbxHover::Picker: hints = kPicker, count = std::size(kPicker); break;
        case SbxHover::Spawner: hints = kSpawner, count = std::size(kSpawner); break;
        case SbxHover::Crafter: hints = kCrafter, count = std::size(kCrafter); break;
        case SbxHover::Upgrades: hints = kUpgrades, count = std::size(kUpgrades); break;
        case SbxHover::Blueprints: hints = kBlueprints, count = std::size(kBlueprints); break;
        case SbxHover::None: break;
    }
    g_sbxBarMin = a, g_sbxBarMax = b;
    if (b.x - a.x < U(60)) return;
    const float r = ImGui::GetStyle().PopupRounding;
    SoftShadow(dl, a, b, r, U(1), U(4), P.shadow);
    dl->AddRectFilled(a, b, P.popBg, r);
    dl->AddRect(a, b, P.popBorder, r, 0, std::max(1.0f, U(1)));
    ImFont* f = theme::Body();
    const float fs = theme::kSmallSize * 0.75f + 4.0f;
    const ImVec2 ts = TextSize(f, fs, "Ag");
    const float cy = std::round((a.y + b.y) * 0.5f);
    const float pad = U(14);
    if (!hints) {
        const char* idle = "Point at a grid to see its controls.";
        const ImVec2 is = TextSize(f, fs, idle);
        Text(dl, f, fs, ImVec2(std::round((a.x + b.x - is.x) * 0.5f), std::round(cy - is.y * 0.5f)), P.dim, idle);
        return;
    }
    const float capScale = g_scale * 0.8f;
    const float gap = U(18);
    // What fits, from the first (the hints are ordered by importance; the rest is left out), centred.
    std::vector<float> widths;
    float total = 0;
    for (size_t k = 0; k < count; ++k) {
        const float w = hud_edit::KeyComboWidth(hints[k].keys, capScale) + U(4) + TextSize(f, fs, hints[k].text.c_str()).x;
        if (total + (widths.empty() ? 0.0f : gap) + w > b.x - a.x - 2 * pad) break;
        total += (widths.empty() ? 0.0f : gap) + w;
        widths.push_back(w);
    }
    float x = std::round((a.x + b.x - total) * 0.5f);
    dl->PushClipRect(a, b, true);
    for (size_t k = 0; k < widths.size(); ++k) {
        const KeyRow& h = hints[k];
        x += hud_edit::DrawKeyCombo(dl, x, cy, h.keys, capScale);
        Text(dl, f, fs, ImVec2(x + U(4), std::round(cy - ts.y * 0.5f)), P.soft, h.text.c_str());
        x += U(4) + TextSize(f, fs, h.text.c_str()).x + gap;
    }
    dl->PopClipRect();
}

void TabTip(const item_grid::Result& r, const std::vector<item_grid::Tab>& tabs) {
    if (r.hoveredTab >= 0 && static_cast<size_t>(r.hoveredTab) < tabs.size()) Tooltip(tabs[static_cast<size_t>(r.hoveredTab)].tip);
}

// A slot's count as the tab last asked for it (quick wheel notches add up before the next snapshot).
int SlotCount(const sbx::Slot& sl, int idx) {
    const SlotPending& p = g_slotPending[idx & 31];
    return p.until > ImGui::GetTime() ? p.count : sl.count;
}

void SetSlotPending(int idx, uintptr_t item, int count) { g_slotPending[idx & 31] = {item, count, ImGui::GetTime() + 0.8}; }

// What a slot holds as the tab last asked for it (see SlotCount).
uintptr_t SlotItem(const sbx::Slot& sl, int idx) {
    const SlotPending& p = g_slotPending[idx & 31];
    return p.until > ImGui::GetTime() ? p.item : sl.item;
}

// True if a vacpack item fits somewhere without removing anything: an unlocked slot that
// accepts it and is empty or holds it and is not full (where a "Give" entry puts it).
bool HasRoomFor(const sbx::Snapshot& s, const sbx::Item& it) {
    const int n = std::min(static_cast<int>(s.slots.size()), 32);
    for (int i = 0; i < n; ++i) {
        const sbx::Slot& sl = s.slots[static_cast<size_t>(i)];
        if (!sl.unlocked || !(it.slotMask & (1u << i))) continue;
        const uintptr_t holds = SlotItem(sl, i);
        if (!holds || (holds == it.id && SlotCount(sl, i) < sl.max)) return true;
    }
    return false;
}

// Puts `amount` (<= 0: the slot's maximum) of a vacpack item into slot `idx`.
void PutInSlot(const sbx::Snapshot& s, int idx, const sbx::Item& it, int amount) {
    const sbx::Slot& sl = s.slots[static_cast<size_t>(idx)];
    const int n = amount > 0 ? std::min(amount, std::max(1, sl.max)) : std::max(1, sl.max);
    SetSlotPending(idx, it.id, n);
    SbxRequest(sbx::Op::PutItem, n, idx, it.id);
}

// A click on a picker item: into a free slot that takes it, so a click never throws anything out
// while there is room ("clicking overrides a slot"): first a slot that already holds it and is not
// full (the selected one first), then an empty one (the selected one first), each in slot order.
// Only with neither the selected slot's contents are replaced. `amount` <= 0: up to the slot's maximum.
void ClickPut(const sbx::Snapshot& s, const sbx::Item& it, int amount) {
    const int n = std::min(static_cast<int>(s.slots.size()), 32);
    auto fits = [&](int i) {
        const sbx::Slot& sl = s.slots[static_cast<size_t>(i)];
        return sl.unlocked && (it.slotMask & (1u << i)) != 0;
    };
    auto find = [&](auto&& want) {
        if (g_sbxSlot >= 0 && g_sbxSlot < n && fits(g_sbxSlot) && want(g_sbxSlot)) return g_sbxSlot;
        for (int i = 0; i < n; ++i)
            if (fits(i) && want(i)) return i;
        return -1;
    };
    const int same = find([&](int i) {
        const sbx::Slot& sl = s.slots[static_cast<size_t>(i)];
        return SlotItem(sl, i) == it.id && SlotCount(sl, i) < sl.max;
    });
    if (same >= 0) {
        const sbx::Slot& sl = s.slots[static_cast<size_t>(same)];
        const int cur = SlotCount(sl, same);
        const int next = amount > 0 ? std::min(sl.max, cur + amount) : sl.max;
        SetSlotPending(same, it.id, next);
        SbxRequest(sbx::Op::SetSlotCount, next, same);
        return;
    }
    const int empty = find([&](int i) { return SlotItem(s.slots[static_cast<size_t>(i)], i) == 0; });
    if (empty >= 0) {
        PutInSlot(s, empty, it, amount);
        return;
    }
    if (SlotItem(s.slots[static_cast<size_t>(g_sbxSlot)], g_sbxSlot) == it.id) return;  // full of it already
    PutInSlot(s, g_sbxSlot, it, amount);  // no free slot: replace what the selected one holds
}

// "Put in slots" for one item (sbx::Op::StowItem): the module fills the selected slot, then
// the slots that hold the item or are empty, and drops what is left in front of the player.
void StowItem(int sel, uintptr_t id, int amount, bool replace) {
    sbx::Action a;
    a.op = sbx::Op::StowItem;
    a.slot = sel;
    a.item = id;
    a.value = amount;
    a.replace = replace;
    sbx::Request(a);
}

// Right-click menu for an item (picker tiles, vacpack slots, spawner tiles): "Give: X" (vacpack)
// or "Spawn: X" (spawner) for the related slimes, foods and plorts from the diets, done the grid's
// way (vacpack: into a slot that holds it or an empty one, the slot's maximum, nothing is replaced;
// spawner: spawn in front of you, SpawnAmount). A slime (not a largo) gets "Give: <slime>" (round 8:
// into the vacpack, Op::StowItem, a slot that holds it and has room or an empty one; grayed with the
// reason when none can take it), in the vacpack also "Spawn: <slime>" before it, and "Spawn: Radiant <slime>" (round 12:
// the game's rare radiant form; a largo in the spawner gets one per base slime it can be radiant from). Opened on a vacpack
// slot (g_ctxSlot), the menu starts with that slot's Fill and Empty (the buttons under the slots). Largos (of a slime, of a plort, or loving a food) are always spawned (SpawnAmount), since
// the vacpack can't hold them, and read
// "Spawn: <this item's slime> <other> Largo". They are not listed directly: one "Largos" row opens
// them in a submenu. That is
// ImGui's BeginMenu, so the first menu stays open while the submenu is used, and its safe
// triangle (from the cursor to the submenu's near edge) keeps the submenu open while the mouse
// moves diagonally into it across the other rows.
// How many a context-menu "Spawn: X" entry spawns: X's amount picked in the spawner, else the amount
// picked on the right-clicked item in its grid, else 1 (at most kSpawnMax).
int SpawnAmount(uintptr_t id) {
    if (const auto it = g_spawnPick.find(id); it != g_spawnPick.end() && it->second > 0) return std::min(it->second, kSpawnMax);
    const auto& pick = g_ctxGrid == SbxGrid::Spawner ? g_spawnPick : g_vacPick;
    if (const auto it = pick.find(g_ctxItem); it != pick.end() && it->second > 0) return std::min(it->second, kSpawnMax);
    return 1;
}

void ItemContextMenu(const sbx::Snapshot& s) {
    const Palette& P = theme::Colors();
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(U(8), U(8)));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(U(6), U(4)));
    PushFloatingStyle();
    if (ImGui::BeginPopup(kSbxCtx)) {
        FloatingSurface(true);
        const sbx::Item* self = FindItem(s, g_ctxItem);
        const sbx::Relation* r = FindRelation(s, g_ctxItem);
        const bool vac = g_ctxGrid == SbxGrid::Vacpack;
        if (!self) {
            ImGui::CloseCurrentPopup();
        } else {
            {
                FontScope f(theme::Body(), theme::kSmallSize);
                ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(P.soft), "%s", self->name.c_str());
            }
            const float e = U(22), rowW = U(250), fs = theme::kSmallSize + 0.5f;
            if (vac && g_ctxSlot >= 0 && g_ctxSlot < std::min(static_cast<int>(s.slots.size()), 32)) {
                // The slot's Fill / Empty, as the buttons under the slots do them: two buttons side by side
                // on one row (round 9, "fill/empty should be buttons next to eachother ... on a single line").
                const int idx = g_ctxSlot;
                const sbx::Slot& sl = s.slots[static_cast<size_t>(idx)];
                const uintptr_t holds = SlotItem(sl, idx);
                const int count = SlotCount(sl, idx), max = std::max(0, sl.max);
                if (sl.unlocked && holds) {
                    FontScope bf(theme::Body(), fs);
                    const float sp = ImGui::GetStyle().ItemSpacing.x, bw = std::floor((rowW - sp) * 0.5f);
                    // Just "Fill", no amount (round 11: "Just "Fill" text, no amount in parenthesis").
                    ImGui::BeginDisabled(count >= max);
                    if (ImGui::Button("Fill", ImVec2(bw, e + U(4)))) {
                        SetSlotPending(idx, holds, max);
                        SbxRequest(sbx::Op::SetSlotCount, max, idx);
                        ImGui::CloseCurrentPopup();
                    }
                    ImGui::EndDisabled();
                    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                        Tooltip(count >= max ? std::string("The slot is full.")
                                             : std::format("Fill slot {} to its maximum ({} / {}).", idx + 1, count, max));
                    ImGui::SameLine(0, sp);
                    if (ImGui::Button("Empty", ImVec2(bw, e + U(4)))) {
                        SetSlotPending(idx, 0, 0);
                        SbxRequest(sbx::Op::SetSlotCount, 0, idx);
                        ImGui::CloseCurrentPopup();
                    }
                    if (ImGui::IsItemHovered()) Tooltip(std::format("Remove everything from slot {}.", idx + 1));
                    ImGui::Separator();
                }
            }
            struct Give {
                std::string label;
                const sbx::Item* it;
                bool can;
                bool spawn;        // spawned in front of the player (largos always: the vacpack can't hold them)
                std::string sort;  // largos: the other slime's short name
                const char* why = nullptr;  // grayed out: the tooltip (else the default reason)
                int radiant = -1;  // >= 0: spawns a radiant form (Op::SpawnRadiant, slot = this)
            };
            std::vector<Give> gives, largos;
            auto spawnable = [&](uintptr_t id) -> const sbx::Item* {
                if (s.spawnables)
                    for (const sbx::Item& c : *s.spawnables)
                        if (c.id == id) return &c;
                return nullptr;
            };
            auto collect = [&](uintptr_t id) {
                // Vacpack: it must be in the vacpack list with room in a slot; spawner: in the spawn list.
                const sbx::Item* it = nullptr;
                bool can = false;
                if (vac && s.catalog) {
                    for (const sbx::Item& c : *s.catalog)
                        if (c.id == id) {
                            it = &c;
                            can = HasRoomFor(s, c);
                            break;
                        }
                } else if (!vac) {
                    if ((it = spawnable(id))) can = !(it->tarr && s.tarrsOff);
                }
                if (!it) it = FindItem(s, id);  // known from the other list: shown grayed out
                if (!it || it->kind == sbx::Kind::Largo) return;
                for (const Give& g : gives)
                    if (g.it == it) return;
                gives.push_back({std::format("{}: {}", vac ? "Give" : "Spawn", it->name), it, can, !vac, {}});
            };
            // Largos are always spawned (they can't go in the vacpack), labelled "Spawn: <this item's base
            // slime> <other> Largo" whatever order the game's own name has, sorted by the other slime.
            auto shortName = [&](uintptr_t id, const std::string& fallback) {
                const sbx::Item* b = id ? FindItem(s, id) : nullptr;
                std::string n = b ? b->name : fallback;
                constexpr std::string_view kSuffix = " Slime";
                if (n.size() > kSuffix.size() && n.ends_with(kSuffix)) n.resize(n.size() - kSuffix.size());
                return n;
            };
            uintptr_t baseOf = 0;  // the hovered item's base slime: the slime itself, or the slime a plort comes from
            if (self->kind == sbx::Kind::Slime) baseOf = self->id;
            else if (self->kind == sbx::Kind::Plort && r)
                for (uintptr_t id : r->plortOf)
                    if (const sbx::Item* m = FindItem(s, id); m && m->kind == sbx::Kind::Slime) {
                        baseOf = id;
                        break;
                    }
            auto collectLargo = [&](uintptr_t id) {
                const sbx::Item* it = spawnable(id);
                const bool can = it && !(it->tarr && s.tarrsOff);
                if (!it) it = FindItem(s, id);
                if (!it || it->kind != sbx::Kind::Largo) return;
                for (const Give& g : largos)
                    if (g.it == it) return;
                std::string first, other;
                if (baseOf && (it->base == baseOf || it->base2 == baseOf)) {
                    first = shortName(baseOf, {});
                    other = it->base == baseOf ? shortName(it->base2, it->family2) : shortName(it->base, it->family);
                } else {
                    first = shortName(0, it->family);
                    other = shortName(0, it->family2);
                }
                std::string label = first.empty() || other.empty() ? "Spawn: " + it->name
                                                                   : std::format("Spawn: {} {} Largo", first, other);
                largos.push_back({std::move(label), it, can, true, other.empty() ? it->name : other});
            };
            if (r) {
                for (uintptr_t id : r->favouriteOf) collect(id);
                for (uintptr_t id : r->favourites) collect(id);
                for (uintptr_t id : r->plorts) collect(id);
                for (uintptr_t id : r->plortOf) collect(id);
                for (uintptr_t id : r->largos) collectLargo(id);
                for (uintptr_t id : r->plortOf) collectLargo(id);
                for (uintptr_t id : r->favouriteOf) collectLargo(id);
            }
            std::stable_sort(largos.begin(), largos.end(), [](const Give& a, const Give& b) { return a.sort < b.sort; });
            int row = 0;
            auto entry = [&](const Give& g) {
                ImGui::PushID(++row);
                const ImVec2 p = ImGui::GetCursorScreenPos();
                const std::string& label = g.label;
                ImGui::BeginDisabled(!g.can);
                const bool hit = ImGui::Selectable("##give", false, 0, ImVec2(rowW, e));
                ImGui::EndDisabled();
                ImDrawList* dl = ImGui::GetWindowDrawList();
                item_grid::DrawIcons(dl, g.it->icon, g.it->icon2, p, ImVec2(p.x + e, p.y + e), g.can ? IM_COL32_WHITE : IM_COL32(255, 255, 255, 90));
                const ImVec2 ts = TextSize(theme::Body(), fs, label.c_str());
                Text(dl, theme::Body(), fs, ImVec2(p.x + e + U(8), p.y + (e - ts.y) * 0.5f), g.can ? P.text : P.dim, label.c_str());
                if (!g.can && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                    Tooltip(g.why                            ? g.why
                            : g.it->tarr && s.tarrsOff ? "Tarrs are disabled in this save's game settings."
                            : g.spawn                  ? "Not in the spawn list."
                                                       : "No unlocked vacpack slot has room for this (holding it, not full, or "
                                                         "empty). Empty a slot first.");
                if (hit && g.can) {
                    if (g.radiant >= 0) SbxRequest(sbx::Op::SpawnRadiant, SpawnAmount(g.it->id), g.radiant, g.it->id);
                    else if (g.spawn) SbxRequest(sbx::Op::Spawn, SpawnAmount(g.it->id), -1, g.it->id);
                    else StowItem(g_sbxSlot, g.it->id, 0, false);
                }
                ImGui::PopID();
            };
            // The game's radiant (rare, glowing) form of this slime: "Spawn: Radiant Pink Slime"; a largo has one per
            // base slime it can be radiant from: "Spawn: Radiant Pink Tabby Largo (radiant Pink)". Spawned in front of
            // you like the spawner's tiles (Op::SpawnRadiant). Grayed with the reason when the game has none.
            auto radiantEntries = [&] {
                const sbx::Item* sp = spawnable(self->id);
                const sbx::Item* it = sp ? sp : self;
                const bool can = sp && !(sp->tarr && s.tarrsOff);
                const char* why = !sp ? "Not in the spawn list." : nullptr;
                if (self->kind == sbx::Kind::Slime || !sp || !sp->radiant) {
                    Give g{"Spawn: Radiant " + self->name, it, can && (sp->radiant & sbx::kRadiantSelf) != 0, true, {}, why, 0};
                    if (g.can) g.why = nullptr;
                    else if (!g.why && can) g.why = "The game has no radiant form of this slime.";
                    entry(g);
                    return;
                }
                const uintptr_t bases[2] = {sp->radiantBase, sp->radiantBase2};
                for (int i = 0; i < 2; ++i) {
                    if (!(sp->radiant & (i == 0 ? sbx::kRadiantBase0 : sbx::kRadiantBase1))) continue;
                    entry({std::format("Spawn: Radiant {} (radiant {})", self->name, shortName(bases[i], "?")), it, can,
                           true, {}, nullptr, i + 1});
                }
            };
            if (self->kind == sbx::Kind::Largo && !vac) radiantEntries();
            if (self->kind == sbx::Kind::Slime) {
                // The slime itself: spawned in front of you (vacpack; the spawner's click does that), then given
                // into the vacpack like the Give entries.
                if (vac) {
                    const sbx::Item* sp = spawnable(self->id);
                    const bool can = sp && !(sp->tarr && s.tarrsOff);
                    entry({"Spawn: " + self->name, sp ? sp : self, can, true, {}});
                }
                radiantEntries();
                const sbx::Item* ci = nullptr;
                if (s.catalog)
                    for (const sbx::Item& c : *s.catalog)
                        if (c.id == self->id) ci = &c;
                Give g{"Give: " + self->name, ci ? ci : self, ci && HasRoomFor(s, *ci), false, {}};
                if (!s.catalog) g.why = "The vacpack's item list is not read yet.";
                else if (!ci) g.why = "No vacpack slot takes this slime.";
                entry(g);
            }
            for (const Give& g : gives) entry(g);
            if (!largos.empty()) {
                // "Largos  >": the first largo's icon, the label and count, ImGui's submenu arrow.
                ImDrawList* dl = ImGui::GetWindowDrawList();
                const ImVec2 p = ImGui::GetCursorScreenPos();
                ImGui::PushFont(theme::Body(), 22.0f);  // the row is one font size high: as high as the entries
                const bool open = ImGui::BeginMenu("##largos");
                ImGui::PopFont();
                const sbx::Item* first = largos.front().it;
                item_grid::DrawIcons(dl, first->icon, first->icon2, p, ImVec2(p.x + e, p.y + e));
                const std::string label = std::format("Largos ({})", largos.size());
                const ImVec2 ts = TextSize(theme::Body(), fs, label.c_str());
                Text(dl, theme::Body(), fs, ImVec2(p.x + e + U(8), p.y + (e - ts.y) * 0.5f), P.text, label.c_str());
                if (open) {
                    FloatingSurface(true);
                    for (const Give& g : largos) entry(g);
                    ImGui::EndMenu();
                }
            }
            if (gives.empty() && largos.empty() && self->kind != sbx::Kind::Largo)
                DimText("No favourites or plorts known for this item.");
        }
        if (PopupCancelled()) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    PopFloatingStyle();
    ImGui::PopStyleVar(2);
}

void OpenItemContext(uintptr_t id, SbxGrid grid, int slot = -1) {
    g_ctxItem = id;
    g_ctxGrid = grid;
    g_ctxSlot = slot;
    ImGui::OpenPopup(kSbxCtx);
}

// Vacpack: the slots in a row across the top, Fill / Empty for the selected slot, then a grid
// of what that slot accepts with group tabs, search and "Put in slots" (the controls: SandboxHintBar).
//   * Item: click puts it into a free slot (ClickPut; the selected one is replaced only when none is
//     free); Ctrl+click selects several; Shift+wheel / Ctrl+wheel
//     sets the amount (+-1 / +-10, shown on the tile); drag onto any slot; right-click: give menu.
//   * Slot: click selects it; Shift+wheel / Ctrl+wheel changes its count (an empty one takes the
//     last picked item); drag onto another slot to move / swap, or onto the grid to empty it;
//     right-click: give menu; a locked slot shows a lock that unlocks it on click.
//   * A plain wheel always scrolls (the grid, else the page).
//   * Right-click on the grid between tiles clears the selection.
void SandboxVacpack(const sbx::Snapshot& s, float w) {
    const Palette& P = theme::Colors();
    BeginCard("sbx_vac", w);
    CardHeading("Vacpack", P.head);
    if (s.slots.empty()) {
        DimText("No vacpack slots found.");
        EndCard();
        return;
    }
    const int n = std::min(static_cast<int>(s.slots.size()), 32);
    g_vacPickerTop = FLT_MAX;  // set where the item grid starts (not drawn: the whole card is the slots)
    g_sbxSlot = std::clamp(g_sbxSlot, 0, n - 1);
    if (!s.slots[static_cast<size_t>(g_sbxSlot)].unlocked)  // move off a locked slot
        for (int i = 0; i < n; ++i)
            if (s.slots[static_cast<size_t>(i)].unlocked) {
                g_sbxSlot = i;
                break;
            }
    const ImGuiIO& io = ImGui::GetIO();
    const sbx::Item* focus = g_vacFocus ? FindItem(s, g_vacFocus) : nullptr;

    // ---- slot row ----
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImFont* body = theme::Body();
    const float gap = U(8), avail = ImGui::GetContentRegionAvail().x;
    const float e = std::max(U(24), std::floor(std::min(U(92), (avail - gap * static_cast<float>(n - 1)) / static_cast<float>(n))));
    uintptr_t slotMenu = 0;  // right-clicked slot's item: its give menu opens after the loop (popup id outside the slot's id)
    int slotMenuIdx = -1;    // that slot
    nav::TabGroup slotNav("##slotnav");  // the slots are one Tab stop, the arrows move between them
    // Each slot's item is its padded cell: the tile plus half the gap to each neighbour, so the cells
    // touch and the tooltip does not flicker between slots (round 12). The tile is drawn inside it.
    const float half = gap * 0.5f;
    for (int i = 0; i < n; ++i) {
        const sbx::Slot& sl = s.slots[static_cast<size_t>(i)];
        const int count = SlotCount(sl, i);
        ImGui::PushID(i);
        if (i > 0) ImGui::SameLine(0, 0);
        const float cl = i > 0 ? half : 0.0f, cr = i < n - 1 ? half : 0.0f;
        slotNav.Item(static_cast<uint64_t>(i));
        const bool pressed = nav::Button("##slot", ImVec2(cl + e + cr, e),
                                         ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
        slotNav.Done();
        const bool right = (pressed && ImGui::IsMouseReleased(ImGuiMouseButton_Right)) || nav::ContextKey();
        const bool clicked = pressed && !right;
        const bool hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
        const bool hot = hovered && (sl.unlocked || sl.unlockable);
        const ImVec2 a(ImGui::GetItemRectMin().x + cl, ImGui::GetItemRectMin().y);
        const ImVec2 b(a.x + e, ImGui::GetItemRectMax().y);
        if (clicked) {
            if (sl.unlocked) g_sbxSlot = i;
            else if (sl.unlockable) SbxRequest(sbx::Op::UnlockSlot, 0, i);
        }
        // Right-click: only a slot with an item has a menu (round 9: the Extra Tank tier menu on empty /
        // locked slots is gone; the tier is on the Upgrades page).
        if (right && sl.unlocked && sl.item) slotMenu = sl.item, slotMenuIdx = i;
        if (sl.unlocked) {
            // Shift / Ctrl + wheel over a slot: its count +-1 / +-10; an empty slot takes the last
            // picked item. A plain wheel is left alone, so the page scrolls.
            if (hovered && (io.KeyShift || io.KeyCtrl)) {
                ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY);
                ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelX);
                const int step = item_grid::WheelStep(io.MouseWheel != 0.0f ? io.MouseWheel : io.MouseWheelH);
                if (step != 0) {
                    const uintptr_t holds = SlotItem(sl, i);
                    if (holds) {
                        const int next = std::clamp(count + step, 0, std::max(0, sl.max));
                        if (next != count) {
                            SetSlotPending(i, next > 0 ? holds : 0, next);
                            SbxRequest(sbx::Op::SetSlotCount, next, i);
                        }
                    } else if (focus && (focus->slotMask & (1u << i)) && step > 0) {
                        PutInSlot(s, i, *focus, step);
                    }
                }
            }
            if (sl.item && ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceNoPreviewTooltip)) {
                ImGui::SetDragDropPayload(kSlotDrag, &i, sizeof(i));
                ImGui::EndDragDropSource();
            }
            if (ImGui::BeginDragDropTarget()) {
                if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(item_grid::kDragType); p && p->DataSize == sizeof(uintptr_t)) {
                    const uintptr_t id = *static_cast<const uintptr_t*>(p->Data);
                    if (const sbx::Item* it = FindItem(s, id)) {
                        g_sbxSlot = i;
                        const auto pick = g_vacPick.find(id);
                        PutInSlot(s, i, *it, pick != g_vacPick.end() ? pick->second : 0);
                    }
                }
                if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(kSlotDrag); p && p->DataSize == sizeof(int)) {
                    const int from = *static_cast<const int*>(p->Data);
                    if (from != i) SbxRequest(sbx::Op::MoveSlot, i, from);
                    g_sbxSlot = i;
                }
                ImGui::EndDragDropTarget();
            }
        }
        item_grid::Tile t;
        t.id = sl.item;
        t.name = sl.item ? &sl.name : nullptr;
        t.icon = sl.item ? sl.icon : 0;
        t.icon2 = sl.item ? sl.icon2 : 0;
        t.accent = sl.item ? KindAccent(sl.kind) : 0;
        t.dimmed = !sl.unlocked;
        item_grid::DrawTile(dl, a, b, t, hot, sl.unlocked && i == g_sbxSlot, g_scale);
        nav::Ring(a, b, U(6));
        const float fs = theme::kSmallSize;
        Text(dl, body, fs, ImVec2(a.x + U(6), a.y + U(4)), P.dim, std::to_string(i + 1).c_str());
        if (!sl.unlocked) {
            // Lock; over an unlockable slot it opens on hover.
            const ImU32 lc = hot ? (theme::Current() == theme::Mode::Dark ? P.grab : P.accent) : P.dim;
            item_grid::DrawLock(dl, ImVec2((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f + U(2)), std::min(U(26), e * 0.42f),
                                hot && sl.unlockable, lc);
        } else if (!sl.item) {
            const ImVec2 ts = TextSize(body, fs, "Empty");
            Text(dl, body, fs, ImVec2((a.x + b.x - ts.x) * 0.5f, (a.y + b.y - ts.y) * 0.5f), P.dim, "Empty");
        } else {
            const std::string c = std::format("{}/{}", count, sl.max);
            const ImVec2 cs = TextSize(body, fs, c.c_str());
            const ImVec2 cp(b.x - U(6) - cs.x, b.y - U(6) - cs.y);
            Text(dl, body, fs, ImVec2(cp.x + 1, cp.y + 1), IM_COL32(0, 0, 0, 150), c.c_str());
            Text(dl, body, fs, cp, P.text, c.c_str());
        }
        if (hot && !sl.unlocked) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort) && !ImGui::GetDragDropPayload() && !ImGui::IsPopupOpen(kSbxCtx)) {
            // The item's name, group and favourites, the left / right click as keycaps (the hint bar
            // lists the rest).
            sandbox_upgrades::Tip tip;
            tip.name = std::format("Slot {}", i + 1);
            if (!sl.unlocked) {
                tip.tag = "Locked";
                tip.tagCol = P.dim;
                if (sl.unlockable) tip.keys = {{"LMB", "unlock for free"}};
                else tip.lines.push_back({"No vacpack upgrade for it was found.", P.dim});
                RichTooltip(tip);
            } else if (sl.item) {
                ItemTooltip(s, sl.name, sl.kind, sl.item, nullptr, nullptr, {{"LMB", "select"}, {"RMB", "context menu"}});
            } else {
                tip.tag = "Empty";
                tip.tagCol = P.dim;
                tip.keys = {{"LMB", "select"}};
                RichTooltip(tip);
            }
        }
        ImGui::PopID();
    }
    if (slotMenu) OpenItemContext(slotMenu, SbxGrid::Vacpack, slotMenuIdx);
    // A dragged slot: its icon follows the cursor.
    int draggedSlot = -1;
    if (const ImGuiPayload* p = ImGui::GetDragDropPayload(); p && p->IsDataType(kSlotDrag) && p->DataSize == sizeof(int)) {
        const int from = *static_cast<const int*>(p->Data);
        if (from >= 0 && from < n) {
            draggedSlot = from;
            const sbx::Slot& sl = s.slots[static_cast<size_t>(from)];
            item_grid::Tile ghost;
            ghost.id = sl.item;
            ghost.name = &sl.name;
            ghost.icon = sl.icon;
            ghost.icon2 = sl.icon2;
            ghost.accent = KindAccent(sl.kind);
            const ImVec2 m = ImGui::GetMousePos(), ga(m.x + U(10), m.y + U(10));
            item_grid::DrawTile(ImGui::GetForegroundDrawList(), ga, ImVec2(ga.x + U(58), ga.y + U(58)), ghost, true, false, g_scale);
        }
    }

    const sbx::Slot& sl = s.slots[static_cast<size_t>(g_sbxSlot)];
    if (!sl.unlocked) {
        DimText("Every slot is locked. Click a lock to unlock its slot.");
        ItemContextMenu(s);
        EndCard();
        return;
    }

    // ---- selected slot: Fill / Empty ----
    ImGui::Dummy(ImVec2(0, U(2)));
    const int max = std::max(0, sl.max);
    {
        const float sp = ImGui::GetStyle().ItemSpacing.x, bw = U(84);
        const float x0 = ImGui::GetCursorPosX(), row = ImGui::GetContentRegionAvail().x;
        ImGui::SetCursorPosX(x0 + row - 2 * bw - sp);
        ImGui::BeginDisabled(!sl.item || sl.count >= max);
        if (ImGui::Button("Fill", ImVec2(bw, 0))) SbxRequest(sbx::Op::SetSlotCount, max, g_sbxSlot);
        ImGui::EndDisabled();
        HoverTip("Fill the selected slot to its maximum.");
        ImGui::SameLine();
        ImGui::BeginDisabled(!sl.item);
        if (ImGui::Button("Empty", ImVec2(bw, 0))) SbxRequest(sbx::Op::SetSlotCount, 0, g_sbxSlot);
        ImGui::EndDisabled();
        HoverTip("Remove everything from the selected slot.");
    }

    // ---- items the selected slot accepts ----
    ImGui::Dummy(ImVec2(0, U(2)));
    const uint32_t bit = 1u << g_sbxSlot;
    static std::vector<int> vacGroups, vacShown;  // sbx::Group (or kGroupMod) per tab (this frame, last frame)
    const auto& tabs = GroupTabs(s, vacGroups, bit);
    item_grid::PredictTab(g_vacGrid);  // a tab pressed this frame filters this frame's tiles (last frame's indices)
    if (vacGroups != vacShown) {  // the tab list changed (the Mod tab comes and goes with the slot): stay on the same group
        const int was = g_vacGrid.tab >= 0 && static_cast<size_t>(g_vacGrid.tab) < vacShown.size()
                            ? vacShown[static_cast<size_t>(g_vacGrid.tab)]
                            : sbx::kGroupAll;
        const auto at = std::find(vacGroups.begin(), vacGroups.end(), was);
        g_vacGrid.tab = at != vacGroups.end() ? static_cast<int>(at - vacGroups.begin()) : 0;
        vacShown = vacGroups;
    }
    g_vacGrid.tab = std::clamp(g_vacGrid.tab, 0, static_cast<int>(vacGroups.size()) - 1);
    const int vacGroup = vacGroups[static_cast<size_t>(g_vacGrid.tab)];
    const sbx::Relation* hoverRel = nullptr;
    static uintptr_t lastHover = 0;  // hovered last frame (related tiles light up)
    if (lastHover) hoverRel = FindRelation(s, lastHover);
    std::vector<item_grid::Tile> tiles;
    std::vector<const sbx::Item*> src;
    if (s.catalog) {
        tiles.reserve(s.catalog->size());
        for (const sbx::Item& it : *s.catalog) {
            if (!(it.slotMask & bit) || !InTab(it, vacGroup)) continue;
            item_grid::Tile t;
            t.id = it.id;
            t.name = &it.name;
            t.icon = it.icon;
            t.icon2 = it.icon2;
            t.accent = KindAccent(it.kind);
            t.mod = it.mod;
            if (const auto pk = g_vacPick.find(it.id); pk != g_vacPick.end()) {
                t.selected = true;
                t.amount = pk->second;
            }
            t.related = Related(hoverRel, it.id);
            tiles.push_back(t);
            src.push_back(&it);
        }
    }
    // Selected items still in the list (a slot change can hide some).
    int picked = 0;
    for (auto it = g_vacPick.begin(); it != g_vacPick.end();)
        if (!FindItem(s, it->first)) it = g_vacPick.erase(it);
        else ++picked, ++it;
    item_grid::Options o = GridOptions(SbxGridHeight(212, 1));
    o.selected = sl.item;
    o.drag = true;
    o.tabs = &tabs;
    o.empty = s.catalog ? "No matching items." : "Reading the item list...";
    const std::string put = picked > 0 ? std::format("Put {} in slots", picked) : std::string("Put in slots");
    o.button = put.c_str();
    o.buttonEnabled = picked > 0;
    o.dropType = kSlotDrag;  // a slot dragged back onto the list is emptied
    const std::string dropHint = draggedSlot >= 0 ? std::format("Drop to empty slot {}", draggedSlot + 1) : std::string();
    o.dropHint = dropHint.c_str();
    g_vacPickerTop = ImGui::GetCursorScreenPos().y;  // the hint bar: slots above, the item grid below
    const item_grid::Result r = item_grid::Draw("vac", tiles, g_vacGrid, o);
    lastHover = r.hovered >= 0 ? tiles[static_cast<size_t>(r.hovered)].id : 0;
    TabTip(r, tabs);
    if (r.buttonHovered)
        Tooltip("Puts the selected items in your vacpack, starting with the selected slot. What doesn't fit drops "
                "in front of you.");
    if (r.hovered >= 0 && !ImGui::GetDragDropPayload() && !ImGui::IsPopupOpen(kSbxCtx)) {
        const sbx::Item& it = *src[static_cast<size_t>(r.hovered)];
        ItemTooltip(s, it.name, it.kind, it.id, nullptr, nullptr, {{"LMB", "put in a slot"}, {"RMB", "context menu"}}, it.mod);
    }
    if (r.dropped && r.dropped->DataSize == sizeof(int)) {
        const int from = *static_cast<const int*>(r.dropped->Data);
        if (from >= 0 && from < n && s.slots[static_cast<size_t>(from)].item) {
            SetSlotPending(from, 0, 0);
            SbxRequest(sbx::Op::SetSlotCount, 0, from);
        }
    }
    if (r.wheelStep != 0 && r.hovered >= 0) {
        // Shift / Ctrl + wheel on an item: its amount (adds it to the selection; 0 removes it). Up to
        // 999: more than the slots hold is dropped in front of you by "Put in slots".
        const sbx::Item& it = *src[static_cast<size_t>(r.hovered)];
        auto pk = g_vacPick.find(it.id);
        const int cur = pk != g_vacPick.end() ? pk->second : 0;
        const int next = std::clamp(cur + r.wheelStep, 0, 999);
        if (next <= 0) g_vacPick.erase(it.id);
        else g_vacPick[it.id] = next;
        g_vacFocus = it.id;
    }
    if (r.clicked >= 0) {
        const sbx::Item& it = *src[static_cast<size_t>(r.clicked)];
        g_vacFocus = it.id;
        if (io.KeyCtrl) {
            if (!g_vacPick.erase(it.id)) g_vacPick[it.id] = 0;
        } else {
            const auto pk = g_vacPick.find(it.id);
            ClickPut(s, it, pk != g_vacPick.end() ? pk->second : 0);
        }
    }
    if (r.clearSelection) g_vacPick.clear();
    if (r.rightClicked >= 0) OpenItemContext(tiles[static_cast<size_t>(r.rightClicked)].id, SbxGrid::Vacpack);
    if (r.button) {
        // One StowItem per selected item, in the module's order: the first may replace what the
        // selected slot holds, the others go into the slots that hold them or are empty.
        SR2_INFO("[sandbox] tab: Put in slots, {} item(s), selected slot {}", g_vacPick.size(), g_sbxSlot + 1);
        std::vector<sbx::Action> acts;
        for (const auto& [id, amount] : g_vacPick) {
            sbx::Action a;
            a.op = sbx::Op::StowItem;
            a.slot = g_sbxSlot;
            a.item = id;
            a.value = amount;
            a.replace = acts.empty();
            acts.push_back(a);
        }
        SbxBatch(acts);
        g_vacPick.clear();
    }
    ItemContextMenu(s);
    EndCard();
}

// Spawner: places any actor the game can spawn (toys, slimes, food, ...) in front of you. The same
// controls as the vacpack picker: click spawns (the tile's amount, else 1); Ctrl+click selects;
// Shift+wheel / Ctrl+wheel sets a tile's amount (+-1 / +-10, up to kSpawnMax, shown on the tile; it
// joins the selection, 0 removes it); "Spawn N" spawns every selected item with its amount;
// right-click opens the menu, right-click between tiles clears the selection.
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
    static std::vector<int> groups, shownGroups;  // sbx::Group per tab (this frame, last frame)
    const auto& tabs = SpawnTabs(s, groups);
    item_grid::PredictTab(g_spawnGrid);  // last frame's tab indices: before the remap below
    if (groups != shownGroups) {  // the tab list changed: stay on the same group
        const int was = g_spawnGrid.tab >= 0 && static_cast<size_t>(g_spawnGrid.tab) < shownGroups.size()
                            ? shownGroups[static_cast<size_t>(g_spawnGrid.tab)]
                            : sbx::kGroupAll;
        const auto at = std::find(groups.begin(), groups.end(), was);
        g_spawnGrid.tab = at != groups.end() ? static_cast<int>(at - groups.begin()) : 0;
        shownGroups = groups;
    }
    g_spawnGrid.tab = std::clamp(g_spawnGrid.tab, 0, static_cast<int>(groups.size()) - 1);
    const int group = groups[static_cast<size_t>(g_spawnGrid.tab)];
    static uintptr_t lastHover = 0;
    const sbx::Relation* hoverRel = lastHover ? FindRelation(s, lastHover) : nullptr;
    std::vector<const sbx::Item*> order;
    order.reserve(s.spawnables->size());
    for (const sbx::Item& it : *s.spawnables)
        if (InTab(it, group)) order.push_back(&it);
    // Largos tab: grouped by base slime (a section per slime, each largo under the alphabetically
    // first of its two), then by the other one.
    // Crates tab: a section per area ("Bluffs Gold Crate" -> "Bluffs"), sorted by area, then name.
    static std::vector<std::string> families;
    families.clear();
    const bool byFamily = group == sbx::kGroupLargos, byArea = group == sbx::kGroupContainers;
    std::map<const sbx::Item*, std::string> areas;
    if (byFamily) {
        std::stable_sort(order.begin(), order.end(), [](const sbx::Item* a, const sbx::Item* b) {
            return a->family != b->family ? a->family < b->family : a->family2 < b->family2;
        });
        for (const sbx::Item* it : order)
            if (families.empty() || families.back() != it->family) families.push_back(it->family);
    } else if (byArea) {
        for (const sbx::Item* it : order) areas[it] = CrateArea(*it);
        std::stable_sort(order.begin(), order.end(), [&](const sbx::Item* a, const sbx::Item* b) {
            const std::string &x = areas[a], &y = areas[b];
            return x != y ? x < y : a->name < b->name;
        });
        for (const sbx::Item* it : order)
            if (families.empty() || families.back() != areas[it]) families.push_back(areas[it]);
    }
    std::vector<item_grid::Tile> tiles;
    tiles.reserve(order.size());
    std::vector<const sbx::Item*> src;
    for (const sbx::Item* ip : order) {
        const sbx::Item& it = *ip;
        item_grid::Tile t;
        t.id = it.id;
        t.name = &it.name;
        t.icon = it.icon;
        t.icon2 = it.icon2;
        t.accent = KindAccent(it.kind);
        t.dimmed = it.tarr && s.tarrsOff;
        t.related = Related(hoverRel, it.id);
        t.mod = it.mod;
        if (const auto pk = g_spawnPick.find(it.id); pk != g_spawnPick.end()) {
            t.selected = true;
            t.amount = pk->second;
        }
        if (byFamily || byArea) {
            const std::string& key = byFamily ? it.family : areas[ip];
            t.section = static_cast<int>(std::find(families.begin(), families.end(), key) - families.begin());
        }
        tiles.push_back(t);
        src.push_back(&it);
    }
    // Selected items still in the spawn list.
    int picked = 0;
    for (auto it = g_spawnPick.begin(); it != g_spawnPick.end();)
        if (!FindItem(s, it->first)) it = g_spawnPick.erase(it);
        else ++picked, ++it;
    item_grid::Options o = GridOptions(SbxGridHeight(250, 2));
    o.tabs = &tabs;
    o.tabsBelow = true;
    if (byFamily || byArea) o.sections = &families;
    const std::string spawnLabel = picked > 0 ? std::format("Spawn {}", picked) : std::string("Spawn");
    o.button = spawnLabel.c_str();
    o.buttonEnabled = picked > 0;
    const item_grid::Result r = item_grid::Draw("spawn", tiles, g_spawnGrid, o);
    lastHover = r.hovered >= 0 ? tiles[static_cast<size_t>(r.hovered)].id : 0;
    TabTip(r, tabs);
    if (r.buttonHovered)
        Tooltip("Spawns the selected items in front of you, each with its amount.");
    if (r.hovered >= 0 && !ImGui::IsPopupOpen(kSbxCtx)) {
        const sbx::Item& it = *src[static_cast<size_t>(r.hovered)];
        const char* info = it.tarr && s.tarrsOff ? "Tarrs are disabled in this save's game settings."
                           : it.mod ? "Spawned through the mod (kept in the mod's file): at most 5 at a time."
                                    : nullptr;
        ItemTooltip(s, it.name, it.kind, it.id, info, nullptr, {{"LMB", "spawn"}, {"RMB", "context menu"}}, it.mod);
    }
    if (r.wheelStep != 0 && r.hovered >= 0) {
        const sbx::Item& it = *src[static_cast<size_t>(r.hovered)];
        const auto pk = g_spawnPick.find(it.id);
        const int next = std::clamp((pk != g_spawnPick.end() ? pk->second : 0) + r.wheelStep, 0, kSpawnMax);
        if (next <= 0) g_spawnPick.erase(it.id);
        else g_spawnPick[it.id] = next;
    }
    if (r.clicked >= 0) {
        const sbx::Item& it = *src[static_cast<size_t>(r.clicked)];
        if (ImGui::GetIO().KeyCtrl) {
            if (!g_spawnPick.erase(it.id)) g_spawnPick[it.id] = 1;
        } else if (it.tarr && s.tarrsOff) {
            sbx::SetStatus(it.name + " not spawned: Tarrs are disabled in this save's game settings", true);
        } else {
            const auto pk = g_spawnPick.find(it.id);
            SbxRequest(sbx::Op::Spawn, pk != g_spawnPick.end() ? std::max(1, pk->second) : 1, -1, it.id);
        }
    }
    if (r.clearSelection) g_spawnPick.clear();
    if (r.button) {
        // One request per selected item, as one batch: one HUD notice, one grid in front of you.
        std::vector<sbx::Action> acts;
        int skipped = 0;
        for (const auto& [id, amount] : g_spawnPick) {
            const sbx::Item* it = FindItem(s, id);
            if (!it || (it->tarr && s.tarrsOff)) {
                ++skipped;
                continue;
            }
            sbx::Action a;
            a.op = sbx::Op::Spawn;
            a.item = id;
            a.value = std::clamp(amount, 1, kSpawnMax);
            acts.push_back(a);
        }
        SR2_INFO("[sandbox] tab: Spawn, {} item(s){}", acts.size(),
                 skipped ? std::format(", {} skipped (Tarrs off)", skipped) : std::string());
        if (skipped && acts.empty()) sbx::SetStatus("Not spawned: Tarrs are disabled in this save's game settings", true);
        SbxBatch(acts);
        g_spawnPick.clear();
    }
    if (r.rightClicked >= 0) OpenItemContext(tiles[static_cast<size_t>(r.rightClicked)].id, SbxGrid::Spawner);
    ItemContextMenu(s);
    EndCard();
}

// Crafter: the fabricator's gadget recipes and the crafting materials the refinery stores, with the
// game Fabricator's tabs after an All tab: All, Pinned (the recipe pinned there), its categories
// (Utilities, Warp Tech, Decorations), then Crafting Resources. Each category lists its gadgets in the
// Fabricator's own order (sbx::Recipe::order); a locked blueprint is dimmed in place. All, and any tab
// while the search box has text, shows every tab's items, a section per tab. Click selects
// (Shift+wheel: amount), Craft adds everything selected at no cost: gadgets to the gadget inventory,
// materials to the refinery.
// Last, while any recipe or material is the mod's own (Recipe::mod), a Mod tab with just those (round 8).
constexpr int kCraftMod = -5, kCraftEverything = -4, kCraftPinned = -3, kCraftMaterials = -2, kCraftAll = -1;

// Right-click menu of a crafter recipe: the gadget itself, then its variants (the game's, then the mod's
// buildables on it, modules/sandbox: Recipe::variants), each crafted like a tile (Op::Craft; the amount
// picked on the recipe, else 1). A locked blueprint is crafted anyway (dimmed, with a tooltip); a mod
// buildable is given the mod's way (its base gadget plus a kit in the mod's file).
void CraftVariantMenu(const sbx::Snapshot& s) {
    const Palette& P = theme::Colors();
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(U(8), U(8)));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(U(6), U(4)));
    PushFloatingStyle();
    if (ImGui::BeginPopup(kCraftCtx)) {
        FloatingSurface(true);
        const sbx::Recipe* rc = nullptr;
        if (s.recipes)
            for (const sbx::Recipe& x : *s.recipes)
                if (x.id == g_craftCtx) rc = &x;
        if (!rc) {
            ImGui::CloseCurrentPopup();
        } else {
            const auto pk = g_craftPick.find(rc->id);
            const int amount = pk != g_craftPick.end() ? std::max(1, pk->second) : 1;
            {
                FontScope f(theme::Body(), theme::kSmallSize);
                ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(P.soft), "%s", rc->name.c_str());
            }
            const float e = U(22), rowW = U(250), fs = theme::kSmallSize + 0.5f;
            int row = 0;
            auto entry = [&](uintptr_t id, const std::string& name, uint32_t icon, bool unlocked, bool mod) {
                ImGui::PushID(++row);
                const ImVec2 p = ImGui::GetCursorScreenPos();
                const bool hit = ImGui::Selectable("##variant", false, 0, ImVec2(rowW, e));
                ImDrawList* dl = ImGui::GetWindowDrawList();
                item_grid::DrawIcons(dl, icon, 0, p, ImVec2(p.x + e, p.y + e), unlocked ? IM_COL32_WHITE : IM_COL32(255, 255, 255, 120));
                const std::string label = amount > 1 ? std::format("Craft {}: {}", amount, name) : "Craft: " + name;
                const ImVec2 ts = TextSize(theme::Body(), fs, label.c_str());
                Text(dl, theme::Body(), fs, ImVec2(p.x + e + U(8), p.y + (e - ts.y) * 0.5f), unlocked ? P.text : P.dim,
                     label.c_str());
                if (ImGui::IsItemHovered() && (mod || !unlocked))
                    Tooltip(mod ? (std::string("Mod buildable: you get a ") + rc->name +
                                   " and a kit to place it as this, kept in the mod's file, not in the save.")
                                      .c_str()
                                : "Blueprint not unlocked: crafting puts it in your inventory anyway.");
                if (hit) SbxRequest(sbx::Op::Craft, amount, -1, id);
                ImGui::PopID();
            };
            entry(rc->id, rc->name, rc->icon, rc->unlocked, false);
            if (rc->variants.empty()) DimText("No variants.");
            for (const sbx::Variant& v : rc->variants) entry(v.id, v.name, v.icon, v.unlocked, v.mod);
        }
        if (PopupCancelled()) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    PopFloatingStyle();
    ImGui::PopStyleVar(2);
}

// Tab order of a fabricator category: Utilities, Warp Tech, Decorations, then the rest (by index).
int CraftCatRank(const std::string& title, int index) {
    std::string low;
    for (char c : title) low += static_cast<char>((c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c);
    if (low.find("util") != std::string::npos) return 0;
    if (low.find("warp") != std::string::npos) return 1;
    if (low.find("decor") != std::string::npos) return 2;
    return 3 + index;
}

void SandboxCrafter(const sbx::Snapshot& s, float w) {
    const Palette& P = theme::Colors();
    BeginCard("sbx_craft", w);
    CardHeading("Refinery Crafter", P.head);
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
    // Tabs: All, Pinned, each fabricator category that has recipes (its icon; without any categories one
    // "Gadgets" tab), Crafting Resources.
    static std::vector<item_grid::Tab> tabs;
    static std::vector<int> tabCat;  // category per tab; kCraftEverything, kCraftPinned, kCraftMaterials, kCraftAll (no categories)
    static std::vector<std::string> tabTitle;
    tabs.clear();
    tabCat.clear();
    tabTitle.clear();
    tabs.push_back({0, 0, 0, "All"});
    tabCat.push_back(kCraftEverything);
    tabTitle.push_back("All");
    // The Fabricator's Pinned category icon once its asset was read (modules/sandbox), else a drawn pin.
    tabs.push_back({s.pinnedIcon, item_grid::kGlyphPin, 0, "Pinned (the recipe pinned in the Fabricator)"});
    tabCat.push_back(kCraftPinned);
    tabTitle.push_back("Pinned");
    std::vector<int> cats;
    if (s.recipeCategories) {
        // Categories: the fabricator's own, else its gadget groups (modules/sandbox/NOTES.md, Crafter).
        std::vector<bool> used(s.recipeCategories->size(), false);
        for (const sbx::Recipe& rc : *s.recipes)
            if (rc.category >= 0 && static_cast<size_t>(rc.category) < used.size()) used[static_cast<size_t>(rc.category)] = true;
        for (size_t i = 0; i < used.size(); ++i)
            if (used[i]) cats.push_back(static_cast<int>(i));
        std::stable_sort(cats.begin(), cats.end(), [&](int x, int y) {
            return CraftCatRank((*s.recipeCategories)[static_cast<size_t>(x)], x) <
                   CraftCatRank((*s.recipeCategories)[static_cast<size_t>(y)], y);
        });
    }
    for (int ci : cats) {
        const size_t i = static_cast<size_t>(ci);
        const std::string& title = (*s.recipeCategories)[i];
        uint32_t icon = s.recipeCategoryIcons && i < s.recipeCategoryIcons->size() ? (*s.recipeCategoryIcons)[i] : 0;
        // The fabricator's category sprite is only there once its UI was loaded: else the icon of
        // the category's first gadget.
        for (size_t k = 0; k < s.recipes->size() && !icon; ++k)
            if ((*s.recipes)[k].category == ci) icon = (*s.recipes)[k].icon;
        const unsigned char first = title.empty() ? '?' : static_cast<unsigned char>(title[0]);
        tabs.push_back({icon, first < 0x80 ? first : '?', KindAccent(sbx::Kind::Gadget), title});
        tabCat.push_back(ci);
        tabTitle.push_back(title);
    }
    if (cats.empty()) {
        tabs.push_back({0, 0, 0, "Gadgets (the fabricator's categories were not found)"});
        tabCat.push_back(kCraftAll);
        tabTitle.push_back("Gadgets");
    }
    if (s.materials && !s.materials->empty()) {
        tabs.push_back({s.groupIcons[sbx::kGroupResources], 'R', KindAccent(sbx::Kind::Resource), "Crafting Resources (refinery)"});
        tabCat.push_back(kCraftMaterials);
        tabTitle.push_back("Crafting Resources");
    }
    bool anyMod = false;
    for (const auto* list : {s.recipes.get(), s.materials.get()})
        if (list)
            for (size_t i = 0; i < list->size() && !anyMod; ++i) anyMod = (*list)[i].mod;
    if (anyMod) {
        tabs.push_back(ModTab("Mod (the mod's own buildables)"));
        tabCat.push_back(kCraftMod);
        tabTitle.push_back("Mod");
    }
    // A tab pressed this frame (last frame's indices, so before the remap below).
    item_grid::PredictTab(g_craftGrid);
    // The tab list changed (categories or materials arrived): stay on the same category. The first
    // time: All.
    static std::vector<int> shownCat;
    if (tabCat != shownCat) {
        const int was = g_craftGrid.tab >= 0 && static_cast<size_t>(g_craftGrid.tab) < shownCat.size()
                            ? shownCat[static_cast<size_t>(g_craftGrid.tab)]
                            : kCraftEverything;
        const auto at = std::find(tabCat.begin(), tabCat.end(), was);
        g_craftGrid.tab = at != tabCat.end() ? static_cast<int>(at - tabCat.begin()) : 0;
        shownCat = tabCat;
    }
    g_craftGrid.tab = std::clamp(g_craftGrid.tab, 0, static_cast<int>(tabs.size()) - 1);
    const int cat = tabCat[static_cast<size_t>(g_craftGrid.tab)];
    const bool searching = g_craftGrid.search[0] != 0;

    std::vector<item_grid::Tile> tiles;
    std::vector<const sbx::Recipe*> src;
    std::vector<bool> isMat;
    auto add = [&](const sbx::Recipe& rc, int section, bool material) {
        item_grid::Tile t;
        t.id = rc.id;
        t.name = &rc.name;
        t.icon = rc.icon;
        t.accent = KindAccent(material ? rc.kind : sbx::Kind::Gadget);
        t.dimmed = !rc.unlocked;
        t.badge = rc.owned;
        t.section = section;
        t.mod = rc.mod;
        if (const auto pk = g_craftPick.find(rc.id); pk != g_craftPick.end()) {
            t.selected = true;
            t.amount = pk->second;
        }
        tiles.push_back(t);
        src.push_back(&rc);
        isMat.push_back(material);
    };
    // One tab's items: a category's gadgets in the Fabricator's order (unknown positions last, by name).
    auto addTab = [&](size_t ti, int section) {
        const int c = tabCat[ti];
        if (c == kCraftPinned) {
            for (const sbx::Recipe& rc : *s.recipes)
                if (s.pinnedRecipe && rc.id == s.pinnedRecipe) add(rc, section, false);
        } else if (c == kCraftMaterials) {
            if (s.materials)
                for (const sbx::Recipe& rc : *s.materials) add(rc, section, true);
        } else if (c == kCraftMod) {
            // The mod's own, in the order the category tabs list them (categories, then materials).
            std::vector<const sbx::Recipe*> v;
            for (const sbx::Recipe& rc : *s.recipes)
                if (rc.mod) v.push_back(&rc);
            std::stable_sort(v.begin(), v.end(), [&](const sbx::Recipe* x, const sbx::Recipe* y) {
                const auto rank = [&](const sbx::Recipe* r) {
                    const auto at = std::find(tabCat.begin(), tabCat.end(), r->category);
                    return at != tabCat.end() ? static_cast<int>(at - tabCat.begin()) : INT_MAX;
                };
                const int a = rank(x), b = rank(y);
                if (a != b) return a < b;
                const int oa = x->order < 0 ? INT_MAX : x->order, ob = y->order < 0 ? INT_MAX : y->order;
                return oa != ob ? oa < ob : x->modRank < y->modRank;
            });
            for (const sbx::Recipe* rc : v) add(*rc, section, false);
            if (s.materials)
                for (const sbx::Recipe& rc : *s.materials)
                    if (rc.mod) add(rc, section, true);
        } else {
            std::vector<const sbx::Recipe*> v;
            for (const sbx::Recipe& rc : *s.recipes)
                if (c == kCraftAll || rc.category == c) v.push_back(&rc);
            // The mod's buildables share their base gadget's position and follow it (modRank 1..; the game's are 0).
            std::stable_sort(v.begin(), v.end(), [](const sbx::Recipe* x, const sbx::Recipe* y) {
                const int a = x->order < 0 ? INT_MAX : x->order, b = y->order < 0 ? INT_MAX : y->order;
                return a != b ? a < b : x->modRank < y->modRank;
            });
            for (const sbx::Recipe* rc : v) add(*rc, section, false);
        }
    };
    const bool everything = searching || cat == kCraftEverything;
    if (everything) {
        for (size_t ti = 0; ti < tabCat.size(); ++ti)  // the pinned one and the mod's are in their categories
            if (tabCat[ti] != kCraftEverything && tabCat[ti] != kCraftPinned && tabCat[ti] != kCraftMod)
                addTab(ti, static_cast<int>(ti));
    } else {
        addTab(static_cast<size_t>(g_craftGrid.tab), 0);
    }
    static std::vector<std::string> sections;
    sections = tabTitle;
    // Selection entries that are gone from both lists are dropped.
    int picked = 0;
    for (auto it = g_craftPick.begin(); it != g_craftPick.end();) {
        bool found = false;
        for (const auto* list : {s.recipes.get(), s.materials.get()})
            if (list)
                for (const sbx::Recipe& rc : *list) found = found || rc.id == it->first;
        if (!found) it = g_craftPick.erase(it);
        else ++picked, ++it;
    }
    item_grid::Options o = GridOptions(SbxGridHeight(250, 2));
    o.hint = "Search gadgets...";
    o.tabs = &tabs;
    o.tabsBelow = true;
    if (everything) o.sections = &sections;
    if (!searching && cat == kCraftPinned && !s.pinnedRecipe)
        o.empty = "Nothing is pinned. Pin a recipe in the game's Fabricator.";
    const std::string craft = picked > 0 ? std::format("Craft {}", picked) : std::string("Craft");
    o.button = craft.c_str();
    o.buttonEnabled = picked > 0;
    const item_grid::Result r = item_grid::Draw("craft", tiles, g_craftGrid, o);
    TabTip(r, tabs);
    if (r.buttonHovered)
        Tooltip("Crafts the selected items for free: gadgets into your inventory, materials into the refinery.");
    if (r.hovered >= 0) {
        // Name, group, the fabricator cost one ingredient per line with its icon, and the counts.
        const sbx::Recipe& rc = *src[static_cast<size_t>(r.hovered)];
        const bool mat = isMat[static_cast<size_t>(r.hovered)];
        std::string info;
        if (mat) info = std::format("In the refinery: {}.", rc.owned);
        else if (rc.mod) {
            // The mod's own buildable (custom_upgrades): given the mod's way, never as itself into the save.
            info = rc.kind == sbx::Kind::Toy  // the Slime Radio
                       ? std::format("Spawned in front of you (at most 5 at a time). Radios in this save: {}.", rc.owned)
                       : std::format("You get its base gadget and a kit to place it as this one (kept in the mod's file, "
                                       "not in the save). Kits: {}.",
                                       rc.owned);
            if (!rc.unlocked) info += " Fabricator card locked: crafting gives it anyway.";
        } else {
            if (rc.costs.empty() && !rc.cost.empty()) info = "Fabricator cost: " + rc.cost + ". ";
            info += std::format("In your inventory: {}{}.", rc.owned, rc.perCraft > 1 ? " (made in pairs)" : "");
            if (!rc.unlocked) info += " Blueprint not unlocked: crafting puts it in your inventory anyway.";
        }
        if (!mat && !rc.variants.empty()) info += std::format(" {} variant{}: right-click.", rc.variants.size(),
                                                              rc.variants.size() == 1 ? "" : "s");
        std::vector<KeyRow> keys{{"LMB", g_craftPick.count(rc.id) ? "deselect" : "select"}, {"Shift+Wheel", "amount"}};
        if (!mat) keys.push_back({"RMB", rc.variants.empty() ? "context menu" : "variants"});
        if (!ImGui::IsPopupOpen(kCraftCtx))
            ItemTooltip(s, rc.name, mat || rc.mod ? rc.kind : sbx::Kind::Gadget, mat ? rc.id : 0, info.c_str(), mat ? nullptr : &rc.costs,
                        keys, rc.mod);
    }
    if (r.wheelStep != 0 && r.hovered >= 0) {
        const sbx::Recipe& rc = *src[static_cast<size_t>(r.hovered)];
        const int cap = isMat[static_cast<size_t>(r.hovered)] ? 999 : 99;
        auto pk = g_craftPick.find(rc.id);
        const int cur = pk != g_craftPick.end() ? pk->second : 0;
        const int next = std::clamp(cur + r.wheelStep, 0, cap);
        if (next <= 0) g_craftPick.erase(rc.id);
        else g_craftPick[rc.id] = next;
    }
    if (r.clicked >= 0) {
        const sbx::Recipe& rc = *src[static_cast<size_t>(r.clicked)];
        if (!g_craftPick.erase(rc.id)) g_craftPick[rc.id] = 1;
    }
    if (r.clearSelection) g_craftPick.clear();
    if (r.rightClicked >= 0 && !isMat[static_cast<size_t>(r.rightClicked)]) {
        g_craftCtx = src[static_cast<size_t>(r.rightClicked)]->id;
        ImGui::OpenPopup(kCraftCtx);
    }
    CraftVariantMenu(s);
    if (r.button) {
        // One request per selected item, as one batch: the module shows one HUD notice for all of them.
        std::vector<sbx::Action> acts;
        for (const auto& [id, amount] : g_craftPick) {
            bool material = false;
            if (s.materials)
                for (const sbx::Recipe& rc : *s.materials) material = material || rc.id == id;
            sbx::Action a;
            a.op = material ? sbx::Op::AddMaterial : sbx::Op::Craft;
            a.value = std::max(1, amount);
            a.item = id;
            acts.push_back(a);
        }
        SbxBatch(acts);
        g_craftPick.clear();
    }
    EndCard();
}

// The module's last status line, only while it is a fresh error (8 s): what worked is in the log and
// the HUD notices, not on the page ("Fabricator: added 1 TankBoosterComponent for AmmoCapacity" stayed
// under the grids).
void SandboxStatus(const Host& h) {
    const sbx::Status st = sbx::GetStatus();
    if (st.text.empty() || !st.error || h.nowMs - st.tick >= 8000) return;
    const Palette& P = theme::Colors();
    ImGui::Dummy(ImVec2(0, U(2)));
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(P.err), "%s", st.text.c_str());
    ImGui::PopTextWrapPos();
}

// Player-card extras (modules/sandbox/player_extras.cpp, sandbox_extras_bridge.h): infinite health, teleport to
// the map marker and unlocking the ranch expansions (after a confirmation, it is a permanent save change).
namespace sbxx = ::sr2::ui::sandbox::extras;
constexpr const char* kSbxExpPopup = "##sandbox_expansions";
bool g_sbxExpConfirm = false;

// A Sandbox confirmation modal (Cancel / `ok`, Esc cancels; Cancel first, so focus never starts on
// `ok`). `open` opens it once. True on the frame `ok` is clicked.
bool SandboxConfirmModal(const char* id, bool& open, const char* title, const std::string& text, const char* dim,
                         const char* ok) {
    const Palette& P = theme::Colors();
    if (open) {
        ImGui::OpenPopup(id);
        open = false;
    }
    bool confirmed = false;
    const float w = U(430);
    ImGui::SetNextWindowPos(ImVec2((g_winMin.x + g_winMax.x) * 0.5f, (g_winMin.y + g_winMax.y) * 0.5f), ImGuiCond_Appearing,
                            ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(w, 0), ImVec2(w, FLT_MAX));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(U(22), U(18)));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(U(10), U(8)));
    ImGui::PushStyleColor(ImGuiCol_Border, P.cardBorder);
    const ImGuiWindowFlags f = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                               ImGuiWindowFlags_AlwaysAutoResize;
    if (ImGui::BeginPopupModal(id, nullptr, f)) {
        {
            FontScope fs(theme::Display(), theme::kDisplaySize + 4);
            ImGui::PushStyleColor(ImGuiCol_Text, theme::Current() == theme::Mode::Dark ? P.grab : P.accent);
            ImGui::TextUnformatted(title);
            ImGui::PopStyleColor();
        }
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted(text.c_str());
        ImGui::PopTextWrapPos();
        DimText(dim);
        ImGui::Dummy(ImVec2(0, U(4)));
        const ImVec2 bs(U(112), U(34));
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - 2 * bs.x -
                             ImGui::GetStyle().ItemSpacing.x);
        if (ImGui::Button("Cancel", bs) || PopupCancelled()) ImGui::CloseCurrentPopup();
        ImGui::SameLine();
        if (AccentButton(ok, bs)) {
            confirmed = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
    return confirmed;
}

void SandboxExpansionsConfirm(const sbxx::State& st) {
    if (SandboxConfirmModal(kSbxExpPopup, g_sbxExpConfirm, "Unlock ranch expansions?",
                            std::format("Opens the {} locked expansion(s) of your ranch as if you had bought them, "
                                        "for free.",
                                        std::max(0, st.lockedExpansions)),
                            "This changes your save for good: the game keeps the expansions open when it next saves, "
                            "and there is no way to lock them again.",
                            "Unlock"))
        sbxx::Request(sbxx::Op::UnlockExpansions);
}

void SandboxPlayer(const Host& h, const sbx::Snapshot& s, float w) {
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
        "Set health to its maximum.");
    row("Energy", std::format("{} / {}", s.energy, s.maxEnergy), "Refill", sbx::Op::RefillEnergy,
        "Set energy to its maximum.");
    row("Radiation", std::to_string(s.rads), "Clear", sbx::Op::ClearRads, "Set radiation to 0.");
    const float x0 = SbxRowStart("Infinite energy");
    ImGui::SameLine(x0 + avail - U(kSwitchW));
    if (Switch("##infenergy", s.infiniteEnergy)) SbxRequest(sbx::Op::InfiniteEnergy, s.infiniteEnergy ? 0 : 1);
    HoverTip("Keeps energy at its maximum. Kept per save.");
    // Infinite health and the two buttons come from the extras bridge (player_extras.cpp). Discover all / Max all
    // are per tab on the Upgrades & Blueprints card (sandbox_upgrades.cpp).
    const auto st = sbxx::Get();
    if (!st || h.nowMs - st->tick > 10000) {
        DimText("Reading...");
        EndCard();
        return;
    }
    const float x1 = SbxRowStart("Infinite health");
    ImGui::SameLine(x1 + avail - U(kSwitchW));
    if (Switch("##infhealth", st->infiniteHealth)) sbxx::Request(sbxx::Op::InfiniteHealth, st->infiniteHealth ? 0 : 1);
    HoverTip(st->healthHook ? "Ignores all damage. Kept per save."
                            : "Keeps health at its maximum. A single hit bigger than your maximum still knocks you "
                              "out. Kept per save.");
    // Side by side when both labels fit in half the card, else one per row.
    const char* kTeleport = "Teleport to waypoint";
    const char* kUnlock = "Unlock ranch expansions";
    const float sp = ImGui::GetStyle().ItemSpacing.x;
    const float half = std::floor((avail - sp) / 2);
    const float need = std::max(ImGui::CalcTextSize(kTeleport).x, ImGui::CalcTextSize(kUnlock).x) +
                       2 * ImGui::GetStyle().FramePadding.x;
    const bool split = half >= need;
    const ImVec2 bs(split ? half : avail, U(32));
    ImGui::BeginDisabled(!st->canTeleport);
    if (ImGui::Button(kTeleport, bs)) sbxx::Request(sbxx::Op::TeleportToMarker);
    ImGui::EndDisabled();
    HoverTip(st->canTeleport ? "Moves you to the marker you placed on the map."
                             : st->teleportWhy);
    if (split) ImGui::SameLine();
    const bool canUnlock = st->lockedExpansions > 0;
    ImGui::BeginDisabled(!canUnlock);
    if (ImGui::Button(kUnlock, bs)) g_sbxExpConfirm = true;
    ImGui::EndDisabled();
    HoverTip(canUnlock ? std::format("Unlocks the {} locked ranch expansion(s) for free. Asks first.",
                                     st->lockedExpansions)
                       : st->expansionWhy);
    EndCard();
    SandboxExpansionsConfirm(*st);
}

// Noclip (modules/sandbox/noclip.cpp): its switch (state from the extras bridge), then its settings as normal
// rows (the key binds use the shared key capture). The settings are the module's own Defs, found once.
void SandboxNoclip(const Host& h, float w) {
    const Palette& P = theme::Colors();
    static std::vector<settings::Def> defs;
    if (defs.empty())
        for (const settings::Def& d : settings::All())
            if (d.module == sbx::kModule && d.group == "Noclip") defs.push_back(d);
    BeginCard("sbx_noclip", w);
    CardHeading("Noclip", P.head);
    const auto st = sbxx::Get();
    const bool on = st && st->noclip;
    const std::string why = !st || h.nowMs - st->tick > 10000 ? std::string("Reading...") : st->noclipWhy;
    const float avail = ImGui::GetContentRegionAvail().x;
    const float x0 = SbxRowStart("Fly through walls");
    ImGui::SameLine(x0 + avail - U(kSwitchW));
    ImGui::BeginDisabled(!on && !why.empty());
    if (Switch("##noclip", on)) sbxx::Request(sbxx::Op::Noclip, on ? 0 : 1);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        Tooltip(!on && !why.empty() ? why
                                    : "No collision and no gravity: your movement keys fly you where you look. Off gives "
                                      "collision and gravity back (also when Sandbox goes off or the scene changes).");
    for (const settings::Def& d : defs) DrawRow(h, d, d.label, false);
    DimText("Move keys fly where you look, Fly up / Fly down straight up and down. Not while a menu or this overlay is "
            "open. Space also jumps, so with the jetpack it may run while you rise.");
    EndCard();
}

// The Sandbox category (module "sandbox"): one tab per page, each with its own scroll position. The
// off card, the "Enable sandbox?" dialog and the snapshot checks are the same on every page.
//   General: the achievements banner, Newbucks and Noclip | Player.
//   Vacpack: the slots and the item picker, across both columns.
//   Spawn & Craft: Spawner | Refinery Crafter.
//   Upgrades: the Upgrades & Blueprints card (sandbox_upgrades.cpp).
//   Weather: the Weather card (sandbox_weather.cpp).
// The key hints for the grid under the mouse are a bar pinned under the page (SandboxHintBar, drawn by
// Draw after the page's body), on the pages with grids.
void DrawSandboxPage(const Host& h, const ModuleView& mv, layout::SandboxPage page) {
    using layout::SandboxPage;
    const Palette& P = theme::Colors();
    FailedBanner(mv);
    SandboxConfirm(mv);
    g_sbxRoom = 0;
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
        DimText("Turning it on marks the loaded save as cheated: Steam achievements stay disabled in that save, "
                "also with Sandbox off. Each save keeps its own Sandbox switch.");
        if (sbx::SaveCheated()) {
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(P.warn),
                               "Steam achievements are disabled for this save (Sandbox was used).");
            ImGui::PopTextWrapPos();
        }
        ImGui::Dummy(ImVec2(0, U(4)));
        if (AccentButton("Enable sandbox", ImVec2(U(170), U(34)))) g_sbxConfirm = true;
        EndCard();
        return;
    }
    // Across both columns, above everything else on the General page.
    if (page == SandboxPage::General)
        Banner(P.warn, sbx::SaveCheated() ? "Steam achievements are disabled for this save (Sandbox was used)."
                                          : "Steam achievements are paused while Sandbox is on.");
    sbx::MarkViewed();
    const auto snap = sbx::Get();
    // The module only refreshes the snapshot while the page is viewed, so after reopening it is as old
    // as the time the overlay was closed: draw it anyway for the first seconds (a fresh one follows
    // within 250 ms) instead of a short "Reading..." page, which clamped the scroll for a frame.
    const bool justShown = ImGui::GetTime() - g_sbxShownSince < 3.0;
    if (!snap || (h.nowMs - snap->tick > 10000 && !justShown)) {
        OffNote("Reading your character...");
        return;
    }
    if (!snap->inGame) {
        OffNote(snap->why.empty() ? std::string("Load a save to use the sandbox.") : snap->why);
        SandboxStatus(h);
        return;
    }
    const float full = ImGui::GetContentRegionAvail().x, w = ColumnWidth();
    // The grid under the mouse, for the key hint bar under the page (drawn after it, this frame). Kept
    // while a menu or drag from it is open, while the mouse is on the bar itself, and for a moment while
    // the mouse crosses the gaps between the cards, so the hints don't blink when moving between grids.
    const bool pageHovered =
        ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
    SbxHover hover = SbxHover::None;
    auto hoverCard = [&](SbxHover what) {
        if (pageHovered && ImGui::IsMouseHoveringRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), false)) hover = what;
    };
    // What is left of the page's visible height from here: the grids grow into it (SbxGridHeight).
    auto room = [] { g_sbxRoom = ImGui::GetWindowHeight() - ImGui::GetCursorPosY(); };
    switch (page) {
        case SandboxPage::General:
            // Left column: Newbucks, Noclip under it; right column: Player.
            ImGui::BeginGroup();
            SandboxNewbucks(*snap, w);
            ImGui::Dummy(ImVec2(0, U(kGap) - ImGui::GetStyle().ItemSpacing.y));
            SandboxNoclip(h, w);
            ImGui::EndGroup();
            ImGui::SameLine(0, U(kGap));
            SandboxPlayer(h, *snap, w);
            break;
        case SandboxPage::Vacpack:
            room();
            SandboxVacpack(*snap, full);
            hoverCard(ImGui::GetMousePos().y < g_vacPickerTop ? SbxHover::Slots : SbxHover::Picker);
            break;
        case SandboxPage::SpawnCraft:
            room();
            SandboxSpawner(*snap, w);
            hoverCard(SbxHover::Spawner);
            ImGui::SameLine(0, U(kGap));
            SandboxCrafter(*snap, w);
            hoverCard(SbxHover::Crafter);
            break;
        case SandboxPage::Upgrades:
            sandbox_upgrades::DrawCard(SbxHelpers(), *snap, full);  // Upgrades & Blueprints (sandbox_upgrades.cpp)
            hoverCard(sandbox_upgrades::ShowingUpgrades() ? SbxHover::Upgrades : SbxHover::Blueprints);
            break;
        case SandboxPage::Weather:
            sandbox_weather::DrawCard(SbxWeatherHelpers(), full);  // Weather (sandbox_weather.cpp)
            // Room for more world controls under the Weather card (time of day goes into the card itself,
            // sandbox_weather.cpp); a further card here: ImGui::Dummy(ImVec2(0, U(kGap) - ItemSpacing.y)), then it.
            break;
    }
    g_sbxRoom = 0;
    if (!ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId) && !ImGui::GetDragDropPayload() &&
        !ImGui::IsMouseHoveringRect(g_sbxBarMin, g_sbxBarMax, false)) {
        const double now = ImGui::GetTime();
        if (hover != SbxHover::None) {
            g_sbxHover = hover;
            g_sbxHoverNone = 0;
        } else if (!pageHovered) {
            g_sbxHover = SbxHover::None;
        } else {
            if (g_sbxHoverNone == 0) g_sbxHoverNone = now;
            if (now - g_sbxHoverNone > 0.35) g_sbxHover = SbxHover::None;  // parked off the grids (between cards)
        }
    }
    SandboxStatus(h);
}

// True while a Sandbox page with grids (Vacpack, Spawn & Craft, Upgrades) shows them: the menu
// reserves the key hint bar under the page then.
bool SandboxHintBarShown(const Tab& tab, const ModuleView& mv) {
    using layout::SandboxPage;
    if (tab.sandboxPage != SandboxPage::Vacpack && tab.sandboxPage != SandboxPage::SpawnCraft &&
        tab.sandboxPage != SandboxPage::Upgrades)
        return false;
    if (!ModuleOn(mv)) return false;
    const auto snap = sbx::Get();
    return snap && snap->inGame;
}

// ------------------------------------------------------------- slime dj ----
// Ranch > Slime DJ (module "slime_dj"). Reads only the snapshot the module copies on the main
// thread (slime_dj_bridge.h); every change is a posted request. Left: the songs in the music
// folder. Right: now playing (progress, controls, shuffle, volume) and the queue.
namespace djb = ::sr2::ui::slime_dj;

char g_djSearch[128] = {};
std::string g_djRadioEdit;  // Corral radio whose playlist the Songs card's + buttons fill ("" = the queue)

void DjRequest(djb::Op op, uint64_t song = 0, uint64_t uid = 0, double seconds = 0) {
    djb::Action a;
    a.op = op;
    a.song = song;
    a.uid = uid;
    a.seconds = seconds;
    djb::Request(a);
}

// Seek bar: while dragging it shows the drag position; after release the target until the module's
// snapshot reaches it (the new decoder needs a moment), at most 3 s.
struct DjSeek {
    bool dragging = false;
    uint64_t song = 0;
    double target = -1;              // seconds, -1 = none
    unsigned long long until = 0;    // GetTickCount64 limit for showing `target`
};
DjSeek g_djSeek;

std::string Clock(double seconds) {
    if (!(seconds > 0)) return "0:00";
    const long long s = static_cast<long long>(seconds + 0.5);
    if (s >= 3600) return std::format("{}:{:02}:{:02}", s / 3600, s / 60 % 60, s % 60);
    return std::format("{}:{:02}", s / 60, s % 60);
}

bool ContainsCi(const std::string& hay, const std::string& needle) {
    if (needle.empty()) return true;
    auto low = [](char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; };
    return std::search(hay.begin(), hay.end(), needle.begin(), needle.end(),
                       [&](char a, char b) { return low(a) == low(b); }) != hay.end();
}

// Small square icon button drawn by `draw(centre, colour)`; true when clicked.
template <class F>
bool DjIconButton(const char* id, float size, const char* tip, F&& draw) {
    const Palette& P = theme::Colors();
    const bool clicked = nav::Button(id, ImVec2(size, size));
    const bool hov = ImGui::IsItemHovered();
    const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (hov) dl->AddRectFilled(a, b, ImGui::GetColorU32(P.frameHover), U(6));
    nav::Ring(U(6));
    draw(ImVec2((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f), ImGui::GetColorU32(hov ? P.text : P.soft));
    HoverTip(tip);
    return clicked;
}

void DrawPlus(ImDrawList* dl, ImVec2 c, ImU32 col) {
    const float r = U(5.5f), th = U(2.2f);
    dl->AddLine(ImVec2(c.x - r, c.y), ImVec2(c.x + r, c.y), col, th);
    dl->AddLine(ImVec2(c.x, c.y - r), ImVec2(c.x, c.y + r), col, th);
}

void DrawArrow(ImDrawList* dl, ImVec2 c, ImU32 col, bool up) {
    const float s = up ? -1.0f : 1.0f;
    dl->PathLineTo(ImVec2(c.x - U(4.5f), c.y - s * U(2.5f)));
    dl->PathLineTo(ImVec2(c.x, c.y + s * U(2.5f)));
    dl->PathLineTo(ImVec2(c.x + U(4.5f), c.y - s * U(2.5f)));
    dl->PathStroke(col, 0, U(2.2f));
}

// One list row: text on the left (clipped), `right` dim at the right edge, room for `buttons`
// icon buttons after it. Returns true when the row itself was clicked.
bool DjRow(const char* id, const std::string& text, const std::string& right, bool current, bool bad, float buttonsW) {
    const Palette& P = theme::Colors();
    const float w = ImGui::GetContentRegionAvail().x - buttonsW, h = U(30);
    const bool clicked = nav::Button(id, ImVec2(std::max(1.0f, w), h));
    const bool hov = ImGui::IsItemHovered();
    const ImVec2 a = ImGui::GetItemRectMin(), b(a.x + w + buttonsW, a.y + h);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (current || hov) dl->AddRectFilled(a, b, ImGui::GetColorU32(current ? P.sel : P.hover), U(8));
    nav::Ring(U(8));
    ImFont* body = theme::Body();
    const float fs = theme::kBodySize;
    const ImVec2 rs = TextSize(body, theme::kSmallSize, right.c_str());
    const float ty = a.y + (h - ImGui::GetFontSize()) * 0.5f;
    const float textMax = a.x + w - rs.x - U(18);
    dl->PushClipRect(a, ImVec2(std::max(a.x, textMax), b.y), true);
    const ImU32 ink = bad ? P.dim : current ? (theme::Current() == theme::Mode::Dark ? P.grab : P.accent) : P.text;
    Text(dl, body, fs, ImVec2(a.x + U(10), ty), ImGui::GetColorU32(ink), text.c_str());
    dl->PopClipRect();
    Text(dl, body, theme::kSmallSize, ImVec2(a.x + w - rs.x - U(8), a.y + (h - rs.y) * 0.5f), ImGui::GetColorU32(P.dim),
         right.c_str());
    return clicked;
}

void DjSongs(const djb::Snapshot& s, float w, float listH) {
    const Palette& P = theme::Colors();
    BeginCard("dj_songs", w);
    const float avail = ImGui::GetContentRegionAvail().x, x0 = ImGui::GetCursorPosX();
    const size_t count = s.songs ? s.songs->size() : 0;
    CardHeading(count ? std::format("Songs ({})", count).c_str() : "Songs", P.head);
    const float rescanW = U(76), folderW = U(96), gap = ImGui::GetStyle().ItemSpacing.x;
    ImGui::SameLine(x0 + avail - folderW - gap - rescanW);
    ImGui::BeginDisabled(s.scanning);
    if (ImGui::Button("Rescan", ImVec2(rescanW, 0))) DjRequest(djb::Op::Rescan);
    ImGui::EndDisabled();
    HoverTip("Read the music folder again (after adding or removing songs).");
    ImGui::SameLine(0, gap);
    if (ImGui::Button("Open folder", ImVec2(folderW, 0))) djb::OpenFolder(s.folder);
    HoverTip("Opens " + s.folder + "\nPut .ogg, .mp3 or .wav files there (subfolders work too), then Rescan.");
    item_grid::SearchBox("##djsearch", "Search songs...", g_djSearch, sizeof(g_djSearch), 0.0f, g_scale);
    const std::string needle = g_djSearch;
    // + adds to a corral radio's playlist while one is being edited (DjRadios "Add songs").
    const djb::Radio* target = nullptr;
    for (const djb::Radio& r : s.radios)
        if (r.key == g_djRadioEdit) target = &r;
    if (!target) g_djRadioEdit.clear();
    const std::string addTip = target ? "Add to " + target->name + "'s playlist" : std::string("Add to the queue");
    if (target) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(P.accent), "%s",
                           std::format("+ adds to {}'s playlist ({} song{}).", target->name, target->playlist.size(),
                                       target->playlist.size() == 1 ? "" : "s").c_str());
        ImGui::PopTextWrapPos();
        ImGui::SameLine();
        if (ImGui::SmallButton("Done")) g_djRadioEdit.clear();
        HoverTip("Back to adding songs to the queue.");
    }

    ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(U(4), U(2)));
    // One Tab stop for the list (the song focused last), the arrows move between rows and their + (nav.h).
    nav::TabGroup songNav("##djsongnav");
    ImGui::BeginChild("##djsonglist", ImVec2(0, listH), ImGuiChildFlags_NavFlattened);
    if (!s.songs) {
        DimText(s.scanning ? "Reading the music folder..." : "The music folder has not been read yet.");
    } else if (s.songs->empty()) {
        DimText("No songs yet. Put .ogg, .mp3 or .wav files in\n" + s.folder + "\nthen press Rescan.");
    } else {
        const float bs = U(26);
        int shown = 0;
        for (const djb::Song& song : *s.songs) {
            if (!ContainsCi(song.name, needle) && !ContainsCi(song.file, needle)) continue;
            ++shown;
            ImGui::PushID(static_cast<const void*>(&song));
            const bool bad = !song.error.empty();
            const bool current = s.hasSong && s.song == song.id;
            const float rowY = ImGui::GetCursorPosY();
            songNav.Item(song.id);
            if (DjRow("##row", song.name, song.seconds > 0 ? Clock(song.seconds) : std::string(), current, bad, bs + U(4)) && !bad)
                DjRequest(djb::Op::Play, song.id);
            songNav.Done();
            HoverTip(bad ? song.file + "\nNot playable: " + song.error : song.file + "\nClick to play now.");
            ImGui::SameLine(0, U(4));
            ImGui::SetCursorPosY(rowY + (U(30) - bs) * 0.5f);
            ImGui::BeginDisabled(bad);
            nav::PushNoTabStop();
            if (DjIconButton("##add", bs, addTip.c_str(), [](ImVec2 c, ImU32 col) { DrawPlus(ImGui::GetWindowDrawList(), c, col); })) {
                if (target) {
                    djb::Action a;
                    a.op = djb::Op::RadioAdd;
                    a.radio = target->key;
                    a.song = song.id;
                    djb::Request(a);
                } else {
                    DjRequest(djb::Op::Enqueue, song.id);
                }
            }
            nav::PopNoTabStop();
            ImGui::EndDisabled();
            // Back to the row's bottom; the Dummy is the item that ends the line (moving the cursor
            // past the content without one trips ImGui's "submit an item" check at EndChild).
            ImGui::SetCursorPosY(rowY + U(30));
            ImGui::Dummy(ImVec2(0, 0));
            ImGui::PopID();
        }
        if (shown == 0) DimText("No song matches the search.");
    }
    ImGui::EndChild();
    songNav.End();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
    if (s.scanning && s.songs) DimText("Reading the music folder...");
    EndCard();
}

void DjNowPlaying(const Host& h, const ModuleView& mv, const djb::Snapshot& s, float w) {
    const Palette& P = theme::Colors();
    BeginCard("dj_now", w);
    CardHeading("Now playing", P.head);
    {
        FontScope f(theme::Display(), theme::kDisplaySize + 1);
        ImGui::PushTextWrapPos(0.0f);
        ImGui::PushStyleColor(ImGuiCol_Text, s.hasSong ? P.text : P.dim);
        ImGui::TextUnformatted(s.hasSong ? s.name.c_str() : "Nothing");
        ImGui::PopStyleColor();
        ImGui::PopTextWrapPos();
    }
    // seek bar: click or drag (the song jumps there on release)
    const float avail = ImGui::GetContentRegionAvail().x;
    const ImVec2 a = ImGui::GetCursorScreenPos();
    const float barH = U(6), hitH = U(16);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const bool seekable = s.hasSong && s.duration > 0;
    DjSeek& sk = g_djSeek;
    if (sk.song != s.song || !s.hasSong) sk = DjSeek{false, s.song, -1, 0};
    double pos = s.position;
    ImGui::BeginDisabled(!seekable);
    ImGui::InvisibleButton("##djseek", ImVec2(std::max(1.0f, avail), hitH));
    ImGui::EndDisabled();
    const bool hov = seekable && ImGui::IsItemHovered();
    if (seekable && ImGui::IsItemActive()) {
        sk.dragging = true;
        const float mx = std::clamp((ImGui::GetIO().MousePos.x - a.x) / std::max(1.0f, avail), 0.0f, 1.0f);
        sk.target = mx * s.duration;
        pos = sk.target;
    } else if (sk.dragging) {
        sk.dragging = false;
        if (seekable && sk.target >= 0) {
            DjRequest(djb::Op::Seek, 0, 0, sk.target);
            sk.until = h.nowMs + 3000;
            pos = sk.target;
        }
    } else if (sk.target >= 0) {
        if (h.nowMs < sk.until && std::fabs(s.position - sk.target) > 1.0) pos = sk.target;
        else sk.target = -1;
    }
    if (hov || sk.dragging) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        if (seekable) Tooltip(Clock(sk.dragging ? pos : std::clamp((ImGui::GetIO().MousePos.x - a.x) / std::max(1.0f, avail), 0.0f, 1.0f) * s.duration));
    }
    const float frac = s.duration > 0 ? static_cast<float>(std::clamp(pos / s.duration, 0.0, 1.0)) : 0.0f;
    const float by = a.y + (hitH - barH) * 0.5f;
    dl->AddRectFilled(ImVec2(a.x, by), ImVec2(a.x + avail, by + barH), ImGui::GetColorU32(P.frame), barH * 0.5f);
    if (frac > 0)
        dl->AddRectFilled(ImVec2(a.x, by), ImVec2(a.x + std::max(barH, avail * frac), by + barH), ImGui::GetColorU32(P.accent),
                          barH * 0.5f);
    if (seekable && (hov || sk.dragging))  // play head knob
        dl->AddCircleFilled(ImVec2(a.x + avail * frac, by + barH * 0.5f), U(6.5f), ImGui::GetColorU32(P.grab));
    {
        FontScope f(theme::Body(), theme::kSmallSize);
        const std::string left = s.hasSong ? Clock(pos) : std::string("-:--");
        const std::string right = s.hasSong && s.duration > 0 ? Clock(s.duration) : std::string("-:--");
        const float x0 = ImGui::GetCursorPosX();
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(P.dim), "%s", left.c_str());
        ImGui::SameLine(x0 + avail - ImGui::CalcTextSize(right.c_str()).x);
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(P.dim), "%s", right.c_str());
    }
    // controls
    const float sp = ImGui::GetStyle().ItemSpacing.x;
    const ImVec2 bs(std::floor((avail - 2 * sp) / 3), U(32));
    const bool playing = s.hasSong && !s.paused;
    if (AccentButton(playing ? "Pause" : "Play", bs)) DjRequest(djb::Op::TogglePause);
    HoverTip(s.hasSong ? (playing ? "Pause the song." : "Resume the song.")
                       : "Start the queue (or a random song with Shuffle on).");
    ImGui::SameLine();
    if (ImGui::Button("Skip", bs)) DjRequest(djb::Op::Skip);
    HoverTip("Next song: the queue's first, or a random one with Shuffle on.");
    ImGui::SameLine();
    ImGui::BeginDisabled(!s.hasSong);
    if (ImGui::Button("Stop", bs)) DjRequest(djb::Op::Stop);
    ImGui::EndDisabled();
    HoverTip("Stop and unload the song. The music box plays its own tune again.");
    if (!s.waiting.empty()) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(P.warn), "%s", s.waiting.c_str());
        ImGui::PopTextWrapPos();
    } else if (s.hasSong && s.speakers > 0) {
        const int boxes = s.speakers - s.emitterSpeakers;
        std::string from = boxes == 1 ? std::string("1 music box") : std::format("{} music boxes", boxes);
        if (s.emitterSpeakers > 0)
            from = (boxes > 0 ? from + " and " : std::string()) +
                   (s.emitterSpeakers == 1 ? std::string("1 speaker") : std::format("{} speakers", s.emitterSpeakers));
        DimText(std::format("Playing from {}{}.", from, s.speakers > 1 ? ", in sync" : ""));
    } else if (!s.hasSong && s.boxes == 0) {
        DimText("No music box nearby. Songs play from the corral Music Box upgrade.");
    }
    ImGui::Dummy(ImVec2(0, U(2)));
    // settings of the module
    for (const char* key : {"shuffle", "volume", "range", "amplify", "mute_vanilla", "muffle_walls", "dance_walls",
                            "dance_intensity"})
        if (const settings::Def* d = FindDef(mv, key); d && ParentChainAllows(mv, *d)) DrawRow(h, *d, d->label, false);
    EndCard();
}

void DjQueue(const djb::Snapshot& s, float w, float listH) {
    const Palette& P = theme::Colors();
    BeginCard("dj_queue", w);
    const float avail = ImGui::GetContentRegionAvail().x, x0 = ImGui::GetCursorPosX();
    CardHeading(s.queue.empty() ? "Queue" : std::format("Queue ({})", s.queue.size()).c_str(), P.head);
    ImGui::SameLine(x0 + avail - U(70));
    ImGui::BeginDisabled(s.queue.empty());
    if (ImGui::Button("Clear", ImVec2(U(70), 0))) DjRequest(djb::Op::ClearQueue);
    ImGui::EndDisabled();
    ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(U(2), U(2)));
    nav::TabGroup queueNav("##djqueuenav");
    ImGui::BeginChild("##djqueue", ImVec2(0, listH), ImGuiChildFlags_NavFlattened);
    if (s.queue.empty()) {
        DimText(s.shuffle ? "Empty: random songs play next (Shuffle is on). Use + on a song to queue it."
                          : "Empty: playback stops after this song. Use + on a song to queue it.");
    }
    const float bs = U(24);
    for (size_t i = 0; i < s.queue.size(); ++i) {
        const djb::QueueEntry& q = s.queue[i];
        ImGui::PushID(static_cast<int>(q.uid));
        const float rowY = ImGui::GetCursorPosY();
        queueNav.Item(q.uid);
        DjRow("##row", std::format("{}. {}", i + 1, q.name), q.seconds > 0 ? Clock(q.seconds) : std::string(), false, false,
              3 * (bs + U(2)) + U(2));
        queueNav.Done();
        nav::PushNoTabStop();
        ImGui::SameLine(0, U(2));
        ImGui::SetCursorPosY(rowY + (U(30) - bs) * 0.5f);
        ImGui::BeginDisabled(i == 0);
        if (DjIconButton("##up", bs, "Move up", [](ImVec2 c, ImU32 col) { DrawArrow(ImGui::GetWindowDrawList(), c, col, true); }))
            DjRequest(djb::Op::MoveUp, 0, q.uid);
        ImGui::EndDisabled();
        ImGui::SameLine(0, U(2));
        ImGui::BeginDisabled(i + 1 == s.queue.size());
        if (DjIconButton("##down", bs, "Move down", [](ImVec2 c, ImU32 col) { DrawArrow(ImGui::GetWindowDrawList(), c, col, false); }))
            DjRequest(djb::Op::MoveDown, 0, q.uid);
        ImGui::EndDisabled();
        ImGui::SameLine(0, U(2));
        if (DjIconButton("##rm", bs, "Remove from the queue", [](ImVec2 c, ImU32 col) { DrawCross(ImGui::GetWindowDrawList(), c, col); }))
            DjRequest(djb::Op::Remove, 0, q.uid);
        nav::PopNoTabStop();
        ImGui::SetCursorPosY(rowY + U(30));
        ImGui::Dummy(ImVec2(0, 0));  // see DjSongs
        ImGui::PopID();
    }
    ImGui::EndChild();
    queueNav.End();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
    EndCard();
}

// Corral radios: one section per loaded corral that owns the Corral Radio (custom_upgrades), with its
// own playlist. "Add songs" points the + buttons of the Songs card at that radio's playlist until
// "Done" (g_djRadioEdit = its key).
void DjRadioRequest(djb::Op op, const std::string& radio, int index = -1, uint64_t song = 0) {
    djb::Action a;
    a.op = op;
    a.radio = radio;
    a.index = index;
    a.song = song;
    djb::Request(a);
}

void DjRadios(const djb::Snapshot& s, float w) {
    const Palette& P = theme::Colors();
    BeginCard("dj_radios", w);
    const float avail = ImGui::GetContentRegionAvail().x, x0 = ImGui::GetCursorPosX();
    CardHeading(s.radios.empty() ? "Corral radios" : std::format("Corral radios ({})", s.radios.size()).c_str(), P.head);
    if (!s.radiosAvailable) {
        DimText("Turn on Custom upgrades to buy the Corral Radio for a corral (needs the Music Box): that corral then plays "
                "its own playlist instead of the queue.");
        EndCard();
        return;
    }
    if (s.radios.empty()) {
        DimText("No corral radio nearby. Buy the Corral Radio in a corral's plot menu (needs the Music Box); the corral "
                "shows up here with its own playlist while you are near it.");
        EndCard();
        return;
    }
    DimText(std::format("These corrals play their own playlist instead of the queue. Up to {} radios play at once.",
                        s.maxRadioStreams));
    const float bs = U(24), gap = ImGui::GetStyle().ItemSpacing.x;
    for (const djb::Radio& r : s.radios) {
        ImGui::PushID(r.key.c_str());
        ImGui::Dummy(ImVec2(0, U(4)));
        // name, distance, on / off
        {
            FontScope f(theme::Body(), theme::kBodySize + 1);
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(P.text), "%s", r.name.c_str());
        }
        HoverTip("Plot " + r.key);
        if (r.distance >= 0) {
            ImGui::SameLine();
            DimText(std::format("{:.0f} m away", r.distance));
        }
        ImGui::SameLine(x0 + avail - U(kSwitchW));
        if (Switch("##on", r.on)) DjRadioRequest(djb::Op::RadioToggle, r.key);
        HoverTip(r.on ? "Switch the radio off (the music box plays its own tune again)." : "Switch the radio on.");
        // now playing / why not
        if (r.hasSong) {
            const std::string pos = r.duration > 0 ? std::format("  {} / {}", Clock(r.position), Clock(r.duration)) : std::string();
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(P.accent), "%s", (r.now + pos).c_str());
            ImGui::PopTextWrapPos();
        }
        if (!r.waiting.empty()) {
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(P.warn), "%s", r.waiting.c_str());
            ImGui::PopTextWrapPos();
        } else if (!r.on) {
            DimText(r.playlist.empty() ? "Off. Add songs to its playlist, then switch it on." : "Off.");
        }
        // controls
        const bool editing = g_djRadioEdit == r.key;
        const ImVec2 b((avail - 3 * gap) / 4, 0);
        ImGui::BeginDisabled(!r.hasSong);
        if (ImGui::Button("Skip", b)) DjRadioRequest(djb::Op::RadioSkip, r.key);
        ImGui::EndDisabled();
        HoverTip("Next song of this playlist.");
        ImGui::SameLine();
        if (ImGui::Button(r.shuffle ? "Shuffle: on" : "Shuffle: off", b)) DjRadioRequest(djb::Op::RadioShuffle, r.key);
        HoverTip("On: a random other song of the playlist after each song. Off: the playlist in order, then from the top.");
        ImGui::SameLine();
        if (editing ? AccentButton("Done", b) : ImGui::Button("Add songs", b)) g_djRadioEdit = editing ? std::string() : r.key;
        HoverTip(editing ? "Stop adding songs to this playlist."
                         : "The + buttons in the Songs list add to this playlist (instead of the queue) until you press Done.");
        ImGui::SameLine();
        ImGui::BeginDisabled(r.playlist.empty());
        if (ImGui::Button("Clear", b)) DjRadioRequest(djb::Op::RadioClear, r.key);
        ImGui::EndDisabled();
        HoverTip("Empty the playlist (switches the radio off).");
        // playlist
        if (!r.playlist.empty()) {
            const float rowH = U(30) + U(2);
            const float listH = std::min(static_cast<float>(r.playlist.size()), 5.0f) * rowH;
            ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(0, 0, 0, 0));
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(U(2), U(2)));
            nav::TabGroup plNav("##plnav");
            ImGui::BeginChild("##pl", ImVec2(0, listH), ImGuiChildFlags_NavFlattened);
            for (size_t i = 0; i < r.playlist.size(); ++i) {
                const djb::RadioEntry& e = r.playlist[i];
                const int idx = static_cast<int>(i);
                ImGui::PushID(idx);
                const float rowY = ImGui::GetCursorPosY();
                const bool current = r.hasSong && idx == r.index;
                plNav.Item(static_cast<uint64_t>(i));
                if (DjRow("##row", std::format("{}. {}", i + 1, e.name), e.seconds > 0 ? Clock(e.seconds) : std::string(), current,
                          e.missing, 3 * (bs + U(2)) + U(2)) &&
                    !e.missing)
                    DjRadioRequest(djb::Op::RadioPlayAt, r.key, idx);
                plNav.Done();
                HoverTip(e.missing ? e.file + "\nNot in the music folder (or not playable): skipped." : e.file + "\nClick to play it now.");
                nav::PushNoTabStop();
                ImGui::SameLine(0, U(2));
                ImGui::SetCursorPosY(rowY + (U(30) - bs) * 0.5f);
                ImGui::BeginDisabled(i == 0);
                if (DjIconButton("##up", bs, "Move up", [](ImVec2 c, ImU32 col) { DrawArrow(ImGui::GetWindowDrawList(), c, col, true); }))
                    DjRadioRequest(djb::Op::RadioUp, r.key, idx);
                ImGui::EndDisabled();
                ImGui::SameLine(0, U(2));
                ImGui::BeginDisabled(i + 1 == r.playlist.size());
                if (DjIconButton("##down", bs, "Move down", [](ImVec2 c, ImU32 col) { DrawArrow(ImGui::GetWindowDrawList(), c, col, false); }))
                    DjRadioRequest(djb::Op::RadioDown, r.key, idx);
                ImGui::EndDisabled();
                ImGui::SameLine(0, U(2));
                if (DjIconButton("##rm", bs, "Remove from the playlist", [](ImVec2 c, ImU32 col) { DrawCross(ImGui::GetWindowDrawList(), c, col); }))
                    DjRadioRequest(djb::Op::RadioRemove, r.key, idx);
                nav::PopNoTabStop();
                ImGui::SetCursorPosY(rowY + U(30));
                ImGui::Dummy(ImVec2(0, 0));  // see DjSongs
                ImGui::PopID();
            }
            ImGui::EndChild();
            plNav.End();
            ImGui::PopStyleVar();
            ImGui::PopStyleColor();
        }
        ImGui::PopID();
    }
    EndCard();
}

void DjStatus(const Host& h) {
    const djb::Status st = djb::GetStatus();
    if (st.text.empty()) return;
    const Palette& P = theme::Colors();
    const bool fresh = h.nowMs - st.tick < 6000;
    ImGui::Dummy(ImVec2(0, U(2)));
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(st.error ? P.err : fresh ? P.head : P.dim), "%s", st.text.c_str());
    ImGui::PopTextWrapPos();
}

void DrawSlimeDjPage(const Host& h, const ModuleView& mv) {
    const Palette& P = theme::Colors();
    FailedBanner(mv);
    if (!ModuleOn(mv)) {
        BeginCard("dj_off", ImGui::GetContentRegionAvail().x);
        CardHeading("Slime DJ", P.head);
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted("Plays your own songs from the corral Music Box: put .ogg, .mp3 or .wav files in the "
                               "sr2mods_music folder in the game folder, pick a song or build a queue, and every music "
                               "box nearby plays it.");
        ImGui::PopTextWrapPos();
        DimText("Slime DJ is off. Turn it on with the switch at the top right.");
        EndCard();
        return;
    }
    djb::MarkViewed();
    const auto snap = djb::Get();
    if (!snap || h.nowMs - snap->tick > 3000) {
        OffNote("Starting the DJ booth...");
        return;
    }
    const float w = ColumnWidth();
    const float gapY = U(kGap) - ImGui::GetStyle().ItemSpacing.y;
    DjSongs(*snap, w, U(380));
    ImGui::SameLine(0, U(kGap));
    ImGui::BeginGroup();
    DjNowPlaying(h, mv, *snap, w);
    ImGui::Dummy(ImVec2(0, gapY));
    DjQueue(*snap, w, U(150));
    ImGui::EndGroup();
    ImGui::Dummy(ImVec2(0, gapY));
    DjRadios(*snap, 2 * w + U(kGap));
    DjStatus(h);
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
            // Not SameLine(x): inside BeginGroup its offset counts from the group's start, which put the
            // first line one card padding too far right (flush with the card's border).
            ImGui::SameLine(0.0f, 0.0f);
            ImGui::SetCursorPosX(x0 + avail - tw);
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

// Discord Rich Presence (module "discord_rpc", modules/discord_rpc): switch, application id, what to show and
// the pipe thread's connection status (discord_bridge.h). The id is a free-text value in sr2mods.json.
namespace dsc = ::sr2::ui::discord;
char g_discordId[32] = {};
bool g_discordIdEditing = false;

// A web page in the default browser. ShellExecuteW runs on a short-lived thread (as Open folder), so a
// slow shell never stalls the render thread.
DWORD WINAPI OpenUrlThread(LPVOID p) {
    std::wstring* url = static_cast<std::wstring*>(p);
    const HINSTANCE r = ShellExecuteW(nullptr, L"open", url->c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(r) <= 32) SR2_WARN("ui: cannot open the browser (ShellExecute {})", reinterpret_cast<INT_PTR>(r));
    delete url;
    return 0;
}

void OpenUrl(const char* asciiUrl) {
    auto* url = new std::wstring(asciiUrl, asciiUrl + std::strlen(asciiUrl));
    if (HANDLE t = CreateThread(nullptr, 0, &OpenUrlThread, url, 0, nullptr)) CloseHandle(t);
    else delete url;
}

void DiscordCard(const Host& h, const ModuleView& mv, float w) {
    const Palette& P = theme::Colors();
    const bool on = ModuleOn(mv);
    BeginCard("discord", w);
    const float avail = ImGui::GetContentRegionAvail().x, x0 = ImGui::GetCursorPosX();
    {
        FontScope f(theme::Display(), theme::kDisplaySize - 1);
        ImGui::AlignTextToFramePadding();
        ImGui::PushStyleColor(ImGuiCol_Text, P.head);
        ImGui::TextUnformatted("Discord Rich Presence");
        ImGui::PopStyleColor();
    }
    if (mv.hasEnabled) {
        ImGui::SameLine(x0 + avail - U(kSwitchW));
        if (Switch("##enabled", on)) Write(mv.enabled, on ? 0.0 : 1.0);
        HoverTip("Show what you are doing in the game on your Discord profile.");
    }
    bool failed = false;
    const std::string status = ModuleStatus(mv.module, &failed);
    if (failed) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(P.err), "Failed: %s", status.c_str());
        ImGui::PopTextWrapPos();
    }
    DimText("Shows the area you are in, \"On the ranch\" or \"Exploring\" and the time since you loaded your save on "
            "your Discord profile (\"In the main menu\" at the title screen). Talks to the Discord desktop app "
            "directly; nothing is sent anywhere else.");
    ImGui::Dummy(ImVec2(0, U(2)));

    const settings::Def* official = nullptr;
    for (const auto& g : mv.groups)
        for (const auto& d : g.defs)
            if (d.key == dsc::kKeyOfficial) official = &d;
    ImGui::BeginDisabled(!on);
    if (official) DrawRow(h, *official, official->label, false);
    ImGui::EndDisabled();
    const bool ownApp = !official || !IsOn(*official);

    if (ownApp) {
        // Application id: kept in sync with sr2mods.json while the field is not being edited.
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Application ID");
        const char* idHelp = "In the Discord Developer Portal (button below), create an application with the name "
                             "your profile should show, then paste its Application ID here.";
        HoverTip(idHelp);
        const float ctlW = std::min(U(260), std::floor(avail * 0.55f));
        ImGui::SameLine(x0 + avail - ctlW);
        if (!g_discordIdEditing) CopyToBuf(g_discordId, sizeof(g_discordId), settings::GetString(dsc::kModule, dsc::kKeyAppId));
        ImGui::SetNextItemWidth(ctlW);
        ImGui::InputTextWithHint("##appid", "paste your application id", g_discordId, sizeof(g_discordId),
                                 ImGuiInputTextFlags_CharsDecimal | ImGuiInputTextFlags_AutoSelectAll);
        g_discordIdEditing = ImGui::IsItemActive();
        if (ImGui::IsItemDeactivatedAfterEdit()) settings::SetString(dsc::kModule, dsc::kKeyAppId, g_discordId);
        HoverTip(idHelp);
        DimText("Create an application in the Discord Developer Portal and paste its Application ID (hover the field "
                "for the steps).");
    }

    ImGui::BeginDisabled(!on);
    for (const auto& g : mv.groups)
        for (const auto& d : g.defs)
            if (&d != official) DrawRow(h, d, d.label, false);
    ImGui::EndDisabled();

    const dsc::Status st = dsc::GetStatus();
    const ImU32 col = !on ? P.dim
                      : st.level == dsc::Level::Ok    ? P.ok
                      : st.level == dsc::Level::Warn  ? P.warn
                      : st.level == dsc::Level::Error ? P.err
                                                      : P.dim;
    // Short state on the line; the technical detail (pipe, handshake, error codes, retry) in its tooltip.
    std::string tip = on ? st.detail : std::string();
    if (on && st.retryAt) {
        const unsigned long long now = GetTickCount64();
        if (st.retryAt > now) {
            if (!tip.empty()) tip += "\n";
            tip += std::format("Next try in {} s.", (st.retryAt - now + 999) / 1000);
        }
    }
    KeyValue("Status", on ? st.text : std::string("Off"), col, tip);

    // Reconnect (skips any retry wait) and, with an own application, the Developer Portal in the browser.
    ImGui::Dummy(ImVec2(0, U(2)));
    const float gap = U(8);
    const float bw = ownApp ? std::floor((avail - gap) * 0.5f) : avail;
    ImGui::BeginDisabled(!on);
    if (ImGui::Button("Reconnect", ImVec2(bw, 0))) dsc::RequestReconnect();
    ImGui::EndDisabled();
    HoverTip("Connect to the Discord app again now, for example after starting Discord.");
    if (ownApp) {
        ImGui::SameLine(0, gap);
        if (ImGui::Button("Developer Portal", ImVec2(bw, 0))) OpenUrl(dsc::kDevPortalUrl);
        HoverTip(std::string("Opens ") + dsc::kDevPortalUrl + " in your browser, to create an application and copy "
                 "its id.");
    }
    EndCard();
}

// Overlay and Status on the left, Discord Rich Presence on the right.
void DrawSettingsPage(const Host& h, const ModuleView* ui, const std::vector<ModuleView>& views) {
    const Palette& P = theme::Colors();
    const float w = ColumnWidth();
    ImGui::BeginGroup();
    BeginCard("overlay", w);
    CardHeading("Overlay", P.head);
    if (ui)
        for (const auto& g : ui->groups)
            for (const auto& d : g.defs)
                if (d.key != kKeyOverlayTheme) DrawRow(h, d, d.key == kKeyOverlayHint ? "Show hint at startup" : d.label, false);
    EndCard();
    ImGui::Dummy(ImVec2(0, U(kGap) - ImGui::GetStyle().ItemSpacing.y));
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
    ImGui::EndGroup();
    if (const ModuleView* mv = FindView(views, layout::kDiscordModule)) {
        ImGui::SameLine(0, U(kGap));
        ImGui::PushID(mv->module.c_str());
        DiscordCard(h, *mv, w);
        ImGui::PopID();
    }
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
    const bool clicked = nav::Button(c.id.c_str(), ImVec2(w, U(kNavH)));
    const bool hovered = ImGui::IsItemHovered();
    const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (selected || hovered) dl->AddRectFilled(a, b, selected ? P.accent : P.hover, U(12));
    nav::Ring(U(12));
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
        if (nav::Button("##theme", ImVec2(half, U(30)))) SetTheme(ui, m);
        const bool hovered = ImGui::IsItemHovered();
        const bool sel = cur == m;
        const ImU32 bg = sel ? P.tabSel : P.frame;
        if (sel) dl->AddRectFilled(p, ImVec2(p.x + half, p.y + U(30)), bg, U(9));
        nav::Ring(U(9));
        ImGui::PopID();
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
    ImGui::BeginChild("##side", ImVec2(U(kSideW), height), ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_NavFlattened,
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
        ImFontBaked* b = bf->GetFontBaked(U(size));  // nullptr only with a locked atlas
        const ImFontGlyph* g = b ? b->FindGlyph('0') : nullptr;
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
        if (nav::Button("##buildstamp", ImVec2(lineW, U(16)))) {
            const char* full = ::sr2::core::build_info::Commit();
            ImGui::SetClipboardText(*full ? full : ::sr2::core::build_info::TimeIso());
            copiedUntil = h.nowMs + 1500;
        }
        const bool hovered = ImGui::IsItemHovered();
        nav::Ring(U(4));
        if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        // After "copied!" the normal text follows at once while the stamp stays
        // hovered: HoverTip's hover delay restarts after the click, which closed the
        // tooltip for a moment and reopened it. `noDelay` lasts until the mouse leaves.
        static bool noDelay = false;
        const char* tip = *::sr2::core::build_info::Commit() ? "Click to copy the commit" : "Click to copy the build time";
        if (copiedUntil != 0 && h.nowMs < copiedUntil) {
            Tooltip("copied!");
            noDelay = hovered;
        } else {
            copiedUntil = 0;
            if (!hovered) noDelay = false;
            if (noDelay) Tooltip(tip);
            else HoverTip(tip);
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
}

// --------------------------------------------------------------- top bar ----
bool IconButton(const char* id, float size) {
    const bool clicked = nav::Button(id, ImVec2(size, size));
    if (ImGui::IsItemHovered())
        ImGui::GetWindowDrawList()->AddRectFilled(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), theme::Colors().frameHover,
                                                  U(10));
    nav::Ring(U(10));
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
    if (t.kind != TabKind::Module && t.kind != TabKind::Sandbox && t.kind != TabKind::SlimeDj) return s;
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
        if (nav::Button("##tab", ImVec2(tw, tabH))) ti = i;
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
        nav::Ring(U(10));
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
        HoverTip(!st.lock.empty()           ? st.lock
                 : st.locked                ? tab.help
                 : !st.sw->tooltip.empty() ? st.sw->tooltip
                                            : st.sw->label);
        // No "Enabled" label: the switch explains itself in its tooltip. (The Sandbox key hints that sat
        // here are a bar under the page since the Sandbox got its own category: SandboxHintBar.)
    }
    if (tab.kind == TabKind::Module || tab.kind == TabKind::Profiles || (tab.kind == TabKind::SlimeDj && st.on)) {
        rx -= ib + U(10);
        ImGui::SetCursorScreenPos(ImVec2(rx, cy - ib * 0.5f));
        if (IconButton("##reset", ib)) {
            if (tab.kind == TabKind::Profiles) profiles::Refresh();
            else if (tab.kind == TabKind::SlimeDj) DjRequest(djb::Op::Rescan);
            else ResetTab(tab, views);
        }
        DrawResetIcon(dl, ImVec2(rx + ib * 0.5f, cy), ImGui::IsItemHovered() ? P.text : P.soft);
        HoverTip(tab.kind == TabKind::Profiles  ? "Rescan the profiles folder"
                 : tab.kind == TabKind::SlimeDj ? "Rescan the music folder"
                                                : "Reset this tab to defaults (middle-click a setting to reset only that one)");
    }
    ImGui::SetCursorScreenPos(origin);
    ImGui::Dummy(ImVec2(barW, U(kTopH)));
    return closeClicked;
}

// Called right after BeginChild("##body_<page>"): each page keeps its scroll position across page switches
// and closing / reopening, and while its content is shorter for a moment. Only the user moves it:
// ImGui clamps the scroll to the content height, so a page whose content shrank for a frame or a few
// (the Sandbox page while its snapshot is late, a list rebuilt, a card collapsed) used to come back
// scrolled up, and the clamped position was saved as the page's ("the sandbox menu scrolls back up
// while working in it"). Now a clamp below the saved position is not saved: the saved one is
// re-applied (SetScrollY lands next frame, clamped again) until the content is long enough, the user
// scrolls (wheel over the page, the scrollbar dragged), or 10 s passed.
void KeepPageScroll(const std::string& page) {
    PageScroll& ps = g_pageScroll[page];
    const double now = ImGui::GetTime();
    if (page != g_lastPage) {
        g_lastPage = page;
        ps.pending = ps.want > 0.5f;
        ps.since = now;
    }
    const ImGuiIO& io = ImGui::GetIO();
    const float y = ImGui::GetScrollY(), maxY = ImGui::GetScrollMaxY();
    const ImVec2 wp = ImGui::GetWindowPos();
    const float sbX = wp.x + ImGui::GetWindowWidth() - ImGui::GetStyle().ScrollbarSize;
    const bool overScrollbar = ImGui::IsMouseHoveringRect(ImVec2(sbX, wp.y), ImVec2(sbX + ImGui::GetStyle().ScrollbarSize, wp.y + ImGui::GetWindowHeight()), false);
    const bool userScroll =
        (io.MouseWheel != 0.0f && !io.KeyCtrl && !io.KeyShift &&
         ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem)) ||
        (ImGui::IsMouseDown(ImGuiMouseButton_Left) && overScrollbar && maxY > 0.0f) ||
        GImGui->NavJustMovedToId != 0;  // Tab / arrows moved the focus (and scrolled it into view)
    if (userScroll) ps.pending = false;
    if (ps.pending) {
        if (std::fabs(y - ps.want) <= 0.5f || now - ps.since > 10.0) {
            ps.pending = false;
        } else {
            ImGui::SetScrollY(ps.want);
            return;
        }
    }
    if (!userScroll && y + 0.5f < ps.want && y >= maxY - 0.5f) {  // clamped: the content got shorter
        ps.pending = true;
        ps.since = now;
        return;
    }
    ps.want = y;
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
    spotlight::Close();  // the search box starts closed
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
        ImGui::BeginChild("##main", ImVec2(ws.x - U(kSideW), ws.y), ImGuiChildFlags_NavFlattened,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoBackground);
        closed = DrawTopBar(cat, ti, views);
        const Tab& tab = cat.tabs[static_cast<size_t>(ti)];
        const std::string page = cat.id + "/" + std::to_string(ti);

        // A page shown again (switched to, or the overlay reopened) starts at its saved scroll position
        // in its first frame: each page has its own child window (ImGui keeps its scroll and content
        // height), and the saved position is applied before Begin (SetScrollY would land a frame later).
        if (page != g_lastPage) {
            if (tab.kind == TabKind::Sandbox) g_sbxShownSince = ImGui::GetTime();
            if (const auto it = g_pageScroll.find(page); it != g_pageScroll.end() && it->second.want > 0.5f)
                ImGui::SetNextWindowScroll(ImVec2(-1.0f, it->second.want));
        }
        // The Sandbox pages with grids keep a strip under the body for their key hint bar.
        const ModuleView* sbxView = tab.kind == TabKind::Sandbox ? FindView(views, tab.module) : nullptr;
        const bool sbxBar = sbxView && SandboxHintBarShown(tab, *sbxView);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(U(18), 0));
        ImGui::BeginChild(("##body_" + page).c_str(), ImVec2(0, sbxBar ? -U(kSbxBarH) : 0.0f),
                          ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_NavFlattened,
                          ImGuiWindowFlags_NoBackground);
        KeepPageScroll(page);
        // The columns' span (the content region: without the padding and the scrollbar), for the hint bar.
        const float colX0 = ImGui::GetCursorScreenPos().x, colX1 = colX0 + ImGui::GetContentRegionAvail().x;
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
            case TabKind::Settings: DrawSettingsPage(h, ui, views); break;
            case TabKind::Sandbox:
                if (sbxView) DrawSandboxPage(h, *sbxView, tab.sandboxPage);
                break;
            case TabKind::SlimeDj:
                if (const ModuleView* mv = FindView(views, tab.module)) DrawSlimeDjPage(h, *mv);
                break;
        }
        ScrollToHighlight(page);  // a search result opened on this page
        ImGui::Dummy(ImVec2(0, U(18)));
        ImGui::EndChild();
        ImGui::PopStyleVar();
        if (sbxBar) {
            // Pinned under the page in the strip the body left free, a little wider than the columns.
            const float top = ImGui::GetItemRectMax().y + U(4);
            SandboxHintBar(ImGui::GetWindowDrawList(), ImVec2(std::floor(colX0 - U(kSbxBarSpill)), top),
                           ImVec2(std::floor(colX1 + U(kSbxBarSpill)), top + U(kSbxBarH) - U(14)));
        } else {
            g_sbxBarMin = g_sbxBarMax = ImVec2(0, 0);
        }
        ImGui::EndChild();
        ImGui::PopStyleVar();
    } else {
        ImGui::PopStyleVar(2);
    }
    ImGui::End();
    // Ctrl+Space / Alt+Space: the search box over the window (spotlight.cpp).
    spotlight::Target jump;
    if (spotlight::Draw({g_scale, g_winMin, g_winMax, &DrawIcon, &Switch, &SoftShadow}, cats, views, jump)) JumpTo(jump);
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
