# ui/ notes

The settings UI has two independent parts. `core/core.cpp` calls `sr2::ui::StartOptionsTab()` and then `sr2::ui::StartOverlay()` on the worker thread, after the modules are set up. Either one can fail on its own; the failing one logs the reason and returns `false`. The overlay is the guaranteed way to change settings.

Both parts draw whatever is registered in `core::settings`, grouped by module (in registration order) and then by `Def::group`. Neither hardcodes any module's settings. Values are written only with `settings::Set`, never into the game's save or options file. `settings_view.cpp` holds the code both parts share: grouping, snapping to `Def::step`, formatting, key names, module status lines, and the "Saved" bookkeeping.

The UI registers two settings of its own under module `ui`:

| key | kind | default | label |
|---|---|---|---|
| `overlay_hotkey` | Key | `VK_F8` | "Overlay hotkey" |
| `overlay_hint` | Bool | on | "Show overlay hint at startup" (shows a note for 10 s after the first frame) |

The UI never draws an automatic `ui.enabled` toggle, because turning the UI off would lock you out of it.

## Files

| file | purpose |
|---|---|
| `ui.h` | `StartOptionsTab()` and `StartOverlay()` |
| `overlay.cpp/.h` | Dear ImGui on the D3D11 swap chain |
| `game_input.cpp/.h` | gameplay input lock and cursor unlock while the overlay is open (main thread) |
| `options_tab.cpp/.h` | "Mods" category in the game's Options menu |
| `settings_view.cpp/.h` | shared helpers (see above) |
| `il2cpp_util.cpp/.h` | by-name IL2CPP helpers: invoke, unbox, object_new, GC handles |
| `../third_party/imgui/` | Dear ImGui **v1.93.0** (official ocornut/imgui release tag, 2026-10-14). Only the core files, the dx11 and win32 backends, and `LICENSE.txt` are vendored, unmodified. |

## Overlay (`overlay.cpp`)

**Hooks.** The hooks use MinHook through `core::HookManager`, module `"ui"`. To read the vtable, the overlay creates a throw-away `D3D11CreateDeviceAndSwapChain` device and swap chain on a hidden 64×64 window. It gets the function with `GetProcAddress` from the already-loaded `d3d11.dll`.

| hook name | function | vtable slot |
|---|---|---|
| `ui.dxgi_present` | `IDXGISwapChain::Present` | 8 |
| `ui.dxgi_present1` | `IDXGISwapChain1::Present1` | 22 (optional) |
| `ui.dxgi_resize_buffers` | `IDXGISwapChain::ResizeBuffers` | 13 |
| `ui.dxgi_resize_buffers1` | `IDXGISwapChain3::ResizeBuffers1` | 39 (optional) |

**First Present.**
- The overlay gets the device with `GetDevice` and the immediate context, and takes the HWND from `GetDesc().OutputWindow`.
- It creates the ImGui context with `IniFilename = nullptr`, so no `imgui.ini` is written. It uses Segoe UI if the font is present.
- It runs `ImGui_ImplWin32_Init` and `ImGui_ImplDX11_Init`, and subclasses the window with `SetWindowLongPtrW(GWLP_WNDPROC)`.
- Every later Present that comes from a different swap chain is ignored.

**Render target.** The RTV is created lazily from back buffer 0. `ResizeBuffers` releases it, and the next Present recreates it. If the back buffer size differs from the client area (scaled fullscreen), `io.DisplayFramebufferScale` is set so that ImGui still works in client coordinates. The previous render targets are saved and restored around `RenderDrawData`. The DX11 backend saves and restores the rest of the pipeline state itself.

**Threading.** Unity presents on its render thread, while the window messages arrive on the main thread.
- The hooked WndProc only copies messages into a queue. The render thread replays them through `ImGui_ImplWin32_WndProcHandler`, so ImGui is only ever touched by one thread and neither thread waits on the other.
- `Platform_SetImeDataFn` is cleared, because IME positioning would call into the window from the render thread.
- Writes from the overlay go to `main_thread::Post`, because module `OnChange` callbacks may touch Unity. Until the write lands, the widget shows the pending value (dropped after 3 s if the dispatcher stalls).

**Toggling and key capture.** Both poll `GetAsyncKeyState` in Present, and only while the game window is in the foreground. This works no matter how the game consumes keyboard input. After a capture or a toggle, the key has to be released before it counts again.
- Capture mode takes the next newly pressed key: any keyboard key (generic Shift/Ctrl/Alt are skipped for their Left/Right codes; `VK_PROCESSKEY`/`VK_PACKET` are ignored) or mouse 3/4/5 (`VK_MBUTTON`, `VK_XBUTTON1`, `VK_XBUTTON2`). Left and right click are never captured, so the UI stays clickable; the left click that starts capture is one of them (ImGui buttons fire on release anyway, and keys held when capture starts are snapshotted and ignored).
- Esc cancels. Backspace or Delete clears the bind (value 0, shown as "(none)"; modules treat 0 as unbound). For the overlay hotkey, clearing resets it to its default (F8) instead, so the overlay can't become unreachable.
- Mouse clicks: the WndProc latches `WM_MBUTTONDOWN`/`WM_XBUTTONDOWN` (also while the overlay is closed) into `g_mouseLatch`, so a click shorter than one frame between two polls is not missed. Capture and the hotkey poll consume the latch; it is cleared at capture start/end and every hotkey poll, so a stale click can't fire after a rebind.
- The overlay hotkey may be mouse 3/4/5. `HotkeyVk()` falls back to F8 for 0 and for left/right click (1/2), e.g. from a hand-edited config.
- Names (`KeyName` in `settings_view.cpp`): "Mouse 1".."Mouse 5", F1..F24, fixed names for L/R modifiers, Win, Menu, Pause, Print Screen and media keys, otherwise `GetKeyNameTextW` on the scan code (`MAPVK_VK_TO_VSC_EX`, extended bit set for the E0 keys plus the nav cluster/arrows/Num /, Num Lock so they aren't named as numpad keys), fallback `VK 0x..`.
- Leak while open: mouse 4/5 messages are in the `WM_MOUSE*` range and are swallowed like every other mouse/keyboard message (so no `WM_APPCOMMAND` "browser back" is generated either); raw input only gets `DefWindowProc`. Modules polling `GetAsyncKeyState` still see them, exactly as for keyboard keys (see Remaining leak below).

**Window contents.**
- One collapsing header per module: status line from `ModuleManager::List()` (FAILED shown in red), an Enabled checkbox, then one widget per `Def`.
- Widgets by kind: Bool → checkbox, Float → slider snapped to `step`, Int → slider snapped to `step`, Enum → combo, Key → capture button.
- Optional group sub-headers, tooltips, and a per-module "Reset to defaults" button.
- At the top: a version-guard warning when `version_guard::Mismatch()`, or the status line otherwise.
- At the bottom: "Saving..." for 1.5 s after the last change, then "Saved". `settings::Flush()` runs when the overlay closes.
- The UI section also shows the status of both UI paths.

**Profiles section** (top of the scrolling area, collapsing header "Profiles"). It uses `core/profiles.h`; the file format and rules are in the README.
- The section shows the active profile line, with "(modified)" or "(file missing)". `profiles::IsModified()` is cached for 250 ms, so it is cheap to call every frame.
- Next is a combo of all profiles: the active one is marked `[active]`, broken files `[broken]`, and hovering shows the description or the parse error. The selection defaults to the active profile.
- **Load** calls `profiles::Load`. From the render thread this posts the apply to the main thread, the same model as `Write`, and the status line shows "Loading..." and then the result.
- **Save** overwrites the active profile after a confirmation modal and keeps its description. **Save as...** opens a modal with a name and an optional description. If the name exists, the button becomes "Overwrite"; Enter never confirms an overwrite. **Rename** opens a modal; a clash also needs "Overwrite". **Delete** asks in a modal.
- **Open profiles folder** runs `ShellExecuteW` on a short-lived thread (shell32 is delay-loaded), so the render thread never waits on the shell. If that fails it starts `explorer.exe` with `CreateProcessW`. **Refresh** rescans the folder; opening the overlay rescans it too.
- File operations (save, rename, delete, rescan) run on the render thread when clicked. The files are small, so the cost is one frame at most.
- `StartOverlay` calls `profiles::Init()`, which creates the folder and the starter profiles after the modules have registered their settings.

**Text input.** The WndProc queues `WM_KEYDOWN`/`WM_KEYUP`/`WM_CHAR` (the `WM_KEYFIRST..WM_KEYLAST` range) for ImGui and swallows them, so typed text never reaches the game's window procedure. `WM_INPUT` only gets `DefWindowProc`, and gameplay actions are locked through the Input System map (see below). Name fields filter characters to what `SanitizeName` keeps. While an ImGui text field has focus (`io.WantTextInput` from the previous frame), a hotkey that produces a character (`MapVirtualKeyW(..., MAPVK_VK_TO_CHAR) != 0`) does not toggle the overlay, so typing that letter does not close it. Esc closes the profile modals. **Remaining leak:** modules that poll keys themselves with `GetAsyncKeyState` (minimap zoom key, hl2_movement keys) still see keys typed into the overlay. They would need to skip polling while `sr2::ui::overlay::IsOpen()`.

**Blocking game input while the overlay is open:**
1. The WndProc swallows `WM_MOUSE*` and `WM_KEY*`/`WM_CHAR`. `WM_INPUT` gets only `DefWindowProcW`, so Unity never sees raw input. `WM_SYSKEY*` still goes to the game, so Alt+F4 and Alt+Enter keep working.
2. SR2 uses the Unity Input System. On every main-thread tick (`main_thread::AddFrameCallback("ui.overlay")`), `game_input.cpp` finds `MonomiPark.SlimeRancher.Input.InputDirector` and gets its `_mainGame` map through `InputActionMapReference.op_Implicit`. If that map is enabled, it calls `InputActionMap.Disable()`. On close it calls `Enable()` again, but only if the overlay disabled it, and it re-asserts the disable each tick, because the game may re-enable maps itself.
3. `UnityEngine.Cursor.lockState` is set to `None` while the overlay is open and restored on close. If the game had the cursor locked, which also hides it, ImGui draws a software cursor. It also draws one if the dispatcher or the Cursor API is not available.

**Overlay limitations:**
- If the main-thread dispatcher is not running, only the window-message filter applies, so gameplay actions polled through the Input System may still fire.
- If Unity reads keyboard or mouse through a path that bypasses the window procedure, step 1 cannot stop it. Step 2 is the real lock.
- ImGui gamepad navigation is off; use mouse or keyboard.

## Options tab (`options_tab.cpp`)

**Approach: inject data, not cloned UI.** The Options menu is data-driven: `OptionsConfiguration` → `OptionsItemCategory` (one tab) → `OptionsItemDefinition` (one row, drawn with its `_controlPrefab`). Injecting data objects lets the game's own adapters build the tab strip entry and the rows. That gives the native look, the game's selection, details panel and gamepad navigation, without any GameObject surgery or navigation rebuilding. Cloning the UI after binding would need the tab strip's view models and row view holders rebuilt by hand, and those are bound to model objects anyway.

All lookups are by name: classes, fields, and methods (including the hook targets). Before installing, `StartOptionsTab` checks that no two hook targets share an address, which would mean IL2CPP folded the methods.

| hook | role |
|---|---|
| `OptionsUIRoot.BindCategories` | **Before the original runs:** pick templates from the passed `OptionsConfiguration`: the first non-conditional, platform-agnostic `ToggleOptionDefinition` and `ScriptedValueRangeOptionDefinition`, plus a category. Then build or update one cloned definition per setting, and append the Mods category to the configuration's `items` list. **After the original runs:** remove it from the list again, so the game's asset is only changed for the duration of the call. |
| `OptionsViewModel.GetOptionsItemsForCategory` | For the Mods category, returns an `OptionsItemModel[]` of models built with `il2cpp_object_new` plus the `PresetOptionsItemModel(definition)` constructor. This is what `CreateOptionItemModel()` does, minus adding the model to the definition's `_optionsItemModels`, which a runtime clone may lack. The models are never added to the game's `OptionsModel`, so nothing reaches `OptionsV02` or the game's settings file. The model constructor picks the current value through `GetDefaultPresetIndex()`, because `_defaultValue` is refreshed from the setting before each bind. |
| `ToggleOptionDefinition.ApplyPresetSelection`, `ScriptedValueRangeOptionDefinition.ApplyPresetSelection` | For our definitions, the original never runs. The value is computed with the game's own mapping and written with `settings::Set`. Toggle "on" is the preset whose label or reference id is `_positiveOption`. Range uses `GetValueForIndex`. |
| `CategoryTabViewHolder.BindData`, `OptionsItemViewHolder.BindData`, `OptionsPresetSelector.OnSelectionChanged`, `OptionsDetailsView.BindData`, `LocalizeStringEvent.UpdateString` | Localization bypass. The BindData hooks register the view's `LocalizeStringEvent` as ours, together with our text, keyed by the object and its `m_CachedPtr`, and release it on the next rebind. The hook on `UpdateString(string)`, the component's only text sink, substitutes our text for the components we own, so paged or recycled row views, deferred async localization and locale changes all show our label. Game components pass through untouched; the components are never disabled. Texts: tab "Mods"; label = module name for the master toggle, otherwise the `Def` label (indented); value "On"/"Off" or the formatted number or enum label; details = tooltip, plus module status, plus version warning. |

**Cloned definitions.** Each cloned definition (`Object.Instantiate` of the template):
- gets a unique `_referenceId` (`sr2mods.<module>.<key>`) and a private clone of the template's `ScriptedBool`/`ScriptedFloat`, so the template's real setting can never be written;
- has `_applyImmediately` on and `_requireConfirmation`, `_showCondition` and `_normalizeOutputValue` cleared;
- is flagged `DontUnloadUnusedAsset` and pinned with a GC handle.

**Mapping settings to rows.**
- Floats, Ints and Enums map onto a stepper with `min`/`max`/`step`. Enums use 0..n-1 with wrap-around. The step is made coarser, staying on the `Def`'s grid, if the stepper would have more than 201 positions.
- Key settings cannot be shown as a stepper, so they stay overlay-only.

**Failures.** Every hook body runs inside SEH plus a C++ try/catch. On any error the tab switches itself off (logged) and the game continues; the original game method still runs for game rows.

**Checked against `re/symbols.md` (IDA pass):**
- `BindCategories` builds `categories` as `this.categories.Concat(config).Append(about).Where(CheckIsActive)`. Appending to `config.items` for the duration of the call therefore puts "Mods" after the stock tabs and before About.
- `SwapCategory` needs a real `OptionsItemCategory`; a clone is one.
- `BindItemCategory` calls `GetOptionsItemsForCategory(...).ToList()`, so an array works.
- `CreateOptionItemModel` is `new PresetOptionsItemModel(this)` and adds the model to `_optionsItemModels`. The constructor sets presets and the current index from `GetDefaultPresetIndex()`.
- Toggle `ApplyPresetSelection` is `TargetBool = (index == 1)`.
- Range: value = `index * step + min`.

**Value flow** is the game's own: the selector changes the temporary value, then **Apply** (or the save/revert prompt when leaving the tab) runs `SaveSelectedValue` → `ApplyPresetSelection` → our hook → `settings::Set`. As with the game's own rows, a change counts once it is applied.

**Profiles:** the Mods tab has no profile row. A stepper row needs a cloned definition that no `settings::Def` backs, with a preset list that changes at runtime. That is too much new surface in the hook code that has crashed before, for something the overlay already does. Use the overlay's Profiles section.

**Known gap:** the game's "reset category to default" on the Mods tab re-applies the current values, not the `Def` defaults, because `_defaultValue` holds the current value. Use the overlay's "Reset to defaults".

**First in-game runs (2026-10-22).**
- Overlay: worked; the user rebound the hotkey to Insert.
- Options tab: the Mods tab was injected (56 to 86 rows), but opening it hit an access violation in `GetOptionsItemsForCategory`. The tab showed no rows, the Display icon, and stale "Language" details. After that the tab stayed disabled for the session, which is also why it was missing after returning to the main menu.

**Probable cause:** core's `Resolver::Invoke` calls `il2cpp_format_exception` (managed `ToString`) outside its SEH guard whenever a managed exception comes back. The likely trigger is `CreateOptionItemModel` throwing a NullReferenceException because the clone has no `_optionsItemModels` list.

**Fixes:**
- `il::Invoke` now has its own guarded `runtime_invoke` and builds the exception text itself (class name plus `_message`).
- Models are built with the constructor directly.
- Each row has its own SEH frame and logs `row m.k skipped ... at step '<step>'`. Every step of each handler is named in the error log, in the form `DISABLED for this session: <why> at step '<step>' (row m.k) in <hook>`. While disabled, every Options open logs why the Mods tab was not injected.
- Injection runs again on every `BindCategories` (every `OptionsUIRoot` instance, every scene). Cached objects are checked with `IsAlive` and rebuilt when dead.
- The Mods tab takes the icon of a "control/input/gameplay/accessibility/other" category, falling back to the About icon.
- An empty Mods tab shows "Mods: settings for sr2mods" in the details panel (optional `OptionsUIRoot.BindItemCategory` hook).

**Third run:** the access violation was in `gchandle_free`. In this IL2CPP build, `Il2CppGCHandle` is pointer-sized: `Il2CppGCHandle il2cpp_gchandle_new(obj, pinned)` and `il2cpp_gchandle_free(Il2CppGCHandle)` (see `re/dump/il2cppinspectorredux/cpp/appdata/il2cpp-api-functions.h`). Core's table declares them as `uint32_t`, so handles were truncated and freeing one faulted.

**Fixes:**
- The UI resolves `il2cpp_gchandle_new` itself as pointer-sized (`il::Keep`). It only keeps assets alive for the whole process and never frees a handle.
- Row models use no handles at all: they are written straight into the managed array that is returned. A volatile stack reference keeps the array alive, because the GC scans stacks conservatively.

**Fault policy** (coordinator, after a game crash): catching an access violation is never normal control flow.
- Any native exception, in a hook or inside game code reached through `il::Invoke`, calls `il::MarkFault`.
- From then on every options-tab hook passes straight through to the original, and no game method is called: no text restore, no category removal, and `game_input` stops.

Two exceptions remain, by design:
- `GetOptionsItemsForCategory` still returns an empty array (an IL2CPP runtime allocation, not a game method) for an already-bound Mods tab, so the game never looks up our definitions.
- Our own definitions never reach the original `ApplyPresetSelection`.

One side effect: after a fault, a game row whose view previously showed one of our rows can keep our label until the menu is reopened.

**Recommendation for core:** fix the gchandle signatures in `core/il2cpp_api.h` (`void*`/`uintptr_t`), and guard or avoid `il2cpp_format_exception` in `Resolver::Invoke`.

**Fourth run:** rows worked, but labels after the first page showed the template's text ("Vertical field of view" / "Enable vsync"). The adapter is paged and binds asynchronously (`AdapterView.AnimateBindDataList`), and the old approach (disable the LSE plus a direct TMP write) was overwritten by a later refresh. It was replaced by the `UpdateString` interception described in the table above.

**ArgumentOutOfRange after a fault** (crash triage `re/crash-2026-10-22-1855.md` §3): my fault-mode fallback returned an empty model array for an already-bound Mods tab. `BindItemCategory` then calls `BindOptionsDetailsView(0)` → `optionsItemModels[0]`, which throws.

**Fix:** the models are now built during `BindCategories`, and the tab is injected only if at least one row model exists. That array is cached and pinned by a pointer-sized handle (the old handle is freed only in healthy mode).
- `GetItems` for the Mods tab returns fresh models when healthy.
- Otherwise it returns the cached non-empty array, a pointer return with no game calls. The tab can never yield an empty list, and our category never reaches the game's own lookup.
- After a fault or disable, the next Options open does not inject the tab, so its category list, tab strip and details stay consistent.

**Detour guarding** (all ui detours). Game code (the original) always runs outside the guard, and after a fault each detour is a pure pass-through.

| Detour | Guard |
|---|---|
| Options tab (9 hooks plus the optional `BindItemCategory`) | `Guarded()`: SEH plus try/catch, `il::MarkFault` on a native fault. `UpdateString` has its own SEH lookup. |
| `Present`/`Present1` | `OnPresentGuarded` |
| `ResizeBuffers`/`ResizeBuffers1` | `ReleaseRtvGuarded`. It still releases our RTV after a fault, because DXGI needs that for the game's resize. |
| `WndProc` | `WndFilterGuarded` |
| Main-thread tick (`game_input`) | stops on `il::AnyFault()` |

I deliberately did **not** adopt `core::DetourGuard` for these. On a fault it disables the module's hooks at the next tick. For the options tab, removing the `GetOptionsItemsForCategory` interception while a Mods tab is bound would bring back the empty-list ArgumentOutOfRange. All ui hooks are also registered under HookManager module `"ui"`, so a tab fault would unhook the overlay too. The ui fault mode is equivalent otherwise: the logic is skipped and the original is called.

## Build

`CMakeLists.txt` (core-owned) globs `ui/*.cpp`, builds `third_party/imgui` as a static lib with `/W0`, links d3d11/dxgi, and delay-loads `d3d11`, `dxgi`, `d3dcompiler_47` (needed by the DX11 backend's `D3DCompile`) and `dwmapi`. When ImGui is missing, `overlay.cpp` builds a stub (`SR2_HAVE_IMGUI`) that logs and returns false. The ui sources build `/W4` clean.
