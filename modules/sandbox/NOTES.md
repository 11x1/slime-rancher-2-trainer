# sandbox

Edits the character's resources from the overlay's **Misc > Sandbox** tab: Newbucks, health / energy / radiation, what each vacpack slot holds (and unlocking locked slots), spawning items in front of the player, crafting fabricator gadgets at no cost, adding crafting materials to the refinery, infinite health, teleporting to the map marker and unlocking the ranch expansions. Off by default. While it is on, achievements are paused (see "Achievements paused").

## Turning it on

- The module's only setting is the automatic `enabled` (default **off**, persisted in `sr2mods.json` like every module's).
- While it is off, the tab shows a short explanation and an **Enable sandbox** button. The button, and the tab bar's Enabled switch, open a confirmation dialog ("Sandbox edits change your save directly. Enable?", Cancel / Enable; Esc cancels). Only **Enable** writes `sandbox.enabled = 1`. Turning it off needs no confirmation.
- No other path turns it on by accident: the in-game Options "Mods" tab has no Sandbox row (`options_tab.cpp` skips the module), and profiles neither save nor load `sandbox` (`core/profiles.cpp`, `Skipped()`), so loading a shared profile can't enable it. Editing `sr2mods.json` by hand still works.

## Save safety

Every change goes through the game's own methods, the ones it uses for pickups, purchases and vacuuming, so their validation applies. Values are clamped to what a vanilla game can hold. Nothing is written into the save file; the game saves the result with its next save.

| tool | game methods | limits |
|---|---|---|
| Newbucks set / add / subtract | `PlayerState.GetCurrency(ICurrency)`; more: `PlayerState.AddCurrency(cur, n, showUiNotification: true, null)` (like a pickup or sale; also counts towards "ever collected"); less: `PlayerState.SpendCurrency(cur, n, null)` (like a purchase) | target clamped to `0..PlayerModel.maxCurrency` (default 999,999,999; `PlayerModel.AddCurrency` clamps to the same field). The currency is `CurrencyUtility.DefaultCurrency`, accepted only if it is a `CurrencyDefinition` with `_persistenceId == 1` (`NEWBUCK_PERSISTENCE_ID`). Both show the game's coin popup |
| Refill health / energy | `GetMaxHealth` + `SetHealth(int)`, `GetMaxEnergy` + `SetEnergy(int)` | exactly the maximum |
| Clear radiation | `SetRad(0)` | |
| Infinite energy | frame callback, every 0.1 s: `GetCurrEnergy` < `GetMaxEnergy` -> `SetEnergy(max)` | session only (not a setting); off on restart and when the module is turned off |
| Slot amount (set / fill / empty) | up: `AmmoSlotManager.MaybeAddResource(id, slot, n, overflow: false)`; down: `AmmoSlotManager.Decrement(int slot, int n)` (clears the slot at 0) | `0..GetSlotMaxCount(slot)`; only unlocked slots (`AmmoSlot.IsUnlocked`), only a slot that holds something |
| Put item | `Decrement` (empty the slot if it holds something else), then `MaybeAddResource` | the item must be in the catalog (below), saveable (below) and accepted by that slot; `1..max`. The tab asks for the item's chosen amount, else the slot's maximum |
| Put in slots (`Op::StowItem`, one per selected item) | per slot in order from the selected one: `Decrement` (only the selected slot, only for the first item), `MaybeAddResource(id, slot, room, overflow: false)`; the rest through the spawner's `InstantiateActor` (below) | the item must be in the catalog and saveable; only unlocked slots that accept it and hold it or are empty (other items are never removed, except the first item's selected slot); amount `<= 0`: the first slot's maximum (nothing is dropped then); at most 60 dropped per item (wider rows of 10), only if the item is also in the spawn list |
| Move slot | merge: `MaybeAddResource` on the target, then `Decrement` the source; onto an empty slot: as much as the target holds, the rest stays; swap: `Decrement` both, `MaybeAddResource` each into the other | both slots unlocked, both items saveable; a swap only when each side fits the other slot's maximum; if re-adding fails, both slots get their old contents back. Slime emotions in the slot are not carried over (the slot is re-filled by id) |
| Unlock slot | `PlayerModel.GetUpgradeModel()`, `UpgradeModel.GetUpgradeLevel` / `IncrementUpgradeLevel(def)` (what `PlayerUpgradeFabricatableItem.FabricateAndSpendCost`, RVA 0x82AFA0, does after spending) | only up to the level that unlocks the slot (below); never past it |
| Spawn | `InstantiationHelpers.InstantiateActor(prefab, RegionRegistry.CurrentSceneGroup, pos, identity, nonActorOk: false, NONE, NONE, metadata: null, false, setCollected: false)` | the item must be in the spawn list (below) and saveable (below); no Tarrs while they are disabled; 1..20 per click |
| Craft | `GadgetDirector.HasBlueprint` (for the message only), `CanAddGadget(def, n)`, then `GadgetDirector.AddItem(def, n)` | the gadget must be in the recipe list (below) and `n` within its limit; 1..99 crafts per request (x2 for `BuyInPairs`). A locked blueprint stays locked (see Crafter) |
| Add material | `GadgetDirector.GetRefinerySpaceAvailable(t)`, then `GadgetDirector.AddItem(t, n)` | the type must be in the materials list (below), saveable; `1..space` (the refinery holds 999 per type) |

### Only saveable items

Fixed (2026-10-24): a Glitch Tarr from the spawner made **every** save fail. `Player.log`: `KeyNotFoundException: The given key 'SlimeDefinition.GlitchTarr' was not present in the dictionary` at `GameModelPullHelpers.BuildActorData` <- `PullActors` <- `PullGame` <- `AutoSaveDirector.SaveGameImpl`. The save aborts before writing, so the files were safe, but nothing saved while the Tarr existed.

The dictionary (1.3.0): `GameContext.AutoSaveDirector._saveReferenceTranslation` (`SaveReferenceTranslation`, AutoSaveDirector +0x40) -> `_identifiableTypeToPersistenceId` (`PersistenceIdLookupTable<IdentifiableType>`, +0x20) -> `_reverseIndex` (`Dictionary<string, int>`, keyed by `IdentifiableType.ReferenceId`, e.g. `SlimeDefinition.GlitchTarr`). `SaveReferenceTranslation.GetPersistenceId(IdentifiableType)` does `_reverseIndex[ReferenceId]` with no fallback. It is built by `SaveReferenceTranslation.OnIdentifiableTypesListChanged` (from `AutoSaveDirector.Awake` and `OnAdditionalContentAdded`) from `AutoSaveDirector._configuration._identifiableTypes.GetAllMembers()` (an `IdentifiableTypeGroup` asset), non-null entries only. A type outside that group exists as an asset (so `FindObjectsOfTypeAll` sees it) but can never be saved.

The module reads that dictionary (the two generic classes are resolved from the live objects) and keeps a type in the vacpack list, spawn list or crafter only if `ContainsKey(type.ReferenceId)` is true; each put / spawn / craft checks again. The left-out types are logged once per list (`[sandbox] spawn list: left out N type(s) the save can't store: ...`). If the dictionary can't be read yet, the list is built on a later refresh; if its symbols are missing, nothing is offered (the spawner and crafter show "save check unavailable"). The vacpack and crafter lists use the same check because ammo slots and the gadget inventory are also saved through `ISaveReferenceTranslation.GetPersistenceId` (not traced one by one; the check costs nothing for types that pass).

### Tarrs disabled

When the save's game settings have Tarrs off (`GameContext.GameSettingsDirector.TarrEnabled`, a `ScriptedBool`), spawn-list entries in the game's own Tarr group (`SceneContext.GameModel.tarrTypeGroup`, `IdentifiableTypeGroup.IsMember`, read when the list is built) are grayed out with the tooltip "Tarrs are disabled in this save's game settings." Clicking one sets an error status; the module refuses the request too. The setting is read with every snapshot (4 Hz while the tab is open). Without these symbols nothing is grayed out (logged).

Notes from the disassembly (1.3.0):
- `MaybeAddResource`: count must be > 0; an empty slot (count 0) first checks `slot.Definition.IsAllowed(id)` and takes the id; then the slot's id must equal `id`; without overflow `count + n <= MaxCount`. It does **not** check `IsUnlocked`, so the module checks that itself (a locked slot can't hold anything in vanilla).
- `AmmoSlotDefinition.IsAllowed`: false if the type is in `_slotBlockList`, true if `_slotTypeGroup` is null, else the group's runtime membership.
- `SpendCurrency` throws `ArgumentException` when the player has less than asked; the module never asks for more than the current amount.

## Vacpack item catalog

Items a vanilla vacpack can store: every loaded `IdentifiableType` (`Resources.FindObjectsOfTypeAll`) that is not `IsPlayer` / `IsSceneObject`, whose `prefab` has a `Vacuumable` with `Size == NORMAL` (LARGE / GIANT ones, such as largos, are only held, never stored), and that at least one slot's `AmmoSlotDefinition.IsAllowed` accepts. Each item carries a slot mask, so the tab lists only what the selected slot accepts. Names: `localizedName.GetLocalizedString()` (rich-text tags stripped), else the asset name; duplicates get the asset name appended.

The catalog is built once (logged with its size and time) and rebuilt only when the slot definitions change. Entries are kept alive with GC handles; old handles are freed in a posted main-thread job. The UI gets item ids as opaque numbers (the type's address) and never dereferences them; a request is checked against the catalog (handle target, class, `IsAlive`) before use.

Each item (and each slot's current item) also carries an **icon** and a **kind**, both cached per type:
- Icon: registered once with `ui/item_icons` (texture kept alive, D3D11 resource and sprite rectangle cached); the tab only gets the small id. The first that exists:
  1. `IdentifiableType.icon`.
  2. The type's own `get_Icon`, resolved on its class (virtual). `SlimeDefinition.get_Icon` (0xBAF310) returns `GetAppearanceForSet(...)._icon`, the default appearance's icon, so a largo whose `icon` field is empty gets its appearance icon (1.3.0 disassembly; what largos' appearances carry is not verified).
  3. A largo (`SlimeDefinition`) with neither: the icons of its first two `BaseSlimes`, sent as `icon` + `icon2`; the tab draws them overlapping (top left / bottom right, 70 % each).
  4. The icon of a group it belongs to: its own `IdentifiableType.groupType`, else the first `IdentifiableTypeGroup` asset with an `_icon` whose `IsMember` is true (leaf groups, without `_memberGroups`, first). The groups with an icon are read once (one `FindObjectsOfTypeAll`, logged with their asset names) and only asked for types without any icon of their own.
  5. Rendered from the item's prefab (below, "Icons rendered from prefabs"), for the types that have none of the above.
- Kind (tile accent, group tabs and tooltip tag): `SlimeDefinition` (largo if `IsLargo`), `GadgetDefinition`, `ToyDefinition`, `LiquidDefinition`, `IsPlort`, a member of the game's `StandardCrateClass` group (Crate), `IsAnimal` (Meat, in the Food tab), a member of `FoodGroup` (Food), of `CraftGroup` (Resource), anything else the refinery stores (`GadgetDirector.IsStorable`: Resource), else the asset names of its `Categories` (the old guess), else Other.

### Item kinds from the game's groups (2026-10-24)

Seen in game (2026-10-24): the Resources tab was empty and the Food tab showed only chickens. The kinds came from the asset names of `IdentifiableType.Categories`, and none of them contain "food" / "veggie" / "craft" / "resource"; only `IsAnimal` (chickens) worked. The game's own groups say it instead (read offline from `identifiabletypegroups_assets_*.bundle` with UnityPy, 1.3.0): `FoodGroup` = `MeatGroup` + `VeggieGroup` + `FruitGroup` + `NectarFoodGroup`; `CraftGroup` = `ResourceOreGroup` + `ResourceHiveGroup` + `ResourceSpoutGroup` + `ResourceWeatherGroup`; `StandardCrateClass` = the 20 crates (`ContainerBluffs01` ... `ContainerStrandGold01`); `LargoGroup` = the 19 `<Slime>LargoGroup`s; `GiantEggGroup` (7), `VineBulbGroup` (1), `RingtailStatueGroup` (2: the Mysterious Statue, large and small), `ToyGroup`, `GadgetGroup` = `GadgetUtilitiesGroup` + `GadgetWarpGroup` + `GadgetDecorGroup` + `GadgetHiddenGroup` (variants). None of the slime, largo, egg, bulb, statue or crate groups has an `_icon`.

The module reads every `IdentifiableTypeGroup` once (the same `FindObjectsOfTypeAll` as the icon groups, the first one-time step of a refresh, before any item kind is cached), keeps them by asset name and collects the members of `FoodGroup`, `CraftGroup` and `StandardCrateClass` with their member groups (`_memberTypes` / `_memberGroups`, field reads only). Logged: `item kinds from the game's groups: N food (FoodGroup), N crafting resources (CraftGroup), N crates (StandardCrateClass)`. After the spawn list: `spawn list by tab: slimes N, largos N, plorts N, food N (meat N), resources N, crates N; only under All: toys N, other N`.

Names: a type whose localized name is missing (`GetLocalizedString` returns `!! <key> !!`, as for the crates: `!! Actor.l.crate_reef_01 !!`) gets a name from its asset name instead: `ContainerBluffsGold01` -> "Bluffs Gold Crate".

### Group tab icons

The vacpack grid has icon-only tabs All / Plorts / Slimes / Food / Resources (Food includes `IsAnimal` meat). The spawner has All / Plorts / Slimes / Largos / Food / Resources / Crates, each only while it has items. The icons are read once, after the spawn list:
- From the drone station's target menu data: the `DroneTargetDefinitions` asset (`MonomiPark.SlimeRancher.UI.Drone.Data`, a `ListAsset` of `DroneUIProgramDefinition<DroneTaskTargetType>` with `_type` / `_icon`, fields read on the live objects). `PLORTS` (2) -> Plorts, `VEGGIES` (3) -> Food, `CRAFTING_MATS` (8) -> Resources. There is no "food" or "slimes" target type (enum: NONE, SINGLE, PLORTS, VEGGIES, FRUIT, MEATS, ELDERS, PLORTS_NO_SPECIALS, CRAFTING_MATS), and the drone station's UI scene bundle has no slime sprite (its sprites, read offline: `iconPlortsPedia`, `iconCategoryResources`, `iconCategoryMeatElder`, drone faces, battery and source icons).
- Slimes and largos (2026-10-24; the log showed `slimes 0`: the user saw a letter "S"): no group has a slime icon, and no multi-slime sprite was found in the game's bundles. The Slimepedia's own icons are used when loaded: `iconCategorySlimes` (pedia / shop UI scenes; a single pink slime) or `pediaHomeSlimes` (the Slimepedia home's slime, shared bundle), and `iconLargoPedia_small` / `iconLargoPedia` for largos. They are looked up by name once (`EnsureNamedSprites`: one `FindObjectsOfTypeAll(Sprite)`, logged as `game sprites by name: N of M wanted found among K loaded sprites`); only sprites that are loaded at that moment are found. Without them the slime tab takes the Pink Slime's own icon (`SlimeDefinition` asset `Pink`), and any tab without an icon takes its first item's icon (the largo and crate tabs: the first largo, the first rendered crate), so no tab shows a letter once the lists are there.
- Anything still missing from the type groups with an icon, by asset name: "plort", "food", "craft". The chosen ids are logged (`[sandbox] group tab icons: ...`).

### Slime diets (favourites)

Read once after the spawn list, for every slime and largo in it: `SlimeDefinition.Diet` (`SlimeDiet`) -> `FavoriteIdents` (favourite foods) and `ProduceIdents` (plorts), with the reverse lookups (food -> slimes that love it, plort -> slimes that make it; base slimes before largos). Sent as `Snapshot::relations`, keyed by item id; the tab uses it for the tooltip rows, the related-tile highlight and the right-click "Give ..." entries (`ui/NOTES.md`). The `EatMap` and `MajorFoodIdentifiableTypeGroups` (everything a slime eats) are not used.

Fixed (2026-10-23): picking an item in the old list did nothing. The list looked up the picked item's name inside its loop, before the click on that row was handled, then cleared the pick when the name was missing, so every new pick was undone in the same frame and **Put** never enabled (the log shows no `Put` attempt at all). The game side (`MaybeAddResource` arguments, `IsAllowed`) was fine.

### Put in slots (2026-10-24)

Reported: "Put N in slots" did nothing at all, and the excess was not dropped. The log of that session has no `[sandbox]` line for it, so no request reached the module. The old button sent one `PutItem` per selected item from the tab's own (up to 250 ms old) snapshot: the selected slot for the first item, then only **empty** slots after it, and silently skipped every item that found none. Picking items with Shift+wheel was also limited to the selected slot's maximum, so there was never anything left over to drop. Which of these hit the user's case is not known (no line was logged); the button now always leaves a trace:
- the tab logs `[sandbox] tab: Put in slots, N item(s), selected slot S` on the click;
- each item is one `Op::StowItem`, applied on the main thread in order against the live slots (so the second item sees what the first one took), with one summary line per item: `Put in slots: Pink Slime -> slot 3 +50, slot 5 +20; dropped 10 in front of you` (or why nothing went anywhere: no unlocked slot takes it, slots hold other items, the game refused, can't be dropped because it is not in the spawn list); the spawner logs its own `Spawned ...` line for the dropped part.

Amounts in the vacpack grid now go up to 999 (Shift+wheel 1, Ctrl+wheel 10), so asking for more than fits is possible; what doesn't fit is dropped in front of the player with the spawner's placement (rows of 10 above 20 items), at most 60 per item. The give menu uses the same op without replacing anything (amount: one slot's maximum).

### Fabricator cost lines

`Recipe::costs` carries the same costs as the `cost` text, one `CostLine` per ident cost (text, the type's icon and kind) or currency cost (text, `currency = true`), so the crafter tooltip lists them one per line with icons. Read in `RecipeCost` when the recipe list is built (the ident icons go through the usual `ItemIcon` cache).

## Spawner

Layout (2026-10-24): the count slider, then the search box with the group tabs on a row of their own under it, right-aligned (`item_grid::Options::tabsBelow`; the vacpack keeps its tabs left of the search box). The Largos tab is grouped by base slime: a section per slime (heading and rule), each largo under the alphabetically first of its two base slimes (`SlimeDefinition.BaseSlimes`, display names, sent as `Item::family` / `family2`), sorted by the other one inside it. Alphabetical order by name alone did not group them: the game's largo names do not always start with the same slime.

Feedback (2026-10-24): a successful spawn no longer writes "Spawned N x Name" into the tab's status line. It shows the game's own HUD notice instead, the one the quantum drones raise for a cloud upload (icon, name, count): `DroneActivitySceneNotifications.AddResourceCollected(IdentifiableType, int)` through `custom_upgrades/sale_notify` (the Sell-All notices' class, which now takes a notice kind; see `modules/custom_upgrades/NOTES.md`). The notices wait while a game menu hides the HUD (the list would discard them) and are dropped after 2 minutes. Crafts and refinery adds use the same notice. Errors still go to the status line; a success clears it. Without the HUD list (`Available()` false) the status line says it as before. The log line stays.

List: every loaded `IdentifiableType` whose `prefab` has an `IdentifiableActor` component (the game's loose actors: slimes, largos, plorts, food, toys, crafting resources, ...), except `IsPlayer`, `IsSceneObject` (gordos and other scene objects), `GadgetDefinition` (placed through gadget mode, `GadgetDirector.InstantiateGadget`), `LiquidDefinition` and types the save can't store (see "Only saveable items"; the vacpack list and the crafter are filtered the same way). Built once on a refresh in game (logged; tried again on the next enable if it came out empty), kept with GC handles, sorted by name. Types with a prefab but no `IdentifiableActor` are logged once by asset name (`spawn list: N type(s) with a prefab but no IdentifiableActor left out: ...`), so a missing tile can be traced. Read offline: the crates, giant eggs, the vine bulb and both Mysterious Statues (`RingtailStatueLarge` / `Small`) are in the save's type list; the 2026-10-24 log shows a Giant Egg and a crate spawned, so they were listed, only without an icon (the crates also without a name). Each refresh builds at most one of the lists and one-time extras (type groups, sprites by name, vacpack catalog, spawn list, recipes, crafting materials, slime diets, group tab icons, one prefab icon), so the one-time scans land in separate frames.

### Icons rendered from prefabs (2026-10-24)

The types without any icon (read offline: the 20 crates, the 7 giant eggs, the vine bulb, the 2 Mysterious Statues, the Echo Net, the pollen cloud, the drone component and the largos without an appearance icon, which get the composed icon instead) get one rendered from their prefab (`prefab_icons.cpp`, header comment for the details). Nothing of the prefab is instantiated (its scripts would register an actor): its enabled MeshRenderers / SkinnedMeshRenderers on active objects (LOD1+ skipped) are copied as bare MeshFilter + MeshRenderer objects (shared mesh and materials) on layer 31, 10 km up; a disabled orthographic camera culling only layer 31 and a directional light culling only layer 31 render them into a 128 x 128 RenderTexture (`Camera.Render`), `Texture2D.ReadPixels` reads it back, the background becomes transparent (the clear alpha if the pipeline kept it, else the magenta clear colour is keyed out), then `Sprite.Create` and `item_icons::Register`. Everything created is destroyed with `DestroyImmediate` in the same call. One icon per refresh (4 Hz while the tab is open), each type once; the lists are copied with the new icon. Logged: the list of items without an icon after the spawn list, the first 5 failures with the reason, and `rendered N icon(s) from prefabs, M failed (T ms in all)`. After 6 failures in a row it stops trying. **Not seen in game:** that `Camera.Render` draws with the game's render pipeline from a frame callback, the lighting, the framing (prefabs assumed to face +Z), and the cost per icon (one render plus a GPU readback).

Spawning uses the vacpack's own path: `VacuumItem.Expel` calls `InstantiationHelpers.InstantiateActor` with `RegionRegistry.CurrentSceneGroup`, which creates the actor model (`GameModel.InstantiateActorModel`), instantiates the prefab, registers it with `DynamicObjectContainer` and initialises the model, so the spawned item is saved like any loose item. The `Nullable<AmmoMetadata>` argument is passed as a zeroed struct (`hasValue = false`; runtime_invoke takes nullable arguments as a pointer to the struct, and `InstantiateActor` only reads it for slimes after checking `hasValue`).

Placement: the main camera's position and its forward direction flattened onto the ground (the body's facing when looking straight up/down), 2.5 m ahead and 0.3 m below eye height; several items go side by side (5 per row, 0.55 m apart, rows 0.7 m higher). An optional `Physics.Raycast(origin, dir, out hit, 3.1 m, DefaultRaycastLayers, QueryTriggerInteraction.Ignore)` pulls the spot in front of a wall (hit distance - 0.6 m, at least 0.5 m).

## Crafter

List: every `GadgetDefinition` asset whose `CraftingCosts` has ident or currency costs (what the fabricator charges for), read from the game data, not hardcoded. Grouped by the fabricator's own categories when its `FabricateMenuConfiguration` asset is loaded (`_categories`, `GadgetFabricateCategory.Contains(def)`, title from `FabricateCategory._title`). That asset is only loaded once the fabricator UI has been opened (2026-10-24 log: `crafter: 417 gadget recipes in 0 fabricator categories`). The earlier fix looked for it again every 5 s, and each look (`FindObjectsOfTypeAll`) cost a 25 ms frame (the same log: `frame:sandbox took 25 ms` every 5 s while the tab was open); that retry is gone. Instead, without the menu asset, the categories come from the groups the fabricator's categories are built on (`GadgetFabricateCategory._gadgetSubGroup`): the `GadgetGroup`'s member groups except the hidden / variant one, i.e. `GadgetUtilitiesGroup`, `GadgetWarpGroup`, `GadgetDecorGroup` (loaded from the start), a recipe's category being the group it is a member of. Title: the group's `_localizedName`, else its asset name ("Utilities", "Warp", "Decor"). Icon: the fabricator's category sprites by name (`iconCategoryUtilities`, `iconCategoryWarptech`, `iconCategoryDecorations`, see "Group tab icons") when loaded, else the tab takes the icon of the category's first gadget. Logged: `crafter: N gadget recipes, M categories from the gadget groups: Utilities N, ...`. Tabs (icons only, under the search box like the spawner's): All, one per category, Materials. When the tab list changes it stays on the category that was selected. The cost text in the tooltip comes from `PurchaseCost.IdentCosts` (`IdentCostEntry {Amount, IdentType}`, struct list read through `List._items`) and `_currencyCosts` (`CurrencyCostEntry`). Blueprint state (`HasBlueprint`) and the inventory count (`GetItemCount`) refresh every 2 s while the tab is open, and right after a craft.

Crafting mirrors `GadgetFabricatableItem.FabricateAndSpendCost` (1.3.0, RVA 0x827E20) without `SpendableDirector.Spend`: `SceneContext.GadgetDirector.AddItem(definition, count * (BuyInPairs ? 2 : 1))`. It respects the gadget's count limit (`CanAddGadget`). It does not send the fabricate analytics event or `MetaGameDirector.OnGadgetFabricated`. The category icons are `FabricateCategory._icon`.

**Without the blueprint** (2026-10-24): `GadgetDirector.AddItem(in AddItemOptions)` (RVA 0x867C20, disassembled) has no blueprint check. For a storable type it clamps to the refinery space; for a `GadgetDefinition` it unlocks the gadget's pedia entry if needed, raises the gadget-obtained event and does `GadgetsModel.SetCount(BaseGadget, GetCount + n)`. `CanAddGadget` only compares the count with the gadget's limit (`GadgetDefinition` +0xF8). So a gadget with a locked blueprint is crafted into the inventory and saved like any other; the blueprint itself stays locked (`AddBlueprint` is not called). **Not seen in game:** whether the gadget menu lists and places a gadget whose blueprint is locked.

### Crafting materials

The crafter's last section: every `IdentifiableType` that `GadgetDirector.IsStorable` accepts (the refinery's storable groups, minus its block list), except gadgets, the player and scene objects, saveable (see above). Built once in game (needs the gadget director), sorted by name, logged; the refinery count (`GetItemCount`) refreshes with the recipes every 2 s. Adding goes through `GadgetDirector.AddItem(type, n)`, the same call the refinery intake uses, clamped to `GetRefinerySpaceAvailable` (999 per type). This includes plorts if the refinery stores them.

### Vacpack slot unlock

`AmmoSlot.IsUnlocked` (0xA3BE20, disassembled): true if the slot definition's `_isUnlockedProperty` is null, else the slot's unlock property value >= `_isUnlockedThreshold`. The property is driven by upgrade effects: `UpgradeDefinition._levels[n].Effects` (`List<FloatModifier>`, `TargetProperty` / `Value` in the generic base `Modifier<float>`, read on the live objects). The module looks through the player's `UpgradeModel.upgradeDefinitions` (`UpgradeDefinitionList`) for the upgrade with a modifier on the slot's property and takes the first level whose `Value` reaches the threshold (else the first level that names the property). The model's level n applies `_levels[n]` (`UpgradeDefinition.GetUpgradeLevel(int)` indexes the list directly; the fabricator buys `GetUpgradeLevel(def) + 1`), so it calls `IncrementUpgradeLevel` until `GetUpgradeLevel` reaches that index, at most 16 times, and stops early once the slot reports unlocked. Earlier slots unlocked by the same levels open too, as when buying them. Locked slots for which an upgrade is found are flagged `unlockable` in the snapshot (field reads plus one `GetUpgradeModel` call per locked slot per refresh). **Not seen in game:** that the property updates within the same call (the bound above makes a late update harmless) and the upgrade asset that owns the slot properties.

## Player extras (Extras card)

Code: `modules/sandbox/player_extras.cpp`, UI bridge `ui/sandbox_extras_bridge.h` (kept apart from the main snapshot), card `SandboxExtras` in `ui/menu.cpp` (full width, between Newbucks | Player and the vacpack). Each tool is optional: a missing symbol turns only that tool off (logged at setup: `[sandbox] player extras: infinite health ..., teleport to marker ..., unlock expansions ...`; the button's tooltip gives the reason). All of it runs only while the module is on, so the achievement pause applies.

### Infinite health

- Session toggle like infinite energy (not saved, off on restart and when the module is turned off).
- Hook `PlayerState.Damage(Damage)` (RVA 0x87AEB0, unique body, created in Setup, switched with the module, DetourGuard). Decompiled: returns false if fast-forwarding or `timeScale <= 0`; else `PlayerModel.LoseHealth(damage.Amount)` and returns true (dead) when health floors to 0, setting `currHealth = 0`, `healthBurstAfter = inf`. While the toggle is on the detour returns false (not dead) without calling the original, so no damage source that goes through it can knock the player out.
- Plus a 10 Hz top-up (`GetCurrHealth` < `GetMaxHealth` -> `SetHealth(max)`) for health lost any other way. If the hook can't be created, only the top-up runs (a single hit bigger than the maximum still knocks you out; the tooltip says so).
- **Not seen in game:** whether every damage source (slime sea, falls, Tarrs, explosions) goes through `PlayerState.Damage`; a source that kills directly through `PlayerDeathHandler` would not be stopped.

### Teleport to waypoint

How the game stores the marker (1.3.0, decompiled): `SceneContext.MapDirector` (`MapDirector`, a ScriptableObject) -> `_navigationMarkerData` (`MapNavigationMarkerData`: `Optional<Vector3> _markerPosition`, `MapDefinition _onMap`, `SceneGroup _firstSceneGroup`). The map UI places it with `MapViewModel.PlaceNavigationMarker(Vector2 ui)` -> `MapDirector.SetPlayerNavigationMarker(new Vector3(-ui.x, 0, -ui.y), currentMap, 16)`, so the stored position is world x / z with **y = 0**. The in-world beam (`NavigationMarkerInWorldController.TurnBeamOn`) is placed at exactly that position. Not saved in the save file as far as seen (no model field).

`MapDirector.TryGetPlayerNavigationMarkerLocation(out Vector3, SceneGroup forSceneGroup, ZoneDefinition forZone)` (0x9C2AC0) returns it when a marker is set, its map's `RelatedScenes` contain `forSceneGroup`, and `forZone` is not in `_centerPlayerInZones` (zones not drawn where they are). The module asks it with `RegionRegistry.CurrentSceneGroup` and `SceneContext.PlayerZoneTracker.GetCurrentZone()` every snapshot (4 Hz while the page is open); without a marker in this zone the button is disabled with the reason as tooltip. A marker on another zone's map is not offered (that would need a scene-group load).

Teleport:
1. Ground: `Physics.Raycast((x, 1500, z), down, out hit, 3000, DefaultRaycastLayers, QueryTriggerInteraction.Ignore)`; the target is 1 m above the hit. No hit: refused with a message.
2. `TeleportNetwork.CompleteInSceneTeleport(SceneGroup, Vector3 position, Vector3 eulerRotation)` (public, 0x65BA00), the game's in-scene teleport: it takes the player's `TeleportablePlayer` and calls `TeleportTo(position, group, rotation, overlayEnabled: true)`, which sets the controller position and rotation, zeroes the velocity, `PlayerModel.SetCurrentSceneGroup`, `RegionLoader.InitializeRegionProxies` (loads the regions around the target), moves a held item along, disables screen effects and updates the audio listener. Rotation: the player's current heading (`Transform.eulerAngles` with x / z zeroed).

Log: `[sandbox] Teleported to the map marker: (x, y, z) -> (x, y, z), N m`.

**Not seen in game:** the ground hit (the first collider from above wins: a roof, a tree or an invisible ceiling collider would put you on top of it; over the slime sea you land on whatever collider is below), and whether distant terrain colliders are loaded before `InitializeRegionProxies` runs (if not, the raycast finds nothing and the teleport is refused rather than dropping you).

### Unlock ranch expansions

The expansions are `AccessDoor`s (`MonomiPark.World`, state `LOCKED 0 / OPEN 1 / CLOSED 2`, stored in `AccessDoorModel.state`, which the save keeps) with an `AccessDoorUIActivator` (`MonomiPark.SlimeRancher.UI.AccessDoor`) that opens the purchase UI. Buying (decompiled): `AccessDoorUI` confirm -> `SpendableDirector.Spend(item._purchaseCost)` -> display result true -> `AccessDoorUIActivator.OnPurchaseMenuResult`: `door.CurrState = OPEN`; each of `door.LinkedDoors` that is `LOCKED` -> `CLOSED`; `door.OnUnlockedEvent.RaiseEvent()`; PlayFab analytics; `MetaGameDirector.Grant(DomainExpansion)`; purchase cue.

The module does the same for every loaded activator whose door is locked, without the spend, the analytics, the achievement grant and the cue. `AccessDoor.set_CurrState` writes the model and plays the door's open animation (`ForceUpdate`). Candidates: `FindObjectsOfTypeAll(AccessDoorUIActivator)`, live, door live with its `_model` set (prefabs have none), and a `_purchaseMenuItem` or `_legacyUIPrefab` (what `TryInitialize` needs). The scan runs on the button and every 5 s while the page is open, for the count in the tooltip. The page asks for confirmation first. Log: `[sandbox] Unlocked N ranch expansion(s): <purchase item asset names>`.

**Not seen in game:** the activators exist only while the ranch scene is loaded (away from the ranch the button says to go there); whether other zones have purchasable access doors too (they would be counted and unlocked as well; the log names them); that `OnUnlockedEvent` is all the follow-up the game needs (the buy path does nothing else, so it should be).

## Achievements paused

While the module is enabled no achievement progress is made: no Steam unlock or stat, and the save's achievement progress does not move. The Sandbox page shows a warning banner across both columns at its top, "Steam achievements are paused while Sandbox is on." (since 2026-10-24; before, a dim line at the bottom). Code: `modules/sandbox/achievements.cpp`.

How the game does it (1.3.0, decompiled):
- `MetaGameDirector` (`MonomiPark.SlimeRancher`) owns `List<AchievementData> Achievements` (nested class `MetaGameDirector.AchievementData`; enum `MetaGameDirector.Achievement`, 27 entries) and an `IMetaGamePlatform _platform`. `AchievementData.CurrentProgress` is the in-game tracker; `PullMetaGameData` saves it into `MetaGameDirectorModel.ProgressPerAchievement` and `SetModel` restores it on load.
- Both `Grant` overloads and `CheckForBehindTheWaterfall` call `AchievementData.IncrementProgress(n)` (0x5FE9C0); when it returns true (progress passed `LargestSavedProgress`) they call `_platform.Grant(data)`. Both `SetAchievementProgress` overloads do the same with `AchievementData.SetProgress(v)`. No other code changes `CurrentProgress` during play (`ForceComplete` has no native callers; `Clear` only on session end / load).
- `SetProgress` is also how the save (`SetModel` -> private `SetAchievementProgress(AchievementData, int)`) and Steam (`SteamMetaGamePlatform.LoadAchievementProgress`) restore progress, so it is not hooked.
- `SteamMetaGamePlatform.Grant` (0xB30F00) is the only caller of `SteamUserStats.SetStat` / `SetAchievement` / `StoreStats` (`IndicateAchievementProgress` is never called).
- Beallionaire: `HandleSoldPlort(price)` grants the first sale of a 30 s window and adds later ones to `_tempBeallionaireProgress`, which `Update` / `ClearPendingBeallionaireUpdate` flush through `SetAchievementProgress` when the window ends.

Hooks (created in `Setup`, on and off with the module; DetourGuard each; the detour checks the pause flag and the object's class, nothing else):

| hook | while Sandbox is on | why |
|---|---|---|
| `AchievementData.IncrementProgress(int)` | returns false, progress unchanged | every Grant path goes through it, and false means the director never calls the platform |
| `MetaGameDirector.SetAchievementProgress(Achievement, int)` (the public overload, picked by its first parameter's class) | skipped | every gameplay set (shop, upgrades, treasure pods, doors, pop, Beallionaire flush, query / event achievements, `ValidatePersistantAchievements`) |
| `MetaGameDirector.HandleSoldPlort(int)` | skipped | keeps Sandbox-time sales out of the pending Beallionaire total, which would otherwise be flushed after Sandbox is turned off |
| `SteamMetaGamePlatform.Grant(AchievementData)` | skipped | backstop: nothing reaches Steam, including a load that syncs older save progress (it syncs again on the next load with Sandbox off) |

Nothing is queued: blocked calls are dropped, so turning Sandbox off replays nothing. The first blocked call per enable logs one line (`[sandbox] achievements paused: blocked ... while Sandbox is on`); turning the module on / off logs `achievements paused` / `resumed`.

The hooks are required: if one can't be resolved the module fails setup and Sandbox can't be enabled (fail closed). If the module faults, its hooks are disabled with it (Sandbox edits stop too).

Limits:
- Achievements the game derives from world state (treasure pods opened, upgrades owned, vacpack contents in `CheckAmmo`, Slimepedia entries, query achievements) are checked again by the game later, for example by `ValidatePersistantAchievements` on every load. Anything Sandbox put into the world (gadgets, items, Newbucks) still counts then if the save is played with Sandbox off. That is the game re-reading the save, not replayed progress.
- A Beallionaire window opened before Sandbox was enabled and ending while it is on loses its pending total.

## Threading and faults

- Hooks only for the achievement pause and infinite health (above). One main-thread frame callback: the snapshot for the tab every 250 ms, only while the tab was drawn in the last 0.6 s (`ui/sandbox_bridge.h`, plus the Extras state in `ui/sandbox_extras_bridge.h`), the infinite-energy / infinite-health top-ups, and the queued HUD notices (a retry every 0.25 s while one waits).
- The tab's buttons post an `Action` to the main thread (`main_thread::Post`); the module validates it, applies it, logs one line (`[sandbox] Newbucks set: 1,200 -> 5,000`, ...), sets the tab's status line (spawns, crafts and refinery adds that worked: a HUD notice instead, see Spawner) and refreshes the snapshot.
- Every game call is `Resolver::Invoke` (runtime_invoke, managed exceptions captured and logged). A native fault there marks the module faulted (`MarkModuleFaulted`); from then on it makes no game calls.
- Each step re-reads the player: `SceneContextUtility.TryGetPlayerState`, `IsAlive` + class checks on `PlayerState`, `PlayerModel`, `AmmoSlotManager`, every `AmmoSlot` and every item.
- Snapshot data is immutable once published: every list is a `shared_ptr<const ...>` built in full on the main thread, then swapped into a new `Snapshot` under the bridge's lock; a change (recipe state, categories) copies the list. The render thread keeps the snapshot it got for the whole frame. The icon table (`ui/item_icons`) is append-only; the render thread draws from its own copy, refreshed under a lock, and an id it doesn't have draws nothing.

### Overlay crash on the Sandbox page (second fix 2026-10-24)

The first fix below (commit ab0f4df) did not stop it. The build after it (ba0a221e, fd173ad) crashed the same way, at the same moment, and this time the guard logged the fault address:
```
01:26:30.389 [sandbox] spawn list: 332 of 806 identifiable types are actors (47 ms)
01:26:30.402 [ERROR] [62724] ui: native exception 0xC0000005 in overlay Present at VERSION.dll+0x22CB2D (reading 0x0) - overlay disabled
```
Where: with that build's PDB (`dist/archive/20261024-012421-ba0a221e/`; `dumpbin /disasm` with `_NT_SYMBOL_PATH` on the archive folder), `+0x22CB2D` is `mov ecx, [rdx]` in `stbrp__skyline_find_best_pos` (`third_party/imgui/imstb_rectpack.h:388`, `while (node->x + width <= c->width)`, after `node = node->next`), called from `stbrp_pack_rects` < `ImFontAtlasPackAddRect`: Dear ImGui packing a new glyph into the font atlas. Not texture code at all, so the earlier guess (Unity freeing an icon texture under the render thread) was wrong; the COM references it added are harmless and stay.

Why it reads null: the skyline search walks the atlas's skyline nodes until one leaves no room for the rectangle; the list ends in a sentinel node at `x = atlas width` whose `next` is null. For a rectangle width > 0 the loop always stops at the sentinel. For a width <= 0 (glyph width + 1 px padding <= 0) it does not, steps to the sentinel's null `next` and reads `node->x` at address 0. ImGui guards against such a rectangle only with `IM_ASSERT(w > 0)`, compiled out in Release. Its glyph loader produces one when stb_truetype returns an inverted glyph box, which happens for a negative, NaN, infinite or >= 2^31 font size (rounding turns the last three into INT_MIN). `PushFont` clamps the size to 1..512, but `ImDrawList::AddText(font, size, ...)` and `ImFont::CalcTextSizeA(size, ...)` pass it to `ImFont::GetFontBaked` unchecked. Reproduced offline (vendored ImGui, the overlay's fonts, Release flags): `CalcTextSizeA` or `AddText` with such a size faults on exactly this instruction (`imstb_rectpack.h:388`, reading 0x0); every glyph of Nunito, Fredoka and Segoe UI at 1..80 px gives a normal box, and heavy atlas growth / repacking with valid sizes never faults.

What fed it the bad size on that frame is not pinned: every font size on the Sandbox page is a constant times the overlay scale (1..3) or the height of an ImGui item, and an offline render of the real page (`menu.cpp` with a made-up snapshot, mouse, scales 1..2, Latin and random BMP names, ImGui asserts on) drew 400 frames with no failed check. Both crashes came on the first frame that drew the new spawn list, right after its 47-63 ms build on the main thread.

Fix (`third_party/imgui`, `ui/NOTES.md` "ImGui checks"): the packer refuses a rectangle with width or height <= 0, the glyph loader skips inverted boxes, and `GetFontBaked` clamps the size (1..512) and the rasterizer density like `PushFont` does. A bad size now draws text at 1 or 512 px instead of crashing. `IM_ASSERT` now logs once per check with the caller's stack (`ui: ImGui check failed: ... called from VERSION.dll+0x...`), so if a bad size still reaches ImGui, the log names the call site; look the offsets up with that build's PDB. Also: the two `GetFontBaked(...)->FindGlyph` calls in `menu.cpp` check for a null baked font, and `item_icons::Draw` skips an icon without a positive aspect.

#### First fix (2026-10-24, commit ab0f4df: did not fix the crash)

Seen: opening the Sandbox page, the whole overlay disappeared. Log (main thread 28372, render thread 28408):
```
01:10:33.527 [sandbox] vacpack item list: 104 of 806 identifiable types fit 7 slot(s) (31 ms)
01:10:33.793 [sandbox] 17 identifiable type group(s) with an icon: ...
01:10:33.797 [sandbox] spawn list: 332 of 806 identifiable types are actors (63 ms)
01:10:33.809 [ERROR] [28408] ui: native exception 0xC0000005 in overlay Present - overlay disabled
```
The fault is in the first overlay frame that drew the new spawn list (the snapshot with it was published at the end of the 54 ms main-thread frame logged as the hitch at 33.809). That frame is the first to draw the spawn list's icons, many on textures no earlier icon used (slime and largo appearance icons through `get_Icon`, group icons).

Suspected cause (wrong, see above; the fault was in ImGui's font atlas): `item_icons` stored Unity's raw `Texture.GetNativeTexturePtr` value at `Register` (main thread) and the render thread called `QueryInterface` on it the first time an icon on that texture was drawn, to make the shader resource view. Only the managed `Texture` was held (GC handle); a GC handle does not hold the GPU resource, which Unity can release or recreate while the managed object stays alive. Between `Register` and the first draw nothing kept the `ID3D11Resource` alive, so the render thread could dereference a freed COM object, an access violation inside the overlay's `Present`. The log has no fault address (the guard did not log one then), so the exact texture is not known; nothing else in the page's render path reads memory the frame does not own (checked below).

Fix: `Register` takes its own COM reference right after the icall, while Unity's pointer is current (`QueryInterface` to `ID3D11Texture2D`, which also rejects non-2D / array textures there), and holds it for the process lifetime, one per texture. The render thread only uses that reference (and the view it makes from it); Unity's pointer is never used again. Since the reference is never released, a texture address can't be reused for another texture either. The Slimepedia logo (`ui/slime_icons.cpp`) had the same pattern and got the same fix. The overlay's SEH guards now log the faulting instruction (`module+offset`) and the access-violation address, so a future fault can be pinned to a line with the PDB.

Audit of the page's render path (no other fault found):
- Snapshot lists, relations, slots: owned by the frame's `shared_ptr<const Snapshot>`; the module never mutates a published list.
- `FindItem`'s lookup table (`ui/menu.cpp`) cached raw pointers into the lists and compared bare list addresses to detect a change: a rebuilt list allocated where a freed one had been would have kept the table with dangling pointers (ABA). It now holds the two lists it was built from (`shared_ptr`), so they can't be freed under it.
- Group-tab icons: ids from the snapshot (0 until `EnsureGroupIcons`), validated by `item_icons::Draw` (`id > local table` draws the glyph fallback).
- Grid tiles keep `const std::string*` into the snapshot's lists, used only within the frame that holds the snapshot.
- Crafter tabs with 0 categories: All and Materials only; the tab index is clamped, and now follows the selected category when the list changes.

The `frame:ui.overlay` cost (avg 0.98 ms, 570 frames > 2 ms in the 10 s before) was not the Sandbox page: that perf stage is `game_input`'s main-thread tick, and the overlay was open on the title screen (the modded label was added at 01:10:12, in the same window), where `game_input` found no `InputDirector` and ran `FindObjectOfType` on every tick (in game, with the director cached, the same stage costs 0.001 ms). It now retries at most once a second (`ui/NOTES.md`). The Sandbox page itself makes no game calls per frame; its main-thread work is the 4 Hz snapshot (`frame:sandbox`, ~0.001 ms) and the one-time list scans (31-63 ms each, one per refresh).

## Not done (possible later)

- **Unlock all vacpack upgrades**: only the slot upgrade is applied (above, one level at a time like the shop). Other upgrades (tank size, jetpack, ...) would go through the same `IncrementUpgradeLevel`; not offered yet.
- Other `CurrencyDefinition`s (same `AddCurrency` / `SpendCurrency` path, with a currency picker), keys (`PlayerState.AddKey` exists, trivially doable), plort market prices, unlocking zones / map, time of day (`TimeDirector`).

## Unverified in game

Everything above was built from the dump and disassembly only: the catalog size and filtering, the `GetLocalizedString` names, the coin popup on set/add/subtract, `SetEnergy` holding against the jetpack drain at 10 Hz, and that `MaybeAddResource` on an emptied slot shows the new item in the HUD at once.

Not yet seen in game (2026-10-23): the icons (sRGB handling as for the Slimepedia logo), the item kinds from category names, the spawn list's size and contents, `InstantiateActor` with the zeroed nullable, spawn placement and the wall check, spawned items persisting after a save and reload, the fabricator categories being loaded outside the fabricator UI, the cost text, and `AddItem` showing the gadget in the gadget menu.

Not yet seen in game (2026-10-24): which types the save check leaves out (expected: Glitch Tarr and other test/unused types; see the log line), that `Dictionary<string, int>.ContainsKey` runs through runtime_invoke (if it doesn't, every list stays empty and the log shows the failed call), and the Tarr gray-out.

Not yet seen in game (2026-10-24, UX round 2): the largo icons from `get_Icon` (or the composed fallback), the group-icon fallback for giant eggs / vine bulbs, which group icons the tabs pick (log line), the diet relations (log: `slime diets: N related item(s)`), the crafting materials list (log: `crafting materials the refinery stores`), crafting a gadget with a locked blueprint, slot unlock through the upgrade, and slot move / swap.

Not yet seen in game (2026-10-24, spawner round 3): the kinds from the game's groups (log `item kinds from the game's groups` and `spawn list by tab`), the slime / largo tab icons (log `game sprites by name` and `group tab icons`), the prefab-rendered icons, the HUD notices for spawns and crafts (`AddResourceCollected` from the sandbox; a gadget's notice may have no icon if its `IdentifiableType.icon` is empty), the crafter categories from the gadget groups and their titles.

Not yet seen in game (2026-10-24, overlay crash, second fix): the Sandbox page staying up when the spawn list arrives, and whether the log then shows `ui: ImGui check failed: ...` lines (they name the call site of a bad font size; see "Overlay crash on the Sandbox page").
