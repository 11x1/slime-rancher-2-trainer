// Built only with Dear ImGui (third_party/imgui), like overlay.cpp.
#if defined(SR2_HAVE_IMGUI)

#include "spotlight.h"

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <set>
#include <string_view>

#include "core/log.h"
#include "hud_edit.h"
#include "imgui_internal.h"  // GetTopMostPopupModal
#include "item_grid.h"  // SearchBox
#include "theme.h"

namespace sr2::ui::spotlight {
namespace {

using layout::Category;
using layout::Tab;
using layout::TabKind;
using theme::Palette;

constexpr int kMaxResults = 8;
constexpr const char* kPopupId = "##sr2_spotlight";
// Sizes at 1080p; U() applies the scale.
constexpr float kBoxW = 580, kSearchH = 58, kRowH = 50, kFooterH = 40, kPad = 8, kTop = 84;
constexpr const char* kSep = " \xE2\x80\xBA ";  // " > " (U+203A, in the embedded fonts' subset)

float g_scale = 1.0f;
float U(float v) { return v * g_scale; }

ImU32 WithAlpha(ImU32 c, float a) {
    const unsigned ca = (c >> IM_COL32_A_SHIFT) & 0xFF;
    return (c & ~IM_COL32_A_MASK) | (static_cast<ImU32>(static_cast<float>(ca) * a + 0.5f) << IM_COL32_A_SHIFT);
}

// One searchable thing: a setting, or a page (a tab, with its switch if it has one).
struct Entry {
    std::string title, crumb, help;     // help (tooltip, enum options, keywords) is only searched
    std::string lTitle, lCrumb, lHelp;  // lower case
    layout::Icon icon = layout::Icon::Misc;
    std::string cat;
    int tab = 0;
    bool page = false;
    bool hasDef = false;
    settings::Def def;     // the setting; on a page its tab switch
    bool moveRow = false;  // HUD position and size: the page's Move button
    int order = 0;
};

struct State {
    bool open = false;
    bool opened = false;       // first frame: open the popup, focus the box
    bool inputActive = false;  // the box had keyboard focus last frame
    char query[96] = {};
    std::string lastQuery;
    int sel = 0;
    double openedAt = 0;
    std::vector<Entry> index;
    std::vector<int> results;  // into index, best first
    int settingsCount = 0, pageCount = 0;
} g;

std::string Lower(std::string_view s) {
    std::string out(s);
    for (char& c : out)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return out;
}

std::string Join(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    if (b.empty()) return a;
    return a + kSep + b;
}

const ModuleView* FindView(const std::vector<ModuleView>& views, std::string_view m) {
    for (const auto& v : views)
        if (v.module == m) return &v;
    return nullptr;
}

// Extra words for the pages that draw their own content (searched, never shown).
const char* PageKeywords(const Tab& t) {
    switch (t.kind) {
        case TabKind::Sandbox:
            switch (t.sandboxPage) {
                case layout::SandboxPage::General:
                    return "cheats newbucks money health energy radiation infinite player teleport waypoint marker ranch "
                           "expansions achievements noclip fly walls";
                case layout::SandboxPage::Vacpack: return "cheats vacpack slots inventory items fill empty give put";
                case layout::SandboxPage::SpawnCraft:
                    return "cheats spawn spawner items slimes largos food crates toys crafter refinery fabricator craft "
                           "gadgets materials";
                case layout::SandboxPage::Upgrades:
                    return "cheats upgrades blueprints unlock discover max fabricator extra tank mod buildables";
                case layout::SandboxPage::Weather:
                    return "cheats weather rain storm forecast zone lock clear skies natural time day night";
            }
            return "";
        case TabKind::SlimeDj: return "music songs queue playlist play pause skip music box";
        case TabKind::Profiles: return "profiles presets save load rename delete folder";
        case TabKind::Settings: return "overlay hotkey hint theme dark light discord rich presence status game build";
        default: return "";
    }
}

// ---------------------------------------------------------------- index ----
struct Builder {
    std::vector<Entry>& out;
    std::set<std::string> seen;  // module.key already listed (the first place wins)

    void Add(Entry e) {
        if (e.hasDef && !seen.insert(e.def.module + "." + e.def.key).second && !e.page) return;
        e.lTitle = Lower(e.title);
        e.lCrumb = Lower(e.crumb);
        e.lHelp = Lower(e.help);
        e.order = static_cast<int>(out.size());
        out.push_back(std::move(e));
    }

    // A module's switch as an entry of its own (cards on Ranch / Misc > General, nested modules, Discord).
    void AddSwitch(const ModuleView& mv, const Entry& base, const std::string& crumb, const std::string& help) {
        if (!mv.hasEnabled) return;
        Entry e = base;
        e.title = mv.title;
        e.crumb = crumb;
        e.help = (help.empty() ? mv.enabled.tooltip : help) + " on off enable";
        e.hasDef = true;
        e.def = mv.enabled;
        Add(std::move(e));
    }

    // The settings a page shows of `mv` (filter: the tab's group filter; skipKey: drawn in the tab bar).
    void AddDefs(const ModuleView& mv, const Tab* filter, const std::string& skipKey, const Entry& base,
                 const std::string& crumb, bool groupInCrumb) {
        for (const auto& grp : mv.groups) {
            if (filter && !filter->onlyGroups.empty() &&
                std::find(filter->onlyGroups.begin(), filter->onlyGroups.end(), grp.name) == filter->onlyGroups.end())
                continue;
            if (filter && std::find(filter->skipGroups.begin(), filter->skipGroups.end(), grp.name) != filter->skipGroups.end())
                continue;
            for (const auto& d : grp.defs) {
                if (d.key == skipKey || hud_edit::Hidden(mv.module.c_str(), d.key.c_str())) continue;
                Entry e = base;
                e.hasDef = true;
                e.def = d;
                if (hud_edit::Element el; hud_edit::MoveRow(mv.module.c_str(), d.key.c_str(), el)) {
                    e.title = "Position and size";
                    e.moveRow = true;
                    e.help = std::string("move resize drag place ") + hud_edit::Name(el);
                } else {
                    e.title = d.label;
                    e.help = d.tooltip;
                    for (const auto& o : d.options) e.help += " " + o;
                }
                e.crumb = groupInCrumb && !grp.name.empty() ? Join(crumb, grp.name) : crumb;
                Add(std::move(e));
            }
        }
    }
};

void BuildIndex(const std::vector<Category>& cats, const std::vector<ModuleView>& views) {
    g.index.clear();
    Builder b{g.index, {}};
    for (const auto& c : cats) {
        for (int i = 0; i < static_cast<int>(c.tabs.size()); ++i) {
            const Tab& tab = c.tabs[static_cast<size_t>(i)];
            Entry base;
            base.cat = c.id;
            base.tab = i;
            base.icon = c.icon;
            const bool oneTab = c.tabs.size() == 1;
            const std::string crumb = tab.title == c.title ? c.title : Join(c.title, tab.title);
            const ModuleView* mv = tab.module.empty() ? nullptr : FindView(views, tab.module);

            // The page itself ("General" tabs go by their category's name).
            Entry p = base;
            p.page = true;
            p.title = tab.title == "General" ? c.title : tab.title;
            p.crumb = tab.title == "General" ? (oneTab ? std::string() : crumb) : (p.title == c.title ? std::string() : c.title);
            p.crumb = Join("Page", p.crumb);
            p.help = tab.help + " " + PageKeywords(tab);
            if (mv && (tab.kind == TabKind::Module || tab.kind == TabKind::Sandbox || tab.kind == TabKind::SlimeDj)) {
                const settings::Def* sw = !tab.masterKey.empty() ? FindDef(*mv, tab.masterKey) : mv->hasEnabled ? &mv->enabled : nullptr;
                if (sw) {
                    p.hasDef = true;
                    p.def = *sw;
                    p.help += " " + sw->label + " " + sw->tooltip;
                }
            }
            b.Add(std::move(p));

            switch (tab.kind) {
                case TabKind::Module:
                    if (!mv) break;
                    b.AddDefs(*mv, &tab, tab.masterKey, base, crumb, tab.onlyGroups.size() != 1);
                    for (const auto& n : tab.nested)
                        if (const ModuleView* nmv = FindView(views, n)) {
                            b.AddSwitch(*nmv, base, crumb, {});
                            b.AddDefs(*nmv, nullptr, {}, base, Join(crumb, nmv->title), true);
                        }
                    break;
                case TabKind::Cards:
                    for (const auto& col : tab.columns)
                        for (const auto& m : col)
                            if (const ModuleView* cmv = FindView(views, m)) {
                                const auto help = tab.cardHelp.find(m);
                                b.AddSwitch(*cmv, base, crumb, help != tab.cardHelp.end() ? help->second : std::string());
                                b.AddDefs(*cmv, nullptr, {}, base, Join(crumb, cmv->title), cmv->groups.size() > 1);
                                // Modules drawn inside this card (Tab::sections): their switch and rows.
                                const auto secs = tab.sections.find(m);
                                if (secs == tab.sections.end()) continue;
                                for (const auto& sec : secs->second)
                                    for (const auto& sm : sec.modules)
                                        if (const ModuleView* smv = FindView(views, sm)) {
                                            const std::string sc = Join(Join(crumb, cmv->title), sec.title);
                                            const auto sh = tab.cardHelp.find(sm);
                                            b.AddSwitch(*smv, base, sc, sh != tab.cardHelp.end() ? sh->second : std::string());
                                            b.AddDefs(*smv, nullptr, {}, base, sc, false);
                                        }
                            }
                    break;
                case TabKind::Settings:
                    if (const ModuleView* ui = FindView(views, kUiModule)) b.AddDefs(*ui, nullptr, {}, base, Join(crumb, "Overlay"), false);
                    if (const ModuleView* dmv = FindView(views, layout::kDiscordModule)) {
                        b.AddSwitch(*dmv, base, crumb, {});
                        b.AddDefs(*dmv, nullptr, {}, base, Join(crumb, dmv->title), false);
                    }
                    break;
                case TabKind::Sandbox:
                case TabKind::SlimeDj:
                    if (mv) b.AddDefs(*mv, nullptr, {}, base, crumb, true);
                    break;
                case TabKind::Profiles: break;
            }
        }
    }
    g.pageCount = static_cast<int>(std::count_if(g.index.begin(), g.index.end(), [](const Entry& e) { return e.page; }));
    g.settingsCount = static_cast<int>(g.index.size()) - g.pageCount;
}

// --------------------------------------------------------------- search ----
bool IsWordChar(char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0; }

// 3 prefix, 2 word start, 1 anywhere, 0 not found.
int Hit(std::string_view hay, std::string_view tok) {
    int best = 0;
    for (size_t pos = hay.find(tok); pos != std::string_view::npos; pos = hay.find(tok, pos + 1)) {
        if (pos == 0) return 3;
        best = std::max(best, IsWordChar(hay[pos - 1]) ? 1 : 2);
    }
    return best;
}

// The token's letters in order, the first one at a word start ("plupg" -> "Plot Upgrades").
bool Fuzzy(std::string_view hay, std::string_view tok) {
    if (tok.size() < 3) return false;
    size_t k = 0;
    for (size_t i = 0; i < hay.size() && k < tok.size(); ++i) {
        if (hay[i] != tok[k]) continue;
        if (k == 0 && i > 0 && IsWordChar(hay[i - 1])) continue;
        ++k;
    }
    return k == tok.size();
}

int Score(const Entry& e, const std::vector<std::string>& toks, const std::string& whole) {
    static constexpr int kTitle[] = {0, 40, 70, 100};
    static constexpr int kCrumb[] = {0, 15, 30, 30};
    static constexpr int kHelp[] = {0, 6, 10, 10};
    int total = 0;
    for (const auto& t : toks) {
        int s = std::max({kTitle[Hit(e.lTitle, t)], kCrumb[Hit(e.lCrumb, t)], kHelp[Hit(e.lHelp, t)]});
        if (s == 0 && Fuzzy(e.lTitle, t)) s = 12;
        if (s == 0) return 0;  // every word has to match somewhere
        total += s;
    }
    if (e.lTitle == whole) total += 40;
    else if (toks.size() > 1 && e.lTitle.starts_with(whole)) total += 50;
    if (e.page) total += 5;
    return total;
}

void Search() {
    g.results.clear();
    g.sel = 0;
    const std::string whole = Lower(g.query);
    std::vector<std::string> toks;
    for (size_t i = 0; i < whole.size();) {
        const size_t j = std::min(whole.find(' ', i), whole.size());
        if (j > i) toks.push_back(whole.substr(i, j - i));
        i = j + 1;
    }
    if (toks.empty()) return;
    std::string trimmed;
    for (const auto& t : toks) trimmed += (trimmed.empty() ? "" : " ") + t;
    std::vector<std::pair<int, int>> scored;  // score, index
    for (int i = 0; i < static_cast<int>(g.index.size()); ++i)
        if (const int s = Score(g.index[static_cast<size_t>(i)], toks, trimmed)) scored.emplace_back(s, i);
    std::sort(scored.begin(), scored.end(), [](const auto& x, const auto& y) {
        if (x.first != y.first) return x.first > y.first;
        const Entry &a = g.index[static_cast<size_t>(x.second)], &b = g.index[static_cast<size_t>(y.second)];
        if (a.title.size() != b.title.size()) return a.title.size() < b.title.size();
        return a.order < b.order;
    });
    for (size_t i = 0; i < scored.size() && i < kMaxResults; ++i) g.results.push_back(scored[i].second);
}

// ---------------------------------------------------------------- actions ----
bool IsBool(const Entry& e) { return e.hasDef && e.def.kind == settings::Kind::Bool; }

// A Bool can be switched here when the page would let you: not pinned by its module, its parent
// chain shows it and its module is on (a module's own switch always), and not Sandbox's switch,
// which needs its confirmation dialog.
bool CanToggle(const Entry& e, const std::vector<ModuleView>& views) {
    if (!IsBool(e) || e.moveRow) return false;
    if (e.def.module == "sandbox" && e.def.key == kKeyEnabled) return false;
    if (!settings::LockReason(e.def.module, e.def.key).empty()) return false;
    const ModuleView* mv = FindView(views, e.def.module);
    return mv && Visible(*mv, e.def);
}

void Toggle(const Entry& e, const std::vector<ModuleView>& views) {
    if (!CanToggle(e, views)) return;
    const bool on = Read(e.def) >= 0.5;
    Write(e.def, on ? 0.0 : 1.0);
    SR2_INFO("ui: search: {}.{} = {}", e.def.module, e.def.key, on ? "off" : "on");
}

// Where a result leads. A setting hidden behind its parent (a nested option while its switch is
// off) pulses the nearest shown ancestor instead.
Target MakeTarget(const Entry& e, const std::vector<ModuleView>& views) {
    Target t;
    t.cat = e.cat;
    t.tab = e.tab;
    if (e.page || !e.hasDef) return t;
    t.module = e.def.module;
    t.key = e.def.key;
    if (const ModuleView* mv = FindView(views, e.def.module)) {
        const settings::Def* cur = &e.def;
        for (int depth = 0; depth < 8 && !ParentChainAllows(*mv, *cur); ++depth) {
            const std::string& up = !cur->parent.empty() ? cur->parent : !cur->anyOf.empty() ? cur->anyOf.front() : std::string();
            const settings::Def* p = up.empty() ? nullptr : FindDef(*mv, up);
            if (!p) break;
            cur = p;
        }
        t.key = cur->key;
    }
    return t;
}

void Open(const std::vector<Category>& cats, const std::vector<ModuleView>& views) {
    g.open = true;
    g.opened = true;
    g.inputActive = false;
    g.query[0] = 0;
    g.lastQuery.clear();
    g.openedAt = ImGui::GetTime();
    BuildIndex(cats, views);
    Search();
}

// ---------------------------------------------------------------- drawing ----
void Magnifier(ImDrawList* dl, ImVec2 c, ImU32 col) {
    const float r = U(7), th = U(2.2f);
    const ImVec2 o(c.x - U(2), c.y - U(2));
    dl->AddCircle(o, r, col, 32, th);
    const float k = 0.7071f;
    dl->AddLine(ImVec2(o.x + r * k + U(1), o.y + r * k + U(1)), ImVec2(o.x + r * k + U(6), o.y + r * k + U(6)), col, U(2.6f));
}

// A footer keycap with an arrow drawn on it (the keycap font is ASCII only).
float ArrowKeycap(ImDrawList* dl, float x, float cy, bool up, float capScale, ImU32 col) {
    const float w = hud_edit::DrawKeycap(dl, x, cy, "  ", capScale);
    const float cx = std::round(x + w * 0.5f), my = std::round(cy - capScale), s = U(3.6f);
    if (up) dl->AddTriangleFilled(ImVec2(cx, my - s), ImVec2(cx + s, my + s * 0.6f), ImVec2(cx - s, my + s * 0.6f), col);
    else dl->AddTriangleFilled(ImVec2(cx - s, my - s * 0.6f), ImVec2(cx + s, my - s * 0.6f), ImVec2(cx, my + s), col);
    return w;
}

// "Up Down select   Enter open   Tab switch   Esc close", keycaps as in the HUD edit help bar
// and the Sandbox key hints.
void Footer(ImDrawList* dl, ImVec2 a, float w, bool canToggle) {
    const Palette& P = theme::Colors();
    ImFont* f = theme::Body();
    const float fs = U(theme::kSmallSize * 0.75f + 4.0f);
    const float capScale = g_scale * 0.8f;
    const float cy = std::round(a.y + U(kFooterH) * 0.5f);
    const float ty = std::round(cy - f->CalcTextSizeA(fs, FLT_MAX, 0, "Ag").y * 0.5f);
    float x = a.x + U(16);
    auto label = [&](const char* text) {
        dl->AddText(f, fs, ImVec2(x + U(5), ty), P.soft, text);
        x += U(5) + f->CalcTextSizeA(fs, FLT_MAX, 0, text).x + U(16);
    };
    x += ArrowKeycap(dl, x, cy, true, capScale, P.text) + U(3);
    x += ArrowKeycap(dl, x, cy, false, capScale, P.text);
    label("select");
    x += hud_edit::DrawKeycap(dl, x, cy, "Enter", capScale);
    label("open");
    if (canToggle) {
        x += hud_edit::DrawKeycap(dl, x, cy, "Tab", capScale);
        label("switch");
    }
    if (x + hud_edit::KeycapWidth("Esc", capScale) + U(60) <= a.x + w) {
        x += hud_edit::DrawKeycap(dl, x, cy, "Esc", capScale);
        label("close");
    }
}

void ClippedText(ImDrawList* dl, ImFont* f, float size, ImVec2 pos, float maxX, ImU32 col, const std::string& s) {
    const ImVec4 clip(pos.x, pos.y - U(4), maxX, pos.y + size + U(6));
    dl->AddText(f, size, pos, col, s.c_str(), nullptr, 0.0f, &clip);
}

}  // namespace

bool IsOpen() { return g.open; }

void Close() { g.open = false; }

bool Draw(const Helpers& h, const std::vector<Category>& cats, const std::vector<ModuleView>& views, Target& out) {
    g_scale = h.scale;
    const ImGuiIO& io = ImGui::GetIO();
    // Ctrl+Space / Alt+Space opens and closes (the overlay's WndProc keeps Alt+Space from the window menu).
    // Not over another dialog (Sandbox's "Enable sandbox?" and the profile dialogs).
    if (ImGui::IsKeyPressed(ImGuiKey_Space, false) && (io.KeyCtrl || io.KeyAlt)) {
        if (g.open) g.open = false;
        else if (!ImGui::GetTopMostPopupModal()) Open(cats, views);
    }
    if (!g.open && !ImGui::IsPopupOpen(kPopupId)) return false;
    if (g.open && g.opened) ImGui::OpenPopup(kPopupId);

    const Palette& P = theme::Colors();
    const float winW = h.winMax.x - h.winMin.x;
    const float W = std::floor(std::min(U(kBoxW), winW - U(60)));
    const ImVec2 pos(std::floor(h.winMin.x + (winW - W) * 0.5f), std::floor(h.winMin.y + U(kTop)));
    auto boxHeight = [&] {
        const float rows = g.results.empty() ? U(kRowH) : static_cast<float>(g.results.size()) * U(kRowH);
        return std::floor(U(kSearchH) + U(kPad) * 2 + rows + U(kFooterH));
    };
    ImGui::SetNextWindowPos(pos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(W, boxHeight()), ImGuiCond_Always);
    ImGui::PushStyleColor(ImGuiCol_ModalWindowDimBg, IM_COL32(0, 0, 0, 0));  // dimmed below, the window only
    ImGui::PushStyleColor(ImGuiCol_PopupBg, IM_COL32(0, 0, 0, 0));           // drawn below, with its shadow
    ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
    const ImGuiWindowFlags wf = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar |
                                ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoNavInputs;
    const bool visible = ImGui::BeginPopupModal(kPopupId, nullptr, wf);
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(2);
    if (!visible) {
        g.open = false;
        return false;
    }
    if (!g.open) {
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return false;
    }

    bool chose = false;
    int chosen = -1;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->ChannelsSplit(2);  // 0: backdrop and box (drawn last, once the height is known), 1: content
    dl->ChannelsSetCurrent(1);
    const ImVec2 a = ImGui::GetWindowPos();
    const float r = U(16);

    // Search box.
    const float sy = a.y + U(kSearchH) * 0.5f;
    Magnifier(dl, ImVec2(a.x + U(28), sy), g.inputActive ? P.text : P.soft);
    {
        ImGui::PushFont(theme::Body(), 19.0f);
        ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, IM_COL32(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_FrameBgActive, IM_COL32(0, 0, 0, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(U(4), U(6)));
        ImGui::SetCursorScreenPos(ImVec2(a.x + U(48), std::round(sy - ImGui::GetFrameHeight() * 0.5f)));
        // Focus on open, and again after a click elsewhere in the box (a row's switch) once it is released.
        if (g.opened || (!g.inputActive && !ImGui::IsAnyItemActive())) ImGui::SetKeyboardFocusHere();
        item_grid::SearchBox("##query", "Search settings and pages", g.query, sizeof(g.query), W - U(48) - U(18), g_scale,
                             &g.inputActive);
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(3);
        ImGui::PopFont();
    }
    g.opened = false;
    if (g.lastQuery != g.query) {
        g.lastQuery = g.query;
        Search();
    }

    // Keys (read without ownership: the box keeps the keyboard focus).
    const int n = static_cast<int>(g.results.size());
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) g.open = false;
    if (n > 0) {
        g.sel = std::clamp(g.sel, 0, n - 1);
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) g.sel = (g.sel + 1) % n;
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) g.sel = (g.sel + n - 1) % n;
        if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)) chosen = g.sel;
        if (ImGui::IsKeyPressed(ImGuiKey_Tab, false)) Toggle(g.index[static_cast<size_t>(g.results[static_cast<size_t>(g.sel)])], views);
    }

    // Results.
    float y = a.y + U(kSearchH);
    dl->AddLine(ImVec2(a.x + U(1), y), ImVec2(a.x + W - U(1), y), P.popBorder, std::max(1.0f, U(1)));
    y += U(kPad);
    ImFont* body = theme::Body();
    const bool mouseMoved = io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f;
    for (int i = 0; i < n; ++i) {
        const Entry& e = g.index[static_cast<size_t>(g.results[static_cast<size_t>(i)])];
        const ImVec2 ra(a.x + U(kPad), y), rb(a.x + W - U(kPad), y + U(kRowH));
        const float cy = (ra.y + rb.y) * 0.5f;
        ImGui::PushID(i);
        ImGui::SetCursorScreenPos(ra);
        ImGui::SetNextItemAllowOverlap();  // the switch on top keeps its own click
        if (ImGui::InvisibleButton("##row", ImVec2(rb.x - ra.x, rb.y - ra.y))) chosen = i;
        if (ImGui::IsItemHovered() && mouseMoved) g.sel = i;
        const bool sel = i == g.sel;
        if (sel) {
            dl->AddRectFilled(ra, rb, P.optHover, U(10));
            dl->AddRectFilled(ImVec2(ra.x, ra.y + U(12)), ImVec2(ra.x + U(3), rb.y - U(12)),
                              theme::Current() == theme::Mode::Dark ? P.grab : P.accent, U(2));
        }
        // Category icon on a tile.
        const ImVec2 ia(ra.x + U(10), std::round(cy - U(16)));
        dl->AddRectFilled(ia, ImVec2(ia.x + U(32), ia.y + U(32)), sel ? P.frameHover : P.frame, U(9));
        if (h.icon) h.icon(dl, e.icon, ImVec2(ia.x + U(7), ia.y + U(7)), sel ? P.text : P.soft);

        // Right side: a switch for Bools, else the value.
        float right = rb.x - U(12);
        if (IsBool(e) && !e.moveRow) {
            const bool can = CanToggle(e, views);
            right -= U(42);
            ImGui::SetCursorScreenPos(ImVec2(right, std::round(cy - ImGui::GetFrameHeight() * 0.5f)));
            ImGui::BeginDisabled(!can);
            if (h.toggle && h.toggle("##switch", Read(e.def) >= 0.5)) Toggle(e, views);
            ImGui::EndDisabled();
            right -= U(12);
        } else if (e.hasDef || e.moveRow) {
            const std::string v = e.moveRow ? std::string("Move") : FormatValue(e.def, Read(e.def));
            const float fs = U(theme::kSmallSize);
            const float vw = std::min(body->CalcTextSizeA(fs, FLT_MAX, 0, v.c_str()).x, U(150));
            ClippedText(dl, body, fs, ImVec2(right - vw, std::round(cy - fs * 0.6f)), right, P.soft, v);
            right -= vw + U(12);
        }
        const float tx = ra.x + U(54);
        ClippedText(dl, body, U(theme::kBodySize), ImVec2(tx, ra.y + U(7)), right, P.text, e.title);
        ClippedText(dl, body, U(theme::kSmallSize), ImVec2(tx, ra.y + U(27)), right, sel ? P.soft : P.dim, e.crumb);
        ImGui::PopID();
        y += U(kRowH);
    }
    if (n == 0) {
        const std::string msg = g.query[0] ? std::string("No matches for \"") + g.query + "\""
                                           : std::to_string(g.settingsCount) + " settings and " + std::to_string(g.pageCount) +
                                                 " pages. Type to search.";
        const float fs = U(theme::kBodySize);
        const ImVec2 ts = body->CalcTextSizeA(fs, FLT_MAX, 0, msg.c_str());
        ClippedText(dl, body, fs, ImVec2(std::round(a.x + (W - ts.x) * 0.5f), std::round(y + (U(kRowH) - ts.y) * 0.5f)),
                    a.x + W - U(kPad), P.dim, msg);
        y += U(kRowH);
    }
    y += U(kPad);

    // Footer.
    dl->AddLine(ImVec2(a.x + U(1), y), ImVec2(a.x + W - U(1), y), P.popBorder, std::max(1.0f, U(1)));
    const bool selToggle = n > 0 && CanToggle(g.index[static_cast<size_t>(g.results[static_cast<size_t>(std::clamp(g.sel, 0, n - 1))])], views);
    Footer(dl, ImVec2(a.x, y), W, selToggle);
    const ImVec2 b(a.x + W, y + U(kFooterH));

    // Backdrop and box, under the content.
    dl->ChannelsSetCurrent(0);
    dl->PushClipRectFullScreen();
    const float fade = std::clamp(static_cast<float>((ImGui::GetTime() - g.openedAt) / 0.12), 0.0f, 1.0f);
    dl->AddRectFilled(h.winMin, h.winMax, WithAlpha(P.scrim, fade), ImGui::GetStyle().WindowRounding);
    if (h.shadow) h.shadow(dl, a, b, r, U(2), U(10), P.shadow);
    dl->AddRectFilled(a, b, P.popBg, r);
    dl->AddRect(a, b, P.popBorder, r, 0, std::max(1.0f, U(1)));
    dl->PopClipRect();
    dl->ChannelsMerge();

    // A click outside the box closes it.
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem))
        g.open = false;

    if (chosen >= 0 && chosen < n && g.open) {
        const Entry& e = g.index[static_cast<size_t>(g.results[static_cast<size_t>(chosen)])];
        out = MakeTarget(e, views);
        SR2_INFO("ui: search \"{}\" -> {} ({})", g.query, e.title, e.crumb);
        g.open = false;
        chose = true;
    }
    if (!g.open) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
    return chose;
}

}  // namespace sr2::ui::spotlight

#endif  // SR2_HAVE_IMGUI
