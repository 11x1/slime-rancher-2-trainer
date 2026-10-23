# sandbox

Edits the character's resources from the overlay's **Misc > Sandbox** tab: Newbucks, health / energy / radiation, what each vacpack slot holds, spawning items in front of the player and crafting fabricator gadgets at no cost. Off by default.

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
| Put item | `Decrement` (empty the slot if it holds something else), then `MaybeAddResource` | the item must be in the catalog (below), saveable (below) and accepted by that slot; `1..max`. The tab always asks for the slot's maximum (click = fill) |
| Spawn | `InstantiationHelpers.InstantiateActor(prefab, RegionRegistry.CurrentSceneGroup, pos, identity, nonActorOk: false, NONE, NONE, metadata: null, false, setCollected: false)` | the item must be in the spawn list (below) and saveable (below); no Tarrs while they are disabled; 1..20 per click |
| Craft | `GadgetDirector.HasBlueprint`, `CanAddGadget(def, n)`, then `GadgetDirector.AddItem(def, n)` | the gadget must be in the recipe list (below), its blueprint unlocked, and `n` within its limit; 1..20 crafts per click (x2 for `BuyInPairs`) |

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
- Icon: `IdentifiableType.icon` registered once with `ui/item_icons` (texture kept alive, D3D11 resource and sprite rectangle cached); the tab only gets the small id.
- Kind (tile accent and spawner filter only): `SlimeDefinition` (largo if `IsLargo`), `GadgetDefinition`, `ToyDefinition`, `LiquidDefinition`, `IsPlort`, `IsAnimal`, else the asset names of its `Categories` (`IdentifiableCategory` assets: "food" / "veggie" / "fruit" / "meat" -> Food, "craft" / "resource" -> Resource), else Other. The category names are a guess; a wrong guess only changes a colour.

Fixed (2026-10-23): picking an item in the old list did nothing. The list looked up the picked item's name inside its loop, before the click on that row was handled, then cleared the pick when the name was missing, so every new pick was undone in the same frame and **Put** never enabled (the log shows no `Put` attempt at all). The game side (`MaybeAddResource` arguments, `IsAllowed`) was fine.

## Spawner

List: every loaded `IdentifiableType` whose `prefab` has an `IdentifiableActor` component (the game's loose actors: slimes, largos, plorts, food, toys, crafting resources, ...), except `IsPlayer`, `IsSceneObject` (gordos and other scene objects), `GadgetDefinition` (placed through gadget mode, `GadgetDirector.InstantiateGadget`), `LiquidDefinition` and types the save can't store (see "Only saveable items"; the vacpack list and the crafter are filtered the same way). Built once on a refresh in game (logged; tried again on the next enable if it came out empty), kept with GC handles, sorted by name. Each refresh builds at most one of the three lists (vacpack catalog, spawn list, recipes), so the one-time scans land in separate frames.

Spawning uses the vacpack's own path: `VacuumItem.Expel` calls `InstantiationHelpers.InstantiateActor` with `RegionRegistry.CurrentSceneGroup`, which creates the actor model (`GameModel.InstantiateActorModel`), instantiates the prefab, registers it with `DynamicObjectContainer` and initialises the model, so the spawned item is saved like any loose item. The `Nullable<AmmoMetadata>` argument is passed as a zeroed struct (`hasValue = false`; runtime_invoke takes nullable arguments as a pointer to the struct, and `InstantiateActor` only reads it for slimes after checking `hasValue`).

Placement: the main camera's position and its forward direction flattened onto the ground (the body's facing when looking straight up/down), 2.5 m ahead and 0.3 m below eye height; several items go side by side (5 per row, 0.55 m apart, rows 0.7 m higher). An optional `Physics.Raycast(origin, dir, out hit, 3.1 m, DefaultRaycastLayers, QueryTriggerInteraction.Ignore)` pulls the spot in front of a wall (hit distance - 0.6 m, at least 0.5 m).

## Crafter

List: every `GadgetDefinition` asset whose `CraftingCosts` has ident or currency costs (what the fabricator charges for), read from the game data, not hardcoded. Grouped by the fabricator's own categories when its `FabricateMenuConfiguration` asset is loaded (`_categories`, `GadgetFabricateCategory.Contains(def)`, title from `FabricateCategory._title`); without it there are no category chips. The cost text in the tooltip comes from `PurchaseCost.IdentCosts` (`IdentCostEntry {Amount, IdentType}`, struct list read through `List._items`) and `_currencyCosts` (`CurrencyCostEntry`). Blueprint state (`HasBlueprint`) and the inventory count (`GetItemCount`) refresh every 2 s while the tab is open, and right after a craft.

Crafting mirrors `GadgetFabricatableItem.FabricateAndSpendCost` (1.3.0, RVA 0x827E20) without `SpendableDirector.Spend`: `SceneContext.GadgetDirector.AddItem(definition, count * (BuyInPairs ? 2 : 1))`. Like the fabricator it needs the blueprint and respects the gadget's count limit (`CanAddGadget`), so it only makes states a vanilla game can reach (with enough resources). It does not send the fabricate analytics event or `MetaGameDirector.OnGadgetFabricated`.

## Threading and faults

- No hooks. One main-thread frame callback: the snapshot for the tab every 250 ms, only while the tab was drawn in the last 0.6 s (`ui/sandbox_bridge.h`), and the infinite-energy top-up.
- The tab's buttons post an `Action` to the main thread (`main_thread::Post`); the module validates it, applies it, logs one line (`[sandbox] Newbucks set: 1,200 -> 5,000`, ...), sets the tab's status line and refreshes the snapshot.
- Every game call is `Resolver::Invoke` (runtime_invoke, managed exceptions captured and logged). A native fault there marks the module faulted (`MarkModuleFaulted`); from then on it makes no game calls.
- Each step re-reads the player: `SceneContextUtility.TryGetPlayerState`, `IsAlive` + class checks on `PlayerState`, `PlayerModel`, `AmmoSlotManager`, every `AmmoSlot` and every item.

## Not done (possible later)

- **Unlock all vacpack upgrades**: upgrades are `UpgradeModel` levels bought through `UpgradeDefinition` purchase flows (components, costs, prerequisites); setting levels directly risks states the shop can't produce. Would need its own investigation of `PlayerModel.GetUpgradeModel()` / the Fabricator purchase path.
- Other `CurrencyDefinition`s (same `AddCurrency` / `SpendCurrency` path, with a currency picker), keys (`PlayerState.AddKey` exists, trivially doable), plort market prices, unlocking zones / map, time of day (`TimeDirector`), refinery contents (`custom_upgrades` already talks to the silo/refinery models).

## Unverified in game

Everything above was built from the dump and disassembly only: the catalog size and filtering, the `GetLocalizedString` names, the coin popup on set/add/subtract, `SetEnergy` holding against the jetpack drain at 10 Hz, and that `MaybeAddResource` on an emptied slot shows the new item in the HUD at once.

Not yet seen in game (2026-10-23): the icons (sRGB handling as for the Slimepedia logo), the item kinds from category names, the spawn list's size and contents, `InstantiateActor` with the zeroed nullable, spawn placement and the wall check, spawned items persisting after a save and reload, the fabricator categories being loaded outside the fabricator UI, the cost text, and `AddItem` showing the gadget in the gadget menu.

Not yet seen in game (2026-10-24): which types the save check leaves out (expected: Glitch Tarr and other test/unused types; see the log line), that `Dictionary<string, int>.ContainsKey` runs through runtime_invoke (if it doesn't, every list stays empty and the log shows the failed call), and the Tarr gray-out.
