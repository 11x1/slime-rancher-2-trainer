# spawn_scaling: resource spawn scaling and "More ores"

Plan Module 2 (resource spawn scaling) and Module 3 (More ores), with per-source control. Game 1.3.0.

All names are resolved at runtime through `ModuleContext` / `Resolver`; no RVAs, no hardcoded field
offsets. Two IL2CPP ABI constants are used (same kind as `unity::kArrayDataOffset`): the object header
size (value-type field offsets include it; the convention is detected at runtime from the first field's
offset) and inline struct sizes (exports `il2cpp_class_value_size` / `il2cpp_class_array_element_size`,
found with `GetProcAddress`, sanity-checked).

RE: `re/world.md` (ResourceNode harvest path, GetResourcesToSpawn body, node model / save),
`re/time-weather.md` (LightningStrike, tornado activities and spawners), `re/drones.md`
(ExplorerDroneConfig), `re/world.md` "Resource nodes" (offsets, RVAs).

## Sources of resources

| Source | Data patched | Implemented |
|---|---|---|
| Ground resource nodes | `ResourceNodeDefinition` `_minResources`/`_maxResources`, weighted `_resources[]` | yes, `nodes` |
| Lightning strikes | strike prefab's `LightningStrike.SpawnOptions` (`ResourceChance` Min/MaxQuantity; null Resource = "nothing"), frequency `SpawnActivity.SecondsBetweenSpawns` | yes, `lightning` |
| Cyclones | `SpawnTornadoActivity` / scene `StaticTornadoSpawner` `SpawnOptions` (struct list, MinCount/MaxCount), resource actor types only; frequency `SecondsBetweenSpawns` / `_min/_maxSpawnDelayGameHours` | yes, `cyclone` |
| Explorer drones | `ExplorerDroneConfig` `_lootTable[i].OutputQuantity` + `_defaultLootConfig` (RangeInt objects shared with DroneDirector's copies). They generate from static positions, not from live nodes, so they need their own source. | yes, `drones` (amount only) |
| Ranch drones | only move existing items | n/a |
| Gardens / wild trees | `ResourceGrowerDefinition`: yield path not decompiled | not done |
| Crates | `BreakOnImpact` counts are shared `ScriptedInt` assets | not done |
| Weather "release resources" | releases already-grown resources | not done |

## Resolve groups and failure isolation

- core (`ResourceNodeDirector._config` + `Awake` hook, `ResourceNodeDirectorConfig`,
  `ResourceSpawnerDefinition` + `GetResourcesToSpawn` hook): failure disables the module.
- weather / drones: failure disables only that source (settings not registered, warning).
- `WeightedResourceEntry`: failure disables only advanced mode for nodes.
- node lists (`_nodeSpawners`, `ResourceNodeSpawner._model`, model `nodeState` / `resourceNodeDefinition`
  / `resourcesToSpawn`, `ResourceNode._model` + `SpawnResources` hook, `NodeState.READY` read from
  metadata, `List<IdentifiableType>` `_items`/`_size`/`_version`/`set_Capacity`): failure disables only
  rescaling of already-rolled yields.
- harvest pacing (needs node lists; `_attachedResourceNode`, `_timeToHarvest`, `_resourceSpawnDelaySeconds`,
  `_breakMode`, `_harvestAtTime` with types checked, `SpawnSingleResource` hook, `NodeState.HARVESTING`):
  failure disables only pacing (`keep_harvest_time` not registered). Without just the hook the sweep still
  scales per-item times, but there is no schedule correction and no `node harvested` lines.

## Patch strategy

Data is patched in memory; no roll function's result is changed.

**Scans.** A main-thread frame callback runs `Resources.FindObjectsOfTypeAll` on
`ResourceNodeDirectorConfig`, `ResourceNodeDefinition`, `SpawnTemporaryObjectActivity` (incl. tornado
activities), `StaticTornadoSpawner`, `ExplorerDroneConfig`; a lightning activity's strike component is
`GetComponent(activity.Prefab)`. Time-sliced: one class per frame (5 frames per cycle), because each call
walks every loaded object (all five in one frame was ~80 ms, the periodic stutter). A cycle runs on
enable/disable, 2 s and 10 s after the director set changed (zone load), and every 300 s as a safety net
(30 s if `ResourceNodeDirector.get_AllResourceDirectors` does not resolve).

**Settings changes do not scan.** Debounced to one re-apply per 0.25 s, in one frame, on what is tracked:
configs and their weight-map definitions via `AllResourceDirectors`; weather / drone objects via a strong
GC handle per record (`IsAlive` checked, dead ones dropped); rolled lists of READY nodes. Untracked
definitions get new values on their next `GetResourcesToSpawn` (records carry the settings generation).
Only switching a lightning / cyclone / drone source from off to on starts a scan. GC handles are released
when a scan no longer finds the object, the source goes off, or on disable; leaked at process exit.

**Between cycles:** a light pass every 2 s walks `AllResourceDirectors` (static list filled in `Awake`,
emptied in `OnDestroy`), patches each `_config` and its `_resourceNodeWeightMap` definitions, detects zone
changes; no `FindObjectsOfTypeAll`, no managed allocation.

**Originals.** Stored per object pointer; every write is computed from them (never applied twice; 1.0 =
exact vanilla). Records keep the written values and the list / items / entry pointers; on mismatch (asset
reloaded, memory reused) current values become the new baseline. Records missing from a scan are dropped;
sources switched off are written back then dropped. **Disable** restores everything still alive.

**Never patched:** live lightning strikes (only the prefab; `Instantiate` copies it); `StaticTornadoSpawner`
objects not active in a scene hierarchy at first sight (excludes prefab assets; once tracked, stays).

**Hooks** (originals called unchanged, logic inside a `DetourGuard` each):
- `ResourceNodeDirector.Awake`: patches that director's `_config`.
- `ResourceSpawnerDefinition.GetResourcesToSpawn`: patches `this` before the call (only
  `ResourceNodeDefinition`; growers share the method), records the fresh rolled list after.
- optional patch-on-touch: `SpawnTemporaryObjectActivity.Spawn`, `SpawnTornadoActivity.Spawn`,
  `StaticTornadoSpawner.OnEnable`.
- `ResourceNode.SpawnResources` (pre): kept for managed callers, but nothing calls it natively in 1.3.0.
- `ResourceNode.SpawnSingleResource` (pre + post): the real vacuum harvest path (below).

### Rolled node lists

A node's yield is rolled once, when it spawns (`ResourceNodeSpawner.SpawnNode` stores
`GetResourcesToSpawn(false)` in `ResourceNodeSpawnerModel.resourcesToSpawn`, saved with the node), and a
harvest spawns exactly that list. So patching definitions only reaches nodes rolled afterwards (first test:
ore x10, Jellystone nodes still gave 2 each). The module also rescales stored lists of unharvested nodes:
- **Where:** every READY node reachable as `AllResourceDirectors[i]._nodeSpawners[j]._model` (loaded zones)
  on settings change, after each scan cycle and in every light pass; plus the harvested node at the first
  pull (`SpawnSingleResource` pre-hook; the vacuum path reads `list[0]` / `RemoveAt(0)`, no enumerator).
  Otherwise only READY lists (the break-node coroutine's enumerator would throw).
- **How:** records keyed by the list object keep the list as rolled / first seen (`base`) and per type the
  multiplier it already carries (`mBase`). Target per type = `count_t x m_now(t) / mBase(t)`, total rounded
  (>= 1; the roll cap only limits increases), split by largest remainder (`scaling_math.h`). Base items keep
  their order (`MinimumAmount` picks first), extra copies are appended, shrinking drops from each type's
  tail. `m_now` = `ore_amount_mult` / `res_<type>` / 1 if the nodes source is off or the module disabled.
- **Never double-applied:** a record is valid only while the list holds exactly what was last written;
  else it is re-baselined.
- **Provenance of `mBase`:** a list from the post-hook was rolled from a definition with the current
  settings (`mBase = m_now`; an unpatched definition counts as vanilla and is scaled at once). Unknown
  provenance (save, module off) counts as vanilla only if its length is inside the definition's vanilla
  `[_minResources, _maxResources]`; otherwise it is taken as already scaled (not compounded). Limit: at
  small multipliers a scaled length can lie inside the vanilla range and gets one extra factor next session.
- **Partial harvests:** a list equal to a proper tail of what was written is `partial`: never rescaled,
  disable leaves it, pacing ratio kept (before this, a 3-of-30 remainder counted as a vanilla roll and the
  node refilled).
- **List writes:** in place on the same `List<IdentifiableType>` (the model and save point at it): growth
  via managed `set_Capacity` (`Resolver::Invoke`), stores with `il2cpp_gc_wbarrier_set_field` when
  exported, `_size` set, `_version` incremented.
- **Disable** sweeps loaded zones back to vanilla-equivalent length (`m_now = 1`), then restores
  definitions. Unknown-provenance lists and unloaded zones are not touched.
- Records not seen in a sweep are pruned above 4096.

### Harvest pacing (`keep_harvest_time`)

A vacuum node yields one item per `_timeToHarvest` while the vac stays on it; a `_breakMode` node pops all
items after one `_timeToHarvest` wait, one per `_resourceSpawnDelaySeconds`. Both are per-instance fields
(liquid nodes are ordinary nodes with slower values). So x10 took 10x as long. Per node instance
(`spawner._attachedResourceNode`, validated: class assignable to `ResourceNode`, `m_CachedPtr` alive,
`node._model == spawner._model`):
- `ratio = vanillaEquivalentCount / writtenCount` (`Σ count_t / mBase_t`, clamped to <= 1: never slower
  than vanilla per item). Vacuum: `_timeToHarvest = orig x ratio`; break: `_resourceSpawnDelaySeconds =
  orig x ratio` (the single wait stays). Floor 0.001 s.
- Written in every sweep for READY nodes and at the first pull. Originals per instance pointer;
  mismatches re-baselined; ratio 1 restores and drops the record. Disable restores every node reachable
  from a live spawner; setting off restores READY nodes at the next re-apply. Prefabs are never touched.
- **Exact schedule** (`SpawnSingleResource` post-hook, paced vacuum nodes): vanilla re-arms with `now + t`
  from the frame that noticed the item was due, losing up to a frame per item. The post-hook sets
  `_harvestAtTime = due + t` (debt capped at 0.25 s) and, if the next item is already due, calls the
  original again (up to 8 extra, only while HARVESTING with a non-empty list). The original always runs
  outside the guard; `now` = `_harvestAtTime - t` as written by the original (no `Time` call).
- Limit: break nodes run from the game's coroutine (`WaitForSeconds`, >= 1 frame per item) and can run a
  few frames over at very short delays.

### Amount math

```
factor = !apply ? 1 : advanced ? res_<type> : ore_amount_mult
```

| Source | Global mode | Advanced mode | Not applied |
|---|---|---|---|
| Resource nodes | `_min/_maxResources` x ore, weights unchanged | weights `w_t x m_t`, count x `Σ(w·m)/Σw` (expected amount of t scales by exactly `m_t`) | vanilla |
| Lightning | each `ResourceChance` Min/MaxQuantity x ore; "nothing" entries unchanged | x `res_<type>` | vanilla incl. frequency |
| Cyclones | `MinCount/MaxCount` x ore, resource actor types only | x `res_<type>` | vanilla incl. frequency |
| Explorer drones | `OutputQuantity.Min/Max` x ore | x `res_<Item>`; the default entry (no item) uses ore | vanilla |

Rounding `round(v x factor)`; >= 1 never rounds to 0; max >= min; factor 1.0 keeps the vanilla pair. The
roll cap `max_yield_per_node` only limits increases. "Resource actor" (cyclones): class exactly
`IdentifiableType` with `IsAnimal` and `IsPlayer` false.

### Frequency

| What | Formula |
|---|---|
| Node director interval | `vanilla x respawn_interval_mult / spawn_rate`, floor 0.1 game hour (never applies above vanilla) |
| `_maxActiveNodes` | `round(vanilla x max_nodes_mult)`; hard cap (0 = auto = 5 x vanilla per zone) only limits increases. Bounded by the spawner count per director. |
| Lightning / cyclone frequency | `SecondsBetweenSpawns` / spawner delays ÷ `spawn_rate`, only while that source applies. Floors 1 s / 0.05 game hours. |
| Explorer drone interval | vanilla (only the amount is scaled) |

## Settings (module `spawn_scaling`)

| Group | Key | Label | Kind | Range | Default |
|---|---|---|---|---|---|
| Spawning | `spawn_rate` | Spawn rate multiplier | Float | 0.25–10 | 1 |
| Spawning | `max_nodes_mult` | Max active nodes multiplier | Float | 0.5–5 | 1 |
| Spawning | `respawn_interval_mult` | Respawn interval multiplier | Float | 0.1–5 | 1 |
| Spawning | `hard_cap_nodes` | Hard cap: active nodes per zone | Int | 0–500 (0 = auto) | 0 |
| More ores | `ore_amount_mult` | Ore amount multiplier | Float | 0.5–10 | 1 |
| More ores | `max_yield_per_node` | Max resources per roll | Int | 1–500 | 100 |
| More ores | `keep_harvest_time` | Resource nodes: keep vanilla harvest time | Bool (only if pacing resolved) | n/a | on |
| More ores | `src_nodes_apply` / `src_nodes_advanced` | Resource nodes: apply multiplier / advanced (per resource) | Bool | n/a | on / off |
| More ores | `src_lightning_apply` / `src_lightning_advanced` | Lightning strikes: … | Bool | n/a | on / off |
| More ores | `src_cyclone_apply` / `src_cyclone_advanced` | Cyclones: … | Bool | n/a | on / off |
| More ores | `src_drones_apply` / `src_drones_advanced` | Explorer drones: … | Bool | n/a | on / off |
| Per resource (advanced) | `res_<IdentifiableTypeName>` | prettified name | Float | 0.1–10 | 1 |

- Per-resource settings are collected while scanning and registered in sorted batches: ores first
  (seen in nodes, lightning or cyclones), then drone-only loot tagged "(drones)", each alphabetical. The
  first batch waits until node definitions were seen once (else after 90 s); later finds go into later
  batches; a full pass re-applies after registering.
- Labels: `IdentifiableType.localizedName` via `LocalizedString.GetLocalizedString()` (optional, once per
  resource); fallback the asset name without trailing `Craft`. Keys stay `res_<raw asset name>`.
- Lightning / cyclone / drone settings exist only if their names resolved. The old `include_weather` Bool
  was replaced by `src_lightning_apply` and `src_cyclone_apply`.
- `ore_amount_mult` max x10 means a 2..4 node gets at most 40, under the default roll cap 100. Raise the
  cap only after raising the slider maximum.

## Log lines

- Scan pass: `[spawn_scaling] pass: tracked [nodes N configs/M defs, lightning L, cyclone C, drones D] |
  changed [...] (new: ...) | node yields: N unharvested nodes in loaded zones, R rescaled (A -> B items) |
  multipliers | sources: ...` with `(scan X ms over 5 frames, max Y ms/frame)`. Info for enable / source
  on, passes that wrote or rescaled, tracked-set changes; Debug for unchanged safety rescans.
- Settings re-apply: `pass (settings: re-applied to tracked objects, no scan, X ms): ...`.
- First 5 per zone (budget resets on a director-set change, then a "suppressed" line):
  - `node spawned: <def> rolled N item(s) (...) | vanilla a..b, patched c..d (ore xF, nodes=...) | definition held the current settings` (or `definition was NOT patched -> list scaled after roll`)
  - `node harvested: <def> (vanilla a..b) gave N item(s) (...) in X s[ incl. K pause(s) with the vac off] | per item P s (vanilla V s, xR) -> vanilla amount ~A would take ~T s | list had M at first pull[, rescaled just now] [<origin>]` (origin: `rolled this session`, `vanilla roll (save / module off)`, `already scaled earlier (outside vanilla range)`). N should equal M; X about T.
  - `node partly harvested: <def> has k of M item(s) left (vac released) - left as it is`
- `node yields (<reason>): rescaled R of N unharvested nodes in loaded zones (A -> B items, X ms)`
- `harvest pacing: <def> (vacuum node | break node): _timeToHarvest|_resourceSpawnDelaySeconds O s -> P s per item (xR ...)` once per definition.

## Not done, on purpose

- No lifetimes or despawn timers (node lifetimes, `LightningStrike.DespawnTime`, tornado durations): 1.3.0
  has a known freeze when a Resource Detector's target despawns.
- `WeightedResourceEntry.MinimumAmount` and the lightning / cyclone / drone weights stay unchanged.
- Gardens, crates, `ReleaseResourcesActivity` (sources table). Vacpack slot capacity (inventory, not
  spawning). The game has no item or per-cell actor cap on the spawn path.

## Save safety

Nodes are persisted as `Persist.ResourceNodeV01` per fixed spawn point (record count never changes). Rolled
and rescaled lists are saved at their length (plain int list; vanilla reads any length) and keep it after
uninstall (yield once). Disabling in game first shrinks tracked lists in loaded zones. After uninstall a
zone may have more active nodes than its vanilla max until they despawn. Weather resources are loose
actors; drone output is capped by the cloud's stack limit. Still to verify: the uninstall-then-load test.

## Verified vs assumed

Verified: all names, argc, field types and offsets against both dumps; hook bodies unique; by disassembly
the roll / store / harvest path, the per-item time fields and that `SpawnResources()` has no native caller;
/W4 clean build.

Assumed, not yet checked:
1. The director reads `MaxActiveNodes` / `UpdateNodesIntervalHours` on every use; `_nextNodeUpdateTime` is
   not rescheduled, so a shorter interval applies after the refill already scheduled.
2. `LightningStrike` reads SpawnOptions at strike time and strikes are not pooled (`UseFXPool` exists; no
   reset method).
3. Tornado activities / spawners hand SpawnOptions to `PeriodicActorSpawner.Init`; live cyclones may keep
   copied values.
4. `SpawnActivity` re-rolls the next spawn time after each spawn.
5. Explorer drones use `OutputQuantity` (inclusive) at collection time.
6. Drift Crystal is a plain `IdentifiableType` with `IsAnimal == false`.
7. `Outer/Inner` nested names work with `il2cpp_class_from_name` (failure disables only that feature).
