# Ranch (quantum) drones, drone station, programs, Quantum Cloud

`MP.SR` = `MonomiPark.SlimeRancher`; drone code in `MonomiPark.SlimeRancher.Drone`. Tags: **code** = decompiled; **dump** = signature only. Mod use: `modules/drone_price_limit/` (price limit, station dialog, owns the station menu hooks), `modules/custom_upgrades/drone_source.cpp` (Hen/Elder Collector and Quantum Storage Link sources), `drone_programs.cpp` (several programs), `drone_battery.cpp`, `quantum_link.cpp`. Hooks: re/hooks.md.

## Structure

- `RanchDrone` placed at a `DroneStation` gadget; programmed with one task: target (type + ident), source, destination (sink). `DroneStationGadgetModel._taskData` (`DroneTaskData`), saved as `DroneTaskV01` via `DroneStationGadgetModel.Pull` -> PullGadget. Station identity across loads: `PlacedGadgetV07.ActorId`.
- `DroneStation.AssignRanchTask` = new task data + `RanchDrone.AssignTask` -> `DroneBehaviorPlexer.OnTaskChanged` (deselects the behaviour, builds a new `RanchDroneProgramPlexer` from `GatherSourceTask` / `GatherSinkTask`). Same call as Start Program.
- `RanchDrone.GatherSourceTask(DroneTaskSourceType)` (unknown type throws), `RanchDrone.GatherSourceTasks`; `RanchDrone.Init`; `RanchDrone._ammo` (`IsEmpty()`), `GetCurrentEnergy()`, `GetCatchUpActionDur`.
- `DroneBehaviorPlexer`: `_currBehaviour`, `PickNextBehavior` (tries `RanchDroneBehaviourIdle` (at-station flourish) before the programs when due), `MaybeHasNextGatherBehavior()` (any source program's `Relevancy(readOnly)`).
- Behaviours: `RanchDroneBehaviourRest`, `RanchDroneBehaviourIdle`, `RanchDroneBehaviourDumpAmmo` (**destroys** held items: `PopFromSelectedSlot` + FX; picked when the drone holds items and no sink is relevant).
- Sources: `RanchDroneSourceProgramCoop` (live chickens above minimums), `RanchDroneSourceProgramElderCollector` (silo-storage source over `RanchMetadata.Plots[].elderCollectors`; accepts every target; `GetSources` returns `StorageMetadata[]`), `RanchDroneSourceProgramDynamic` (Outside Plots / Free Range: `GetSources` picks a cell item (predicate, not in a plot, no parent); `ConeFindTarget` = `Physics.SphereCastNonAlloc`, layers Actor + ActorIgnorePlayer, same IdentifiableType; nothing found -> gather completes empty and re-picks the same item).
- Sinks: `RanchDroneSinkProgramPlortMarket : RanchDroneSinkProgramBase<ScorePlort>`, caretaker shop sink, silo, corral, feeder, incinerator, refinery.
- **Incinerator sink** (`RanchDroneSinkProgramIncinerator : RanchDroneSinkProgramBase<Incinerate>`, code): `GetSinks` 0xA2B570 = plots' `LandPlotMetadata.incinerator` (+0x48) where `Incinerate.GetAshSpace() > 0` (lambda 0xA368D0); `GetAvailableSpace` 0xA2B430 = sum of GetAshSpace. `Incinerate.GetAshSpace` 0x987DA0 = 0 unless `AshTrough` (+0x48, FillableAshSource) is active and enabled, else `ceil(trough.GetAshSpace() / AshPerIncineration)` (+0x50). So a drone burns only into an incinerator whose **Ash Trough is built and not full**; with the trough full a "-> Incinerator" program has nothing to do (sources only gather while a sink has space: `MaybeHasNextGatherBehavior` false, the drone rests). plot_upgrades' Ash Booster (AshPerIncineration x2) halves that room. Round 10 "drone not starting with chickens" (Meats / Outside Plots -> Incinerator) was most likely this; drone_programs logs the ash room on the first idle switch of such a program.
- `RanchDroneBehaviourRest.FixedTick` 0xA23870: state 2 (at the station) returns `ShouldRethink()` (task set, battery, `_rethinkTime` reached) only; state 1 (flying home) also needs `MaybeHasNextGatherBehavior`. `RanchDroneProgramPlexer.MaybeHasNextGatherBehaviour` 0xA28DD0 = `_sources.Any(s => s.Relevancy(true))`; `RanchDroneSourceProgramBase.Relevancy(readOnly)` 0x163A2C0: ammo slot not full, battery, then any `GetSources(IsSourceValid)` not in `RanchDrone.SOURCE_BLOCKLIST` (readOnly: no path check; else `GeneratePath`). `IsSourceValid` 0x163A000: CanTarget, `CouldAddToSelectedSlot`, any sink `HasAvailableSpace`. `RanchDroneSourceProgramDynamic.GetSources` 0xA2F6C0 walks `_cellObjIndices` live (no cache needed); OutsidePlots `SourcePredicate` = not in a plot (`RanchMetadata.GetContaining` null), no parent, active, resources only when ripe / edible.
- `StorageMetadata(slot, storage, catcher)`; `StorageMetadata.Decrement` (only callers: the silo-storage source's `OnAction` and fast-forward).
- `RanchMetadata.Register(LandPlot)` 0x98D7D0, `GetStorageMetadata` 0x98D290, `OnUpgradesChanged(plot)` (rebuilds the plot entry): re/plots.md.
- UI: `DroneUIDefinitionSource.FindSourceDefinition`, `GatherSourceOptions` (keeps list order; sort index); `DroneTaskTarget.<Predicate>k__BackingField`; task config `MeatTypeGroup`, `HenType`.

## Market sink and price limit (code)

- `ScorePlort.CanDroneDeposit(id, ignoreMarketShutdown)` 0x991E40 = `GetMarketValue(id).HasValue` (+ active / not charging). Only callers: the `GetSinks` iterators of the market and caretaker-shop sink programs (always `ignoreMarketShutdown = true`). `HasAvailableSpace`, `GetAvailableSpace`, `Relevancy`, per-tick `OnAction` re-check and source gather checks all go through `GetSinks`. `_isCaretakerShop`.
- Fast-forward: `FastForward` 0xA2C2C0 does `Prioritize(GetSinks).First()` + `Deposit`; its only caller `DroneFastForwarder.FastForward_Deposit` first filters by `HasAvailableSpace(group.id)`.
- `ScorePlort.Deposit` -> `PlortEconomyDirector.TrySell` (re/shop.md).
- drone_price_limit hooks `CanDroneDeposit` and `RanchDroneBehaviourDumpAmmo.Relevancy`.

## Battery (code)

`DroneStationGadgetModel._energyDepleteTime` (ObservableValue<double>, saved). Energy = max(0, depleteTime - worldTime)/3600 x `DroneDirector.EnergyDepletedPerHour` (+0x38). Model: `GetCurrEnergy` 0xA8D0D0, `GetEnergyAtTime` 0xA8D160, `IsEnergyFull` 0xA8D4B0, `AddEnergy` 0xA8CFB0 (max 1000). No per-drone rate. Readers: `DroneStation.UpdateEnergy`, `RanchDrone.GetCurrentEnergy`, audio controller, `DroneFuelDepositor`, `ExplorerDroneProgram`.

## Quantum Cloud

`DroneDirector.GetCloudModel()` -> `DroneCloudModel._amounts` Dictionary<IdentifiableType,int> (saved `DroneCloudV01`); `GetAmount` / `SetAmount`. Explorer drones upload; the station's cloud menu withdraws. `DroneStationCloudExtractionController._spawnFX` (pooled `FXHelpers.SpawnAndPlayFX`).

## Visuals

`DroneStationOutputPortVisualController` (spout look). Ghost drones: `ComponentAcquisitionDroneController` fades its shader float `_PhaseValue` 0 -> 1 in `Update` (`_phaseTimer`).

### Drone phase-through pass (data + IDA)
The look of a drone flying through geometry is an HDRP custom pass, not the drone's shader.
- Core scene GameObject `DronePhaseThroughCustomPass` (inactive in the scene data): a global `CustomPassVolume` (priority 12, injection point 3 = BeforePostProcess) with two `MP.SR.Rendering.Pass.DronePhaseThroughCustomPass` entries: "Ranch Drone Phase Through Custom Pass" (`_renderingLayerMaskFilter` 65536 = bit 16, material `DronePhaseThroughCustomPass`) and "Explorer Drone ..." (bit 17, material `ExplorerDronePhaseThroughCustomPass`); both `_layerMask` = layer 27, `_playerLayer` = layer 8, target colour buffer Custom, depth Camera.
- `DronePhaseThroughCustomPass.Execute` 0x8CDED0: `DrawRenderers` of layer mask (`_layerMask | _playerLayer`) filtered by `_renderingLayerMaskFilter`, shader pass `GBuffer` (`_shaderTags[2]`; Setup 0x8CE3C0 builds Forward, ForwardOnly, GBuffer, SRPDefaultUnlit, FirstPass, DepthOnly, DepthForwardOnly), depth LessEqual, then `HDUtils.DrawFullScreen(_fullscreenPhaseMaterial)`. Fields +0xD8 `_drawInSceneView`, +0xDC `_layerMask`, +0xE0 `_playerLayer`, +0xE4 `_renderingLayerMaskFilter`, +0xE8 `_fullscreenPhaseMaterial`, +0xF0 `_shaderTags`.
- Shader `DronePhaseThroughCustomPass` (properties): scanlines (`_ScanlineBGColor` cyan, `_ScanlineLineColor`, `_ScanlineOutlineColor` HDR, `_ScanlineSize` 0.5, `_ScreenUVPanningSpeed` 3), intersection edge (`_IntersectionEdgeColorClose` HDR cyan ~350, `_DepthDifference` 1.8 ranch / 0.25 explorer, `_MinDepth` 0.8), noise.
- `DroneCustomPassController` (+0x20 `_phaseThroughCustomPassVolume`, +0x28 `_currSceneGroup`): `Start` 0x9F65C0 subscribes to scene group loads; `OnSceneGroupLoaded` 0x9F60E0 (gameplay groups) subscribes `DroneDirector` drone registered / deregistered -> `UpdateCustomPassActive` 0x9F6780 = `volume.SetActive(GameModel.AllDroneStations(...).Any(s => s.SceneGroup == current))`. So the pass runs only while the scene group has a drone station.
- Mod use: quantum_link.cpp (the Quantum Storage Link's phase ghost: renderers on layer 27 with rendering layer bit 16 inside the link; switches the volume on during a moment when it is off).

## From the custom_upgrades notes

### RanchDrone (MonomiPark.SlimeRancher.Drone)
- methods: `GatherSourceTasks(target)` 0xA1AA90 - GatherSourceTask for ten fixed types, kept when every source program accepts the target
- methods: `GatherSourceTask(type)` 0xA19B40 - builds `DroneTaskSource(type, IRanchDroneSourceProgram[])`; COOP = [RanchDroneSourceProgramCoop, RanchDroneSourceProgramElderCollector]; unknown type throws ArgumentOutOfRangeException
- methods: `GatherSinkTasks` 0xA196D0, filter 0xA37690 - drops the SILO sink only for the SILO source; otherwise only compares the source with SILO
- methods: `Init` calls GatherSourceTask(model._taskData.SourceType) for the saved program; `AssignTask` -> `DroneBehaviorPlexer.OnTaskChanged`
- fields: `_ammo` (AmmoSlotManager; `IsEmpty()`); `GetCurrentEnergy()` (<= 0 -> drone rests)
- gotchas: enum ToString of an unknown DroneTaskSourceType is a number (analytics only); nothing else switches on it
- mod: drone_source.cpp hooks GatherSourceTask, GatherSourceTasks; drone_programs.cpp reads _ammo, energy

### DroneTaskSourceType (enum)
- int enum NONE .. SPRINKLE_CANISTER = 10; mod values 0x53520101 (Hen Collector), 0x53520102 (Elder Collector), 0x53520110 (Quantum Storage Link)

### DroneStation / DroneStationGadgetModel
- methods: `DroneStation.AssignRanchTask(target, source, sink, analytics)` 0x9FE4E0 - `SetTaskData(new DroneTaskData{TargetType, TargetIdentType, SourceType, SinkType})` + `RanchDrone.AssignTask`; exactly what Start Program does
- methods: `DroneStationGadgetModel.Pull` 0xA8D550 - hands out the LIVE `_taskData` via out param (a local of `GameModelPullHelpers.PullGadget`), which converts it at once into `DroneTaskV01 {TargetType, TargetIdentTypeId, SourceType, SinkType}`
- fields: `_taskData` (DroneTaskData) - a station has ONE program
- gotchas: station key for the sidecar = `PlacedGadgetV07.ActorId` (stable across loads; pick up + re-place = new id)
- mod: drone_source.cpp `Pull` detour (save copy says COOP / SILO), drone_programs.cpp (SaveFilter, AssignProgram)

### DroneBehaviorPlexer
- methods: `OnTaskChanged()` 0x9F5270 - deselects the current behaviour, builds a new RanchDroneProgramPlexer from fresh source / sink programs; new program tried on the next FixedTick
- methods: `PickNextBehavior` order: RanchDroneBehaviourIdle (at-station flourish; picked FIRST whenever at station and a flourish is due, even with work) -> program plexer (gather / deposit) -> RanchDroneBehaviourDumpAmmo when ammo not empty (DESTROYS cargo) -> RanchDroneBehaviourRest
- methods: `MaybeHasNextGatherBehavior()` - any source program's `Relevancy(readOnly)`; the game's own "is there work" query (Rest uses it)
- fields: `_currBehaviour` (IRanchDroneBehaviour)
- mod: drone_programs.cpp ("nothing to do" = Rest + ammo empty + !MaybeHasNextGatherBehavior; Idle neither counts nor resets)

### RanchDroneSourceProgramDynamic (Outside Plots / Free Range)
- methods: `GetSources` - predicate, not inside a plot, no parent; `ConeFindTarget` - `Physics.SphereCastNonAlloc`, layer mask 0x8800 (Actor + ActorIgnorePlayer), same IdentifiableType, in the cone
- gotchas: a scan that finds nothing completes empty, next pick flies to the same item: drone "keeps scanning", never rests; live chickens walk away from the spot (suspected, not verified)
- mod: drone_programs.cpp (stuck rule: working, no cargo for 90 s; logs `GetProgramStateStr`)

### RanchDroneSourceProgramSiloStorage / RanchDroneSourceProgramElderCollector
- methods: `RanchDroneSourceProgramElderCollector.GetSources` 0xA31830 (virtual) - walks `RanchMetadata.Plots -> LandPlotMetadata.elderCollectors`, filtered by target, ordered by count
- behaviour (SiloStorage base): flies to `StorageMetadata.catcher` (GetTargetPosition, no null check), takes min(drone space, slot count, sink space), one item per OnAction per GatherTimePerItem (`MaybeAddToAnySlot` into the drone, `StorageMetadata.Decrement`); `CanCancel` = storage GameObject not active in hierarchy; `Relevancy` calls GetSources(IsSourceValid)
- gotchas: the elder-collector part accepts every target, so Coop is offered for meats, elders, single hens / roosters / elders; arrays are IEnumerable<T> (GetSources can return a StorageMetadata[])
- mod: drone_source.cpp GetSources detour (our types -> only that kind's slots; quantum -> quantum::Sources)

### StorageMetadata
- fields: {SiloStorage storage, SiloCatcher catcher (drone flies to its transform), int index}
- methods: `Decrement` 0x9969C0 - callers only `RanchDroneSourceProgramSiloStorage.OnAction` and `DroneFastForwarder.GatherGroup.Storage.Decrement`
- mod: quantum_link.cpp Decrement detour (lowers the cloud amount)

### RanchMetadata
- chains: `Register(LandPlot)` runs from LandPlot.Start / ApplyUpgrades / OnUpgradesChanged; each Register replaces the coop's entry (built from DeluxeStuff only)
- mod: coop_world.cpp `LinkDrones` (new array = live old entries + Hen Collector slots, every ~1 Hz) / `UnlinkDrones` (`RanchMetadata.OnUpgradesChanged(plot)` rebuilds the vanilla entry)

### Drone UI (source picker)
- methods: `DroneProgramViewModel.GatherSourceOptions` 0x821F80 - every DroneTaskSource -> DroneSourceOptionEntry with `IDroneUIDefinitionSource.FindSourceDefinition(type)` (0x83ABF0, DroneUIDefinitionSource; null = skipped), list order (no sort)
- methods: `DroneProgramViewModel.RevertChangesToLastAppliedProgram` 0x822E80 - compares target / source / destination sub view models with the station's task data, resets the differing ones (ProgramChanged -> redraw)
- methods: applying = `TryFindSourceProgram(type)` (GatherSourceTasks again, match by SourceType) + AssignRanchTask
- fields: `DroneUIProgramDefinition<DroneTaskSourceType>` {_type, _displayName (LocalizedString), _icon, _sortIndex}; drone list / read-only views use the same lookup
- fields: `DroneTaskTarget.<Predicate>k__BackingField` - the target's type predicate
- task config: `MeatTypeGroup`, `HenType` (icon), ElderGroup
- mod: drone_source.cpp FindSourceDefinition detour (copy of Coop / Silo definition); drone_price_limit station_dialog.cpp calls RevertChangesToLastAppliedProgram

### DroneDirector / Quantum Cloud
- `DroneDirector.GetCloudModel()` -> `DroneCloudModel {Dictionary<IdentifiableType,int> _amounts (one key per storable type, 0 when empty), int _limit}`, saved as `DroneCloudV01 {IDs, Amounts}`
- methods: `GetAmount` 0xA201E0; `SetAmount` 0xA20BD0 = min(amount, _limit); `AddAmount` 0xA1FE90 = min(cur + n, _limit), NO lower bound (use Get + Set); unsupported types log an exception
- `DroneDirector._droneStationType` - the Drone Station GadgetDefinition (`icon`, `_pediaLink`)
- `DroneStationCloudExtractionController._spawnFX` - cloud-withdraw FX (pooled `FXHelpers.SpawnAndPlayFX`)
- `ComponentAcquisitionDroneController` ("GhostDrone") keeps renderer material instances and fades shader float `_PhaseValue` 0 -> 1 in Update after arriving (`_phaseTimer`); own shader property
- mod: quantum_link.cpp, drone_battery.cpp (station type)

### Drone battery
- `AddEnergy(timeDir, rate, value)` = depleteTime -> now + min(energy + value, 1000) / rate hours; only caller `DroneFuelDepositor.OnTriggerEnter` (+ station init / Push write `_energyDepleteTime`); `SetEnergy` has no caller; fast-forwarder only reads depleteTime
- `ENERGY_OVERCHARGE_THRESH` 100, max 1000; measured vanilla drain ~0.8 energy per game hour (1 real s = 60 world s)
- gotchas: across a sleep the fast-forward runs drones on the vanilla battery first
- mod: drone_battery.cpp (refund R0 = depleteTime - t0, R1 = max(0, R0 - dt/factor), refund = R1 - max(0, depleteTime - now))

## From the slime_dj, spawn_scaling, hl2_movement, quick_start, free_placement, misc_tweaks, plot_upgrades, drone_price_limit notes

### Explorer mode stations (dump + IDA names; round 14, custom_upgrades drone_programs)
- `DroneStationGadgetModel._type` +0x80 (DroneType: RANCH_DRONE 0, EXPLORER_DRONE 1); `_taskData` +0xA0 (DroneTaskData: TargetType +0x10, TargetIdentType +0x18, SourceType +0x20, SinkType +0x24; explorer: source / sink NONE); `_program` +0xA8 (ExplorerDroneProgram, keeps its own `_taskData`, re-plans in `TaskUpdated(newTaskData, worldTime)` 0xA11120)
- `DroneStation` (scene object): `_model` +0xC0, `_droneTaskConfig` +0x40, `_ranchDrone` +0xE0, `_explorerDrone` +0xE8; `AssignExplorerTask(DroneTaskTarget, bool sendAnalytics)` 0x9FE1A0 (vs `AssignRanchTask(target, source, sink, bool)` 0x9FE4E0); `DroneTaskTarget(DroneTaskConfig, DroneTaskTargetType, IdentifiableType)` 0xA07EC0
- mod: drone_source `Impl::AssignExplorer` finds the DroneStation by `_model` (cached, one scan per miss) and calls AssignExplorerTask; the programs rotation never switches explorer stations (it watches the RanchDrone)

### ExplorerDroneConfig (MonomiPark.SlimeRancher.Drone)  [: ScriptableObject]
- fields: `_defaultLootConfig` LootTableEntry (inline struct) +0x20, `_lootTable` List<LootTableEntry> +0x48 (dump)
- nested `LootTableEntry` (struct): `Item` 0x0, `Weight`, `MaxSourcesConsidered`, `GenerationIntervalHours` Range, `OutputQuantity` 0x18 (`MonomiPark.SlimeRancher.Util.RangeInt`, a class, not UnityEngine.RangeInt), `MinimumTimeFactor`; element size 0x28 (unboxed) (dump)
- `Util.RangeInt`: `Min` 0x10, `Max` 0x14
- chains: `DroneDirector._explorerDroneLootTable` Dictionary<IdentifiableType, LootTableEntry> copies the entries, but the copies share the same RangeInt objects, so patching OutputQuantity.Min/Max reaches them (dump)
- gotchas: explorer drones generate from this table based on static `DiscoverableResourceData` positions; they do NOT harvest live ResourceNode spawns
- mod: spawn_scaling (source `drones`)

### ScorePlort.CanDroneDeposit(IdentifiableType id, bool ignoreMarketShutdown) 0x991E40 (disasm)
- false for an inactive GameObject or a Market Link still charging; else tail-calls `GetMarketValue(id, ignore).HasValue`: `_sprinkleType` -> 1; caretaker shop (`_isCaretakerShop`) never; market shut down (unless ignored) -> null; else `PlortEconomyDirector.GetCurrValue(id)` = `RoundToInt(_currValueMap[id].CurrValue)` (null without a market entry) = the board price paid by Deposit/TrySell
- only two callers (rel32 xref scan): GetSinks iterators of `RanchDroneSinkProgramPlortMarket` and `RanchDroneSinkProgramCaretakerShop`, both with ignore = true
- mod: drone_price_limit detour

### RanchDroneSinkProgramPlortMarket
- `<GetSinks>d__16.MoveNext` 0xA356A0, field `<>4__this` = the program (disasm)
- every GetSinks use enumerates inside the calling method (Any / FirstOrDefault / First): `HasAvailableSpace` / `GetAvailableSpace` = GetSinks(id).Any(); `Relevancy(overflow)`: `_sink = Prioritize(GetSinks(id)).FirstOrDefault()`; `OnAction` re-validates `GetSinks(heldId).Any(s => s == _sink)` each tick; source programs (`RanchDroneSourceProgramBase.Relevancy` / `IsSourceValid`) only gather while a sink reports space; `FastForward` 0xA2C2C0: `Prioritize(GetSinks).First()` then Deposit - its caller `DroneFastForwarder.FastForward_Deposit` filters with HasAvailableSpace first
- chains: program `._drone` (RanchDroneSinkProgramBase<ScorePlort>) -> `RanchDrone._stationModel` (DroneStationGadgetModel) -> `IdentifiableModel.actorId` (saved as `PlacedGadgetV06.ActorId`, stable)
- mod: drone_price_limit detours MoveNext (publishes the program while enumerating)

### DroneBehaviorPlexer / RanchDroneBehaviourDumpAmmo
- `PickNextBehavior`: Idle? -> program plexer (gather / deposit) -> `RanchDroneBehaviourDumpAmmo` if ammo slot not empty -> Rest (disasm)
- DumpAmmo `OnActionDumpAmmo` = `PopFromSelectedSlot` + FX: DESTROYS held items. Rest re-checks via `Rest.ShouldRethink` / `MaybeHasNextGatherBehavior`. `ForceDumpAmmoAndDestroy` on station destroy
- mod: drone_price_limit detours `RanchDroneBehaviourDumpAmmo.Relevancy`

### Drone station menu (scene `MenuLayer_DroneStationMenu`, bundle via UnityPy + IDA)
- hierarchy: DroneUIDisplay (1470 x 980) > DroneMenuRoot (DroneMenuUI) > ContentContainer > TopViewSwitcher > ProgrammingPage (DroneProgramMenu) > PageContainer (RectMask2D) > ProgramViewSwitcher > OptionSubPage > ProgramSettings > ButtonRow > Target / Source / DestinationProgramOption (DroneProgramOptionButton, 445 x 380)
- `DroneProgramMenu.SetViewModel(DroneProgramViewModel)` 0x81BE10 binds the page when the menu opens for a station; `_targetButton`, `_marketConfig` (MarketUIConfiguration `_plorts`, `PlortEntry.IdentType` = market terminal order), `_activeStateSprite`; hotkeys `HandleActivateDrone` / `HandleClearAllOptions` (InputEvent-bound)
- "Change" pill: `DroneProgramOptionButton._promptContainer` (inactive in prefab): SDFRoundedRectangle cyan (0.32, 0.78, 0.84), radius 50, bottom-left at (132, 0), 264 x 56, EncapsulateChildren min width 264, `_horizontalPadding`, HorizontalLayout; InputEventDisplay (InputIconGroup, 4 InputIcons: Button image / Key (Border, Background, Text, Image) / DoubleTap) + InputLabel (TMP + LocalizeStringEvent "Change", UI table key 263830960522432512); FocusEventTriggers toggle the pill on focus. `InputIcon.SetKey` 0x7C6FE0 = buttonRoot off, keyRoot on, keyText on / keyImage off, keyLocalizeStringEvent off, keyText.text = HumanReadableControl
- legend `DroneProgramInputLegendConfiguration`: Start Program, Clear All, Close, shown in `DroneProgramMenu._inputLegend` +0xE0 (ComponentInterface<IInputLegend>, dump); C in the "Paused" map is only MapZoomIn; pad right-stick click only marks Pedia entries seen. mod: drone_price_limit adds a legend_hint "[P] Programs" entry there (round 13) and uses the Clear All event (`HandleClearAllOptions`, Z) as the programs dialog's Delete
- view model: `DroneProgramViewModel._destinationForTarget._currentSelection` (DroneDestinationOptionEntry) `.get_Type()` 0x815070 = DroneTaskSinkType (PLORT_MARKET = 5); `._station` (DroneStationGadgetModel) `.actorId`; `._definitionSource` (DroneUIDefinitionSource `FindTarget/Source/DestinationDefinition` -> DroneUIProgramDefinition<T> `_displayName` / `_icon`); `RevertChangesToLastAppliedProgram` 0x822E80 (resets page options that differ from the station's task data); `HasAnyChanges()`
- `DroneUIDisplay.OnCancel` opens `_unappliedChangesConfirm` when target/source/destination differ from the station's task data (`MatchesCurrentProgram` of the three sub view models)
- `DroneStation.AssignRanchTask` (used by custom_upgrades drone_programs::Run)
- mod: drone_price_limit `station_dialog.cpp`
- opening it without a station (IDA, round 11; mod: quantum_link.cpp `OpenCloudMenu`): the display name is the Drone Station prefab's `UIDisplayInteractable._display` (`DroneDirector._droneStationType.prefab`). `UIDisplayManager.OpenDisplay(UIDisplayHandle)` 0x7A5530 (struct = string pointer) returns a `UIDisplayRequestHandle<Object>`; `TryGetRequestedDisplay(out UIRuntimeDisplay)` once bound; `UIRuntimeDisplay._displayGameObject` once loaded. `UIDisplayShowOptions<T>.Apply` 0x1935E10 only calls `IDisplayShowDataHandler`s whose types match the data, so null data calls nothing. `DroneUIDisplay`'s explicit `IDisplayShowDataHandler<IDroneTaskAssignable>.UpdateShowData` 0x824AD0 handles null: `DroneCloudInventoryViewModel(cloud, defs, canManageInventory: false, null)`, no program view model; `DroneMenuUI.SetViewModel(DroneMenuViewModel)` 0x817A30 then `RemoveTab`s the programming page (as `MapDroneMenuContainer.Start` 0x706DB0, the map's drone panel: `SetViewModel(dd, td, null)` 0x818150). `OnCancel` 0x824F20 null-checks the program view model. Tabs: `DroneMenuTabs` DRONE_PROGRAMMING 0 / DRONE_LIST 1 / QUANTUM_CLOUD 2; `DroneMenuUI._tabs` (DroneTabSetup.Target = the page's transform), `_tabbedControl.GoToTab(int)` 0x7D6CD0, `_cloudInventory`. Assumed (not checked): a display is created per open (`UIRuntimeLayer.CreateDisplay`), so the removed tab does not carry over to a station's next open.
- calling `UpdateShowData(null)` the first frame `UIRuntimeDisplay._displayGameObject` exists threw a NullReferenceException every time (log, round 12; no managed trace) and the menu stayed on its first tab (the drone list, legend "[SPACE] Go To"). Assumed cause (not proven): the display is not active yet, so its components' Awake / Start (DroneMenuUI.Awake adds the TabbedControl / ViewSwitcher listeners) have not run. Round 13: quantum_link waits until the display GameObject is active in hierarchy plus 0.15 s, retries up to 3 times, and (until round 14) selected the cloud tab / hid its hints even when it still threw (log `UpdateShowData(null) threw N time(s)` once per session, or `filled on try N`). Candidate throw sites if it persists: `DroneMenuUI.SetViewModel` 0x817A30 (many fields, `GetCurrentTabType` 0x817320 via `ViewSwitcher.get_ActiveChild`), `TryShowTutorial`.
- cloud page legend (IDA, round 12): `DroneMenuUI.SetViewModel` configures `DroneCloudInventoryMenu._inputLegend` (ComponentInterface<IInputLegend>, the component at `ComponentInterfaceBase._object` +0x10) with `_manageInventoryLegendConfig` + `TryUpdateInputLegend` 0x8133E0 (enables withdraw / send-to-market / send-to-refinery per selected item) + `BindForCloudManagementInputs` 0x812520 when `_canManageInventory`, else with `_defaultLegendConfig` (the map drone panel's; nothing binds its events in the cloud menu: the "[SPACE] + magnifier" row the user saw from a Quantum Storage Link). `InputLegendConfiguration._hints` (InputHintConfiguration[]: `Label` +0x10, `InputEvent` +0x18, `AdditionalInputEvent` +0x20); `IInputLegend.SetItemEnabled(InputEvent, bool)`. mod: quantum_link.cpp `HideCloudHints`
- drone list "Go To" (IDA, round 14): `DroneListMenu` (+0x20 `_pageSwitcher`, +0x28 `_focusTarget`, +0x30 `_list`; no legend field; `SetViewModel` 0x816460). `DroneListButton.OnSubmit` 0x815AC0 invokes `DroneListEntryViewModel.GoToInMapRequested` (+0x18), focus received / lost invoke `HighlightToggled` (+0x10, true / false); `DroneListViewModel` forwards them as `GoToEntryInMapRequested` (+0x18) / `EntryHighlightToggled` (+0x10). Only `MapDroneMenuContainer` (`GoToDroneInMap` +0x28) subscribes, so in the station menu (and a Quantum Storage Link's) Space on a drone does nothing. `InputLegendFocusRegistrar` (`_passThrough` +0x20, `_defaultConfiguration` +0x28, `_defaultHints` +0x30, `_currentConfiguration` +0x40, `_disabledHints` +0x48, `_currentHints` +0x50) and `InputLegend` (`_configuration` +0x28, `_defaultHints` +0x40, `_disabledHints` +0x58, `_currentHints` +0x70) both keep a disabled-events set. mod: quantum_link.cpp `HideListHints` (round 14) turns the drone list page's hints off; the menu now opens on its first tab (the forced `GoToTab` of rounds 11-13 threw a NullReferenceException every open and was removed)

## From the sandbox, sell_upgrades, collectibles, instant_build notes

### DroneTargetDefinitions (MonomiPark.SlimeRancher.UI.Drone.Data)
- facts: a `ListAsset` of `DroneUIProgramDefinition<DroneTaskTargetType>` with `_type` / `_icon`; enum DroneTaskTargetType: NONE, SINGLE, PLORTS (2), VEGGIES (3), FRUIT, MEATS, ELDERS, PLORTS_NO_SPECIALS, CRAFTING_MATS (8); no slimes/food type
- asset: drone station UI scene sprites: `iconPlortsPedia`, `iconCategoryResources`, `iconCategoryMeatElder`, drone faces, battery, source icons (no slime sprite)
- mod: sb group tab icons

### DroneActivitySceneNotifications
- methods: `AddResourceCollected` (text "[Item] sent to the Quantum Cloud"), `AddPlortSold` (silo Sell-All)
- mod: sb used to use AddResourceCollected; now custom_upgrades/sale_notify text notices

### Research / ghostly drones  [col]
- facts: no static per-zone list (`GameModel.researchDrones` only registered ones; `ResearchDroneEntry` has no zone); `ResearchDroneController._model.read`; `ComponentAcqDroneSpawner` sits on a TreasurePod (`InitDrone: SRGetComponent<TreasurePod>`); ghostly drone IdentifiableType has no icon
- gotchas: `Object.FindObjectsByType(includeInactive)` walks every scene object, ~15-20 ms in the open world
