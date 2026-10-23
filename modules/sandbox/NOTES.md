# sandbox

Edits the character's resources from the overlay's **Misc > Sandbox** tab: Newbucks, health / energy / radiation, and what each vacpack slot holds. Off by default.

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
| Put item | `Decrement` (empty the slot if it holds something else), then `MaybeAddResource` | the item must be in the catalog (below) and accepted by that slot; `1..max` |

Notes from the disassembly (1.3.0):
- `MaybeAddResource`: count must be > 0; an empty slot (count 0) first checks `slot.Definition.IsAllowed(id)` and takes the id; then the slot's id must equal `id`; without overflow `count + n <= MaxCount`. It does **not** check `IsUnlocked`, so the module checks that itself (a locked slot can't hold anything in vanilla).
- `AmmoSlotDefinition.IsAllowed`: false if the type is in `_slotBlockList`, true if `_slotTypeGroup` is null, else the group's runtime membership.
- `SpendCurrency` throws `ArgumentException` when the player has less than asked; the module never asks for more than the current amount.

## Vacpack item catalog

Items a vanilla vacpack can store: every loaded `IdentifiableType` (`Resources.FindObjectsOfTypeAll`) that is not `IsPlayer` / `IsSceneObject`, whose `prefab` has a `Vacuumable` with `Size == NORMAL` (LARGE / GIANT ones, such as largos, are only held, never stored), and that at least one slot's `AmmoSlotDefinition.IsAllowed` accepts. Each item carries a slot mask, so the tab lists only what the selected slot accepts. Names: `localizedName.GetLocalizedString()` (rich-text tags stripped), else the asset name; duplicates get the asset name appended.

The catalog is built once (logged with its size and time) and rebuilt only when the slot definitions change. Entries are kept alive with GC handles; old handles are freed in a posted main-thread job. The UI gets item ids as opaque numbers (the type's address) and never dereferences them; a request is checked against the catalog (handle target, class, `IsAlive`) before use.

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
