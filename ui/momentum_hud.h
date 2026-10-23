#pragma once
// Minimal momentum readout (horizontal speed) drawn by the ImGui overlay, by
// default just below the screen centre, also while the settings window is
// closed. Placed with the HUD edit mode (hud_edit.h, Element::SpeedPanel).
//
// Threading: the owner (hl2_movement) publishes a State from the Unity main
// thread (its frame callback); every field is a separate relaxed atomic, so a
// torn read only mixes two consecutive frames (harmless for a readout). The
// render thread draws inside the overlay's Present hook. A state older than
// kStaleMs is not drawn (main thread stalled, loading screen, module stopped).
// No game calls happen on the render thread.

namespace sr2::ui::momentum_hud {

inline constexpr unsigned long long kStaleMs = 250;

struct State {
    float speed = 0;       // horizontal speed, module units (Source u/s)
    float trend = 0;       // speed change over the last ~0.25 s (u/s); > 0 gaining
    float preSpeed = 0;    // horizontal speed at the last take-off (0 = none yet)
    float preDelta = 0;    // preSpeed minus the take-off before it
    float sync = -1;       // strafe sync % of the current / last air phase (< 0 = none)
    bool details = false;  // draw the secondary line
    bool bar = false;      // draw the thin underline bar
    bool fade = true;      // fade out while standing still
    float x = 50, y = 57.1f;  // position: % of the free space (hud_edit::Placement)
    float size = 1.6f;     // main number's font size, % of the screen height
    float barFull = 640;   // speed at which the bar is full (u/s)
};

// The panel's box in units of the main font size (fs = screen height * size / 100):
// kBoxW wide, the number on a kLine high line, then the bar and the details line.
// The content is centred horizontally in the box.
inline constexpr float kBoxW = 3.2f, kLine = 1.2f, kBarH = 0.25f, kDetailsH = 0.9f;
inline float BoxHeightUnits(bool bar, bool details) { return kLine + (bar ? kBarH : 0.0f) + (details ? kDetailsH : 0.0f); }
// The box in px for `sizePct` on a screen `screenH` high, with the bar / details
// settings as stored now (render thread; for the edit mode).
void EditBox(float sizePct, float screenH, float& w, float& h);

// Any thread (normally the Unity main thread).
void Publish(const State& s);
void Clear();
// Render thread: true while a fresh state exists (the overlay renders then).
bool HasContent();
// Render thread, between ImGui::NewFrame and ImGui::Render (overlay only).
void Draw();

}  // namespace sr2::ui::momentum_hud
