# minimap module notes

A corner minimap built from the game's own full-map art. Game 1.3.0; everything is resolved by name at runtime (no RVA or offset is used by the code). The game-side facts (MapUI, Map projection, fog, markers, map UI scene lifetime, prefab and shader assets) are in `re/map.md`; IL2CPP / uGUI facts in `re/unity.md`; HudUI and the UI display framework in `re/ui.md`; GameModel, events, pods, nodes, walls in `re/world.md`.

Files:

- `module.cpp`: lifecycle, hooks, tick, the time-sliced page copy, markers, icons, fog, layout.
- `game.h` / `game.cpp`: game-side symbols (Assembly-CSharp), each with a comment naming the class/field.
- `ugui.h` / `ugui.cpp`: Unity / uGUI / TMP bindings, GC-handle `Ref` with a freeze switch, pinned-array iteration, Dictionary and List<struct> walkers, a native-fault guard that reports module+offset.
- `procgen.h`: procedural RGBA images (mask shapes, fallback frame, arrow, dot, wall / gear / hexagon icons).
- `api.h`: the API for `map_tools` (below).

## Approach

The full map instantiates one page prefab per `MapDefinition` (RE: re/map.md, MapUI.OpenMap). The minimap never instantiates game prefabs: it reads the page prefab in place and copies its content into our own uGUI objects.

- Every visible `Image` / `RawImage` becomes our own `Image` / `RawImage` with: sprite or texture + uvRect; a custom material only if its shader has `_Stencil` (so our mask still clips it); a non-stencil custom material on a sprite-less Image becomes a RawImage of the material's main texture (+ offset/scale); otherwise the material is dropped; colour x the alpha of every `CanvasGroup` above it; Image type + pixelsPerUnitMultiplier; its four rect corners converted into page space (`TransformPoint` / `InverseTransformPoint`), which set size, position, rotation and mirroring.
- Skipped (counted per reason in the "page copied" line): inactive subtrees (activeSelf walk up to the page root, but GameObjects in `Map._forceActiveOnAwake` count as active, because `Map.Awake` activates them; RE: re/map.md, Map); disabled Graphics; the Image of a `Mask` with `showMaskGraphic == false`; sprite-less Images without a texture source outside fog/dynamic groups ("page placeholders skipped"); `Map._zoneMarkersContainer`; every `MapMarkerBase` subtree that is not an `IFilterableMapMarker` (player / navigation marker art) and any image using a player-marker sprite; fully transparent images.
- Point-filtered map textures are switched to bilinear ("page activation" line).
- Images under a `FogMapElement` or a `DynamicMapElement` become groups shown or hidden with the game's own pure queries (`ShouldBeRevealed`, `get_CanBeShown`), see "Fog of war".
- `IFilterableMapMarker` components become our own icons: sprite of `MapMarker._image` or `GordoMarkerUI._iconImage`, else the first sprite in the marker's subtree.
- The copy is time-sliced (`kBuildChunk` = 32 graphics per frame); the minimap is hidden until it is done. `FinishBuild` (markers, cone, first fog query) is one frame.

### Page prefab source and the map UI preload

The page prefabs live only while the full map's Addressables UI scene is loaded (RE: re/map.md, "Map UI scene / page prefab lifetime"). The minimap holds that scene itself with the game's preload API (RE: re/ui.md, UIDisplayManager):

- `UIDisplayManager.Instance.RequestPreloadDisplay(MapUIRoot._mapDisplay, "SR2Mods.Minimap.PagePreload")`. MapUIRoot is a scene object (one lookup, then cached). No MapUI is instantiated, but the mapping and pages stay in memory.
- `UIDisplaySceneLoadEntry.get_IsDone` polled at 10 Hz until true, then one `Resources.FindObjectsOfTypeAll(MapPrefabMapping)` (~25 ms, once).
- Re-requested every 10 s (idempotent) and after every world reset; released (`ReleasePreloadDisplay`) when the module is disabled. No asset scan while the preload is loading (up to 30 s).

`FindPagePrefab`, first hit wins, the page must have a `Map` component:

1. the mapping we hold (from `MapUI.Awake` or a scan): `GetPrefabFor(mapDef)`, then its serialized `_entries` in case the runtime dictionary is stale;
2. `MapDirector._viewModel._prefabMapping` (exists once any full map was used);
3. asset scan `Resources.FindObjectsOfTypeAll(MapPrefabMapping)`, every loaded mapping tried: right after the preload is done, otherwise every 15 s while missing (6 tries), then every 60 s. The MapUI hook takes over when the full map opens.

The build is re-tried every second while there is no page; the error line names the state ("map UI preload not requested yet" / "map UI scene still loading" / "map UI scene loaded but no MapPrefabMapping has this map" / "map UI preload unavailable").

### UI hierarchy

Under the HudUI GameObject (it carries the HUD Canvas, so the minimap shows and hides with the HUD), or the root canvas above it, or an own overlay canvas after 10 s:

```
SR2Mods.Minimap        RectTransform, anchor = pivot = (pos_x, 1 - pos_y) / 100, nested Canvas, CanvasGroup (opacity, no raycasts)
  Mask                 Image (procedural disc / 9-sliced rounded square) + Mask, inset max(2, round(1.95% size)) px
                       = the middle of the frame's opaque band, so no map pixels show through the frame's AA edge
    Background         MapDefinition.MapBackground sprite + TintColor (screen-fixed, like MapUI)
    BackgroundOverlay  MapDefinition.MapBackgroundOverlay
    Rotator            z rotation for "rotate with player"
      Page             our copy of the page; local space == prefab page space
        Art / fog / dynamic copies, NegativeMask (outside-fog mask transform)
        PlayerCone     game marker only: the view cone (own nested Canvas), under every icon
        Marker icons, Icon (pods / nodes / walls / gadgets)
  Arrow                always centred (GTA style), setting arrow_style:
    PlayerArrow        classic: the procedural arrow
    GameMarker         game marker: copy of the full map's PlayerMarker prefab (own nested Canvas)
      Facing           = PlayerMapMarker._arrowRect, turns with the heading: FacingArrow
      BeaIcon          upright
  Frame                procedural ring or a configured game sprite
  Level                TMP label, Grey Labyrinth only
```

## Map position

Projection = the game's `Map.GetMapPosition` (RE: re/map.md, Map / MapUtils.GetSlopeAndOffset): scale and offset from the prefab `Map`'s serialized `_anchor1/_anchor2` (logged "projection from anchors: ..."); `_projectionHelper` is only a fallback for degenerate anchors (it is null on the prefab).

- The same formula and clamp are applied to the `PlayerDisplayOnMap` transform position `p`; `Page.localScale = s`, `Page.localPosition = -s*p`, `s = (size/2) / ((160 m / zoom) * avg|scale|)`.
- The arrow never moves: the page moves (and in rotate mode turns) under it. Rotation follows `PlayerMapMarker.Rotate` with `rot = PlayerDisplayOnMap.GetCurrentRotation()` (RE: re/map.md): `arrowZ = rot - yaw - _rotationOffset` (game marker; offset 0 in 1.3.0) or `rot - yaw` (classic arrow). The old `arrow_rotation_offset` slider is gone; a saved value is ignored and logged once at startup.
- Rotate-with-player: the Rotator turns by `+yaw`, so the arrow points up.
- Arrow size: 13 % of the minimap x `clamp(zoom / 2.5, 0.4, 1.6)` (min 10 px).
- Icons are counter-rotated and counter-scaled (upright, not growing with the page). Size 8.5 % of the minimap x `IconZoomFactor()` = `clamp(zoom / kIconBaseZoom(1.0), 0.6, 1.75)`.

## Player marker ("Game marker" style)

A copy of the full map's `MapMarkerPrefabMapping._playerMarkerPrefab` (prefab layout and animator clips: re/map.md, MapMarkerPrefabMapping). `LoadGameMarker` copies every visible Image once per page build while its sprites live: sprite, colour x CanvasGroup alphas, type, preserveAspect, rect corners in `_arrowRect` space (turning parts) or the root's space (upright parts), original pivot.

- `Arrow/GameMarker`: centred, scale = 1.5 x arrow px / 50 (`kGameMarkerScale`), own nested Canvas; a `Facing` container at `_arrowRect`'s origin turned to `arrowZ` holds FacingArrow; BeaIcon upright after it.
- The cone (`_moveBelowMarkersTransform` subtree) goes into the page as `PlayerCone` (own nested Canvas), created in `FinishBuild` after the art and before every icon, so icons draw over it (like `PlayerMapMarker.SetBelowMarkerLayer`). Placed every frame at the player point + the facing origin (turned by -rot, divided by the page scale), rotation `arrowZ - rot`, scale `k / s`; each set only on change.
- Animation reproduced from the clips, driven by the module clock: `MapMarkerPulse` (FacingArrow scale) and `MapMarkerPulse_Cone` (cone y + alpha). The rare BeaIcon wobble is NOT reproduced.
- No visible image in the prefab: classic arrow (WARN). Sprites unloaded: the copy is dropped and read again at the next page build. Page art using these sprites is skipped.
- Log: `minimap: player marker from the game (N images, root 50 high, facing origin (x,y), prefab rotation offset 0): ...`.

## Object sources (no periodic scene scans)

Hooks (optional) record raw `this` only: `MapUI.Awake` (full map open; its `_mapPrefabMapping` and `_mapDirector`), `HudUI.Awake`, `PlayerDisplayOnMap.Awake`, `TreasurePod.Awake` / `OnDestroy` (queues feeding the pod registry).

Fallbacks:

- HudUI, player, SceneLoader: `Object.FindFirstObjectByType(Type, FindObjectsInactive)` at most every 5 s and only while missing. HudUI is searched including inactive objects, because it deactivates its own GameObject while the HUD is hidden (RE: re/ui.md, HudUI).
- MapDirector: `MapUIRoot._mapDirector` or `MapUI._mapDirector`.
- MapPrefabMapping: `MapUI._mapPrefabMapping`, else the preload + scan above.
- Pods that woke before the module was enabled: one `FindObjectsByType` per enable.

World icons (RE: re/world.md, re/fabricator.md):

- Resource nodes: `ResourceNodeDirector.get_AllResourceDirectors()` -> `_nodeSpawners` -> READY model (`nodeState == 1`) + an alive, active `_attachedResourceNode` + non-empty `resourcesToSpawn`; placed at the node object; icon `resourcesToSpawn[0].icon`.
- Treasure pods: LOCKED only (`CurrState == 0`); icon from `MapDirector._counters` -> `MapTreasurePodCounterDefinition.Icon`. World position read once per pod (pods never move); only pods in view cost game calls.
- Nullifier walls: `modules/common/discordant_walls.*` (`DiscordantWall._activeWalls`, active walls only; see `modules/wall_markers/NOTES.md`).
- Placed gadgets: `GameModel.identifiablesByIdent`, the lists of `GadgetModel`s, at `IdentifiableModel.lastPosition`, player's scene group only, field reads only. Icon = `GadgetDefinition._optionalMapIcon`, else `IdentifiableType.icon`, else procedural. Drone ports use the full map's drone-type sprite (`Map._markerPrefabMapping._droneStationPrefab` -> `DroneStationMapMarker._displayConfiguration` -> rancher / explorer icon by `DroneStationGadgetModel._type`), else the gadget icon, else a teal hexagon. Order under `kMaxIcons` (160): drone ports + teleporters, other, decorations. (This replaced the old `MapDirector.Markers` teleporter scan; `Markers` only holds `DisplayOnMap` gadgets.)

Placed-gadget groups (1.3.0 findings): `GadgetDefinition.Type` 10 = drone station; 1 / 2 = teleporter / warp depot, checked on the gadget or else its base (`VariantOf`); `DEFAULT` gadgets with "teleport" / "warpdepot" / "marketlink" / "refinerylink" in the asset or icon name count as teleporters too, and a `TeleporterGadgetModel` / `WarpDepotModel` always does. Decorations are `DEFAULT` with an empty `groupType`; only the icon name separates them (`iconDecor*_small`), so the "decor" test on asset / icon / group name is in practice the icon test. Each definition is logged once per session: `placed gadget kinds (new): name[type t(, base type b), group 'g', <group>, icon 'i']`. A placed warp depot has not been seen in a log yet.

## API for other modules (`api.h`)

- `Get(Snapshot&)`: root, zone, page prefab, projection (collectibles).
- `CollectPageIcons(query, out, &mapDef)`: the full map's extra icons (`modules/map_tools`). Runs `ScanPods` / `ScanNullWalls` / `ScanPlacedGadgets` / `ScanNodes` with the caller's categories and a 2000-icon budget over the whole page (no view radius, no fog test); inactive Nullifier walls of proxied regions are included; with `undiscoveredMarkers` also the page markers whose `CanBeShown` is false but which are not done (`QueryNotDone`). `stats_` is saved / restored. Same fault policy as the tick. Positions are page-local (valid inside the live page of the same MapDefinition).
- `NullWallSprite(rgba)`: the procedural Nullifier wall sprite and its tint (map_tools' remembered walls on other tabs).

## Fog of war

RE: re/map.md (MapUIFogReveal, FogMapElement, fog shaders), re/world.md (EventDirector).

- Per area: `state` = the game's answer (1 revealed / 0 unrevealed / -1 UNKNOWN), separate from the drawn state (`vis`). Unknown counts as unrevealed everywhere (fog drawn, icons inside hidden). Failed queries leave the area unknown. The setting only changes what is drawn; turning it on re-queries every area.
- Reveal source: `ShouldBeRevealed(director)` with director = the reveal event's own `StaticGameEvent._eventDirector`, else `MapDirector._eventDirector`, the first whose `_model` IS `GameModel.eventRecordModel`.
- Readiness: GameModel present, its record set, the director's `_model` == that record, stable for 1 s. Until then every area is unknown = fogged. A director whose record never matches is used after 10 s with a WARN. Polled at 10 Hz after a world reset until settled, then re-checked with every fog evaluation; a changed record (save / session change) makes every area unknown again.
- Cleared on every world reset, page rebuild and zone change (all areas re-queried in the same tick, no flash).
- Drawing: `SetActive` per area. Every copy that keeps a fog shader gets its own CanvasGroup with `ignoreParentGroups = true`: the fog shaders read vertex alpha as a reveal threshold, so the opacity setting would shrink the fog instead of fading it (RE: re/map.md, fog shaders). Fog is therefore drawn opaque; opacity fades everything else.
- Fog areas as shapes: each `FogMapElement` group keeps one page-space parallelogram per copied image (or the element's own rect without art). An icon is "under fog" when it lies in a shape of an UNREVEALED or UNKNOWN area.
- Static fog (`fog_static`, RainbowIslandMap only): the cover images are copied as one static group, shown while "Respect fog of war" is on (the full map always draws them); they are not fog areas, icons under them are not hidden.
- Outside fog (`OutsideFogNegativeMasked`): the game's `NegativeMaskMap` writes a global shader matrix only while the full map is open. At build time a `NegativeMask` transform is created in our page where the prefab's NegativeMaskMap sits; while the static fog is shown and the full map is closed, the module computes the same matrix from that transform relative to the minimap's ROOT canvas and sets `_NegativeMask` itself. ScreenSpaceOverlay root: canvas matrix applied as the game does; Camera / World: only `worldToLocal(mask)`. ASSUMED: which space the shader position is in (check in game that the outside fog ring follows the island edge). If anything is missing, the outside-fog image is not copied at all.

### Visibility rules

- "Respect fog of war" ON: page markers need the game's `CanBeShown` (discovery + not done) and must not be under fog; world icons must not be under fog. The minimap hides while `PlayerDisplayOnMap.IsInUnknownArea`.
- OFF: fog art (areas + static fog) hidden, nothing hidden by fog or discovery, but the DONE state still applies: gordos `GameModel.GetGordoModel(_gordoIdentifier).HasPopped()`; plort doors `PuzzleSlotMarkerUI.CalculateIsSolved()`; ancient teleporters always; other markers (StaticMapMarker -> DynamicMapElement) their `CanBeShown`; if a done state cannot be read, `CanBeShown` is used.
- Treasure pods: LOCKED only in both modes. Resource nodes: see above. Player-placed gadgets ignore fog; `_isPrePlaced` ones follow it.
- Page markers are hidden until their first `CanBeShown` answer; a failing query hides the marker.

## Update rates

| What | Rate / rule |
|---|---|
| Transform | per frame: player position + rotation read; page scale / position, rotator, arrow set only when they change (nothing set while standing still); icon counter-rotation in 0.5 deg steps, counter-scale on zoom changes. Game marker: pulse animation (own nested canvases). |
| Outside-fog mask | only when the page moved / turned / scaled, after the full map closed or the minimap reappeared, on layout / canvas changes, and at least every 0.5 s |
| Zone, hide rules, unknown area, marker visibility | 10 Hz |
| `CanBeShown` per page marker | round robin, max 8 calls per 10 Hz tick, each marker every 2 s, cached in between |
| Fog | every 2 s: unrevealed + unknown areas. Every area: after the build, when the save data becomes ready, on a zone change, after the full map closes, when the setting changes, and every 30 s. Our objects are touched only when an area's visibility changes. |
| Dynamic elements | every 10 s |
| Pods / nodes / walls / gadgets | every 3 s; an icon's sprite / colour / position / orientation is set only when it changed (cached per icon); a settings change rescans at once |
| Page copy | once per map change (or after a failed build), 32 graphics per frame |
| World reset | on scene-load start, a new / destroyed `PlayerDisplayOnMap`, or the page prefab / MapDefinition no longer alive. Drops every cached game object and rebuilds (page copy, markers, fog state, icons, descriptor cache, MapDirector, dead pods); the MapPrefabMapping / pod icon are kept only while alive. |
| Map UI preload | first tick and after each world reset, re-asserted every 10 s; `IsDone` polled at 10 Hz only while loading |
| Zoom hotkey | per frame `GetAsyncKeyState`; skipped while the settings overlay is open (`sr2::ui::overlay::IsOpen()`), and the key then counts as held, so typing or a key held when the overlay closes does not zoom |

## Performance

- `IsSceneLoadInProgress` is checked every frame (it protects every map call during loads).
- The only hidden cost is Unity's canvas rebuild caused by our sets (pure uGUI, no camera / render texture): hence "set only on change" everywhere and nested Canvases for per-frame animated parts. Visible only as overall fps (minimap on vs off, standing still).
- Measured before the set-on-change work (2026-10-24, ~70-85 fps): `frame:minimap` avg 0.03-0.05 ms, max 0.4-2.5 ms, a few > 2 ms frames per minute; one-off 49 ms at the first UI build and ~24 ms at a page build after a zone change.
- Next step if `minimap.build finish` shows large numbers: time-slice `FinishBuild`.

Log lines: perf scopes in the 10 s summaries and "frame hitch ... top:" lists: `minimap.transform`, `minimap.negmask`, `minimap.slow (10 Hz)`, `minimap.markers (10 Hz)`, `minimap.fog (2 s)`, `minimap.dynamic (10 s)`, `minimap.icons (3 s)`, `minimap.build (page copy)`, `minimap.build finish (markers, first fog)`, `minimap.page build start`, `minimap.ui build (once)`, `minimap.ui sprites (procedural, once)`. With `perf_log` on, every 30 s: `minimap: perf (30 s): N frames shown, page transform set in P of them; outside-fog mask set M times; icon orient calls O; icon property sets S (U icon updates with nothing to set); pods A in view / B skipped without a game call` (standing still: P = 0, M about 60, S near 0).

## Fault policy

- Every game call made by the tick runs under `core::Try` + `ugui::GuardNative`; managed exceptions from per-element queries are contained.
- Any native fault (tick, teardown step or hook) shuts the session down: `ugui::FreezeRuntime`, every `Ref` drops its object without calling IL2CPP (handles leaked), the frame callback is removed, hooks become pass-throughs, no teardown calls into Unity. Log: `minimap: <step>: native exception … at <module>+0x<offset> … - minimap disabled for the rest of this session`. `Die()` also calls `core::MarkModuleFaulted` (ModuleManager shows FAILED).
- Each detour calls the original first (unguarded), then records `self` inside a `core::DetourGuard`; it never frees or creates GC handles (RE: re/unity.md, GC handles are `void*`).
- If core's GC-handle self-test fails, `NewGCHandle` returns nullptr; `Ref` never keeps an unpinned pointer and the minimap finds nothing.
- Dictionary / List<struct> walkers use `ElementLayout` (element class, `array_element_size == value_size`, field-range checks; layout logged once, e.g. "entry stride 24, hashCode +0, value +16"), cached per (class, field), and stop at the first implausible pointer (`ugui::PlausiblePointer`). RE: re/unity.md (array_element_size takes the element class).
- Teardown (normal disable) destroys only our own objects and releases each reference in its own guarded step.

## Mask / frame quality

- Procedural sprites: mask / frame 512 px (circle), 128 px with a 52 px 9-slice border (rounded square), arrow 128 px, dot 64 px; full mip chain + trilinear (`MakeSprite(..., mipmaps)`), antialiased SDF edges.
- The uGUI `Mask` edge is binary (stencil), so it is hidden under the frame's opaque cream band (both frames ~3.8 % thick, cream ~0.9-3 %).

## Settings (module "minimap")

All settings apply live.

| Key | Type | Default / range |
|---|---|---|
| `enabled` | core | — |
| `pos_x`, `pos_y` | float | 98.7 / 2.6; % of the free space (screen minus minimap), set in the overlay's HUD edit mode (`ui/NOTES.md`). Replaced `position` / `offset_x` / `offset_y`, migrated once at the first layout |
| `size` | float | 24; 10-50 % of screen height (edit mode: drag a corner) |
| `shape` | enum | circle / rounded square |
| `opacity` | float | fades everything but the fog |
| `zoom` | float | 0.5–4; 1.0 ≈ 160 m |
| `zoom_key` | key | `]`; ×2, wraps to 0.5 |
| `orientation` | enum | north up / rotate with player |
| `arrow_style` | enum | game marker / classic arrow (default classic); see "Map position" and "Player marker" |
| `show_altitude` | bool | off: "Alt N m" label in the Grey Labyrinth (single flat page) |
| `icon_teleporters` | bool | on (teleporters baked into the page) |
| `icon_treasure_pods` | bool | on |
| `icon_gordos` | bool | on |
| `icon_resource_nodes` | bool | off |
| `icon_other` | bool | on (plort doors, landmarks: page markers of no other category) |
| `icon_null_walls` | bool | on: intact active Nullifier walls, procedural cracked-brick icon tinted violet; fog applies |
| `icon_placed_gadgets` | bool | on: master toggle for placed gadgets |
| `icon_drone_ports` | bool | on |
| `icon_placed_teleporters` | bool | on (teleporters, warp depots, market / refinery links) |
| `icon_placed_decor` | bool | off |
| `respect_fog` | bool | on |
| `hide_full_map`, `hide_menus` | bool | on (menus = cursor unlocked or `Time.timeScale == 0`) |
| `hide_conservatory` | bool | off |

Optional keys in `sr2mods.json` under `modules.minimap`: `"frame_sprite": "<name>"` (a loaded game sprite as the frame; one asset scan at UI build), `"log_sprite_candidates": true`.

## Resolved names

R = required (Setup fails), O = optional (feature degrades, reason logged). The full list with comments is `game.h`; the Unity side is `ugui.h`.

- R: `MapDirector.GetMapForZone`, `MapDefinition.MapBackground` (+ `ImageSetup.Image/.TintColor`), `MapPrefabMapping.GetPrefabFor`, `Map._anchor1/_anchor2` (+ `MapAnchorCoordinates`), `MapUI` (+ `_mapPrefabMapping`), `MapUIRoot._mapDirector`, one of `MapUIUtilities.GetPlayerCurrentZone` / `PlayerDisplayOnMap.GetZoneDefinition`, `PlayerDisplayOnMap`.
- O: fog (`FogMapElement.ShouldBeRevealed/_fogRevealEvent`, `StaticGameEvent`, `EventDirector._model`, `GameModel.eventRecordModel`), dynamic elements, outside-fog mask (`NegativeMaskMap` fields, `Shader.SetGlobalMatrix`, matrix getters, `Canvas.renderMode`), `CanvasGroup.ignoreParentGroups`, marker interfaces (via `il2cpp_object_get_virtual_method`), marker icons, done-state classes, player marker prefab parts, page prefab sources (`_viewModel`, `_entries`), map UI preload (`UIDisplayManager`, `UIDisplaySceneLoadEntry.get_IsDone`, `MapUIRoot._mapDisplay`), `Map._forceActiveOnAwake`, `SceneLoader.get_IsSceneLoadInProgress`, HudUI, pods, nodes, gadgets / drone ports, walls.

## Known limits / assumed

- `TransformPoint` on prefab asset transforms gives the editor layout (art lines up with the arrow in practice; check "page '…' rect … scale … offset" and "page copied: N images …").
- Outside-fog shader space (see "Fog of war").
- White map (2026-10-22): cause never pinned down; handled candidates are placeholders, hidden mask graphics, disabled Images, custom materials. Check the "page copied" (skip reasons), "page materials", "page placeholders skipped" and "page art (largest first)" lines; "sprites without a texture" > 0 means a late-bound atlas loaded only with the full map.
- Marker classification keywords: "page copied … markers (…)" histogram. Zone names "conservatory"/"ranch", "labyrinth": "zone -> '…'" line.
- Treasure pods, nodes and gadgets are only as fresh as the 3 s scan.

## History

- 2026-10-22: faults traced to GC handles declared `uint32_t` (core fix, `re/crash-2026-10-22-1855.md`); HudUI deactivating itself (include-inactive lookup + Awake hook); save-change fault from the Dictionary stride bug (fixed walkers, `IsA` checks, world reset).
- 2026-10-22 evening: page lost after the map closed (map UI scene flush) -> map UI preload; fog areas without shapes -> `_forceActiveOnAwake`; fog-off showed popped gordos -> done states; READY nodes without a node object; mask edge quality.
- 2026-10-22 night: fog state cached as revealed with the fog ignored / on failed queries / before the save was ready -> per-area state + readiness.
- 2026-10-23: static fog, outside-fog mask, fog drawn at full alpha (opacity shrank it).
- 2026-10-24: perf pass (set only on change, 0.5 deg icon steps, pod positions once); game marker copies every prefab image (was the cone only); placed-gadget scan and group classification.
