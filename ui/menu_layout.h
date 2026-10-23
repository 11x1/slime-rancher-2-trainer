#pragma once
// How the overlay arranges the registered settings: sidebar categories, tabs,
// options nested under the switch they depend on, and short dropdown labels
// with descriptions. Presentation only: modules, keys and sr2mods.json are
// unchanged. Anything not listed still shows up: an unknown module lands on
// Misc > General, an unknown setting in its module's tab.

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "settings_view.h"

namespace sr2::ui::layout {

enum class Icon { Movement, World, Hud, Ranch, Misc, Profiles, Settings };

enum class TabKind {
    Module,    // one module's groups as cards (plus nested modules below)
    Cards,     // several small modules, one card each with its own switch
    Profiles,  // the profiles page
    Settings,  // the overlay's own settings (module "ui") and status
};

struct Tab {
    std::string title;
    TabKind kind = TabKind::Module;
    std::string module;                     // Module
    std::vector<std::string> onlyGroups;    // Module: only these groups ("" = ungrouped); empty = all
    std::vector<std::string> skipGroups;    // Module: groups shown on another tab
    std::vector<std::string> nested;        // Module: modules drawn below as sections with their own switch
    std::string masterKey;                  // Module: a Bool of `module` used as the tab's switch instead of "enabled";
                                            //   the tab also needs `module` enabled
    std::string help;                       // tooltip of the (?) next to the switch
    std::vector<std::vector<std::string>> columns;  // Cards: module ids per column
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

// The option a setting is nested under. value < 0: shown while the parent is
// on (non-zero); otherwise shown while the parent equals `value` (Enum index).
struct Parent {
    std::string key;
    int value = -1;
};
std::optional<Parent> ParentOf(std::string_view module, std::string_view key);
bool ParentAllows(const Parent& p, double parentValue);

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
