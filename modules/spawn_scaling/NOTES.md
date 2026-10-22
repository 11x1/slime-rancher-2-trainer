# spawn_scaling: resource spawn scaling and "More ores"

This module covers plan Module 2 (resource spawn scaling) and Module 3 (More ores), with per-source control. Target: game 1.3.0, IL2CPP metadata v31.

All names are resolved at runtime through `ModuleContext` / `Resolver`. The module uses no RVAs and no hardcoded field offsets. The RVAs below come from the dump and are listed only for cross-checking in IDA.

Two IL2CPP runtime ABI constants are used, the same kind as `unity::kArrayDataOffset`:
- The object header size (2 pointers). It is used to turn value-type field offsets into offsets inside an inline element. The code detects at runtime which convention applies, using the first field's offset.
- Inline struct sizes. These come from the standard exports `il2cpp_class_value_size` / `il2cpp_class_array_element_size`, looked up by name with `GetProcAddress` and sanity-checked.

## Sources of resources (from the dump)

| Source | Where the amount comes from | Implemented |
|---|---|---|
| Ground resource nodes (ores, etc.) | `ResourceNodeDefinition` (ScriptableObject : `ResourceSpawnerDefinition`): `_minResources`/`_maxResources` and weighted `_resources[]` (`WeightedResourceEntry {IdentifiableType ResourceIdentifiableType, float Weight, int MinimumAmount}`) | yes, source `nodes` |
| Lightning strikes | `MonomiPark.SlimeRancher.World.LightningStrike` (MonoBehaviour on a prefab), `List<ResourceChance> SpawnOptions`, `ResourceChance {IdentifiableType Resource (null = "spawn nothing"), float Weight, int MinQuantity, int MaxQuantity}` ("how many will spawn if this is chosen, flat distribution"). Strikes are spawned by a weather `SpawnTemporaryObjectActivity` asset (`Prefab` = the strike prefab, `SecondsBetweenSpawns` = `Range {Min, Max}` seconds). | yes, source `lightning` |
| Cyclones | `Weather.Activity.SpawnTornadoActivity` (ScriptableObject) and the scene-placed `MonomiPark.SlimeRancher.StaticTornadoSpawner`. Both have `List<PeriodicActorSpawner.SpawnOption> SpawnOptions`, where `SpawnOption` is a struct `{IdentifiableType ActorType, int Weight, int MinCount, int MaxCount, float VerticalForce, float LateralForce, Range RandomSleepRange}`. A spawn burst picks an option by weight and spawns MinCount..MaxCount. Options mix resources (Drift Crystal) with slimes. Frequency comes from `SecondsBetweenSpawns` (activity) or `_min/_maxSpawnDelayGameHours` (static spawner). | yes, source `cyclone`; resource actor types only |
| Explorer drones | `MonomiPark.SlimeRancher.Drone.ExplorerDroneConfig` (ScriptableObject): `List<LootTableEntry> _lootTable` plus `LootTableEntry _defaultLootConfig` (used for items not in the table). `LootTableEntry` is a struct `{IdentifiableType Item, int Weight, int MaxSourcesConsidered, Range GenerationIntervalHours, Util.RangeInt OutputQuantity, float MinimumTimeFactor}`. `Util.RangeInt` is a class `{int Min, int Max}`. `DroneDirector` copies the table into `Dictionary<IdentifiableType, LootTableEntry> _explorerDroneLootTable`. Those struct copies share the same `RangeInt` objects, so patching `OutputQuantity.Min/Max` reaches them. Explorer drones generate items from this table, based on the static `DiscoverableResourceData` positions near the station. They do **not** harvest the live ResourceNode spawns, so the node patch does not reach them; they need their own source. | yes, source `drones` (amount only) |
| Ranch drones | `RanchDrone*` source and sink programs (garden, coop, corral, silo, pond, plort collector and so on). They only move items that already exist from a source to a sink. They have no amount or yield data. | n/a: nothing to scale. Garden and coop output reaches them unchanged. |
| Gardens / wild fruit trees | `ResourceGrowerDefinition` / `SpawnResource`. It shares the `ResourceSpawnerDefinition` base, but its yield path (`SpawnJoints`, `_maxActiveSpawns`) has not been decompiled. It is unclear whether `_min/_maxResources` are used. | not done (can be added once confirmed in IDA) |
| Crates | `BreakOnImpact`: `_minActorSpawns`/`_maxActorSpawns` are `ScriptedInt` ScriptableObjects, which can be shared with other systems. | not done: patching shared ScriptedInts is not clean |
| Weather "release resources" | `ReleaseResourcesActivity` `Min/MaxResourcesPerReleaseEvent` releases already-grown resources from existing grower spawners. It creates nothing new. | not done |

## Resolved names (Assembly-CSharp)

| Part | Class (namespace) | Members | Dump RVA / offset |
|---|---|---|---|
| core | `ResourceNodeDirector` (`MonomiPark.SlimeRancher.World.ResourceNode`) | `_config`; hook `Awake()`/0 | 0x28; 0x66B350 (unique body) |
| core | `ResourceNodeDirectorConfig` (same namespace) | `_maxActiveNodes`, `_updateNodesIntervalHours` | 0x20, 0x24 |
| core | `ResourceSpawnerDefinition` (global) | `_resources`, `_minResources`, `_maxResources`; hook `GetResourcesToSpawn(bool)`/1 | 0x20, 0x28, 0x2C; 0x53C5C0 (unique body) |
| core | `ResourceNodeDefinition` (global) | enumerated | n/a |
| nodes, advanced (optional) | `ResourceSpawnerDefinition/WeightedResourceEntry` | `ResourceIdentifiableType`, `Weight` | 0x10, 0x18 |
| weather (optional) | `IdentifiableType` (global) | `IsPlayer`, `IsAnimal` | 0x18, 0x19 |
| weather | `LightningStrike` (`MonomiPark.SlimeRancher.World`) | `SpawnOptions` | 0x48 |
| weather | `LightningStrike/ResourceChance` | `Resource`, `MinQuantity`, `MaxQuantity` | 0x10, 0x1C, 0x20 |
| weather | `SpawnActivity` (`MonomiPark.SlimeRancher.Weather.Activity`) | `SecondsBetweenSpawns` | 0x20 |
| weather | `SpawnTemporaryObjectActivity` (same namespace) | `Prefab` | 0x40 |
| weather | `SpawnTornadoActivity` (same namespace) | `SpawnOptions` | 0x70 |
| weather | `StaticTornadoSpawner` (`MonomiPark.SlimeRancher`) | `SpawnOptions`, `_minSpawnDelayGameHours`, `_maxSpawnDelayGameHours` | 0x48, 0x60, 0x64 |
| weather | `PeriodicActorSpawner/SpawnOption` (`MonomiPark.SlimeRancher.World`, struct) | `ActorType`, `MinCount`, `MaxCount` (unboxed 0x0 / 0xC / 0x10, size 0x28) | n/a |
| weather | `Range` (global) | `Min`, `Max` | 0x10, 0x14 |
| drones (optional) | `ExplorerDroneConfig` (`MonomiPark.SlimeRancher.Drone`) | `_defaultLootConfig` (inline struct), `_lootTable` | 0x20, 0x48 |
| drones | `ExplorerDroneConfig/LootTableEntry` (struct) | `Item`, `OutputQuantity` (unboxed 0x0 / 0x18, size 0x28) | n/a |
| drones | `RangeInt` (`MonomiPark.SlimeRancher.Util`) | `Min`, `Max` | 0x10, 0x14 |
| runtime | `List<T>` | `_items` (by name, per generic instance) | n/a |
| node lists (optional) | `ResourceNodeDirector` | `_nodeSpawners` (`List<ResourceNodeSpawner>`) | 0x38 |
| node lists | `ResourceNodeSpawner` (global) | `_model` | 0x38 |
| node lists | `ResourceNodeSpawnerModel` (`MonomiPark.SlimeRancher.DataModel`) | `nodeState`, `resourceNodeDefinition`, `resourcesToSpawn` (`List<IdentifiableType>`) | 0x28, 0x38, 0x48 |
| node lists | `ResourceNode` (global) | `_model`; hook `SpawnResources()`/0 | 0xE0; 0x53B990 (unique body) |
| node lists | `ResourceNode/NodeState` (enum) | `READY` value read with `il2cpp_field_static_get_value` (1 in 1.3.0) | n/a |
| node lists | `List<IdentifiableType>` (from `il2cpp_field_get_type` of `resourcesToSpawn`) | `_items`, `_size`, `_version`, `set_Capacity(int)` | n/a |
| harvest pacing (optional) | `ResourceNodeSpawner` | `_attachedResourceNode` | 0x40 |
| harvest pacing | `ResourceNode` | `_timeToHarvest` (float), `_resourceSpawnDelaySeconds` (float), `_breakMode` (bool), `_harvestAtTime` (double); hook `SpawnSingleResource()`/0. Field types are checked with `il2cpp_type_get_type`. | 0x3C, 0x6C, 0x90, 0xA8; 0x53BA90 (unique body) |
| harvest pacing | `ResourceNode/NodeState` | `HARVESTING` (2 in 1.3.0), read from metadata | n/a |

Failure isolation:
- A failure in the core group disables the module.
- A failure in the weather or drones group disables only that source: its settings are not registered and a warning is logged.
- A failure resolving `WeightedResourceEntry` disables only advanced mode for nodes.
- A failure in the node-lists group disables only the rescaling of already-rolled node yields (warning; new rolls still scale through the definitions). A missing `SpawnResources` hook leaves the periodic sweep.
- A failure in the harvest-pacing group (needs node lists) disables only the pacing: the `keep_harvest_time` setting is not registered, and scaled nodes take N× longer to harvest. If only the `SpawnSingleResource` hook is missing, the sweep still scales the per-item time, but the schedule is not corrected for frame rounding and there are no `node harvested` lines.

## Patch strategy

Data is patched in memory. No roll function is hooked to change its result.

**When patching runs.** A main-thread frame callback runs `Resources.FindObjectsOfTypeAll` on these types:
- `ResourceNodeDirectorConfig`, `ResourceNodeDefinition`, `SpawnTemporaryObjectActivity` (which includes tornado activities), `StaticTornadoSpawner` and `ExplorerDroneConfig`.
- For a lightning activity, the strike prefab's `LightningStrike` component is found with `GetComponent(activity.Prefab)`.

The scan is **time-sliced**: one `FindObjectsOfTypeAll` class per frame (5 frames per cycle). Each call walks every loaded object and allocates a managed array; the old version ran all five in one frame every 2 s (about 80 ms per pass, inferred from log timestamps: enable at 18:45:07.407, pass line at .491, the follow-up pass at .579), which was the periodic stutter. A cycle runs on enable/disable, 2 s and 10 s after the resource director set changed (zone load), and every 300 s as a safety net (every 30 s if `ResourceNodeDirector.get_AllResourceDirectors` does not resolve).

**Settings changes do not scan.** The 19:21 test log showed a full 5-frame cycle (~90 ms, max ~19 ms/frame) started every 0.5 s while a slider was dragged. Now a settings change (debounced to one re-apply per 0.25 s) re-applies the new values in one frame to what is already tracked:
- configs and their weight-map node definitions through the live `AllResourceDirectors` list,
- weather and drone objects through a strong GC handle each record holds (checked with `IsAlive` before writing; dead ones are dropped),
- the rolled lists of READY nodes (below).

Node definitions that no live director references get the new values on their next `GetResourcesToSpawn` (each record carries the settings generation it was applied with, so the patch-on-touch fast path re-applies after a change). The only settings change that still starts a scan is switching a lightning/cyclone/drone source from off to on, because that source's objects are not tracked while it is off. The re-apply writes one Info line: `pass (settings: re-applied to tracked objects, no scan, X ms): tracked [...] | changed [...] | node yields: N unharvested nodes in loaded zones, R rescaled (A -> B items) | multipliers | sources`.

The GC handles keep the few tracked weather/drone objects (2 lightning, 1 cyclone, 1 drone config in the test) from being collected or unloaded while tracked; they are released when a scan no longer finds the object, when the source is switched off, and on disable. At process exit they are leaked on purpose (no IL2CPP calls during teardown).

Between cycles:
- A light pass every 2 s walks `ResourceNodeDirector.AllResourceDirectors` (static list filled in `Awake`, emptied in `OnDestroy`), patches each `_config` and the node definitions in its `_resourceNodeWeightMap`, and detects zone changes. It makes no `FindObjectsOfTypeAll` call and no managed allocation.
- Patch-on-touch hooks: `ResourceNodeDirector.Awake`, `ResourceSpawnerDefinition.GetResourcesToSpawn`, and (optional) `SpawnTemporaryObjectActivity.Spawn`, `SpawnTornadoActivity.Spawn`, `StaticTornadoSpawner.OnEnable` (unique bodies at 0x66DB30, 0x66DBF0, 0x640BF0 in the 1.3.0 dump).

**Originals and re-apply.**
- Original values are stored per object pointer, and every write is computed from them. Values are never applied twice, and at 1.0 everything is exact vanilla.
- Each record also keeps the values the module wrote, and the pointers to the list, items array and entry objects.
- If those no longer match (the asset was reloaded, or the memory was reused by a different object), the current values become the new baseline.
- Records for objects missing from a scan are dropped. Records for sources whose "apply" was switched off are dropped after their values are written back.

**Disable** restores everything that is still alive and forgets all pointers.

**What is never patched:**
- Live lightning strikes. Only the prefab component is patched, and `Instantiate` copies its SpawnOptions into each new strike.
- `StaticTornadoSpawner` objects that are not active in a scene hierarchy at first sight. This excludes prefab assets, which avoids double-applying to their runtime instances. Once tracked, an object stays tracked.

**Patch-on-touch hooks** (originals called unchanged, mod logic inside a `DetourGuard` each):
- `ResourceNodeDirector.Awake` patches that director's `_config`.
- `GetResourcesToSpawn` patches `this` before the call, but only for `ResourceNodeDefinition`. Growers are skipped. After the call it records the fresh rolled list (below).
- `ResourceNode.SpawnResources` (before the call) brings the harvested node's rolled list to the current settings. **Nothing calls it natively in 1.3.0** (byte scan of `GameAssembly.dll` for call/jmp to 0x53B990 finds none), so this hook never fired in the 19:59 test. That is why the log had no `node harvested` lines. It is kept for any managed caller.
- `ResourceNode.SpawnSingleResource` (the real harvest path, see "Harvest path and pacing" below), before and after the call.

### Rolled node lists: what a harvest actually yields

In-game test (19:21–19:27): `ore_amount_mult` = 10, the log showed both passes changing "22 defs", and yet 2 Jellystone nodes gave 4 Jellystone in total (2 each, vanilla). Cause, confirmed by disassembly of the 1.3.0 `GameAssembly.dll` (capstone, no IDA):
- `ResourceSpawnerDefinition.GetResourcesToSpawn(forceMax)` (0x53C5C0): `count = forceMax ? _maxResources : Randoms.GetInRange(_minResources, _maxResources + 1)`; first, for each `_resources` entry with `MinimumAmount > 0`, it adds `min(count, MinimumAmount)` of that type and subtracts it; then it adds `count` weighted random picks. The list length is exactly the rolled count.
- `ResourceNodeSpawner.SpawnNode(def)` (0x53A7C0) sets `model.nodeState = READY`, `model.resourceNodeDefinition = def` and `model.resourcesToSpawn = def.GetResourcesToSpawn(false)`. This is the only roll; the model is saved (`Persist.ResourceNodeV01`).
- `ResourceNode.SpawnResources()` (0x53B990) starts `SpawnResourcesAsync`, whose `MoveNext` (0x53FC80) enumerates `_model.resourcesToSpawn` and instantiates one actor per entry (`InstantiationHelpers.InstantiateActor`), with `_resourceSpawnDelaySeconds` between them. Then the node goes to the empty state.

So the definition patch worked, but only nodes rolled after it got the bigger yield. Every node that already existed (rolled while the module was off — it was disabled 19:22:24–19:25:50 — rolled before the change, or loaded from the save) kept its stored vanilla list. The 5 `hook:spawn_scaling.touch` calls after the 19:25:50 enable show how few new rolls happen in a few minutes.

Fix: the module also rescales the stored lists of unharvested nodes.
- **Where:** every READY node reachable as `AllResourceDirectors[i]._nodeSpawners[j]._model` (loaded zones), on settings change, after each scan cycle, in every 2 s light pass (zone loads, nodes from the save), plus the harvested node itself at the first pull of a harvest (`SpawnSingleResource` pre-hook; the vacuum path reads `list[0]` / `RemoveAt(0)`, so there is no enumerator). Only READY lists are touched otherwise: the break-node coroutine's enumerator would throw on a modified list.
- **How:** records are keyed by the list object and keep the list as rolled / first seen (`base`) and, per resource type, the multiplier that base already carries (`mBase`). The target is `count_t × m_now(t) / mBase(t)` per type, the total rounded (≥ 1, the roll cap only limits increases) and split by largest remainder (`scaling_math.h`). Base items are kept in order (the `MinimumAmount` picks come first) and extra copies are appended; shrinking drops from the tail of each type. `m_now` follows the definition rule: `ore_amount_mult` (global), `res_<type>` (advanced), 1 if the nodes source is off or the module is disabled.
- **Never double-applied:** every write is computed from `base`. A record is valid only while the list still holds exactly what was last written; otherwise it is re-baselined from what it holds now.
- **Provenance of `mBase`:** a list seen in the `GetResourcesToSpawn` post-hook was rolled from a definition that held the current settings (`mBase = m_now` at roll time). If the definition was not patched, it counts as vanilla and is scaled right away. A list of unknown provenance (from the save, or rolled while the module was off) counts as vanilla only if its length lies inside the definition's vanilla `[_minResources, _maxResources]`. Otherwise it was scaled earlier (an earlier session) and is taken as matching the current settings (`mBase = m_now`), so it is not compounded; it changes only when the multiplier changes afterwards. Limit: at small multipliers (for example ×1.25 on 2..4) a scaled length can still lie inside the vanilla range and gets scaled once more on the next session (bounded by one extra factor).
- **List writes:** in place on the same `List<IdentifiableType>` object (the model and the save keep pointing at it): growth through the managed `set_Capacity` (`Resolver::Invoke`), element stores with `il2cpp_gc_wbarrier_set_field` when exported, `_size` set, `_version` incremented.
- **Disable** first sweeps the loaded zones and brings every tracked list back to its vanilla-equivalent length (`m_now = 1`; a list rolled under ×10 shrinks to a tenth), then the definitions are restored. Lists of unknown provenance are not touched. Lists in zones that are not loaded at that moment keep their scaled length.
- **Save:** rescaled existing nodes are saved with the longer (or shorter) list. It is a plain list of resource ids that vanilla loads at any length, so this is save-compatible; after uninstall those nodes simply yield the stored amount once.
- Records not seen in a sweep are pruned once there are more than 4096.

**Log lines for the next test** (Info, first 5 of each per zone; the budget resets when the director set changes):
- `node spawned: <definition> rolled N item(s) (<Type xN, ...>) | vanilla a..b, patched c..d (ore xF, nodes=global|advanced|off) | definition held the current settings` (or `definition was NOT patched -> list scaled after roll`).
- `node harvested: <definition> yields N item(s) (<Type xN, ...>) | stored list had M[, rescaled just now] [<origin>] | vanilla a..b, patched c..d (ore xF, nodes=...)`. The origin is `rolled this session`, `vanilla roll (save / module off)` or `already scaled earlier (outside vanilla range)`. If "yields" shows the vanilla count with ore ×10, the harvest is not coming from this list.
- `node yields (<reason>): rescaled R of N unharvested nodes in loaded zones (A -> B items, X ms)` when a sweep rewrote lists (light pass / disable). Settings re-applies and scan passes include the same numbers in their pass line.
- After the 5th line of a kind: `(further 'node spawned' / 'node harvested' lines suppressed until the next zone change)`.
- **New (harvest pacing):**
  - `node harvested: <def> (vanilla a..b) gave N item(s) (<Type xN>) in X s[ incl. K pause(s) with the vac off] | per item P s (vanilla V s, xR) -> vanilla amount ~A would take ~T s | list had M at first pull[, rescaled just now] [<origin>[, partly harvested before]]`. Logged when a vacuum harvest empties the node (SpawnSingleResource path), first 5 per zone. X should be about T when nothing paused. N should equal M.
  - `harvest pacing: <def> (vacuum node: one item per interval | break node: items pop after one wait): _timeToHarvest|_resourceSpawnDelaySeconds O s -> P s per item (xR = vanilla amount / scaled amount; ...)`, once per definition per session. It shows the real per-item times of liquid nodes and other nodes, and which mode each node uses.
  - `node partly harvested: <def> has k of M item(s) left (vac released) - left as it is`, first 5 per zone.
  - Sweep and pass lines gained `harvest pacing on P node(s)` / `, P paced`. The settings summary gained `harvest time vanilla|per item|n/a`.

### Harvest path and pacing (keep the vanilla harvest time)

User feedback after the 19:59 test: scaling works, but a ×10 node takes ten times longer to harvest. Disassembly of the 1.3.0 `GameAssembly.dll` (capstone):
- `ResourceNode.UpdateForState` (0x53BED0), called every frame for every node:
  - **READY** (1): when the vac touches the node (`Vacuumable.Pending` = touched within the last 2 frames) and `_isHarvestable`, it sets the state to HARVESTING and `_harvestAtTime = now + _timeToHarvest`.
  - **HARVESTING** (2), vac still on the node, normal node: if the list is not empty and `now >= _harvestAtTime`, it calls `SpawnSingleResource()`.
  - **HARVESTING**, `_breakMode` node: once `now >= _harvestAtTime`, it starts `SpawnResourcesAsync` **inline** (not through `SpawnResources()`), plays the FX and sets the node EMPTY.
  - **HARVESTING**, vac no longer on the node: back to READY, with the rest of the list. This is a partial harvest.
- `SpawnSingleResource` (0x53BA90; its only caller is `UpdateForState`): sends the harvested event, sets `_harvestAtTime = now + _timeToHarvest`, instantiates `list[0].prefab`, calls `RemoveAt(0)`, and goes EMPTY when the list is empty. **One item per `_timeToHarvest` seconds**, at most one per frame.
- `SpawnResourcesAsync.MoveNext` (0x53FC80): for each list entry it instantiates the item, then waits `new WaitForSeconds(this._resourceSpawnDelaySeconds)`. The field is read live at each step. There is no item cap.
- So a harvest takes `count × _timeToHarvest` (vacuum nodes), or `_timeToHarvest + count × _resourceSpawnDelaySeconds` (break nodes). Both fields are serialized per **ResourceNode instance** (from the node prefab variant), not on the definition and not in a ScriptedFloat. Liquid nodes (Deep Brine, Primordy Oil, ...) are ordinary `ResourceNode`s with their own prefab values (the user reports they are slower per item), so they are covered by the same per-instance fields. Their actual values and mode get logged on the first pacing per definition (see the log lines below).

**Pacing.** Per node instance, reached as `spawner._attachedResourceNode` and validated (class assignable to `ResourceNode`, `m_CachedPtr` alive, `node._model == spawner._model`):
- `ratio = vanillaEquivalentCount / writtenCount`, from the rolled-list record: `Σ count_t / mBase_t` over the list as written, clamped to at most 1.
- Vacuum node: `_timeToHarvest = orig × ratio`. Break node: `_resourceSpawnDelaySeconds = orig × ratio`, while the single break wait `_timeToHarvest` stays vanilla. Floor 0.001 s.
- **Never slower than vanilla per item.** With an ore multiplier below 1 the node has fewer items at the vanilla per-item time.
- Written in every node sweep for READY nodes (settings, light pass, scan, zone load) and at the first pull of a harvest. Originals are kept per instance pointer. A value that no longer matches what was written is re-baselined. Writing ratio 1 restores the original and drops the record.
- **Disable** restores the timing of every node reachable from a live spawner, in any state. The `keep_harvest_time` setting set to off restores the READY nodes in the next re-apply. Nodes in unloaded zones are destroyed with their zone: the values are per instance and the prefab is never touched.

**Exact schedule (SpawnSingleResource post-hook).** Vanilla re-arms with `now + t`, measured from the frame that noticed the item was due. That loses up to one frame per item, which matters at a paced `t` of a few frames: for example ×10 on a 0.3 s node gives 0.03 s and about 2 frames at 60 fps, about 10 % over the target. For a paced vacuum node the post-hook sets `_harvestAtTime = due + t`, where `due` is the value before the call, and caps the carried debt at 0.25 s (frame hitch). If the next item is already due in this frame, it calls the original `SpawnSingleResource` again, up to 8 extra times. Each extra call runs only while the state is still HARVESTING and the list is not empty: the same conditions the game checked in that frame, with the vac on the node. The original always runs outside the `DetourGuard`. `now` is derived from the value the original wrote (`_harvestAtTime − t`), so `Time` is never called. The total vacuum time is then `scaledCount × t × ratio` = the vanilla amount × t.

Limit: break nodes run their items from the game's coroutine (`WaitForSeconds`, at least one frame per item). At very short paced delays they can run a few frames over the vanilla time. Hooking the compiler-generated `MoveNext` was not worth it, because the player does not have to hold the vac for those.

**Partial harvests** (a fix found on the way). After the vac is released, the node is READY with the tail of its list. Before this fix the sweep saw a list different from what it had written and re-baselined it as unknown provenance. A remainder inside the vanilla range (for example 3 left of 30 for a 2..4 node) then counted as a vanilla roll and was scaled ×10 again: the node refilled. Now a list that equals a proper tail of what was written is marked `partial`: it is never rescaled, disable leaves it alone, and its pacing ratio stays the same.

### The "hard cap" (item 2 of the 20:0x feedback)

Checked:
- **Our caps.** `max_yield_per_node` (default 100) only limits increases. In the 19:59–20:08 log no node got near it: every node definition seen is vanilla 2..4, patched 20..40 at ×10, and the lists hold 22–34 items. `ore_amount_mult` has a slider maximum of ×10, so 40 is the most a 2..4 node can get. Neither cap is reached at the defaults, so neither was changed. Raise `max_yield_per_node` only after raising the slider maximum.
- **Game, spawn path.** No item cap: `SpawnSingleResource` and `MoveNext` spawn the whole list, and `InstantiationHelpers.InstantiateActor` (0x870C40) always instantiates (`GameModel.InstantiateActorModel`, `Object.Instantiate`, `RegisterDynamicObject`). The dump has no max-actors or per-cell limit field (searched for max/limit/cap + actor names). The only "max active" values are `_maxActiveNodes` (nodes, not items) and growers' `_maxActiveSpawns`.
- **No harvested counts in the log.** The old harvest hook was on a dead method, so the log has no "harvested N of M" data. The new `node harvested` line provides it.
- **Likely causes of the "cap" feeling, all from the harvest mechanics:**
  1. Items come out only while the vac stays on the node. One pause sets it back to READY, and the rest waits in the node (now logged as `node partly harvested`). At ×10 without pacing that took 10× as long. Pacing fixes the duration.
  2. The vacpack slot has the game's own capacity limit, which upgrades raise. 20–40 items of one type from one node can exceed a slot, so the rest stays on the ground. That is inventory, not spawning, and this module does not touch it (a separate feature with save/UI implications).
  3. A node despawns at its lifetime (`despawnAtWorldTime`). A long ×10 harvest was more likely to be cut off by that. Lifetimes are deliberately untouched (see "Not done").

The next test's `node harvested` lines ("gave N item(s) ... list had M at first pull") will show directly whether N < M ever happens.

### Amount math ("More ores")

For each source:

```
factor = !apply    ? 1 (vanilla)
       : advanced  ? res_<type>          (per-resource multiplier, replaces the global one)
       :             ore_amount_mult
```

| Source | Global mode | Advanced mode | Not applied |
|---|---|---|---|
| Resource nodes | `_min/_maxResources` × ore. Weights unchanged. | Weights `w_t × m_t`, count × `Σ(w·m)/Σw`. The expected amount of resource t scales by exactly `m_t`. | vanilla |
| Lightning | each `ResourceChance` Min/MaxQuantity × ore. "Nothing" entries are never changed. | × `res_<type>` | vanilla, including frequency |
| Cyclones | `MinCount/MaxCount` × ore, for resource actor types only | × `res_<type>` | vanilla, including frequency |
| Explorer drones | `OutputQuantity.Min/Max` × ore | × `res_<Item>`. The default entry has no item, so it uses the global ore multiplier. | vanilla |

Rounding: `round(v × factor)`. A value of 1 or more never rounds down to 0. max ≥ min. At factor 1.0 the vanilla pair is kept untouched. The roll cap `max_yield_per_node` only limits increases.

What counts as a "resource actor" for cyclones: the class is exactly `IdentifiableType`, and `IsAnimal` and `IsPlayer` are false. `SlimeDefinition`, `GadgetDefinition`, `ToyDefinition` and `LiquidDefinition` are subclasses, so they are excluded.

### Frequency

| What | Formula |
|---|---|
| Node director interval | `vanilla × respawn_interval_mult / spawn_rate`, with a 0.1 game-hour floor. The floor never applies above vanilla. |
| `_maxActiveNodes` | `round(vanilla × max_nodes_mult)`. The hard cap (0 = auto = 5 × vanilla per zone) only limits increases. |
| Lightning and cyclone spawn frequency | `SecondsBetweenSpawns` / StaticTornadoSpawner delays ÷ `spawn_rate`, but only while that source's "apply" is on. Floors: 1 s and 0.05 game hours. |
| Explorer drone gather interval | Left vanilla (`GenerationIntervalHours`, `MinimumTimeFactor`). Only the amount is scaled. |

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

- `enabled` is registered by the ModuleManager.
- Per-resource settings are collected while scanning (node `_resources`, lightning/cyclone SpawnOptions, explorer loot table) and registered in sorted batches.
  - **Order:** ores first, meaning resources seen in nodes, lightning or cyclones, alphabetical. Then resources seen so far only in explorer-drone loot (plorts, Moondew Nectar, ...), alphabetical and tagged "(drones)".
  - **Timing:** the first batch waits until node definitions have been seen once. They load with the game scene, after the weather and drone assets. If nodes never appear, the batch registers after 90 s of scanning. Resources found later go into later batches.
  - **Re-apply:** after registering, a full pass re-applies, so saved per-resource values take effect.
  - **Labels:** the localized display name, from `IdentifiableType.localizedName` via `UnityEngine.Localization.LocalizedString.GetLocalizedString()` (argc 0, Unity.Localization). It is resolved by name and is optional; it is called once per new resource through `Resolver::Invoke`. If that fails or returns an error or an implausible text, the label is the asset name with the trailing `Craft` removed, e.g. `RadiantOreCraft` becomes "Radiant Ore".
  - **Keys** are unchanged: `res_<raw asset name>`, e.g. `res_RadiantOreCraft`.
- **Pass summary log.** Each scan pass writes one line: `[spawn_scaling] pass: tracked [nodes N configs/M defs, lightning L, cyclone C, drones D] | changed [...] (new: ...) | node yields: N unharvested nodes in loaded zones, R rescaled (A -> B items) | multipliers | sources: nodes=global|advanced|off, ...`.
  - It is logged at Info for passes caused by enable (or a source switched on), passes that wrote values or rescaled node lists, and passes where the tracked set changed (zone load).
  - Unchanged safety rescans log at Debug. The line includes the scan cost: `(scan X ms over 5 frames, max Y ms/frame)`.
  - A settings change logs the `pass (settings: re-applied to tracked objects, no scan, X ms)` line instead (see "When patching runs").
- Lightning and cyclone settings exist only if the weather names resolved; drone settings only if the drone names resolved.
- The earlier single `include_weather` Bool was replaced by `src_lightning_apply` and `src_cyclone_apply`.
- The tooltips say changes apply from the next spawn roll and that unharvested resource nodes in loaded zones are rescaled too.

## Not done, on purpose

- **No lifetimes or despawn timers** are touched: node lifetimes, `LightningStrike.DespawnTime`, tornado durations. 1.3.0 has a known freeze when a Resource Detector's target despawns.
- **`WeightedResourceEntry.MinimumAmount`** and the lightning, cyclone and drone weights are left unchanged. Advanced mode re-weights only the nodes' `_resources`.
- **Gardens/growers, crates and ReleaseResourcesActivity**: see the sources table above.

## Save safety (from the dump)

Nodes are persisted. Each scene-placed `ResourceNodeSpawner` owns a `ResourceNodeSpawnerModel`, which is saved as `Persist.ResourceNodeV01`: `ID`, `ResourceNodeState`, `DespawnAtWorldTime`, `ResourceNodeDefinitionId`, `ResourceNodeVariantIndex` and `List<int> ResourcesToSpawn`.

- The number of records equals the number of spawn points, which this module never changes.
- The rolled yield is stored with each node. Nodes rolled while the multiplier was active, and existing unharvested nodes the module rescaled, are saved with the bigger list and keep it after uninstall. The field is a plain int list, which vanilla reads at any length (save-compatible). Disabling the module in game first shrinks the tracked lists in loaded zones back to their vanilla-equivalent length.
- After uninstall, a zone may have more active nodes than its vanilla max until they despawn on their vanilla lifetimes.
- Weather resources are ordinary loose actors. Explorer drone output goes into the drone cloud, which is limited by the game's own stack limit.
- The hard cap and the roll cap bound growth.
- Still to verify in game: the uninstall-then-load test.

## Verified vs assumed

Verified offline (Cpp2IL diffable C# + Il2CppInspector `types.cs`):
- All names, argument counts, field types and offsets listed above.
- `SpawnTornadoActivity.SpawnOptions` is a `List<PeriodicActorSpawner.SpawnOption>`, a struct list (Inspector shows the fully qualified type).
- `LootTableEntry.OutputQuantity` is a `MonomiPark.SlimeRancher.Util.RangeInt` class, not `UnityEngine.RangeInt`.
- `DroneDirector` keeps a `Dictionary<IdentifiableType, LootTableEntry>`.
- The persisted node model.
- `Awake` / `GetResourcesToSpawn` / `ResourceNode.SpawnResources` have unique bodies.
- By disassembly (capstone on the local copy `re/input/GameAssembly.dll`): `GetResourcesToSpawn` reads `_minResources`/`_maxResources`/`_resources` on every call and the list length equals the rolled count; `ResourceNodeSpawner.SpawnNode` is the only place that stores it into the model; the harvest coroutine spawns exactly that list. This settles assumption 1 for the yield part.
- Harvest path (UpdateForState / SpawnSingleResource / SpawnResourcesAsync.MoveNext), the per-item time fields and the absence of native callers of `SpawnResources()` (capstone + call/jmp byte scan of the il2cpp section).
- /W4 clean build.

Assumed, not yet checked in IDA or in game:
1. The node director reads `MaxActiveNodes` / `UpdateNodesIntervalHours` from its config on every use. `GetResourcesToSpawn` reads min/max/weights on every call and runs when a node spawns. `_nextNodeUpdateTime` is not rescheduled, so a shorter interval applies after the refill that is already scheduled.
2. `LightningStrike.Update` reads `SpawnOptions` (Min/MaxQuantity) at strike time, and strikes are created with `Instantiate` from the prefab. If a strike is pooled (`SpawnTemporaryObjectActivity.UseFXPool`), instances created before the patch keep vanilla amounts until they are re-created. `LightningStrike` has no reset method, which suggests it is not reused.
3. `SpawnTornadoActivity.Spawn` / `StaticTornadoSpawner` hand their `SpawnOptions` to the tornado's `PeriodicActorSpawner.Init`. Whether by reference or by copy, a cyclone spawned after the patch uses the patched counts. Cyclones already alive may keep their copied values.
4. `SpawnActivity` re-rolls its next spawn time from `SecondsBetweenSpawns` after each spawn, so a frequency change applies after the spawn that is already scheduled.
5. Explorer drones use `OutputQuantity` (Min..Max, both inclusive per the tooltip) as the item count per collection, read when the collection happens.
6. Cyclone `SpawnOptions` contain Drift Crystal as a plain `IdentifiableType` with `IsAnimal == false`.
7. The nested-class name convention `Outer/Inner` works with `il2cpp_class_from_name`. If it fails, only that optional feature is disabled.

`re/symbols.md` did not exist when this was written. Re-check items 1–5 against it.
