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
- **Select to step** (feedback round 4: "only when selected should it be focused, right now arrow
  buttons still navigate"). Nothing selected: the arrows / d-pad / stick navigate the menu as usual,
  the sliders ignore them. A slider is selected by a mouse press on it (`Selectable.IsPressed`, polled)
  or by Submit (Enter / pad A: the hens slider). The selected slider's label reads `< Keep hens: 18 >`;
  left / right (arrows, d-pad, left stick, Shift or not) step it and no move reaches the menu; up / down
  switch the selection to the other slider. Released by Submit again, Cancel (Esc / B: the menu stays
  open), a mouse press anywhere else (judged two frames later, EventSystem order), or, when it was
  selected with the mouse, the mouse leaving it (not while the button is held). A press on one slider
  releases the other. Hooks: `ListUI.Submit(InputEventData)` (0x83F450, the list menus' submit listener;
  the owned Hen Collector card's OnAction does nothing anyway) and `BaseUI.OnCancelPressed` (0x683490,
  `if (CanClose()) Close()`), both shared bodies with one flag check; up / down / left / right through
  the `TryHandleAsNavigation` detour below. Without the two hooks: the old always-on keys (d-pad / left
  stick / arrows for hens, right stick / Shift + arrows for roosters).

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

### Slider keys (fix after the test of build 506a4f60)

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
- Not a separate "Hen Collector" source: a new `DroneTaskSourceType` value would need the UI's
  source definitions (an asset) and would be written into the vanilla save's `DroneTaskData`.
- Known edge: turning custom upgrades off while a drone flies to a Hen Collector port that still
  holds chickens leaves that drone with a destroyed port (vanilla has the same case when a deluxe coop
  is demolished). Selling is refused while it holds chickens, so selling can't cause it.

Not checked in game yet: a drone with Coop -> Silo (target Meat, or Hen) emptying the Hen Collector,
the log lines, no errors after buying another coop upgrade (re-link) or selling the Hen Collector.

### Drone source "Hen Collector" (user request, on top of the Coop linking; `drone_source.cpp`)

- A real source in the program's source picker, right after Coop, while custom upgrades are on and
  at least one coop has a Hen Collector, for the targets Coop is offered for (meat, hens, roosters).
  It takes only from the Hen Collector ports: no live chickens, no elder collector.
- In memory the program carries our `DroneTaskSourceType` value `0x53520101`. Hooks:
  `RanchDrone.GatherSourceTask` (our type -> `DroneTaskSource` with one new
  `RanchDroneSourceProgramElderCollector`; an unknown type would throw in vanilla),
  `RanchDrone.GatherSourceTasks` (insert after Coop), `RanchDroneSourceProgramElderCollector.GetSources`
  (for a station whose program has our type: only the Hen Collector slots, as a `StorageMetadata[]`),
  `DroneUIDefinitionSource.FindSourceDefinition` (a runtime copy of the Coop definition: "Hen
  Collector" through text_override, the Hen icon, sort index Coop + 1).
- **Save (user decision: "save it as coop, the sidecar says the true source").**
  `DroneStationGadgetModel.Pull` gives the save a copy of the program data that says COOP (PullGadget
  turns it into the saved `DroneTaskV01` right away); the live data keeps ours. If the copy can't be
  made the live data becomes Coop (logged). The sidecar value `drone_hen_source.stations` lists the
  station actor ids (`PlacedGadgetV07.ActorId`, stable across loads) whose program is Hen Collector. A
  ranch drone station has one program, so the station id is the key.
- **Load:** the first ~1 Hz tick that sees a listed station whose saved program is Coop switches it
  back (`DroneStation.AssignRanchTask` when its drone exists, else the program data; `RanchDrone.Init`
  then builds our source). Until then (about a second) the drone runs as Coop.
- **Stale entries:** a listed station whose program is now something else (changed by the player,
  or not Coop in the save) is dropped; listed ids that no station has any more (picked up and placed
  again = a new actor id, demolished) are dropped with a log line - a re-placed station has a Coop
  program. No Hen Collector left (sold) -> the program falls back to Coop (logged) and the entry goes.
- **Custom upgrades off / faulted:** `OnDisable` writes Coop into every program that had ours (plain
  field writes, before the hooks go off, so a later save can't see our value); `Cleanup` (main thread)
  re-programs those drones with Coop. When it is on again the sidecar's stations are mapped back.
- **Icon (user request): the Plort Collector source icon with the Hen on its output opening**
  (`drone_icon.cpp`). Asset data (UnityPy): `DroneSourceDefinitions` in the drone station menu scene;
  PLORT_COLLECTOR uses `iconPatchCorralCollector` (512 x 512, full-colour art, not readable): a
  collector whose dark round opening spans x 141..314, y 147..320 (Unity y up), a pink plort leaving
  it. The Hen sprite (the task config's `HenType.icon` = `iconBirdHen`) is scaled so its opaque box is
  200 px wide and centred on the opening (228, 233); no tint (both are coloured art, not glyphs). Built
  once, at the first definition lookup after a drone source was built (that brings the Hen icon):
  `Graphics.Blit` into a temporary RenderTexture + `Texture2D.ReadPixels` of each sprite rect, composite
  on the CPU (bilinear, premultiplied alpha), new Texture2D + `Sprite.Create`, DontUnloadUnusedAsset +
  GC handle (never destroyed). The definition's `_icon` is that sprite, so the picker, the applied
  program and the Drone List all show it. Any failure: the plain Hen icon (log `icon not composed`).
  Offline preview with the same numbers: `scratchpad/hen-source-icon.png` (`scratchpad/henicon/compose.py`).
- **Without the mod:** the save only ever holds Coop programs; it loads and runs as Coop (the drone
  takes live chickens and the elder collector; the Hen Collector itself is gone without the mod).

Not checked in game yet: the option in the source picker (name, icon, position), applying it, a drone
emptying only the Hen Collector, save + reload (log `restored from the sidecar`), the Drone List tab
showing it, selling the Hen Collector (fallback log), turning custom upgrades off with such a program.

## Buy menu cards (fixes after the test of build 506a4f60)

- **Purchase / Sell overlap.** The buttons size themselves: the prefab `TextButton_NoneSelectable`
  (UnityPy, `duplicateassetisolation_assets__54b00e46...bundle`) has the game's own layout
  (`LayoutManager`, `HorizontalLayout`, `EncapsulateChildren` -> `SetSizeWithCurrentAnchors` in
  LateUpdate; the label has `ResizeToPreferred`), so the old half-width split was overwritten and the
  buttons grew back to full width. Now (sell_upgrades `PlaceButtons`) only pivots / positions are
  set: owned card -> PURCHASED hidden, Sell alone in its place; tier card -> Purchase pivot x 1 left
  of the old centre, Sell pivot x 0 right of it (16 px gap), re-centred for a few ticks once the
  layout has given them their widths. Applies to every plot menu.
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

Not checked in game yet: all of the above (layout and centring, the hidden icon, that selling no longer
buys, the label, the slider keys on keyboard and pad, the Activate button's position, prompt and
animation).

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
  root takes the click (Button.onClick is empty in the prefab). The keyboard / pad focus is a teal
  (Cancel) / pink (Sell) ring behind the focused button, fitted to its self-sized rect for 30 frames
  after a build; disabled Sell = `Selectable.interactable` false (the prefab's disabled colours).
  Fallback when any piece is missing: our own SDF buttons with "(Esc / B)" / "(Enter / A)" in the
  label. Log: `silo receipt: buttons cloned from the game's popup, rows fit|scroll (N px, canvas H high)`.

- **Input lock = the game's own.** The dialog root carries the game's `UIInputLocker` component, as
  menu prefabs do: `OnEnable` -> `TimeDirector.Pause` (timeScale 0, ref-counted) ->
  `InputDirector.OnPausedChanged` disables the player action map and enables the UI map (no movement,
  no look, no interact), `CursorLockHandler` frees the cursor while paused, non-UI sound pauses.
  Closing deactivates the root first (`OnDisable` -> `Unpause` at once), then destroys it. Every close
  path (cancel, sell, load, module off, fault, scene change) goes through that, so the game cannot
  stay paused because of the dialog.
- **Keys** (polled from `Keyboard.current` / `Gamepad.current`, nothing hooked): Left / Right, A / D,
  Tab, d-pad, left stick switch Cancel / Sell; Enter / Space / pad A press the focused one (Sell is
  focused first); Esc / pad B cancel. Mouse: the two buttons, a click on the dimmed backdrop cancels.
  Input is ignored for 0.3 s after opening or refreshing, so the press that opened it never confirms.
- **What is sold.** Sell quotes the silo again. Only an identical quote (same types, counts and
  prices, market still open) is sold, with the quote as the plan: at most the shown count per type
  (`Summarize(..., plan)`). Anything else refreshes the receipt with an orange note ("Prices or
  contents changed - this is the new receipt", "The Plort Market just closed") and sells nothing; Sell
  is disabled while nothing can be sold. Time is paused while it is open, so a change is unlikely.
- **After the sale** the dialog is already closed; the per-plort HUD notices are queued as before
  (shown once the HUD takes them), the text fallback above the button stays for a missing HUD list.
  `SaleNotices` (`sale_notify.cpp`) also takes a notice kind now: the sandbox module queues
  `Notice::kCollected` (`AddResourceCollected`, the drones' cloud-upload notice) for its spawns and
  crafts; the Sell-All keeps `kPlortSold`.
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

Storage per silo slot: 100 -> 200 (vanilla) -> 300 -> 400 -> 500 (ours).

- **Vanilla** (prefab `patchSilo` read with UnityPy, disassembly): one `SiloStorage` with 12 slot
  definitions, all `SiloNonSlimes` (`_maxAmount` 100). STORAGE2 / 3 / 4 (500 / 575 / 650) only swap
  slot GameObjects (`StorageUpgrader.StorageAdd*` / `StorageOnly*`); the single capacity upgrade
  STORAGE_CAPACITY_INCREASE ("Storage Capacity Upgrade", 1800, in the
  `scriptableobjectlookupdata_assets_all_*` bundle) runs `AmmoSlotManager.UpdateMaxCount(
  StorageCapacityUpgradeAmount = 200)` (`StorageUpgrader.Apply` 0x996E30) and shows the four
  `CapacityUpgrade1..4` parts. So vanilla has one capacity tier, 100 -> 200.
- **Ours:** Silo Capacity II / III / IV (`0x53520060..62`, keys `silo_capacity_2..4`), 300 / 400 / 500
  per slot, **3000 / 4500 / 6500**. A tier chain on the vanilla Storage Capacity card: once that is
  owned the card shows Silo Capacity II, then III, then IV (greyed with the checkmark when all are
  owned); `ChainVanillaRoot` gives every tier of a chain that starts on a vanilla upgrade that
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
- **Not sellable**, like the vanilla silo storage upgrades: `CanSell` gives the same permanent reason
  ("silo storage"), so sell_upgrades shows no Sell button and no alt-action prompt.

Not checked in game yet: the card chain (vanilla card -> II -> III -> IV), the slot displays showing
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

Not checked in game yet: the Music Box II speaker (its Animator runs without the game's music box
script; it may stand still or play its idle), the scattered coop layout, the -x sprinkler head,
and whether the phantom purchase is gone. Also not checked yet: sizes and heights (hay bales, nests, pillow), whether the incinerator's
`Glow` / `Embers` play on their own once active (they may be sub-emitters of `FireTest`) and look
right, and that nothing of the stripped clones still logs errors.

## Vacpack upgrades (Fabricator)

`vacpack_upgrades.cpp`. Two new cards in the Fabricator's Upgrades category, each a clone of a vanilla
`UpgradeDefinition`, bought through the vanilla flow (recipe panel, cost check, `SpendableDirector.Spend`,
`UpgradeModel.IncrementUpgradeLevel`; the game's own modifier code applies the effect).

| card | levels | effect | shown once |
|---|---|---|---|
| Tank Booster IX-XII (id `0x53520101`) | 4 | +25 / +50 / +75 / +100 items per slot on top of Tank Booster VIII: 125 / 150 / 175 / 200 | Tank Booster VIII owned |
| Extra Tank III-IV (id `0x53520102`) | 2 | a 7th / 8th vacpack slot | Extra Tank II owned |

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

**Costs (our choice).** Each level copies the refinery items of a late vanilla level, in the vanilla style:
Tank Booster IX-XII use V-VIII's items with the amounts doubled (+2 for the rare ones vanilla asks 1-3 of) and
60,000 / 75,000 / 90,000 / 120,000 Newbucks (vanilla VIII: 50,000). Extra Tank III / IV use VII / VIII's items
unchanged and 25,000 / 50,000 Newbucks (vanilla Extra Tank II: 5,000; a slot this late is worth more than +25 per
slot). No upgrade component: vanilla consumes one Tank Booster / Extra Tank component per level (counted items from
treasure pods / the shop) and the world may hold no more than vanilla needs, so ours would be unbuyable; the cloned
component list is cleared. The log lists every level's exact cost (`vacpack: TankBoosterPlus ready ...`).

**How it works.**

- Definitions: `Object.Instantiate(AmmoCapacity)` (deep copy of levels, modifiers, costs and LocalizedStrings; checked,
  else nothing is offered), levels cut to the template range, `_uniqueId` set, effect value / target, costs and texts
  changed (core/text_override; the Fabricator shows level texts through `LocalizeStringEvent.set_StringReference`).
  Extra Tank III-IV points its modifier at `AmmoSlotsAvailable` and takes the vanilla Extra Tank icon and pedia entry.
- Registration (world tick, in game): our ids into `UpgradeModel.upgradeLevels` (-1) first, then the definitions into
  the player's `UpgradeDefinitionList` behind their vanilla parent (`ActorUpgradeHandler.SetModifiersFromModel` asks
  every listed definition for its level; a missing key would throw). The Fabricator category's list gets them in a
  `PlayerUpgradeFabricateCategory.GetItems` pre-hook; the post-hook drops our cards while the vanilla upgrade is not
  maxed (and when the module is off). The Fabricator builds its items when it opens, so a card that becomes
  available while it is open shows after reopening it.
- Save: `UpgradeModel.Pull` (post) removes our ids from the save's copy. The level is a per-save value
  (`custom_upgrades.vacpack.tank_booster` / `.extra_tank`, 0-based) written by the `UpgradeModel.SetUpgradeLevel`
  detour on a purchase; the tick puts it back on the model after a load (`SetUpgradeLevel`, modifiers applied by the
  game). A mod-free load sees the vanilla dictionary only (Push ignores unknown ids anyway).
- Achievement: `CountUniqueUpgrades` (TinkerTailorSlimes progress) subtracts ours.
- Extra slots: `PlayerState.InitModel` / `Reset` (pre) append two clones of `ExpansionAmmoSlot 2` (thresholds 7 / 8,
  no hotkey) to the player's slot definitions before the game builds the `AmmoSlotManager`, while the module is on.
  Locked slots are hidden by the HUD (`AmmoSlotAdapter.PrefilterData` filters on `IsUnlocked`), so nothing shows until
  Extra Tank III. `AmmoModel.Pull` (post, the player's model only) writes the slots past the vanilla count to the
  per-save value `custom_upgrades.vacpack.extra_slots` (`index=referenceId*count;...`) and hands the save a copy of
  the first 7 slots; after the next load the tick refills empty extra slots from it (`MaybeAddResource` with overflow,
  type by `LookupDirector.TryFindIdentifiableTypeByReferenceId`). Slimes in those slots lose their emotions / radiant
  look (only type and count are kept). A save made before the refill keeps the old value.
- Module off (only possible from the title screen or a save without custom upgrades, see the gate): levels set to -1
  (modifiers removed), our ids and definitions out of the game's lists, `PlayerState` gets its vanilla array back for
  the next load (the live vacpack keeps its 8 slots, locked, until then). The sidecar keeps everything. Switched on
  after a save was loaded (the gate does this a moment after the load): the levels come back at once, the extra slots
  only after loading the save again (logged).

**The real slot limit.** No hard limit in code: the slot list is an array of any length (`AmmoModel.Reset`;
`ValidateAndAdjustSlots` pads / trims by index with a warning), the HUD binds one view per slot, the save is a list.
Practical limits: input actions exist only for `MainGame/Slot1..Slot7` (7 = the water slot), so slots 8+ have no
number key (mouse wheel / next-previous only; `BindInputKey(null)` logs the game's "No binding set for ammo slot!"
warning); new slots come after the water slot (index order; putting them before it would move the saved water slot
into a regular one); and the width of the HUD row (not measured). Two extra slots = 9 icons in the row.

**Not verified in game yet:** everything above. Known rough edges: Unity logs "Unserializing ammo slot length different
than expected" on every load with the module on (7 saved vs 9 defined, harmless); the save's "viewed upgrades" set
(`ViewedItemsV02`, a set of ints) may get our ids (harmless for a mod-free load, unknown ids are never looked up);
pinning one of our upgrades as the recipe (`PinnedRecipeDirector`, saved as a string) was not checked for a mod-free
load. After a fault the hooks are gone: the next save then contains our ids and 9 slots; a mod-free load ignores the
ids and drops slots 8-9 with the game's warning.

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
