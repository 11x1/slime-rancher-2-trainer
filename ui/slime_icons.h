#pragma once
// The sidebar logo: a different slime every time the overlay opens, using the
// game's own slime icons (each SlimeDefinition's `icon` sprite, drawn straight
// from Unity's D3D11 texture). Until they are found, or if they can't be, the
// menu draws its own pink slime instead.

#include <string>

#include "imgui.h"

struct ID3D11Device;

namespace sr2::ui::slime_icons {

// Render thread, once the overlay has its device. Without it Draw() is false.
void Init(ID3D11Device* device);

// Overlay opened: picks another slime (never the same twice in a row) and, if
// no icons are known yet, asks the main thread to look for them.
void OnOpen();

// Draws the current icon fitted (aspect kept) into [min, max]. False if there
// is none; the caller then draws its fallback.
bool Draw(ImDrawList* dl, ImVec2 min, ImVec2 max);

// Display name of the current icon ("" if none).
std::string CurrentName();

}  // namespace sr2::ui::slime_icons
