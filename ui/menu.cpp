// Built only with Dear ImGui (third_party/imgui), like overlay.cpp.
#if defined(SR2_HAVE_IMGUI)

#include "menu.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "core/profiles.h"
#include "menu_layout.h"
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
std::string g_cat = "movement";
std::map<std::string, int> g_tab;  // selected tab per category
std::string g_lastPage;            // category/tab drawn last frame (scroll reset on change)
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

// Background, optional border and flat drop shadow of a floating window (tooltip, open
// dropdown), drawn by the window itself: its own ImGui background is turned off
// (PopupBg transparent, no border) so the shadow can sit underneath.
void FloatingSurface(bool outline) {
    const Palette& P = theme::Colors();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 a = ImGui::GetWindowPos(), sz = ImGui::GetWindowSize(), b(a.x + sz.x, a.y + sz.y);
    const float r = ImGui::GetStyle().PopupRounding;
    dl->PushClipRectFullScreen();
    dl->AddRectFilled(ImVec2(a.x + U(3), a.y + U(5)), ImVec2(b.x + U(3), b.y + U(5)), P.shadow, r);
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

void Tooltip(const std::string& text) {
    if (text.empty()) return;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(U(12), U(9)));
    PushFloatingStyle();
    if (ImGui::BeginTooltip()) {
        FloatingSurface(false);
        {
            FontScope f(theme::Body(), theme::kSmallSize + 0.5f);  // popped before EndTooltip
            ImGui::PushTextWrapPos(U(300));
            ImGui::TextUnformatted(text.c_str());
            ImGui::PopTextWrapPos();
        }
        ImGui::EndTooltip();
    }
    PopFloatingStyle();
    ImGui::PopStyleVar();
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

const settings::Def* FindDef(const ModuleView& mv, std::string_view key) {
    if (mv.hasEnabled && mv.enabled.key == key) return &mv.enabled;
    for (const auto& g : mv.groups)
        for (const auto& d : g.defs)
            if (d.key == key) return &d;
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

void SliderDef(const settings::Def& d, double cur, float w) {
    ImGui::SetNextItemWidth(w);
    if (d.kind == settings::Kind::Float) {
        float v = static_cast<float>(cur);
        char fmt[16];
        std::snprintf(fmt, sizeof(fmt), "%%.%df", StepDecimals(d.step));
        if (ImGui::SliderFloat("##v", &v, static_cast<float>(d.min), static_cast<float>(d.max), fmt, ImGuiSliderFlags_AlwaysClamp)) {
            const double s = Snap(d, v);
            if (std::fabs(s - cur) > 1e-9) Write(d, s);
        }
    } else {
        int v = static_cast<int>(std::lround(cur));
        if (ImGui::SliderInt("##v", &v, static_cast<int>(std::lround(d.min)), static_cast<int>(std::lround(d.max)), "%d",
                             ImGuiSliderFlags_AlwaysClamp)) {
            const double s = Snap(d, v);
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
    // The chosen option keeps a soft fill (and a pink label); the hovered one gets a stronger one.
    ImGui::PushStyleColor(ImGuiCol_Header, P.optSel);
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, P.optHover);
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, P.optHover);
    for (int i = 0; i < static_cast<int>(texts.size()); ++i) {
        const auto& t = texts[static_cast<size_t>(i)];
        const float innerW = ImGui::GetContentRegionAvail().x;
        const float lineH = ImGui::GetFontSize();
        const ImVec2 ds = t.desc.empty() ? ImVec2(0, 0) : TextSize(body, theme::kSmallSize, t.desc.c_str(), innerW - U(20));
        const float h = U(16) + lineH + (t.desc.empty() ? 0.0f : U(3) + ds.y);
        const bool sel = i == idx;
        ImGui::PushID(i);
        if (ImGui::Selectable("##opt", sel, 0, ImVec2(0, h)) && !sel) Write(d, i);
        if (sel) ImGui::SetItemDefaultFocus();
        const ImVec2 mn = ImGui::GetItemRectMin();
        ImDrawList* pdl = ImGui::GetWindowDrawList();
        const ImU32 labelCol = sel ? (theme::Current() == theme::Mode::Dark ? P.grab : P.accent) : P.text;
        Text(pdl, body, theme::kBodySize, ImVec2(mn.x + U(10), mn.y + U(8)), labelCol, t.label.c_str());
        if (!t.desc.empty())
            Text(pdl, body, theme::kSmallSize, ImVec2(mn.x + U(10), mn.y + U(8) + lineH + U(3)), P.dim, t.desc.c_str(),
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

    ImU32 col = nested ? P.soft : P.text;
    if (isBool && cur < 0.5) col = P.dim;
    ImGui::AlignTextToFramePadding();
    ImGui::PushTextWrapPos(x0 + avail - ctlW - U(12));
    ImGui::PushStyleColor(ImGuiCol_Text, col);
    ImGui::TextUnformatted(label.c_str());
    ImGui::PopStyleColor();
    ImGui::PopTextWrapPos();
    HoverTip(d.tooltip);
    ImGui::SameLine(x0 + avail - ctlW);
    switch (d.kind) {
        case settings::Kind::Bool:
            if (Switch("##b", cur >= 0.5)) Write(d, cur >= 0.5 ? 0.0 : 1.0);
            HoverTip(d.tooltip);
            break;
        case settings::Kind::Float:
        case settings::Kind::Int: SliderDef(d, cur, ctlW); break;
        case settings::Kind::Enum: ComboDef(d, cur, ctlW); break;
        case settings::Kind::Key: KeyDef(h, d, cur, ctlW); break;
    }
    ImGui::PopID();
}

// Options that depend on `parent`: slide open below it, indented on a thread line.
void DrawKids(const Host& h, const settings::Def& parent, const std::vector<settings::Def>& kids, bool visible,
              std::string_view masterLabel) {
    const Palette& P = theme::Colors();
    ImGuiStorage* st = ImGui::GetStateStorage();
    const ImGuiID id = ImGui::GetID(("kids/" + parent.key).c_str());
    float* t = st->GetFloatRef(id, visible ? 1.0f : 0.0f);
    *t = Approach(*t, visible ? 1.0f : 0.0f, 6.0f);
    if (*t <= 0.001f) return;
    float* fullH = st->GetFloatRef(id + 1, 0.0f);
    const float height = std::max(1.0f, *t >= 0.999f ? *fullH : *fullH * Smooth(*t));
    const ImVec2 start = ImGui::GetCursorScreenPos();
    ImGui::BeginChild(id, ImVec2(0, height), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoBackground);
    ImGui::Indent(U(22));
    ImGui::BeginDisabled(!visible);
    for (const auto& k : kids) DrawRow(h, k, layout::ShortLabel(k, &parent, "", masterLabel), true);
    ImGui::EndDisabled();
    *fullH = ImGui::GetCursorPosY();
    ImGui::Unindent(U(22));
    ImGui::EndChild();
    ImGui::GetWindowDrawList()->AddLine(ImVec2(start.x + U(9), start.y), ImVec2(start.x + U(9), start.y + height - U(10)),
                                        ImGui::GetColorU32(P.line), U(2));
}

// Rows of one group, with nested options under their parent.
struct Nesting {
    std::map<std::string, std::vector<settings::Def>> kidsOf;
    std::set<std::string> isKid;
};

Nesting BuildNesting(const ModuleView& mv, const std::vector<const GroupView*>& groups, std::string_view skipKey) {
    std::set<std::string> shown;
    for (const auto* g : groups)
        for (const auto& d : g->defs) shown.insert(d.key);
    Nesting n;
    for (const auto* g : groups)
        for (const auto& d : g->defs) {
            auto p = layout::ParentOf(mv.module, d.key);
            if (!p || p->key == skipKey || !shown.count(p->key)) continue;
            n.kidsOf[p->key].push_back(d);
            n.isKid.insert(d.key);
        }
    return n;
}

void DrawGroupRows(const Host& h, const ModuleView& mv, const GroupView& g, const Nesting& n, std::string_view skipKey,
                   std::string_view masterLabel) {
    for (const auto& d : g.defs) {
        if (d.key == skipKey || n.isKid.count(d.key)) continue;
        DrawRow(h, d, layout::ShortLabel(d, nullptr, g.name, masterLabel), false);
        auto it = n.kidsOf.find(d.key);
        if (it == n.kidsOf.end()) continue;
        const auto p = layout::ParentOf(mv.module, it->second.front().key);
        DrawKids(h, d, it->second, p && layout::ParentAllows(*p, Read(d)), masterLabel);
    }
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
    const Nesting n = BuildNesting(mv, groups, skipKey);
    std::vector<const GroupView*> cols[2];
    float heights[2] = {0, 0};
    for (const auto* g : groups) {
        float rows = g->name.empty() ? 0.0f : 1.3f;
        for (const auto& d : g->defs)
            if (d.key != skipKey && !n.isKid.count(d.key)) rows += 1.0f;
        if (rows <= 0.0f) continue;
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
    ImGui::BeginDisabled(!on);
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
                if (Switch("##enabled", on)) Write(mv->enabled, on ? 0.0 : 1.0);
                HoverTip(mv->enabled.tooltip);
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
            const Nesting n = BuildNesting(*mv, groups, {});
            ImGui::BeginDisabled(!on);
            for (const auto* g : groups) {
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

// ------------------------------------------------------------- settings ----
void KeyValue(const char* k, const std::string& v, ImU32 vcol) {
    const float avail = ImGui::GetContentRegionAvail().x, x0 = ImGui::GetCursorPosX();
    const float keyW = std::min(U(130), avail * 0.45f);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(k);
    ImGui::SameLine(x0 + keyW);
    ImGui::PushTextWrapPos(x0 + avail);
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(vcol), "%s", v.c_str());
    ImGui::PopTextWrapPos();
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
    KeyValue("Overlay", OverlayStatus(), P.dim);
    KeyValue("Options menu tab", OptionsTabStatus(), P.dim);
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
    const std::string slime = slime_icons::CurrentName();
    if (!slime.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) Tooltip(slime);

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

    const unsigned long long last = LastWriteTick();
    const bool busy = last != 0 && h.nowMs - last < 1500;
    const ImU32 col = busy ? P.warn : P.ok;
    const ImVec2 sp = Add(ImGui::GetCursorScreenPos(), ImVec2(U(12), U(14)));
    dl->AddCircleFilled(ImVec2(sp.x + U(4), sp.y), U(4), col);
    Text(dl, theme::Body(), theme::kSmallSize, ImVec2(sp.x + U(14), sp.y - U(8)), col, busy ? "Saving..." : "Saved");
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
    bool locked = false;       // switch unusable (master tab whose module is off)
};

TabState StateOf(const Tab& t, const std::vector<ModuleView>& views) {
    TabState s;
    if (t.kind != TabKind::Module) return s;
    const ModuleView* mv = FindView(views, t.module);
    if (!mv) return s;
    const bool moduleOn = ModuleOn(*mv);
    if (t.masterKey.empty()) {
        s.sw = mv->hasEnabled ? &mv->enabled : nullptr;
        s.on = moduleOn;
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
            ImGui::InvisibleButton("##help", ImVec2(hs, hs));
            const bool hov = ImGui::IsItemHovered() || ImGui::IsItemFocused();
            const ImVec2 c(rx + hs * 0.5f, cy);
            dl->AddCircle(c, hs * 0.5f - U(1), hov ? P.tabSel : P.line, 0, U(2));
            // Centre the glyph's ink, not its line box (which has room for descenders).
            ImFont* qf = theme::Body();
            const ImFontGlyph* g = qf->GetFontBaked(U(13))->FindGlyph('?');
            const ImVec2 qp = g ? ImVec2(c.x - (g->X0 + g->X1) * 0.5f, c.y - (g->Y0 + g->Y1) * 0.5f) : c;
            Text(dl, qf, 13, qp, hov ? P.text : P.soft, "?");
            if (hov) Tooltip(tab.help);
        }
        rx -= U(kSwitchW) + U(8);
        const float fh = ImGui::GetFrameHeight();
        ImGui::SetCursorScreenPos(ImVec2(rx, cy - fh * 0.5f));
        ImGui::BeginDisabled(st.locked);
        const bool on = IsOn(*st.sw);
        if (Switch("##enable", on)) Write(*st.sw, on ? 0.0 : 1.0);
        ImGui::EndDisabled();
        HoverTip(st.locked ? tab.help : st.sw->tooltip);
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

void FillPoly(ImDrawList* dl, const Poly& p, ImVec2 o, ImVec2 s, float lo, float hi, ImU32 clo, ImU32 chi, bool textured) {
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
        dl->PrimWriteVtx(v, textured ? ImVec2(v.x / disp.x, v.y / disp.y) : white, textured ? IM_COL32_WHITE : Mix(clo, chi, t));
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
    const ImVec2 o = g_peel.min, s(g_peel.max.x - g_peel.min.x, g_peel.max.y - g_peel.min.y);
    const Poly rect = {o, ImVec2(o.x + s.x, o.y), g_peel.max, ImVec2(o.x, o.y + s.y)};
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    // The old look, still on the top-left side of the edge.
    dl->PushTexture(ImTextureRef(g_peel.tex));
    FillPoly(dl, ClipDiagonal(rect, o, s, p, true), o, s, 0, 0, 0, 0, true);
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

void OnOpened() { slime_icons::OnOpen(); }

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
        ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(pos.x + U(6), pos.y + U(8)), ImVec2(g_winMax.x + U(6), g_winMax.y + U(8)),
                                                      P.shadow, rounding);
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
        if (page != g_lastPage) ImGui::SetScrollY(0);
        g_lastPage = page;
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
                ImGui::BeginDisabled(!st.on);
                DrawModuleCards(h, *mv, tab, tab.masterKey, masterLabel);
                for (const auto& n : tab.nested)
                    if (const ModuleView* nmv = FindView(views, n)) DrawNested(h, *nmv);
                ImGui::EndDisabled();
                break;
            }
            case TabKind::Cards: DrawCardsPage(h, tab, views); break;
            case TabKind::Profiles: DrawProfilesPage(h); break;
            case TabKind::Settings: DrawSettingsPage(h, ui); break;
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
    ImGui::SetNextWindowPos(ImVec2(U(16), U(16)), ImGuiCond_Always);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, P.pill);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, U(12));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(U(14), U(9)));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, U(4)));
    const ImGuiWindowFlags f = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
                               ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
    if (ImGui::Begin("##sr2mods_hint", nullptr, f)) {
        const ImVec4 key = ImGui::ColorConvertU32ToFloat4(theme::Current() == theme::Mode::Dark ? P.grab : P.accent);
        ImGui::TextUnformatted("sr2mods: press ");
        ImGui::SameLine(0, 0);
        ImGui::TextColored(key, "%s", h.hotkeyName.c_str());
        ImGui::SameLine(0, 0);
        ImGui::TextUnformatted(" for mod settings");
        if (h.versionMismatch)
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(P.warn), "Version warning: %s", h.versionStatus.c_str());
    }
    ImGui::End();
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
