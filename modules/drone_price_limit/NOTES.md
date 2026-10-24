# drone_price_limit

Quantum (ranch) drones only sell a plort to the Plort Market / Market Link while its current market
price is at or above **their station's** limit. Below it they keep the plort (they don't gather it,
or a drone already carrying it waits at its station instead of dumping it) and sell once the price
recovers. Also applies while sleeping / away (fast forward).

The module also sells the per-station **Battery Saver** tiers (section below): it owns the station
menu hooks, and MinHook allows one hook per address.

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

## How the station is known in the drone logic

`ScorePlort.CanDroneDeposit(id, ignoreShutdown)` has no drone parameter. Its market caller is the
iterator `RanchDroneSinkProgramPlortMarket.<GetSinks>d__16`; every GetSinks use enumerates it inside
the calling method. A detour on that `MoveNext` publishes `<>4__this` (the program) while it runs;
the CanDroneDeposit detour reads `program._drone._stationModel.actorId`. The dump guard
(`RanchDroneBehaviourDumpAmmo.Relevancy`) has the drone directly.

The rest of the findings (sink programs, fast forward, the dump trap) are in the header of
`module.cpp` and `re/custom-upgrades.md` section 2.3.

## Battery Saver (battery.cpp, station_dialog.cpp)

A tier chain per drone station: Battery Saver I / II / III / IV, the drone uses 1/1.25, 1/1.5, 1/1.75,
1/2 of the normal battery (80 / 67 / 57 / 50 %, lasts 1.25x .. 2x as long). Only the top owned tier
counts. Shown and applied only while custom_upgrades is on (its gate); stored in the per-save sidecar
`values`: `"drone_battery.station.<actor id>": "2"`. Sell gives back half the top tier's price.

- **Where it is bought.** The drone station has no purchase menu, and the fabricator's upgrades are
  vacpack `UpgradeDefinition`s paid in craft materials and saved in the vanilla `PlayerUpgrades`
  model (a custom entry there is not save-safe). So the station menu gets a `[B] Battery` pill (label
  `Battery II` with a tier; short like the `Limit` pill so it can't wrap; the same clone of the
  Change prompt as the price limit) right after the drone's status indicators
  (DetailsPanel > DescriptionArea > StatusRow: Charge / Mode / Location, `DroneProgramMenu._batteryWidget`
  / `_modeWidget` / `_locationWidget`), with the status row's layout kept off it (`SkipLayout`). B, the
  pad's left stick click or a click opens a dialog: the four tiers (effect, price, Owned / Next), the
  player's Newbucks, Buy next / Sell top / Close (Left / Right + Enter, or the mouse; B / Esc / a click
  on the backdrop close). Payment: `PlayerState.SpendCurrency(CurrencyUtility.DefaultCurrency, price)`
  after a `GetCurrency` check; refund `AddCurrency`. Input is ignored for 0.3 s after opening.
- **Prices: 1500 / 3000 / 5000 / 7500** per station (17,000 for IV). In line with the other tier chains
  (Faster Hens 2000 / 4000, Silo Capacity 3000 / 4500 / 6500, Sell-All 10,000), each tier costing more
  than the last; drone stations are a late-game gadget, so a full set costs about as much as a fully
  upgraded silo. Less fuel is the only gain (the drones work the same, just longer per refill).
- **How the battery is changed** (header of battery.h): the battery is one saved number, the station
  model's `_energyDepleteTime`; energy = time left x `DroneDirector.EnergyDepletedPerHour`, pure world
  time. Every ~2 s the saver gives back what the station drained beyond its tier since its last look,
  through the game's own `AddEnergy` (stations from `GameModel.identifiablesByIdent[DroneDirector.
  _droneStationType]`, so unloaded ones too). The battery stays a plain vanilla value: saves load the
  same without the mod, and turning the tier / module / custom_upgrades off just stops the refunds.
- Approximation: across a sleep (one big time jump) the game's fast-forward runs the drones on the
  vanilla battery first and the refund comes after; a drone that ran empty during the night stopped
  working early, its battery afterwards is right.
- Log: `battery saver ready`, `station <id>: bought Battery Saver II for 3000 Newbucks`, `station <id>:
  Battery Saver II active (battery drain x0.67)`, `battery dialog opened / closed`, `battery saver action
  cloned ...`.

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
8. Battery Saver (custom_upgrades on): the `[B] Battery` pill sits right of the Location
   indicator under the drone description (position, size; log `battery saver action cloned ...`).
   Hidden with custom_upgrades off.
9. B / left stick click / a click opens the battery dialog; buy I (Newbucks go down by 1500, the pill
   reads "Battery I"), buy up to IV, sell back (half refund); not enough Newbucks shows the
   orange note; B / Esc / backdrop close it; Enter / A press the focused button.
10. With a tier owned the station's battery lasts longer: compare the charge indicator over a few game
   hours with a station without a tier (log `Battery Saver N active (battery drain x...)`). Save and
   reload: the tier is still there; the battery value is the same with the mod off.
