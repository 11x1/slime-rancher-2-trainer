# minimap module notes

Game 1.3.0 (Unity 6000.0.73f1, IL2CPP v31). Everything is resolved by name at runtime. RVAs and offsets in this file come from the offline dump and capstone disassembly of `re/input/GameAssembly.dll`, and are here only so the analysis can be re-checked after an update. The code never uses them.

Files:

- `module.cpp`: lifecycle, hooks, tick, the time-sliced page copy, markers, layout.
- `game.h` / `game.cpp`: game-side symbols (Assembly-CSharp).
- `ugui.h` / `ugui.cpp`: Unity / uGUI / TMP bindings, plus:
  - GC-handle `Ref` with a freeze switch;
  - pinned-array iteration;
  - Dictionary and List<struct> walkers;
  - a native-fault guard that reports module+offset.
- `procgen.h`: procedural RGBA images (mask shapes, fallback frame, arrow, dot).

## Approach (current design)

The full map (`MapUI.OpenMap`, verified by disassembly) does this:

1. `MapDirector.GetMapForZone(MapUIUtilities.GetPlayerCurrentZone())` returns a `MapDefinition`.
2. `MapViewModel.GetMapPrefabFromDefinition` (backed by `MapPrefabMapping`) returns the page prefab.
3. It instantiates the page, then calls `GetComponent<Map>()` and `GetComponentsInChildren<IFilterableMapMarker>()`.
4. It shows the background `MapDefinition.MapBackground`.

The minimap never instantiates game prefabs. The page prefab is read in place, and its content is copied into our own uGUI objects:

- Every visible `Image` and `RawImage` in the prefab is copied into our own `Image` / `RawImage`. The copy carries:
  - sprite or texture + uvRect;
  - a custom material if its shader has `_Stencil` (so the minimap mask still clips it); a non-stencil custom material on a sprite-less Image becomes a RawImage of the material's main texture (+ its offset/scale); otherwise the material is dropped;
  - colour × the alpha of any `CanvasGroup` above it;
  - Image type + pixelsPerUnitMultiplier;
  - its four rect corners, converted into the page's local space with `TransformPoint` / `InverseTransformPoint`. They set the size, position, rotation and mirroring of our copy.
- Skipped (counted per reason in the "page copied" line):
  - inactive subtrees (activeSelf walk up to the page root);
  - disabled Graphic components (`Behaviour.enabled == false`, invisible in game);
  - the Image of a `Mask` with `showMaskGraphic == false` (a stencil shape, invisible in game);
  - sprite-less Images without a texture source outside fog/dynamic groups (white placeholders / raycast blockers; listed in "page placeholders skipped");
  - the zone-label container (`Map._zoneMarkersContainer`) and every `MapMarkerBase` subtree that is not an `IFilterableMapMarker` (player / navigation marker art), plus any image using the player-marker sprite;
  - fully transparent images (colour alpha x CanvasGroup alpha).
- Point-filtered map textures (FilterMode.Point) are switched to bilinear (counted in the "page activation" line).
- Images under a `FogMapElement` or a `DynamicMapElement` become groups that we show or hide with the game's own pure queries:
  - `FogMapElement.ShouldBeRevealed(EventDirector)`, only once the save's event record is loaded (see "Fog of war state" below);
  - `DynamicMapElement.get_CanBeShown`.
- `IFilterableMapMarker` components become our own icon Images. The icon is the sprite of `MapMarker._image` or `GordoMarkerUI._iconImage`, otherwise the first sprite in the marker's subtree.
- The copy is time-sliced: 32 graphics per frame, with the minimap hidden until the copy is done.

### Where the page prefab comes from (map UI preload)

The full map is a UI display loaded from an Addressables SCENE (`UILayerDefinition.LayerEntry._sceneAsset`, a "soft reference" scene with a `UIDisplaySceneTag{_uiDisplay, _uiContainer}`). `UIDisplaySceneLoadHelper.Instantiate` (disassembly) does `Scene.GetRootGameObjects` -> `GetComponent<UIDisplaySceneTag>` -> `UIDisplayUtility.InstantiatePrefabs`. The MapUI prefab holds `_mapPrefabMapping`, which hard-references every page prefab. So the mapping and the pages are loaded ONLY while that scene is loaded: while the full map is open, plus until the `UIDisplayLoadManager` flush unloads it after the map closes (`PendingUnload` + `UnloadUnusedAssets`). That is item 1 of the 2026-10-22 evening test (see below).

The minimap now holds the scene itself, with the game's own preload API (used by `UIDisplayPreloadOnDisplayOpenClose`):

- `UIDisplayManager.Instance.RequestPreloadDisplay(UIDisplayHandle{name}, object requester)` (0x7A5B10): `FindDisplayLayerEntry(name)`, requires a valid `_sceneAsset` key, `UIDisplayLoadManager.GetOrCreateLoadEntry(sceneAsset)`, `entry.AddPreloadRequester(requester)` (a `HashSet<object>`, types.cs), queues the load, returns the `UIDisplaySceneLoadEntry`. Idempotent for the same requester.
- The display name is `MapUIRoot._mapDisplay._name` (`UIDisplayHandleWithData<MapDirector>` = struct { string _name }; passed by value = the string pointer in rdx, verified in the disassembly). MapUIRoot is a scene object (one lookup, then cached).
- The requester is a managed string "SR2Mods.Minimap.PagePreload" (HashSet<object> uses string equality, so a new string with the same text releases it again).
- While any requester holds the entry, the scene stays loaded: no MapUI is instantiated (the tag only references prefabs; `UIDisplaySceneTag.Awake` is empty), but the mapping + page prefabs are in memory.
- `UIDisplaySceneLoadEntry.get_IsDone` (0x7B1370) is polled at 10 Hz until true; then one `Resources.FindObjectsOfTypeAll(MapPrefabMapping)` scan (~25 ms, once) finds the mapping.
- Re-requested every 10 s (cheap, idempotent) and after every world reset (the load manager may drop requests on a scene change). Released (`ReleasePreloadDisplay`) when the module is disabled.
- No asset scan while the preload is loading (up to 30 s), so the old 25 ms scan every 15 s is gone.

Map page prefab source (`FindPagePrefab`), first hit wins, the page must have a `Map` component:

1. the mapping we hold (from `MapUI.Awake` or an earlier scan): `GetPrefabFor(mapDef)`, then its serialized `_entries` (`List<MappingEntry{Map, Prefab}>`) in case the runtime dictionary is stale;
2. `MapDirector._viewModel._prefabMapping` (exists once any full map was used);
3. asset scan `Resources.FindObjectsOfTypeAll(MapPrefabMapping)` (finds the preloaded one): EVERY loaded mapping is tried and the one that has a page for this map is kept. Run right after the preload is done, otherwise every 15 s while missing (6 tries), then every 60 s; the MapUI hook still takes over when the full map opens.

The build is re-tried every second while there is no page; the error line names the state ("map UI preload not requested yet" / "map UI scene still loading" / "map UI scene loaded but no MapPrefabMapping has this map" / "map UI preload unavailable").

### Map.Awake activates disabled containers

`Map._forceActiveOnAwake` (GameObject[], "containers temporarily disabled to see under them") is `SetActive(true)` in `Map.Awake` (0x715790, verified). The copy used to treat them as inactive: 30 of 67 page images were skipped as "inactive", including the art of almost every `FogMapElement` (only 1 of 20 fog areas had art), so fog areas had no shape and icons were never "under fog" (item 4). The activeSelf walk now treats those GameObjects as active; logged in "page activation: N images copied from containers Map.Awake activates".

UI hierarchy: under the HudUI GameObject (it carries the HUD Canvas; the minimap then shows and hides with the HUD), or the root canvas above it, or an own overlay canvas after 10 s:

```
SR2Mods.Minimap        corner-anchored RectTransform, nested Canvas, CanvasGroup (opacity, no raycasts)
  Mask                 Image (procedural disc / 9-sliced rounded square) + Mask, inset by round(1.5% size)+1 px
                       so the stencil edge lies inside the frame's opaque band (no map pixels through the
                       frame's anti-aliased outer edge = no white fringe)
    Background         MapDefinition.MapBackground sprite + TintColor (screen-fixed, like MapUI)
    BackgroundOverlay  MapDefinition.MapBackgroundOverlay
    Rotator            z rotation for "rotate with player"
      Page             our copy of the page; local space == prefab page space
        Art / fog / dynamic copies, Marker icons, Icon (pods / nodes / teleporter gadgets)
  Arrow/PlayerArrow    Image, always centred (GTA style): the game's player-marker sprite pivoted on the
                       sprite's own pivot, or the classic procedural arrow (setting arrow_style)
  Frame                procedural ring or a configured game sprite
  Level                TMP label, Grey Labyrinth only
```

## Map position (verified)

The math below comes from `Map.GetMapPosition(Vector3)` (0x715C30), `Map.SetupInitialValues` (0x715FA0) and `MapUtils.GetSlopeAndOffset` (0x72A090):

```
(slope.x, offset.x) = GetSlopeAndOffset((_anchor1.WorldPosition.x, _anchor1.MapPosition.x),
                                        (_anchor2.WorldPosition.x, _anchor2.MapPosition.x))
(slope.y, offset.y) = same with .y            (WorldPosition.y is world Z)
GetSlopeAndOffset(p1, p2): slope = (p2.y - p1.y) / (p2.x - p1.x); offset = p2.y - slope * p2.x
GetMapPosition(world) = clamp((world.x*slope.x + offset.x, world.z*slope.y + offset.y), rect) , z = world.y
```

How the minimap uses it:

1. It computes scale and offset from the prefab `Map`'s serialized `_anchor1/_anchor2` (logged: "projection from anchors: ..."). `_projectionHelper` is not serialized (null on the prefab) and is only a fallback for degenerate anchors. RainbowIslandMap's anchors give scale (1,1) offset (0,0): one map unit per metre.
2. It applies the same formula and clamp to the `PlayerDisplayOnMap` transform position.
3. It sets `Page.localScale = s` and `Page.localPosition = -s*p`.
4. `s = (size/2) / ((160 m / zoom) * avg|scale|)`.

The arrow follows `PlayerMapMarker.Rotate` (0x7332A0): `z = -(eulerY + _rotationOffset)` (constants 57.2958 / 0.0174533 read from the binary), where the rotation is `PlayerDisplayOnMap.GetCurrentRotation()` (0x52D9F0) = `gameObject.transform.rotation` (identity while in a hidden cell).

- The arrow never moves: the page moves (and in rotate mode turns) under it.
- `arrowZ = rot - yaw + styleBase + arrow_rotation_offset`; styleBase = `-_rotationOffset` for the game marker, `+180` for the classic arrow (its texture points up, the game's marker sprite was observed 180 deg off), so the default offset 180 is right for both styles.
- Rotate-with-player mode: the Rotator turns by `+yaw` (rot == yaw), so the arrow points up.
- Icons are counter-rotated and counter-scaled, so they stay upright and keep a fixed screen size.

## Object sources (no periodic scene scans)

Hooks (optional) record raw `this` only:

- `MapUI.Awake`: "full map open" detection; its `_mapPrefabMapping` and `_mapDirector`.
- `HudUI.Awake`.
- `PlayerDisplayOnMap.Awake`.
- `TreasurePod.Awake` / `OnDestroy`: queues that feed the pod registry.

Fallbacks:

- HudUI, player, SceneLoader: `Object.FindFirstObjectByType(Type, FindObjectsInactive)`.
  - At most every 5 s, and only while missing.
  - HudUI is searched including inactive objects, because `HudUI.UpdateHudVisibility` (0x6997D0) deactivates its own GameObject while the HUD is hidden.
- MapDirector: `MapUIRoot._mapDirector` (found the same way) or `MapUI._mapDirector`.
- MapPrefabMapping:
  - from `MapUI._mapPrefabMapping` when a MapUI awakes;
  - otherwise the map UI preload + one asset scan when it is loaded, see "Where the page prefab comes from" above (fallback: view model, asset scan every 15 s while missing, 60 s after 6 misses; the scan time is logged).
- Pods that woke before the module was enabled: one `FindObjectsByType` per enable.

Other sources:

- Resource nodes: `ResourceNodeDirector.get_AllResourceDirectors()` → `_nodeSpawners` → `ResourceNodeSpawner._model.nodeState == READY` + a live, active `_attachedResourceNode` + non-empty `resourcesToSpawn`; placed at the node object.
  - Icon: `resourcesToSpawn[0].icon`.
- (Superseded by the placed-gadget scan below) Teleporter gadgets: `MapDirector.Markers`, a `Dictionary<string, Marker>` walked by field name.
  - Source → `GetMapMarkerDescriptor` → `IdentifiableModel._ident`, whose asset name must contain "teleport".
  - Placed only if the unclamped projection lies on this page.
- Placed gadgets (all, incl. teleporters and drone ports): `GameModel.identifiablesByIdent` (replaces the old `MapDirector.Markers` teleporter-gadget scan, which only held `DisplayOnMap` gadgets). Background: `GadgetModel.SetupMapMarker` (0xA915A0) registers a gadget when `GadgetDefinition.DisplayOnMap` (+0x10A); the source is the `DroneStationGadgetModel` itself (class check). The full map's `DroneStationMapMarker.UpdateIcon` (0x704740) sets `_image` to `DroneTypeDisplayConfiguration.PickDroneTypeSprite(_type)` (0x8247E0: 0 -> +0x18 rancher, 1 -> +0x28 explorer).
- Treasure pod icon: `MapDirector._counters` (`List<ZoneToCounterMapping>`) → `MapTreasurePodCounterDefinition.Icon`.

## Update rates

| What | Rate / rule |
|---|---|
| Transform | per frame: player position + rotation, page scale/position, arrow. Rotator / icon updates only when the rotation or scale changes. |
| Zone, hide rules, unknown area, marker visibility | 10 Hz |
| `CanBeShown` per marker | re-queried every 2 s, cached in between |
| Fog | every 2 s: unrevealed + unknown areas are queried (reveals are permanent within a save). Every area: after the build, when the save data becomes ready, on a zone change, after the full map closes, when the setting changes, and every 30 s. Readiness polled at 10 Hz only until settled. Our objects are touched only when an area's visibility changes. |
| Dynamic elements | every 10 s |
| Pods / nodes / gadgets | every 3 s, from the sources above |
| Page copy | once per map change (or after the build fails), 32 graphics per frame |
| `CanBeShown` per page marker | round robin, max 8 calls per 10 Hz tick, each marker every 2 s; hidden until its first answer |
| World reset | on scene-load start, a new / destroyed `PlayerDisplayOnMap`, or the page prefab / MapDefinition no longer alive |
| Map UI preload | requested on the first tick and after each world reset, re-asserted every 10 s; `IsDone` polled at 10 Hz only while loading |
| Zoom hotkey | per frame `GetAsyncKeyState`, skipped while the settings overlay is open |

## Fault policy

- Every game call made by the tick runs under `core::Try` + `ugui::GuardNative`.
- Managed exceptions from per-element queries are contained.
- Any native fault, in the tick, a teardown step or a hook, triggers a session shutdown:
  - the module freezes itself (`ugui::FreezeRuntime`);
  - every `Ref` drops its object without calling IL2CPP (handles are leaked);
  - the frame callback is removed;
  - hooks become pure pass-throughs;
  - no teardown calls into Unity are made.

  The log line is `minimap: <step>: native exception … at <module>+0x<offset> … - minimap disabled for the rest of this session`.
- Each detour calls the original first (unguarded). It then records `self` inside a `core::DetourGuard` and never frees or creates GC handles.
  - A fault inside a guard marks the module faulted: all guards pass through and core disables the hooks.
  - The next tick freezes the module.
  - The module's own `Die()` also calls `core::MarkModuleFaulted`, so ModuleManager shows the minimap as FAILED.
- If core's GC-handle self-test fails, `NewGCHandle` returns nullptr. `Ref` then never keeps an unpinned pointer, and the minimap simply finds nothing.
- Teardown (normal disable) destroys only our own objects and releases each reference in its own guarded step.

## Visibility rules (fog vs. done state)

- Fog areas: each `FogMapElement` group keeps one page-space parallelogram per copied image (its four rect corners), or the element's own RectTransform when it has no art. An icon (page marker, pod, node, teleporter gadget) is "under fog" when its page position lies in a shape of an area that is UNREVEALED or still UNKNOWN. Logged once per build: "fog: N areas (A shaped by their art, R by their own rect, E without an area); X revealed, Y unrevealed, U unknown".
- "Respect fog of war" ON: page markers need the game's `CanBeShown` (discovery + not done) and must not be under fog; world-scanned icons must not be under fog. The minimap hides while `PlayerDisplayOnMap.IsInUnknownArea`.
- "Respect fog of war" OFF: fog art (fog areas and the static fog) is hidden and no icon is hidden by fog or discovery, but the DONE state still applies:
  - gordos: `SceneContextUtility.TryGetGameModel` -> `GameModel.GetGordoModel(_gordoIdentifier)` (TryGetValue, null if never fed) -> `GordoModel.HasPopped()` (eat count == -1, disassembly). `GordoMarkerUI.CanBeShown` = seen && !popped (0x704ED0), so it cannot be used without the fog.
  - plort doors: `PuzzleSlotMarkerUI.CalculateIsSolved()` (`CanBeShown` = discovered && !solved, 0x734730).
  - ancient teleporters: always (their `CanBeShown` is discovery only).
  - other markers (StaticMapMarker -> DynamicMapElement): their `CanBeShown` (a world state).
  - treasure pods: only LOCKED (`TreasurePod.CurrState == 0`) in both modes.
  - If a done state cannot be read, the game's `CanBeShown` is used.
- Resource nodes: READY spawner model (`nodeState == 1`) AND the spawner's `_attachedResourceNode` is an alive, active `ResourceNode` AND `resourcesToSpawn` is not empty; the icon sits at the node's transform. A READY model without a node object (empty spawn point / unloaded cell) is not shown (item 6). Logged once per build: "icons (first scan): ... resource nodes R READY spawners, L with a live node, S shown".

## In-game test 2026-10-22 evening (build 6d8bdf2c)

Log evidence (sr2mods.log 19:53-20:09):

- Every asset scan: "MapPrefabMapping asset scan: 0 loaded (-)", at 15 s / 60 s intervals, before and after every world reset. The view-model path never hit (`MapDirector._viewModel` is null until a full map was used).
- The page was only built right after a full map open (MapUI.Awake capture), e.g. 19:53:45, 19:57:34, 20:06:20.
- "map page prefab unloaded - rebuilding the copy" followed within 1-5 s or minutes, each time on a GC/flush hitch (19:57:29, 19:57:34, 20:06:21): the `UIDisplayLoadManager` flush unloaded the map UI scene after the map closed, destroying the prefab (and its sprites), and nothing could load it again.
- After the scene load at 20:06:12 (world reset #1, zone change) the first map open built the page at 20:06:20 and lost it at 20:06:21 (the flush), the second open (20:06:22) kept it long enough - "open the map twice".

Root cause (item 1): the page prefab lives in the map UI's Addressables scene; nothing but the open full map kept that scene loaded, and `Resources.FindObjectsOfTypeAll` cannot find an asset that is not loaded. Fix: the map UI preload (above).

Other items of that test: fog areas without shapes (`_forceActiveOnAwake`, above); fog-off showed popped gordos (marker queries were skipped entirely when the fog was ignored); resource nodes from READY models without a node object; arrow style / size; mask edge quality (below).

## Fog of war state (fix after the 2026-10-22 night test, build 313a2760)

User reports: explored save: fog over explored areas on the first load, right after opening the full map once; new save on the starting island: NO fog; unmapped Powderfall Bluffs: fog right.

What the game does (disassembly):

- `MapUIFogReveal.Start` (0x7297C0): `MapDirector.StealRequestedAnimations(_animatedEvents)`, `GetComponentsInChildren<FogMapElement>`, `RevealKnownZones`, `AnimateReveals`.
- `RevealKnownZones` (0x729190): per element, if its `_fogRevealEvent` is in `_animatedEvents` it goes to `PlayReveal` (the tween), else `SetRevealState(ShouldBeRevealed(_eventDirector))`. No other state is involved.
- `ShouldBeRevealed` (0x704B50) = `StaticEventQueryComponent.CreateStaticEventQueryComponent(_fogRevealEvent, 1).IsSatisfied(director)` (vslot 0x1B8) -> `director.GetRecordEntriesForEventType(event)` = `_model.GetEventEntries(event)`, or null when `_model` is null (0xA71860).
- `MapNodeActivator.Activate` (0x70D790) ends with `_fogRevealEvent.RaiseEvent()` = `StaticGameEvent._eventDirector.RaiseEvent(this, 1)` (0xA854B0): the reveal is written into THAT director's `_model`.
- `EventDirector.SetModel` (0xA71DC0) stores `_model` and subscribes `SceneContext.onSessionEnded` -> `_model = null` (0x674540). `Init(GameModel)` registers it as a participant of the save's `GameModel.eventRecordModel`.

Root causes in our code:

1. The reveal state was cached as "revealed" whenever the fog was not queried: with "Respect fog of war" OFF every area was stored as revealed, and the 2 s update skipped revealed areas forever. Turning the setting ON afterwards never fogged anything. The log shows exactly this for the new save (20:39:41 "20 revealed, 0 unrevealed (fog of war ignored by setting)", setting turned on at 20:40:46 with only 3/17 teleporters discovered) and for the first explored-save session (20:32 / 20:33).
2. A failed query counted as revealed ("unknown -> reveal") and was then permanent.
3. No readiness check: the first query ran at the page build, whatever `EventDirector._model` was at that moment (null between sessions, or the previous save's record while a save loads), and the answer was kept. Answers taken from another save's record are wrong in both directions; "revealed" ones were never corrected.
4. The director was always `MapDirector._eventDirector`; the reveals are recorded in `StaticGameEvent._eventDirector`'s model (normally the same asset, not checked).

Fix:

- Per area: `state` = the game's answer (1 revealed / 0 unrevealed / -1 UNKNOWN), kept separate from the drawn state (`vis`). Unknown counts as unrevealed everywhere (fog drawn, icons inside hidden). Failed queries leave the area unknown. The setting only changes what is drawn; turning it on re-queries every area.
- Reveal source: `ShouldBeRevealed(director)`, director = the reveal event's own `StaticGameEvent._eventDirector`, else `MapDirector._eventDirector`, the first whose `_model` IS `GameModel.eventRecordModel` (from `SceneContextUtility.TryGetGameModel`).
- Readiness: GameModel present, its record set, the director's `_model` == that record, stable for 1 s. Until then every area is unknown = fogged. If a director has a record that never matches GameModel's, it is used after 10 s with a warning. Polled at 10 Hz from the first tick after a world reset until settled, then checked with every fog evaluation; a changed record (save / session change) makes every area unknown again.
- Cleared on every world reset (scene load, new player object), on a page rebuild, and on a zone change (all areas re-queried in the same tick, so no flash when ready).
- Cadence: unrevealed + unknown every 2 s; every area after the build / readiness / zone change / full map close / setting change and every 30 s.
- Drawing: `SetActive` per area (fully on or off). The user opacity (root CanvasGroup) does not reach the fog: see "Fog shaders and minimap opacity".

New log lines:

- `minimap: resolved (fog yes, fog readiness EventDirector._model == GameModel record, ...)`
- `minimap: fog: waiting for the save's map data: <reason> - every area fogged (unknown) until then` (reasons: no GameModel / GameModel has no event record yet / EventDirector._model is null / is not the loaded save's event record / no EventDirector)
- `minimap: fog: reveal source FogMapElement.ShouldBeRevealed(EventDirector '<name>' from <field>[, same instance as / NOT MapDirector._eventDirector's instance]) = the full map's MapUIFogReveal.RevealKnownZones query; readiness: ...`
- `minimap: fog: save data ready [after waiting X s] (event record stable for 1.0 s) - fog areas are queried from now on`
- `minimap: fog: X revealed / Y unrevealed / Z unknown (<reason>[, all areas queried]); revealed: '<event key>', ...; fogged again: ...; N queries failed (kept fogged)` - only when something changed.
- `minimap: fog: event record changed (save / session change) - all N areas unknown again`
- `minimap: fog: N fog-shader images outside every FogMapElement (static fog: ...): '<name>' in '<parent>', ...` (see "Static fog")
- WARN `minimap: fog: <field>'s event record is not GameModel.eventRecordModel after 10 s - querying it anyway`

## Static fog: "fog_static" (verified 2026-10-23 from the prefab)

Read with UnityPy from `duplicateassetisolation_assets__54b00e46….bundle` (the page prefabs). Only `RainbowIslandMap` has it:

```
RainbowIslandMap (Map)
  zone_fog_areas            _forceActiveOnAwake[0]; 14 x map_fog_* (Image + CanvasGroup + FogMapElement)
  fog_static (Transform)    _forceActiveOnAwake[1]
    Temporary Static Fog Covering (RectTransform only)
      11 x "covering up static fog (n)"   Image only (one inactive), same material as map_fog_* (PosterizedFogOfWarMask)
    OutsideFogNegativeMasked              Image, PosterizedFogOfWarNegativeMask material
    Negative Mask                         NegativeMaskMap (softness 0.236, roundness 0.79, noise 5.14 / 0.88, updateConstantly)
```

- The covers have no component besides Image and nothing references them (no serialized reference in the prefab, no string literal in the metadata). Their parent is force-activated by `Map.Awake`. So the full map ALWAYS draws them; no reveal ever hides them. Their colours repeat the colours of single fog areas (colour rg = the noise UV offset, so a cover's clouds join its neighbour's seamlessly): they fill the gaps between the zone fog areas and the outside fog.
- Minimap: copied like the full map, kept as one static group, shown while "Respect fog of war" is on and hidden with it off (like all fog art). They are not fog areas: icons under them are not hidden.

Outside fog (`OutsideFogNegativeMasked`, `Custom/PosterizedFogOfWarNegativeMask`):

- `NegativeMaskMap.OnEnable` (0x6346A0) caches `GetComponentInParent<Canvas>().transform` and registers `UpdateBeforeRender` on `Canvas.willRenderCanvases`. `UpdateBeforeRender` (0x6347A0, only with `updateConstantly`): `M = transform.worldToLocalMatrix * canvasTransform.localToWorldMatrix`, then `M[3] = maskEdgeNoiseScale`, `M[7] = maskEdgeNoiseStrength`, `M[11] = softness`, `M[15] = roundness`, `Shader.SetGlobalMatrix(_NegativeMask, M)`. A GLOBAL, written only while the full map's mask is enabled.
- Shader: the position goes through M into mask space, a rounded box (`|2p| < 1`, roundness, softness, edge noise) is cut OUT of the fog: fog only outside the island's rounded rect.
- Before this fix the minimap drew this image with whatever the full map wrote last (or nothing if it was never opened): mask in the wrong place, so the outside fog could cover the whole minimap, explored land included, right after the full map was opened once.
- Now: at build time a "NegativeMask" transform is created in our page where the prefab's NegativeMaskMap sits relative to the Map (position, angle, scale from TransformPoint/InverseTransformPoint, like the art copies). Every frame while the static fog is shown and the full map is closed (it owns the global while open), the module computes the same M from that transform and sets the global. The canvas is the ROOT canvas of the minimap (uGUI batches vertices in the root canvas's space; our nested Canvas has no overrideSorting). For a ScreenSpaceOverlay root the canvas matrix is applied exactly as the game does; for Camera / World render modes the shader position is already the world position, so only `worldToLocal(mask)` is used. ASSUMED (not testable offline): which space the shader position is in; check in game that the outside fog ring follows the island edge like on the full map.
- If anything is missing (class, fields, `Shader.SetGlobalMatrix`, matrix getters, no NegativeMaskMap on the page), the outside-fog image is not copied at all (no stale mask).
- Logs: "fog: outside fog (NegativeMask shader): N image(s) copied, M skipped; mask = our copy of NegativeMaskMap ... / skipped: <reason>" and once "fog: outside-fog mask relative to root canvas '<name>' (render mode N: ...)".

## Fog shaders and minimap opacity (verified 2026-10-23)

Source: the three shaders in `shaders_assets__b2000203….bundle` (`Custom/PosterizedFogOfWarMask` = the FogMapElement art and the static covers, `…Clouds`, `…NegativeMask` = the outside fog), DXBC disassembled with `D3DDisassemble`, constant buffer names from the serialized shader.

- Vertex shader: `COLOR = vertex colour * _Color`. The vertex colour is the Graphic colour times the alpha of every CanvasGroup above it (uGUI bakes it into the vertices).
- Mask pixel shader (Clouds reads `COLOR.a` the same way): `a = round(COLOR.a * 255) / 255` sets a radial threshold: `radial = saturate(smoothstep(dist_from_centre / (1 - smoothstep((a + 0.5) * 2/3))) + a)` multiplies the SDF before the posterize / `_OpacityLevel` steps. At `a = 1` the shape is complete; below about 0.8 a hole opens from the centre of each fog image. This is the reveal animation (`FogMapElement.PlayReveal` tweens its CanvasGroup 1 -> 0). The output alpha only gets `1 - (1 - a)^4` (0.94 at a = 0.5), so alpha cannot fade the fog anyway.
- `COLOR.rg` is a UV offset for the noise (that is why every fog image has its own odd colour).
- `_Color.a` is multiplied into the vertex colour as well, so a material alpha would shrink the fog too.

So "opacity < 1 shrinks the fog" was real: the root CanvasGroup alpha reached the fog's vertex alpha. Uniform fading of the fog would need the fog rendered to a texture first (an extra HDRP camera, too expensive for a HUD element). Chosen fix: every copy that keeps a fog shader gets its own CanvasGroup with `ignoreParentGroups = true` (and no raycasts). Fog keeps its exact shape and is drawn opaque; the opacity setting fades everything else. Visibility is still `SetActive`, which ignoreParentGroups does not affect (the minimap hides with `SetActive` on its root too). Log: "fog: N fog-shader images drawn at full alpha (CanvasGroup.ignoreParentGroups)".

## Mask / frame quality

- Procedural mask and frame sprites: 512 px (circle), 128 px with a 52 px 9-slice border (rounded square), arrow 128 px, dot 64 px; all with a full mip chain and trilinear filtering (`MakeSprite(..., mipmaps)`), antialiased SDF edges.
- The uGUI `Mask` is a stencil mask: its edge is binary whatever the sprite. It is hidden under the frame: inset `max(2, round(1.95% size))` = the middle of the frame's opaque cream band (both frames ~3.8% thick, cream from ~0.9% to ~3%).

## Zoom hotkey and the overlay

The zoom key is polled with `GetAsyncKeyState`; while `sr2::ui::overlay::IsOpen()` (text fields, e.g. profile names) the poll is skipped and the key counts as held, so neither typing nor a key still held when the overlay closes zooms.

## Fault on save change (2026-10-22, build 4c0f9b8a)

`step 'icons': native exception 0xC0000005 at VERSION.dll+0x544DE (reading 0xffffffff38c325ab)`.

Symbolized with the archived PDB (dbghelp; identical-code folding gives the misleading name `vector<Ref>::operator=`) and the disassembly: +0x544DE is in the `ScanGadgets` dictionary lambda, `mov rcx, [rdx+rax]` = `F<Il2CppObject*>(marker, markerSource)` with `rdx = marker` (the `items.size() >= kMaxIcons` check, 0xC00 = 96 x 32 bytes, right before it identifies the lambda). The caller is the inlined `ForEachDictValue` over `MapDirector.Markers`.

Root cause: `DictLayoutOf` (and `ListStructLayout`) passed the ARRAY class to `il2cpp_class_array_element_size`, which takes the ELEMENT class (like `mono_class_array_element_size`) and so returned 8 (a reference element). `Dictionary<string, Marker>.Entry` is 24 bytes (`hashCode, next, key, value`). With an 8-byte stride, "entry" 1 started at `key0`; its "value" at +16 was the real entry 1's `hashCode | next << 32`, and `next == -1` gave the `0xffffffff........` pointer. It only shows once the dictionary has 2+ entries, which the second save had (the first one apparently not).

Fix:

- `ElementLayout` asks the element class and requires `array_element_size == value_size` (an inline struct), plus field-range checks; the layout is logged once ("Dictionary layout ...: entry stride 24, hashCode +0, value +16").
- Dictionary / List<struct> walkers stop at the first implausible pointer (`ugui::PlausiblePointer`).
- `ScanGadgets` validates every value with `IsA(MapDirector.Marker)` before reading its fields.
- `ListStructLayout` is cached per (class, field) (it was per class, one field only).
- World reset (`ResetWorld`) on save change / scene unload drops every cached game object and rebuilds: page copy, markers, fog state, icons, descriptor cache (now pinned), MapDirector, dead pods; the MapPrefabMapping / pod icon are kept only while alive.

## Root causes of the in-game faults (2026-10-22, earlier)

`re/crash-2026-10-22-1855.md` traces the access violations and the crash to one core bug: `il2cpp_gchandle_*` handles were declared `uint32_t` but are `void*` in this IL2CPP build. Effects:

- every free of a stored handle passed a truncated pointer;
- the "find HudUI" fault at `GameAssembly.dll+0x44AF0B` came from a `Ref` freeing a handle;
- so did the teardown faults;
- the game crash came from the old `MapUI.Awake` detour freeing a handle inside the hook.

Fixed in core (`Il2CppGCHandle = void*`). The minimap no longer touches handles inside hooks.

The HUD not being found had a separate cause: HudUI deactivates its own GameObject while hidden. Fixed with the include-inactive lookup and the Awake hook.

## Resolved names

R = required, Setup fails without it. O = optional, the feature degrades and the reason is logged.

| Name | Use | R/O |
|---|---|---|
| `MonomiPark.SlimeRancher.Map.MapDirector` `.GetMapForZone(ZoneDefinition)` | zone → MapDefinition | R |
| `MapDirector._eventDirector`, `.Markers`, `._counters`, nested `Marker` `.ShouldDisplay/.GetPosition/._mapMarkerSource` | fog queries, gadgets, pod icon | O |
| `MonomiPark.SlimeRancher.Map.MapDefinition.MapBackground` (+ nested `ImageSetup.Image/.TintColor`), `.MapBackgroundOverlay` | backdrop | R / O |
| `MonomiPark.SlimeRancher.Map.MapPrefabMapping.GetPrefabFor(MapDefinition)` | page prefab (map art) | R |
| `MonomiPark.SlimeRancher.UI.Map.Map._anchor1/_anchor2` (+ nested `MapAnchorCoordinates.WorldPosition/MapPosition`) | projection | R |
| `Map._projectionHelper` + `MapProjectionHelper._worldToMapScale/_worldToMapOffset` | projection (if initialised) | O |
| `Map._zoneMarkersContainer`, `Map._markerPrefabMapping` | skipped subtree, arrow sprite source | O |
| `MonomiPark.SlimeRancher.UI.Map.MapUI` (`Awake` hook, `_mapPrefabMapping` R, `_mapDirector` O) | full map open, mapping, director | R |
| `MonomiPark.SlimeRancher.UI.Map.MapUIRoot._mapDirector` | director lookup | R |
| `MapUIUtilities.GetPlayerCurrentZone()` or `PlayerDisplayOnMap.GetZoneDefinition()` | current zone | R (one) |
| `PlayerDisplayOnMap` (`Awake` hook, `IsInUnknownArea`) | player transform, hidden cells | R / O |
| `FogMapElement.ShouldBeRevealed`, `DynamicMapElement.get_CanBeShown` | fog / dynamic groups | O |
| `FogMapElement._fogRevealEvent`, `StaticGameEvent._eventDirector/._eventKey`, `EventDirector._model`, `EventRecordModel`, `GameModel.eventRecordModel` | fog reveal source + readiness, area names in logs | O |
| `NegativeMaskMap.softness/roundness/maskEdgeNoiseScale/maskEdgeNoiseStrength`, `Shader.PropertyToID/SetGlobalMatrix(int, Matrix4x4)`, `Transform.localToWorldMatrix/worldToLocalMatrix`, `Canvas.renderMode` | outside-fog mask (image skipped if missing) | O |
| `CanvasGroup.ignoreParentGroups` | fog copies ignore the opacity group | O |
| `IFilterableMapMarker.get_CanBeShown`, `IMapMarkerDescriptorProvider.GetMapMarkerDescriptor`, `IMapMarkerDescriptor.get_MapIcon` (interface slots via `il2cpp_object_get_virtual_method`) | markers | O |
| `MapMarker._image`, `GordoMarkerUI._iconImage` | marker icons | O |
| `MapDirector.Marker` (nested class) | validates `Markers` dictionary values | O |
| `MapMarkerBase` | non-filterable marker art (player / nav markers) is not copied | O |
| `MapDirector._viewModel`, `MapViewModel._prefabMapping`, `MapPrefabMapping._entries` (`Map`, `Prefab`) | page prefab sources | O |
| `MapMarkerPrefabMapping._playerMarkerPrefab`, `PlayerMapMarker._arrowRect/_rotationOffset` | arrow sprite + offset | O |
| `SceneLoader.get_IsSceneLoadInProgress` | no map queries during loads | O |
| `HudUI` (`Awake` hook, `_debugText` font) | parent canvas | O |
| `TreasurePod` (`Awake`/`OnDestroy` hooks, `get_CurrState`), `MapTreasurePodCounterDefinition`/`MapCounterDefinition.Icon` | pods | O |
| `ResourceNodeDirector.get_AllResourceDirectors/_nodeSpawners`, `ResourceNodeSpawner._model/_attachedResourceNode`, `ResourceNode`, `ResourceNodeSpawnerModel.nodeState/resourcesToSpawn`, `IdentifiableType.icon` | resource nodes | O |
| `MonomiPark.SlimeRancher.UI.Framework.Displays.UIDisplayManager` `.get_Instance/.RequestPreloadDisplay/.ReleasePreloadDisplay`, `UIDisplaySceneLoadEntry.get_IsDone`, `MapUIRoot._mapDisplay` | map UI preload (page without the full map) | O |
| `Map._forceActiveOnAwake` | containers active in game | O |
| `GordoMarkerUI._gordoIdentifier`, `SceneContextUtility.TryGetGameModel`, `GameModel.GetGordoModel`, `GordoModel.HasPopped`, `PuzzleSlotMarkerUI.CalculateIsSolved`, `AncientTeleporterMarkerUI` | done state with the fog ignored | O |

Unity side:

- `GameObject(string, Type[])`, `AddComponent(Type)`, `GetComponentsInChildren(Type,bool)`, `GetComponentInParent(Type,bool)`, layer, activeSelf, scene.
- `Object.Destroy`, `hideFlags`, `FindFirstObjectByType(Type, FindObjectsInactive)`, `FindObjectsByType(Type, FindObjectsInactive, FindObjectsSortMode)`.
- `Resources.FindObjectsOfTypeAll(Type)`.
- `Transform`: parent, SetParent, local TRS, position, rotation, TransformPoint, InverseTransformPoint.
- `RectTransform`: anchors, pivot, sizeDelta, anchoredPosition, rect.
- `Image` (sprite, type, ppu multiplier, material), `RawImage` (texture, uvRect), `Graphic` (color, raycastTarget, material, defaultMaterial), `Mask` (showMaskGraphic), `Canvas`, `CanvasGroup`.
- `Material` (`HasProperty(string)`, mainTexture, mainTextureOffset/Scale, shader), `Sprite` (texture, pivot, rect, border).
- `Texture2D` / `Sprite.Create`, `Cursor.lockState`, `Time.timeScale`, `Screen.height`, TMP.

Extra exports:

- `il2cpp_object_new`, `il2cpp_array_new`, `il2cpp_object_get_virtual_method`;
- `il2cpp_class_get_nested_types`, `il2cpp_class_instance_size`, `il2cpp_class_value_size`;
- `il2cpp_field_get_type`, `il2cpp_class_get_element_class`, `il2cpp_class_array_element_size`;
- `il2cpp_gc_wbarrier_set_field`.

Inner offsets of struct fields are computed as `field_offset - (instance_size - value_size)`.

## Settings (module "minimap")

All settings apply live.

| Key | Type | Default / range |
|---|---|---|
| `enabled` | core | — |
| `position` | enum | top left / top right / bottom left / bottom right |
| `offset_x`, `offset_y` | float | % of screen height |
| `size` | float | % of screen height |
| `shape` | enum | circle / rounded square |
| `opacity` | float | — |
| `zoom` | float | 0.5–4; 1.0 ≈ 160 m |
| `zoom_key` | key | `]`; ×2, wraps to 0.5 |
| `orientation` | enum | north up / rotate with player |
| `arrow_style` | enum | game marker / classic arrow (default: classic). Size = 13% of the minimap x clamp(zoom / 2.5, 0.4, 1.6), both styles |
| `arrow_rotation_offset` | float | 180; -180..180 (degrees added to the arrow heading) |
| `show_altitude` | bool | OFF: the "Alt N m" label at the bottom of the minimap in the Grey Labyrinth (hidden, and its text re-set when shown again) |
| `icon_teleporters` (Icons) | bool | on |
| `icon_treasure_pods` (Icons) | bool | on |
| `icon_gordos` (Icons) | bool | on |
| `icon_resource_nodes` (Icons) | bool | OFF |
| `icon_other` (Icons) | bool | on (plort doors, landmarks: page markers of no other category) |
| `icon_null_walls` (Icons) | bool | on: intact Nullifier walls (`DiscordantWall`, Grey Labyrinth), procedural cracked-brick icon tinted violet; harmonized walls hidden; fog applies. Source: `modules/common/discordant_walls.*` (the game's static `_activeWalls` registry, read every 3 s with the other world icons), see `modules/wall_markers/NOTES.md` |
| `icon_placed_gadgets` (Icons) | bool | on: master toggle for every gadget in the save (`GameModel.identifiablesByIdent`, lists of `GadgetModel`s, at `IdentifiableModel.lastPosition`, every 3 s, field reads only, player's scene group only). Icon = `GadgetDefinition._optionalMapIcon`, else `IdentifiableType.icon`, else a procedural gear. Player-placed gadgets ignore fog; `_isPrePlaced` ones follow it. Order under `kMaxIcons` (160): drone ports + teleporters, other, decorations |
| `icon_drone_ports` (Icons) | bool | on: `GadgetDefinition.Type == DRONE_STATION (10)` or a `DroneStationGadgetModel`. Icon = the full map's drone-type sprite (`Map._markerPrefabMapping._droneStationPrefab` -> `DroneStationMapMarker._displayConfiguration` -> `_rancherDroneIcon` / `_explorerDroneIcon` by `_type`), else the gadget icon, else a procedural teal hexagon |
| `icon_placed_teleporters` (Icons) | bool | on: `Type == TELEPORTER (1)` / `WARP_DEPOT (2)`. (`icon_teleporters` now covers only teleporters baked into the map page.) |
| `icon_placed_decor` (Icons) | bool | OFF: `Type == DEFAULT` whose asset name or `groupType` name contains "deco" (heuristic; every new gadget kind is logged once as "placed gadget kinds (new): name[type, group, category, icon]" so it can be refined) |
| `respect_fog` (Icons) | bool | on: unrevealed areas and every icon inside them are hidden. Done states (popped gordos, opened pods, solved plort doors) are hidden in both modes |
| `hide_full_map` (Hide when) | bool | on |
| `hide_menus` (Hide when) | bool | on |
| `hide_conservatory` (Hide when) | bool | off |

Optional keys in `sr2mods.json` under `modules.minimap`:

- `"frame_sprite": "<name>"` uses a loaded game sprite as the frame. This costs one asset scan at UI build.
- `"log_sprite_candidates": true` lists candidate sprite names.

## Verified vs assumed

**Verified offline** (dump + disassembly):

- The projection math above.
- `PlayerMapMarker.Rotate`, `PlayerDisplayOnMap.GetCurrentRotation`, `MapUIUtilities.GetPlayerCurrentZone`.
- `MapDirector.GetMapForZone`, `MapUI.OpenMap` flow.
- `FogMapElement.SetRevealState` / `Awake` / `ShouldBeRevealed`.
- `MapDirector.Marker.ShouldDisplay/GetPosition`, `GadgetModel` descriptor = `IdentifiableModel._ident`.
- `HudUI.UpdateHudVisibility` / `HideUI` toggle the HudUI GameObject; `HudUI.GetSortOrder` uses a Canvas on HudUI.
- Dictionary layout (corefx 2.x: `_entries`, `_count`, free slot hashCode -1).
- Wrapper ABIs: Vector2 in registers; Rect / Vector3 / Vector4 by hidden pointer; Texture2D ctor; Sprite.Create; SetParent; get_scene.
- Hook target RVAs are unique (not folded): MapUI.Awake 0x710890, HudUI.Awake 0x698620, PlayerDisplayOnMap.Awake 0x52D970, TreasurePod.Awake 0x553550, TreasurePod.OnDestroy 0x5538F0.

**Assumed** (to check in game; log lines named):

- `TransformPoint` / `GetRect` on prefab asset transforms give the editor layout. Check:
  - the "page '…' rect … scale … offset" line;
  - the "page copied: N images …" line;
  - visually, the art should line up with the arrow.
- Page graphics use stencil-capable materials. Materials are not copied: our copies use UI/Default, so special shader effects on the map art are lost but masking works.
- Confirmed (2026-10-22 evening): MapPrefabMapping is only loaded with the map UI scene. Now preloaded; check "map UI preload requested for display '…'", "map UI scene loaded (N s after the request)" and the scan line after it ("1 loaded, '…' has page '…'").
- Page marker visibility = the game's `IFilterableMapMarker.CanBeShown` evaluated on the PREFAB component (AncientTeleporterMarkerUI discovery/show query, GordoMarkerUI seen && !popped via GameModel, PuzzleSlotMarkerUI discovered && !solved, StaticMapMarker -> its DynamicMapElement). Disassembly shows these read only serialized data / GameModel / EventDirector (queries are created lazily on the prefab). Check the "marker visibility from the game's CanBeShown" line; failures hide the marker.
- Treasure pods: shown while LOCKED and (fog respected) not under fog; resource nodes: READY + live node object, behind their toggle; teleporter gadgets: `MapDirector.Marker.ShouldDisplay()`.
- White map (user test 2026-10-22): the cause is not known for sure. Handled candidates: sprite-less placeholders, hidden mask graphics, disabled Images, custom materials (copied / main texture). Check the "page copied" (skip reasons), "page materials", "page placeholders skipped" and "page art (largest first)" lines; "sprites without a texture" > 0 means a late-bound sprite atlas that is only loaded when the full map opens.
- Fog direction is verified: `FogMapElement.SetRevealState(bool)` sets its CanvasGroup alpha to `!isRevealed` (0x704B20), so fog images are overlays shown while NOT revealed. The game evaluates `ShouldBeRevealed(MapUIFogReveal._eventDirector)` (0x729650 / 0x729190); we pass the director whose `_model` is the loaded save's record (see "Fog of war state").
- Verified: the fog shaders use vertex alpha as the reveal threshold; fog copies ignore the root CanvasGroup (see "Fog shaders and minimap opacity").
- Marker classification keywords: see the "page copied … markers (…)" histogram.
- Zone names "conservatory"/"ranch", "labyrinth": see "zone -> '…'".
- Teleporter gadget asset names contain "teleport".
- Menus unlock the cursor or pause time.
- The Grey Labyrinth has a single flat page (no per-floor fields were found), so the minimap shows an altitude label.
