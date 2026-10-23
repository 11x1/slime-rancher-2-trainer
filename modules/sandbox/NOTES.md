# sandbox

Edits the character's resources from the overlay's **Misc > Sandbox** tab: Newbucks, health / energy / radiation, what each vacpack slot holds (and unlocking locked slots), spawning items in front of the player, crafting fabricator gadgets at no cost and adding crafting materials to the refinery. Off by default. While it is on, achievements are paused (see "Achievements paused").

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
  4. The icon of a group it belongs to: its own `IdentifiableType.groupType`, else the first `IdentifiableTypeGroup` asset with an `_icon` whose `IsMember` is true (leaf groups, without `_memberGroups`, first). `TargetingUI` has `giantEggGroup` and `vineBulbGroup`, so giant eggs and vine bulbs are expected to land here. The groups with an icon are read once (one `FindObjectsOfTypeAll`, logged with their asset names) and only asked for types without any icon of their own.
- Kind (tile accent, group tabs and tooltip tag): `SlimeDefinition` (largo if `IsLargo`), `GadgetDefinition`, `ToyDefinition`, `LiquidDefinition`, `IsPlort`, `IsAnimal`, else the asset names of its `Categories` (`IdentifiableCategory` assets: "food" / "veggie" / "fruit" / "meat" -> Food, "craft" / "resource" -> Resource), else Other. The category names are a guess; a wrong guess only changes a colour or the tab an item shows under.

### Group tab icons

The vacpack and spawner grids have icon-only tabs All / Plorts / Slimes / Food / Resources (Slimes include largos, Food includes `IsAnimal` meat). The icons are read once, after the spawn list:
- From the drone station's target menu data: the `DroneTargetDefinitions` asset (`MonomiPark.SlimeRancher.UI.Drone.Data`, a `ListAsset` of `DroneUIProgramDefinition<DroneTaskTargetType>` with `_type` / `_icon`, fields read on the live objects). `PLORTS` (2) -> Plorts, `VEGGIES` (3) -> Food, `CRAFTING_MATS` (8) -> Resources. There is no "food" or "slimes" target type (enum: NONE, SINGLE, PLORTS, VEGGIES, FRUIT, MEATS, ELDERS, PLORTS_NO_SPECIALS, CRAFTING_MATS).
- Anything still missing from the type groups with an icon, by asset name: "slime" (not largo / tarr / gordo / feral), "plort", "food", "craft". The chosen ids are logged (`[sandbox] group tab icons: ...`); a missing one is drawn as a coloured letter. **Which group gives the slime icon is a guess until the log line shows the group names.**

### Slime diets (favourites)

Read once after the spawn list, for every slime and largo in it: `SlimeDefinition.Diet` (`SlimeDiet`) -> `FavoriteIdents` (favourite foods) and `ProduceIdents` (plorts), with the reverse lookups (food -> slimes that love it, plort -> slimes that make it; base slimes before largos). Sent as `Snapshot::relations`, keyed by item id; the tab uses it for the tooltip rows, the related-tile highlight and the right-click "Give ..." entries (`ui/NOTES.md`). The `EatMap` and `MajorFoodIdentifiableTypeGroups` (everything a slime eats) are not used.

Fixed (2026-10-23): picking an item in the old list did nothing. The list looked up the picked item's name inside its loop, before the click on that row was handled, then cleared the pick when the name was missing, so every new pick was undone in the same frame and **Put** never enabled (the log shows no `Put` attempt at all). The game side (`MaybeAddResource` arguments, `IsAllowed`) was fine.

## Spawner

List: every loaded `IdentifiableType` whose `prefab` has an `IdentifiableActor` component (the game's loose actors: slimes, largos, plorts, food, toys, crafting resources, ...), except `IsPlayer`, `IsSceneObject` (gordos and other scene objects), `GadgetDefinition` (placed through gadget mode, `GadgetDirector.InstantiateGadget`), `LiquidDefinition` and types the save can't store (see "Only saveable items"; the vacpack list and the crafter are filtered the same way). Built once on a refresh in game (logged; tried again on the next enable if it came out empty), kept with GC handles, sorted by name. Each refresh builds at most one of the lists and one-time extras (vacpack catalog, spawn list, recipes, crafting materials, slime diets, group tab icons), so the one-time scans land in separate frames.

Spawning uses the vacpack's own path: `VacuumItem.Expel` calls `InstantiationHelpers.InstantiateActor` with `RegionRegistry.CurrentSceneGroup`, which creates the actor model (`GameModel.InstantiateActorModel`), instantiates the prefab, registers it with `DynamicObjectContainer` and initialises the model, so the spawned item is saved like any loose item. The `Nullable<AmmoMetadata>` argument is passed as a zeroed struct (`hasValue = false`; runtime_invoke takes nullable arguments as a pointer to the struct, and `InstantiateActor` only reads it for slimes after checking `hasValue`).

Placement: the main camera's position and its forward direction flattened onto the ground (the body's facing when looking straight up/down), 2.5 m ahead and 0.3 m below eye height; several items go side by side (5 per row, 0.55 m apart, rows 0.7 m higher). An optional `Physics.Raycast(origin, dir, out hit, 3.1 m, DefaultRaycastLayers, QueryTriggerInteraction.Ignore)` pulls the spot in front of a wall (hit distance - 0.6 m, at least 0.5 m).

## Crafter

List: every `GadgetDefinition` asset whose `CraftingCosts` has ident or currency costs (what the fabricator charges for), read from the game data, not hardcoded. Grouped by the fabricator's own categories when its `FabricateMenuConfiguration` asset is loaded (`_categories`, `GadgetFabricateCategory.Contains(def)`, title from `FabricateCategory._title`); without it there are no category chips. That asset is only loaded once the fabricator UI has been opened (2026-10-24 log: `crafter: 417 gadget recipes in 0 fabricator categories`), so while the list has no categories the module looks for them again every 5 s (one `FindObjectsOfTypeAll(FabricateMenuConfiguration)`, only while the tab is open, never in the same refresh as another one-time scan). When they turn up it regroups the existing list (`Contains` per recipe) and logs `crafter: N fabricator categories loaded now`. The tab shows All (its tooltip says to open the fabricator once) and Materials until then; when the tab list changes it stays on the category that was selected. The cost text in the tooltip comes from `PurchaseCost.IdentCosts` (`IdentCostEntry {Amount, IdentType}`, struct list read through `List._items`) and `_currencyCosts` (`CurrencyCostEntry`). Blueprint state (`HasBlueprint`) and the inventory count (`GetItemCount`) refresh every 2 s while the tab is open, and right after a craft.

Crafting mirrors `GadgetFabricatableItem.FabricateAndSpendCost` (1.3.0, RVA 0x827E20) without `SpendableDirector.Spend`: `SceneContext.GadgetDirector.AddItem(definition, count * (BuyInPairs ? 2 : 1))`. It respects the gadget's count limit (`CanAddGadget`). It does not send the fabricate analytics event or `MetaGameDirector.OnGadgetFabricated`. The category icons are `FabricateCategory._icon`.

**Without the blueprint** (2026-10-24): `GadgetDirector.AddItem(in AddItemOptions)` (RVA 0x867C20, disassembled) has no blueprint check. For a storable type it clamps to the refinery space; for a `GadgetDefinition` it unlocks the gadget's pedia entry if needed, raises the gadget-obtained event and does `GadgetsModel.SetCount(BaseGadget, GetCount + n)`. `CanAddGadget` only compares the count with the gadget's limit (`GadgetDefinition` +0xF8). So a gadget with a locked blueprint is crafted into the inventory and saved like any other; the blueprint itself stays locked (`AddBlueprint` is not called). **Not seen in game:** whether the gadget menu lists and places a gadget whose blueprint is locked.

### Crafting materials

The crafter's last section: every `IdentifiableType` that `GadgetDirector.IsStorable` accepts (the refinery's storable groups, minus its block list), except gadgets, the player and scene objects, saveable (see above). Built once in game (needs the gadget director), sorted by name, logged; the refinery count (`GetItemCount`) refreshes with the recipes every 2 s. Adding goes through `GadgetDirector.AddItem(type, n)`, the same call the refinery intake uses, clamped to `GetRefinerySpaceAvailable` (999 per type). This includes plorts if the refinery stores them.

### Vacpack slot unlock

`AmmoSlot.IsUnlocked` (0xA3BE20, disassembled): true if the slot definition's `_isUnlockedProperty` is null, else the slot's unlock property value >= `_isUnlockedThreshold`. The property is driven by upgrade effects: `UpgradeDefinition._levels[n].Effects` (`List<FloatModifier>`, `TargetProperty` / `Value` in the generic base `Modifier<float>`, read on the live objects). The module looks through the player's `UpgradeModel.upgradeDefinitions` (`UpgradeDefinitionList`) for the upgrade with a modifier on the slot's property and takes the first level whose `Value` reaches the threshold (else the first level that names the property). The model's level n applies `_levels[n]` (`UpgradeDefinition.GetUpgradeLevel(int)` indexes the list directly; the fabricator buys `GetUpgradeLevel(def) + 1`), so it calls `IncrementUpgradeLevel` until `GetUpgradeLevel` reaches that index, at most 16 times, and stops early once the slot reports unlocked. Earlier slots unlocked by the same levels open too, as when buying them. Locked slots for which an upgrade is found are flagged `unlockable` in the snapshot (field reads plus one `GetUpgradeModel` call per locked slot per refresh). **Not seen in game:** that the property updates within the same call (the bound above makes a late update harmless) and the upgrade asset that owns the slot properties.

## Achievements paused

While the module is enabled no achievement progress is made: no Steam unlock or stat, and the save's achievement progress does not move. The Sandbox card says "Achievements are paused while Sandbox is on." Code: `modules/sandbox/achievements.cpp`.

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

- Hooks only for the achievement pause (above). One main-thread frame callback: the snapshot for the tab every 250 ms, only while the tab was drawn in the last 0.6 s (`ui/sandbox_bridge.h`), and the infinite-energy top-up.
- The tab's buttons post an `Action` to the main thread (`main_thread::Post`); the module validates it, applies it, logs one line (`[sandbox] Newbucks set: 1,200 -> 5,000`, ...), sets the tab's status line and refreshes the snapshot.
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
- **Rendered icons** for types that have no icon and no group icon (containers / treasure-pod-like items were reported without one; their type names are not known yet): rendering the prefab to a texture needs a camera, a render texture and a lit layer set up from native code, which is a lot of game-object work on the main thread for a cosmetic gain. Not done; the tile shows the name instead. Check the `group(s) with an icon` log line and the tiles that still show names to see what is left.
- Other `CurrencyDefinition`s (same `AddCurrency` / `SpendCurrency` path, with a currency picker), keys (`PlayerState.AddKey` exists, trivially doable), plort market prices, unlocking zones / map, time of day (`TimeDirector`).

## Unverified in game

Everything above was built from the dump and disassembly only: the catalog size and filtering, the `GetLocalizedString` names, the coin popup on set/add/subtract, `SetEnergy` holding against the jetpack drain at 10 Hz, and that `MaybeAddResource` on an emptied slot shows the new item in the HUD at once.

Not yet seen in game (2026-10-23): the icons (sRGB handling as for the Slimepedia logo), the item kinds from category names, the spawn list's size and contents, `InstantiateActor` with the zeroed nullable, spawn placement and the wall check, spawned items persisting after a save and reload, the fabricator categories being loaded outside the fabricator UI, the cost text, and `AddItem` showing the gadget in the gadget menu.

Not yet seen in game (2026-10-24): which types the save check leaves out (expected: Glitch Tarr and other test/unused types; see the log line), that `Dictionary<string, int>.ContainsKey` runs through runtime_invoke (if it doesn't, every list stays empty and the log shows the failed call), and the Tarr gray-out.

Not yet seen in game (2026-10-24, UX round 2): the largo icons from `get_Icon` (or the composed fallback), the group-icon fallback for giant eggs / vine bulbs, which group icons the tabs pick (log line), the diet relations (log: `slime diets: N related item(s)`), the crafting materials list (log: `crafting materials the refinery stores`), crafting a gadget with a locked blueprint, slot unlock through the upgrade, and slot move / swap.

Not yet seen in game (2026-10-24, overlay crash, second fix): the Sandbox page staying up when the spawn list arrives, and whether the log then shows `ui: ImGui check failed: ...` lines (they name the call site of a bad font size; see "Overlay crash on the Sandbox page").
