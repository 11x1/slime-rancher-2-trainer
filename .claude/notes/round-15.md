# Round 15 plan (feedback from the round 14 test, 2026-10-25)

Build tested: 082aeda (develop). Results are in the work-account checklist https://claude.ai/artifact/12HrkuKyWbJs1PtsufwVvE (collection `results`, ids `r14*`; free text in `misc/notes`). User quotes are verbatim.

## Status (2026-10-25)
All tasks A-J are merged and deployed (d1f076b, DLL 49ae59d9; one build for the round). Checklist: round 15 is published to https://claude.ai/artifact/12HrkuKyWbJs1PtsufwVvE (34 new cases in `.claude/checklist/r15-cases.json`, carried-over cases under Skipped). Open questions for the user:
- Play now from the radio's song list was merged into Queue (A); bring it back?
- The Auto-Feeder lock slider is mouse only (I).
- The upward-menu offset (F) has a placement log line to read after the test.

## How to run this round
Same as round 14 (auto-memory: orchestrator mode, Opus subagents at medium effort in worktrees, git workflow, no session link, plain commands, checklist conventions, read logs yourself, deploy once per round; agent builds take turns via build.ps1):
1. One agent per task below, up to 9 at a time. Each prompt is "Read `.claude/agent-brief.md` and follow it", the task's section, and the branch name.
2. Merge each branch with `git merge --no-ff <branch> -m "Merge <branch> into develop"`.
3. When all are merged, run one `.\deploy.ps1 -Build` (with `-WaitForExit` in the background if the game runs).
4. Build the round 15 checklist (`.claude/checklist/README.md`) and publish it to the same artifact.

Passed:
- walls dance from far away, and each corral follows its own song
- key bar entry and Save Program; navigation and buttons in the programs dialog
- Explorer mode programs
- Hen Collector vacuum after teleports
- highlighter; lamp colours and range tooltip
- Labyrinth walls only on their own map page
- all corral radio behaviour cases (notification, R spam, off/on keeps the song, Clear?, scrolling names, taller pill)
- largo radiant names (except the food note); 200 px tooltips
- upgrades search paths

The user, in notes: "OMG THE PROGRAM IS SHOWN FOR THE DRONE IN THE DRONE PREVIEW ILY ITS SO GOOD".

Shared rule for every task that shows key hints (user note): "when adding key hints, the game uses a custom icon for [SPACE] (maybe other keys aswell), make sure our UI keys match it". Use `game_button`'s `Spec.control`, added in round 14, which takes the game's own `KeyboardMouseIconSet` entry and `InputIcon.SetIcon`. Never draw our own key caps where the game has an icon.

On hold: the release workflow (`chore/ci-release-research`).

## A. Corral Radio menu layout (`feature/corral-radio-r15`)
- "we have a lot of free space at the top so we can move the columns up by like 120px, when no radio, move it down like 20px or so that the warning pill fits".
- Bottom key area as two 2x2 grids: "from left->right top->bottom: [radio, shuffle, close, <empty>] on bottom left / on the right we have (same logic) [remove/queue, play now, <empty>, clear]".
- "space and enter do the same thing on left column so those can be merged when on the left column".
- Keys: "use [Z] to remove songs, [SPACE] to play now (also the game uses a different icon for SPACE keybinds, use it)". So: playlist entry: [Z] Remove, [SPACE] Play now. Song list (left): one merged action.
- "esc keytip makes the text overflow the key, fix please". Use the game's own key icons (shared rule above).

## B. Drone programs and price-limit dialogs (`fix/drone-dialogs-r15`)
- Programs dialog (r14dp2: "Oh my god its almost perfect"):
  - "the programs row should only show the buttons enabled when in that row, otherwise gets confusing". Edit and Delete look enabled only on the focused row.
  - "the < > are too close to the option, should be at the edges and then some side padding".
  - "also looks disabled rn".
  - "option pill can be made wider as 'After each delivery' gets wrapped text atm".
- Price-limit dialog (r14dp5):
  - "Change limit by to the same component as mode selection in programs". Use the cloned game selector from the programs dialog.
  - "slider should have a speed up if key held, holding shift scrolls by +-10, holding ctrl scrolls to start/end".
  - "remove the 'Up /Down: choose Left / Right: change C or esc: close' text".

## C. Fabricator pictures still blurry (`fix/card-pictures-r15`)
User: "both look blurry still"; "custom ones look blurry and pixelated"; "quantum link shows refinery default one, DJ booth blurry / on reopen quantum preview on the right uses its own but its blurry and pixelated".

The round 14 log:
- **DJ cards:** 512 -> 440 Lanczos copy, point filter, 1:1 on screen, root canvas 'Fabricator_Root' overlay, 1920x1080, canvas scale 1, shader UI/Default. So the sprite reaching the Image is a 1:1 copy, and the user still sees blur.
- **Quantum Storage Link:** used `iconGadgetRefineryLink_small (sr2 tint)`, 128 px, upscaled to 440. That is the "pixelated" one: the 128 px card icon, not the full art. It also showed the plain Refinery picture the first time.
- **Vanilla comparison line:** logged our own tinted copy as "a vanilla card". Fix it so it really logs a vanilla card.
- **Displays:** the user has two monitors, a 1920x1080 primary and a 3440x1440 one turned to portrait. Check which display and resolution the game runs on (Player.log, Screen.currentResolution versus Screen.width/height, fullscreen mode, render scale or dynamic resolution, any FSR/upscaler in the game's options). An upscaled render or a non-native fullscreen mode would blur everything.

Find the real cause before changing the picture path again. A useful check: write our resampled copy to a PNG under the game folder (e.g. `sr2mods_debug/`). These are the user's own drawings, so this is fine to keep locally; never commit or upload it. If the PNG is sharp in an image viewer but looks soft in game, the cause is the render or display path, not our texture.
- For the Quantum Storage Link, use the Refinery Link's full art (the 512 px loaded picture), never the 128 px small icon.

## D. Slime Radio physics (`fix/slime-radio-physics-r15`)
- "it still doesnt behave naturally physics wise or how other default game items behave, after a bounce it stays locked on the bounce position (yep this is it, after 1st touch with any wall it just stops the physics for it)".
- After the first collision the radio freezes in place. Suspects:
  - our grip or anti-slide code zeroing velocity or setting kinematic or constraints on contact
  - a sleep threshold
  - the game's vacuumable or launch code re-freezing it
  - a physics material with huge friction
- Compare component by component with a vanilla toy (Fox Toy / Night Light, which the radio is built from) and match how vanilla toys behave. Log the Rigidbody state (isKinematic, constraints, sleep, velocity) for a second after the first contact.

## E. Quantum Storage Link polish (`fix/quantum-link-r15`)
- r14ql1: "[SPACE] Go To is still there but grayed out this time, magnifying glass and [SPACE] still show". `SetItemEnabled(false)` greys the hint out instead of hiding it. Hide the hint's GameObject, or remove it from the legend, so neither the key icon nor the magnifier shows.
- r14qv1: "Nice I like the effects, also looks like i want to with the arrows good job / Remove the green halo coming from the center of it and make the quantum blue effect bigger by a medium amount that's coming from the top of it".
- r14qv2: "When looking at a placed one in non-gadget placement view it still shows up as 'Refinery Link' at the top (above [T] Enter gadget mode)". The normal look-at label (not gadget mode) must read "Quantum Storage Link" too. The log confirms the gadget-mode label already works.

## F. Sandbox menu and Ranch > Upgrades layout (`fix/overlay-r15`)
- r14sp1: "when no room on the bottom now though (menu at the bottom), clicking on row half visible on the bottom of the screen, the context menu shows higher than usual and not from the icon". When the menu opens upward, its bottom edge should sit at the cursor or the clicked tile, not higher.
- r14sp2: "for food. when no favourites, dont show the 'No favourites or plorts known for this item'".
- r14ug1: "make the custom upgrades toggle show on the top right where the menu close button is, its supposed to be a subtab switch, plot upgrades group on the left refinery on the right". Move the Custom Upgrades master switch into the tab header (top right, by the close button) as the subtab's switch. Plot upgrades card in the left column, Refinery upgrades card in the right.

## G. Lamps: ice pulse and faster start (`fix/lamps-r15`)
- r14hl2: "Debug shows them but they dont seem to pulse (just the ice ones, other lights work as expected)". The log found 116 of 143 crystals in PowderfallBluffs, but they don't pulse. Probably the LightControl rewrites the light each tick (the EV100 handling), the pulse doesn't reach those lights, or the crystals' glow is a material, not the Light.
- Note: "lamps take a bit to kick in with reacting to the music". The lamp refresh runs every 4 s. Make lamps pick up a newly playing speaker within about half a second, cheaply (an event when a speaker starts, or a faster refresh only while a song starts).

## H. Map closes on open (`fix/map-open-r15`)
- r14mp1: "pressed [M], map closed instantly after 1-5 frames, after map opening works as it should".
- The round 14 log shows our icon and hint work is not the cause. It also happens on later opens, not only the first:
  - `full map closed 0.06 s (1 frames) after it opened (open #1 ...; icons / hint not started yet)`
  - `0.07 s (4 frames) ... open #5`
  - `0.35 s ... open #3`
- Right before the 1-frame close, in the same frames:
  - `drone source: "Quantum Storage Link" definition built from Silo`, which suggests the map's drone panel builds our drone sources on open.
  - `map_tools: reveal: 20 fog areas forced revealed ... 12 static fog image(s) switched off`.
- Suspects:
  - the reveal pass run in the open frame
  - the drone source build
  - an input or focus change: the close is bound to M's release
  - another module reacting to M
- Bisect with log lines or settings and find it. Delaying the reveal until after the settle period is one cheap test.

## I. Hen Collector popup and autofeeder item lock (`feature/plot-upgrades-r15`)
- Hen Collector (r14hc2): "Remove the popup when manually clicking the collector, just start sucking (dont get them in if over limit)". Activate gives no popup; the vacuum starts at once. Read "dont get them in if over limit" carefully: probably no chickens are taken in when the coop isn't over its keep limits, or when the collector's storage is full. Pick the reading that matches the existing limits and say which one you used.
- New feature (user): "Give the autofeeder an option to lock 1 item in it as always in the buy menu so drones can be filtered to put specific food items in them not just everything". In the corral's plot buy menu (the Auto-Feeder card), add a way to lock the feeder to one food item. While locked:
  - the feeder accepts only that item
  - drones whose destination is that feeder deliver only that item; others are filtered out
  - the lock is saved per plot in the sidecar

  Find how drones pick items for a feeder destination (re/drones.md, DroneProgram destination filters) and how the feeder accepts items (re_lookup SlimeFeeder). Reuse the overlay or game UI patterns (item picker, game buttons). Unlocked behaviour stays vanilla.

## Carry-over (untested, re-add under Skipped)
- r12pg4, r12nc2, r12pg1, r12lp4, r11qs2-4, r11cr3
- Earlier: r7sb8, r9wx3, r9wx4, r10cr3, r10cr4, r7bd6, r9rs3, r10dj6, r10dp3, r9dp4, r9dp5, r7dr2, r4dr3, r10sh2-5, r9sh4, r9sh5, r9sh7, r8tp3, r4mm4, r6vu4, hc4, p7, ug2, ug3, sv2, ut6

## J. New Fabricator upgrade: slime shot distance (`feature/slime-launch-upgrade-r15`)
User: "a new fabricator upgrade: Slime Punch or something (maybe less aggressive) to upgrade (max 3 tiers), shoots slimes further, in inventory and picked up largos".
- A custom vacpack upgrade in the Fabricator with 3 tiers. Build it like the existing custom upgrades (Battery Saver, Drone Program Scheduler): ids, costs, card, sidecar, Sandbox raise and lower.
- Each tier makes slimes shot from the vacpack fly further, both slimes from inventory slots and largos held in the vacuum (largos can't be stored, only held and shot).
- Name: something gentler than "Slime Punch", e.g. "Slime Launcher", "Bounce Shot", "Slime Toss". Pick one that fits the game's upgrade names and say which one you chose. Use a vanilla icon until the user draws one (see custom-icons.md).
- Find where the vacpack's shoot force or speed is applied (re/vacpack.md, re_lookup Vacuum / Shoot / launch speed). Scale it only for slimes (and largos), not for food, plorts or other items. Keep tier 0 vanilla.
