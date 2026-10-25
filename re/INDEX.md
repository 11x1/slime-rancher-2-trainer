# RE index: start here

What is known about Slime Rancher 2 1.3.0 (Steam, Unity 6000.0.73f1, IL2CPP metadata v31, D3D11) and
where the mod's code lives. Read this, then only the system files and module NOTES your task needs.
Before grepping the dumps or opening IDA, run `python re/re_lookup.py <Name>`.

Conventions in every file: `MP.SR` = `MonomiPark.SlimeRancher`; RVA = offset in GameAssembly.dll
(IDA VA = 0x180000000 + RVA); field offsets include the 0x10 object header; tags **IDA/code** =
decompiled, **dump** = signature/offset only, **data** = read from asset bundles, **log** = seen in
sr2mods.log, **inferred/assumed** = not checked. The mod resolves everything by name at runtime; RVAs
are for cross-checking only. Each system file starts with a curated part; sections titled
"From the <module> notes" were harvested from module notes and code comments.

## Game systems

| file | system |
|---|---|
| [vacpack.md](vacpack.md) | vacpack, ammo slots (AmmoSlotManager, AmmoModel, AmmoSlotDefinition.IsAllowed), VacuumItem slot input, capture path, player inventory |
| [upgrades.md](upgrades.md) | player upgrades: UpgradeDefinition levels, UpgradeModel, ActorUpgradeHandler.ApplyUpgrade -> BatchModifiers -> AmmoSlot.OnSlotChanged NRE chain, vanilla Tank Booster / Extra Tank data |
| [fabricator.md](fabricator.md) | Fabricator UI and categories (GetItems inlined into the runtime ctor, IFabricatableItem explicit names), gadgets, variants, placement, blueprints, decorations, type groups |
| [drones.md](drones.md) | ranch/quantum drones, drone station and its menu, programs, sources/sinks, market price checks, battery, Quantum Cloud |
| [plots.md](plots.md) | land plots, plot purchase menu (LandPlotUIRoot), Upgrade enum, plot prefabs and child paths, collectors, silos, coops, corrals, gardens, incinerator, upgraders |
| [shop.md](shop.md) | ProntoMart shop (categories, daily deals, purchase), plort economy (TrySell, prices), shop music |
| [time-weather.md](time-weather.md) | TimeDirector (pause, FastForwardTo, world time), weather registry and its debug API |
| [map.md](map.md) | map UI, projection, fog (shaders, events), zones, markers, teleporters / warp network |
| [slimes.md](slimes.md) | slime definitions, diet and eating, emotions and hunger, Ringtail / Phosphor / feral / shadow rules, radiant (rare) slimes, largos, chickens |
| [audio.md](audio.md) | music and SECTR audio (buses, cues, sources), music box, Boombox (GadgetMusicPlayer), Unity AudioSource / AudioClip / AudioLowPassFilter |
| [ui.md](ui.md) | Options menu, list menus (ListUIRoot/ListUI, details panel, alt action), popups, HUD and notifications, prompt pills / input hints, input system, UI stack and input locking, main menu, Slimepedia, D3D11 / Steam overlay host facts |
| [save.md](save.md) | AutoSaveDirector (file naming, rotation, load), actor save, persistence ids, the mod sidecar format and rules |
| [player.md](player.md) | character controller, movement math, abilities (jump, sprint, jetpack), energy, teleport, knockout |
| [world.md](world.md) | context singletons (SceneContext, GameContext, LookupDirector), identifiables and groups, resource nodes, growers, cells, spawners, world buttons, scenes, regions, achievements, reachable sprites |
| [unity.md](unity.md) | Unity / IL2CPP engine facts: resolving, overloads, virtual dispatch, folded bodies, GC handles, icalls, version guard, ScriptedValue, uGUI / Physics / Texture gotchas |

Other files:
- [hooks.md](hooks.md): every hook the mod installs, by target. **Check it before hooking anything** (MinHook allows one hook per address; IL2CPP folds identical bodies). Add your hook there.
- [debug-mode.md](debug-mode.md): developer debug tooling left in 1.3.0, debug scene bundles, easter eggs and secrets (shop songs, the cat, achievements).
- [custom-upgrades.md](custom-upgrades.md): the custom_upgrades framework design (ids, sidecar, gate, menu injection, world objects).
- [crash-2026-10-22-1855.md](crash-2026-10-22-1855.md): crash analysis (truncated GC handles), minidump method.
- `symbols.md`, `candidates.md`, `shadow-slimes.md`: moved; they only redirect.

## Lookup script (offline, stdlib only)

`python re/re_lookup.py NAME [NAME ...] [-s TEXT] [-c TEXT] [--rva HEX] [-f|-m] [-g TEXT] [-a] [-n N]`

| call | prints |
|---|---|
| `re_lookup.py TimeDirector` | class header (namespace, assembly, bases), fields (+offset type name), methods (RVA signature), nested types; `-f` fields only, `-m` methods only, `-g text` filter, `-n 0` no limit |
| `re_lookup.py TimeDirector.FastForwardTo AmmoSlot.OnSlotChanged` | the member(s): RVA + signature, or field offset; `Class.Prop` also finds `get_/set_Prop` |
| `re_lookup.py MonomiPark.SlimeRancher.UI.Fabricator.GadgetFabricateCategory` | namespace-qualified; nested types as `Outer.Inner` (e.g. `TimeDirector.DayWindow`); enums print their values (`LandPlot.Upgrade`) |
| `re_lookup.py -s CheckCanFabricate` | every member whose name contains the text (explicit interface implementations show their full `Ns.IFace.Method` name) |
| `re_lookup.py -c FabricateCategory` | class names containing the text |
| `re_lookup.py --rva 0x4ED3F0` | every method at that address; more than one = folded body, never hook |

- `[shared xN]` after a method = its body is folded with N-1 others.
- Compiler-generated types (`<>c`, `d__N`, DisplayClass) are hidden unless `-a`.
- Source: `re/dump/cpp2il_diffable-cs_rva/DiffableCs/<Assembly>/<Namespace>/<Class>.cs` in the **main checkout only** (`C:\Users\franz\Documents\sr2-mods\re\dump\`, not in git); the script finds it from any worktree via `git rev-parse --git-common-dir` (override: `SR2_DUMP`) and keeps a sqlite index there (built once, ~10 s; `--rebuild`).
- Also in the dump folder: `il2cppinspectorredux/types.cs` (single-file C#), `cpp/appdata/il2cpp-types.h` / `il2cpp-functions.h`; its `metadata.json` export addresses are 0xC00 too high (re/unity.md).
- IDA: `re/ida/GameAssembly.i64` (main checkout), names and types applied; IDA MCP tools are approved for offline analysis. Asset bundles (prefab data, type groups) were read with UnityPy from the scratchpad; no lister script in the repo.

## Where things live in the repo

Modules (`modules/<name>/`, each with `NOTES.md` when it has design worth writing down): collectibles, custom_upgrades (also sidecar owner), debug_tools, discord_rpc, drone_price_limit, free_placement, hl2_movement, instant_build, map_tools, minimap, misc_tweaks, plot_upgrades, quick_start, quick_travel, sandbox, sell_upgrades, slime_dj, spawn_scaling, wall_markers; `modules/common/` shared module code. UI: `ui/` (overlay + Options tab, `ui/NOTES.md`). Loader/proxy: `proxy/`.

Core helpers (`core/`):
- `resolver.h` `il2cpp::Resolver::Get()`: `Class`, `Method(k, name, argc)`, `MethodByParams(k, name, {type names})` (overload-exact, searches parents), `Field/FieldOffset`, `MethodPointer`, guarded `Invoke`, `ToUtf8`, `Rva(p)`.
- `module.h` / `module_manager.cpp`: `ModuleContext::HookMethod(name, image, ns, class, method, argc, detour, &orig)` / `HookAddress` (hook names `<module>.<name>`, created disabled, toggled with the module); `Module::SetupWhileDisabled`.
- `hook_manager.h`: `HookManager::Get().Create(module, name, target, detour, &orig)` / `Enable` / `Remove` over MinHook; refuses an already hooked target.
- `detour_guard.h`: `DetourGuard{module, name}.Run(lambda)` around a detour's mod logic (never the original call); a fault marks the module FAILED and its hooks pass through. `guard.h`: `Try`, `GuardedCall`, `DescribeAddress`.
- `main_thread.h`: the dispatcher. `Post(fn)` (runs next tick on the main thread), `AddFrameCallback(owner, fn(dt))` / `RemoveFrameCallback`, `IsMainThread`; driven by an `EventSystem.Update` hook. All IL2CPP calls go here.
- `settings.h`: `settings::Def` (kind, group, label, tooltip, unit, parent / parentValue, anyOf, zeroText, displayScale), `Register / Get* / Set / OnChange`, `Lock / Unlock / LockReason`, `GetString / SetString`, `Flush`. Shown in both UIs automatically.
- `text_override.h`: texts for LocalizedString instances the mod owns (`Set / Remove / Lookup`, `SinkActive`; sink = options_tab's `LocalizeStringEvent.UpdateString` hook).
- `unity.h`: `FindObjectsOfTypeAll` (never per frame), `IsAlive`, `GetComponent`, `Instantiate`, `SetActive`, `Field<T>`, arrays / lists, `NewGCHandle / GCHandleTarget / FreeGCHandle` (pointer-sized), `Camera_main`.
- `perf.h` `SR2_PERF_SCOPE(name)`; `paths.h`; `config.h` (sr2mods.json); `profiles.h`; `version_guard.h` (GameAssembly hash check); `log.h` (`SR2_INFO/WARN/DEBUG`, sr2mods.log).

UI components to reuse (`ui/`; details in ui/NOTES.md):
- `item_grid.h` searchable icon-tile grid (`Draw`, `DrawTile`, `SearchBox`); `item_icons.h` game Sprite as ImGui image (`Register` main thread, `Draw` render thread); `mod_icons.h` embedded PNGs from `assets/icons` as Unity Sprites (`Get(name, sizeLike, greyed)`, `Has`).
- `nav.h` keyboard focus for drawn widgets: `nav::Button` instead of `InvisibleButton` for anything clickable, `nav::Ring` after drawing it, `nav::TabGroup` for a grid / list (one Tab stop, arrows inside).
- `theme.h` palettes and fonts; `menu.h` / `menu_layout.h` the overlay window and sidebar; `settings_view.h` shared settings rendering and posted writes; `spotlight.h` Ctrl/Alt+Space search.
- `hud.h` world-space markers and line segments (`Publish(owner, Frame)` with `markers` / `lines`, `Clear`); `momentum_hud.h` speed panel; `hud_edit.h` HUD edit mode and keycaps (`DrawKeycap`, `DrawKeyCombo`).
- `game_input.h` input lock while the overlay is open; `overlay.h` `IsOpen()` (modules polling keys skip while true).
- `il2cpp_util.h` (`sr2::ui::il`) by-name lookups, guarded `Invoke`, `IsA`, `Keep/Release`.
- `modded_label.h` `SetMenuEnableListener` (the single `MainMenuLandingRootUI.OnEnable` listener); `quick_start_hint.h`.
- Bridges (module publishes a snapshot on the main thread, the render thread draws, the UI posts actions back): `sandbox_bridge.h`, `sandbox_extras_bridge.h`, `sandbox_upgrades_bridge.h`, `sandbox_weather_bridge.h`, `slime_dj_bridge.h`, `discord_bridge.h`.
- Debug highlighter registry: `modules/common/debug_marks.h` (modules publish labelled world boxes of what they touch while `Wanted()`; debug_tools draws them, `modules/debug_tools/highlight.cpp`).
- Game-styled button: `modules/common/game_button.h` (the game's popup button as a runtime uGUI button: label, key glyph, selected tint, disabled grey, polled click; used by the silo receipt, the drone station dialogs and the Corral Radio menu's header buttons).
- Game-UI helpers in modules: `modules/custom_upgrades/plot_menu_slider.h` (uGUI slider in the plot menu), `sale_notify.h` (HUD notices), `silo_receipt.cpp` (runtime uGUI dialog with input lock), `gadget_look.h` (looks of mod gadget definitions), `variant_ext.h` (plug into the gadget-variant hooks), `modules/minimap/ugui.cpp` (runtime uGUI).

## Example: "change how a corral upgrade card sells"

re/plots.md (LandPlotUIRoot, PlotUpgradePurchaseItemModel, purchase flow) -> re/ui.md "List menus" (Sell button / alt action, details panel) -> re/hooks.md (sell_upgrades and custom_upgrades already own `DetailsMoreButtonView.BindData`, the `ListUIRoot` alt-action and `RebuildUI` hooks) -> `modules/custom_upgrades/NOTES.md` "Plot menu" and the sell_upgrades header comment.

## Adding findings

New RE facts go into the matching `re/<system>.md` (class, RVA, signature, field offsets, chain, gotcha, which module uses it), new hooks into re/hooks.md. Module NOTES keep the design and link to the system file.
