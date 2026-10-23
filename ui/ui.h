#pragma once
// Public entry points of the UI. Each is called once from the core worker
// thread (the settings UI after the modules were set up). They are independent:
// each may fail (and logs why) without affecting the others.

namespace sr2::ui {

// Injects a "Mods" category into the game's own Options menu
// (MonomiPark.SlimeRancher.UI.Options.OptionsUIRoot). Installs hooks only; the
// tab is built the next time the Options menu binds its categories.
bool StartOptionsTab();

// Dear ImGui overlay on the game's D3D11 swap chain, toggled with a hotkey
// (settings "ui.overlay_hotkey", default F8). Always the guaranteed way to
// configure the mod, even if the Options-menu injection fails.
bool StartOverlay();

// "[modded]" line on the title screen (ui/modded_label.cpp). Not a setting, always on. Called
// once from the core worker BEFORE the modules are set up, because it owns the
// MainMenuLandingRootUI.OnEnable hook that quick_start listens to.
bool StartModdedLabel();

}  // namespace sr2::ui
