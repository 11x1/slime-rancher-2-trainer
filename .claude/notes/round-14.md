# Round 14 plan (feedback from the round 13 test, 2026-10-25)

Build tested: 5acbad3 (develop). Results are in the work-account checklist https://claude.ai/artifact/12HrkuKyWbJs1PtsufwVvE (collection `results`, ids `r13*`; free text in `misc/notes`). User quotes are verbatim.

## How to run this round
The same as round 13 (see the auto-memory: orchestrator mode, Opus subagents at medium effort in worktrees, git workflow, no session link, plain commands, checklist conventions, read logs yourself, deploy once per round):
1. One agent per task below, up to 9 at a time. Each prompt is "Read `.claude/agent-brief.md` and follow it", the task's section from this file, and the branch name.
2. Merge each branch with `git merge --no-ff <branch> -m "Merge <branch> into develop"`. Merging doesn't build.
3. When all branches are merged, run one `.\deploy.ps1 -Build`. If the game is running, use `-WaitForExit` in the background.
4. Build the round 14 checklist from the agents' test cases (`.claude/checklist/README.md`). Publish it to the same artifact.

Passed: hitboxes up close, the programs entry in the key bar, the 3-program limit, lamp strength with speaker count, noclip key-only, noclip under the slime sea and the clouds, the greyed playlist without a Music Box, the real radio, spawn plain slime, radiant icons, Sell-All button. The log shows custom_upgrades slow frames went from 233 to 103 per session.

On hold: the release workflow (`chore/ci-release-research`, not merged; the user said to wait).

## A. Corral Radio (`feature/corral-radio-r14`)
- Notification: "Just show 'Slime DJ is off'".
- "I see a '4 radios active, waiting for one to stop' (or a similiar error) when spamming R in the radio overlay". Toggling fast must not trip the 4-radio limit (count only radios really playing, or let the toggle release its slot at once).
- "'[C] clear?' should go back to normal [C] once any other input is pressed".
- "song names should scroll when they dont fit" (marquee on overflow, e.g. while hovered or focused).
- "make the songs buttons so i can have a hint of what to press to queue a song up, play now, remove from queue". Song rows become game-style buttons (`modules/common/game_button`) with key hints for queue / play now / remove.
- "song still restarts if slime radio is disabled and enabled". Switching the radio off and on (R, and the Slime Radio toy's switch) must also keep the song position like the real radio does.
- "the key hints should be at the bottom, aswell as '[ESC] Close' on the left, others on the right".
- "No Music Box" warning pill: "needs a bit more space (margin) on y".

## B. Drone programs dialog (`fix/drone-programs-dialog-r14`)
The user's main complaint: our buttons are not the game's. Clone the game's own button and key-icon (InputIcon) objects; do not draw imitations.
- "Not using the game's buttons as rows... Use the same button style, same animations, just have the row be a higher than default buttons". The user's preferred layout: "separate the Edit and delete buttons to the start of each program row (i like this idea) as the game's buttons". So each program row starts with an Edit and a Delete game button (with the game's key icons), followed by the program's description.
- "cant use wasd/arrowkeys (so probably gamepad aswell)". Still broken after round 13: find out why from the game's navigation (EventSystem / FocusManager / Selectable navigation) and make it work.
- "'[SPACE] Add program' can be renamed to '[SPACE] Add', use the game's key icons please".
- "Next program mode selection doesnt align with options menu, rough style is there but the ingame select component looks different". Clone the game's own options-menu selector (the `< value >` component in the game's Settings), not ours.
- Key bar entry: "Move it to the leftmost position, its grayed out by default, even at 'program 1/2'". The [P] entry goes first in the key bar and must look enabled.

## C. Fabricator preview pictures still pixelated (`fix/card-pictures-r14`)
- "pixelated in the preview menu still :p, viewed in image viewer in windows and didnt look so pixelated on given size". The Quantum Storage Link one: "still pixelated, maybe using icon's texture for all our custom previews?"
- Real bug in the log: `gadget look: 'QuantumStorageLink' details picture shown = 'sr2mods_icon_dj_booth_large (sr2 tint)'`. The link shows the DJ Booth's drawing tinted. Fix that mix-up (probably a shared cache key or a stale reference).
- The round 13 mip bias change (log: 512x512, mips, trilinear, bias -1) did not help. Find out how the picture really reaches the screen: the Image's material and shader, canvas scaling, the actual on-screen pixel size, and whether a vanilla full-art picture looks smooth because it is set up differently.
- If nothing else works: make a high-quality downscaled copy (area or Lanczos filter, e.g. stb_image_resize) at the exact on-screen pixel size, with no mips, so the GPU does no scaling.

## D. Slime Radio and Quantum Link (`fix/buildables-r14`)
- Slime Radio: "the egg shape hitbox is still too big, stays stuck on the bottom hitbox's edge somehow, like it gets stuck on an EDGE HOW". Make the egg smaller. Make sure the base and the egg don't form a ledge that catches the ground: for example a rounded base, the egg reaching below the box's bottom edge, or one compound shape.
- Quantum Storage Link: "quantum cloud tab shouldnt be the default, leave it be as Drones tab, Quantum Cloud tab is fine, always was feature wise". Undo the forced Quantum Cloud tab.
- "In the DRONES menu, there is the 'Go To' bug (still exists)". In the link's Drones tab, hide or disable the "[SPACE] Go To" hint and action, since there is no station to go to.
- Log: `quantum link: TabbedControl.GoToTab failed: System.NullReferenceException` (twice). This goes away once the forced tab switch is gone. Check it does.

## E. Sandbox menus (`fix/sandbox-menus-r14`)
- Largo submenu: "width is ~44px on largo context menu now, should fit text". The radiant rows: "use the name which they belong to first so we can omit the '(radiant Type...)' text after it". That is, name the radiant variant by putting its radiant parent first (e.g. "Radiant Tabby Pink Largo" style, or the game's own radiant largo name if it has one) instead of "Pink Tabby Largo (radiant Tabby)".
- Menu position: "now the lucky slime menu opens almost at the top of the screen, and the context menu doesnt open down when there's room seems to open randomly up/down". Rule: open down-right at the cursor; open up only when there isn't room below, and left only when there isn't room right. The same goes for the vacpack picker ("only issue is the context menu positioning").
- Tooltips: "Nice, but for example beach ball is insanely long aswell now, shouldnt fit to text but like 200px max, same on resources, crates / favourite of should still make it longer in width which it does". Grid tooltips wrap at about 200 px; the "favourite of" list can stay wider.

## F. Lamps and a debug highlighter (`feature/lamps-r14`)
- "pulse lamps (electric pillar can stay rgb) in their own color light". The other lamps pulse in their own light colour, with no rainbow shift.
- "'Lamp range' option should have a tooltip saying it means the distance from a speaker".
- Powderfall glowing ice: "didnt see, maybe add a debug menu to highlight what you want me to check, which entities/buildables/map parts". Add a Debug option (Misc > Developer tools, next to the hitboxes) that highlights what the mod touches, by category: lamps that pulse, glowing ice crystals, DJ speakers in range, and so on. A wireframe box or label in the world is enough; reuse the hitbox drawing code. Log how many crystals were found. If none were found in Powderfall, find out why.

## G. Ranch > Upgrades groups (`fix/upgrades-groups-r14`)
- "Plot and refinery should be different groups". Split the Custom Upgrades card into two separate groups or cards (Plot upgrades; Refinery upgrades), in the overlay's usual group style. Keep search paths working.

## H. Map bugs (`fix/map-r14`)
- "map closes on first M press of the game in a save, have to reopen". The first M after loading a save opens the map and closes it again at once. Find which of our map hooks (map_tools / minimap / quick_travel) causes it.
- "the gray labyrinth walls show on the default map (rainbow island) when viewing default map in gray labyrinth". While in the Grey Labyrinth, switching the full map to the Rainbow Island page still draws the Labyrinth's walls (our Nullifier walls / markers, or the minimap's page art). Draw them only on their own map page.

## Carry-over (untested, re-add under Skipped)
- Not tested this round: r12ml6, r12pg4, r12nc2, r12pg1, r12dj6, r12lp4, r11qs2-4, r11cr3.
- r13lp3 (glowing ice) is retested with F's highlighter.
- From earlier rounds: r7sb8, r9wx3, r9wx4, r10cr3, r10cr4, r7bd6, r9rs3, r10dj6, r10dp3, r9dp4, r9dp5, r7dr2, r4dr3, r10sh2-5, r9sh4, r9sh5, r9sh7, r8tp3, r4mm4, r6vu4, hc4, p7, ug2, ug3, sv2, ut6.

## I. Quantum Storage Link look (`feature/quantum-link-look-r14`)
Added by the user while round 14 was running:
- "the drone has a cool effect when passing through it, can we make the quantum link have the same effect in random intervals (so it shows its quantum?)". Find the effect a drone shows when it passes through the link or the Quantum Cloud (a teleport / dissolve / glitch shader or particle), and play it on the link at random intervals.
- "the purple is too strong". Tone the purple tint down.
- "i dont like the pinging effect it has, too quick for my liking". Slow the pulse or ping down, or remove it.
- "also has a suction effect on the output (comes from the refinery link's model) maybe it's removable?" Remove or switch off the Refinery Link's suction effect on the link.
- "also the gadget name for it shows 'Refinery Link'". The placed gadget's name (the look-at label / gadget mode name) should read "Quantum Storage Link".
- "maybe the agent can even switch the small arrows to the other side or somehow take them from elder collector outputs? shows like items can go in the link which they cant". The arrow markers suggest items go into the link. Flip them to point outwards, or use the Elder Collector's output arrows.
