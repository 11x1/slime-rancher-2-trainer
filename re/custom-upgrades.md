# Custom land-plot upgrades: research + design (SR2 1.3.0, Steam, Unity 6000.0.73f1)

Status: the framework, the silo **Sell-All Button** and the per-plot upgrade **tiers** (2b.2)
are implemented in `modules/custom_upgrades/` + `modules/plot_upgrades/`; the **Sell button**
(2b.1) in `modules/sell_upgrades/`; the **Coop Air Net** (2.4) and the **Hen Collector** (2.1)
as world objects (`modules/custom_upgrades/coop_world.cpp`, section 1.4). All built, not yet
tested in game. Everything else is design.
RVAs are only for reference (GameAssembly.dll 1.3.0). The code finds everything by name.

---------------------------------------------------------------------------------------------
## 1. Framework

### 1.1 Upgrade ids and the vanilla save

* `LandPlotModel.upgrades` is a `HashSet<LandPlot.Upgrade>` (int enum, values 1..20). Push/Pull
  write it to the save as a list of enum values. We did **not** check how vanilla reads an unknown
  value (it could become an unknown enum int, get dropped, or throw), so we treat it as unsafe:
  **custom ids are never put into that set.**
* Custom ids sit in their own range `0x5352xxxx` ("SR" << 16), see
  `modules/custom_upgrades/api.h` (`IsCustomUpgrade`). They only exist in memory: in the menu items
  and the hook parameters.
* Two detours keep vanilla code away from them:
  * `LandPlot.HasUpgrade(Upgrade)` (0x989190, a tail call into `LandPlotModel.HasUpgrade`). For a
    custom id it returns our sidecar ownership and never calls the original. For a vanilla id it
    costs one compare.
  * `LandPlot.AddUpgrade(Upgrade)` (0x988750: `model.upgrades.Add` + `ApplyUpgrades`). A custom
    id is **never** passed to the original, not even when the module is off or faulted. It is
    marked owned in the sidecar state.

### 1.2 Persistence: a sidecar file per .sav file ("extended save file")

* How the game names its files (disassembly, 1.3.0):
  * Folder: `SteamContext.GetStorageProvider` (0xB2FE60) builds a `FileStorageProvider` with
    `savePath = Path.Combine(persistentDataPath, "Steam", <steam id>)`;
    `FileStorageProvider.GetFullFilePath(name)` = `savePath\<name>.sav`.
  * Save: `SaveGameImpl` (0x5D10A0) takes `_currentGameMetadata` (Nullable<GameMetadata> at
    +0x68: hasValue +0x68, GameName +0x70, DisplayName +0x78, SaveNumber +0x80, SlotIndex +0x88),
    stores `IncrementSaveNumber()` back, pulls the game, then loops `i = 0, 1, ...` over
    `string.Format("{0}_{1}", GameName, i)` until `StorageProvider.HasGameData(name)` is false and
    calls `StorageProvider.StoreGameData(DisplayName, name, stream)`. Then `CleanupAutosaves`
    (0x5CE040) deletes every save of that game past the 5 newest (`GetSavesByGameName`, newest
    first). So rotation indices are **reused** (files `_1.._5` exist -> the next save is `_0`, and
    `_1` is deleted): the index is not an age order, the save number is.
  * Load: `Load(GameSaveIdentifier, bool, Stream)` (0x5CFDC0); `GameSaveIdentifier` =
    `{string GameName; string SaveName}` (16 bytes, by reference on x64), `SaveName` = the file
    name without `.sav`. Load sets `_currentGameMetadata = new GameMetadata(loadedGame)` before it
    returns, so the loaded file's save number is readable right after it.
* What we do:
  * Detour `FileStorageProvider.StoreGameData` (0x744BC0): while inside our `SaveGameImpl`
    detour, note the file name (and `savePath`). After `SaveGameImpl` returns a game, write
    `sr2mods_saves\<name>.sav.json`:
    ```json
    { "format": 2, "game": "<GameName>", "save": "<GameName>_<i>", "save_number": 42, "values": {},
      "plots": { "<plot id>": { "type": 5, "upgrades": ["silo_sell_all"], "values": {} } } }
    ```
    keyed by `LandPlotLocation._id` (+ plot type); coop plots also carry `hen_collector`
    (contents) and `hen_collector_keep` (slider limit) as before. Temp file + rename. Then delete this game's
    `*.sav.json` whose `.sav` no longer exists in `savePath` (only if the file just written is
    there).
  * The `Load` detour notes `identifier.SaveName` (+ the save number right after the original).
    The sidecar is read lazily, the first time a custom upgrade is asked about, in this order:
    1. `<SaveName>.sav.json` if its `save` matches and its `save_number` equals the loaded file's
       (a different number = a sidecar of an older file of the same name, replaced while the mod
       was off);
    2. the old per-game file `<GameName>.json` (format 1, migration; renamed to
       `.json.migrated` after the first per-save write, so it is used only until then);
    3. this game's sidecar with the highest `save_number` below the loaded file's;
    4. nothing. The log line says which one was used.
  * Re-enabling the module (its hooks, `Load` included, were off meanwhile, so another save of
    the same game may be loaded now) drops the in-memory state at the next main-thread question
    and re-reads it for the loaded save: `_currentGameMetadata` gives GameName + SaveNumber; this
    game's sidecar with that `save_number` (whose `.sav` still exists; several -> newest `.sav`)
    stands in for `SaveName`, then the order above. No metadata (or SaveNumber 0) -> empty state
    for that game, logged; without the metadata fields (legacy mode) the per-game file, as on
    load. Nothing is written by this re-read; the 1 Hz tick rebuilds the world objects.
  * Once a game has had a sidecar, every save writes one (also an empty one), so loading that
    save never falls back to an older state. A game that never had a custom upgrade gets no files.
  * Without the StoreGameData hook (other storage provider, name not found) it falls back to the
    old per-game file.
  * Generic per-save values (`custom_upgrades::PlotValue` / `SetPlotValue` / `SaveValue` /
    `SetSaveValue`, api.h) live in the same file. sell_upgrades stores the ash of a sold Ash
    Trough there.
* Why the game folder and not the Steam save folder: Steam Cloud syncs
  `LocalLow\MonomiPark\SlimeRancher2\Steam\<id>\` (steam_autocloud.vdf). Extra files there would
  be uploaded to the game's cloud quota and land on PCs without the mod, and unknown files in the
  game's own save folder are a risk we don't need (the game enumerates that folder). The cost:
  custom-upgrade state does not follow a save to another PC through the cloud (the sidecars can
  be copied by hand; the names match the .sav files).
* Game name: `AutoSaveDirector.CurrentSaveGameName()` (0x5CE220, returns
  `_currentGameMetadata.GameName` or ""). `AutoSaveDirector` comes from
  `GameContext.Instance.<AutoSaveDirector>k__BackingField`.
* Reset: detours on `AutoSaveDirector.Load(GameSaveIdentifier, bool, Stream)` (0x5CFDC0; the
  struct comes in by reference on x64) and `StartNewGame(int, GameSettingsModel)` (0x5D1A70) drop
  the in-memory state. Purchases you haven't saved disappear together with the unsaved Newbucks,
  just like vanilla.
* Save safety: the vanilla save never holds anything of ours. Without the mod the save loads
  unchanged and only the custom upgrades are gone. Deleting the sidecar does the same.
* Known limits:
  * A save written while the mod is off has no sidecar: loading it uses the newest older one.
  * Steam Cloud does not carry the sidecars to another PC.
  * A plot that is demolished and rebuilt as the **same** type at the same spot keeps its custom
    upgrade. When the type changes, the entry is dropped. Fix later with a hook on the demolish
    closure (`LandPlotUIRoot.<>c__DisplayClass8_0.<PromptForDemolish>b__0`).
  * A deleted game leaves its sidecar behind (harmless). A later hook on
    `AutoSaveDirector.DeleteGame(string)` could delete it.

### 1.2b Gate: off by default, locked on per save

* `custom_upgrades.enabled` defaults to **off**: no custom item in any purchase menu (and no tier
  effect, world object or per-save value) until the switch is on. Overlay: Ranch > General.
* Turning it on with a save loaded marks that save: per-save value `custom_upgrades.on` = "1" in
  its sidecar (written with the next game save, like a purchase). While a marked save, or one whose
  sidecar has any owned custom upgrade, is loaded, the switch is pinned on with `settings::Lock`
  (the overlay shows it locked, tooltip "Locked on: this save uses custom upgrades. ..."). On the
  title screen nothing is locked.
* With the switch off the module is still set up (`Module::SetupWhileDisabled`, hooks disabled), so
  its frame callback (`GateTick`, 2 Hz) can watch the loaded save: in game = `SceneContext.GameModel`
  set (the metadata keeps the last game on the title screen) and a game name; save =
  `_currentGameMetadata` (GameName, SaveNumber), checked once per new save with a read-only sidecar
  lookup in `LoadSidecar`'s order (exact per-save sidecar -> per-game sidecar -> newest older one).
  Such a save turns the module on (posted `settings::Set`, the normal enable path re-reads the
  loaded save); leaving it again (title screen, or a save without custom upgrades) turns it back
  off, so the global switch stays what the player chose.
* A session that is never saved leaves no mark: quit without saving and the save is not locked.
* sell_upgrades keeps the ash of a sold Ash Trough in the sidecar only while custom upgrades are on;
  otherwise only in memory (lost on reload).

### 1.3 Menu injection (`LandPlotUIRoot`, the plot purchase menu)

* `LandPlotUIRoot : ListUIRoot<PlotPurchaseItemModel, PlotPurchaseCategory, ...>` has
  `menuConfig` (a `MenuConfiguration<,>` ScriptableObject on the plot's UI prefab). Its
  `categories` hold `StaticCategory.items` (`List<PlotPurchaseItemModel>`).
* `ListUIRoot.CreateUI` first calls the virtual `UpdateMenuItemAvailabilityAndHidden`. The
  LandPlotUIRoot override is at 0x6E8150 and calls `UpdateAvailability(Activator)` /
  `UpdateHidden(Activator)` on every item. Only then does CreateUI build the list views.
  `SetActivator` and `Upgrade()` both end in `RebuildUI`, but `ListUIRoot.RebuildUI` (0x11AFB30,
  shared generic body) only calls `CreateUI` while the menu has no swapper yet (first open). With
  the menu open it calls `IListViewSwapper.RefreshAndAdvance` → `ListUI.itemAdapter` refresh,
  which re-runs the fetch closure `ListUI.<ShowMenu>b__6` (it maps
  `menuConfiguration.Categories[i].Items` through the mapper, live). So after a purchase or sale
  the item lists are re-read, but `UpdateMenuItemAvailabilityAndHidden` does NOT run: vanilla items
  don't need it (their availability is a live `Func`), ours did (a new tier only showed after
  reopening, checklist u10). custom_upgrades therefore also hooks `RebuildUI` (pre: list update,
  post: texts; other menus: one class check).
* **Hook (pre):** for each menu configuration that has an upgrade item, our items are cloned once
  per session with `Object.Instantiate(<vanilla PlotUpgradePurchaseItemModel>)`. Unity
  serialization deep-copies the LocalizedStrings and the cost list. The clone is checked to have
  its own instances; otherwise that menu gets no custom items. The template is the silo's
  STORAGE_CAPACITY_INCREASE item if it exists, otherwise any item with one cost entry. On the
  clone we set:
  * `_upgrade` = our id
  * the first cost entry's `_amount` = the setting price (other entries = 0)
  * `_icon` = the Market Link icon (`LookupDirector.MarketLink.icon`)
  * name `sr2mods_<key>` and hideFlags `DontUnloadUnusedAsset`; a GC handle that is never freed
    holds it
  
  Items are added to or removed from the category list depending on the activator plot: its type,
  plus a prerequisite. The list is shared by all plots of a type, so each update starts from the
  vanilla list (our items out, vanilla items we took out back at their index) and then places
  this plot's cards: one card per tier chain (`Shown`); a tier on a vanilla upgrade (Music Box II,
  Ash Booster) replaces that vanilla item at its index once it is owned (the vanilla item is
  pinned by a GC handle while out of the list and put back for other plots and on cleanup);
  `after` places an item right behind a vanilla one (Fast Growth after the Sprinkler);
  everything else is appended. `List.Insert` / `IndexOf` / `Remove` via runtime_invoke.
* **Hook (post):** refreshes dynamic texts (the Hen Collector's limit).
* Availability: vanilla's `IsAvailable = () => !plot.HasUpgrade(_upgrade)` goes through our
  HasUpgrade detour, so an owned custom upgrade is greyed out like a vanilla one.
  `PlotUpgradePurchaseItemModel.UpdateHidden` only gives STORAGE3/4 and the incinerator collector
  a prerequisite. Every other value, ours included, is never hidden.
* Purchase: the whole vanilla flow. `OnAction` → `PromptForUpgrade` (error if already owned) →
  game popup with the item's `_title` / `_promptMessage` → `<PromptForUpgrade>b__0` →
  `Upgrade(upgrade, cost)`: HasUpgrade, `Spend(cost)` (which fails if you can't afford it), then
  **AddUpgrade (our detour)**, purchase cue, `RebuildUI`, `PlayPurchaseFX`. Side effect: the
  game's analytics event `PlotUpgraded` gets `upgrade.ToString()`, which is a number for our ids.
* Texts (localisation): `core/text_override.h` maps LocalizedString **instances** to texts. The
  UI's existing `LocalizeStringEvent.UpdateString` hook (`ui/options_tab.cpp`) now also checks the
  event's `m_StringReference` against that map. This covers the list rows, the details panel and
  the confirm popup (PopupPrompt shows texts through LocalizeStringEvents). If that hook is not
  active (`text_override::SinkActive()`), the custom items are **not shown**, because they would
  display the template's text.
* Fault and disable policy: when the module is disabled, the items are removed from the menus.
  On a fault, a per-frame flag check does the same within one frame. Their price is also locked to
  int.MaxValue, so a menu that is already open can't buy them through vanilla code once our hooks
  are gone.
* sell_upgrades integration: owned custom items are sold with sell_upgrades' Sell button (no
  popup). It calls `custom_upgrades::CanSell` / `Remove` (api.h) instead of the HashSet/upgrader
  path, refunds half of the item's cost (fixed, rounded down), then calls `RebuildUI`. On a tier
  card that shows the next tier, `custom_upgrades::ChainSellTarget` names the owned tier below it
  (custom, or the vanilla upgrade whose card the tier took over) and the item that prices it.

### 1.4 World objects (implemented for the coop upgrades in `coop_world.cpp`, with a ~1 Hz tick instead of the planned ApplyUpgrades hook)

* **Prefabs we can reach at runtime, no scan needed:**
  * `LookupDirector.GetIfReady()` (static), or `GameContext.Instance.LookupDirector`.
  * `GetPlotPrefab(LandPlot.Id)`: plot prefabs for CORRAL / COOP / GARDEN / SILO / POND /
    INCINERATOR, **with all upgrade parts as inactive children**: air nets, solar shields,
    collector with button, feeder, music box, deluxe coop parts, silo slot buttons.
  * `get_GadgetDefinitions()` (every gadget prefab: Market Link, warp depots, toys...),
    `get_MarketLink()`, `get_RefineryLink()`, `_toyDefinitions`.
* **Cloning:** `Object.Instantiate(childGameObjectOfPrefab, plotTransform)` copies just that
  sub-hierarchy, then set the local position, rotation and scale. Components that reference
  plot-level objects of the source prefab need care. Example: `PlortCollectorActivator.Collector`
  points into the prefab, so on our clone we intercept `Activate()` first (see 1.5). Clones are
  never part of the save: they are rebuilt from the sidecar when a plot's `LandPlotModel` is
  pushed or loaded. Plan: hook `LandPlot.ApplyUpgrades`, which runs after load and after every
  change, and (re)attach our objects for the plot's custom upgrades. When an upgrade is sold, the
  clone is destroyed (`Destroyer.Destroy(go, reason)`, the call sell_upgrades already uses).
* **Custom meshes (Blender) later:** build a GameObject with MeshFilter and MeshRenderer, a
  `new Mesh()` with `set_vertices` / `set_triangles` (or `SetIndices`) / `set_uv` /
  `set_normals` / `RecalculateBounds`, and the `sharedMaterial` of a game renderer we clone (for
  example the silo's or coop's material, so it uses the game's HDRP Lit shader). Load the mesh
  data from a simple binary or OBJ file next to the DLL: plain C++ parsing, no managed code.
  Colliders: `MeshCollider.sharedMesh` or primitive BoxColliders.

### 1.5 Interaction + custom UI (design)

* **World button, reused as is:** `ITechActivator` (`Activate()`, `GetCustomGuiPrefab()`) is the
  game's "press E on a button" interface. `TechUIInteractable` shows the prompt and calls it.
  Ready-made button components: `PlortCollectorActivator` (the corral collector button, with
  Animator trigger + `PressButtonCue`), `SiloStorageActivator` (silo slot buttons),
  `ResourceCollectorActivator`, `FeederSpeedSelector` (a 3-state button: slow/normal/fast),
  `ObjectToggleActivator`. Plan: clone the collector button (from the CORRAL prefab) onto our
  plot, and detour `PlortCollectorActivator.Activate`: for our clones (address set), play the
  button animation and cue, run our action, and skip the original.
* **Opening a menu from a button:** `UIActivator` (`uiPrefab`) / `LandPlotUIActivator` are
  `BaseUIInteractable`s whose `OnInteract` pushes a UI prefab through
  `UIDirector.CreateAndPush`. That is how the plot menu opens. We can open the plot purchase menu
  itself, or our own panel (below).
* **Slider:** the game UI has no settings slider (options use steppers). Two options:
  * uGUI `UnityEngine.UI.Slider` built at runtime. `modules/minimap/ugui.cpp` already builds
    runtime uGUI; style it with `SliderStyler` / `SliderStyle`.
  * Clone the map's zoom slider (`MapZoomSlider`, on the map UI prefab).
  
  The panel opens from our cloned button as a BaseUI pushed onto the UIDirector stack, so input
  and cursor are handled like the game's own menus. Fallback: the existing ImGui overlay, as a
  small window.
* **Cheapest "slider" for v1:** a `FeederSpeedSelector`-style multi-state button (limit
  2/4/6/8/...), or stepper items inside the plot purchase menu: two action items, "Limit +2" and
  "Limit −2", with the value in the title. No new UI at all.

---------------------------------------------------------------------------------------------
## 2. The ideas: feasibility + plan

### 2.1 Coop "Hen Collector" - IMPLEMENTED (`modules/custom_upgrades/coop_world.cpp`)

User decision: works like the deluxe coop's elder collector, for adult hens and roosters, same
hardcoded limit, chicks don't count, no slider / custom UI. Findings (asset bundles + disassembly):
* The elder collector is ONE `ElderCollector` object in the COOP prefab's `Deluxe` child
  (`DeluxeCoopUpgrader.DeluxeStuff`, inactive): `SiloStorage` (AmmoSetReference
  `CorralPlortCollectorAmmo`, 2 slots, both `AmmoSlotDefinition "ElderCollector"`: `_slotTypeGroup`
  ElderGroup {ElderHen, ElderRooster}, `_maxAmount` 100), `PlortCollector` (CollectionArea = whole
  coop, CollectPeriod 6 game hours), 2 `SiloCatcher` SILO_OUTPUT_ONLY ports (slot 0 / 1), 2
  `SiloSlotUI`, a `PlortCollectorActivator` button. The "2 collectors" are its 2 slots / ports.
* `PlortCollector.DoCollection` (0x98B670) has no count limit: every actor in the area that the
  storage `CouldAddToAnySlot` is sprung in and stored. The only hardcoded limit is the storage:
  **2 slots x 100**. `AmmoSlotDefinition.IsAllowed` (0x866B40) = !blockList.Contains(id) &&
  (group == null || group.IsMember(id)).
* Save: `SiloStorage.Awake` -> `InitAmmo` (0x9956C0) builds a local `AmmoSlotManager` from the
  slot definitions. Only `SetModel` (0x995920) links it to `LandPlotModel.siloAmmo[guid]`, and
  plot children only get `InitModel` / `SetModel` from `LandPlotModel.Init` /
  `NotifyParticipants`, which only `InstantiatePlot` calls (a new plot object).
* Groups: MeatGroup = {Hen, Rooster, BriarHen, StonyHen, SeaHen, ThunderHen, PaintedHen,
  CandiedHen, ElderHen, ElderRooster, UnstableMeat}; ChickGroup = the 7 chicks; ElderGroup.
* Reproduce (hens): MaxDensityInsideCoops 58 within 10 m (DensityIds include chicks and elders),
  4..13 game hours per chick; chicks grow in 14 game hours (9 hen : 1 rooster).

As built:
* Save safety: storing hens in the vanilla elder storage is NOT valid (mod-free load: the slots
  are ElderGroup slots again, hens dropped by ValidateAndAdjustSlots). So the Hen Collector is a
  clone of `Deluxe` under the coop plot (TV and button removed, placed in the free (+x,-z) corner;
  the `Deluxe` container is itself turned -90 deg in the prefab, which puts the vanilla elder
  collector in (+x,+z): see `modules/custom_upgrades/NOTES.md`) whose SiloStorage gets ONE session clone of the elder slot definition (same
  `_maxAmount` 100, group MeatGroup, block list ElderGroup + UnstableMeat) in both slots before
  activation. Its storage stays local (never linked; a tick check logs an error if it ever were),
  but it needs an `AmmoModel` of its own: `SiloStorage.InitAmmo` only builds the AmmoSlotManager,
  whose slots live in its AmmoModel (+0x18), set only via `SiloStorage.SetModel(LandPlotModel)` ->
  `LandPlotModel.GetAmmoSet` (vanilla: `new AmmoModel(null)`) -> `AmmoSlotManager.SetModel` (Resets
  it to the definitions). Without it every slot call NREs (first in-game test: `StorageSlotUI.Start`
  / `Update` every frame, `AmmoSlotManager.get_Slots` in our tick). So CreateHen calls `InitAmmo`,
  then `SetModel(new AmmoModel(null))` while the clone is still inactive. A one-shot log line
  ("hen collector placement") gives the clone's world position, renderer count and visibility at
  build and ~5 s later.
  contents persisted in the sidecar (`hen_collector`), output ports and slot displays work as
  vanilla. Selling is refused while it holds chickens.
* Collecting: the clone's `PlortCollector` runs with a private LandPlotModel whose
  collectorNextTime never comes (its own DoCollection would take everything). Our ~1 Hz tick counts
  adult hens and roosters (MeatGroup - ElderGroup - UnstableMeat) in the active CoopRegion box
  (`Physics.OverlapBox`, Identifiable.identType, not held by a vac); chicks are never counted or
  taken. While more hens or roosters than the coop's two limits are in the coop and the
  collector's period (its CollectPeriod, 6 game hours) is due, each excess chicken gets the joint
  DoCollection would give it, and the collector's vanilla FixedUpdate vacuums it in (reel, store,
  puff, cyclone, vac sound), until the slots are full. (Build 4d16e38c stored them directly and
  they just vanished.)
* Why 20: a collector that takes every adult (the elder collector's literal behaviour) empties the
  coop and stops breeding. 20 adults (~18 hens) produce ~2 chicks per game hour; with 14 h to grow
  that is ~30 chicks, ~50 birds in total, under the 58 density cap where breeding stalls.
  20 is now only the default (18 hens + 2 roosters): see the limit sliders below.
* Price 1000 (Deluxe Coop 1300, Plort Collector 750), icon = the Deluxe Coop item's.

Limit sliders (user request, follow-up): the keep-limits are per coop, hens **0..58** (58 =
`MaxDensityInsideCoops`, default 18) and roosters **0..20** (default 2), set with two sliders side
by side in the coop's purchase menu while the owned Hen Collector is selected (first a single
2..58 adults slider; split after the test of 4d16e38c).
* Where: `PurchaseItemDetailsContentView.BindData(vm)` (0x6D9EA0, non-generic, unique body) binds
  the details panel of every list menu: `_detailsText` (+0x58) <- `vm._description`,
  `_craftCostView` (+0x60) active only when `vm._showCost` (= IsAvailable && NewbuckCost > 0, so
  hidden for owned items). custom_upgrades detours it (post): the item is ours when
  `vm._description` is our clone's `_description` LocalizedString instance, the plot comes from the
  last `UpdateMenuItemAvailabilityAndHidden` root. The slider takes the hidden cost row's slot.
* Slider prefab: none reachable. The plot UI has no slider, the options ranges are preset
  steppers, the map zoom bar is `MapZoomSlider` (custom drag handler bound to the map view model).
  The game styles uGUI sliders with `SliderStyler` (`_style : SliderStyle` asset; OnEnable 0x6CB430
  -> ApplyStyle 0x6CAC90: `transform.Find("Background")` Image, `slider.fillRect` / `handleRect`
  Images, `Selectable.colors`). So `modules/custom_upgrades/plot_menu_slider.cpp` builds the standard
  uGUI slider hierarchy (Background, Fill Area/Fill, Handle Slide Area/Handle) at runtime, adds a
  `SliderStyler` with the first loaded `SliderStyle` asset (one-time FindObjectsOfTypeAll; plain
  colours if none), and a TMP label with the description text's font. Reusable: `SliderSpec`
  {name, min, max, step, value, label(v), onChange(v)}, `Create(contentView, spec)` / `Destroy()` /
  `Tick(dt)`.
* Input: mouse through Unity's Slider; gamepad d-pad / left stick and keyboard arrows step it (with
  repeat), checked to be interactable and without an active `PopupPrompt` (one
  `FindAnyObjectByType` per new press). A mouse drag's EventSystem selection is handed back to the
  previously selected object on release.
* First in-game test: shown but not changeable. (1) `PurchaseItemDetailsContent` carries its own
  nested `Canvas` and no `GraphicRaycaster` (prefab data), and uGUI raycasts a graphic only through
  a raycaster on its nearest canvas: nothing below the panel is ever hit. The slider root now gets
  a `Canvas` (overrideSorting, order = panel canvas + 1, same sorting layer) + `GraphicRaycaster`,
  plus a full-size alpha-1/255 hit Image on the bar. (2) The plot menu's items are a
  `UniformGridView` (GridArea / GenericGridItemViewHolder), whose
  `INavigationHandler.OnNavigate` (0x7DB210) moves the selection on left / right: a pad press also
  selected the neighbouring item and the rebind removed the slider. custom_upgrades detours it: while
  the slider is shown, Left / Right (`NavigationEventData.MoveDirection` 0 / 2) go to
  `PlotMenuSlider::Step` and the event is `Use(true)`d; everything else passes through. (3) Look:
  the game's UI shapes are `SDFRoundedRectangle` (MonomiPark.SlimeRancher.UI.SDFUI; material from
  `Shader.Find("SDFUI/" + type)`), e.g. the scroll bars' pink handle (0.871, 0.310, 0.522). The slider
  uses them (track = panel text colour at 22 %, pink fill, white handle with a pink border).
* Save: sidecar per plot `"hen_collector_keep_hens"` / `"hen_collector_keep_roosters"` (written
  while the Hen Collector is owned, saved with the next game save like ownership; an old
  `"hen_collector_keep": n` reads as min(2, n) roosters + the rest hens), read live through
  `CoopRequest.keepHens` / `keepRoosters`. The menu description shows the coop's current limits.

Original plan:

* Reproduce (on hens): `ShouldReproduce` → `NearMateAndSparseEnough`: it counts `DensityIds`
  within `DensityDist` against `MaxDensity()` (`MaxDensityInsideCoops`, ×`DeluxeDensityFactor` in
  a deluxe coop; `MaxDensityOutsideCoops` otherwise). `CoopRegion.IsWithin(pos)` uses the static
  `_allCoops` list. When the density is reached, the coop stalls.
* The deluxe coop already has the elder-hen collectors. That is the "2 collectors like elder hens"
  (drone program `RanchDroneSourceProgramElderCollector` reads them as silo storage). They are a
  `PlortCollector` (`CollectionArea` TrackCollisions, vac FX `CollectFX`, `CollectAnim`, vac
  audio) feeding a `SiloStorage` on the plot, so the stored hens are **saved in
  `LandPlotModel.siloAmmo`** (vanilla save).
* Plan:
  1. Upgrade item on COOP plots, price TBD. Visual: clone the deluxe coop's collector pair (from
     `GetPlotPrefab(COOP)` → DeluxeStuff) **without its SiloStorage** (we must not add new ammo
     sets to the vanilla save), or use the collector GameObjects only for their look and FX.
  2. Logic: a low-rate tick (every ~2 s of game time, main thread). Count hens and roosters
     inside this coop: `CoopRegion` bounds, or the plot's `TrackContainedIdentifiables` (the
     `_trackers` of the LandPlot). Above the limit, vacuum the excess using the collector's own
     vac FX and sound. The chicken is destroyed with `Destroyer.DestroyActor`.
  3. Where the chickens go (pick one):
     * (a) Counted into a sidecar "hen bank" per plot. An output spout on the coop (cloned
       `SiloCatcher` with `SILO_OUTPUT_ONLY`) or a panel button ejects them again.
       Recommended: nothing is lost and nothing touches the vanilla save.
     * (b) Converted to meat and pushed into a nearby silo.
     * (c) Released outside the coop.
  4. Limit: per plot, stored in the sidecar. UI is the stepper menu items for v1, then the slider
     panel from 1.5.
  * Risk: vacuuming "real" actors safely needs the game's own path. Collector `DoCollection`
    springs Vacuumables to `CollectPt` and then calls `storage.MaybeAddToAnySlot`, so we need our
    own sink. Alternative: no physics, a direct destroy plus a puff FX (simpler and robust).

### 2.2 Silo "Sell All" - IMPLEMENTED (menu action + world button)

* Economy: `SceneContext.PlortEconomyDirector.TrySell(id, count, ignoreMarketShutdown,
  showUiNotification, source)` (0x9E52A0):
  * returns false if the market is shut down (`IsMarketShutdown`: time-of-day based) or the type
    has no market value
  * otherwise `PlayerState.AddCurrency(null = Newbucks, value×count, showUi)` and
    `RegisterSold(id, count, total)`, which raises saturation (`worldModel` dictionary
    `+= count`), the OnSold event, MetaGame stats and session counts
  
  These are the same calls the Plort Market makes. The price is the rounded current value, and
  saturation lowers it at the director's next price update. That matches shooting a full vac pack
  into the market. Ranch drones go through `ScorePlort.Deposit` → TrySell as well.
* Silo contents: `SiloStorage` (children of the silo plot) `.LocalAmmo` → `AmmoSlotManager`:
  `Slots[]`, `GetSlotName(i)`, `GetSlotCount(i)`, `Decrement(i, n)` (0xA39790; it clears the slot
  at 0 and fires the slot's change delegate, which updates the silo display). Only
  `IdentifiableType.IsPlort` types with a market value are sold. Order per slot: TrySell first,
  then Decrement, so you never lose plorts without being paid.
* Implemented flow:
  1. Buy "Sell-All Button" in the silo menu (fixed price 10,000 Newbucks in `kDefs`; the old
     `sell_all_price` setting was removed, checklist si2). It is
     only offered when the world button below can be built.
  2. The button on the silo sells (HUD notices per plort type, log line with every type × count
     @ price). If the market is closed, it says so and nothing is sold. The former "Sell All
     Plorts Now" menu action (an action item, id 0x53520002) was retired (checklist u12).
  3. The upgrade can be sold back with sell_upgrades.
* World button (`modules/custom_upgrades/silo_world.cpp`, built, not yet tested in game): while
  the upgrade is owned, the corral Plort Collector's press button (`PlortCollectorUpgrader.Collector`
  -> child "techActivator": Animator, stand, button mesh, "triggerActivate" with
  `PlortCollectorActivator` + `TechUIInteractable`) is cloned under the silo plot as
  `sr2mods_SiloSellButton#<generation>`, at the outer (+x, -z) corner (5.58, 1.20, -5.58), turned
  135 deg to face outwards (front side, opposite corner from the plot menu terminal at
  (-5.54, -5.54); the slot panels sit mid-side at 3.05 m). The clone's `Collector` is set to null.
  A detour on `PlortCollectorActivator.Activate` never calls the original for an activator without
  a Collector; for ours it plays the button animation (`_buttonAnimator.SetTrigger`) and
  `PressButtonCue` (0.4 s rate limit like vanilla), checks ownership and runs the same sale as the
  menu action. Feedback: the game's currency HUD notification (TrySell showUi) plus a short
  world-space text above the button through `ui::hud` (new `Frame.textOnly`): "The Plort Market
  is closed - nothing sold", "No plorts in this silo". A successful sale (button or menu) instead
  raises one HUD notification per plort type, the quantum drones' "plorts sold" ones
  (`modules/custom_upgrades/sale_notify.cpp`): `DroneActivitySceneNotifications.AddPlortSold(type,
  count)` on the HUD drone provider's `_activeNotifications`, then
  `ItemAcquisitionNotificationList.HandleNotificationAvailable(provider)` (icon, PlortSoldFormat
  name, count). Only while `get_CanQueueAny` is true (the list discards notices while the HUD is
  suppressed by an open menu), so menu sales show after the menu closes. Without the HUD list the
  button falls back to the text "Sold N plorts for V Newbucks".
  * Same look as the drones' cloud-upload notice (checklist si2, decompiled 2026-10-23):
    `DroneActivitySceneNotifications.DequeueResourceCollected` (0x75B430) and `DequeuePlortsSold`
    (0x75B200) are the same code: a `NotificationEntry(provider, group null, type.icon,
    Format.WithVariable("0", type.localizedName (+0x28)), count, config.DefaultStyle)`; only the
    format differs (`QuantumCloudResourceCollectedFormat` vs `PlortSoldFormat`, falling back to the
    bare localized name if the format is empty). The provider's `DequeueNext` serves low battery,
    then resources collected, then plorts sold. So the sale keeps `AddPlortSold` (the text says
    sold).
  * Test n7 (20:04 build) showed no notices: the world button found no list with the drone
    provider and used its text. Since then every loaded list is scanned (FindObjectsOfTypeAll, at
    most 1 Hz, only on a press or while notices are queued); queued notices may go to a started
    but inactive list, the world-button check needs an active one. The log says once which list
    was used, or why none fits (the first list's provider classes): check it on the next test.
  * Not used (unverified, risky): building a `NotificationEntry` ourselves and adding it to the
    list's `_pendingNotifications` with another or no provider. The list calls the entry's provider
    (`TryDequeueMergeNext`, paging), and the shop provider expects its own entry type.
  Silos come
  from the sidecar ids -> `SceneContext.GameModel.GetLandPlotModel(id).gameObj` (the
  LandPlotLocation) -> `GetComponentInChildren<LandPlot>` at ~1 Hz; sold / disabled / faulted
  removes the button, load / plot rebuild rebuilds it. The menu action stays.

### 2.3 Quantum drones: "sell only at or above a limit price" - IMPLEMENTED (`modules/drone_price_limit/`)

Built, not yet tested in game. Full findings are in the header comment of
`modules/drone_price_limit/module.cpp`. Summary:

* "Automation bees" = the **ranch / quantum drones**: `MonomiPark.SlimeRancher.Drone.RanchDrone`,
  placed at a `DroneStation` gadget and programmed with source and sink programs. The market sink
  is `RanchDroneSinkProgramPlortMarket : RanchDroneSinkProgramBase<ScorePlort>`.
* `ScorePlort.CanDroneDeposit(id, ignoreMarketShutdown)` (0x991E40) is
  `GetMarketValue(id).HasValue` (plus active / not charging). Its only callers are the `GetSinks`
  iterators of the market and caretaker-shop sink programs (always `ignoreMarketShutdown = true`).
  `HasAvailableSpace`, `GetAvailableSpace`, `Relevancy`, the per-tick `OnAction` re-check and the
  source programs' gather checks all go through `GetSinks`.
* Fast-forward (sleeping / away): `FastForward` (0xA2C2C0) does `Prioritize(GetSinks).First()`
  and `Deposit`, but its only caller `DroneFastForwarder.FastForward_Deposit` first filters the sink
  programs with `HasAvailableSpace(group.id)` and returns "nothing deposited" when none is left. So
  the `CanDroneDeposit` filter covers it, `First()` never sees an empty sequence, and the source is
  only decremented by what was really deposited.
* Trap found: when a drone holds items and no sink program is relevant,
  `DroneBehaviorPlexer.PickNextBehavior` picks `RanchDroneBehaviourDumpAmmo`, which **destroys**
  the held items (`PopFromSelectedSlot` + FX). A drone that is carrying plorts when the price
  drops below the limit (daily price update, limit raised, loaded save) would lose them.
* Implemented:
  * Detour `CanDroneDeposit`: after a yes from the original, for a real market (not
    `_isCaretakerShop`, not the sprinkle type) return false while
    `GetCurrValue(id) < max(min_price, ceil(fraction x base value))`. Base value =
    `PlortEconomySettings.PlortsTable` `InitialValue` (= `CurrValueEntry.BaseValue`).
  * Detour `RanchDroneBehaviourDumpAmmo.Relevancy`: false while the drone holds a plort held back
    by the limit and has a Plort Market sink program. The drone then rests at its station with the
    plorts, re-thinks on the Rest timer and deposits once the price is back.
  * Settings: `min_price` (Newbucks, 0 = off) and `min_price_fraction_of_base` (0 = off); the
    higher limit counts. One log line per change of state per plort type.
* Later: per-plort-type / per-station limits (sidecar keyed by the station's gadget id, UI from the
  drone station menu), "fall back to silo" when a silo sink program is also set (vanilla already
  tries every relevant sink program in order, so a second silo program takes held-back plorts).

### 2.4 Coop roof - IMPLEMENTED as the "Coop Air Net" (`modules/custom_upgrades/coop_world.cpp`)

User decision: an air net, cloned from the corral. As built:
* Prefab data (read from the asset bundles, `duplicateassetisolation_assets_*.bundle`): the
  CORRAL prefab `patchCorral` has `AirNetUpgrader.AirNets = [Base Air Net, High Air Net]`. Each
  is one `NetTrigger` (MeshRenderer + `AirNet` + BoxCollider 9.5 x 0.6 x 9.5, layer 29 = the
  barrier layer of corral and coop walls). Base net bottom at y 3.39 (corral base barrier top
  3.45). `AirNet`: HitForceToDestroy 100, HoursToStartRecovery 0.05, HoursToRecover 0.1.
* COOP prefab `patchCoop`: barriers on layer 29, top y 2.0 (High Walls 2.8); `Coop Region Trigger`
  (CoopRegion + TrackContainedIdentifiables) 9.5 x 2 x 9.5 at y 1 (High Walls 9.5 x 3.5 x 9.5).
  Same 9.5 footprint as the corral.
* Build: `Instantiate(Base Air Net, coopPlot.transform)`, renamed `sr2mods_CoopAirNet#<gen>`,
  activated. Fit (every ~1 s, moved only when off by > 2 cm): net collider bottom = top of the
  coop's active CoopRegion collider, X/Z scale + centre = the region's. Buying High Walls later
  lifts the net at the next tick.
* Function: whatever the vanilla net does, unchanged (the collider blocks what collides with
  layer 29: chickens launched by slimes or the vac stay in, flying things stay out; it breaks
  under hard hits and recovers). `TrackContainedIdentifiables.AirNets` of the coop is left empty
  (in the corral it only feeds a slime "integrity" check).
* Price 425 (= corral Air Net), icon = the vanilla Air Net item's icon (found in the corral menu
  asset by a one-time scan of the loaded `PlotUpgradePurchaseItemModel`s).

Original research:

* Existing roofs: the **corral Solar Shield** (`SolarShieldUpgrader.Shields`, a GameObject[]
  canopy) and the **Air Net** (`AirNetUpgrader.AirNets`). Both are inactive children of the CORRAL
  plot prefab, reachable via `LookupDirector.GetPlotPrefab(CORRAL)`. The user said the roof is a
  "slime port" upgrade. No class with that name exists in 1.3.0 (searched for SlimePort / Port /
  Roof / Canopy / Cover upgraders). The only roof-like upgraders are SolarShield and AirNet, so
  "slime port roof" most likely means the Solar Shield. **To confirm with the user.**
* Fit: all plot prefabs share the same plot footprint (LandPlotLocation grid). Clone the
  Shields objects under the coop plot with the same local transform. Then compare
  `Renderer.bounds` of the coop's tallest part (coop house plus the deluxe coop extras) with the
  shield's lower bound, and raise the shield on local Y if needed. Optionally scale X/Z to the
  coop fence bounds.
* What it does: suggested, (1) Solar-Shield logic: no sunlight inside, which feral / tarr spawning
  rules key on. (2) Keep chickens in (an invisible ceiling collider): hens are often launched out
  by slimes or vacs. (3) Keep tarr / feral slimes out of the coop (an air-net-style collider
  layer). Option (2) is the cheapest and most useful. (1) needs a check of what the solar shield
  actually toggles (`SolarShield` components inside Shields).

---------------------------------------------------------------------------------------------
## 2b. User feedback (IMPLEMENTED, differs from the plan below)

* **2b.1 as built:** not the alt-action route. Disassembly showed the PURCHASED text is the
  category's `detailsInfo` prefab (a label-only twin of the Purchase button, label =
  `item._infoButtonLabel`) that `ListUI.CreateDetailsMoreButton` instantiates into
  `mainActionButtonContainer` for unavailable items; the alt (Info) button comes from
  `category.altDetailsButton` and only exists with a Pedia entry. sell_upgrades now clones the
  category's own `detailsButton` prefab (the game's Purchase button) next to it, labelled
  "Sell +N", from a `DetailsMoreButtonView.BindData` detour (non-generic; the ListUI methods are
  shared generic code without a name-resolvable pointer). `MainActionButtonContainer` has no
  LayoutGroup, so the vanilla view's rect is split in halves (PURCHASED / Purchase left, Sell
  right). On a tier card showing the next tier, Buy and Sell stand side by side. A press sells
  at once (no popup); confirming an owned item does nothing. A passing reason ("holds food")
  shows as a disabled button, a permanent one ("silo storage") as no button. The prefab
  (`TextButton_NoneSelectable`) is not reachable by gamepad navigation, so selling is mouse-only.
  Details in the sell_upgrades header.
* **2b.2 as built:** prices/multipliers as proposed below, ids `0x5352_0013..0040` (Plort
  Collector II / III and Auto-Feeder II, `0x5352_0010..0012`, were retired), one menu card per
  chain (section 1.3), effects in
  plot_upgrades via `custom_upgrades::Multiplier(obj, family)`; music box per MusicBoxRegion
  (`_currentEmotions` membership), coop per CoopRegion collider bounds. The global sliders were
  removed. See `modules/plot_upgrades/NOTES.md`.

### 2b.1 sell_upgrades: a visible SELL button on owned entries

Today an owned (greyed out, "PURCHASED") item only reacts if you confirm on it. Players don't
discover that, and it may not fire at all on some input paths. The purchase menu already has a
second button: the **alt action** button ("Info", which opens the Pedia).
* `ListUIRoot.ShowAltActionButton(item)` (0x11AFE20, shared generic code) shows it when
  `item.PediaEntry != null`.
* `ListUIRoot.OnAltAction(item)` (0x11AF260) opens `PediaDirector.ShowPedia(entry)`.
* The label is the item's `_infoButtonLabel` LocalizedString.
* The "PURCHASED" text is the `CraftCostView` `_label` LSE in the details panel
  (`PurchaseItemDetailsView` → `PurchaseItemDetailsContentView._craftCostView`).

Plan, inside sell_upgrades, filtered with `IsA(self, LandPlotUIRoot)` because the code is shared
with every list UI (fabricator etc.):
1. Detour `ShowAltActionButton`: for an owned, sellable upgrade item, return true.
2. Detour `OnAltAction`: for an owned upgrade item, open the existing sell popup (sell_upgrades'
   `OpenPrompt`) instead of the Pedia. Not owned: vanilla Info / Pedia.
3. Label: while an item is owned, register "Sell (N Newbucks)" for that item's own
   `_infoButtonLabel` instance through `core/text_override.h`. Re-register or remove it in the
   menu-update hook (custom_upgrades' `UpdateMenuItemAvailabilityAndHidden` detour publishes a
   per-item callback), so the label goes back to "Info" when the item isn't owned.
4. Result: a real button in the game's own action bar, with the gamepad prompt, right where the
   player looks at "PURCHASED". It reuses the game's button, so the style matches. Cost: owned
   items lose their Info / Pedia button (the Pedia stays reachable before buying).

Alternative: clone the action-bar button GameObject next to the CraftCostView label, as a real
extra button. That needs runtime uGUI (`modules/minimap/ugui.cpp`) plus focus and navigation
wiring, so more risk. Recommended: the alt-action route first.

To verify in game:
* Does the action bar re-query `ShowAltActionButton` on every selection change?
* Is the button label really bound to `item.InfoButtonLabel`, or to a fixed UITemplates string?
  If fixed, override that LSE per bind with the same text_override mechanism keyed on a
  LocalizedString we own.

### 2b.2 plot_upgrades as purchasable, per-plot tiers

The user wants upgrades they buy, not global switches. Built on this framework:
* **Definitions:** tier families, each tier a custom id with `needs` = the previous tier.
  `needs` is already implemented: a tier only shows once the previous one is owned. Owned tiers
  stay listed greyed. Selling is refused while a higher tier is owned (implemented in `CanSell`).
  Proposed families and default prices (to confirm):
  * Corral: Plort Collector II / III (collector period ÷1.5 / ÷2, needs vanilla PLORT_COLLECTOR):
    2500 / 5000
  * Corral: Auto-Feeder II (feeder amount ×2, needs vanilla FEEDER): 2500
  * Corral: Music Box II (calm ×2, needs vanilla MUSIC_BOX): 2000
  * Coop: Faster Hens I / II (reproduce period ÷1.5 / ÷2): 2000 / 4000
  * Garden: Fast Growth I / II (growth ×1.5 / ×2): 2000 / 4000
  * Incinerator: Ash Booster (ash ×2, needs vanilla ASH_TROUGH): 1500
  
  Vanilla prerequisites go through `LandPlot.HasUpgrade(vanilla)` in `Visible()`, a small
  extension: `needs` can be a vanilla id.
* **Effect lookup (plot_upgrades):** each hook resolves its plot, then asks
  `custom_upgrades::Tier(plot, family)` and uses the multiplier for that tier (setting per tier,
  default above). If nothing is owned, the multiplier is 1 (vanilla). The global switches become
  either "per-tier multipliers" or a debug override.
  * `PlortCollector.DoCollection`: `this._model` (LandPlotModel) → `LandPlot` via
    `GetComponentInParent<LandPlot>()`
  * `SlimeFeeder`: the same lookup
  * `SpawnResource._landPlot`: direct
  * `Incinerate`: its LandPlot parent
  * `Reproduce` (per hen): find the coop plot from the hen's position: per-coop bounds cached
    from `CoopRegion` / the plot transform, refreshed when the plot set changes
* **Hot-path cost:** `Tier()` must not do GetComponent / runtime_invoke per call. custom_upgrades
  keeps a `LandPlot* → {plot id, owned keys}` cache, invalidated by a version counter on every
  buy, sell, load and plot replace. Lookups are O(1) map reads on the main thread; a cache miss
  takes the slow path once.
* **Save safety:** unchanged. Tiers live in the sidecar. Effects are runtime-only (plot_upgrades
  already writes only vanilla-valid values). Without the mod, plots are plain vanilla.

---------------------------------------------------------------------------------------------
## 3. Reusable assets found (class → how to reach it at runtime → what it could serve)

| Piece | Class / component | Reachable via | Use |
|---|---|---|---|
| Plot prefabs (all upgrade parts as inactive children) | `LookupDirector._plotPrefabDict` | `LookupDirector.GetIfReady().GetPlotPrefab(Id)` | source for every clone below |
| Collector button (animated, cue) | `PlortCollectorActivator` (ITechActivator) + `TechUIInteractable` | CORRAL prefab, collector child | world "Sell All" button, Hen Wrangler on/off |
| Silo slot buttons | `SiloStorageActivator` | SILO prefab | button look matching the silo |
| 3-state button | `FeederSpeedSelector` | CORRAL prefab, feeder child | multi-state "slider" (limit presets) |
| Generic toggle button | `ObjectToggleActivator` | world objects | on/off parts |
| Vacuum FX + sound | `PlortCollector.CollectFX`, `CollectAnim`, vac `SECTR_AudioSource` | CORRAL / POND / INCINERATOR / COOP (deluxe) prefabs | Hen Wrangler vacuum |
| Elder-hen collectors | deluxe coop `DeluxeStuff` (PlortCollector + SiloStorage) | COOP prefab | Hen Wrangler visuals |
| Output spout | `SiloCatcher` `Type = SILO_OUTPUT_ONLY` (eject rate 0.25 s, dist 1.2) | SILO prefab, warp depot / storage gadgets | eject stored hens and items |
| Input port | `SiloCatcher` `SILO_INPUT_ONLY`, `ScorePlort` depositor (`_depositorPortAnimator`) | SILO prefab, Market Link gadget | input for custom machines |
| Drone output port visuals | `DroneStationOutputPortVisualController` | drone station gadget | spout look |
| Roof / canopy | `SolarShieldUpgrader.Shields`, `AirNetUpgrader.AirNets` | CORRAL prefab | coop roof |
| Music box | `MusicBoxUpgrader.MusicBox`, `MusicBoxRegion` | CORRAL prefab | decoration / attachment |
| Toys | `ToyDefinition` (`LookupDirector._toyDefinitions`), `PlayWithToys` | LookupDirector | decoration / attachment |
| Gadget prefabs | `GadgetDefinition` (IdentifiableType.prefab) | `LookupDirector.get_GadgetDefinitions()`, `get_MarketLink()` | Market Link body as a "sell" part |
| Popup | `ListUIRoot.OpenPopupPrompt`, `PopupPrompt` | plot menu (sell_upgrades) | confirmations |
| Slider | `UnityEngine.UI.Slider`, `MapZoomSlider`, `SliderStyler` | runtime uGUI / map UI prefab | Hen Wrangler limit panel |

**Own icons (current):** every custom item shows its own sprite from `assets/icons/` (`ui/mod_icons.h`, `ModIconName` in module.cpp), set in `CloneItem` for `_icon`. `_unavailableIcon` is not an item icon: every vanilla plot upgrade item points it at the shared `checkmark` sprite, which `GenericViewHolder.BindData` (0x83C900) enables as an overlay image (`unavailableIcon`, +0x68) over the list icon (tinted with `unavailableColor`) while the item is unavailable (= owned). The clones keep the template's checkmark, so owned custom items show the same tick as vanilla ones. The borrowed sprites below are the fallback when the sprite cannot be created and the size reference (pixelsPerUnit is scaled to their native size).

**Icons (sprites reachable without scanning):**
* `IdentifiableType.icon` on any identifiable: hens, roosters, chicks, plorts, food, gadgets.
  Examples: `LookupDirector.MarketLink.icon` (used for Sell All now), the drone station gadget
  icon, chicken icons from the hen IdentifiableTypes.
* `MenuPurchaseItemModel._icon` of every vanilla plot upgrade (plot menus): plort collector,
  air net, solar shield, music box, deluxe coop, silo storage...
* Upgrade-definition icons in `LookupDirector._upgradeDefinitions` (vac pack upgrades).
* Currency icon: `CurrencyDefinition` in `LookupDirector.CurrencyList`. Good candidate for "sell".

For a name list, a one-time `Resources.FindObjectsOfTypeAll(Sprite)` dump to the log (on a debug
key) would show every loaded sprite name. It isn't done automatically.

---------------------------------------------------------------------------------------------
## 4. What to check in game (Sell-All slice)

1. Log at startup: `[custom_upgrades] setup ok: ... hooks`. When the silo menu opens:
   `plot type 5: 2 custom item(s) cloned from '...'` and
   `using the Market Link icon '...'`.
2. Silo menu: "Sell-All Button" with the Market Link icon and the price 10,000, and
   correct title, description and popup texts (not the storage upgrade's text). Buy it; Newbucks
   are deducted and the item turns greyed (owned).
3. Press the button on the silo: plorts leave the silo, the sale notices pop up, and the log
   shows the sale. Non-plort items stay. Check the market price board for saturation.
4. Save, quit to the menu, load the game: the upgrade is still owned. Check
   `sr2mods_saves\<game>_<i>.sav.json` (one per .sav file, the same name). Load a different save:
   no upgrade there. Load an older autosave of the same game from before the purchase: no upgrade
   (log: "using the sidecar of this save file").
5. Buy, then quit without saving and reload: the upgrade is gone and the money is back.
6. Sell it back (sell_upgrades): refund, and the silo button disappears.
7. Remove the mod (or disable `custom_upgrades`) and load the save: the game loads normally,
   with no custom items.
8. Other plot menus (corral, coop, garden) are unchanged.

---------------------------------------------------------------------------------------------
## 5. Open questions for the user

1. Prices: Sell-All Button (10,000, fixed), Hen Wrangler, Coop Roof, drone price-limit feature
   (free setting, or an upgrade on the drone station?).
2. Sell All: keep it as a menu action, or do you want the world button (cloned corral collector
   button) on the silo? Should the action need the market to be open (vanilla rule, current
   behaviour)?
3. Hen Wrangler: what happens to removed chickens? (a) stored in a "hen bank" with an output
   spout (recommended), (b) turned into meat in a silo, (c) released outside. Separate limits for
   hens and roosters? Count chicks?
4. Coop roof: is the "slime port" roof the corral **Solar Shield**? What should it do: keep
   chickens in (recommended), block sun, keep tarr / ferals out?
5. Quantum drones: one global minimum price, or per plort type / per drone station? If the price
   is too low, should the drone hold the plorts or put them into a silo?
6. Sell button: is it OK to use the menu's alt-action (Info) button as "Sell" on owned items (they lose their Pedia link)?
7. Per-plot tiers: confirm the families, tier counts, multipliers and prices in 2b.2. Should the current global plot_upgrades switches stay as an extra override?
8. Slider: is the in-menu stepper (Limit +/−) fine for v1, or do you want the real slider panel
   right away?
