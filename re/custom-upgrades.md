# Custom land-plot upgrades: framework design

The custom_upgrades framework: ids, sidecar, gate, menu injection, world objects. Current features
and their state: `modules/custom_upgrades/NOTES.md`. Game facts (classes, RVAs, fields, prefab data)
moved to the system files: re/plots.md (plots, plot menu, prefabs, collectors, silos, coops),
re/save.md (save files, sidecar format), re/ui.md (list menus, popups, notifications, input locking),
re/drones.md, re/shop.md, re/slimes.md, re/vacpack.md. Section numbers below are kept because code
comments and NOTES cite them.

## 1. Framework

### 1.1 Upgrade ids and the vanilla save
- `LandPlotModel.upgrades` is a saved `HashSet<LandPlot.Upgrade>` (enum 1..20). How vanilla reads an
  unknown value is unchecked, so **custom ids are never put into that set.**
- Custom ids: `0x5352xxxx` ("SR" << 16), `api.h` `IsCustomUpgrade`. They exist only in memory (menu
  items, hook parameters) and, by key, in the sidecar.
- Detours keep vanilla code away from them: `LandPlot.HasUpgrade` answers custom ids from the sidecar
  (one compare for vanilla ids); `LandPlot.AddUpgrade` never passes a custom id to the original, not
  even when the module is off or faulted.

### 1.2 Persistence: the sidecar ("extended save file")
- One `sr2mods_saves\<save>.sav.json` per .sav, in the game folder; format, read order, write rules
  and limits: re/save.md "The sidecar". Generic per-save / per-plot values: `api.h`.
- Why the game folder and not the Steam save folder: Steam Cloud syncs
  `LocalLow\MonomiPark\SlimeRancher2\Steam\<id>\`; extra files there would use the game's cloud quota,
  land on PCs without the mod, and the game enumerates that folder. Cost: sidecars don't follow a save
  through the cloud (copy by hand; names match the .sav files).
- Reset: the `AutoSaveDirector.Load` / `StartNewGame` detours drop the in-memory state; unsaved
  purchases vanish together with the unsaved Newbucks, like vanilla.
- Save safety: the vanilla save never holds anything of ours. Without the mod (or without the sidecar)
  the save loads unchanged and only the custom upgrades are gone.
- Demolish: the `LandPlotLocation.Replace` detour drops the plot id's whole entry (in memory; written
  with the next save).

### 1.2b Gate: off by default, locked on per save
- `custom_upgrades.enabled` defaults to **off**: no custom item, tier effect, world object or per-save
  value until it is on (overlay: Ranch > General).
- Turning it on with a save loaded marks that save (per-save value `custom_upgrades.on` = "1", written
  with the next game save). While a marked save, or one whose sidecar owns any custom upgrade, is
  loaded, the switch is pinned on with `settings::Lock` (tooltip "Locked on: this save uses custom
  upgrades. ..."). Nothing is locked on the title screen.
- With the switch off the module is still set up (`Module::SetupWhileDisabled`, hooks disabled); its
  2 Hz `GateTick` watches the loaded save (in game = `SceneContext.GameModel` set; save =
  `_currentGameMetadata` GameName + SaveNumber, checked once per new save with a read-only sidecar
  lookup in `LoadSidecar`'s order). Such a save turns the module on (posted `settings::Set`); leaving it
  turns it back off, so the global switch stays what the player chose.
- A session never saved leaves no mark. sell_upgrades keeps a sold Ash Trough's ash in the sidecar only
  while custom upgrades are on (else in memory only).

### 1.3 Menu injection (the plot purchase menu)
- Hooks: `LandPlotUIRoot.UpdateMenuItemAvailabilityAndHidden` (pre: list update, post: dynamic texts)
  and `ListUIRoot.RebuildUI` (shared generic body, class-filtered; needed because RebuildUI with the
  menu open does not re-run the availability update: re/ui.md "List menus").
- Items: for each menu configuration with an upgrade item, our items are cloned once per session with
  `Object.Instantiate(<vanilla PlotUpgradePurchaseItemModel>)` (template: the silo's
  STORAGE_CAPACITY_INCREASE item, else any item with one cost entry; the clone must own its
  LocalizedStrings and cost list, else that menu gets no custom items). Set on the clone: `_upgrade` =
  our id, first cost `_amount` = price (others 0), `_icon` (mod icon, `ui/mod_icons.h`), name
  `sr2mods_<key>`, hideFlags `DontUnloadUnusedAsset`, a never-freed GC handle. `_unavailableIcon`
  keeps the template's checkmark (owned tick like vanilla).
- The category list is shared by all plots of a type: each update starts from the vanilla list (ours
  out, vanilla items we took out back at their index), then places this plot's cards: one card per tier
  chain; a tier on a vanilla upgrade replaces that vanilla item at its index once owned (the vanilla
  item is pinned by a GC handle while out); `after` places an item behind a vanilla one; the rest is
  appended. `List.Insert` / `IndexOf` / `Remove` via runtime_invoke.
- Availability: vanilla `IsAvailable = () => !plot.HasUpgrade(_upgrade)` goes through our HasUpgrade
  detour, so owned custom items grey out like vanilla ones. Purchase: the whole vanilla flow (popup,
  `Spend`, AddUpgrade = our detour, cue, RebuildUI, FX); analytics get a number for our ids.
- Texts: `core/text_override.h` maps our LocalizedString instances to texts through the
  `LocalizeStringEvent.UpdateString` hook (`ui/options_tab.cpp`). Without that sink
  (`text_override::SinkActive()`) custom items are **not shown** (they would show the template text).
- Fault / disable: items are removed from the menus (on a fault within one frame) and their price set to
  int.MaxValue so an open menu can't buy them through vanilla code.
- sell_upgrades: owned custom items sell through `custom_upgrades::CanSell` / `Remove` (half price,
  rounded down), then `RebuildUI`; `ChainSellTarget` names the owned tier under a tier card.

### 1.4 World objects
- Prefabs without a scan: `LookupDirector.GetPlotPrefab(Id)` (all upgrade parts as inactive children),
  `get_GadgetDefinitions()`, `get_MarketLink()`, `get_RefineryLink()`, `_toyDefinitions` (re/plots.md).
- Clone with `Object.Instantiate(childOfPrefab, plotTransform)`, set local transform; null or intercept
  components that point into the prefab (e.g. `PlortCollectorActivator.Collector`). Clones are never
  saved: a ~1 Hz tick rebuilds them from the sidecar after load and plot changes; sold -> destroyed
  (`Destroyer.Destroy`).
- Custom meshes (not done): GameObject + MeshFilter/MeshRenderer, `new Mesh()` + vertices/triangles/uv/
  normals, a cloned game material (HDRP Lit); data from a file next to the DLL; colliders via
  `MeshCollider.sharedMesh` or BoxColliders.

### 1.5 Interaction and custom UI
- World buttons: reuse `ITechActivator` components (re/world.md "Interaction interfaces"); clone the
  corral collector button and detour `PlortCollectorActivator.Activate` (our clones: animation + cue,
  our action, skip the original).
- Opening a menu: `UIActivator` / `LandPlotUIActivator` push a UI prefab via `UIDirector.CreateAndPush`.
  A runtime dialog locks input with a `UIInputLocker` on its root (re/ui.md).
- Sliders: runtime uGUI (`modules/custom_upgrades/plot_menu_slider.cpp`, styled with `SliderStyler`);
  the game has no slider prefab in the plot UI.

## 2. Features (current state in modules/custom_upgrades/NOTES.md)

- **2.1** Coop Hen Collector: NOTES "Coop: Hen Collector and Air Net"; elder collector / PlortCollector /
  breeding facts in re/plots.md.
- **2.2** Silo Sell-All: NOTES "Silo Sell-All"; economy in re/shop.md; notices and input locking in re/ui.md.
- **2.2b** Silo capacity tiers: NOTES "Silo capacity tiers"; StorageUpgrader / AmmoSlotManager in
  re/plots.md, re/vacpack.md.
- **2.3** Drone price limit: `modules/drone_price_limit/NOTES.md`; market sink in re/drones.md.
- **2.4** Coop Air Net: NOTES "Coop: Hen Collector and Air Net"; corral/coop prefab data in re/plots.md.
- **2.5** Drone Battery Saver: NOTES "Drone Battery Saver"; battery model in re/drones.md.
- **2b.1 / 2b.1b** Sell button and gamepad/keyboard selling (sell_upgrades): list-menu internals in
  re/ui.md "List menus".
- **2b.2** Per-plot tiers: NOTES "Plot upgrades (ids, keys)", `modules/plot_upgrades/NOTES.md`.
  Hot path: `Tier()` / `Multiplier()` use a `LandPlot* -> {plot id, owned keys}` cache invalidated by a
  version counter on buy, sell, load and plot replace (no GetComponent / runtime_invoke per call).
