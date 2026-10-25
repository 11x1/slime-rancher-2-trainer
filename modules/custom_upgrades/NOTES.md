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
| `plot_menu_slider.cpp` | reusable uGUI slider for the plot menu details panel (also used by drone_price_limit) |
| `silo_world.cpp`, `silo_receipt.cpp`, `sale_notify.cpp` | Sell-All world button, receipt dialog, HUD notices |
| `plot_props.cpp` | decorative props for owned tiers |
| `drone_source.cpp`, `drone_programs.cpp`, `drone_battery.cpp` | drone sources, several programs per station, Battery Saver effect |
| `quantum_link.cpp`, `variant_ext.h` | Quantum Storage Link; owns the gadget-variant hooks the DJ buildables plug into |
| `dj_buildables.cpp`, `radio_shop.cpp` | DJ Booth, DJ Speaker Stack, Slime Radio; the radio in the shop |
| `gadget_look.cpp`, `buildables.cpp` | UI looks of mod gadget definitions; `buildables.h` API for the Sandbox |
| `vacpack_upgrades.cpp`, `fabricator_levels.cpp`, `shadow_catcher.cpp` | Fabricator vacpack upgrades, extra level circles, Shadow Catcher effect |

## Plot upgrades (ids, keys)

Ids `0x5352xxxx` (`api.h kIdBase`); the key is stored per plot in the sidecar. Never reuse a retired id or
key: `0x0002 silo_sell_all_now`, `0x0010 corral_collector_2`, `0x0011 corral_collector_3`,
`0x0012 corral_feeder_2` (a sidecar that still lists one loads fine; ignored and logged).

| id | key | plot | needs | price | what |
|---|---|---|---|---|---|
| 0x0001 | `silo_sell_all` | silo | - | 10000 | Sell-All world button + receipt |
| 0x0060..63 | `silo_capacity_2..5` | silo | Storage Capacity / previous | 3000 / 4500 / 6500 / 9000 | 300 / 400 / 500 / 600 per slot |
| 0x0013 | `corral_music_2` | corral | Music Box | 2000 | Music Box II, 2x calming |
| 0x0014 | `corral_radio` | corral | Music Box | 1500 | Corral Radio (slime_dj plays the corral's own playlist) |
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
corral Air Net's (`coop_air_net`), Silo Capacity the Storage Capacity's. The Hen Collector has the user's
`hen_collector_source.png`. A drawing under one of those names takes over at the next build
(`.claude/notes/custom-icons.md`).

Per-plot values: `hen_collector`, `hen_collector_keep_hens` / `_roosters` (plus other modules':
sell_upgrades' Ash Trough fill, `slime_dj.radio`). Per-save values: `custom_upgrades.on` (gate mark),
`custom_upgrades.vacpack.*`, `drone_*_source.stations`, `drone_programs.station.<id>`, `quantum_link.*`,
`dj.*`, `buildables.unlock.<id>`; formats in the sections below.

## Hooks owned here

MinHook allows one hook per address; other modules must not hook these:
- module.cpp: `PlortCollectorActivator.Activate` (null Collector -> never the original; Sell-All buttons,
  Hen Collector Activate), `LandPlot.HasUpgrade` / `AddUpgrade`, `LandPlotLocation.Replace`, the menu
  update / `RebuildUI` hooks.
- quantum_link.cpp (DJ buildables via `variant_ext.h`): `SaveReferenceTranslation.GetPersistenceId`,
  `GadgetDirector.HasBlueprint` / `GetItemCount`, `GadgetDirectorHelper.HasBlueprint`,
  `GadgetFabricateCategoryRuntime..ctor`, `GadgetFabricatableItem.get_ShouldShowPlaceholder` /
  `FabricateAndSpendCost`, `GadgetItem.PlaceGadgetEvent` / `PickupGadget`, `Gadget.ChangeVariant`,
  `StorageMetadata.Decrement`, `UIActivator.OnInteract`, `UIDisplayInteractable.OnInteract`.
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
  tick to the top and footprint of the active CoopRegion (High Walls lift it).
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
- **Limits**: hens 0..58 (default 18), roosters 0..20 (default 2) per coop; `hen_collector_keep_hens` /
  `_roosters` (an old `hen_collector_keep` n = min(2, n) roosters, rest hens). Two `PlotMenuSlider`s in one
  row (`SliderSpec.columns`, `Create(..., rowOf)`), **mouse only** (`keys = false`, gamepad later);
  `PlotMenuSlider` keeps pad / key support for drone_price_limit.
- **Activate button**: the vanilla collector button is kept (Collector null) while the Activate detour is
  installed (`CoopWorld::KeepHenButton`; build line `Activate button kept` / `no Activate button`). A press
  runs the excess-only vacuum at once (the 6 h period restarts) with animation, cue and 0.4 s rate limit
  (`SiloButtons::Press`) and a short world text ("Collecting N chickens", "Nothing to collect (keeps H hens /
  R roosters)", "The Hen Collector is full", "Already collecting").
- **Ranch drones (Coop)**: the tick appends the Hen Collector's two slots to the coop's
  `RanchMetadata ... elderCollectors` (`LinkDrones`, log `linked to the ranch drones`); every Register
  replaces the entry, so the next tick adds them again. Sold / not owned / off:
  `RanchMetadata.OnUpgradesChanged(plot)` (`unlinked from the ranch drones`). Coop with a meat / hen /
  rooster target thus also empties the Hen Collector, fast-forward included. Edge: turning the module off
  while a drone flies to a port that still holds chickens leaves it a destroyed port (as vanilla on a
  deluxe coop demolish). RE: re/drones.md (RanchMetadata).

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

`drone_programs.cpp`; UI (`[P] Programs` pill and dialog) in drone_price_limit `station_dialog.cpp`, only
while custom upgrades are on. RE: re/drones.md (AssignRanchTask, DroneBehaviorPlexer,
RanchDroneSourceProgramDynamic).

- Up to 3 programs per station; the game keeps one (`_taskData`); switching = `DroneStation.AssignRanchTask`
  (what Start Program does).
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
  nothing to gather that the destination has room for".
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
- **Look** (tick): materials instanced, colours 60 % purple, emission cyan, tagged ` [sr2q]` (originals
  back when plain). Holder `sr2qlink#<gen>`: a teleporter `_activeFX` clone (0.3 size, 1 m up) that
  glitches every 3.5-8 s for ~0.5 s (`QuantumLink::Frame`); the cloud-withdraw FX pulses every ~7 s and at
  every take. Icon: a purple copy of the Refinery Link sprite (`TintedSprite`) in `icon` and
  `quantum::SourceIcon()`.
- **Use**: HUD notices (what it is, then the five largest cloud amounts or "The Quantum Cloud is empty";
  `QueueText`, 1.5 s cooldown) instead of the refinery screen (fallback without a HUD list). Still a
  refinery input. Log `Use component '<class>' on '<object>'`; `- not hooked` = Use stays vanilla.
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

`gadget_look.h` (owns four hooks, above). RE: re/fabricator.md (GadgetDefinition, GetTargetGadgetInfo,
SetHeldGadget, FabricatorItemDetails). `Register(def, Look{name, description, icon, dressPreview})`: own
description on the card (vanilla walks `VariantOf` to the root), our name as the gadget-mode label while
held and, via `AddPlacedLabel` providers, when aiming at a listed placed gadget; `dressPreview` runs on the
placeholder after `SetHeldGadget`; `icon` is the card's details picture (`_fullArt` / `_requiresFullArt`
cleared). Helpers `TintTree` (tag ` [sr2look]`), `TintedSprite`. Log `mod gadget looks ready: ...`.

## Slime DJ buildables

`dj_buildables.cpp`. Uses the quantum hooks through `variant_ext.h` (nothing without the Quantum Storage
Link's Setup); songs through `slime_dj/api.h` emitters. RE: re/fabricator.md (decoration / toy prefabs,
ToggleVariant, stacking), re/audio.md (GadgetMusicPlayer, corral Music Box), re/slimes.md (SlimeAttachPoint).

- **DJ Booth** = Scientist Desk variant. Faces +z, DJ behind (-z). Clutter and its sound off; a boombox
  face up in the middle (deck); two small ones on the front corners (x +-0.8, z 0.3, 0.4 size); two big
  ones on the floor at x +-1.75 (emitters, kind Booth); behind (z -1.8) the Boombox's bench with the
  Flower Pillow's mesh, colliders and `SlimeAttachmentPoint` with its scripts (largos: `_allowLargo` before
  Awake, trigger 1.5x). Other clones script-free. E: a `GadgetMusicPlayer` then a `GadgetUIInteractable`
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
    loop; fallback the Boombox track); stack / Boombox -> the Boombox track via the original `OnInteract`.
    One gadget track at a time: when the game stops ours, the record switches off.
  Logs `dj buildables: <label> actor N switched on|off (interact)`, `... plays the Slime DJ song|the
  Boombox's own track (...)|nothing`, `standby tune = the corral music box's '<cue>'`, `DJ Booth actor N: a
  slime sits behind the desk|nobody behind the desk any more`.
- **Plain Boomboxes** (up to 40): emitter on `model_boombox`, same E / dance rules (log `N plain
  Boombox(es) play the Slime DJ song when switched on (...)`).
- **Slime Radio** = a vanilla Night Light toy; its card definition is a Boombox clone, never a variant or
  in the inventory: fabricating spends the cost and spawns the toy 1.8 m in front of the camera (posted,
  at most 5 per click), id into `dj.radios`. Dressing: night light off, a small speaker stack (0.26)
  centred on the root with its box collider (ball off), upright with rotation x / z frozen (re-applied if
  the vacuum resets it); emitter kind Radio (0.6x range, 80 %), plays whenever a song plays. A radio that
  leaves the world raises `dj.radios_away` (max 20); the next Night Light appearing within 6 m of the
  camera is a radio again (logged). Failed spawns go to `dj.radios_owed` (up to 5 per tick later).
- **Fabricator** (Decorations): booth after the Scientist Desk (Newbucks x2.5, items x2), stack after the
  Boombox (x1.5), radio after the stack (x0.5), Newbucks rounded to 5. Booth / stack: vanilla decoration +
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
- **Buttons**: clones of the popup's `TextButton_NoneSelectable` (InputEventButton and label LSE off, own
  `LayoutManager`, slot scaled 0.72, placed by the outer edge for 30 frames). Click polled from the Button
  (`IsPressed` last frame, not now, pointer inside; a Toggle can't be added). A failed build logs `panel
  build failed at: ...` and retries with our own SDF buttons.
- **Keys** (polled): Sell = the popup's positive InputEvent, Enter, pad A; Cancel = C, Esc, pad B (glyph
  shows "C" for keyboard). A key held at open is no press; input ignored 0.3 s after open / refresh; a
  backdrop click cancels.
- **Input lock**: the game's `UIInputLocker` on the root; every close path deactivates the root first.
- **What is sold**: Sell re-quotes; only an identical quote is sold (at most the shown count per type),
  else the receipt refreshes with an orange note.
- Logs: `silo receipt: buttons cloned from the game's popup, rows fit|scroll (...)`, `sell all (plot
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
the corral's own playlist (per-plot `slime_dj.radio`, key from `PlotKey`). Selling the Music Box is refused
while the radio is owned; selling the radio keeps its playlist. Details: `modules/slime_dj/NOTES.md`
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

### Shadow Catcher

Card in `vacpack_upgrades.cpp` (`kShadowCatcher`), effect in `shadow_catcher.cpp`. RE: re/slimes.md
(ShadowSlimeScatter, ProducePlortsOnHit, SlimeEat / hunger, DestroyOutsideHoursOfDay), re/world.md
(IsOnRanch, CaveTrigger), re/slimes.md.

- **Card**: like the Battery Saver (value `custom_upgrades.vacpack.shadow_catcher`), always shown. 40,000
  + Tank Booster VII's items with the 70 Hyper plorts swapped for 25 Shadow plorts once the Shadow slime is
  found; Shadow icon; description ends with the sunlight warning.
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
- Vacpack: 12 circles / pips, buying the tiers, save + reload, mod-free load, extra slots and keys, centred
  row. Shadow Catcher: the whole feature.

## History

- Rounds 4-5: Hen Collector two limits and sliders (now mouse only), Additional Storage merged, Silo
  Capacity V, Battery Saver moved to the Fabricator, vacpack tiers merged into the vanilla cards.
- Rounds 7-8: Fabricator cards moved to the category-runtime constructors; Slime DJ buildables.
- Rounds 9-10: gadget looks, one DJ Speaker Stack, radio as a daily toy, Shadow Catcher hunger / sunlight,
  gold button on HDRP/Lit, Sell button diagnostics. Details: git log.
