# Vacpack and ammo slots

`MP.SR` = `MonomiPark.SlimeRancher`. Tags: **IDA/code** = decompiled; **dump** = signature only; **data** = read from asset bundles. Mod use: `modules/custom_upgrades/vacpack_upgrades.cpp` (extra slots, tiers), `shadow_catcher.cpp`, `modules/sandbox/` (inventory, put in slots), `modules/hl2_movement/` (scroll-jump), silos/collectors (re/plots.md). Hooks: re/hooks.md.

### VacuumItem (MP.SR.Player.PlayerItems) : SRBehaviour
- `NextSlot(InputEventData)` 0xA67D00 (0x63 bytes, unique), `PreviousSlot(InputEventData)` 0xA68F80 (unique): handlers of `_nextSlot` / `_previousSlot`; subscribed in `OnEnable` 0xA68520, removed in `OnDisable` 0xA67D70. Scroll-wheel slot cycling arrives with `Source == POINTER (1)` (hl2_movement hooks these).
- `SelectSlot(InputEventData)` 0xA69640 / `SelectSlot(int)` 0xA69790 (number keys).
- `ConsumeVacuumable` 0xA645B0 code: a `Vacuumable` with `Size` 0 (NORMAL) and `CanCapture` -> `StartConsuming` -> `Vacuumable.TryConsume` 0x644640 -> `AmmoSlotManager.MaybeAddToAnySlot` -> `TryFindSlot` -> `AmmoSlotDefinition.IsAllowed`; no other check. Also unlocks the gold/shadow pedia entries (`ShadowPlortIdentifiableType`, `ShadowSlimePediaEntry` fields).
- fields: `_shoot` +0x168, `_vac` +0x170, `_selectSlot` +0x178, `_previousSlot` +0x180, `_nextSlot` +0x188, `_burst` +0x190 (all InputEvent).

### AmmoSlotDefinition
- `IsAllowed(IdentifiableType)` 0x866B40 code: `!_slotBlockList.Contains(id) && (_slotTypeGroup == null || _slotTypeGroup.IsMember(id))`. Hot (every vac tick, every slot): a detour must pointer-compare first (shadow_catcher hooks it).
- fields: `_slotTypeGroup`, `_slotBlockList`, `_maxAmount` (e.g. silo `SiloNonSlimes` 100, `ElderCollector` 100).
- data: player slots `AmmoSlot1-4`, `ExpansionAmmoSlot 1/2` use group **VaccableNonLiquids** (block list empty); its member group **VaccableBaseSlimeGroup** has 23 types and neither Shadow nor Gold.

### AmmoSlotManager
- `Decrement(i, n)` 0xA39790: clears the slot at 0 and fires the slot change delegate (updates silo displays).
- `MaybeAddToAnySlot`, `CouldAddToAnySlot`, `Slots[]`, `GetSlotName(i)`, `GetSlotCount(i)`, `get_Slots`.
- `UpdateMaxCount` 0xA3AB70 -> `AmmoModel.UpdateMaxCount` 0xA6EB00 (each slot `set_MaxCount` = `_maxCountValue` NullableFloatProperty default), or `_pendingMaxCountOverride` while no model.
- `SetModel` 0xA3A860 = `Reset(defs)`, the pending override, `ValidateAndAdjustSlots`.
- `ValidateAndAdjustSlots` 0xA3AC00: resizes the slot array to the definitions and clears slots whose type is not allowed; never clamps a count (a max below the contents deletes nothing).
- Slots live in its `AmmoModel` (+0x18): without a model every slot call NREs (e.g. `StorageSlotUI.Start/Update`, `get_Slots`).

### AmmoModel / AmmoSlot
- `AmmoModel.Push` 0xA6E530 (load): copies saved slots by index, Count via `set_Count` 0xA3C110 (no clamp). `AmmoModel.Pull`: save copy (shadow_catcher replaces Shadow slots with empty ones there). `new AmmoModel(null)` is what `LandPlotModel.GetAmmoSet` uses.
- Save format `AmmoDataV02` = ID, EmotionData, Count, Appearance, Radiant (no max count).
- `AmmoSlot` (MP.SR.Player) : IListItemViewModel — `OnSlotChanged` delegate +0x58. Upgrade NRE chain: re/upgrades.md.

- holding (IDA, round 11): `Hold` 0x643C00 / `SetHeld` 0x6444F0 set `_held` (+0x70), the collider layer 13 (`UpdateColliderLayer` 0x644800: launched 9, held 13, tornadoed 15, captive 11) and `UpdateMaterialsHeldState` 0x644A00 (`_Held` int property block, shadows off) on its renderers (`GetComponentsInChildren<Renderer>(false)`, so also child renderers a mod adds). `VacuumItem.UpdateHeld` 0xA6AAE0 puts the held object at `LockJoint`'s position and connects `LockJoint` to its Rigidbody (anchors zero); the joint turns it to the nozzle. A Rigidbody with frozen rotation axes fights that joint: the Slime Radio (rotation X / Z frozen) was thrown away 1-2 s after it was vacuumed (dj_buildables.cpp frees the axes while `_held`).
### Vacuumable
`Size` (0 NORMAL), `CanCapture`, `TryConsume` 0x644640, `Hold` (held layer, `_Held` renderer property; toys are held, not stored), `_preventForcesFromWeaponVacuum`.

## From the custom_upgrades notes

### PlayerState
- fields: `_ammoSlotDefinitions` (corescenes): AmmoSlot1-4 (always), "ExpansionAmmoSlot 1" / "2" (AmmoSlotsAvailable >= 5 / 6), "LiquidAmmoSlot" (LiquidSlotEnabled >= 1); all `_maxAmount` 30, `_maxAmountProperty` = AmmoLimit; vanilla uses every one (6 regular + water)
- methods: `InitModel` 0x87B4F0 / `Reset` 0x87BC70 - build the AmmoSlotManager from `_ammoSlotDefinitions`; InitModel -> `AmmoModel.Reset(defs)` (one AmmoSlot per def, Slot.Definition = defs[i])
- gotchas: InitModel is a virtual target (hookable)
- mod: vacpack_upgrades.cpp (pre: insert two clones of ExpansionAmmoSlot 2, thresholds 7 / 8, before LiquidAmmoSlot)

### AmmoModel / AmmoSlotManager
- methods: `AmmoModel.Push` 0xA6E530 - saved slots copied BY INDEX into the existing slots (count, id, emotions, appearance, radiant); then `SetModel -> ValidateAndAdjustSlots` clears ids a slot's definition does not allow (pads / trims by index with a warning)
- methods: `AmmoModel.Pull` 0xA6E510 - returns the live array; body SHARED with `TrackedActorsModel.Pull`
- methods: `AmmoSlotManager.Decrement` clears the slot (id null) at 0; `MaybeAddResource` (overflow)
- gotchas: no hard slot limit (array of any length; HUD binds one view per slot; save is a list)
- mod: vacpack_upgrades.cpp (Pull post: vanilla 7-slot order for the player's model, extra / shadow slots to sidecar; Push pre: spread 7 into 9)

### AmmoSlotDefinition
- methods: `IsAllowed(IdentifiableType)` 0x866B40 - callers: AmmoSlotManager.TryFindSlot (MaybeAddToAnySlot <- Vacuumable.TryConsume, the vac consume coroutine), CouldAddToSlot, MaybeAddResource, ValidateAndAdjustSlots (load)
- fields: `_slotTypeGroup` (player regular slots: "VaccableNonLiquids", leaves Shadow out), `_slotBlockList`, `_maxAmount`
- mod: shadow_catcher.cpp IsAllowed detour (Shadow on published player slots)

### VacuumItem (number keys)
- methods: `SelectSlot(InputEventData)` 0xA69640 - `SetSelectedSlot(round(value))`, value = slot 0..6 (`Optional<float>` at +4 of the data); `SelectSlot(int)` (sound, vac animation; `SetSelectedSlot` refuses locked slots)
- fields: `_selectSlot` - ONE InputEvent for MainGame/Slot1..Slot7; Slot1..6 bound to 1-6, Slot7 (water) to R; no Slot8 / Slot9 actions (an action asset can't be added at runtime without managed code)
- mod: vacpack_upgrades.cpp (SelectSlot(InputEventData) detour remaps vanilla index at/after our slots; own keys polled with GetAsyncKeyState)

### HUD ammo row (corescenes, HudUI/UIContainer/"Ammo Slots")
- 900 x 189, anchored bottom centre, HorizontalLayoutGroup (spacing 10, LowerCenter, no width control) over 108 px slots; Unity centres children only while total width < rect width (9 slots = 1052 px overflow right)
- `AmmoSlotAdapter.PrefilterData` hides locked slots; `AmmoSlotViewHolder.BindInputKey(definition.Hotkey)` logs "No binding set for ammo slot!" for a null hotkey; writes `KeybindingText`
- mod: vacpack_upgrades.cpp (row widened to 1160 px; BindInputKey detour writes our key name)

### Vacuumable
- `Vacuumable.Hold` - held layer, `_Held` property on its renderers (toys held at the nozzle)
- mod: dj_buildables.cpp (radio look)

## From the slime_dj, spawn_scaling, hl2_movement, quick_start, free_placement, misc_tweaks, plot_upgrades, drone_price_limit notes

### VacuumItem (MP.SR.Player.PlayerItems) - shooting (IDA)
- methods: `Update` 0xA6B750 shoot mode: when `Time.fixedTimeAsDouble >= _nextShot`: `f = GetShootSpeedFactor(inVac)`, `Expel(1 / f)` (argument = the shot's audio volume), `_nextShot = now + ShootCooldown / f`; at most one shot per frame
- methods: `GetShootSpeedFactor(HashSet<GameObject>)` 0xA67940 (private): largest `IVacShootAccelerator.GetVacShootSpeedFactor()` among objects in the vac cone, else 1
- methods: `Expel(float shootAudioVolume)` 0xA66C00 (private; resolve by parameter type)
- mod: misc_tweaks detours GetShootSpeedFactor and Expel

### IVacShootAccelerator / VacAccelerationHelper
- implementers: `SiloCatcher` (silo, refinery, storage ports), `ScorePlort` (market), `DroneFuelDepositor`, `ResourceDepositor`; each returns its `VacAccelerationHelper.Factor`
- `VacAccelerationHelper.CreateInput()` 0x99A530: MinFactor 1, MaxFactor 3, Speed 0.5, Duration 1 s; `CreateOutput()` 0x99A590: MaxFactor 1.75. `Factor = min(Min + (now - timeBegin) * Speed, Max)` while `now < timeEnd`, else Min; `OnTriggered` (per item fed) keeps timeBegin, pushes timeEnd 1 s ahead -> 1x to 3x over 4 s, back 1 s after the last item

## From the sandbox, sell_upgrades, collectibles, instant_build notes

### AmmoSlotManager  [PlayerState.Ammo]
- methods: `MaybeAddResource(IdentifiableType id, int slot, int n, bool overflow)` - n must be > 0; empty slot (count 0) first checks `slot.Definition.IsAllowed(id)` and takes the id; then slot id must equal `id`; without overflow `count + n <= MaxCount`. Does NOT check `AmmoSlot.IsUnlocked` (IDA)
- methods: `Decrement(int slot, int n)` - clears the slot at 0 (dump/inferred)
- methods: `GetSlotMaxCount(slot)` (dump)
- methods: `UpdateMaxCount` 0xA3AB70 / `set_MaxCount` 0xA3C180 - only write the slot property's `DefaultValue` (the silo capacity upgrade's call) (IDA)
- fields: `_ammoModel` -> `AmmoModel.Slots` (AmmoSlot[])
- gotchas: MaybeAddResource accepts a locked slot; callers must check IsUnlocked themselves (a locked slot holds nothing in vanilla)
- mod: sb put / stow / move / slot amount (DoPutItem, DoStow, DoMoveSlot); sb upgrades.cpp PruneSlotListeners walks `_ammoModel.Slots`; su STORAGE_CAPACITY_INCREASE uses UpdateMaxCount on the silo (excluded from selling)

### AmmoSlot
- methods: `IsUnlocked` 0xA3BE20 - true if the slot definition's `_isUnlockedProperty` is null, else `_isUnlockedValue` (NullableProperty<float> on that property) >= `_isUnlockedThreshold` (IDA)
- methods: `get_MaxCount` 0xA3BED0 - `floor(_maxCountValue)` (NullableProperty<float>) (IDA)
- methods: `Setup(tracker)` 0xA3B570 - `DefaultValue = definition._maxAmount`, then `UnitPropertyTracker.DoWithProperty(_maxAmountProperty, ...)` (AmmoLimit) binds the value; subscribes `<Setup>b__32_2` / `_3` to the unlock property and AmmoLimit (IDA)
- methods: `<Setup>b__32_2` / `<Setup>b__32_3` 0xA3B960 - one shared body: `OnSlotChanged(this)` (IDA)
- methods: `remove_OnSlotChanged`, `remove_OnAmmoChanged` (events OnSlotChanged, OnAmmoChanged)
- gotchas: unlock and max are property driven (upgrade modifiers), see re/upgrades.md; `AmmoSlotsAvailable` default 4; Extra Tank I-II +1/+2 ADD modifiers; custom_upgrades extra slots 8/9 have thresholds 7/8
- gotchas: game keeps counts above a lowered maximum (takes nothing more); a re-locked slot keeps its contents hidden
- mod: sb slot unlock (DoUnlockSlot), upgrades.cpp PruneSlotListeners

### AmmoSlotDefinition
- methods: `IsAllowed(type)` - false if in `_slotBlockList`, true if `_slotTypeGroup` null, else group runtime membership (IDA)
- fields: `_isUnlockedProperty`, `_isUnlockedThreshold`, `_maxAmount`, `_maxAmountProperty`, `_slotBlockList`, `_slotTypeGroup`
- mod: sb vacpack catalog (slot mask per item)

### AmmoSlotViewHolder / AmmoSlotAdapter (HUD slot row)
- methods: `AmmoSlotViewHolder.BindData` 0x67EFE0 - subscribes `UpdateAmmoDisplay` to the slot's OnSlotChanged and OnAmmoChanged; only unsubscribes from the previous slot when bound again; no OnDestroy (IDA)
- methods: `AmmoSlotViewHolder.UpdateAmmoDisplay` 0x67FE80 - Image.set_sprite etc. (IDA)
- methods: `AmmoSlotAdapter.OnSlotChanged` 0x82F280 -> `Refresh` -> `AdapterView.BindDataList(List,int)` 0x1F281F0 (`PrefilterData` = unlocked slots), `Destroyer.Destroy`s holders past the new count; `AmmoSlotAdapter.Start` 0x82F3F0 never unsubscribes (IDA)
- gotchas: fewer unlocked slots (never in vanilla; a lowered Extra Tank) leaves DESTROYED view holders subscribed to the slots -> NRE on the next slot notification (see re/upgrades.md chain). Destroy runs at end of frame
- mod: sb upgrades.cpp PruneSlotListeners removes invocation entries whose `Delegate.get_Target` is a destroyed UnityEngine.Object (m_CachedPtr null)

### Vacuumable / vacpack storability
- facts: an IdentifiableType is vacpack-storable when not IsPlayer/IsSceneObject, its prefab has `Vacuumable` with `Size == NORMAL` (LARGE/GIANT, e.g. largos, are held only), and some slot's `IsAllowed` accepts it (inferred, dump)
- mod: sb vacpack catalog

### VacuumItem
- chains: `VacuumItem.Expel` -> `InstantiationHelpers.InstantiateActor(..., RegionRegistry.CurrentSceneGroup, ...)` (the vacpack's shoot-out path; sb Spawn reuses it, see re/world.md)

### PlayerState (currency, health, energy)
- methods: `GetCurrency(ICurrency)`, `AddCurrency(cur, n, showUiNotification, null)` 0x87A9A0 -> `PlayerModel.AddCurrency` (clamps to `PlayerModel.maxCurrency`, default 999,999,999; raises `PlayerModel.CurrencyChanged`; counts towards "ever collected") (IDA)
- methods: `SpendCurrency(cur, n, null)` - throws ArgumentException when the player has less (dump/IDA)
- methods: `HandleModelCurrencyChanged` 0x87B330 -> `DisplayCurrencyChanged` (HUD `CurrencyDisplayList`, unscaled time) (IDA)
- methods: `GetMaxHealth`/`SetHealth(int)`/`GetCurrHealth`, `GetMaxEnergy`/`SetEnergy(int)`/`GetCurrEnergy`, `SetRad(0)`, `AddKey` (exists, unused)
- methods: `Damage(Damage)` 0x87AEB0 (unique body) - returns false if fast-forwarding or `timeScale <= 0`; else `PlayerModel.LoseHealth(damage.Amount)`; returns true (dead) when health floors to 0 (sets `currHealth = 0`, `healthBurstAfter = inf`) (IDA)
- gotchas: Newbucks = `CurrencyUtility.DefaultCurrency`, a `CurrencyDefinition` with `_persistenceId == 1`
- mod: sb Newbucks tools, refills, infinite energy (10 Hz top-up), player_extras.cpp infinite health (Damage detour returns false without calling the original; plus 10 Hz top-up)
