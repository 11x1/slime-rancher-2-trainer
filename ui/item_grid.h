#pragma once
// A searchable grid of item tiles for the overlay (Misc > Sandbox: vacpack
// editor, spawner, crafter). Looks like an inventory screen: square dark tiles
// with the game's icon, a thin category accent along the bottom edge, a
// highlight on hover and an outline on selected tiles. Names show on hover
// (the caller draws the tooltip for Result::hovered, so it can add details).
//
// Above the grid: optional icon-only tabs, the search box and an optional
// action button, on one row. Tiles can be split into sections (a heading and a
// horizontal rule before the first tile of each section).
//
// Render thread only. Draws nothing from the game itself: icons are ids from
// ui/item_icons.h, registered (and cached) by the module on the main thread.

#include <cstdint>
#include <string>
#include <vector>

#include "imgui.h"

namespace sr2::ui::item_grid {

// Drag-and-drop payload type of item tiles (payload: the tile's uintptr_t id).
inline constexpr const char* kDragType = "SR2_ITEM";

struct Tile {
    uintptr_t id = 0;                   // opaque to the grid
    const std::string* name = nullptr;  // searched and shown when there is no icon; must outlive Draw()
    uint32_t icon = 0;                  // item_icons id (0: the name is drawn instead)
    uint32_t icon2 = 0;                 // second icon: the two are drawn overlapping (composed largo icon)
    ImU32 accent = 0;                   // bottom accent (0: none)
    bool dimmed = false;                // drawn faded (e.g. not unlocked); still hoverable and clickable
    bool selected = false;              // part of a multi-selection (outlined like the selected tile)
    bool related = false;               // related to the hovered tile (soft highlight)
    int badge = -1;                     // small number top right (e.g. how many you own); < 0: none
    int amount = 0;                     // amount chosen for this tile, bottom right ("x5"); <= 0: none
    int section = 0;                    // index into Options::sections; tiles must be ordered by section
};

// An icon-only tab left of the search box.
struct Tab {
    uint32_t icon = 0;       // item_icons id; 0 draws `glyph`
    int glyph = 0;           // fallback drawing: 0 = "all" (four squares), else a letter (e.g. 'P')
    ImU32 tint = 0;          // fallback glyph colour (0: text colour)
    std::string tip;         // tooltip (the tab has no title)
};

struct State {
    char search[64] = {};
    int tab = 0;             // selected tab (index into Options::tabs)
};

struct Options {
    float scale = 1.0f;          // overlay scale (1 = 1080p)
    float tile = 58.0f;          // minimum tile edge before scaling; tiles stretch to fill each row
    float height = 220.0f;       // grid height before scaling (it scrolls)
    uintptr_t selected = 0;      // tile drawn as selected
    bool drag = false;           // tiles are drag sources (kDragType)
    const char* hint = "Search items...";
    const char* empty = "No matching items.";
    const std::vector<Tab>* tabs = nullptr;              // icon tabs before the search box (none: no tabs)
    bool tabsBelow = false;                              // tabs on their own row under the search box, right-aligned
    const char* button = nullptr;                        // action button after the search box (nullptr: none)
    bool buttonEnabled = true;
    const std::vector<std::string>* sections = nullptr;  // section headings (Tile::section); "" = rule only
    const char* dropType = nullptr;  // the grid area accepts drops of this payload type (Result::dropped)
    const char* dropHint = nullptr;  // drawn over the grid while such a payload is dragged
};

struct Result {
    int clicked = -1;       // index into `tiles` clicked this frame
    int rightClicked = -1;  // index right-clicked this frame (open a context menu)
    int hovered = -1;       // index of the hovered tile
    float wheel = 0;        // Shift / Ctrl + mouse wheel over the hovered tile (notches; + = up); the grid does not scroll then
    int wheelStep = 0;      // that wheel as an amount step: +-1 with Shift, +-10 with Ctrl (0: no wheel). Plain wheel scrolls
    int shown = 0;          // tiles that match the search
    bool button = false;    // the action button was clicked
    bool buttonHovered = false;  // (short delay; also while disabled) for the caller's tooltip
    int hoveredTab = -1;    // tab under the mouse (the caller shows Tab::tip)
    bool tabChanged = false;
    bool clearSelection = false;            // right-click on the grid outside the tiles (the caller clears its selection)
    const ImGuiPayload* dropped = nullptr;  // Options::dropType payload dropped on the grid this frame
};

// Amount step of one wheel notch with the current modifiers: +-10 with Ctrl, +-1
// with Shift, 0 with neither (a plain wheel scrolls). Slots and lists share it.
int WheelStep(float wheel);

// Case-insensitive substring match, as the search box uses it.
bool Matches(const std::string& name, const char* needle);

// The header row (tabs, search box, button) and the grid below it. The caller
// filters tiles by State::tab itself (before calling, from the last frame's tab).
Result Draw(const char* id, const std::vector<Tile>& tiles, State& st, const Options& o);

// One tile at the cursor (used for the vacpack slots too): background, accent,
// icon or name, badge, amount. `hot` / `selected` change the frame. Pure
// drawing; the caller places an item (InvisibleButton) over [min, max].
void DrawTile(ImDrawList* dl, ImVec2 min, ImVec2 max, const Tile& t, bool hot, bool selected, float scale);

// An item icon (or two composed, as tiles draw them) fitted into [min, max].
// False if neither can be drawn.
bool DrawIcons(ImDrawList* dl, uint32_t icon, uint32_t icon2, ImVec2 min, ImVec2 max, ImU32 tint = IM_COL32_WHITE);

// A padlock centred at `c` with height `h`; `open` lifts the shackle.
void DrawLock(ImDrawList* dl, ImVec2 c, float h, bool open, ImU32 col);

}  // namespace sr2::ui::item_grid
