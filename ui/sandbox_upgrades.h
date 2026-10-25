#pragma once
// The Sandbox page's "Upgrades & Blueprints" card (modules/sandbox/upgrades.cpp, sandbox_upgrades_bridge.h):
// tabs Upgrades (the Fabricator's vacpack / player upgrades with their game icon and tier) and one per
// Fabricator gadget category (Utilities, Warp Tech, Decorations: blueprints locked / unlocked) and "Mod
// buildables" (custom_upgrades' own cards: follow the base blueprint / unlocked / locked per save).
// Render thread only; every change is a posted request. Kept out of menu.cpp, which only calls these and
// hands over its card / tooltip / popup / sub-tab helpers.

#include <string>
#include <utility>
#include <vector>

#include "imgui.h"
#include "sandbox_bridge.h"

namespace sr2::ui::sandbox_upgrades {

// A key hint: the keys as keycaps (one per '+'-joined part, "Ctrl+LMB") and what they do.
struct KeyRow {
    const char* keys;
    std::string text;
};

// A tooltip in the Sandbox page's item style: the name, a small coloured tag, coloured lines, then the
// key hints as keycap rows (left-aligned, each label after its own keycaps: menu.cpp TipKeys).
struct Tip {
    std::string name;
    std::string tag;                                   // "" = no tag
    ImU32 tagCol = 0;
    std::vector<std::pair<std::string, ImU32>> lines;  // text, colour (0: the soft text colour)
    // After the lines: "Fabricator cost", one row per entry with its icon (the crafter tooltip's rows). nullptr: none.
    const std::vector<::sr2::ui::sandbox::CostLine>* costs = nullptr;
    std::vector<KeyRow> keys;
};

// menu.cpp's drawing helpers, so the card looks like the rest of the page.
struct Helpers {
    float scale = 1.0f;                                // overlay scale (1 = 1080p)
    void (*beginCard)(const char* id, float w) = nullptr;
    void (*endCard)() = nullptr;
    void (*heading)(const char* text, ImU32 col) = nullptr;
    void (*tooltip)(const std::string& text) = nullptr;
    void (*pushFloating)() = nullptr;                  // popup style (PushFloatingStyle / PopFloatingStyle)
    void (*popFloating)() = nullptr;
    void (*surface)(bool outline) = nullptr;           // popup background (FloatingSurface)
    void (*tip)(const Tip& t) = nullptr;               // item-style tooltip (menu.cpp RichTooltip)
    // The overlay's sub-tab strip (the tab bar's look, smaller): `labels` in a row, `sel` changed on click.
    void (*subTabs)(const char* id, const std::vector<std::string>& labels, int& sel) = nullptr;
    ImU32 gadgetAccent = 0;                            // the item grids' gadget colour (tags)
    ImU32 upgradeAccent = 0;                           // the upgrades' tag colour
};

// True while the card shows its Upgrades tab (false: a blueprint tab), for the page's key hints.
bool ShowingUpgrades();

// A menu entry with its keys drawn as keycaps at the right (instead of ImGui's shortcut text), then
// `after` in the dim colour ("up"); nullptr keys: a plain entry. Inside a popup.
bool KeyMenuItem(const char* label, const char* keys, const char* after, bool enabled, float scale);

// The card, `w` wide. `s`: the main snapshot (the crafter's recipes and categories for the blueprint tabs).
void DrawCard(const Helpers& h, const ::sr2::ui::sandbox::Snapshot& s, float w);

}  // namespace sr2::ui::sandbox_upgrades
