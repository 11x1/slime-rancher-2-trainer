#pragma once
// Minimal momentum readout (horizontal speed) drawn by the ImGui overlay just
// below the screen centre, also while the settings window is closed.
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
    float yOffset = 0.06f; // fraction of the screen height below the centre
    float scale = 1.0f;    // user size multiplier
    float barFull = 640;   // speed at which the bar is full (u/s)
};

// Any thread (normally the Unity main thread).
void Publish(const State& s);
void Clear();
// Render thread: true while a fresh state exists (the overlay renders then).
bool HasContent();
// Render thread, between ImGui::NewFrame and ImGui::Render (overlay only).
void Draw();

}  // namespace sr2::ui::momentum_hud
