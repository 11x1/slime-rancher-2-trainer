# Round 13 plan (feedback from the round 12 test, 2026-10-25)

Build tested: 78aeb0d (develop). The Slime Radio drawing (4fd1a0c) came after it. Results are in the work-account checklist https://claude.ai/artifact/12HrkuKyWbJs1PtsufwVvE (collection `results`, ids `r12*`; free text in `misc/notes`). User quotes are verbatim.

## Status (2026-10-25)
All tasks A-H are merged and deployed (5acbad3, DLL af2af341; one build for the round). Checklist: round 13 is published to https://claude.ai/artifact/12HrkuKyWbJs1PtsufwVvE (27 new cases in `.claude/checklist/r13-cases.json`, carried-over cases under Skipped). Open: the release workflow draft on branch `chore/ci-release-research` (not merged; waiting for the user's call on the release audience and the two collector icons).

## How to run this round
The same as round 12 (see the auto-memory: orchestrator mode, Opus subagents at medium effort in worktrees, git workflow, no session link, plain commands, checklist conventions, read logs yourself, deploy once per round):
1. One agent per task below, up to 9 at a time. Each prompt is "Read `.claude/agent-brief.md` and follow it", the task's section from this file, and the branch name.
2. Merge each branch with `git merge --no-ff <branch> -m "Merge <branch> into develop"`. Merging doesn't build: `sr2mods.autodeploy` is false.
3. When all branches are merged, run one `.\deploy.ps1 -Build`. If the game is running, use `-WaitForExit` in the background. It backs up the saves and the old DLL.
4. Build the round 13 checklist from the agents' test cases (`.claude/checklist/README.md`). Publish it to the same artifact.

Passed: Tarr DJ, booth silent while off, every booth speaker plays, no crackle with Amplify, switched-off booth, speakers boost walls, the Scheduler card / buy / Sandbox raise-lower (and the log shows on/off with stored programs works), Sell-All icon, receipt buttons ("ITS PERFECT"), station pills ("YES YES YES SO NICE"), lamp pulse and range, grid tooltips, Upgrades hover, grid keyboard, Ranch > Upgrades layout.

## A. Corral Radio (`feature/corral-radio-r13`)
- Notification: "remove the parenthesis from the ingame notification".
- No Music Box: "no speaker upgrade shows the queue as empty in radio dialog, make it grayed out with an error warning on top of the dialog". Without a speaker, show the queue greyed out (not empty) with a warning line at the top of the dialog.
- Real radio: "buying radio II plays the selected music from the start not where it continued, so in theory its not really a radio if it doesnt play when no output...?" The song position keeps advancing while there is no output, so a new or returning speaker joins mid-song.
- Menu: "Use game's button for the overlay please, they have support to show what icon to press to activate the button". Switch the radio menu to `modules/common/game_button`. It still has its own copy of that code.
- "Currently playing (not hovered) song in the queue text is the same color as background, shows name in white when hovered, if possible move the '>' outside before the row currently playing".
- "Remove the top status row 'Now playing: ... (paused while this menu is open)' text please".

## B. Drone programs dialog (`fix/drone-programs-dialog-r13`)
User, on r12gb3:
- "Move the 'Program X/Y' to the same row as '[X] Start Program', '[Z] Clear All', '[ESC] Close'".
- "make the programs menu like long buttons ... they should function as buttons (same height as right now), by on the left showing '[SPACE] edit' and on the right '[Z] Delete'".
- "Add program should be a game button aswell".
- "right now cant navigate with wasd/arrow keys".
- "make next program like in settings, the '< option1 >' so its toggleable with keyboard/gamepad".
- "remove 'Arrows: choose' 'Enter: select' 'Esc: Close' text".

Use `modules/common/game_button` and `ui/nav.h`. For the `< option >` look, match the game's own settings selector, or the one in our Options tab.

## C. Sharp details pictures (`fix/card-pictures-r13`)
- "lowk all custom icons are pixelated asf on the right pane bigger preview, fine with the small icons". Also "blurry :(" for the DJ Booth drawing, even with its 512 px `dj_booth_large` (the source is 512 x 512).
- So the blur happens in how the texture or sprite is made: filter mode, mipmaps, compression, a sprite rect or pixelsPerUnit that scales up, or the small copy being picked after all.
- Find the cause. Make every custom card's Fabricator details picture sharp, the user's drawings and the tinted or vanilla ones alike.
- Look at how a vanilla upgrade's details picture is set up (its texture size and filter) and match it.
- The log says which icon is used, so add a line if needed.

## D. Buildables and debug (`fix/buildables-r13`)
- Slime Radio physics: "the round hitbox should be more like the Fox Toy's but the egg shaped oval should be taller, otherwise nice (i think the speaker has too much weight though, doesnt seem to roll over at ALL now)". Copy the Fox Toy's collider setup with a taller egg, and lighten the mass or raise the centre of mass so it can tip and roll again.
- Debug hitboxes: "They sometimes disappear when on screen and close enough". Likely the physics-query refresh, the culling or the draw range. The log shows "showing 12 entity collider(s) within 17 m ... (285 found)".
- Quantum Storage Link: "nope, in the link still shows '[SPACE] Go To'". Hide or fix that hint row in the station-less Quantum Cloud menu.
- Log once: `[custom_upgrades] quantum link: DroneUIDisplay.UpdateShowData(null) failed: System.NullReferenceException`. Avoid that call or guard it.

## E. Sandbox spawner (`feature/sandbox-spawner-r13`)
- "in spawner menu, context menu should also have an option to spawn the default slime".
- Largos: "in largo context menus (from normal slime context menu), Clicking should result in crafting but another context menu should open to spawn a variant of a radiant slime of that largo (both slimes)". Clicking a largo entry still crafts. A submenu on it offers that largo's radiant variants (each of the two parents' radiant forms).
- Bug: "opening grid, right clicking on 4th row (5th half visible) results in the tooltip appearing under the search bar (only the 4th rows first entry) or i think only lucky slime yea, gold slime is fine". The popup for a partly scrolled-out tile is placed or clipped wrongly.
- "werent there 3 special slimes? I only see Lucky Slime and Gold Slime in spawner grid". Check the game's special slimes (SlimeDefinitions, Slimepedia). Add any that are missing and can be spawned.
- "can we use the slimepedia's radiant slime icon for them? one exists for each". Radiant entries use the Slimepedia radiant icon.
- Notes: "show gold and lucky slime in inventory but gray them out, as they cant be in vacpack (context menu should be able to be opened still)".
- Notes: "the tooltip are too large for filters now, they only show titles and the tooltips are like a mile long when 'Slimes' filter is hovered". Filter tooltips stay short.

## F. Noclip (`fix/noclip-r13`)
- "water knocks me out, remove the toggle, just have noclip key (no key by default)". Drop the Sandbox noclip switch: the key alone toggles it, and it is unbound by default. Sandbox off still turns it off.
- Fix water knocking the player out while noclipping. This may be the water or drown or knockback handling with collision off: find what the game does on entering water and suppress it while noclip is on.
- Keep `modules/common/noclip_state.h` in sync with hl2_movement.

## G. Lamps (`feature/lamps-r13`)
- "lamps shouldnt be as bright when only one speaker close and playing, otherwise works". Scale the brightness with the loudness or number of nearby sources. One speaker gives a gentle pulse.
- "Add support for 'Pink Striped Lamp', 'Conservatory Lamp', 'Blue Lava Lamp', 'Ice Lamp' and the glowing ice things in the snow map if possible". Find each decoration's identifiable and its glow or material setup, the same way as `decorElectricLampPillar`.
- The glowing ice in Powderfall Bluffs is scene geometry, not a placed decoration. Add it only if that can be done cheaply: cache it once per scene load, with no per-frame Find.

## H. Ranch > Upgrades and per-frame cost (`feature/upgrades-page-r13`)
- "move to 2 segments 'Plot upgrades' and 'Refinery upgrades', whis way we can remove thew subheadings". The Custom Upgrades card has two sections: Plot upgrades (with Tier effects and Sell back) and Refinery upgrades. Drop the subheadings.
- Perf: the log shows `frame:custom_upgrades took 2-3.5 ms` 233 times this session. `minimap`, `drone_price_limit` and `spawn_scaling` sometimes go over 2 ms too. Profile custom_upgrades' per-frame work and cut it: cache, throttle, and skip work when nothing changed. Look at the others only if the cause is obvious.

## Carry-over (untested, re-add under Skipped)
- Not tested this round: r12dj6 (walls follow the corral radio), r12lp4 (lamp follows the corral radio), r12ml6, r12nc2, r12nc3, r12pg1 (old save without the upgrade), r12pg4 (per save).
- r12ml5 is replaced by H.
- From earlier rounds: r11cr3, r11qs2-4, r7sb8, r9wx3, r9wx4, r10cr3, r10cr4, r7bd6, r9rs3, r10dj6, r10dp3, r9dp4, r9dp5, r7dr2, r4dr3, r10sh2-5, r9sh4, r9sh5, r9sh7, r8tp3, r4mm4, r6vu4, hc4, p7, ug2, ug3, sv2, ut6.
