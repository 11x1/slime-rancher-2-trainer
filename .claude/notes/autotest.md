# Automated in-game tests (planned, 2026-10-25)

The user: "Can you make a test framework so you can automatically test what I've skipped (if needed flash the screen edges and ask for my input), for saves, slot 5 & 6 are unused for you to test and clean up later, will go through review and then would let you test everything else that ive skipped".

Goal: Claude (the orchestrator) drives the running game from outside, runs the checklist cases the user skipped, and asks the user in the game only when a human has to look, listen or press something. Wave 1 (this note) builds the framework and a few pilot tests. The user reviews it. Wave 2 writes and runs tests for the rest of the skipped cases.

Hard rules (same as the whole project):
- Native only inside the game: the harness is C++ in the DLL (Win32 named pipe, il2cpp_api, the ImGui overlay). No managed code, no injected tools.
- The client is offline tooling (Python 3 standard library only). It lives in the repo, never in the game folder.
- **Saves: only the games in save slots 5 and 6 may be loaded, saved, created or deleted by the harness.** Slots 1-4 are the user's real games.
- Back up the saves before every test session (copy the save folder to `save-backups\<date>_<time>_autotest`, like deploy.ps1 does).
- Game assets never leave the machine. Screenshots go to a gitignored folder and are never committed or uploaded.

## A. The harness in the DLL (`feature/autotest-harness`)

New module `modules/autotest/` (module.cpp, NOTES.md, more files as needed). Setting `Debug > Test harness` (settings::Def, default **off**). While off, no pipe exists and nothing is hooked.

**Transport.** Named pipe `\\.\pipe\sr2mods-autotest`, created with `PIPE_REJECT_REMOTE_CLIENTS` and the default (current user) security. One client at a time. The pipe runs on its own thread; it never touches Unity. Messages are one JSON object per line (UTF-8, `\n`), using `core/json`.
- Request: `{"id": 7, "cmd": "state", "args": {...}}`
- Reply: `{"id": 7, "ok": true, "result": {...}}` or `{"id": 7, "ok": false, "error": "text"}`
- Each command that touches the game runs on the main thread through the existing dispatcher (`core/main_thread`). The pipe thread waits for its result, with a timeout given per command (default 10 s). A timeout returns `ok:false, error:"timeout"`.
- Events the harness pushes without a request: `{"event": "log", "line": "..."}` (only after `subscribe_log`) and `{"event": "answer", ...}` (see ask).

**Commands (the contract; you may add more, don't change these):**
| cmd | args | result |
|---|---|---|
| `ping` | | `{build, sha, version}` |
| `state` | | scene, in_game, paused, game name, display name, slot (as the user sees it, 1-based), save number, player position and rotation, zone / region name, time of day, newbucks, open menus (names of active UI windows the mod knows), overlay open |
| `log_mark` | | a marker string written to sr2mods.log; the client reads the log after it |
| `settings_get` / `settings_set` | `key`, `value` | the setting by its settings::Def key; set applies like the Options tab does |
| `saves_list` | | every game with name, display name, slot, last save time |
| `load_slot` | `slot` (5 or 6) | loads that slot's newest save from the title screen (quit to the title first if needed) and waits until in game |
| `new_game` | `slot` (5 or 6), optional `name` | starts a new game in that slot (skip the intro if quick_start allows) and waits until in game |
| `save` | | saves now; **refused unless the current game is in slot 5 or 6** |
| `quit_to_title` | | quits without saving |
| `delete_slot` | `slot` (5 or 6) | deletes that slot's game (cleanup) |
| `teleport` | `pos [x,y,z]`, optional `rot`, or `to` a named place | moves the player (reuse quick_travel / map_tools code) |
| `find` | `type` (IL2CPP class name), optional `name` substring, `near [x,y,z]`, `radius`, `limit`, `fields [..]` | live objects: instance pointer id, GameObject name, position, the requested fields read by name through il2cpp_api |
| `probe` | `name`, `args` | calls a probe a module registered (below) |
| `exec` | `name`, `args` | calls an action a module registered (spawn, give items, buy an upgrade, open a plot menu, press a game button, set time, ...) |
| `input` | `action` (Input System action name) or `key`, `press`/`release`/`tap` | simulates player input, for cases that need a real key press |
| `screenshot` | optional `name` | grabs the back buffer in Present (BGRA -> PNG through stb_image_write, already vendored or add it) into `<game folder>\sr2mods_autotest\shots\`; returns the path |
| `ask` | `question`, `options` (2-4 labels), optional `timeout_s` | see below; returns the chosen label, the user's typed note (optional) or `"timeout"` |
| `subscribe_log` | | pushes every new sr2mods.log line as an event |

**Probes and actions registry.** `modules/autotest/api.h`: `autotest::RegisterProbe(name, fn)` and `RegisterAction(name, fn)`, where fn takes the JSON args and returns JSON (runs on the main thread). Modules register what their tests need, e.g. `feeder.lock` (per plot), `gadget_stacking.parent`, `slime_dj.lamps`. Keep registration cheap and outside hot paths; when the harness is off the registry is never called. Wave 1 adds the generic ones (spawn an identifiable by name near the player, give newbucks, set time, open a plot's menu by plot id, list plots with ids, types and upgrades, sandbox upgrade level set) by reusing the sandbox and plot_upgrades code, not copying it.

**Ask the user (`ask`).** While a question is open:
- The screen edges flash: a pulsing coloured border (about 12 px, 1 Hz, the overlay theme's accent pink) drawn by the ImGui overlay on top of everything, also when the overlay menu is closed.
- A small panel at the top centre shows "Test question" plus the question and the option buttons, with keys 1-4 for the options and an optional one-line note field. Mouse works without opening the overlay; the game's input is locked only while the cursor is over the panel or the note field is focused (use `ui/game_input` the per-action way, see the map lesson in re/hooks.md / ui/NOTES.md).
- The game window's taskbar entry flashes (`FlashWindowEx`) so the user notices when tabbed out.
- The answer goes back as the command's result. `cancel_ask` closes it.
- The case id and step number show in small text under the question (args `case`, `step`).

**Save guard.** While a client is connected ("armed"), hook nothing new for saving if an existing hook can be reused (re/hooks.md: `SaveGameImpl`, `BeginLoad`); refuse (skip, log) any save of a game outside slots 5 and 6. When the client disconnects, the guard lifts. Also work out how the save slot the user sees maps to `GameMetadata.SlotIndex` (+0x88) and write it in re/save.md; verify against the user's real saves list (read only).

**Status line.** While armed, a small "Test harness connected" tag in a screen corner, so the user knows Claude is driving.

Docs: modules/autotest/NOTES.md (protocol, safety, how to add a probe), re/hooks.md for any new hook, README settings table.

## B. The client and pilot tests (`feature/autotest-client`)

Folder `autotest/` at the repo root (Python 3, standard library only; `tools/` is gitignored, so not there):
- `sr2test.py`: the client library. Connects to the pipe (`open(r'\\.\pipe\sr2mods-autotest', 'r+b', buffering=0)` or ctypes), sends requests, matches replies by id, collects events. Helpers: `state()`, `wait_until(pred, timeout)`, `ask(question, options, case=, step=)`, `screenshot()`, `log_since(mark)` (reads `sr2mods.log` in the game folder from the marker), `player_log_since()` (Player.log).
- `run.py`: the runner. `python autotest/run.py [case ids | --all-skipped]`. Before anything: back up the saves folder (see deploy.ps1 for the path and the backup layout) and refuse to run if the backup failed. Runs each case's test, writes `autotest/results/<date>_<time>.json` (gitignored): per case `pass` / `fail` / `needs_user` / `skipped` / `error`, a note, screenshots, log excerpts. Prints a summary. Never touches slots 1-4.
- `cleanup.py`: deletes the slot 5 and 6 games (after a backup), and the screenshot folder, when asked.
- `tests/<case id>.py`: one file per checklist case, a function `run(t)` that follows the case's numbered steps (texts are in `.claude/checklist/r*-cases.json` and in the checklist html for older ids). Asserts through state / find / probe / log; uses `t.ask(...)` only for what a person must judge (looks, sound, feel). Steps that the harness can't do yet: list the missing probe or action at the top of the file and raise `NeedsHarness("...")`.
- `mock_server.py`: a fake pipe server speaking the protocol, so the client and runner can be tested without the game. Test them against it.
- `README.md`: how to run, safety rules, how to write a test.
- `coverage.md`: a table of **every** case under the "Skipped / ..." folds of round 16 (list below), with: automatic / semi (asks the user) / manual only, and the probes or actions it needs. This table drives wave 2 and is what the user reviews.

Pilot tests in wave 1 (5 cases that cover the kinds): r15sl5 (Slime Launcher kept per save: new game in slot 5, buy, save, load slot 6, back), r12pg4, r9wx3, r10cr3, r6vu4. If one of those turns out impossible, swap it for another skipped case and say why.

The protocol in section A is the contract between A and B; don't change it. If B needs another command, write it in coverage.md as "needs harness: ..." and use the mock.

Round 16 skipped cases (from `.claude/checklist/r16-new.json` sections "Skipped / ..."):
r11qs2, r11qs3, r11qs4, r12pg4, r15sl5, r12nc2, r7sb8, r9wx3, r9wx4, r12lp4, r11cr3, r10cr3, r10cr4, r7bd6, r9rs3, r10dj6, r12pg1, r10dp3, r9dp4, r9dp5, r7dr2, r4dr3, r10sh2, r10sh3, r10sh4, r10sh5, r9sh4, r9sh5, r9sh7, r8tp3, r4mm4, r6vu4, hc4, p7, ug2, ug3, sv2, ut6. Cases the user skips in the round 16 test get added after their results come in.

## After wave 1
1. Merge A and B, one deploy, the harness setting stays off by default.
2. The user reviews (code, coverage.md, the ask panel in game).
3. With the user's go: turn the setting on in the game folder's sr2mods.json, run the pilots, then wave 2 (one agent per skipped group writes the remaining tests and probes), then run them all and write results into the checklist (`results/<id>` note "auto: ...").
4. Clean up slots 5 and 6 when the user says so.

## C. Showcase screenshots (`feature/autotest-showcase`)
**Done** (not run in game yet): `autotest/showcase.py`, `autotest/showcase/`, `autotest/build_gallery.py`,
`run.py --showcase`; harness: `screenshot` hide options + `menu`, the `overlay` command. Docs in autotest/README.md
("Showcase screenshots") and modules/autotest/NOTES.md. Most shot places are saved by hand on the first run.
The user: "also during testing, make another tool to take screenshots for a showcase, would go in a new release artifact once im happy with this run".
- `autotest/showcase.py` plus a `showcase` folder of shot definitions: each shot has an id, a title, a caption, the feature it shows, and its setup through the harness (slot 5 or 6 only, teleport or a saved place, time of day, open a menu or the overlay page, spawn things). It takes the screenshot and writes `autotest/showcase_out/<date>_<time>/<id>.png` and `manifest.json` (title, caption, feature, build sha, settings used). The output folder is gitignored; screenshots are never committed.
- During test runs: `t.showcase(id, title, caption)` in a test grabs a shot when the test reaches a state worth showing. `run.py --showcase` turns these on, and they are off by default.
- Framing: an option to let the user frame the shot by hand. `ask` "Frame the shot, then press 1", and shoot after the answer with the question panel and the tag hidden.
- Harness additions, if needed: screenshot options to hide the game HUD and the mod's own HUD bits (the tag, panel and minimap tracker as asked), and an `overlay` choice for shots of the mod menu. Hidden only for the frame of the shot, then restored.
- `autotest/build_gallery.py`: builds one HTML page from a manifest (cards with title, caption, image) as the base for the release artifact later. Nothing is uploaded until the user says so (game imagery: the user decides what goes public).
- An initial set of shot definitions for the mod's headline features (Sandbox menu, Slime DJ / Corral Radio, Quantum Link, gadget stacking, plort glow, custom upgrade cards with the user's icons, minimap tracker). Read README.md for the feature list.
