# Shadow slimes: research for "vacuum them, keep them on the ranch, feed them" (SR2 1.3.0)

User idea: a refinery / Fabricator vacuum upgrade that lets you collect shadow slimes; on your own
ranch they don't disappear and they eat food (maybe a new food source for them).

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

## 5. Suggested order

1. Check in game (sandbox spawn list log) whether "Shadow" is saveable. If not, stop at (a) + a "they
   still vanish" warning, or keep ranch shadow slimes in the sidecar like the Hen Collector's chickens.
2. (a) with the scatter suppression - the visible part of the idea.
3. (b) for corrals only (simplest "on the ranch" test).
4. (c) with an existing food first.
