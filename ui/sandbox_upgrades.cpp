// The Sandbox page's "Upgrades & Blueprints" card and the vacpack slot row's tier menu
// (sandbox_upgrades.h). Built only with Dear ImGui, like menu.cpp.
#if defined(SR2_HAVE_IMGUI)

#include "sandbox_upgrades.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <format>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include "hud_edit.h"
#include "imgui_internal.h"  // ImGuiSelectableFlags_SpanAvailWidth
#include "item_grid.h"
#include "sandbox_upgrades_bridge.h"
#include "theme.h"

namespace sr2::ui::sandbox_upgrades {
namespace {

namespace sbx = ::sr2::ui::sandbox;
namespace ub = ::sr2::ui::sandbox::upgrades;
using theme::Palette;

constexpr const char* kUpgradeCtx = "##sbx_upg_ctx";
constexpr const char* kBlueprintCtx = "##sbx_bp_ctx";
constexpr const char* kModCtx = "##sbx_mod_ctx";
constexpr double kPendingSeconds = 0.8;  // a requested change shown until the state has it

float g_scale = 1.0f;
float U(float v) { return v * g_scale; }

int g_tab = 0;                                  // 0 = Upgrades, 1 + i = recipe category i, 1 + count = Mod buildables
std::map<int, item_grid::State> g_bpGrid;       // search text per blueprint tab
uintptr_t g_ctxUpgrade = 0;                     // right-clicked upgrade
uintptr_t g_ctxBlueprint = 0;                   // right-clicked blueprint
int g_ctxCategory = -1;                         // its category
std::string g_ctxMod;                           // right-clicked mod buildable (id)
item_grid::State g_modGrid;                     // search text of the Mod buildables tab

// Changes asked for but not in a state yet, so quick wheel notches add up and tiles don't flicker.
struct Pending {
    int value = 0;
    double until = 0;  // ImGui::GetTime() deadline
};
std::unordered_map<uintptr_t, Pending> g_levelPending, g_bpPending;
std::unordered_map<std::string, Pending> g_modPending;  // value = requested mode

ImU32 Mix(ImU32 a, ImU32 b, float t) {
    const ImVec4 x = ImGui::ColorConvertU32ToFloat4(a), y = ImGui::ColorConvertU32ToFloat4(b);
    return ImGui::ColorConvertFloat4ToU32(ImVec4(x.x + (y.x - x.x) * t, x.y + (y.y - x.y) * t, x.z + (y.z - x.z) * t,
                                                 x.w + (y.w - x.w) * t));
}

void Text(ImDrawList* dl, float size, ImVec2 pos, ImU32 col, const char* text) {
    dl->AddText(theme::Body(), U(size), pos, col, text);
}

ImVec2 TextSize(float size, const char* text) { return theme::Body()->CalcTextSizeA(U(size), FLT_MAX, 0.0f, text); }

void DimText(const std::string& text) {
    ImGui::PushFont(theme::Body(), theme::kSmallSize);
    ImGui::PushStyleColor(ImGuiCol_Text, theme::Colors().dim);
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(text.c_str());
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
    ImGui::PopFont();
}

void HoverTip(const Helpers& h, const std::string& text) {
    if (!text.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort | ImGuiHoveredFlags_AllowWhenDisabled))
        h.tooltip(text);
}

std::string Roman(int n) {
    static const char* const kTens[] = {"", "X", "XX", "XXX"};
    static const char* const kOnes[] = {"", "I", "II", "III", "IV", "V", "VI", "VII", "VIII", "IX"};
    if (n <= 0 || n >= 40) return std::to_string(n);
    return std::string(kTens[n / 10]) + kOnes[n % 10];
}

int Level(const ub::Upgrade& u) {
    auto it = g_levelPending.find(u.id);
    if (it == g_levelPending.end()) return u.level;
    if (ImGui::GetTime() < it->second.until && it->second.value != u.level) return it->second.value;
    g_levelPending.erase(it);
    return u.level;
}

// "III of XII", "Not bought", "Hidden until found".
std::string TierText(const ub::Upgrade& u, int level) {
    if (level < 0) return u.discovered ? "Not bought" : "Hidden until found";
    std::string t = std::format("{} of {}", Roman(level + 1), Roman(u.levels));
    if (u.vanillaLevels > 0 && level >= u.vanillaLevels) t += " (custom tier)";
    return t;
}

void RequestLevel(const ub::Upgrade& u, int to) {
    to = std::clamp(to, -1, u.levels - 1);
    if (to == Level(u)) return;
    g_levelPending[u.id] = {to, ImGui::GetTime() + kPendingSeconds};
    ub::Action a;
    a.op = ub::Op::SetLevel;
    a.id = u.id;
    a.value = to;
    ub::Request(a);
}

void RequestOp(ub::Op op, uintptr_t id = 0) {
    ub::Action a;
    a.op = op;
    a.id = id;
    ub::Request(a);
}

bool Unlocked(const sbx::Recipe& r) {
    auto it = g_bpPending.find(r.id);
    if (it == g_bpPending.end()) return r.unlocked;
    if (ImGui::GetTime() < it->second.until && (it->second.value != 0) != r.unlocked) return it->second.value != 0;
    g_bpPending.erase(it);
    return r.unlocked;
}

void RequestBlueprints(std::vector<uintptr_t> ids, bool unlock) {
    if (ids.empty()) return;
    for (uintptr_t id : ids) g_bpPending[id] = {unlock ? 1 : 0, ImGui::GetTime() + 2.5};  // the recipe state lags a bit more
    ub::Action a;
    a.op = ub::Op::SetBlueprints;
    a.ids = std::move(ids);
    a.value = unlock ? 1 : 0;
    ub::Request(a);
}

const ub::Upgrade* FindUpgrade(const ub::State& st, uintptr_t id) {
    for (const ub::Upgrade& u : st.upgrades)
        if (u.id == id) return &u;
    return nullptr;
}

// The level circles, as the Fabricator draws them: filled up to the owned level; custom_upgrades' tiers
// past the vanilla max ringed in the warning colour. Fitted into `w`.
void Circles(ImDrawList* dl, ImVec2 p, float w, const ub::Upgrade& u, int level) {
    const Palette& P = theme::Colors();
    const int n = std::max(1, u.levels);
    const float r = std::clamp(w / (static_cast<float>(n) * 2.9f), U(2.5f), U(5.0f));
    const float step = r * 2.9f;
    for (int i = 0; i < n; ++i) {
        const ImVec2 c(p.x + r + step * static_cast<float>(i), p.y + r);
        const bool custom = u.vanillaLevels > 0 && i >= u.vanillaLevels;
        const ImU32 ring = custom ? P.warn : P.dim;
        if (i <= level) dl->AddCircleFilled(c, r, custom ? P.warn : (theme::Current() == theme::Mode::Dark ? P.grab : P.accent));
        else dl->AddCircle(c, r, ring, 0, std::max(1.0f, U(1.2f)));
    }
}

// Shift + wheel: one level up / down; Ctrl + wheel: to the last level / not bought. Over the last item.
void WheelLevels(const ub::Upgrade& u) {
    const ImGuiIO& io = ImGui::GetIO();
    if (!ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) || !(io.KeyShift || io.KeyCtrl)) return;
    ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY);
    ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelX);
    const float wheel = io.MouseWheel != 0.0f ? io.MouseWheel : io.MouseWheelH;
    if (wheel == 0.0f) return;
    const int cur = Level(u);
    if (io.KeyCtrl) RequestLevel(u, wheel > 0 ? u.levels - 1 : -1);
    else RequestLevel(u, cur + (wheel > 0 ? 1 : -1));
}

// The upgrade's right-click menu entries (the vacpack slots' Extra Tank tier menu, which shared them, is gone
// since round 9).
void LevelMenuItems(const ub::Upgrade& u, bool discovery) {
    const int cur = Level(u), last = u.levels - 1;
    if (KeyMenuItem("Upgrade", "Shift+Wheel", "up", cur < last, g_scale)) RequestLevel(u, cur + 1);
    if (KeyMenuItem("Downgrade", "Shift+Wheel", "down", cur >= 0, g_scale)) RequestLevel(u, cur - 1);
    if (KeyMenuItem("Max", "Ctrl+Wheel", "up", cur < last, g_scale)) RequestLevel(u, last);
    if (KeyMenuItem("Reset (not bought)", "Ctrl+Wheel", "down", cur >= 0, g_scale)) RequestLevel(u, -1);
    if (!discovery || !u.discoverable) return;
    ImGui::Separator();
    if (ImGui::MenuItem("Discover", nullptr, false, !u.discovered)) RequestOp(ub::Op::Discover, u.id);
    if (ImGui::MenuItem("Undiscover", nullptr, false, u.discovered || cur >= 0)) RequestOp(ub::Op::Undiscover, u.id);
}

void MenuTitle(const std::string& name, const std::string& tier) {
    const Palette& P = theme::Colors();
    ImGui::PushFont(theme::Body(), theme::kSmallSize);
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(P.soft), "%s", name.c_str());
    if (!tier.empty()) {
        ImGui::SameLine();
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(P.dim), "%s", tier.c_str());
    }
    ImGui::PopFont();
    ImGui::Separator();
}

template <class F>
void Popup(const Helpers& h, const char* id, F&& body) {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(U(10), U(8)));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(U(8), U(5)));
    h.pushFloating();
    if (ImGui::BeginPopup(id)) {
        h.surface(true);
        body();
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    h.popFloating();
    ImGui::PopStyleVar(2);
}

// ------------------------------------------------------------- upgrades ----
// Each upgrade: icon, name, tier and level circles, - / + at the right. Right-click: menu; Shift / Ctrl +
// wheel: levels.
void UpgradesTab(const Helpers& h, const ub::State& st) {
    const Palette& P = theme::Colors();
    int hidden = 0, left = 0;
    for (const ub::Upgrade& u : st.upgrades) {
        hidden += u.discoverable && !u.discovered;
        left += std::max(0, u.levels - 1 - std::max(Level(u), -1));
    }
    // Header: counts, then Discover all / Max at the right.
    const float sp = ImGui::GetStyle().ItemSpacing.x, bh = U(30);
    const float bw1 = std::max(U(120), ImGui::CalcTextSize("Discover all (00)").x + U(20)), bw2 = U(84);
    const float avail = ImGui::GetContentRegionAvail().x;
    const float x0 = ImGui::GetCursorPosX();
    ImGui::AlignTextToFramePadding();
    ImGui::PushFont(theme::Body(), theme::kSmallSize);
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(P.dim), "%s",
                       std::format("{} upgrades, {} level(s) to go{}", st.upgrades.size(), left,
                                   hidden ? std::format(", {} hidden until found", hidden) : std::string())
                           .c_str());
    ImGui::PopFont();
    ImGui::SameLine(x0 + avail - bw1 - bw2 - sp);
    ImGui::BeginDisabled(hidden == 0);
    if (ImGui::Button(std::format("Discover all ({})###disc", hidden).c_str(), ImVec2(bw1, bh))) RequestOp(ub::Op::DiscoverAll);
    ImGui::EndDisabled();
    HoverTip(h, hidden ? "Makes the hidden upgrades available to buy, as if found. Buys no level."
                       : "Every upgrade is discovered already.");
    ImGui::SameLine(0, sp);
    ImGui::BeginDisabled(left == 0);
    if (ImGui::Button("Max###max", ImVec2(bw2, bh))) RequestOp(ub::Op::MaxAll);
    ImGui::EndDisabled();
    HoverTip(h, left ? std::format("Raises every upgrade to its last level for free ({} level(s) to go).", left)
                     : "Every upgrade is at its last level already.");

    // Entries, in two columns when the card is wide enough.
    const int cols = avail >= U(560) ? 2 : 1;
    const float gap = U(8);
    const float colW = std::floor((avail - gap * static_cast<float>(cols - 1)) / static_cast<float>(cols));
    const float eh = U(58), pad = U(7), icon = eh - 2 * pad, btn = U(26);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    uintptr_t openMenu = 0;
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const size_t n = st.upgrades.size(), rows = (n + static_cast<size_t>(cols) - 1) / static_cast<size_t>(cols);
    for (size_t i = 0; i < n; ++i) {
        const ub::Upgrade& u = st.upgrades[i];
        const int level = Level(u);
        ImGui::PushID(static_cast<int>(i));
        const float col = static_cast<float>(i % static_cast<size_t>(cols));
        const float row = static_cast<float>(i / static_cast<size_t>(cols));
        const ImVec2 a(origin.x + col * (colW + gap), origin.y + row * (eh + gap)), b(a.x + colW, a.y + eh);
        ImGui::SetCursorScreenPos(a);
        ImGui::SetNextItemAllowOverlap();
        const bool pressed = ImGui::InvisibleButton("##entry", ImVec2(colW, eh), ImGuiButtonFlags_MouseButtonRight);
        const bool hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem | ImGuiHoveredFlags_AllowWhenOverlappedByItem);
        if (pressed) openMenu = u.id;
        WheelLevels(u);
        // No tooltip on the entry (round 8 feedback: name and tier are on it; the controls are in the
        // right-click menu).
        dl->AddRectFilled(a, b, hovered ? P.frameHover : P.frame, U(8));
        // Icon tile, dimmed while not bought.
        item_grid::Tile t;
        t.id = u.id;
        t.name = &u.name;
        t.icon = u.icon;
        t.dimmed = level < 0;
        const ImVec2 ia(a.x + pad, a.y + pad);
        item_grid::DrawTile(dl, ia, ImVec2(ia.x + icon, ia.y + icon), t, hovered, false, g_scale);
        // Name, tier, circles.
        const float tx = ia.x + icon + U(10), textW = b.x - tx - 2 * btn - U(14);
        dl->PushClipRect(ImVec2(tx, a.y), ImVec2(tx + std::max(0.0f, textW), b.y), true);
        Text(dl, theme::kBodySize, ImVec2(tx, a.y + U(6)), u.discovered || level >= 0 ? P.text : P.dim, u.name.c_str());
        const std::string tier = TierText(u, level);
        Text(dl, theme::kSmallSize, ImVec2(tx, a.y + U(25)), level < 0 ? P.dim : P.soft, tier.c_str());
        dl->PopClipRect();
        Circles(dl, ImVec2(tx, b.y - U(15)), std::max(U(40), textW), u, level);
        // - / +, the glyphs drawn as lines in the button's centre (the font's '-' / '+' sit above the
        // middle of a square button).
        const float by = a.y + (eh - btn) * 0.5f;
        auto glyph = [&](bool plus) {
            const ImVec2 c((ImGui::GetItemRectMin().x + ImGui::GetItemRectMax().x) * 0.5f,
                           (ImGui::GetItemRectMin().y + ImGui::GetItemRectMax().y) * 0.5f);
            const float half = std::round(btn * 0.2f), th = std::max(1.0f, std::round(U(1.6f)));
            const ImU32 col = ImGui::GetColorU32(ImGuiCol_Text);  // includes the disabled alpha
            dl->AddRectFilled(ImVec2(c.x - half, c.y - th * 0.5f), ImVec2(c.x + half, c.y + th * 0.5f), col);
            if (plus) dl->AddRectFilled(ImVec2(c.x - th * 0.5f, c.y - half), ImVec2(c.x + th * 0.5f, c.y + half), col);
        };
        ImGui::SetCursorScreenPos(ImVec2(b.x - pad - 2 * btn - U(4), by));
        ImGui::BeginDisabled(level < 0);
        if (ImGui::Button("##dn", ImVec2(btn, btn))) RequestLevel(u, level - 1);
        glyph(false);
        ImGui::EndDisabled();
        HoverTip(h, level == 0 ? "Back to not bought." : "One level down.");
        ImGui::SameLine(0, U(4));
        ImGui::BeginDisabled(level >= u.levels - 1);
        if (ImGui::Button("##up", ImVec2(btn, btn))) RequestLevel(u, level + 1);
        glyph(true);
        ImGui::EndDisabled();
        HoverTip(h, "One level up, free.");
        ImGui::PopID();
    }
    ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + static_cast<float>(rows) * (eh + gap) - gap));
    ImGui::Dummy(ImVec2(avail, 0));  // the entries' extent (the card sizes itself to it)
    if (openMenu) {
        g_ctxUpgrade = openMenu;
        ImGui::OpenPopup(kUpgradeCtx);
    }
    Popup(h, kUpgradeCtx, [&] {
        const ub::Upgrade* u = FindUpgrade(st, g_ctxUpgrade);
        if (!u) return ImGui::CloseCurrentPopup();
        MenuTitle(u->name, TierText(*u, Level(*u)));
        LevelMenuItems(*u, true);
    });
}

// ----------------------------------------------------------- blueprints ----
// One Fabricator gadget category: the crafter's recipes in it as tiles, faded while the blueprint is locked.
// Click toggles, right-click: menu; "Unlock all" for the tab.
void BlueprintTab(const Helpers& h, const sbx::Snapshot& s, int cat, const std::string& title) {
    std::vector<const sbx::Recipe*> list;
    if (s.recipes)
        for (const sbx::Recipe& r : *s.recipes)
            if (r.category == cat && !r.mod) list.push_back(&r);  // the mod's buildables: the Mod buildables tab
    std::vector<item_grid::Tile> tiles;
    std::vector<uintptr_t> locked, unlocked;
    tiles.reserve(list.size());
    for (const sbx::Recipe* r : list) {
        const bool on = Unlocked(*r);
        (on ? unlocked : locked).push_back(r->id);
        item_grid::Tile t;
        t.id = r->id;
        t.name = &r->name;
        t.icon = r->icon;
        t.dimmed = !on;
        t.accent = on ? theme::Colors().ok : 0;
        tiles.push_back(t);
    }
    item_grid::Options o;
    o.scale = g_scale;
    o.tile = 54.0f;
    o.height = 206.0f;
    o.hint = "Search blueprints...";
    o.empty = list.empty() ? "No blueprints in this category." : "No matching blueprints.";
    const std::string button = std::format("Unlock all ({})", locked.size());
    o.button = button.c_str();
    o.buttonEnabled = !locked.empty();
    const item_grid::Result r = item_grid::Draw(("##bp" + std::to_string(cat)).c_str(), tiles, g_bpGrid[cat], o);
    if (r.button) RequestBlueprints(locked, true);
    if (r.buttonHovered)
        h.tooltip(locked.empty() ? "Every blueprint of " + title + " is unlocked already."
                                 : std::format("Unlocks the {} locked blueprint(s) of {}, as if found.", locked.size(), title));
    if (r.clicked >= 0) {
        const sbx::Recipe* rec = list[static_cast<size_t>(r.clicked)];
        RequestBlueprints({rec->id}, !Unlocked(*rec));
    }
    if (r.rightClicked >= 0) {
        g_ctxBlueprint = list[static_cast<size_t>(r.rightClicked)]->id;
        g_ctxCategory = cat;
        ImGui::OpenPopup(kBlueprintCtx);
    }
    // Only the name (the tiles are icons); click locks / unlocks, the faded tile shows the state.
    if (r.hovered >= 0 && !ImGui::IsPopupOpen(kBlueprintCtx)) h.tooltip(list[static_cast<size_t>(r.hovered)]->name);
    Popup(h, kBlueprintCtx, [&] {
        const sbx::Recipe* rec = nullptr;
        for (const sbx::Recipe* x : list)
            if (x->id == g_ctxBlueprint) rec = x;
        if (!rec || g_ctxCategory != cat) return ImGui::CloseCurrentPopup();
        const bool on = Unlocked(*rec);
        MenuTitle(rec->name, on ? "unlocked" : "locked");
        if (ImGui::MenuItem("Unlock blueprint", nullptr, false, !on)) RequestBlueprints({rec->id}, true);
        if (ImGui::MenuItem("Lock blueprint", nullptr, false, on)) RequestBlueprints({rec->id}, false);
        ImGui::Separator();
        if (ImGui::MenuItem(std::format("Unlock all {} ({})", title, locked.size()).c_str(), nullptr, false, !locked.empty()))
            RequestBlueprints(locked, true);
        if (ImGui::MenuItem(std::format("Lock all {} ({})", title, unlocked.size()).c_str(), nullptr, false, !unlocked.empty()))
            RequestBlueprints(unlocked, false);
    });
    DimText("Lock takes a blueprint off your list again; gadgets you already have stay.");
}

// ------------------------------------------------------- mod buildables ----
// The mod's own buildables (Quantum Storage Link, DJ Booth, DJ Speaker, Slime Radio). Their Fabricator card
// follows the base gadget's blueprint; Sandbox can unlock / lock it per save (the mod's sidecar, never the
// vanilla save). Click toggles, right-click: menu with "follow the base blueprint".
int ModMode(const ub::ModBlueprint& m) {
    auto it = g_modPending.find(m.id);
    if (it == g_modPending.end()) return m.mode;
    if (ImGui::GetTime() < it->second.until && it->second.value != m.mode) return it->second.value;
    g_modPending.erase(it);
    return m.mode;
}

bool ModUnlocked(const ub::ModBlueprint& m) {
    const int mode = ModMode(m);
    return mode == 1 || (mode == 0 && m.baseUnlocked);
}

void RequestMod(const ub::ModBlueprint& m, int mode) {
    g_modPending[m.id] = {mode, ImGui::GetTime() + kPendingSeconds};
    ub::Action a;
    a.op = ub::Op::SetModBlueprint;
    a.key = m.id;
    a.value = mode;
    ub::Request(a);
}

std::string ModState(const ub::ModBlueprint& m) {
    switch (ModMode(m)) {
        case 1: return "Unlocked by Sandbox";
        case 2: return "Locked by Sandbox";
        default: return std::format("{} (follows the {} blueprint)", m.baseUnlocked ? "Unlocked" : "Locked", m.base);
    }
}

void ModTab(const Helpers& h, const ub::State& st) {
    std::vector<item_grid::Tile> tiles;
    tiles.reserve(st.mods.size());
    for (const ub::ModBlueprint& m : st.mods) {
        const bool on = ModUnlocked(m);
        item_grid::Tile t;
        t.id = std::hash<std::string>{}(m.id);
        t.name = &m.name;
        t.icon = m.icon;
        t.dimmed = !on;
        t.accent = on ? theme::Colors().ok : 0;
        t.badge = m.owned > 0 ? m.owned : -1;
        t.mod = true;
        tiles.push_back(t);
    }
    item_grid::Options o;
    o.scale = g_scale;
    o.tile = 54.0f;
    o.height = 120.0f;
    o.hint = "Search mod buildables...";
    o.empty = st.mods.empty() ? (st.modWhy.empty() ? "No mod buildables." : st.modWhy.c_str()) : "No matching buildables.";
    const item_grid::Result r = item_grid::Draw("##bpmod", tiles, g_modGrid, o);
    if (r.clicked >= 0) {
        const ub::ModBlueprint& m = st.mods[static_cast<size_t>(r.clicked)];
        RequestMod(m, ModUnlocked(m) ? 2 : 1);
    }
    if (r.rightClicked >= 0) {
        g_ctxMod = st.mods[static_cast<size_t>(r.rightClicked)].id;
        ImGui::OpenPopup(kModCtx);
    }
    if (r.hovered >= 0 && !ImGui::IsPopupOpen(kModCtx)) {
        // The item tooltips' look: name, "Mod" tag, the state in its colour, what unlocks it.
        const ub::ModBlueprint& m = st.mods[static_cast<size_t>(r.hovered)];
        const Palette& P = theme::Colors();
        Tip tip;
        tip.name = m.name;
        tip.tag = "Mod";
        tip.tagCol = h.gadgetAccent;
        tip.lines.push_back({ModState(m), ModUnlocked(m) ? P.ok : P.dim});
        tip.lines.push_back({m.hint, 0});
        if (m.owned > 0) tip.lines.push_back({std::format("{} unplaced", m.owned), 0});
        h.tip(tip);
    }
    Popup(h, kModCtx, [&] {
        const ub::ModBlueprint* m = nullptr;
        for (const ub::ModBlueprint& x : st.mods)
            if (x.id == g_ctxMod) m = &x;
        if (!m) return ImGui::CloseCurrentPopup();
        const int mode = ModMode(*m);
        MenuTitle(m->name, ModUnlocked(*m) ? "unlocked" : "locked");
        if (ImGui::MenuItem("Unlock blueprint", nullptr, mode == 1)) RequestMod(*m, 1);
        if (ImGui::MenuItem("Lock blueprint", nullptr, mode == 2)) RequestMod(*m, 2);
        if (ImGui::MenuItem(std::format("Follow the {} blueprint", m->base).c_str(), nullptr, mode == 0)) RequestMod(*m, 0);
    });
    DimText("The mod's own Fabricator cards unlock with their base gadget's blueprint. Unlock / lock here keeps your "
            "choice for this save in the mod's sidecar file; the game's save never sees it.");
}

}  // namespace

void DrawCard(const Helpers& h, const sbx::Snapshot& s, float w) {
    g_scale = h.scale;
    const Palette& P = theme::Colors();
    h.beginCard("sbx_upgrades", w);
    h.heading("Upgrades & Blueprints", P.head);
    const auto st = ub::Get();
    const int nCats = s.recipeCategories ? static_cast<int>(s.recipeCategories->size()) : 0;
    // The overlay's sub-tabs (the tab bar's strip, smaller), drawn before the content, so a click shows
    // the new tab in the same frame. Last: the mod's own buildables.
    std::vector<std::string> labels{"Upgrades"};
    for (int i = 0; i < nCats; ++i) labels.push_back((*s.recipeCategories)[static_cast<size_t>(i)]);
    labels.push_back("Mod buildables");
    if (g_tab > nCats + 1) g_tab = 0;
    h.subTabs("##sbx_upg_tabs", labels, g_tab);
    ImGui::Dummy(ImVec2(0, U(2)));
    if (g_tab == nCats + 1) {
        if (!st || GetTickCount64() - st->tick > 3000) DimText("Reading the mod's buildables...");
        else ModTab(h, *st);
    } else if (g_tab == 0) {
        if (!st || GetTickCount64() - st->tick > 3000) DimText("Reading your upgrades...");
        else if (!st->inGame || st->upgrades.empty()) DimText(st->why.empty() ? "No upgrades found." : st->why);
        else UpgradesTab(h, *st);
    } else if (!s.craftWhy.empty()) {
        DimText("Blueprints unavailable: " + s.craftWhy);
    } else if (st && !st->blueprintWhy.empty()) {
        DimText("Blueprints unavailable: " + st->blueprintWhy);
    } else {
        BlueprintTab(h, s, g_tab - 1, (*s.recipeCategories)[static_cast<size_t>(g_tab - 1)]);
    }
    h.endCard();
}

bool ShowingUpgrades() { return g_tab == 0; }

bool KeyMenuItem(const char* label, const char* keys, const char* after, bool enabled, float scale) {
    if (!keys) return ImGui::MenuItem(label, nullptr, false, enabled);
    const Palette& P = theme::Colors();
    const float cap = scale * 0.8f, gap = 28.0f * scale;
    const float afterW = after ? 4.0f * scale + ImGui::CalcTextSize(after).x : 0.0f;
    const float keysW = hud_edit::KeyComboWidth(keys, cap) + afterW;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::PushID(label);
    const bool r = ImGui::Selectable("##item", false,
                                     ImGuiSelectableFlags_SpanAvailWidth | (enabled ? 0 : ImGuiSelectableFlags_Disabled),
                                     ImVec2(ImGui::CalcTextSize(label).x + gap + keysW, 0));
    ImGui::PopID();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float lh = ImGui::GetTextLineHeight(), cy = std::round(p.y + lh * 0.5f);
    const float alpha = enabled ? 1.0f : 0.45f;
    dl->AddText(p, ImGui::GetColorU32(ImGuiCol_Text, alpha), label);
    const float right = ImGui::GetItemRectMax().x - ImGui::GetStyle().ItemSpacing.x * 0.5f;
    const float kx = right - keysW;
    hud_edit::DrawKeyCombo(dl, kx, cy, keys, cap, alpha);
    if (after) {
        const ImU32 dim = P.dim;
        dl->AddText(ImVec2(right - afterW + 4.0f * scale, p.y), (dim & ~IM_COL32_A_MASK) |
                    (static_cast<ImU32>(static_cast<float>((dim >> IM_COL32_A_SHIFT) & 0xFF) * alpha) << IM_COL32_A_SHIFT), after);
    }
    return r;
}

}  // namespace sr2::ui::sandbox_upgrades

#endif  // SR2_HAVE_IMGUI
