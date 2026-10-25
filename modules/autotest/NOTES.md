# autotest: the in-game test harness

Lets Claude (the orchestrator) drive the running game from outside to run the checklist cases the user skipped,
and ask the user in the game only when a person must look, listen or press something. The client is offline
tooling in `autotest/` at the repo root (Python, branch `feature/autotest-client`); this module is the DLL side.
Plan: `.claude/notes/autotest.md`.

Module key `autotest`, **off by default**. Options: Misc > General > Debug Tools card > "Test harness" section
(one switch). Not in profiles (`core/profiles.cpp`): a shared profile can never open the pipe. While off: no pipe,
no threads, no frame callback, no save veto; the probe / action registry is never called.

## Files

| file | what |
|---|---|
| `module.cpp` | the Module, every pipe command, the save guard, input, find, teleport, screenshot, ask |
| `pipe.cpp/.h` | the named pipe: server thread (accept, read lines, quick commands, log tail) and command thread |
| `game.cpp/.h` | game access by name (contexts, scene loader, saves, metadata, clock, Newbucks, menus) |
| `actions.cpp` | the generic probes and actions (below) |
| `registry.cpp`, `api.h` | the probe / action registry other modules use |
| `png.cpp` | PNG encoding (stb_image_write, the only TU with its implementation) |
| `ui/autotest_view.h/.cpp` | the overlay side: "Test harness connected" tag, question panel, flashing edges, screenshots |

## Transport

`\\.\pipe\sr2mods-autotest`, `PIPE_REJECT_REMOTE_CLIENTS`, default security (the current user), one instance
(`FILE_FLAG_FIRST_PIPE_INSTANCE`), byte mode, overlapped I/O. One JSON object per line (UTF-8, `\n`), written with
`json::SerializeCompact` (added to core/json for this).

- Request `{"id": 7, "cmd": "state", "args": {...}}`; reply `{"id": 7, "ok": true, "result": ...}` or
  `{"id": 7, "ok": false, "error": "text"}`. The id is echoed as given.
- Events: `{"event": "log", "line": "..."}` after `subscribe_log` (the server thread tails `sr2mods.log` every
  100 ms, from the moment of subscribing; off again on disconnect), `{"event": "answer", "ask_id", "answer", "note"}`
  for `ask` with `"wait": false`.
- Threads: the **server thread** answers the quick commands itself (`ping`, `log_mark`, `cancel_ask`,
  `subscribe_log`, `settings_get`, `list_names`: thread-safe, no game calls) and queues the rest; the **command
  thread** runs them one at a time. Neither touches Unity: game work is posted with `main_thread::Post` and the
  command thread waits for it (50 ms polls of the cancel flag). A request whose client left is dropped; a running
  one is cancelled.
- Timeouts: every command takes `args.timeout_s`; defaults 10 s, `load_slot`/`load_save` 120, `new_game` 180,
  `quit_to_title` 60, `save`/`delete_slot` 30, `screenshot` 15, `teleport` 20. A timeout replies
  `ok:false, error:"timeout"` (a job already posted may still run afterwards).
- Multi-frame work (loading, menus, teleports) runs as a main-thread step function called every ~100 ms until done
  (`Steps` in module.cpp). Probes / actions use the same through `Retry()` (api.h).
- Errors for an unknown command / probe / action / setting contain "unknown".
- A native fault inside harness code marks the module faulted (`MarkModuleFaulted`); every command then fails with
  that reason until the game restarts.

## Commands

The contract from the plan (unchanged) plus additions (marked +). Slots are the ones the user sees (1-based).

| cmd | args | result |
|---|---|---|
| `ping` | | `{build, sha, version, save_guard, faulted}` |
| `state` | | `{scene, in_game, at_title+, loading+, paused, game_name, display_name, slot, save_number, pos, rot, zone, time, day+, world_time+, newbucks, menus, overlay_open, question_open+, save_guard+}`; game fields are null outside a game |
| `log_mark` | | `{mark}`: the exact text written to sr2mods.log (`[autotest] autotest mark N-tick`) |
| `settings_get` | `key` = `"<module>.<key>"` | `{key, value, locked, lock_reason, tooltip, label, kind, default, min, max, options?}` (also free-text values) |
| `settings_set` | `key`, `value` (bool / number) | as settings_get, after `settings::Set` on the main thread (module switches apply a tick later; `sandbox.enabled` needs no dialog). Refused for a locked setting |
| `saves_list` | | list, one entry per game: `{game_name, display_name, slot, slot_index, last_save, last_save_utc, save_number, summaries+, folder+, saves: [{save_name, file+, file_time+, save_number, time, invalid, has_summary+}]}` (newest first); see "Save list" below. `save_number` is null without a summary |
| `load_slot` | `slot` (5/6) | quits a running game to the title (no save), waits there 1.5 s, loads the slot's newest save (`BeginLoad`), waits until in game + 1 s; returns `state` |
| `load_save`+ | `slot` (5/6), `save_name` (file name, `.sav` optional) or `save_number` | the same for one given save of that slot (`save_number` needs the summaries) |
| `new_game` | `slot` (5/6), `name` (ignored), `replace`+ | same flow, then `StartNewGame(slot - 1, settings)`; refused if the slot has a game unless `replace: true` (then it is deleted first). The game names new games itself (`<time>_<slot>`, display name = the slot number), so `name` is only logged |
| `save` | | `SaveAllNow` when the game in play is in slot 5 or 6 (refused otherwise); `{save_number, slot}` |
| `quit_to_title` | | `SceneContext.OnSessionEnded` + `LoadMainMenuSceneGroup` (what the pause menu's Quit does, minus its save); waits at the title + 1 s; returns `state` |
| `quit_game`+ | | `Application.Quit` 0.5 s later; only on the title screen |
| `delete_slot` | `slot` (5/6) | `AutoSaveDirector.DeleteGame` for every game in that slot; refused while that game is loaded; `{deleted: [names], files_left+: [files still in the folder]}` |
| `teleport` | `pos` [x,y,z] (exact) or [x,z] (onto the ground), `rot` (yaw or [x,y,z]), or `to` (a saved place) | `{pos}`; exact = `ground_teleport::TeleportExact` (new, same `TeleportablePlayer.TeleportTo`), ground = map_tools' ground teleport (region loading, floor search) |
| `find` | `type`, `name`, `near`, `radius` (30 with near), `limit` (50), `fields`, `include_inactive`+ | `{objects: [{id, name, class, pos, distance, fields}], matched, scanned}`; `FindObjectsOfTypeAll` on request only; fields by name (instance fields, else a `get_<name>` getter): numbers, bools, strings, enums (int), Vector2/3, Quaternion/Color, objects as `{class, ptr, name}` |
| `probe` / `exec` | `name`, `args` | the registered function's result |
| `input` | `action` or `key`, `press` / `release` / `tap` (bools; default tap), `hold_ms`+ (80) | `key`: `SendInput` with scan codes from the command thread, refused unless the game window is in front; `action`: a game `InputEvent` by asset name or Input System action name (`InputActionController` bindings), press = `StartInput` + `Perform`, release = `Cancel`, tap = press now and release `hold_ms` later on the main thread. `probe list_inputs` lists the names |
| `screenshot` | `name`, `overlay`+ (false), `hide_hud`+, `hide_minimap`+, `hide_harness`+ (all false), `menu`+ `{cat, tab}`, `settle_ms`+ | `{path, width, height}`; `<game folder>\sr2mods_autotest\shots\<name>_<time>.png`. The back buffer of the next frame, before the overlay drew (with `overlay: true` after it). Showcase options below |
| `overlay`+ | `open` (true), `cat`, `tab` | `{open, page}`: opens / closes the mod menu, at a page (category id or title, tab title or 0-based index); waits until the menu drew that page. An unknown page fails with the list of pages |
| `ask` | `question`, `options` (2-4), `timeout_s` (600), `case`, `step`, `wait`+ (true) | `{answer, note}`: the chosen label, `"timeout"` or `"cancelled"`. `wait: false`: `{pending, ask_id}` now and an `answer` event later |
| `cancel_ask` | `ask_id`+ (optional) | `{cancelled}` |
| `subscribe_log` | `on`+ (true) | `{subscribed}` |
| `list_names`+ | | `{commands, probes, actions}` |

## Screenshots for the showcase

`autotest/showcase.py` (README there) takes release-page shots through `screenshot`. What is hidden is hidden
for the shot only and put back right after (also on errors and timeouts, `ShotRestore`):
- **The mod's own bits**: the game-only frame (`overlay: false`, before the overlay drew) never has them (tag,
  question panel, edges, HUD markers, speed panel: all ImGui). With `overlay: true` and `hide_harness`,
  `autotest_view` leaves the tag, panel and edges out of the frame it captures: `Draw` skips them while such a
  shot is pending and marks the frame (`g_frameHidden`); `ShotWanted(true)` takes only a marked frame, so a shot
  asked for between `Draw` and the capture waits one frame.
- **`hide_hud`**: `Behaviour.enabled = false` on the Canvas of HudUI's GameObject (re/ui.md, HudUI: that Canvas
  is the HUD's), plus the minimap below; `game::HideUi` / `RestoreUi`. Not `SetActive` on HudUI: its
  OnDisable / OnEnable run game logic. Only an enabled Canvas is touched; GC handles keep what was changed until
  the restore. While the game itself hides the HUD (HudUI's GameObject inactive) there is nothing to hide.
- **`hide_minimap`**: the minimap root (`minimap::api::Get` -> `Snapshot.root`, the collectibles tracker is its
  child) `SetActive(false)`; back on only if the minimap still says `visible` with the same root (the minimap sets
  its root's active state only when its own visibility changes, so it does not fight the shot).
- **`menu {cat, tab}`**: the overlay opened (`overlay::SetOpen`) and the page asked for through
  `autotest_view::RequestPage`; `menu.cpp` applies it at the top of its next `Draw` (`ApplyPageRequest`: category
  by id or title, tab by title or index) and reports back. Closed again after the shot if it was closed.
- Then `settle_ms` (default 300 when something changed, else 0) before asking for the frame, so the change is
  on screen.
One log line per shot that changed something: `screenshot '<name>': menu sandbox / Spawn & Craft, hid HUD, minimap`.

## Save guard

- **Armed** while a client is connected. A connect turns on the "Test harness connected" tag (bottom right) and the
  guard; a disconnect lifts both, closes an open question and stops the taskbar flash.
- Every game save goes through `AutoSaveDirector.SaveGameImpl`, which custom_upgrades hooks (one hook per target):
  its detour asks `custom_upgrades::SetSaveVeto`'s function first, also while custom_upgrades is off or faulted. The
  veto (main thread, own DetourGuard) reads `_currentGameMetadata`: no game -> allowed; `SlotIndex + 1` is 5 or 6 ->
  allowed; anything else -> skipped (`SaveGameImpl` returns null, which every caller handles) with a WARN line. If
  the slot can't be read while armed, the save is skipped.
- While armed the module keeps custom_upgrades' hook enabled (checked every second: switching custom_upgrades off
  disables its hooks) and switches it back off on disconnect if custom_upgrades is off. If the hook does not exist
  (custom_upgrades failed to set up) the guard is unavailable: logged, `ping`/`state` say `save_guard: false`.
- The commands check slots themselves too: `load_slot`, `load_save`, `new_game`, `delete_slot` take only 5 or 6;
  `save` only saves a game in slot 5 or 6. The client has a third check.
- Slot mapping (re/save.md): the slot the user sees = `GameMetadata.SlotIndex` / `Summary.SaveSlotIndex` + 1; the
  game name ends in `_<slot>`. Checked against the user's saves (games `_1` to `_4` in slots 1-4).
- A last check (`GuardTestSlot`) right before `BeginLoad` and before each `DeleteGame` refuses a save whose
  file-name slot is not 5 or 6, whatever picked it.

## Save list (`game::ListSaves`)

- **Source of truth: the save folder's file names.** `FileStorageProvider.savePath` (read from
  `AutoSaveDirector._storageProvider` by the field name `savePath`; if that fails, the one account folder under
  `%USERPROFILE%\AppData\LocalLow\MonomiPark\SlimeRancher2\Steam\`; several accounts = no guess). Found once, logged
  (`save folder (storage provider): ...`). Every `*.sav` named `<yyyyMMddHHmmss>_<slot>[_<k>]_<i>.sav` is a save:
  game name = the part before the last `_`, slot = the number after the 14 digits. Other files (`.cfg`, `.prf`,
  `.vdf`) are ignored. This decides which slots are occupied, and gives file names and file times without any
  internal layout.
- **Extras from the summaries.** `AutoSaveDirector.AvailableGamesByGameName()` (a `Dictionary<string,
  List<Summary>>`, walked by its `_entries`) adds display name, save number, save time and `IsInvalid`, matched by
  save name. If reading them fails, the list still works (`summaries: false`, WARN line); a summary without a file is
  dropped (the file wins).
- **Newest save** (`game::NewerThan`): the higher save number when both saves have one, else the newer file time
  (the rotation index `_<i>` is reused and says nothing about age).
- **Loading** builds the `GameSaveIdentifier { GameName, SaveName }` itself (what `Summary.get_SaveIdentifier`
  returns, IDA 0x963BD0) from the file name, so `load_slot` needs no summary. `BeginLoad` copies the struct into its
  closure at once.
- First live run (2026-10-25): `saves_list` / `new_game` failed with "unexpected Dictionary entry size 8". The layout
  was right (dump: `Entry { int hashCode; int next; String* key; List* value }` = 24 bytes, `_entries` after
  `_buckets`); the stride check was wrong: it passed the array's class to `il2cpp_class_array_element_size`, which
  wants the element class (for an SZARRAY class it answers `sizeof(void*)` = 8; same lesson as minimap/ugui.cpp
  `ElementLayout`). It now asks `il2cpp_class_get_element_class` first.

## Ask the user

`ui/autotest_view.h`. While a question is open:
- a pulsing border (12 px, ~1 Hz, the theme's accent) on the foreground draw list, also with the overlay closed;
- a panel at the top centre: "Test question", the question, "Case X, step N" in small text, the option buttons
  ("1  Label" ...), an optional note field and a hint line. Keys 1-4 (top row or numpad) answer when the game window
  is in front and no text field has the keyboard. The panel draws above the settings window;
- the window's input messages go to ImGui too (overlay.cpp WndProc), with the overlay closed as well; the game loses
  only clicks over the panel and keys while the note field types; then `game_input::Tick` locks the game's input the
  per-action way (as the overlay does), and only then;
- `FlashWindowEx(FLASHW_ALL | FLASHW_TIMERNOFG)` on the game window; stopped when answered.
In gameplay the cursor is locked in the middle of the screen, so the mouse reaches the panel only with a free
cursor (a game menu open, or the mod menu); keys 1-4 always work.

## Generic probes and actions (wave 1)

Names as the client expects them. Sandbox ones need the Sandbox on (`settings_set sandbox.enabled 1`).

| name | kind | args | does |
|---|---|---|---|
| `plots` | probe | | `{plots: [{id, type, upgrades, custom_upgrades, pos}]}`: live plots, id = `LandPlotLocation._id`, upgrades via `LandPlot.HasUpgrade` 1-20 (lower-case enum names), custom ones via `custom_upgrades::Owns` |
| `plot_menu` | action | `plot` | `OnInteract` on that plot's `LandPlotUIActivator`; waits until `LandPlotUIRoot` is active |
| `set_time` | action | `hour` (0-24) and/or `add_hours` | `TimeDirector.FastForwardTo` (as sleeping does; forward only) |
| `give_newbucks` | action | `amount` | Sandbox `AddNewbucks`; `{status, newbucks}` |
| `spawn` | action | `id` (item name as the Sandbox spawner lists it; exact, else a unique substring), `count` (1-60) | Sandbox `Spawn` in front of the player (`pos` is not supported) |
| `sandbox.upgrade_level` | action | `id` (upgrade name, e.g. `tank_booster`), `level` (-1 = not bought, 0 = tier I) | Sandbox upgrades `SetLevel` |
| `sandbox.status` | probe | | the Sandbox page's last status line |
| `place_save` | action | `name` | stores the player's position in `sr2mods_autotest\places.json` for `teleport {to}` |
| `places` | probe | | that file |
| `list_inputs` | probe | | every game input event name (with its action) for `input {action}` |

The Sandbox ones go through the bridges (`ui/sandbox_bridge.h`, `sandbox_upgrades_bridge.h`): `MarkViewed()`
makes the Sandbox publish its catalog as if the page were open, the request is the page's own, and the result is
the page's status line (two-phase through `Retry()`).

## Adding a probe or action (wave 2)

```cpp
#include "modules/autotest/api.h"
namespace at = sr2::modules::autotest;
// in Setup:
at::RegisterProbe("feeder.lock", [](const sr2::json::Value& args) {
    sr2::json::Value r = at::Obj();
    r.set("kept", at::Bool(...));
    return r;
});
```
Main thread, one call at a time; throw `at::Error("why")`; return `at::Retry()` to be called again ~100 ms later
(`at::Attempt(args)` counts). Names `<module>.<thing>`. Never load / save / delete games in one.

## Not verified (no game run)

Everything here was built and reasoned about but not run: the pipe with the Python client, `StartNewGame` with a
settings model built outside the New Game screen (which `OptionsItemDefinitionList` it picks is logged), `BeginLoad`
right after the title loads (1.5 s settle), the `InputEvent` press/release path, `SendInput` reaching the Input
System, the panel's input routing with the overlay closed, the screenshot formats (8-bit RGBA/BGRA, 10-bit and
FP16 handled), and `quit_to_title` without the save leaving the game in a clean state. The showcase options too:
whether the HudUI Canvas holds every HUD element (anything on another canvas stays in the shot), whether 300 ms is
enough for the change to reach the frame at low frame rates, and the mod menu opened from the command thread.
