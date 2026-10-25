# Save system and the mod sidecar

`MP.SR` = `MonomiPark.SlimeRancher`. Tags: **code** = decompiled/disassembled; **data** = asset bundles. Mod use: `modules/custom_upgrades/` (sidecar owner, `api.h`), `modules/sandbox/cheated_save.cpp` (cheated mark), every module that stores per-save values. Hooks: re/hooks.md.

## Rule: the vanilla save never holds anything of ours

Custom ids (`0x5352xxxx`, "SR" << 16; `custom_upgrades::IsCustomUpgrade`) never enter vanilla sets, enums or inventories. Mod-only objects are saved as their vanilla twin (variant gadgets as the base gadget, drone programs as Coop/Silo, Shadow slots as empty) and the truth goes into the sidecar. A mod-free load must work.

## File naming and rotation (code)

- Folder: `SteamContext.GetStorageProvider` 0xB2FE60 builds `FileStorageProvider` with `savePath = Path.Combine(persistentDataPath, "Steam", <steam id>)`; `GetFullFilePath(name)` = `savePath\<name>.sav`. Steam Cloud syncs that folder (steam_autocloud.vdf): keep mod files out of it.
- `AutoSaveDirector.SaveGameImpl` 0x5D10A0: takes `_currentGameMetadata` (Nullable<GameMetadata> +0x68: hasValue +0x68, GameName +0x70, DisplayName +0x78, SaveNumber +0x80, SlotIndex +0x88), stores `IncrementSaveNumber()`, pulls the game, then loops `string.Format("{0}_{1}", GameName, i)` until `StorageProvider.HasGameData(name)` is false and calls `StorageProvider.StoreGameData(DisplayName, name, stream)`. Then `CleanupAutosaves` 0x5CE040 deletes saves of that game past the 5 newest (`GetSavesByGameName`, newest first). Rotation indices are reused: the index is not an age order; the save number is.
- `FileStorageProvider.StoreGameData` 0x744BC0 (custom_upgrades hooks it to learn the file name).
- `AutoSaveDirector.Load(GameSaveIdentifier, bool, Stream)` 0x5CFDC0: `GameSaveIdentifier` = `{string GameName; string SaveName}` (16 bytes, **by reference on x64**), SaveName = file name without `.sav`. Sets `_currentGameMetadata = new GameMetadata(loadedGame)` before returning.
- `AutoSaveDirector.StartNewGame(int, GameSettingsModel)` 0x5D1A70; `CurrentSaveGameName()` 0x5CE220 (`_currentGameMetadata.GameName` or ""); `DeleteGame(string)`; `SaveProfile()` (options). AutoSaveDirector = `GameContext.Instance.<AutoSaveDirector>k__BackingField`.
- On the title screen the metadata keeps the last game: "in game" = `SceneContext.GameModel` set.

## Save slots: what the user sees vs `SlotIndex` (code, checked against the user's saves)

- **The slot the user sees = `SlotIndex + 1`.** `SaveGamesRootUI.CreateBehaviorModels` 0x7348C0 (IDA) makes an array of
  `AutoSaveDirectorConfiguration._saveSlotCount` (+0x64) buttons, takes each game's newest summary
  (`AvailableGamesByGameName()` values, `<CreateBehaviorModels>b__20_0` = `list[0]`), orders them by `SaveSlotIndex`
  and puts the game into button `SaveSlotIndex` (out of range: logged "Game Summary {0} has save slot index {1}. This
  is out of range", not shown); the empty buttons become New Game buttons (`NewGameUISetup.SelectedIndex` = that index
  -> `NewGameOptionsUIRoot.SaveSlotIndex` -> `AutoSaveDirector.StartNewGame(SaveSlotIndex, settings)`).
- **The slot is also in the game's name.** `StartNewGame` 0x5D1A70 (IDA): `saveSlotIndex` must be in
  `[0, _saveSlotCount)`; `GameName = string.Format("{0:yyyyMMddHHmmss}_{1}", DateTime.Now, saveSlotIndex + 1)` (plus
  `"_<i>"` while that name has data); `DisplayName` = `(saveSlotIndex + 1).ToString()`; `GameMetadata(name, display,
  0, saveSlotIndex)`. So save files are `<yyyyMMddHHmmss>_<slot>_<rotation>.sav`.
- Checked read-only against the user's save folder (2026-10-25): four games `20261020220332_1`, `20261022183338_2`,
  `20261023190137_3`, `20261024162000_4` = slots 1-4 (SlotIndex 0-3). Slots 5 and 6 (SlotIndex 4 and 5, names ending in
  `_5` / `_6`) are the test slots of `modules/autotest` (it refuses anything else).
- `Summary` (MP.SR.Persist): `Name` +0x10 (GameName), `DisplayName` +0x18, `SaveSlotIndex` +0x20, `SaveTimestamp`
  DateTimeOffset +0x48 (`_dateTime` UTC ticks in the low 62 bits, `_offsetMinutes` short at +8), `SaveName` +0x58,
  `SaveNumber` +0x60, `IsInvalid` +0x44, `GameOver` +0x45; `get_SaveIdentifier()` -> `GameSaveIdentifier`.
  `AvailableGamesByGameName()` returns a fresh `Dictionary<string, List<Summary>>` (corefx layout: `_entries` of
  `{uint hashCode; int next; string key; List value}` = 24 bytes, `_count`). autotest reads it (game.cpp).
- Quit without saving (autotest `quit_to_title`): `PauseMenuDirector.Quit` = `SaveAllNow()` then
  `SceneContext.OnSessionEnded()` and `SystemContext.SceneLoader.LoadMainMenuSceneGroup(null)`; the harness makes the
  last two calls only. `AutoSaveDirector.OnSessionEnded` 0x5D08E0 clears `_currentGameMetadata` (hasValue false on the
  title screen after a quit).
- Every game save goes through `SaveGameImpl` (IDA callers: `OnApplicationQuit`, `SaveAllNow`, `SaveGameAndFlush`,
  `SaveGame_GetSummary`, `SaveGame`); returning null there skips the save and every caller handles null (no summary, the
  profile is still saved). autotest's save guard vetoes through custom_upgrades' detour (re/hooks.md).

## Actors and persistence ids (code/data)

- `GameModelPullHelpers.PullActors` 0x5E5C00: writes `BuildActorData(actor)` for every actor in `GameModel.AllActors()` whose type is not in `GameModel.ignoreOnLoad` (group "SceneObjectGroup" = Player only). No other filter, no despawn on save. `PushActors` / `PushActorData` 0x5EC010 / 0x5EB980 load them: skip non-finite positions, **throw for an unknown type id**, skip unknown scene groups.
- `SaveReferenceTranslation`: `GetPersistenceId(IdentifiableType)` (custom_upgrades hooks it to map mod gadget definitions to their vanilla twin); `_primaryIndex` (the save's id table) and `_reverseIndex` (ReferenceId -> persistence id; mod aliases are added there). A type whose ReferenceId is not a key is unsaveable (`Sandbox::Saveable` filters on it; 12 vanilla types are, e.g. SpicyTofu, GingerVeggie, GlitchTarr, LemonPhase; Shadow is saveable).
- `GadgetsModel.Pull` throws on an unknown or duplicate id: mod definitions must never enter `_itemCounts` / blueprints.
- `LandPlotModel.upgrades` HashSet<LandPlot.Upgrade> (enum 1..20) saved as a list of ints; how vanilla reads an unknown value is unchecked, so custom ids never go in.
- Ammo: re/vacpack.md (`AmmoDataV02`, `AmmoModel.Push`, `ValidateAndAdjustSlots`). Resource nodes: re/world.md (`ResourceNodeV01`). Options: re/ui.md (`OptionsV02`). Drone tasks: `DroneTaskV01`, `PlacedGadgetV07.ActorId` (stable across loads; re/drones.md). Quantum Cloud: `DroneCloudV01`.

## The sidecar ("extended save file")

Owner: `modules/custom_upgrades/` (sidecar code in `module.cpp`; generic API `custom_upgrades::PlotValue / SetPlotValue / SaveValue / SetSaveValue` in `api.h`).
- One file per .sav in the game folder: `sr2mods_saves\<GameName>_<i>.sav.json`, written after `SaveGameImpl` returns (name noted in the `StoreGameData` detour), temp file + rename; sidecars whose .sav is gone are deleted.
  ```json
  { "format": 2, "game": "<GameName>", "save": "<GameName>_<i>", "save_number": 42, "values": {},
    "plots": { "<plot id>": { "type": 5, "upgrades": ["silo_sell_all"], "values": {} } },
    "sandbox": { ... } }
  ```
  Plots keyed by `LandPlotLocation._id` (+ plot type). `"sandbox"` = the cheated mark (`modules/sandbox/cheated_save.cpp`), kept by `WriteSidecar` when its save_number matches; a file with only that entry counts as no sidecar for custom upgrades.
- Read lazily, in this order: (1) `<SaveName>.sav.json` if `save` and `save_number` match; (2) legacy per-game `<GameName>.json` (format 1; renamed `.json.migrated` after the first per-save write); (3) this game's sidecar with the highest save_number below the loaded one; (4) nothing. Re-enabling the module re-reads using `_currentGameMetadata` (GameName + SaveNumber).
- Once a game had a sidecar, every save writes one (also empty). Purchases not saved vanish with the unsaved Newbucks (state dropped in the `Load` / `StartNewGame` detours).
- Limits: a save written with the mod off has no sidecar (the newest older one is used); Steam Cloud does not carry sidecars; deleted games leave sidecars.
- Known keys (grep `SetSaveValue` / `SetPlotValue` for the full list): `custom_upgrades.on`, `custom_upgrades.vacpack.*`, `drone_price_limit.station.<actor id>`, `drone_programs.station.<actor id>`, `drone_hen_source.stations`, `drone_elder_source.stations`, `drone_quantum_source.stations`, `quantum_link.actors` / `.kits`, `dj.*`, plot `hen_collector*`.

## From the custom_upgrades notes

### SaveReferenceTranslation / PersistenceIdLookupTable
- methods: `SaveReferenceTranslation.GetPersistenceId(IdentifiableType)` 0x60DDC0 - reached only through the interface; = `PersistenceIdLookupTable._reverseIndex[ReferenceId]` (no fallback: KeyNotFoundException)
- fields: `_reverseIndex` (ReferenceId -> persistence id; rebuilt when content is added), `_primaryIndex` (string[], the save's id table, separate)
- gotchas: adding an alias to `_reverseIndex` writes nothing new to the save
- mod: quantum_link.cpp (GetPersistenceId detour + `_reverseIndex` alias every tick; DJ via variant_ext.h)

### Save contents relevant to mod objects
- `ViewedItems` keeps viewed blueprints as `IdentifiableType.StableHashedId` ints (Animator.StringToHash(name)) - harmless for a mod-free load
- favourites saved as persistence ids (GadgetsModel.Pull)
- `PlacedGadgetV07.ActorId` stable across loads
- `GameModel.ignoreOnLoad` ("SceneObjectGroup" = the player) - every other actor saves; Shadow has a persistence id
- `SlimeModel.Emotions` saves hunger
- `AmmoDataV02` = id, count, emotions, appearance, radiant (no max) (in re)
- `ShopDataModel` saves purchase counts and pending spawn queues
- `DroneTaskV01`, `DroneCloudV01` (see re/drones.md)

## From the slime_dj, spawn_scaling, hl2_movement, quick_start, free_placement, misc_tweaks, plot_upgrades, drone_price_limit notes

- `AutoSaveDirector`: `GetSaveToContinue()`, `HasContinue()`, `BeginLoad(SaveIdentifier, null)` (2 args), `StartNewGame` (load step `<>c.<StartNewGame>b__29_2` 0x5E4150 -> `CutsceneDirector.SpawnIntroSequence`) - quick_start
- `Persist.ResourceNodeV01` (see re/world.md) - spawn_scaling
- `PlacedGadgetV06.ActorId` = `IdentifiableModel.actorId` of a gadget model (drone station key, stable across save/load) - drone_price_limit
- `PlacedGadgetV07.Rot` (Vector3 euler, all three axes) - free_placement
- mod sidecar per-plot value `slime_dj.radio` (custom_upgrades) - slime_dj (format in slime_dj NOTES)

## From the sandbox, sell_upgrades, collectibles, instant_build notes

### AutoSaveDirector  [GameContext.AutoSaveDirector]
- fields: `_saveReferenceTranslation` +0x40 (`SaveReferenceTranslation`), `_configuration._identifiableTypes` (IdentifiableTypeGroup asset), `_currentGameMetadata` (Nullable<GameMetadata>: hasValue +0, struct +8; GameName, SaveNumber - incremented already after a save)
- methods: `BeginLoad(GameSaveIdentifier, Stream)` 0x5CDB50 - struct `{string GameName; string SaveName}` (16 bytes, x64) arrives by reference; SaveName = .sav name without extension. Save list loads and Continue go through it (`LoadGameBehaviorModel.InvokeBehavior`); `LoadFallbackSave` does not
- methods: `CleanupAutosaves(string)` 0x5CE040 - only caller `SaveGameImpl` (call at 0x5D15FA, after the file is stored, success path only; found by scanning the binary)
- chains: `SaveGameImpl` -> `StoreGameData` -> `CleanupAutosaves` -> return -> custom_upgrades `AfterSave`; `SaveGameImpl` -> `PullGame` -> `PullActors` -> `GameModelPullHelpers.BuildActorData` (throws KeyNotFound for unsaveable types; save aborts before writing)
- gotchas: SaveGameImpl takes the first free `<GameName>_<i>` (rotation names are reused); no "copy save" function exists; save files under `FileStorageProvider.savePath`
- mod: sb cheated_save.cpp hooks `sandbox_saves` (BeginLoad pre, CleanupAutosaves post)

### SaveReferenceTranslation / persistence ids
- fields: `_identifiableTypeToPersistenceId` +0x20 (`PersistenceIdLookupTable<IdentifiableType>`) -> `_reverseIndex` (`Dictionary<string,int>` keyed by `IdentifiableType.ReferenceId`, e.g. `SlimeDefinition.GlitchTarr`)
- methods: `GetPersistenceId(IdentifiableType)` - `_reverseIndex[ReferenceId]`, no fallback (throws); `OnIdentifiableTypesListChanged` (from `AutoSaveDirector.Awake` and `OnAdditionalContentAdded`) builds from `_configuration._identifiableTypes.GetAllMembers()`, non-null only
- gotchas: a type outside that group exists as an asset (FindObjectsOfTypeAll sees it) but can never be saved (Glitch Tarr): spawning it breaks EVERY save while it exists. Ammo slots and gadget inventory also go through `ISaveReferenceTranslation.GetPersistenceId`. Generic classes resolved from live objects; check with `ContainsKey`
- facts: crates, giant eggs, vine bulb, both Mysterious Statues are in the save's type list
- mod: sb "only saveable" filter for vacpack, spawner, crafter, materials

### Mod sidecar `sr2mods_saves\<GameName>_<i>.sav.json`
- facts: shared with custom_upgrades (same SanitizeFileName); sb writes only its `"sandbox"` entry (cheated, since, game, save, save_number, enabled, toggles, weather); custom_upgrades `WriteSidecar` keeps a `sandbox` entry with matching save_number; a sandbox-only sidecar reads as `kForeignSidecar` (no custom data) for custom_upgrades; su stores `sell_upgrades.ash_trough` per plot via custom_upgrades api `SetPlotValue`
- gotchas: sidecars live in the game folder, not Steam Cloud
- mod: sb cheated_save.cpp

### Persistence of actors
- facts: currency `CurrencyDefinition._persistenceId == 1` = Newbucks; `AccessDoorModel.state` is saved; `GadgetsModel.blueprints` saved by Pull; upgrade modifiers runtime only; weather running patterns not saved; time pause / fast-forward not saved
