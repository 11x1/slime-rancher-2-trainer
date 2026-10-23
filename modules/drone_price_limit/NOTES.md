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
- The old global settings `min_price` / `min_price_fraction_of_base` (F8) are gone and ignored if
  still in `sr2mods.json`. F8 keeps only the module's on / off switch.
- A demolished station loses nothing visible; its entry stays in the sidecar (harmless, a new
  station gets a new actor id).

## Game UI (station_dialog.cpp)

- Drone station menu, Programming page, destination **Plort Market** selected: a second pill next to
  the destination's "Change" pill: `[C] Price limit` (or `Price limit: 120` / `: 80%` when set). It is
  a clone of the game's `PromptContainer` with the key drawn by hand (C) and our label.
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

## How the station is known in the drone logic

`ScorePlort.CanDroneDeposit(id, ignoreShutdown)` has no drone parameter. Its market caller is the
iterator `RanchDroneSinkProgramPlortMarket.<GetSinks>d__16`; every GetSinks use enumerates it inside
the calling method. A detour on that `MoveNext` publishes `<>4__this` (the program) while it runs;
the CanDroneDeposit detour reads `program._drone._stationModel.actorId`. The dump guard
(`RanchDroneBehaviourDumpAmmo.Relevancy`) has the drone directly.

The rest of the findings (sink programs, fast forward, the dump trap) are in the header of
`module.cpp` and `re/custom-upgrades.md` section 2.3.

## To check in game

1. Log: `[drone_price_limit] setup ok: ...` with `<GetSinks>.MoveNext`, and `station ui ready: ...`
   (any "NOT available" part is listed there).
2. Open a ranch drone station, set the destination to Plort Market: the `[C] Price limit` pill
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
