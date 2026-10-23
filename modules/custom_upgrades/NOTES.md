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

## Tier props (checklist u5, u6, u9, u11)

`plot_props.cpp` puts a visible prop on a plot while a tier is owned; it goes when the tier is
sold or the module is turned off. No assets of ours: every piece is a child of a vanilla prefab,
cloned under an inactive holder `sr2props_<Kind>#<generation>` (prefix not `sr2mods_`, which
coop_world's scan would destroy), stripped of colliders, joints, rigidbodies and game scripts
(MonoBehaviours outside `UnityEngine.*`), then activated. Lookups are cached and pinned.

| tier | prop | source |
|---|---|---|
| Faster Hens I or II (u5) | two small hay bales (scale 0.5) along the +z side | decoration `decorHayBales`: `hayBale_2`, `hayBale_0 (1)` |
| Faster Hens II (u6) | nests in the middle of the -x / +x sides, a stuffed chicken in the +x nest, a flower pillow in the middle of the -z side | coop prefab `Deco/nestPlain01` (the vanilla nest), toy `toyStuffedChicken`: `model_stuffedChicken`, decoration `decorFlowerPillowSmall`: `mesh` |
| Fast Growth I or II (u9) | a third sprinkler head in the middle of the +x side, spraying inwards | garden prefab `Sprinkler/SprinklerBase` (vanilla heads are on the -z / +z sides) |
| Ash Booster (u11) | glow + embers in the fire, embers rising out of the opening | incinerator prefab `.../meshIncinerator/FireTest/Glow` and `/Embers` (inactive in vanilla, switched on), decoration `decorFireFlower`: `FX Fireflower` |

Gadget / toy prefabs are found by prefab name in `LookupDirector._gadgetDefinitions` /
`_toyDefinitions` (member types, then sub-groups). A missing model is logged once
(`plot props: model '...' not found`) and that piece is left out.

Not checked in game yet: sizes and heights (hay bales, nests, pillow), whether the incinerator's
`Glow` / `Embers` play on their own once active (they may be sub-emitters of `FireTest`) and look
right, and that nothing of the stripped clones still logs errors.
