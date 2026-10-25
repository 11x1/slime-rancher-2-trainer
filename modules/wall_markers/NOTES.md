# wall_markers module notes

Game 1.3.0 (Unity 6000.0.73f1, HDRP, IL2CPP v31). Resolved by name at runtime; RVAs below are only for re-checking after an update.

## What the Nullifier is

- Localisation: Pedia entry id 505644092240850944 = "Nullifier", shared key `m.gadget.name.harmonizer`. Gigi's comm lines: "Use the Nullifier to open hidden paths in the Labyrinth. Try looking for walls that look strange, as if they were altered by prisma waves."
- Code: the gadget is `MonomiPark.SlimeRancher.Labyrinth.Harmonizer`; placing / removing it dispatches to the walls.

## The walls: `MonomiPark.SlimeRancher.Labyrinth.DiscordantWall`

RE (fields with offsets, Awake/OnDestroy registry, harmonize logic with RVAs): re/world.md (DiscordantWall, Harmonizer). Key facts: static `List<DiscordantWall> _activeWalls` (Add in Awake, Remove in OnDestroy); a placed Nullifier within 1 m of `_harmonizerDetectionCollider` harmonizes the wall (`_targetHarmonizationAmount` 1, solid collider off), picking it up disharmonizes it; inactive walls only queue harmonizers until `OnEnable`.

So the walls are not destroyed: they are "harmonized" (made passable) while a placed Nullifier is near and turn solid again when it is picked up. There is no per-wall save record and no "discovered" flag; the state follows the placed gadgets (saved as gadgets). The walls have no map marker of their own (not an `IStaticMapMarkerComponent`).

Intact = `_targetHarmonizationAmount < 0.5` (the target flips immediately; the current amount tweens).

## Discovery

No hooks, no scene scans: `DiscordantWall._activeWalls` is read through `il2cpp_class_get_static_field_data` + the field's static offset (null static data / null list = no wall has woken yet; we never run the class constructor). The list object's class must equal the field's declared type, every element must be an alive `DiscordantWall`. Walls with `isActiveAndEnabled == false` are skipped (their queued Nullifiers are only evaluated in `OnEnable`, so their state can be stale).

Position: centre of `_harmonizerDetectionCollider.bounds` (world AABB) when it has a size, else the wall's transform position.

**Inactive walls (full map).** In-game log (1.3.0): on entering the Labyrinth all 17 walls register and are active for a few seconds, then `17 registered, 17 alive, 0 active`: the walls are only `Awake`n once, while the scene group loads; afterwards every region away from the player is proxied (`Region.Root` deactivated), so their walls stay registered but inactive. The full map (`minimap::api::CollectPageIcons`, used by `map_tools`) therefore calls `Collect(out, stats, includeInactive = true)`: inactive walls are returned too, with their last harmonized flag and the collider-bounds centre remembered from when they were last active (a disabled collider reports empty bounds; `discordant_walls.cpp` keeps a wall -> centre map, dropped when the registry empties), else the transform position. The minimap and the world markers keep active walls only. Log: `minimap: full map: Nullifier walls A active + I inactive (unloaded regions, C positioned from when they were loaded), S shown` when the count changes.

Shared by the minimap (`modules/common/discordant_walls.*`).

## Markers

- Main thread, every 0.25 s: `walls::Collect` -> intact active walls (positions copied, no managed pointer kept).
- Main thread, every frame while intact walls exist: `Camera.main`, `get_worldToCameraMatrix`, `get_projectionMatrix` (Matrix4x4 by hidden return pointer, verified 0x33A9760 / 0x33A8370), vp = P * V, camera position = -R^T t, range filter, max 64 nearest, `ui::hud::Publish`.
- Hidden while the settings overlay is open, the cursor is not locked, or `Time.timeScale == 0` (menus / map / pause; same rule as the minimap's "menus open").
- Render thread: `ui/hud.cpp` projects with that vp (Unity/GL clip space, w > 0 in front) onto the ImGui display and draws on the foreground draw list; frames older than 250 ms are dropped. The overlay now renders an ImGui frame whenever markers are published (also with the settings window closed).

## Settings (module `wall_markers`)

| Key | Default | |
|---|---|---|
| `enabled` | on | the world-marker toggle |
| `range` | 60 m (10-250) | no line-of-sight check |
| `show_distance` | on | label under the marker |
| `size` | 1.0 (0.5-3) | |

Minimap: `minimap.icon_null_walls` (Icons, default on); fog applies like every world icon (`respect_fog`).

## Log lines

- `wall_markers: resolved (...)` at start.
- `wall_markers: Nullifier walls: N registered, A alive, B active, C intact (D harmonized), E positioned by collider bounds` - whenever the counts change (zone load, a wall opened / closed).
- `wall_markers: first markers drawn: ...` once.
- `minimap: icons (first scan): ... Nullifier walls I intact / A active, S shown ...`.
