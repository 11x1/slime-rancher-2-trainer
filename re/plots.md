# Land plots, plot menu, plot upgrades, collectors, silos, coops

`MP.SR` = `MonomiPark.SlimeRancher`. Tags: **code** = decompiled/disassembled; **dump** = signature only; **data** = asset bundles (UnityPy). Mod use: `modules/custom_upgrades/` (menu injection, tiers, world objects: `coop_world.cpp`, `silo_world.cpp`, `plot_props.cpp`, `plot_menu_slider.cpp`), `modules/plot_upgrades/` (tier effects), `modules/sell_upgrades/` (selling). Plot-menu list UI internals: re/ui.md "List menus". Hooks: re/hooks.md.

## LandPlot / LandPlotModel / LandPlotLocation

- `LandPlot.Upgrade` enum: NONE 0, WALLS 1, MUSIC_BOX 2, STORAGE2 3, STORAGE3 4, STORAGE4 5, SOIL 6, SPRINKLER 7, SCARESLIME 8, FEEDER 9, VITAMIZER 10, AIR_NET 11, PLORT_COLLECTOR 12, SOLAR_SHIELD 13, ASH_TROUGH 14, MIRACLE_MIX 15, DELUXE_GARDEN 16, DELUXE_COOP 17, PLORT_COLLECTOR_POND 18, PLORT_COLLECTOR_INCINERATOR 19, STORAGE_CAPACITY_INCREASE 20. `LandPlot.Id`: CORRAL / COOP / GARDEN / SILO / POND / INCINERATOR ...
- `LandPlot.HasUpgrade(Upgrade)` 0x989190: tail call into `LandPlotModel.HasUpgrade`. custom_upgrades hooks it (custom ids answered from the sidecar).
- `LandPlot.AddUpgrade(Upgrade)` 0x988750: `model.upgrades.Add` + `ApplyUpgrades`. Hooked: custom ids never reach the original.
- `LandPlot.ApplyUpgrades`: runs after load and every change; `RanchMetadata.OnUpgradesChanged(plot)` rebuilds drone metadata (re/drones.md).
- `LandPlotModel.upgrades` HashSet<Upgrade> (saved). `LandPlotModel.siloAmmo[guid]` (silo/collector storage, saved). `LandPlotModel.Init` / `NotifyParticipants`: only `InstantiatePlot` (a new plot object) calls `InitModel`/`SetModel` on plot children. `collectorNextTime`; `GetAmmoSet` (vanilla `new AmmoModel(null)`).
- `LandPlotLocation._id` (plot id, sidecar key). `LandPlotLocation.Replace(LandPlot, GameObject)` 0x871E60: the only plot swap (destroys the old LandPlot, unregisters its model, registers a new one under the same id). Callers: `LandPlotUIRoot.BuyPlot` (buying on an empty plot; the demolish closure `<PromptForDemolish>b__0` 0x703350 buys the empty plot through BuyPlot) and `LandPlotUIRoot.Replace`. custom_upgrades hooks it to drop the id's sidecar entry.
- Plot from a sidecar id: `SceneContext.GameModel.GetLandPlotModel(id).gameObj` (the LandPlotLocation) -> `GetComponentInChildren<LandPlot>`.
- `LandPlot._trackers` (TrackContainedIdentifiables).

## Plot purchase menu (LandPlotUIRoot)

- `LandPlotUIRoot : ListUIRoot<PlotPurchaseItemModel, PlotPurchaseCategory, ...>`; `menuConfig` (a `MenuConfiguration<,>` SO on the plot's UI prefab) -> `categories` -> `StaticCategory.items` List<PlotPurchaseItemModel>. The list is **shared by all plots of a type**: per-plot changes must restore it.
- `UpdateMenuItemAvailabilityAndHidden` override 0x6E8150 (per item `UpdateAvailability(Activator)` / `UpdateHidden(Activator)`); not re-run on `RebuildUI` with the menu open (re/ui.md). custom_upgrades hooks both.
- `PlotUpgradePurchaseItemModel`: `_upgrade`, cost list (`_amount` per entry), `_icon`, `_unavailableIcon` (every vanilla item: the shared `checkmark` sprite), `_title`, `_description`, `_promptMessage`, `_infoButtonLabel` (LocalizedStrings). `IsAvailable = () => !plot.HasUpgrade(_upgrade)` (goes through the HasUpgrade hook). `UpdateHidden` only gives STORAGE3/4 and the incinerator collector a prerequisite.
- Cloning items: `Object.Instantiate(<vanilla PlotUpgradePurchaseItemModel>)` deep-copies LocalizedStrings and the cost list (check the clone owns its instances). Template: the silo's STORAGE_CAPACITY_INCREASE item.
- Purchase flow: `OnAction` -> `PromptForUpgrade` (error if owned) -> popup (`_title` / `_promptMessage`) -> `<PromptForUpgrade>b__0` -> `Upgrade(upgrade, cost)`: HasUpgrade, `Spend(cost)` (fails if unaffordable), `AddUpgrade`, purchase cue, `RebuildUI`, `PlayPurchaseFX`. Analytics `PlotUpgraded` gets `upgrade.ToString()`.

## Plot prefabs (data: `LookupDirector.GetPlotPrefab(Id)`, all upgrade parts as inactive children)

| Piece | Where | Note |
|---|---|---|
| Collector button | CORRAL `PlortCollectorUpgrader.Collector` -> child "techActivator" (Animator, stand, button mesh, "triggerActivate" with `PlortCollectorActivator` + `TechUIInteractable`) | `PlortCollectorActivator.Collector` points into the prefab: null it on clones; hooked `Activate` |
| Air nets | CORRAL `patchCorral` `AirNetUpgrader.AirNets` = [Base Air Net, High Air Net]; each one `NetTrigger` (MeshRenderer + `AirNet` + BoxCollider 9.5 x 0.6 x 9.5, layer 29 = barrier layer of corral and coop walls); base net bottom y 3.39 | `AirNet`: HitForceToDestroy 100, HoursToStartRecovery 0.05, HoursToRecover 0.1 |
| Solar shield | CORRAL `SolarShieldUpgrader.Shields` (GameObject[]); a `CaveTrigger` box (10 x 5 / 10 x 8 x 10 m) | blocks daylight rules (re/slimes.md Phosphor) |
| Music box | CORRAL `MusicBoxUpgrader.MusicBox` -> `Music Box/techCorralMusicBox/MusicBox` (SECTR_AudioSource), `MusicBoxRegion` | re/audio.md |
| Feeder | CORRAL feeder child, `FeederSpeedSelector` (3 states) | |
| Coop | COOP `patchCoop`: barriers layer 29, top y 2.0 (High Walls 2.8); `Coop Region Trigger` (CoopRegion + TrackContainedIdentifiables) 9.5 x 2 x 9.5 at y 1 (High Walls 9.5 x 3.5 x 9.5) | same 9.5 footprint as the corral |
| Coop corners (plot-local) | plot menu `techActivator` (-5.54, -5.54); `Vitamizer/objCoopVitamizer` (-4.75, 4.75); `Deluxe/ElderCollector/Collector Unit/CollectionPoint` (+x,+z) | (+x,-z) is free |
| Deluxe coop | COOP `Deluxe` child = `DeluxeCoopUpgrader.DeluxeStuff`, **localRotation (0,-0.707,0,0.707) = -90 deg about Y** (children laid out in a (+x,-z) frame end up in (+x,+z)) | conservatory coops are turned 180 deg |
| Silo | `patchSilo`: `SiloStorage.AmmoSlotDefinitions` = 12 x `SiloNonSlimes` (`_maxAmount` 100, block list = slimes); plot menu terminal (-5.54,-5.54); slot panels mid-side at 3.05 m; `SiloStorageActivator` slot buttons | |
| Output spout | `SiloCatcher` `Type = SILO_OUTPUT_ONLY` (eject 0.25 s, dist 1.2); input `SILO_INPUT_ONLY`; `ScorePlort` depositor (`_depositorPortAnimator`) | |

Cloning: `Object.Instantiate(childOfPrefab, plotTransform)` copies the sub-hierarchy. Clones are never saved: rebuild from the sidecar.

## Elder collector / PlortCollector / SiloStorage (code + data)

- Deluxe coop elder collector = ONE `ElderCollector` object in `Deluxe`: `SiloStorage` (AmmoSetReference `CorralPlortCollectorAmmo`, 2 slots of `AmmoSlotDefinition "ElderCollector"`: `_slotTypeGroup` ElderGroup, `_maxAmount` 100), `PlortCollector` (CollectionArea = whole coop, CollectPeriod 6 game hours), 2 `SiloCatcher` SILO_OUTPUT_ONLY ports (slot 0/1), 2 `SiloSlotUI`, a `PlortCollectorActivator`.
- `PlortCollector.DoCollection` 0x98B670: no count limit: every actor in the area that `storage.CouldAddToAnySlot` is sprung (joint) and stored. `PlortCollector.FixedUpdate`: once a joint exists it reels in (maxDistance -5 m/s), stores via `_storage.MaybeAddToAnySlot`, `CollectFX` puff, `DestroyActor`, cyclone Animator bool, vac audio. Its only `_model` use is `HasReached(_model.collectorNextTime)` before DoCollection; null `_model`/`_region`/`_storage`/`_vacAudio` NREs every physics frame. `JointReference` is a private nested class (`il2cpp_class_get_nested_types`); `JointReference.Destroy` releases a joint. `StartCollection` = DoCollection of everything.
- `SiloStorage.Awake` -> `InitAmmo` 0x9956C0 builds a local `AmmoSlotManager`; only `SetModel(LandPlotModel)` 0x995920 links it to `LandPlotModel.siloAmmo` via `GetAmmoSet`. `LocalAmmo` -> AmmoSlotManager (re/vacpack.md).
- `RanchMetadata.Register(LandPlot)` 0x98D7D0 fills a coop's `LandPlotMetadata.elderCollectors` from `DeluxeStuff` only; `GetStorageMetadata` 0x98D290 = `StorageMetadata(i, storage, First(catchers, c => c.SlotIdx == i))` (re/drones.md).

## Silo capacity (code)

`StorageUpgrader` (plot root): STORAGE2/3/4 toggle `StorageAdd2..4` / `StorageOnly1/2/3And4`; STORAGE_CAPACITY_INCREASE (item "Storage Capacity Upgrade", 1800) = `StorageSilo.Ammo.UpdateMaxCount(StorageCapacityUpgradeAmount = 200)` + `CapacityUpgrade1..4` active (`Apply` 0x996E30). Max count is not saved (re/vacpack.md).

## Coops and hens

- `CoopRegion.IsWithin(pos)` (static `_allCoops`); hen `Reproduce`: `ShouldReproduce` -> `NearMateAndSparseEnough` counts `DensityIds` (chicks and elders included) within `DensityDist` (10 m) vs `MaxDensity()` (`MaxDensityInsideCoops` 58, x`DeluxeDensityFactor` in a deluxe coop; `MaxDensityOutsideCoops` otherwise). 4..13 game hours per chick; chicks grow in 14 game hours (9 hen : 1 rooster).

## Other plot components

- `Incinerate.GetAshSpace()` (the incinerator only takes items while the ash trough has room); `SlimeFeeder`; `SpawnResource._landPlot` (gardens); `TrackContainedIdentifiables.AirNets` (corral slime integrity check).

## From the custom_upgrades notes

### LandPlotUIRoot (plot purchase menu)  : ListUIRoot`4
- methods: `PromptForPlot()` 0x6E77D0 - buying a plot on an empty plot; calls `UITemplates.CreatePopupPrompt` directly, NOT `OpenPopupPrompt` (IDA)
- methods: `PromptForUpgrade` -> `ListUIRoot.OpenPopupPrompt` (same for Space / A / the Purchase button); `<PromptForUpgrade>b__0` is the only caller of `LandPlotUIRoot.Upgrade` (IDA)
- chains: purchase popup confirmed in the same click as a Sell press -> phantom purchase 2-12 ms after the sale (log); PromptForUpgrade always opens the popup
- gotchas: `RebuildUI` (menu already open) does NOT call `UpdateMenuItemAvailabilityAndHidden`, so `PlotUpgradePurchaseItemModel.UpdateAvailability(plot)` is not re-run after a purchase / sale with the menu open
- mod: module.cpp (`RefreshAvailability` at the end of `BeforeMenuUpdate`); sell_upgrades detours PromptForPlot + CreatePopupPrompt (button order), OpenPopupPrompt (shared generic body, LandPlotUIRoot only)

### PlotUpgradePurchaseItemModel
- fields: `_upgrade` +0x88 (in re); `IsAvailable` Func `() => !plot.HasUpgrade(_upgrade)` set ONLY by `UpdateAvailability(plot)`; a fresh `Object.Instantiate` clone's ctor Func says "available" (or keeps the Func of the last plot it was bound for)
- fields: `_unavailableIcon` (checkmark sprite), `_infoButtonLabel` (LocalizedString), `_title` (pinned items can get text_override)
- gotchas: card greyed icon + checkmark + Purchase button all come from IsAvailable
- mod: module.cpp (RefreshAvailability calls UpdateAvailability / UpdateHidden for every list item before the original rebinds); sell_upgrades matches details to items by `_infoButtonLabel`

### LandPlot / LandPlotModel (silo, coop hooks)
- chains: `LandPlot.AddUpgrade` -> `ApplyUpgrades` -> `AshTroughUpgrader.Apply` 0x983930 (`SetActive(true)` of `patchIncinerator/AshTrough` for ASH_TROUGH; also on load)
- chains: plot lookup by id: `SceneContext.GameModel.GetLandPlotModel(id)` (TryGetValue, null if unknown) -> `LandPlotModel.gameObj` (the LandPlotLocation) -> `GetComponentInChildren<LandPlot>`; a demolished / rebuilt plot is a new object
- mod: silo_world.cpp, plot_props.cpp, module.cpp silo capacity tick

### Coop prefab "patchCoop" (asset data, plot-local = prefab root)
- `Deluxe` container (`DeluxeCoopUpgrader.DeluxeStuff`, inactive) is NOT identity: localRotation (0, -0.707, 0, 0.707) = -90 deg about Y; its children are laid out in a (+x, -z) frame, so the vanilla elder collector (`Deluxe/ElderCollector/Collector Unit/CollectionPoint`, Deluxe-local (3.75, 0.5, -3.75)) stands in the plot's (+x, +z) corner
- corners: plot menu terminal `techActivator` (-5.54, -5.54) = (-x, -z); vitamizer `Vitamizer/objCoopVitamizer` (-4.75, 4.75) = (-x, +z); elder collector (+x, +z); (+x, -z) free
- ElderCollector renderers: `plortCollector`, `_funnel`, `_emmit` (vac tower), 2 x techDepositor (frame, inputOutput, Body), 2 x techDisplay1x1; `FX Vac Dust` / `FX Vac Rings` particle systems (inactive between bursts; make the "built" bounds huge); sibling `coopDeluxe_tv`
- `Deco/nestPlain01` = the vanilla nest in the middle (mesh `nestChicken01`, 482 vertices: bowl, floor disc, straw planks; + leaves decal; no eggs); nest floor ~0.09, rim ~0.40
- `Deco/decoFieldsGrass01 _Green` / `Deco/decoFieldsGrass02_Green` = grass tufts (mesh + LODGroup + 2 game scripts, no collider)
- conservatory coops are turned 180 deg (world vs plot-local axes)
- mod: coop_world.cpp (`PickCorner` reads the corners from the live plot's children by path + InverseTransformPoint, prefab layout as fallback), plot_props.cpp (CoopLayout keeps out of corner squares |x|,|z| > 2.5)

### PlortCollector  (elder / corral collector)
- methods: `DoCollection()` 0x98B670 - per taken actor: a "CollectJoint" (kinematic Rigidbody under CollectPt, SpringJoint spring = CollectSpringiness, maxDistance = distance, no auto anchor, connectedAnchor 0), `SafeJointReference.AttachSafely(actor, joint, true)`, connectedBody.WakeUp, `Vacuumable.Capture(joint)`, a JointReference into `_joints`; then `_forceCollectUntil` = HoursFromNow(1/12), `_endCollectAt` = HoursFromNow(1/6)
- methods: `FixedUpdate()` 0x98BE90 - Region hibernated -> nothing; joints and `_endCollectAt` reached -> every `JointReference.Destroy` (joint destroyed, Vacuumable.Release); no joints -> DoCollection once `HasReached(_model.collectorNextTime)` (the ONLY `_model` use); per joint: storage CouldAddToAnySlot else released; farther than 1 m -> maxDistance -= 5*dt; else `storage.MaybeAddToAnySlot` + `SpawnAndPlayFX(CollectFX)` + `Destroyer.DestroyActor`; cyclone Animator bool = joints || !HasReached(_forceCollectUntil); vac audio on/off
- nested: `JointReference` (private nested class; find with `il2cpp_class_get_nested_types`): AmmoMetadata +0x10 (from `Identifiable.get_AmmoMetadata`), Vacuumable +0x30, Joint +0x38
- gotchas: null `_model`, `_region`, `_storage` or `_vacAudio` -> NRE every physics frame; `Identifiable.get_AmmoMetadata` is virtual: look it up on the actor's own class (runtime_invoke does not dispatch)
- mod: coop_world.cpp (Hen Collector clone runs with a private LandPlotModel, collectorNextTime = 1e300; the tick builds the joints for chosen chickens; joints released before removing the clone)

### PlortCollectorActivator / corral collector button
- methods: `Activate()` 0x52DD60 - returns if `Time.timeAsDouble < _nextAllowed`; `Collector.StartCollection()`; `_buttonAnimator.SetTrigger(_buttonPressedTriggerId)`; `SECTR_AudioSystem.Play(PressButtonCue, pos, false)`; `_nextAllowed = now + 0.4`
- methods: `Awake` - `_buttonAnimator = GetComponentInParent<Animator>()` (the techActivator object), `_buttonPressedTriggerId = Animator.StringToHash("ButtonPressed")`
- prefab: CORRAL `PlortCollectorUpgrader.Collector` = inactive "PlortCollector" part (rotated -90 deg); child "techActivator" at (5.58, 1.20, -5.58) / 135 deg in that part's space = the plot's outer (+x, +z) corner facing out: Animator (trigger "ButtonPressed"), SphereCollider, ApplyPalette; children "triggerActivate" (PlortCollectorActivator + TechUIInteractable), "button_stand" (local y -1.16), "button_base", "button_button"
- gotchas: techActivator is active in the prefab, so Instantiate runs Awake at once
- mod: module.cpp `PlortCollectorActivator.Activate` detour (null Collector -> never the original; silo Sell-All button and Hen Collector Activate button -> `CoopWorld::Activate`), silo_world.cpp, coop_world.cpp (`SiloButtons::Press`)

### Silo prefab "patchSilo"
- `Silo Upgrade N/Addition N/Capacity Addon N/techSiloCapacity` = vanilla Storage Capacity tanks, one per storage section, high on the diagonals (section 0 about (-1.6, 3.3, -1.8), next ones turned 90 deg and ~0.8 m higher); each Addition is section 0's layout turned 90 deg (same low pipes on every diagonal)
- tank colliders: `collider` child MeshCollider + foot plate MeshCollider, layer Default; tank has ApplyPalette
- clearances: silo wall r 2.1; pipe elbows / caps r <= 3.05; slot panels / depositors mid-side (Addition 0..3 at 3.05 m); corner posts 4.75; plot menu terminal (-5.54, -5.54)
- Additional Storage: three vanilla cards `Storage2 / 3 / 4 Upgrade` (STORAGE2 / 3 / 4 = upgrade values 3 / 4 / 5), same title and icon; STORAGE3 / 4 hidden until the one before is owned
- mod: plot_props.cpp (tanks at 0.8 size, (+-2.9, +-2.9), yaw 19.6 + k*90), module.cpp `MergeStorageCards`, silo_world.cpp

### Corral prefab "patchCorral"
- `Music Box/techCorralMusicBox/MusicBox/Music` = Music Box speaker (Animator, a game script, capsule collider; children bone_musicBase -> bone_musicEmmit -> "FX Music" particles + "musicbox (1)" mesh, skinned "musicbox"); container turned 180 deg, inner offsets cancel -> (-x, +z) corner post, head ~0.8 m inwards at 2.5 m
- `Music Box/techCorralMusicBox/MusicBox` carries a `SECTR_AudioSource` (concrete subclass; see re/audio.md)
- corners: terminal (-x, -z) outside the post at ground height, speaker (-x, +z), plort collector (+x, +z), auto-feeder distributor (+x, -z) at 2.5 m
- the ground outside the (+x, -z) post is free (the feeder hangs up on the post), and so is the ground outside the (-x, +z) post (the speaker is 2.5 m up and leans inwards): the mod's blue Corral Radio button stands at (-5.58, 1.20, 5.58) turned 315 deg since round 12 (round 11: (5.58, 1.20, -5.58), 135 deg, below the feeder; silo_world.cpp `RadioButtonLook`)
- Solar Shield: children "Base SolarShield" / "High SolarShield" (`SolarShieldUpgrader.Shields`, inactive until bought), each with a "Cave Trigger" child: BoxCollider layer 22, 10 x 5 x 10 / 10 x 8 x 10 m, `CaveTrigger` component
- Air Net: Base net y 3.39..3.99; coop high-walls CoopRegion top 2.75
- mod: plot_props.cpp (Music Box II speaker), shadow_catcher.cpp (shade test), dj_buildables.cpp (standby tune), coop_world.cpp (Air Net)

### Garden prefab "patchGarden"
- `Sprinkler/SprinklerBase` = one of the two Sprinkler heads (Animator disabled, sprinkler + sprinklerpart meshes, 3 spray particle systems; loop sound is on "Sprinkler"); vanilla heads mid -z / +z sides (Sprinkler at y 0.11 turned 90 deg, heads at (+-4.38, -0.44, 0))
- corners: terminal, deluxe planters (+x, -z) / (-x, +z), scareslime (+x, +z); deposit port at (-5.44, -3.14)
- mod: plot_props.cpp

### Incinerator prefab "patchIncinerator"
- `objIncinerator/Meshes/meshIncinerator/FireTest` (fire PS at (0, 3.55, 0), scale (0.75, 1.5, 0.75)) has two INACTIVE children vanilla never shows: "Glow" (scale 0.67), "Embers" (at -0.96, turned -90 deg about x = emits up, scale 1.4 / 1.2 / 1.2)
- `AshTrough` (floor, four low walls) child `Ash Level` = FillableAshSource; its transform moves between MinYPos and MaxYPos with the ash; prefab y 0.75 = full
- `Incinerate.GetAshSpace()` > 0 required for the incinerator to take items (drone sink)
- mod: plot_props.cpp (Ash Booster props, `sr2ash` embers follow Ash Level, FollowAsh); sell_upgrades (Ash Trough fill: FillableAshSource is on a child -> GetComponentInChildren)

### CoopRegion
- `_allCoops` static list (registry used for the coop tick); region collider 9.5 x 2 x 9.5 base, top 2.0; high walls top 2.75 / 2.8
- mod: coop_world.cpp (adults counted with `Physics.OverlapBox` over the active region; rooster = type name has "Rooster")

## From the slime_dj, spawn_scaling, hl2_movement, quick_start, free_placement, misc_tweaks, plot_upgrades, drone_price_limit notes

### MusicBoxUpgrader (global)  [: PlotUpgrader]
- methods: `Apply(Upgrade)` 0x98A810 - own body, 0x2A bytes; `if (upgrade == MUSIC_BOX) MusicBox.SetActive(true)` (IDA)
- fields: `MusicBox` (GameObject the vanilla Music Box upgrade switches on) (dump)
- chains: `LandPlot.ApplyUpgrades` 0x9887E0 -> `Apply` on every PlotUpgrader of the plot for every owned upgrade; runs on plot load and on every purchase (IDA). So every corral with a box passes `Apply` when its plot appears or the box is bought.
- gotchas: sits on the plot root (`patchCorral`) next to LandPlot, WallUpgrader, AirNetUpgrader; a box "counts" while `MusicBox` is activeInHierarchy (owned + plot loaded); selling the box switches MusicBox off. Music Box II (custom_upgrades) keeps the same GameObject. `Object.FindObjectsByType(MusicBoxUpgrader, ...)` walks every loaded object: 8-14 ms on the ranch, ~2.5 ms off it (log, perf) - don't call it periodically
- mod: slime_dj `module.cpp` hook `slime_dj.MusicBoxUpgrader.Apply` (post: records `this`; nothing else hooks it)

### LandPlot (global)
- methods: `ApplyUpgrades` 0x9887E0 (IDA, see above)
- mod: slime_dj reaches it from a component with `Component.GetComponentInParent(Type, bool)`; plot key = `LandPlotLocation._id` (via custom_upgrades::PlotKey)

### WallUpgrader (global)
- fields: `StandardWalls` (GameObject "Base Corral"), `UpgradeWalls` ("High Wall Corral"); one of them active (bundle, dump)
- mod: slime_dj (High Walls = UpgradeWalls active; muffle + walls dance)

### AirNetUpgrader (global)
- fields: `AirNets` = [Base Air Net (under Base Corral), High Air Net (under High Wall Corral)]; the active one = the owned net for the current walls (bundle)
- mod: slime_dj

### Corral prefab `patchCorral` (bundle)
- plot root components: LandPlot, WallUpgrader, MusicBoxUpgrader, AirNetUpgrader, ... (same GameObject)
- walls: `<walls root>/Frame/techCorralPostNN` (4, 90 deg apart) `/empty_postBase` (corner) `/corralField`: one MeshRenderer, L-shaped energy field covering half of the two sides at that corner (mesh bounds 4.9 x 3.2 x 4.9, y 0.17-3.39). High Walls add `corralField (1)` on top (y 3.38). So 4 or 8 field renderers. Corral half-size ~4.9 m.
- fields have no collider; the barriers are separate "corralBarrier" boxes (layer 29)
- material "CorralBarrier": shader `SR/RanchTech/CorralBarrier` (HDRP transparent/unlit shader graph, tessellated); `_Intensity` 3, `_Color` white, `_EmissionColor`, `_HitTime`, `_Health`, `_RipplePushScale` 0.5, `_RippleStrength` 1, `_RippleFadeDuration` 1, `_RippleSize` 5, `_RippleStartSize` 1
- net: "NetTrigger" = MeshRenderer + `AirNet` + BoxCollider on one object (never move/scale it). Material "AirNet" (same shader family): `_Intensity` 1; ripple props 1 / 5 / 5 / 5 / 0 (PushScale / Strength / Fade / Size / StartSize)
- shader domain stage (DXBC disassembled from `shaders_assets_*.bundle`): loops over 4 ripples of global `_WorldRippleArray` (x, y, z, start time w): age = `_Time.y - w`, skipped once age >= `_RippleFadeDuration`; t = age / fade; radius = lerp(`_RippleStartSize`, `_RippleSize`, t); vertices within radius pushed away from the centre by `_RipplePushScale x (1 - d / radius) x (1 - t^4)` (object space). Ripple props are per-material floats (a MaterialPropertyBlock can set them per renderer); the ring is global.
- mod: slime_dj `walls.cpp` (walls dance)

### AirNet (global)  [: MonoBehaviour]
- methods: `Awake`/`Update` set `_Health` on its own material instance (0x982840 loads `StringLiteral__Health`); `OnCollisionEnter` sets `_HitTime` (IDA)
- gotchas: don't touch those two props or hit / broken net feedback is lost
- mod: slime_dj (only `_Intensity` / `_Color` via property block)

### CorralBarrierRipples (MonomiPark.SlimeRancher.VFX.EnvironmentInteraction)  [: MonoBehaviour]
- fields: `worldRippleArray` Vector4[4] +0x20, `nextRipple` +0x28, id of `_WorldRippleArray` +0x2C (IDA)
- methods: `ResetRipples` (Awake, OnDestroy; fills w with -10000); `SpawnRippleHere(Vector3)` 0xB65EB0 - advances the ring, writes (x, y, z, `Time.time`), `Shader.SetGlobalVectorArray` (IDA)
- chains: `SceneContext.FXDirector` +0xD0 -> `FXDirector.corralBarrierRipples` +0x40
- mod: slime_dj walls.cpp (one ripple per corral per beat; found with `FindObjectOfType`, GC handle)

### CorralBarrierRippleCollision
- methods: `OnTriggerEnter` 0xB65A40 - same as SpawnRippleHere inline, at `Collider.ClosestPointOnBounds` of the barrier, for layer-9 objects with a `VacDelaunchTrigger` (vacpack shots) (IDA)

### Per-plot upgrade internals (plot_upgrades, disasm + dump)
- PlotUpgrader components (MusicBoxUpgrader, AshTroughUpgrader, ...) only switch GameObjects on/off; the numbers live in the activated components
- Coop: `Reproduce.ReproducePeriod()` = `Randoms.GetInRange(MinReproduceGameHours, MaxReproduceGameHours)`, used by `InitModel` / `RegistryUpdate` to set `model.nextReproduceTime`. `WithinCoop()` = `CoopRegion.IsWithin(pos)`: any `CoopRegion` (static `_allCoops`) whose `Collider.bounds` contains the position. mod: plot_upgrades detour `Reproduce.ReproducePeriod` (result / m)
- Garden: `SpawnResource.UpdateToTime(worldTime, deltaWorldTime, timeOfDay)`: within spawning hours and without spawn blockers, `model.nextSpawnTime -= (IsWatered ? def._wateredTimeFactor : 0) * deltaWorldTime` (the sprinkler mechanism); crops spawn once worldTime reaches nextSpawnTime. `SpawnResource._landPlot` gives the plot. mod: plot_upgrades detour (pre: advances nextSpawnTime by (m-1)(1+watered factor)dt)
- Incinerator: `Incinerate.ProcessIncinerateResults(id, amount[, pos, rot])` (2 overloads): for food `AshTrough.AddAsh(amount * AshPerIncineration)`; `FillableAshSource.AddAsh` clamps `plotModel.ashUnits` to `MaxUnits`. mod: plot_upgrades detours both overloads (AshPerIncineration x m during the call)
- Music box: see re/slimes.md (SlimeEmotions / MusicBoxRegion)

## From the sandbox, sell_upgrades, collectibles, instant_build notes

### LandPlot / LandPlotModel  [su]
- fields: `LandPlotModel.upgrades` HashSet<LandPlot.Upgrade> (int enum, saved as list by Push/Pull)
- methods: `LandPlot.HasUpgrade(u)` -> `model.upgrades.Contains(u)`; `AddUpgrade(u)` 0x988750 = Add + `ApplyUpgrades({u}, true)`
- methods: `LandPlot.ApplyUpgrades(set, isInitialPurchase)` 0x9887E0 - every PlotUpgrader on the LandPlot GameObject: `Apply(u)` (+ `OnInitialPurchase(u)` when buying); then `_trackers = GetComponentsInChildren<TrackContainedIdentifiables>(false)`, `RanchMetadata.Deregister/Register(this)`
- gotchas: on load the plot prefab is instantiated fresh and the model set applied: plot always matches its saved set. Demolish/replace (`LandPlotLocation.Replace` -> `GameModel.ReplaceLandPlot`) builds a NEW model
- mod: su sale (model.upgrades.Remove + inverted upgrader + ApplyUpgrades(empty,false))

### PlotUpgraders  [su]
- WallUpgrader WALLS: StandardWalls off / UpgradeWalls on; MusicBoxUpgrader MUSIC_BOX: MusicBox on (`MusicBoxRegion.OnDisable` releases slimes); MineralSoil / Sprinkler / Scareslime / Feeder / Vitamizer / AshTrough: one GameObject; AirNetUpgrader / SolarShieldUpgrader: GameObject[]; PlortCollectorUpgrader PLORT_COLLECTOR / _POND / _INCINERATOR: Collector on; MiracleMixUpgrader: MiracleMix on, NormSoil off; DeluxeCoopUpgrader: DeluxeStuff on, `CoopRegions[i]._isDeluxe = true`; DeluxeGardenUpgrader: DeluxeStuff on, planted crop destroyed and `GardenCatcher.Plant(cropId, true)` (DeluxePlantedPrefab because model has DELUXE_GARDEN), `new.InitAsReplacement(old)` keeps growth
- StorageUpgrader STORAGE2/3/4: order-dependent slot GameObject swaps; STORAGE_CAPACITY_INCREASE: `AmmoSlotManager.UpdateMaxCount(+amount)` on the silo
- OnInitialPurchase: FeederUpgrader resets `SlimeFeeder model.nextFeedingTime`; AshTroughUpgrader fills the trough (`model.ashUnits = FillableAshSource.StartingUnits`)
- gotchas: `FillableAshSource` sits on the trough child "Ash Level" (GetComponentInChildren; GetComponent on AshTrough fails); `FillableAshSource.AddAsh(0)` clamps + moves the ash mesh; `AshTroughUpgrader.AshTrough` = trough model

### LandPlotUIRoot (MonomiPark.SlimeRancher.UI.Plot; plot purchase menu, a ListUIRoot)  [su, sb]
- items: `PlotUpgradePurchaseItemModel` (ScriptableObject): `_upgrade` +0x88, price `MenuPurchaseItemModel._purchaseCost`, `_title`, `_promptMessage`, `_infoButtonLabel`. `IsAvailable = !plot.HasUpgrade(_upgrade)`; owned stay listed greyed. Hidden: STORAGE3 needs STORAGE2, STORAGE4 needs STORAGE3, PLORT_COLLECTOR_INCINERATOR needs ASH_TROUGH
- methods: `OnAction(item)` 0x6E6F50 - runs for owned items too (ListUI sets submit with no availability check); vanilla returns silently when !IsAvailable
- chains: OnAction -> `PromptForUpgrade` -> `ListUIRoot.OpenPopupPrompt(title, message, warning, pos, neg, Action pos, Action neg)` with closure `LandPlotUIRoot.<>c__DisplayClass6_0 {<>4__this, upgrade, cost, item}`; `<PromptForUpgrade>b__0` 0x702E70 -> `LandPlotUIRoot.Upgrade(upgrade, cost)`: HasUpgrade abort / `ISpendableSpender.Spend` / AddUpgrade / cue / `RebuildUI()` / `swapper.PlayPurchaseFX()`
- methods: `PromptForPlot` 0x6E77D0 -> `UITemplates.CreatePopupPrompt(body, misc, pos, neg, expiration)` 0x6B1910 (plot buying, not OpenPopupPrompt)
- methods: `SetActivator` (su remembers the open menu)
- gotchas: menu reads currency amounts when building its list; rebuilds only after its own purchases; `ListUIRoot.RebuildUI` 0x11AFB30 -> `IListViewSwapper.RefreshAndAdvance` (sb calls the most derived RebuildUI after Newbucks changes)
- mod: su detours OnAction, <PromptForUpgrade>b__0, SetActivator, PromptForPlot, CreatePopupPrompt; sb RefreshPurchaseMenus
