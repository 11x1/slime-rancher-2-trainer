# Round 19 plan (feedback from the round 18 test, 2026-10-26)

Build tested: 32fc64f7 (develop 6a775b7, before the history rewrite). Results are in the work-account checklist https://claude.ai/artifact/12HrkuKyWbJs1PtsufwVvE (collection `results`, ids `r18*`). User quotes are verbatim.

## How to run this round
Same as round 18 (auto-memory: orchestrator mode, Opus subagents at medium effort in worktrees, git workflow, no session link, plain commands, checklist conventions, read logs yourself, deploy once per round). Then build the round 19 checklist and publish it.
- Before starting: the user force-pushes the rewritten history (`git push --force origin --all`). Afterwards run `git fetch origin --prune; git reflog expire --expire=now --all; git gc --prune=now` in the main checkout, so no old objects are left. Check `git rev-list --objects --all` for the removed paths (`.claude/notes/history-rewrite.md`).
- That push creates the first CI release (`dev-...`). Check the Actions run, especially verify_exports on the runner.
- Autotest wave 2 (`.claude/notes/autotest.md` section D) is planned for the same evening; don't let its agents and these touch the same files at the same time.

Passed: feeder selector, fill-lights setting position, collector icons, tracker rows without placeholders, upgrade tooltip text and Newbucks icon, other slot drags.
Skipped: r18cf1, r18cf2 (config cases; see D).

## A. Feeder fill lights still off: the Dictionary stride bug again (`fix/feeder-lights-r19`)
- r18fl2: "no lights (maybe needs same shader fix as for the button in silo)". r18fl3, r18fl5 failed too.
- The log says why: `[custom_upgrades] auto-feeder fill lights: unexpected Dictionary entry size 8 - off`. This is the same mistake the autotest save list had (fixed in `fix/autotest-saves-r18`): `il2cpp_class_array_element_size` was given the entries *array's* class, which returns the pointer size 8. It needs the element class from `il2cpp_class_get_element_class` first.
- Fix it in `feeder_lights.cpp`, then grep the whole repo for every other `il2cpp_class_array_element_size` / Dictionary entry reader and fix them the same way. Better: one shared helper in core (e.g. `unity::DictEntries` / `ArrayElementSize`) used everywhere, with the gotcha written in re/INDEX.md.
- Look at the user's hint too: the "button in silo" got a shader fix in an earlier round (grep the silo / sell button glow code and its NOTES). If the LED colour needs the same approach, use it.
- r18fl5 ("no hitch every 10 s") failed without a note; check the perf lines once the lights run.

## B. Vacpack page: drop onto the list still fails; remove Fill / Empty (`fix/vacpack-page-r19`)
- r18vd1: "slot not emptied, also remove the "Fill" and "Emtpy" buttons under inventory slots".
- The round 18 log lines show the cause. Every release was over the item list's own child window, `.../sbx_vac_21825E17/##grid_36347F4A`, yet the code said `not on a slot or the item list`. The release-over-list check compares against the wrong rect or ID (e.g. the rect of an outer card, the rect before scrolling, or the wrong window). Fix it: count a release as a drop when the hovered window is the grid's child window (by ID), or use that child's real clip rect.
- Remove the "Fill" and "Empty" buttons under the vacpack slots (the right-click menu and drag-to-empty stay).

## C. Elder Collector missing as a drone source (`fix/elder-source-r19`)
- r18ql1 (partial, on the Quantum Link case): "Elder collectors dont show in Source, others show". Most likely the drone station's program Source list (or the Quantum Link's drone rows, which show sources) lists Hen Collector and others but not Elder Collector. Find which list it is (custom_upgrades drone_source.cpp, drone_elder_source, quantum_link.cpp), check the log lines (`drone source: Elder Collector ...`), and make the Elder Collector show wherever the Hen Collector does, with the user's local icon when present.

## D. Startup hint: no "All features start off" line (`fix/startup-hint-r19`)
- r18cf1 (skipped): "remove the popup for "All features start off" when this gets tested". Remove the second hint line, "All features start off; enable what you want", and keep the plain "Press F8 for mod settings" hint. The stripped defaults themselves stay.

## E. Frame hitches: re-measure first (`fix/hitches-r19`, only if they persist)
In the round 18 session the log has 210 `frame hitch` lines: frame:sandbox 29 times (40-75 ms), frame:collectibles 8 (~38 ms), custom_upgrades 6, spawn_scaling 3, drone_price_limit 3. That session ran alongside heavy builds (the history rewrite's clean build check ran 01:13-01:20; the hitches cluster from 01:16). The user: "the hitches couldve been because a build step was running". So:
- First read the perf lines from the next test session, with no build running (`git config --unset sr2mods.parallelbuilds` while the user plays). Only spawn this task if the hitches persist there.
- One finding is real regardless: collectibles repeats its ~1500-sprite "extra scan" for the ghost drone sprite (4 times; the sprite gets unloaded and preloaded again). Keeping the sprite referenced once found would stop that. It can go to the collectibles owner, or to wave 2's world agent, if E isn't spawned.
