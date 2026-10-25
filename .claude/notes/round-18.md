# Round 18 plan (feedback from the round 17 test, 2026-10-26)

Build tested: 9494cc5d (develop 744628c). Results are in the work-account checklist https://claude.ai/artifact/12HrkuKyWbJs1PtsufwVvE (collection `results`, ids `r17*`; free text in `misc/notes`). User quotes are verbatim.

## How to run this round
Same as round 17 (auto-memory: orchestrator mode, Opus subagents at medium effort in worktrees, git workflow, no session link, plain commands, checklist conventions, read logs yourself, deploy once per round; agent builds take turns via build.ps1 and must `Set-Location` into their own worktree). Then build the round 18 checklist and publish it to the same artifact.

Passed: feeder switch shows and the last item stays, both icon positions, the Vacpack page refresh, Extra Tank slots after loading, every plort glow case, Powderfall ice, the see-through glitch, station rows keeping their hint.
Not marked: r17ov1 (less-rounded upgrade menus); carry it over. The older Skipped folds also carry over.

## A. Feeder: switch style, fill lights broken and slow (`fix/feeder-r18`)
- r17fl1 (pass): "use the same component as for drone modes, not a slider". The "Leave last food item in" control should use the game's own selector component from the drone station's mode choice (the one that picks between options), not the slider-style switch. Find that component in re/drones.md / the drone station UI and reuse it on the Auto-Feeder card.
- r17fl3: "Nope, doesnt change". r17fl4: "No color, put under "Plot upgrades"". r17fl5: failed. The fill lights never change colour. Find out why (wrong renderer, the property block overridden by the Animator or the material instance, wrong property names; check the log lines from `feeder_lights.cpp`).
- The setting goes under the "Plot upgrades" group, not Corrals.
- Performance: the log shows `custom_upgrades.world: feeder lights` costing about 22-32 ms once every 10 s, which causes frame hitches. That is the feeder search. Make it cheap: find feeders through the plots the mod already knows (plot list or LandPlot hooks), not a scene-wide search, or spread it over frames.

## B. Upgrade cost tooltip like the Slimepedia (`fix/upgrade-tooltip-r18`)
- r17ov2 (partial): "Also the slimepedia's entry text, the newbucks icon is wrong should use assets/icons/game/upgrades/shop/iconNewBuck.png".
- Add the upgrade's Slimepedia / Fabricator description text to the tooltip, as the Slimepedia slime tooltips show their entry text.
- The Newbucks icon: use the game's `iconNewBuck` sprite, loaded at runtime by name (never embed or commit the PNG; `assets/icons/game/` is gitignored game art). Use it wherever the overlay shows a Newbucks cost.

## C. Quantum Link: hint comes back after Quantum Cloud (`fix/quantum-link-r18`)
- r17ql1 (partial): "when going back from quantum cloud, fully visible doesnt go away". When going back from the Quantum Cloud page to the Drones tab, the magnifier + [SPACE] hint shows on the rows and stays. Rows are rebound or refocused on the way back without passing the new hooks, or the sweep is off by then. Cover that path too.

## D. Dropping to the inventory selection (`fix/vacpack-drop-r18`)
- Note: "Dropping to inventory selection doesnt work, doesnt empty slot". Most likely the overlay's Sandbox > Vacpack page: dragging an item out of a slot (or dropping it onto the selection / outside) should empty that slot, but the slot stays filled. Check the item grid's drag-and-drop and the empty-slot action in `ui/item_grid.cpp` and the sandbox vacpack bridge. If the cause turns out to be in the game's own inventory, say so.

## Status (2026-10-26)
A-D merged and deployed (6a775b7, DLL 32fc64f7), together with the autotest save list / title fixes, stripped defaults (`feature/stripped-defaults`), CI releases (`feature/ci-release`, not pushed yet) and the icon cleanup (`chore/icons-custom-only`). Checklist: round 18 published (14 new cases in `.claude/checklist/r18-cases.json`). r17ov1 was actually marked pass. Next: the history rewrite with the user (`.claude/notes/history-rewrite.md`), then autotest wave 2 (`.claude/notes/autotest.md` section D).
