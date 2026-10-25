# Hook inventory

Every hook the mod installs (MinHook detours plus the two Win32 hooks of the overlay). SR2 1.3.0,
RVAs from `re/dump/cpp2il_diffable-cs_rva` (GameAssembly.dll unless noted).

**One hook per address.** MinHook allows a single hook per target, and `HookManager::Create`
(core/hook_manager.cpp) refuses a target that is already hooked ("target X already hooked by 'name'"),
so the second module's Setup fails. IL2CPP also folds identical bodies: two different methods can
share one address (see `re/unity.md` "Folded bodies"; `python re/re_lookup.py --rva <hex>`), so compare RVAs, not names.

**Adding a hook**
1. Look the target up here first, by class and by RVA. If it is taken, plug into the owner (see Notes)
   instead of hooking again. Check the dump for other methods at the same RVA (folded bodies).
2. In a module: `ctx.HookMethod("Class.Method", image, ns, klass, method, argc, &Detour, &orig)` or
   `ctx.HookAddress("Class.Method", ptr, ...)` (core/module_manager.cpp). The name becomes
   `<module>.<name>`; hooks are created disabled and enabled/disabled with the module.
   Hooks that must stay on while the module is off use `HookManager::Get().Create(<own group>, ...)` +
   `Enable` (see sandbox_* groups below).
3. Wrap the detour's mod logic (never the call to the original) in a `DetourGuard`
   (core/detour_guard.h); a native fault then turns the module's detours into pass-throughs.
4. Add a row to the table below.

Hook groups (HookManager "module"): the module name for `ctx.Hook*` and `ctx.hooks().Create(kModule, ...)`;
`core`, `ui`, `sandbox_achievements`, `sandbox_saves`, `sandbox_fabricator` are always on once created.

## By target

`opt` = optional (missing only degrades a feature). Hook names are as registered.

| Target | RVA | Hook name | Module / file | Why |
|---|---|---|---|---|
| AccessDoorUI.PromptForExpansion(1) | 0x84B9D0 | sell_upgrades.AccessDoorUI.PromptForExpansion | sell_upgrades/module.cpp | expansion popup button order |
| AchievementData.IncrementProgress(1) (nested in MetaGameDirector) | 0x5FE9C0 | sandbox_achievements.AchievementData.IncrementProgress | sandbox/achievements.cpp | block achievements (always on) |
| AmmoModel.Pull(1) | 0xA6E510 **folded** | custom_upgrades.AmmoModel.Pull | custom_upgrades/vacpack_upgrades.cpp | strip shadow slimes from save; body shared with TrackedActorsModel.Pull, filters player's model |
| AmmoModel.Push(1) | 0xA6E530 | custom_upgrades.AmmoModel.Push | custom_upgrades/vacpack_upgrades.cpp | restore extra slots / shadows on load |
| AmmoSlotDefinition.IsAllowed(1) | 0x866B40 | custom_upgrades.AmmoSlotDefinition.IsAllowed | custom_upgrades/shadow_catcher.cpp | allow shadow slimes in vac slots |
| AmmoSlotViewHolder.BindInputKey(1) | 0x67F160 | custom_upgrades.AmmoSlotViewHolder.BindInputKey | custom_upgrades/vacpack_upgrades.cpp | HUD key label for extra slots (opt) |
| AutoSaveDirector.BeginLoad(2) | 0x5CDB50 | sandbox_saves.AutoSaveDirector.BeginLoad | sandbox/cheated_save.cpp | track cheated save on load (always on) |
| AutoSaveDirector.CleanupAutosaves(1) | 0x5CE040 | sandbox_saves.AutoSaveDirector.CleanupAutosaves | sandbox/cheated_save.cpp | keep cheated marks in step (always on) |
| AutoSaveDirector.Load(3) | 0x5CFDC0 | custom_upgrades.AutoSaveDirector.Load | custom_upgrades/module.cpp | load sidecar |
| AutoSaveDirector.SaveGameImpl(2) | 0x5D10A0 | custom_upgrades.AutoSaveDirector.SaveGameImpl | custom_upgrades/module.cpp | write sidecar |
| AutoSaveDirector.StartNewGame(2) | 0x5D1A70 | custom_upgrades.AutoSaveDirector.StartNewGame | custom_upgrades/module.cpp | reset sidecar state |
| Button.Press(0) (UnityEngine.UI) | 0x35A0730 | sell_upgrades.UnityEngine.UI.Button.Press | sell_upgrades/module.cpp | Sell button click; every uGUI button, pointer compare |
| Canvas.SendWillRenderCanvases(0) (UIModule, static) | 0x37BF0A0 | core.main_thread.Canvas.SendWillRenderCanvases | core/main_thread.cpp | main-thread tick (secondary; only with frameCount or if primary failed) |
| CategoryTabViewHolder.BindData(1) | 0x832F90 | ui.options_tab_bind | ui/options_tab.cpp | Mods tab title |
| CompanyLogoScene.Start(0) | 0x688500 | quick_start.CompanyLogoScene.Start | quick_start/module.cpp | skip intro logos |
| ConfirmPopup.IDisplayShowDataHandler<ConfirmPopupSetup>.UpdateShowData(1) | 0x6D09D0 | sell_upgrades.ConfirmPopup.UpdateShowData | sell_upgrades/module.cpp | expansion popup order; explicit interface impl, found by name suffix |
| ConfirmPopup.<>c.<...UpdateShowData>b__27_0(2) | 0x6E5960 | sell_upgrades.ConfirmPopup.<UpdateShowData>b__27_0 | sell_upgrades/module.cpp | prompt sort comparison; nested <>c, by suffix (6-byte body) |
| DeathObscurer.StartObscuring(3) | 0x8670C0 | quick_travel.DeathObscurer.StartObscuring | quick_travel/module.cpp | shorten death fade |
| DetailsMoreButtonView.BindData(1) | 0x839F00 | sell_upgrades.DetailsMoreButtonView.BindData | sell_upgrades/module.cpp | Sell button on plot card |
| DroneProgramMenu.HandleActivateDrone(1) | 0x81AA20 | drone_price_limit.DroneProgramMenu.HandleActivateDrone | drone_price_limit/station_dialog.cpp | station menu dialog / programs |
| DroneProgramMenu.HandleClearAllOptions(1) | 0x81AF90 | drone_price_limit.DroneProgramMenu.HandleClearAllOptions | drone_price_limit/station_dialog.cpp | station menu dialog / programs |
| DroneProgramMenu.SetViewModel(1) | 0x81BE10 | drone_price_limit.DroneProgramMenu.SetViewModel | drone_price_limit/station_dialog.cpp | inject Price limit / Programs actions |
| DroneStationGadgetModel.Pull(9) | 0xA8D550 | custom_upgrades.DroneStationGadgetModel.Pull | custom_upgrades/drone_source.cpp | vanilla-safe save of custom sources / program 1 |
| DroneUIDefinitionSource.FindSourceDefinition(1) | 0x83ABF0 | custom_upgrades.DroneUIDefinitionSource.FindSourceDefinition | custom_upgrades/drone_source.cpp | UI definition of custom sources |
| EventSystem.Update(0) (UnityEngine.UI) | 0x37F3FB0 | core.main_thread.EventSystem.Update | core/main_thread.cpp | main-thread tick (primary) |
| FabricatorConfirmPopup.HideForConfirm(0) | 0x800A50 | misc_tweaks.FabricatorConfirmPopup.HideForConfirm | misc_tweaks/module.cpp | skip upgrade animation |
| FabricatorItemButton.ConfigurePips(2) | 0x806CE0 | custom_upgrades.FabricatorItemButton.ConfigurePips | custom_upgrades/fabricator_levels.cpp | more level pips (opt) |
| FabricatorItemDetails.SetItem(1) | 0x8089B0 | custom_upgrades.FabricatorItemDetails.SetItem | custom_upgrades/gadget_look.cpp | card picture of mod gadgets |
| FabricatorUI.OnDestroy(0) | 0x8263D0 | sandbox_fabricator.FabricatorUI.OnDestroy | sandbox/module.cpp | Fabricator key: open Fabricator tracking (always on) |
| FabricatorUI.OnItemActivated(1) | 0x826560 | misc_tweaks.FabricatorUI.OnItemActivated | misc_tweaks/module.cpp | fast Fabricator purchases |
| FabricatorUI.Start(0) | 0x826D30 | sandbox_fabricator.FabricatorUI.Start | sandbox/module.cpp | Fabricator key: open Fabricator tracking (always on) |
| FileStorageProvider.StoreGameData(3) | 0x744BC0 | custom_upgrades.FileStorageProvider.StoreGameData | custom_upgrades/module.cpp | exact .sav name for per-save sidecar (opt) |
| FocusManager.NavigateInDirection(3) (static) | 0x7926D0 | drone_price_limit.FocusManager.NavigateInDirection | drone_price_limit/station_dialog.cpp | dialog navigation; every menu passes through |
| FocusManager.SendCancelActionToFocus(1) (static) | 0x7939A0 | drone_price_limit.FocusManager.SendCancelActionToFocus | drone_price_limit/station_dialog.cpp | dialog cancel |
| FocusManager.SendSubmitActionToFocus(1) (static) | 0x793BE0 | drone_price_limit.FocusManager.SendSubmitActionToFocus | drone_price_limit/station_dialog.cpp | dialog submit |
| FogMapElement.SetRevealState(1) | 0x704B20 | map_tools.FogMapElement.SetRevealState | map_tools/module.cpp | reveal map (opt) |
| Gadget.ChangeVariant(2) | 0x651CD0 | custom_upgrades.Gadget.ChangeVariant | custom_upgrades/quantum_link.cpp | variant toggle of placed clones (variant_ext) |
| Gadget.IsCompletelyGrounded(2) | 0x6530D0 | free_placement.Gadget.IsCompletelyGrounded | free_placement/module.cpp | place anywhere |
| Gadget.IsOverlapping(float, LayerMask, bool) | 0x653270 | free_placement.Gadget.IsOverlapping | free_placement/module.cpp | ignore overlap; MethodByParams (ref overload 0x653330 not hooked) |
| Gadget.Update(0) | 0x654E60 | instant_build.Gadget.Update | instant_build/module.cpp | skip gadget cooldowns/charge-up |
| GadgetDefinition.get_LocalizedDescription(0) | 0x51B440 | custom_upgrades.GadgetDefinition.get_LocalizedDescription | custom_upgrades/gadget_look.cpp | own description for clones (any thread) |
| GadgetDirector.GetItemCount(1) | 0x8688C0 | custom_upgrades.GadgetDirector.GetItemCount | custom_upgrades/quantum_link.cpp | clone item counts (variant_ext) |
| GadgetDirector.GetTargetGadgetInfo(4) | 0x868E80 | custom_upgrades.GadgetDirector.GetTargetGadgetInfo | custom_upgrades/gadget_look.cpp | gadget-mode label |
| GadgetDirector.HasBlueprint(1) | 0x869360 | custom_upgrades.GadgetDirector.HasBlueprint | custom_upgrades/quantum_link.cpp | clone blueprints (variant_ext) |
| GadgetDirectorHelper.IFabricatorGadgetHelper.HasBlueprint(1) | 0x827B50 | custom_upgrades.GadgetDirectorHelper.HasBlueprint | custom_upgrades/quantum_link.cpp | Fabricator card unlocked; explicit interface, by suffix |
| GadgetFabricatableItem.IFabricatableItem.FabricateAndSpendCost(1) | 0x827E20 | custom_upgrades.GadgetFabricatableItem.FabricateAndSpendCost | custom_upgrades/quantum_link.cpp | fabricate clones; explicit interface, by suffix |
| GadgetFabricatableItem.get_ShouldShowPlaceholder(0) | 0x828470 | custom_upgrades.GadgetFabricatableItem.get_ShouldShowPlaceholder | custom_upgrades/quantum_link.cpp | no placeholder for clone cards |
| GadgetFabricateCategoryRuntime..ctor(3) | 0x8287D0 | custom_upgrades.GadgetFabricateCategoryRuntime..ctor | custom_upgrades/quantum_link.cpp | insert Fabricator cards (GetItems inlined) |
| GadgetFavoritesBar.NextSlot(1) | 0x75E180 | hl2_movement.GadgetFavoritesBar.NextSlot | hl2_movement/module.cpp | scroll-wheel jump (opt) |
| GadgetFavoritesBar.PreviousSlot(1) | 0x75E500 | hl2_movement.GadgetFavoritesBar.PreviousSlot | hl2_movement/module.cpp | scroll-wheel jump (opt) |
| GadgetItem.IsInLinkedGadgetRange(1) | 0xA43770 | free_placement.GadgetItem.IsInLinkedGadgetRange | free_placement/module.cpp | unlimited link range |
| GadgetItem.PickupGadget(1) | 0xA44AD0 | custom_upgrades.GadgetItem.PickupGadget | custom_upgrades/quantum_link.cpp | pick up clones (variant_ext) |
| GadgetItem.PlaceGadgetEvent(1) | 0xA45410 | custom_upgrades.GadgetItem.PlaceGadgetEvent | custom_upgrades/quantum_link.cpp | place clones (variant_ext) |
| GadgetItem.SetHeldGadget(1) | 0xA46810 | custom_upgrades.GadgetItem.SetHeldGadget | custom_upgrades/gadget_look.cpp | placement preview look |
| GadgetItem.UpdateFootprint(0) | 0xA48050 | free_placement.GadgetItem.UpdateFootprint | free_placement/module.cpp | placement validity (scope for the other free_placement detours) |
| GadgetMusicPlayer.OnInteract(0) | 0x747930 | custom_upgrades.dj.GadgetMusicPlayer.OnInteract | custom_upgrades/dj_buildables.cpp | E on DJ booths / speakers |
| GadgetMusicPlayer.ShowDeactivatePrompt(0) | 0x748180 | custom_upgrades.dj.GadgetMusicPlayer.ShowDeactivatePrompt | custom_upgrades/dj_buildables.cpp | DJ prompt |
| HudUI.Awake(0) | 0x698620 | minimap.HudUI.Awake | minimap/module.cpp | record HUD instance (opt) |
| IDXGISwapChain::Present (vtable slot 8) | dxgi.dll | ui.dxgi_present | ui/overlay.cpp | ImGui overlay render (vtable read from a dummy swap chain) |
| IDXGISwapChain::ResizeBuffers (slot 13) | dxgi.dll | ui.dxgi_resize_buffers | ui/overlay.cpp | release RTV before resize (required) |
| IDXGISwapChain1::Present1 (slot 22) | dxgi.dll | ui.dxgi_present1 | ui/overlay.cpp | overlay render (opt; skipped if same as Present) |
| IDXGISwapChain3::ResizeBuffers1 (slot 39) | dxgi.dll | ui.dxgi_resize_buffers1 | ui/overlay.cpp | release RTV (opt) |
| Incinerate.ProcessIncinerateResults(2) | 0x988360 | plot_upgrades.Incinerate.ProcessIncinerateResults2 | plot_upgrades/module.cpp | Ash Booster |
| Incinerate.ProcessIncinerateResults(4) | 0x988130 | plot_upgrades.Incinerate.ProcessIncinerateResults4 | plot_upgrades/module.cpp | Ash Booster |
| InputEvent.Cancel(1) | 0x9DF990 | hl2_movement.InputEvent.Cancel | hl2_movement/module.cpp | jump/sprint hold tracking (opt; InputEventData layout checked) |
| InputEvent.StartInput(1) | 0x9DFDE0 | hl2_movement.InputEvent.StartInput | hl2_movement/module.cpp | jump/sprint hold tracking (opt) |
| IntroSequenceUIRoot.Start(0) | 0x7254C0 | quick_start.IntroSequenceUIRoot.Start | quick_start/module.cpp | skip new-game intro |
| IntroSequenceUIRoot.StartFrame(1) | 0x725250 | quick_start.IntroSequenceUIRoot.StartFrame | quick_start/module.cpp | skip new-game intro |
| JumpAbilityBehavior.Start(0) | 0xA78070 | hl2_movement.JumpAbilityBehavior.Start | hl2_movement/module.cpp | jump queue |
| JumpAbilityBehavior.UpdateVelocity(2) | 0xA782E0 | hl2_movement.JumpAbilityBehavior.UpdateVelocity | hl2_movement/module.cpp | Source-style jump |
| KFCCharacterController.ClampAirborneHorizontalVelocity(2) | 0x5BBD40 | hl2_movement.ClampAirborneHorizontalVelocity | hl2_movement/module.cpp | air strafing |
| KFCCharacterController.HandleAirborneDrag(3) | 0x5BC310 | hl2_movement.HandleAirborneDrag | hl2_movement/module.cpp | air movement |
| KFCCharacterController.HandleGravity(2) | 0x5BC3D0 | hl2_movement.HandleGravity | hl2_movement/module.cpp | gravity |
| KFCCharacterController.HandleHorizontalAirborneMovement(3) | 0x5BC840 | hl2_movement.HandleHorizontalAirborneMovement | hl2_movement/module.cpp | air acceleration |
| KFCCharacterController.HandleStablyGroundedDrag(2) | 0x5BCCA0 | hl2_movement.HandleStablyGroundedDrag | hl2_movement/module.cpp | ground friction |
| KFCCharacterController.HandleStablyGroundedVelocity(2) | 0x5BCCF0 | hl2_movement.HandleStablyGroundedVelocity | hl2_movement/module.cpp | ground acceleration |
| KFCCharacterController.UpdateVelocity(2) | 0x5BEF50 | hl2_movement.UpdateVelocity | hl2_movement/module.cpp | movement frame entry |
| KinematicCharacterMotor.HandleVelocityProjection(3) | 0x599AB0 | hl2_movement.KinematicCharacterMotor.HandleVelocityProjection | hl2_movement/module.cpp | ramp clip (opt); every KCC motor, filters player |
| LandPlot.AddUpgrade(1) | 0x988750 | custom_upgrades.LandPlot.AddUpgrade | custom_upgrades/module.cpp | custom plot tiers |
| LandPlot.HasUpgrade(1) | 0x989190 | custom_upgrades.LandPlot.HasUpgrade | custom_upgrades/module.cpp | custom plot tiers |
| LandPlotLocation.Replace(2) | 0x871E60 | custom_upgrades.LandPlotLocation.Replace | custom_upgrades/module.cpp | drop tiers on demolish/swap (opt) |
| LandPlotUIRoot.<>c__DisplayClass6_0.<PromptForUpgrade>b__0(0) | 0x702E70 | sell_upgrades.LandPlotUIRoot.PromptForUpgrade_b__0 | sell_upgrades/module.cpp | purchase confirm (closure class) |
| LandPlotUIRoot.OnAction(1) | 0x6E6F50 | sell_upgrades.LandPlotUIRoot.OnAction | sell_upgrades/module.cpp | sell on action |
| LandPlotUIRoot.PromptForPlot(1) | 0x6E77D0 | sell_upgrades.LandPlotUIRoot.PromptForPlot | sell_upgrades/module.cpp | plot purchase popup order (opt) |
| LandPlotUIRoot.SetActivator(1) | 0x6E8110 | sell_upgrades.LandPlotUIRoot.SetActivator | sell_upgrades/module.cpp | Sell button (the override itself) |
| LandPlotUIRoot.UpdateMenuItemAvailabilityAndHidden(0) | 0x6E8150 | custom_upgrades.LandPlotUIRoot.UpdateMenuItemAvailabilityAndHidden | custom_upgrades/module.cpp | custom tiers in plot menu (the override itself) |
| ListUIRoot`4.OnAltAction(1) | 0x11AF260 **shared generic** | sell_upgrades.ListUIRoot.OnAltAction | sell_upgrades/module.cpp | alt-action sell; LandPlotUIRoot only, by class |
| ListUIRoot`4.OpenPopupPrompt(7) | 0x11AF570 **shared generic** | sell_upgrades.ListUIRoot.OpenPopupPrompt | sell_upgrades/module.cpp | popup button order (opt); LandPlotUIRoot only |
| ListUIRoot`4.RebuildUI(0) | 0x11AFB30 **shared generic** | custom_upgrades.ListUIRoot.RebuildUI | custom_upgrades/module.cpp | refresh plot menu after buy/sell (opt); every list menu |
| ListUIRoot`4.ShowAltActionButton(1) | 0x11AFE20 **shared generic** | sell_upgrades.ListUIRoot.ShowAltActionButton | sell_upgrades/module.cpp | alt-action sell; LandPlotUIRoot only |
| LoadingScreenView.FadeToBlack(0) | 0x7260E0 | quick_travel.LoadingScreenView.FadeToBlack | quick_travel/module.cpp | shorten loading fade (opt) |
| LocalizeStringEvent.UpdateString(1) (Unity.Localization) | 0x2DEAFA0 | ui.options_lse_update_string | ui/options_tab.cpp | text overrides for injected rows |
| MainMenuLandingRootUI.OnEnable(0) | 0x7284B0 | ui.main_menu_on_enable | ui/modded_label.cpp | [modded] label + quick_start listener (shared, see Notes) |
| MapClickPlaceMarker.IPointerClickHandler.OnPointerClick(1) | 0x706AB0 | map_tools.MapClickPlaceMarker.OnPointerClick | map_tools/module.cpp | map click teleport (opt); explicit interface name |
| MapDirector.IsZoneHidden(1) | 0x9C2030 | map_tools.MapDirector.IsZoneHidden | map_tools/module.cpp | show zone names (opt) |
| MapListConfiguration.<>c__DisplayClass1_0.<GetAvailableMaps>b__0(1) | 0x9D1FC0 | map_tools.MapListConfiguration.GetAvailableMaps filter | map_tools/module.cpp | all map tabs (opt; name has a space) |
| MapUI.Awake(0) | 0x710890 | minimap.MapUI.Awake | minimap/module.cpp | record map page (opt) |
| MapUI.LateUpdate(0) | 0x711E90 | map_tools.MapUI.LateUpdate | map_tools/module.cpp | open-map tick (required) |
| MetaGameDirector.HandleSoldPlort(1) | 0x5F4980 | sandbox_achievements.MetaGameDirector.HandleSoldPlort | sandbox/achievements.cpp | block achievements (always on) |
| MetaGameDirector.SetAchievementProgress(Achievement, int) | 0x5F56D0 | sandbox_achievements.MetaGameDirector.SetAchievementProgress | sandbox/achievements.cpp | block achievements; overload by first param (AchievementData overload 0x5F5650 not hooked) |
| MusicBoxRegion.OnTriggerEnter(1) | 0x98A580 | plot_upgrades.MusicBoxRegion.OnTriggerEnter | plot_upgrades/module.cpp | music box booster |
| MusicBoxUpgrader.Apply(1) | 0x98A810 | slime_dj.MusicBoxUpgrader.Apply | slime_dj/module.cpp | track music boxes (opt) |
| OptionsDetailsView.BindData(1) | 0x7183E0 | ui.options_details_bind | ui/options_tab.cpp | Mods tab details panel |
| OptionsItemViewHolder.Activate(0) | 0x718640 | ui.options_row_activate | ui/options_tab.cpp | click toggles row (opt; skipped if folded) |
| OptionsItemViewHolder.BindData(1) | 0x718690 | ui.options_row_bind | ui/options_tab.cpp | injected rows |
| OptionsPresetSelector.OnSelectionChanged(1) | 0x7198A0 | ui.options_selector_changed | ui/options_tab.cpp | injected row value change |
| OptionsUIRoot.BindCategories(2) | 0x71A010 | ui.options_bind_categories | ui/options_tab.cpp | add Mods tab |
| OptionsUIRoot.BindItemCategory(1) | 0x71A2F0 | ui.options_bind_item_category | ui/options_tab.cpp | empty-tab details text (opt) |
| OptionsViewModel.GetOptionsItemsForCategory(3) | 0x71E0E0 | ui.options_get_items | ui/options_tab.cpp | rows of Mods tab |
| PlatformEngagementPrompt.Onloaded(0) | 0x7326E0 | quick_start.PlatformEngagementPrompt.Onloaded | quick_start/module.cpp | skip press-any-key |
| PlayerDeathHandler.OnDeath(3) | 0x879310 | quick_travel.PlayerDeathHandler.OnDeath | quick_travel/module.cpp | fast respawn |
| PlayerDisplayOnMap.Awake(0) | 0x52D970 | minimap.PlayerDisplayOnMap.Awake | minimap/module.cpp | record player marker (opt) |
| PlayerItemController.BlockGadgetMode(1) | 0xA5EE90 | free_placement.PlayerItemController.BlockGadgetMode | free_placement/module.cpp | gadget mode anywhere |
| PlayerItemController.ToggleItem(1) | 0xA5F650 | free_placement.PlayerItemController.ToggleItem | free_placement/module.cpp | gadget mode anywhere |
| PlayerState.Damage(1) | 0x87AEB0 | sandbox.PlayerState.Damage | sandbox/player_extras.cpp | infinite health (opt, toggles with sandbox) |
| PlayerState.InitModel(1) | 0x87B4F0 | custom_upgrades.PlayerState.InitModel | custom_upgrades/vacpack_upgrades.cpp | apply vacpack tiers |
| PlayerState.Reset(1) | 0x87BC70 | custom_upgrades.PlayerState.Reset | custom_upgrades/vacpack_upgrades.cpp | apply vacpack tiers |
| PlayerUpgradeFabricateCategoryRuntime..ctor(2) | 0x82BE90 | custom_upgrades.PlayerUpgradeFabricateCategoryRuntime..ctor | custom_upgrades/vacpack_upgrades.cpp | add upgrade cards (own ctor, no parent walk) |
| PlortCollectorActivator.Activate(0) | 0x52DD60 | custom_upgrades.PlortCollectorActivator.Activate | custom_upgrades/module.cpp | silo Sell-All + Hen Collector buttons (silo_world, coop_world) (opt) |
| ProducePlortsOnHit.ProducePlort(0) | 0x885CA0 | custom_upgrades.ProducePlortsOnHit.ProducePlort | custom_upgrades/shadow_catcher.cpp | lift hit-plort despawn limit (opt) |
| PurchaseItemDetailsContentView.BindData(1) | 0x6D9EA0 | custom_upgrades.PurchaseItemDetailsContentView.BindData | custom_upgrades/module.cpp | Hen Collector limit sliders (opt; the class's own override) |
| RanchDrone.GatherSourceTask(1) | 0xA19B40 | custom_upgrades.RanchDrone.GatherSourceTask | custom_upgrades/drone_source.cpp | custom drone sources |
| RanchDrone.GatherSourceTasks(1) | 0xA1AA90 | custom_upgrades.RanchDrone.GatherSourceTasks | custom_upgrades/drone_source.cpp | custom drone sources |
| RanchDroneBehaviourDumpAmmo.Relevancy(1) | 0xA22CD0 | drone_price_limit.RanchDroneBehaviourDumpAmmo.Relevancy | drone_price_limit/module.cpp | hold plorts below limit |
| RanchDroneSinkProgramPlortMarket.<GetSinks>d__16.MoveNext(0) | 0xA356A0 | drone_price_limit.RanchDroneSinkProgramPlortMarket.<GetSinks>.MoveNext | drone_price_limit/module.cpp | price limit; nested iterator found by prefix |
| RanchDroneSourceProgramElderCollector.GetSources(1) | 0xA31830 | custom_upgrades.RanchDroneSourceProgramElderCollector.GetSources | custom_upgrades/drone_source.cpp | custom drone sources |
| RanchHouseUI.Sleep(3) | 0x6A4890 | quick_travel.RanchHouseUI.Sleep | quick_travel/module.cpp | fast sleep |
| RaycastHit.get_normal(0) (PhysicsModule) | 0x339E700 **folded** | free_placement.RaycastHit.get_normal | free_placement/module.cpp | upside-down placement; body shared with Ray.get_direction, Bounds.get_extents, BoundsInt.get_size, ContactPoint/ParticleCollisionEvent.get_normal |
| Reproduce.ReproducePeriod(0) | 0x536F60 | plot_upgrades.Reproduce.ReproducePeriod | plot_upgrades/module.cpp | coop booster |
| ResourceNode.SpawnResources(0) | 0x53B990 | spawn_scaling.ResourceNode.SpawnResources | spawn_scaling/module.cpp | rescale node yields (opt) |
| ResourceNode.SpawnSingleResource(0) | 0x53BA90 | spawn_scaling.ResourceNode.SpawnSingleResource | spawn_scaling/module.cpp | harvest pacing (opt) |
| ResourceNodeDirector.Awake(0) | 0x66B350 | spawn_scaling.ResourceNodeDirector.Awake | spawn_scaling/module.cpp | patch node config |
| ResourceSpawnerDefinition.GetResourcesToSpawn(1) | 0x53C5C0 | spawn_scaling.ResourceSpawnerDefinition.GetResourcesToSpawn | spawn_scaling/module.cpp | scale yields |
| SaveGamesRootUI.FetchGameSummaryItemData(1) | 0x7351A0 | sandbox_saves.SaveGamesRootUI.FetchGameSummaryItemData | sandbox/cheated_save.cpp | "Sandbox: Used" row (opt, always on) |
| SaveReferenceTranslation.GetPersistenceId(IdentifiableType) | 0x60DDC0 | custom_upgrades.SaveReferenceTranslation.GetPersistenceId(IdentifiableType) | custom_upgrades/quantum_link.cpp | save clones as vanilla ids (variant_ext SaveAs); MethodByParams |
| SceneLoader.EnqueueDelaySeconds(2) | 0x8D8200 | quick_travel.SceneLoader.EnqueueDelaySeconds | quick_travel/module.cpp | cut load delays |
| SceneLoader.EnqueueLoadDeathLoadingScene(0) | 0x8D82D0 | quick_travel.SceneLoader.EnqueueLoadDeathLoadingScene | quick_travel/module.cpp | fast respawn |
| SceneLoader.EnqueueUnloadDeathLoadingScene(0) | 0x8D98A0 | quick_travel.SceneLoader.EnqueueUnloadDeathLoadingScene | quick_travel/module.cpp | fast respawn |
| SceneLoader.LoadSceneGroup(2) | 0x8DB0F0 | quick_travel.SceneLoader.LoadSceneGroup | quick_travel/module.cpp | track scene loads |
| ScorePlort.CanDroneDeposit(2) | 0x991E40 | drone_price_limit.ScorePlort.CanDroneDeposit | drone_price_limit/module.cpp | price limit |
| ScriptedValueRangeOptionDefinition.ApplyPresetSelection(1) | 0x9D0030 | ui.options_range_apply | ui/options_tab.cpp | injected slider rows |
| ShadowSlimeScatter.Action(0) | 0x8A12D0 | custom_upgrades.ShadowSlimeScatter.Action | custom_upgrades/shadow_catcher.cpp | kept shadows don't scatter |
| ShadowSlimeScatter.DoAlert(1) | 0x8A1890 | custom_upgrades.ShadowSlimeScatter.DoAlert | custom_upgrades/shadow_catcher.cpp | kept shadows don't scatter |
| ShadowSlimeScatter.Relevancy(1) | 0x8A2120 | custom_upgrades.ShadowSlimeScatter.Relevancy | custom_upgrades/shadow_catcher.cpp | kept shadows don't scatter |
| ShadowSlimeScatter.ScatterIfPlayerNear(0) | 0x8A2280 | custom_upgrades.ShadowSlimeScatter.ScatterIfPlayerNear | custom_upgrades/shadow_catcher.cpp | kept shadows don't scatter |
| ShopCategoryRuntime.GetRuntimeItems(1) | 0x8B37C0 | custom_upgrades.radio_shop.ShopCategoryRuntime.GetRuntimeItems | custom_upgrades/radio_shop.cpp | Slime Radio shop entry |
| ShopDirector.SpawnNextItem(5) | 0x8B9590 | custom_upgrades.radio_shop.ShopDirector.SpawnNextItem | custom_upgrades/radio_shop.cpp | Slime Radio shop entry |
| ShopRuntimeItem.get_Asset(0) | 0x8C1E20 | custom_upgrades.radio_shop.ShopRuntimeItem.get_Asset | custom_upgrades/radio_shop.cpp | Slime Radio shop entry |
| ShopRuntimeItem.get_Description(0) | 0x8C1EC0 | custom_upgrades.radio_shop.ShopRuntimeItem.get_Description | custom_upgrades/radio_shop.cpp | Slime Radio shop entry |
| ShopRuntimeItem.TryPurchase(3) | 0x8C1040 | custom_upgrades.radio_shop.ShopRuntimeItem.TryPurchase | custom_upgrades/radio_shop.cpp | Slime Radio shop entry |
| SlimeEmotions.ComputeAgitationChangePerHour(0) | 0x88ABA0 | plot_upgrades.SlimeEmotions.ComputeAgitationChangePerHour | plot_upgrades/module.cpp | music box booster |
| SpawnResource.UpdateToTime(3) | 0xBC2C00 | plot_upgrades.SpawnResource.UpdateToTime | plot_upgrades/module.cpp | garden booster |
| SpawnTemporaryObjectActivity.Spawn(2) | 0x66DB30 | spawn_scaling.SpawnTemporaryObjectActivity.Spawn | spawn_scaling/module.cpp | weather patch-on-touch (opt) |
| SpawnTornadoActivity.Spawn(2) | 0x66DBF0 | spawn_scaling.SpawnTornadoActivity.Spawn | spawn_scaling/module.cpp | weather patch-on-touch (opt) |
| SprintAbilityBehavior.Update(1) | 0xA84A90 | hl2_movement.SprintAbilityBehavior.Update | hl2_movement/module.cpp | HL2 sprint |
| StandaloneEngagementPrompt.OnInteract(1) | 0x736630 | quick_start.StandaloneEngagementPrompt.OnInteract | quick_start/module.cpp | skip press-any-key |
| StaticTornadoSpawner.OnEnable(0) | 0x640BF0 | spawn_scaling.StaticTornadoSpawner.OnEnable | spawn_scaling/module.cpp | weather patch-on-touch (opt) |
| SteamMetaGamePlatform.Grant(1) | 0xB30F00 | sandbox_achievements.SteamMetaGamePlatform.Grant | sandbox/achievements.cpp | block Steam grant (always on) |
| StorageMetadata.Decrement(1) (in RanchMetadata.cs) | 0x9969C0 | custom_upgrades.StorageMetadata.Decrement | custom_upgrades/quantum_link.cpp | quantum storage takes |
| TeleportNetwork.Teleport_Death(3) | 0x65D1C0 | quick_travel.TeleportNetwork.Teleport_Death | quick_travel/module.cpp | fast respawn |
| TeleportNetwork.Teleport_ResetPlayer(2) | 0x65D580 | quick_travel.TeleportNetwork.Teleport_ResetPlayer | quick_travel/module.cpp | fast respawn |
| TeleportNetwork.TeleportToDestinationImpl(5) | 0x65CD90 | quick_travel.TeleportNetwork.TeleportToDestinationImpl | quick_travel/module.cpp | fast teleports (map_tools only calls it) |
| ToggleOptionDefinition.ApplyPresetSelection(1) | 0x9D0D90 | ui.options_toggle_apply | ui/options_tab.cpp | injected toggle rows |
| TreasurePod.Awake(0) | 0x553550 | minimap.TreasurePod.Awake | minimap/module.cpp | pod icons (opt, pair) |
| TreasurePod.OnDestroy(0) | 0x5538F0 | minimap.TreasurePod.OnDestroy | minimap/module.cpp | pod icons (opt, pair) |
| UIActivator.OnInteract(0) | 0x6ACB60 | custom_upgrades.UIActivator.OnInteract | custom_upgrades/quantum_link.cpp | Use on Quantum Storage Link; base virtual, filters own objects |
| UIDisplayInteractable.OnInteract(0) | 0x6B0AD0 | custom_upgrades.UIDisplayInteractable.OnInteract | custom_upgrades/quantum_link.cpp | Use on Quantum Storage Link; filters own objects |
| UITemplates.CreatePopupPrompt(5) | 0x6B1910 | sell_upgrades.UITemplates.CreatePopupPrompt | sell_upgrades/module.cpp | plot purchase popup order (opt) |
| UpgradeChain.Show(2) | 0x82ED90 | custom_upgrades.UpgradeChain.Show | custom_upgrades/fabricator_levels.cpp | 12 level circles (Tank Booster IX-XII) |
| UpgradeModel.CountUniqueUpgrades(0) | 0xAD92B0 | custom_upgrades.UpgradeModel.CountUniqueUpgrades | custom_upgrades/vacpack_upgrades.cpp | don't count custom tiers |
| UpgradeModel.Pull(1) | 0xAD99C0 | custom_upgrades.UpgradeModel.Pull | custom_upgrades/vacpack_upgrades.cpp | clamp save to vanilla max |
| UpgradeModel.SetUpgradeLevel(2) | 0xAD9C20 | custom_upgrades.UpgradeModel.SetUpgradeLevel | custom_upgrades/vacpack_upgrades.cpp | custom tiers + sidecar |
| VacuumItem.Expel(float) | 0xA66C00 | misc_tweaks.VacuumItem.Expel | misc_tweaks/module.cpp | shot volume floor (opt); MethodByParams (private overload) |
| VacuumItem.GetShootSpeedFactor(1) | 0xA67940 | misc_tweaks.VacuumItem.GetShootSpeedFactor | misc_tweaks/module.cpp | shoot speed |
| VacuumItem.NextSlot(1) | 0xA67D00 | hl2_movement.VacuumItem.NextSlot | hl2_movement/module.cpp | scroll-wheel jump (opt) |
| VacuumItem.PreviousSlot(1) | 0xA68F80 | hl2_movement.VacuumItem.PreviousSlot | hl2_movement/module.cpp | scroll-wheel jump (opt) |
| VacuumItem.SelectSlot(InputEventData) | 0xA69640 | custom_upgrades.VacuumItem.SelectSlot | custom_upgrades/vacpack_upgrades.cpp | remap water slot key (opt); MethodByParams (int overload only called) |
| Win32 WndProc of the game window | user32 | (no HookManager entry) | ui/overlay.cpp | `SetWindowLongPtrW(GWLP_WNDPROC)` subclass: overlay input |
| Win32 WH_GETMESSAGE hook (window thread) | user32 | (no HookManager entry) | ui/overlay.cpp | `SetWindowsHookExW` while overlay has input: block Steam Shift+Tab, feed Tab to ImGui; refreshed every 2 s |

Count: 177 MinHook hooks (core 2, ui 16, custom_upgrades 58, hl2_movement 17, sell_upgrades 13,
quick_travel 11, sandbox 10 in four groups, drone_price_limit 9, free_placement 7, spawn_scaling 7,
plot_upgrades 6, map_tools 5, minimap 5, quick_start 5, misc_tweaks 4, instant_build 1, slime_dj 1)
plus the two Win32 hooks. No two hooks share a target. Modules without hooks: collectibles,
debug_tools, discord_rpc, wall_markers (reads the overlay's Present), proxy/.

## Notes

**Shared-hook owners (plug in, do not hook again)**
- `MainMenuLandingRootUI.OnEnable`: owned by ui/modded_label.cpp (always on, group `ui`). Others register
  one listener with `sr2::ui::modded_label::SetMenuEnableListener` (quick_start auto_continue does).
  The listener runs after the original, outside the label's guard: wrap it in your own DetourGuard.
- Quantum Storage Link hooks (quantum_link.cpp): GetPersistenceId, GadgetDirector.HasBlueprint /
  GetItemCount, GadgetDirectorHelper.HasBlueprint, GadgetFabricateCategoryRuntime..ctor,
  GadgetFabricatableItem.get_ShouldShowPlaceholder / FabricateAndSpendCost, GadgetItem.PlaceGadgetEvent /
  PickupGadget, Gadget.ChangeVariant. Other runtime GadgetDefinition clones implement
  `variant_ext::Extension` (custom_upgrades/variant_ext.h) and call `variant_ext::Set` (dj_buildables does).
- gadget_look.cpp owns GadgetDefinition.get_LocalizedDescription, GadgetDirector.GetTargetGadgetInfo,
  GadgetItem.SetHeldGadget, FabricatorItemDetails.SetItem ("nobody else may hook them"). Use
  `gadget_look::Register(def, Look)` and `AddPlacedLabel(provider)` (custom_upgrades/gadget_look.h).
- `PlortCollectorActivator.Activate` (custom_upgrades/module.cpp) serves both silo_world (Sell-All
  button) and coop_world (Hen Collector button); the Hen button exists only with this detour.
- drone_price_limit/station_dialog.cpp owns the DroneProgramMenu / FocusManager hooks and also hosts
  custom_upgrades' drone-programs UI (drone_programs.h). drone_source.cpp's DroneStationGadgetModel.Pull
  detour asks drone_programs through `drones::SetProgramHooks` (callbacks, not a second hook).
- radio_shop: "none of these is hooked anywhere else"; dj_buildables: GadgetMusicPlayer hooked nowhere else.
- UpgradeModel.SetUpgradeLevel is custom_upgrades'; sandbox/upgrades.cpp relies on it and has no hooks.
  sandbox/upgrades.cpp uses the sandbox_fabricator FabricatorUI hooks for the open Fabricator.

**Shared / folded bodies (detour must filter)**
- ListUIRoot`4 RebuildUI / ShowAltActionButton / OnAltAction / OpenPopupPrompt: resolved through
  LandPlotUIRoot's inflated generic base (method lookup walking parents); the body is shared by every
  list menu, so the detours check the class (LandPlotUIRoot). A hook on these via any other ListUIRoot
  subclass would collide.
- AmmoModel.Pull = TrackedActorsModel.Pull (0xA6E510): detour checks it is the player's AmmoModel.
- RaycastHit.get_normal (0x339E700) = Ray.get_direction, Bounds.get_extents, BoundsInt.get_size,
  ContactPoint.get_normal, ParticleCollisionEvent.get_normal: called everywhere; acts only inside
  UpdateFootprint on the main thread.
- Hot engine/global entry points that see everything: Button.Press, FocusManager.*, UIActivator.OnInteract,
  EventSystem.Update, KinematicCharacterMotor.HandleVelocityProjection, InputEvent.*: keep the fast path a
  pointer compare.
- ui/options_tab.cpp refuses to install if two of its targets share an address, and skips
  options_row_activate if it is folded with another.

**Lifetime / toggling**
- Always on (own HookManager groups, enabled at creation, not tied to the module switch):
  `core.*`, `ui.*`, `sandbox_achievements.*`, `sandbox_saves.*`, `sandbox_fabricator.*` (detours check a
  flag). sandbox and custom_upgrades use `SetupWhileDisabled`, so their hooks exist (disabled) while off.
- Everything else is created disabled and switched with its module; on a native fault in a DetourGuard
  the module's hooks are disabled at the next main-thread tick.
- Many optional hooks only exist when their resolve succeeded (marked opt); quick_travel's groups,
  quick_start's three groups, spawn_scaling's weather/node hooks and plot_upgrades' four features
  resolve independently.
- core.main_thread.Canvas.SendWillRenderCanvases is created only when Time.frameCount resolved (to
  de-duplicate ticks) or when the EventSystem.Update hook failed.
