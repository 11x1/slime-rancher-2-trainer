# drone_price_limit

Quantum (ranch) drones only sell a plort to the Plort Market / Market Link while its current market
price is at or above **their station's** limit. Below it they keep the plort (they don't gather it,
or a drone already carrying it waits at its station instead of dumping it) and sell once the price
recovers. Also applies while sleeping / away (fast forward).

Status: built, not yet tested in game.

## Where the limit lives

- **Per station.** Key: the station's `DroneStationGadgetModel.actorId` (an `IdentifiableModel`
  field, saved as `PlacedGadgetV06.ActorId`, so stable across save / load).
- Stored in the custom_upgrades per-save sidecar (`sr2mods_saves\<save>.sav.json`, `values`):
  `"drone_price_limit.station.<actor id>": "price:120"` or `"fraction:80"` (percent of the base
  price); no entry = no limit. Written with the next game save like the custom upgrades; a load
  brings back that save's limits.
- Without custom_upgrades (disabled / failed) the limits work for the session only (log warning, and
  the dialog says so).
  Since upgrades-gate, custom_upgrades is **off by default** (switched on per save), so on a save
  without it the limits are session-only. Open question: let the per-save values work while the
  custom_upgrades gate is off (needs an api.h change in custom_upgrades).
- The old global settings `min_price` / `min_price_fraction_of_base` (F8) are gone and ignored if
  still in `sr2mods.json`. F8 keeps only the module's on / off switch.
- A demolished station loses nothing visible; its entry stays in the sidecar (harmless, a new
  station gets a new actor id).

## Game UI (station_dialog.cpp)

- Drone station menu, Programming page, destination **Plort Market** selected: a second pill next to
  the destination's "Change" pill: `[C] Limit` (or `Limit: 120` / `Limit: 80%` when set; the longer "Price limit: 200%" wrapped). It is
  a clone of the game's `PromptContainer` with the key drawn by hand (C) and our label. The clone's
  `EncapsulateChildren` fits it to the label (minimum width 0) with 22 px `_horizontalPadding` per side.
- Opens with **C**, the pad's **right stick click** (the button R3, not the stick deflection that steps
  the second Hen Collector slider; that is the plot menu, not this one), or a click on the pill. The
  dialog uses one primary slider (d-pad / left stick / arrows). The dialog:
  - title + one line of explanation;
  - "Limit by": `Minimum price` | `Fraction of base price` (click, or Left / Right on that row,
    Submit toggles);
  - a slider (the reusable `PlotMenuSlider`, `CreateIn`): price 0..max (max = 2 x the highest base
    price rounded up to 10, at least 100; step 1 / 5 / 10), fraction 0..200 % (step 5); 0 shows
    "no limit"; the label shows the unit (newbucks / % of the base price);
  - base-price cheat sheet: every plort of the market terminal's list (`MarketUIConfiguration` of the
    menu), icon, name, coin + base price; in fraction mode each cell also shows "min N";
  - footer help + Close.
- Input while open: Up / Down choose the row, Left / Right change it (the FocusManager detours, plus
  the slider's own arrow / d-pad polling on the focused row), C / Esc / pad B / Close / a click on
  the dimmed backdrop close it. The page's Start Program / Clear All hotkeys are blocked while it is
  open. Changes apply immediately.
- The dialog closes by itself when the page goes away (tab switch, destination changed, menu closed).

## Programs action and dialog (custom_upgrades' several programs per drone)

The data and the rotation are custom_upgrades' (`modules/custom_upgrades/drone_programs.h`, NOTES
"Several programs per drone"); the UI is here because this module owns the station menu hooks (one hook
per address). It shows only while custom_upgrades is on and the save owns the Drone Program Scheduler
upgrade (`drone_programs::Owned`; round 12: hidden without it, the station has one program as vanilla).

- Programming page (round 13: "Move the 'Program X/Y' to the same row as '[X] Start Program', '[Z] Clear
  All', '[ESC] Close'"; round 14: "Move it to the leftmost position, its grayed out by default"): an entry
  `[P] Programs` (one program) / `[P] Program 2/3` in the page's own key bar (`DroneProgramMenu._inputLegend`
  +0xE0), a `modules/common/legend_hint.h` clone of one of the bar's entries placed **before the bar's first
  entry**. The grey came from the template: the Start Program entry's SRButton is disabled while there is
  nothing to apply, and a clone of a disabled SRButton never runs Start, so it stayed grey; legend_hint now
  enables the clone's SRButtons (re/ui.md "InputHint.SetEnabled"). A transparent raycast Image + Toggle child
  (`LayoutElement.ignoreLayout`) makes it clickable like the game's own entries. Log `[P] Programs entry
  cloned from the page's key bar entry '...'`. The bar can rebuild its entries; a dead clone is made again on
  the next state read (~10 frames). When the bar has no entry to clone for 3 s (or legend_hint is
  unavailable): the old game-button pill right of the **target's** Change prompt (log `no [P] Programs entry
  in the page's key bar (...) - pill instead`). Opens with **P**, the pad's **left stick click** or a click.
- **Save Program** (round 14: "rename 'Start Program' to 'Save Program' makes more sense imo how the button
  actually functions"): while custom upgrades are on (`drone_programs::Available`) the key bar entry bound to
  `DroneProgramMenu._activateEvent` (found with `legend_hint::HintFor`) reads "Save Program": its
  `InputHintConfiguration.Label` LocalizedString is registered in `core/text_override` (the game's own
  LocalizeStringEvent then writes our text on every refresh), and its `_labelText` TMP is set at once. Off
  again (custom upgrades off / module shutdown): the override is removed and the old text put back. Log
  `key bar entry 'Start Program' reads "Save Program" (text override active)`. There is no separate Start
  Program button on the page (the key bar entry is the button).
- Dialog (same frame as the price-limit one). Round 14: "Not using the game's buttons as rows ... separate the
  Edit and delete buttons to the start of each program row ... as the game's buttons", "'[SPACE] Add program'
  can be renamed to '[SPACE] Add', use the game's key icons please", "Next program mode selection doesnt align
  with options menu". Rows (rounded boxes, 80 high for programs, 72 for Add / Next program):
  - one per program: two game buttons at the start, **[Space] Edit** and **[Z] Delete** (Delete greyed and
    not a focus stop with one program; it is placed after Edit's laid-out width for ~40 frames), then the
    number, the page's "active" icon on the running one (that row is tinted), target (icon + name), source,
    destination (names / icons from the menu's `DroneUIDefinitionSource`). Explorer mode stations: target and
    "Explorer drone" only;
  - **[Space] Add** (a game button) + what it does (a copy of the running one, which then runs; the dialog
    closes so it can be changed on the page; up to 3; when full a text line instead);
  - with 2+ programs (not in Explorer mode) **Next program** with a clone of the game's own options-menu
    selector (`PresetOptionsItemDefinition._controlPrefab` with an `OptionsPresetSelector`, found once per
    session, cloned with the behaviour and the value's LocalizeStringEvent off, a polled Toggle on each arrow;
    re/ui.md "OptionsPresetSelector"); logs `options selector prefab '...'` and `Next program selector cloned
    ... (rect W x H -> ..., scale S)`. Without it: the drawn white pill with teal arrows.
  - Key icons: game_button `Spec.control` (`<Keyboard>/space`, `/z`, `/p`): the game's KeyboardMouseIcon entry
    handed to `InputIcon.SetIcon`, as the key bar draws it (log `game button: key icon for '<keyboard>/space':
    the game's KeyboardMouseIcon entry`); key cap text if no entry.
  - Focus: the focused button has the game's hover look (game_button `SetFocused` = ButtonAnimator.OnPointerEnter:
    DeepBlue + bounce; re-entered after a pointer leaves it); Submit plays the press animation (OnSubmit). The
    selector row (and rows of fallback buttons) shows an opaque teal ring behind the row.
  - Input: Up / Down move between rows, Left / Right between Edit and Delete (on Next program: step the
    selector); Submit (Space / Enter / A) presses the focused button; **Z** (polled) or the page's Clear All
    event (`HandleClearAllOptions` detour, so also its pad button) deletes the focused program (at most one per
    300 ms). The pointer: hovering a button focuses it, a click presses it, an arrow click steps. P / Esc /
    Close / backdrop close. The dialog opens with the running program's Edit focused.
  - Why arrows / WASD "did not work" in rounds 12 and 13: the input arrived (FocusManagerBehaviour's move
    action -> `FocusManager.NavigateInDirection` -> our detour, re/ui.md "FocusManagerBehaviour") and the focus
    index moved, but the focus frame was an SDF shape with a fully transparent fill, which the runtime
    CanvasRenderer culls (`cullTransparentMesh`, re/unity.md), so nothing ever showed (round 12 behind the
    rows, round 13 above them). The price-limit dialog's frame had the same problem; it is opaque (light teal
    with a teal border, behind the row) now. Log `navigation input reached the programs dialog (direction N)`
    once per session.
- Edit = `drone_programs::Run` + `DroneProgramViewModel.RevertChangesToLastAppliedProgram`, so the page
  shows the program that now runs; the dialog closes. Delete rebuilds the dialog in place (focus stays on
  that row or the one above; log `program N deleted in the programs dialog`); the selector applies at once
  and only changes its text.
- Look (round 11: "use Silo's GUI buttons, theyre the best ones we have rn that look similiar to SR2's
  style"): the `[C] Limit` pill, the fallback `[P] Programs` pill and the dialogs' Close and limit-mode
  buttons are the shared game button (`modules/common/game_button.h`), the same clone of the game's popup
  button as the silo receipt's: pink pill, the game's hover colour / bounce / hover sound, key glyph on the
  left. Pills: scale 0.8, key cap C / P, placed 12 px right of the option's Change prompt in its anchor space
  (log `... action is a game popup button (key C)`). Dialog buttons: scale 0.72, selected states teal
  through our own ScriptableColor (hover stays the game's blue); Close shows C (limit dialog) / P (programs
  dialog). The programs dialog's buttons show the game's own key icons (Spec.control). Without
  the popup template: the earlier Change-prompt clones (with the receipt's glyph via `UsePopupGlyph`), then
  drawn buttons.
- While the station menu is open (any tab; a tab other than the program page counts for 2 min after the
  page was last seen, so a closed display that stays loaded can't hold it) the station's rotation waits
  (`NoteMenuOpen`).
- **"Discard program" prompt** (round 9): closing the menu runs `DroneUIDisplay.OnCancel`, which opens
  `_unappliedChangesConfirm` when the page's target / source / destination differ from the station's task
  data (`MatchesCurrentProgram` of the three sub view models). The station's program can change under an
  open menu without the player (the rotation on another tab of the menu, which did not pause it before; a
  drone source falling back to Coop; the sidecar restore). Now every state read (~10 frames) while the menu
  is open compares the station's program with the last one seen; when it changed and
  `DroneProgramViewModel.HasAnyChanges()` is true, `RevertChangesToLastAppliedProgram` makes the page show
  the station's program (log `station ui: station N: its program changed while the menu was open - the page
  shows it again`). A program the player applied with Start Program matches the page, so nothing happens;
  real unapplied edits still get the game's prompt.

To check in game (programs):

1. Log: `[custom_upgrades] drone programs ready (up to 3 per station)`, `station ui ready` without
   "page refresh ... NOT available" / "program names: NOT available".
2. Custom upgrades on, open a station: `[P] Programs` in the key bar between Clear All and Close. Custom
   upgrades off: no entry.
3. P / click: the dialog with program 1 (names and icons right, active icon). Add program: closes, entry
   `Program 2/2`, page unchanged; change the target / source / destination, Start Program: log
   `program 2 changed`.
4. Reopen: two rows, Edit on program 1 -> page shows program 1, entry `Program 1/2`.
5. Close the menu: the drone works program 1; when it has nothing to do (rests at the station), log
   `program 1 -> 2 (nothing to do)` and it works program 2; and back.
6. "After each delivery": with two programs that both have work, the drone alternates after each load.
   A drone carrying something never switches (no dumped cargo).
7. Delete the running program: the other one runs, the page shows it; down to one program: entry
   `Programs`, sidecar entry gone.
8. Save while program 2 runs, reload: log `2 programs restored from the sidecar`, program 1 runs. Load
   the save without the mod (or with custom upgrades off): the station has program 1.
9. Program 1 with the Hen Collector / Quantum Storage Link source: saved as Coop / Silo, restored with
   the true source after a load.
10. Pick the station up: log `extra programs dropped (the station is gone ...)`.
11. Close shows the P key; the running row has the page's "active" icon. Switching programs (Edit, or the
    rotation) and closing the menu without touching the page: no "discard" prompt.
12. A program that never gets cargo (e.g. Meats, Outside Plots, with chickens running around): after 90 s
    the next program runs and the log says what the drone was doing.

## How the station is known in the drone logic

`ScorePlort.CanDroneDeposit(id, ignoreShutdown)` has no drone parameter. Its market caller is the
iterator `RanchDroneSinkProgramPlortMarket.<GetSinks>d__16`; every GetSinks use enumerates it inside
the calling method. A detour on that `MoveNext` publishes `<>4__this` (the program) while it runs;
the CanDroneDeposit detour reads `program._drone._stationModel.actorId`. The dump guard
(`RanchDroneBehaviourDumpAmmo.Relevancy`) has the drone directly.

The rest of the findings (sink programs, fast forward, the dump trap) are in the header of
`module.cpp` and `re/custom-upgrades.md` section 2.3.

## Battery Saver: moved

The Drone Battery Saver is a Fabricator upgrade of custom_upgrades now (global for every station;
`modules/custom_upgrades/NOTES.md`, "Vacpack upgrades"). The station menu's `[B] Battery` pill, its
dialog and the per-station purchase are gone; this module has only the price limit again.

## To check in game

1. Log: `[drone_price_limit] setup ok: ...` with `<GetSinks>.MoveNext`, and `station ui ready: ...`
   (any "NOT available" part is listed there).
2. Open a ranch drone station, set the destination to Plort Market: the `[C] Limit` pill
   appears right of the Change pill (position, size, key glyph, font). Log line
   `price-limit action cloned ...`. Switch to another destination: it disappears.
3. Press C / click it: the dialog opens over the menu (log `dialog opened for station <id>`). Check
   the look, the cheat sheet (icons, names, prices, coin icon), mode buttons, slider by mouse,
   keyboard arrows / WASD and gamepad; C, Esc and B close it and do not close the station menu;
   Space / Enter / A on the page do not open the destination picker underneath.
4. Set a limit above the current price of a plort the drone carries: the drone stops selling it
   (log `station <id>: holding ...`), a loaded drone waits at its station. Lower it: it resumes.
5. Two stations with different limits behave independently.
6. Save, reload: the limit is still there (sidecar `values`). Load an older save: its own limits.
7. Disable custom_upgrades: limits still work for the session, the dialog shows the "Not saved" note.
