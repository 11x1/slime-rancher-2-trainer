# Custom land-plot upgrades: research + design (SR2 1.3.0, Steam, Unity 6000.0.73f1)

Status: the framework and the first upgrade (silo **Sell-All Button**) are implemented in
`modules/custom_upgrades/` (built, not yet tested in game). Everything else here is design.
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
    marked owned in the sidecar state, or, for an action item, the action runs.

### 1.2 Persistence: a sidecar file per save game

* File: `<game folder>\sr2mods_saves\<game name>.json`, next to version.dll. It is keyed by
  `LandPlotLocation._id` (the same plot id sell_upgrades logs) and stores the plot type:
  ```json
  { "format": 1, "game": "<name>", "plots": { "<plot id>": { "type": 5, "upgrades": ["silo_sell_all"] } } }
  ```
* Game name: `AutoSaveDirector.CurrentSaveGameName()` (0x5CE220, returns
  `_currentGameMetadata.GameName` or ""). `AutoSaveDirector` comes from
  `GameContext.Instance.<AutoSaveDirector>k__BackingField`.
* Read: lazily, the first time a custom upgrade is asked about in a game (at most one name lookup
  per frame).
* Write: after every `AutoSaveDirector.SaveGameImpl` (0x5D10A0, which manual saves and autosaves
  both go through) that returned a game. The file is written to a temp file and then renamed over
  the old one.
* Reset: detours on `AutoSaveDirector.Load(GameSaveIdentifier, bool, Stream)` (0x5CFDC0; the
  struct comes in by reference on x64) and `StartNewGame(int, GameSettingsModel)` (0x5D1A70) drop
  the in-memory state. Purchases you haven't saved disappear together with the unsaved Newbucks,
  just like vanilla.
* Save safety: the vanilla save never holds anything of ours. Without the mod the save loads
  unchanged and only the custom upgrades are gone. Deleting the sidecar does the same.
* Known limits:
  * One sidecar per game, not per save file. If you load an older autosave or backup of the same
    game, you get the newest custom-upgrade state.
  * A plot that is demolished and rebuilt as the **same** type at the same spot keeps its custom
    upgrade. When the type changes, the entry is dropped. Fix later with a hook on the demolish
    closure (`LandPlotUIRoot.<>c__DisplayClass8_0.<PromptForDemolish>b__0`).
  * A deleted game leaves its sidecar behind (harmless). A later hook on
    `AutoSaveDirector.DeleteGame(string)` could delete it.

### 1.3 Menu injection (`LandPlotUIRoot`, the plot purchase menu)

* `LandPlotUIRoot : ListUIRoot<PlotPurchaseItemModel, PlotPurchaseCategory, ...>` has
  `menuConfig` (a `MenuConfiguration<,>` ScriptableObject on the plot's UI prefab). Its
  `categories` hold `StaticCategory.items` (`List<PlotPurchaseItemModel>`).
* `ListUIRoot.CreateUI` first calls the virtual `UpdateMenuItemAvailabilityAndHidden`. The
  LandPlotUIRoot override is at 0x6E8150 and calls `UpdateAvailability(Activator)` /
  `UpdateHidden(Activator)` on every item. Only then does CreateUI build the list views.
  `SetActivator` and `Upgrade()` both end in `RebuildUI` → `CreateUI`.
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
  plus a prerequisite (the action item only shows while the upgrade is owned).
* **Hook (post):** refreshes dynamic texts, such as the plort count and value shown on "Sell All
  Plorts Now".
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
* sell_upgrades integration: owned custom items go through sell_upgrades' sell popup. It calls
  `custom_upgrades::CanSell` / `Remove` (api.h) instead of the HashSet/upgrader path, refunds
  `refund_fraction` of the item's cost, then calls `RebuildUI`.

### 1.4 World objects (design, not implemented yet)

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

### 2.1 Coop "Hen Wrangler" (two elder-hen-style collectors, slider limit) - FEASIBLE, medium-high effort

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

### 2.2 Silo "Sell All" - IMPLEMENTED (v1 = menu action; world button = next step)

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
  1. Buy "Sell-All Button" in the silo menu (setting `sell_all_price`, default 1000).
  2. The silo menu then shows "Sell All Plorts Now" (price 0). Its description shows the plort
     count and the estimated value. The vanilla confirm popup comes up and the sale runs
     (HUD currency notification from the game, log line with every type × count @ price). If the
     market is closed, it says so and nothing is sold.
  3. The upgrade can be sold back with sell_upgrades.
* Next: world button. Clone the corral `PlortCollectorActivator` button GameObject next to the
  silo's slot buttons (position from the SILO prefab bounds) and detour
  `PlortCollectorActivator.Activate` for our clones → the same sale, plus a short popup or HUD
  text. Needs in-game placement tuning.

### 2.3 Quantum drones: "sell only at or above a limit price" - FEASIBLE, low effort

* "Automation bees" = the **ranch / quantum drones**: `MonomiPark.SlimeRancher.Drone.RanchDrone`
  (QuantumDroneAnimator), placed at a `DroneStation` gadget (`DroneStationGadgetModel`) and
  programmed with source and sink programs. The market sink is
  `RanchDroneSinkProgramPlortMarket : RanchDroneSinkProgramBase<ScorePlort>`.
* Online path: `GetSinks(id, overflow)` (iterator `<GetSinks>d__16.MoveNext`, 0xA356A0) filters
  markets with **`ScorePlort.CanDroneDeposit(id, ignoreMarketShutdown)`** (0x991E40).
  `OnAction_Deposit` then checks `IsMarketShutdown` and calls `ScorePlort.Deposit(...,
  wasDepositedByDrone: true)`.
* Fast-forward path (sleeping / away): `FastForward(id, overflow, endTime, max)` (0xA2C2C0) calls
  `Deposit` directly on the first sink. **Verify** whether it runs only after `HasAvailableSpace`
  / `GetSinks`, so that our filter also applies there and `Enumerable.First` never sees an empty
  sequence.
* Plan:
  * Detour `CanDroneDeposit`: return false if the current value (`TryGetMarketValues`) is below
    the limit. A global limit setting for v1. Per-plort-type and per-station limits later,
    stored in the sidecar, keyed by the station's gadget id, with UI from the drone station menu
    (`DroneStationActivator` → drone UI: `DroneProgramOptionButton`, `DroneListButton`), or as an
    overlay/options entry.
  * Also guard `FastForward`: skip (return the "nothing deposited" response) when the price is
    below the limit.
  * Behaviour when the price is too low: the drone keeps the plorts or leaves them at the source,
    and sells them once the price recovers (prices update daily).
  * Optional: "fall back to silo" if a silo sink program is also set.

### 2.4 Coop roof - FEASIBLE (visual), the function needs a decision

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
## 2b. User feedback (design, not implemented yet)

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
2. Silo menu: "Sell-All Button" with the Market Link icon and the price from the setting, and
   correct title, description and popup texts (not the storage upgrade's text). Buy it; Newbucks
   are deducted and the item turns greyed (owned).
3. "Sell All Plorts Now" appears. Its description shows the count and the value. Confirm: plorts
   leave the silo, the currency HUD pops up, and the log shows the sale. Non-plort items stay.
   After closing the menu, check the market price board for saturation.
4. Save, quit to the menu, load the game: the upgrade is still owned. Check
   `sr2mods_saves\<game>.json`. Load a different save: no upgrade there.
5. Buy, then quit without saving and reload: the upgrade is gone and the money is back.
6. Sell it back (sell_upgrades): refund, and "Sell All Plorts Now" disappears.
7. Remove the mod (or disable `custom_upgrades`) and load the save: the game loads normally,
   with no custom items.
8. Other plot menus (corral, coop, garden) are unchanged.

---------------------------------------------------------------------------------------------
## 5. Open questions for the user

1. Prices: Sell-All Button (default 1000), Hen Wrangler, Coop Roof, drone price-limit feature
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
