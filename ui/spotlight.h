#pragma once
// Search over the settings window, like macOS Spotlight: Ctrl+Space or
// Alt+Space while the overlay is open shows a centred search box over the
// dimmed window. Typing filters every setting (label, tooltip, enum options,
// module, category, tab and card names) and the pages (tabs, Profiles,
// Settings, Sandbox, Slime DJ), ranked prefix > word start > substring >
// fuzzy, at most 8 results with their breadcrumb and the category's icon.
// Up/Down select, Enter or a click opens the result (menu.cpp switches to the
// page, scrolls to the row and pulses it), Tab or the row's switch toggles a
// Bool in place, Esc / a click outside / the chord again closes it.
//
// Render thread only, called by menu::Draw after the settings window. Kept
// out of menu.cpp, which hands over its icon, switch and shadow helpers.

#include <string>
#include <vector>

#include "imgui.h"
#include "menu_layout.h"
#include "settings_view.h"

namespace sr2::ui::spotlight {

// menu.cpp's drawing helpers, so the box looks like the rest of the window.
struct Helpers {
    float scale = 1.0f;                                                   // overlay scale (1 = 1080p)
    ImVec2 winMin, winMax;                                                // the settings window (dimmed, box centred on it)
    void (*icon)(ImDrawList* dl, layout::Icon icon, ImVec2 topLeft, ImU32 col) = nullptr;  // 18 x 18 at scale
    bool (*toggle)(const char* id, bool on) = nullptr;                    // the cards' on/off switch; true when clicked
    void (*shadow)(ImDrawList* dl, ImVec2 a, ImVec2 b, float rounding, float dy, float spread, ImU32 col) = nullptr;
};

// Where a chosen result leads. `key` empty: the page only (no row to pulse).
struct Target {
    std::string cat;
    int tab = 0;
    std::string module, key;
};

// The chord (open / close) and, while open, the box. Returns true once when a
// result was chosen; `out` is then where to go.
bool Draw(const Helpers& h, const std::vector<layout::Category>& cats, const std::vector<ModuleView>& views, Target& out);

bool IsOpen();
void Close();  // the overlay closed: the box starts closed next time

}  // namespace sr2::ui::spotlight
