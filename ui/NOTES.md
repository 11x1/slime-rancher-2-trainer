# ui/ notes

The settings UI has two independent parts. `core/core.cpp` calls `sr2::ui::StartOptionsTab()` and then `sr2::ui::StartOverlay()` on the worker thread, after the modules are set up. Either one can fail on its own; the failing one logs the reason and returns `false`. The overlay is the guaranteed way to change settings.

Both parts draw whatever is registered in `core::settings`, grouped by module (in registration order) and then by `Def::group`. Neither hardcodes any module's settings. Values are written only with `settings::Set`, never into the game's save or options file. `settings_view.cpp` holds the code both parts share: grouping, snapping to `Def::step`, formatting, key names, module status lines, and the "Saved" bookkeeping.

The UI registers three settings of its own under module `ui`:

| key | kind | default | label |
|---|---|---|---|
| `overlay_hotkey` | Key | `VK_F8` | "Overlay hotkey" |
| `overlay_hint` | Bool | on | "Show overlay hint at startup" (shows "Press F8 for mod settings" top left for 10 s after the first frame) |
| `overlay_theme` | Enum | Dark | "Overlay theme": Dark or Light (also the Dark/Light tabs at the bottom of the overlay's sidebar) |

The UI never draws an automatic `ui.enabled` toggle, because turning the UI off would lock you out of it.

## Files

| file | purpose |
|---|---|
| `ui.h` | `StartOptionsTab()` and `StartOverlay()` |
| `overlay.cpp/.h` | Dear ImGui on the D3D11 swap chain: hooks, input, key capture, back-buffer snapshot |
| `menu.cpp/.h` | the overlay window: sidebar, tabs, setting cards, Profiles and Settings pages, theme peel (no Win32/D3D) |
| `menu_layout.cpp/.h` | which category and tab each module is on, nested options, short dropdown labels and descriptions |
| `theme.cpp/.h` | the Slime Rancher 2 look: Dark and Light palettes, style metrics, embedded fonts |
| `slime_icons.cpp/.h` | sidebar logo: the game's own slime icons, a different one every time the overlay opens |
| `fonts/` | Nunito and Fredoka (SIL OFL) as compressed arrays, JetBrains Mono (SIL OFL, ASCII, 9 KB) for inline code, with their licences and how they were made |
| `game_input.cpp/.h` | gameplay input lock and cursor unlock while the overlay is open (main thread) |
| `hud_edit.cpp/.h` | HUD edit mode: move and resize the minimap and the Speed Panel with the mouse (see below) |
| `momentum_hud.cpp/.h` | the Speed Panel (`hl2_movement`'s speed readout), drawn by the overlay |
| `options_tab.cpp/.h` | "Mods" category in the game's Options menu |
| `mod_icons.cpp/.h` | the mod's own icons (`assets/icons/`, embedded as `mod_icons_data.inc`) as Unity sprites: stb_image decode, `Texture2D` + `Sprite.Create`, cached and pinned; used by the Mods tab and the custom plot upgrades, with the game's sprite as fallback |
| `settings_view.cpp/.h` | shared helpers (see above) |
| `quick_start_hint.cpp/.h` | "Hold Shift to stay in the main menu" note: `quick_start` publishes its pending auto-continue (and countdown) from the main thread through atomics; `menu::DrawHint` stacks it under the hotkey note. Shown whenever auto-continue is pending, independent of `overlay_hint`; after a Shift cancel it says "Staying in the main menu" for 2.5 s |
| `sandbox_bridge.cpp/.h` | Misc > Sandbox: the `sandbox` module publishes a snapshot (Newbucks, health/energy/radiation, vacpack slots, item catalog, spawn list, fabricator recipes) from the main thread; the tab reads only that copy and posts `Action`s, which `main_thread::Post` hands to the module's handler. Item ids are opaque to the UI |
| `item_icons.cpp/.h` | game sprites (item icons) for ImGui: `Register(sprite)` on the main thread reads the texture's D3D11 resource and the sprite rectangle once (sprite and texture kept alive by GC handles, cached per sprite) and returns a small id; `Draw(id, ...)` on the render thread samples it through a shader resource view made once per texture, with the same sRGB gamma shader as the slime logo. No per-frame game calls |
| `item_grid.cpp/.h` | shared item grid: search box plus a clipped, scrolling grid of icon tiles (dark tiles, category accent along the bottom, hover and selection outlines, optional badge, dimmed state); names on hover through the caller's tooltip; tiles can be drag sources (`kDragType`, payload = the opaque id). `DrawTile` draws a single tile (the vacpack slots use it) |
| `il2cpp_util.cpp/.h` | by-name IL2CPP helpers: invoke, unbox, object_new, GC handles |
| `../third_party/stb/` | `stb_image.h` v2.30 (nothings/stb, MIT / public domain), PNG decoder only, compiled into `mod_icons.cpp` |
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
- It creates the ImGui context with `IniFilename = nullptr`, so no `imgui.ini` is written. It loads the embedded Nunito Bold (body) and Fredoka SemiBold (headings) and merges Segoe UI into both, if it is installed, for characters outside the embedded subset (`fonts/README.md`).
- It runs `ImGui_ImplWin32_Init` and `ImGui_ImplDX11_Init`, hands the device to `slime_icons`, and subclasses the window with `SetWindowLongPtrW(GWLP_WNDPROC)`.
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
- Leak while open: mouse 4/5 messages are in the `WM_MOUSE*` range and are swallowed over the overlay like every other mouse message (so no `WM_APPCOMMAND` "browser back" is generated there); outside the overlay they reach the game (see Blocking game input). Modules polling `GetAsyncKeyState` still see them, exactly as for keyboard keys (see Remaining leak below).

**Window** (`menu.cpp`). A 900×600 window (times the scale, centred the first time) without a title bar; drag it by any empty spot. Everything is built each frame from `BuildModuleViews()` and arranged by `menu_layout`.
- **Sidebar:** a big slime logo (see below), the categories (Movement, World, HUD, Ranch, Misc), then Profiles and Settings at the bottom, the Dark/Light tabs, and a footer line: a save-state dot (amber for 1.5 s after a change, then green, red when the last write of `sr2mods.json` failed per `settings::LastSaveError()`; the tooltip says it in words, with the error) followed by the DLL's build stamp `dd/MM HH:mm · abc1234` in small text, grayer than the other dim text (dim mixed 40 % towards the sidebar colour), the digits' ink centred on the dot (the commit a size smaller; if the line would overflow the sidebar it shrinks further, then shows `-dirty` as `*`; tooltip: only "Click to copy the commit", or "... the build time" without a commit). Clicking the stamp copies the full short commit, with `-dirty` when present (the ISO build time when there is no commit), to the Windows clipboard through `ImGui::SetClipboardText` (ImGui's default Win32 handler), and the tooltip reads just "copied!" for 1.5 s. `settings::Flush()` runs when the overlay closes.
- **Build stamp:** `cmake/build_info.cmake` runs on every build (always-out-of-date target `sr2mods_build_info`) and writes `build/<Config>/generated/sr2mods_build_info.h` (local build time, `git rev-parse --short HEAD` plus `-dirty`, empty without git) only when its text changes; `core/build_info.cpp` is the only includer, so a new minute recompiles one small file and relinks. The startup log line uses the same stamp.
- **Tab bar:** each category has at least one tab, also when it is the only one. On the right: reset (tab back to defaults, switches excluded; on Profiles it rescans the folder), the tab's **Enabled** switch (the module's `enabled`, or the tab's master key, see Speed Panel) and close.
- **Cards:** each `Def::group` is a card ("" = no heading), placed in two columns, shorter column first. A row is the label on the left and the control on the right: Bool → switch, Float/Int → slider snapped to `step` (Ctrl+click to type), Enum → dropdown, Key → capture button. A Bool's label is dimmed while it is off. A Bool row is clickable as a whole (label and gap included): an invisible button sized like the row last frame sits under it with `SetNextItemAllowOverlap`, so the switch on top keeps its own hover and click and one click never toggles twice; the dimmed label brightens while the row is hovered. Tooltips come from `Def::tooltip`.
- **Switched off:** with its switch off, a tab still shows its cards, grayed out and read-only (`BeginDisabled`), and the tab label is dimmed; the tab bar's Enabled switch stays usable. A master tab whose module is off (Speed Panel while Hl2 Movement is off) adds a one-line note saying which module to turn on. The same goes for a nested module (Collectibles under Minimap: its own switch stays usable) and for the cards on Misc > General (header switch usable, rows grayed). The in-game Options tab still hides a module's rows while it is off.
- **Nested options:** settings that only matter while another one is on are drawn under it, indented on a thread line, and slide open or closed with it (0.22 s, smoothstep). `DrawKids` draws the block in the card itself, not in a child window: while sliding, the rows are shifted up by the hidden part and clipped (`PushClipRect`, which also stops hover/clicks) to the visible part, the layout advances by exactly the visible part including the trailing item spacing, and the rows fade with the slide instead of snapping to the disabled dim; a block that has never been measured (first open of one that started closed) is drawn parked far above, fully clipped, for one frame. The block's height is re-measured every frame it is drawn. The parent comes from the settings schema (`settings::Def::parent` / `parentValue`, set by each module, see below); a setting is nested when its parent is a top-level row of the same card, otherwise it stays where it is and is hidden while its parent chain does not allow it. A card whose rows are all hidden is left out. Their labels drop the parent's name ("Autobhop hold key" under "Autobhop" reads "Hold key").
- **Slider units:** `settings::Def::unit` is drawn after the number ("%", " m", " s", "x", " u/s", "°"), `Def::zeroText` replaces it at 0 ("auto", "instant", "free"), and `Def::displayScale` scales the shown value (opacity 0.2-1 shows as 20-100%). The overlay slider runs on the displayed value; Ctrl+click types the bare number. `FormatValue` applies the same rules, so the Options tab's stepper text matches. Modules whose registration helper takes no unit call `settings::SetDisplay` (a later `Register` without a display keeps it). Units moved out of labels: "Size (%)" is now "Size" with "%", "Jump height (m)" is "Jump height" with " m", and so on; keys are unchanged.
- **Inline code:** console names (`IsConsoleName`: `sv_`, `hl2_`, `cl_` or `mp_` followed by lower-case letters, digits and `_`) are drawn as monospace text (JetBrains Mono, `theme::Mono()`) on a small rounded chip in the frame colour: as a whole row label (HL2 Movement's `sv_accelerate` ... `sv_maxvelocity`) and inside tooltips (`RichText`, a word-by-word wrapper used only when the text has such a name; trailing punctuation and brackets stay outside the chip). The Options tab shows them as plain text.
- **Dropdowns** show a short label; the open list shows each option with a one-line description under it (`layout::EnumTexts`: a table, else options written "Label (description)" are split).
- **Banners** at the top of the scroll area: a version-guard warning when `version_guard::Mismatch()`, and a module's FAILED status.
- **Scroll memory:** each page (category + tab) keeps its scroll position for the session (`g_pageScroll`, in memory only), across page switches and closing / reopening the overlay. A restore is re-applied for up to 4 frames, because `SetScrollY` is clamped to the previous frame's content height; while no restore is pending, the current position is recorded every frame.
- **Settings page:** the `ui` settings (hotkey, startup hint) and a status card with the Options tab status and the game build. No "Overlay" row (you are looking at it). The game build reads "matches known build" in green while `GameAssembly.dll` matches the known hash and nothing mismatches, with the full `version_guard::Status()` as its hover tooltip; otherwise the full status is shown, in the warning colour on a mismatch. In the status card the labels sit on the left and the values are right-aligned to the card's edge (`KeyValue`); a long value wraps with every line right-aligned.

**Layout** (`menu_layout.cpp`). Presentation only: modules, keys and `sr2mods.json` do not change.

| category | tabs |
|---|---|
| Movement | Hl2 Movement (without its HUD group) |
| World | Spawn Scaling, Wall Markers |
| HUD | Minimap (Collectibles below it, with its own switch: the tracker is drawn under the minimap and reads its map), Speed Panel |
| Ranch | General: Custom Upgrades on the left; Plot Upgrades and Sell Upgrades on the right, one card each like Misc > General. `Tab::cardHelp` gives each card's switch its own tooltip (what the module does) |
| Misc | General: Quick Start on the left; Instant Build and Quick Travel on the right, one card each with the module switch in the card header. Sandbox: its own page (see below) |
| Profiles, Settings | one tab each |

- **Sandbox** (`TabKind::Sandbox`, module `sandbox`, default off) draws its own page instead of setting cards. Off: a card with an explanation and an **Enable sandbox** button. The button and the tab bar's Enabled switch (only when turning on) open a modal, "Enable sandbox?" / "Sandbox edits change your save directly. Enable?" with Cancel / Enable (Esc cancels; Cancel comes first, so keyboard focus never starts on Enable); only Enable writes `sandbox.enabled`. Turning it off is the plain switch. On: Newbucks (amount, input, Set / Add / Subtract) and Player (health, energy, radiation with Refill / Clear, an infinite-energy switch) side by side; Vacpack across both columns (the slots as tiles in a row with icon, count and slot number; the selected slot's name, an amount slider and Set / Fill / Empty; below, an item grid of what the selected slot accepts: click an item to fill the selected slot, or drag it onto any slot to fill that one); then Spawner (kind chips, a count, a grid of spawnable items; click spawns in front of you) and Crafter (fabricator category chips, a count, a grid of gadget recipes with the owned count as badge, locked blueprints dimmed; click crafts for free) side by side; a status line with the last result. All three grids are `item_grid` with their own search text; icons come from `item_icons` ids in the snapshot, so drawing them makes no game calls. The page reads `sandbox_bridge::Get()` only and marks itself viewed each frame, which makes the module refresh the snapshot at 4 Hz. The Options "Mods" tab has no Sandbox row and profiles skip the module, so nothing else turns it on. Details: `modules/sandbox/NOTES.md`.
- **Locked switches.** A module can pin a setting with `settings::Lock(module, key, value, reason)`; while pinned, `Set` with another value is ignored (overlay, Options tab, profiles). The overlay draws a pinned `enabled` switch (card header or tab bar) disabled, with the reason as its tooltip (`settings::LockReason`). Used by Custom Upgrades while a save with custom upgrades is loaded (`re/custom-upgrades.md` 1.2b). The Options "Mods" tab does not show the lock: its row can read "Off" after a click until it is rebound, while the value stays on.
- **Speed Panel** is `hl2_movement`'s HUD group. Its switch is `hl2_movement.momentum_hud`, it is locked while Hl2 Movement is off, and a (?) next to it says so.
- **Position and size** of the minimap and the Speed Panel are not sliders: their placement keys show as one row, "Position and size" with a **Move** button, which starts the HUD edit mode (below). `hud_edit::MoveRow` names the row (the x key), `hud_edit::Hidden` the other two. The pages keep only the style settings (minimap: shape, opacity; Speed Panel: details, fade, bar). The game's Options "Mods" tab still shows the placement keys as sliders.
- **(?) markers** come from one helper, `HelpMarker`: centre snapped to the pixel grid, a filled disc with a 1.5 px ring (48 segments), the "?" in the display font at about 62 % of the marker size, its ink centred and placed on whole pixels (it was thin and blurry with the body font at a fractional position).
- A module the layout does not list gets a card on Misc > General, so new modules show up without touching the UI. A setting the layout does not list stays in its module's tab.

**Theme** (`theme.cpp`). Flat Slime Rancher 2 colours: Dark (plum panels, slime pink, sky-blue selection) and Light (cream panels, white cards, the same accents). `menu::SyncTheme` applies the style when the `overlay_theme` value or the scale changes; scale = client height / 1080, at least 1.

**Theme peel.** Switching Dark/Light peels the old look off the window from the bottom-right corner (0.85 s). The click marks the frame; after rendering it (still the old theme) `overlay.cpp` copies the back buffer into a texture of its own (`CopyResource`, recreated when the size or format changes), and the next frames draw that copy over the window, cut along a moving diagonal, with a pale flap and its shadow on the edge. The copy and the flap are clipped to the window's rounded outline (`RoundedRect`, the style's window rounding), so the game pixels in the snapshot's corners never show as square corners at the end. They are drawn at the window's current position and sampled at the position the snapshot was taken, so the peel follows the window when it is dragged mid-animation. A multisampled back buffer or a failed copy just switches without the animation.

**Slime logo** (`slime_icons.cpp`). The logo is a slime from the Slimepedia that the loaded save has discovered; symbols in `re/symbols.md`, "UI: Slimepedia".
- **On every open** a job is posted to the main thread (`Refresh()`). It gets `SceneContext.Instance` (`SRSingleton<SceneContext>.get_Instance`), requires its `GameModel` (null on the title screen) and its `PediaDirector` with a `_pediaModel` whose `unlocked` set exists. The slime entries are the `IdentifiablePediaEntry` items of the director's `PediaConfiguration._categoryList` categories (`PediaCategory._items`) whose `_identifiableType` is a non-largo `SlimeDefinition`; only if that finds none, every loaded `IdentifiablePediaEntry` (`Resources.FindObjectsOfTypeAll`). Each is checked with `PediaDirector.IsUnlocked(PediaEntry)` (resolved by parameter type; the `IdentifiableType` overload has the same name and argc). Discovery is re-read on every open because it changes during play and with the loaded save.
- **The pick:** the discovered entries are shuffled with the last pick moved to the back, so it never repeats while there are two or more. The first one whose icon can be read wins. The icon is the entry's `get_Icon()` (resolved on the object's class, it is virtual), else the type's `icon` field. The rect is `Sprite.textureRect`, or, for tightly packed sprites (`packed` and `packingMode == Tight`, where `textureRect` throws), the bounds of `Sprite.uv`. The texture is kept alive with a GC handle (once per texture). Its D3D11 resource comes from the engine icall `UnityEngine.Texture::GetNativeTexturePtr_Injected` (`il2cpp_resolve_icall`, called with the texture's `m_CachedPtr` under SEH; a fault puts the UI into fault mode): the managed `Texture.GetNativeTexturePtr()` is stripped from this build, which is why the logo was always the fallback up to this fix.
- **Text:** the entry's `_title` (else the type's `localizedName`, else the asset name), its `_description` as an untitled intro, then each `PediaEntryDetail` in order: `Section._title` and `Text`. Each `LocalizedString` is read with `GetLocalizedString()` after `IsEmpty`; rich-text tags are stripped. Controller variants (`TextGamepad`, `TextPS4`) and sub-contents are not shown.
- **Threading:** the job copies everything into a `Snapshot` (UTF-8 strings, native texture, rect, texture size) under a mutex; the render thread applies a newer snapshot in `Draw()`/`OnOpen()` and keeps one shader resource view per native texture for later picks (typeless formats as UNORM; for typed sRGB formats a small copy of ImGui's pixel shader puts the sRGB curve back, set through an `ImDrawList` callback). Between an open and the new snapshot (a frame or two, at most 0.5 s) the logo area stays empty, so neither the previous slime nor the fallback flashes.
- **Hover** shows `EntryTooltip` (`menu.cpp`): the name in the display font, then each section's title and text, wrapped at 360 px (1080p).
- **Default (Pink):** when no save is loaded (title screen, initial load), nothing is discovered, or no discovered entry's icon can be read, the logo is the game's own Pink Slime icon: the loaded `SlimeDefinition` whose asset name is `Pink` (`Resources.FindObjectsOfTypeAll`, non-largo), its `IdentifiableType.icon` sprite, read like any other pick. The slime definitions are loaded by the game from the start, so this also works on the title screen. No hover tooltip in this state (the snapshot carries no entry).
- **Fallback:** the menu's own drawn pink slime (`DrawSlimeFallback`) only as the last resort: the Pink definition or its icon can't be read (logged once), the main-thread dispatcher is not running, the UI is in fault mode, or a required class/field/method is missing (logged once). No hover tooltip then.
- **Unverified in game:** the vertical UV orientation assumes Unity's bottom-up texture rows on D3D11 (if the logo is upside down, swap `uv0.y`/`uv1.y` in `Sync()`); the UV path for tightly packed sprites; that the category walk finds every slime (the `FindObjectsOfTypeAll` path only runs if it finds none).

**Profiles page.** It uses `core/profiles.h`; the file format and rules are in the README.
- The left card lists the profiles. The active one has an ACTIVE badge, MODIFIED when the values differ from its file (`profiles::IsModified()` is cached for 250 ms, so it is cheap to call every frame), broken files a BROKEN badge. Hovering shows the description or the parse error; double-click loads. The selection defaults to the active profile.
- **Load** calls `profiles::Load`. From the render thread this posts the apply to the main thread, the same model as `Write`, and the status line shows "Loading..." and then the result.
- **Save** overwrites the active profile after a confirmation modal and keeps its description. **Save as...** opens a modal with a name and an optional description. If the name exists, the button becomes "Overwrite"; Enter never confirms an overwrite. **Rename** opens a modal; a clash also needs "Overwrite". **Delete** asks in a modal.
- **Open folder** runs `ShellExecuteW` on a short-lived thread (shell32 is delay-loaded), so the render thread never waits on the shell. If that fails it starts `explorer.exe` with `CreateProcessW`. The tab bar's refresh icon rescans the folder; opening the overlay rescans it too.
- File operations (save, rename, delete, rescan) run on the render thread when clicked. The files are small, so the cost is one frame at most.
- `StartOverlay` calls `profiles::Init()`, which creates the folder and the starter profiles after the modules have registered their settings.

**Text input.** The WndProc queues `WM_KEYDOWN`/`WM_KEYUP`/`WM_CHAR` (the `WM_KEYFIRST..WM_KEYLAST` range) for ImGui and swallows them, so typed text never reaches the game's window procedure. `WM_INPUT` only gets `DefWindowProc`, and gameplay actions are locked through the Input System map (see below). Name fields filter characters to what `SanitizeName` keeps. While an ImGui text field has focus (`io.WantTextInput` from the previous frame), a hotkey that produces a character (`MapVirtualKeyW(..., MAPVK_VK_TO_CHAR) != 0`) does not toggle the overlay, so typing that letter does not close it. Esc closes the profile modals. **Remaining leak:** modules that poll keys themselves with `GetAsyncKeyState` (minimap zoom key, hl2_movement keys) still see keys typed into the overlay. They would need to skip polling while `sr2::ui::overlay::IsOpen()`.

**Blocking game input while the overlay is open:**
1. The WndProc swallows `WM_KEY*`/`WM_CHAR`, and `WM_MOUSE*` while ImGui wants the mouse. `WM_INPUT` from the keyboard, and from the mouse while ImGui wants it, gets only `DefWindowProcW`. `WM_SYSKEY*` still goes to the game, so Alt+F4 and Alt+Enter keep working.
   - **Mouse outside the overlay goes to the game.** `io.WantCaptureMouse` of the last overlay frame (pointer over an overlay window, an ImGui drag, a modal; forced on for the first two frames after opening) is kept in an atomic. While it is false, mouse messages and raw mouse input pass on to the game, so the game's own menus next to the overlay window (Slimepedia, Options) stay clickable; gameplay actions stay locked by step 2. A button whose press went to the game also gets its release, wherever it happens. Before (first test build), every mouse message was swallowed while the overlay was open, but part of the input still reached Unity outside the window procedure, so clicks into the Slimepedia beside the open overlay only worked after several tries (o1 test: the log shows overlay frames being drawn for the whole Slimepedia session).
2. SR2 uses the Unity Input System. On every main-thread tick (`main_thread::AddFrameCallback("ui.overlay")`), `game_input.cpp` finds `MonomiPark.SlimeRancher.Input.InputDirector` and gets its `_mainGame` map through `InputActionMapReference.op_Implicit`. If that map is enabled, it calls `InputActionMap.Disable()`. On close it calls `Enable()` again, but only if the overlay disabled it, and it re-asserts the disable each tick, because the game may re-enable maps itself.
3. `UnityEngine.Cursor.lockState` is set to `None` while the overlay is open and restored on close. If the game had the cursor locked, which also hides it, ImGui draws a software cursor. It also draws one if the dispatcher or the Cursor API is not available.

**Overlay limitations:**
- If the main-thread dispatcher is not running, only the window-message filter applies, so gameplay actions polled through the Input System may still fire.
- If Unity reads keyboard or mouse through a path that bypasses the window procedure, step 1 cannot stop it. Step 2 is the real lock.
- ImGui gamepad navigation is off; use mouse or keyboard.

## HUD edit mode (`hud_edit.cpp`)

One edit mode for the HUD elements, `hud_edit::Element::Minimap` and `::SpeedPanel`. Each element is a table row in `hud_edit.cpp`: module, the three placement keys and the size range.

| element | keys | size |
|---|---|---|
| minimap | `minimap.pos_x`, `pos_y`, `size` | edge length, 10-50 % of the screen height |
| Speed Panel | `hl2_movement.momentum_hud_x`, `_y`, `_size` | the speed number's font size, 0.8-8 % of the screen height |

**Placement.** x and y are percent of the free space: 0 = the element touches the left / top edge, 100 = the right / bottom edge, where the free space is the screen size minus the element size (`hud_edit::ToPx` / `ToPct`, clamped to 0..100). The size is percent of the screen height. All of it is resolution independent: the element keeps its place relative to the edges when the resolution or the aspect changes.
- The minimap sets its RectTransform's anchors and pivot to the same point (x/100, 1 - y/100) with a zero anchored position: the left edge then lies at x % of (parent width - size), which is exactly the free-space percent, without knowing the canvas width.
- The Speed Panel's box is `kBoxW` (3.2) font sizes wide and `kLine` (1.2) high plus `kBarH` (0.25) for the bar and `kDetailsH` (0.9) for the details line (`momentum_hud.h`). Each text is centred on its own line, so the box does not depend on font metrics; the content is centred horizontally in the box. Turning the bar or details on makes the box taller, so the panel moves a little to keep its percent.

**Starting.** The Move button calls `hud_edit::Begin(element)`; the overlay sees `Active()` after `menu::Draw` and closes the window. Edit mode keeps the input like the open window: `overlay::IsOpen()` (and the internal `Captured()`) is true for both, so the WndProc queues and swallows keyboard and mouse (no mouse message or raw mouse input goes to the game, the camera stays still), `game_input` keeps the action map disabled and the cursor unlocked, the software cursor is drawn if the game had the cursor locked, and modules that skip their key polling while the overlay is open (minimap zoom key, hl2_movement keys, wall markers) keep doing so.

**Controls** (render thread, `hud_edit::Draw`, after the HUD content, on the foreground draw list):
- The element gets a long-dashed outline 6 px (at 1080p) outside it, with a dark shadow, and a square handle on each corner. A help line sits at the top centre (at the bottom while the element is up there).
- Drag inside the outline: move. When the element's centre comes within 12 px of the horizontal screen centre it snaps there; while it is centred a long-dashed vertical guide runs through the screen centre.
- Shift while moving: the nearest corner is picked by the element's centre. If the drag is mostly horizontal (since the press), the side gap is set equal to the top/bottom gap of that corner; if mostly vertical, the top/bottom gap is set equal to the side gap. Either way the element ends up equally far from both edges of its corner, and the other axis follows the mouse.
- Drag a corner handle: resize with the opposite corner fixed; with Alt, around the centre. The size follows the larger of the two axes (both elements keep their aspect), clamped to the element's range and to the screen; the result is pushed back onto the screen if it would stick out.
- Enter or a click outside the outline keeps the change: the three values go through `Write` (posted to the main thread, snapped to the 0.01 step). Esc cancels (nothing is written). The overlay hotkey cancels too. Either way the settings window opens again on the page it came from.

**Live preview.** `hud_edit::Preview(element, out)` (any thread, mutex) returns the placement being dragged; after a keep it keeps returning it until the stored settings match (or 1.5 s), so the element does not jump back for the frames the posted writes take. The minimap compares it with what it applied last on every tick and re-runs `ApplyLayout` when it differs; while it is the one being edited, its "hide while a menu is open" rule ignores the cursor unlock. The Speed Panel is drawn while edited even without a fresh state (a sample speed of 320, details "320  80%" when on), at full opacity.

**Migration.** Done once, only when the new keys have never been saved (`settings::Saved`) and at least one old key has.
- Speed Panel (at registration): `momentum_hud_y_offset` (fraction of the height below the centre) and `momentum_hud_scale` (x the overlay's 15 px body font x 1.15 at 1080p) become x = 50, size = 1.597 % x scale, and the same top edge in percent of the free space.
- Minimap (at its first layout, because it needs the canvas aspect): `position` (corner) with `offset_x` / `offset_y` (% of the height) become the same left/top edge in percent of the free space. A placement made before that (edit mode, profile) wins.
The old keys stay in `sr2mods.json` but are no longer registered or used.

**Unverified in game:** everything above (built, not run): the minimap's anchor/pivot placement on the HUD canvas, the input capture during edit mode (camera still, Esc not opening the pause menu), Alt-resize (Alt is a system key; `GetAsyncKeyState` is used for Shift and Alt), and that the migrations land where the old settings put the elements.

## Options tab (`options_tab.cpp`)

**Approach: inject data, not cloned UI.** The Options menu is data-driven: `OptionsConfiguration` → `OptionsItemCategory` (one tab) → `OptionsItemDefinition` (one row, drawn with its `_controlPrefab`). Injecting data objects lets the game's own adapters build the tab strip entry and the rows. That gives the native look, the game's selection, details panel and gamepad navigation, without any GameObject surgery or navigation rebuilding. Cloning the UI after binding would need the tab strip's view models and row view holders rebuilt by hand, and those are bound to model objects anyway.

All lookups are by name: classes, fields, and methods (including the hook targets). Before installing, `StartOptionsTab` checks that no two hook targets share an address, which would mean IL2CPP folded the methods.

| hook | role |
|---|---|
| `OptionsUIRoot.BindCategories` | **Before the original runs:** pick templates from the passed `OptionsConfiguration`: the first non-conditional, platform-agnostic `ToggleOptionDefinition` and `ScriptedValueRangeOptionDefinition`, plus a category. Then build or update one cloned definition per setting, and append the Mods category to the configuration's `items` list. **After the original runs:** remove it from the list again, so the game's asset is only changed for the duration of the call. |
| `OptionsViewModel.GetOptionsItemsForCategory` | For the Mods category, returns an `OptionsItemModel[]` of models built with `il2cpp_object_new` plus the `PresetOptionsItemModel(definition)` constructor. This is what `CreateOptionItemModel()` does, minus adding the model to the definition's `_optionsItemModels`, which a runtime clone may lack. The models are never added to the game's `OptionsModel`, so nothing reaches `OptionsV02` or the game's settings file. The model constructor picks the current value through `GetDefaultPresetIndex()`, because `_defaultValue` is refreshed from the setting before each bind. |
| `ToggleOptionDefinition.ApplyPresetSelection`, `ScriptedValueRangeOptionDefinition.ApplyPresetSelection` | For our definitions, the original never runs. The value is computed with the game's own mapping and written with `settings::Set`. Toggle "on" is the preset whose label or reference id is `_positiveOption`. Range uses `GetValueForIndex`. |
| `CategoryTabViewHolder.BindData`, `OptionsItemViewHolder.BindData`, `OptionsPresetSelector.OnSelectionChanged`, `OptionsDetailsView.BindData`, `LocalizeStringEvent.UpdateString` | Localization bypass. The BindData hooks register the view's `LocalizeStringEvent` as ours, together with our text, keyed by the object and its `m_CachedPtr`, and release it on the next rebind. The hook on `UpdateString(string)`, the component's only text sink, substitutes our text for the components we own, so paged or recycled row views, deferred async localization and locale changes all show our label. Game components pass through untouched; the components are never disabled. Texts: tab "Mods"; label = module name for the master toggle, otherwise the `Def` label (indented); value "On"/"Off" or the formatted number or enum label; details = tooltip, plus module status, plus version warning. |
| `OptionsItemViewHolder.Activate` (optional) | A click on a row, or Submit on it, calls `Activate`, which only acts on "activatable" controls; a toggle's preset selector is not one, so the click did nothing. For our toggle rows the detour (after the original no-op) steps the selector to the other value with the row's own `NextSetting`/`PreviousSetting`, the same path as the arrows (`SelectValue` -> apply immediately -> our `ApplyPresetSelection` hook). The first such toggle is logged ("a row click toggled ..."). **Unverified in game:** that the row button's click (a serialized UnityEvent; `Activate` has no native callers) really is `Activate`. If it is not, nothing changes. |

**Cloned definitions.** Each cloned definition (`Object.Instantiate` of the template):
- gets a unique `_referenceId` (`sr2mods.<module>.<key>`) and a private clone of the template's `ScriptedBool`/`ScriptedFloat`, so the template's real setting can never be written;
- has `_applyImmediately` on and `_requireConfirmation`, `_showCondition` and `_normalizeOutputValue` cleared;
- is flagged `DontUnloadUnusedAsset` and pinned with a GC handle.

**Mapping settings to rows.**
- Floats, Ints and Enums map onto a stepper with `min`/`max`/`step`. Enums use 0..n-1 with wrap-around. The step is made coarser, staying on the `Def`'s grid, if the stepper would have more than 201 positions.
- Key settings cannot be shown as a stepper, so they stay overlay-only.

**Child rows (`settings::Def::parent`).** `BuildModels` lists only rows for which `settings::Visible` is true: the parent chain allows them and the module's `enabled` is on (the master row itself is always listed). Hidden rows keep their cloned definition and their `g_rowByDef` entry, so an Apply or a recycled view that still refers to one is still recognised as ours (the original `ApplyPresetSelection` never runs for our definitions). When the value of a row that other rows depend on changes (`Row::isParent`: a module's `enabled` or any key named as a present row's parent), `ScheduleRefresh` posts one main-thread job. The value is already written at that point: our definitions have `_applyImmediately` set, so the selector's `PresetOptionsItemModel.set_TempPresetIndex` calls `ApplyTemporaryValue` -> `ApplyValue` -> `ApplyPresetSelection` (our hook) in the same call; nothing waits for Apply. That job looks for the `OptionsUIRoot` that shows the Mods tab: `categories[currentCategoryIndex] == g_category`, or (fallback) the root's `optionsItemModels` are our models. It tries `FindFirstObjectByType` first and then every loaded instance with an active GameObject (`Resources.FindObjectsOfTypeAll`), because the first active instance is not always the one on screen; first test build: the toggle was written (module disabled/enabled) but no rows changed and nothing was logged, which only fits this lookup failing. If no root matches, one warning lists every loaded root (name, active, tab index, models; at most 5 per session). The job then calls `OptionsUIRoot.BindItemCategory(Mods)`, the game's own tab-swap path: `GetOptionsItemsForCategory` returns the new row set, the adapter rebinds (labels re-registered through the `BindData` hooks) and selects row 0. If the two root fields are missing, rows update the next time the tab is opened. A change made in the overlay while the Mods tab is open also shows at the next bind.

**Keeping the user's place across that rebind.** `BindItemCategory` calls `optionsAdapterView.BindDataList(models, 0)` and `BindOptionsDetailsView(0)`, so on its own the selection, the details panel and the scroll position all jump to row 0 (IDA: `BindItemCategory` 0x71A2F0; `AdapterView<…>.BindDataList` 0x1F281F0 ends with `currentlySelectedIndex = -1` and `SelectItem(initialSelection)`). The refresh job therefore:
1. saves the scroll offset of the list's scroll container: the nearest `MonomiPark.SlimeRancher.UI.Framework.CommonControls.ScrollArea` above the adapter's `container` (`_scrollPosition`, world units), or, as a fallback, a `UnityEngine.UI.ScrollRect` (`content.anchoredPosition`). The one found is logged once ("Mods list scrolls with ...");
2. rebinds as above;
3. finds the changed row (same setting key) in the root's new `optionsItemModels` and calls `AdapterView.SelectItem(int)` (0x1F29BC0, virtual, looked up on the adapter's runtime class). That is the adapter's own selection path: it moves the navigator (gamepad up/down continue from there), moves the row highlight and raises `OnItemSelect`, which the root answers with `BindOptionsDetailsView(index)`, so the details text goes through the `OptionsDetailsView.BindData` hook as usual;
4. moves the game's UI focus to the changed row, if the focus was inside the row list or already gone (see below);
5. puts the saved offset back (`ScrollArea.ScrollTo(offset, ScrollTransition 0)`, i.e. a jump that also stops a running animation; or `StopMovement` plus `set_anchoredPosition`). The absolute offset is right because rows above the changed one never change: child rows follow their parent.

Focus changes can scroll the list a frame later (`ScrollArea.HandleFocusChange` → `ScrollIntoView`, which only scrolls when the row is outside the viewport), so the job settles for 6 ticks, each a fresh lookup of the root: it re-applies the offset while the changed row is still selected, and puts the focus back on the selected row if it was lost. No object is held across ticks. If the adapter field, the selection API, the focus API or both scroll classes are missing, the refresh still works and only that part is skipped (logged once).

**UI focus and Back (test m2, menu could not be closed).** The game sends Cancel (Escape, gamepad B) to `FocusManager`'s focused GameObject and up its parents, and only while that object is alive (`FocusManager.SendCancelActionToFocus` 0x7939A0: `if (CurrentFocus) SRExecuteEvents.ExecuteCancelHierarchy(CurrentFocus)`); `OptionsUIRoot.OnCancel` → `Close` is therefore reached only while the focus is somewhere inside the Options screen. Clicking a row's value arrows focuses that row's value control. The rebind rebinds every row view, and `OptionsItemViewHolder.BindData` (0x718690) destroys the old control (`Destroyer.Destroy`) before creating a new one, so after toggling a parent row with the mouse the focus sat on a destroyed object, Cancel went nowhere, and the Options menu stayed open until the game was restarted. `FocusManager.Update` never repairs a dead focus. A vanilla tab swap does the same rebind but never hits this, because the focus is then on the tab strip. The refresh job therefore checks `FocusManager.get_CurrentFocus()` before the rebind; if it is under the adapter (or dead), it calls `FocusManager.SetFocus(GameObject, InteractionCause.PROGRAMMATIC)` on the changed row's `ButtonViewHolder.button` GameObject (else the row view itself; the row views are rebound, never destroyed) and checks that the focus landed inside the root. While settling, a focus that is dead, or deactivated inside the root, is put back on the selected row. A focus outside the Options screen is never touched.

**No save/revert prompt for Mods rows.** `OptionsUIRoot.Close` (0x71A860) and `CheckThenSwapCategory` go through `SaveOrRevert` (0x71C720), which shows a save-or-revert popup when any model in `optionsItemModels` has `HasUnsavedSelection()` (`CurrentPresetIndex != _tempPresetIndex`; `CurrentPresetIndex` comes from the model's saved preset key, which only `SaveSelectedValue` → `set_CurrentPresetIndex` moves). Our rows apply and save on every change, so that prompt asked about values already saved, and its Revert wrote the old values back through our hook (queueing a rebind) while the menu closed. After every change of one of our rows a posted job (`CommitModsSelections`) finds the root that shows the Mods tab and, for each of our models with an unsaved selection, calls `set_CurrentPresetIndex(TempPresetIndex)` (0xAC8360: sets the key and raises `OnSelectionChanged`, nothing else). Game rows are not touched, so the game's own Apply/Revert behaviour is unchanged.

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
- The Mods tab takes the icon of a "control/input/gameplay/accessibility/other" category, falling back to the About icon. Since the custom icons, it shows `assets/icons/mods_tab.png` (`mod_icons::Get("mods_tab", stock)`); the stock icon is only the fallback and the size reference.
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
