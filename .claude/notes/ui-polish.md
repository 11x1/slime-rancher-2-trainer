# UI polish (branch feature/ui-polish)

- Collectible tracker (c1, n3, c4): always 3 columns, rows grow, left-aligned (no centering of partial rows), centered under the minimap; label must not cover the icon when alone.
- n7 marked "still old": check which build it ran on.
- Sidebar commit stamp: grayer text; tooltip shows only the copy hint.
- Peel animation: last frame shows opaque corners; it stays where it started when the theme changes or the menu moves.
- Minimap icons (resources etc.) scale with zoom like the player arrow; same size at base zoom.
- Clicking a checkbox's title or row toggles it (F8 and in-game Options).
- Settings page status panel: remove the "Overlay" row (also from the Options menu tab); "Game build" row reads "matches known build" in green, version in a hover tooltip (mismatch stays a visible warning).
- (?) help marker (Speed Panel subtab and everywhere): centered, not thin/pixelated. Fix in the shared helper.
- Sliders support suffixes (%, m, s, x, m/s) and special text (e.g. "no limit" at 0); add units to every slider that lacks one.
- "Per resource (advanced)" shows when none of its per-resource toggles are on. Add an "any of these on" parent rule; audit other groups.
- HL2 movement console command names shown as inline code (monospace chip). Embed a small monospace font in the overlay.
- F8 subtab of a disabled feature: gray out its settings (read-only) instead of "this is turned off"; the feature's own switch stays active at the top. In-game Options tab keeps hide-when-off.
