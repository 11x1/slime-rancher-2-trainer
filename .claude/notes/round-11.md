# Round 11 plan (feedback from the round 10 test, 2026-10-25)

Build tested: cc71186 (develop). Results are in the checklist artifact https://claude.ai/artifact/LVUtmDa1e6KDWD2WpQfY3V (collection `results`, ids `r10*`; free-text notes in `misc/notes`). Start with [agent-context.md](agent-context.md): it cuts what every agent below spends on finding context.

## How to run this round (for the session that picks it up)
The user's working rules are in the auto-memory (orchestrator mode, Opus subagents at medium effort, git workflow, no session link, plain commands, checklist conventions, read logs yourself). In short:
1. Back up the saves first. `deploy.ps1` does it on every deploy; for a manual backup, copy `%USERPROFILE%\AppData\LocalLow\MonomiPark\SlimeRancher2\Steam\` to `save-backups\<timestamp>`.
2. Spawn one agent per task below: `model: "opus"`, `effort: "medium"`, `isolation: "worktree"`, at most 3 at a time unless the user says otherwise. Each prompt is: "Read `.claude/agent-brief.md` and follow it", the task's section from this file, and the branch name. The brief tells the agent to start from `re/INDEX.md`.
3. When an agent reports, check its branch builds. Merge it into develop with `git merge --no-ff <branch> -m "Merge <branch> into develop"`. The post-merge hook runs `deploy.ps1 -Build`, which backs up the saves and the old DLL. If the game is running, the deploy is refused: run `.\deploy.ps1 -Build -WaitForExit` in the background. Set `SR2MODS_NO_DEPLOY=1` for docs-only merges. Never merge into master; that happens only when the user calls a build stable. `git push` is the user's job: give them `git push origin develop`.
4. Collect each agent's test cases (WHERE plus numbered steps). When all are merged, build the Round 11 checklist as described in `.claude/checklist/README.md`. Add the carry-over list at the end of this file under the Skipped fold.
5. After the user's test, read the results and write `round-12.md` the same way. Delete this file once its tasks are merged and tested.

Waves (3 agents each, tasks are independent):
- Wave 1: `fix/quick-start-crash` + `feature/sandbox-r11` (tasks 1 and 3; one agent, two branches), `feature/buildables-r11` (task 4), `feature/slime-dj-r11` (task 5).
- Wave 2: `feature/menu-nav-r11` (task 2), `fix/drone-switch-r11` (task 6), `chore/vanilla-card-icons` (task 7, with `custom-icons.md`).
Tasks 2 and 3 both touch the overlay (Sandbox tab, tooltips); if they run at the same time, the second to finish rebases onto develop.

## Tasks
Suggested agent split (one branch each, off develop). User quotes are verbatim.

## 1. Quick start crash (blocker)
- "doesnt auto load last save: Failed: FAILED: faulted: native exception 0x00005 at VERSION.dll+0x1E59CF in hook frame".
- sr2mods.log 02:39:17-18: `auto_continue: the main menu (MainMenuLandingRootUI) is not active 3 s after no scene load was running - continuing anyway`, then `module quick_start: FAULTED - native exception 0xC0000005 at VERSION.dll+0x1E5D7F in hook frame`. Round 10's 3 s fallback invokes Continue on a menu that isn't up. Map the RVA to the function (the build's .map or `dist/archive`), don't invoke while the landing menu is inactive, wait for the real ready state, and never fault in the hook.

## 2. Menu: keyboard navigation, tooltips, sidebar icons
- Shift+Tab vs Steam works ("OMG IT WORKS"). Now: "add proper shift + shift tab nav, for example navigating in upgrades grid, spamming shift tab only loops search and grid, all interactable menu elements should be working"; "checkboxes dont get focused". Every interactive element (switches, checkboxes, grid tiles, buttons, sub-tabs) must take Tab focus in order; grids should be navigable tile by tile (arrows) once entered.
- "inventory ones with no context menu options are narrow still": vacpack/picker tooltips without RMB rows still squeezed.
- "Sandbox icon doesnt match the menu's theme, too big and looks off with sharp corners, also round profiles": Sandbox sidebar icon must match the other sidebar icons (size, rounded style); round the Profiles icon too.
- "Move plot & sell upgrades under "Custom upgrades"": in Ranch > General put the Plot Upgrades and Sell Upgrades cards under the Custom Upgrades card (nested or grouped).
- Not yet tested: the Sandbox menu in a save that doesn't have Sandbox enabled.

## 3. Sandbox details
- Slot context menu: "Just "Fill" text, no amount in parenthesis".
- Gadget menu refresh FAILED: "had to refresh the tab for it to update". The round 10 re-read (`GadgetCategoryRuntime.UpdateItemList` + `NewGadgetSelectionUI.OnCategorySelected`) doesn't refresh the visible tab.

## 4. Mod buildables: models, Fabricator icons, physics
- DJ cards: "fabricator icon is still the default"; placing a booth / speaker stack shows the vanilla model for ~1 s before the dressed one ("on placement have to wait a second for it to be the correct model"). Dress on the placement event, not the next tick.
- DJ Booth: "the middle small speaker is looking up" (the face-up deck boombox); otherwise works.
- Slime Radio: "It slides so much when moved, also hitbox doesnt seem to match the entities"; "it disappears after vaccing for 1-2 secs, also music is gone once they disappear". Fix friction/collider; keep it visible and playing while held.
- New: a debug option to show entity hitboxes (colliders) under Misc > Debug Tools.
- Quantum Storage Link: "teleport effect too big, disappears in like 6-7s intervals"; "[E] should open the quantum cloud GUI menu like it shows in drones menu" (open the game's Quantum Cloud UI that the drone station uses, instead of HUD notices).
- Drone source icon for the Quantum Storage Link "is wider than normally": the tinted sprite's rect/pivot or size differs from the vanilla icons.
- Shadow Catcher: "The fabricator menu shows the vac slot amount in one stack icon not the shadow catcher": the card's icon/preview shows the vacpack-capacity art instead of the shadow slime icon.

## 5. Slime DJ
- Walls: "on a chill part of the songs, the walls still freak out like its a BANGER part". Round 10 made the level relative (falling peak / rising floor), so quiet parts get stretched to full range. Keep the snappiness for loud parts but make quiet sections stay calm (absolute loudness gate, slower floor, or blend absolute and relative energy).
- Amplify: "amplified sound should be a bit louder than base based on nearby speakers, also sound should play from each speaker" (misc notes). With "Amplify with more music boxes", loudness should rise a bit with the number of nearby speakers, and every speaker should audibly emit (spatial), not one.
- Corral Radio: "Add a button in the last empty corner (color it blue; like you did for silo's button) and let the button open a menu to build a queue from". An in-world button on the corral (the free corner post, blue like the gold Sell-All button on the silo) that opens an in-game playlist menu (game-styled, like the station dialogs) to build that corral's queue. The radio playlist cases were skipped until this exists.

## 6. Drone programs
- "did plorts program, not starting w chickens after completed plorts, dropped another plort -> woke up, collected it -> to refinery like program said": after the plort program finishes, the drone rests instead of switching to the chicken program. The idle rule (Rest + no work from MaybeHasNextGatherBehavior) apparently never triggers the switch; read the log for the switch lines.
- "keybind icons still look off, use the same look as for silo dialog menus": the dialog's key glyphs should match the silo receipt dialog's key glyphs (the mod's sell dialog).

## 7. Plots
- Music Box II card: "use the default music box icon for the upgrade, leave a plan for me to make custom ones, trying to move away from AI art". Use vanilla icons for all mod upgrade cards that currently use script-drawn icons; see [custom-icons.md](custom-icons.md).

## Carry-over (untested, re-add to the round 11 checklist)
Skipped this round: corral radio playlist / save+limits (r10cr3, r10cr4), shadow slime sun / shade / hunger / Labyrinth (r10sh2-5), drone menu-open rule (r10dp3), radio daily rotation (r10dj6). Still untested from earlier rounds: r8dt2, r9nr3, r7sb8, r9wx3, r9wx4, r7bd6, r9rs3, r9dp4, r9dp5, r9sh4, r9sh5, r9sh7, r7dr2, r4dr3, r8tp3, r8tp4, r4mm4, r7vu2, r6vu4, hc4, p7, ug2, ug3, sv2, ut6.

## Checklist conventions
Group cases by where they're tested, steps as numbered lists, skipped cases under the Skipped fold, never ask the user to send logs (read sr2mods.log / Player.log yourself).
