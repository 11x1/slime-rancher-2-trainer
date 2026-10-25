#pragma once
// How the overlay arranges the registered settings: sidebar categories, tabs,
// and short dropdown labels with descriptions. Which option is nested under
// which switch comes from the settings schema (settings::Def::parent). Presentation only: modules, keys and sr2mods.json are
// unchanged. Anything not listed still shows up: an unknown module lands on
// Misc > General, an unknown setting in its module's tab.

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "settings_view.h"

namespace sr2::ui::layout {

// Drawn as its own card on the Settings page (not a tab of its own).
inline constexpr const char* kDiscordModule = "discord_rpc";

enum class Icon { Movement, World, Hud, Ranch, Misc, Sandbox, Profiles, Settings };

enum class TabKind {
    Module,    // one module's groups as cards (plus nested modules below)
    Cards,     // several small modules, one card each with its own switch
    Profiles,  // the profiles page
    Settings,  // the overlay's own settings (module "ui"), status and Discord Rich Presence (module "discord_rpc")
    Sandbox,   // module "sandbox": one of its pages (SandboxPage), switch only after a confirmation dialog
    SlimeDj,   // module "slime_dj": its own page (song list, queue, now playing); refresh icon = rescan
};

// The Sandbox category's tabs, one page each.
enum class SandboxPage { General, Vacpack, SpawnCraft, Upgrades, Weather };

// Cards: a card of its own under one module's card (round 14; was a heading inside it), named by `title`.
// It holds some of the card module's own groups and other modules, each drawn as its switch (a row) and
// then its rows.
struct CardSection {
    std::string title;
    std::string note;                   // dim line under the heading ("" = none)
    std::vector<std::string> groups;    // the card module's groups shown here
    std::vector<std::string> modules;   // other modules shown here (not cards of their own)
    std::vector<std::string> needCard;  // of those, the ones grayed out while the card's module is off
                                        //   (the card module's own groups always are)
};

struct Tab {
    std::string title;
    TabKind kind = TabKind::Module;
    std::string module;                     // Module, Sandbox, SlimeDj; Cards: the tab's own module (also in
                                            //   columns), its switch in the tab bar, only its sections shown
    std::vector<std::string> onlyGroups;    // Module: only these groups ("" = ungrouped); empty = all
    std::vector<std::string> skipGroups;    // Module: groups shown on another tab
    std::vector<std::string> nested;        // Module: modules drawn below as sections with their own switch
    std::string masterKey;                  // Module: a Bool of `module` used as the tab's switch instead of "enabled";
                                            //   the tab also needs `module` enabled
    std::string help;                       // tooltip of the (?) next to the switch
    std::vector<std::vector<std::string>> columns;  // Cards: module ids per column
    bool stacked = false;                           // Cards: columns[0] full width on top, the other columns
                                                    //   side by side under it
    std::map<std::string, std::string> cardHelp;    // Cards: tooltip of a module's switch (default: its own)
    std::map<std::string, std::vector<CardSection>> sections;  // Cards: a card module's section cards (none:
                                                               //   its groups as one list of rows in its card)
    std::map<std::string, std::string> switchLabel;  // Cards: row label of a module's switch inside a section
                                                     //   (default: the module's title)
    SandboxPage sandboxPage = SandboxPage::General;  // Sandbox: which of its pages
};

struct Category {
    std::string id, title;
    Icon icon = Icon::Misc;
    bool bottom = false;  // pinned to the bottom of the sidebar (Profiles, Settings)
    std::vector<Tab> tabs;
};

// Categories for the modules registered now. Tabs whose module is missing are
// dropped, then empty categories; Profiles and Settings are always present.
std::vector<Category> Build(const std::vector<ModuleView>& views);

// Short label and one-line description per option of an Enum. Falls back to
// splitting "Label (description)" options; descriptions may be empty.
struct OptionText {
    std::string label, desc;
};
std::vector<OptionText> EnumTexts(const settings::Def& d);

// Label without the part the layout already shows: the parent's name for a
// nested option ("Autobhop hold key" under "Autobhop" -> "Hold key"), the card
// name ("Coop: hen laying speed" in "Coop" -> "Hen laying speed") or the tab
// switch's name ("Momentum HUD: details" -> "Details").
std::string ShortLabel(const settings::Def& d, const settings::Def* parent, std::string_view group,
                       std::string_view masterLabel);

}  // namespace sr2::ui::layout
