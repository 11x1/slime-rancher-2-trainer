#pragma once
// The sidebar logo: a different slime every time the overlay opens, picked
// from the Slimepedia slime entries the loaded save has unlocked, drawn with
// the entry's own icon straight from Unity's D3D11 texture. With no save
// loaded, nothing discovered, or the Pedia not readable, the menu draws its
// own pink slime instead.

#include <string>
#include <vector>

#include "imgui.h"

struct ID3D11Device;

namespace sr2::ui::slime_icons {

// One block of a Slimepedia page. `title` is empty for the entry's intro text.
struct Section {
    std::string title;
    std::string text;
};

// The current logo's Slimepedia entry, as plain UTF-8 copied on the main thread.
struct Entry {
    std::string name;
    std::vector<Section> sections;
};

// Render thread, once the overlay has its device. Without it Draw() is false.
void Init(ID3D11Device* device);

// Overlay opened: asks the main thread to re-read the discovered slimes of the
// loaded save and pick another one (never the same twice in a row).
void OnOpen();

// Draws the current icon fitted (aspect kept) into [min, max]. False if there
// is none; the caller then draws its fallback. While a pick just requested by
// OnOpen is on its way (a frame or two), it draws nothing and returns true.
bool Draw(ImDrawList* dl, ImVec2 min, ImVec2 max);

// The current icon's Pedia entry, or nullptr if there is none (render thread;
// valid until the next Draw/OnOpen).
const Entry* CurrentEntry();

}  // namespace sr2::ui::slime_icons
