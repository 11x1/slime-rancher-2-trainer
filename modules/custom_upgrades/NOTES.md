# custom_upgrades notes

Design and research: `re/custom-upgrades.md` (framework, sidecar, menu, world objects). Code
comments in `module.cpp`, `coop_world.cpp`, `plot_menu_slider.h` have the details. This file keeps
findings from in-game tests that are easy to get wrong again.

## Hen Collector

### Placement (fix after the test of build 4d16e38c)

- The coop prefab's `Deluxe` container (`DeluxeCoopUpgrader.DeluxeStuff`) is **not** identity: its
  localRotation is (0, -0.707, 0, 0.707), -90 degrees about Y. Its children are laid out in a
  (+x, -z) frame, so the vanilla elder collector ends up in the plot's **(+x, +z)** corner.
- Coop corners (plot-local, from the prefab): plot menu `techActivator` (-5.54, -5.54) = (-x, -z);
  vitamizer `Vitamizer/objCoopVitamizer` (-4.75, 4.75) = (-x, +z); elder collector
  `Deluxe/ElderCollector/Collector Unit/CollectionPoint` = (+x, +z). **(+x, -z) is free.**
- 4d16e38c set the clone root to -90 degrees, thinking the container was identity. That is the
  vanilla transform, so in a deluxe coop the Hen Collector stood exactly inside the vanilla elder
  collector (same meshes, same place) and could not be seen. The log showed it: renderers enabled
  and visible to a camera, bounds centre (-4.97, 1.89, -4.95) relative to the plot in world axes.
  The conservatory coops are turned 180 degrees, so that is plot-local (+x, +z).
- Now `PickCorner` reads the three occupied corners from the live plot's children (by path,
  `InverseTransformPoint` into plot space; the prefab layout is the fallback) and turns the clone
  by a multiple of 90 degrees into the first free corner, (+x, -z) first. Log: `hen collector
  corner on plot '...': collector turned 0 deg into the (+x, -z) corner ...`. The placement lines
  now also give the plot-local centre and `vanilla deluxe parts built / not built`.
- The clone's renderers (prefab data): `plortCollector`, `_funnel`, `_emmit` (the vac tower),
  2 x techDepositor (frame, inputOutput, Body), 2 x techDisplay1x1, plus the `FX Vac Dust` /
  `FX Vac Rings` particle systems (inactive between bursts, they make the huge "built" bounds).

### Vacuum

- `PlortCollector.FixedUpdate` does the whole suction once a joint exists: reel in (maxDistance
  -5 m/s), store through `_storage.MaybeAddToAnySlot`, `CollectFX` puff, `DestroyActor`, cyclone
  Animator bool, vac audio. Its only `_model` use is `HasReached(_model.collectorNextTime)` before
  its own `DoCollection`. A null `_model`, `_region`, `_storage` or `_vacAudio` throws an NRE
  every physics frame.
- So the clone's collector runs with a private `LandPlotModel` (collectorNextTime = 1e300, never
  registered) and the tick builds `DoCollection`'s joints for the chosen chickens only (see the
  comment in `coop_world.cpp`). `JointReference` is a private nested class, found with
  `il2cpp_class_get_nested_types`. `Identifiable.get_AmmoMetadata` is virtual, so it is looked up on
  the chicken's own class (runtime_invoke does not dispatch).
- Joints are released (`JointReference.Destroy`) before a clone is removed (sold, not owned,
  module off), so no chicken stays captive on a destroyed joint.

### Limits

- Two keep-limits per coop: hens 0..58 (default 18), roosters 0..20 (default 2). Chicks never
  count. Sidecar `hen_collector_keep_hens` / `hen_collector_keep_roosters`; an old
  `hen_collector_keep` n reads as min(2, n) roosters and the rest hens.
- The coop menu shows two `PlotMenuSlider`s in one row (`SliderSpec.columns`, `Create(..., rowOf)`).
- **Mouse only** (feedback round 5: "remove the icons and arrow key functionality for now, will deal
  with gamepads later"). The sliders take mouse clicks / drags only (`SliderSpec.keys = false`: no pad
  / keyboard polling, `Step()` refuses); the arrows / d-pad / stick navigate the menu as usual, Submit
  and Cancel do what they do in vanilla. Removed with it: the select-to-step mode of round 4 (label
  `< Keep hens: 18 >`, a slider selected by a mouse press or Submit) and its hooks `ListUI.Submit`
  (0x83F450) and `BaseUI.OnCancelPressed` (0x683490), the `INavigationHandler.TryHandleAsNavigation`
  (0x79C940) detour (see "Slider keys" below), and the "Pad: d-pad hens, right stick roostros." hint
  in the card text. `PlotMenuSlider` keeps its pad / key support (`keys = true`, the default) for
  drone_price_limit, which drives it through `SetPolling` / `Step` from its own navigation.

### Activate button (checklist hc3)

- The clone keeps the vanilla elder collector's press button (`PlortCollectorActivator` +
  `TechUIInteractable`, inside the collector branch, so it stands next to the collector in the
  turned corner) with its `Collector` set to null. The module's `PlortCollectorActivator.Activate`
  detour never runs the original for a null Collector (vanilla would `StartCollection` = a
  DoCollection of every adult) and hands the press to `CoopWorld::Activate`: the tick's excess-only
  vacuum at once (no warm-up, no period wait; the 6 h period restarts). Button animation, press cue
  and 0.4 s rate limit through `SiloButtons::Press`; a short text above the button ("Collecting N
  chickens", "Nothing to collect (keeps H hens / R roosters)", "The Hen Collector is full",
  "Already collecting"). Log: `hen collector on plot '...' (Activate button): ...`.
- The button is kept only while that detour is installed (`CoopWorld::KeepHenButton`); the build line
  says `Activate button kept` / `no Activate button`.

### Slider keys (fix after the test of build 506a4f60; hook removed in round 5, sliders mouse only)

Kept for when gamepad support comes back:

- The arrows still moved the menu selection: `FocusManager.NavigateInDirection` hands a move to every
  `IMoveHandler` between the focused card and the spatial target through
  `INavigationHandler.TryHandleAsNavigation` (0x79C940), and the focused card's own Selectable comes
  first, so the old `UniformGridView.OnNavigate` detour never ran (no "taken from the menu's grid
  navigation" line in the log). The detour is now on `TryHandleAsNavigation`; right after each call
  NavigateInDirection checks the event's `used` flag (used -> no SetFocus). `NavigateInDirection`
  itself is hooked by drone_price_limit (one hook per address).

### Ranch drones (user report: "no option to select chicken output as a source")

- The drone program's source **Coop** already has two parts (`RanchDrone.GatherSourceTask(COOP)`):
  `RanchDroneSourceProgramCoop` (live chickens above the task config's minimums) and
  `RanchDroneSourceProgramElderCollector` (a silo-storage source over
  `RanchMetadata.Plots[].elderCollectors`). The UI offers a source when all its programs accept the
  target; the elder-collector part accepts every target, so Coop shows for meat / hen / rooster
  targets as before.
- `RanchMetadata.Register(LandPlot)` fills a coop's `elderCollectors` only from its own
  `DeluxeCoopUpgrader.DeluxeStuff` storage, one `StorageMetadata(slot, storage, SiloCatcher with that
  SlotIdx)` per slot. The ~1 Hz tick appends the Hen Collector's two slots the same way
  (`LinkDrones`: new array = live old entries + ours). Every `ApplyUpgrades` / `OnUpgradesChanged`
  builds a new entry without them, so the next tick adds them again (log `linked to the ranch
  drones`). Sold / not owned / module off: `RanchMetadata.OnUpgradesChanged(plot)` rebuilds the
  vanilla entry (log `unlinked from the ranch drones`).
- So a program with the source **Coop**, a target such as Meat or a hen / rooster type and any
  destination (silo, corral feeder, ...) now also takes the chickens out of the Hen Collector, through
  its two output ports, like the deluxe coop's elder collector. Fast-forward (sleep) uses the same
  entries. The contents the drones take leave the sidecar copy with the next tick.
- Separate "Hen Collector" / "Elder Collector" sources: see the next section.
- Known edge: turning custom upgrades off while a drone flies to a Hen Collector port that still
  holds chickens leaves that drone with a destroyed port (vanilla has the same case when a deluxe coop
  is demolished). Selling is refused while it holds chickens, so selling can't cause it.

Not checked in game yet: a drone with Coop -> Silo (target Meat, or Hen) emptying the Hen Collector,
the log lines, no errors after buying another coop upgrade (re-link) or selling the Hen Collector.

### Drone sources "Hen Collector" and "Elder Collector" (user requests, on top of the Coop linking; `drone_source.cpp`)

- Two real sources in the program's source picker, right after Coop (Coop, Hen Collector, Elder
  Collector), while custom upgrades are on. Hen Collector takes only from the Hen Collector ports;
  Elder Collector (round 4: "elder hens show up as Hen Collector, should be Elder Collector ... using
  the game's own coop upgrade") takes only from the deluxe coop's vanilla elder collector. Neither
  takes live chickens. Coop still covers everything (live chickens, elder collector, Hen Collector).
- **Which source for which target.** The vanilla elder-collector part accepts every target, so Coop
  is offered for meats, elders and single hens / roosters / elders. Before round 4 the Hen Collector
  was inserted whenever Coop was, so it also showed for an elder target (empty: the Hen Collector
  never holds elders). Now a source is inserted only when the target's predicate
  (`DroneTaskTarget.<Predicate>k__BackingField`) wants a type the kind holds: Hen Collector =
  task config `MeatTypeGroup` minus `ElderGroup` minus UnstableMeat (the Hen Collector's slot rule);
  Elder Collector = `ElderGroup` members (found once per session by name with one
  FindObjectsOfTypeAll; log `elder types for Elder Collector: ...`). And only when a storage of that
  kind exists: Hen Collector = the coop tick's Hen Collector storages, Elder Collector = any other
  storage in `RanchMetadata.plots[].elderCollectors` (that is the vanilla elder collector of a deluxe
  coop; the RanchMetadata comes from the drones seen).
- In memory a program carries our `DroneTaskSourceType` value `0x53520101` (Hen) / `0x53520102`
  (Elder). Hooks: `RanchDrone.GatherSourceTask` (our type -> `DroneTaskSource` with one new
  `RanchDroneSourceProgramElderCollector`; an unknown type would throw in vanilla),
  `RanchDrone.GatherSourceTasks` (insert after Coop, see above), `RanchDroneSourceProgramElderCollector.GetSources`
  (for a station whose program has our type: only that kind's slots, as a `StorageMetadata[]`),
  `DroneUIDefinitionSource.FindSourceDefinition` (a runtime copy of the Coop definition: name through
  text_override, sort index Coop + 1 for both; `GatherSourceOptions` keeps the list order).
- **Icons** (round 4: "remove the plot icon from the collector and replace it with chicken/elder"): the
  plain sprites, no composing. Hen Collector = the task config's `HenType.icon`; Elder Collector = the
  `ElderGroup` member whose name has "Hen" (the elder hen). Until known, Coop's icon. The composed
  Plort Collector + Hen icon (`drone_icon.cpp`) is gone.
- **Save (user decision: "save it as coop, the sidecar says the true source").**
  `DroneStationGadgetModel.Pull` gives the save a copy of the program data that says COOP (PullGadget
  turns it into the saved `DroneTaskV01` right away); the live data keeps ours. If the copy can't be
  made the live data becomes Coop (logged). The sidecar values `drone_hen_source.stations` /
  `drone_elder_source.stations` list the station actor ids (`PlacedGadgetV07.ActorId`, stable across
  loads) per kind. A ranch drone station has one program, so the station id is the key.
- **Load:** the first ~1 Hz tick that sees a listed station whose saved program is Coop switches it
  back (`DroneStation.AssignRanchTask` when its drone exists, else the program data; `RanchDrone.Init`
  then builds our source). Until then (about a second) the drone runs as Coop. An Elder Collector
  station waits until the ranch is known (a drone seen).
- **Migration:** a Hen Collector program whose target is elders (ELDERS, or SINGLE with an elder; made
  before round 4) becomes Elder Collector (log `... now Elder Collector` / `elder target: Elder
  Collector holds the elders`), or Coop when no coop has an elder collector.
- **Stale entries:** a listed station whose program is now something else (changed by the player,
  or not Coop in the save) is dropped; listed ids that no station has any more (picked up and placed
  again = a new actor id, demolished) are dropped with a log line - a re-placed station has a Coop
  program. No storage of the kind left (Hen Collector sold, deluxe coop gone) -> the program falls
  back to Coop (logged) and the entry goes.
- **Custom upgrades off / faulted:** `OnDisable` writes Coop into every program that had ours (plain
  field writes, before the hooks go off, so a later save can't see our value); `Cleanup` (main thread)
  re-programs those drones with Coop. When it is on again the sidecar's stations are mapped back.
- **Without the mod:** the save only ever holds Coop programs; it loads and runs as Coop (the drone
  takes live chickens and the elder collector; the Hen Collector itself is gone without the mod).

Checked in game (round 3): Hen Collector source works. Not checked yet: the new icons, Elder Collector
in the picker (only for elder targets, only with a deluxe coop), a drone emptying only the elder
collector, Hen Collector no longer offered for an elder target, save + reload of both (log `restored
from the sidecar`), the migration of an elder-target Hen Collector program.

## Quantum Storage Link (user request: "drones take items out of a quantum storage link and put them into acceptable inputs")

`quantum_link.cpp` (gadget, visuals, cloud mirror) and `drone_source.cpp` (the source; shares the Hen /
Elder Collector hooks). Findings with RVAs are in the header comments of both files.

- **"Quantum storage" = the Quantum Cloud** (`DroneDirector.GetCloudModel()` -> `DroneCloudModel._amounts`,
  `Dictionary<IdentifiableType, int>`, one key per storable type; saved as `DroneCloudV01`). Explorer drones upload
  into it, the drone station's cloud menu withdraws. Not the refinery, not the warp depots.
- **The gadget is a variant of the Refinery Link.** A runtime clone of `LookupDirector.RefineryLink`
  (`Object.Instantiate`, name `QuantumStorageLink`, ReferenceId `GadgetDefinition.QuantumStorageLink`), `VariantOf` =
  Refinery Link, the SAME prefab, variant gradient purple -> cyan, own name / description (text_override).
  - A placed gadget's model type comes from the prefab (`GadgetDirector.InstantiateGadget`: `TryGetId(prefab)`), so a
    placed Quantum Storage Link is a vanilla Refinery Link model and is saved as one. Variants share their base's
    inventory (`GetItemCount` -> `BaseGadget`, placing spends `{BaseGadget, 1}`), so our definition never enters
    `GadgetsModel._itemCounts` / blueprints (their `Pull` would throw on an unknown or duplicate id).
  - The sidecar lists the quantum actor ids (`quantum_link.actors`) and the unplaced Quantum Storage Links
    (`quantum_link.kits`).
  - Registered in the live `GadgetDirector._gadgetVariantMap` (in the Refinery Link's list, plus a key of its own with
    no variants: every variant lookup is `get_Item` without a fallback). Checked every tick (a new scene has a new
    director).
  - Favourite slots can hold the variant (`OnGadgetVariantToggled` -> `SetFavorite`) and ARE saved. Two guards: the
    save index `_reverseIndex` gets our ReferenceId -> the Refinery Link's persistence id (data only, re-added every
    tick; the save's id table `_primaryIndex` is separate, so nothing new is written), and a
    `SaveReferenceTranslation.GetPersistenceId(IdentifiableType)` detour maps our definition to the Refinery Link.
    Cleanup (module off / faulted) also puts the Refinery Link into those slots. After a load the slot shows the
    Refinery Link (toggle the variant again).
- **Fabricator**: its own card right after the Refinery Link (`GadgetFabricateCategory.GetItems` detour). Cost =
  the Refinery Link's with 3x the Newbucks and 2x the items (log `Fabricator cost ...`). Fabricating it never runs the
  vanilla body (it would `AddItem` our type): the card's own `CheckCanFabricate`, `SpendableDirector.Spend(our cost)`,
  `AddItem(Refinery Link, n)`, kits += n. The card is unlocked with the Refinery Link blueprint (helper `HasBlueprint`
  detour); its owned count is `min(kits, Refinery Links)` (`GetItemCount` detour).
- **Placing**: hold a Refinery Link, press the toggle-variant key -> Quantum Storage Link (offered while a kit is left:
  `GadgetDirector.HasBlueprint(ours)` = kits > 0 and the Refinery Link blueprint), place. `PlaceGadgetEvent` detour: the
  new gadget's actor id is listed and a kit used (log `Quantum Storage Link placed`). No kit left: the held gadget
  becomes the Refinery Link first (a plain one is placed, logged).
- **Picking up** a quantum link (`PickupGadget` detour) gives its kit back and holds the variant, so placing it again
  keeps it quantum. Removed any other way (stored): the tick gives the kit back. **Toggle-variant on a placed Refinery
  Link** (`Gadget.ChangeVariant(ours)` detour) flips it quantum <-> plain in place (kit used / back), never the
  original (that destroys and re-places the gadget: new actor id). The toggle is only offered while a kit is left.
- **Look** (tick, every loaded listed link): its renderers' materials are instanced and tinted: every colour property
  (`Material.GetPropertyNames(Vector)`, names with color / tint / albedo) 60 % towards purple, emission properties
  (emiss / glow) cyan (HDR x1.5..6); instances are tagged ` [sr2q]` and the originals put back when the link stops
  being quantum. Holder `sr2qlink#<gen>`: a script-free clone of a teleporter gadget's `TeleporterNode._activeFX`
  (tinted, 0.5 size, 1.1 m up) - the glow; plus the drone storage. The drone station's cloud-withdraw FX
  (`DroneStationCloudExtractionController._spawnFX`, pooled `FXHelpers.SpawnAndPlayFX`) pulses every ~7 s and at
  every drone take. The log names the shaders and properties it tinted (`shader '...' (link): N colour / M emission`).
- **Drone source "Quantum Storage Link"** (`kQuantumSourceType` 0x53520110, listed after Silo for every target while
  a link is built): a `RanchDroneSourceProgramElderCollector` whose `GetSources` returns `StorageMetadata` of the
  links' hidden storages (`sr2qstore`: a `SiloStorage` added to our holder, 8 slots of a session `AmmoSlotDefinition`
  copy without type group and 999 per slot, its own `AmmoModel`; active in the hierarchy, else the drone cancels).
  The catcher (where the drone flies) is the link's own `SiloCatcher` (its refinery input). The cloud types with an
  amount that the program's predicate accepts (cached 1 s per program, at most 4) get a slot; every link's slot
  mirrors `min(cloud amount, 999)`. Every take goes through `StorageMetadata.Decrement` (its only callers: the
  silo-storage source's `OnAction` and fast-forward): its detour lowers the cloud amount by the count taken
  (`GetAmount` + `SetAmount`, never below 0) and every mirror follows. The tick re-syncs with the cloud (uploads,
  station menu withdrawals) and frees slots unused for 30 s. Destinations are the vanilla sinks (silo, corral,
  feeder, incinerator, market, refinery, caretaker shop); the Silo sink stays offered (vanilla drops it only for the
  Silo source).
- **Save of the program**: like the Hen Collector, but saved as **Silo** (the vanilla source that accepts every
  target); sidecar `drone_quantum_source.stations`; restored at the first tick that sees the station. The option is
  offered while a link is built (loaded, with a drone port); saved programs are kept while the save lists any link
  (also with the ranch not loaded: the drone then just finds nothing); no link left in the save -> back to Silo
  (logged).
- **Without the mod**: every Quantum Storage Link loads as a plain Refinery Link where it stood; unplaced Quantum
  Storage Links are gone, but the Refinery Links fabricated with them stay in the inventory; drone programs run as
  Silo -> same destination (a Silo -> Silo program just moves items between silos); a favourite slot that held the
  variant holds the Refinery Link. Nothing else of ours is in the save (viewed-blueprint hashes are plain ints).
- Known limits: drones need a path to the link (place it on the ranch); the variant (toggle key) is offered only while
  a kit is left, so a placed quantum link without a spare kit turns plain only by picking it up; the variant's
  inventory icon is the Refinery Link's (recoloured gradient only); a player withdrawing from the cloud in the station
  menu while a drone takes the same type can give at most the items in flight twice (the tick re-syncs every second).

Not checked in game yet: the Fabricator card (position, cost, unlock), fabricating (kits log), the variant toggle
while holding a Refinery Link (name, gradient), placing / picking up / toggling a placed link (logs), the tint and the
glow (log lines name what was found), the source in the drone menu (after Silo, icon = the Refinery Link icon), a
drone taking from the cloud to a silo / feeder / refinery (cloud amount going down in the station's cloud menu), save
+ reload (links rebuilt, program restored), turning custom upgrades off (links plain, favourites back), a mod-free
load of such a save.

## Demolish clears the plot (user report round 4)

"Sold the silo plot, capacity stayed at full upgrade" / "bought corral, Music Box II, sold plot, bought
Music Box -> Music Box II already bought". The sidecar keys plots by `LandPlotLocation._id`, which
stays the same when a plot is demolished and rebuilt; only a rebuild as another type dropped the entry.
Now a detour on `LandPlotLocation.Replace` (the one plot swap path: buying on an empty plot and the
demolish closure, both through `LandPlotUIRoot.BuyPlot`) drops the id's whole entry after the
original: tiers, world upgrades, Hen Collector contents and limits, other modules' per-plot values
(sell_upgrades' Ash Trough fill). Props, the Sell-All button and the Hen Collector clone are children of
the old plot and die with it. Vanilla upgrades need nothing: the new plot gets a new LandPlotModel.
Not written to the sidecar at once (it belongs to the loaded .sav; the next game save writes it), so a
reload without saving restores the plot and its upgrades together. Log: `plot '...' demolished /
replaced (old type T, stored type T): dropped owned [...], values [...]`.

## Buy menu cards (fixes after the test of build 506a4f60)

- **Purchase / Sell overlap.** The buttons size themselves: the prefab `TextButton_NoneSelectable`
  (UnityPy, `duplicateassetisolation_assets__54b00e46...bundle`) has the game's own layout
  (`LayoutManager`, `HorizontalLayout`, `EncapsulateChildren` -> `SetSizeWithCurrentAnchors` in
  LateUpdate; the label has `ResizeToPreferred`), so the old half-width split was overwritten and the
  buttons grew back to full width. Now (sell_upgrades `PlaceButtons`) only pivots / positions are
  set: owned card -> PURCHASED hidden, Sell alone in its place; tier card -> Sell pivot x 1 left
  of the old centre, Purchase pivot x 0 right of it (16 px gap; swapped in plot-polish-7, user request
  "switch sell and buy button positions"), re-centred for a few ticks once the layout has given them
  their widths. Applies to every plot menu.
- **Sell also bought the next tier.** The prefab's `InputEventButton` performs the menu's submit on
  pointer down / up (before the Button click reaches the Press detour): on a tier card that opened
  the purchase prompt of the shown tier and the same click confirmed it (log: "sold Music Box" and
  "bought Music Box II" 4 ms apart). Disabled on our clone; its submit-key icon (`InputEventDisplay`
  under "Icon Background") now shows the alt-action key instead (`SetInputEvent(ListUI.altSubmit)`,
  the key that sells, user feedback "sell should have a key"), hidden when the button is disabled. The popup-confirm guard from feature/plot-props-2 stays as a
  backstop.
- **Stale card label** ("High Walls" on the Music Box card). The clones' texts were registered only
  in the post hook of `UpdateMenuItemAvailabilityAndHidden` / `RebuildUI`. With the menu open,
  RebuildUI's original rebinds the cards, so a card placed by the pre hook for the first time this
  session (Music Box II taking over the Music Box card right after its purchase) ran
  `LocalizeStringEvent.UpdateString` with no override yet and showed its template's text (the corral
  clones' template is `Walls Upgrade`). Texts are now registered in the pre hook too.

- **Maxed tier showed Purchase and no checkmark** (user report after round 4: "upgrade tier II doesn't put
  it as checked and shows the buy button"; "a checkbox on the icon if upgrades are on max tier"). A card's
  greyed icon + checkmark and its Purchase button come from the item's `IsAvailable` Func (`() =>
  !plot.HasUpgrade(_upgrade)`), which only `PlotUpgradePurchaseItemModel.UpdateAvailability(plot)` sets -
  called by `UpdateMenuItemAvailabilityAndHidden` for the items in the list on a menu OPEN. A purchase or
  sale with the menu open goes through `RebuildUI`, which does not call it. A clone is a fresh
  ScriptableObject whose constructor Func says "available", so a tier card we swap in there (Faster Hens II
  after buying I, Silo Capacity V after IV, the next Additional Storage section) showed Purchase and no
  checkmark until the menu was opened again (or used the Func of the last plot it was bound for). Now
  `RefreshAvailability` calls `UpdateAvailability` / `UpdateHidden` with the plot for every item in the
  list at the end of `BeforeMenuUpdate`, before the original rebinds the views (on a menu open the
  original does it once more). The checkmark itself is the game's: `GenericViewHolder.BindData`
  (0x83C900) enables its `unavailableIcon` image with the item's `_unavailableIcon` sprite while
  IsAvailable is false; our clones copy that sprite from their icon source / template.
- **Coop Air Net icon**: the corral's own Air Net upgrade icon (`iconFrom` = Air Net, found in the corral
  menu's items by `GlobalUpgradeItem`), no mod icon any more (user: "use the air net icon from corrals").

Not checked in game yet: all of the above (layout and centring, the hidden icon, that selling no longer
buys, the label, the Activate button's position, prompt and
animation), and from plot-polish-7: Sell left / Purchase right, buying the last tier with the menu open
showing the checkmark and no Purchase button at once, the Coop Air Net icon.

## Sell-All receipt

Pressing the silo's Sell-All button no longer sells at once: it opens a receipt (`silo_receipt.cpp`).
One row per sellable plort type (icon, name, "x count", unit price, line total + coin), the grand
total, Cancel and Sell. Market closed / unreachable or no plorts: the old text above the button, no
dialog.

- **Fits the screen** (feedback round 4: it ran off-screen on a smaller resolution). The panel height
  is capped at the canvas height (Screen size through the CanvasScaler formula, ScaleWithScreenSize
  1920 x 1080 match 0.5 -> 16:9 is 1080 units high, ultrawide less) minus 36 px each side. Rows shrink
  from 56 to 40 px, then the row list scrolls: a uGUI `ScrollRect` (Clamped) on a `RectMask2D`
  viewport with a thin `Scrollbar` in the right margin (draggable, hidden when not needed). Its own
  wheel handling is off (sensitivity 0); the mouse wheel (`Mouse.scroll.y`, one row per notch whatever
  the delta units), Up / Down, W / S, d-pad and left stick up / down step one row. Without these
  classes the rows only shrink (to 34 px) as before.
- **Game buttons.** Cancel and Sell are clones of the game's own confirm-popup buttons:
  `GameContext.UITemplates.popupPrompt` (prefab `PopupPrompt`, gamecore_scenes bundle, read with
  UnityPy) -> `PopupPrompt._negativeActionButton` / `_positiveActionButton`, both
  `TextButton_NoneSelectable` (the Purchase button's prefab: Button, ButtonAnimator, EventTrigger,
  DetailsMoreButtonView, InputEventButton, EncapsulateChildren, the key icon `InputEventDisplay`
  under "Icon Background" bound to the popup's positive / negative InputEvent). On the clone the
  InputEventButton is disabled (it would perform that InputEvent on pointer down / up), the label's
  LocalizeStringEvent is disabled and the TMP text set to "Cancel" / "Sell"; a polled Toggle on its
  root was meant to take the click (Button.onClick is empty in the prefab). (The teal / pink focus ring
  of round 4 is gone, see "Buttons like the popup's" below.) Disabled Sell = `Selectable.interactable`
  false (the prefab's disabled colours).
  Fallback when any piece is missing: our own SDF buttons with "(C / B)" / "(Enter / A)" in the
  label. Log: `silo receipt: buttons cloned from the game's popup, rows fit|scroll (N px, canvas H high)`.
- **Regression after feature/polish-4 ("Sell menu didn't show up").** The receipt never opened: log
  `silo receipt: not opened: panel failed`, then `the receipt could not be opened - selling at once`;
  Player.log `Can't add 'Toggle' to Cancel because a 'Button' is already added to the game object!`.
  `Selectable` is `[DisallowMultipleComponent]` (inherited), so a Toggle can't go on the cloned button
  and the build stopped. Now the game button's click is polled from its own Button:
  `Selectable.IsPressed()` (protected) true last frame, false now, `get_isPointerInside` (private) true.
  IsPressed is false while the Selectable is not interactable, so a disabled Sell can't be clicked. The
  button keeps its hover animation (no overlay on top). If a build still fails, the failing step is
  logged (`panel build failed at: ...`) and the panel is built again with our own buttons (game buttons
  off for the rest of the session, `retrying with our own buttons`); a failed clone logs `game button
  not cloned (...)` once.

- **Input lock = the game's own.** The dialog root carries the game's `UIInputLocker` component, as
  menu prefabs do: `OnEnable` -> `TimeDirector.Pause` (timeScale 0, ref-counted) ->
  `InputDirector.OnPausedChanged` disables the player action map and enables the UI map (no movement,
  no look, no interact), `CursorLockHandler` frees the cursor while paused, non-UI sound pauses.
  Closing deactivates the root first (`OnDisable` -> `Unpause` at once), then destroys it. Every close
  path (cancel, sell, load, module off, fault, scene change) goes through that, so the game cannot
  stay paused because of the dialog.
- **Buttons like the popup's** (feedback round 4: "buttons ugly, very big, no padding on the right,
  cancel on C, the keys aren't on the left side of the buttons like the game has them"). Popup prefab
  (UnityPy, `gamecore_scenes`, `PopupPrompt`): Panel 960 wide with `LayoutManager` + `EncapsulateChildren`,
  ButtonPanel 64 px in from each side, Accept (positive) on the LEFT at x -169, Cancel on the right at
  +169, each `TextButton_NoneSelectable` 321.5 x 58.6: `Label Background` (SDF pill, label 42 pt at x 30)
  and `Icon Background` (the `InputEventDisplay` key glyph, 64 px icons) laid out by the button's
  `HorizontalLayout` / `EncapsulateChildren` / `ResizeToPreferred`. Those are the game's own layout
  behaviours (`MonomiPark.SlimeRancher.UI.Framework.Layout`); only a `LayoutManager` above them runs them
  (`OnEnable` / `LateUpdate` -> `ApplyLayout`). Our clones had none, so they were never laid out: the
  prefab size and the glyph left over the label. Now each clone gets its own `LayoutManager` (the glyph
  on the left inside the pill, as in the game), sits in a zero-size slot scaled 0.72 (the receipt's 40 pt
  title vs the popup's 55 pt; on the slot, not the button, whose ButtonAnimator may animate its scale),
  and hangs off its outer edge: Cancel's left edge at the left padding, Sell's right edge at the right
  padding (pivot 0 / 1, re-placed for 30 frames). Cancel left, Sell right (plot-polish-7, user request "switch
  sell and cancel button locations"; the game's popup has the positive button on the left). Not focusable
  (TextButton_**NoneSelectable**): no focus ring any more. Log once: `Sell button laid out W x H (x0.72 on
  screen)` and the key line below. Fallback (no LayoutManager class): prefab layout, unscaled.
- **Keys** (polled, nothing hooked). Sell: the popup's positive `InputEvent` (`PopupPrompt._positiveInput`,
  read through its `PollValue` Func, the bound action's value: the key its glyph shows), Enter, pad A.
  Cancel: **C** (user request), Esc, pad B and the popup's negative InputEvent. Cancel's glyph shows the
  negative event's binding; while it shows a keyboard key its key text is set to "C" (its
  LocalizeStringEvent off, extra icons of the group hidden); a gamepad glyph stays (B). A key still held
  when the dialog opens is not a press (edge from the open state). Own-button fallback: Left / Right, A /
  D, Tab, d-pad, stick move the focus, Enter / Space / pad A press it. Mouse: the two buttons, a click on
  the dimmed backdrop cancels. Input is ignored for 0.3 s after opening or refreshing, so the press that
  opened it never confirms.
- **What is sold.** Sell quotes the silo again. Only an identical quote (same types, counts and
  prices, market still open) is sold, with the quote as the plan: at most the shown count per type
  (`Summarize(..., plan)`). Anything else refreshes the receipt with an orange note ("Prices or
  contents changed - this is the new receipt", "The Plort Market just closed") and sells nothing; Sell
  is disabled while nothing can be sold. Time is paused while it is open, so a change is unlikely.
- **After the sale** the dialog is already closed; the per-plort HUD notices are queued as before
  (shown once the HUD takes them), the text fallback above the button stays for a missing HUD list.
  `SaleNotices` (`sale_notify.cpp`) also takes a notice kind: `Notice::kCollected`
  (`AddResourceCollected`, the drones' cloud-upload notice, "... sent to the Quantum Cloud"). The
  sandbox used it for its spawns and crafts at first; since 2026-10-24 it queues its own free-text
  notices instead (`QueueText`, "Spawned 5 Pink Plort"). The Sell-All keeps `kPlortSold`, unchanged.
- **Gold Sell-All button** (plot-polish-7, user: "modify the sell button model colors so I can see it's not
  the menu-open one"). The clone of the corral collector button looks like the plot menu terminal's, so its
  cap (renderers whose GameObject name has `button_button`) is tinted towards gold (60 %); without a cap
  the whole button 30 %. Done one tick after the build, after the prefab's `ApplyPalette.Start` has swapped
  in the palette's materials: `Renderer.get_materials` (instanced copies), every Color property of each
  shader (`Shader.GetPropertyCount` / `GetPropertyType` / `GetPropertyNameId`, `Material.GetColor` /
  `SetColor`), HDR intensity kept, alpha kept. Shader-agnostic because the tech shaders' property names
  are unknown. Log once: `silo sell button on plot '...' tinted gold (cap): N colour(s) on <renderer> (k),
  ...` - if it says `no cap found` or 0 colours, the renderer names / shader need a look. A later palette
  change leaves our instanced materials alone (they are in no palette map).
- Log: `silo receipt ready`, `sell all (plot '...'): receipt shown, N plort(s) in T type(s) for V`,
  `receipt cancelled, nothing sold`, `receipt changed before the sale (...) - refreshed`, then the
  usual `sell all (receipt, plot '...'): sold ...`. Without the dialog parts (setup failed) the button
  sells at once as before (`sell-all receipt unavailable`).

Not checked in game yet: the look (font taken from a HUD text, sizes at other resolutions), that the
locker really freezes movement / camera and shows the cursor here, that Esc / Start do not open the
pause menu as well (they should not: `PauseMenuDirector.TogglePause` (0xAEFF20) only calls `PauseGame`
while `Time.timeScale > 0`, and the locker sets it to 0), mouse clicks (needs the scene's
EventSystem), pad A / B and the d-pad / stick.

## Silo capacity tiers

Storage per silo slot: 100 -> 200 (vanilla) -> 300 -> 400 -> 500 -> 600 (ours).

- **Vanilla** (prefab `patchSilo` read with UnityPy, disassembly): one `SiloStorage` with 12 slot
  definitions, all `SiloNonSlimes` (`_maxAmount` 100). STORAGE2 / 3 / 4 (500 / 575 / 650) only swap
  slot GameObjects (`StorageUpgrader.StorageAdd*` / `StorageOnly*`); the single capacity upgrade
  STORAGE_CAPACITY_INCREASE ("Storage Capacity Upgrade", 1800, in the
  `scriptableobjectlookupdata_assets_all_*` bundle) runs `AmmoSlotManager.UpdateMaxCount(
  StorageCapacityUpgradeAmount = 200)` (`StorageUpgrader.Apply` 0x996E30) and shows the four
  `CapacityUpgrade1..4` parts. So vanilla has one capacity tier, 100 -> 200.
- **Ours:** Silo Capacity II / III / IV / V (`0x53520060..63`, keys `silo_capacity_2..5`), 300 / 400 /
  500 / 600 per slot, **3000 / 4500 / 6500 / 9000** (V added in round 4: "add tier V, 600"). A tier
  chain on the vanilla Storage Capacity card: once that is owned the card shows Silo Capacity II, then
  III, IV, V (greyed with the checkmark when all are owned); `ChainVanillaRoot` gives every tier of a chain that starts on a vanilla upgrade that
  upgrade's card (before, only the first tier took it over). Icon = the vanilla Storage Capacity icon
  (no own sprite).
- **Prices:** vanilla pays 1800 for +100 per slot (12 slots = +1200 items, 1.5 Newbucks per slot
  item). Each of ours adds +100 per slot as well and costs more than the last (x ~1.5, like the
  vanilla storage units climb), so IV is the most expensive plot upgrade after the Sell-All Button
  (10,000): 14,000 for the three, 15,800 with the vanilla one.
- **Applying it:** the max count is runtime only (`AmmoDataV02` saves id, count, emotions,
  appearance, radiant; no max) and vanilla `ApplyUpgrades` writes 200 again on load and after every
  purchase on that plot. So the ~1 Hz world tick sets the owned tier's value on every silo plot
  (`UpdateMaxCount`, only when `GetSlotMaxCount(0)` differs; log `silo capacity on plot '...': 200 ->
  500 per slot`). Ownership is in the sidecar like every custom upgrade, so it survives save / reload.
- **Never deletes items:** load restores counts without clamping (`AmmoModel.Push` -> `AmmoSlot.
  set_Count`), `ValidateAndAdjustSlots` never trims counts, and `UpdateMaxCount` only sets the max. A
  lower max (between a load and the next tick, module off / faulted, or a mod-free load) leaves the
  contents above it in the silo; it just takes nothing more until they are below (logged as a warning
  with the count over the limit when we lower it).
- **One "Additional Storage" card** (round 4: "merge additional storage to 1 buy menu option, I don't
  want to scroll to upgrade it"). Vanilla lists three cards with the same title and icon, `Storage2 /
  3 / 4 Upgrade` (STORAGE2 / 3 / 4 = upgrades 3 / 4 / 5, one more storage section and port each;
  STORAGE3 / 4 hidden until the one before is owned, owned ones stay greyed). `MergeStorageCards`
  keeps one of them in the silo's list, the first section not owned yet (STORAGE4 greyed once all
  are owned), the others go out as hidden vanilla cards at their vanilla index (back for other plots /
  module off, like a tier card's vanilla card). The capacity side was already one card (Storage
  Capacity -> II -> ... -> V). So the silo menu has two storage cards: Additional Storage (more
  sections) and Storage Capacity (more per slot); they are different upgrades with different
  purchases, so they stay two cards.
- **Tier labels** (plot-polish-7, user: "add additional storage tier labels"): the three vanilla cards'
  titles read "Additional Storage II / III / IV" (STORAGE2 / 3 / 4, the section each one adds), like
  our tier cards; a text_override on each vanilla item's own `_title` (pinned items), removed in Cleanup.
  Log once per menu: `silo: 3 Additional Storage card(s) labelled with their section (II / III / IV)`.
- **Not sellable**, like the vanilla silo storage upgrades: `CanSell` gives the same permanent reason
  ("silo storage"), so sell_upgrades shows no Sell button and no alt-action prompt.

- **Visuals** (feedback round 5: "I don't see any visual upgrades for tiers II, III and IV"). The
  vanilla capacity upgrade shows one `techSiloCapacity` tank per owned storage section, high on the
  silo's diagonals (`Capacity Addon 0..3`, plot-local about (-1.6, 3.3, -1.8) for section 0, each next
  section turned 90 degrees and ~0.8 m higher). Our tiers add the same tank (0.8 size) standing on the
  ground on a diagonal at (+-2.9, +-2.9): II (+x, +z), III also (-x, +z), IV also (+x, -z), V also
  (-x, -z), each turned like the vanilla tank on that diagonal (19.6 + k * 90 degrees). Every diagonal
  has the same low pipes (each section's Addition is section 0's layout turned 90 degrees), the plot
  menu terminal of the (-x, -z) corner stands outside the fence, 3.7 m from tank V.
- **Collision** (round 4: "the silo tank props need collision"). The tank props keep the prefab's
  colliders (`SolidKind` in plot_props: the `collider` child's MeshCollider and the foot plate's
  MeshCollider), on their prefab layer, Default, the layer of the vanilla tanks and pipes; game
  scripts, joints and rigidbodies are still stripped, so they are static colliders. The player walks
  around them; the vac and drones treat them like the vanilla tank (which they already pass). Build
  log: `SiloTankII props built on plot '...' (1 of 1 piece(s), colliders kept)`. Clearances: silo wall r 2.1, vanilla pipe elbows /
  caps r <= 3.05 (>= 1.37 m from a tank centre), depositors in the middle of each side, corner posts at
  4.75, the Sell-All button outside the (+x, -z) corner. Built by `plot_props` (holders
  `sr2props_SiloTankII..IV#<gen>`), gone when the tier is not owned / the module is off, rebuilt
  after a load from the sidecar. The tank's `ApplyPalette` is stripped, so it keeps the prefab's
  default colours, not the silo's palette.

Not checked in game yet: the tanks (position on the ground, no overlap with the silo, pipes or
depositors at every storage level, colours), the card chain (vanilla card -> II -> III -> IV), the slot displays showing
the new maximum, drones / silo input filling to 500, and the log lines after a load (200 -> 500 within
a second).

## Selling with gamepad / keyboard

The plot menu's alt action (the Info / Pedia button) sells the selected card's sellable upgrade:
the same target as the mouse Sell button (`ChainSellTarget` on tier cards, the owned item
otherwise) and the same `CanSell` checks, one sale per press, no popup, no Pedia for that press.
The prompt is a copy of the game's own alt-action hint (`category.altDetailsButton`: key / button
icon + label) relabelled "Sell +N"; it is only shown while the upgrade can be sold now (a passing
reason such as "holds food" shows no prompt, the mouse Sell button shows the reason). Code and
findings: sell_upgrades header (`alt-action sell`), `re/custom-upgrades.md` 2b.1b.

Not checked in game yet: that the plot categories have an `altDetailsButton`, that the prompt's
label is set (log `alt-action sell: prefab ... label via ...`), and the keyboard key of the
altSubmit event (the prompt shows it).

## Tier props (checklist u5, u6, u9, u11, uv1-uv4)

`plot_props.cpp` puts a visible prop on a plot while a tier is owned; it goes when the tier is
sold or the module is turned off. No assets of ours: every piece is a child of a vanilla prefab,
cloned under an inactive holder `sr2props_<Kind>#<generation>` (prefix not `sr2mods_`, which
coop_world's scan would destroy), stripped of colliders, joints, rigidbodies and game scripts
(MonoBehaviours outside `UnityEngine.*`), then activated. Lookups are cached and pinned.

| tier | prop | source |
|---|---|---|
| Faster Hens I or II (u5) | two small hay bales (scale 0.5) and three grass tufts, scattered | decoration `decorHayBales`: `hayBale_2`, `hayBale_0 (1)`; coop prefab `Deco/decoFieldsGrass01 _Green`, `Deco/decoFieldsGrass02_Green` |
| Faster Hens II (u6) | two nests (a stuffed chicken in the second), a flower pillow and two more grass tufts, scattered | coop prefab `Deco/nestPlain01` (the vanilla nest), toy `toyStuffedChicken`: `model_stuffedChicken`, decoration `decorFlowerPillowSmall`: `mesh`, coop grass as above |
| Fast Growth I or II (u9) | a third sprinkler head in the middle of the +x side, spraying inwards | garden prefab `Sprinkler/SprinklerBase` (vanilla heads are on the -z / +z sides) |
| Fast Growth II | a fourth head in the middle of the -x side, turned 180 degrees | same |
| Music Box II | a second speaker on the (-x, -z) corner post (above the plot menu terminal), leaning inwards | corral prefab `Music Box/techCorralMusicBox/MusicBox/Music` (the vanilla speaker sits on the (-x, +z) post) |
| Ash Booster (u11) | glow + embers in the fire, embers rising out of the opening | incinerator prefab `.../meshIncinerator/FireTest/Glow` and `/Embers` (inactive in vanilla, switched on), decoration `decorFireFlower`: `FX Fireflower` |
| Silo Capacity II / III / IV / V | one capacity tank per tier standing on the ground beside the silo: II (+x, +z), III adds (-x, +z), IV adds (+x, -z), V adds (-x, -z); solid (colliders kept) | silo prefab `Silo Upgrade 0/Addition 0/Capacity Addon 0/techSiloCapacity` (the vanilla Storage Capacity Upgrade's tank, with its pipe-connector foot) at 0.8 size |

Gadget / toy prefabs are found by prefab name in `LookupDirector._gadgetDefinitions` /
`_toyDefinitions` (member types, then sub-groups). A missing model is logged once
(`plot props: model '...' not found`) and that piece is left out.

Coop scatter: one layout of 10 slots per coop (`CoopLayout`), seeded by FNV-1a of the plot id
(splitmix64), so it is stable across loads and the Hens II slots do not move when Hens I changes.
Slots stay within +-3.7 m, out of the corner squares (|x| and |z| both > 2.5), about 1.2 m off the
vanilla nest and clear of each other (radius per slot); 64 draws, then a fixed fallback spot (a
simulation over 2000 ids never needed it).

Corral corners: plot menu terminal (-x, -z) outside the post at ground height, vanilla speaker
(-x, +z), plort collector (+x, +z), auto-feeder distributor (+x, -z) at 2.5 m. The second speaker
shares the terminal's corner, 2.5 m up.

Test 2 "sprinkler left behind after selling Fast Growth" was not a props bug: the log shows every
sale of Fast Growth I (and of Music Box / Ash Trough) from its tier card followed 2-12 ms later by
`bought Fast Growth II` (Music Box II, Ash Booster), so the plot still owned a tier and the prop
stayed correctly. `LandPlotUIRoot.Upgrade` has one caller, `<PromptForUpgrade>b__0`, and
`PromptForUpgrade` always opens the popup, so a purchase popup was confirmed in the click of the
sale. sell_upgrades now refuses that confirm in the sale's frame and the next (log `purchase of
... right after a sale refused`). Removal itself (CoopNests, IncineratorGlow) logged fine.

Feedback round 5, Ash Trough sold part-filled and bought back: "I see nothing visually added". The
trough model is vanilla (`patchIncinerator/AshTrough`: floor, four low walls, child `Ash Level` =
FillableAshSource); the sale hides it, vanilla `AshTroughUpgrader.Apply` (0x983930: `SetActive(true)`
for ASH_TROUGH) shows it again on the purchase (`LandPlot.AddUpgrade` -> `ApplyUpgrades`), and on
load. The log of that test had `Ash Trough bought back ...: trough not found, vanilla fill kept`:
sell_upgrades looked for the FillableAshSource on `AshTrough` itself, so the remembered ash was never
restored (the trough came back full, as vanilla). Fixed in sell_upgrades (GetComponentInChildren);
after every Ash Trough purchase it now also checks the trough model and switches it on if it is still
hidden (log `trough model shown` / `was still hidden - shown now`), so the next test tells which case it
was.

Not checked in game yet: the Music Box II speaker (its Animator runs without the game's music box
script; it may stand still or play its idle), the scattered coop layout, the -x sprinkler head,
and whether the phantom purchase is gone. Also not checked yet: sizes and heights (hay bales, nests, pillow), whether the incinerator's
`Glow` / `Embers` play on their own once active (they may be sub-emitters of `FireTest`) and look
right, and that nothing of the stripped clones still logs errors.

## Vacpack upgrades (Fabricator)

`vacpack_upgrades.cpp`, `fabricator_levels.cpp`. Bought through the vanilla flow (recipe panel, cost check,
`SpendableDirector.Spend`, `UpgradeModel.IncrementUpgradeLevel`; the game's own modifier code applies the effect).

| upgrade | levels | effect | where |
|---|---|---|---|
| Tank Booster IX-XII | 4 | 125 / 150 / 175 / 200 items per slot (absolute, like every vanilla level) | levels 9-12 of the vanilla Tank Booster card |
| Extra Tank III-IV | 2 | a 7th / 8th vacpack slot, placed before the water slot | levels 3-4 of the vanilla Extra Tank card |
| Drone Battery Saver I-IV (id `0x53520103`) | 4 | every drone station's drone uses 1/1.25, 1/1.5, 1/1.75, 1/2 of the battery (80 / 67 / 57 / 50 %) | own card, once the Drone Station blueprint is owned |

**Merged into the vanilla cards** (feedback round after build 1aacffc: "Tank Booster IX is upgradeable before the
previous tank booster ... base tank is 50, bought IX -> now 75"; "merge ... so they have more tiers and ours isn't a
separate upgrade"). Before, Tank Booster IX-XII and Extra Tank III-IV were separate cards (cloned definitions with
their own ids) whose ADD modifier stacked with the vanilla one (+25 on whatever Tank Booster level was owned), and the
card could be bought with Tank Booster III. Now:

- **Where the levels live.** `UpgradeDefinition._levels` is a `List<UpgradeLevelDefinition>` (Effects, `_costs`,
  Title, Description); `LevelCount` is its Count everywhere (Fabricator card, details panel, `SetUpgradeLevel`'s range
  check, `IncrementUpgradeLevel` / `UpgradeLevelExist`, `ActorUpgradeHandler.ApplyUpgrade`). The tick appends our level
  objects to the vanilla `AmmoCapacity` / `AmmoSlots` definitions (looked up by id in the player's list every tick, so
  a reloaded asset gets them again; log `'AmmoCapacity' now has 12 levels (8 vanilla + 4)`). The level objects are the
  deep copies from `Object.Instantiate(AmmoCapacity)` (checked), with our values, costs and texts. So a tier can only
  be bought after the vanilla max, the vanilla card shows the next level's title / costs / "fully upgraded", and the
  level circles continue.
- **Absolute values.** Vanilla levels replace each other, so each tier's modifier is the vanilla max level's value plus
  ours: Tank Booster ADD 80 + 25 / 50 / 75 / 100 on `AmmoLimit` (default 20) = 125 / 150 / 175 / 200; Extra Tank ADD 2 +
  1 / 2 on `AmmoSlotsAvailable` (default 4) = 7 / 8 slots.
- **Keeping the vanilla save clean.** `UpgradeModel.Pull` hands the save a new dictionary copy; its post-detour clamps
  ids 20 / 40 to the vanilla max (7 = VIII, 1 = II). The real level is a per-save sidecar value
  (`custom_upgrades.vacpack.tank_booster_level` / `.extra_tank_level`, absolute 0-based level, only written past the
  vanilla max) set by the `SetUpgradeLevel` detour on a purchase (and by Sandbox's unlock-all, which goes through the
  same call); a level at or below the vanilla max clears it (at exactly the max since the Sandbox upgrades
  manager can lower levels: Tank Booster X -> VIII kept the old value, which the tick would have put back after a load). `UpgradeModel.Push` writes saved levels straight into the
  dictionary (no `SetUpgradeLevel`), so a load gives VIII / II; the tick then sets the sidecar level (`'AmmoCapacity'
  level 8 -> 10 (Tank Booster X, from the sidecar)`) - only when the save holds exactly the vanilla max (a save that
  disagrees wins). A mod-free load sees a valid vanilla value (a level past `LevelCount` would throw in
  `UpgradeDefinition.GetUpgradeLevel`). Module off / faulted: `Cleanup` sets the live level back to the vanilla max and
  removes our levels from the definitions, so a later save without our hooks is vanilla too.
- **Older builds' cards.** Their sidecar values `custom_upgrades.vacpack.tank_booster` / `.extra_tank` (level of the
  separate card) carry over to the vanilla card when the vanilla upgrade is maxed in the save (log `... carried over to
  the vanilla card`); otherwise (bought out of order) they stay pending and are checked again after each load. Their
  ids were never in a save (removed by the old Pull detour).
- **Level circles** (`fabricator_levels.cpp`). The Fabricator prefabs have 8 circles: the details panel's
  `UpgradeChain` (42 x 22, every 52 px, absolute positions) and each grid button's `FabricatorItemButtonPips` (22 x 11,
  two rows of four). `UpgradeChain.Show` moves its highlight onto `_levels[current + 1]`, which throws at Tank Booster
  VIII with 12 levels. Pre-hooks on `UpgradeChain.Show` (0x82ED90) and `FabricatorItemButton.ConfigurePips` (0x806CE0,
  `Configure` inlined) clone the last circle / pip (same parent) and install a longer array, then lay them out for the
  count shown: more than 8 -> narrower and closer in the prefab's width (chain: 12 circles 27.8 px wide every 34.4 px,
  highlight 39.8 px; pips: two rows of six, 14 px wide), else the prefab's own positions. Without the chain hook Tank
  Booster keeps its 8 levels (logged). Log: `fabricator levels: details panel shows 12 level circles ...`.

**Costs (our choice).** Each level copies the refinery items of a late vanilla level, in the vanilla style:
Tank Booster IX-XII use V-VIII's items with the amounts doubled (+2 for the rare ones vanilla asks 1-3 of) and
60,000 / 75,000 / 90,000 / 120,000 Newbucks (vanilla VIII: 50,000). Extra Tank III / IV use VII / VIII's items
unchanged and 25,000 / 50,000 Newbucks (vanilla Extra Tank II: 5,000; a slot this late is worth more than +25 per
slot). No upgrade component: vanilla consumes one Tank Booster / Extra Tank component per level (counted items from
treasure pods / the shop) and the world may hold no more than vanilla needs, so ours would be unbuyable; the cloned
component list is cleared. The log lists every level's exact cost (`vacpack: TankBoosterTiers ready ...`).

**Vanilla data** (UnityPy on `duplicateassetisolation_assets__54b00e46...bundle` and `corescenes_scenes_...bundle`,
names from the English string table):

- `AmmoCapacity` (`_uniqueId` 20) = Tank Booster I-VIII: one ADD modifier per level on `AmmoLimit` (default 20):
  +10 ... +80, so 30 ... 100 per slot (levels replace each other, they do not add up). 450 / 1000 / 3000 / 10000 /
  15000 / 20000 / 30000 / 50000 Newbucks plus refinery items (V: 50 Sloomber plorts, 20 Stable plorts, 20 Black
  Indigonium, 1 Royal Jelly; VI: 60 Twin, 25 Stable, 20 Dream Bubble, 3 Sun Sap; VII: 70 Hyper, 50 Sloomber,
  20 Magma Comb, 3 Royal Jelly; VIII: 80 Ringtail, 50 Twin, 30 Aqua Glass, 3 Royal Jelly). II+ also need the
  `TankBoosterComponent`.
- `AmmoSlots` (40) = Extra Tank I-II: ADD +1 / +2 on `AmmoSlotsAvailable` (default 4). 1500 / 5000 Newbucks + items,
  II also the `ExtraTankComponent`. `LiquidSlot` (80) = Water Tank.
- Player slot definitions (`PlayerState._ammoSlotDefinitions`): `AmmoSlot1-4`, `ExpansionAmmoSlot 1` / `2`
  (`AmmoSlotsAvailable` >= 5 / 6), `LiquidAmmoSlot` (`LiquidSlotEnabled` >= 1). Vanilla uses all of them:
  6 regular slots + water.

### Extra slots

Feedback: "they aren't selectable with buttons; would be amazing if they are; also they're after the water tank and
the inventory slots aren't centered."

- **Order.** `PlayerState.InitModel` / `Reset` (pre) insert two clones of `ExpansionAmmoSlot 2` (thresholds 7 / 8) right
  after it, before `LiquidAmmoSlot`, while the module is on: slots 1-6, Extra Tank III, IV, water. The HUD binds one
  view per slot in index order (locked ones hidden by `AmmoSlotAdapter.PrefilterData`), so they show before the water
  tank, and next / previous slot walk them in that order.
- **Save.** A load copies the saved slots **by index** into the player's slots (`InitModel` -> `AmmoModel.Reset(defs)`
  first, then `PlayerModel.Push` -> `AmmoModel.Push` 0xA6E530), and `ValidateAndAdjustSlots` clears ids a slot's
  definition does not allow - so the saved water (index 6) would land in Extra Tank III's slot and be lost. Now:
  `AmmoModel.Pull` (post, the player's model only) hands the save the 7 vanilla slots in the vanilla order (water back
  at index 6) and writes the extra slots to `custom_upgrades.vacpack.extra_slot_items` (`k=referenceId*count;...`, k =
  0 / 1); `AmmoModel.Push` (pre, the player's model only: its slot 6 has our definition) spreads a 7-slot array into the
  9-slot layout, our two positions taking their own empty live slots (Push copies them onto themselves). Log `loaded
  vacpack spread into the extended layout (saved slot 7 = water -> slot 9)`. The tick refills the extra slots from the
  sidecar (`MaybeAddResource` with overflow); an older build's `custom_upgrades.vacpack.extra_slots` (indices 7 / 8)
  is read once and converted. Slimes in those slots lose their emotions / radiant look (only type and count kept).
- **Number keys.** The game has one `selectSlot` InputEvent for `MainGame/Slot1..Slot7`; `VacuumItem.SelectSlot(
  InputEventData)` (0xA69640) selects slot `round(value)` (0..6, `Optional<float>` at +4 of the data). No Slot8 / 9
  actions exist and an action asset can't be added at runtime without managed code, so:
  - two Key settings (custom upgrades, group "Vacpack": "Extra Tank III slot key" = 7, "Extra Tank IV slot key" = 8),
    polled every frame (`GetAsyncKeyState` edge, game window in front, overlay closed, cursor locked and time running,
    `VacuumItem` active = not gadget mode); a press calls the game's own `VacuumItem.SelectSlot(int)` (sound, vac
    animation; a locked slot is refused by `SetSelectedSlot`). Log `number key 7 selects vacpack slot 7 (Extra Tank
    III)` once.
  - Defaults (round 4, user: "R selects water, not 7, so it's offset; 7 is missing as a key slot"): the game binds
    Slot1..Slot6 to 1-6 and Slot7 (the water slot) to R, so 7 / 8 are free; the first builds used 8 / 9. A key still on
    its old default (saved 8 for III, 9 for IV) moves once to the new one (marker `extra_slot_keys_v2` in sr2mods.json,
    log `slot keys moved to the new defaults`); the HUD labels follow the setting.
  - The water slot keeps the game's key (R): a `VacuumItem.SelectSlot(InputEventData)` detour maps a vanilla index at or
    after our slots to its index in our layout.
  - HUD key label: `AmmoSlotViewHolder.BindInputKey(definition.Hotkey)` with our null hotkey logged "No binding set for
    ammo slot!"; its detour now writes our key's name into `KeybindingText` (localized text off) for our slots and keeps
    the key box shown (hidden when the key is set to none). The label follows a changed key the next time the game
    rebinds labels (device / keys changed, reload).
- **Centred row.** HUD `Ammo Slots` is 900 x 189 with a `HorizontalLayoutGroup` (spacing 10, LowerCenter, no width
  control) over 108 px slots. Unity centres the children only while they fit (`GetTotalPreferredSize < size`); 9 slots
  are 1052 px, so the row started at the rect's left edge and overflowed to the right. The tick widens the rect to 900 +
  2 x 118 + 24 = 1160 px once the vacpack is extended (log `HUD slot row 900 -> 1160 px wide ...`); `Cleanup` puts the
  width back.
- **Module off / faulted:** `PlayerState` gets the vanilla array back for the next load, and the live slots are put back
  in the vanilla order with ours last, so a save made without our hooks keeps the water slot at index 6 (a mod-free load
  then drops slots 8-9 with the game's warning; their contents stay in the sidecar).

**The real slot limit.** No hard limit in code: the slot list is an array of any length (`AmmoModel.Reset`;
`ValidateAndAdjustSlots` pads / trims by index with a warning), the HUD binds one view per slot, the save is a list.
Practical limits: number keys (ours are polled), the save layout (handled above) and the HUD width (widened).

### Drone Battery Saver

First built as per-station tiers bought from a `[B] Battery` pill in the drone station menu; moved here after
user feedback ("they should be refinery upgrades, not in a separate panel"). Now one global upgrade, its own card.

- **Card.** A clone of `AmmoCapacity`, its single modifier set to ADD 0 (no vacpack effect; the effect list
  keeps the one entry the game expects). Icon and pedia entry: the Drone Station gadget's (`DroneDirector.
  _droneStationType`, a `GadgetDefinition`: `icon`, `_pediaLink`), set by the tick once the scene has it (log
  `Drone Battery Saver card uses the '...' icon`); until then / if missing, Tank Booster's. Listed at the end of
  the category (no vanilla parent). Its id is registered in `UpgradeModel.upgradeLevels` (-1) before the definition goes
  into the player's list (`ActorUpgradeHandler.SetModifiersFromModel` asks every listed definition for its level), is
  removed from the save's copy by the Pull detour and is not counted for TinkerTailorSlimes.
- **Prerequisite: the Drone Station blueprint** (`GadgetDirector.HasBlueprint`, checked when the Fabricator
  builds its items). An owned level always shows the card; an unknown answer (lookup failed) shows it too.
- **Prices: 7,500 / 10,000 / 15,000 / 25,000 Newbucks (57,500 for IV) plus half of Tank Booster V-VIII's refinery
  items** (rounded up, at least 1).
- **Effect** (`drone_battery.cpp`): every ~2 s each station gets back what it drained beyond the level's share since the
  last look, through the game's own `AddEnergy`; the saved battery stays a vanilla value. Level = the per-save value
  `custom_upgrades.vacpack.drone_battery` (0-based).
- **Verified by reading the code again** (user: "idk check logs"; the log of that test had only the `... active (battery
  drain x0.50)` lines, nothing about refunds). The drain is pure world time: `_energyDepleteTime` is written only by
  `AddEnergy` (one caller, `DroneFuelDepositor.OnTriggerEnter`), the station's init / `Push`; `SetEnergy` has no caller,
  so `energy = (depleteTime - now) / 3600 x EnergyDepletedPerHour` falls steadily and the refund `dt x (1 - 1/factor)`
  is right. Fixed: a refund below 0.01 energy was dropped and its time window forgotten (t0 moved on); now t0 stays
  until the accumulated refund is worth adding. New log lines: `station <id>: first refund +X energy for N world s
  (battery A -> B of 1000)` per station and level, then once a minute `Drone Battery Saver IV gave back X energy to N
  station(s) in the last 60 s: <id> +x (battery y), ...`.
- **Checked in the log (round 4, user: "check urself pls").** Refunds happen. Battery Saver IV, 3 stations: first refund
  +0.02 energy for 164 world s each, then +0.4 energy per station per real minute (1.1-1.3 in total; 0.0 / 0.9 in minutes
  the game was paused / in menus). Battery Saver II on one station: +0.1-0.2 a minute. Numbers: 1 real s = 60 world s, so
  a real minute is a game hour; +0.4 at x0.50 means the vanilla drain (`EnergyDepletedPerHour`) is about 0.8 energy per
  game hour and the station now loses about 0.4 (log: 321.8 -> 318 in 12 minutes). The refund is right, it is just small
  in absolute terms because the vanilla drain is slow (`ENERGY_OVERCHARGE_THRESH` 100, max 1000). The `active` line now also
  prints the drain per game hour (with and without the saver).
- **Migration.** Old sidecars hold `drone_battery.station.<actor id>` = 1..4. On the first tick after a load the
  highest tier of any station becomes the global level (if higher than an owned one), all those keys are erased
  and one line is logged.

**Not verified in game yet:** the 12 circles / 12 pips (positions, the highlight), buying IX-XII and III-IV on the
vanilla cards, the capacity values, save + reload (`... from the sidecar` lines), a mod-free load of such a save, the
extra slots before the water slot, the number keys (7 / 8) and their HUD labels, R still selecting water, the centred row,
the battery saver's refund lines. Known rough edges: the save's "viewed upgrades" set (`ViewedItemsV02`) may get the
Battery Saver id (harmless for a mod-free load); pinning the Battery Saver as the recipe was not checked for a mod-free
load.

### Lava tank: not built (research)

Lava in 1.3.0 is not a liquid the vacpack knows: it is `LavaVolume`, a trigger that calls `ILavaTriggerReact` on
whatever enters it (`DestroyOnLava` poofs items after `_hoursOfContactAllowed`, `TakeDamageInLava` hurts the player,
`BounceInLava` makes slimes / chickens hop out). There is no lava `IdentifiableType`: the only `LiquidDefinition`s are
`Water` and `MagicWater` (identifiabletypes bundle). The vacpack takes liquids from a `LiquidSourceSurface`
(`LiquidIdentifiableType`, `DestroyOnVac`, `FillVac`) registered in `LiquidSourceRegistry`. A lava tank would need:

1. a new `LiquidDefinition` "Lava" made at runtime (clone of `Water`) with a reference id, icon, colour, localized
   name, a shoot prefab (what leaves the vac) and the in / vac-fail FX;
2. registration in `LookupDirector` (`_identifiableTypeByRefId`, the type groups) and, the hard part, the save's
   persistence-id tables (`SaveReferenceTranslation` / `LoadReferenceTranslation`): a slot holding an unknown type
   cannot be saved, so it needs the same strip-and-sidecar path as the extra slots, and lava must be kept out of every
   other place a liquid can end up (silos, drones, plots and gadgets that take water);
3. `LiquidSourceSurface` components (and colliders on the vac's layer) added to every `LavaVolume` object at runtime,
   so the vac can suck from it, with `DestroyOnVac` off;
4. a slot that takes it: a "lava tank" `AmmoSlotDefinition` (clone of `LiquidAmmoSlot` with its own type group that
   holds only Lava, unlocked by a new upgrade property), appended like the extra slots;
5. something for lava to do when shot: nothing in the game reacts to it (no food for fire slimes, no incinerator
   input), so this is new gameplay.

1, 3 and 4 follow the patterns above; 2 is the risky part (save format); 5 needs a design.
