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
- legend `DroneProgramInputLegendConfiguration`: Start Program, Clear All, Close; C in the "Paused" map is only MapZoomIn; pad right-stick click only marks Pedia entries seen
- view model: `DroneProgramViewModel._destinationForTarget._currentSelection` (DroneDestinationOptionEntry) `.get_Type()` 0x815070 = DroneTaskSinkType (PLORT_MARKET = 5); `._station` (DroneStationGadgetModel) `.actorId`; `._definitionSource` (DroneUIDefinitionSource `FindTarget/Source/DestinationDefinition` -> DroneUIProgramDefinition<T> `_displayName` / `_icon`); `RevertChangesToLastAppliedProgram` 0x822E80 (resets page options that differ from the station's task data); `HasAnyChanges()`
- `DroneUIDisplay.OnCancel` opens `_unappliedChangesConfirm` when target/source/destination differ from the station's task data (`MatchesCurrentProgram` of the three sub view models)
- `DroneStation.AssignRanchTask` (used by custom_upgrades drone_programs::Run)
- mod: drone_price_limit `station_dialog.cpp`

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
