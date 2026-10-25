# Round 17 plan (feedback from the round 16 test, 2026-10-25)

Build tested: 3eaf3b60 (develop b50245e). Results are in the work-account checklist https://claude.ai/artifact/12HrkuKyWbJs1PtsufwVvE (collection `results`, ids `r16*`). User quotes are verbatim.

## How to run this round
Same as round 16 (auto-memory: orchestrator mode, Opus subagents at medium effort in worktrees, git workflow, no session link, plain commands, checklist conventions, read logs yourself, deploy once per round; agent builds take turns via build.ps1 and must `Set-Location` into their own worktree). Then build the round 17 checklist and publish it to the same artifact. The autotest harness (`.claude/notes/autotest.md`) is a separate track; don't mix it into these tasks.

Passed: gadget stacking (place on top, overlap, plots still block, switch + Free Placement), Music Box / Corral Radio icons, plort door icon, Music Pillar and Tile, highlighter labels, tracker row on every ranch island, ghost icon, smaller submenu arrow, dark and special plorts, plort range, radio key grid, Next: labels, Hen Collector description.

Skipped (re-add): r16fl2-fl5 (blocked by A), r16gs4 (stacks save and pick up together), r16uc3 (Coop Air Net), plus the round 16 Skipped folds (the autotest track takes those over once the user approves it).

## A. Auto-Feeder switch never shows (`fix/feeder-lock-r17`)
- r16fl1: "i dont see the "locking" checkbox"; r16fl6: "I dont see the option to lock the feeder".
- The log says why: `[custom_upgrades] auto-feeder 'leave last food item in' unavailable (class 'LandPlotModel' not found in Assembly-CSharp.dll) - feeders stay vanilla`. The class lookup uses the wrong namespace or assembly; find the right one with re_lookup (re/plots.md) and fix the lookup in `modules/custom_upgrades/feeder_lock.cpp`. Check every other class name that file resolves the same way.
- New, r16fl6: "for the feeder models ingame, color them red -> yellow -> green based on how full the are (the smaller lights besides the button that are green)". Tint the feeder model's small indicator lights by fill level (empty red, half yellow, full green) through a property block; find the light renderers on the SlimeFeeder prefab. Make it a setting (default on), cheap (refresh on change or every second, not per frame).

## B. Extra Tank lost its slot; refinery tooltips (`fix/vacpack-upgrades-r17`)
- r16ov1: "Extra tank doesnt give me a space anymore". Log: `vacpack after Extra Tank IV: ... 7 of 7 slots unlocked` (expected 8 with IV), and `Extra Tank I: ... 6 of 7 slots unlocked (before: ... 7 of 7)`. Find the regression in the Extra Tank II-IV slot code (custom_upgrades vacpack, sandbox upgrades) and check the round 16 changes that touched it (overlay H removed the +/- tooltips; see git log since a77631c).
- r16ov1: "also add refinery upgrade tooltips to upgrades (like it is for slimes in slimeipedia)". In Sandbox > Upgrades & Blueprints, hovering an upgrade entry shows its refinery cost (the items and counts the Fabricator asks for), styled like the Slimepedia slime tooltips in the overlay.

## C. Icons keep their place on the card (`fix/card-icons-r17`)
- r16ic1: "should still be in the same position, they get centered, same for corral music box icons". The user's drawings on vanilla plot cards (Sprinkler, Music Box via `VanillaIconName`) are centred, but the vanilla icon sits elsewhere on the card. Keep the vanilla icon's rect / anchors / pivot and only swap the sprite (check `preserveAspect` and whether the sprite's pivot or padding moves it; pad_icon output is 256 px with padding).

## D. Overlay context menu rounding (`fix/overlay-r17`)
- r16ov2: "too rounded now for them, slimes ctx menu is fine". The Upgrades / Blueprints right-click menus (Discover / Undiscover, Unlock blueprint) are rounded too much; match the slime spawner context menu's rounding.

## E. Plorts and lamps: instant start, default glow (`fix/plort-glow-r17`)
- r16pg1: "not instantly, also theys till have their default glow :(". Plorts should start glowing as soon as music plays near them (as fast as lamps now do), and the plort's own default glow should be replaced by the beat glow while it pulses (not added on top), back to default when the music stops.
- r16pg5: "why not all?". The 64-plort cap: raise it or remove it if the cost allows (measure with the perf counters; property blocks only on changed plorts), otherwise explain the cap in the setting's tooltip.
- r16lp1: "ice boxes on powderfall still take time (same for plorts), otherwise works". Powderfall ice boxes (the ice pulse objects) start as fast as placed lamps.

## F. Quantum Link polish (`fix/quantum-link-r17`)
- r16ql1: "Its there for a split second, like 200-500ms, otherwise gone nice". The drone row magnifier + [SPACE] hint flashes for 200-500 ms when the Drones tab opens. Hide it before the first frame it's visible (hide on the row's creation / enable, not on a later tick).
- r16ql2: "can we somehow make the white transparent? otherwise clean". The glitch copy still draws white; make the white parts transparent (alpha from the drone pass, or a tint with alpha near 0 on the bright parts).

## G. Stale inventory for one frame (`fix/inventory-refresh-r17`)
- User: "when closing the menu with inventory open, emptying slots, reopening inventory, for a split frame it shows old inventory".
- Most likely the overlay's inventory page (Sandbox vacpack / item grid, `ui/item_grid.cpp`): close the overlay on that page, empty vacpack slots in game, reopen it, and the old contents show for one frame before the refresh. Refresh the cached slot data before the first frame the page draws on reopen (or draw nothing until fresh data is in), not on a later tick. If the overlay isn't the cause, check the game's own inventory UI with the mod's hooks the same way.

## Status (2026-10-25)
All tasks A-G merged and deployed (744628c, DLL 9494cc5d; one build for the round), together with the autotest harness, client and showcase tool (setting off by default). Checklist: round 17 published to https://claude.ai/artifact/12HrkuKyWbJs1PtsufwVvE (20 new cases in `.claude/checklist/r17-cases.json`; r16gs4, r16uc3, r16fl4 and the older skipped cases carried over). Open question: B's cost tooltip uses the Sandbox page's tooltip style with icons; the user asked for it "like it is for slimes in slimeipedia".
