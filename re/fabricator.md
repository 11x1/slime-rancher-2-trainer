# Fabricator, gadgets, decorations

`MP.SR` = `MonomiPark.SlimeRancher`; Fabricator UI in `MonomiPark.SlimeRancher.UI.Fabricator`. Tags: **IDA/code** = decompiled; **dump** = signature only. Mod use: `modules/custom_upgrades/` (`quantum_link.cpp`, `dj_buildables.cpp`, `gadget_look.cpp`, `variant_ext.h`, `fabricator_levels.cpp`, `vacpack_upgrades.cpp`), `modules/sandbox/` (crafter, blueprints, Fabricator G key), `modules/free_placement/`, `modules/instant_build/`. Hooks: re/hooks.md (several gadget hooks have one owner; others extend via `variant_ext.h` / `gadget_look.h`).

### Fabricator categories (dump)
- `GadgetFabricateCategory` : FabricateCategory, IValidatable — `GetItems(IFabricatorGadgetHelper, IRecipeFactory)` 0x8289E0, `Contains` 0x828980, `IndexOf` 0x828B50, `TotalCount` 0x828BD0, `UnlockedCount` 0x828C20; `_gadgetSubGroup` +0x28, `_gadgetPurchaseEvent` +0x30.
- gotcha: hooking `GadgetFabricateCategory.GetItems` never fired (inlined into its caller); custom_upgrades hooks `GadgetFabricateCategoryRuntime..ctor` instead to add cards.
- `IFabricatableItem` (interface): `CheckCanFabricate(int)`, `CheckCanAffordFabricateCosts`, `CheckCanBeFabricatedIgnoringCosts`, `FabricateAndSpendCost(int)`, `get_PurchaseCost`, `get_IsMissingBlueprint`, ... Implementations name them explicitly: `GadgetFabricatableItem` -> `MonomiPark.SlimeRancher.UI.Fabricator.IFabricatableItem.CheckCanFabricate` 0x827D50. `GadgetDirectorHelper` implements `IFabricatorGadgetHelper.CheckCanFabricate(GadgetDefinition, int)` 0x827950.
- Other runtime categories: `PlayerUpgradeFabricateCategory(Runtime)`, `PinnedFabricateCategory(Runtime)`.

### GadgetDefinition / GadgetDirector
- `GadgetDefinition` (IdentifiableType): `VariantOf`, `get_LocalizedDescription` 0x51B440 walks `VariantOf` to the root and returns ITS `_localizedDescription` (variants show the base text unless hooked: gadget_look); `localizedName` read directly; `get_Icon` returns its own icon when set; `_fullArt` / `_requiresFullArt` (details picture, addressable sprite via `UIImageDynamicLoader`); `_pediaLink`.
- `GadgetDirector`: `InstantiateGadget` (model type from the prefab via `TryGetId(prefab)`), `_gadgetVariantMap` (every variant lookup is `get_Item` without fallback: register a key for mod definitions), `HasBlueprint`, `GetItemCount` (variants -> `BaseGadget`), `GetTargetGadgetInfo` 0x868E80 (gadget-mode label: `DisplayName` from `_pediaLink`), `TargetedGadget`, `CurrentGadgetMusicPlayer`.
- `GadgetsModel._itemCounts` / blueprints: `Pull` throws on unknown or duplicate ids (re/save.md).
- `Gadget.ChangeVariant(def)` (vanilla destroys and re-places: new actor id), `Gadget.AllowGadgetStackingOnTop` (instance field), `_stackedGadgetsIds`; prefab flag `CanStackOnStackableGadgets`.
- `GadgetItem.SetHeldGadget` 0xA46810 builds `_gadgetPlaceholderInstance` (the placement preview) from the base prefab.
- `PlaceGadgetEvent`, `PickupGadget`, `OnGadgetVariantToggled` -> `SetFavorite` (favourite slots are saved).
- `FabricatorItemDetails.SetItem` (details panel picture).
- `SpendableDirector.Spend(cost)`.

### Prefabs used by mods (data)
Scientist Desk (DJ Booth base), Boombox (`model_boombox`, `Collision` ball, bench, `FX_MusicalNotes`), Night Light toy (`model_nightlight`, root ball collider r 0.47, Rigidbody 0.5 kg), Refinery Link (`LookupDirector.RefineryLink`), teleporter `TeleporterNode._activeFX`, Flower Pillow (`SlimeAttachmentPoint`). `SlimeAttachPoint`: `_isJointActive`, `_slime`, `_allowLargo` (OnTriggerEnter drops largos without it), `_rotationSpeed` (FixedUpdate 0x752700 rotates the joint bodies by it).

## From the custom_upgrades notes

### GadgetDefinition  : IdentifiableType
- methods: `get_LocalizedDescription` 0x51B440 - walks `VariantOf` to the root and returns ITS `_localizedDescription` (a variant's own description is never asked); `localizedName` is read directly
- methods: `get_Icon` - returns the definition's own `icon` when set (Fabricator grid tile, hotbar)
- fields: `VariantOf`, `BaseGadget` (= VariantOf ?? self), `_pediaLink`, `icon`, `_fullArt` / `_requiresFullArt` (IdentifiableType; addressable full art, copied by Instantiate), `CraftingCosts`
- gotchas: `ReferenceId` (0x8700C0) = "{GetType().Name}.{name}", computed once into private `referenceId`
- mod: gadget_look.cpp (owns the get_LocalizedDescription hook), quantum_link.cpp / dj_buildables.cpp (clones)

### GadgetDirector
- methods: `InstantiateGadget` 0x869880 - `IdentifiableType.TryGetId(prefab)` + `GameModel.InstantiateGadget(that def)`: a placed gadget's model type comes from the PREFAB
- methods: `GetVariantsForGadget` 0x869140, `GetAvailableVariantsForGadget` 0x868750, `HasVariantsForGadget` 0x869540 - all `_gadgetVariantMap.get_Item` without fallback (every definition asked needs a key)
- methods: `HasBlueprint` 0x869360 (blueprints HashSet); `GetItemCount` 0x8688C0 (answers BaseGadget's count); `OnGadgetVariantToggled` 0x86A1C0 = `GadgetsModel.SetFavorite(selected slot, variant)` (favourites ARE saved, as persistence ids)
- methods: `GetTargetGadgetInfo(def, includeCount, isInRanch, droneType)` 0x868E80 - gadget-mode label; DisplayName = def._pediaLink title (shared with base), SubName = Ranch/RangeLocalizedName; def = `SelectedSlottedGadget` when held, `Gadget.IdentTypeAsDefinition` when aiming at a placed one (vanilla def); `_isTargetingPlaceholder`
- fields: `_gadgetVariantMap` (Dictionary<GadgetDefinition, List<GadgetDefinition>>, filled in Awake; new scene = new director), `TargetedGadget`, `CurrentGadgetMusicPlayer`
- mod: quantum_link.cpp (HasBlueprint, GetItemCount detours; registers variants every tick), gadget_look.cpp (GetTargetGadgetInfo), variant_ext.h (DJ buildables via quantum hooks)

### GadgetItem (held gadget / placement)
- methods: `PlaceGadgetEvent` 0xA45410 - spends {BaseGadget, 1}, then `InstantiateGadget(GetGadgetDefinitionToPlace(held).prefab)` -> `_lastPlacedGadget`
- methods: `SetHeldGadget` 0xA46810 - variants = [BaseGadget] + GetVariantsForGadget(base, unlockedOnly: true) (unlocked = HasBlueprint); destroys the old placeholder, builds `_gadgetPlaceholderInstance` from the base prefab (CopyPlaceholderGameObject + CopyMeshComponents / CopyGadgetComponents / CopySpecialComponents); native callers: PickupGadget, OnItemActivated, PlaceGadgetEvent, HandleSelectedSlottedGadgetChanged (variant toggle via SetFavorite)
- methods: `ToggleVariant` 0xA47AC0 - held: cycles `_heldGadgetVariants` -> OnGadgetVariantToggled; aimed at placed: `Gadget.ChangeVariant(_targetedGadgetVariants[(IndexOf(gadget.IdentTypeAsDefinition)+1) % n])` - a placed clone is a vanilla model (index 0), so it always asks for the FIRST variant
- methods: `PickupGadget`
- gotchas: placeholder lives on layer 26 (overlay)
- mod: quantum_link.cpp (PlaceGadgetEvent, PickupGadget detours), gadget_look.cpp (SetHeldGadget post: dressPreview)

### Gadget
- methods: `ChangeVariant(variant, regionRegistry)` 0x651CD0 - destroys and re-places the gadget (new actor id)
- fields: `AllowGadgetStackingOnTop` (instance field; prefab default false), `CanStackOnStackableGadgets`; `GadgetModel._stackedGadgetsIds` (vanilla stacking relation); `IdentTypeAsDefinition`
- gotchas: a placed gadget has no Rigidbody (prefab); stacking ray accepts a hit gadget with AllowGadgetStackingOnTop when the placeholder has CanStackOnStackableGadgets (free_placement/NOTES.md)
- mod: quantum_link.cpp ChangeVariant detour (flips quantum/plain in place; DJ cycles kinds)

### GadgetsModel
- methods: `Pull` 0xA91FC0 - blueprints / availBlueprints / favourites through `ISaveReferenceTranslation.GetPersistenceId`; `_itemCounts` through `Enumerable.ToDictionary` (duplicate key throws) -> never AddItem / AddBlueprint a runtime clone
- mod: quantum_link.cpp, dj_buildables.cpp, buildables.h, sandbox (IsModGadget)

### Fabricator gadget categories
- methods: `GadgetFabricateCategory.GetItems` 0x8289E0 - NO native caller in 1.3.0 (metadata only)
- methods: `GadgetFabricateCategoryRuntime..ctor(category, helper, recipeFactory)` 0x8287D0 - same LINQ inlined (`_gadgetSubGroup.GetAllMembers().Cast<GadgetDefinition>().Select(new GadgetFabricatableItem(helper, recipeFactory, _gadgetPurchaseEvent, def)).ToList()`) -> `FabricateCategoryRuntime<T>..ctor`
- methods: `FabricateCategoryRuntime<T>..ctor` 0x20F5F80 - `Category` @0x10, `_categoryCount` = category.TotalCount() @0x18, `_itemList` @0x20
- chains: `FabricatorCategoryList.FindCategoriesToShow` builds one runtime per category each time the Fabricator opens (also the Sandbox's rebuild)
- methods: `GadgetFabricatableItem.FabricateAndSpendCost` 0x827E20 = CheckCanFabricate (helper: recipe has items, HasBlueprint, CanAddGadget, costs) + `SpendableDirector.Spend(def.CraftingCosts, n)` + `GadgetDirector.AddItem(def, n)` + analytics, events
- methods: `GadgetFabricatableItem.get_IsMissingBlueprint` = recipe has items && !helper.HasBlueprint(def); `get_ShouldShowPlaceholder` 0x828470 = def's pedia entry not unlocked ("?" tile)
- methods: `GadgetDirectorHelper.HasBlueprint` 0x827B50 -> GadgetDirector.HasBlueprint
- methods: `FabricatorItemGrid.SetCategory` 0x80A170 - pages = ceil(TotalCount / 36 or 16), contents from Items -> raise `_categoryCount` when inserting
- gotchas: GadgetFabricatableItem methods are interface targets (no direct code refs; fine to hook)
- mod: quantum_link.cpp (ctor detour: quantum::InsertItem + DJ InsertCards, FixPageCount; FabricateAndSpendCost, helper HasBlueprint, ShouldShowPlaceholder detours)

### Fabricator details panel / grid pips (menulayer_fabricator scene)
- methods: `FabricatorItemDetails.SetItem` 0x8089B0 - `UIImageDynamicLoader` loads `IFullArtAndIconSource.FullArtReference`; without a valid reference leaves the image alone (RequiresFullArt false)
- methods: `UpgradeChain.Show(current, count)` 0x82ED90 - shows first `count` circles, highlight onto `_levels[current+1]` (12 levels at VIII -> IndexOutOfRange)
- fields: UpgradeChain ("Upgrade Chain"): 8 circles Pip0..Pip7 (`_levels`, SDFGraphic[], 42 x 22 at x = 21 + 52 i, absolute, no layout group), empty `_rects`, highlight 54 x 34
- methods: `FabricatorItemButton.ConfigurePips(unlocked, total)` 0x806CE0 (`Configure` inlined) - walks the array only; `FabricatorItemButtonPips` 8 pips 22 x 11, two rows of four, pitch 24 / -14
- mod: gadget_look.cpp (SetItem: card picture), fabricator_levels.cpp (pre-hooks on Show and ConfigurePips)

### Type groups (identifiabletypegroups_assets_*.bundle, UnityPy)
- `GadgetWarpGroup` 41 members: TeleporterHome x4, TeleporterZone x4, TeleporterSanctuary x4, MarketLink, RefineryLink (member 13), Teleporter x10, WarpDepot x10, LinkedCannon x7; Sandbox crafter shows 40 (Teleporter Sanctuary Blue has no crafting cost)
- `GadgetDecorGroup` 241 members (Scientist Desk, Boombox inside)

### Decoration / toy prefabs (identifiabletypes bundle, UnityPy, local units)
- decorScientistDesk: "model" (BoxCollider layer 27 + LODGroup; desk top y ~1.22, x -1.1..1.1, z -0.5..0.5); clutter children scientistDesk_bubbleScroll, scientistDesk_glass (+_LOD01, bubbles/drip particles), vials (+_LOD01), decoLabBeaker01, decoLabClipboard, decoLabWrenchMonkey03_0, pens, scientistDesk_misc01 (+_LOD01) = tubes between the flasks; looping sound "SoundScientistDesk"; three point lights; AllowGadgetStackingOnTop 0
- decorBoombox: root = Gadget + GadgetMusicPlayer + GadgetUIInteractable (gadgetMusic "BoomboxMusic", timeBetweenActivations 3, _blockInteractInGadgetMode 1); bench "ranchHouseBench" (layer 27, mesh + box collider); "model_boombox" at y 0.72 (Animator + LODGroup; bbox_base_jnt / bbox_head_jnt / FX_MusicalNotes; skinned meshes), faces +z (eyes z 0.31); "Collision" sphere r 0.5 at y 1.08 (layer 27); AllowGadgetStackingOnTop 0, CanStackOnStackableGadgets 1
- decorFlowerPillowSmall: "mesh" (LODGroup, 3 LODs, no collider), "COL" (4 capsules), "SlimeAttachmentPoint" at y 1.30 (SphereCollider trigger r 1 + SlimeAttachPoint: base slimes, `_allowLargo` 0, no tarr, detach cooldown 5 s; children LockJoint / LargoLockJoint rigidbodies, "Attractor" 15x sphere trigger with Attractor script)
- decorHayBales: "hayBale_2", "hayBale_0 (1)" (each "hayBaleA": MeshRenderer, MeshCollider, collision-FX script), "hayStack", "pitchfork", two grass tufts, Bounds / env interaction objects
- decorFireFlower: "FX Fireflower" (ember PS "Embers_01" / "Embers_02" at y 2.59 / 1.17)
- toyNightLight (root scale 1.75): "model_nightlight" (mesh, hinged handle, point light); root sphere collider r 0.47 + small bottom box, Rigidbody 0.5 kg, no constraints (rolls)
- toyStuffedChicken (root scale 1.33): "model_stuffedChicken" (one MeshRenderer; mesh `stuffedChicken`, origin at body centre, radius 0.44, row of round bulbs low on front/sides y -0.48 .. +0.05 that read as eggs; front assumed +z, unverified)
- lookups: `LookupDirector._gadgetDefinitions` / `_toyDefinitions` (member types, then sub-groups), matched by prefab name
- mod: dj_buildables.cpp, plot_props.cpp

## From the minimap, map_tools, quick_travel, wall_markers, collectibles notes

### GadgetModel (MonomiPark.SlimeRancher.DataModel) : IdentifiableModel
- methods: `SetupMapMarker` 0xA915A0 - registers the gadget in `MapDirector.Markers` when `GadgetDefinition.DisplayOnMap` (+0x10A) (IDA)
- fields: `_isPrePlaced` bool (placed by the level, not the player)
- gotchas: `DroneStationGadgetModel` (`_type` DroneType int: 0 rancher, 1 explorer) is its own map marker source (class check); `TeleporterGadgetModel`, `WarpDepotModel` both `: GadgetModel, LinkDestroyer` (dump)
- mod: MM (placed-gadget icons)

### GadgetDefinition (global ns) : IdentifiableType
- fields: `Type` (Types enum: DEFAULT 0, TELEPORTER 1, WARP_DEPOT 2, SNARE 3, ECHO_NET 4, DRONE 5, CANNON 6, ITEM_DISPLAY 7, LINKED_CANNON 8, PRISMA_DETECTOR 9, DRONE_STATION 10), `DisplayOnMap` +0x10A, `_optionalMapIcon` Sprite, `VariantOf` (null on a base gadget) (dump)
- gotchas: decorations are all `DEFAULT` with empty `groupType`; only their icon names separate them (`iconDecor*_small`, e.g. `iconDecorSingleChair_small`); functional DEFAULT gadgets use `iconGadget*_small` (log 1.3.0)
- gotchas: MarketLink / RefineryLink are `DEFAULT` (icons `iconGadgetMarketLink_small`, `iconGadgetRefineryLink_small`); `TeleporterHomeYellow` is type 1 (`iconGadgetTeleportHomeYellow_small`); colour variants may leave Type at DEFAULT (unverified) -> check base via VariantOf
- gotchas: the game's own categorisation is `GadgetCategory._gadgetSubGroup` (IdentifiableTypeGroup asset, reachable only by asset name)
- mod: MM (gadget classification), MT (teleporter icon for pop-ups = home teleporter definition, Type == TELEPORTER)

## From the slime_dj, spawn_scaling, hl2_movement, quick_start, free_placement, misc_tweaks, plot_upgrades, drone_price_limit notes

### GadgetItem (MonomiPark.SlimeRancher.Player.PlayerItems) - placement (IDA)
- methods: `Update` 0xA494D0 -> `UpdateFootprint` 0xA48050 each frame while a footprint exists; `PlaceGadgetEvent` 0xA45410 calls UpdateFootprint once more then reads, in order: `_isFootprintVisible` (silent return), `IsLinkedGadgetInRange` (NotInRange toast), `_isGrounded` (player grounded via IPlayerController; GroundedCheck toast), `_isPlacementBlocked` (BlockedPlacement), `_isPlacementValid` (InvalidPlacement). All pass -> `SpendableDirector.HasSpendables` / spend, `GadgetDirector.InstantiateGadget(prefab, RegionRegistry.CurrentSceneGroup, pos, rot, true, false)`, `Gadget.OnPlaced`, `AddStackedGadget` when `_baseGadget` is set
- footprint visible: ray (length `PlacementFootprintVisibleDist`, mask -1610662149) hits farther than `MinPlacementFootprintVisibleDist` (measured horizontally), on layer 0 (or a stackable base gadget), with `normal.y > sin(FootprintVisibleSlopeThreshold)` (strict)
- `IsPlacementValid(Ray, RaycastHit)` 0xA439F0: distance <= `MaxValidPlacementDist`; stacking only if placeholder `CanStackOnStackableGadgets`; `sin(MaxValidPlacementSlope) <= normal.y`; placeholder `Gadget.IsCompletelyGrounded(GroundedCheckAdjustment, GroundedRaycastDistance)`
- `_isPlacementBlocked` = `Gadget.IsOverlapping(OverlapPlacementFloorTolerance, GadgetOverlapLayers, false)`; same overload also called by `ShouldHideGadgetBasedOnPlayerCollision` (inside UpdateFootprint, other layers) and `DefaultDynamicObjectSpawner*.InstantiateGadgetModel`
- `IsInLinkedGadgetRange(Vector3)` 0xA43770: only gadgets with `LinkedDefinition` and `LinkedGadgetRange > 0`; first placed partner within range
- colour: `SetGadgetPlacementValidity(!blocked && valid && grounded && linked)` (only with a held gadget + visible footprint)
- rotation: `_rotatingClockwise/_rotatingCounterClockwise` (rotate input callbacks) -> placeholder `Transform.Rotate(up, ±GadgetRotationSpeed * dt, World)`; placeholder is a child of the footprint (yaw accumulates), footprint rigidbody re-aligned to the surface every frame, position via `Rigidbody.set_position` (applied at the physics step)
- footprint rotation = `LookRotation(player direction projected onto surface, normal)`; PlaceGadgetEvent instantiates with the placeholder's rotation; the footprint ray (`RaycastNonAlloc`) hits from any side
- the tunables (slope thresholds, MaxValidPlacementDist, GadgetOverlapLayers, ...) are fields of the placement metadata asset
- mod: free_placement (UpdateFootprint detour swaps metadata values for the call; IsCompletelyGrounded / IsOverlapping / IsInLinkedGadgetRange detours; `_isGrounded` forced after)

### PlayerItemController - gadget mode entry
- `ToggleItem` refuses inside the ranch house (`_isInRanchHouse`), when dead, while fast-forwarding, or when `_gadgetModeBlocker` is set (a `DisableGadgetModeTrigger` volume via `BlockGadgetMode`, cleared by `UnblockGadgetMode`) (IDA)
- no zone/site ownership: a placed gadget is a `GadgetModel` with scene group + position
- mod: free_placement (ignore_zones skips BlockGadgetMode, clears a stored blocker)

### GadgetModel - rotation persistence
- `PullBase` saves full euler angles (`PlacedGadgetV07.Rot`, Vector3); `GetRot` rebuilds from all three (upside-down gadgets load as placed) (IDA)

### Teleporter gadgets
- `GadgetTeleporterNode` in the prefab; `TeleportNetwork` passes the destination node's `eulerAngles` to `TeleportablePlayer.TeleportTo` -> `IPlayerController.set_Rotation` -> `KinematicCharacterMotor.SetRotation`; `KFCCharacterController.UpdateRotation` keeps the motor's `_characterUp` -> a tilted teleporter tilts the player (IDA)
- mod: free_placement keeps vanilla slope limits for teleporters

### FabricatorUI / FabricatorConfirmPopup (MonomiPark.SlimeRancher.UI.Fabricator) (IDA)
- `FabricatorUI.OnItemActivated(IFabricatableItem)` 0x826560 (grid activate / Fabricate / Upgrade): returns unless `CheckCanBeFabricatedIgnoringCosts(1)` (interface slot 5) and `SpendableDirector.HasSpendables(cost, 1)`; then `_confirming = true`, disables `_content`, hides the key bar, `FabricatorConfirmPopup.Show(item)`, focus, cue
- `FabricatorConfirmPopup.Show` 0x801150: title/name/icon, `_countToMake = 1`, `_maxCountCanOwn` (99 or refinery space for gadgets), resets success animator, `CheckCanFabricate(_countToMake)`
- confirm = `ISubmitHandler.OnSubmit` 0x8016B0: returns if `_isClosing`; `CheckCanFabricate(_countToMake)` (slot 7) != NONE -> `PlayFabcricationDenied`; else start cue `PlayConfirmationConfirmed{Upgrade,Gadget}Start`, count label, `Confirmed` event, `HideForConfirm`
- `Confirmed` -> `FabricatorUI.OnConfirmationConfirmed(item, count)` 0x826220: pinned-recipe update, `FabricateAndSpendCost(count)` (slot 10), `UpdateInputLegendBasedOnItemUpgrade`, `FabricatorContent.ItemCrafted(item)`
- `HideForConfirm` 0x800A50: `_isClosing = true`, `_onFadeOutStart.Invoke()`; with `_successAnimator`: enable, `SetBool(IS_UPGRADE)`, `SetTrigger(SUCCESS)`, return; without: `_transitionFader` fade-out + `FadeOutComplete` at once
- `NotifyAnimatorFadeOutComplete` 0x800FB0 (`[PublicAPI]`, animation event): disables the Animator, fader fade-out, `FadeOutComplete` -> `FabricatorUI.HandleConfirmationFadeOutComplete` -> `ReturnToContent` 0x826CB0 (`_confirming = false`, content enabled + focused, key bar back). Throws NRE without `_successAnimator` / `_transitionFader`
- `PlaySuccessAudioCue` 0x8010C0 (`[PublicAPI]`, animation event); `FabricatorSoundBank.PlayConfirmationConfirmedForUpgrade` / `ForGadget`
- no hold-to-confirm anywhere (`SRButton` has no hold)
- gotchas: `CheckCanFabricate`, `CheckCanAffordFabricateCosts`, `CheckCanBeFabricatedIgnoringCosts`, `FabricateAndSpendCost` are **explicit interface implementations** on `GadgetFabricatableItem` / `PlayerUpgradeFabricatableItem`: metadata name `MonomiPark.SlimeRancher.UI.Fabricator.IFabricatableItem.CheckCanFabricate` (Gadget 0x827D50, Upgrade 0x82AE30); the plain name is not found (log). `ShouldSkipAnimations` compiles to `return false`, a shared 3-byte stub at 0x510250 (unhookable)
- mod: misc_tweaks detours `OnItemActivated` (Instant) and `HideForConfirm` (Skip animation)

## From the sandbox, sell_upgrades, collectibles, instant_build notes

### GadgetDirector  [SceneContext.GadgetDirector]
- methods: `AddItem(in AddItemOptions)` 0x867C20 - NO blueprint check; storable type: clamps to refinery space; GadgetDefinition: unlocks pedia entry if needed, raises gadget-obtained event, `GadgetsModel.SetCount(BaseGadget, GetCount + n)` (IDA). Convenience `AddItem(def, n)` used by sb
- methods: `CanAddGadget(def, n)` - only compares count with the gadget's limit (`GadgetDefinition` +0xF8) (IDA)
- methods: `HasBlueprint` 0x869360 - exactly `GadgetsModel.blueprints` HashSet membership (IDA)
- methods: `AddBlueprint(def, showPediaUnlockNotif)` 0x867910 - `GadgetsModel.blueprints.Add`, pedia unlock, `BlueprintObtained`, `_blueprintObtainedEvent`, `GadgetUnlockedAnalyticsEvent` (IDA)
- methods: `GetRefinerySpaceAvailable(t)` (999 per type), `IsStorable(t)` (refinery storable groups minus block list), `GetItemCount`, `InstantiateGadget`
- fields: `_model` (GadgetsModel: `blueprints` HashSet<GadgetDefinition>, saved by `GadgetsModel.Pull`), `_waitForChargeupPrefab` (ib)
- gotchas: no RemoveBlueprint exists: lock = `HashSet.Remove` on `GadgetsModel.blueprints` (method resolved from the live set's class); pedia entry stays unlocked
- gotchas: a locked-blueprint gadget added by AddItem lands in inventory and is saved; blueprint stays locked
- gotchas: variants count on their base gadget (`SetCount(BaseGadget, ...)`)
- mod: sb crafter (DoCraft), materials (DoAddMaterial), upgrades.cpp blueprints

### GadgetDefinition  [: IdentifiableType]
- fields: `CraftingCosts` (PurchaseCost), `BuyInPairs` (x2 per craft), count limit +0xF8, `VariantOf` +0x120 (base gadget for colour/style variants; variants live in `GadgetHiddenGroup`)
- mod: sb crafter recipe list and variants menu

### PurchaseCost
- fields: `IdentCosts` (`IdentCostEntry {Amount, IdentType}`, struct list read through `List._items`), `_currencyCosts` (`List<CurrencyCostEntry {CurrencyDefinition _currency, int _amount}>`)
- mod: sb RecipeCost; su refund pricing (`MenuPurchaseItemModel._purchaseCost`)

### GadgetFabricatableItem
- methods: `FabricateAndSpendCost` 0x827E20 - `SpendableDirector.Spend` + `GadgetDirector.AddItem(def, count * (BuyInPairs ? 2 : 1))` + fabricate analytics + `MetaGameDirector.OnGadgetFabricated` (IDA)
- methods: `get_IsMissingBlueprint` - `IFabricatorGadgetHelper.HasBlueprint` (GadgetDirector list)
- mod: sb DoCraft mirrors it without Spend / analytics / meta-game

### Fabricator menu data
- classes: `FabricateMenuConfiguration` (`_categories`), `GadgetFabricateCategory` (`Contains(def)`, `IndexOf(def)`, `_gadgetSubGroup`), `FabricateCategory` (`_title`, `_icon`), `PinnedFabricateCategory` (`_icon`)
- gotchas: FabricateMenuConfiguration asset is loaded only once the Fabricator UI has been opened; a FindObjectsOfTypeAll retry costs ~25 ms per frame
- gotchas: categories are built on the `GadgetGroup` member groups except hidden/variants: `GadgetUtilitiesGroup`, `GadgetWarpGroup`, `GadgetDecorGroup` (loaded from the start); order within = index in the group's `_memberTypes` (what IndexOf reads)
- gotchas: category sprites by name `iconCategoryUtilities`, `iconCategoryWarptech`, `iconCategoryDecorations` are loaded from the start
- gotchas: the Pinned category sprite is `pin` (Addressables `Icon/pin.png`, next to `Pinned Fabricate Category.asset` and scene `MenuLayer_Fabricator.unity`): exists ONLY while the Fabricator UI scene is loaded
- mod: sb crafter tabs, EnsurePinnedIcon, PinIconTick (preload, see re/ui.md UIDisplayManager)

### PlayerModel.PinnedRecipe
- fields: `PlayerModel.PinnedRecipe` (IRecipe, `RecipeFactory.BaseRecipe`, saved as reference id); `get_ReferenceId` matches `IdentifiableType.ReferenceId`
- mod: sb crafter Pinned tab

### FabricatorUI
- methods: `Start` 0x826D30 (once per instance, wires content events), `OnDestroy` 0x8263D0 (IDA)
- fields: `_inputLegend` (InputLegend; key bar bottom left), `_sidebarInputLegend` / `_playerUpgradeInputLegend` / `_gadgetInputLegend` / `_pinnedRecipeCategoryInputLegend` (per page legend configs: fabricate / upgrade / mark all seen / pin), `_pinRecipeEvent`, `_spendableDirector`, `_confirmPopup` (FabricatorConfirmPopup), `_confirming`, `_itemDetails._recipe._itemCount` (craft count)
- chains: `FabricatorUI.OnItemSelected` -> `UpdateInputLegendBasedOnItemUpgrade` (legend Fabricate/Upgrade enabled with `SpendableDirector.HasSpendables`)
- gotchas: `FindObjectOfType(FabricatorUI)` (Unity 6 FindFirstObjectByType) returned nothing while the menu was up (cause unknown; maybe hide flags of UI-framework display objects) and cost 6-14 ms per call: track via Start/OnDestroy hooks instead; open = listed instance activeInHierarchy
- gotchas: no G binding in its legend configs (not checked against bindings asset); Fabricator takes game input while open, so OS key state (GetAsyncKeyState) is used
- mod: sb hooks `sandbox_fabricator` (always on), FabricatorTick, FabricatorHintTick ([G] entry cloned via modules/common/legend_hint.h, placed before the Pin hint)

### FabricatorContent / FabricatorItemDetails / FabricatorItemGrid / FabricatorConfirmPopup
- methods: `FabricatorContent.OnItemSelected(item, cause)` 0x804700 - `FabricatorItemDetails.SetItem(item)` 0x8089B0 (name, icon, details, `FabricatorRecipe.SetItem(item, 1)` have/need, button state from `CheckCanFabricate`), `UpdateVariantsButton`, `ItemSelected` event (IDA). `InteractionCause.PROGRAMMATIC` = 0 (no sound)
- methods: `FabricatorContent.OnCategorySelected` - fades grid out until category activated (avoid for refresh)
- methods: `FabricatorItemGrid.TryGetSelectedItem(out item)` 0x80AAD0 - `_gridNavigation._selectedIndex` into `_currentPageOfItems` (IDA); `_hoveredIndex` is pointer hover (-1 off-tile)
- methods: `FabricatorItemGrid.SetCategory(runtime)` 0x80A170 - acts only for a different object: grid size, `PaginatedView.SetNumberOfPages` -> `SetPage(0, forced)` -> `OnPageChanged` 0x809420 (buttons from runtime's Items, clears selection, `OnSelectionChanged`) (IDA)
- methods: `FabricatorConfirmPopup.IncrementCount` 0x800DB0 / `DecrementCount` - `UpdateCountAndRecipeDisplay(item.CheckCanFabricate(count))` (IDA)
- fields: FabricatorConfirmPopup `_item`, `_countToMake`, `_isClosing`
- gotchas: `CheckCanFabricate` is an explicit interface implementation: method named `IFabricatableItem.CheckCanFabricate` on the item's class
- mod: sb RefreshFabricator, DoFabricatorMaterials (quantity popup)

### FabricatorCategoryList (rebuild as on opening)
- methods: `Start` 0x7FF0A0 -> `FindCategoriesToShow` 0x7FE4E0 (returns at once while `_categoriesShown` set; else new `*FabricateCategoryRuntime` per `_configuration` category = fresh placeholder/tier/icon state) -> `SetupElements(_buttons, _categoriesShown, SetData)` -> select (IDA)
- chains (sb rebuild): `_categoriesShown = null`, `FindCategoriesToShow`, `FabricatorCategoryButton.SetData` per button, `FabricatorItemGrid.SetCategory(new runtime)`, `FabricatorCategoryTitle.SetCategory`, `PaginatedView.SetPage(old)`, `GridNavigation.SetSelectedIndex(old)` + `OnSelectionChanged(PROGRAMMATIC)`, then OnItemSelected
- gotchas: grid items are built once per open; calling Start again double-subscribes events
- mod: sb upgrades.cpp RebuildFabricator

### Gadget-mode menu (NewGadgetSelectionUI, MonomiPark...UI.Gadget)
- classes: `NewGadgetSelectionUI` (`OnCategorySelected(cat, cause)`), `NewGadgetCategoryList` (`SelectedCategory`), `NewGadgetItemGrid` (`_category`, `SetCategory` returns early for the same one), `GadgetCategoryRuntime` (`_itemList` read once from `GadgetCategory.GetItems`, `UpdateItemList()`), `GadgetItemData` (`IsAvailable` fixed at construction), `NewGadgetItemButton.SetData` (counts from GadgetDirector)
- gotchas: the menu is rebuilt each time it opens; an open one does not pick up new inventory without the refresh below
- chains: refresh = `UpdateItemList()` per category button `_category`, clear grid `_category`, `OnCategorySelected(SelectedCategory, PROGRAMMATIC)` -> SetCategory -> SetNumberOfPages -> SetPage(0, force) -> OnPageChanged -> SetData
- gotchas (spec): GadgetFabricateCategory.GetItems inlined - not observed in these sources (the sandbox notes cite GadgetCategory.GetItems for the gadget-mode menu only)
- methods: `NewGadgetSelectionUI.OnCategorySelected(cat, cause)` 0x787440 (IDA): `_itemList._size == 0` -> `_nonEmptyCategoryDisplay` off, `_emptyCategoryDisplay` on; else the reverse and `_itemGrid.SetCategory(cat)`; then `UpdateInputLegendStates`. `OnEnable` 0x7880E0 subscribes events, `UpdateFadersImpl`, `NewGadgetItemGrid.SetCategory`
- gotchas: `FindObjectOfType(NewGadgetSelectionUI)` did not find the open menu in the round 10 test (no refresh log line after a craft), like FabricatorUI and MainMenuLandingRootUI: track the instance with the OnEnable / OnDestroy hooks
- mod: sb RefreshGadgetMenu (after every Op::Craft; waits in GadgetMenuTick until the menu is open); hooks `sandbox_gadget_menu` OnEnable / OnDestroy (always on)

### Gadget (charge-up)  [ib]
- fields/methods: `Gadget.CooldownTimeGameHrs` (formerly WaitTimeGameHrs); `Gadget.InitModel` sets `model.waitForChargeupTime = TimeDirector.HoursFromNow(hrs)` when unset (|v| < 0.1) and hrs > 0; saved via `GadgetModel.PushBase/PullBase`
- methods: `Gadget.InGadgetCooldown()` = `!TimeDirector.HasReached(model.waitForChargeupTime)`; consumers `TeleporterNode.IsLinkActive`, `ScorePlort.Start/CanDroneDeposit` (Market Link), `SiloCatcher.Start/IsActive`, `VacSpawner.Start`; ScorePlort/SiloCatcher subscribe `Gadget.OnCooldownFinish` when starting during the wait
- methods: `Gadget.Update` - first Update while charging spawns `_waitingObj` (GadgetDirector._waitForChargeupPrefab); when time reached, `_waitingObj` exists and `!_triggeredCooldownOutro`: invokes OnCooldownFinish, cue, sets flag; indicator destroyed after `_chargeupTimePadding`
- gotchas: `SiloCatcher._nextEject` is the 0.25 s eject rate, not a cooldown; 1 game hour = 1 real minute at normal speed
- mod: ib detour on Gadget.Update sets waitForChargeupTime = now while indicator up and outro not triggered

### Mod buildables (custom_upgrades) facts used by sb
- gotchas: runtime gadget clones (Quantum Storage Link, DJ Booth, DJ Speakers, Slime Radio) have a save alias but must never reach `AddItem` / saved inventory (duplicate id fails the save); `custom_upgrades::IsModGadget`, `buildables::List()` (`Info.baseDef`: Refinery Link, Scientist Desk, Boombox), `buildables::Grant`, `buildables::SetUnlock` (sidecar `buildables.unlock.<id>`)
- asset: `decorBoombox/model_boombox` has `model_boombox_animated` + `_LOD01` + `_LOD02` SkinnedMeshRenderers; Scientist Desk has a `_LOD01` twin of every part (UnityPy)
- mod: sb MergeModRecipes / MergeModVariants / MergeModSpawnables / DoCraftMod / DoSpawnMod, upgrades.cpp Mod buildables tab
