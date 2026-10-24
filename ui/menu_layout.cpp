#include "menu_layout.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <set>

namespace sr2::ui::layout {
namespace {

Tab ModuleTab(std::string module, std::string title = {}) {
    Tab t;
    t.kind = TabKind::Module;
    t.module = std::move(module);
    t.title = std::move(title);
    return t;
}

Category Cat(std::string id, std::string title, Icon icon, bool bottom = false) {
    Category c;
    c.id = std::move(id);
    c.title = std::move(title);
    c.icon = icon;
    c.bottom = bottom;
    return c;
}

// The sidebar, top to bottom. Tabs keep this order.
std::vector<Category> Spec() {
    std::vector<Category> cats;

    Category ranch = Cat("ranch", "Ranch", Icon::Ranch);
    Tab ranchGeneral;
    ranchGeneral.title = "General";
    ranchGeneral.kind = TabKind::Cards;
    ranchGeneral.columns = {{"custom_upgrades"}, {"plot_upgrades", "sell_upgrades"}};
    ranchGeneral.cardHelp = {
        {"custom_upgrades",
         "Adds extra plot upgrades and Fabricator upgrades to the game's menus. Stays on while a save that uses "
         "them is loaded."},
        {"plot_upgrades", "Applies the effects of the upgrade tiers bought through Custom Upgrades."},
        {"sell_upgrades",
         "Sell an owned plot upgrade back for half its price, from the plot's purchase menu. Silo storage "
         "upgrades can't be sold."},
    };
    ranch.tabs.push_back(ranchGeneral);
    Tab dj;
    dj.title = "Slime DJ";
    dj.kind = TabKind::SlimeDj;
    dj.module = "slime_dj";
    dj.help = "Plays your own songs from the corral Music Box. Put .ogg, .mp3 or .wav files in the sr2mods_music "
              "folder in the game folder, then Rescan.";
    ranch.tabs.push_back(dj);
    cats.push_back(ranch);

    Category movement = Cat("movement", "Movement", Icon::Movement);
    Tab hl2 = ModuleTab("hl2_movement");
    hl2.skipGroups = {"HUD"};  // on HUD > Speed Panel
    movement.tabs.push_back(hl2);
    cats.push_back(movement);

    Category hud = Cat("hud", "HUD", Icon::Hud);
    Tab minimap = ModuleTab("minimap");
    hud.tabs.push_back(minimap);
    Tab collectibles = ModuleTab("collectibles", "Collectibles");
    collectibles.help = "Shown under the minimap. Needs the Minimap on.";
    hud.tabs.push_back(collectibles);
    Tab mapTools = ModuleTab("map_tools", "Full Map");
    mapTools.help = "Extras for the game's full-screen map. The extra icons need the Minimap on; reveal and "
                    "teleport work without it.";
    hud.tabs.push_back(mapTools);
    Tab speed = ModuleTab("hl2_movement", "Speed Panel");
    speed.onlyGroups = {"HUD"};
    speed.masterKey = "momentum_hud";
    speed.help = "Part of Hl2 Movement, so it needs Hl2 Movement on.";
    hud.tabs.push_back(speed);
    cats.push_back(hud);

    Category world = Cat("world", "World", Icon::World);
    world.tabs.push_back(ModuleTab("spawn_scaling"));
    world.tabs.push_back(ModuleTab("wall_markers"));
    cats.push_back(world);

    Category misc = Cat("misc", "Misc", Icon::Misc);
    Tab general;
    general.title = "General";
    general.kind = TabKind::Cards;
    general.columns = {{"quick_start", "free_placement"}, {"instant_build", "quick_travel", "misc_tweaks"}};
    general.cardHelp = {
        {"free_placement",
         "Gadget mode ignores its placement limits: overlap, slope, distance, jumping. The cost and stacking rules "
         "stay, and placed gadgets are saved like any other."},
        {"misc_tweaks",
         "Small speed-ups, each off until you turn it on: instant Fabricator purchases and a faster vacpack when "
         "shooting items out."},
    };
    misc.tabs.push_back(general);
    Tab sandbox;
    sandbox.title = "Sandbox";
    sandbox.kind = TabKind::Sandbox;
    sandbox.module = "sandbox";
    misc.tabs.push_back(sandbox);
    cats.push_back(misc);

    Category profiles = Cat("profiles", "Profiles", Icon::Profiles, true);
    Tab pt;
    pt.title = "Profiles";
    pt.kind = TabKind::Profiles;
    profiles.tabs.push_back(pt);
    cats.push_back(profiles);

    Category settingsCat = Cat("settings", "Settings", Icon::Settings, true);
    Tab st;
    st.title = "General";
    st.kind = TabKind::Settings;
    settingsCat.tabs.push_back(st);
    cats.push_back(settingsCat);
    return cats;
}

bool HasGroup(const ModuleView& mv, std::string_view group) {
    return std::any_of(mv.groups.begin(), mv.groups.end(), [&](const GroupView& g) { return g.name == group; });
}

bool StartsWith(std::string_view s, std::string_view p) { return s.size() >= p.size() && s.substr(0, p.size()) == p; }

std::string Capitalize(std::string s) {
    if (!s.empty() && s[0] >= 'a' && s[0] <= 'z') s[0] = static_cast<char>(s[0] - 'a' + 'A');
    return s;
}

// "prefix: rest" or "prefix rest" -> "Rest" (empty if `label` does not start with it).
std::string StripPrefix(std::string_view label, std::string_view prefix) {
    if (prefix.empty() || !StartsWith(label, prefix)) return {};
    std::string_view rest = label.substr(prefix.size());
    if (StartsWith(rest, ": ")) rest.remove_prefix(2);
    else if (StartsWith(rest, " ")) rest.remove_prefix(1);
    else return {};
    if (rest.empty() || rest.front() == '(') return {};
    return Capitalize(std::string(rest));
}

}  // namespace

std::vector<Category> Build(const std::vector<ModuleView>& views) {
    auto find = [&](std::string_view m) -> const ModuleView* {
        for (const auto& v : views)
            if (v.module == m) return &v;
        return nullptr;
    };

    std::vector<Category> spec = Spec();
    std::set<std::string> placed{kUiModule, kDiscordModule};  // discord_rpc: its own card on the Settings page
    for (auto& c : spec) {
        std::vector<Tab> keep;
        for (auto& t : c.tabs) {
            if (t.kind == TabKind::Module) {
                const ModuleView* mv = find(t.module);
                if (!mv) continue;
                if (!t.onlyGroups.empty() &&
                    std::none_of(t.onlyGroups.begin(), t.onlyGroups.end(), [&](const std::string& g) { return HasGroup(*mv, g); }))
                    continue;
                if (t.title.empty()) t.title = mv->title;
                std::erase_if(t.nested, [&](const std::string& n) { return !find(n); });
                placed.insert(t.module);
                placed.insert(t.nested.begin(), t.nested.end());
            } else if (t.kind == TabKind::Sandbox || t.kind == TabKind::SlimeDj) {
                if (!find(t.module)) continue;
                placed.insert(t.module);
            } else if (t.kind == TabKind::Cards) {
                for (auto& col : t.columns) {
                    std::erase_if(col, [&](const std::string& m) { return !find(m); });
                    placed.insert(col.begin(), col.end());
                }
            }
            keep.push_back(std::move(t));
        }
        c.tabs = std::move(keep);
    }

    // Modules the layout does not know yet: cards on Misc > General.
    std::vector<std::string> extra;
    for (const auto& v : views)
        if (!placed.count(v.module)) extra.push_back(v.module);
    if (!extra.empty()) {
        auto misc = std::find_if(spec.begin(), spec.end(), [](const Category& c) { return c.id == "misc"; });
        Tab* cards = nullptr;
        if (misc != spec.end())
            for (auto& t : misc->tabs)
                if (t.kind == TabKind::Cards) cards = &t;
        if (cards) {
            if (cards->columns.size() < 2) cards->columns.resize(2);
            for (size_t i = 0; i < extra.size(); ++i) cards->columns[i % 2 == 0 ? 1 : 0].push_back(extra[i]);
        }
    }

    for (auto& c : spec)
        for (auto& t : c.tabs)
            if (t.kind == TabKind::Cards)
                std::erase_if(t.columns, [](const std::vector<std::string>& col) { return col.empty(); });
    std::erase_if(spec, [](const Category& c) {
        if (c.bottom) return false;
        return std::none_of(c.tabs.begin(), c.tabs.end(),
                            [](const Tab& t) { return t.kind != TabKind::Cards || !t.columns.empty(); });
    });
    return spec;
}

std::vector<OptionText> EnumTexts(const settings::Def& d) {
    struct Entry {
        const char* module;
        const char* key;
        std::vector<OptionText> options;
    };
    static const std::vector<Entry> kTable = {
        {"hl2_movement", "gravity_mode",
         {{"HL2", "sv_gravity and a fixed jump height"}, {"Vanilla SR2", "The game's gravity and jump; movement stays HL2"}}},
        {"hl2_movement", "jump_boost_mode",
         {{"Uncapped", "Forward boost is never limited"},
          {"Vanilla HL2", "Source SDK 2013 clamp, with ABH"},
          {"Uncapped + ABH", "Uncapped forward, vanilla formula backwards"},
          {"Accelerated", "Forward hops gain speed like back hops"}}},
        {"minimap", "orientation", {{"North up", "The map stays fixed"}, {"Rotate", "The map turns with the player"}}},
        {"minimap", "arrow_style", {{"Game marker", "The game's own player icon"}, {"Classic", "A plain arrow"}}},
    };
    for (const auto& e : kTable)
        if (d.module == e.module && d.key == e.key && e.options.size() == d.options.size()) return e.options;

    std::vector<OptionText> out;
    for (const auto& o : d.options) {
        OptionText t{o, {}};
        const size_t open = o.find(" (");
        if (open != std::string::npos && o.size() > open + 3 && o.back() == ')') {
            t.label = o.substr(0, open);
            t.desc = Capitalize(o.substr(open + 2, o.size() - open - 3));
        }
        out.push_back(std::move(t));
    }
    return out;
}

std::string ShortLabel(const settings::Def& d, const settings::Def* parent, std::string_view group,
                       std::string_view masterLabel) {
    if (parent) {
        if (auto s = StripPrefix(d.label, parent->label); !s.empty()) return s;
        // "Resource nodes: advanced" under "Resource nodes: apply multiplier"
        const size_t a = d.label.find(": "), b = parent->label.find(": ");
        if (a != std::string::npos && a == b && d.label.compare(0, a, parent->label, 0, b) == 0)
            if (auto s = StripPrefix(d.label, d.label.substr(0, a)); !s.empty()) return s;
    }
    if (auto s = StripPrefix(d.label, masterLabel); !s.empty()) return s;
    if (!group.empty() && StartsWith(d.label, std::string(group) + ": "))
        if (auto s = StripPrefix(d.label, group); !s.empty()) return s;
    return d.label;
}

}  // namespace sr2::ui::layout
