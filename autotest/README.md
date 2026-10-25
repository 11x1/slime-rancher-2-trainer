# autotest: automated in-game tests

Claude drives the running game from outside through the test harness in the DLL
(`modules/autotest`, setting **Debug > Test harness**, off by default) and runs the checklist cases the
user skipped. When a person has to look or listen, the game flashes the screen edges and asks.

Python 3, standard library only, Windows (the harness is a named pipe, `\\.\pipe\sr2mods-autotest`).
Offline tooling: it lives in the repo, never in the game folder.

| file | what |
|---|---|
| `sr2test.py` | the client: connects to the pipe, sends commands, matches replies, collects events; log helpers; the client-side slot guard |
| `run.py` | the runner: backs up the saves, runs cases, writes `results/<date>_<time>.json` |
| `cleanup.py` | deletes the slot 5 and 6 test games (after a backup) and the screenshot folder |
| `mock_server.py` | a fake harness with a small simulated game; `--selftest` tests client, runner, cleanup, showcase and gallery |
| `tests/<case id>.py` | one test per checklist case |
| `coverage.md` | every round 16 skipped case: auto / semi / manual, and the probes it needs |
| `showcase.py` | showcase screenshots for a release page, from the shot definitions in `showcase/` |
| `showcase/<id>.py` | one shot definition each (`_common.py`: helpers they share) |
| `build_gallery.py` | one HTML page (cards: image, title, caption) from a showcase run's `manifest.json` |

## Safety rules

- **Save slots 1-4 are the user's games.** Tests only load, start, save and delete games in slots 5
  and 6. The client refuses (before sending) `load_slot` / `new_game` / `delete_slot` for any other
  slot, any `slot` arg outside 5-6 in a probe or action, `exec` actions named like save / load /
  delete, and `save` unless `state` says the game in play is in slot 5 or 6. The DLL has its own guard;
  this is the second one.
- `run.py` and `cleanup.py` back up the saves first and stop if the backup failed:
  `<main checkout>\save-backups\<yyyy-MM-dd_HHmmss>_autotest\` (same layout as deploy.ps1: one Steam
  account folder's files flat, several -> a subfolder each). The game folder's `sr2mods_saves\`
  (sidecars) and `sr2mods.json` go to `..._autotest_gamefolder\` next to it.
- `run.py` refuses to start while a game outside slots 5 and 6 is loaded (save it yourself and quit to
  the title, or load slot 5 / 6). It never quits the user's game without saving.
- Settings a test changes (`t.set_setting`) are put back after the case.
- Screenshots stay in `<game folder>\sr2mods_autotest\shots\`; results in `autotest/results/`
  (gitignored). Neither is ever committed or uploaded. Showcase shots and their galleries go to
  `autotest/showcase_out/` (gitignored): game imagery, published only when the user says so.

## Running

1. Turn on **Debug > Test harness** in the game (or its key in the game folder's `sr2mods.json`, see
   `modules/autotest/NOTES.md`), start the game. A "Test harness connected" tag shows while a client is on.
2. From the repo root:
   ```
   python autotest/run.py r15sl5 r9wx3      # these cases
   python autotest/run.py --all-skipped     # every round 16 skipped case (no test file -> skipped)
   python autotest/run.py --list            # cases with a test file
   python autotest/run.py -v r6vu4          # print every request and reply
   ```
   Options: `--game-dir`, `--saves-root`, `--backup-root`, `--results-dir`, `--pipe`,
   `--ask-timeout` (seconds per question, default 600). Env: `SR2_GAME_DIR`, `SR2_SAVES_ROOT`,
   `SR2_PLAYER_LOG`, `SR2TEST_PIPE`.
3. Each case ends as `pass`, `fail`, `needs_user` (the user answered "Can't tell" or did not answer),
   `skipped` (no test file, or the harness lacks a probe: the note says "needs harness: ...") or
   `error` (harness error, broken test). Parts a test leaves out are in the note as "not covered".
4. Clean up when the user says so:
   ```
   python autotest/cleanup.py                 # dry run: shows what it would delete
   python autotest/cleanup.py --yes --shots   # delete the slot 5 and 6 games and the screenshots
   ```

Test the tooling without the game: `python autotest/mock_server.py --selftest`.

## Showcase screenshots

Shots of the mod's headline features for a release page. Same safety as `run.py`: saves backed up
first (`..._autotest-showcase`), slots 5 and 6 only, refused while another slot's game is loaded,
settings put back after each shot.

```
python autotest/showcase.py --list                 # the shot definitions
python autotest/showcase.py                        # every shot
python autotest/showcase.py sandbox_menu slime_dj  # these
python autotest/showcase.py --frame                # the user frames every shot by hand
python autotest/showcase.py --no-ask               # never ask
python autotest/showcase.py --save-place showcase_corral   # remember where the player stands
python autotest/run.py --showcase r10cr3           # tests take their t.showcase(...) shots too
python autotest/build_gallery.py                   # index.html for the newest showcase run
python autotest/build_gallery.py <run> --embed --only sandbox_menu,slime_dj
```

Out: `autotest/showcase_out/<date>_<time>/<id>.png` and `manifest.json` (per shot: title, caption,
feature, status, the settings it used, its options, slot / zone / time / position; the build's
ping with its sha). A shot ends as `shot`, `skipped` (the user pressed Skip, or "needs harness: ...")
or `error`.

A shot definition, `showcase/<id>.py` (every constant is optional except the texts):

```python
TITLE = "Corral Radio"
CAPTION = "One sentence for the card."
FEATURE = "slime_dj"                       # module / feature (the gallery's tag)
SLOT = 5                                   # 5 or 6, anything else is refused before connecting
SETTINGS = {"slime_dj.enabled": True}      # set for the shot, put back after
PLACE = "showcase_corral"                  # a saved place (exec place_save) or [x, y, z]
PLACE_HINT = "Stand a few metres from the radio corral."   # asked when the place is not saved yet
ROT = None                                 # yaw or [x, y, z] (default: the saved place's)
TIME = 19.5                                # hour of day (forward only, as sleeping)
MENU = {"cat": "ranch", "tab": "Slime DJ"} # a mod menu page (an overlay shot)
PLOT_MENU = "corral"                       # open the first plot of this type's menu (or a plot id)
HIDE_HUD = True                            # the game HUD (and the minimap) off for the frame
HIDE_MINIMAP = False                       # only the minimap and the collectibles tracker
OVERLAY = False                            # take the frame after the mod's overlay drew
FRAME = False                              # always let the user frame it by hand
def setup(t): ...                          # extra steps: t is the runner's Test (spawn, exec, ...)
```

Framing by hand: the game asks "Frame the shot, then press 1" (Shoot / Skip); the shot is taken
after the answer, without the question panel and the "Test harness connected" tag. A place that is
not saved yet is framed by hand and then saved, so the next run goes there by itself. Key 1 also
reaches the game (it selects vacpack slot 1), so frame with the vacpack slot you want last.

In a test: `t.showcase(id, title, caption, feature, hide_hud=..., menu=..., frame=...)` when it
reaches a state worth showing; it does nothing unless `run.py --showcase`, and never fails the case.

The gallery (`index.html` next to the manifest; `--embed` puts the images inside one file) is the base
for the release artifact. Nothing is uploaded until the user picks the shots and says so.

## Writing a test

`tests/<case id>.py`, named after the checklist id. The docstring quotes the case's steps and lists the
probes and actions it needs that the harness may not have yet.

```python
"""r9xx1 Title. Checklist steps: ... Needs harness: probe foo.bar -> {...}"""
CASE = "r9xx1"
TITLE = "Title"
WHERE = "ranch: corral (slot 5)"        # as the checklist groups it
NEEDS = ["probe foo.bar"]               # for coverage.md and wave 2

def run(t):
    t.set_setting("custom_upgrades.enabled", True)   # restored after the case
    t.in_slot(5)                                     # load slot 5 (new game if empty); fresh=True: new game
    t.step(1, "what this step does")
    m = t.mark()                                     # marker in sr2mods.log
    t.exec("foo.do", plot="plot01")                  # module action (main thread in the game)
    t.check(t.probe("foo.bar")["ok"], "foo.bar not ok")
    t.wait_log(m, r"foo: did it", 10)                # a log line after the mark
    t.save(); t.reload()                             # save (slot 5/6 only), quit + load the same slot
    t.no_faults(m)                                   # no [ERROR], module fault or managed exception
    t.confirm("Does it look right?")                 # asks the user: Yes / No / Can't tell (+ screenshot)
```

`t` (the runner's `Test`): `step`, `note`, `uncovered`, `check`, `eq`, `state`, `wait_until`, `find`,
`teleport`, `input`, `probe`, `exec`, `setting` / `set_setting`, `mark`, `log_since`, `wait_log`,
`no_log`, `no_faults`, `screenshot`, `showcase`, `ask` / `confirm`, `game_in_slot`, `in_slot`, `save`, `reload`,
and the wave-1 generic wrappers `give_newbucks`, `set_time`, `spawn`, `plots`, `open_plot_menu`,
`set_upgrade_level`. `t.c` is the raw `sr2test.Client`.

Rules:
- Assert through state / find / probes / the logs. Ask only for what a person must judge (looks,
  sound, feel), and say exactly what to look at.
- A probe or action the harness does not have yet ends the case as skipped ("needs harness") by
  itself; list it in the docstring and `NEEDS`, and add it to the mock (`probe_<module>__<name>` /
  `exec_<module>__<name>` methods) so the test runs in the self-test.
- Never write a step that asks the user to send or open a log; read them with `t.log_since` /
  `t.c.player_log_since`.
- Leave the game in slot 5 or 6 (or at the title); never in another slot.
