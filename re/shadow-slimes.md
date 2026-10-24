# Shadow slimes: research for "vacuum them, keep them on the ranch, feed them" (SR2 1.3.0)

User idea: a refinery / Fabricator vacuum upgrade that lets you collect shadow slimes; on your own
ranch they don't disappear and they eat food (maybe a new food source for them).

**Status:** built as the Fabricator upgrade **Shadow Catcher** (`modules/custom_upgrades/shadow_catcher.cpp`,
card in `vacpack_upgrades.cpp`; design and test cases in `modules/custom_upgrades/NOTES.md` "Shadow
Catcher"). The save blocker is resolved: the save stores Shadow slime actors natively (section 6).

Sources: asset bundles read with UnityPy (`identifiabletypes_assets_*.bundle`,
`identifiabletypegroups_assets_*.bundle`, `corescenes_scenes_*.bundle`), the Il2CppInspectorRedux dump
and IDA decompilation of GameAssembly.dll 1.3.0. **[data]** = read from the assets, **[code]** =
decompiled, **[inferred]** = not checked. RVAs for reference only.

## 1. What a shadow slime is

- `SlimeDefinition` asset **"Shadow"** (identifiabletypes bundle), pedia suffix `shadow_slime`, plort
  **"ShadowPlort"**, appearance "ShadowDefault", sounds "Shadow". It is built exactly like the Gold slime
  ("Gold" asset: same flags, same layout) **[data]**: not a largo, `CanLargofy` 0, native to four zones
  (`NativeZones`; Grey Labyrinth areas going by the other Shadow assets - "ContainerLabyShadow01"),
  `defaultPropertyValues`: `EmotionSleepinessRecoveryRate` -1000 and `EmotionHungerRecoveryRate` 0.
- Diet **[data]**: `MajorFoodIdentifiableTypeGroups`, `FavoriteIdents`, `AdditionalFoodIdents` all **empty**;
  `ProduceIdents` = ShadowPlort; `FavoriteProductionCount` 5. So it eats nothing and never gets hungry.
- Spawning: "shadow pots" (`ShadowPotMinSpawn` = 3, `ShadowPotMaxSpawn` = 7, PropertyDefinitions)
  **[data]**; the spawner class itself was not traced.
- Prefab "slimeShadow" (+ module "moduleSlimeShadow") components **[data]**, identified by field names:
  `SlimeEat` (no foods to find), `ShadowSlimeScatter` (`_scatterConfig`), `ProducePlortsOnHit`
  (`_despawnAfterPlortCount`, 50 % chance of an extra plort, `BonusPlortProperty` = the Shadow Sureshot
  upgrade's `ShadowSureshotBonus`), a `SlimeFeral`-like component (`FeralLifetimeHours` 3, `DestroyFX`),
  `Vacuumable` with **`Size` 0 = NORMAL**, `SlimeHealth` 20, the usual emotions / face / attack parts,
  `CanHibernate` 1.
- `VacuumItem` has dedicated fields `ShadowPlortIdentifiableType` and `ShadowSlimePediaEntry` (like the
  gold ones) - the vac unlocks the pedia entry when it grabs one **[code: ConsumeVacuumable]**.

## 2. How they disappear

1. **Scatter** (`MonomiPark.SlimeRancher.Slime.Shadow.ShadowSlimeScatter`, a `Subbehaviour`/`IFleeBehavior`)
   **[code]**: `ScatterIfPlayerNear` (0x8A2280) forces the scatter behaviour when the player is within
   `PlayerReactRadius` (10 m); `DoAlert` (0x8A1890) sets `_latestAlertObject`, alerts other shadow slimes
   within `AlertBroadcastRadius` (3 m) and sets `_destroyTime = now + Random(TimeBeforeDisappearingMin 10,
   Max 15)` seconds **[data]**; `Action` (0x8A12D0) plays `DisappearFX` / `DisappearCue` and calls
   `Destroyer.DestroyActor(gameObject, "SlimeFlee.Action")` once `fixedTimeAsDouble >= _destroyTime`.
   So a shadow slime that has seen the player is gone 10-15 s later.
2. **Plort limit**: `ProducePlortsOnHit` with `ShadowSlimeDespawnAfterPlortCountMetadata` (`PlortCount` 10,
   `DespawnVfx`) **[data]**: it despawns after spawning 10 plorts from hits.
3. Possibly the feral lifetime (`FeralLifetimeHours` 3 + `DestroyFX` on the prefab) **[inferred]**.

## 3. Why the vac can't keep them

- `Vacuumable.Size` is NORMAL, so the vac can pull them like any slime **[data]**.
- Storing needs a slot whose `AmmoSlotDefinition.IsAllowed` (0x866B40: false if in `_slotBlockList`, true if
  `_slotTypeGroup` null, else group membership) accepts the type. The player's regular slots
  (`AmmoSlot1-4`, `ExpansionAmmoSlot 1/2`) use the group **"VaccableNonLiquids"** (block list empty), whose
  member groups include **"VaccableBaseSlimeGroup"**: 23 member types, and **neither Shadow nor Gold is
  among them** **[data]**. So a caught shadow slime cannot go into any slot (the consume coroutine gets no
  slot; the capture fails) **[code for the path up to `StartConsuming`, inferred for the failure branch]**.
- Even before that, the scatter makes it flee at 10 m and vanish 10-15 s after the first alert.

## 4. Feasibility per part

| part | what to hook / change | effort | risk |
|---|---|---|---|
| **(a) Vac upgrade "Shadow Catcher"** | A Fabricator upgrade like `vacpack_upgrades.cpp` (our UpgradeDefinition, level in the sidecar). Effect: a detour on `AmmoSlotDefinition.IsAllowed(IdentifiableType)` that returns true for the Shadow type on the player's regular slot definitions while the upgrade is owned (scoped: silos, drones and feeders keep vanilla rules). Optional: skip `ShadowSlimeScatter.ScatterIfPlayerNear` / `DoAlert` while the player holds the vac with the upgrade, so they don't flee before you reach them. | small-medium (1-2 days) | `IsAllowed` is hot (every vac tick, every slot): needs a pointer compare first. Slimes in a slot keep only type + emotions; shooting one out spawns a fresh shadow slime (vanilla `Expel`), which will scatter and vanish again unless (b) applies. |
| **(b) No despawn on the own ranch** | Detour `ShadowSlimeScatter.Action` (pre): when the slime stands on the ranch, clear `_latestAlertObject` / push `_destroyTime` to infinity (or skip `DoAlert`); and `ProducePlortsOnHit`'s despawn branch (reset `_numPlortsSpawned` or skip the despawn) for ranch slimes. "On the ranch": the slime's `RegionMember` regions / zone vs the Conservatory scene group, or simply inside a corral (`CorralRegion` / `LandPlot` under it). Check the feral-lifetime path too. | medium (2-3 days) | Saving: a shadow slime left in a corral is a normal actor of type Shadow in the save. Whether the save's persistence-id table (`SaveReferenceTranslation.GetPersistenceId`, the dictionary sandbox checks) contains "Shadow" is **not verified** - the sandbox's spawn list logs the types it leaves out ("left out N type(s) the save can't store"). If it is missing, a ranch shadow slime cannot be saved at all and (b) is blocked. A mod-free load keeps them as vanilla shadow slimes (they vanish once alerted) - harmless. |
| **(c) They eat food** | Runtime diet change: add foods to the Shadow `SlimeDefinition.Diet` (`MajorFoodIdentifiableTypeGroups` / `AdditionalFoodIdents`, produce stays ShadowPlort), give it a hunger recovery rate (its `defaultPropertyValues` override sets `EmotionHungerRecoveryRate` 0), then let the diet rebuild its eat map (`SlimeDiet.RefreshEatMap`-style call, not traced) so `SlimeEat` finds the food. Existing food (e.g. a craft resource or a fruit) = data changes only. A **new** food item = a new IdentifiableType: same save problem as the lava tank (persistence ids, vanilla save must never hold it). | medium with an existing food; high with a new one | Economy: shadow plorts become farmable; check their market value. Diet changes apply to every shadow slime (also wild ones) unless the eat check is detoured per slime. |

## 5. Suggested order (historical; see section 6 for what was built)

1. Check in game (sandbox spawn list log) whether "Shadow" is saveable. If not, stop at (a) + a "they
   still vanish" warning, or keep ranch shadow slimes in the sidecar like the Hen Collector's chickens.
2. (a) with the scatter suppression - the visible part of the idea.
3. (b) for corrals only (simplest "on the ranch" test).
4. (c) with an existing food first.

## 6. Save: resolved, and what was built

### Can the save store a Shadow slime actor? Yes.

- **Actor list** **[code]**: `GameModelPullHelpers.PullActors` (0x5E5C00) walks `GameModel.AllActors()` and
  writes `BuildActorData(actor)` for every actor whose type is **not** in `GameModel.ignoreOnLoad`
  (`IdentifiableTypeGroup.IsMember`). There is no other type filter, no "persist" flag and no despawn on
  save. `PushActors` / `PushActorData` (0x5EC010 / 0x5EB980) load every saved actor back: they only skip
  one with a non-finite position, throw for an unknown type id and skip an unknown scene group.
- **`ignoreOnLoad`** **[data]**: the `GameModel` MonoBehaviour in `corescenes_scenes_*.bundle` points it at
  the group **"SceneObjectGroup"**, whose only member is **Player**. So Shadow is saved like any slime.
- **Persistence id** **[data, log]**: the Sandbox spawn list leaves out every actor type whose
  `ReferenceId` is not a key of the save's `SaveReferenceTranslation` table (`Sandbox::Saveable`). The
  user's sr2mods.log (2026-10-24) lists the 12 left-out types (SpicyTofu, GingerVeggie, GlitchTarr, ...,
  LemonPhase): **Shadow is not among them**, so it has a persistence id. (Shadow is also a member of
  `IdentifiableTypesGroup`, the big all-types group, besides `ShadowAlertableGroup`, `SmallSlimeGroup` and
  `SlimesSinkInShallowWaterGroup`.)
- **Runtime-only state**: the scatter's `_latestAlertObject` / `_destroyTime` and `ProducePlortsOnHit.
  _numPlortsSpawned` are not saved, so a loaded shadow slime starts calm with a fresh plort count.
  `SlimeFeral.UpdateInstance` (0xBBD5D0) never poofs a slime on the ranch (`CellDirector.IsOnRanch`) or in
  the wilds: it pushes `_expireAt` an hour on.
- **Vacpack slots**: different story. `AmmoModel.Push` copies saved slots by index and
  `AmmoSlotManager.ValidateAndAdjustSlots` (on `SetModel`) clears a slot whose definition does not allow
  its id, and no vanilla slot allows Shadow. So shadow slimes in the vacpack are kept out of the vanilla
  save and stored in the sidecar instead (below).

### More hooks found

- `ShadowSlimeScatter.Relevancy` (0x8A2120) is 1 while the player is within `PlayerReactRadius` (not only
  when alerted); the plexer then selects the behaviour and `Selected` (0x8A23F0) calls `TriggerAlert(player)`.
  `ScatterIfPlayerNear` is called from `SubbehaviourPlexer.FixedUpdateInstance`. `StartFleeing`
  (`IFleeBehavior`, called by `ProducePlortsOnHit.ProcessCollisionEnter` after a hit plort) and
  `DoJump` also go through `TriggerAlert`; `OnDisruptionLevelChanged` calls `DoAlert` directly.
- `ProducePlortsOnHit.ProducePlort` (0x885CA0) returns early once `_numPlortsSpawned >= PlortCount`;
  `LateUpdateAll` (0x8856B0) does the despawn (`Destroyer.DestroyActor(..., "ProducePlortsOnHit")`). The
  component is also on Gold slimes (`PlortIdentifiableType` tells them apart).
- Vac capture path **[code]**: `VacuumItem.ConsumeVacuumable` (0xA645B0): a `Vacuumable` with `Size` 0 and
  `CanCapture` goes to `StartConsuming` -> `Vacuumable.TryConsume` (0x644640) ->
  `AmmoSlotManager.MaybeAddToAnySlot` -> `TryFindSlot` -> `AmmoSlotDefinition.IsAllowed`; no other check.
  The shadow prefab's `Vacuumable`: `Size` 0, `_preventForcesFromWeaponVacuum` 0 **[data]**.
- Eating **[code]**: `SlimeEat.WillNowEat` = `DoesEat(id)` (`_allEats`, built from `Diet.EatMap` in
  `CalculateAllEats` = `InitFood`) && (feral || `_keepEatingWhenFull` || `IsHungryFor(id)`). The shadow
  prefab's `SlimeEat` has `MinDriveToEat` 0.333 like the Pink slime's **[data]**, but its hunger never rises.
  `GotoConsumable.Relevancy` uses `KeepEatingWhenFullDrive` (0.9-1) for a slime that keeps eating when full.
  `SlimeDefinitions.RefreshDefinitions` rebuilds every `EatMap` (new lists).
- Plort gate when full (the Ringtail rule) **[code]**: `SlimeEat.FinishChomp` (0xBB6780) eats with
  `skipProduction = _keepEatingWhenFull && !IsHungryFor(id)`. `IsHungryFor` (0xBB7380): `!SlimeModel.isSleeping`
  and `EmotionDriveCalculator.Drive` (0xAB3D70, `max(emotion, MinDrive) + ExtraDrive`, clamped >= 0) >=
  `MinDriveToEat`. `InvokeOnEat` (0xBB7190): hunger -= `DrivePerEat`, agitation -= `AgitationPer(Fav)Eat`.
  `CanEat` / `WantsToEat` / `IsHungryFor` all say no while `SlimeModel.isSleeping` (saved field, set by
  `SlimeEmotions.SetAsleep` from daze / ignite / vortex / explode, not by Ringtails). Ringtails have the
  `KeepEatingWhenFull` component (Awake sets the flag; UpdateInstance sets `SlimeFaceAnimator._isMischievous` =
  `!IsHungryForNonPlorts` when `_showMischievousFace`). Their stone form is `TurnToStoneInDaylight.BecomeStatue`
  (0xBDCB60): sets `SlimeModel.isStatue`, instantiates the `RingtailStatue` actor and destroys the slime.
- Hunger rate **[code]**: `SlimeEmotions.SetModel` (0x88B980) subscribes `_hungerRecoveryRate` to the
  `SlimeEmotionConfig.HungerRate` property of the actor's property tracker (Shadow's override: 0);
  `ComputeHungerChangePerHour` (0x88AE10) = rate scaled by `_hungerRecoveryFactor` (+ day / night extra), times
  `_hungerDirection` (Awake: `_hungerConfig.RecoversTowardOne ? 1 : -1`); `UpdateToTime` (0x88C040) integrates
  `_changeRate` per game hour into `_emotions` and copies them to `SlimeModel.Emotions` (saved).

### What the Shadow Catcher does (all only while it is owned)

| part | how |
|---|---|
| (a) vac them | `AmmoSlotDefinition.IsAllowed` detour: true for the Shadow `SlimeDefinition` on the player's regular slot definitions (same type group as `AmmoSlot1`, so the six vanilla slots and Extra Tank III / IV; not the water slot, silos, drones, feeders). Shadow slimes don't notice the player any more (`ScatterIfPlayerNear` skipped, `Relevancy` 0 unless alerted), so you can walk up to them; a hit still alerts them as in vanilla. |
| (b) stay on the ranch | on the ranch (`CellDirector.IsOnRanch(scatter._member)`): `DoAlert` skipped, an old alert cleared (`_latestAlertObject` null, `_destroyTime` 1e300, face `_isFleeing` false), `Action` skipped, `Relevancy` 0; `ProducePlort` skipped (no hit plorts on the ranch; `ProcessCollisionEnter` 0x8859F0 is its only caller). They save as normal actors. |
| (b2) need shade by day | the Phosphor rule (`DestroyOutsideHoursOfDay`: live window + endure hours, stopped by any `CaveTrigger`; the corral Solar Shield is a `CaveTrigger` box, 10 x 5 / 10 x 8 x 10 m): in daylight a kept slime outside every active `CaveTrigger` collider's bounds gets a fade clock (Phosphor's endure, floored at 0.25-0.5 game h), then a deliberate vanilla `DoAlert` makes it vanish. Card text and the English Slimepedia risks entry carry the note. |
| (c) eat | the Shadow diet's `EatMap` gets copies of the Pink slime's food entries (no plorts, no transformations, `MinDrive` / `ExtraDrive` 0) producing Shadow plorts; each kept slime gets `_keepEatingWhenFull` and `InitFood`, plus the Ringtail rhythm: `DrivePerEat` >= 1, the vanilla default hunger rate, mischievous face while full (in memory). |
| save of the vacpack | `AmmoModel.Pull` (player): an empty `AmmoSlot` replaces each slot holding Shadow slimes in the save's copy; `custom_upgrades.vacpack.shadow_slots` = `v=count;...` (vanilla slot index). The tick puts them back after a load. |

Not done / limits: no new food item (an existing diet copied instead); the diet change is global, so a
feral wild shadow slime could eat too (non-feral wild ones never get hungry and don't keep eating); kept
slimes eat whenever food reaches them like Ringtails, but only a meal while hungry makes a plort; emotions of a
vacuumed shadow slime are lost (only type and count are stored, like the extra slots). No Shadow largos exist in
1.3.0 (`CanLargofy` 0, no largo definition or name pairs Shadow with anything); new ones would need new type ids
(not save-safe), so none were made.
