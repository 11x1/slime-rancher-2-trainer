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
  Keys: d-pad / left stick / arrows for hens (`SliderInput::kPrimary`), right stick / Shift +
  arrows for roosters (`kSecondary`). The grid navigation detour offers left / right to both;
  each takes only its own (Shift held or not).

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
  under "Icon Background") is hidden too. The popup-confirm guard from feature/plot-props-2 stays as a
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
One row per sellable plort type (icon, name, "x count", "@ unit price", line total + coin), the grand
total, Cancel and Sell. Market closed / unreachable or no plorts: the old text above the button, no
dialog.

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
