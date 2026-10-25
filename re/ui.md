# UI: Options, list menus, HUD, popups, input, Slimepedia

`MP.SR` = `MonomiPark.SlimeRancher`. Tags: **IDA** = decompiled; **dump** = signature/offset from the dumps only. Hooks: re/hooks.md.

## Options menu (MP.SR.UI.Options / MP.SR.Options)

Data-driven: `OptionsConfiguration` (ListAsset<OptionsItemCategory>) = tabs; `OptionsItemCategory` (ListAsset<OptionsItemDefinition>) = rows; `OptionsItemDefinition._controlPrefab` = row control. Mod use: `ui/options_tab.cpp` injects the "Mods" tab.

### OptionsUIRoot (MP.SR.UI.Options) : DisplayBaseUI
- `Start()` 0x71CEF0 IDA: builds `OptionsViewModel(profileOptions, gameOptions)` into +0x230, `optionsConfiguration` (+0x238) from provider (+0xD0) `GetAssetForCurrentPlatform()`, `BindCategories(config, aboutCategory +0xD8)`, starts `LateStart`. Once per options-screen instantiation.
- `BindCategories(OptionsConfiguration, OptionsAboutCategory)` 0x71A010 IDA: `categories = this.categories.Concat(config).Append(about).Where(CheckIsActive).ToList()` -> +0x240, then `categoryAdapter.BindDataList(categories.Select(c => new CategoryViewModel(c.Title, c.Icon)))`. Entries already in `this.categories` come first; About is always last. Only caller: Start.
- `SwapCategory(int idx, bool)` 0x71D1A0 IDA: `OptionsItemCategory` (exact class check) -> `BindItemCategory`; `OptionsAboutCategory` -> `BindAboutCategory`; anything else does nothing. **An injected tab must be a real `OptionsItemCategory`.**
- `BindItemCategory(OptionsItemCategory)` 0x71A2F0 IDA: Apply/Revert buttons, items panel, rebind button from `_showRebindButton`; `optionsItemModels`(+0x208) = `optionsViewModel.GetOptionsItemsForCategory(config, cat, true).ToList()`; `optionsAdapterView`(+0x140)`.BindDataList`; `BindOptionsDetailsView(0)`.
- `CheckThenSwapCategory(int)` 0x71A710 (save/revert prompt, then SwapCategory via `<>c__DisplayClass56_0.<CheckThenSwapCategory>b__0` 0x71F9B0).
- `ApplyChanges()` 0x719A30 IDA: for each model with `HasUnsavedSelection()` (interface slot 6) -> `SaveSelectedValue()` (slot 9), exceptions -> popup; then `SaveSettings()` 0x71CBA0 (-> `AutoSaveDirector.SaveProfile()` + `InputDirector.NotifyKeyBindingsChanged()`).
- `TempApplyChanges()` 0x71D320 (slot 10 `ApplyTemporaryValue`); `MaybeApplyChanges()` 0x71B760; `CreateOptionsViewModel` 0x71AB90; `Close` 0x71A860.
- fields: `optionsConfigurationProvider` +0xD0, `categoryAdapter` (CategoryAdapterView, tab strip) +0xE0, `optionsAdapterView` +0x140, `optionsItemModels` List<IOptionsItemModel> +0x208, `currentCategoryIndex` +0x210, `optionsViewModel` +0x230, `optionsConfiguration` +0x238, `categories` List<IOptionsCategory> +0x240.
- gotcha: if a category was injected and the mod stops answering, `BindOptionsDetailsView(int)` indexes past the details list (ArgumentOutOfRange from `SwapCategory`): remove the injected category on fault.

### OptionsViewModel
- `GetOptionsItemsForCategory(OptionsConfiguration, OptionsItemCategory, bool profileSettingsFirst)` 0x71E0E0 IDA: `GameSpecificOptions(+0x18).GetOptionModelsFromDefinitionList(config.GetDefinitionsFromCategory(cat)).Where(ShouldDisplay)` + same for `ProfileSpecificOptions(+0x10)`. Models are looked up, not created (unknown definitions: unverified, `OptionsModelBase.GetOptionModelsFromDefinitionList` 0x15BBC30). Best row-injection hook: return our own models for our category.

### Definitions and models
- `OptionsConfiguration` : ListAsset — `items` +0x18; `GetDefinitionsFromCategory` 0x9C54C0.
- `OptionsItemCategory` : ListAsset<OptionsItemDefinition>, IOptionsCategory — `items` +0x18, `_title` LocalizedString +0x20, `_icon` Sprite +0x28, `_showRebindButton` +0x30, `_activeCondition` +0x38. `CheckIsActive()` 0x9C6BC0: true when `_activeCondition` null. Ctor 0x9C6C50.
- `OptionsItemDefinition` (abstract SO): `_referenceId` +0x18, `_label` +0x20, `_detailsText` +0x28, `_controlPrefab` GameObject +0x30, `_applyImmediately` +0x38, `_isProfileSetting` +0x39, `_requireConfirmation` +0x3A, `_confirmationPopupConfig` +0x40, `_showTutorialDisclaimer` +0x48, `_indent` +0x4C, `_showCondition` +0x50. `CreateInstance<T>(name, other)` 0xF73740 IDA = `Instantiate(other)` + `set_name` + `_referenceId = "setting." + name`. `ShouldDisplay` 0x9C76A0.
- `PresetOptionsItemDefinition`: `CreateOptionItemModel()` 0x9C9D20 IDA = `new PresetOptionsItemModel(this)`, added to `_optionsItemModels` (+0x60). `_wrapAround` +0x58, `SupportedInputDeviceAssets` +0x68, `SupportedPlatforms` +0x70. `SetTempPresetIndex` 0x9C9E00.
- `ToggleOptionDefinition`: `ApplyPresetSelection(int)` 0x9D0D90 IDA = `TargetBool.set_Value(index == 1)`. Fields `TargetBool` ScriptedBool +0x78, `_negativeReferenceId` +0x80, `_positiveReferenceId` +0x88, `_negativeOption` +0x90, `_positiveOption` +0x98, `_defaultValue` +0xA0. `GetOptionsPresets` 0x9D0E00, `GetDefaultPresetIndex` 0x9D0DF0.
- `ScriptedValueRangeOptionDefinition`: `ApplyPresetSelection(int)` 0x9D0030 IDA: `v = index*_valueStep + _minValue`, normalized if `_normalizeOutputValue`, `_targetValue.set_Value(v)`. A stepper, not a slider. `GetValueForIndex` 0x9D0440, `GetOptionsPresets` 0x9D0310, `GetDisplayForValue` 0x9D0130. Fields `_targetValue` ScriptedFloat +0x78, `_minValue` +0x80, `_maxValue` +0x84, `_valueStep` +0x88, `_normalizeOutputValue` +0x8C, `_defaultValue` +0x90, `_displayFormat` +0x98.
- `PresetOptionsItemModel` (MP.SR.DataModel) : OptionsItemModel — ctor 0xAC7D00 (def +0x48, `_presets` +0x50, `_tempPresetIndex` +0x58); `SelectValue(int)` 0xAC7C60 (selector left/right); `ApplySelectedValue` 0xAC7070 / `ApplyValue(int)` 0xAC70B0 (virtual `def.ApplyPresetSelection` in try/catch); `SaveSelectedValue` 0xAC7A60; `CreateOptionsControl` 0xAC73E0 = `Instantiate(_controlPrefab)` + `GetComponent<OptionsPresetSelectorBase>().BindData(this)`.
- `OptionsItemModel`: `optionsItemDefinition` +0x20, `optionValueKey` +0x28, `ValuePushed` +0x30.
- `IOptionsItemModel` slots used by OptionsUIRoot: 6 `HasUnsavedSelection`, 9 `SaveSelectedValue`, 10 `ApplyTemporaryValue`.
- `OptionsItemViewHolder` : ButtonViewHolder — `BindData(IOptionsItemModel)` 0x718690, `NextSetting` 0x718B60, `PreviousSetting` 0x718CE0; `label` +0x60, `controlHolder` +0x68, `_control` +0x88, `_stepperOptionControl` +0xA0, `_model` +0xB0.
- `OptionsPresetSelectorBase` / `OptionsPresetSelector`: `BindData(PresetOptionsItemModel)` 0x718F00, `Next` 0x719060, `Previous` 0x7194B0, `OnSelectionChanged` 0x719570 / 0x7198A0; `_previousButton` +0x60, `_nextButton` +0x68, `optionsItemModel` +0x70, `_currentSelectionText` +0x88.
- `ScriptedFloatOptionsControl.BindData` 0x71E430.
- chain (value delivery): selector -> `PresetOptionsItemModel.SelectValue` (temp) -> Apply -> `OptionsUIRoot.ApplyChanges` -> `SaveSelectedValue` -> `ApplyTemporaryValue`/`ApplyValue` -> `def.ApplyPresetSelection` -> `ScriptedBool/Float.set_Value` -> `OnValueChanged`.

### OptionsDirector (MP.SR.Options) : SRBehaviour
- `Awake()` 0x9C5570 IDA: `_optionsItemDefinitions` (+0x68) from provider; `_optionsModel` (+0x60) = `new OptionsModel(GetRuntimeOptionsListFor(defs)._profileOptions)`. The set of rows in OptionsModel is fixed at Awake, so injected rows are never saved by the game (the mod keeps its own config).
- `PullSettings(OptionsV02)` 0x9C5E30 / `PushSettings` 0x9C5E60: `_optionsModel.Pull/Push`. Persistence `OptionsV02.OptionItems` List<OptionItemDataV01> (+0x10), keyed by referenceId. `GetRuntimeOptionsListFor` 0x9C57F0; `_runtimeOptionsLists` +0x70.
- `RuntimeOptionsItemList` ctor 0x9CF970: `_profileOptions` +0x10, `_gameOptions` +0x18.

### TabbedControl (MP.SR.UI.Framework.CommonControls)
Generic tab strip used by the map UI (the options menu uses CategoryAdapterView): `GoToTab(int)` 0x7D6CD0, `UpdateTabs` 0x7D7620, `set_TabCount` 0x7D7E80; `_buttonList` +0x38, `_tabCount` +0xC8, `_selectedIndex` +0xD0.

## List menus (plot purchase menu, Fabricator, shop, ...)

- `ListUIRoot`4` (`LandPlotUIRoot : ListUIRoot<PlotPurchaseItemModel, PlotPurchaseCategory, ...>`) is **shared generic code**: methods have no name-resolvable pointer of their own; hook through an inflated base (e.g. LandPlotUIRoot's) and filter by class.
  - `CreateUI` calls virtual `UpdateMenuItemAvailabilityAndHidden` first (LandPlotUIRoot override 0x6E8150: `UpdateAvailability(Activator)` / `UpdateHidden(Activator)` per item).
  - `RebuildUI` 0x11AFB30 (shared): calls `CreateUI` only while the menu has no swapper yet (first open); with the menu open it calls `IListViewSwapper.RefreshAndAdvance` -> `ListUI.itemAdapter` refresh, which re-runs the fetch closure `ListUI.<ShowMenu>b__6` (maps `menuConfiguration.Categories[i].Items` live). **`UpdateMenuItemAvailabilityAndHidden` does not re-run after a purchase** (vanilla availability is a live Func). custom_upgrades hooks `RebuildUI`.
  - `ShowAltActionButton(item)` 0x11AFE20 = item is an `IPediaEntryProvider` with an entry; `OnAltAction(item)` 0x11AF260 = `PediaDirector.ShowPedia(entry)`. Shared; sell_upgrades hooks them through LandPlotUIRoot.
  - `OpenPopupPrompt` -> `PopupPrompt` (confirm popups; texts through LocalizeStringEvents).
- `ListUI`: `OnParentUIFocused` subscribes `Submit` / `AltSubmit` to the `submit` / `altSubmit` InputEvents; `AltSubmit` 0x83EE30 invokes `onAltSubmitActionPerformed`; `Submit` 0x83F450. `<ShowMenu>b__4` (selection) 0x10F7310: sets `onAltSubmitActionPerformed = onAltActionPerformed(item)` for every item, then `CreateDetailsMoreButton` (-> `DetailsMoreButtonView.BindData`), then `CreateDetailsAltButton(item, category, showAlt)` 0xF47ED0 (destroys `altActionButtonGameObject`; instantiates `category.altDetailsButton` into `altActionButtonContainer` when `showAlt(item)`; label from the prefab's own InputHint).
- PURCHASED text = the category's `detailsInfo` prefab (label-only twin of the Purchase button, label `item._infoButtonLabel`) instantiated by `ListUI.CreateDetailsMoreButton` into `mainActionButtonContainer` for unavailable items. `MainActionButtonContainer` has no LayoutGroup. Purchase button prefab `TextButton_NoneSelectable` is not reachable by gamepad navigation.
- `DetailsMoreButtonView.BindData` (non-generic): sell_upgrades' Sell button hook.
- `PurchaseItemDetailsContentView.BindData(vm)` 0x6D9EA0 (non-generic, unique): `_detailsText` (+0x58) <- `vm._description`; `_craftCostView` (+0x60) active only when `vm._showCost` (IsAvailable && NewbuckCost > 0). It carries its own nested `Canvas` and **no `GraphicRaycaster`**: runtime uGUI under it gets no clicks unless it adds Canvas (overrideSorting, order +1) + GraphicRaycaster. custom_upgrades hooks it (Hen Collector sliders).
- `GenericViewHolder.BindData` 0x83C900: enables `unavailableIcon` (+0x68, the shared `checkmark` sprite from the item's `_unavailableIcon`) over the list icon, tinted `unavailableColor`, while the item is unavailable (owned).
- `UniformGridView` (GridArea / GenericGridItemViewHolder): `INavigationHandler.OnNavigate` 0x7DB210 moves the selection left/right.
- `FocusManager.NavigateInDirection` hands a move to every `IMoveHandler` between the focused card and the target via `INavigationHandler.TryHandleAsNavigation` 0x79C940 (focused card's Selectable first) and checks the event's `used` flag after each call. drone_price_limit hooks `NavigateInDirection`. `NavigationEventData.MoveDirection` 0 = left, 2 = right.
- `BaseUI.OnCancelPressed` 0x683490.
- `InputHint.Start` calls `Configure(color, _configuration)` when not configured (sets `_label.StringReference`; a disabled LocalizeStringEvent does not apply it).

## Input locking, UI stack, cursor

- `UIInputLocker` (MP.SR.UI): `OnEnable` 0x6B0F00 = `TimeDirector.Pause` + `SECTR_AudioSystem.PauseNonUiSfx`; `OnDisable` 0x6B0DD0 = `TimeDirector.Unpause` (coroutine). Add one to a runtime dialog's root to lock input like a game menu (custom_upgrades silo receipt).
- `InputDirector.OnPausedChanged` -> `UpdateInputMaps(uiMode)` 0x9DBB00 (player map off, UI map on).
- `CursorLockHandler.LateUpdate` 0x68AF70 enables the cursor when the pause state changes (not on gamepad).
- `UIDirector.Push(IStackableUI, ...)` 0x6AF0C0 only hides/disables the previous UI, hides popups/tutorials, raises AddedToStack; it does not lock input, and `BaseUI.Awake` does not push itself. `UIDirector.CreateAndPush` is what `UIActivator`/`LandPlotUIActivator.OnInteract` use.
- `UIActivator.OnInteract` 0x6ACB60, `UIDisplayInteractable.OnInteract` 0x6B0AD0 (custom_upgrades hooks both for the Quantum Storage Link's Use).
- UI display scenes: `UIDisplayLoadManager.Update` 0x7A25B0 -> `UIDisplaySceneLoadHelper.HandleSceneLoaded` 0x7B1900 -> `Instantiate` 0x7B1B30 -> `UIDisplayUtility.InstantiatePrefabs` 0x7B251C. Full-screen UIs (map, shop, ...) are scenes loaded on demand: their components exist only while open.

## Input system (MP.SR.Input)

Unity's new Input System only, through a ScriptableObject event bus; gameplay subscribes to `InputEvent` assets, never to InputActions.
- `InputActionController` (SO): `_inputBindings` InputEventBinding[] +0x18; `GetInputActionFromEvent(InputEvent)` 0x9D8D60 (2-arg list overload 0x9D8F30), `TryFindEvent(string, out InputEvent)` 0x9D9040, `BindControls` 0x9D8A30.
- `InputEventBinding`: `_input` InputActionReference +0x10, `_inputEvents` InputEvent[] +0x18, `ActionInstance` +0x38. `PerformAction(CallbackContext)` 0x9DF160 (builds InputEventData, fires each event's Performed; hot), `StartAction` 0x9DF220, `CancelAction` 0x9DEDE0, `MakeInputEventData` 0x9DEEF0, `BindInput` 0x9DEB00, `UnbindInput` 0x9DF440.
- `InputEvent` : ScriptableObjectWithRuntimeData: `Started` +0x18, `Performed` +0x20, `Canceled` +0x28 (Action<InputEventData>), `PollValue` Func<float> +0x30, `PollVectorValue` Func<Vector2> +0x38, `GetActiveValueType` +0x40. `Perform()` 0x9DFA50, `Perform(InputEventData)` 0x9DFA10, `Perform(Vector2)` 0x9DFAB0, `StartInput` 0x9DFDE0, `Cancel` 0x9DF940, `add_Performed` 0x9E0240 / `remove_Performed` 0x9E0450. Don't hook (all inputs).
- `InputEventData` struct (12 bytes, **passed by hidden pointer on x64**): `Source` int +0x0 (0 UNKNOWN, 1 POINTER, 2 KEYBOARD, 3 GAMEPAD), `_vecValue` Vector2 +0x4 (`Value` = x).
- Action names and bindings (e.g. `<Mouse>/scroll`) are asset data: read at runtime from `binding.ActionInstance.bindings`.

## HUD notifications

- `DroneActivitySceneNotifications` (HUD drone provider): `AddPlortSold(type, count)`; `_activeNotifications`. `DequeueResourceCollected` 0x75B430 and `DequeuePlortsSold` 0x75B200 are the same code: `NotificationEntry(provider, null, type.icon, Format.WithVariable("0", type.localizedName +0x28), count, config.DefaultStyle)` with `QuantumCloudResourceCollectedFormat` / `PlortSoldFormat` (bare name if empty). `DequeueNext` order: low battery, resources collected, plorts sold.
- `ItemAcquisitionNotificationList.HandleNotificationAvailable(provider)`; only while `get_CanQueueAny` (the list drops notices while the HUD is suppressed by a menu). Building a `NotificationEntry` by hand is unsafe (the list calls back into the entry's provider; the shop provider expects its own entry type).
- Mod use: `modules/custom_upgrades/sale_notify.cpp` (`SaleNotices`).

## Widgets and styling

- `SliderStyler` (`_style` SliderStyle asset): `OnEnable` 0x6CB430 -> `ApplyStyle` 0x6CAC90 (`transform.Find("Background")` Image, `slider.fillRect`/`handleRect` Images, `Selectable.colors`). The plot UI has no slider prefab; options ranges are steppers; `MapZoomSlider` is a custom drag handler bound to the map view model. Mod: `modules/custom_upgrades/plot_menu_slider.cpp` builds a uGUI slider at runtime.
- `SDFRoundedRectangle` (MP.SR.UI.SDFUI): the game's UI shapes, material from `Shader.Find("SDFUI/" + type)`; scroll-bar handle pink (0.871, 0.310, 0.522).
- `LocalizeStringEvent.UpdateString`: hooked by `ui/options_tab.cpp`; also serves `core/text_override.h` (texts for LocalizedString instances the mod owns: list rows, details panel, confirm popups).

## Main menu

- `LocalizedVersionText.OnEnable` (MP.SR.UI.MainMenu) 0x726E80: version label (`ui/modded_label.cpp`).

## Slimepedia (read-only; ui/slime_icons.cpp)

- `SceneContext.<PediaDirector>k__BackingField` +0x38; `SceneContext.<GameModel>k__BackingField` +0x98 (IDA via `SceneContextUtility.TryGetGameModel` 0x693A80; null = no save loaded). `SRSingleton<SceneContext>.get_Instance()` shared generic 0x4DC510.
- `PediaDirector`: `_pediaConfiguration` +0x28, `_pediaModel` +0x80 (null before a save loads). `IsUnlocked(PediaEntry)` 0x52C210 IDA = `_pediaModel.unlocked.Contains(entry)` (NRE if model null); overload `IsUnlocked(IdentifiableType)` 0x52C140: resolve with `MethodByParams("MonomiPark.SlimeRancher.Pedia.PediaEntry")`. `ShowPedia(entry)`.
- `PediaModel.unlocked` HashSet<PediaEntry> +0x18.
- `PediaConfiguration._categoryList` Object[] +0x18 (PediaCategory, MusicPediaCategory, ...); `PediaCategory._items` PediaEntry[] +0x30.
- `PediaEntry` (abstract SO): `_details` PediaEntryDetail[] +0x20, `_title` +0x38, `_description` +0x40, abstract `get_Icon()`. `IdentifiablePediaEntry._identifiableType` +0x58 (`get_Icon` 0x99E3D0 IDA: type's icon via virtual call). Other kinds: `FixedPediaEntry` (own `_icon`), `RadiantSlimePediaEntry` (`_slimeDefinition` +0x58).
- `PediaEntryDetail` (plain class): `Section` +0x10 (PediaDetailSection), `Text` +0x18, `TextGamepad` +0x20, `TextPS4` +0x28, `Contents` +0x30. `PediaDetailSection`: `_title` +0x18, `_icon` +0x20.

## From the custom_upgrades notes

### Plot menu navigation (slider keys; hooks since removed)
- `FocusManager.NavigateInDirection` hands a move to every IMoveHandler between the focused card and the spatial target via `INavigationHandler.TryHandleAsNavigation` 0x79C940; the focused card's own Selectable comes first (so `UniformGridView.OnNavigate` is never reached); after each call it checks the event's `used` flag (used -> no SetFocus)
- `ListUI.Submit` 0x83F450, `BaseUI.OnCancelPressed` 0x683490 (were hooked for select-to-step, removed)
- mod: none now (custom_upgrades sliders are mouse only); `NavigateInDirection` hooked by drone_price_limit

### SliderStyler / uGUI slider
- fields: SliderStyler `_style` +0x28 (SliderStyle), `_slider` +0x30
- Selectable `Navigation` struct {m_Mode +0, m_WrapAround +4, 4 x Selectable}; all zero = Mode.None; `Slider.OnMove` still steps left/right while it is the selection
- mod: plot_menu_slider.cpp

### PopupPrompt (gamecore_scenes bundle, UnityPy)
- `GameContext.UITemplates.popupPrompt`: Panel 960 wide with LayoutManager + EncapsulateChildren; `Panel/BodyContent/ButtonPanel` = game HorizontalLayout (`_reverseChildren` 0, centred, spacing 16), 64 px in from each side; `TextButton_NoneSelectable (Accept)` first at x -169 (positive on the LEFT), `(Cancel)` at +169; each 321.5 x 58.6; title 55 pt, labels 42 pt
- fields: `_positiveActionButton`, `_negativeActionButton`, `_positiveInput` (InputEvent; `PollValue` Func = bound action value), negative input event
- `UITemplates.CreatePopupPrompt(body, misc, positive, negative, expiration)` 0x6B1910; layout re-lays out on OnTransformChildrenChanged
- mod: silo_receipt.cpp (button clones, key polling), sell_upgrades (reorders)

### TextButton_NoneSelectable (duplicateassetisolation_assets__54b00e46...bundle / gamecore_scenes)
- components: Button (onClick empty), ButtonAnimator (hover anim / colours, may animate scale), EventTrigger, DetailsMoreButtonView (label LocalizeStringEvent), InputEventButton (performs the bound InputEvent on pointer down / up - before the Button click), EncapsulateChildren; children `Label Background` (SDF pill, label 42 pt at x 30, ResizeToPreferred) and `Icon Background` (`InputEventDisplay` key glyph, 64 px icons; `SetInputEvent(...)`)
- layout behaviours (`MonomiPark.SlimeRancher.UI.Framework.Layout`: HorizontalLayout, EncapsulateChildren -> SetSizeWithCurrentAnchors in LateUpdate, ResizeToPreferred) run only under a `LayoutManager` (OnEnable / LateUpdate -> ApplyLayout)
- gotchas: not focusable (NoneSelectable); half-width splits get overwritten by the layout; InputEventButton on a Sell clone made one click sell AND confirm a purchase
- mod: silo_receipt.cpp, sell_upgrades

### ConfirmPopup / map expansions
- `AccessDoorUI.PromptForExpansion` -> ConfirmPopup; prompts sorted by `<>c.<UpdateShowData>b__27_0` (`b.PromptType - a.PromptType`, SUBMIT first) into its InputLegend; `ConfirmPopup.UpdateShowData` (setup Title = item `_title` LocalizedString)
- mod: sell_upgrades

### ListUI alt / menu items
- `ListUI.altSubmit` InputEvent (the sell key); `GenericViewHolder.BindData` 0x83C900 enables `unavailableIcon` with `_unavailableIcon` while !IsAvailable (in re)

### Selectable (uGUI)
- `[DisallowMultipleComponent]` (inherited): can't add a Toggle to a GameObject with a Button ("Can't add 'Toggle' ... because a 'Button' is already added")
- `IsPressed()` protected (false while not interactable), `get_isPointerInside` private; click detection = IsPressed true last frame, false now, pointer inside
- `interactable` false = prefab disabled colours
- mod: silo_receipt.cpp

### CanvasScaler
- ScaleWithScreenSize 1920 x 1080 match 0.5 -> 16:9 is 1080 units high, ultrawide less; ScrollRect / RectMask2D / Scrollbar used for overflow
- mod: silo_receipt.cpp

### UIInputLocker / pause
- `PauseMenuDirector.TogglePause` 0xAEFF20 only calls PauseGame while `Time.timeScale > 0` (Esc does not open the pause menu while a locker pauses)
- `CursorLockHandler` frees the cursor while paused; non-UI sound pauses

### Interactables / UIDetector
- `UIDetector.Update` 0x6AD6B0 - sphere cast, then GetComponents + GetComponentsInParent<IUIInteractable> on every hit collider
- `GadgetUIInteractable.Awake` 0x763FA0 - GetComponentInParent<IGadgetInteractor>; prompt = `_deactivateInteractionPrompt` when IGadgetInteractor.ShowDeactivatePrompt()
- `UIActivator.OnInteract` 0x6ACB60, `UIDisplayInteractable.OnInteract` 0x6B0AD0 (Refinery Link Use opens the refinery screen via one of these; class logged)
- `GadgetLabelDisplay.UpdateLabels` 0x75F820 -> GetTargetGadgetInfo (gadget-mode label)
- mod: quantum_link.cpp (Use hooks), dj_buildables.cpp (booth E prompt: GadgetMusicPlayer added before GadgetUIInteractable), gadget_look.cpp

### HUD notifications
- `DroneActivitySceneNotifications.AddPlortSold(IdentifiableType, int)` 0x75A9D0 (private; adds / merges per type), `AddResourceCollected`
- `ItemAcquisitionNotificationList.HandleNotificationAvailable(provider)` 0x76B7B0 (dequeues every item into its pending queue; drains and DISCARDS when `get_CanQueueAny` 0x76D2B0 is false = menus open); `_providers`; provider `DroneActivityNotificationProvider._activeNotifications`
- mod: sale_notify.cpp (list kept with a GC handle; FindObjectsOfTypeAll at most 1 Hz when lost)

### Slimepedia string tables
- English `PediaPage_en` entry `m.risks.shadow_slime` ("Rancher Risks"); edit with `StringTable.GetEntry` / `StringTableEntry.set_Value` (in memory); table found by name via FindObjectsOfTypeAll(StringTable)
- mod: shadow_catcher.cpp

### Plot menu details panel
- `PurchaseItemDetailsContent` has its own nested Canvas and no GraphicRaycaster (in re); `ListUIRoot.OpenPopupPrompt` shared generic body

## From the minimap, map_tools, quick_travel, wall_markers, collectibles notes

### HudUI (MonomiPark.SlimeRancher.UI)
- methods: `Awake` 0x698620 - unique body (hooked) (IDA)
- methods: `UpdateHudVisibility` 0x6997D0 / `HideUI` - deactivate HudUI's OWN GameObject while the HUD is hidden (find with include-inactive) (IDA)
- methods: `GetSortOrder` - uses a Canvas on HudUI (HudUI GameObject carries the HUD Canvas) (IDA)
- fields: `_debugText` (TMP; font source)
- mod: MM (parent canvas)

### UIDisplayManager (MonomiPark.SlimeRancher.UI.Framework.Displays)
- methods: `get_Instance` (static)
- methods: `RequestPreloadDisplay(UIDisplayHandle{string _name}, object requester)` 0x7A5B10 - `FindDisplayLayerEntry(name)`, requires a valid `_sceneAsset` key, `UIDisplayLoadManager.GetOrCreateLoadEntry(sceneAsset)`, `entry.AddPreloadRequester(requester)` (HashSet<object>, types.cs), queues the load, returns `UIDisplaySceneLoadEntry` (IUISceneLoadHandle); idempotent per requester (IDA)
- methods: `ReleasePreloadDisplay(UIDisplayHandle, object)` - HashSet<object> uses string equality: a new string with the same text releases (IDA/inferred)
- methods: `CloseDisplayByObjectInHierarchy(Component)` - closes the display holding the component (what `DroneUIDisplay.CloseThisDisplay` does)
- methods: `UIDisplaySceneLoadEntry.get_IsDone` 0x7B1370
- gotchas: game's own user: `UIDisplayPreloadOnDisplayOpenClose`; the load manager may drop requests on a scene change (re-request after world reset)
- gotchas: `UIDisplayHandle` struct passed by value = string pointer in rdx
- mod: MM (map UI preload), MT (close map)

### UIAudioCueHandle
- methods: `PlayAudioCue` (NO_WARNING)
- mod: MT (`MapUI._failedPlaceWaypointCue`)

### InputLegend / InputHint / InputIcon (key bar)
- fields: `InputLegend._hints` InputHint[] (children of one layout, laid out left to right in sibling order); `InputHint._configuration` (InputHintConfiguration: Label, InputEvent (asset name), AdditionalInputEvent), `_labelText` (TMP_Text), `_inputEnabled`; `ComponentInterfaceBase._object`; `InputLegendFocusRegistrar._passThrough` +0x20 (ComponentInterface<IInputLegend>) (dump)
- methods: `InputIcon.SetButtonImage` 0x7C6B20 - `_buttonRoot` on, `_keyRoot` off, `_buttonImage.sprite` (IDA); `InputIcon.SetKey` - `_buttonRoot`/`_doubleTapRoot` off, `_keyRoot` on, `_keyImage`/`_keyLocalizeStringEvent` off, `_keyText` (IDA)
- gotchas: `IInputLegend` has 3 implementations: `InputLegend`, `PediaRoot`, `InputLegendFocusRegistrar` (forwards via `_passThrough`); `MapUI._inputLegend` can be the registrar
- gotchas: InputHint fades its CanvasGroup with a spring; a cloned hint must lose `InputHint` and `InvokeInputEventOnPointerClick` (destroy) and `InputEventDisplay`, `LocalizeStringEvent`, `Selectable` (disable)
- gotchas: mouse glyphs: `KeyboardMouseIconSet` assets (`InputIconSet<KeyboardMouseIcon>._items`), entries `_control` (`<Mouse>/rightButton`, ...) + `_buttonSprite`; only leftButton may have a sprite (mirror it)
- mod: LH (shared), MT ("[RMB] Teleport"), sandbox ("[G] Add materials")

### LoadingScreenView (MonomiPark.SlimeRancher.UI.Loading)
- methods: `FadeToBlack()` (0) - alpha += dt/_fadeOutTime, clamped to 1; returns a 16-byte `Continuation` via hidden return buffer (rcx), `this` in rdx (IDA)
- fields: `_fadeOutTime`
- mod: QT

### RanchHouseUI (MonomiPark.SlimeRancher.UI)
- methods: `Sleep` (3) -> `SleepUntil`; `Close()` (0) = Esc path
- fields: `_sleeping`, `_isClosing`, `_timeDir`
- mod: QT (auto-close after death time skip)

### TimeDirector (global ns)
- methods: `HoursUntil(1)` - returns boxed double via runtime_invoke (value after the 16-byte header)
- mod: QT (log only)

## From the slime_dj, spawn_scaling, hl2_movement, quick_start, free_placement, misc_tweaks, plot_upgrades, drone_price_limit notes

### Boot flow (quick_start; dump + disasm)
- `level0` (bootstrap, `BootstrapCompanyLogoScene`) -> SceneLoader loads group **CompanyLogo**
- `CompanyLogoScene.Start` queues startup music, starts `StartUpCoroutine` (16 states; every wait is `WaitForSeconds(<serialized float>)` or a `Time.time` loop; nothing loads): logo fade in `logoFadeInTime`, hold `logoHoldTime` (blink `logoBlinkTime` inside), fade out `logoFadeOutTime`; black `logoBackgroundHoldTime`; warning screen fade in `warningFadeInTime`, hold until `warningHoldTime` or `_skipWarningScreen` (set by key via `CompanyLogoScene.OnInteract`), fade out `warningFadeOutTime`; black again; game logo bg `splashFadeInWaitTime`, logo `splashFadeInTime`; also `logoBackgroundFadeOutTime`; then `SceneLoader.LoadSceneGroup(PlatformInfoProvider.standaloneEngagementPrompt)` = group **StandaloneStart**, LoadStep `OnMainMenuLoaded`
- `CompanyLogoScene.OnMainMenuLoaded` finds `PlatformEngagementPrompt`, subscribes `AnimateOut` (OnSceneActivated) and `StartLoadingIndicator` (BeforeAnimateOut), calls `PlatformEngagementPrompt.Onloaded()` (texts + `RegisterInput()` on the submit action's `canceled`) -> "press any key"
- `StandaloneEngagementPrompt.OnInteract(CallbackContext)`: click, unregister input, disable EventSystem; `IUserManager` establishes the user -> `SystemContext.InitializePlatformForCurrentUser` -> `OnSuccessfullySetupUser`; `BeforeAnimateOut`; `Animate(AnimationDuration)`; `LoadSceneGroup(SceneGroupToLoad)` = **MainMenuFromBoot**, LoadStep `OnSceneGroupLoad` (optional EA message fade `EaMessageFadeTime`) -> `CompanyLogoScene.AnimateOut`. The context argument is never read (rdx overwritten before use) - invoking with a zeroed CallbackContext is safe in 1.3.0
- `StandaloneStartScreen` (auto-loads `defaultSceneGroup`) is not part of this flow
- fields: CompanyLogoScene (MonomiPark.SlimeRancher.UI) the 11 timing floats above + `_skipWarningScreen`; PlatformEngagementPrompt (…UI.MainMenu) `AnimationDuration`, `EaMessageFadeTime`
- mod: quick_start (pre-hook CompanyLogoScene.Start, post-hook PlatformEngagementPrompt.Onloaded, pass-through StandaloneEngagementPrompt.OnInteract)

### Main menu Continue (dump + disasm)
- `MainMenuLandingRootUI` (…UI.MainMenu) builds `ButtonBehaviorModel`s from `ButtonBehaviorConfiguration`; fields `_models`, `_subListModels`; `OnSubmit` -> selected model `InvokeBehavior()`; `OnEnable` (hooked by ui/modded_label.cpp; one hook per target)
- Continue = `ContinueGameItemDefinition` -> `ContinueGameBehaviorModel` (: `LoadGameBehaviorModel`, …UI.MainMenu.Model), `<GameDataSummary>k__BackingField` = `AutoSaveDirector.GetSaveToContinue()`
- `LoadGameBehaviorModel.InvokeBehavior()`: returns if `SceneLoader.IsSceneLoadInProgress`; else `AutoSaveDirector.BeginLoad(summary.SaveIdentifier, null)`
- visibility: `ButtonBehaviorConditionUtility.HasContinueGame()` -> `AutoSaveDirector.HasContinue()`
- `Persist.Summary.get_SaveIdentifier()`; boxed `GameSaveIdentifier` / bool from runtime_invoke read at 2*sizeof(void*)
- mod: quick_start auto_continue

### IntroSequenceUIRoot (MonomiPark.SlimeRancher.UI.IntroSequence) - new-game story intro (disasm)
- `Start`: black screen alpha 1, `PauseNonUiSfx(true)`, `_cameraEnabled.Value = false`, `StartUpCoroutine`: queue `_introSequenceMusic`, wait `_fadeInWaitTime`, fade out over `_fadeInTime`, `StartFrame(_frameIndex)`
- `StartFrame(int)` tweens the cutscene camera; on completion shows text, sets `_isInputEnabled`. `_next` input -> `TryToIterateFrame`, which after the last frame does what `EndSequence()` does: `_isInputEnabled = false`, `MusicController.Dequeue(_currentMusic)`, `StartCoroutine(EndCoroutine)`
- `EndCoroutine`: fade to black `_fadeOutTime`, wait `_fadeOutWaitTime`, virtual close (pop + destroy)
- `OnDestroy`: `PauseNonUiSfx(false)`, destroy cutscene camera, `_cameraEnabled.Value = true`, `MetaGameDirector.StartMainQuest(true)`, `FadeIntoGameplay()` (`TutorialDirector.SuppressTutorials(HudUI)`, `LockOnDeath.Freeze()`, `DeathObscurer.StartObscuring(false, _fadeOutTime, cb)` with cb = `LockOnDeath.Unfreeze()` + `UnsuppressTutorials`), `CutsceneDirector.ClearIntroSequenceUIRoot()`
- gotchas: `EndSequence` has no direct caller (probably the debug skip); fades also drive tweens / particles / DeathObscurer (keep them > 0)
- mod: quick_start (pre-hook Start, replace StartFrame -> EndSequence)

### FocusManager (static) / FocusManagerBehaviour
- move / submit / cancel actions call `NavigateInDirection(MoveDirection, InteractionCause, AxisEventData)` 0x7926D0, `SendSubmitActionToFocus(InputAction)` 0x793BE0, `SendCancelActionToFocus(InputAction)` 0x7939A0 (IDA)
- mod: drone_price_limit station_dialog.cpp detours all three (modal dialog input)

### PromptContainer / InputIcon / EncapsulateChildren
- see re/drones.md "Drone station menu" (prompt pill layout, `InputIcon.SetKey` 0x7C6FE0)
- pointer input without managed delegates: a `UnityEngine.UI.Toggle` (navigation None, no transition) polled and reset; own Canvas (sorted above) + GraphicRaycaster

### FontsUsedDebugUI (MonomiPark.SlimeRancher.UI.Debug) - already in re/debug-mode.md; RVAs: `StartDebugUI` 0x83C210, `OnEnable` 0x83B500, `OnGUI` 0x83B690
- `StartDebugUI()` = `new GameObject("FontsUsedDebugUI", RectTransform, Canvas, FontsUsedDebugUI)`; OnEnable: `TMPro_EventManager.TEXT_CHANGED_EVENT` + `FindObjectsOfType<TMP_Text>(true)` once; OnGUI Repaint only
- mod: debug_tools (DontDestroyOnLoad + GC handle)

## From the sandbox, sell_upgrades, collectibles, instant_build notes

### ItemAcquisitionNotificationList (HUD notifications)
- methods: `Update` 0x76CF10 - starts a page only when every slot is free (`_availableDisplayCount == _displayEntries.Length`); `ShowNextPage` 0x76C040 - takes up to `_activeEntries.Length` pending entries, staggers by `_timeBetweenShowItems`, copies the entry's LocalizedString (resolved only when the entry starts); `HandleEntryFadeOutComplete` 0x76B680 - frees a slot; with nothing pending deactivates every slot (reactivated by ShowNextPage since `_isFirstPageInList` is set after every page) (IDA)
- fields: `_pendingNotifications`, `_availableDisplayCount`, `_displayEntries`, `_activeEntries`
- gotchas: `Time.timeScale == 0` stops Update (page stays up while paused); the list discards notices while a menu hides the HUD; reused LocalizedStrings must not be reused while pending; `AnimatedFaderGroup.FadeIn`
- mod: sb notices via custom_upgrades/sale_notify (`SaleNotices::QueueText`, `FlushText`)

### UIDisplayManager (scene preloading)
- fields: `UIDisplayManager.Instance._layerSet._layerList[]._displayList[]` (`UILayerDefinition.LayerEntry._name`, `_sceneAsset`, offsets from live object)
- methods: `RequestPreloadDisplay(UIDisplayHandle{name}, string reason)`, `ReleasePreloadDisplay`, `UIDisplaySceneLoadEntry.IsDone`, `OpenDisplay`
- facts: display named "Fabricator" (scene `MenuLayer_Fabricator`)
- mod: sb PinIconTick (minimap does the same for the map UI)

### InputLegend / InputHint
- fields: `InputHint._inputEnabled`, `InputHint._label`, `_labelText`, `InputHintConfiguration.InputEvent`
- mod: sb [G] Add materials (modules/common/legend_hint.h); su alt prompt

### ListUI / ListUIRoot`4 (list menus)  [su]
- chains: `ListUI.<ShowMenu>b__4` -> `CreateDetailsMoreButton(item, category, vm)`: available -> Instantiate(`category.detailsButton`) (Purchase), else `category.detailsInfo` (PURCHASED label twin), into `ListUI.mainActionButtonContainer` (no LayoutGroup), `DetailsMoreButtonView.BindData(vm)` (label = vm.actionLabel with Button, else vm.infoLabel = item's `_infoButtonLabel` instance); then `CreateDetailsAltButton(item, category, showAlt)`: destroy `altActionButtonGameObject`, instantiate `altDetailsButton` into `altActionButtonContainer` if `ShowAltActionButton(item)`
- methods: `ListUIRoot`4.OnAltAction(item)` 0x11AF260 (shared generic body; Pedia for IPediaEntryProvider), `ShowAltActionButton` 0x11AFE20 (shared body; item has Pedia entry), `OpenPopupPrompt` (shared generic body), `RebuildUI` 0x11AFB30
- gotchas: Purchase prefab click performs the menu submit input event (= gamepad A); `onAltSubmitActionPerformed` set for every item; prefab `TextButton_NoneSelectable` not reachable by gamepad navigation; `InputEventDisplay.SetInputEvent(ListUI.altSubmit, null)`; `Selectable.set_interactable(false)`; `UnityEngine.UI.Button.Press`
- asset: PopupPrompt Panel/BodyContent/ButtonPanel HorizontalLayout (`_reverseChildren` 0, centred, spacing 16), Accept first then Cancel (gamecore_scenes bundle); `GameContext.UITemplates.popupPromptPositive/NegativeActionLabel`; PopupPrompt texts via LocalizeStringEvent (`_messageText`, `_positiveActionButtonText`, `_positiveActionButton`)
- mod: su detours BindData, Button.Press, ShowAltActionButton, OnAltAction, OpenPopupPrompt

### ConfirmPopup / AccessDoorUI  [su]
- methods: `AccessDoorUI.PromptForExpansion` 0x84B9D0 - opens ConfirmPopup (UIDisplayManager.OpenDisplay, `ConfirmPopupSetup(item._title, item._promptMessage)` + Confirm/Cancel prompts); `ConfirmPopup.UpdateShowData` 0x6D09D0 sorts prompts with `<>c.<UpdateShowData>b__27_0` 0x6E5960 (`b.PromptType - a.PromptType`: SUBMIT before CANCEL) into its InputLegend
- mod: su reorders Cancel left for expansions

### Save list details panel (SaveGamesRootUI)
- methods: `SaveGamesRootUI.FetchGameSummaryItemData(GameSummaryModel)` 0x7351A0 - one `GameSummaryItemViewModel` per `GameSummaryItemDefinition` of `_gameSummaryItems` (Newbucks, zone, day, discoveries, Tarrs, slime aggression) (IDA)
- classes: `TarrEnabledGameSummaryItemDefinition` (`_enabledString`, `_disabledString`, `_icon`), `TarrEnabledGameSummaryItemViewModel(definition, model)` (.ctor), `GameSummaryModel._gameSummary` (`Summary.SaveName`, `SaveNumber`)
- gotchas: an Instantiate copy of the definition gets fresh LocalizedString instances (override them, not the originals)
- mod: sb cheated_save.cpp "Sandbox: Used" row
- other: `LoadGameBehaviorModel.get_Label` (save name label; hooked in an old version, removed), `LoadGameBehaviorModel.InvokeBehavior` -> `AutoSaveDirector.BeginLoad`

### Sprites by name (loaded-at-scan facts)
- `iconCategorySlimes` (pedia/shop UI scenes, single pink slime), `pediaHomeSlimes` (shared bundle), `iconLargoPedia_small` / `iconLargoPedia`, `iconCategoryUtilities` / `iconCategoryWarptech` / `iconCategoryDecorations` (loaded from start), `pin` (Fabricator scene only); decor sprites `iconDecorPinwheel...`, `iconDecorGreenPine1`
- gotchas: `FindObjectsOfTypeAll(Sprite)` sees only currently loaded sprites
- mod: sb EnsureNamedSprites
- collectibles `FindNamedSprites` (unverified in game, see its log line `collectibles: game sprites by name`): `iconGordoPedia_small` / `iconGordoPedia` (gordos), `iconPediaDrone_small` / `iconPediaDrone` (research drones), `iconExplorerDronePedia` (ghostly drones), `pediaHomeWorld` / `iconPediaHomeWorld` (map data nodes; a Slimepedia home tile, may be unloaded in game)

## From the ui/ (overlay, Options tab, icons, input) notes

### OptionsUIRoot (MP.SR.UI.Options)  [: DisplayBaseUI]
- methods: `Close()` 0x71A860 - goes through `SaveOrRevert` (IDA)
- methods: `SaveOrRevert()` 0x71C720 - shows a save-or-revert popup when any model in `optionsItemModels` has `HasUnsavedSelection()`; also reached from `CheckThenSwapCategory` (IDA)
- methods: `OnCancel` -> `Close` - reached only while FocusManager's focus is inside the Options screen (see FocusManager) (IDA)
- methods: `BindItemCategory(OptionsItemCategory)` 0x71A2F0 - additionally: calls `optionsAdapterView.BindDataList(models, 0)` and `BindOptionsDetailsView(0)`, so selection, details and scroll jump to row 0; `BindOptionsDetailsView(0)` reads `optionsItemModels[0]` -> ArgumentOutOfRange on an empty model list (IDA, crash triage re/crash-2026-10-22-1855.md §3)
- methods: `BindOptionsDetailsView(int)` - answers the adapter's `OnItemSelect` (IDA)
- fields (by name): `categories`, `currentCategoryIndex`, `optionsItemModels`, `optionsDetailsView`, `optionsAdapterView` (offsets above)
- chains: BindCategories builds `this.categories.Concat(config).Append(about).Where(CheckIsActive)` -> a category appended to `config.items` for the duration of the call lands after the stock tabs, before About (verified in game)
- chains: live tab refresh = `OptionsUIRoot.BindItemCategory(Mods)` (the game's own tab-swap path): GetOptionsItemsForCategory -> adapter rebind -> row 0 selected
- gotchas: several OptionsUIRoot instances can be loaded; `FindFirstObjectByType` is not always the one on screen -> match `categories[currentCategoryIndex] == ourCategory` (fallback: `optionsItemModels` are ours), scanning `Resources.FindObjectsOfTypeAll` instances with an active GameObject
- gotchas: only alive while the Options screen is open
- mod: ui/options_tab.cpp hooks `BindCategories` (`ui.options_bind_categories`), optional `BindItemCategory` (`ui.options_bind_item_category`, empty-tab details text "Mods: settings for sr2mods"); calls BindItemCategory for the child-row refresh

### OptionsViewModel (MP.SR.UI.Options)
- methods: `GetOptionsItemsForCategory(config, category, bool)` - result is `.ToList()`ed by BindItemCategory, so returning an `OptionsItemModel[]` works (IDA)
- mod: ui/options_tab.cpp `ui.options_get_items`: for the Mods category returns models built with `il2cpp_object_new` + `PresetOptionsItemModel..ctor(def)`; never an empty array (see BindItemCategory gotcha)

### PresetOptionsItemDefinition / ToggleOptionDefinition / ScriptedValueRangeOptionDefinition (MP.SR.Options)
- methods: `CreateOptionItemModel()` - throws NRE on a runtime clone whose `_optionsItemModels` list is null (probable cause of the first in-game crash) -> build models with the ctor instead (inferred, log)
- methods: `ToggleOptionDefinition.GetOptionsPresets()`, `ScriptedValueRangeOptionDefinition.GetValueForIndex(int)` - used for the game's own value mapping (dump)
- fields (by name): toggle `TargetBool`, `_defaultValue`, `_positiveOption`, `_positiveReferenceId`; range `_targetValue`, `_minValue`, `_maxValue`, `_valueStep`, `_normalizeOutputValue`, `_defaultValue`; definition `_referenceId`, `_applyImmediately`, `_requireConfirmation`, `_showTutorialDisclaimer`, `_indent`, `_showCondition`; preset `_wrapAround`, `_optionsItemModels`, `SupportedInputDeviceAssets`, `SupportedPlatforms`; `OptionPresetSet._optionPresets`
- chains: with `_applyImmediately` set, `PresetOptionsItemModel.set_TempPresetIndex` -> `ApplyTemporaryValue` -> `ApplyValue` -> `ApplyPresetSelection` in the same call (no Apply button needed)
- chains: toggle "on" = the preset whose label or reference id is `_positiveOption`
- gotchas: `_defaultValue` is refreshed from the current setting before each bind, so the model ctor's `GetDefaultPresetIndex()` gives the current value; side effect: the game's "reset category to default" re-applies current values
- mod: ui/options_tab.cpp clones a template (first non-conditional, platform-agnostic Toggle and ScriptedValueRange definition in the passed config) with `Object.Instantiate`; gives each clone `_referenceId = sr2mods.<module>.<key>`, a private ScriptedBool/ScriptedFloat, `_applyImmediately` on, `_requireConfirmation`/`_showCondition`/`_normalizeOutputValue` cleared, `hideFlags = DontUnloadUnusedAsset`, GC-handle pinned. Hooks `ApplyPresetSelection` on both (`ui.options_toggle_apply`, `ui.options_range_apply`); for our defs the original never runs

### PresetOptionsItemModel (MP.SR.DataModel)
- methods: `set_CurrentPresetIndex(int)` 0xAC8360 - sets the saved preset key and raises `OnSelectionChanged`, nothing else (IDA)
- methods: `HasUnsavedSelection()` - `CurrentPresetIndex != _tempPresetIndex`; `CurrentPresetIndex` only moves through `SaveSelectedValue` -> `set_CurrentPresetIndex` (IDA)
- methods: `get_TempPresetIndex()`, `RebuildOptions()` (dump)
- fields (by name): `_presets`; `OptionsItemModel.optionsItemDefinition`
- mod: ui/options_tab.cpp `CommitModsSelections` calls `set_CurrentPresetIndex(TempPresetIndex)` on our models after each change so `SaveOrRevert` never prompts for Mods rows

### OptionsItemViewHolder (MP.SR.UI.Options)  [: ButtonViewHolder<T>]
- methods: `BindData(model)` 0x718690 - destroys the old control (`Destroyer.Destroy`) before creating a new one (IDA)
- methods: `Activate()` - row click / Submit; acts only on "activatable" controls, a toggle's preset selector is not one (no-op for toggles). No native callers: the row button's click is a serialized UnityEvent (unverified that it is Activate)
- methods: `NextSetting()`, `PreviousSetting()` - the arrows' path (`SelectValue` -> apply)
- fields (by name): `label`, `_model`; `ButtonViewHolder<T>.button`
- gotchas: rebinding destroys row controls; a FocusManager focus on one becomes dead (see FocusManager)
- mod: ui/options_tab.cpp hooks `BindData` (`ui.options_row_bind`, LSE registration) and optional `Activate` (`ui.options_row_activate`: toggles our rows via Next/PreviousSetting; skipped if folded)

### OptionsDetailsView / OptionsPresetSelector / OptionsPresetSelectorBase / CategoryTabViewHolder / CategoryViewModel
- fields (by name): `OptionsDetailsView._header`, `_details`; `OptionsPresetSelector._currentSelectionText`; `OptionsPresetSelectorBase.optionsItemModel`; `CategoryTabViewHolder.titleTextString` (LocalizeStringEvent), `titleText`; `CategoryViewModel.title`; `OptionsItemCategory._icon`, `OptionsAboutCategory._icon`
- namespaces: OptionsDetailsView/OptionsPresetSelector(Base)/OptionsItemViewHolder/OptionsAdapterView in MP.SR.UI.Options; CategoryTabViewHolder/CategoryViewModel in MP.SR.UI.Adapter
- mod: hooks `OptionsDetailsView.BindData` (`ui.options_details_bind`), `OptionsPresetSelector.OnSelectionChanged(int)` (`ui.options_selector_changed`), `CategoryTabViewHolder.BindData` (`ui.options_tab_bind`)

### AdapterView<...> / OptionsAdapterView (MP.SR.UI.Options)
- methods: `AdapterView<…>.BindDataList(list, int)` 0x1F281F0 - ends with `currentlySelectedIndex = -1` and `SelectItem(initialSelection)` (IDA)
- methods: `AdapterView.SelectItem(int)` 0x1F29BC0 - virtual (look up on the runtime class); moves the navigator (gamepad up/down continue from there) and the row highlight, raises `OnItemSelect` (IDA)
- methods: `AnimateBindDataList` - the adapter is paged and binds asynchronously; direct TMP writes get overwritten by later refreshes (log)
- gotchas: shared generic bodies; never hook
- mod: ui/options_tab.cpp calls SelectItem after the child-row rebind

### ScrollArea (MP.SR.UI.Framework.CommonControls)
- methods: `ScrollTo(float, ScrollTransition)` - transition 0 = jump, also stops a running animation (dump/inferred)
- methods: `HandleFocusChange` -> `ScrollIntoView` - scrolls a frame after a focus change, only when the row is outside the viewport
- fields: `_scrollPosition` (world units)
- mod: ui/options_tab.cpp saves/restores the Mods list offset (nearest ScrollArea above the adapter's `container`; fallback `UnityEngine.UI.ScrollRect`: `get_content`, `StopMovement`, `RectTransform.get/set_anchoredPosition`); re-applied for 6 ticks

### FocusManager (MP.SR.UI.Framework.Interaction.Focus), static
- methods: `SendCancelActionToFocus()` 0x7939A0 - `if (CurrentFocus) SRExecuteEvents.ExecuteCancelHierarchy(CurrentFocus)`: Cancel (Esc, gamepad B) goes to the focused GameObject and up its parents, only while it is alive (IDA)
- methods: `get_CurrentFocus()`, `SetFocus(GameObject, InteractionCause)` (resolve by params; `InteractionCause.PROGRAMMATIC`)
- gotchas: `FocusManager.Update` never repairs a dead focus -> a focus on a destroyed row control makes Back do nothing and the Options menu could not be closed (test m2). A vanilla tab swap does not hit this because focus is on the tab strip
- mod: ui/options_tab.cpp moves focus to the changed row's `ButtonViewHolder.button` GameObject before/after rebinds; never touches a focus outside the Options screen

### LocalizeStringEvent (UnityEngine.Localization.Components, Unity.Localization)
- methods: `UpdateString(string)` - the component's only text sink; hooking it covers paged/recycled views, deferred async localization and locale changes (log, 4th run)
- fields: `m_StringReference` (the LocalizedString instance; optional, enables core/text_override)
- gotchas: disabling the LSE + direct TMP write does not stick (refreshed later)
- mod: ui/options_tab.cpp `ui.options_lse_update_string`: substitutes our text for components registered by the BindData hooks (keyed by object + `m_CachedPtr`) and for `m_StringReference` instances registered in core/text_override (identity lookup); game components pass through. ui/modded_label.cpp disables the clone's LSE

### LocalizedString / LocalizedReference (UnityEngine.Localization)
- methods: `LocalizedString..ctor()` - optional, fresh title for the tab; `GetLocalizedString()`, `get_IsEmpty()` (above)

### MainMenuLandingRootUI (global, Assembly-CSharp)
- methods: `OnEnable()` - fires on title screen show and on return from a submenu (log)
- prefab (UnityPy, `scriptableobjectlookupdata_assets_all` bundle, SR2 1.3.0): `MainMenu` (MainMenuLandingRootUI, CanvasScaler) > `MenuContent_OverscanAdjustment` > `LayoutGroup_Left (Labels)` (VerticalLayoutGroup, child alignment LowerLeft, spacing 7, anchored bottom-left (30,30), 490x500) > `SwitchUserButton`, `Version Label` (TMP "v x.y.z" + LocalizeStringEvent + LocalizedVersionText), `SupportLabel` (TMP "support.slimerancher.com", 19.2 pt, grey-brown, 24 high, LocalizeStringEvent + LocalizeFontEvent)
- gotchas: MinHook allows one hook per target -> ui/modded_label.cpp owns it; `quick_start` registers via `modded_label::SetMenuEnableListener`
- mod: ui/modded_label.cpp `ui.main_menu_on_enable` post-hook; `Transform.Find("MenuContent_OverscanAdjustment/LayoutGroup_Left (Labels)/SupportLabel")`, Instantiate into the same group (appended last = bottom row), named `sr2mods_ModdedLabel`, LSE disabled, `TMP_Text.set_text("[modded]")` re-set for 10 frames

### InputDirector (MP.SR.Input)
- fields (by name): `_mainGame` (InputActionMapReference: gameplay map), `_paused` (menu map: full map, pause menu, Slimepedia)
- methods: `OnPausedChanged` - swaps `_mainGame` for `_paused` while a game menu is open (dump/inferred)
- methods: `InputActionMapReference.op_Implicit(InputActionMapReference)` -> `InputActionMap`
- gotchas: the game re-enables maps itself -> re-assert Disable each tick; no InputDirector on title screen / loading (FindObjectOfType retried at most 1/s: running it every tick cost 1-2 ms/frame)
- mod: ui/game_input.cpp (`InputActionMap.get_enabled/Enable/Disable`, Unity.InputSystem)

### EventSystem (UnityEngine.EventSystems, UnityEngine.UI)
- methods: `get_current` (static), `get_currentInputModule`; the module is `InputSystemUIInputModule` (reads the Input System, not window messages); disable via `Behaviour.set_enabled(false)`
- gotchas: `EventSystem.IsPointerOverGameObject` is useless for HUD elements: the minimap is itself a UI object on the HUD canvas
- mod: ui/game_input.cpp disables the module while the pointer is over the overlay

### CursorManager (MP.SR.UI.Framework.Interaction), static
- methods: `get_LockState()` (what the game wants), `HideCursorFor(region)` (full map hides the hardware cursor, draws its own reticle), `UpdateCursorLockState()` private (`Cursor.lockState = _lockState`), `UpdateCursorVisibility()` private (`Cursor.visible = ShouldCursorBeVisible()`), `ShouldCursorBeVisible()` private (`_isCursorVisible`, no hide requesters, not force-hidden)
- gotchas: writes `Cursor.visible` on pointer enter/exit events -> re-assert each tick
- mod: ui/game_input.cpp; `GameMenuOpen()` = `CursorManager.get_LockState() != Locked`

### Overlay host: D3D11 / DXGI / Win32 (not game code)
- IDXGISwapChain vtable slots: Present 8, ResizeBuffers 13; IDXGISwapChain1::Present1 22; IDXGISwapChain3::ResizeBuffers1 39. Read from a throw-away `D3D11CreateDeviceAndSwapChain` on a hidden 64x64 window (GetProcAddress on the loaded d3d11.dll)
- Unity presents on its render thread; window messages arrive on the main thread
- Win32 ImGui backend reads modifiers with `GetKeyState` (per thread, stale on the render thread) and queues them with every key message; holding Shift repeats WM_KEYDOWN -> modifier toggles; with `ConfigInputTrickleEventQueue` `io.KeyShift` flickers. Fix: strip ImGuiMod_Shift/Ctrl/Alt events from `g.InputEventsQueue`, then queue `GetAsyncKeyState`
- `Platform_SetImeDataFn` would call into the window from the render thread (cleared)
- Alt+Space: `WM_SYSKEYDOWN/UP` VK_SPACE, `WM_SYSCHAR ' '`, `WM_SYSCOMMAND SC_KEYMENU` open the system menu / menu mode
- mouse 4/5 are in the WM_MOUSE* range; swallowing them prevents `WM_APPCOMMAND` browser-back
- `MapVirtualKeyW(vk, MAPVK_VK_TO_CHAR)`; `GetKeyNameTextW` with `MAPVK_VK_TO_VSC_EX` + extended bit for E0 keys / nav cluster / arrows / Num / / Num Lock
- mod: ui/overlay.cpp

### Steam overlay renderer (GameOverlayRenderer64.dll, Steam client 2026-10-22; RVAs for that build)
- find again by strings "Detected hot-key via base input", "Reseting get message hook"
- methods: message handler 0xAC2B0, fed by its `WH_GETMESSAGE` hook on the game window thread (proc 0xAE400), a `WH_CALLWNDPROC` hook 0xAD5C0 (IME only) and detours of GetMessageA/W, PeekMessageA/W (original, then handler), DispatchMessageA/W (handler first) (IDA)
- methods: hotkey combination check 0x61E60 on a `vk -> down` map (IDA)
- methods: per-frame poll 0xC8230 -> 0xADF10: real `GetAsyncKeyState(VK_TAB)`; on change posts msg 0x14FC, wParam 0x198239AC, lParam 1 down / 0 up; handler turns it into a synthetic Tab event (pt = (-1,-1)) for the map (IDA)
- gotchas: real Tab WM_KEYDOWN/UP never updates the map; the handler special-cases `wParam == VK_TAB` with a real cursor pos and eats the key-down only while the hotkey's modifiers are held (so neither the game nor our WndProc sees Shift+Tab). WH_KEYBOARD_LL and WM_KEYDOWN filtering do not stop it. Raw input detours only fake devices/block input while the Steam overlay is up; DirectInput only if the game makes a dinput keyboard (Unity does not). Steam reinstalls its hook only if it stops being called while DispatchMessage still is (500 ms) or the window changes
- mod: ui/overlay.cpp `SyncSteamKeyBlock`: own WH_GETMESSAGE hook (newest on the thread, re-installed every 2 s) turns msg 0x14FC lParam 1 into WM_NULL; real Tab WM_KEYDOWN queued for ImGui and nulled. Re-check 0xADF10 after Steam updates

---
