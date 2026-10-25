# custom_upgrades notes

What the module does now and what an agent needs to work on it. Framework (ids, sidecar, gate, menu
injection, world objects): `re/custom-upgrades.md`. Game facts (classes, RVAs, prefab data): the
per-system `re/*.md` files linked below. Each `.cpp` header comment has the file's details.

## Files

| file | what |
|---|---|
| `module.cpp` | upgrade table `kDefs`, menu injection, HasUpgrade / AddUpgrade, sidecar, gate, tiers, Sell-All sale, silo capacity, demolish, Activate detour |
| `api.h` | for other modules: `Owns`, `Multiplier`, `CanSell` / `Remove`, `ChainSellTarget`, per-plot / per-save values, `PlotKey`, `HiddenMenuItem`, `IsModGadget` |
| `coop_world.cpp` | Coop Air Net, Hen Collector (clone, vacuum, Activate, ranch drone linking) |
| `feeder_lock.cpp` | Auto-Feeder "Leave last food item in" (two SlimeFeeder + five SiloStorage hooks, kept feeder table) |
| `plot_menu_slider.cpp` | reusable uGUI slider for the plot menu details panel (also used by drone_price_limit) |
| `silo_world.cpp`, `silo_receipt.cpp`, `sale_notify.cpp` | Sell-All world button (and the Corral Radio button: same class, `ButtonLook`), receipt dialog, HUD notices |
| `plot_props.cpp` | decorative props for owned tiers |
| `drone_source.cpp`, `drone_programs.cpp`, `drone_battery.cpp` | drone sources, several programs per station, Battery Saver effect |
| `quantum_link.cpp`, `variant_ext.h` | Quantum Storage Link; owns the gadget-variant hooks the DJ buildables plug into |
| `dj_buildables.cpp`, `radio_shop.cpp` | DJ Booth, DJ Speaker Stack, Slime Radio; the radio in the shop |
| `gadget_look.cpp`, `buildables.cpp` | UI looks of mod gadget definitions; `buildables.h` API for the Sandbox |
| `vacpack_upgrades.cpp`, `fabricator_levels.cpp`, `shadow_catcher.cpp`, `slime_launcher.cpp` | Fabricator vacpack upgrades, extra level circles, Shadow Catcher and Slime Launcher effects |

## Plot upgrades (ids, keys)

Ids `0x5352xxxx` (`api.h kIdBase`); the key is stored per plot in the sidecar. Never reuse a retired id or
key: `0x0002 silo_sell_all_now`, `0x0010 corral_collector_2`, `0x0011 corral_collector_3`,
`0x0012 corral_feeder_2` (a sidecar that still lists one loads fine; ignored and logged).

| id | key | plot | needs | price | what |
|---|---|---|---|---|---|
| 0x0001 | `silo_sell_all` | silo | - | 10000 | Sell-All world button + receipt |
| 0x0060..63 | `silo_capacity_2..5` | silo | Storage Capacity / previous | 3000 / 4500 / 6500 / 9000 | 300 / 400 / 500 / 600 per slot |
| 0x0013 | `corral_music_2` | corral | Music Box | 2000 | Music Box II, 2x calming |
| 0x0014 | `corral_radio` | corral | Music Box | 1500 | Corral Radio (slime_dj plays the corral's own playlist); blue world button opens its playlist menu |
| 0x0020 / 21 | `coop_hens_1` / `_2` | coop | - / I | 2000 / 4000 | hens lay 1.5x / 2x |
| 0x0030 / 31 | `garden_growth_1` / `_2` | garden | Sprinkler / II | 2000 / 4000 | Sprinkler II / III, crops 1.5x / 2x (keys from the old name Fast Growth) |
| 0x0040 | `incinerator_ash_boost` | incinerator | Ash Trough | 1500 | 2x ash |
| 0x0050 | `coop_air_net` | coop | - | 425 | corral Air Net on the coop |
| 0x0051 | `coop_hen_collector` | coop | - | 1000 | Hen Collector |

Tier effects are applied by plot_upgrades through `Multiplier(obj, family)`. One card per chain
(`Shown`); a chain that starts on a vanilla upgrade shows on that upgrade's card (`ChainVanillaRoot`).
Icons: `ModIconName` uses the user's drawing `assets/icons/<name>.png` when embedded (`mod_icons::Has`),
else `iconFrom` (a vanilla card's icon). Since round 11 no card has a script-drawn icon ("trying to move
away from AI art"): Sell-All Button the Market Link's (`sell_all`), Music Box II and Corral Radio the Music
Box's (`music_2`, `corral_radio`), Faster Hens I / II the Deluxe Coop's (`hens_1`, `hens_2`), Sprinkler II /
III the Sprinkler's (`growth_1`, `growth_2`), Ash Booster the Ash Trough's (`ash_boost`), Coop Air Net the
corral Air Net's (`coop_air_net`), Silo Capacity the Storage Capacity's. The Market Link icon comes from
`LookupDirector.MarketLink`, which is null in 1.3.0 (like RefineryLink), so `MarketIcon` finds the gadget
definition named `MarketLink` in `_gadgetDefinitions` and its member groups (round 12: the card showed the
cloned template's Storage Capacity icon; log `using the Market Link icon '<sprite>' ... (definition from the
gadget groups)`). The Hen Collector has the user's
`hen_collector_source.png`, Sprinkler II the user's `growth_1.png`. A drawing under one of those names takes
over at the next build (`.claude/notes/custom-icons.md`).
Vanilla cards with a drawing (`VanillaIconName`: Sprinkler `sprinkler_1`, Music Box `music_1`): the game's own
menu item gets the drawing in its `_icon` while the module is on. `BuildInjection` remembers each such item
with its vanilla sprite (`NoteVanillaIcon`, item pinned), the menu pre-hook sets the drawing
(`ApplyVanillaIcons`, only with the text sink, i.e. when our cards show; log `vanilla card '<item>' shows the
drawing '<name>'`), `Cleanup` writes the vanilla sprite back (`RestoreVanillaIcons`, also after a fault: a
field write). The item is a shared asset, so the swap shows on every plot of that type. Our tiers that borrow
that card's icon (`iconFrom`: Sprinkler III, Music Box II, Corral Radio) read the vanilla sprite through
`ItemIcon`, not the drawing.
The swap changes only the sprite, never the card Image's rect, anchors or pivot, so the drawing's place on
the card comes from the PNG: the user draws on the vanilla 512 px canvas, and `pad_icon.py --keep-place`
keeps that canvas (1/2 scale, no crop). Round 16 cropped and centred them, which moved the Sprinkler base
(vanilla: bottom of the icon, under the water drops) to the middle and nudged the Music Box. Both are square
full-rect sprites like the vanilla ones (`PpuLike` matches the native size), so nothing else shifts them.
`growth_1` (Sprinkler II, same row) is made the same way.
Game sprites by name for our cards (`GameSpriteName`, the user's pick; a drawing under `ModIconName` wins):
the Corral Radio shows the Slimepedia's `iconPediaHomeMusicPlayer`. `GameSpriteIcons` (menu pre-hook, text
sink on) scans `Resources.FindObjectsOfTypeAll(Sprite)` while one is missing, at most 3 times per session and
20 s apart (only loaded sprites are found); found: `_icon` of the clone set, the sprite pinned and flagged
`DontUnloadUnusedAsset` (`KeepLoaded`), log `Corral Radio: card icon = the game sprite '...'`; not found: the
card keeps the Music Box icon (log `game sprite '...' not loaded (scan n of 3 ...)`). Game art is never
embedded (`assets/icons/game/` is gitignored).

Per-plot values: `hen_collector`, `hen_collector_keep_hens` / `_roosters` (plus other modules':
sell_upgrades' Ash Trough fill, `slime_dj.radio`). Per-save values: `custom_upgrades.on` (gate mark),
`custom_upgrades.vacpack.*`, `drone_*_source.stations`, `drone_programs.station.<id>`, `quantum_link.*`,
`dj.*`, `buildables.unlock.<id>`; formats in the sections below.

## Per-frame work (perf)

One frame callback (`FrameCheck`; `gadget_look.cpp` adds a second, cheap one for card art). Each part is
timed with `SR2_PERF_DETAIL` (`custom_upgrades.gate (0.5 s)`, `.battery saver`, `.world: <part>`, ...), so
the perf 10 s summary and the "> 2 ms" warnings in sr2mods.log name the slow part.
- The ~1 Hz world tick (`WorldStep`, round 13) runs one part per frame in the old order: coops, quantum
  links, DJ buildables, radio shop, drone sources, drone programs, silo buttons, radio buttons, plot props,
  silo capacity, shadow catcher, vacpack. Missing parts take no frame. Before, all of it ran in one frame
  (2-3.5 ms, ~230 warnings a session). `worldAccum_ = kWorldTickSeconds` still asks for a tick at once; it
  starts when the running one is done (at most 12 frames later).
- The ~20 ms hitches in round 12 were `drone_programs`' idle line scanning for incinerators (now cached,
  "Several programs per drone").

## Hooks owned here

MinHook allows one hook per address; other modules must not hook these:
- module.cpp: `PlortCollectorActivator.Activate` (null Collector -> never the original; Sell-All buttons,
  Corral Radio buttons, Hen Collector Activate), `LandPlot.HasUpgrade` / `AddUpgrade`, `LandPlotLocation.Replace`, the menu
  update / `RebuildUI` hooks.
- quantum_link.cpp (DJ buildables via `variant_ext.h`): `SaveReferenceTranslation.GetPersistenceId`,
  `GadgetDirector.HasBlueprint` / `GetItemCount`, `GadgetDirectorHelper.HasBlueprint`,
  `GadgetFabricateCategoryRuntime..ctor`, `GadgetFabricatableItem.get_ShouldShowPlaceholder` /
  `FabricateAndSpendCost`, `GadgetItem.PlaceGadgetEvent` / `PickupGadget`, `Gadget.ChangeVariant`,
  `StorageMetadata.Decrement`, `UIActivator.OnInteract`, `UIDisplayInteractable.OnInteract`, and (optional)
  `DroneListButton.BindItem` / `IFocusReceivedHandler.OnFocusReceived`.
- gadget_look.cpp: `GadgetDefinition.get_LocalizedDescription`, `GadgetDirector.GetTargetGadgetInfo`,
  `GadgetItem.SetHeldGadget`, `FabricatorItemDetails.SetItem`.
- dj_buildables.cpp: `GadgetMusicPlayer.OnInteract` / `ShowDeactivatePrompt`.
- drone_source.cpp: `RanchDrone.GatherSourceTask` / `GatherSourceTasks`,
  `RanchDroneSourceProgramElderCollector.GetSources`, `DroneUIDefinitionSource.FindSourceDefinition`,
  `DroneStationGadgetModel.Pull` (drone_programs asks it through `SaveFilter`).
- vacpack_upgrades.cpp: `UpgradeModel.Pull` / `SetUpgradeLevel`, `AmmoModel.Pull` / `Push`,
  `PlayerState.InitModel` / `Reset`, `PlayerUpgradeFabricateCategoryRuntime..ctor`,
  `VacuumItem.SelectSlot(InputEventData)`, `AmmoSlotViewHolder.BindInputKey`; fabricator_levels.cpp:
  `UpgradeChain.Show`, `FabricatorItemButton.ConfigurePips`.
- shadow_catcher.cpp: `AmmoSlotDefinition.IsAllowed`, `ShadowSlimeScatter` (ScatterIfPlayerNear,
  Relevancy, DoAlert, Action), `ProducePlortsOnHit.ProducePlort`.
- feeder_lock.cpp: `SlimeFeeder.ProcessFeedOperation` / `RemainingFeedOperationsFastForward`,
  `SiloStorage.CanSpecificSlotAccept` / `CanAnySlotAccept` / `MaybeAddToSpecificSlot` /
  `MaybeAddToAnySlot` / `MaybeAddAsResource` (pass-through except for a kept feeder).
- module.cpp: `ListUI.Submit` (flips the Auto-Feeder switch while it is shown; every other Submit passes).
- slime_launcher.cpp: `VacuumItem.Expel(AmmoMetadata, bool, float, bool)`, `VacuumItem.ExpelHeld`.
- radio_shop.cpp: `ShopCategoryRuntime.GetRuntimeItems`, `ShopRuntimeItem.get_Asset` / `get_Description` /
  `TryPurchase`, `ShopDirector.SpawnNextItem`.

Elsewhere: `FocusManager.NavigateInDirection` and the drone station menu (drone_price_limit), plot menu
popups and Sell buttons (sell_upgrades).

## Plot menu

RE: re/plots.md (LandPlotUIRoot, PlotUpgradePurchaseItemModel), re/ui.md (PopupPrompt,
TextButton_NoneSelectable).

- **Availability**: greyed icon, checkmark and Purchase button come from the item's `IsAvailable` Func,
  set only by `UpdateAvailability(plot)` on a menu open; `RebuildUI` (buy / sell with the menu open) does
  not call it and a fresh clone says "available". `RefreshAvailability` calls `UpdateAvailability` /
  `UpdateHidden` for every list item at the end of `BeforeMenuUpdate`. Clones copy the checkmark sprite.
- **Vanilla cards kept loaded** (round 12): every vanilla upgrade item of an injected menu gets
  `HideFlags.DontUnloadUnusedAsset` (`KeepLoaded`). A card taken out of the shared list (a tier card stands in
  for it) was referenced only by our GC handle, which does not survive `Resources.UnloadUnusedAssets`; the
  Music Box item was destroyed and the Music Box II card lost its Sell button (log `vanilla menu item of
  upgrade 2 not found`; now `... was destroyed` if it ever happens again). `RestoreVanilla` never puts a
  destroyed item back (WARN).
- **Texts** are registered in the pre hook too (a card swapped in there showed its template's text).
- **Sell / Purchase buttons** (sell_upgrades `PlaceButtons`): only pivots / positions are set (the prefab
  lays itself out). Owned card: Sell in PURCHASED's place; tier card: Sell left, Purchase right, 16 px
  gap. The Sell clone's `InputEventButton` is off (it confirmed the purchase popup in the same click); its
  key icon shows `ListUI.altSubmit`. A purchase in a sale's frame or the next is refused (log `purchase of
  ... right after a sale refused`).
- **Popup order** (sell_upgrades): upgrade, plot purchase and map expansion popups show Cancel left,
  Purchase right (one log line each).
- **Sell matching** (sell_upgrades): details matched by `_infoButtonLabel` with an owned state that fits;
  a vanilla item a tier card stands in for comes from `HiddenMenuItem`; a tier card on a vanilla upgrade
  (Music Box II, Sprinkler II, Ash Booster) sells that upgrade whenever owned; a vanilla card showing
  Purchase although owned offers Sell. Logs `no Sell button: ...` (why, once per reason), `sell offer:
  card '...' (...) sells X on plot '...' for +N`.

## Corral Radio button (round 11)

User: "Add a button in the last empty corner (color it blue; like you did for silo's button) and let the
button open a menu to build a queue from". `SiloButtons` takes a `ButtonLook` (clone name prefix, plot
type, plot-local place, tint colours, material name, log name, HUD owner); the silo uses the defaults,
`radioButtons_` uses `RadioButtonLook()`: "sr2mods_RadioButton#<gen>" on corrals that own `corral_radio`,
on the ground outside the (-x, +z) post (-5.58, 1.20, 5.58), 315 deg, below the Music Box speaker (round 12:
"it should be in the opposite corner, as right now its in the auto feeder's corner"; round 11 had it at the
feeder's (+x, -z) corner; the other corners hold the terminal and the plort collector button),
blue cap material "sr2mods_blue (HDRP/Lit)" and blue palette tint on the base. Built / removed by the ~1 Hz
tick like the silo button, removed at once when the radio is sold. A press (Activate detour ->
`OnRadioButton`): animation + cue + 0.4 s rate limit, then slime_dj `OpenRadioMenu(plot, &why)`; a refusal
("Slime DJ is off"; round 14: "Just show 'Slime DJ is off'", before a sentence about the overlay) is a game
notification with just that reason and the card's icon (no "Corral Radio: " prefix since round 14; round 12: "USE
GAME's NOTIFICATIONS PLEASE"; `Notify` = `SaleNotices::QueueText`, `CardIcon` = the injected card's
`_icon`, none before the corral menu was opened this session). The world text above the button
(`SiloButtons::Message`) is only the fallback when the HUD can't take a notice. The Hen Collector button's
result goes the same way. The menu itself:
`modules/slime_dj/NOTES.md` "Corral Radio menu". Logs `corral radio button built on plot '...'`,
`corral radio (plot '...'): playlist menu requested` / `no playlist menu (why)`.

## Demolish clears the plot

Plots are keyed by `LandPlotLocation._id`, which survives a demolish. The `LandPlotLocation.Replace` detour
drops the id's whole entry after the original (tiers, world upgrades, Hen Collector contents and limits,
other modules' per-plot values); props, Sell-All button and Hen Collector die with the old plot. Written
with the next game save, so a reload without saving restores both. Log `plot '...' demolished / replaced
(old type T, stored type T): dropped owned [...], values [...]`.

## Coop: Hen Collector and Air Net

RE: re/plots.md (patchCoop corners, PlortCollector, PlortCollectorActivator, CoopRegion),
re/custom-upgrades.md 2.1 / 2.4.

- **Air Net**: the corral's `Base Air Net` cloned under the coop (`sr2mods_CoopAirNet#<gen>`), fitted every
  tick to the top and footprint of the active CoopRegion (High Walls lift it). Round 16: the fit works from
  the clone root's transform and the net collider's offsets read once from a fresh clone (`NetGeom`,
  `MeasureNet`, log `net bottom N above the plot root`), not from the live collider bounds. Those stayed
  stale for many ticks (round 15 log: the same "moved 0.00, 5.14, 0.00" every second, then `fit offset
  (0.00, -190.33, 0.00) too large` 108 times): each tick moved the root again, the net drifted ~190 m up
  until the bounds caught up. The too-large warning is logged once per coop.
- **Description** (round 16, "should be generic upgrade text, nothing to do with the configuration of
  it"): a fixed text; the keep-limits only show on the card's sliders.
- **Hen Collector clone**: `Deluxe` cloned under the coop, TV removed, both slots one session clone of the
  elder slot definition (100 each, group MeatGroup, block list ElderGroup + UnstableMeat), local storage
  with its own `AmmoModel`, never linked to the plot model. Contents in the sidecar (`hen_collector`:
  `[{type, count}]` per slot), copied every tick and before every save. Selling refused while it holds
  chickens.
- **Placement**: the prefab's `Deluxe` container is turned -90 deg, so the vanilla elder collector sits in
  (+x, +z). `PickCorner` reads the occupied corners from the live plot and turns the clone by a multiple of
  90 deg into the first free one, (+x, -z) first. Log `hen collector corner on plot '...': ...`; placement
  lines give the plot-local centre and `vanilla deluxe parts built / not built`.
- **Vacuum**: the clone's `PlortCollector` runs with a private `LandPlotModel` (collectorNextTime 1e300,
  never registered); the ~1 Hz tick counts adults in the region (MeatGroup - ElderGroup - UnstableMeat, not
  held; chicks never) and, when the 6 game hour period is due, builds DoCollection's joints for the excess
  only; the vanilla FixedUpdate reels them in. Joints are released before a clone is removed. Fallback
  without those parts: direct store + puff.
  **Don't regress (twice now):** the parts (`_region`, `_timeDir`, `_storage`, `_vacAudio`) are set by
  Awake, which only runs once the clone is active in the hierarchy. A clone built while the plot is
  inactive (rebuild right after a map teleport) has them null, so the build must not decide "stored
  directly" for good: `CreateHen` only enables the vacuum when `activeInHierarchy`, and `ServeHen`
  retries `EnableVacuum` every tick while the clone is active and not yet vacuuming (log `vacuum switched
  on after activation`; the missing-parts WARN only after the warm-up). Build line says `inactive: vacuum
  switched on once active` in that case.
- **Limits**: hens 0..58 (default 18), roosters 0..20 (default 2) per coop; `hen_collector_keep_hens` /
  `_roosters` (an old `hen_collector_keep` n = min(2, n) roosters, rest hens). Two `PlotMenuSlider`s in one
  row (`SliderSpec.columns`, `Create(..., rowOf)`), **mouse only** (`keys = false`, gamepad later);
  `PlotMenuSlider` keeps pad / key support for drone_price_limit.
- **Activate button**: the vanilla collector button is kept (Collector null) while the Activate detour is
  installed (`CoopWorld::KeepHenButton`; build line `Activate button kept` / `no Activate button`). A press
  runs the excess-only vacuum at once (the 6 h period restarts) with animation, cue and 0.4 s rate limit
  (`SiloButtons::Press`). Round 15 (user: "Remove the popup when manually clicking the collector, just start
  sucking (dont get them in if over limit)"): no notification and no world text any more; every press spins
  the cyclone and plays the vac sound for the vanilla minimum (`_forceCollectUntil` = HoursFromNow(1/12), as
  DoCollection does), and only the chickens over the coop's keep-limits are pulled in, as long as the
  2 x 100 storage has room ("over limit" read as the storage limit: nothing goes in once it is full; the
  keep-limits stay as before). The result ("Collecting N chickens", "Nothing to collect (keeps H hens /
  R roosters)", "The Hen Collector is full", "Already collecting") is only logged (`hen collector button:`).
- **Ranch drones (Coop)**: the tick appends the Hen Collector's two slots to the coop's
  `RanchMetadata ... elderCollectors` (`LinkDrones`, log `linked to the ranch drones`); every Register
  replaces the entry, so the next tick adds them again. Sold / not owned / off:
  `RanchMetadata.OnUpgradesChanged(plot)` (`unlinked from the ranch drones`). Coop with a meat / hen /
  rooster target thus also empties the Hen Collector, fast-forward included. Edge: turning the module off
  while a drone flies to a port that still holds chickens leaves it a destroyed port (as vanilla on a
  deluxe coop demolish). RE: re/drones.md (RanchMetadata).

## Auto-Feeder: Leave last food item in (round 16)

Round 15 was a food picker (user: "Give the autofeeder an option to lock 1 item in it as always in the buy
menu so drones can be filtered to put specific food items in them not just everything"); on its test the
user said "No no no no no, it should be just a toggle, 'Leave last food item in'". RE: re/plots.md
"SlimeFeeder", re/drones.md "Feeder sink".
- **Menu**: in a corral's plot menu, while the OWNED vanilla Auto-Feeder card is selected, an on/off switch
  sits in the cost row's slot: a two-position `PlotMenuSlider` ("FeederKeepLast", 0 = off, 1 = on), half
  the panel wide (`columns = 2`), label "Leave last food item in: On / Off". The plot menu has no toggle
  widget of its own (only the game's buttons, and our sliders), so the switch is our slider at two
  positions: handle left = off, handle right with the pink fill = on. Mouse: click the other half or drag.
  Pad A / Enter: `ListUI.Submit` (the list menus' submit listener; on an owned card its OnAction does
  nothing) flips it while it is shown (`PlotMenuSlider::Flip`, one flip per frame, not under a PopupPrompt);
  left / right stay with the menu's grid (`keys = false`). The card is found by its `_description`
  instance among the corral menu's items (`_upgrade` FEEDER 9). Not offered for an unbought feeder (no
  active SlimeFeeder, no storage).
- **Save**: per plot `PlotValue` `custom_upgrades.feeder_lock` = "1" (on; "" = off, nothing stored). Round
  15 stored a food's asset name under the same key: it reads as on. Never in the vanilla save.
- **Feeding out** (`feeder_lock.cpp`): every feed is `SlimeFeeder.ProcessFeedOperation(ejectFood)`
  (FixedUpdate with true, fast-forward with false). With the switch on and at most one item in the feeder,
  the detour runs vanilla's empty path only (`remainingFeedOperations` - 1, nothing fed), so the last item
  stays. The fast-forward source (`RanchCellFastForwarder.FeedingSource.AutoFeeder`) takes its budget from
  `RemainingFeedOperationsFastForward` = min(pending, count): the detour makes it min(that, count - 1), so a
  fast-forward never counts the kept item as eaten. Log once per feeder: `auto-feeder kept its last food item
  in (feeding / fast-forward)`.
- **Putting food in**: the feeder slot holds one food type, and with one item always in it keeps that type.
  The round 15 SiloStorage hooks (CanSpecificSlotAccept, CanAnySlotAccept, MaybeAddToSpecificSlot,
  MaybeAddToAnySlot, MaybeAddAsResource) refuse any other food on a kept feeder's storage, with the kept
  food = the feeder's current food (`GetFoodId` while `GetFoodCount` > 0, read by the ~1 Hz tick and at a
  switch change). So drones (GetSinks) and shots add only more of the same food. An empty feeder with the
  switch on takes any food; the first one becomes the kept food. Logs: `auto-feeder on plot '<id>': leave
  last food item in ON, keeps '<food>'` (or `empty`) / `OFF`, `auto-feeder keeping its last food refused
  '<food>' (...)` once per food.
- **Switch off**: the next feeds take the last item as vanilla; an empty feeder takes any food.
- World tick part 12 hands the corrals with the switch on to `FeederLock::Tick` (table of feeder, storage,
  kept food); `SetNow` applies a change from the menu at once. Feeders without the switch, and every other
  storage, go straight to the originals (one atomic load while no feeder keeps one).
- Module off / load / fault: `Cleanup` empties the table (vanilla feeders); the next tick rebuilds it from the
  loaded save's sidecar.

## Drone sources Hen Collector / Elder Collector

`drone_source.cpp`. RE: re/drones.md (GatherSourceTask(s), source picker, DroneStationGadgetModel.Pull).

- Two sources right after Coop. Hen Collector takes only from Hen Collector ports, Elder Collector only
  from the deluxe coop's vanilla elder collector; neither takes live chickens; Coop takes everything.
- **Offered** when the target's predicate wants a type the kind holds (Hen: `MeatTypeGroup` - `ElderGroup` -
  UnstableMeat; Elder: `ElderGroup`, found once per session, log `elder types for Elder Collector: ...`) and
  a storage of the kind exists.
- In memory the program has `0x53520101` / `0x53520102`. `GatherSourceTask(ours)` = one new
  `RanchDroneSourceProgramElderCollector` whose `GetSources` returns only that kind's slots;
  `FindSourceDefinition(ours)` = a copy of Coop's definition (sort index Coop + 1).
- **Icons**: `hen_collector_source.png` / `elder_collector_source.png` when embedded (both are in
  `assets/icons`), else `HenType.icon` / the elder hen; the card and the source share one cached sprite.
- **Save**: the `Pull` detour gives the save a copy saying COOP (no copy possible -> the live data becomes
  Coop, logged). Sidecar `drone_hen_source.stations` / `drone_elder_source.stations` = station actor ids.
- **Load**: the first tick that sees a listed station with a Coop program switches it back
  (`AssignRanchTask`, else the data); Elder Collector waits until a drone was seen. Log `restored from the
  sidecar`.
- **Stale / migration**: listed stations whose program changed, or ids no station has (re-placed = new
  id), are dropped; no storage of the kind left -> Coop (logged); a Hen Collector program with an elder
  target becomes Elder Collector (or Coop).
- **Off / faulted**: `OnDisable` writes Coop into our programs (field writes, before the hooks go off),
  `Cleanup` re-programs the drones; on again they are mapped back. Without the mod: Coop programs.

## Several programs per drone

`drone_programs.cpp`; UI (`[P] Programs` key bar entry and dialog) in drone_price_limit `station_dialog.cpp`, only
while custom upgrades are on and the save owns the **Drone Program Scheduler** (below). RE: re/drones.md
(AssignRanchTask, DroneBehaviorPlexer, RanchDroneSourceProgramDynamic).

- **Upgrade** (round 12, "this should be behind a refinery upgrade"): Fabricator card Drone Program
  Scheduler (`vacpack_upgrades.cpp`, "Vacpack upgrades" below). Without it: the pill is hidden, Add /
  Remove / Run / SetMode refuse (`drone_programs::Owned`), no rotation. Stored programs are not touched:
  `Load` still restores the entry (program 1 with its true source runs; page edits keep updating program 1,
  so the sidecar matches the save), logged `station N: K programs stored in the sidecar, inactive until the
  Drone Program Scheduler is bought`. Once owned they take turns again (`... owned now: stored programs take
  turns again`). Lowered again (Sandbox): every station back on program 1 at once, even with cargo (rare;
  `... not owned (any more): ...`). The tick waits while the state is unknown (save loading). Without the
  vacpack upgrades (setup failed, card not created) the feature is not locked.

- Up to 3 programs per station; the game keeps one (`_taskData`); switching = `DroneStation.AssignRanchTask`
  (what Start Program does; the page shows "Save Program" while custom upgrades are on, round 14).
- **Explorer mode** (round 14: "can't add multiple programs to a explorer mode drone as it has no source and
  destination"): a program is its target only (`ProgramData::ValidFor(explorer)`, `drones::IsExplorer` =
  `DroneStationGadgetModel._type` EXPLORER_DRONE); switching = `DroneStation.AssignExplorerTask` (the station
  object found by `_model`, re/drones.md "Explorer mode stations"), log `explorer station N program set (...;
  explorer drone re-programmed)`. No rotation for explorer stations (the explorer drone gathers non-stop; the
  rotation watches the RanchDrone): the player picks the program with Edit; the dialog has no Next program
  selector there. A station switched between modes keeps its stored programs (a ranch program on an explorer
  station only uses its target).
- **When idle** (default): behaviour Rest, ammo empty, `MaybeHasNextGatherBehavior()` false for 2 ticks.
  The at-station flourish neither counts nor resets. **After each delivery**: also when the ammo went full
  -> empty. Never with cargo (DumpAmmo would destroy it), without battery, or with the station menu open
  (`NoteMenuOpen`). Each program runs >= 5 s; after a whole idle round the next try waits 20 s.
- **Stuck**: working with no cargo for 90 s (`kStuckMs`) = nothing to do; logged with the behaviour class
  and `GetProgramStateStr`.
- Log `drone programs: station N: program 1 -> 2 (nothing to do)`, at most one per station per 30 s.
  The first idle switch of a program since the drone last held cargo is always logged with the reason:
  `program 1 -> 2 (nothing to do: program 1 (target .. source .. sink ..) - <reason>)`. For an Incinerator
  destination the reason counts the incinerators with room in their Ash Trough (a drone only burns into
  one whose Ash Trough is built and not full: re/drones.md "Incinerator sink"); otherwise "the game finds
  nothing to gather that the destination has room for". The incinerators come from one
  `FindObjectsOfTypeAll(Incinerate)` kept as GC handles, rescanned at most every 5 min when one is gone or
  none was found (round 13: the scan per line cost ~20 ms and a resting drone logs this every ~1.5 min).
- Round 10 "after the plort program it rests instead of doing the chicken program": the log had the
  switches (`program 2 -> 1 (nothing to do)` every ~40 s); program 1 was Meats / Outside Plots ->
  Incinerator and had nothing to do by the game's own rule (`MaybeHasNextGatherBehavior` false), most
  likely a full Ash Trough. The drone rested on it 20 s, went back to the plort program, and so on.
- **Editing**: the page shows the running program; Start Program changes that one (`program N changed`),
  as does a game fallback (Hen Collector -> Coop).
- **Save**: the vanilla save holds program 1 (source mapped to Coop / Silo when ours). Sidecar
  `drone_programs.station.<actor id>` = `v1;idle|delivery;<t>:<ident asset name or ->:<source>:<sink>|...`,
  only for stations with 2+ programs.
- **Load**: program 1 vs the save's program (source modulo the mapping): same -> restored (`N programs
  restored from the sidecar`), else extra programs dropped (logged).
- Stations with an entry are skipped by the source lists (`ManagedFilter`).
- **Off / faulted**: program 1 back everywhere; the sidecar stays.
- Limits: fast-forward runs only the current program; a drone holding plorts back for drone_price_limit
  stays on its program.

## Quantum Storage Link

`quantum_link.cpp` (+ source in `drone_source.cpp`). RE: re/fabricator.md (GadgetDirector variants,
GadgetItem, Fabricator categories), re/save.md (GetPersistenceId, `_reverseIndex`), re/drones.md
(Quantum Cloud, StorageMetadata).

- **Quantum storage = the Quantum Cloud** (`DroneCloudModel._amounts`).
- **Gadget**: runtime clone of the Refinery Link definition (`QuantumStorageLink`), `VariantOf` = Refinery
  Link, same prefab, gradient purple -> cyan, own texts. A placed one IS a vanilla Refinery Link; variants
  share the base's inventory, so ours never enters `_itemCounts` / blueprints. Registered every tick in the
  live `_gadgetVariantMap`. If `LookupDirector.RefineryLink` is null: the `RefineryLink` definition from
  the gadget groups (logged; WARN after 30 ticks).
- **Sidecar**: `quantum_link.actors` (quantum actor ids), `quantum_link.kits` (unplaced).
- **Favourites** hold the variant and are saved: `_reverseIndex` alias to the Refinery Link's persistence
  id (every tick) + `GetPersistenceId` detour; Cleanup puts the Refinery Link back; after a load the slot
  shows the Refinery Link.
- **Fabricator**: card after the Refinery Link (Warp Tech), cost = Refinery Link's with 3x Newbucks, 2x
  items. Fabricating: `CheckCanFabricate`, `Spend`, `AddItem(Refinery Link, n)`, kits += n (never the
  vanilla body). Owned count `min(kits, Refinery Links)`.
- **Placing**: hold a Refinery Link, toggle-variant (offered while a kit is left and the card unlocked),
  place: id listed, kit used (`Quantum Storage Link placed`); no kit -> plain (logged). Pick-up gives the
  kit back and holds the variant; removed otherwise, the tick gives it back. Toggle on a placed one flips
  quantum <-> plain in place.
- **Look** (tick): materials instanced, colours 30 % purple (round 14, was 60 %: "the purple is too strong";
  the placement preview too), emission cyan, tagged ` [sr2q]` (originals back when plain). Holder
  `sr2qlink#<gen>`: a teleporter `_activeFX` clone (0.60 size; round 15: 0.22 -> 0.30, "make the quantum blue effect
  bigger by a medium amount"; round 16: 0.30 -> 0.60, "The blue effect could be doubled"; 1 m up; its green `Point light` and green `Waves` particles destroyed, round 15:
  "Remove the green halo coming from the center of it", `DropGreen`, re/fabricator.md "Teleporter FX"; its child particle systems in Local scaling
  mode get the factor on their own transforms, `ScaleParticles`) that never switches off (round 11: the old
  drop-out restarted the particles, "disappears in like 6-7s intervals"). Icon: a purple copy of the Refinery Link
  sprite (`TintedSprite`) in `icon`; `quantum::SourceIcon()` = the same on a square canvas (round 11: the drone
  source icon "is wider than normally").
  Round 14 (re/drones.md "Drone phase-through pass", re/fabricator.md "Refinery Link prefab"):
  - **Quantum moments** every 14-32 s (`Glitch`, `QuantumLink::Frame`; was every 3.5-8 s, "too quick for my
    liking"): the glow jitters ~0.5 s and the **phase ghost** shows 0.9-1.6 s ("the drone has a cool effect when
    passing through it, can we make the quantum link have the same effect in random intervals"). The ghost
    (`sr2qghost`, `BuildGhost`) = a copy of `model_refineryLink` under the holder, renderers only (colliders,
    scripts, particles gone), 0.97 of its size so it sits just inside the link, its renderers' `renderingLayerMask`
    | `1 << 16`: the ranch drones' `DronePhaseThroughCustomPass` draws it like a drone inside the link (scanlines,
    cyan edge where the ghost crosses the surface); nudged a few cm at every flip. The pass volume is active only
    while the scene group has a drone station: `PhaseOn` switches it on when off and `PhaseOff` back
    (`DroneCustomPassController._phaseThroughCustomPassVolume`, looked up with FindObjectsOfTypeAll at most every
    30 s while missing). Logs `phase ghost = a copy of ...`, `drone phase-through pass volume '...' (on|off now ...)`.
    Not seen in game: how strong the overlay looks on a ghost fully inside its occluder.
    Round 16 ("the 'glitch' effect has a 'chams' look white, not very transparent, just feels like a model copy
    glitching"): the pass's fullscreen material is shared with the drones, so the ghost itself changed: 0.90 of the
    model (was 0.97; the intersection edge, HDR cyan ~350 within `_DepthDifference` 1.8 m, likely covered the whole
    copy hugging the surface), shown 0.6-1.1 s (was 0.9-1.6), visible on 45 % of the 30-80 ms flips only (a flicker,
    `kGhostOnChance`), and its own materials (`GhostMaterials`: `new Material(copy)` of the link's tinted ones set as
    the ghost's shared materials; colour properties dark cyan alpha 0.25, emission cyan x0.35; destroyed in
    `Undecorate`). Assumed, not proven: the pass's overlay takes something from the ghost's own GBuffer colour (if not,
    the flicker and depth do the work). Log: `phase ghost = ... N dim cyan material(s) ... flickers for 0.6-1.1 s`.
    Round 17 ("can we somehow make the white transparent? otherwise clean"): disassembled, the pass's fullscreen shader
    (re/drones.md "Drone phase-through pass") reads only the ghost's depth, not its colours (so the round 16 materials
    did nothing there), blends SrcAlpha / OneMinusSrcAlpha with alpha = scanline alpha (`_ScanlineBGAlpha` /
    `_ScanlineLineAlpha`) x intersection fade, and fades the colour into `_IntersectionEdgeColorClose` (HDR ~350) the
    closer the hidden surface is to the occluder. The ghost sits a few cm behind the link's surface, so all of it
    drew that edge colour: the white. Now, while a ghost phases (first `PhaseOn` to last `PhaseOff`),
    `ApplyPassLook` sets the ranch pass's material (the `CustomPassVolume.customPasses` entry with filter bit 16):
    close edge = cyan x0.7 (`kPassEdge`), other colours clamped to LDR, scanline alphas x0.35 (`kPassAlpha`);
    `RestorePassLook` puts the saved values back (also posted after a `Reset`). The material is the drones' too: a
    drone crossing a wall during that 0.6-1.1 s looks the same way. A faulted module can't restore (calls off): the
    toned look then stays for the session. Log once: `phase pass material '...' toned down ...; its values: ...`.
  - **No idle ping**: the cloud-withdraw FX no longer pulses every ~7 s, only at a take (1.5 s cooldown).
  - **No suction**: the Refinery Link's vac particles at the port (`model_refineryLink/Dust ` with `Rings`) are
    switched off ("has a suction effect on the output").
  - **Outward arrows**: the port's `techDepositorPort01_input` mesh is replaced by `techDepositorPort01_output`,
    taken from the Drone Station prefab's chute ("shows like items can go in the link which they cant"; also on the
    placement preview). The link's own mesh is kept and put back when plain.
  - **Label**: the placed-label provider (`gadget_look::AddPlacedLabel`) existed since round 9, yet the user saw
    "Refinery Link" in round 14; it now logs `gadget-mode label of a placed link (actor N) = Quantum Storage Link`
    once, so the next log tells whether it runs (round 14 log: it does). Round 15 ("in non-gadget placement view
    it still shows up as 'Refinery Link'"): the look-at label outside gadget mode is TargetingUI's
    (`GetGadgetTargetInfo` -> the same `GetTargetGadgetInfo` detour), for its raycast target, not the director's
    `TargetedGadget`; `gadget_look::LookAtGadget` finds that gadget (`TargetingUI.GetTargetObject` + the Gadget
    in its parents, only while gadget mode is off) and asks the providers with it. Logs `look-at label (outside
    gadget mode) of a placed 'RefineryLink' replaced` once. GadgetLabelDisplay shows the HELD gadget's name while the
    placement footprint is visible (`GadgetItem.UpdateFootprint` -> `SetTargetGadget(..., preferPlaceholder)`,
    `_isTargetingPlaceholder`), so aiming near a link while holding a plain Refinery Link reads "Refinery Link".
- **Use** (the link's `ScienceUIDisplayActivator`, a UIDisplayInteractable): opens the game's Quantum Cloud
  menu, the drone station menu without a station (`OpenCloudMenu` / `CloudFrame`, RE: re/drones.md "Drone
  station menu"): view only (no withdraw / send: those need a station), programming tab removed by the game,
  opens on the drone list tab (round 14). Fallback (menu members missing, display not found): HUD notices (what it
  is, the five largest cloud amounts; `QueueText`, 1.5 s cooldown). Still a refinery input. Logs `Quantum
  Cloud menu = the drone station menu '<name>'`, `Quantum Cloud menu open (view only, no station, filled on
  try N | not filled; cloud legend: hid ... kept ...; drone list legend: <Class> '<object>': hid ... kept ...)`,
  `did not open within 10 s`.
  Round 12 ("the row shows to press [SPACE] and a magnifying glass, #watdatmean"): without a station the cloud
  page's legend is `DroneCloudInventoryMenu._defaultLegendConfig`, the map drone panel's (map controls that
  nothing in this menu handles). `HideCloudHints` switches its hints off (`IInputLegend.SetItemEnabled(event,
  false)` on the `_inputLegend` component) except events named cancel / back / close / exit; the log names
  each hint's label and event. Not seen in game: whether the station's own menu reuses that display object
  (then those hints would stay off there too; the withdraw / send hints are re-enabled by the game).
  Round 13 ("nope, in the link still shows '[SPACE] Go To'"; log `UpdateShowData(null) failed:
  System.NullReferenceException` on every open): the call ran the first frame the display GameObject existed
  and its throw returned before the tab switch and `HideCloudHints`, so the menu stayed on the drone list tab
  ("[SPACE] Go To"). `CloudFrame` now waits until the menu is active plus 0.15 s, retries a throw up to 3
  times, and selects the cloud tab and hides its hints either way. Logs `Quantum Cloud menu open (cloud tab N;
  view only, no station, filled on try N | not filled; legend: ...)` and, once per session,
  `UpdateShowData(null) threw N time(s) (...) - the legends are set anyway`. RE: re/drones.md.
  Round 14 ("quantum cloud tab shouldnt be the default, leave it be as Drones tab"; log `TabbedControl.GoToTab
  failed: System.NullReferenceException` on every open): the forced cloud tab is gone, so is that warning.
  The drone list's "[SPACE] Go To" (the user's "Go To bug"): `DroneListButton.OnSubmit` raises
  `GoToInMapRequested`, which only the map's drone panel subscribes to, so Space already does nothing here.
  `HideListHints` switches the hints off (except cancel / back / close / exit) on every
  `InputLegendFocusRegistrar` / `InputLegend` under `DroneMenuUI._droneList`, events read from their
  configurations and hint entries (`HideHints`, shared with `HideCloudHints`). Round 14 log: the
  `InputLegendFocusRegistrar 'DroneListPage'` holds it.
  Round 15 ("[SPACE] Go To is still there but grayed out this time, magnifying glass and [SPACE] still show"):
  `SetItemEnabled(false)` only greys a hint. `HideHints` now gives the legend the kept hints alone with
  `SetInputHints(InputHintConfiguration[], true)`, so the others are not shown at all (on the registrar the list
  survives focus changes; RE: re/ui.md "Hiding a hint"); `SetItemEnabled` stays the fallback when no hint is kept
  or the call is missing. The log says `hid 'Go To' [Submit] (removed from the legend)` or `(greyed out only: ...)`.
  Round 16 ("Go to hint is gone from the bottom, nice / On drone rows i still see '(Magnifying icon) [SPACE]'"): each
  drone row has its own focus prompt, `DroneListButton > AnimateInRoot > FocusedContent` (GoToIcon + InputEventDisplay;
  re/drones.md). The row's FocusEventTriggers activate FocusedContent on focus, so `HideRowHints` switches its two
  children off instead (they stay off). Rows under the link's menu only, never the prefab (the map's drone panel uses
  Go To); `RowsFrame` re-checks every 0.25 s while the menu is open (repopulated rows) and, once the list is gone or
  inactive, turns the parts back on for a menu kept alive. Log: `drone rows' Go To hint: N row(s), M hint part(s)
  hidden` in the `Quantum Cloud menu open` line.
  Round 17 ("Its there for a split second, like 200-500ms"): the sweep came after the menu loaded, was active
  0.15 s and was filled, while the first row had focus from the start. Two optional detours,
  `DroneListButton.BindItem` and `IFocusReceivedHandler.OnFocusReceived`, hide that row's parts (`HideRow`) right
  after the original, in the same frame, while the link's menu is opening or open (`LinkMenu`: a cloud request or
  the watched list); every other row costs a pointer check. The sweep stays as the fallback. Log: `..., M hint
  part(s) hidden, K before by the row detours`.
- **Drone source** (`0x53520110`, after Silo, every target, while a link with a drone port is built):
  `GetSources` returns hidden storages (`sr2qstore`: `SiloStorage`, 8 slots, no type group, 999 each,
  own `AmmoModel`, active in hierarchy); catcher = the link's own `SiloCatcher`. Accepted cloud types
  (cached 1 s, at most 4) mirror `min(cloud, 999)`; the `Decrement` detour lowers the cloud (never below 0);
  the tick re-syncs and frees slots unused 30 s. All vanilla sinks offered. Saved as **Silo**, sidecar
  `drone_quantum_source.stations`; no link left in the save -> Silo (logged).
- **Without the mod**: plain Refinery Links; unplaced kits gone (their Refinery Links stay); programs run
  as Silo; favourites hold the Refinery Link.
- Limits: drones need a path; a placed link without a spare kit turns plain only by pick-up; a cloud
  withdrawal in the station menu during a take can double the items in flight; hover prompt unchanged.

## Mod gadget looks

`gadget_look.h` (owns four hooks, above). `TintedSprite` keeps a trimmed atlas sprite's full rect (its pixels
at `textureRectOffset`) and can pad to a square (RE: re/unity.md "From round 11"). RE: re/fabricator.md (GadgetDefinition, GetTargetGadgetInfo,
SetHeldGadget, FabricatorItemDetails). `Register(def, Look{name, description, icon, dressPreview})`: own
description on the card (vanilla walks `VariantOf` to the root), our name as the gadget-mode label while
held and, via `AddPlacedLabel` providers, when aiming at a listed placed gadget; `dressPreview` runs on the
placeholder after `SetHeldGadget`; `icon` is the card's details picture (`_fullArt` / `_requiresFullArt`
cleared). Helpers `TintTree` (tag ` [sr2look]`), `TintedSprite`. Log `mod gadget looks ready: ...`.
`artTint` / `artAmount` (round 12, "The icons are so low quality on the right pane": a 128 px icon stretched
over the details picture): the full-art reference stays; the first time the card is shown the details' loader
puts the base's 512 px full art on the image, a frame callback (`ArtFrame`, up to 10 s; the bigger sprite
than the icon = the art) recolours it with `TintedSprite`, keeps it for the session and clears the reference,
so later opens show the copy at once. Used by the DJ buildables (not for a user drawing) and the Quantum
Storage Link. Logs `'<def>' card picture = its full art '<sprite>' recoloured (after N s)` / `... stays the
icon (<why>)`.
Round 13 ("pixelated asf on the right pane", "blurry" even with the 512 px DJ Booth drawing): the box is
440 px (re/fabricator.md); the game's full art is 512 px with mips, trilinear and **mip bias -1** (re/unity.md),
so it samples mip 0 only. Ours had bias 0 (mod_icons) or no mips (TintedSprite); now both match the game.
`ArtFrame` also waits past the loader's 160 px `iconEmpty` placeholder (round 12's Slime Radio picture was that
placeholder recoloured): only a sprite of 256 px or more, not named `iconEmpty`, counts as the loaded art.
Round 14 ("still pixelated", the bias did not help): at the user's 1920 x 1080 (registry) the CanvasScaler
factor is 1, so the 512 px picture lands in the 440 px box at 0.86x. Mip bias -1 means plain bilinear on mip 0
at 0.86x: it skips texels (thin outlines go uneven: "pixelated"); bias 0 blended mip 1 in ("blurry"). The
vanilla full art is set up the same (re/unity.md) but is soft painted art where that shows less. Now
`ShowPicture` draws every details picture of ours from a copy at its exact on-screen size (`ScreenSized`: GPU
read-back, Lanczos-3 in linear light on premultiplied alpha, no mips, point filter when no parent between the
image and the root canvas is scaled, else bilinear; cached per sprite and size). On-screen size = the Image's
RectTransform rect x the root canvas's `scaleFactor`, sprite fitted (preserveAspect). Logs per card (again
when it changes): `'<def>' details picture shown = '<sprite>' WxH px of a TWxTH texture (format, mips, filter,
bias); on screen W x H px (box, canvas scale, parent scale, root canvas name / mode / pixel perfect, screen
size, material / shader); drawn from a WxH copy (...)`, plus `sprite '<x>' resampled A -> B`. Twice per session
a vanilla gadget card logs the same for the game's own full art (`a vanilla card's details picture = ...`, 1 s
after shown). If the next round is still bad, those lines say whether the size or the canvas is off.
Round 13 bug (log: `'QuantumStorageLink' details picture shown = 'sr2mods_icon_dj_booth_large (sr2 tint)'`):
the image still showed the DJ Booth card's drawing (set by us) and `ArtFrame` took it for the loaded full art.
Now, before the original SetItem of a card whose art is pending, the image gets our icon and the details'
`UIImageDynamicLoader._iconSource` is cleared (SetFullArtSource skips an unchanged source with a valid
handle, re/fabricator.md), and `ArtFrame` never accepts a sprite of ours (`IsOurs`: made here, registered,
or named `sr2mods_icon_*`).
**Round 15, the real cause** ("both look blurry still", "blurry and pixelated", "quantum link shows refinery
default one"): the details image draws `Image.overrideSprite`. The loader's `_onSetSprite` event is wired to
`Image.set_overrideSprite` (scene data) and sets it on every SetItem to the item's small icon (`GadgetDefinition.icon`:
the 256 px DJ tile drawings, the 128 px tinted Refinery Link icon), to the loaded full art when done, and again on
OnEnable / OnDisable (re/fabricator.md). Rounds 12-14 set and read `Image.sprite`, hidden under that override:
the mip bias and the Lanczos copies never reached the screen, the logs named a sprite that was not drawn,
`ArtFrame` never saw the Refinery Link's full art arrive (the 10 s timeout, then the 128 px icon), and the
vanilla probe logged our own leftover sprite. Now `ShowPicture` sets both `sprite` and `overrideSprite` and
detaches the loader (`ReleaseCurrentHandle`, `_iconSource` = null: OnEnable sets nothing, OnDisable clears the
override so `sprite`, also ours, shows; the next SetItem sets the source again). `ArtFrame` and the vanilla probe
read `get_overrideSprite` (= what is drawn). `DropArt` no longer falls back to the 128 px icon: a timeout waits
again the next time the card is shown, a failed recolour leaves the base's own full art to the game's loader
for the session (`plainArt`). The picture log adds the display mode (`display WxH @ Hz, borderless fullscreen`).
Display check (registry and `slimerancher.cfg`, round 14 setup): borderless fullscreen 1920 x 1080 on monitor 0
(the 1920 x 1080 primary, native 1920 x 1080 @ 144), no render scale or upscaler setting, SMAA; the card canvas
is Screen Space Overlay (drawn at backbuffer size after post-processing), so the display path does not blur it.

## Slime DJ buildables

`dj_buildables.cpp`. Uses the quantum hooks through `variant_ext.h` (nothing without the Quantum Storage
Link's Setup); songs through `slime_dj/api.h` emitters. RE: re/fabricator.md (decoration / toy prefabs,
ToggleVariant, stacking), re/audio.md (GadgetMusicPlayer, corral Music Box), re/slimes.md (SlimeAttachPoint).

- **DJ Booth** = Scientist Desk variant. Faces +z, DJ behind (-z). Clutter and its sound off; a boombox
  upright in the middle facing the audience (round 11: the old face-up "deck" was "looking up"); two small ones on the front corners (x +-0.8, z 0.3, 0.4 size); two big
  ones on the floor at x +-1.75; behind (z -1.8) the Boombox's bench with the
  Flower Pillow's mesh, colliders and `SlimeAttachmentPoint` with its scripts (largos: `_allowLargo` before
  Awake, trigger 1.5x; round 12, "Tarrs should be able to go behind the DJ table": `_allowTarr` too, a
  Tarr sits on the largo joint and counts as the DJ). Round 12 ("only the right speaker is playing",
  "pretty quiet for the table's size"): all five boomboxes are emitters (kind Booth); floor speakers
  1.5x range / 1.4x volume (`kBoothFloor`), desk ones 0.6x volume (`kBoothDesk`); `Rec::scales` keeps the
  factors per emitter for every re-register (`ScaleOf`; radios 0.6x range). Other clones script-free. E: a `GadgetMusicPlayer` then a `GadgetUIInteractable`
  (copied from the Boombox) on the desk root; undress destroys both (`DestroyImmediate`).
- **DJ Speaker Stack** (`SlimeDJSpeaker`) = Boombox variant: bench, `model_boombox`, `Collision` off; two
  boombox clones stacked (1.3, 0.85 on top at 1 m), one `BoxCollider` 1.2 x 1.7 x 0.9 on layer 27, base =
  emitter (kind Speaker). `AllowGadgetStackingOnTop = true` on placed ones. Old saved kinds 1 / 2 load as the
  stack. On a placed one the toggle cycles plain <-> stack (the detour picks the kind; offered while a kit
  is left or when aiming at a listed one).
- **On / off with E** (booth, stack, plain Boombox; hooks answer only for our records), sidecar `dj.on`,
  default off. `Apply` (every tick):
  - off: silent, no dance, a running vanilla track stopped;
  - on + a song loaded (booth: and a slime on the pillow): the song; boomboxes dance (`_gadgetActive`) and
    show `FX_MusicalNotes`; the seat spins (`SlimeAttachPoint._rotationSpeed`, 0 otherwise);
  - on otherwise: booth -> the corral Music Box tune (`sr2dj_standby`, a copy of its `SECTR_AudioSource`,
    loop, `RestartLoopsOnEnabled` off: round 12 "At spawn, when deactived, plays the default sound" was
    `SECTR_AudioSource.OnEnable` playing the looping copy when the holder went active; fallback the
    Boombox track); stack / Boombox -> the Boombox track via the original `OnInteract`.
    One gadget track at a time: when the game stops ours, the record switches off.
  Logs `dj buildables: <label> actor N switched on|off (interact)`, `... plays the Slime DJ song|the
  Boombox's own track (...)|nothing`, `standby tune = the corral music box's '<cue>'`, `DJ Booth actor N: a
  slime sits behind the desk|nobody behind the desk any more`.
- **Plain Boomboxes** (up to 40): emitter on `model_boombox`, same E / dance rules (log `N plain
  Boombox(es) play the Slime DJ song when switched on (...)`).
- **Slime Radio** = a vanilla Night Light toy; its card definition is a Boombox clone, never a variant or
  in the inventory: fabricating spends the cost and spawns the toy 1.8 m in front of the camera (posted,
  at most 5 per click), id into `dj.radios`. Dressing (round 12: "a bit small tbh, use the original radio
  hitboxes with a flattened base so they can still rroll and have collision"): night light off; the toy's
  ball collider (r 0.47 toy units, read at runtime) stays on; its small bottom box becomes a flat base
  (1.15 r wide, 0.14 r high, bottom = the ball's bottom); centre of mass 0.55 r below the ball's centre
  (`Rigidbody.centerOfMass`, reset on undress) so it rolls when pushed and rights itself on the base; rotation
  free; a speaker stack filling 82 % of the ball's height (about 0.45 size, was 0.26) stands on the base;
  emitter kind Radio (0.6x range, 80 %), plays whenever a song plays. A radio that
  leaves the world raises `dj.radios_away` (max 20); the next Night Light appearing within 6 m of the
  camera is a radio again (logged). Failed spawns go to `dj.radios_owed` (up to 5 per tick later).
  Rounds 11-14: both colliders got a grippy, non-bouncy `PhysicsMaterial` (friction 0.9 / 1.0, Maximum combine;
  round 11 "slides so much"), linear damping >= 0.6 and angular damping >= 1.5 (removed in round 15). Round 11 froze rotation X / Z, which
  fought the vacuum's lock joint ("disappears after vaccing for 1-2 secs"); with rotation free all the time
  `RadioHeld` only clears stale freeze bits. Logs `Slime Radio actor N held in the vacuum` / `let go`; the
  dressed line ends with `ball on (r 0.47), flat base yes, low centre of mass yes, grip yes, rolls yes`.
  Hitboxes can be checked with debug_tools' Show entity hitboxes.
  Round 13 ("the round hitbox should be more like the Fox Toy's but the egg shaped oval should be taller ...
  (i think the speaker has too much weight though, doesnt seem to roll over at ALL now)"): the Fox Toy's setup
  (re/fabricator.md `toySteamFox`: a box base under a round top) with a taller egg. The ball is switched off and
  a `CapsuleCollider` added on the toy root (radius 0.8 r, 2.1 r high, bottom 0.05 r above the floor; reused if a
  lost record left one, destroyed on undress); the bottom box becomes the base in the fox's proportions (1.47 x
  1.29 egg radii = 1.18 r wide, 1.03 r high, bottom = the floor); the stack stands 0.14 r up as before. The centre
  of mass is automatic again (`ResetCenterOfMass` after the collider changes: about 0.9 r up, so it tips at
  ~30 degrees and rolls on the egg), angular damping >= 0.8 (was 1.5), mass unchanged (0.5 kg, as the fox).
  Without CapsuleCollider members the round 12 ball + thin base stay. Dressed line: `ball r 0.47 off, egg r
  0.38 x 0.99 high, box base yes, automatic centre of mass yes, grip yes, rolls yes`.
  Round 14 ("the egg shape hitbox is still too big, stays stuck on the bottom hitbox's edge somehow, like it
  gets stuck on an EDGE"): the round 13 box stuck out 0.24 r below and around the egg's bottom; tipped, the
  radio lay on the box's bottom edge plus the egg (a flat facet) with the automatic centre of mass only 0.07 r
  above the egg's bottom centre, so it was nearly balanced there and the grippy material kept it. Now: egg 0.62 r
  x 1.8 r with its bottom on the floor (was 0.8 x 2.1, 0.05 r up); the box a low footprint inside the egg's
  bottom (1.1 x 0.4 egg radii = 0.68 r wide, 0.25 r high, bottom on the floor; its top corners inside the egg,
  only the bottom face stands out). It stands upright on the base, tips at about 22 degrees, then the centre of
  mass (about 0.37 egg radii above the egg's bottom centre) is beyond the egg's contact, so it falls onto the egg
  and lies on its side, where the base is inside the capsule and nothing catches. Dressed line: `egg r 0.29 x
  0.85 high`. Not tried in game; the numbers are from the geometry.
  Round 15 ("after 1st touch with any wall it just stops the physics for it"): compared with the vanilla toy
  (Night Light / Fox Toy: Rigidbody 0.5 kg, no constraints, the toy's own colliders' materials and damping) the
  radio differed in two ways. (1) `Apply`, which runs for every record on each tick, still had the round 11
  freeze: while not held it set FreezeRotationX | Z (16 | 64) again, so right after a throw the radio kept its
  mid-air tilt and could not tip or roll; with the round 11 grip it stopped dead at the first contact. That
  branch now clears stale X / Z freeze bits like `RadioHeld` (log `rotation unfrozen (constraints now N)`).
  (2) The grip material (friction 0.9 / 1.0, Maximum combine, so it won against every surface, walls included)
  and the raised damping (>= 0.6 / 0.8) are gone: the body keeps the toy's vanilla values and the egg takes the
  ball's own shared material. Nothing else in the module touches the Rigidbody (the upright euler reset is only
  at dressing). The dressed line ends with `vanilla body: kinematic no, constraints 0, asleep .., v .., mass 0.50,
  damping .., angular damping .., sleep threshold .., at (..)`. The first 4 throws of a session are traced:
  `Slime Radio actor N +T ms: <same state>` every 0.15 s for 2.4 s after `let go` (stops when vacuumed again).
  If it still freezes, that trace says whether it is kinematic, constrained, asleep or just has no velocity.
- **Fabricator** (Decorations): booth after the Scientist Desk (Newbucks x2.5, items x2), stack after the
  Boombox (x1.5), radio after the stack (x0.5), Newbucks rounded to 5. Card icons (tile + details):
  `assets/icons/dj_booth.png` / `dj_speaker.png` / `slime_radio.png` when the user has drawn them, else the
  base's vanilla icon recoloured (booth pink, stack orange, radio cyan; log `<label> card icon = ...`); the
  details picture is the base's full art recoloured the same way once it has loaded (gadget looks `artTint`).
  A drawing always wins over the recoloured vanilla icon, in the tile and the details; the details use its
  512 px copy `<iconName>_large.png` when embedded (`pad_icon.py <source> <name>_large 512`; the DJ Speaker
  Stack has one), else the 256 px icon. Log `<label> details picture = the mod icon '<name>'`.
- **Dressed at once**: placing (AfterPlace) and the toggle on a placed one (ChangeVariant) build the record
  right away (`DressNow`); the tick keeps it (round 11: "have to wait a second for it to be the correct
  model"). Booth / stack: vanilla decoration +
  one kit (`dj.booth_kits` / `dj.speaker_kits`); placing uses a kit (none: plain, logged), pick-up gives it
  back, removed otherwise the tick gives it back.
- **Gadget mode**: booth and stack registered with gadget_look; `dressPreview` builds the dressed look
  script- and collider-free on layer 26 (logs `gadget looks registered for N definition(s)`, `<label>
  placement preview dressed (N part(s))`).
- **Sidecar**: `dj.booths`, `dj.speakers` (`id:kind`), `dj.radios`, `dj.booth_kits`, `dj.speaker_kits`,
  `dj.on`, `dj.radios_away`, `dj.radios_owed`, `dj.shop_radios_pending`, `dj.shop_radio_bought`.
- **Save** as the Quantum Storage Link (alias + `GetPersistenceId` for favourites). Per tick: no scene
  searches (`identifiablesByIdent`, cached keys). Without the mod: plain desks (clutter back), upright
  Boomboxes (stacks stay), Night Lights; unused kits gone.
- **Sandbox**: the Sandbox's own crafter / blueprint paths skip our definitions (`IsModGadget`: crafting one
  would write a duplicate persistence id); it lists and grants them through `buildables.h`.

## Mod buildables: Fabricator cards, unlock, API

RE: re/fabricator.md (GadgetFabricateCategoryRuntime..ctor, FabricateCategoryRuntime, FabricatorItemGrid).

- **Cards** are inserted in the `GadgetFabricateCategoryRuntime..ctor` detour (`GetItems` is never called):
  `quantum::InsertItem` and the DJ `InsertCards` insert into `_itemList`; `FixPageCount` raises
  `_categoryCount` (log `Fabricator category '...': entry count raised for N mod card(s) (41 -> 42
  entries)`). Warp Tech has 41 vanilla entries, Decorations 241.
- **Locked look**: `get_IsMissingBlueprint` follows our helper `HasBlueprint`; `get_ShouldShowPlaceholder`
  is false for our cards (name and icon always). Card text ends with how to place it and "Needs the <base>
  blueprint." (also the Sandbox `hint`).
- **Unlock rule**: the base gadget's blueprint (radio: the Boombox's), unless the sidecar has
  `buildables.unlock.<id>` = `1` / `0`.
- **Log** at a Fabricator opening when the state changes: `quantum link: Fabricator card added after the
  Refinery Link (Warp Tech, position N): LOCKED (...)`, `dj buildables: Fabricator card 'DJ Booth' added
  ...`; `Fabricator opened before the definition(s) were built` before the first tick of a loaded save.
- **API** (`buildables.h`): `List()`, `Grant(id, n, result)` (n base gadgets + n kits; radio: n toys, max 5),
  `SetUnlock(id, Unlock, result)` (`kFollowBase` clears). Ids `quantum_link`, `dj_booth`, `dj_speaker`,
  `slime_radio`. Never `AddItem` / `AddBlueprint` our definitions. Sandbox side: `modules/sandbox/NOTES.md`
  "Mod buildables".

## Slime Radio in the shop

`radio_shop.cpp`. RE: re/shop.md (Daily Deals data, ShopCategoryRuntime, ShopRuntimeItem, ShopDataModel).

- **Daily Deals only** (by analytics name; never Range Exchange / Coo-Coo Corner): at one restock in four
  (`kOdds`) the radio takes the daily toy's place in the list the UI reads. Restock key = FNV of
  `_lastSeedUsed` and the toy's asset GUID (stable across reloads). The vanilla entry is only taken out of
  `_cachedItemList` (pinned, put back later). Offered while the radio card is unlocked and the text sink
  is live.
- **Item**: `MemberwiseClone` of the toy entry with our own `ShopCategoryItemEntry` (the Night Light's
  asset reference in own AssetReference objects), 1000 Newbucks, the toy's sort index. `get_Asset` /
  `get_Description` return, for our item only, a `ToyDefinition` clone of the Night Light named "Slime
  Radio" (Boombox icon and full art, with `m_SubObjectName` / `m_SubObjectType`) and our text.
- **Buying**: `Spend`, `QueueToSpawnItem(spawnHandlerId, Night Light, count)`, `dj.shop_radios_pending` +=
  count; one per restock (`dj.shop_radio_bought` = key, `_isSoldOut`); not in the shop's purchase records.
  A null spawner id spawns in front of the player (retried via `dj.radios_owed`).
- **Dispensing**: after `SpawnNextItem`, a Night Light while radios are pending is listed in `dj.radios`;
  the tick lowers the pending count to the Night Lights still queued.
- Without the mod (or with the module faulted): Night Lights. Logs `daily toy restock XXXXXXXX: the Slime
  Radio is the daily toy | a vanilla toy (...)`, `Slime Radio stands in for the daily toy (position P of
  N)`, `N Slime Radio(s) bought for X Newbucks, queued at ...`, `the shop dispensed a Slime Radio (actor N;
  K still pending)`.

## Silo Sell-All: button and receipt

`silo_world.cpp`, `silo_receipt.cpp`, `sale_notify.cpp`. RE: re/plots.md (corral techActivator),
re/ui.md (PopupPrompt, TextButton_NoneSelectable, Selectable, UIInputLocker, HUD notifications),
re/unity.md (RanchTech01, HDRP/Lit), re/custom-upgrades.md 2.2.

- **Button**: clone of the corral collector's `techActivator` at the silo's (+x, -z) corner (5.58, 1.20,
  -5.58), 135 deg (`sr2mods_SiloSellButton#<gen>`), Collector null. No world text over it; the sale
  shows through HUD notices and the currency popup.
- **Gold**: the cap (`button_button`) gets our own `HDRP/Lit` material (`_BaseColor` (1, 0.74, 0.16),
  metallic 0.6, smoothness 0.75, `sr2mods_gold (HDRP/Lit)`, one per session); the base keeps a 35 %
  palette tint (instanced materials + property block). Each tick re-tints a renderer whose material was
  replaced. Logs `renderers before the gold: ...` (once), `made gold (cap + base): ...`; without HDRP/Lit
  `shader 'HDRP/Lit' not found` and the palette tint.
- **Receipt**: rows per sellable plort type (icon, name, count, unit price, total), grand total, Cancel
  left, Sell right. No plorts / market closed: opens empty with a note, Sell disabled. Height capped at
  the canvas minus 36 px each side: rows 56 -> 40 px, then a ScrollRect (wheel, Up / Down, W / S, d-pad,
  stick step one row).
- **Buttons**: the shared game button (`modules/common/game_button.h`: a clone of the popup's
  `TextButton_NoneSelectable`, scale 0.72, placed by the outer edge). Cancel shows the key cap "C" (the pad
  button with a pad), Sell the popup's confirm glyph. Sell is non-interactable (the game's grey, no hover)
  when there is nothing to sell or the market is closed; round 11's "Sell doesn't react to hover" was that
  state without the grey (set before the clone's Awake, see re/ui.md "ButtonAnimator"). A failed build
  logs `panel build failed at: ...` and retries with our own SDF buttons.
- **Keys** (polled): Sell = the popup's positive InputEvent, Enter, pad A; Cancel = C, Esc, pad B (glyph
  shows "C" for keyboard). A key held at open is no press; input ignored 0.3 s after open / refresh; a
  backdrop click cancels.
- **Input lock**: the game's `UIInputLocker` on the root; every close path deactivates the root first.
- **What is sold**: Sell re-quotes; only an identical quote is sold (at most the shown count per type),
  else the receipt refreshes with an orange note.
- Logs: `silo receipt: buttons the game's popup buttons, rows fit|scroll (...)`, `Sell button laid out W x H on screen`, `sell all (plot
  '...'): receipt shown, ...`, `receipt cancelled, nothing sold`, `receipt changed before the sale (...) -
  refreshed`, `sell all (receipt, plot '...'): sold ...`; `sell-all receipt unavailable` -> sells at once.
- `SaleNotices`: `Notice::kPlortSold` (Sell-All), `kCollected`, and `QueueText` free-text notices (used by
  the sandbox and the Quantum Storage Link).

## Silo capacity tiers

RE: re/plots.md (patchSilo, Additional Storage cards), re/custom-upgrades.md 2.2b.

- 100 -> 200 (vanilla Storage Capacity) -> 300 / 400 / 500 / 600 per slot, a chain on that card. Vanilla
  pays 1800 for +100; ours climb ~1.5x each (23,000 for II-V).
- **Applying**: the max is runtime only and vanilla `ApplyUpgrades` writes 200 on load and every purchase,
  so the ~1 Hz tick sets the owned tier (`UpdateMaxCount` when `GetSlotMaxCount(0)` differs; log `silo
  capacity on plot '...': 200 -> 500 per slot`).
- **Never deletes items**: a lower max leaves the contents; the silo takes nothing more until below (WARN
  when we lower it).
- **Not sellable** (reason "silo storage", like vanilla silo storage).
- **One Additional Storage card**: `MergeStorageCards` keeps the first not-owned of the three vanilla
  `Storage2 / 3 / 4` cards (STORAGE4 greyed once all owned); the others are hidden vanilla cards at their
  index. Titles "Additional Storage II / III / IV" (text_override, removed in Cleanup). After every menu
  update the list is checked: `silo menu: Additional Storage 1 card(s): ...` or WARN `Additional Storage
  NOT merged after the game's update - ...`.

## Selling with gamepad / keyboard

The plot menu's alt action (Info) sells the selected card's sellable upgrade (same target and checks as the
mouse Sell button), no popup, no Pedia; the prompt is the category's `altDetailsButton` relabelled "Sell +N",
shown only while it can be sold now. Code in sell_upgrades; RE: re/custom-upgrades.md 2b.1b.

## Tier props

`plot_props.cpp`: a prop per owned tier, gone when sold / off. Clones of vanilla prefab children under an
inactive holder `sr2props_<Kind>#<gen>` (not `sr2mods_`: coop_world's scan destroys those), stripped of
colliders (except silo tanks), joints, rigidbodies, game scripts, then activated. A missing model logs
`plot props: model '...' not found` once. RE: re/plots.md (plot prefabs), re/fabricator.md (decoration /
toy prefabs).

| tier | prop | source |
|---|---|---|
| Faster Hens I or II | two hay bales (0.5), three grass tufts, scattered | `decorHayBales`; coop `Deco/decoFieldsGrass0*` |
| Faster Hens II | two nests, flower pillow, two tufts (scattered); plush hen upright in the (-x, -z) corner | coop `Deco/nestPlain01`, `decorFlowerPillowSmall`, `toyStuffedChicken` |
| Sprinkler II or III | third head mid +x side; III: fourth mid -x (turned 180) | garden `Sprinkler/SprinklerBase` |
| Music Box II | second speaker on the (-x, -z) post, 2.5 m up, leaning in; a Slime DJ emitter (in sync) | corral `Music Box/techCorralMusicBox/MusicBox/Music` |
| Ash Booster | glow + embers in the fire, embers out of the opening, three `sr2ash` emitters on the ash at r ~3.3 following `AshTrough/Ash Level` (`FollowAsh`) | incinerator `FireTest/Glow`, `/Embers`; `decorFireFlower` |
| Silo Capacity II-V | one tank per tier on the ground at (+-2.9, +-2.9): II (+x, +z), III (-x, +z), IV (+x, -z), V (-x, -z); 0.8 size; colliders kept; prefab colours | silo `.../Capacity Addon 0/techSiloCapacity` |

- **Coop layout** (`CoopLayout`): 10 slots seeded by FNV-1a of the plot id, stable across loads; within
  +-3.7 m, out of the corner squares, ~1.2 m off the vanilla nest; the two nests never in one line through
  it; up to 256 draws, then a fixed spot. Build line `CoopNests props built ...; nests at (x, z) and (x, z),
  the plush hen upright in the (-x, -z) corner`.
- **Plush hen**: (-3.45, 0.53, -3.45), size 1.1, yaw 65 deg (front assumed +z; facing the wall -> 245). In
  a nest its low bulbs looked like eggs.

## Corral Radio

`corral_radio` (`api.h kCorralRadio`). No effect here: slime_dj asks `Owns(plot, kCorralRadio)` and plays
the corral's own playlist (per-plot `slime_dj.radio`, key from `PlotKey`). Round 12: the Music Box can be
sold while the radio is owned ("bought radio and i cant sell speakers I again"); `VanillaDependentOf` only
counts tier chains, and `Visible` keeps an owned non-tier card listed without its prerequisite, so the radio
stays sellable and plays again once the Music Box is bought back. Selling the radio keeps its playlist. Details: `modules/slime_dj/NOTES.md`
"Channels".

## Vacpack upgrades (Fabricator)

RE: re/upgrades.md (UpgradeDefinition, UpgradeModel, vanilla data, PlayerUpgradeFabricateCategoryRuntime),
re/vacpack.md, re/fabricator.md (UpgradeChain, pips). Bought through the vanilla flow.

| upgrade | levels | effect | where |
|---|---|---|---|
| Tank Booster IX-XII | 4 | 125 / 150 / 175 / 200 per slot | levels 9-12 of the vanilla card |
| Extra Tank III-IV | 2 | 7th / 8th slot, before water | levels 3-4 of the vanilla card |
| Drone Battery Saver I-IV (`0x53520103`) | 4 | drones use 80 / 67 / 57 / 50 % battery | own card, needs the Drone Station blueprint |
| Shadow Catcher (`0x53520104`) | 1 | vacpack holds shadow slimes; they stay and eat on the ranch | own card, always shown |
| Drone Program Scheduler (`0x53520105`) | 1 | up to 3 programs per drone station ("Several programs per drone") | own card, needs the Drone Station blueprint |
| Slime Launcher I-III (`0x53520106`) | 3 | slimes and held largos shot 1.2x / 1.4x / 1.6x as fast | own card, always shown |

### Tiers on the vanilla cards

- The tick appends our level objects (deep copies from `Object.Instantiate(AmmoCapacity)`) to the vanilla
  `AmmoCapacity` / `AmmoSlots` definitions, found by id every tick (log `'AmmoCapacity' now has 12 levels
  (8 vanilla + 4)`). Values are absolute: ADD 80 + 25..100 on `AmmoLimit`, ADD 2 + 1 / 2 on
  `AmmoSlotsAvailable`.
- **Save**: `UpgradeModel.Pull` post clamps ids 20 / 40 to VIII / II. Real level in
  `custom_upgrades.vacpack.tank_booster_level` / `.extra_tank_level` (absolute 0-based, only past the
  vanilla max), set by the `SetUpgradeLevel` detour (also Sandbox); at or below the max clears it. After a
  load the tick restores it only when the save holds exactly the vanilla max (log `'AmmoCapacity' level 8
  -> 10 (Tank Booster X, from the sidecar)`). Off: live level back to the max, our levels removed. Older
  builds' `.tank_booster` / `.extra_tank` carry over when the vanilla upgrade is maxed.
- **Circles** (`fabricator_levels.cpp`): pre-hooks clone the last circle / pip and lay out more than 8
  narrower (chain 12 x 27.8 px every 34.4 px; pips two rows of six). Without the chain hook Tank Booster
  keeps 8 levels (logged).
- **"Next:" label** (round 16, r15sl1 "'Next: Tank Booster IV' ... uses wrong next label"): the details
  panel puts the next level's Title in as a nested variable, which the text sink never sees, so our cloned
  levels showed their template's title (Slime Launcher: Tank Booster IV; Tank Booster IX: V). A
  `PlayerUpgradeDetails.Show` post-detour (`fabricator_levels.cpp`) swaps it for a `StringVariable` with
  our title when that Title is registered in `text_override` (log once per title: `details panel 'Next:'
  label shows '...'`). RE: re/fabricator.md "Fabricator details panel".
- **Costs**: IX-XII = V-VIII's items doubled (+2 for the rare ones), 60,000 / 75,000 / 90,000 / 120,000;
  Extra Tank III / IV = VII / VIII's items, 25,000 / 50,000. No upgrade component (the world has no spares).
  Log `vacpack: TankBoosterTiers ready ...`.

### Extra slots

- `PlayerState.InitModel` / `Reset` (pre) insert two clones of `ExpansionAmmoSlot 2` (thresholds 7 / 8)
  before `LiquidAmmoSlot`: slots 1-6, Extra Tank III, IV, water.
- **Save**: `AmmoModel.Pull` (player only) gives the save the 7 vanilla slots in vanilla order and writes
  the extra slots to `custom_upgrades.vacpack.extra_slot_items` (`k=referenceId*count;...`, k = 0 / 1);
  `AmmoModel.Push` (pre) spreads 7 slots into 9 (log `loaded vacpack spread into the extended layout
  ...`); the tick refills the extra slots from the sidecar. Old `.extra_slots` is converted once. Slimes
  there lose emotions / radiant look.
- **Keys**: settings "Extra Tank III slot key" = 7, "Extra Tank IV slot key" = 8 (group "Vacpack"), polled
  (`GetAsyncKeyState` edge; game in front, overlay closed, cursor locked, time running, vacpack out) ->
  `VacuumItem.SelectSlot(int)`. Old defaults 8 / 9 moved once (marker `extra_slot_keys_v2`). R still
  selects water (`SelectSlot(InputEventData)` remap). HUD labels via the `BindInputKey` detour.
- **Centred row**: `Ammo Slots` widened 900 -> 1160 px; Cleanup restores it.
- **Off / faulted**: vanilla array back for the next load; live slots reordered vanilla-first, so a save
  keeps water at index 6 (a mod-free load drops slots 8-9; contents stay in the sidecar).
- **Module on after the player was built** (`ExtendLive`, round 17): the per-save gate turns custom upgrades on
  only after the save has loaded, so `InitModel` ran with the hooks off and the player had 7 slots ("Extra Tank
  doesn't give me a space anymore": `7 of 7 slots unlocked` with Extra Tank IV; broken since the gate,
  2026-10-23, whenever the switch was off at load). The tick now extends the live vacpack: PlayerState defs
  extended as in `ExtendSlots`, the two new `AmmoSlot`s made by the game (`AmmoModel.Reset` on a temporary
  2-slot array: ctor, OnAmmoChanged, Definition, `Setup(tracker)`), the old slot objects kept (contents, HUD
  holders) with the new ones before water; `AmmoSlotManager._ammoSlotDefinitions` = ours, selected index
  shifted. Switched off and on again: VanillaOrder's layout is moved back. Then `AmmoSlotUI.Start` is invoked
  once to rebind the HUD row (the adapter keeps the list it was bound with). Logs `live vacpack 7 -> 9 slots`,
  `HUD slot row bound to the extended slots`.

### Drone Battery Saver

- **Card**: `AmmoCapacity` clone with an empty effect list (else no vacpack upgrade is set up, logged); id
  registered in `upgradeLevels` (-1) before the definition enters the player's list; out of the save's
  copy; not counted for TinkerTailorSlimes. `SetUpgradeLevel` detour writes the sidecar **before** the
  original (a throwing subscriber skips post-call code). Icon / pedia from the Drone Station gadget, else
  Tank Booster's.
- **Fabricator**: `PlayerUpgradeFabricateCategoryRuntime..ctor` detour: pre appends the card to the
  category holding id 20, post removes it while locked / no player / off and lowers `_categoryCount`. Log
  `vacpack: Fabricator category '<name>': DroneBatterySaver: card shown|hidden|removed (...)`.
- **Prices**: 7,500 / 10,000 / 15,000 / 25,000 + half of Tank Booster V-VIII's items.
- **Effect** (`drone_battery.cpp`): every ~2 s each station gets back what it drained beyond the level's
  share via `AddEnergy`; the saved battery stays vanilla; refunds below 0.01 accumulate. Level
  `custom_upgrades.vacpack.drone_battery` (0-based). Logs `... active (battery drain x0.50, ...)`, `station
  <id>: first refund ...`, once a minute `Drone Battery Saver IV gave back X energy to N station(s) ...`
  (checked: +0.4 per station per real minute at IV). Old `drone_battery.station.<id>` keys migrate to the
  global level.
- Rough edges: `ViewedItemsV02` may get its id (harmless); pinning it as a recipe unchecked for a mod-free
  load.

### Drone Program Scheduler

- **Card** (round 12): built exactly like the Battery Saver (`Kind::kDronePrograms`, `IsDroneCard`): one
  level, empty effect list, shown once the Drone Station blueprint is owned, the Drone Station's icon and
  pedia entry (vanilla art; log `DroneProgramScheduler card uses the '<gadget>' icon ...`). Value
  `custom_upgrades.vacpack.drone_programs` ("0" = owned), read by `VacpackUpgrades::DroneProgramsOwned`
  (1 / 0 / -1 unknown; 1 when the cards could not be created).
- **Price**: 20,000 Newbucks + half of Tank Booster VI's refinery items (rounded up), like the Battery Saver
  halves V-VIII: between Battery Saver II and III.
- **Text**: "A scheduling chip for your drone stations: each station's drone can hold up to 3 programs and
  switches to the next one when the current one runs out of work. Add programs with Programs in the drone
  station's menu."
- Effect and lock behaviour: "Several programs per drone". Sandbox: listed in its upgrades manager like
  every card in the player's upgrade list; a level change there goes through the `SetUpgradeLevel` detour.

### Slime Launcher

Card in `vacpack_upgrades.cpp` (`Kind::kSlimeLauncher`, id `0x53520106`), effect in `slime_launcher.cpp`
(round 15). RE: re/vacpack.md "VacuumItem - shooting".

- **Name**: "Slime Launcher I-III" (the user asked for something gentler than "Slime Punch"; it reads like the
  game's own tech names, Tank Booster / Pulse Wave).
- **Card**: like the Shadow Catcher, always shown, empty effect list. Value `custom_upgrades.vacpack.slime_launcher`
  (owned level, 0-based). 5,000 / 12,000 / 25,000 Newbucks + Tank Booster III-V's refinery items unchanged.
  Art: the drawing `slime_launcher` when embedded (`mod_icons::Has`), else the Pink slime's icon; details
  picture the Pink slime's pedia entry (logs `Slime Launcher card uses ...` / `card picture = ...`). Until the
  Pink SlimeDefinition is loaded (polled every ~10 s) it keeps Tank Booster's.
- **Effect**: slime shots 1.2x / 1.4x / 1.6x as fast (`launcher::kSpeedFactor`; distance about 1.44x / 1.96x /
  2.56x on flat ground). The vacpack tick publishes the owned level every second (`launcher::SetLevel`, from the
  UpgradeModel, so a Sandbox raise / lower applies within a second); Cleanup and a load set 0.
- **Hooks**: `VacuumItem.Expel(AmmoMetadata, bool, float, bool)` (slot shots; skipped when the caller passes a
  velocityOverride) and `VacuumItem.ExpelHeld(float)` (held largos). Both scale the VacuumItem's `EjectSpeed`
  for that one call when the type is a `SlimeDefinition` (largos included) and put the remembered vanilla value
  back after it. Food, plorts, toys, water: vanilla. Log `slime launcher: vacpack slime / held slime '<type>' shot
  at 1.4x speed (EjectSpeed a -> b ...)` once per level and path, and `slime launcher: level a -> b`.
- Also affects the boss fight's vacpack slime shots (`BossFightController.<ExpelAmmo>d__106` calls the same
  Expel), consistent with "slimes shot from the vacpack".

### Shadow Catcher

Card in `vacpack_upgrades.cpp` (`kShadowCatcher`), effect in `shadow_catcher.cpp`. RE: re/slimes.md
(ShadowSlimeScatter, ProducePlortsOnHit, SlimeEat / hunger, DestroyOutsideHoursOfDay), re/world.md
(IsOnRanch, CaveTrigger), re/slimes.md.

- **Card**: like the Battery Saver (value `custom_upgrades.vacpack.shadow_catcher`), always shown. 40,000
  + Tank Booster VII's items with the 70 Hyper plorts swapped for 25 Shadow plorts once the Shadow slime is
  found; Shadow icon; description ends with the sunlight warning. Details picture: the Shadow slime's pedia
  entry (`_pediaEntry`; the clone kept Tank Booster's, whose full art showed: round 11; RE: re/upgrades.md
  "PlayerUpgradeFabricatableItem"); log `Shadow Catcher card picture = the Shadow slime's pedia entry`.
- **Owned flag**: the vacpack tick publishes `shadow::SetOwned` and the regular slot definitions (AmmoSlot1's
  type group: six vanilla + Extra Tank III / IV) every second.
- **Vac**: `IsAllowed`: Shadow on a published slot -> true (never water, silos, drones, feeders).
- **Calm**: `ScatterIfPlayerNear` skipped, `Relevancy` 0 while not alerted; hits still alert as vanilla.
- **Ranch**: tracked when a scatter hook runs (GC handle); kept while `CellDirector.IsOnRanch`: `DoAlert` /
  `Action` skipped, alert cleared, no hit plorts (`ProducePlort` skipped). They save natively. Logs `shadow
  slime '...' is on the ranch: kept ...` / `left the ranch ...`.
- **Eating**: copies of the Pink slime's food entries (fruit, veggies, meat) producing Shadow plorts go
  into the Shadow `EatMap` (re-added after `RefreshDefinitions`).
- **Ringtail rule** per kept slime (in memory): our entries `MinDrive` / `ExtraDrive` 0;
  `_keepEatingWhenFull` + `InitFood`; `DrivePerEat` >= 1.0 (one plort meal empties the hunger);
  `_hungerRecoveryRate` = `SlimeEmotionConfig.HungerRate.DefaultValue`, `_hungerDirection` 1. Next plort
  when hunger is back at 0.333. Face `_isMischievous` while full. Logs `ate and is full ... hungry again in
  ~N game h` / `is hungry again` (max 12 per minute). Hunger saves (`SlimeModel.Emotions`); the rest not.
- **Phosphor rule**: day = outside the Phosphor's window (default 18:00-06:00), endure floored at
  0.25-0.5 game h. Shade = inside an enabled, active `CaveTrigger` collider's AABB (Solar Shield, caves;
  rescanned ~10 s). In the sun: scared face + fade clock (`is out in the sun: it fades in ~N game h`);
  shade / night stop it (`is safe again`); run out -> vanilla `DoAlert`, it vanishes (`fades away in the
  sunlight`). Not saved.
- **Slimepedia**: English `m.risks.shadow_slime` gets a note while owned (in memory).
- **Vacpack save**: `AmmoModel.Pull` gives the save an empty slot for each slot with shadow slimes and
  writes `custom_upgrades.vacpack.shadow_slots` = `v=count;...` (vanilla index; Extra Tank slots via the
  extra-slot stash); the tick puts them back (or they wait while not owned).
- **Module off**: vanilla again; shadow slimes still in the vacpack go into the next save and are dropped
  by the load after it (WARN at cleanup).
- Limits: no Shadow largo in 1.3.0 (a new SlimeDefinition would be unloadable without the mod); no new food;
  kept slimes eat whatever reaches them (a garden-fed corral loses food); hunger does not rise while
  unloaded; vacuumed ones lose emotions; feral wild ones could eat.

### Lava tank: not built

Lava is a `LavaVolume` trigger, not a liquid (RE: re/world.md "Lava"). A tank would need a runtime Lava
`LiquidDefinition`, `LookupDirector` registration and a strip-and-sidecar save path (the risky part),
`LiquidSourceSurface` on every `LavaVolume`, a lava slot appended like the extra slots, and new gameplay
for shot lava.

## Untested in game

- Drones: Hen / Elder Collector sources (icons, offering rules, save + reload, elder migration), Coop
  emptying the Hen Collector; several programs per drone (all; list in drone_price_limit NOTES).
- Quantum Storage Link: card, fabricating, toggle, place / pick up, look and glitch, Use notices, drone
  source, save + reload, module off, mod-free load.
- DJ buildables: looks and positions (estimated from prefab numbers), standby tune, seat spin, labels and
  preview ghost, largo seat, stacking on a stack, radio at the nozzle and re-adopt, owed radios, save +
  reload, mod-free load. Mod buildable cards: positions, locked look, unlock, Sandbox override.
- Radio shop: daily toy, picture, sold out, Coo-Coo Corner without it.
- Plot menu: Sell / Purchase layout; Sell button after selling Music Box II (round 9 FAIL, cause not proven,
  every silent exit now logs); checkmark after buying the last tier with the menu open.
- Sell-All: receipt at other resolutions, input lock, pad keys, mouse; gold on HDRP/Lit.
- Silo: tanks, slot displays at the new max, the merged Additional Storage card (round 8 saw separate cards;
  the list check logs it). Alt-action sell prompt and key.
- Props: Music Box II speaker animation, sizes, `Glow` / `Embers` playing, ember spots, plush hen facing.
- Auto-Feeder food lock: the whole feature (slider, drones, shots, save + reload).
- Vacpack: 12 circles / pips, buying the tiers, save + reload, mod-free load, extra slots and keys, centred
  row. Shadow Catcher: the whole feature. Slime Launcher: the whole feature (round 15).

## History

- Rounds 4-5: Hen Collector two limits and sliders (now mouse only), Additional Storage merged, Silo
  Capacity V, Battery Saver moved to the Fabricator, vacpack tiers merged into the vanilla cards.
- Rounds 7-8: Fabricator cards moved to the category-runtime constructors; Slime DJ buildables.
- Rounds 9-10: gadget looks, one DJ Speaker Stack, radio as a daily toy, Shadow Catcher hunger / sunlight,
  gold button on HDRP/Lit, Sell button diagnostics. Details: git log.
