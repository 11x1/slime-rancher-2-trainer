# map_tools module notes

Extensions for the game's own full-screen map (`MapUI`). Module key `map_tools`, on by default (every feature has its own switch; the defaults change nothing but the extra icons). Options tab: HUD > Full Map. Game 1.3.0; everything is resolved by name, the RVAs below are only for re-checking after an update (Cpp2IL dump + IDA, `re/dump/`).

## Map open detection

`MapUI.LateUpdate` (0x711E90) is hooked: it runs every frame while the full map is shown and records `this`. No `MapUI.LateUpdate` for 3 frames = map closed (all per-map state dropped). The minimap's `MapUI.Awake` hook is not reused (one hook per address, and Awake is once per map UI scene load, not per open).

## Reveal everything (`reveal_all`, default off)

What the game does (disassembly):

- `MapUIFogReveal.RevealZonesInMap(Map)` (0x729650), run when a page opens (and on `Start`), collects the page's `FogMapElement`s and calls `RevealKnownZones` (0x729190): per element, if its reveal event is waiting for the reveal animation it is tweened (`PlayReveal`), otherwise `SetRevealState(ShouldBeRevealed(_eventDirector))`.
- `FogMapElement.SetRevealState(bool)` (0x704B20) sets the element's CanvasGroup alpha to `!isRevealed`. Its only code caller is `RevealKnownZones`.
- `MapDirector.IsZoneHidden(ZoneMapData)` (0x9C2030): `!_unlockedOnMapByDefault && !zone-entered query`. Callers: `MapUI.LateUpdate`, `ZoneMarkerUI.LateUpdate`, `LabyrinthZoneMarkerUI.LateUpdate` only (the zone names / "???" on the map).
- The static fog (`fog_static`: covers + outside fog, see `modules/minimap/NOTES.md`) is never hidden by the game.

What the module does:

- Hook `SetRevealState`: `false` becomes `true` while the switch is on.
- Hook `IsZoneHidden`: `true` becomes `false` while the switch is on (zone names shown).
- On the live page: every Graphic with a `*FogOfWar*` shader outside every `FogMapElement` gets `Behaviour.enabled = false`; switched back on when the switch goes off or the map closes.
- Switch changed while the map is open: `RevealZonesInMap(MapUI._map)` re-runs the game's own pass.

**Persistence: none.** Nothing is written to the save or to the event record: `ShouldBeRevealed` / the zone queries are untouched, so the minimap, the collectibles tracker (map data nodes) and discovery stay real. With the switch off, the next map open (or at once if the map is open) shows the real fog.

Not covered: map pages the player has not unlocked are still not listed as tabs (`MapUI.UpdateAvailableMaps`).

## Icons on the full map

Source: `modules/minimap/api.h` `CollectPageIcons` (the minimap's own scanners, whole page, with this module's categories), so the icons need the minimap running and only fit the page of the player's zone: when the full map shows another map (tabs), they are hidden. Every 2 s while the map is open, and at once after a settings change.

Categories (`icon_*`, each with `size_*` in 1080p pixels, 12-64):

| setting | default | source |
|---|---|---|
| treasure pods | on | locked `TreasurePod`s (pod registry) |
| resource nodes | off | READY spawners with a live node |
| Nullifier walls | on | intact `DiscordantWall`s |
| placed gadgets (+ drone ports, teleporters/warp depots/links, decorations) | on (decorations off) | `GameModel.identifiablesByIdent` gadgets |
| undiscovered ancient teleporters / gordos / plort doors and landmarks | off | page markers the full map does not show yet (`CanBeShown` false) and are not done (popped gordo, solved door): spoilers |

Drawing: our own uGUI Images under a `SR2Mods.MapIcons` container that is a child of the live page (`MapUI._map`, a `Map` RectTransform), at local position 0 / scale 1, so icon positions are page-local (the same space as `Map.GetMapPosition` and the minimap's page copy). The game pans and zooms by moving / scaling the page, so the icons stay aligned whatever the zoom, and the map's mask clips them. Chosen over an ImGui overlay, which would have to re-derive the page's screen rect from canvas / camera every frame and would draw over the game's own popups.

- Constant screen size: each icon's local scale = (root canvas scale) / (page scale), recomputed per frame (two `TransformPoint`s each), applied only on a change > 0.5 %.
- `raycastTarget = false`: clicks and drags still reach the map.
- `icons_under_fog` (on): the container is inserted at the child index of the first direct child of the page that contains a `FogMapElement`, so the fog layer draws over the icons like over the map art. Off (or no fog on the page): before the marker containers (`Map._belowMarkersContainer` / `_staticMarkerContainer`). Logged: `icons: container on page '…' at child index N (…; fog layer '…' at F, markers '…' at M)`. ASSUMED: the fog layer's direct child holds only fog (true for RainbowIslandMap's `zone_fog_areas`); check that icons are not hidden under land art.

## Right-click teleport (`rmb_teleport`, default off)

- Input: `MapClickPlaceMarker.IPointerClickHandler.OnPointerClick` (0x706AB0) is the map surface's click handler (left button = toggle waypoint; other buttons do nothing in the game). Hooked: a right click (`PointerEventData.button == 1`, `_isDragging` false) sets a flag; the frame callback handles it. So a right click on a map button / panel does nothing.
- Cursor page point: `MapUI.LateUpdate` uses `-MapViewModel.CursorPosition` = `-(_mapAnchor + _cursorOffset / _mapZoom)` as the page-local cursor (it feeds `TryFindZoneAt`). `_cursorOffset` is set by `MapPanZoomPointerControls.OnPointerMove` -> `SetViewportCursorPosition`. Must lie inside `_fullMapRect`.
- World point: inverse of `Map.GetMapPosition` (`map = mapMin + (world - worldMin) * mapSize / worldSize`, the view model's `_mapRect` / `_worldRect` = the page anchors, `Map.GetMapArea` / `GetWorldArea`). Note: the game's own waypoint code (`MapViewModel.ToggleNavigationMarkerAtCursorPos`, 0x72BF80) puts the waypoint at `-(worldMin + (CursorPosition - mapMin) * k)`, which equals ours only for pages whose anchors have no offset (RainbowIslandMap: scale 1, offset 0). Check the "teleported ... (map point ...)" log line against where you clicked on other maps.
- Checks: the shown map must be the map of the player's zone (`MapUI.get_CurrentMapDefinition` vs `MapDirector.GetMapForZone(MapUIUtilities.GetPlayerCurrentZone())`); the target must be in the player's zone (`MapUI.TryFindZoneAt` at the UI-world point of the target and of the player, `ZoneDetectionArea._zoneMapData` compared; outside every area is allowed). Other zones are other scene groups: getting there needs the game's scene load, which this module does not drive.
- Ground: `Physics.Raycast((x, 3000, z), down, 6000 m, all layers but Ignore Raycast and the player's, triggers ignored)`. No hit (area not loaded, outside the world) = refused. The player lands 1 m above the hit point, so never inside the ground; the first solid surface from above wins (a roof, a tree top, a rock).
- Teleport: `TeleportablePlayer.TeleportTo(position, get_SceneGroup(), null rotation, true)` (0x54EC90), the game's in-scene teleport (teleporters inside one zone use it): `IPlayerController.Position`, `PlayerModel.SetCurrentSceneGroup`, `RegionLoader.InitializeRegionProxies`, the held item, screen effects, audio position. The camera follows the player.
- Then the map closes: `UIDisplayManager.Instance.CloseDisplayByObjectInHierarchy(MapUI)` (what `DroneUIDisplay.CloseThisDisplay` does). The game has no map fast travel to copy; closing shows where you landed.
- Refused clicks play the map's own `_failedPlaceWaypointCue` (`UIAudioCueHandle.PlayAudioCue`, NO_WARNING) and log `teleport refused: <reason>`.
- Caveats: in the Grey Labyrinth (roofed) the ray may land on a roof; a click over the sea lands on the sea floor (the game's water rules apply).

### "[RMB] Teleport" in the key bar

`MapUI._inputLegend` (`ComponentInterface<IInputLegend>`, `ComponentInterfaceBase._object`) is the `InputLegend`; its `_hints` (`InputHint[]`) are the key bar entries. While the switch is on, the first active hint is cloned (`Object.Instantiate`, same parent, last sibling, `SR2Mods.TeleportHint`); in the clone the `InputHint` and `InvokeInputEventOnPointerClick` components are destroyed (they would reconfigure it from the template's input event and fire it on click), `InputEventDisplay`, `LocalizeStringEvent` and `Selectable`s are disabled, no Graphic is a raycast target; the first `InputIcon` shows the key cap with "RMB" (as `InputIcon.SetKey`: `_buttonRoot` / `_doubleTapRoot` off, `_keyRoot` on, `_keyImage` / `_keyLocalizeStringEvent` off, `_keyText` = "RMB"), the label TMP (the one outside every icon) says "Teleport". Re-applied for 4 frames. Destroyed when the switch goes off or the map closes. Log: `[RMB] Teleport hint cloned from the map legend's '…'`.

## Fault policy

- Detours: the original is always called; the module's logic (record a pointer, flip `isRevealed` / the hidden result, set the click flag) runs in a `DetourGuard`. No IL2CPP calls inside hooks.
- Frame callback: `core::Try` + `ugui::GuardNative`; managed exceptions of single game calls are contained; a native fault disables the module for the session (references abandoned, no teardown calls). A frozen minimap (shared `ugui` bindings) stops this module too.
- No scene scans: the player (`TeleportablePlayer`) is found once with `FindFirstObjectByType` when a teleport needs it; the page's children are walked once per map open (static fog, container placement) and the legend once per open.

## Resolved names

MapUI (`_map`, `_viewModel`, `_fogReveal`, `_inputLegend`, `_mapDirector`, `_failedPlaceWaypointCue`, `LateUpdate`, `get_CurrentMapDefinition`, `TryFindZoneAt`), Map (`_belowMarkersContainer`, `_staticMarkerContainer`), MapViewModel (`_mapAnchor`, `_cursorOffset`, `_mapZoom`, `_mapRect`, `_worldRect`, `_fullMapRect`), MapUIFogReveal.`RevealZonesInMap`, FogMapElement.`SetRevealState`, MapDirector (`IsZoneHidden`, `GetMapForZone`), MapUIUtilities.`GetPlayerCurrentZone`, ZoneDetectionArea.`_zoneMapData`, MapClickPlaceMarker (`IPointerClickHandler.OnPointerClick`, `_isDragging`), PointerEventData.`<button>k__BackingField`, ComponentInterfaceBase.`_object`, InputLegend.`_hints`, InputHint, InputIcon (`_buttonRoot`, `_keyRoot`, `_keyText`, `_keyImage`, `_keyLocalizeStringEvent`, `_doubleTapRoot`), InputEventDisplay, InvokeInputEventOnPointerClick, LocalizeStringEvent, Selectable, TeleportablePlayer (`TeleportTo`(4), `get_SceneGroup`), UIDisplayManager (`get_Instance`, `CloseDisplayByObjectInHierarchy(Component)`), UIAudioCueHandle.`PlayAudioCue`, Physics.`Raycast`(6), RaycastHit.`m_Point`, Behaviour.`set_enabled`, Transform (`SetSiblingIndex`, `GetSiblingIndex`). Only `MapUI`, `Map`, `MapUI._map` and the `LateUpdate` hook are required; every feature degrades on its own (see the "resolved" log line).
