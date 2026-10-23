# SR2 symbols for the native mod suite

- Game: Slime Rancher 2 **1.3.0**, Steam buildid **24131940**, Unity 6000.0.73f1, IL2CPP metadata v31
- GameAssembly.dll SHA-256: `786006ede0d198f6e15a2c01f90ef9108afe694745fa0ad894e1c3ed342385b7` (86,650,368 bytes)
- Image base 0x180000000. IDA VA = 0x180000000 + RVA.
- IDA database: `re/ida/GameAssembly.i64` (IDA 9.4). Il2CppInspectorRedux `il2cpp_ida.py` was applied in full (names, types and string literals). It took 14m35s with no type errors. The script skips function boundaries because `.pdata` is present, which is the script's expected behaviour.

**The mod resolves everything by name at runtime.** Use `il2cpp_domain_assembly_open("Assembly-CSharp")` → `il2cpp_class_from_name(image, ns, name)` → `il2cpp_class_get_method_from_name(klass, name, argc)` / `il2cpp_class_get_field_from_name` + `il2cpp_field_get_offset`. Nested classes are resolved with `il2cpp_class_get_nested_types`. RVAs and offsets in this file are for reference and cross-checking only.

## Verification levels

| Tag | Meaning |
|---|---|
| **IDA** | Decompiled in IDA (Hex-Rays) this session. The behaviour column describes the actual code. |
| **dump** | Signature, RVA and field offsets agree between Cpp2IL DiffableCs, Il2CppInspector `il2cpp-functions.h` and `il2cpp-types.h`, and IDA applied the name at that RVA. The behaviour is inferred from names and types and was **not decompiled**. IDA `execute_python` was blocked by the permission classifier part-way through the session, so those bodies could not be read. |

## Shared (folded) bodies: never hook these

IL2CPP/ICF folds identical bodies. IDA shows a single arbitrary name for each of these addresses:

| RVA | Body | Examples of methods mapped here |
|---|---|---|
| 0x4ED3F0 | empty `ret` (IDA name: `SRAssert.EditorAreNotEqual<…>`) | `OptionsItemDefinition.Awake`, `AbilityBehavior<T>.Update/UpdateVelocity/LateUpdate`, `OnSceneLoaded`, many more |
| 0x4E3350 | `return true` (IDA name: `VerticalVirtualizationController.get_alwaysRebindOnRefresh`) | `OptionsItemDefinition.get_Cacheable/ShouldEnable`, `JetpackAbilityBehavior.get_UsesEnergy`, `AbilityBehavior.get_StopsOnTeleport` |
| 0x4DD840 | `return *(this+0x10)` | `InputActionMapReference.get_Map`, `MapDirector.Marker.get_Source`, `OptionsViewModel.get_ProfileSpecificOptions` |
| 0x4E3AE0 | `return *(this+0x18)` | `ResourceNodeDirectorConfig.get_ResourceNodeWeightMap`, `AbilityData.get_StartInputEvent`, `ListAsset` getters |
| 0x4DCFB0 / 0x516AE0 / 0x531320 / 0x52D890 / 0x4F9D90 | `return *(this+0x20/0x28/0x30/0x38/0x40)` | `OptionsItemDefinition.get_Label/DetailsText/ControlPrefab`, `PlayerZoneTracker.GetLatestZone` (0x4F9D90), `ResourceNodeDirector.get_NodeSpawners` (0x52D890) |
| 0x5732F0, 0x4EE590, 0x4EBExx, 0x539Cxx/0x539Dxx | int/float field getters | `ResourceNodeDirectorConfig.get_MaxActiveNodes/UpdateNodesIntervalHours`, all `CharacterControllerParameters` and `JumpAbilityData` getters |
| 0x4DCFC0, 0x4E3290 | empty ctor / `ScriptableObject` base ctor | every `<>c` ctor, `OptionsItemDefinition()`, `AbilityData()` |
| 0x1EC9xxx-0x1ECAxxx, 0x1715xxx, 0x171Bxxx | generic `AbilityBehavior<T>`, `SRAbilityBehavior<T>` and `ScriptedValue<T>` code | shared by every instantiation. Calling them is fine; hooking them hits every ability or every ScriptedValue |

Patch the data instead of hooking a shared getter.

---

## Module 1: Settings tab ("Mods" category)

Namespace abbreviations: `MP.SR` = `MonomiPark.SlimeRancher`.

| Class (namespace) | Member | Kind | RVA / offset | Verified behaviour | Hook-safe? |
|---|---|---|---|---|---|
| OptionsUIRoot (MP.SR.UI.Options) | `Start()` 0 args | method | 0x71CEF0 | **IDA.** Calls `DisplayBaseUI.Start` and creates `OptionsDetailsView`. Creates `OptionsViewModel(profileOptions=GameContext…OptionsDirector._optionsModel, gameOptions=SceneContext…GameSettingsModel)` into +0x230. Sets `optionsConfiguration`(+0x238) from `optionsConfigurationProvider`(+0xD0)`.GetAssetForCurrentPlatform()`, then calls `BindCategories(optionsConfiguration, aboutCategory(+0xD8))`, then starts the `LateStart` coroutine. Runs once each time the options screen is instantiated. | Yes (unique, once per open) |
| OptionsUIRoot | `BindCategories(OptionsConfiguration, OptionsAboutCategory)` 2 args | method | 0x71A010 | **IDA.** `categories = this.categories.Concat(config).Append(about).Where(c => c.CheckIsActive()).ToList()`, stored at +0x240. Then `categoryAdapter(+0xE0).BindDataList(categories.Select(c => new CategoryViewModel(c.Title, c.Icon)).ToList(), 0)`. **Pre-existing entries in `this.categories` come first**, and the about tab is always last. Only caller is `Start`. | Yes (unique, once per open) |
| OptionsUIRoot | `SwapCategory(int categoryIndex, bool changesAppliedWhenChangingCategories)` 2 args | method | 0x71D1A0 | **IDA.** `cat = categories[idx]`. If `cat` is an `OptionsItemCategory` (exact class-hierarchy check) it calls `BindItemCategory(cat)`. Else if it is an `OptionsAboutCategory` it calls `BindAboutCategory`. Otherwise it **does nothing**. Then sets `currentCategoryIndex`(+0x210), plays the cue and runs `ForceButtonsPanelLayout`. ⇒ **An injected tab must be a real `OptionsItemCategory` instance.** | Yes |
| OptionsUIRoot | `BindItemCategory(OptionsItemCategory)` 1 arg | method | 0x71A2F0 | **IDA.** Sets the Apply/Revert buttons and shows `itemsPanel` (hides `aboutPanel`). Sets rebind-button visibility from `_showRebindButton`. Then `optionsItemModels(+0x208) = optionsViewModel.GetOptionsItemsForCategory(optionsConfiguration, category, true).ToList()` and `optionsAdapterView(+0x140).BindDataList(models)`. Hooks `OnItemSelect` and finishes with `BindOptionsDetailsView(0)`. | Yes |
| OptionsUIRoot | `CheckThenSwapCategory(int)` 1 arg | method | 0x71A710 | dump (IDA decompile failed). Prompts save/revert, then calls `SwapCategory` through `<>c__DisplayClass56_0.<CheckThenSwapCategory>b__0` 0x71F9B0 (**IDA**: forwards to `SwapCategory`). | Yes |
| OptionsUIRoot | `ApplyChanges()` 0 args | method | 0x719A30 | **IDA.** For each model in `optionsItemModels` where `IOptionsItemModel.HasUnsavedSelection()` (interface slot 6) is true, calls `SaveSelectedValue()` (slot 9), catching exceptions into a popup. Then `SaveSettings()` (→ `AutoSaveDirector.SaveProfile()` + `InputDirector.NotifyKeyBindingsChanged()`). | Yes |
| OptionsUIRoot | `TempApplyChanges()` 0 args | method | 0x71D320 | **IDA.** Same loop, calling slot 10 (`ApplyTemporaryValue`). | Yes |
| OptionsUIRoot | `MaybeApplyChanges()` 0 args | method | 0x71B760 | **IDA.** If any model needs confirmation it calls `SaveOrRevert`. Else if any `HasUnsavedSelection` it calls `ApplyChanges()`. Else it plays the "nothing to apply" cue. | Yes |
| OptionsUIRoot | fields | field | `categoryAdapter` 0xE0, `optionsAdapterView` 0x140, `optionsItemModels` 0x208, `currentCategoryIndex` 0x210, `optionsViewModel` 0x230, `optionsConfiguration` 0x238, **`categories` List\<IOptionsCategory\> 0x240** | dump + IDA (used as shown) | - |
| OptionsViewModel (MP.SR.UI.Options) | `GetOptionsItemsForCategory(OptionsConfiguration, OptionsItemCategory, bool profileSettingsFirst)` 3 args | method | 0x71E0E0 | **IDA.** Builds the row models as `GameSpecificOptions(+0x18).GetOptionModelsFromDefinitionList(config.GetDefinitionsFromCategory(cat)).Where(ShouldDisplay)` concatenated with the same for `ProfileSpecificOptions(+0x10)`. **Models are looked up in OptionsModel or GameSettingsModel and are not created here.** Whether an unknown definition is skipped or lazily created is **unverified** (`OptionsModelBase<,>.GetOptionModelsFromDefinitionList` 0x15BBC30 and `TryGetOptionsModelFromDefinition_NoRebuild` 0x15BC1A0 were not decompiled). | Yes (unique). **Best row-injection hook.** |
| OptionsConfiguration (MP.SR.Options) : ListAsset\<OptionsItemCategory\> | `items` (ListAsset backing List) | field | 0x18 | dump (il2cpp-types.h `ListAsset_1_…OptionsItemCategory___Fields.items`) | - |
| OptionsConfiguration | `GetDefinitionsFromCategory(OptionsItemCategory)` 1 arg | method | 0x9C54C0 | dump | Yes |
| OptionsItemCategory (MP.SR.Options) : ListAsset\<OptionsItemDefinition\>, IOptionsCategory | `items` 0x18, `_title` LocalizedString 0x20, `_icon` Sprite 0x28, `_showRebindButton` 0x30, `_activeCondition` 0x38 | field | - | dump. Ctor 0x9C6C50 (**IDA**: just the `ListAsset` ctor). | - |
| OptionsItemCategory | `CheckIsActive()` 0 args | method | 0x9C6BC0 | **IDA.** Returns `true` if `_activeCondition == null`, else `_activeCondition.<virtual>()`. ⇒ leave `_activeCondition` null on a runtime-created category. | Yes, but no need |
| OptionsItemDefinition (MP.SR.Options), abstract ScriptableObject | `_referenceId` 0x18, `_label` 0x20, `_detailsText` 0x28, **`_controlPrefab` GameObject 0x30**, `_applyImmediately` 0x38, `_isProfileSetting` 0x39, `_requireConfirmation` 0x3A, `_confirmationPopupConfig` 0x40, `_showTutorialDisclaimer` 0x48, `_indent` 0x4C, `_showCondition` 0x50 | field | - | dump | - |
| OptionsItemDefinition | `CreateInstance<TItemDefinition>(string name, TItemDefinition other)` static, 2 args (generic) | method | 0xF73740 (shared `<object>` instantiation) | **IDA.** `Object.Instantiate(other)`, `set_name(name)`, `ResetReferenceId()` (sets `_referenceId = "setting." + name`). **This is the game's own clone helper for a row definition.** Call it with an inflated `MethodInfo` (`il2cpp` generic method), or reproduce it: `Object.Instantiate` + `set_name` + write `_referenceId`. | Call only |
| PresetOptionsItemDefinition (MP.SR.Options) | `CreateOptionItemModel()` 0 args | method | 0x9C9D20 | **IDA.** `new PresetOptionsItemModel(this)`, adds it to `_optionsItemModels`(+0x60) and returns it. ⇒ **Call this on our definitions to get row models without touching OptionsModel.** | Call (virtual; shared by Toggle/Range) |
| PresetOptionsItemDefinition | `_wrapAround` 0x58, `_optionsItemModels` 0x60, `SupportedInputDeviceAssets` 0x68, `SupportedPlatforms` 0x70 | field | - | dump | - |
| ToggleOptionDefinition (MP.SR.Options) : PresetOptionsItemDefinition | `ApplyPresetSelection(int index)` 1 arg | method | 0x9D0D90 | **IDA.** `TargetBool.set_Value(index == 1)`. That is the whole body. | Yes (unique). Filter on `this` == our definition |
| ToggleOptionDefinition | `TargetBool` ScriptedBool 0x78, `_negativeReferenceId` 0x80, `_positiveReferenceId` 0x88, `_negativeOption` 0x90, `_positiveOption` 0x98, `_defaultValue` bool 0xA0 | field | - | dump | - |
| ScriptedValueRangeOptionDefinition (MP.SR.Options) : PresetOptionsItemDefinition | `ApplyPresetSelection(int index)` 1 arg | method | 0x9D0030 | **IDA.** `v = index*_valueStep + _minValue`. If `_normalizeOutputValue`, `v = (v-min)/(max-min)`. Then `_targetValue.set_Value(v)`. **Stepper with (max-min)/step+1 entries, not a free slider.** | Yes (unique) |
| ScriptedValueRangeOptionDefinition | `GetValueForIndex(int)` 1 arg | method | 0x9D0440 | **IDA.** `index*_valueStep + _minValue` | tiny, avoid |
| ScriptedValueRangeOptionDefinition | `_targetValue` ScriptedFloat 0x78, `_minValue` 0x80, `_maxValue` 0x84, `_valueStep` 0x88, `_normalizeOutputValue` 0x8C, `_defaultValue` 0x90, `_displayFormat` LocalizedString 0x98 | field | - | dump | - |
| PresetOptionsItemModel (MP.SR.DataModel) : OptionsItemModel | ctor `(PresetOptionsItemDefinition)` 1 arg | method | 0xAC7D00 | **IDA.** Stores the definition (+0x48), `_presets`(+0x50) = `def.GetOptionsPresets()`, `CurrentPresetIndex` = `def.GetDefaultPresetIndex()`, `_tempPresetIndex`(+0x58) = current. | - |
| PresetOptionsItemModel | `SelectValue(int)` 1 arg | method | 0xAC7C60 | **IDA.** `_tempPresetIndex = index` and fires `OnTempSelectionChanged`(+0x40). If changed, it also calls the `OnSelectionChanged`(+0x38) path. This is what the left/right selector calls. | Yes |
| PresetOptionsItemModel | `ApplySelectedValue()` / `ApplyValue(int)` | method | 0xAC7070 / 0xAC70B0 | **IDA.** `ApplyValue(CurrentPresetIndex)` checks cacheability, then calls virtual `def.ApplyPresetSelection(index)` inside try/catch. | Yes |
| PresetOptionsItemModel | `SaveSelectedValue()` | method | 0xAC7A60 | **IDA.** `ApplyTemporaryValue()`, then `CurrentPresetIndex = _tempPresetIndex`. Has extra handling for `OptionGroupPresetOptionDefinition`. | Yes |
| PresetOptionsItemModel | `CreateOptionsControl()` | method | 0xAC73E0 | **IDA.** `Instantiate(def._controlPrefab)`, then `GetComponent<OptionsPresetSelectorBase>().BindData(this)` and returns the GameObject. **This is how `_controlPrefab` becomes a row control.** | Yes |
| OptionsItemModel (MP.SR.DataModel) | `optionsItemDefinition` 0x20, `optionValueKey` 0x28, `ValuePushed` 0x30 | field | - | dump | - |
| IOptionsItemModel (MP.SR.Options) | interface: `ApplySelectedValue, ApplyTemporaryValue, CreateOptionsControl, DiscardSelectedValue, get_*, GetDetails, HandleApplyError, HasUnsavedSelection, Initialize, Reset, SaveSelectedValue, ShouldDisplay, ShouldEnable` | - | - | dump. IDA showed `OptionsUIRoot` using slot 6 as the "has changes" filter, slot 9 in Apply and slot 10 in TempApply. | - |
| ScriptedValue\<T\> (MonomiPark.ScriptedValue); ScriptedBool / ScriptedFloat | `OnValueChanged` delegate 0x18, `DefaultValue` 0x20, `_value` (bool 0x21 / float 0x24), `_valueInitialized` (bool 0x22 / float 0x28) | field | - | dump (il2cpp-types.h). `set_Value` bool 0x171B6B0, float 0x171B690. `get_Value` bool 0x171B340, float 0x171B2E0. | **Shared generic: don't hook.** Read `_value` or subscribe `add_OnValueChanged` |
| OptionsDirector (MP.SR.Options) : SRBehaviour | `Awake()` 0 args | method | 0x9C5570 | **IDA.** `_optionsItemDefinitions`(+0x68) = provider.GetAssetForCurrentPlatform(). Registers `sceneLoaded`. `_optionsModel`(+0x60) = `new OptionsModel(GetRuntimeOptionsListFor(defs)._profileOptions)`. ⇒ **OptionsModel's set of rows is fixed at Awake.** | Yes |
| OptionsDirector | `PullSettings(OptionsV02)` / `PushSettings(OptionsV02)` 1 arg | method | 0x9C5E30 / 0x9C5E60 | **IDA.** One-liners: `_optionsModel.Pull(data)` / `_optionsModel.Push(data)` (virtual). Persistence is `OptionsV02.OptionItems` List\<OptionItemDataV01\> (+0x10), keyed by definition referenceId. | Yes, but not needed |
| RuntimeOptionsItemList (MP.SR.Options) | ctor `(IReadOnlyList<OptionsConfiguration>, IReadOnlyList<OptionsItemDefinition>)`; `_profileOptions` 0x10, `_gameOptions` 0x18 | method/field | 0x9CF970 | dump | - |

### Recommended hook points: Settings

1. **Post-hook `OptionsUIRoot.BindCategories` (2 args), or pre-hook it.** Before the original runs, append our runtime `OptionsItemCategory` to `this.categories` (+0x240): the list is concatenated *first*, so "Mods" becomes tab 0. To put it before "About" instead, append it once to `OptionsConfiguration.items` (+0x18) in the asset: it then appears after the stock tabs. Cloning UI after the bind is not needed, because the tab strip is fully data-driven (`CategoryViewModel(Title, Icon)`).
   - Create the category with `ScriptableObject.CreateInstance(OptionsItemCategory)`.
   - Fill `items` with our definitions, and copy `_title`/`_icon` from an existing category. `_title` must be a `LocalizedString`, so either reuse one or set the tab label TMP text after the bind.
   - Leave `_activeCondition` null.
2. **Hook `OptionsViewModel.GetOptionsItemsForCategory` (3 args).** When `category == ourCategory`, return a `List<OptionsItemModel>` built by calling `def.CreateOptionItemModel()` on each of our definitions. This avoids depending on the unverified OptionsModel lookup, because our definitions are not in `OptionsModel` (built in `OptionsDirector.Awake`).
3. **Rows:** clone an existing `ToggleOptionDefinition` and an existing `ScriptedValueRangeOptionDefinition` with `Object.Instantiate` + `set_name` + `_referenceId`, which is what `OptionsItemDefinition.CreateInstance` does.
   - Point `TargetBool` (+0x78) or `_targetValue` (+0x78) at our own `ScriptableObject.CreateInstance(ScriptedBool/ScriptedFloat)`.
   - Set `_label`, and for ranges `_min/_max/_valueStep/_defaultValue`.
   - The cloned `_controlPrefab` already works.
4. **Value delivery path:** selector → `PresetOptionsItemModel.SelectValue(idx)` (temp) → Apply button → `OptionsUIRoot.ApplyChanges` → `SaveSelectedValue` → `ApplyTemporaryValue`/`ApplyValue` → `def.ApplyPresetSelection(idx)` → `ScriptedBool/ScriptedFloat.set_Value` → `OnValueChanged`. Two ways to observe it:
   - Read our ScriptedValue's `_value` (bool +0x21, float +0x24) or subscribe to `OnValueChanged`.
   - Hook `ToggleOptionDefinition.ApplyPresetSelection` / `ScriptedValueRangeOptionDefinition.ApplyPresetSelection` (unique bodies) and filter on `this`.

   Set `_applyImmediately` (+0x38) if the row should apply without the Apply button (not decompiled).
5. **Persistence:** our rows are not in OptionsModel, so the game will not save them. Keep the mod's own INI, and load it into the ScriptedValues and `CurrentPresetIndex` at startup.

---

## Module 2: Spawn scaling / more ores

| Class (namespace) | Member | Kind | RVA / offset | Verified behaviour | Hook-safe? |
|---|---|---|---|---|---|
| ResourceNodeDirector (MP.SR.World.ResourceNode) : SRBehaviour | `get_AllResourceDirectors()` static, 0 args | method | 0x66BF60 | dump. Static `List<ResourceNodeDirector>` (static field offset 0x0 of the class statics). Directors register in `Awake` 0x66B350 and unregister in `OnDestroy` 0x66B480. **This is the runtime enumeration path for configs:** each director's `_config`. | Call (unique) |
| ResourceNodeDirector | `Awake()` 0 args | method | 0x66B350 | dump (not decompiled) | Yes |
| ResourceNodeDirector | `Update()` 0 args | method | 0x66BC30 | dump. Per frame; when `TimeDirector` world time ≥ `_nextNodeUpdateTime`(+0x30) it calls `SpawnUpToMaxNodes` and reschedules by `_config._updateNodesIntervalHours` (inferred). | Yes (per frame per director) |
| ResourceNodeDirector | `SpawnUpToMaxNodes()` 0 args | method | 0x66BA30 | dump. Lambda `<SpawnUpToMaxNodes>b__15_0(ResourceNodeSpawner)` 0x66FBA0 counts spawners with an attached node, and the method loops `SpawnNode()` up to `_config._maxActiveNodes` (inferred). | Yes |
| ResourceNodeDirector | `SpawnNode()` 0 args | method | 0x66B6F0 | dump. Weighted pick over `_resourceNodeWeightMap` (lambda 0x5403C0 is a *shared* weight getter), then a free spawner (lambda 0x66FB70), then `spawner.SpawnNode(def)`. | Yes |
| ResourceNodeDirector | `_config` 0x28, `_nextNodeUpdateTime` double 0x30, `_nodeSpawners` List 0x38, `_spawnersByNodeType` 0x40, `_timeDirector` 0x48 | field | - | dump (il2cpp-types.h agrees) | - |
| ResourceNodeDirectorConfig (MP.SR.World.ResourceNode) : ScriptableObject | `_resourceNodeWeightMap` WeightedEntry[] 0x18 (entry: `NodeDefinition` 0x10, `Weight` float 0x18), `_maxActiveNodes` int 0x20, `_updateNodesIntervalHours` float 0x24 | field | - | dump (il2cpp-types.h agrees). Getters are shared (0x5732F0, 0x4EE590). | **Patch data** |
| ResourceSpawnerDefinition (global ns), abstract ScriptableObject | `GetResourcesToSpawn(bool forceMaxCount)` 1 arg | method | 0x53C5C0 | dump. Rolls a count in [`_minResources`, `_maxResources`] (or the max if `forceMaxCount`), and picks ids by weight through `GetRandomResourceId` 0x53C450 (lambdas `b__10_0/1` share 0x53C820). | Yes (unique; shared by node **and** grower definitions) |
| ResourceSpawnerDefinition | `_nameKey` 0x18, `_resources` WeightedResourceEntry[] 0x20 (entry: `ResourceIdentifiableType` 0x10, `Weight` 0x18, **`MinimumAmount` int 0x1C**), `_minResources` int 0x28, `_maxResources` int 0x2C, `_persistenceId` 0x30 | field | - | dump | **Patch data** |
| ResourceNodeDefinition (global ns) : ResourceSpawnerDefinition | `_nodePrefabs` 0x38, `_minLifetimeHours` float 0x40, `_maxLifetimeHours` float 0x44; `GetRandomLifetimeHours` 0x539D90 | field/method | - | dump. **Despawn timer**: lifetime in game hours → `ResourceNodeSpawnerModel.despawnAtWorldTime`. | Patch data |
| ResourceNodeSpawner (global ns) : IdHandler | `SpawnNode(ResourceNodeDefinition)` 1 arg | method | 0x53A7C0 | dump. Creates the node (`CreateResourceNode` 0x53A080). `DespawnNode()` 0x53A280. Fields: `_resourceNodeDefinitions` 0x30, `_model` 0x38, `_attachedResourceNode` 0x40. | Yes |
| ResourceNode (global ns) | `SpawnResources()` 0 args | method | 0x53B990 | dump. Starts `SpawnResourcesAsync` (MoveNext 0x53FC80), which spawns each id in `_model.resourcesToSpawn` with `_resourceSpawnDelaySeconds`(+0x6C) spacing and `_resourceSpawnRadius`(+0x68). `SpawnSingleResource` 0x53BA90. | Yes |
| ResourceNode | `_timeToHarvest` 0x3C, `_resourceSpawnRadius` 0x68, `_resourceSpawnDelaySeconds` 0x6C, `_harvestAtTime` double 0xA8, `_model` ResourceNodeSpawnerModel 0xE0 | field | - | dump | - |
| ResourceNodeSpawnerModel (MP.SR.DataModel) : PersistedDataModel\<…, ResourceNodeV01\> | `nodeId` 0x20, `nodeState` 0x28, `despawnAtWorldTime` double 0x30, `resourceNodeDefinition` 0x38, `resourceNodeVariantIndex` 0x40, **`resourcesToSpawn` List\<IdentifiableType\> 0x48**, `resourceNodeDefinitions` 0x50 | field | - | dump. `Pull` 0xACDE60 / `Push` 0xACDF20 persist it into the save (`ResourceNodeV01`). | - |

**Findings for Module 2** (from dump structure; bodies not decompiled):

- Data patching is sufficient for "more ores".
- Nodes persist in saves: state, despawn time and the already-rolled `resourcesToSpawn` list are in `ResourceNodeSpawnerModel`. So a yield multiplier applied to `_minResources`/`_maxResources` affects **only nodes spawned after the patch**. Existing saved nodes keep their stored yield, and nothing has to be un-patched in the save.
- Raising `_maxActiveNodes` is bounded by the number of `ResourceNodeSpawner` points per director (`_nodeSpawners`). Extra active nodes beyond spawner count are impossible without adding spawners.
- Enumerating configs: iterate `ResourceNodeDirector.AllResourceDirectors` and read `_config`. Configs can be shared between directors, so de-duplicate by pointer and remember the original values. Alternatively use `Resources.FindObjectsOfTypeAll(Il2CppType ResourceNodeDirectorConfig / ResourceNodeDefinition)` for all loaded assets.
- Directors live in zone scenes, so re-apply on scene load (hooking `ResourceNodeDirector.Awake` is the clean trigger).

### Recommended hook points: Spawn scaling

1. **Post-hook `ResourceNodeDirector.Awake` (0 args).** Patch `this._config` once per distinct config: `_maxActiveNodes *= k` (clamped to the spawner count) and `_updateNodesIntervalHours /= k`. Patch every `ResourceNodeDefinition` in `_resourceNodeWeightMap[i].NodeDefinition`: `_minResources/_maxResources *= m`. Unique body, called once per director.
2. Simplest yield-only alternative: post-hook `ResourceSpawnerDefinition.GetResourcesToSpawn(bool)` (1 arg, unique) and append extra entries to the returned list when `this` is a `ResourceNodeDefinition`. Check the class so garden and wild growers that share the method are not scaled. The result is persisted into the model, so it is applied exactly once per node.

---

## Module 3: Movement (HL2/Source movement, uncapped bhop)

| Class (namespace) | Member | Kind | RVA / offset | Verified behaviour | Hook-safe? |
|---|---|---|---|---|---|
| KFCCharacterController (MonomiPark.KFC) : MonoBehaviour, ICharacterController | `UpdateVelocity(ref Vector3 currentVelocity, float deltaTime)` 2 args | method | 0x5BEF50 | dump (IDA name applied). The KCC `ICharacterController` callback, called from `KinematicCharacterMotor.UpdatePhase2` each physics tick. It dispatches to the `Handle*` methods below, active abilities (`UpdateVelocityForActiveAbilities` 0x5BEB70) and external forces (0x5BECC0). | Yes (unique, per FixedUpdate). **Primary hook.** |
| KFCCharacterController | `HandleStablyGroundedVelocity(ref Vector3, float)` 2 args | method | 0x5BCCF0 | dump. Ground accel toward `MovementVector * _maxGroundedMoveSpeed` with `_stableMovementSharpness`. | Yes (virtual, unique) |
| KFCCharacterController | `HandleStablyGroundedDrag(ref Vector3, float)` 2 args | method | 0x5BCCA0 | dump. Ground friction (`_groundedDrag`). 0x50 bytes. | Yes (small but unique) |
| KFCCharacterController | `HandleHorizontalAirborneMovement(ref Vector3, float, Vector3 movementVector)` 3 args | method | 0x5BC840 | dump. Air accel (`_airAccelerationSpeed`, `_airMovementSharpness`, `_maxAirMoveSpeed`). | Yes (unique) |
| KFCCharacterController | `ClampAirborneHorizontalVelocity(ref Vector3, float)` 2 args | method | 0x5BBD40 | dump. Air speed cap (uses `_airDecelerationSharpness`). **Must be neutered for uncapped bhop.** | Yes (unique) |
| KFCCharacterController | `HandleAirborneDrag(ref Vector3, float, Vector3)` 3 args / `HandleGravity(ref Vector3, float)` 2 args | method | 0x5BC310 / 0x5BC3D0 | dump | Yes |
| KFCCharacterController | `UpdateMovementAndLookVectors(Vector2 inputVector, Quaternion cameraRotation)` 2 args | method | 0x5BE720 | dump. Converts the stick/WASD vector + camera rotation into `MovementVector` (camera-planar wish direction, +0x94) and `LookVector` (+0x88), and stores `InputVector` (+0xB0). | Yes |
| KFCCharacterController | `get_IsGrounded()` 0 args | method | 0x5BF6D0 | dump (unique 0x40-byte body; reads motor grounding) | Call |
| KFCCharacterController | `_parameters` 0x20, `_motor` KinematicCharacterMotor 0x48, `AbilityBehaviors` IAbilityBehavior[] 0x50, `MaxHeight` 0x58, `LookVector` Vector3 0x88, **`MovementVector` Vector3 0x94**, `_gravityMagnitude` Nullable\<float\> 0xA0, `GravityAmount` 0xA8, `BypassGravity` 0xAC, `InputVector` Vector2 0xB0 | field | - | dump (both dumps agree) | - |
| CharacterControllerParameters (MonomiPark.KFC) : ScriptableObject | `_maxGroundedMoveSpeed` 0x18, `_stableMovementSharpness` 0x1C, `_orientationSharpness` 0x20, `_groundedDrag` 0x24, `_maxAirMoveSpeed` 0x28, `_airAccelerationSpeed` 0x2C, `_airMovementSharpness` 0x30, `_airDecelerationSharpness` 0x34, `_airborneDrag` 0x38, `_gravityMagnitude` 0x3C, `_maxExternalForceFrames` int 0x40, `_gravityAccumulationSharpness` 0x44, `_defaultMovementAndLookType` 0x48, `_abilityData` AbilityData[] 0x50 | field | - | dump. All getters shared. | Patch data |
| SRCharacterController (MP.SR.Player.CharacterController) : KFCCharacterController | `HandleGroundOnAirborneMovement(ref Vector3, ref Vector3)` 2 args | method | 0xA7E9F0 | dump (override; base 0x5BC500) | Yes |
| SRCharacterController | `_energyResource` 0xE8, `_energyShouldRegen` 0x108, **`_currentEnergy` UnitResource 0x110**, `_propertyTracker` 0x118, `_characterInput` ICharacterInput 0x148; `get_IsSprinting` 0xA80160, `get_CurrentEnergy` 0xA80050, `StartEnergyRegen` 0xA7F090 / `StopEnergyRegen` 0xA7F230 / `TryStartEnergyRegen` 0xA7F270 | field/method | - | dump | Yes |
| UnitResource (MonomiPark.UnitPropertySystem) | `_unitResourceValue` float 0x10, `OnValueChanged` 0x18; `set_Value(float)` 0x5CAB50 (unique); `get_Value` 0x516B10 (shared) | field/method | - | dump. **Energy read and write** go through `_currentEnergy`. Use `set_Value` so the HUD updates. | Call `set_Value` |
| KinematicCharacterMotor (KinematicCharacterController) | `MaxStableSlopeAngle` 0x44, `StepHandling` 0x50, **`GroundingStatus` CharacterGroundingReport 0x98** (`FoundAnyGround` +0, **`IsStableOnGround` +1 ⇒ motor+0x99**, `SnappingPrevented` +2, `GroundNormal` +4), `LastGroundingStatus` 0xD8, `_attachedRigidbodyVelocity` 0x1A4, `CharacterController` 0x1C0, `BaseVelocity` 0x1F8, `_mustUnground` 0x269 | field | - | dump (types header + DiffableCs agree) | - |
| KinematicCharacterMotor | `UpdatePhase1(float)` 0x59D490, `UpdatePhase2(float)` 0x59EA60, `ProbeGround(…)` 0x59BD20, **`ForceUnground(float time=0.1)` 0x598FD0**, `MustUnground()` 0x59BC50, `get_Velocity` 0x5A0100 | method | - | dump (stock KCC). Grounding is resolved in Phase1 before `UpdateVelocity`. | Don't hook Phase1/2; call `ForceUnground` when jumping |
| SRCharacterInput (MP.SR.Player.CharacterController) : MonoBehaviour | `PollInput()` 0 args | method | 0xA80FF0 | dump. Polls the `_moveHorizontal`(+0x20) / `_moveVertical`(+0x28) / `_lookHorizontal`(+0x30) / `_lookVertical`(+0x38) InputEvents (via `InputEvent.PollValue` Func at +0x30), applies deadzone/sensitivity processors, and writes `_movementVector` Vector2 (+0x60) and `_lookVector` Vector2 (+0x68). Gated by `_inputEnabled` (+0x98). **Jump and sprint are not read here.** They come from ability InputEvents (below). | Yes |
| SRCharacterInput | `get_MovementInput()` 0xA812E0 / `get_LookInput()` 0xA81270 | method | - | dump | Call |
| AbilityData (MonomiPark.KFC) : ScriptableObject | `_startInputEvent` InputEvent 0x18, `_stopInputEvent` 0x20, `_useAlternativeInput` ScriptedBool 0x28, `_alternativeStartInputEvent` 0x30, `_alternativeStopInputEvent` 0x38, `_movementAndLookTypeOverride` 0x40, `_isConcurrent` 0x48 | field | - | dump | - |
| AbilityBehavior\<T\> (MonomiPark.KFC), generic | `PerformStartInputManually()` 0 args 0x1EC9B50, `RegisterInputEvents` 0x1EC9B90, `StartInputEventPerformed` (virtual) | method | shared generic | dump. Ability start is driven by the InputEvent `Performed` callback, i.e. **edge-triggered**. | **Shared: call only, never hook** |
| JumpAbilityBehavior (MP.SR.Player.CharacterController.Abilities) : SRAbilityBehavior\<JumpAbilityData\> | `StartInputEventPerformed(InputEventData)` 1 arg | method | 0xA78000 | dump. Override; sets `_jumpRequested` (+0x8C) on a jump-press edge. | Yes (unique) |
| JumpAbilityBehavior | `CanStart()` 0 args | method | 0xA77AF0 | dump. Gate: `_jumpRequested`, grounded or within `_coyoteTime`, ceiling clearance, and time since `_landingTime` ≥ `_recurrenceDelay` (inferred from fields). | Yes (unique) |
| JumpAbilityBehavior | `UpdateVelocity(ref Vector3, float)` 2 args | method | 0xA782E0 | dump. Applies `_jumpUpSpeed` and side/back multipliers, hold-velocity damping. | Yes (unique) |
| JumpAbilityBehavior | `OnGroundedStateChanged(GroundedStateChangeEventInfo)` 0xA77E90, `Start` 0xA78070, `Stop` 0xA78290, `ShouldStop` 0xA77FB0 | method | - | dump. Landing records `_landingTime`. | Yes |
| JumpAbilityBehavior | `_landingTime` Nullable\<double\> 0x68, `_inAirTime` Nullable\<double\> 0x78, `_holdForce` 0x88, **`_jumpRequested` bool 0x8C**; base `AbilityData` at 0x10 (generic base), `SRCharacterController` after the AbilityBehavior fields | field | - | dump | - |
| JumpAbilityData : SRAbilityData | `_jumpUpSpeed` 0x60, `_sidewaysSpeedMultiplier` 0x64, `_backwardsSpeedMultiplier` 0x68, `_airborneForceDamping` 0x6C, `_minCeilingJumpHeight` 0x70, `_coyoteTime` 0x74, **`_recurrenceDelay` 0x78**, `_initialHoldVelocity` 0x7C, `_holdVelocityDamping` 0x80, `_verticalVelocityStopThreshold` 0x84, `_jumpStartCue` 0x88, `_jumpEvent` 0x90 | field | - | dump. The jump InputEvent is `AbilityData._startInputEvent` (+0x18) of this asset. | Patch data |
| SprintAbilityBehavior : SRAbilityBehavior\<SprintAbilityData\> | `Update(float)` 0xA84A90, `CanStart` 0xA844A0, `ShouldStop` 0xA84890, `Start` 0xA849B0, `CanApplyMultiplier` 0xA84480, `get_EnergyDecreasePerSecond` 0xA84FA0, `get_IsGroundedAndSprinting` 0xA85150 | method | - | dump. **Energy drain lives in `Update`**: after `EnergyDecreaseStartDelay` it subtracts `EnergyDecreasePerSecond * dt * efficiency` from `SRCharacterController._currentEnergy` (inferred from property names). | Yes (unique) |
| SprintAbilityBehavior / SprintAbilityData | Behavior: `_energyDecreaseStartTime` double 0x68, `_freeEnergyActiveLastFrame` 0x70, `_player` PlayerModel 0x78, `_fullStopTimer` 0x80. Data: `_sprintMultiplier` 0x60, `_minSprintValue` 0x64, `_maxSprintValue` 0x68, `_fullStopTimer` 0x6C, `_energyDecreaseStartDelayDefinition` 0x70, `_energyDecreasePerSecondDefinition` 0x78, `_sprintEfficiency` 0x80 | field | - | dump | - |
| JetpackAbilityBehavior : SRAbilityBehavior\<JetpackAbilityData\> | `UpdateVelocity(ref Vector3, float)` 0xA76F00, `Update(float)` 0xA77010, `CanStart` 0xA75860, `Start` 0xA76260, `OnJumpStart(object, EventArgs)` 0xA75FC0, `OnJumpStop` 0xA76050, `StartInputEventCanceled` 0xA76180 | method | - | dump. Listens to the jump ability's start and stop events (`_firstJumpInput` +0xD0). Engages on held jump after `_jumpToJetpackPause`. | Leave vanilla |

### Recommended hook points: Movement

1. **Hook `KFCCharacterController.UpdateVelocity(ref Vector3, float)` (2 args, unique, once per physics tick)** for the player instance only (`this` class == `SRCharacterController`).
   - Call the original. Then, when the jetpack ability is **not** active, replace the horizontal (XZ) components of `*currentVelocity` with the Source `PM_Friction` + `PM_Accelerate`/`PM_AirAccelerate` result.
   - Inputs:
     - wishdir from `MovementVector` (+0x94; already camera-relative and planar).
     - grounded from `_motor`(+0x48) → `GroundingStatus.IsStableOnGround` (+0x99).
     - dt from the argument.
   - Keep Y from the original (gravity and jump), so KCC collision, steps and slopes stay intact because the motor still integrates the velocity.
   - Finer-grained alternative: replace `HandleStablyGroundedVelocity` + `HandleStablyGroundedDrag` (ground) and `HandleHorizontalAirborneMovement` + `ClampAirborneHorizontalVelocity` (air) individually. All four are unique virtual bodies.
2. **Uncapped bhop:** neuter `ClampAirborneHorizontalVelocity` (return immediately) or skip it inside the UpdateVelocity replacement. Set `JumpAbilityData._recurrenceDelay` (+0x78) = 0. Do not apply ground friction on the landing tick when a jump is queued.
3. **Sprint → energy:** do not port the stock sprint. Gate HL2 sprint on the sprint ability's InputEvent (`SprintAbilityData` → `AbilityData._startInputEvent`, poll `InputEvent.PollValue`). Drain with `UnitResource.set_Value` on `SRCharacterController._currentEnergy` (+0x110). Call `StopEnergyRegen`/`StartEnergyRegen`, or leave `SprintAbilityBehavior.Update` doing it and only change speed.
4. **Read-outs:**
   - Yaw: `LookVector` (+0x88) or the camera transform.
   - Move vector: `SRCharacterInput._movementVector` (+0x60).
   - Grounded: motor+0x99.
   - Energy: `_currentEnergy._unitResourceValue` (UnitResource+0x10).

---

## Module 3b: Scroll-jump / autobhop

The game uses **Unity's new Input System only**, through its own ScriptableObject event bus. There is no `Input.GetAxis("Mouse ScrollWheel")` path:

- An `InputActionController` asset (MP.SR.Input) holds `InputEventBinding[] _inputBindings` (+0x18).
- Each `InputEventBinding` maps an `InputActionReference _input` (+0x10) to `InputEvent[] _inputEvents` (+0x18). Its runtime action is `ActionInstance` (+0x38).
- `InputEventBinding.PerformAction(CallbackContext)` 0x9DF160 builds an `InputEventData` (`Source` POINTER/KEYBOARD/GAMEPAD and the value) and fires each `InputEvent.Performed`.
- Gameplay code subscribes to `InputEvent` assets, never to InputActions directly. Action names and the `<Mouse>/scroll` binding live in the InputActionAsset data, not in code, so read them at runtime from `binding.ActionInstance.bindings`.

| Class (namespace) | Member | Kind | RVA / offset | Verified behaviour | Hook-safe? |
|---|---|---|---|---|---|
| VacuumItem (MP.SR.Player.PlayerItems) : SRBehaviour | `NextSlot(InputEventData)` 1 arg | method | 0xA67D00 (0x63 bytes) | dump. Handler for `_nextSlot` InputEvent (+0x188). Cycles the ammo slot forward. Subscribed in `OnEnable` 0xA68520, removed in `OnDisable` 0xA67D70. | **Yes (unique).** Scroll-slot suppression point |
| VacuumItem | `PreviousSlot(InputEventData)` 1 arg | method | 0xA68F80 (0x63 bytes) | dump. Handler for `_previousSlot` InputEvent (+0x180). | **Yes (unique)** |
| VacuumItem | `SelectSlot(InputEventData)` 0xA69640 / `SelectSlot(int)` 0xA69790 | method | - | dump. Number-key slot select (`_selectSlot` +0x178). | Yes |
| VacuumItem | `_shoot` 0x168, `_vac` 0x170, `_selectSlot` 0x178, **`_previousSlot` 0x180**, **`_nextSlot` 0x188**, `_burst` 0x190 (all `InputEvent`) | field | - | dump | - |
| InputEvent (MP.SR.Input) : ScriptableObjectWithRuntimeData | `Started` 0x18, `Performed` 0x20, `Canceled` 0x28 (Action\<InputEventData\>), **`PollValue` Func\<float\> 0x30**, `PollVectorValue` Func\<Vector2\> 0x38, `GetActiveValueType` 0x40 | field | - | dump | - |
| InputEvent | `Perform()` 0 args 0x9DFA50, `Perform(InputEventData)` 0x9DFA10, `Perform(Vector2)` 0x9DFAB0, `StartInput(InputEventData)` 0x9DFDE0, `Cancel()` 0x9DF940, `add_Performed` 0x9E0240 / `remove_Performed` 0x9E0450 | method | - | dump. `Perform*` invokes `Performed` subscribers. | Call. Don't hook (all inputs pass through) |
| InputEventData (MP.SR.Input), struct 12 bytes | `Source` InputEventSource int 0x0 (0 UNKNOWN, **1 POINTER**, 2 KEYBOARD, 3 GAMEPAD), `_vecValue` Vector2 0x4 (`Value` = x) | field | - | dump. **x64 ABI: passed by hidden pointer** (12-byte struct), so the hook's 2nd argument is `InputEventData*`. | - |
| InputEventBinding (MP.SR.Input) | `PerformAction(InputAction.CallbackContext)` 0x9DF160, `StartAction` 0x9DF220, `CancelAction` 0x9DEDE0, `MakeInputEventData` 0x9DEEF0, `BindInput` 0x9DEB00 / `UnbindInput` 0x9DF440 | method | - | dump | Yes, but hot (every input) |
| InputActionController (MP.SR.Input) : ScriptableObject | `GetInputActionFromEvent(InputEvent)` 1 arg 0x9D8D60 (IEnumerable) / 2-arg list overload 0x9D8F30, `TryFindEvent(string, out InputEvent)` 0x9D9040, `BindControls` 0x9D8A30 | method | - | dump. Maps an InputEvent back to its Unity `InputAction`(s). | Call |
| JumpAbilityBehavior | `StartInputEventPerformed(InputEventData)` 0xA78000, `CanStart()` 0xA77AF0, `_jumpRequested` 0x8C, `_landingTime` 0x68; data `_recurrenceDelay` 0x78, `_coyoteTime` 0x74 | - | - | dump. **Jump is press-edge** (InputEvent `Performed` → `_jumpRequested`), gated in `CanStart` by grounded/coyote and `_recurrenceDelay` since landing. Holding the button does not re-jump. | Yes |
| AbilityBehavior\<T\> | `PerformStartInputManually()` 0x1EC9B50 | method | shared generic | dump. Triggers the ability's start-input path without a real input event. | Call only |

### Recommended hook points: Scroll-jump / autobhop

1. **Scroll suppression + scroll-jump: hook `VacuumItem.NextSlot` and `VacuumItem.PreviousSlot` (1 arg each, unique).** When scroll-jump is enabled and `data->Source == POINTER (1)`, skip the original so the slot does not change, and request a jump instead. Keyboard and gamepad slot cycling still work because their Source differs.
   - Coarser alternative: find the scroll binding with `InputActionController.GetInputActionFromEvent(vacuumItem._nextSlot)` and call `InputAction.Disable()`, or apply an empty binding override on the `<Mouse>/scroll` binding. This also kills any non-mouse binding on the same action, so the hook is preferred.
2. **Requesting a jump from code:** set `JumpAbilityBehavior._jumpRequested` (+0x8C) = true. Find the behaviour by iterating `KFCCharacterController.AbilityBehaviors` (+0x50) and comparing the class to `JumpAbilityBehavior`.
   - Alternatively call `PerformStartInputManually()` on that behaviour.
   - Avoid `jumpInputEvent.Perform()`. It also reaches the jetpack's start-input listeners and could arm the jetpack.
3. **Autobhop (hold to re-jump):** each tick (inside the `KFCCharacterController.UpdateVelocity` hook from Module 3, or a pre-hook of `JumpAbilityBehavior.CanStart`), if `jumpInputEvent.PollValue() > 0.5`, the motor is stable on ground (+0x99) and the jump ability is not active, set `_jumpRequested = true`. Also set `JumpAbilityData._recurrenceDelay` = 0 so landing does not block the re-jump.
   - The jump InputEvent is `JumpAbilityData._startInputEvent` (+0x18). Get it from `CharacterControllerParameters._abilityData` (+0x50) → the `JumpAbilityData` entry.
   - Keep the jetpack vanilla: it engages on a held jump after `_jumpToJetpackPause` while airborne. With autobhop, holding jump keeps re-jumping on landing, and the jetpack still kicks in mid-air after the pause. To prevent accidental jetpack use during bhop, only auto-rejump within N ms of landing, or require a fresh press for the jetpack.

---

## Module 4: Minimap

| Class (namespace) | Member | Kind | RVA / offset | Verified behaviour | Hook-safe? |
|---|---|---|---|---|---|
| Map (MP.SR.UI.Map) : SRBehaviour | `GetMapPosition(Vector3 worldPos)` 1 arg | method | 0x715C30 | dump. Projects world XZ to the map RectTransform's local position through `_projectionHelper`. | Yes, but you can reproduce it natively |
| Map | `GetMapPosition(Vector3, RectTransform)` 0x715E70, `GetWorldArea()` 0x715F60, `GetMapArea()` 0x715AF0, `GetFullMapArea()` 0x715A60, `SetupInitialValues()` 0x715FA0 (builds `_projectionHelper` from the anchors), `Awake` 0x715790 | method | - | dump | Yes |
| Map | `_anchor1` MapAnchorCoordinates 0x28, `_anchor2` 0x38 (struct: `WorldPosition` Vector2 +0x0, `MapPosition` Vector2 +0x8), `_markerPrefabMapping` 0x48, `_rectTransform` 0x70, `_projectionHelper` 0x78 | field | - | dump (both dumps agree) | - |
| MapProjectionHelper (MP.SR.UI.Map) | ctor `(Vector2 minWorld, Vector2 maxWorld, Vector2 minMap, Vector2 maxMap)` 0x70F7F0; `GetMapPosition(Vector3, RectTransform)` 0x70F740 | method | - | dump. Linear: `map = worldXZ * _worldToMapScale + _worldToMapOffset`. | Yes |
| MapProjectionHelper | `_worldToMapScale` Vector2 0x10, `_worldToMapOffset` Vector2 0x18 | field | - | dump | - |
| MapDirector (MP.SR.Map) : ScriptableObject | `GetMapForZone(ZoneDefinition)` 1 arg | method | 0x9C1A30 | dump. `ZoneMappings.FirstOrDefault(m => m.Zone == zone)?.Map ?? DefaultMap` (lambda 0x9D20E0; the fallback is inferred). | Yes |
| MapDirector | `DefaultMap` 0x20, `ZoneMappings` List\<ZoneMapping{Zone 0x10, Map 0x18}\> 0x28, `_mapList` MapListConfiguration 0x70, `_eventDirector` 0x78, `UIRoot` MapUIRoot 0x98, `Markers` Dictionary 0xA0, `LastZoom` 0xB0 | field | - | dump | - |
| MapDefinition (MP.SR.Map) : ScriptableObject | `MapIcon` 0x18, `MapName` 0x20, `MapOutOfBoundsIcon` 0x28, `MapOutOfBoundsLabel` 0x30, `MapAvailableCondition` 0x38, **`MapBackground` ImageSetup 0x40** (ImageSetup: `TintColor` Color +0x0, **`Image` Sprite +0x10 ⇒ def+0x50**, `Material` +0x18), `MapBackgroundOverlay` 0x60 (Sprite at def+0x70), `RelatedScenes` 0x80 | field | - | dump | - |
| MapListConfiguration (MP.SR.Map) | `_maps` MapDefinition[] 0x18; `GetAvailableMaps(IEventDirector)` 0x9C3110 | field/method | - | dump. List of all map pages (tabs). | - |
| MapUI (MP.SR.UI.Map) | `UpdatePlayerMarker(PlayerDisplayOnMap, PlayerMapMarker)` 0x715360, `OpenMap(MapDefinition)` 0x7127F0, `Update` 0x7155C0, `IsPlayerInMap` 0x711DD0 | method | - | dump (from candidates; not re-decompiled) | Yes |
| MapUIFogReveal (MP.SR.UI.Map) | `RevealKnownZones()` 0x729190, `RevealZonesInMap(Map)` 0x729650, `Start` 0x7297C0; fields `_eventDirector` 0x20, `_fogClearCue` 0x28, `_mapDirector` 0x38, `_fogElements` 0x40, `_animatingElements` 0x48, `_animatedEvents` 0x50, `_hasStarted` 0x58, `_pendingMap` 0x60 | method/field | - | dump | Yes |
| FogMapElement (MP.SR.UI.Map) | `ShouldBeRevealed(EventDirector)` 1 arg 0x704B50, `SetRevealState(bool)` 0x704B20; `_fogRevealEvent` StaticGameEvent 0x20, `_revealTime` 0x28, `_revealCurve` 0x2C, `_canvasGroup` 0x30 | method/field | - | dump. Revealed means EventDirector has recorded `_fogRevealEvent`. | Call |
| PlayerZoneTracker (global ns) : MonoBehaviour | `GetCurrentZone()` 0 args 0x98AA80 | method | - | dump. First of `_currentZones` (+0x38). `GetLatestZone` 0x4F9D90 is a **shared getter** that returns `_lastNotNullZone` (+0x40). Read the field instead. | Call |
| PlayerZoneTracker | `_member` 0x20, `_unlockedEvent` 0x28, `_lastZones` 0x30, `_currentZones` LinkedList 0x38, `_lastNotNullZone` 0x40, `PlayerOnZoneChanged` 0x48, `PlayerOnZoneEntered` 0x50, `PlayerOnZoneExited` 0x58; `add_PlayerOnZoneChanged` 0x98B120 | field/method | - | dump | - |
| MapUIUtilities (MP.SR.UI.Map), static | `GetPlayerCurrentZone()` 0 args 0x729E40 | method | - | dump | Call |
| LabyrinthZoneMarker / MapElevationDisplay (MP.SR.UI.Map) | `MapElevationDisplay.SetElevationDifference(float)` 0x7075D0; sprites `_belowElevationIcon` 0x28, `_sameElevationIcon` 0x30, `_aboveElevationIcon` 0x38 | method/field | - | dump | - |

**Grey Labyrinth:** no per-floor map type exists in code. There is a single `LabyrinthZoneMarker` and a `MapElevationDisplay` (above/same/below icon) used for vertical separation. That suggests one `MapDefinition` per labyrinth zone, with markers showing elevation instead of separate floor maps. The number of labyrinth `MapDefinition`s is asset data and must be confirmed at runtime by dumping `MapListConfiguration._maps` and `MapDirector.ZoneMappings`.

### Recommended hook points: Minimap

No hook is needed for the projection:

1. Get the zone: `PlayerZoneTracker._lastNotNullZone` (+0x40), or call `MapUIUtilities.GetPlayerCurrentZone()`.
2. Get the map: `MapDefinition def = MapDirector.GetMapForZone(zone)`. The MapDirector comes from `SceneContext` or `Resources.FindObjectsOfTypeAll`.
3. Get the background sprite: `def+0x50`.
4. The projection lives on the map page prefab (`Map` component), not on `MapDefinition`. Get one of two ways:
   - Instantiate or keep the page prefab via `MapUI._mapPrefabMapping` (+0x140), let `Map.Awake`/`SetupInitialValues` build `_projectionHelper`, then read `_worldToMapScale`/`_worldToMapOffset`.
   - Or read `_anchor1`/`_anchor2` and compute `scale = (mapB-mapA)/(worldB-worldA)` and `offset = mapA - worldA*scale` yourself.

   Cache the result per MapDefinition.
5. Fog: for each `FogMapElement` on the page, call `ShouldBeRevealed(eventDirector)`.
6. Optional: post-hook `MapUI.UpdatePlayerMarker` to copy the game's own arrow position and rotation while the full map is open.

---

## Module 5: Version guard

| Class (namespace) | Member | Kind | RVA | Verified behaviour | Hook-safe? |
|---|---|---|---|---|---|
| Application (UnityEngine), UnityEngine.CoreModule | `get_version()` static, 0 args | method | 0x339D370 | IDA name applied (`UnityEngine.Application.get_version`). icall wrapper returning the bundle version ("1.3.0" per globalgamemanagers). | Call via `il2cpp_class_get_method_from_name(Application,"get_version",0)` + `il2cpp_runtime_invoke`, or `il2cpp_resolve_icall("UnityEngine.Application::get_version")` |
| Application | `get_buildGUID()` static, 0 args | method | 0x339C700 | dump + IDA name. Per-build GUID, the strongest identity check. | Call |
| Application | `get_unityVersion()` static, 0 args | method | 0x339D250 | dump. "6000.0.73f1" | Call |

### Recommended guard

1. SHA-256 (or PE TimeDateStamp + SizeOfImage) of the loaded `GameAssembly.dll` against the hash above, before any RVA cross-check.
2. `Application.version == "1.3.0"`.
3. Log `buildGUID` on first run and pin it.

Name-based resolution makes the mod survive minor patches. Fail closed on any missing class, method or field.

---

## UI: Slimepedia (overlay logo)

Used by `ui/slime_icons.cpp` (the overlay sidebar logo). Read-only: no hooks, only field reads and calls on the main thread.

| Class (namespace) | Member | Kind | RVA / offset | Verified behaviour | Hook-safe? |
|---|---|---|---|---|---|
| SceneContext (global) | `<PediaDirector>k__BackingField` | field | +0x38 | dump. The scene's PediaDirector. `SceneContext` comes from `SRSingleton<SceneContext>.get_Instance()` (parent class, shared generic body 0x4DC510). | Read |
| SceneContext | `<GameModel>k__BackingField` | field | +0x98 | **IDA** (`SceneContextUtility.TryGetGameModel` 0x693A80 reads +0x98 and tests it with `Object.op_Inequality`). Null means no save is loaded. | Read |
| PediaDirector (global) | `_pediaConfiguration` | field | +0x28 | dump. `PediaConfiguration` asset. | Read |
| PediaDirector | `_pediaModel` | field | +0x80 | dump. The loaded save's `PediaModel`; null before a save is loaded. | Read |
| PediaDirector | `IsUnlocked(PediaEntry)` | method | 0x52C210 | **IDA (capstone).** `return _pediaModel.unlocked.Contains(entry)`; throws NullReferenceException if `_pediaModel` or `unlocked` is null. Overloaded with `IsUnlocked(IdentifiableType)` (0x52C140), so resolve by parameter type (`MethodByParams`, `"MonomiPark.SlimeRancher.Pedia.PediaEntry"`). | Call (check the model first) |
| PediaModel (MonomiPark.SlimeRancher.DataModel) | `unlocked` | field | +0x18 | dump. `HashSet<PediaEntry>` of the save's unlocked entries; changes during play (`Unlock`). | Read |
| PediaConfiguration (MP.SR.Pedia) | `_categoryList` | field | +0x18 | dump. `Object[]`: the Slimepedia's categories (PediaCategory and others, e.g. MusicPediaCategory). | Read |
| PediaCategory (MP.SR.Pedia) | `_items` | field | +0x30 | dump. `PediaEntry[]` shown in that category (`Count` reads its length). | Read |
| PediaEntry (MP.SR.Pedia), abstract ScriptableObject | `_details` | field | +0x20 | dump. `PediaEntryDetail[]`: the entry's sections in page order (`Pages`). | Read |
| PediaEntry | `_title`, `_description` | field | +0x38, +0x40 | dump. `LocalizedString` name and intro text. | Read |
| PediaEntry | `get_Icon()` | abstract | | dump. The list icon. `IdentifiablePediaEntry.get_Icon` (0x99E3D0, **IDA (capstone)**) returns `_identifiableType`'s icon through a virtual call, null if the type is null. | Call (virtual: resolve on the object's class) |
| IdentifiablePediaEntry (MP.SR.Pedia) : PediaEntry | `_identifiableType` | field | +0x58 | dump. The `IdentifiableType` the entry describes; slime entries have a `SlimeDefinition` here. Other entry kinds: `FixedPediaEntry` (own `_icon`), `RadiantSlimePediaEntry` (`_slimeDefinition` +0x58). | Read |
| PediaEntryDetail (MP.SR.Pedia), plain class | `Section`, `Text` | field | +0x10, +0x18 | dump. `Section` is a `PediaDetailSection` asset; `Text` the section body (`LocalizedString`). `TextGamepad` +0x20 / `TextPS4` +0x28 are controller variants, `Contents` +0x30 sub-contents (input prompts). | Read |
| PediaDetailSection (MP.SR.Pedia), ScriptableObject | `_title`, `_icon` | field | +0x18, +0x20 | dump. Section heading (`LocalizedString`) and icon. | Read |
| LocalizedReference (UnityEngine.Localization) | `get_IsEmpty()` | method | 0x2DAF4A0 | dump. True when no table entry is set; checked before `LocalizedString.GetLocalizedString()` (0x2DB1FC0). | Call |
