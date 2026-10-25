#pragma once
// The settings window of the overlay: sidebar (slime logo, categories,
// Profiles, Settings, Dark/Light), a tab bar per category and the settings as
// cards. Built only from what is registered in core::settings, arranged by
// menu_layout. Render thread only; no Win32 or D3D here (overlay.cpp owns
// input, key capture and the back buffer).

#include <string>

#include "imgui.h"
#include "settings_view.h"

namespace sr2::ui::menu {

// What the overlay provides each frame.
struct Host {
    float scale = 1.0f;                // 1 = 1080p
    unsigned long long nowMs = 0;      // same clock as LastWriteTick()
    std::string hotkeyName;            // "F8"
    bool versionMismatch = false;      // core::version_guard (kept out of here: it needs <windows.h>)
    std::string versionStatus;
    bool versionKnown = false;         // GameAssembly.dll hash matches the known build (and no mismatch)
    // Startup notes (DrawHint): the "press <hotkey>" note, and quick_start's auto-continue
    // note (0 none, 1 "Hold Shift..." pending, 2 "Staying in the main menu").
    bool menuHint = false;
    bool allOff = false;               // no module switched on: the note adds "All features start off..."
    int quickHint = 0;
    float quickSeconds = -1;           // countdown of the pending auto-continue, < 0 = unknown
    // Key capture (overlay polls the keyboard): start, and whether `d` is being captured.
    void (*startCapture)(const settings::Def& d) = nullptr;
    bool (*isCapturing)(const settings::Def& d) = nullptr;
    void (*openProfilesFolder)() = nullptr;
};

// Theme from the "ui.overlay_theme" setting (applies the style when it or the
// scale changed). Call every frame before ImGui::NewFrame.
void SyncTheme(float scale);

// Overlay was opened (new slime logo, profiles rescan is done by the overlay).
void OnOpened();

// Draws the window. Returns false once the user closed it.
bool Draw(const Host& host);

// The startup notes, stacked top left: "Press F8 for mod settings" and, while
// quick_start's auto-continue is pending, "Hold Shift to stay in the main menu".
void DrawHint(const Host& host);

// Theme peel: switching Dark/Light peels the old look off the window. The
// overlay copies the frame it just rendered (still the old theme) into a
// texture when WantSnapshot() is true, then calls StartPeel with it (or with
// 0 if the copy failed: the theme then changes without the animation).
bool WantSnapshot();
void StartPeel(ImTextureID oldFrame);

}  // namespace sr2::ui::menu
