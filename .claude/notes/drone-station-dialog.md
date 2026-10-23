# Quantum drone station dialog (branch feature/drone-station-dialog)

**Status: built on the branch, not yet tested in game.** Design, findings and the in-game check list
are in `modules/drone_price_limit/NOTES.md`; delete this note once the in-game check passes.

Checklist d1 + misc notes.
- In the Quantum Drone station with Plort Market selected, add a button next to "Change", also opened with C.
- It opens a game-UI dialog: mode (minimum price or "fraction price") and the value as sliders; 0 shows "no limit".
- Show a base-price cheat sheet for each plort in the ranch market terminal's format.
- Remove the drone controls from the F8 menu (it is a game-UI feature now).

Decisions taken:
- Per station, keyed by the station's saved actor id in the custom_upgrades per-save sidecar.
- Mode is two buttons (not a 2-step slider); the value is one PlotMenuSlider (CreateIn).
- Gamepad: right stick click opens it (C has no pad binding); the pill always shows the C key.
