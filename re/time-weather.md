# Time, pause, fast-forward, weather

Tags: **code** = decompiled; **dump** = signature only. Mod use: `modules/sandbox/` (time and weather tools, `ui/sandbox_weather*.cpp`), custom_upgrades receipt (pause via UIInputLocker). Hooks: re/hooks.md.

### TimeDirector (global) : SRBehaviour, Participant
- `FastForwardTo(double fastForwardUntil)` 0x54F6F0.
- `Pause` 0x550570 code: `_pauserCount`++, timeScale 0, `OnPausedChanged(true)`. `Unpause` 0x5507F0 (coroutine `<DelayedUnpause>d__61`).
- fields: `OnFastForwardChanged` +0x28, `OnPausedChanged` +0x30, `SecsPerGameDay` float +0x38, `FfSecsPerGameDay` +0x3C, `_timeOfDayValue` ScriptedFloat +0x60, `_onDayPassed` StaticGameEvent +0x68, `_onHourPassed` +0x70, `_lastDay` +0x78, `_lastHour` +0x7C, `_timeFactor` +0x80, `_ffTimeFactor` +0x84 (run `python re/re_lookup.py TimeDirector -f` for the rest).
- consts: START_HOUR 9, SECS_PER_DAY 86400, HOUR_MORNING 6 / AFTERNOON 12 / NIGHT 18. World time is in seconds; game time of day = `worldTime % 86400`.
- `UIInputLocker.OnEnable/OnDisable` call Pause/Unpause (re/ui.md).

## From the slime_dj, spawn_scaling, hl2_movement, quick_start, free_placement, misc_tweaks, plot_upgrades, drone_price_limit notes

### LightningStrike (MonomiPark.SlimeRancher.World)  [: MonoBehaviour, on a prefab]
- fields: `SpawnOptions` List<ResourceChance> +0x48; `DespawnTime` (dump)
- nested `LightningStrike/ResourceChance` (class): `Resource` 0x10 (null = "spawn nothing"), `Weight`, `MinQuantity` 0x1C, `MaxQuantity` 0x20 (flat distribution, tooltip) (dump)
- gotchas: strikes are instantiated from the prefab; `Instantiate` copies SpawnOptions, so patch the prefab component only (patching instances double-applies). Assumed: `Update` reads SpawnOptions at strike time; not pooled (no reset method) though `SpawnTemporaryObjectActivity.UseFXPool` exists
- mod: spawn_scaling (source `lightning`)

### SpawnActivity (MonomiPark.SlimeRancher.Weather.Activity)
- fields: `SecondsBetweenSpawns` Range +0x20 (dump). Assumed: next spawn re-rolled from it after each spawn

### SpawnTemporaryObjectActivity (same ns)  [: SpawnActivity]
- fields: `Prefab` +0x40 (the strike prefab), `UseFXPool` (dump)
- methods: `Spawn` - unique body (0x66DB30 / 0x66DBF0 / 0x640BF0 are the three patch-on-touch targets, see below)
- chains: lightning prefab component via `GetComponent(activity.Prefab)`
- mod: spawn_scaling hook `Spawn` (patch-on-touch)

### SpawnTornadoActivity (same ns)
- fields: `SpawnOptions` List<PeriodicActorSpawner.SpawnOption> +0x70 (a struct list; Inspector shows the full type) (dump)
- methods: `Spawn` (patch-on-touch hook)
- gotchas: options mix resources (Drift Crystal) with slimes. Assumed: hands SpawnOptions to the tornado's `PeriodicActorSpawner.Init`

### StaticTornadoSpawner (MonomiPark.SlimeRancher)  [scene-placed]
- fields: `SpawnOptions` +0x48, `_minSpawnDelayGameHours` +0x60, `_maxSpawnDelayGameHours` +0x64 (dump)
- methods: `OnEnable` (patch-on-touch hook)
- gotchas: prefab assets also show up in `FindObjectsOfTypeAll`; only track ones active in a scene hierarchy
- RVAs of the three optional hooks in the 1.3.0 dump: 0x66DB30, 0x66DBF0, 0x640BF0 (SpawnTemporaryObjectActivity.Spawn, SpawnTornadoActivity.Spawn, StaticTornadoSpawner.OnEnable; unique bodies)

### PeriodicActorSpawner.SpawnOption (MonomiPark.SlimeRancher.World, struct)
- fields: `ActorType` 0x0, `Weight`, `MinCount` 0xC, `MaxCount` 0x10, `VerticalForce`, `LateralForce`, `RandomSleepRange` (unboxed offsets; element size 0x28) (dump)

### Range (global)
- fields: `Min` 0x10, `Max` 0x14 (boxed) (dump)

### ReleaseResourcesActivity
- `Min/MaxResourcesPerReleaseEvent`: releases already-grown resources from existing growers; creates nothing (dump)

## From the sandbox, sell_upgrades, collectibles, instant_build notes

### WorldModel (MonomiPark.SlimeRancher.DataModel)
- fields: `worldTime` double (game seconds; day = floor(t/86400)+1; new game starts 09:00 = 32400 = `worldTimeStart`), `worldTimeStart`, `fastForwardUntil` Nullable<double>, `pauseWorldTime` bool, `lastWorldTime` Nullable<double> (il2cpp layout hasValue +0, value +8; 16 bytes). Offsets resolved by name at runtime (setup log showed `worldTime 0x30`)
- methods: `Pull/Push` save `worldTime`, `worldTimeStart`, `worldTimeFastForwarded` (and set lastWorldTime); NOT `pauseWorldTime` or `fastForwardUntil`
- gotchas: nothing in the game sets `pauseWorldTime` except `InitModel` (false)
- mod: sb weather.cpp time (pause, rewind writes worldTime + lastWorldTime together)

### TimeDirector
- methods: `Update` 0x550870 - returns while `Time.timeScale == 0`, outside a gameplay scene group or while pauseWorldTime; else `worldTime += deltaTime * _timeFactor` (86400 / `SecsPerGameDay`), or while fastForwardUntil set `* _ffTimeFactor` (86400 / `FfSecsPerGameDay`) until reached (then cleared, `OnFastForwardChanged(false)`); day/hour events when differing from `_lastDay`/`_lastHour`; `_timeOfDayValue` (sky, lights) (IDA)
- methods: `LateUpdate` - runs `OnPassedTime` delegates crossed by `lastWorldTime < t <= worldTime`, then `lastWorldTime = worldTime`
- methods: `FastForwardTo(double t)` 0x54F6F0 - sets fastForwardUntil (rounded to whole seconds), raises `OnFastForwardChanged(true)`; `ForceFastForwardEnd()`; `GetHourAfter(0, hour)` = next occurrence; `WorldTime`, `HasReached`, `HoursFromNow`
- fields: `_worldModel`, `SecsPerGameDay`, `FfSecsPerGameDay`, `_lastDay`, `_lastHour`
- chains: sleep `RanchHouseUI.SleepUntil` / death `LockOnDeath.FastForward` -> FastForwardTo -> `RanchCellFastForwarder.OnFastForwardChanged` simulates every non-hibernated ranch cell (corrals, feeders, gardens, incinerators, drones) for the skipped time
- gotchas: rewinding: set `_lastDay/_lastHour` to avoid day/hour events; everything timed compares against worldTime (scheduled things simply come later); `PlayerState.Damage` returns false while fast-forwarding
- mod: sb weather.cpp Op::TimeSkip / TimeRewind / TimePause; `SceneContextUtility.TryGetTimeDirector`

### Weather data (MonomiPark.SlimeRancher.Weather)
- classes: `WeatherPatternDefinition` (ScriptableObjectWithGuid): `Metadata` (`WeatherTypeMetadata`: `PediaEntry`, `AnalyticsName`, `Icon` = map indicator sprite), `CooldownHours`, `DroneGatherableResources`, `StartingTransitions` (`List<Transition {ToState, ChancePerHour, Conditions}>`), `RunningTransitions` (`List<TransitionList {FromState, Transitions}>`)
- classes: `WeatherStateDefinition`: `StateName`, `Activities` (`List<{AbstractActivity, Intensity}>`), `MinDurationHours`, `MapTier` 0..3 (map chevrons)
- activities (`Weather.Activity`): rain / wind / regional VFX, `PlayMusicActivity`, `ApplyForceToPlayer/Actors`, `ApplyStatusEffect`, `SpawnActorActivity`, `SpawnTemporaryObjectActivity` (lightning), `SpawnTornadoActivity`, `ReleaseResourcesActivity`, `WaterCropsActivity`, `UnlockPediaActivity`, `SpawnTangleStalkActivity`
- classes: `ZoneWeatherConfig` (`Zone`, `Patterns`, `PresetWeather`, `ForecastConditions`); `ForecastEntry` (`Pattern`, `State`, `StartTime`, `IsEndTimeKnown`, `EndTime`, `Started`; world-time seconds)
- gotchas: Transition / TransitionList are nested: offsets read from the first live object

### WeatherRegistry  [SceneContext.WeatherRegistry, scene MonoBehaviour; SceneContextUtility.TryGetWeatherRegistry]
- fields: `_zones: Dictionary<ZoneDefinition, ZoneWeatherData>` (`Config`, `_modelData` WeatherModel.ZoneData, `CurrentWeatherPatterns` List<IWeatherPattern> of `WeatherPattern {_definition, _activeState}`, `NextChangeAt`, `ForecastUnlocked`, `Director`), `_model`, `_firstLoad`
- methods: `Update` 0x6781A0 - per zone once `worldTime >= NextChangeAt`: endable entries -> `StopPatternState` + `RemoveWeatherPatternInstance`, removed from forecast; startable -> `Started = true`, instance found/created, its state stopped, `RunPatternState(entry.State, immediate: _firstLoad)`; `UpdateChangeTimestamp` (IDA)
- methods: `CheckForForecast` / `Forecast(zone, from, to)` (`DaysToForecast`, `ForecastHourIntervalLow/High`, `ChooseTransition`, cooldowns, conditions)
- methods: `RunPatternState` 0x6771E0 = `pattern.EnterState(state)` + if director `WeatherDirector.RunState(state, zoneParams, immediate)` 0x671E20 (every activity's Start; `_runningStates`); `StopPatternState` 0x677D30 -> `ExitState` + `WeatherDirector.StopState` 0x6721C0 (fade unless immediate); `RegisterDirector` 0x676DC0 (zone scene loaded) runs every current state immediately; `CalculateZoneMapData` 0x675110 (map shows CurrentWeatherPatterns icon + MapTier, not forecast)
- methods (debug, real bodies): `DEBUG_GetWeatherZones()` (new list of keys), `DEBUG_GetZoneWeathers(zone)` (config Patterns, live), `DEBUG_GetRunningWeather(zone)` (CurrentWeatherPatterns, live), `DEBUG_GetForecast(zone)` (model Forecast, live), `DEBUG_SetZoneWeather(zone, pattern, state)` 0x675B10 (needs `_model`; instance found/created, current state stopped, `RunPatternState(state, immediate:false)`; state null -> `RemoveWeatherPatternInstance`)
- gotchas: `_firstLoad` set by `SetModel`, cleared at end of first Update after a load (save's running entries start then); changes before that are undone. Zone lookups use `Dictionary.get_Item` (throws for a zone without weather): only pass zones from DEBUG_GetWeatherZones. The ranch (The Conservatory) is not a weather zone
- mod: sb weather.cpp (catalog, Op::Start / Natural, 1 Hz Enforce)

### WeatherModel (save)
- methods: `Pull/Push` (`WeatherV01`, `WeatherZoneEntryV01`, `WeatherForecastEntryV01`, `WeatherIndexTableV01`): per zone forecast entries (persistence index), last completion times, wind params
- gotchas: `CurrentWeatherPatterns` NOT saved; after load running weather rebuilt from started forecast entries
