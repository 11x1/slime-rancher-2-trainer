# Round 12 plan (feedback from the round 11 test, 2026-10-25)

Build tested: b39d2e5 (develop). Results are in the round 11 checklist https://claude.ai/artifact/12HrkuKyWbJs1PtsufwVvE (user's work account; collection `results`, ids `r11*`; free text in `misc/notes`). User quotes are verbatim.

## How to run this round (for the session that picks it up)
The user's working rules are in the auto-memory (orchestrator mode, Opus subagents at medium effort, git workflow, no session link, plain commands, checklist conventions, read logs yourself). In short:
1. Back up the saves first. `deploy.ps1` does it on every deploy; for a manual backup, copy `%USERPROFILE%\AppData\LocalLow\MonomiPark\SlimeRancher2\Steam\` to `save-backups\<timestamp>`.
2. Spawn one agent per task below: `model: "opus"`, `effort: "medium"`, `isolation: "worktree"`, up to 9 at a time (the user's current limit). Each prompt is: "Read `.claude/agent-brief.md` and follow it", the task's section from this file, and the branch name. The brief tells the agent to start from `re/INDEX.md`.
3. When an agent reports, check its branch builds. Merge it into develop with `git merge --no-ff <branch> -m "Merge <branch> into develop"`. The post-merge hook runs `deploy.ps1 -Build`, which backs up the saves and the old DLL. If the game is running, the deploy is refused: run `.\deploy.ps1 -Build -WaitForExit` in the background. Set `SR2MODS_NO_DEPLOY=1` for docs-only merges. Never merge into master; that happens only when the user calls a build stable. `git push` is the user's job: give them `git push origin develop`.
4. Collect each agent's test cases (WHERE plus numbered steps). When all are merged, build the Round 12 checklist as described in `.claude/checklist/README.md`. Add the carry-over list at the end of this file under the Skipped fold.
5. After the user's test, read the results and write `round-13.md` the same way. Delete this file once its tasks are merged and tested.

Passed: quick start auto-continue, Sandbox gadget refresh and Fill, all keyboard navigation cases except the grid flicker, Slime DJ calm walls, corral radio playlist and keyboard, vanilla card icons (except Sell-All), Shadow Catcher picture, Quantum Link glow / Quantum Cloud menu / source icon, the Slime Radio grip, the Mods tab icon.

## A. Corral Radio button and menu (`feature/corral-radio-r12`)
- Button corner: "it should be in the opposite corner, as right now its in the auto feeder's corner".
- Buy/sell bugs: "cant buy 2nd speaker when radio bought" (Music Box II blocked once the Corral Radio is owned); "bought radio and i cant sell speakers I again, same bug as before with there being no sell & only buy".
- Menu look: "the playlist could use more slime ranchification, we dont need keybinds when we put the list items as slime rancher's buttons (or design similiar to it, like Silo's sell all hud), what way we can remove the bottom hint, DONE PLAY UP DOWN REMOVE buttons". List rows become game-styled buttons (the silo Sell-All receipt's look); drop the key hint bar and the separate action buttons (a row's own button / small inline controls do it).
- "USE GAME's NOTIFICATIONS PLEASE": the "Slime DJ is off" text above the button and any other world text become the game's own notification popups.

## B. Slime DJ audio and walls (`feature/slime-dj-r12`)
- DJ Booth: "At spawn, when deactived, plays the default sound, should only play when its activated, sound is pretty quiet for the table's size tbh, i think only the right speaker is playing music (im infront of the table, slime sitting on other side)". Silence the vanilla boombox sound while the booth is off; louder booth; every booth speaker emits.
- Amplify: "sound is crackling a bit"; "sound seems to come from dj table only, or like its not enabled and has no slime but still plays music from it maybe? otherwise with speaker stacks seems surround enough". Fix the crackle (PCM gain / limiter / chunk boundaries); a booth that is off or has no slime must not play.
- Walls: "the walls dont dance to the corral's specific radio though (they dont move at all)": beat walls follow that corral's Corral Radio song.
- Idea from the user: "If we add speaker buildables/toy to a plot, it should amplify the walls moving aswell" (speaker stacks / boomboxes / Slime Radios inside a corral boost its walls).

## C. Game-styled buttons: silo receipt and drone dialogs (`fix/game-buttons-r12`)
- "Sell button in silo's Sell All doesnt react to hover, cacnel does".
- Drone station: "no doesnt look like silo's sell GUI cancel button, check the rendering for game's buttons; [P] has wrong style for button, same for add program"; "use Silo's GUI buttons, theyre the best ones we have rn that look similiar to SR2's style". All buttons in the drone programs dialog and the station pills use the same rendering as the silo receipt's Cancel button (key glyph included). Make it one shared helper that agent A's radio menu can reuse.

## D. Buildables and icons (`feature/buildables-r12`)
- DJ card icons: "The icons are so low quality on the right pane": the tinted icon is too low-res for the Fabricator details picture; tint the 512 px sprite (see `assets/icons/game/upgrades/` for sizes) or keep separate small/large copies.
- Slime Radio: "works, but it a bit small tbh, use the original radio hitboxes with a flattened base so they can still rroll and have collision".
- Debug hitboxes: "No hitboxes visible" (fail). Read the log; find why nothing draws.
- Quantum Cloud menu from the link: "the row shows to press [SPACE] and a magnifying glass, #watdatmean". Find what that prompt does in the station-less menu; hide it or make it work.
- Sell-All Button card icon: "Nope, uses Silo Capactity for some reason" (should be Market Link).

## E. Menu layout (`feature/menu-layout-r12`)
- Grids: "on grids, the slots should be padded themselves (outer container), and inner container should be the icon, that way there is no tooltip flickering when moving across grids over different items". The hover / tooltip area is the full padded cell, no gaps.
- Ranch > General: "Should be under 1 card, left column, Custom upgrades, rename the tab to "Upgrades" and make it a whole section, one card on the left, plot upgrades; sell upgrades; custom refinery upgrades". Rename the tab to "Upgrades"; one Custom Upgrades card in the left column holding the Plot upgrades, Sell upgrades and the refinery/drone (custom refinery) upgrades groups.

## F. Sandbox additions (`feature/sandbox-r12`)
- "add (to slime context menu) to spawn their shiny version (or whatever the cool rare types are)". Find SR2's rare variants (e.g. shiny / gilded / other rare appearance) and add a spawn entry for them.
- "noclip would be nice to have aswell as a sandbox feature (bindable to a key)". Sandbox-only, bindable through the existing keybind component, off when Sandbox is off.

## G. Drone programs behind an upgrade (`feature/drone-programs-upgrade`)
- "this should be behind a refinery upgrade tbf". The user confirmed this means several programs per station: lock that feature behind a new custom drone upgrade (bought like Battery Saver). Without the upgrade, one program (vanilla). Programs already stored in a save stay stored and come back when the upgrade is bought.

## Carry-over (untested, re-add under Skipped)
Skipped this round: r11cr3 (radio playlist after reload), r11qs2-4 (manual Continue, Shift, submenu). Still untested: r7sb8, r9wx3, r9wx4, r10cr3, r10cr4, r7bd6, r9rs3, r10dj6, r10dp3, r9dp4, r9dp5, r7dr2, r4dr3, r10sh2-5, r9sh4, r9sh5, r9sh7, r8tp3, r4mm4, r6vu4, hc4, p7, ug2, ug3, sv2, ut6.
