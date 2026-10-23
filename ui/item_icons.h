#pragma once
// Game sprites (IdentifiableType icons and the like) as ImGui images, for the
// overlay's item grids (ui/item_grid.h). Same texture path as the Slimepedia
// logo (slime_icons.cpp): the sprite's Unity texture is sampled straight from
// its D3D11 resource; nothing is copied or decoded.
//
// Main thread: Register(sprite) reads the sprite's texture and rectangle once
// and returns a small id (0 = none). The sprite and its texture are kept alive
// with GC handles for the rest of the process (they are game assets that stay
// loaded anyway), so the D3D11 resource stays valid while the overlay draws
// from it. Registering the same sprite again is a map lookup.
//
// Render thread: Draw(id, ...) draws the icon fitted into a rectangle. The
// shader resource view per texture is created on first use and reused.

#include <cstdint>

struct Il2CppObject;

namespace sr2::ui::item_icons {

// Main thread. 0 if the sprite can't be drawn (missing, destroyed, no texture)
// or the UI is in fault mode. Failures are remembered per sprite too.
uint32_t Register(Il2CppObject* sprite);

}  // namespace sr2::ui::item_icons

#if defined(SR2_HAVE_IMGUI)
#include "imgui.h"

namespace sr2::ui::item_icons {

// Render thread. Draws icon `id` fitted (aspect kept, centred) into [min, max],
// multiplied by `tint`. False if there is no such icon or it can't be drawn.
bool Draw(ImDrawList* dl, uint32_t id, ImVec2 min, ImVec2 max, ImU32 tint = IM_COL32_WHITE);

}  // namespace sr2::ui::item_icons
#endif
