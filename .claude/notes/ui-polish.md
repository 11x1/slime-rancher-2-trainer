# UI polish (branch feature/ui-polish-2) - done, needs an in-game check

The old `feature/ui-polish` branch name was taken (merged earlier), so this work is on `feature/ui-polish-2`. Details are in `ui/NOTES.md`, `modules/minimap/NOTES.md`, the collectibles header comment and `README.md`.

- [x] Collectible tracker (c1, n3, c4): 3 columns as wide as the widest cell, block centred under the minimap, partial rows left-aligned; texts left-aligned so a label never spills over its icon.
- [x] n7 "still old": the test ran on the 20:04 build (c3a71f0), which had the notices; the world-button sale logged no notice line, so the HUD list with the drone provider was not found. Now every loaded active list is checked and the reason is logged once. Unverified in game.
- [x] Sidebar commit stamp: grayer text; tooltip only the copy hint.
- [x] Peel animation: clipped to the rounded window (no square corners), follows the window when it moves.
- [x] Minimap icons scale with the zoom like the player arrow (same size at 2.5x).
- [x] Clicking a switch row's label or anywhere on the row toggles it (F8). Options tab: optional hook on `OptionsItemViewHolder.Activate`; unverified that a row click calls it.
- [x] Settings status: no "Overlay" row; known game build reads "matches known build" in green with the details as tooltip. The "Options menu tab" row was kept (the note's "also from the Options menu tab" was read as "the Options tab has no such row").
- [x] (?) marker: one `HelpMarker` helper, crisp and centred.
- [x] Slider units and zero texts (`Def::unit`, `zeroText`, `displayScale`), added across modules (drone price sliders left alone: being removed on feature/drone-station-dialog).
- [x] "Any of these on" parent rule (`Def::anyOf`); Per resource (advanced) needs a source in advanced mode; ore amount and its cap need a source applied.
- [x] HL2 console names as inline code chips (JetBrains Mono, OFL, 9 KB).
- [x] F8 disabled feature: settings grayed and read-only, switch usable; Options tab still hides.
