#pragma once
// A searchable grid of item tiles for the overlay (Misc > Sandbox: vacpack
// editor, spawner, crafter). Looks like an inventory screen: square dark tiles
// with the game's icon, a thin category accent along the bottom edge, a
// highlight on hover and an outline on the selected tile. Names show on hover
// (the caller draws the tooltip for Result::hovered, so it can add details).
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
    ImU32 accent = 0;                   // bottom accent (0: none)
    bool dimmed = false;                // drawn faded (e.g. not unlocked); still hoverable and clickable
    int badge = -1;                     // small number in the corner (e.g. how many you own); < 0: none
};

struct State {
    char search[64] = {};
};

struct Options {
    float scale = 1.0f;          // overlay scale (1 = 1080p)
    float tile = 58.0f;          // minimum tile edge before scaling; tiles stretch to fill each row
    float height = 220.0f;       // grid height before scaling (it scrolls)
    uintptr_t selected = 0;      // tile drawn as selected
    bool drag = false;           // tiles are drag sources (kDragType)
    const char* hint = "Search items...";
    const char* empty = "No matching items.";
};

struct Result {
    int clicked = -1;  // index into `tiles` clicked this frame
    int hovered = -1;  // index of the hovered tile
    int shown = 0;     // tiles that match the search
};

// Case-insensitive substring match, as the search box uses it.
bool Matches(const std::string& name, const char* needle);

// The search box (full width) and the grid below it.
Result Draw(const char* id, const std::vector<Tile>& tiles, State& st, const Options& o);

// One tile at the cursor (used for the vacpack slots too): background, accent,
// icon or name, badge. `hot` / `selected` change the frame. Pure drawing; the
// caller places an item (InvisibleButton) over [min, max].
void DrawTile(ImDrawList* dl, ImVec2 min, ImVec2 max, const Tile& t, bool hot, bool selected, float scale);

}  // namespace sr2::ui::item_grid
