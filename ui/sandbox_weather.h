#pragma once
// The Sandbox page's "Time & weather" card (modules/sandbox/weather.cpp, sandbox_weather_bridge.h). Time of
// day first: the game's clock, quick skips to the next 06:00 / 12:00 / 18:00 / 00:00, a time slider with Skip
// ahead (fast-forward, as sleeping) and Rewind (after a confirmation), Pause time. Then the weather
// running in a zone (the player's by default) and its forecast, a weather picker (the zone's patterns with
// the map's icons, plus Clear skies) with the pattern's states, "All zones" and "Lock" switches, Start now,
// End weather and Back to natural weather. Render thread only; every change is a posted request. Kept out
// of menu.cpp, which only calls DrawCard and hands over its card / tooltip / popup / switch helpers.

#include <string>
#include <vector>

#include "imgui.h"

namespace sr2::ui::sandbox_weather {

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
    void (*subTabs)(const char* id, const std::vector<std::string>& labels, int& sel) = nullptr;
    bool (*toggle)(const char* id, bool on) = nullptr;               // the overlay's on/off switch (true = clicked)
    bool (*accentButton)(const char* label, ImVec2 size) = nullptr;  // the page's accent button
};

// The card, `w` wide.
void DrawCard(const Helpers& h, float w);

}  // namespace sr2::ui::sandbox_weather
