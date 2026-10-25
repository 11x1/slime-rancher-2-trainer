# sandbox

Edits the character's resources and the world from the overlay's **Sandbox** category (`ui/NOTES.md`). Off by default. While it is on, achievements are paused, and a save it was used in is marked as cheated for good (achievements stay blocked there; see "Cheated saves").

Code: `module.cpp` (snapshot, vacpack, spawner, crafter, Fabricator key), `player_extras.cpp` (infinite health, teleport, expansions), `noclip.cpp` (noclip), `upgrades.cpp` (upgrades manager), `weather.cpp` (time & weather), `prefab_icons.cpp` (rendered icons), `achievements.cpp`, `cheated_save.cpp`. UI: `ui/menu.cpp`, `ui/sandbox_upgrades.cpp`, `ui/sandbox_weather.cpp`; bridges `ui/sandbox_bridge.h` (main snapshot), `sandbox_extras_bridge.h`, `sandbox_upgrades_bridge.h`, `sandbox_weather_bridge.h`.

RE: re/vacpack.md (AmmoSlotManager, AmmoSlot, PlayerState), re/upgrades.md (UpgradeModel, ApplyUpgrade NRE chain), re/fabricator.md (GadgetDirector, Fabricator UI, gadget menu), re/time-weather.md (TimeDirector, WeatherRegistry), re/save.md (persistence ids, AutoSaveDirector, sidecar), re/world.md (InstantiateActor, type groups, AccessDoor, MetaGameDirector), re/ui.md (HUD notification list, UIDisplayManager, save list panel), re/unity.md (prefab rendering).

## Tabs

| tab | contents |
|---|---|
| General | Newbucks set / add / subtract; Noclip (status line, key binds, speed); Player card: refill health / energy, clear radiation, infinite energy, infinite health, teleport to map marker, unlock ranch expansions |
| Vacpack | slot row (amount set / fill / empty, move / swap, unlock a locked slot, right-click tier menu), item picker grid (Put, Put in slots) |
| Spawn & Craft | spawner (items in front of the player) and "Refinery Crafter" (gadgets for free, variants, mod buildables, refinery materials) |
| Upgrades | "Upgrades & Blueprints": every player upgrade raised / lowered / discovered, gadget blueprints locked / unlocked, mod buildable cards |
| Weather | "Time & weather": clock (skip ahead, rewind, pause) and weather changer |

Plus, outside the overlay: **G** in the game's Fabricator adds the selected recipe's missing materials.

## Turning it on

- Only setting: the automatic `enabled` (default **off**, in `sr2mods.json`).
- Off: the tab shows an explanation and **Enable sandbox**. That button and the tab bar's switch open a confirmation ("Sandbox edits change your save directly. Enable?", warns that the save gets marked as cheated; Cancel / Enable; Esc cancels). Only Enable writes `sandbox.enabled = 1`. Turning it off needs no confirmation, but is impossible while a marked save is loaded.
- No accidental path: the Options "Mods" tab skips the module (`options_tab.cpp`), profiles neither save nor load `sandbox` (`core/profiles.cpp` `Skipped()`). Editing `sr2mods.json` by hand still works.
- The setting is kept **per save** (see "Sandbox on / off per save").

## Save safety

Every change goes through the game's own methods (pickup, purchase, vacuum paths), so their validation applies; values are clamped to what vanilla can hold; nothing is written into the save file directly.

| tool | game call | limits |
|---|---|---|
| Newbucks set / add / subtract | more: `PlayerState.AddCurrency(cur, n, true, null)`; less: `SpendCurrency` | `0..PlayerModel.maxCurrency` (999,999,999); currency = `CurrencyUtility.DefaultCurrency` only if `_persistenceId == 1`; coin popup shows; an open plot menu is rebuilt (`RefreshPurchaseMenus`) |
| Refill health / energy, clear radiation | `SetHealth(max)`, `SetEnergy(max)`, `SetRad(0)` | |
| Infinite energy | 10 Hz: `GetCurrEnergy < max` -> `SetEnergy(max)` | per save toggle; only while on |
| Slot amount | up `AmmoSlotManager.MaybeAddResource(id, slot, n, false)`; down `Decrement` | `0..GetSlotMaxCount`; unlocked slots only (MaybeAddResource does not check) |
| Put item | `Decrement` other contents, then `MaybeAddResource` | catalog + saveable + slot accepts it |
| Put in slots (`Op::StowItem`) | slots in order from the selected one, then the rest dropped through the spawner | other items never removed (except the first item's selected slot); up to 60 dropped per item, only if in the spawn list |
| Move slot | merge / onto empty / swap with `MaybeAddResource` + `Decrement` | both unlocked + saveable; swap only if both fit; rollback on failure; slime emotions not carried |
| Unlock slot | `UpgradeModel.IncrementUpgradeLevel` on each upgrade with a modifier on the slot's unlock property, one level at a time until `IsUnlocked` | never past the last level, 16 per upgrade |
| Spawn | `InstantiationHelpers.InstantiateActor(prefab, CurrentSceneGroup, ...)` (the vacpack's shoot path; saved like any loose item) | spawn list + saveable; no Tarrs while disabled; up to 60 per request (`kMaxDrop`) |
| Craft | `GadgetDirector.CanAddGadget`, `AddItem(def, n)` (x2 for `BuyInPairs`) | recipe list; 1..99; locked blueprint stays locked but the gadget is crafted |
| Add material | `GetRefinerySpaceAvailable`, `AddItem(t, n)` | materials list; 999 per type |

### Only saveable items

A type the save can't store breaks **every** save while it exists (the Glitch Tarr did: `KeyNotFoundException` in `BuildActorData`, save aborted). The module reads the save's persistence-id table and keeps a type in the vacpack list, spawn list, crafter or materials only if `ContainsKey(type.ReferenceId)`; each request checks again. Left-out types are logged once per list (`spawn list: left out N type(s) the save can't store: ...`). Table not readable yet: list built on a later refresh; symbols missing: nothing offered ("save check unavailable"). RE: re/save.md (SaveReferenceTranslation).

custom_upgrades' runtime clones (Quantum Storage Link, DJ Booth, DJ Speakers, Slime Radio; `custom_upgrades::IsModGadget`) never go through `AddItem` / `InstantiateActor` from here; they are granted through `buildables::Grant` (see "Mod buildables").

### Tarrs disabled

With the save's Tarrs off (`GameSettingsDirector.TarrEnabled`), spawn-list members of `GameModel.tarrTypeGroup` are grayed out ("Tarrs are disabled in this save's game settings.") and refused. Read with every snapshot.

## Vacpack item catalog

Every loaded `IdentifiableType` a vanilla vacpack can store (not player / scene object, prefab `Vacuumable` of size NORMAL, accepted by some slot's `IsAllowed`), with a slot mask so the grid lists only what the selected slot takes. Names: localized name (rich text stripped; a missing translation `!! key !!` falls back to the asset name), duplicates get the asset name appended. Built once (logged with size and time), rebuilt only when slot definitions change; entries kept with GC handles (freed in a posted job). The UI gets opaque ids (the type's address) and never dereferences them; requests are checked against the catalog (handle, class, `IsAlive`).

**Icon** (cached per type, registered with `ui/item_icons`), first that exists: `IdentifiableType.icon`; the class's virtual `get_Icon` (largo appearance icon); for a largo with neither, its two base slimes' icons drawn overlapping (`icon` + `icon2`); a group icon (`groupType`, else the first leaf `IdentifiableTypeGroup` with an `_icon` that contains it); else rendered from the prefab.

**Kind** (tile accent, group tabs, tooltip tag): `SlimeDefinition` (largo if `IsLargo`), `GadgetDefinition`, `ToyDefinition`, `LiquidDefinition`, `IsPlort`, member of `StandardCrateClass` (Crate), `IsAnimal` (Meat, in Food), member of `FoodGroup` (Food), of `CraftGroup` (Resource), refinery-storable (Resource), else category asset names, else Other. All type groups are read once (one `FindObjectsOfTypeAll`, the first one-time step of a refresh). Logs: `item kinds from the game's groups: ...`, `spawn list by tab: ...`. Group contents: re/world.md (IdentifiableTypeGroup).

**Crate names**: from the asset name's area code (`kCrateCodes`: Fields -> Rainbow Fields, Gorge -> Ember Valley, Strand -> Starlight Strand, Bluffs -> Powderfall Bluffs, Laby<Area> -> Dream Land / Lava Depths / Terrarium / Waterworks / Shadow / Labyrinth Hub, Gold variants). Labyrinth area names are English (no string table entry).

**Group tabs** (icon only). Vacpack: All / Plorts / Slimes / Food / Resources, plus Other while it has items. Spawner: All / Plorts / Slimes / Largos / Food / Resources / Crates / Other, each only while non-empty. Other (`kGroupOther`) = Toy, Other, Liquid, Gadget, drawn as three dots. Icons, read once after the spawn list: the drone station's `DroneTargetDefinitions` (PLORTS -> Plorts, VEGGIES -> Food, CRAFTING_MATS -> Resources); Slimepedia sprites by name when loaded (`iconCategorySlimes` / `pediaHomeSlimes`, `iconLargoPedia_small` / `iconLargoPedia`; `EnsureNamedSprites`, one `FindObjectsOfTypeAll(Sprite)`, logged `game sprites by name: ...`); else the Pink Slime's icon; else type groups with an icon by asset name; else the tab's first item's icon. Logged `group tab icons: ...`.

**Slimes the vacpack can't hold** (round 13: "show gold and lucky slime in inventory but gray them out"): the picker grid also lists every non-mod slime from the spawn list that is not in the vacpack catalog (Gold, Lucky, Tarr), sorted in by name, grayed (`Tile::dimmed`, `Tile::noDrag`). A click only sets the status line, Ctrl+click / wheel do nothing, the tooltip says "The vacpack can't hold this slime"; the right-click menu opens as usual (Spawn, radiant, a grayed Give).

**Slime diets**: read once after the spawn list: `SlimeDefinition.Diet` -> `FavoriteIdents` / `ProduceIdents` and reverse lookups, sent as `Snapshot::relations` (tooltip rows, related-tile highlight, right-click "Give ..."). Largos carry their base slimes (`Item::base` / `base2`); `Relation::largos` lists per slime the largos it is part of (right-click "Spawn: ... Largo"; largos are always spawned, never given).

### Put in slots

Each selected item is one `Op::StowItem`, applied in order on the main thread against the live slots; batch notices (see Spawner). Amounts in the grid go up to 999 (Shift+wheel 1, Ctrl+wheel 10); what doesn't fit is dropped in front of the player (spawner placement, at most 60 per item). The give menu uses the same op without replacing anything. Logs: `tab: Put in slots, N item(s), selected slot S` and per item `Put in slots: Pink Slime -> slot 3 +50, slot 5 +20; dropped 10 in front of you` (or why nothing went anywhere).

### Vacpack slot unlock

A slot's unlock is a property driven by upgrade modifiers (ADDs on `AmmoSlotsAvailable`, default 4; several upgrades stack: custom_upgrades' slots 8 / 9 need its Extra Tank III-IV on top of a maxed Extra Tank II). `SlotUnlockUpgrades` lists every upgrade with a modifier on the slot's property (in `upgradeDefinitions` order); `DoUnlockSlot` raises them one level at a time, checking `IsUnlocked` after each. A slot is `unlockable` in the snapshot when one of them still has a level. Log: `Slot 8 unlocked (<upgrade> -1 -> 0)`. The open Fabricator is refreshed afterwards. RE: re/vacpack.md (AmmoSlot.IsUnlocked).

## Spawner

**List**: every loaded `IdentifiableType` whose prefab has an `IdentifiableActor` (slimes, largos, plorts, food, toys, resources, crates, eggs, ...), except player, scene objects (gordos), `GadgetDefinition` (placed through gadget mode), `LiquidDefinition` and unsaveable types; sorted by name; the Slime Radio added as a mod item (`MergeModSpawnables`). Types with a prefab but no `IdentifiableActor` are logged once. Built once on a refresh in game (retried on the next enable if empty).

**Layout**: search box with **Spawn N**, group tabs on their own row under it (`item_grid::Options::tabsBelow`); tiles take amounts like the vacpack picker. Largos tab grouped by base slime (section per slime, each largo under the alphabetically first of its two bases, `Item::family` / `family2`). Crates tab: a section per area (`EnsureCrateAreas`: one `FindObjectsOfTypeAll(ZoneDefinition)`, each zone's `_crates` named by its `_localizedName`; unlisted crates fall back to the asset-name code). Log `crate areas: ...`.

**Placement**: main camera position, forward flattened (body facing when looking straight up / down), 2.5 m ahead, 0.3 m below eye height; rows of 5 (0.55 m apart, rows 0.7 m higher; rows of 10 above 20 items); a 3.1 m raycast pulls the spot in front of a wall (hit - 0.6 m, at least 0.5 m). A batch's spawns share one grid (`spawnNext_`). The `Nullable<AmmoMetadata>` argument is passed zeroed (re/world.md).

**Radiant slimes** (round 12, "spawn their shiny version"): SR2's rare variant is the radiant slime (re/slimes.md "Radiant slimes"); there are no other rare spawnable variants (secret styles are a cosmetic appearance choice). The right-click menu of a slime (spawner and vacpack picker) has "Spawn: Radiant <slime>", of a largo (spawner) one entry per base slime it can be radiant from ("Spawn: Radiant Pink Tabby Largo", round 14 naming); grayed out ("The game has no radiant form of this slime.") when the definition has none (`Item::radiant` from `SlimeDefinition.RadiantBase` / `RadiantLargo0` / `RadiantLargo1`). `Op::SpawnRadiant` (slot 0 / 1 / 2) goes through `DoSpawn` like a normal spawn, then `SlimeRadiant.SetRadiant(base)` on each actor before its first frame, so `Start` applies the radiant look and the save keeps it. Not marked collected: vacuuming one counts as catching it (pedia entry), losing it adds nothing to the Lost and Found. Logs `spawner: radiant slimes available ...` at setup, `Spawned 3 x Radiant Pink Slime`, a warning per actor that has no `SlimeRadiant`.

**Round 13 context menu**: radiant entries show the Slimepedia's radiant icon (`Item::radiantIcon[slot]`: the radiant `SlimeAppearance._icon`, which `RadiantSlimePediaEntry.get_Icon` returns; a largo's own `RadiantLargo0/1` icon unless it is missing or its plain icon, else the base slime's radiant icon; the item's icon when there is none). The spawner's slime menu also has "Spawn: <slime>" (it was vacpack only). In a slime's **Largos** submenu a click on a largo still spawns it; a largo with radiant forms is a submenu row (`largoEntry`: click read before `BeginMenu`, then `CloseCurrentPopup` closes the chain) whose own submenu has "Spawn: <largo>" (keyboard path) and its radiant forms. The menu opens anchored at the right-click (`item_grid::Result::menuAt`, kept inside the grid's visible area; keyboard: the tile's bottom left), left to ImGui, Lucky Slime's menu (round 12) opened under the search bar.

**Round 14 context menu**: placement rule: down-right of the right-click; up only when the menu does not fit below, left only when it does not fit to the right; fitting neither way, against the screen's far edge (main viewport work rect). Round 13 used `SetNextWindowPos` with a pivot (`ImGuiCond_Appearing`) chosen by screen half, which read as random; and ImGui applies a pivot on the frame after opening with the size measured on the popup's hidden first frame, which lays out at width 0 (`Begin` resets a reappearing popup's Size / ContentSize), so the `PushTextWrapPos(0)` "No favourites or plorts" line wrapped word by word and made Lucky Slime's menu tall enough to land near the top of the screen. Now the menu is placed every frame (`ImGuiCond_Always`) from `g_ctxSize`, the layout size measured at the end of the previous frame (`DC.CursorMaxPos / IdealMaxPos - CursorStartPos + 2 * WindowPadding`, what auto-fit uses next frame); the first frame is hidden, so its stale size is never seen. Every row has a fixed layout width (no window-width-dependent wrapping), so frame 1's measurement is exact. Submenus are still placed by ImGui (child menus: right, else left). Rows are at least 250 px and wider for a long label (`rowWidth`), with `SpanAvailWidth` highlights; a `BeginMenu` row (empty label, text drawn by hand) reserves its width on the parent window before `BeginMenu` (`reserveRow`), since a submenu made only of such rows came out ~44 px wide. A largo's radiant rows are named with the slime it is radiant from first: "Spawn: Radiant Tabby Pink Largo" (the spawn status says the same; the old "(radiant X)" form is the fallback when a base name is unknown). Log at the spawn list: `spawn list slimes: ...; N slime(s) / largo(s) with a Slimepedia radiant icon`.

**Special slimes** (round 13, "werent there 3 special slimes?"): the game ships 26 slime icons (`iconSlime*` bundles: 23 regular, Gold, Lucky, Tarr) and the spawn list has 26 slimes (log `spawn list by tab: slimes 26`), so all three specials (Gold, Lucky, Tarr; the Tarr grayed while the save's Tarrs are off) are listed. The only one left out is the Glitch Tarr: its save id is missing, and a type the save can't store breaks every save (see "Only saveable items").

**HUD notices**: spawns, crafts and refinery adds that worked show pop-ups in the game's notification list, not the status line ("Spawned 5 Pink Plort", "Crafted 2 Home Teleporter", "+5 Pink Plort (refinery)"), queued as free-text entries through `custom_upgrades/sale_notify` (`SaleNotices::QueueText`, item sprite, text via `core/text_override`). The tab sends Craft N / Spawn N / Put in slots as one batch (`Action::batch` 1-based, `batchSize`); notices are collected (`batchNotices_`) and `FlushNotices` queues **one notice per item type and kind**, all handed over together (`SaleNotices::FlushText`) so the first page holds as many as the HUD shows; later pages are handed over as soon as nothing is pending. A batch whose last request never arrives is flushed on the next request or after 2 s. Items before a batch's last skip the snapshot / Fabricator refreshes. Notices wait while a menu hides the HUD, are dropped after 2 minutes, and fall back to the status line when the HUD list or text sink is missing. The HUD list is kept with a GC handle; queued notices retry every 0.05 s. Log: `text notices: N notice(s) handed to the HUD as one page (page size P, ...)`. RE: re/ui.md (ItemAcquisitionNotificationList).

### Icons rendered from prefabs

Types with no icon (crates, giant eggs, vine bulb, Mysterious Statues, Echo Net, pollen cloud, drone component) get one rendered from their prefab (`prefab_icons.cpp`, header comment): meshes copied (nothing instantiated) on layer 31 far above the world, an orthographic camera and three directional lights, 128 x 128 sRGB target, auto brightness (up to 3 renders, then levels / gamma), background keyed out, `Sprite.Create`, everything destroyed in the same call. LOD<n> (n > 0) children and grass-interaction quads are skipped. One icon per refresh, each type once; stops after 6 failures in a row. Log: `rendered N icon(s) from prefabs, M failed (T ms in all)`. RE: re/unity.md.

## Crafter ("Refinery Crafter")

**Recipes**: every `GadgetDefinition` whose `CraftingCosts` has ident or currency costs (read from game data). **Tabs** (icons, like the Fabricator): All (every tab's items, a section per tab; also used for search results), Pinned, the categories Utilities / Warp Tech / Decorations (then others), Crafting Resources (the refinery materials). Opens on All; keeps the selected category when the tab list changes.
- Categories: with the Fabricator's `FabricateMenuConfiguration` loaded (only after the Fabricator UI was opened), its `_categories` / `GadgetFabricateCategory.Contains`; else the gadget groups the categories are built on (`GadgetUtilitiesGroup`, `GadgetWarpGroup`, `GadgetDecorGroup`), titles from `_localizedName`, icons from the category sprites by name (`iconCategoryUtilities` ...), else the first gadget's icon. No retry timer (a `FindObjectsOfTypeAll` retry cost 25 ms frames). Log `crafter: N gadget recipes, M categories ...`.
- Order: the Fabricator's (`GadgetFabricateCategory.IndexOf`, else the group's `_memberTypes` index; `Recipe::order`, unknown last).
- Locked blueprints are dimmed in place; crafted anyway (the blueprint stays locked; `AddItem` has no blueprint check).
- **Pinned**: the game's one pinned recipe (`PlayerModel.PinnedRecipe`, matched by reference id; log `crafter: pinned recipe '<id>'`). The tab icon is the Fabricator's `pin` sprite, which exists only while the Fabricator UI scene is loaded: the module preloads that scene briefly with `UIDisplayManager.RequestPreloadDisplay` ("Fabricator" display; `PinIconTick`, every 10 s at most, 6 tries, released when found / Sandbox off / after 30 s) and also searches after each `FabricatorUI.Start`. Until found the overlay draws a push pin. Logs `preloading the Fabricator UI ...`, `the Fabricator's Pinned icon found (...)`.
- Cost tooltip: one `CostLine` per ident / currency cost with icon (`RecipeCost`). Blueprint state and inventory counts refresh every 2 s while the tab is open and right after a craft.
- Crafting mirrors `GadgetFabricatableItem.FabricateAndSpendCost` without the spend, analytics and `OnGadgetFabricated`.

**Variants** (right-click menu): `GadgetDefinition`s with `VariantOf` set go into their base recipe's `Recipe::variants` and `recipeById_`; crafting one is `AddItem(variant, n)` (counted on the base gadget). A variant with its own cost is also a recipe (category -1, no tab). Log `crafter: N gadget variant(s) of M recipe(s) ...`.

**Mod buildables** (custom_upgrades, `buildables::List()`): crafter tiles (`MergeModRecipes`, after their base recipe: Quantum Storage Link after the Refinery Link, DJ Booth after the Scientist Desk, DJ Speaker and Slime Radio after the Boombox; a tile without a found base goes into the first category), also appended to their base's variants (`MergeModVariants`), and the Slime Radio in the spawner. Crafting / spawning goes to `DoCraftMod` / `DoSpawnMod` -> `buildables::Grant` (vanilla base gadget + kit in the mod's sidecar; radio spawned, at most 5 per request, `kMaxRadio`); the clone never reaches the save. Tiles carry an **[M]** badge and "Added by the mod" (overlay only). Icons: composite renders of the game prefabs posed like custom_upgrades' dressing (`RenderNextModIcon`, `prefab_icons.h` `RenderPieces`; Storage Link tinted purple; booth = desk without clutter + deck + monitors + speakers + bench + pillow), shared via `sandbox::SetModIcon`; on failure the base gadget's icon. Refreshed every 2 s with the recipe state; they go when custom upgrades are off. Logs: `crafter: N mod buildable tile(s) ...`, `mod icon: <name> rendered (...)`.

**Open gadget-mode menu**: after every `Op::Craft`, `RefreshGadgetMenu` re-reads an open `NewGadgetSelectionUI`'s categories and re-selects the shown one, so new gadgets appear without reopening or switching tabs (selection and page reset). Round 11: the round 10 version found the menu with `FindObjectOfType`, which never found it (no refresh line in the log). The instance now comes from two always-on hooks (HookManager module `sandbox_gadget_menu`): `NewGadgetSelectionUI.OnEnable` (post, stores the raw pointer; `Tick` takes a GC handle next frame) and `OnDestroy` (pre, drops it; handle freed by a posted job). When the menu is not open at the craft, the refresh waits (`GadgetMenuTick`, checked every 0.25 s) until it is. Log `crafter: gadget menu tracked by hooks ...` at setup and `crafter: open gadget menu refreshed ...` per refresh. RE: re/fabricator.md.

**Crafting materials**: every type `GadgetDirector.IsStorable` accepts (minus gadgets, player, scene objects, unsaveable), sorted by name; added with `AddItem(type, n)` clamped to the refinery space (999).

## Player extras

`player_extras.cpp`, bridge `ui/sandbox_extras_bridge.h`, drawn at the bottom of the Player card. Each tool optional (setup logs which are available; a disabled button's tooltip gives the reason). Only while the module is on.

- **Infinite health**: per save toggle. Hook `PlayerState.Damage` (DetourGuard, switched with the module) returns "not dead" without applying damage; plus a 10 Hz top-up for health lost other ways. Without the hook only the top-up runs (tooltip says so).
- **Teleport to waypoint**: `MapDirector.TryGetPlayerNavigationMarkerLocation` with the current scene group and zone (asked every snapshot; disabled without a marker in this zone; another zone's map is not offered). Then the shared ground teleport `modules/map_tools/ground_teleport.h` (loads the target regions in place, picks a safe floor, waits up to 6 s, lands; refuses if nothing safe; never while knocked out or loading; heading kept). The result can come seconds later; button disabled meanwhile. Log `Teleported to the map marker: ...`; result also as a HUD pop-up (teleporter icon). RE: re/map.md (MapDirector).
- **Unlock ranch expansions**: for every loaded `AccessDoorUIActivator` with a locked door, what the purchase result does minus spend / analytics / achievement / cue: door OPEN, linked locked doors CLOSED, `OnUnlockedEvent` raised (state saved in `AccessDoorModel`). Scan on the button and every 5 s while the page is open (count in the tooltip); asks for confirmation. Only at the ranch. Log `Unlocked N ranch expansion(s): ...`. RE: re/world.md (AccessDoor).

## Noclip

`noclip.cpp/.h` (round 12: "noclip would be nice to have aswell as a sandbox feature (bindable to a key)"). Card "Noclip" on the General page under Newbucks: a status line (extras bridge `noclip` / `noclipWhy`: on, unavailable and why, "bind a key", or off), then the settings of group `Noclip` (module `sandbox`, so neither profiles nor the Options tab carry them): **Noclip key** (unbound by default), **Fly up** (Space), **Fly down** (Ctrl), **Noclip speed** (15 m/s, 1..200), **Shift speed multiplier** (3x). RE: re/player.md "Noclip".
- **Toggle**: only the key's edge (`GetAsyncKeyState`, game window in front, overlay closed, cursor locked = no game menu or text field), only while Sandbox is on. Round 13 ("water knocks me out, remove the toggle, just have noclip key (no key by default)") dropped the page switch and `extras::Op::Noclip`; the key defaults to 0 (unbound).
- **On**: the player's `SRCharacterController` (`SceneContext.player`, else `GetComponentInChildren`), its `KinematicCharacterMotor`. Saves `_solveMovementCollisions`, `_solveGrounding`, `<BypassGravity>`, then every frame: both solvers off, `BaseVelocity` 0, `BypassGravity` on, `GravityAmount` 0, and the motor's `_transientPosition` + this frame's move through `SetPosition(pos, true)`. The move: the controller's `<InputVector>` (the game's movement input, so rebinds and gamepads work; the WASD keys when it reads zero) along the main camera's forward / flattened right, the up / down keys straight up / down, `speed * Time.deltaTime` (nothing while paused, in a menu or with the overlay open). The motor keeps running, so mouse look, abilities and the KCC interpolation are untouched; what one KCC tick adds between frames (air control) is zeroed the next frame. Game teleports simply move the transient position, so they keep working.
- **Off** (key, Sandbox off or faulted, the player object gone or replaced, no scene context, a scene group change; checked every 0.25 s): the saved solver flags and gravity bypass written back, `BaseVelocity` and `GravityAmount` 0. Field writes only, so it also restores with the module faulted. Turning it off inside a wall leaves KCC's decollision to push you out.
- **Kill volumes** (round 13): flying into the slime sea knocked the player out (`KillOnTrigger` -> `DeathHandler.Kill` -> `PlayerDeathHandler.OnDeath`; the capsule still overlaps triggers with the solvers off). Two optional hooks, toggled with the module: `DeathHandler.Kill` returns without killing while noclip is on and the GameObject is the noclipping player (controller's GameObject, `GetComponent<SRCharacterController>` == it, or `SceneContext.player`), which covers `KillOnTriggerCloudOcean` and any other kill path too; `KillOnTrigger.OnTriggerEnter` is skipped for `PhysicsUtil.IsPlayerMainCollider(collider)` while on (no "Fallthrough destroying" line, no splash FX). Log `noclip: kept the player out of a kill volume (...)` once per noclip. Gotcha: the skipped trigger enter does not come again, so turning noclip off while inside a kill volume (under the slime sea) does not knock you out either; you fall until something else catches you (noclip on again to fly out). Leaving the volume and entering it again with noclip off knocks you out as usual. RE: re/world.md "Hazard volumes".
- **hl2_movement**: `modules/common/noclip_state.h` flag; its `UpdateVelocity` detour runs the vanilla tick while it is set and drops its fall-through reference (else "150 m below the last ground for 3 s" would teleport you back after flying down), the ramp projection and the HL2 sprint drain.
- Space also starts jumps: in the air with the jetpack it may run (energy, sound) while you rise; rebind Fly up if that bothers.
- HUD pop-ups "Noclip on" / "Noclip off" (`PlayerExtrasNotice`, teleporter icon). Logs: `noclip ready (...)` at setup, `noclip on at (x, y, z): ... (were: collisions .., grounding .., bypass ..), N m/s`, `noclip off (<reason>): collision and gravity back`.
- Not stored: noclip is off after a load. A save made while inside a wall loads you there.

## Upgrades manager

`upgrades.cpp/.h`, bridge `ui/sandbox_upgrades_bridge.h`, UI `ui/sandbox_upgrades.cpp`. Hooked into `module.cpp` by `SetupUpgrades` / `UpgradesEnabled` / `UpgradesTick` and a host (recipe lookup, Fabricator refresh). Each part optional; setup logs `upgrades manager: levels ok, discover ok, blueprints ok, Fabricator rebuild ok`.

- **State** (every 0.5 s while open and after each change): every `upgradeDefinitions` entry with levels: base name (level 0's title without its tier numeral), icon, level, level count, discoverable / discovered, the slot-count upgrade (`_uniqueId` 40). Vanilla level counts of the two upgrades custom_upgrades extends (20 Tank Booster: 8, 40 Extra Tank: 2) are sent so the UI marks custom tiers.
- **Levels**: `UpgradeModel.SetUpgradeLevel(def, level)` (-1 .. last; levels can jump; Max goes straight to the last; nothing spent). A lower level really takes the effect back. Resetting a discoverable upgrade to "not bought" gives back its missing level-0 components so it stays discovered. Lowering never deletes items (a locked slot keeps its contents hidden, a smaller tank keeps counts above the maximum). A `SetUpgradeLevel` that stored the level but threw counts as done (warning). For Tank Booster / Extra Tank the vacpack is logged before / after; if slot 1's maximum differs from the AmmoLimit value, every slot's maximum is set with `AmmoSlotManager.UpdateMaxCount`. custom_upgrades' tiers are levels of the same definition; its `SetUpgradeLevel` detour keeps its sidecar in step (cleared at `level <= vanilla max`). Log `Tank Booster: VIII -> X (of XII), free`.
- **Stale HUD listeners** (`PruneSlotListeners`): a lowered Extra Tank leaves destroyed HUD slot views subscribed to the slots, which made every later slot notification throw a `NullReferenceException` inside `SetUpgradeLevel` and leak modifiers. Before every level change (and every 3 s with Sandbox on) listeners whose target is a destroyed `UnityEngine.Object` are removed from each slot's `OnSlotChanged` / `OnAmmoChanged`. Logged only when it removed something. RE: re/upgrades.md (the full chain).
- **Effects repair** (`Heal`, safety net): after each level change, once after Max, every 3 s with Sandbox on in a save. Per handler, modifier nodes from upgrade levels stay only for the upgrade's current level and once per modifier; stray ones are removed, stale remembered entries dropped, an upgrade without its modifiers gets `ApplyUpgrade` once. Non-upgrade modifiers are never touched. Log `upgrade effects repaired (...)`. Modifiers are runtime only, so saves are never inflated; counts put in while capacity was inflated stay (lower them in the vacpack editor).
- **Discover / Undiscover** (discoverable upgrades only): Discover = `AcquisitionUtility.AcquireUpgradeComponent(component, 1)` per missing level-0 component (the pickup path). Undiscover = `SetUpgradeLevel(-1)` if bought, then `UpgradeComponentsModel.LoseComponent` for level 0's components. Log `Jetpack undiscovered (...)`.
- **Max all / Discover all**: per-tab buttons; the legacy ops `Op::DiscoverUpgrades` / `Op::UnlockUpgrades` in `module.cpp` remain but the page no longer sends them.
- **Blueprints** (the crafter's categories): unlock `GadgetDirector.AddBlueprint(def, false)`; lock = remove from `GadgetsModel.blueprints` (the game has no RemoveBlueprint; pedia entry and inventory stay). The UI shows the requested state for 2.5 s until the snapshot follows. Log `Unlocked 12 blueprint(s): ...`.
- **Mod buildables tab**: cards from `buildables::List()`; `Op::SetModBlueprint` (0 follow the base gadget's blueprint / 1 unlock / 2 lock) -> `buildables::SetUnlock` (`buildables.unlock.<id>` in the save's sidecar, never the vanilla save). Custom upgrades off: the tab says so.
- **Rebuilding an open Fabricator** (`RebuildFabricator`, also used by slot unlock and the legacy ops via `RefreshFabricator(true)`): re-runs what opening does (fresh category runtimes, buttons rebound, grid / title on the same category, page and selection kept, details refreshed), so icons, tiers and new gadgets show without reopening. A grid on the Pinned category is left alone. Log `Fabricator rebuilt as on opening: ...`. RE: re/fabricator.md (FabricatorCategoryList).

## Time & weather

`weather.cpp/.h`, bridge `ui/sandbox_weather_bridge.h`, UI `ui/sandbox_weather.cpp`; `SetupWeather` / `WeatherEnabled` / `WeatherTick`. The clock and the weather are each optional with their own reason (`time of day ready: ...` / `weather changer ready: ...` or `... unavailable: <why>`). No hooks; all calls `Resolver::Invoke` on the main thread; state published every 0.5 s while the page is open (field reads, no FindObjects). RE: re/time-weather.md.

### Time of day

The clock ("Day 12, 14:35", period of day, "paused", or "skipping ahead to 06:00, ... left"), quick buttons for the next 06:00 / 12:00 / 18:00 / 00:00, a 5-minute slider with **Skip ahead** / **Rewind**, **Pause time**, and **Stop skipping** while skipping.
- **Forward** (`Op::TimeSkip`): only through the game's fast-forward (`TimeDirector.FastForwardTo`, the sleep path) to the next occurrence (at least 30 s ahead), after unpausing and ending a running skip. Everything timed runs through the skipped hours and the ranch catches up; takes `FfSecsPerGameDay * hours / 24` real seconds. No instant jump forward (it would hand every system one huge step and skip the ranch simulation).
- **Back** (`Op::TimeRewind`, after a "Set the clock back?" confirmation): the last occurrence, at most 24 h, never before `worldTimeStart`. `worldTime` and `lastWorldTime` written together, `_lastDay` / `_lastHour` set (no events). Nothing is undone; whatever was scheduled comes that much later. The save keeps the new time.
- **Pause** (`Op::TimePause`): `WorldModel.pauseWorldTime`; game runs, clock stops. Not saved; reset with the save's toggles; Sandbox off unpauses a clock the card paused.
- Log / status: `Time: skipping ahead from ... to ... (15.4 game hours, about 19 s), as sleeping does`, `Time: set back from ... to ...`, `Time paused at ...`, `Time runs again from ...`, `Time: skip stopped at ...`; once per save `clock: day N, HH:MM ...; a game day takes X real s, Y s while fast-forwarding`.

### Weather changer

The current zone's weather and forecast; pick a weather and state for this zone or all zones; start now, end weather, optionally lock, back to natural.
- **Catalog** (once per `WeatherRegistry`, i.e. per load; GC handles): zones (`DEBUG_GetWeatherZones`), patterns (`DEBUG_GetZoneWeathers`), states from transitions (starting first). Names: pedia title / `StateName` / zone `_localizedName`, else asset names; icons `WeatherTypeMetadata.Icon`. Logged once: `weather: N zone(s), N pattern(s), N state(s): ...`.
- **All changes go through `DEBUG_SetZoneWeather`** (the game's transitions, fades, sounds, spawns apply). The forecast is never written: the save holds only vanilla forecast entries.
- **Start now** (`Op::Start`): ends other running patterns in the zone, runs the picked pattern / state (Clear skies = only the ending). Without **Lock**: for the state's `MinDurationHours` clamped to 1..24 game hours (4 h for clear skies / no length), then back to natural. With Lock: until **Back to natural**.
- **All zones**: pattern matched per zone (same asset, else same `WeatherTypeMetadata`, else same name), state likewise (asset, name, position). Unmatched zones are left alone and named.
- **End weather** = Start with Clear skies. **Back to natural** (`Op::Natural`): ends what runs, starts the forecast entries that are started and not over, removes the lock.
- **1 Hz enforce** (only with Sandbox on and the registry ready: `_model` set, `_firstLoad` false): per overridden zone, other patterns ended and ours put back. Log (first 30 per save) `weather: <weather> in <zone> re-applied (the game changed the weather): ...`; timed expiry `weather: <zone> back to natural weather (...)`.
- **Locks persist** in the save's sidecar Sandbox entry under `"weather": {"note", "locks": [{"zone", "pattern" ("" = clear), "state", "label"}]}` (`cheated_save.h` `SaveData` / `SetSaveData`, key `kDataWeather`); applied once weather is up after a load; a lock whose pattern is gone is dropped. Timed overrides are not stored (running weather is not saved anyway).
- **Where you are**: a zone without weather (the ranch, The Conservatory) shows as a disabled first dropdown entry "Where you are (<zone>: no weather)" (`hereName`).
- Turning Sandbox off leaves a running override until the forecast changes it or the save is reloaded.

## Fabricator: G adds the missing materials

While Sandbox is on and the game's Fabricator is open, **G** adds what the selected recipe still lacks. Code `module.cpp` (`FabricatorTick`, `DoFabricatorMaterials`, `FabricatorHintTick`, `RefreshFabricator`).
- **Open detection**: two always-on hooks (module / fault name `sandbox_fabricator`, created in `SetupUnlocks`): `FabricatorUI.Start` adds the instance to a list (GC handle), `OnDestroy` removes it; open = a listed instance active in the hierarchy, checked at 4 Hz. (`FindObjectOfType(FabricatorUI)` never found it and cost 6-14 ms.) Log `Fabricator open: G adds ...` / `Fabricator closed`.
- **Key**: `GetAsyncKeyState('G')` edge, game window in front, overlay closed (OS state, so the Fabricator's input capture doesn't matter).
- **Key bar entry**: "[G] Add materials" cloned from a hint of `FabricatorUI._inputLegend` (`modules/common/legend_hint.h`, shared with map_tools' "[RMB] Teleport"), placed right before the Pin entry (re-checked at 4 Hz since the bar is rebuilt per page; last if no Pin). Dies with the UI; Sandbox off destroys it; own fault guard. Log `[G] Add materials entry cloned from ...` or once `no [G] Add materials entry ... : <why>`.
- **Recipe**: `FabricatorItemGrid.TryGetSelectedItem` (fallback the hovered index). While the quantity popup is up (`FabricatorUI._confirming`, popup active, not closing) its item and `_countToMake` are used and the popup refreshed like its own +/- (`IFabricatableItem.CheckCanFabricate`).
- **What it adds** (only the missing part, nothing crafted or bought): gadget = `CraftingCosts` x craft count; upgrade = next level's `_costs`. Refinery items `GadgetDirector.AddItem` (clamped to space); upgrade components `AcquireUpgradeComponent` (so G also discovers a hidden upgrade); Newbucks `AddCurrency` (other currencies left alone).
- **Panel refresh** (`RefreshFabricator`): `FabricatorContent.OnItemSelected(item, PROGRAMMATIC)` for the selected item (requirements, button, legend), after G, crafter Add material / Craft, Newbucks changes, slot unlock, Discover, Max; fallback `FabricatorItemDetails.SetItem`.
- Every outcome is logged only (not the status line; `quiet_`): `Fabricator: added 20 Pink Plort, 150 Newbucks for Home Teleporter`, `... for 4 x Home Teleporter (quantity popup)`, `you already have everything for ...`, `select a recipe first`, `the Fabricator is not open`.

## Achievements paused

While Sandbox is on **or** a marked save is loaded, no achievement progress is made (no Steam unlock or stat, the save's progress doesn't move). Banner across the page top: "Steam achievements are paused while Sandbox is on." / in a marked save "Steam achievements are disabled for this save (Sandbox was used)." Code `achievements.cpp`. RE: re/world.md (MetaGameDirector).

Hooks (always on, module / fault name `sandbox_achievements`, DetourGuard; the detour checks the two flags and the object's class):

| hook | while blocked | why |
|---|---|---|
| `AchievementData.IncrementProgress(int)` | returns false | every Grant path; false = platform never called |
| `MetaGameDirector.SetAchievementProgress(Achievement, int)` (public overload) | skipped | every gameplay set |
| `MetaGameDirector.HandleSoldPlort(int)` | skipped | keeps sales out of the pending Beallionaire total |
| `SteamMetaGamePlatform.Grant(AchievementData)` | skipped | backstop: nothing reaches Steam |

Nothing is queued (turning off replays nothing). First blocked call per enable / marked save logs `achievements blocked: ... (Sandbox is on | this save is marked as cheated)`; toggling logs `Sandbox on: achievements paused` / `Sandbox off: achievements resumed`. The hooks are required: if one can't be resolved Sandbox can't be enabled (fail closed). A fault in the editing code doesn't lift the block; a fault in these detours makes them pass-through (logged).

Limits: world-state achievements (pods, upgrades owned, vacpack contents, pedia, query achievements) are re-checked by the game later (e.g. `ValidatePersistantAchievements` on load), so Sandbox-made items still count in a save played with Sandbox off and unmarked. A Beallionaire window opened before enabling loses its pending total.

## Cheated saves

Using Sandbox marks a save as **cheated** for good: achievements stay blocked in it even with Sandbox off. Code `cheated_save.cpp` (block in `achievements.cpp`, `SetSaveCheated`).

- **When**: turning Sandbox on (confirmed) while a save runs, or loading a save after switching it on on the title screen. The file the session was loaded from is marked at once; for the rest of the session every game save marks its new `.sav` too.
- **Where**: the per-save sidecar `sr2mods_saves\<GameName>_<i>.sav.json` (shared with custom_upgrades):
  ```json
  "sandbox": { "cheated": true, "since": "2026-10-24 13:05:12", "game": "<GameName>", "save": "<GameName>_<i>", "save_number": 42, "enabled": true,
               "toggles": {"infinite_energy": false, "infinite_health": false}, "weather": {...} }
  ```
  A mark counts only if `save` and `save_number` match the file (rotation names are reused; 0 = not checked). Only the `sandbox` entry is added / replaced (temp file + rename; an unparsable sidecar is never replaced). custom_upgrades' `WriteSidecar` (runs after our mark in the same save) keeps a `sandbox` entry with its save number; a sandbox-only sidecar counts as "no custom upgrade data" for it (`kForeignSidecar`). Sidecars live in the game folder, not Steam Cloud.
- **Clearing**: not from the UI. By hand, with the game closed or on the title screen: delete the `"sandbox"` entry (or `"cheated": false`) in every sidecar of that game.
- **Hooks** (always on, module / fault name `sandbox_saves`; `SetupWhileDisabled`, so `Setup` runs at startup even with Sandbox off):
  - `AutoSaveDirector.BeginLoad` (pre): reads the loaded file's sidecar; a mark turns the block on before scenes load (so load-time checks are blocked), no mark turns it off.
  - `AutoSaveDirector.CleanupAutosaves` (post; only called by `SaveGameImpl` after a successful store): the newest `<GameName>_<digits>.sav` gets the mark when the session is marked or Sandbox is on.
  - `SaveGamesRootUI.FetchGameSummaryItemData` (post, optional): a marked save gets a "Sandbox: Used" row in the save list's details panel (a cloned Tarr row definition with text overrides and the `iconCategoryUtilities` icon). Sidecar read once, then only `stat`'ed (2 s per file).
- **Poll** (1 Hz frame callback): title screen clears the session (lifts the block); a running save not seen loading (fallback load) is looked up by game name and save number; Sandbox on in an unmarked save marks it.
- **Log**: `loading save '<file>' (game '<game>'): marked as cheated (...)` / `no Sandbox mark`; `new save file '<file>' (save #N) marked as cheated, ...`; `left save game '<game>' (title screen) ...`; `save list: '<file>' shows "Sandbox: Used"`; `cheated-save hooks ready (always on): ...`.
- **Overlay**: the banner (above); with Sandbox off and a marked save the off card shows the same line; the confirmation explains marking and per-save switches.

### Sandbox on / off per save

`enabled` is global in `sr2mods.json`, so `cheated_save.cpp` keeps it per save (`SetSandbox`; from a hook the change is posted):
- At startup and whenever a save is left (title screen): off.
- Loading a save without the mark: off. Switching on on the title screen (confirmed) carries over to the next save loaded (or a new game), which is then marked (`Sandbox switched on with no save loaded: ...`).
- A marked save keeps Sandbox **on and pinned** (`settings::Lock`, `PinSandbox`; `Set(0)` ignored from overlay, Options and profiles; the tab-bar switch is drawn disabled with the reason). Pinned when a marked save is loaded or found running, and right after Sandbox is switched on in an unmarked save; freed when the save is left, an unmarked save loads, or the mark belongs to an older file. Old marks with `enabled: false` also load with Sandbox on.
- Log: `Sandbox switch pinned on: save '<file>' is marked as cheated ...`, `Sandbox switch free again: left save game '<game>'`, `Sandbox off: save '<file>' has no Sandbox mark ...`.

### Toggles per save

Infinite energy and infinite health live in the mark's `"toggles"` (`SaveToggle`, `SetSaveToggle`, `SaveTogglesVersion`). Loading takes the mark's toggles (no mark, invalid mark, title screen: all off). A switch writes the sidecar at once in a marked save. `Tick` (energy) and `PlayerExtrasTick` (health) apply the stored value when the version changes or Sandbox comes back on; Sandbox off stops both without changing what is stored. Spawner / vacpack / crafter state is not stored. Log `Sandbox toggles for save '<file>': ...`.

### Limits

- A save written without the mod has no sidecar, so no mark; deliberately no fallback to older sidecars (another branch of the same game must not mark a clean one).
- Loads that skip `BeginLoad` (`LoadFallbackSave`) are seen only by the poll: their load-time achievement checks are not blocked.
- Sandbox-only sidecars of rotated-out saves stay behind (harmless).

## Threading, faults, performance

- Hooks only for the achievement block, the cheated-save mark, the Fabricator open tracking and infinite health. One main-thread frame callback: the snapshot every 250 ms only while the tab was drawn in the last 0.6 s, the infinite-energy / health top-ups, the queued HUD notices.
- Tab buttons post an `Action` (`main_thread::Post`); the module validates, applies, logs one line (`[sandbox] Newbucks set: 1,200 -> 5,000`, ...), sets the status line (shown only for errors, 8 s) and refreshes the snapshot.
- Every game call is `Resolver::Invoke` (managed exceptions captured with the first five managed frames via `il2cpp_format_stack_trace`); a native fault marks the module faulted and it stops calling the game. Each step re-reads and checks the player objects (`TryGetPlayerState`, `IsAlive` + class on `PlayerState`, `PlayerModel`, `AmmoSlotManager`, slots, items).
- Snapshot data is immutable once published (`shared_ptr<const ...>` lists built on the main thread, swapped under the bridge's lock; changes copy). The render thread keeps its snapshot for the frame. `ui/item_icons` is append-only and holds its own COM reference per texture.
- **Perf rules**: no `FindObjectOfType` / `FindObjectsOfTypeAll` on a timer (they cost 6-25 ms frames). Each refresh does at most one one-time step (type groups, sprites by name, vacpack catalog, spawn list, recipes, materials, diets, group tab icons, crate areas, one prefab icon, one mod icon), so the 30-60 ms scans land in separate frames. The page makes no game calls per frame; the 4 Hz snapshot costs ~0.001 ms.
- The overlay crash on the Sandbox page (2026-10-24) was ImGui's font atlas packing a glyph for an invalid font size, not texture code; fixed in `third_party/imgui` (`ui/NOTES.md` "ImGui checks").

## Not done (possible later)

Other `CurrencyDefinition`s (same Add / Spend path with a picker), keys (`PlayerState.AddKey`), plort market prices, unlocking zones / map.

## Not yet seen in game

Most of this was built from the dump and IDA. Open items worth checking in a log or in play:
- Crafter / spawner: crafting a locked-blueprint gadget then placing it; a crafted variant showing as that variant; HUD page size (`_displayEntries.Length`) and leftover notice delay; crate area sections (which zones are loaded, Labyrinth sub-zones); the Pinned icon preload; mod composite icon framing and lighting; the open gadget menu refresh.
- Vacpack: Shift/Ctrl+wheel with dropped modifier events, slot move / swap, Put in slots drops.
- Upgrades: the Fabricator rebuild without reopening, effects going away on a downgrade, Undiscover hiding, locked blueprints staying locked after save + reload, base names from level 0's title.
- Player extras: whether every damage source goes through `PlayerState.Damage` (a direct `PlayerDeathHandler` kill is not stopped); the teleport landing on a roof / tree; expansions in other zones.
- Time & weather: pattern / state names (the catalog log line), weather for an unloaded zone showing on arrival, the map indicators following, fast-forward while walking around, real `FfSecsPerGameDay`, anything misbehaving after a rewind.
- Fabricator G: the key bar entry's look and place on every page, whether it shows while the quantity popup is up, the popup's counts following.
- Cheated saves: the disabled switch and tooltip, Sandbox coming on by itself in a marked save.
- Newbucks: whether the plot menu rebuild moves its selection.

## History

- 2026-10-23: first version (Newbucks, health / energy, vacpack slots, spawner, crafter, achievements paused).
- 2026-10-24: save check (Glitch Tarr broke saves), item kinds from type groups, rendered prefab icons, cheated-save mark, upgrades manager, weather changer, overlay crash (ImGui font size).
- Rounds 5-10: Put in slots, crate areas, slot unlock through stacked upgrades, per-save on / off and toggles, pinned switch in marked saves, one HUD notice per item, Fabricator G key and quantity popup, variants and mod buildables in the crafter, Pinned icon preload, HUD-listener NRE fix (`PruneSlotListeners`), time of day, "Where you are" at the ranch. Details in git history.
