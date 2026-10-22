# SR2 v1.3.0 hook candidates

Game: Slime Rancher 2 v1.3.0, Steam buildid 24131940, Unity 6000.0.73f1, IL2CPP metadata v31.
Input hashes: `re/input/HASHES.txt`.

All addresses are **RVAs** relative to the GameAssembly.dll image base. The preferred base is 0x180000000, so the IDA VA is 0x180000000 + RVA. Field offsets are from the start of the managed object, with the 0x10 Il2CppObject header already included. They match what the dump prints.

Tools: Cpp2IL 2022.1.0-pre-release.21 (`--use-processor attributeinjector --output-as diffable-cs`) and Il2CppInspectorRedux Legacy CLI 2026.2 (C#, IDA script, C++ header, JSON), run on .NET runtime 10.0.13.

Sources (Cpp2IL and Inspector agree on the RVAs I spot-checked):
- `re/dump/cpp2il_diffable-cs_rva/DiffableCs/<Assembly>/<Namespace>/<Class>.cs`: one file per class, with `[Address(RVA=...)]` and `//Field offset` on every member.
- `re/dump/il2cppinspectorredux/types.cs`: single-file C#, with VAs as `// 0x0000000180xxxxxx-...`.
- `re/dump/il2cppinspectorredux/cpp/appdata/il2cpp-types.h` (structs `<Class>__Fields`) and `il2cpp-functions.h` (`DO_APP_FUNC(rva, ...)`).

Caveats:
- **Shared RVAs.** Some RVAs are shared because IL2CPP folds identical method bodies. Trivial getters such as 0x4DD840, 0x4E3AE0, 0x4ED3F0 (empty method) and 0x4E3350 (`return true`) are reused by many methods. Never hook those. Hook the class-specific RVAs instead.
- **Option values.** Many option values are not held directly by the menu classes. They are `ScriptedBool`, `ScriptedFloat` and `ScriptedInt` ScriptableObjects (MonomiPark.SlimeRancher.ScriptedValue) that game systems subscribe to.

---

## 1. Settings tab (inject a "Mods" category with slider and toggle rows)

The menu is data-driven:
- `OptionsConfiguration` is a ListAsset of `OptionsItemCategory`, and each category is one tab.
- An `OptionsItemCategory` is a ListAsset of `OptionsItemDefinition`, and each definition is one row.
- Each `OptionsItemDefinition._controlPrefab` names the row prefab.

The cleanest injection point is `OptionsUIRoot.BindCategories`: append a runtime-created `OptionsItemCategory` to the `categories` list.

| Class (namespace) | Key methods (RVA) | Key fields (offset) | Why |
|---|---|---|---|
| `OptionsUIRoot` (MonomiPark.SlimeRancher.UI.Options) : DisplayBaseUI | `Start` 0x71CEF0, `BindCategories(OptionsConfiguration, OptionsAboutCategory)` 0x71A010, `BindItemCategory(OptionsItemCategory)` 0x71A2F0, `SwapCategory(int,bool)` 0x71D1A0, `CheckThenSwapCategory` 0x71A710, `ApplyChanges` 0x719A30, `SaveSettings` 0x71CBA0, `CreateOptionsViewModel` 0x71AB90, `Close` 0x71A860 | `optionsConfigurationProvider` 0xD0, `categoryAdapter` (CategoryAdapterView, the tab strip) 0xE0, `optionsAdapterView` 0x140, `optionsItemModels` List<IOptionsItemModel> 0x208, `currentCategoryIndex` 0x210, `optionsViewModel` 0x230, `optionsConfiguration` 0x238, `categories` List<IOptionsCategory> 0x240 | The options screen controller. Owns the tab list and builds the rows for each tab. |
| `OptionsConfiguration` (…Options) : ListAsset<OptionsItemCategory> | `GetDefinitionsFromCategory` 0x9C54C0 | (ListAsset backing list) | The asset that lists the tabs. Append a category here, or in `OptionsUIRoot.categories`. |
| `OptionsItemCategory` (…Options) : ListAsset<OptionsItemDefinition>, IOptionsCategory | `CheckIsActive` 0x9C6BC0, `get_ShowRebindButton` 0x573A90 | `_title` LocalizedString 0x20, `_icon` Sprite 0x28, `_showRebindButton` 0x30, `_activeCondition` 0x38 | One tab: title, icon and list of rows. |
| `OptionsItemDefinition` (…Options), abstract ScriptableObject | `CreateOptionItemModel` (abstract), `ShouldDisplay` 0x9C76A0, `get_ControlPrefab` 0x531320 (shared) | `_referenceId` 0x18, `_label` 0x20, `_detailsText` 0x28, `_controlPrefab` GameObject 0x30, `_applyImmediately` 0x38, `_isProfileSetting` 0x39, `_requireConfirmation` 0x3A, `_indent` 0x4C, `_showCondition` 0x50 | Row base class. `_controlPrefab` is the slider or toggle row prefab to clone. |
| `ToggleOptionDefinition` (…Options) : PresetOptionsItemDefinition | `ApplyPresetSelection(int)` 0x9D0D90, `GetOptionsPresets` 0x9D0E00, `GetDefaultPresetIndex` 0x9D0DF0 | `TargetBool` ScriptedBool 0x78, `_negativeOption` 0x90, `_positiveOption` 0x98, `_defaultValue` 0xA0 | Toggle row. Clone an existing instance and point `TargetBool` at our own ScriptedBool. |
| `ScriptedValueRangeOptionDefinition` (…Options) : PresetOptionsItemDefinition | `ApplyPresetSelection` 0x9D0030, `GetOptionsPresets` 0x9D0310, `GetValueForIndex` 0x9D0440, `GetDisplayForValue` 0x9D0130 | `_targetValue` ScriptedFloat 0x78, `_minValue` 0x80, `_maxValue` 0x84, `_valueStep` 0x88, `_normalizeOutputValue` 0x8C, `_defaultValue` 0x90, `_displayFormat` 0x98 | Slider-style row (min/max/step). It is a stepper, not a free slider. |
| `PresetOptionsItemDefinition` (…Options) | `CreateOptionItemModel` 0x9C9D20, `SetTempPresetIndex` 0x9C9E00, `ShouldDisplay` 0x9CA240 | `_wrapAround` 0x58, `_optionsItemModels` 0x60 | Shared base for toggle and range rows. Creates the PresetOptionsItemModel. |
| `OptionsItemViewHolder` (…UI.Options) : ButtonViewHolder | `BindData(IOptionsItemModel)` 0x718690, `NextSetting` 0x718B60, `PreviousSetting` 0x718CE0 | `label` 0x60, `controlHolder` 0x68, `_control` 0x88, `_stepperOptionControl` 0xA0, `_model` 0xB0 | Row view. Instantiates the `_controlPrefab` under `controlHolder`. |
| `OptionsPresetSelectorBase` / `OptionsPresetSelector` (…UI.Options) | `BindData(PresetOptionsItemModel)` 0x718F00, `Next` 0x719060, `Previous` 0x7194B0, `OnSelectionChanged` 0x719570 / 0x7198A0 | `_previousButton` 0x60, `_nextButton` 0x68, `optionsItemModel` 0x70, `_currentSelectionText` 0x88 | Left/right selector control used by toggle and range rows. |
| `OptionsDirector` (…Options) : SRBehaviour | `Awake` 0x9C5570, `PullSettings(OptionsV02)` 0x9C5E30, `PushSettings(OptionsV02)` 0x9C5E60, `GetRuntimeOptionsListFor` 0x9C57F0 | `_optionsModel` 0x60, `_optionsItemDefinitions` 0x68, `_runtimeOptionsLists` 0x70 | Persistence of option values. Useful if mod rows should save into the game's options file. Otherwise keep our own INI. |
| `TabbedControl` (…UI.Framework.CommonControls) | `GoToTab(int)` 0x7D6CD0, `UpdateTabs` 0x7D7620, `set_TabCount` 0x7D7E80 | `_buttonList` 0x38, `_tabCount` 0xC8, `_selectedIndex` 0xD0 | Generic tab strip, used by the map UI. The options menu uses CategoryAdapterView. |

Also relevant:
- `OptionsViewModel.GetOptionsItemsForCategory` 0x71E0E0 builds the row models for a tab.
- `RuntimeOptionsItemList` 0x9CF970 (ctor).
- `ScriptedFloatOptionsControl.BindData` 0x71E430 (control prefab for ScriptedFloatOptionDefinition).

---

## 2. Minimap

The game's map is a full-screen UI with no built-in minimap.
- `Map` (one per map page prefab) holds the world-to-map projection.
- `MapDirector` (ScriptableObject) maps each zone to a `MapDefinition`, which holds the background sprite.
- Fog reveal is event-driven: each `FogMapElement` carries a `StaticGameEvent` that is checked against `EventDirector`.

| Class (namespace) | Key methods (RVA) | Key fields (offset) | Why |
|---|---|---|---|
| `Map` (MonomiPark.SlimeRancher.UI.Map) : SRBehaviour | `GetMapPosition(Vector3 worldPos)` 0x715C30, `GetMapPosition(Vector3, RectTransform)` 0x715E70, `GetWorldArea` 0x715F60, `GetMapArea` 0x715AF0, `SetupInitialValues` 0x715FA0 | `_anchor1` 0x28, `_anchor2` 0x38 (MapAnchorCoordinates: world and map pairs), `_rectTransform` 0x70, `_projectionHelper` 0x78 | World-to-map transform per map page. |
| `MapProjectionHelper` (…UI.Map) | ctor `(minWorld, maxWorld, minMap, maxMap)` 0x70F7F0, `GetMapPosition` 0x70F740 | `_worldToMapScale` Vector2 0x10, `_worldToMapOffset` Vector2 0x18 | The linear XZ-to-UV projection (scale+offset). Read these two Vector2s and you can reproduce the transform natively. |
| `MapDirector` (MonomiPark.SlimeRancher.Map) : ScriptableObject | `GetMapForZone(ZoneDefinition)` 0x9C1A30, `GetAllMarkers` 0x9C18E0, `GetMarkers(scenes)` 0x9C1BD0, `RegisterMarker(string, IMapMarkerSource)` 0x9C2540, `IsZoneHidden` 0x9C2030, `TryGetPlayerNavigationMarkerLocation` 0x9C2AC0 | `DefaultMap` 0x20, `ZoneMappings` List<ZoneMapping{Zone 0x10, Map 0x18}> 0x28, `_hiddenZoneIcon` 0x40, `_filtersConfiguration` 0x68, `_eventDirector` 0x78, `UIRoot` 0x98, `Markers` Dictionary<string,Marker> 0xA0, `LastZoom` 0xB0 | Central registry of zones, maps and markers. Reachable via `SceneContextUtility.TryGetMapDirector`. |
| `MapDefinition` (…Map) : ScriptableObject | (data only) | `MapIcon` 0x18, `MapName` 0x20, `MapBackground` ImageSetup 0x40, `MapBackgroundOverlay` ImageSetup 0x60, `RelatedScenes` 0x80 | Zone map textures. Each ImageSetup holds the background Sprite. |
| `MapUI` (…UI.Map) | `OpenMap(MapDefinition)` 0x7127F0, `Update` 0x7155C0, `LateUpdate` 0x711E90, `UpdatePlayerMarker(PlayerDisplayOnMap, PlayerMapMarker)` 0x715360, `SetupMarker<T>` 0xF4DB10, `IsPlayerInMap` 0x711DD0 | `_mapBackground` Image 0xD0, `_tabbedControl` 0x108, `_fogReveal` 0x138, `_mapPrefabMapping` 0x140, `_markerPrefabMapping` 0x148, `_mapDirector` 0x188, `_map` 0x190, `_markers` 0x198, `_playerMarker` 0x1C0, `_zoneDetectionAreas` 0x1F0 | Full-screen map controller. Shows how the game instantiates a Map page, its markers and the player arrow. Template for a minimap. |
| `MapUIFogReveal` / `FogMapElement` (…UI.Map) | `RevealKnownZones` 0x729190, `RevealZonesInMap(Map)` 0x729650; FogMapElement `ShouldBeRevealed(EventDirector)` 0x704B50, `SetRevealState(bool)` 0x704B20 | MapUIFogReveal `_eventDirector` 0x20, `_fogElements` 0x40; FogMapElement `_fogRevealEvent` StaticGameEvent 0x20, `_canvasGroup` 0x30 | Fog and reveal state. Revealed means the zone's StaticGameEvent has fired in EventDirector. |
| `PlayerZoneTracker` (global ns) : MonoBehaviour | `GetCurrentZone` 0x98AA80, `GetLatestZone` 0x4F9D90 (shared getter, don't hook), `OnRegionsChanged` 0x98ACE0, `add_PlayerOnZoneChanged` 0x98B120 | `_currentZones` LinkedList<ZoneDefinition> 0x38, `_lastNotNullZone` 0x40, `PlayerOnZoneChanged` 0x48 | Current-zone tracker on the player. |
| `MapUIUtilities` (…UI.Map) static | `GetPlayerCurrentZone()` 0x729E40 | — | One-call helper for the current zone. |
| `PlayerDisplayOnMap` : DisplayOnMap | `GetZoneDefinition` 0x52DB10, `GetCurrentRotation` 0x52D9F0, `IsInUnknownArea` 0x52DCB0 | `_playerZoneTracker` 0x30, base `_sceneGroup` 0x20 | Player marker source: position, rotation and zone. |
| `MapMarkerDefinition` (…UI.Map) / `PlayerMapMarker` | `get_MapIcon` (shared); PlayerMapMarker `SetAnchoredPosition` 0x7333E0, `Rotate` 0x7332A0 | MapMarkerDefinition `_icon` Sprite 0x20; PlayerMapMarker `_arrowRect` 0x28 | Marker sprites and the player arrow. |
| `MapUtils` (…UI.Map) static | `ComputeMarkerSize(float)` 0x729F10, `ClampZoom` 0x729EF0 | consts DEFAULT_ZOOM 1, MIN 0.55, MAX 2 | Marker scaling versus zoom. |

Also relevant:
- `ZoneDetectionArea.ContainsPoint(Vector2)` 0x739800 is the polygon zone test on the map.
- `MapUIRoot.OpenMap` 0x729B90.
- `ZoneDefinition` lives in MonomiPark.SlimeRancher.World.

---

## 3. Spawn scaling / more ores

Ores are `ResourceNode`s (mineral deposits that the player harvests).
- One `ResourceNodeDirector` per area owns many `ResourceNodeSpawner` points.
- On a timer of `ResourceNodeDirectorConfig._updateNodesIntervalHours` (game hours), it fills up to `_maxActiveNodes`, picking node types by weight.
- The yield per node comes from `ResourceSpawnerDefinition._minResources` / `_maxResources` and its weighted `_resources`.

Garden and wild growers (`SpawnResource` / `ResourceGrowerDefinition`) are the other resource system. Slimes and animals come from `CellDirector` plus `DirectedActorSpawner`.

| Class (namespace) | Key methods (RVA) | Key fields (offset) | Why |
|---|---|---|---|
| `ResourceNodeDirector` (MonomiPark.SlimeRancher.World.ResourceNode) : SRBehaviour | `Update` 0x66BC30, `SpawnUpToMaxNodes` 0x66BA30, `SpawnNode` 0x66B6F0, `RegisterNodeSpawner` 0x66B530, `get_AllResourceDirectors` (static) 0x66BF60 | `_config` 0x28, `_nextNodeUpdateTime` double 0x30, `_nodeSpawners` List<ResourceNodeSpawner> 0x38, `_spawnersByNodeType` 0x40, `_timeDirector` 0x48; static `AllResourceDirectors` (static field 0x0) | The node director. Hook `SpawnUpToMaxNodes`, or scale config values at `Awake` 0x66B350. |
| `ResourceNodeDirectorConfig` (…World.ResourceNode) : ScriptableObject | `get_MaxActiveNodes` 0x5732F0, `get_UpdateNodesIntervalHours` 0x4EE590 (both shared getters, so patch the data instead) | `_resourceNodeWeightMap` WeightedEntry[] {NodeDefinition 0x10, Weight 0x18} 0x18, `_maxActiveNodes` int 0x20, `_updateNodesIntervalHours` float 0x24 | Max-active nodes, respawn interval and node-type weights. Multiplying `_maxActiveNodes` and dividing the interval gives "more ores". |
| `ResourceNodeSpawner` : IdHandler | `SpawnNode(ResourceNodeDefinition)` 0x53A7C0, `CreateResourceNode` 0x53A080, `DespawnNode` 0x53A280 | `_resourceNodeDefinitions` 0x30, `_model` 0x38, `_attachedResourceNode` 0x40 | A single spawn point. The list lives in the director's `_nodeSpawners`. |
| `ResourceNode` : RegisteredBehaviourType<ResourceNode> | `SpawnResources` 0x53B990, `SpawnSingleResource` 0x53BA90, `SetStateReady` 0x53B6D0, `SetStateHarvesting` 0x53B570, `UpdateInstance` 0x53C320 | `_timeToHarvest` 0x3C, `_resourceSpawnRadius` 0x68, `_resourceSpawnDelaySeconds` 0x6C, `_harvestAtTime` 0xA8, `_model` 0xE0 | The live node and its harvest logic. Yield is spawned here from `_model.resourcesToSpawn`. |
| `ResourceSpawnerDefinition` (base of ResourceNodeDefinition and ResourceGrowerDefinition) | `GetResourcesToSpawn(bool forceMax)` 0x53C5C0, `GetRandomResourceId` 0x53C450 | `_resources` WeightedResourceEntry[] {ResourceIdentifiableType 0x10, Weight 0x18} 0x20, `_minResources` 0x28, `_maxResources` 0x2C | Yield data asset. Hooking `GetResourcesToSpawn`, or bumping min/max, scales the drops per node. |
| `ResourceNodeDefinition` : ResourceSpawnerDefinition | `GetRandomLifetimeHours` 0x539D90, `GetRandomResourceSpawnerPrefab` 0x539E00 | `_nodePrefabs` 0x38, `_minLifetimeHours` 0x40, `_maxLifetimeHours` 0x44 | Node lifetime before despawn. |
| `ResourceNodeSpawnerModel` (…DataModel) | ctor 0xACE090, `Pull` 0xACDE60, `Push` 0xACDF20 | `nodeState` 0x28, `despawnAtWorldTime` 0x30, `resourceNodeDefinition` 0x38, `resourcesToSpawn` List 0x48 | Persisted per-spawner state, saved in the save file. Be careful with changes that persist. |
| `SpawnResource` : IdHandler | `Update` 0xBC32F0, `UpdateToTime` 0xBC2C00, `Spawn(SpawnRequest)` 0xBC1DD0, `UpdateNextSpawnTime` 0xBC2B70 | `_resourceGrowerDefinition` 0x30, `SpawnJoints` 0x38, `_spawnQueue` 0x68, `_totalSpawnsRemaining` 0x78 | Garden and wild grower spawns (crops, fruit trees). |
| `ResourceGrowerDefinition` : ResourceSpawnerDefinition | (getters, all shared) | `_minSpawnIntervalGameHours` 0x48, `_maxSpawnIntervalGameHours` 0x4C, `_maxActiveSpawns` 0x50, `_maxTotalLifetimeSpawns` 0x54 | Grower interval and max-active data. |
| `CellDirector` : SRBehaviour | `Update` 0x515ED0, `UpdateToTime(double)` 0x515780, `ForceCheckSpawn` 0x513530 | `TargetSlimeCount` 0x30, `TargetAnimalCount` 0x34, `MinPerSpawn` 0x38, `MaxPerSpawn` 0x3C, `AvgSpawnTimeGameHours` 0x4C | Slime and animal population director per cell, if spawn scaling should cover creatures too. |
| `DirectedActorSpawner` (abstract) : IdHandler | `Spawn(int, Randoms)` 0x517540, `CanSpawn` 0x516D60 | `Radius` 0x40, `SpawnDelayFactor` 0x44, `Constraints` 0x50, `SpawnLocs` 0x58 | Spawn points for slimes, animals, crates and so on. |

---

## 4. HL movement (bhop, air-strafe, ground friction)

The controller stack is:
- `SRCharacterController` : `KFCCharacterController`, which implements the KCC `ICharacterController` interface (the KinematicCharacterController asset).
- `KinematicCharacterSystem.FixedUpdate` drives `KinematicCharacterMotor.UpdatePhase1/2`, which call back into `UpdateVelocity`.

All per-frame velocity shaping lives in the virtual `Handle*` methods on `KFCCharacterController`. These are ideal detours for Quake/HL-style acceleration.

| Class (namespace) | Key methods (RVA) | Key fields (offset) | Why |
|---|---|---|---|
| `KFCCharacterController` (MonomiPark.KFC) : MonoBehaviour, ICharacterController | `UpdateVelocity(ref Vector3, float)` 0x5BEF50, `HandleStablyGroundedVelocity` 0x5BCCF0, `HandleStablyGroundedDrag` 0x5BCCA0, `HandleHorizontalAirborneMovement` 0x5BC840, `ClampAirborneHorizontalVelocity` 0x5BBD40, `HandleAirborneDrag` 0x5BC310, `HandleGravity` 0x5BC3D0, `UpdateMovementAndLookVectors(Vector2, Quaternion)` 0x5BE720, `get_IsGrounded` 0x5BF6D0, `PostGroundingUpdate` 0x5BDC40 | `_parameters` CharacterControllerParameters 0x20, `_motor` 0x48, `AbilityBehaviors` 0x50, `LookVector` 0x88, `MovementVector` Vector3 0x94, `GravityAmount` 0xA8, `InputVector` Vector2 0xB0 | The core movement math. Replace `HandleHorizontalAirborneMovement` (air-accel), the `ClampAirborneHorizontalVelocity` speed cap and `HandleStablyGroundedDrag` (friction) for HL movement. |
| `CharacterControllerParameters` (MonomiPark.KFC) : ScriptableObject | (getters, shared) | `_maxGroundedMoveSpeed` 0x18, `_stableMovementSharpness` 0x1C, `_groundedDrag` 0x24, `_maxAirMoveSpeed` 0x28, `_airAccelerationSpeed` 0x2C, `_airMovementSharpness` 0x30, `_airDecelerationSharpness` 0x34, `_airborneDrag` 0x38, `_gravityMagnitude` 0x3C, `_abilityData` 0x50 | Tunables. A data-only patch, such as raising max air speed and zeroing air drag, gives a cheap first pass. |
| `SRCharacterController` (MonomiPark.SlimeRancher.Player.CharacterController) : KFCCharacterController | `Update` 0xA7FEE0, `LateUpdate` 0xA7EB90, `HandleGroundOnAirborneMovement` 0xA7E9F0, `get_IsSprinting` 0xA80160, `get_CurrentEnergy` 0xA80050, `StartEnergyRegen` 0xA7F090 / `StopEnergyRegen` 0xA7F230 | `_energyResource` 0xE8, `_energyShouldRegen` 0x108, `_currentEnergy` UnitResource 0x110, `_characterInput` 0x148 | The player-specific subclass, handling energy and the animator. |
| `KinematicCharacterMotor` (KinematicCharacterController) | `UpdatePhase1` 0x59D490, `UpdatePhase2` 0x59EA60, `ProbeGround` 0x59BD20, `ForceUnground(float)` 0x598FD0 | `MaxStableSlopeAngle` 0x44, `StepHandling` 0x50, `GroundingStatus` CharacterGroundingReport 0x98, `LastGroundingStatus` 0xD8, `CharacterController` 0x1C0, `BaseVelocity` Vector3 0x1F8 | Grounded check (`GroundingStatus.IsStableOnGround`) and the raw velocity. |
| `KinematicCharacterSystem` | `FixedUpdate` 0x5C0600, `Simulate(float, motors, movers)` 0x5C11E0 | — | The simulation tick. |
| `JumpAbilityBehavior` / `JumpAbilityData` (…CharacterController.Abilities) | `UpdateVelocity` 0xA782E0, `CanStart` 0xA77AF0, `Start` 0xA78070, `OnGroundedStateChanged` 0xA77E90 | Behavior: `_landingTime` 0x68, `_jumpRequested` 0x8C. Data: `_jumpUpSpeed` 0x60, `_sidewaysSpeedMultiplier` 0x64, `_backwardsSpeedMultiplier` 0x68, `_airborneForceDamping` 0x6C, `_coyoteTime` 0x74, `_recurrenceDelay` 0x78 | Jump impulse and the re-jump delay. Auto-bhop needs `_recurrenceDelay`=0 plus a held-jump re-trigger. |
| `SprintAbilityBehavior` / `SprintAbilityData` | `Update` 0xA84A90, `CanStart` 0xA844A0, `ShouldStop` 0xA84890 | Data: `_sprintMultiplier` 0x60, `_minSprintValue` 0x64, `_maxSprintValue` 0x68, `_energyDecreasePerSecondDefinition` 0x78 | Sprint and energy drain. |
| `JetpackAbilityBehavior` / `JetpackAbilityData` | `UpdateVelocity` 0xA76F00, `Update` 0xA77010, `CanStart` 0xA75860, `Start` 0xA76260 | Data: `_jumpToJetpackPause` 0x64, `_hoverHeight` 0x68, `_maxUpwardThrustForce` 0x6C, `_energyPerSecond` 0x8C, `_initEnergyUsed` 0x90 | The jetpack, which must coexist with bhop (it engages on held jump). |
| `SRCharacterInput` (…CharacterController) | `PollInput` 0xA80FF0 | `_moveHorizontal` 0x20, `_moveVertical` 0x28, `_characterController` 0x50, `_movementVector` Vector2 0x60, `_lookVector` 0x68 | Raw move and look input for the wishdir in strafe code. |

Also relevant:
- `KFCCharacterController.AddForce` 0x5BB9D0.
- `UpdateVelocityWithExternalForces` 0x5BECC0.
- `SRAbilityBehavior<T>` / `SRAbilityData` are the base classes.
- `MonomiPark.KFC.GroundedStateChangeEventInfo`.

---

## 5. Version guard

`globalgamemanagers` contains the bundle version string "1.3.0", so `Application.version` returns "1.3.0".

| Where | RVA | Notes |
|---|---|---|
| `UnityEngine.Application.get_version()` (UnityEngine.CoreModule) | 0x339D370 | Best option. Call it via `il2cpp_resolve_icall` / the method pointer, or in IDA via `DO_APP_FUNC(0x0339D370, String*, Application_get_version, ...)`. `get_unityVersion` is 0x339D250 and `get_buildGUID` is 0x339C700; buildGUID is unique per build, which makes it a strong guard. |
| `SceneSavedGameInfoProvider.GetVersion()` (global ns, implements interface `SavedGameInfoProvider`) | 0x991B60 | The version the game writes into saves (`GameSummaryV03.GameVersion` at field offset 0x10). |
| `MonomiPark.SlimeRancher.UI.MainMenu.LocalizedVersionText.OnEnable` | 0x726E80 | The main-menu version label, a good place to append a "mods loaded" tag. |

Recommended native guard: check the SHA-256 of GameAssembly.dll (or its PE TimeDateStamp and SizeOfImage) against `re/input/HASHES.txt` before applying any RVA-based hooks. Use `Application.version == "1.3.0"` as a secondary check. RVAs are only valid for this exact binary.
