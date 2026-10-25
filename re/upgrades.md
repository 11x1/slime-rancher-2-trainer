# Player upgrades and modifiers

`MP.SR` = `MonomiPark.SlimeRancher`. Mod use: `modules/custom_upgrades/vacpack_upgrades.cpp` (extra slots, tiers, Battery Saver, Shadow Catcher cards: clones of vanilla `UpgradeDefinition`s, level in the sidecar), `fabricator_levels.cpp`, `modules/sandbox/` (upgrades manager). Hooks: re/hooks.md.

- `UpgradeDefinition`s: `LookupDirector._upgradeDefinitions` (icons for vac pack upgrades).
- Fabricator category: `PlayerUpgradeFabricateCategory(Runtime)` (re/fabricator.md).

## From the custom_upgrades notes

### UpgradeDefinition
- fields: `_levels` List<UpgradeLevelDefinition> (Effects, `_costs`, Title, Description); `LevelCount` = `_levels.Count` everywhere (Fabricator card, details, SetUpgradeLevel range check, IncrementUpgradeLevel / UpgradeLevelExist, ActorUpgradeHandler.ApplyUpgrade); `_uniqueId`
- methods: `GetUpgradeLevel` throws for a level past LevelCount (so the save must never hold one)
- gotchas: `Object.Instantiate(AmmoCapacity)` deep-copies the level objects (checked)
- mod: vacpack_upgrades.cpp (appends level objects to AmmoCapacity / AmmoSlots every tick, looked up by id)

### UpgradeModel (DataModel)
- methods: ctor 0xAD9FA0 - `upgradeLevels` Dictionary<int,int> keyed by _uniqueId, -1 for every listed definition
- methods: `SetUpgradeLevel` 0xAD9C20 - throws for level >= LevelCount, stores the level, TinkerTailorSlimes progress = CountUniqueUpgrades(), raises OnUpgradeChanged -> `ActorUpgradeHandler.ApplyUpgrade(def, level)` (level's effects replace that upgrade's previous ones)
- methods: `IncrementUpgradeLevel` 0xAD9510 = SetUpgradeLevel(level+1) while UpgradeLevelExist
- methods: `Pull` 0xAD99C0 - NEW Dictionary copy for the save; `Push` 0xAD9A40 - writes saved levels straight into the dictionary (no SetUpgradeLevel), only for keys the model has
- gotchas: a throwing OnUpgradeChanged subscriber unwinds past post-call code (write side effects before the original); `ActorUpgradeHandler.SetModifiersFromModel` asks every listed definition for its level (register the id in upgradeLevels before adding a definition)
- mod: vacpack_upgrades.cpp (Pull post clamps ids 20/40 to 7/1 and removes own ids; SetUpgradeLevel detour writes the sidecar first)

### Vanilla vacpack upgrade data (duplicateassetisolation_assets__54b00e46... + corescenes bundles)
- `AmmoCapacity` (_uniqueId 20) Tank Booster I-VIII: one FloatModifier ADD per level on `AmmoLimit` (default 20): +10 ... +80 -> 30 ... 100 per slot (levels replace, not sum); 450 / 1000 / 3000 / 10000 / 15000 / 20000 / 30000 / 50000 Newbucks + items (V: 50 Sloomber, 20 Stable, 20 Black Indigonium, 1 Royal Jelly; VI: 60 Twin, 25 Stable, 20 Dream Bubble, 3 Sun Sap; VII: 70 Hyper, 50 Sloomber, 20 Magma Comb, 3 Royal Jelly; VIII: 80 Ringtail, 50 Twin, 30 Aqua Glass, 3 Royal Jelly); II+ need `TankBoosterComponent`
- `AmmoSlots` (40) Extra Tank I-II: ADD +1 / +2 on `AmmoSlotsAvailable` (default 4); 1500 / 5000 + items; II needs `ExtraTankComponent`
- `LiquidSlot` (80) = Water Tank
- components are counted items from treasure pods / shop; world holds no more than vanilla needs
- mod: vacpack_upgrades.cpp

### PlayerUpgradeFabricateCategory / Runtime
- methods: `PlayerUpgradeFabricateCategory.GetItems` 0x82C000 - NO native caller (metadata only)
- methods: `PlayerUpgradeFabricateCategoryRuntime..ctor(category, recipeFactory)` 0x82BE90 - inlined `_upgradeDefinitions.Select(new PlayerUpgradeFabricatableItem(...)).Cast().ToList()` -> FabricateCategoryRuntime<T>..ctor (`_categoryCount` = Count(_upgradeDefinitions) @0x18, `_itemList` @0x20)
- gotchas: the card shows the next level (GetUpgradeLevel + 1, clamped to LevelCount - 1): title, costs, "fully upgraded"; player's `UpgradeModel.upgradeDefinitions` may be the same asset as the category list (unverified)
- mod: vacpack_upgrades.cpp ctor detour (pre: card appended to the category holding id 20; post: removed from `_itemList` while locked, `_categoryCount` lowered)

### Save of upgrades
- `ViewedItemsV02` may receive own upgrade ids (harmless)

## From the sandbox, sell_upgrades, collectibles, instant_build notes

### UpgradeModel  [PlayerModel.GetUpgradeModel()]
- methods: `GetUpgradeLevel(def)`, `IncrementUpgradeLevel(def)` 0xAD9510 - returns false at last level (`UpgradeLevelExist`), else `SetUpgradeLevel(level + 1)` (IDA)
- methods: `SetUpgradeLevel(def, level)` 0xAD9C20 - range -1 .. LevelCount-1 (else throws), writes `upgradeLevels`, `MetaGameDirector.SetAchievementProgress(TinkerTailorSlimes, ...)`, then `OnUpgradeChanged(def, from, to)` (IDA)
- methods: `add_OnUpgradeChanged` 0xADA410 - only subscriber besides ctor no-op is `ActorUpgradeHandler.SetModel` (xrefs)
- fields: `upgradeDefinitions` (list; vanilla first, custom_upgrades tiers behind their parent), `upgradeLevels`
- gotchas: levels replace each other (not additive); levels can jump; -1 = not bought
- mod: sb upgrades.cpp SetLevel / Max / Undiscover; module.cpp DoUnlockSlot, DoUnlockUpgrades (Max all, legacy op); custom_upgrades detours SetUpgradeLevel (sidecar for tiers past vanilla max)

### ActorUpgradeHandler
- methods: `OnUpgradeChanged` 0x865980 - calls ApplyUpgrade when level differs (IDA)
- methods: `ApplyUpgrade(def, level)` 0x8646B0 - builds new modifiers per property from `_levels[level].Effects` (none at -1), calls `ModifiableFloat.BatchModifiers(add, remove)`, and only at the end stores new nodes in `_appliedModifiers[_uniqueId]` (`List<AppliedModifier>`, struct {Property @0, Node @8}) (IDA)
- methods: `SetModifiersFromModel` - load builds modifiers from saved levels (modifiers are runtime only)
- chains: `UpgradeModel.SetUpgradeLevel` -> `OnUpgradeChanged` -> `ActorUpgradeHandler.ApplyUpgrade` 0x8646B0 -> `ModifiableValue<float>.BatchModifiers` 0x1814D3730 (`Select(AddLast).ToList()`, `ForEach(Remove)`, then 0x1800AC530 recomputes the value and invokes `OnValueChanged` with no null check) -> `AmmoSlot.<Setup>b__32_2/_3` 0xA3B960 (`OnSlotChanged(this)`) -> subscribers incl. `AmmoSlotViewHolder.UpdateAmmoDisplay` 0x67FE80 -> `Image.set_sprite` on a destroyed holder -> `UnityEngine.Bindings.ThrowHelper.ThrowNullReferenceException` 0x345DA50 (plain NRE in player builds) (IDA)
- gotchas: a throw between BatchModifiers' add and the `_appliedModifiers` store LEAKS the added modifiers (unremembered): next change stacks the next level on top (Tank Booster to 1880 per slot). Removed-but-remembered nodes make every later ApplyUpgrade of that upgrade throw "The LinkedList node does not belong to current LinkedList"
- gotchas: the Fabricator purchase path (IncrementUpgradeLevel) goes through the same notification, so the NRE is not specific to calling SetUpgradeLevel directly
- mod: sb upgrades.cpp `Heal` (effects repair) and `PruneSlotListeners`

### ModifiableFloat / ModifiableValue<float> / UnitPropertyTracker
- methods: `UnitPropertyTracker.TryGetProperty`, `DoWithProperty`; `ModifiableFloat.RemoveModifier`; `BatchModifiers` 0x1814D3730
- fields: `Modifiers`: `SortedList<int, SortedList<int, LinkedList>>` (walked via `values` / `_size`, LinkedList `head` / `count`, node `next` / `item` / `list`)
- gotchas: `LinkedListNode.list` null = node no longer in a list
- mod: sb upgrades.cpp Heal

### UpgradeDefinition
- fields: `_levels[n]` (UpgradeLevelDefinition: `Effects` = `List<FloatModifier>` with `TargetProperty` in the generic base, `_costs` with `ComponentCosts`, `Title`), `_icon`, `_uniqueId` (20 AmmoCapacity / Tank Booster: vanilla 8 levels; 40 AmmoSlots / Extra Tank: vanilla 2), `_requireComponentToBeVisibleToPlayer`, `LevelCount`
- gotchas: FloatModifiers are ADDs relative to the property default; several upgrades can stack on one property (custom tiers on top of vanilla)
- mod: sb upgrades manager state, slot unlock (`SlotUnlockUpgrades`)

### PlayerUpgradeFabricatableItem
- card picture (IDA, round 11): `get_Icon` / `get_SmallIcon` 0x67DED0 = `UpgradeDefinition._icon` (grid tile); `get_FullArtReference` 0x82B5D0 / `get_RequiresFullArt` 0x82B920 = `UpgradeDefinition.get_FullArtReference` 0x99A410 / `get_RequiresFullArt` = its `_pediaEntry`'s (virtual). A cloned UpgradeDefinition keeps the original's pedia entry, so the Fabricator details show the original's full art. `FabricatorItemDetails.SetItem` 0x8089B0 -> `UIImageDynamicLoader.SetFullArtSource(item)` 0x7B52A0 (event with the item's icon, then `TryAcquireCurrentHandle` loads the full art). Shadow Catcher: `_pediaEntry` = `PediaDirector.GetEntry(Shadow slime)` (an `IdentifiablePediaEntry`: `FullArtReference` / `RequiresFullArt` / `Icon` from its `_identifiableType`). mod: custom_upgrades vacpack_upgrades.cpp `ShadowPedia`
- methods: `FabricateAndSpendCost` 0x82AFA0 - `SpendableDirector.Spend`, analytics, `IncrementUpgradeLevel`, `_eventProducer.RaiseEventForData(def)` (IDA)
- methods: `get_ShouldShowPlaceholder` 0x82B940 (`get_IsMissingBlueprint` returns the same) - true while not bought (level < 0), `_requireComponentToBeVisibleToPlayer` set, and a level-0 `_costs.ComponentCosts` entry missing (`SpendableDirector.HasUpgradeComponent(c, 1)` false); `CheckCanFabricate` then returns MISSING_BLUEPRINT (IDA)
- gotchas: "discovering" an upgrade = owning its level-0 upgrade component (treasure pod pickup); buying level 0 spends it
- mod: sb upgrades.cpp discover state; module.cpp DoDiscoverUpgrades (legacy op)

### Upgrade components
- methods: `AcquisitionUtility.AcquireUpgradeComponent(component, n)` - the pickup path; `SpendableDirector.GetUpgradeComponentAmount`, `HasUpgradeComponent`
- methods: `UpgradeComponentsModel.LoseComponent(component, n)` 0xA55D40 - removes the dict entry at 0, raises `OwnedComponentsChanged` (IDA)
- fields: `SpendableDirector._upgradeComponentsModel`
- mod: sb Discover / Undiscover, Fabricator G key
