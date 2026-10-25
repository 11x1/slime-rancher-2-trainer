# World: context singletons, identifiables, spawning, resource nodes

`MP.SR` = `MonomiPark.SlimeRancher`. Tags: **IDA/code** = decompiled; **dump** = signature only; **data** = asset bundles. Hooks: re/hooks.md.

## Context singletons

- `SceneContext` (global; `SRSingleton<SceneContext>.get_Instance()` shared generic 0x4DC510): `<PediaDirector>` +0x38, `<GameModel>` +0x98 (null = no save loaded; `SceneContextUtility.TryGetGameModel` 0x693A80), also `PlortEconomyDirector`, `TimeDirector`, `PlayerState`, `MapDirector` (`SceneContextUtility.TryGetMapDirector`). Exists per scene; a new scene group = new directors (re-check cached pointers).
- `GameContext.Instance`: `<AutoSaveDirector>k__BackingField`, `LookupDirector`.
- `LookupDirector`: `GetIfReady()` static; `GetPlotPrefab(LandPlot.Id)` (`_plotPrefabDict`; plot prefabs with every upgrade part as inactive children: re/plots.md); `get_GadgetDefinitions()`, `get_MarketLink()`, `get_RefineryLink()`, `_toyDefinitions`, `_upgradeDefinitions`, `CurrencyList`.
- `GameModel`: `AllActors()`, `GetLandPlotModel(id)` (`.gameObj` = the LandPlotLocation), `ignoreOnLoad` (group "SceneObjectGroup" = Player only). Save details: re/save.md.

## Identifiables

- `IdentifiableType.icon`, `.localizedName` (+0x28), `.prefab`, `IsPlort`, `ReferenceId`. `IdentifiableTypeGroup.IsMember(id)`.
- `Identifiable.identType`; `Identifiable.get_AmmoMetadata` is virtual (look it up on the object's class).
- Groups (data): MeatGroup = {Hen, Rooster, BriarHen, StonyHen, SeaHen, ThunderHen, PaintedHen, CandiedHen, ElderHen, ElderRooster, UnstableMeat}; ChickGroup = the 7 chicks; ElderGroup = {ElderHen, ElderRooster}; `IdentifiableTypesGroup` = all types. More in re/slimes.md, re/vacpack.md.
- `Destroyer.Destroy(go, reason)`, `Destroyer.DestroyActor(go, reason)`; `InstantiationHelpers.InstantiateActor` (spawn an actor; sandbox spawner, Slime Radio).

## Resource nodes (ores; modules/spawn_scaling)

One `ResourceNodeDirector` per area owns many `ResourceNodeSpawner` points; on a timer of `_updateNodesIntervalHours` (game hours) it fills up to `_maxActiveNodes`, picking node types by weight. Yield per node from `ResourceSpawnerDefinition._minResources/_maxResources` and weighted `_resources`.
- `ResourceNodeDirector` (MP.SR.World.ResourceNode) : SRBehaviour — `get_AllResourceDirectors()` static 0x66BF60 (static List, registered in `Awake` 0x66B350, removed in `OnDestroy` 0x66B480), `Update` 0x66BC30 (per frame; when world time >= `_nextNodeUpdateTime` -> `SpawnUpToMaxNodes`), `SpawnUpToMaxNodes` 0x66BA30 (lambda `b__15_0` 0x66FBA0 counts spawners with a node), `SpawnNode` 0x66B6F0 (weight lambda 0x5403C0 is shared; free spawner lambda 0x66FB70), `RegisterNodeSpawner` 0x66B530. Fields `_config` +0x28, `_nextNodeUpdateTime` double +0x30, `_nodeSpawners` List +0x38, `_spawnersByNodeType` +0x40, `_timeDirector` +0x48. Directors live in zone scenes: re-apply on scene load (`Awake` is the trigger).
- `ResourceNodeDirectorConfig` (SO; getters folded 0x5732F0 / 0x4EE590: patch data): `_resourceNodeWeightMap` WeightedEntry[] +0x18 (entry `NodeDefinition` +0x10, `Weight` float +0x18), `_maxActiveNodes` int +0x20, `_updateNodesIntervalHours` float +0x24. Configs may be shared between directors (dedupe by pointer). Max active is bounded by the spawner count.
- `ResourceSpawnerDefinition` (global, abstract SO; base of node AND grower definitions) — `GetResourcesToSpawn(bool forceMax)` 0x53C5C0 (count in [min,max], ids by weight via `GetRandomResourceId` 0x53C450; lambdas share 0x53C820). Fields `_nameKey` +0x18, `_resources` WeightedResourceEntry[] +0x20 (`ResourceIdentifiableType` +0x10, `Weight` +0x18, `MinimumAmount` int +0x1C), `_minResources` +0x28, `_maxResources` +0x2C, `_persistenceId` +0x30.
- `ResourceNodeDefinition` : ResourceSpawnerDefinition — `_nodePrefabs` +0x38, `_minLifetimeHours` +0x40, `_maxLifetimeHours` +0x44; `GetRandomLifetimeHours` 0x539D90, `GetRandomResourceSpawnerPrefab` 0x539E00.
- `ResourceNodeSpawner` : IdHandler — `SpawnNode(ResourceNodeDefinition)` 0x53A7C0, `CreateResourceNode` 0x53A080, `DespawnNode` 0x53A280; `_resourceNodeDefinitions` +0x30, `_model` +0x38, `_attachedResourceNode` +0x40.
- `ResourceNode` : RegisteredBehaviourType<ResourceNode> — `SpawnResources` 0x53B990 (coroutine `SpawnResourcesAsync` MoveNext 0x53FC80: each id of `_model.resourcesToSpawn`, spaced by `_resourceSpawnDelaySeconds`, radius `_resourceSpawnRadius`), `SpawnSingleResource` 0x53BA90, `SetStateReady` 0x53B6D0, `SetStateHarvesting` 0x53B570, `UpdateInstance` 0x53C320. Fields `_timeToHarvest` +0x3C, `_resourceSpawnRadius` +0x68, `_resourceSpawnDelaySeconds` +0x6C, `_harvestAtTime` double +0xA8, `_model` +0xE0.
- `ResourceNodeSpawnerModel` (MP.SR.DataModel) — `nodeId` +0x20, `nodeState` +0x28, `despawnAtWorldTime` double +0x30, `resourceNodeDefinition` +0x38, `resourceNodeVariantIndex` +0x40, `resourcesToSpawn` List<IdentifiableType> +0x48, `resourceNodeDefinitions` +0x50; ctor 0xACE090, `Pull` 0xACDE60 / `Push` 0xACDF20 (saved as `ResourceNodeV01`). The rolled yield is saved: a yield change only affects nodes spawned afterwards.

## Growers, cells, spawners

- `SpawnResource` : IdHandler (garden/wild growers) — `Update` 0xBC32F0, `UpdateToTime` 0xBC2C00, `Spawn(SpawnRequest)` 0xBC1DD0, `UpdateNextSpawnTime` 0xBC2B70; `_resourceGrowerDefinition` +0x30, `SpawnJoints` +0x38, `_spawnQueue` +0x68, `_totalSpawnsRemaining` +0x78; `_landPlot` (garden plots).
- `ResourceGrowerDefinition` : ResourceSpawnerDefinition (getters folded) — `_minSpawnIntervalGameHours` +0x48, `_maxSpawnIntervalGameHours` +0x4C, `_maxActiveSpawns` +0x50, `_maxTotalLifetimeSpawns` +0x54.
- `CellDirector` : SRBehaviour (slime/animal population per cell) — `Update` 0x515ED0, `UpdateToTime(double)` 0x515780, `ForceCheckSpawn` 0x513530, static `IsOnRanch(member)`; `TargetSlimeCount` +0x30, `TargetAnimalCount` +0x34, `MinPerSpawn` +0x38, `MaxPerSpawn` +0x3C, `AvgSpawnTimeGameHours` +0x4C.
- `DirectedActorSpawner` (abstract) : IdHandler — `Spawn(int, Randoms)` 0x517540, `CanSpawn` 0x516D60; `Radius` +0x40, `SpawnDelayFactor` +0x44, `Constraints` +0x50, `SpawnLocs` +0x58.

## Interaction interfaces (world buttons)

- `ITechActivator` (`Activate()`, `GetCustomGuiPrefab()`): the "press E on a button" interface; `TechUIInteractable` shows the prompt and calls it. Ready-made: `PlortCollectorActivator` (Animator trigger + `PressButtonCue`), `SiloStorageActivator`, `ResourceCollectorActivator`, `FeederSpeedSelector` (3 states), `ObjectToggleActivator`.
- `UIActivator` (`uiPrefab`) / `LandPlotUIActivator`: `BaseUIInteractable`s whose `OnInteract` pushes a UI prefab via `UIDirector.CreateAndPush` (re/ui.md).

## Sprites reachable without a scan

`IdentifiableType.icon` (hens, plorts, food, gadgets; e.g. `LookupDirector.MarketLink.icon`); `MenuPurchaseItemModel._icon` of every vanilla plot upgrade item; `LookupDirector._upgradeDefinitions` icons; `CurrencyDefinition` in `LookupDirector.CurrencyList`; the shared `checkmark` sprite (`_unavailableIcon`). A full name list needs a one-time `Resources.FindObjectsOfTypeAll(Sprite)` dump. Mod icons: `ui/mod_icons.h`.

## Misc

- Achievements: `MetaGameDirector.Grant(Achievement)`; enum `Achievement` 0-26 (e.g. 14 Behind the Waterfall via `CheckForBehindTheWaterfall`, 26 `YouCanPetIt`). Cut achievements are not in the enum. More: re/debug-mode.md "Easter eggs".
- `SceneLoader`: `_gameDebugScene` / `_sceneDebugScene` refs never loaded in release. Debug tooling left in 1.3.0: re/debug-mode.md.

## From the custom_upgrades notes

### LookupDirector
- gotchas: `get_RefineryLink()` returned null for a whole session (user log); fallback = GadgetDefinition named `RefineryLink` in `_gadgetDefinitions` / member groups (member 13 of GadgetWarpGroup)
- `GetPlotPrefab(CORRAL)` etc. (in re)
- mod: quantum_link.cpp (EnsureDefinition fallback, WARN after 30 ticks)

### Actors / spawning
- `InstantiationHelpers.InstantiateActor` (as the sandbox spawner) - spawn a toy in front of the camera
- `GameModel.identifiablesByIdent[type]` - per-type actor lists (stations, Boomboxes, Night Lights, lamp decorations and Music Pillars / Tiles: slime_dj `lamps.cpp`, which also watches the lists' `_size` / `_version` to notice a placement); a cheap alternative to a scene search for placed gadgets of one type (`GadgetModel.transform` +0x38 null / destroyed while its scene is unloaded)
- `CellDirector.IsOnRanch(RegionMember)` 0x514EA0 (static) - any of member.Regions whose CellDirector is a ranch cell; member.Regions must not be null
- mod: dj_buildables.cpp, drone_source.cpp, shadow_catcher.cpp

### CaveTrigger
- `CaveTrigger.OnTriggerEnter` 0x511D60 calls `IListener.OnCaveEnter` on the entering collider's components; natural caves and the corral Solar Shield
- mod: shadow_catcher.cpp (shade = inside world AABB of an enabled, active CaveTrigger collider; rescan FindObjectsOfTypeAll every ~10 s; prefab assets have empty bounds)

### TeleporterNode / FX
- `TeleporterNode._activeFX` (teleporter gadget glow) - cloned script-free for the quantum link glow
- `FXHelpers.SpawnAndPlayFX` pooled

### Lava (research, nothing built)
- lava is `LavaVolume`, a trigger calling `ILavaTriggerReact` (`DestroyOnLava` poofs after `_hoursOfContactAllowed`, `TakeDamageInLava`, `BounceInLava`); no lava IdentifiableType; only `LiquidDefinition`s are `Water` and `MagicWater`
- vac liquids come from `LiquidSourceSurface` (`LiquidIdentifiableType`, `DestroyOnVac`, `FillVac`) registered in `LiquidSourceRegistry`

### Time
- `TimeDirector.CurrHour`, `HasReached`, `HoursFromNow`

## From the minimap, map_tools, quick_travel, wall_markers, collectibles notes

### GameModel (MonomiPark.SlimeRancher.DataModel)
- fields: `identifiablesByIdent` Dictionary<IdentifiableType, List<IdentifiableModel>> (lists of GadgetModels = every gadget; `GameModel.AllGadgets` does the same), `player` (PlayerModel : IdentifiableModel), `eventRecordModel` (the loaded save's event record) (dump)
- methods: `GetGordoModel(string id)` - TryGetValue, null if never fed (IDA)
- mod: MM

### IdentifiableModel (MonomiPark.SlimeRancher.DataModel)
- fields: `sceneGroup`, `_ident` (IdentifiableType), `lastPosition` Vector3 (= GadgetModel's IMapMarkerSource.MapPosition) (dump)
- mod: MM

### GordoModel
- methods: `HasPopped()` = eat count == -1 (IDA)
- mod: MM (done state with fog ignored)

### SceneContextUtility / SceneContext
- methods: `SceneContextUtility.TryGetGameModel(out GameModel)`, `TryGetSceneContext`; `SceneContext.get_Player`
- mod: MM, GT

### EventDirector (MonomiPark.SlimeRancher.Event) / StaticGameEvent / EventRecordModel
- methods: `EventDirector.GetRecordEntriesForEventType(event)` 0xA71860 = `_model.GetEventEntries(event)`, null when `_model` is null (IDA)
- methods: `EventDirector.SetModel` 0xA71DC0 - stores `_model`, subscribes `SceneContext.onSessionEnded` -> `_model = null` (0x674540) (IDA)
- methods: `EventDirector.Init(GameModel)` - registers as participant of `GameModel.eventRecordModel` (IDA)
- methods: `StaticGameEvent.RaiseEvent()` 0xA854B0 = `_eventDirector.RaiseEvent(this, 1)` (IDA)
- fields: `EventDirector._model` (EventRecordModel; null between sessions; may still be the previous save's record while a save loads), `StaticGameEvent._eventDirector`, `StaticGameEvent._eventKey` (string)
- gotchas: reveals are written into `StaticGameEvent._eventDirector`'s model (normally the same asset as MapDirector._eventDirector, not guaranteed); query readiness = director `_model` IS `GameModel.eventRecordModel`
- gotchas: a new save has the record but no reveal events: every fog area answers unrevealed
- mod: MM (fog readiness)

### TreasurePod (global ns)
- methods: `Awake` 0x553550, `OnDestroy` 0x5538F0 - unique bodies (hooked); `get_CurrState` (0 LOCKED, 1 OPEN)
- gotchas: pods never move (read position once)
- mod: MM (pod registry)

### ResourceNodeDirector (MonomiPark.SlimeRancher.World.ResourceNode) / ResourceNodeSpawner / ResourceNodeSpawnerModel
- methods: static `ResourceNodeDirector.get_AllResourceDirectors()` -> List
- fields: `_nodeSpawners`; `ResourceNodeSpawner._model`, `_attachedResourceNode` (ResourceNode); `ResourceNodeSpawnerModel.nodeState` (1 = READY), `resourcesToSpawn` (icon = `[0].icon`, IdentifiableType.icon)
- gotchas: a READY model may have no node object (empty spawn point / unloaded cell): require an alive, active `_attachedResourceNode`
- mod: MM

### SceneLoader (MonomiPark.SlimeRancher.SceneManagement)
- methods: `get_IsSceneLoadInProgress` (`<IsSceneLoadInProgress>k__BackingField`), `get_DefaultGameplaySceneGroup` (ranch group `ConservatoryFields`), `IsCurrentSceneGroupDefault()`, `LoadSceneGroup` (2), `EnqueueDelaySeconds(name, seconds)` (2), `EnqueueLoadDeathLoadingScene` / `EnqueueUnloadDeathLoadingScene` (0); fields `_currentSceneGroup`, `_deathLoadingScene`
- chains: `EnqueueLoadSceneGroup` queue (teleport, death, save load, menu): "Block TriggerExitLoadingAnimation" -> load `transitionScene`, "Setup Loading Scene" (`LoadingScreenView.Init`) -> "Wait For Loading Scene Fade In" = `EnqueueDelaySeconds(1.0)` (padding) -> camera off, destroy dynamic actors, unload old scenes, load core + group scenes, GC, UnloadUnusedAssets, world population, scene-group-loaded delegates, `BeforeLoadScreenRemoved`, camera on -> "Loading Screen Fade to Black" (`LoadingScreenView.FadeToBlack`, alpha += dt/_fadeOutTime) -> "Mark Load Complete" (IsSceneLoadInProgress = false), "Stop Stopwatch", "Wait for Loading Outro" (1.0 s only if `TriggerExitLoadingAnimation`, i.e. death screen) -> unload transitionScene (IDA)
- gotchas: the Player.log "Scene Transition from A to B took N seconds" is CUMULATIVE: `MonomiPark.SlimeRancher.Performance.Stopwatch.Start` calls System Stopwatch Start/Stop without Reset; diff consecutive lines (IDA)
- gotchas: teleport loads 3-5 s, of which >= 1.0 s fixed wait (log 2026-10-22); save load 7.1-9.1 s
- gotchas: `EnqueueDelaySeconds` returns an 8-byte `SceneLoaderQueueHandle` in rax; step names compared as strings ("Wait For Loading Scene Fade In", "Wait For Min Delay On Death", "Wait for Death Loading Outro")
- mod: QT (hooks), MM/MT/GT (IsSceneLoadInProgress guard)

### RegionLoader / Region / RegionRegistry  -- world streaming
- fields: `RegionLoader` (on the player): `_regionReg`, `LoadSize`, `WakeSize` boxes, `UpdateProxied(position)`; `Region.Bounds`, `_nonProxyRefCount`, `Root`; `Region.Proxied` = `_nonProxyRefCount == 0`
- methods: `RegionRegistry.GetRegions(group)` (new List<Region>); `Region.AddNonProxiedReference()` 0x8F5D00 (first ref -> `Unproxy`: `Root.SetActive(true)` + adjuncts); `RemoveNonProxiedReference()` 0x8F6B40 (`Proxy` at 0) (IDA)
- gotchas: proxied regions: Root (terrain, rocks, colliders) deactivated, only a collider-less proxy mesh drawn: raycasts over far spots hit nothing; objects inside stay registered but inactive (e.g. DiscordantWall)
- gotchas: unverified: Region.Bounds is world space
- mod: GT

### LightControl (MonomiPark.SlimeRancher.VFX.Lights) : MonoBehaviour  -- animated lights (glowing ice)
- fields (dump): `thisLight` +0x20, `lightData` (HDAdditionalLightData) +0x28, `controlFloat` (ScriptedFloat) +0x30, `isAnimated` +0x38, `loopAnimation` +0x39, `sampleGradientByTime` +0x3A, `gradient` +0x40, `gradientSampleSpeed` +0x48, `lerpIntensity` +0x4C, `intensityLerp0/1` +0x50/+0x54, `life` +0x60, `materialRenderer` +0x80, `materialToColor` +0x88, `shaderPropertyID` +0x90, `updateTick` +0xA4, `updateClock` +0xA8, `intensityValue` +0xB0
- `Update` 0xB851D0 (IDA): only while `thisLight.enabled`; intensityValue = intensityLerp1 (or lerp0..1 by controlFloat between transitionMin/Max); if animated: updateClock += dt, and once it reaches updateTick (and `materialRenderer` is visible, if set): gradient colour (by time with a random timeOffset, or by life) -> `HDAdditionalLightData.set_color`, `Material.SetColor(materialToColor, shaderPropertyID)` (the `_LightControlColor` of the ice material), intensityValue x gradient alpha, then `LightExtensions.SetIntensity(light, intensityValue)` 0x633920 (converts from the light's own unit to the native one, `Light.set_intensity`). Not animated: SetIntensity every frame. Static `CanUpdateThisFrame` throttles updates per frame.
- gotchas: intensityLerp0/1 are in the light's unit (EV100 for the ice lights: logarithmic), so scaling them is wrong; scale `Light.intensity` (native, linear) and accept LightControl's own writes as the new base.
- `Update` writes the light (SetIntensity) every frame when not animated, or on each tick while `materialRenderer` is visible; it only checks `thisLight.enabled`, not its own. Anything writing the light from another Update (the mod's tick runs in EventSystem.Update) is overwritten in an undefined order. `Awake` 0xB849B0: `thisLight` / `lightData` from the same GameObject, `materialToColor = materialRenderer.material` (an instance). `OnEnable` 0xB84E20: enables `thisLight` and itself, resets the clocks. No OnDisable. `+0x20 thisLight` (re_lookup).
  - `CullablePointLight.Cull` 0xAE5A80 (IDA): toggles only the Light and HDAdditionalLightData `enabled` (`culledAfter > level`), never the LightControl.
- Powderfall Bluffs glowing ice (data, `powderfallbluffs_scenes` bundle, UnityPy): scene objects `iceCube03_light` / `iceCube04_light` (material `objIce_IceCube01_lightGradient` / `_lightGreen` / `_lightPurple`, HDR `_InnerEmission`) each with child "Point Light Gradient" (point Light 8192, range 5, (0, 0.99, 0.55), LightControl + CullablePointLight); ~136 "Point Light Gradient" lights among the scene's 602 Lights. The Ice Lamp decoration has the same cube.
- mod: slime_dj `lamps.cpp` (one `FindObjectsOfTypeAll(LightControl)` per scene group, keeps `iceCube*` parents without a Gadget above them). While a lamp / crystal pulses its LightControl is disabled and its `Update` invoked from the pulse before the light is scaled (round 15); re-enabled on restore, with the Light's enabled state put back after OnEnable.
- log (round 13): the ranch (ConservatoryFields) has 20 LightControls and no `iceCube*` scene ice; Powderfall was never scanned in that test (the scan only ran while a speaker played). Whether the in-game names match the bundle data is still open: the scan now logs other LightControl parent names when 0 match.

### MusicTile (MonomiPark.SlimeRancher) : MonoBehaviour, IControllerCollisionListener, IControllerStepOnListener  -- Music Pillar / Music Tile decorations
- definitions (data, UnityPy, round 16): `MusicPillar` -> prefab `decorMusicPillar`, `MusicTile` -> `decorMusicTile` (no variants; prefabs in `scriptableobjectlookupdata_assets_all`). Root Gadget (FootprintRadius 1.25 / 2.0) + RegionMember + SanctuaryRoomMemberGadget; child "Tile" (BoxCollider, MusicTile, a Spring for the bounce) with `lightUpPillar_frame` / `lightUpTile_frame` (frame mesh), under it `lightUp*_light` (the `_tileRenderer`: material `objIce_lightUpPillar`, keyword `_USELIGHTCONTROLCOLOR_ON`, HDR `_InnerEmission` (0, 196351, 58597), `_LightControlColor` (0.38, 0.25, 0.38), `_SUVBrightness` 1) and `lightUp*_glow` (the `_effectRenderer`: material `LightUpTiles`, `_Brightness` 150, `_LightControlColor`, `_RainbowIntensity` 0.93), a point Light (`_pointLight`, pink (0.75, 0.51, 0.76), range 4 / 2.1, HDAdditionalLightData, no LightControl), "Sound FX Feedback" note particles (`MusicalNotes_exposure`).
- fields (dump): `_tileRenderer` +0x48, `_effectRenderer` +0x50, `_pointLight` +0x58, `_effectDuration` +0x60 (pillar 1.5 s, tile 2.5 s), `_tileBrightnessBoost` +0x64 (0.15 / 0.10), `_effectAnimationCurve` +0x68 (keys t 0 -> 0.50, 0.020 -> 3.05, 0.076 -> 3.37, 0.49 -> 0), `_playerFXPalette` +0x70, `_effectAnimationTime` +0xD0, `_effectAnimationValue` +0xD4, `_tileMaterial` +0xD8, `_effectMaterial` +0xE0, `_currentPalette` +0xE8 (TilePalette: MaskEdgeColor, BaseSpecularColor, LightControlColor)
- `Start` 0x5F6B60 (IDA): `_tileMaterial` / `_effectMaterial` = `Renderer.get_material` (instances, so `sharedMaterial` returns them afterwards), particle main modules.
- `Update` 0x5F6F30 (IDA): `_effectAnimationTime >= 1` -> `_effectRenderer.enabled = false` (every frame, idle). Else time += dt / `_effectDuration`, effect renderer on, v = curve(time), `_tileMaterial.SetFloat(_SUVBrightness, 1 + v x _tileBrightnessBoost)`, `_effectMaterial.SetFloat(_EffectOpacity, v)`. Never writes the Light's intensity.
- `PlayFeedback` 0x5F6A00 (from OnCollisionEnter / OnControllerCollision / OnControllerStepOn): SECTR cue at the tile, note particles, Spring.Nudge, `UpdateColors(player palette or _currentPalette)`, `_effectAnimationTime = 0`. `UpdateColors` 0x5F6C80: `_Mask_EdgeColor`, `_BaseSpecularColor`, `_LightControlColor` on the tile material, `_LightControlColor` on the effect material, `Light.set_color`, particle start colours.
- mod: slime_dj `lamps.cpp` (round 16): pulses `_InnerEmission` / `_LightControlColor` (property blocks) and `Light.intensity` like the other lamps, and on a beat sets `_effectAnimationTime` back (only ever smaller) so Update replays the hit flash without the sound. No hook.
- `List<T>` fields `_items`, `_size`, `_version` (bumped by every Add / Remove); `Dictionary<K,V>` `_buckets`, `_count`, `_freeCount` (mscorlib dump). slime_dj `lamps.cpp` watches `identifiablesByIdent._count` and the lamp definitions' list `_size` / `_version` to see a lamp placed within 0.25 s (round 16).

### DiscordantWall (MonomiPark.SlimeRancher.Labyrinth) : MonoBehaviour, IDisruptable  (TypeDefIndex 4768)
- fields: `_wallVfx` +0x20, `_harmonizerDetectionCollider` +0x28 MeshCollider, `_prismaDirector` +0x30, `_currentLevel` +0x38, `_disruptionRegion` +0x40, `_disharmonizedCollisionObject` +0x48, `_harmonizedCollisionObject` +0x50, `_transitionCollisionObject` +0x58, `_harmonizersToCheck` +0x70, `_relevantHarmonizers` +0x78, `_harmonizationTween` +0x80, `_currentHarmonizationAmount` +0x88 float, `_targetHarmonizationAmount` +0x8C float, static `_activeWalls` List<DiscordantWall> (dump)
- methods: `Awake` 0x9B8CD0 (caches PrismaDirector/DisruptionRegion, `_activeWalls.Add`), `OnDestroy` 0x9B9740 (Remove, kill tween), `.cctor` 0x9B9F60 (`_activeWalls = new List`), `DispatchHarmonizerAdded` 0x9B9070 (per wall: isActiveAndEnabled -> OnHarmonizerAdded, else queue in `_harmonizersToCheck`, evaluated in `OnEnable` 0x9B9850), `OnHarmonizerAdded` 0x9B99D0 (gadget within 1 m of `_harmonizerDetectionCollider.ClosestPoint` -> `_relevantHarmonizers.Add`; first -> Harmonize), `Harmonize` 0x9B9430 (target 1, tween current), `Disharmonize` 0x9B8E30 (target 0), `OnHarmonizerRemoved` 0x9B9C10, `UpdateHarmonizationCollision` 0x9B9D70 (curve(current) < 0.01 solid collider on; > 0.99 harmonized collider on; between: transition collider shrinks), `OnDisruptionLevelChanged` (VFX only) (IDA)
- gotchas: walls are never destroyed; intact = `_targetHarmonizationAmount < 0.5`; no per-wall save data, no discovered flag, no map marker (not IStaticMapMarkerComponent); state follows placed Nullifier gadgets
- gotchas: all 17 Labyrinth walls Awake once while the scene group loads; then proxied regions make them inactive (registered, alive, not active); a disabled collider reports empty bounds; inactive walls' state may be stale (harmonizers queued until OnEnable)
- gotchas: read `_activeWalls` via `il2cpp_class_get_static_field_data` + static offset; null static data = no wall woke yet (never run the cctor)
- mod: CW (Collect), WM, MM (icons), MT (remembered walls)

### Harmonizer (MonomiPark.SlimeRancher.Labyrinth) : MonoBehaviour, IGadgetPlacementListener  -- the "Nullifier" gadget
- methods: `Start` 0x9C05B0, `OnDestroy` 0x9C0450, `OnWallHarmonized(DiscordantWall)`; placing dispatches `DiscordantWall.DispatchHarmonizerAdded`, removing `DispatchHarmonizerRemoved` (IDA)
- gotchas: localisation key `m.gadget.name.harmonizer`, Pedia entry id 505644092240850944 = "Nullifier"
- mod: WM (reference)

### ZoneDefinition (MonomiPark.SlimeRancher.World)
- fields: `_isRanchZone` bool, `_localizedName` (LocalizedString, `GetLocalizedString()`)
- mod: MT

### Hazard volumes
- classes: `KillOnTrigger` (fall-through kill volumes, slime sea), `KillOnTriggerCloudOcean`, `LavaVolume`, `DamagePlayerOnEnterTrigger`
- chains: `KillOnTrigger.OnTriggerEnter` -> `DeathHandler.Kill` -> `PlayerDeathHandler.OnDeath` -> `ResetPlayer` (Player.log "Fallthrough destroying: PlayerControllerKCC")
- **IDA** `KillOnTrigger.OnTriggerEnter(Collider)` 0x871A40: `rb = SRGetComponentInParent<Rigidbody>(collider)`; skips trigger colliders, no rigidbody, and kinematic bodies unless `PhysicsUtil.IsPlayerMainCollider(collider)` (0x51D570; the player's KCC body is kinematic); logs "Fallthrough destroying: <name>", `DeathHandler.Kill(rb.gameObject, new Damage{SourceObject = this.gameObject, DamageSource = damageSource})`, then `playerKillFx` (player) or `killFX` at the body. `KillOnTriggerCloudOcean` (Labyrinth clouds): `OnTriggerEnter` 0x8716E0 only adds the collider to `_collidiers` (same filter); `FixedUpdate` 0x871240 kills every listed collider under `CloudOceanSurface.IsUnderClouds(pos)` the same way and removes it from the list.
- **IDA** `DeathHandler.Kill(GameObject, Damage)` 0xAC5BA0 (MP.SR.Damage, static, unique body): `SRGetComponent<IDeathHandler>(go)`; none -> `Destroyer.DestroyActor(go, DamageSource._logMessage)`; else the interface call (player: `PlayerDeathHandler.OnDeath(source, sourceObject, logMessage)`). The common end of both kill volumes. Mod: sandbox noclip drops it for the noclipping player (re/hooks.md).
- gotchas: render helpers with colliders: `Depth Only Quad`, names with `VISBLOCKER` (player falls through)
- gotchas: collider names seen: `caveRoof01`, `greenhouseRoof_03`, `greenhouseHangingPlanter_02`, `areaFieldsPlane03` (one-sided ground plane: no hit from below)
- mod: GT

## From the slime_dj, spawn_scaling, hl2_movement, quick_start, free_placement, misc_tweaks, plot_upgrades, drone_price_limit notes

### ResourceNode (global) - additions
- fields: `_breakMode` bool +0x90, `_isHarvestable` (dump/disasm)
- methods: `UpdateForState` 0x53BED0 - every frame per node (disasm):
  - READY (1): vac touching (`Vacuumable.Pending` = touched within the last 2 frames) and `_isHarvestable` -> HARVESTING, `_harvestAtTime = now + _timeToHarvest`
  - HARVESTING (2), vac on, normal node: list not empty and `now >= _harvestAtTime` -> `SpawnSingleResource()`
  - HARVESTING, `_breakMode`: once due, starts `SpawnResourcesAsync` **inline** (not via `SpawnResources()`), FX, EMPTY
  - HARVESTING, vac gone: back to READY with the rest of the list (partial harvest)
- methods: `SpawnSingleResource` 0x53BA90 - only caller UpdateForState; harvested event, `_harvestAtTime = now + _timeToHarvest`, instantiates `list[0].prefab`, `RemoveAt(0)`, EMPTY when empty. One item per `_timeToHarvest`, max one per frame (disasm)
- methods: `SpawnResourcesAsync.MoveNext` 0x53FC80 - per entry: instantiate, then `new WaitForSeconds(_resourceSpawnDelaySeconds)` (read live each step); no item cap; one actor per entry via `InstantiationHelpers.InstantiateActor` (disasm)
- gotchas: `SpawnResources()` 0x53B990 has **no native callers** in 1.3.0 (call/jmp byte scan of the il2cpp section); a hook on it never fires. Harvest time = count x `_timeToHarvest` (vacuum) or `_timeToHarvest + count x _resourceSpawnDelaySeconds` (break). Both time fields are per ResourceNode instance (prefab variant), not on the definition; liquid nodes (Deep Brine, Primordy Oil) are ordinary ResourceNodes with slower prefab values. `NodeState` enum: READY 1, HARVESTING 2 (read from metadata via `il2cpp_field_static_get_value`). Modifying a list during the break coroutine's enumeration throws; the vacuum path reads `list[0]`/`RemoveAt(0)` (no enumerator)
- mod: spawn_scaling hooks `SpawnSingleResource` (pre + post; post re-arms `_harvestAtTime = due + t` and may call the original up to 8 more times), `SpawnResources` (kept, dead)

### ResourceSpawnerDefinition.GetResourcesToSpawn(bool forceMax) 0x53C5C0 - body (disasm)
- `count = forceMax ? _maxResources : Randoms.GetInRange(_minResources, _maxResources + 1)`; first for each `_resources` entry with `MinimumAmount > 0` adds `min(count, MinimumAmount)` of that type and subtracts; then `count` weighted picks. List length == rolled count. Reads min/max/weights on every call.
- `ResourceNodeSpawner.SpawnNode(def)` 0x53A7C0 is the only roll: `model.nodeState = READY`, `model.resourceNodeDefinition = def`, `model.resourcesToSpawn = def.GetResourcesToSpawn(false)`; saved (`Persist.ResourceNodeV01`).
- gotchas: shared by grower definitions (`ResourceGrowerDefinition`); check the class is `ResourceNodeDefinition`
- mod: spawn_scaling hook (pre: patch `this`; post: record the rolled list)

### ResourceNodeSpawner / model (additions)
- `ResourceNodeSpawner._attachedResourceNode` +0x40 -> the live ResourceNode (validate `node._model == spawner._model`)
- `WeightedResourceEntry` (nested `ResourceSpawnerDefinition/WeightedResourceEntry`): `ResourceIdentifiableType` 0x10, `Weight` 0x18 (boxed offsets)
- `Persist.ResourceNodeV01`: `ID`, `ResourceNodeState`, `DespawnAtWorldTime`, `ResourceNodeDefinitionId`, `ResourceNodeVariantIndex`, `List<int> ResourcesToSpawn` - vanilla loads the list at any length (save-compatible to lengthen)
- spawn points are fixed scene `ResourceNodeSpawner`s; record count = spawn point count

### InstantiationHelpers
- `InstantiateActor` 0x870C40 - always instantiates (`GameModel.InstantiateActorModel`, `Object.Instantiate`, `RegisterDynamicObject`); no max-actor / per-cell limit field in the dump (disasm + dump search)

### Other resource sources (dump; not hooked)
- `ResourceGrowerDefinition` / `SpawnResource` (gardens, wild trees): shares ResourceSpawnerDefinition base; yield path (`SpawnJoints`, `_maxActiveSpawns`) not decompiled
- `BreakOnImpact` (crates): `_minActorSpawns` / `_maxActorSpawns` are shared `ScriptedInt` ScriptableObjects
- Ranch drone programs only move existing items (no yield data)

### SceneLoader (MonomiPark.SlimeRancher.SceneManagement)
- fields: `<IsSceneLoadInProgress>k__BackingField` (dump)
- methods: `LoadSceneGroup(group)`; `get_Stopwatch` 0x8DC6B0 (lazy, one `MonomiPark.SlimeRancher.Stopwatch` per SceneLoader); `Stopwatch.Start/Stop` 0x99EF80 / 0x99F010 only resume/pause the wrapped System.Diagnostics.Stopwatch (disasm)
- gotchas: the Player.log line `Scene Transition ... took N seconds` (from `SceneLoader+<>c__DisplayClass99_0.<EnqueueLoadSceneGroup>b__16` 0x8E3150) is **cumulative** load time since launch; one step's time = difference to the previous line (log confirms monotonic). The game's instance is `SystemContext.Instance.SceneLoader`.
- typical loads (log): CompanyLogo ~0.73 s, StandaloneStart ~0.14 s, MainMenuFromBoot ~8.6-8.9 s (~88k objects), MainMenu -> ConservatoryFields ~9.4-9.6 s
- mod: quick_start

### CutsceneDirector
- methods: `SpawnIntroSequence()` 0xAC4290 - activates the registered `IntroSequenceUIRoot` (registers itself in Awake) and pushes it on UIDirector; only caller is `AutoSaveDirector.StartNewGame`'s load step `<>c.<StartNewGame>b__29_2` 0x5E4150; `ClearIntroSequenceUIRoot()` (disasm)
- mod: quick_start (see re/ui.md IntroSequenceUIRoot)

### SceneContextUtility / currency
- `SceneContextUtility.TryGetPlayerState` -> live `PlayerState` = "in a save" (dump)
- `CurrencyUtility.DefaultCurrency` (the `CurrencyDefinition` with `_persistenceId == 1`, Newbucks) + `PlayerState.GetCurrency` (dump)
- mod: discord_rpc

## From the sandbox, sell_upgrades, collectibles, instant_build notes

### InstantiationHelpers
- methods: `InstantiateActor(prefab, SceneGroup, pos, rot, nonActorOk, NONE, NONE, Nullable<AmmoMetadata> metadata, bool, setCollected)` - creates actor model (`GameModel.InstantiateActorModel`), instantiates prefab, registers with `DynamicObjectContainer`, inits model (saved like any loose item)
- gotchas: runtime_invoke takes Nullable args as a pointer to the struct; pass zeroed (hasValue false); InstantiateActor only reads it for slimes after checking hasValue
- mod: sb DoSpawn (with `RegionRegistry.CurrentSceneGroup`)

### IdentifiableType
- fields: `icon`, `get_Icon` (virtual), `localizedName` (`GetLocalizedString`; missing translation returns `!! <key> !!`, e.g. `!! Actor.l.crate_reef_01 !!`), `prefab`, `groupType`, `Categories`, `ReferenceId`, `IsPlayer`, `IsSceneObject`, `IsPlort`, `IsAnimal`
- facts: loose actors = prefab has `IdentifiableActor`; gordos etc. are IsSceneObject; `GadgetDefinition` placed via gadget mode; `LiquidDefinition`, `ToyDefinition` kinds
- facts: types without any icon (offline): 20 crates, 7 giant eggs, vine bulb, 2 Mysterious Statues (`RingtailStatueLarge/Small`), Echo Net, pollen cloud, drone component, some largos
- mod: sb catalog, spawn list

### IdentifiableTypeGroup
- fields: `_memberTypes`, `_memberGroups`, `_icon`, `_localizedName`; `IsMember` (runtime membership), `GetAllMembers`
- asset (UnityPy, identifiabletypegroups_assets_*.bundle): `FoodGroup` = MeatGroup + VeggieGroup + FruitGroup + NectarFoodGroup; `CraftGroup` = ResourceOreGroup + ResourceHiveGroup + ResourceSpoutGroup + ResourceWeatherGroup; `StandardCrateClass` = 20 crates; `LargoGroup` = 19 `<Slime>LargoGroup`s; `GiantEggGroup` (7), `VineBulbGroup` (1), `RingtailStatueGroup` (2), `ToyGroup`, `GadgetGroup` = GadgetUtilitiesGroup + GadgetWarpGroup + GadgetDecorGroup + GadgetHiddenGroup (variants). Slime/largo/egg/bulb/statue/crate groups have no `_icon`
- gotchas: `IdentifiableType.Categories` asset names don't contain food/veggie/craft/resource; use the groups
- facts: Tarr group = `SceneContext.GameModel.tarrTypeGroup`; Tarrs setting `GameContext.GameSettingsDirector.TarrEnabled` (ScriptedBool)
- mod: sb item kinds, tabs, Tarr gray-out

### Crate assets
- asset names (identifiabletypes_assets_*.bundle): ContainerBluffs01, ContainerBluffsGold01, ContainerFields01, ContainerFieldsGold01, ContainerGorge01, ContainerGorgeGold01, ContainerLabyDreamlands01/Gold01, ContainerLabyHub01/Gold01, ContainerLabyLavaDepths01/Gold01, ContainerLabyShadow01, ContainerLabyTerrarium01/Gold01, ContainerLabyWaterworks01/Gold01, ContainerStrand01, ContainerStrandGold01 (ContainerGoldSlime01, ContainerBits_* are not crates). Gorge = Ember Valley, Strand = Starlight Strand, Bluffs = Powderfall Bluffs
- mod: sb kCrateCodes

### AccessDoor (ranch expansions)
- classes: `AccessDoor` (MonomiPark.World; `LOCKED 0 / OPEN 1 / CLOSED 2` in `AccessDoorModel.state`, saved; `set_CurrState` writes model + plays open animation (`ForceUpdate`); `LinkedDoors`, `OnUnlockedEvent`, `_model`), `AccessDoorUIActivator` (MonomiPark.SlimeRancher.UI.AccessDoor; `_purchaseMenuItem` / `_legacyUIPrefab` needed by `TryInitialize`)
- chains: `AccessDoorUI` confirm -> `SpendableDirector.Spend(item._purchaseCost)` -> `AccessDoorUIActivator.OnPurchaseMenuResult`: CurrState = OPEN; LOCKED linked doors -> CLOSED; `OnUnlockedEvent.RaiseEvent()`; PlayFab analytics; `MetaGameDirector.Grant(DomainExpansion)`; cue
- gotchas: activators exist only while the ranch scene is loaded; prefabs have no `_model`
- mod: sb player_extras.cpp unlock expansions

### MetaGameDirector (achievements)
- fields: `List<AchievementData> Achievements` (nested `MetaGameDirector.AchievementData`; enum `MetaGameDirector.Achievement`, 27 entries), `IMetaGamePlatform _platform`, `_tempBeallionaireProgress`
- methods: both `Grant` overloads and `CheckForBehindTheWaterfall` -> `AchievementData.IncrementProgress(n)` 0x5FE9C0; true (passed `LargestSavedProgress`) -> `_platform.Grant(data)`. Both `SetAchievementProgress` overloads do the same via `AchievementData.SetProgress(v)`
- methods: `HandleSoldPlort(price)` - Beallionaire: first sale of a 30 s window granted, later added to `_tempBeallionaireProgress`, flushed by `Update` / `ClearPendingBeallionaireUpdate` via SetAchievementProgress; `ValidatePersistantAchievements` (every load); `PullMetaGameData` -> `MetaGameDirectorModel.ProgressPerAchievement`; `SetModel` -> private `SetAchievementProgress(AchievementData, int)`; `OnGadgetFabricated`
- gotchas: `ForceComplete` has no native callers; `Clear` only on session end/load. SetProgress is also used by save load and `SteamMetaGamePlatform.LoadAchievementProgress` (don't hook it). Public SetAchievementProgress overload picked by first parameter class
- methods: `SteamMetaGamePlatform.Grant(AchievementData)` 0xB30F00 - only caller of `SteamUserStats.SetStat/SetAchievement/StoreStats` (`IndicateAchievementProgress` never called)
- mod: sb achievements.cpp hooks `sandbox_achievements` (IncrementProgress, SetAchievementProgress, HandleSoldPlort, SteamMetaGamePlatform.Grant), always on

### Spawners / TreasurePod  [col]
- methods: `TreasurePod.get_CurrState()` (OPEN; throws while no model), `GetZoneId()` (CellDirector); `ZoneSceneGroupMapping` zone -> SceneGroup[] (a SceneGroup loads all its scenes)
