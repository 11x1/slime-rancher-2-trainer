#pragma once
// The Sandbox page's "Upgrades & Blueprints" card (modules/sandbox/upgrades.cpp, sandbox_upgrades_bridge.h):
// tabs Upgrades (the Fabricator's vacpack / player upgrades with their game icon and tier) and one per
// Fabricator gadget category (Utilities, Warp Tech, Decorations: blueprints locked / unlocked). Also the
// tier menu of the vacpack slot row (right-click on an empty or locked slot: the Extra Tank tier).
// Render thread only; every change is a posted request. Kept out of menu.cpp, which only calls these and
// hands over its card / tooltip / popup helpers.

#include <string>

#include "imgui.h"
#include "sandbox_bridge.h"

namespace sr2::ui::sandbox_upgrades {

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
};

// The card, `w` wide. `s`: the main snapshot (the crafter's recipes and categories for the blueprint tabs).
void DrawCard(const Helpers& h, const ::sr2::ui::sandbox::Snapshot& s, float w);

// The vacpack slot row's tier menu: call once per frame after the slot row, outside the slots' ID scope;
// `open` = an empty or locked slot was right-clicked this frame.
void SlotTierMenu(const Helpers& h, bool open);

}  // namespace sr2::ui::sandbox_upgrades
