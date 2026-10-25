# Map, fog, zones, markers, teleporters

`MP.SR` = `MonomiPark.SlimeRancher`. Tags: **IDA** = decompiled; **dump** = signature only. Mod use: `modules/minimap/`, `modules/map_tools/`, `modules/quick_travel/`, `modules/collectibles/`, `modules/wall_markers/`. Hooks: re/hooks.md.

The game's map is a full-screen UI scene (exists only while open: re/ui.md "UI display scenes"); no built-in minimap.
- `Map` (one per map page prefab) holds the world-to-map projection.
- `MapDirector` (SO) maps each zone to a `MapDefinition` (background sprite).
- Fog reveal is event-driven: each `FogMapElement` carries a `StaticGameEvent` checked against `EventDirector`.

### Map (MP.SR.UI.Map) : SRBehaviour
- `GetMapPosition(Vector3 worldPos)` 0x715C30 (world XZ -> map RectTransform local via `_projectionHelper`), `GetMapPosition(Vector3, RectTransform)` 0x715E70, `GetWorldArea` 0x715F60, `GetMapArea` 0x715AF0, `GetFullMapArea` 0x715A60, `SetupInitialValues` 0x715FA0 (builds `_projectionHelper` from the anchors), `Awake` 0x715790.
- fields: `_anchor1` MapAnchorCoordinates +0x28, `_anchor2` +0x38 (struct `WorldPosition` Vector2 +0x0, `MapPosition` Vector2 +0x8), `_markerPrefabMapping` +0x48, `_rectTransform` +0x70, `_projectionHelper` +0x78.

### MapProjectionHelper (MP.SR.UI.Map)
ctor `(Vector2 minWorld, Vector2 maxWorld, Vector2 minMap, Vector2 maxMap)` 0x70F7F0; `GetMapPosition(Vector3, RectTransform)` 0x70F740. Linear: `map = worldXZ * _worldToMapScale (+0x10) + _worldToMapOffset (+0x18)`. Or compute from the anchors: `scale = (mapB-mapA)/(worldB-worldA)`, `offset = mapA - worldA*scale`.

### MapDirector (MP.SR.Map) : ScriptableObject
- `GetMapForZone(ZoneDefinition)` 0x9C1A30 (`ZoneMappings.FirstOrDefault(m => m.Zone == zone)?.Map ?? DefaultMap`, lambda 0x9D20E0), `GetAllMarkers` 0x9C18E0, `GetMarkers(scenes)` 0x9C1BD0, `RegisterMarker(string, IMapMarkerSource)` 0x9C2540, `IsZoneHidden` 0x9C2030, `TryGetPlayerNavigationMarkerLocation` 0x9C2AC0.
- fields: `DefaultMap` +0x20, `ZoneMappings` List<ZoneMapping{Zone +0x10, Map +0x18}> +0x28, `_hiddenZoneIcon` +0x40, `_filtersConfiguration` +0x68, `_mapList` MapListConfiguration +0x70, `_eventDirector` +0x78, `UIRoot` MapUIRoot +0x98, `Markers` Dictionary<string,Marker> +0xA0, `LastZoom` +0xB0. Reach via `SceneContextUtility.TryGetMapDirector`.
- `MapDirector.Marker.get_Source` is folded (0x4DD840).

### MapDefinition (MP.SR.Map) : ScriptableObject
`MapIcon` +0x18, `MapName` +0x20, `MapOutOfBoundsIcon` +0x28, `MapOutOfBoundsLabel` +0x30, `MapAvailableCondition` +0x38, `MapBackground` ImageSetup +0x40 (ImageSetup: `TintColor` +0x0, `Image` Sprite +0x10 => def+0x50, `Material` +0x18), `MapBackgroundOverlay` +0x60 (Sprite at def+0x70), `RelatedScenes` +0x80.
`MapListConfiguration._maps` MapDefinition[] +0x18 (all map pages/tabs); `GetAvailableMaps(IEventDirector)` 0x9C3110.

### MapUI (MP.SR.UI.Map) — full-screen map controller
- `OpenMap(MapDefinition)` 0x7127F0, `Update` 0x7155C0, `LateUpdate` 0x711E90, `UpdatePlayerMarker(PlayerDisplayOnMap, PlayerMapMarker)` 0x715360, `SetupMarker<T>` 0xF4DB10, `IsPlayerInMap` 0x711DD0.
- fields: `_mapBackground` Image +0xD0, `_tabbedControl` +0x108, `_fogReveal` +0x138, `_mapPrefabMapping` +0x140, `_markerPrefabMapping` +0x148, `_mapDirector` +0x188, `_map` +0x190, `_markers` +0x198, `_playerMarker` +0x1C0, `_zoneDetectionAreas` +0x1F0.
- gotcha: `MapUI.Awake` runs from `Object.Instantiate` inside the UI display loader (crash 2026-10-22: never free GC handles in that hook).
- `MapUIRoot.OpenMap` 0x729B90.

### Fog
- `MapUIFogReveal`: `RevealKnownZones` 0x729190, `RevealZonesInMap(Map)` 0x729650, `Start` 0x7297C0; `_eventDirector` +0x20, `_fogClearCue` +0x28, `_mapDirector` +0x38, `_fogElements` +0x40, `_animatingElements` +0x48, `_animatedEvents` +0x50, `_hasStarted` +0x58, `_pendingMap` +0x60.
- `FogMapElement`: `ShouldBeRevealed(EventDirector)` 0x704B50 (revealed = EventDirector recorded `_fogRevealEvent`), `SetRevealState(bool)` 0x704B20; `_fogRevealEvent` StaticGameEvent +0x20, `_revealTime` +0x28, `_revealCurve` +0x2C, `_canvasGroup` +0x30.

### Zones and player position
- `PlayerZoneTracker` (global) : MonoBehaviour — `GetCurrentZone()` 0x98AA80 (first of `_currentZones`), `GetLatestZone` 0x4F9D90 is **folded** (read `_lastNotNullZone` +0x40 instead), `OnRegionsChanged` 0x98ACE0, `add_PlayerOnZoneChanged` 0x98B120. Fields `_member` +0x20, `_unlockedEvent` +0x28, `_lastZones` +0x30, `_currentZones` LinkedList<ZoneDefinition> +0x38, `_lastNotNullZone` +0x40, `PlayerOnZoneChanged` +0x48, `PlayerOnZoneEntered` +0x50, `PlayerOnZoneExited` +0x58.
- `MapUIUtilities.GetPlayerCurrentZone()` static 0x729E40.
- `PlayerDisplayOnMap` : DisplayOnMap — `GetZoneDefinition` 0x52DB10, `GetCurrentRotation` 0x52D9F0, `IsInUnknownArea` 0x52DCB0; `_playerZoneTracker` +0x30, base `_sceneGroup` +0x20.
- `ZoneDetectionArea.ContainsPoint(Vector2)` 0x739800 (polygon zone test on the map). `ZoneDefinition` is in MP.SR.World.

### Markers
- `MapMarkerDefinition._icon` Sprite +0x20 (`get_MapIcon` folded). `PlayerMapMarker`: `SetAnchoredPosition` 0x7333E0, `Rotate` 0x7332A0, `_arrowRect` +0x28.
- `MapUtils` static: `ComputeMarkerSize(float)` 0x729F10, `ClampZoom` 0x729EF0; consts DEFAULT_ZOOM 1, MIN 0.55, MAX 2.
- `MapElevationDisplay.SetElevationDifference(float)` 0x7075D0; sprites `_belowElevationIcon` +0x28, `_sameElevationIcon` +0x30, `_aboveElevationIcon` +0x38. Grey Labyrinth: no per-floor map type in code; one `LabyrinthZoneMarker` plus elevation icons.

### Recipe: minimap projection without hooks
Zone (`_lastNotNullZone`) -> `MapDirector.GetMapForZone` -> background sprite def+0x50 -> projection from the page prefab's `Map` (via `MapUI._mapPrefabMapping`, or the anchors) cached per MapDefinition -> fog via `FogMapElement.ShouldBeRevealed`.

## From the minimap, map_tools, quick_travel, wall_markers, collectibles notes

### Map (MonomiPark.SlimeRancher.UI.Map) : SRBehaviour  -- the map page component
- methods: `Awake` 0x715790 - does `foreach (go in _forceActiveOnAwake) go.SetActive(true)` (IDA, verified)
- methods: `GetMapPosition(Vector3)` 0x715C30 - `clamp((w.x*slope.x + offset.x, w.z*slope.y + offset.y), rect)`, z = w.y (IDA, verified)
- methods: `SetupInitialValues` 0x715FA0 - (slope.x, offset.x) = `MapUtils.GetSlopeAndOffset((_anchor1.WorldPosition.x, _anchor1.MapPosition.x), (_anchor2.WorldPosition.x, _anchor2.MapPosition.x))`, same with .y (WorldPosition.y is world Z); builds `_projectionHelper` (IDA, verified)
- methods: `GetMapArea` / `GetWorldArea` - the rects MapViewModel stores as `_mapRect` / `_worldRect` (= the page anchors) (dump)
- fields: `_forceActiveOnAwake` GameObject[] - "containers temporarily disabled to see under them"; on RainbowIslandMap = [zone_fog_areas, fog_static] (dump + UnityPy)
- fields: `_zoneMarkersContainer` - zone-label container (MM skips it)
- fields: `_markerPrefabMapping` (+0x48) - MapMarkerPrefabMapping (player marker prefab, drone station prefab)
- fields: `_belowMarkersContainer`, `_staticMarkerContainer` - marker layers of the live page (MT inserts icons before them)
- gotchas: `_projectionHelper` is NOT serialized: null on the prefab; compute from `_anchor1/_anchor2` instead (MM logs "projection from anchors")
- gotchas: RainbowIslandMap anchors give scale (1,1) offset (0,0): one map unit per metre
- gotchas: a prefab read in place has the force-activated containers inactive: treat them as active (30 of 67 images incl. nearly all fog art were otherwise "inactive")
- gotchas: Grey Labyrinth: single flat page, no per-floor fields found
- mod: MM (`ReadProjection`, page copy activeSelf walk), MT (icons container, inverse projection)

### MapUtils (MonomiPark.SlimeRancher.UI.Map), static
- methods: `GetSlopeAndOffset(p1, p2)` 0x72A090 - `slope = (p2.y - p1.y)/(p2.x - p1.x); offset = p2.y - slope*p2.x` (IDA, verified) (new)
- mod: MM reproduces it natively

### MapUI (MonomiPark.SlimeRancher.UI.Map)
- methods: `OpenMap(MapDefinition)` - `MapDirector.GetMapForZone(MapUIUtilities.GetPlayerCurrentZone())` -> `MapViewModel.GetMapPrefabFromDefinition` (MapPrefabMapping) -> instantiate page -> `GetComponent<Map>()` + `GetComponentsInChildren<IFilterableMapMarker>()` -> shows `MapDefinition.MapBackground` (IDA, verified)
- methods: `Awake` 0x710890 - unique body (hookable); once per map UI scene load, not per open (IDA)
- methods: `LateUpdate` 0x711E90 - runs every frame while the full map is shown; uses `-MapViewModel.CursorPosition` as the page-local cursor (feeds `TryFindZoneAt`); calls `MapDirector.IsZoneHidden` (IDA)
- methods: `TryFindZoneAt(point)` - zone under a UI-world point (ZoneDetectionArea._zoneMapData) (dump)
- methods: `get_CurrentMapDefinition`, `MakeMapViewModel` (builds MapViewModel) (dump)
- fields: `_mapPrefabMapping`, `_mapDirector`, `_map` (live page, a Map RectTransform), `_viewModel`, `_fogReveal`, `_inputLegend` (ComponentInterface<IInputLegend>), `_failedPlaceWaypointCue` (UIAudioCueHandle) (dump)
- gotchas: one hook per address: MM hooks Awake, MT hooks LateUpdate (absence for 3 frames = map closed)
- gotchas: close the map like `DroneUIDisplay.CloseThisDisplay`: `UIDisplayManager.Instance.CloseDisplayByObjectInHierarchy(MapUI)`
- methods: `Start` 0x713C30 - `_inputRegistrar.BindCanceled(_closeInput, OnCloseInput)` (and `_toggleFiltersInput`, `_toggleDroneMenuInput`, also on *canceled*): the map closes on the RELEASE of its close input; UIInputFocusRegistrar applies the bindings when it receives focus (IDA)
- methods: `ICancelHandler.OnCancel` 0x714C90 - only closes the filter menu / drone menu when they hold focus, never the map (IDA)
- other: `MapUIRoot.TryOpenMap` 0x729D40 - `_openMapInput` *Performed* (press) -> `UIDisplayManager.OpenDisplay(_mapDisplay, WithData(_mapDirector))`; skipped in the ranch house, while dead, while time fast-forwards (IDA)
- gotchas: round 13/14 "M opens and closes the map": the cause (round 15) was the overlay's input lock. It re-enabled the whole `Paused` map in gameplay, which armed `Paused/CloseMap` (also M), so the opening press started CloseMap and its release closed the map. CloseMap is a Button with no initial state check, so in vanilla the map's own Paused.Enable while M is held never starts it. Fixed in ui/game_input.cpp (re/ui.md "InputDirector"). The map_tools 0.35 s "Open settle" stays as a cheap safeguard
- mod: MM (hook Awake: full-map-open detection, mapping, director), MT (hook LateUpdate; teleport close)

### MapUIRoot (MonomiPark.SlimeRancher.UI.Map)  -- scene object
- fields: `_mapDirector` - director lookup fallback
- fields: `_mapDisplay` `UIDisplayHandleWithData<MapDirector>` = struct { string _name } - the full map's display name; passed by value = the string pointer in rdx (IDA, verified) (new)
- mod: MM (preload display name, director)

### Map UI scene / page prefab lifetime
- chains: full map = UI display loaded from an Addressables SCENE (`UILayerDefinition.LayerEntry._sceneAsset`, scene holds `UIDisplaySceneTag{_uiDisplay, _uiContainer}`); `UIDisplaySceneLoadHelper.Instantiate`: `Scene.GetRootGameObjects` -> `GetComponent<UIDisplaySceneTag>` -> `UIDisplayUtility.InstantiatePrefabs` (IDA)
- chains: MapUI prefab `_mapPrefabMapping` hard-references every page prefab -> mapping + pages are in memory ONLY while that scene is loaded (map open, plus until the `UIDisplayLoadManager` flush: `PendingUnload` + `UnloadUnusedAssets` after close) (IDA + log 2026-10-22)
- gotchas: `Resources.FindObjectsOfTypeAll(MapPrefabMapping)` finds nothing while the scene is unloaded (log: "0 loaded"); page sprites die with it
- gotchas: holding a preload request (re/ui.md UIDisplayManager.RequestPreloadDisplay) keeps the scene loaded without instantiating MapUI (`UIDisplaySceneTag.Awake` is empty)
- gotchas: `MapDirector._viewModel` is null until a full map was used
- mod: MM (map UI preload, requester string "SR2Mods.Minimap.PagePreload")

### MapPrefabMapping (MonomiPark.SlimeRancher.Map)
- methods: `GetPrefabFor(MapDefinition)` -> GameObject page prefab (dump)
- fields: `_entries` List<MappingEntry{Map, Prefab}> - serialized; use if the runtime dictionary is stale (dump)
- gotchas: several mappings may be loaded: try each, keep the one with a page for this map
- mod: MM (`FindPagePrefab`)

### MapDirector (MonomiPark.SlimeRancher.Map) : ScriptableObject
- methods: `IsZoneHidden(ZoneMapData)` 0x9C2030 - `!_unlockedOnMapByDefault && !zone-entered query`; callers only MapUI.LateUpdate, ZoneMarkerUI.LateUpdate, LabyrinthZoneMarkerUI.LateUpdate (zone names / "???") (IDA)
- fields: `_counters` List<ZoneToCounterMapping> -> `MapTreasurePodCounterDefinition.Icon` (MapCounterDefinition.Icon) = treasure pod icon (dump)
- fields: `_viewModel` MapViewModel (set once a full map was used) (dump)
- fields: `Markers` Dictionary<string, Marker>: only gadgets with `GadgetDefinition.DisplayOnMap` (see GadgetModel.SetupMapMarker); values are nested `MapDirector.Marker` (`ShouldDisplay()`, `GetPosition()` Vector3, `_mapMarkerSource`) (IDA)
- chains: Marker source -> `IMapMarkerDescriptorProvider.GetMapMarkerDescriptor` -> for GadgetModel the descriptor = `IdentifiableModel._ident` (IDA, verified)
- gotchas: `Dictionary<string, Marker>.Entry` is 24 bytes (hashCode +0, next +4, key +8, value +16) - see re/unity.md array_element_size gotcha
- mod: MM (`_eventDirector` fog fallback, `_counters`, `_viewModel`; `Markers` resolved for the resolved-line only; old teleporter scan replaced by GameModel.identifiablesByIdent), MT (`IsZoneHidden` hook + direct call past the hook, `GetMapForZone`)

### MapViewModel (MonomiPark.SlimeRancher.UI.Map)
- methods: `CursorPosition` = `_mapAnchor + _cursorOffset / _mapZoom` (page-local cursor is its negation) (IDA)
- methods: `ToggleNavigationMarkerAtCursorPos` 0x72BF80 - waypoint at `-(worldMin + (CursorPosition - mapMin) * k)`; equals the inverse projection only for anchors without offset (IDA)
- methods: `GetMapPrefabFromDefinition` (backed by MapPrefabMapping) (IDA)
- fields: `_prefabMapping` (IMapPrefabMapping), `_availableMaps` (tabs, filled once in the ctor), `_mapAnchor`, `_cursorOffset`, `_mapZoom`, `_mapRect`, `_worldRect`, `_fullMapRect` (dump)
- chains: `_cursorOffset` set by `MapPanZoomPointerControls.OnPointerMove` -> `SetViewportCursorPosition` (IDA)
- chains: inverse projection `map = mapMin + (world - worldMin) * mapSize / worldSize` with `_mapRect`/`_worldRect` (bound to the tab shown)
- mod: MM (`_prefabMapping` fallback), MT (cursor -> world, remembered walls on other tabs, clip to `_fullMapRect`)

### MapListConfiguration (MonomiPark.SlimeRancher.Map)
- methods: `GetAvailableMaps(eventDirector)` 0x9C3110 = `_maps.Where(m => m.MapAvailableCondition.IsSatisfied(ed))`; only caller: MapViewModel ctor (`.ToArray()` -> `_availableMaps`) (IDA)
- methods: lambda `<>c__DisplayClass1_0.<GetAvailableMaps>b__0(MapDefinition)` 0x9D1FC0 - only used there (hookable) (IDA)
- gotchas: tabs are fixed per view model: a hook change shows on the next map open
- mod: MT (hook lambda: Reveal everything lists every tab)

### MapDefinition (MonomiPark.SlimeRancher.Map)
- fields: `MapBackground` ImageSetup{Image, TintColor}, `MapBackgroundOverlay`, `MapAvailableCondition` (offsets above)
- mod: MM (backdrop: screen-fixed like MapUI)

### MapUIUtilities (static)
- methods: `GetPlayerCurrentZone()` (verified IDA); alternative `PlayerDisplayOnMap.GetZoneDefinition()`
- mod: MM, MT

### PlayerDisplayOnMap (global ns) : DisplayOnMap
- methods: `Awake` 0x52D970 - unique body, hooked (IDA)
- methods: `GetCurrentRotation()` 0x52D9F0 = `gameObject.transform.rotation`; identity while in a hidden cell (IDA, verified) (new detail)
- methods: `IsInUnknownArea`, `GetZoneDefinition`
- mod: MM (hook Awake; player transform; hide in unknown area)

### PlayerMapMarker (MonomiPark.SlimeRancher.UI.Map)
- methods: `Rotate` 0x7332A0 - `z = -(eulerY + _rotationOffset)` (constants 57.2958 / 0.0174533 in the binary) (IDA, verified)
- methods: `SetBelowMarkerLayer` - moves `_moveBelowMarkersTransform` (the cone) under the marker layer (dump)
- fields: `_arrowRect` (= FacingFrame, the turning part), `_rotationOffset` (0 in 1.3.0), `_moveBelowMarkersTransform` (= Cone) (dump + UnityPy)
- mod: MM (arrow rotation, game marker copy)

### MapMarkerPrefabMapping (MonomiPark.SlimeRancher.UI.Map)
- fields: `_playerMarkerPrefab`, `_droneStationPrefab` (dump)
- asset: player marker prefab (bundle `duplicateassetisolation_assets__54b00e46….bundle`, UnityPy):
  ```
  PlayerMarker 50x50, pivot (0.5,0.45); PlayerMapMarker{_arrowRect=FacingFrame, _moveBelowMarkersTransform=Cone, _rotationOffset=0}, CanvasGroup 1, Animator "PlayerMarker"
    FacingFrame 50x50 (= _arrowRect)
      Cone  scale 0.7, CanvasGroup, Animator "PlayerMarkerCone"
        Cone  Image fxPlayerMarkerCone 100x100, pivot (0.5,0), y -16
      FacingArrow  Image framePlayerMarker 54x62.75, pivot (0.5,0.43)
    BeaIcon  Image player_face 24x28, y +2, preserveAspect (upright)
  ```
- asset: animator clips (streamed clips decoded; path hashes = CRC32 of paths): root controller loops `ChooseLoop` -> `MapMarkerPulse` (4 s, FacingArrow scale.xy 1,0.75,1,0.75,1 at 0..4 s, per-segment cubics) or by random `LoopChoice` `MapMarkerPulse_Rare` (+ BeaIcon wobble 30/-20/10/-4/1 deg around t=1 s); cone controller loops `MapMarkerPulse_Cone` (4 s, Cone y -16 -> -14 -> -16, alpha 1 -> 0.75 -> 1, smoothstep)
- gotchas: the FIRST Image under `_arrowRect` is the cone, not the arrow
- mod: MM (`LoadGameMarker`)

### DroneStationMapMarker (MonomiPark.SlimeRancher.UI.Map) / DroneTypeDisplayConfiguration (MonomiPark.SlimeRancher.UI.Drone)
- methods: `DroneStationMapMarker.UpdateIcon` 0x704740 - `_image.sprite = _displayConfiguration.PickDroneTypeSprite(model._type)` (IDA)
- methods: `DroneTypeDisplayConfiguration.PickDroneTypeSprite` 0x8247E0 - 0 -> +0x18 (`_rancherDroneIcon`), 1 -> +0x28 (`_explorerDroneIcon`) (IDA)
- fields: `DroneStationMapMarker._displayConfiguration`
- mod: MM (drone port icons via `Map._markerPrefabMapping._droneStationPrefab`)

### MapUIFogReveal (MonomiPark.SlimeRancher.UI.Map)
- methods: `Start` 0x7297C0 - `MapDirector.StealRequestedAnimations(_animatedEvents)`, `GetComponentsInChildren<FogMapElement>`, `RevealKnownZones`, `AnimateReveals` (IDA)
- methods: `RevealKnownZones` 0x729190 - per element: reveal event in `_animatedEvents` -> `PlayReveal` (tween), else `SetRevealState(ShouldBeRevealed(_eventDirector))`; no other state (IDA)
- methods: `RevealZonesInMap(Map)` 0x729650 - run when a page opens (and from Start): collects the page's FogMapElements, calls RevealKnownZones (IDA)
- mod: MM (same query), MT (re-run `RevealZonesInMap(MapUI._map)` on switch change; `_eventDirector`)

### FogMapElement (MonomiPark.SlimeRancher.UI.Map)
- methods: `ShouldBeRevealed(EventDirector)` 0x704B50 = `StaticEventQueryComponent.CreateStaticEventQueryComponent(_fogRevealEvent, 1).IsSatisfied(director)` (vslot 0x1B8) -> `director.GetRecordEntriesForEventType(event)`; allocates a query object per call (GC garbage) (IDA)
- methods: `SetRevealState(bool)` 0x704B20 - CanvasGroup alpha = `!isRevealed` (fog images are overlays shown while NOT revealed); only code caller RevealKnownZones (IDA)
- methods: `PlayReveal` - tweens its CanvasGroup 1 -> 0 (the fog shader turns that into a hole growing from the centre, see fog shaders)
- fields: `_fogRevealEvent` StaticGameEvent, `_canvasGroup`
- gotchas: pure query on serialized data + EventDirector: safe to call on the prefab component
- gotchas: zone links (`zone_links/zone_link_*`: strandmain, gorgemain, bluffsmain, strandrear, gorgerear, bluffsrear, labyrinth*) ALSO carry FogMapElement; their graphics are a dotted portal line (`PortalLineSpline/PortalLineTest`, custom Graphic, no sprite, default UI material) and portal icons, not clouds. Test only fog-shader art (`FogOfWar` in the shader name)
- mod: MM (fog groups), MT (hook SetRevealState: Reveal everything; `FogAt` click test)

### MapNodeActivator
- methods: `Activate` 0x70D790 - ends with `_fogRevealEvent.RaiseEvent()` (map data node reveal) (IDA)
- mod: none (explains reveal source)

### DynamicMapElement / IFilterableMapMarker and marker classes (MonomiPark.SlimeRancher.UI.Map)
- methods: `DynamicMapElement.get_CanBeShown` (pure query)
- methods: `IFilterableMapMarker.get_CanBeShown`, `IMapMarkerDescriptorProvider.GetMapMarkerDescriptor`, `IMapMarkerDescriptor.get_MapIcon` - interface methods; resolve via `il2cpp_object_get_virtual_method`
- methods: `GordoMarkerUI.CanBeShown` 0x704ED0 = seen && !popped (via GameModel) (IDA)
- methods: `PuzzleSlotMarkerUI.CanBeShown` 0x734730 = discovered && !solved; `CalculateIsSolved()` GameModel query (IDA)
- methods: `AncientTeleporterMarkerUI.CanBeShown` = discovery only; `StaticMapMarker` -> its DynamicMapElement (IDA)
- fields: `MapMarker._image`, `GordoMarkerUI._iconImage`, `GordoMarkerUI._gordoIdentifier` (string)
- gotchas: these CanBeShown read only serialized data / GameModel / EventDirector; queries are created lazily on the prefab, so evaluating on the PREFAB component works (verified in-game via log)
- gotchas: `MapMarkerBase` subtrees that are not IFilterableMapMarker = player / navigation marker art
- mod: MM (page markers, done state with fog ignored), MT (undiscovered-marker icons via api)

### MapClickPlaceMarker (MonomiPark.SlimeRancher.UI.Map)
- methods: `IPointerClickHandler.OnPointerClick` 0x706AB0 (interface-named) - map surface click; left = toggle waypoint, other buttons nothing (IDA)
- fields: `_isDragging`; `PointerEventData.<button>k__BackingField` (1 = right)
- mod: MT (hook: right-click teleport flag)

### ZoneDetectionArea / ZoneMarkerUI
- fields: `ZoneDetectionArea._zoneMapData` (ZoneMapData of the clicked area)
- mod: MT

### NegativeMaskMap (MonomiPark.SlimeRancher)  -- outside fog
- methods: `OnEnable` 0x6346A0 - caches `GetComponentInParent<Canvas>().transform`, registers `UpdateBeforeRender` on `Canvas.willRenderCanvases` (IDA)
- methods: `UpdateBeforeRender` 0x6347A0 (only with `updateConstantly`) - `M = transform.worldToLocalMatrix * canvasTransform.localToWorldMatrix`; `M[3]=maskEdgeNoiseScale, M[7]=maskEdgeNoiseStrength, M[11]=softness, M[15]=roundness`; `Shader.SetGlobalMatrix(_NegativeMask, M)` (IDA)
- fields: `softness`, `roundness`, `maskEdgeNoiseScale`, `maskEdgeNoiseStrength`, `updateConstantly`
- gotchas: `_NegativeMask` is a GLOBAL written only while the full map's mask is enabled: drawing the outside-fog image elsewhere uses whatever the full map wrote last
- mod: MM (own NegativeMask transform, writes the global itself while the full map is closed)

### Map page prefab assets (bundle `duplicateassetisolation_assets__54b00e46….bundle`, UnityPy, 1.3.0)
- RainbowIslandMap:
  ```
  RainbowIslandMap (Map)
    zone_fog_areas           _forceActiveOnAwake[0]; 14 x map_fog_* (Image + CanvasGroup + FogMapElement)
    fog_static               _forceActiveOnAwake[1]
      Temporary Static Fog Covering
        11 x "covering up static fog (n)"   Image only (one inactive), material PosterizedFogOfWarMask
      OutsideFogNegativeMasked              Image, PosterizedFogOfWarNegativeMask material
      Negative Mask                         NegativeMaskMap (softness 0.236, roundness 0.79, noise 5.14 / 0.88, updateConstantly)
    zone_links/zone_link_*   FogMapElement + portal line/icons (not clouds)
  ```
- only RainbowIslandMap has `fog_static`; covers are unreferenced (no serialized ref, no string literal) and never hidden by any reveal: full map always draws them; colours repeat neighbouring fog areas (rg = noise UV offset)
- fog sprites are SDF (`map_fog_fields1_sdf`, `map_fog_strand1_sdf`, 256 px, not CPU-readable); image rects are far larger than the clouds (`map_fog_fields1` rect 984 x 984 page units)
- 20 fog areas on the island page in the logs; 17 teleporters
- mod: MM (static fog group), MT (fog click test, static fog disable)

### Fog shaders (bundle `shaders_assets__b2000203….bundle`, DXBC via D3DDisassemble)
- shaders: `Custom/PosterizedFogOfWarMask` (FogMapElement art + static covers), `…Clouds`, `…NegativeMask` (outside fog)
- vertex: `COLOR = vertex colour * _Color`; vertex colour already includes every parent CanvasGroup alpha
- mask pixel: `a = round(COLOR.a*255)/255`; `radial = saturate(smoothstep(dist_from_centre / (1 - smoothstep((a+0.5)*2/3))) + a)` multiplies the SDF before posterize: a = 1 full shape, below ~0.8 a hole opens from the centre (= reveal animation). Output alpha only `1 - (1-a)^4` -> alpha cannot fade fog; a material/_Color alpha shrinks it too
- `COLOR.rg` = noise UV offset (why each fog image has an odd colour)
- cbuffer: `_EdgeSoftness, _NoiseIntensity, _NoiseScale, _NoiseSpeed, _ilmin, _ilmax, _PosterBiasPower, _PosterSteps, _OutlineColor, _OutlineOpacity, _OpacityLevel, _OpacityMinimumBoost`; t0 `_Noise`, t1 sprite
- threshold: n = two animated noise samples mapped to -2..2; `G = s^2 - s^2(n+s) + s(n+s)`; `x = sat(G(NoiseIntensity*n - ilmin)/(ilmax - ilmin))`; level = `pow(x, PosterBiasPower) * PosterSteps`; step `floor(level+0.5)` = 0 (no cloud) below level 0.5; alpha = `1 - (1 - smoothstep(step/steps))^OpacityMinimumBoost` (0.4 at step 1)
- material `FogOfWar`: `_NoiseIntensity` 1, `_ilmin` -6.81, `_ilmax` 0.71, `_PosterSteps` 5.5, `_PosterBiasPower` 1 -> with n = 0 a cloud where s >= ~0.24 (edge moves s 0.1-0.4 for n = +-0.5)
- NegativeMask: position through M into mask space; rounded box (|2p| < 1, roundness, softness, edge noise) cut OUT of the fog
- mod: MM (fog copies get CanvasGroup.ignoreParentGroups), MT (`FogAt` SDF read-back test)

### TeleportNetwork (MonomiPark.SlimeRancher.World.Teleportation)
- methods: `Teleport(player, source)` - finds TeleporterModel, calls TeleportToDestinationImpl (dump)
- methods: `TeleportToDestinationImpl(player, destinationNodeId, destinationSceneGroup, forceSceneTransition, isBoatRide)` 0x65CD90 (5 args) - node registered and not forced: in-scene `BeforeArrive`, `TeleportTo`, `OnArrive` (instant, no fade); else `SceneLoader.LoadSceneGroup(destGroup, {TeleportPlayer, PauseGame, delegates for position/rotation/arrive FX})`, placed at `_nodeIdToTeleporterNodeDict[id]` after load (IDA)
- methods: `Teleport_Death(player, _delayTime, cb)` (3), `Teleport_ResetPlayer` (2) - see re/player.md knockout flow
- fields: `_networkDefinition` StaticTeleportNetworkDefinition = `ListAsset<TeleportSystemDefinition>.items`; each `DestinationNodes` entry `{DestinationNode (TeleporterNodeDefinition, get_NodeId), DestinationSceneGroup}`; `_nodeIdToTeleporterNodeDict`
- gotchas: unique bodies (one method per address)
- gotchas: `*OneWay*` nodes (e.g. `TeleporterOneWayLabyrinthHub`) put the player back in the start zone; avoid as arrival
- gotchas: the home/ranch has no teleporter node in the network
- mod: QT (wrap TeleportToDestinationImpl, Teleport_Death, Teleport_ResetPlayer), MT (calls TeleportToDestinationImpl for cross-zone travel)

### TeleporterNode
- methods: `OnTriggerEnter` - player main collider, no load in progress, `CanTeleport()` (no unstable object in the tank) -> `TeleportNetwork.Teleport`; no delay (dump/IDA)
- fields: TeleporterModel `ForceSceneTransition`, node `IsBoatRide`
- gotchas: `ForceSceneTransition` teleporters load even within one zone
- mod: QT (reference)

### ZoneSceneGroupMapping
- fields: `_mappings` List<ZoneMapping> struct `{ZoneDefinition Zone, SceneGroup[] SceneGroups, SceneGroupList SceneGroupListAsset}`; `SceneGroupList` : ListAsset<SceneGroup> (`items`)
- methods: `TryGetValue(group)` - answers only the FIRST zone of a group
- gotchas: The Conservatory (ranch) and Rainbow Fields share scene group `ConservatoryFields`; the Conservatory has NO mapping entry -> use `ZoneDefinition._isRanchZone` -> `SceneLoader.DefaultGameplaySceneGroup`
- gotchas: the mapping asset is loaded while the map is open (the map UI's drone menu references it); found with `Resources.FindObjectsOfTypeAll`
- mod: MT (cross-zone route)

## From the slime_dj, spawn_scaling, hl2_movement, quick_start, free_placement, misc_tweaks, plot_upgrades, drone_price_limit notes

### PlayerZoneTracker / ZoneDefinition
- `SceneContext.PlayerZoneTracker.GetCurrentZone()`, else `GetLatestZone()` (last non-null zone) (dump)
- `ZoneDefinition._localizedName.GetLocalizedString()` (rich text in it), `_nameKey`, `_richPresenceId` (dump)
- gotchas: ranch detection by `_nameKey` / `_richPresenceId` containing "ranch" or "conservatory" is a guess (not verified)
- mod: discord_rpc

## From the sandbox, sell_upgrades, collectibles, instant_build notes

### MapDirector  [SceneContext.MapDirector, ScriptableObject]
- fields: `_navigationMarkerData` (`MapNavigationMarkerData`: `Optional<Vector3> _markerPosition`, `MapDefinition _onMap`, `SceneGroup _firstSceneGroup`), `_centerPlayerInZones`, `_counters` (`List<ZoneToCounterMapping{Zone, Counter}>`, col), `_eventDirector` (col)
- methods: `SetPlayerNavigationMarker(Vector3, map, 16)`; `TryGetPlayerNavigationMarkerLocation(out Vector3, SceneGroup, ZoneDefinition)` 0x9C2AC0 - returns marker when set, its map's `RelatedScenes` contain the scene group, and zone not in `_centerPlayerInZones` (IDA)
- chains: `MapViewModel.PlaceNavigationMarker(Vector2 ui)` -> `SetPlayerNavigationMarker(new Vector3(-ui.x, 0, -ui.y), currentMap, 16)`: stored position has y = 0; `NavigationMarkerInWorldController.TurnBeamOn` at that position
- gotchas: marker not saved (no model field seen)
- mod: sb player_extras.cpp teleport to marker (with `RegionRegistry.CurrentSceneGroup`, `PlayerZoneTracker.GetCurrentZone()`; ground teleport in modules/map_tools/ground_teleport.h, `Region.AddNonProxiedReference`, `TeleportablePlayer.TeleportTo`)

### Map counters / markers  [col]
- methods: virtual `MapCounterDefinition.GetState()` -> `MapCounterState{Current, Maximum, Image}`; counters hold static ID lists (totals include unvisited areas)
- classes: map page prefab baked markers: `GordoMarkerUI._zone`, `ZoneDetectionArea` (`_points` local space; `ContainsPoint` 0x739800 = InverseTransformPoint(_transform) + point-in-polygon), zone label markers, `IFilterableMapMarker.CanBeShown` (discovery query), `CalculateIsSolved()`
- classes: `FogMapElement` (one fog area per reveal event; `ShouldBeRevealed(EventDirector)`); `MapNodeActivator.Activate -> _fogRevealEvent.RaiseEvent`; use the director whose record is the loaded save's (`GameModel.eventRecordModel`)
- mod: col

### ZoneDefinition
- fields: `_localizedName`, `_nameKey`, `_crates` (`SpawnableCrate[]`, nested class: `SpawnableCrate.Type` offset from live element)
- facts: English zone names (`Pedia_en`): `t.fields` Rainbow Fields, `t.gorge` Ember Valley, `t.strand` Starlight Strand, `t.bluffs` Powderfall Bluffs, `t.labyrinth` The Grey Labyrinth; no string entries for Labyrinth sub-areas
- gotchas: `FindObjectsOfTypeAll(ZoneDefinition)` is not free; find once
- mod: sb EnsureCrateAreas, weather zone names, sale_notify template
