# Slimes: definitions, diet, eating, emotions, hunger, special rules

`MP.SR` = `MonomiPark.SlimeRancher` (slime code mostly in `MonomiPark.SlimeRancher.Slime`). Tags: **code** = decompiled; **data** = asset bundles (`identifiabletypes_assets_*`, `identifiabletypegroups_assets_*`, `corescenes_scenes_*`); **inferred** = not checked. Mod use: `modules/custom_upgrades/shadow_catcher.cpp` (Shadow Catcher), `modules/plot_upgrades/` (music box, coops), `modules/sandbox/` (spawner, diets). Hooks: re/hooks.md.

## Eating (code)

- `SlimeEat.WillNowEat(id)` 0xBB8750 = `DoesEat(id)` (`_allEats`, built from `Diet.EatMap` in `CalculateAllEats` = `InitFood`) && (feral || `_keepEatingWhenFull` || `IsHungryFor(id)`).
- `SlimeEat.IsHungryFor` 0xBB7380: `!SlimeModel.isSleeping` && `EmotionDriveCalculator.Drive` 0xAB3D70 (`max(emotion, MinDrive) + ExtraDrive`, clamped >= 0) >= `MinDriveToEat` (Pink and Shadow 0.333).
- `SlimeEat.FinishChomp` 0xBB6780: eats with `skipProduction = _keepEatingWhenFull && !IsHungryFor(id)` (the Ringtail rule: no plort when full).
- `SlimeEat.InvokeOnEat` 0xBB7190: hunger -= `DrivePerEat`, agitation -= `AgitationPer(Fav)Eat`.
- `CanEat` / `WantsToEat` / `IsHungryFor` refuse while `SlimeModel.isSleeping` (saved; set by `SlimeEmotions.SetAsleep` from daze / ignite / vortex / explode).
- `GotoConsumable.Relevancy` uses `KeepEatingWhenFullDrive` (0.9-1) for a slime that keeps eating when full.
- `SlimeDefinitions.RefreshDefinitions` rebuilds every `EatMap` (new lists). Diet fields on `SlimeDefinition.Diet`: `MajorFoodIdentifiableTypeGroups`, `FavoriteIdents`, `AdditionalFoodIdents`, `ProduceIdents`, `FavoriteProductionCount`, `EatMap` (entries with `MinDrive` / `ExtraDrive`).

## Emotions and hunger (code)

- `SlimeEmotions.SetModel` 0x88B980 subscribes `_hungerRecoveryRate` to `SlimeEmotionConfig.HungerRate` of the actor's property tracker (`defaultPropertyValues` overrides per definition).
- `ComputeHungerChangePerHour` 0x88AE10 = rate x `_hungerRecoveryFactor` (+ day/night extra) x `_hungerDirection` (Awake: `_hungerConfig.RecoversTowardOne ? 1 : -1`).
- `UpdateToTime` 0x88C040 integrates `_changeRate` per game hour into `_emotions` and copies them to `SlimeModel.Emotions` (saved). `FastForwardToTime(double worldTime, int sloomberSlimesCount)` 0x88B050.
- `SlimeFaceAnimator._isMischievous`, `_isFleeing`.

## Ringtail (code)

`KeepEatingWhenFull` component: Awake sets `SlimeEat._keepEatingWhenFull`; UpdateInstance sets `SlimeFaceAnimator._isMischievous = !IsHungryForNonPlorts` when `_showMischievousFace`. Stone form: `TurnToStoneInDaylight.BecomeStatue` 0xBDCB60 (sets `SlimeModel.isStatue`, instantiates the `RingtailStatue` actor, destroys the slime).

## Phosphor (daylight rule)

`DestroyOutsideHoursOfDay`: a live window + endure hours, stopped by any `CaveTrigger` (the corral Solar Shield is a `CaveTrigger` box). Shadow Catcher reuses this rule for kept shadow slimes.

## Feral

`SlimeFeral.UpdateInstance` 0xBBD5D0 never poofs a slime on the ranch (`CellDirector.IsOnRanch`) or in the wilds: it pushes `_expireAt` an hour on. Prefab field `FeralLifetimeHours` 3 + `DestroyFX`.

## Shadow slime

- `SlimeDefinition` "Shadow" (data): pedia suffix `shadow_slime`, plort "ShadowPlort", appearance "ShadowDefault"; built like Gold (not a largo, `CanLargofy` 0, no largo definition pairs it), native to four zones (Grey Labyrinth). `defaultPropertyValues`: `EmotionSleepinessRecoveryRate` -1000, `EmotionHungerRecoveryRate` 0. Diet groups/favorites/additional all **empty**, produce ShadowPlort, `FavoriteProductionCount` 5: eats nothing, never hungry. Groups: `ShadowAlertableGroup`, `SmallSlimeGroup`, `SlimesSinkInShallowWaterGroup`, `IdentifiableTypesGroup`. Spawned from "shadow pots" (`ShadowPotMinSpawn` 3, `ShadowPotMaxSpawn` 7).
- Prefab "slimeShadow" (data): `SlimeEat`, `ShadowSlimeScatter` (`_scatterConfig`), `ProducePlortsOnHit` (`_despawnAfterPlortCount`, 50 % extra plort, `BonusPlortProperty` = Shadow Sureshot upgrade's `ShadowSureshotBonus`), feral-like component, `Vacuumable` Size 0 (NORMAL), `SlimeHealth` 20, `CanHibernate` 1.
- `ShadowSlimeScatter` (MP.SR.Slime.Shadow; Subbehaviour, IFleeBehavior), code:
  - `ScatterIfPlayerNear` 0x8A2280 (called from `SubbehaviourPlexer.FixedUpdateInstance`): forces scatter within `PlayerReactRadius` 10 m.
  - `Relevancy` 0x8A2120: 1 while the player is within `PlayerReactRadius`; `Selected` 0x8A23F0 -> `TriggerAlert(player)`. `StartFleeing` (from `ProducePlortsOnHit.ProcessCollisionEnter` after a hit) and `DoJump` also `TriggerAlert`; `OnDisruptionLevelChanged` calls `DoAlert`.
  - `DoAlert` 0x8A1890: sets `_latestAlertObject`, alerts shadow slimes within `AlertBroadcastRadius` 3 m, `_destroyTime = now + Random(10, 15)` s.
  - `Action` 0x8A12D0: `DisappearFX` / `DisappearCue`, `Destroyer.DestroyActor(go, "SlimeFlee.Action")` once `fixedTimeAsDouble >= _destroyTime`.
- `ProducePlortsOnHit`: `ProducePlort` 0x885CA0 returns once `_numPlortsSpawned >= PlortCount` (`ShadowSlimeDespawnAfterPlortCountMetadata` PlortCount 10); `LateUpdateAll` 0x8856B0 despawns (`"ProducePlortsOnHit"`); `ProcessCollisionEnter` 0x8859F0 is ProducePlort's only caller. Also on Gold slimes (`PlortIdentifiableType` tells them apart).
- Vac: player slots don't allow Shadow (re/vacpack.md); saving: Shadow actors save natively (re/save.md); scatter/plort counters are runtime-only.

## Chickens

Hen groups and coop breeding: re/world.md (groups), re/plots.md (Reproduce, density).

## Music box / calm

`MusicBoxRegion._currentEmotions` membership (plot_upgrades Music Box II); details in re/audio.md / plot_upgrades.

## From the custom_upgrades notes

### Chicken groups / coop
- rooster = IdentifiableType name contains "Rooster" (mod rule); task config `MeatTypeGroup`, `ElderGroup` (elder hen = member whose name has "Hen")
- mod: coop_world.cpp, drone_source.cpp

### ShadowSlimeScatter (MonomiPark.SlimeRancher.Slime.Shadow, Subbehaviour)
- methods: `Relevancy` 0x8A2120 (1 when player within PlayerReactRadius or `_latestAlertObject` set), `Selected` 0x8A23F0 -> TriggerAlert(player), `ScatterIfPlayerNear` 0x8A2280 (from SubbehaviourPlexer.FixedUpdateInstance), `TriggerAlert` 0x8A2410 (DoAlert self + neighbours within AlertBroadcastRadius), `DoAlert` 0x8A1890 (also OnDisruptionLevelChanged; sets `_latestAlertObject` once, `_destroyTime` = now + Random(TimeBeforeDisappearingMin, Max), jumps, `_sfAnimator._isFleeing`), `Action` 0x8A12D0 (alerted and past `_destroyTime` -> DisappearFX + `Destroyer.DestroyActor("SlimeFlee.Action")`)
- vanilla: alert at 10 m, gone 10-15 s later
- mod: shadow_catcher.cpp (ScatterIfPlayerNear, Relevancy, DoAlert, Action detours)

### ProducePlortsOnHit (Gold / Shadow)
- methods: `ProducePlort` 0x885CA0 (returns at `_numPlortsSpawned >= DespawnAfterPlortCount` 10), `LateUpdateAll` 0x8856B0 (despawns at the count), `ProcessCollisionEnter` 0x8859F0 (ProducePlortGroups object faster than `_speedThresholdToProducePlort` -> ProducePlort, then IFleeBehavior.StartFleeing; only caller of ProducePlort)
- fields: `_regionMember`
- mod: shadow_catcher.cpp ProducePlort detour (skipped for Shadow on the ranch)

### SlimeEat / diet / hunger (Ringtail rule)
- methods: `WillNowEat` 0xBB8750 = DoesEat(id) && (feral || _keepEatingWhenFull || IsHungryFor(id)); `_allEats` from Diet.EatMap in CalculateAllEats (InitFood, SetModel)
- methods: `FinishChomp` 0xBB6780 - skipProduction = _keepEatingWhenFull && !IsHungryFor(food) -> EatAndProduce (then waits TimePerEatWhenFull)
- methods: `IsHungryFor` 0xBB7380 = !isSleeping && drive >= MinDriveToEat; `EmotionDriveCalculator.Drive` 0xAB3D70 = max(hunger, entry.MinDrive) + entry.ExtraDrive; `InvokeOnEat` 0xBB7190 lowers hunger by DrivePerEat
- methods: `SlimeEmotions.ComputeHungerChangePerHour` 0x88AE10 -> `_changeRate[0]` = `_hungerRecoveryRate` x recovery factor (+ day/night) x `_hungerDirection` (Awake: RecoversTowardOne ? 1 : -1)
- fields: `SlimeEat._keepEatingWhenFull`, `DrivePerEat`; `SlimeEmotions._hungerRecoveryRate`, `_hungerDirection`; face `_isMischievous`, `_isFleeing`
- `KeepEatingWhenFull` component (Ringtail prefab) only sets `_keepEatingWhenFull` and (with `_showMischievousFace`) `_isMischievous` while !IsHungryForNonPlorts
- `SlimeEmotionConfig` lives in namespace `MonomiPark.SlimeRancher.Slime` (not global); `HungerRate.DefaultValue` = vanilla rate; Shadow's defaultPropertyValues make it 0
- `GotoConsumable.Relevancy` uses KeepEatingWhenFullDrive (0.9-1) for keep-eating slimes
- `EatMapEntry` ctor: FavoriteProductionCount 2, ProductionCount 1; `SlimeDefinitions.RefreshDefinitions` rebuilds every EatMap (new lists)
- Shadow prefab `MinDriveToEat` 0.333; Shadow diet empty, hunger rate 0
- Ringtail stone form: `TurnToStoneInDaylight.BecomeStatue` swaps in a `RingtailStatue` actor (unrelated to eating)
- mod: shadow_catcher.cpp

### SlimeFeral
- methods: `UpdateInstance` 0xBBD5D0 - at `_expireAt` a slime on the ranch (CellDirector.IsOnRanch) or in the wilds gets another hour; only elsewhere it poofs

### DestroyOutsideHoursOfDay (Phosphor rule)
- fields: StartHour / EndHour (hours they live in), Min/MaxEndureHoursOutsideWindow, CavesPreventShutdown
- methods: `Update` 0xAB3AB0 (kill + destroy at `HasReached(_shutdownAt)`), `StartShutdownClock` 0xAB3960 (next EndHour + Random(endure) game h), `OnCaveEnter` 0xAB3860 (infinity while any CaveTrigger holds the slime)
- read from Phosphor `SlimeDefinition.prefab` (or its SlimeModules); defaults 18:00-06:00
- mod: shadow_catcher.cpp (own clock per kept slime)

### Largos
- 1.3.0 has no Shadow largo: Shadow `CanLargofy` 0; `Actor_en` has `l.<a>_<b>_largo` names, none with shadow

### SlimeAttachPoint (pillow seat)
- methods: `OnTriggerEnter` 0x753480 (largo needs `_allowLargo`), `FixedUpdate` 0x752700 (rotates the joint bodies by `_rotationSpeed` while a slime is attached = the "dance"; keeps `_isJointActive` / `_slime`)
- fields (dump): +0x78 `TarrTypeGroup`, +0x80 `_rotationSpeed`, +0x8E `_allowLargo`, +0x8F `_allowBaseSlimes`, +0x90 `_allowTarr`, +0xB8 `_slime`, +0xC0 `_isJointActive`
- `OnTriggerEnter` rules (**IDA**): the collider's `IdentifiableType` must be a `SlimeDefinition`; largo (`IsLargo`) -> needs `_allowLargo`; a `TarrTypeGroup` member -> needs `_allowTarr` (else rejected); base slime -> needs `_allowBaseSlimes`. Largos and Tarrs use `LargoJointBody`, others `JointBody`. `TarrTypeGroup` must be set (it is read for every non-largo; the Flower Pillow prefab has it).
- gotchas: set `_allowLargo` / `_allowTarr` before the clone's Awake (dj_buildables sets both on the DJ Booth seat, round 12 for Tarrs)
- mod: dj_buildables.cpp

## From the slime_dj, spawn_scaling, hl2_movement, quick_start, free_placement, misc_tweaks, plot_upgrades, drone_price_limit notes

### SlimeEmotions
- methods: `ComputeAgitationChangePerHour()`: factor = `_agitationRecoveryFactor` (+ daylight/darkness extra) + (`_musicBoxCount > 0` ? `MusicBoxRegion.EXTRA_CALMING_FACTOR` (0.5, inlined const) : 0) + `_toyAgitationFactor`; recomputed on many events (disasm)
- methods: `AddMusicBox` / `RemoveMusicBox` (called by MusicBoxRegion)
- gotchas: EXTRA_CALMING_FACTOR is an inlined constant, so plot_upgrades raises `_toyAgitationFactor` by 0.5 (m-1) during the original call instead
- mod: plot_upgrades detour (Music Box II)

### MusicBoxRegion
- methods: `OnTriggerEnter/Exit`: slime's `SlimeEmotions.Add/RemoveMusicBox` and `_currentEmotions` (List<SlimeEmotions>) Add/Remove = the slimes inside that box (disasm)
- mod: plot_upgrades `OnTriggerEnter` pre-detour registers the region (GC handle)

### IdentifiableType (global)
- fields: `IsPlayer` +0x18, `IsAnimal` +0x19 (dump); `localizedName` (LocalizedString) -> `UnityEngine.Localization.LocalizedString.GetLocalizedString()` (argc 0, Unity.Localization) gives the display name
- gotchas: "resource actor" test = class exactly `IdentifiableType` and !IsAnimal && !IsPlayer (SlimeDefinition, GadgetDefinition, ToyDefinition, LiquidDefinition are subclasses). Asset names often end in `Craft` (e.g. `RadiantOreCraft`)
- mod: spawn_scaling (cyclone filter, per-resource labels)

## From the sandbox, sell_upgrades, collectibles, instant_build notes

### SlimeDefinition  [: IdentifiableType]
- methods: `get_Icon` 0xBAF310 - returns `GetAppearanceForSet(...)._icon` (default appearance icon) (IDA)
- fields: `Diet` (`SlimeDiet`: `FavoriteIdents`, `ProduceIdents`, `EatMap`, `MajorFoodIdentifiableTypeGroups`), `BaseSlimes`, `IsLargo`
- gotchas: largos may have empty `icon`; use get_Icon (virtual, resolved on the class), else compose from BaseSlimes icons
- mod: sb item icons, slime diets relations, largo families
