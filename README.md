# sr2mods

A native C++ mod suite for Slime Rancher 2 (PC, Unity 6000.0.73f1, IL2CPP, game v1.3.0).

The DLL loads itself. It does not use MelonLoader, BepInEx, Doorstop, SRML, Harmony, CoreCLR or any other managed code. It is a `version.dll` proxy: `UnityPlayer.dll` imports `VERSION.dll`, and Windows loads the copy in the game folder before the one in System32. The only third-party code is [MinHook](https://github.com/TsudaKageyu/minhook) and, for the overlay, [Dear ImGui](https://github.com/ocornut/imgui). Both are compiled in statically.

The core provides a loader, name-based IL2CPP resolving, hooks, a Unity main-thread dispatcher, persistent runtime settings, Unity helpers and a version guard. The modules (`spawn_scaling`, `hl2_movement`, `minimap`) and the UI (in-game options tab and ImGui overlay) are built on top of it.

## Layout

```
CMakeLists.txt  build.ps1  build.bat  sr2mods.json  README.md
proxy/         version.dll export forwarding
  gen_proxy.py       generates version.def / version_stubs.asm / proxy_exports.inc
                     from the real C:\Windows\System32\version.dll (via pefile)
  verify_exports.py  checks the built DLL against System32\version.dll
  proxy.cpp          lazily loads <GetSystemDirectory>\version.dll by full path
  dllmain.cpp        DllMain: only spawns the worker thread
core/          worker thread, IL2CPP API + resolver, hook manager (MinHook),
               module manager, logging, JSON config, version guard,
  main_thread.h      Unity main-thread dispatcher (Post / per-frame callbacks)
  settings.h         runtime settings (registered by modules, shown by the UI)
  profiles.h         named settings profiles (profiles\<name>.json)
  unity.h            Unity helpers (strings, Type, Find*, components, arrays, List<T>)
  guard.h            Try(): C++/managed + SEH exception net for risky calls
  module_registry.cpp  the list of built-in module factories
ui/            options tab (ui/options_tab.cpp) and ImGui overlay (ui/overlay.cpp)
modules/<name>/module.cpp   one module each: spawn_scaling, hl2_movement, minimap
third_party/minhook/   official MinHook sources, master @ 8af6b4a (2026-09-08)
third_party/imgui/     Dear ImGui (optional; picked up by CMake if present)
```

## Build

You need Visual Studio 2022/2026 or the Build Tools, with the "Desktop development with C++" workload and the "C++ CMake tools" component. The build uses MSVC x64, C++20 and the static CRT (`/MT`).

```powershell
.\build.ps1                  # Release -> build\Release\version.dll, dist\Release\
.\build.ps1 -Config Debug    # Debug build; a log console opens if debug_console=true
.\build.ps1 -Clean           # delete the build directory first
.\build.ps1 -Regen           # regenerate the proxy export stubs (needs Python + pefile)
```

`build.ps1` finds MSVC with `vswhere`, imports `vcvars64`, then configures and builds with CMake and Ninja. CMake globs `modules/**` and `ui/**` (`CONFIGURE_DEPENDS`), so new files there are picked up without editing `CMakeLists.txt`. If `third_party/imgui/imgui.cpp` exists, ImGui and its DX11/Win32 backends are built as a static library (warnings off). Every system DLL other than `kernel32` and `user32` is delay-loaded (`bcrypt`, `d3d11`, `dxgi`, `d3dcompiler_47`, `dwmapi`, `shell32`, `imm32`). The proxy is loaded before the game initialises D3D, so it must not pull those DLLs in at load time. Release builds compile cleanly at `/W4`. When it finishes it runs `proxy/verify_exports.py`, which checks that the DLL is x64, that its export names and ordinals match System32's `version.dll`, and that it imports no CRT DLLs. This step needs Python and `pefile`.

## Install

The easy way is the deploy script. Close the game first.

```powershell
deploy.bat                     # deploy dist\Release\version.dll
.\deploy.ps1 -Build            # build first (build.ps1), deploy only if the build succeeds
.\deploy.ps1 -GameDir <dir>    # game folder override (default: Steam folder, then libraryfolders.vdf)
.\deploy.ps1 -WaitForExit      # wait for the game to close instead of refusing
.\deploy.ps1 -NoSaveBackup     # skip the save backup
.\deploy.ps1 -Uninstall        # remove version.dll (backed up first); sr2mods.json and sr2mods_saves\ stay
.\deploy.ps1 -Uninstall -Purge # also delete sr2mods.json, sr2mods.log, sr2mods.prev.log, sr2mods_saves\ (asks; -Force skips)
```

`deploy.ps1` finds the game folder (the default Steam path, otherwise the Steam library that holds app 1657630 according to `libraryfolders.vdf`; the `SR2MODS_GAMEDIR` environment variable or `-GameDir` override it). It refuses to run while `SlimeRancher2.exe` is running. Then it:

1. copies your saves (`%USERPROFILE%\AppData\LocalLow\MonomiPark\SlimeRancher2\Steam\<steamid>\`) to `save-backups\<yyyy-MM-dd_HHmmss>\` (git-ignored; with more than one Steam account, one subfolder per account),
2. copies the currently deployed `version.dll` to `dist\deployed-backup\version.<timestamp>.<sha8>.dll` (the last 5 are kept),
3. copies the new `version.dll` into the game folder, or skips the copy if the deployed one is identical,
4. copies the default `sr2mods.json` only if the game folder has none. Your live `sr2mods.json` is never overwritten.

Nothing else is written into the game folder. Exit code 0 means success.

### Auto-deploy on merge into develop

`githooks/post-merge` runs `deploy.ps1 -Build` after every local merge into `develop`, fast-forward pulls included (not after rebases or checkouts). Enable it once per checkout:

```powershell
.\githooks\install.ps1           # git config core.hooksPath githooks (local config, not committed)
.\githooks\install.ps1 -Remove   # turn it off again
```

- It runs only when the current branch is `develop` and only in the main checkout, never in a linked worktree.
- It never fails the merge. If the game is running, the build fails or anything else goes wrong, it prints a one-line warning and exits 0. Run `.\deploy.ps1 -Build` by hand afterwards.
- Its output is appended to `dist\deploy-hook.log`, with a timestamp and the merge commit.
- To skip it, set `SR2MODS_NO_DEPLOY=1` for one merge, or run `git config sr2mods.autodeploy false` to turn it off for the checkout.

To install by hand instead, back up your saves and copy two files from `dist\Release\` into the game folder, next to `SlimeRancher2.exe`:

- `version.dll`
- `sr2mods.json`

The default game folder is `C:\Program Files (x86)\Steam\steamapps\common\Slime Rancher 2\`.

Start the game. The log is written to `sr2mods.log` in the same folder. On each launch the previous log is renamed to `sr2mods.prev.log` (replacing the older one), so the log of a crashed run survives one restart.

### Matching a log to its PDB

Every DLL that `build.ps1` produces is archived with its `version.pdb` and `sr2mods.json` in `dist\archive\<yyyyMMdd-HHmmss>-<first 8 hex of SHA-256>\`, together with a `build-info.txt` that records the full SHA-256 and the PDB GUID and age. The last 20 archives are kept. At startup the log prints `self: PE timestamp 0x…, sha256 <first 16 hex>, pdb <GUID> age <n> (…)`. Find the archive folder whose name ends in the first 8 hex digits of that SHA-256.

## Uninstall

Run `.\deploy.ps1 -Uninstall` (add `-Purge` to also delete the settings, logs and `sr2mods_saves\`), or delete `version.dll`, `sr2mods.json`, `sr2mods.log` and `sr2mods.prev.log` from the game folder by hand. Nothing else is changed.

## Configuration (`sr2mods.json`)

The file is JSON, but `//` and `/* */` comments and trailing commas are allowed. If the file or a key is missing, the default is used.

| key | default | meaning |
|---|---|---|
| `log_level` | `info` | `trace`, `debug`, `info`, `warn`, `error` or `off` |
| `debug_console` | `true` | opens a console window (Debug builds only) |
| `startup_timeout_ms` | `300000` | how long to wait for GameAssembly.dll and IL2CPP to initialise |
| `settle_delay_ms` | `1500` | extra wait after IL2CPP looks ready, before attaching |
| `dump_assemblies` | `true` | logs every image and its class count |
| `version_guard_invoke` | `true` | calls `Application.unityVersion` and `Application.version` |
| `expected_game_version` / `expected_unity_version` | `1.3.0` / `6000.0.73f1` | a mismatch only logs a warning |
| `modules.<name>.enabled` | module default | turns one module on or off (also switchable at runtime) |
| `modules.<name>.<key>` | module default | one runtime setting of a module (see below) |
| `active_profile` | `""` | last loaded or saved profile; remembered only, never re-applied at startup (see [Profiles](#profiles)) |

### Runtime settings and saving

Modules register their settings with `sr2::core::settings::Register` (`core/settings.h`). The UI lists them with `settings::All()` and changes them with `settings::Set()`. A value is clamped to its definition, the module's `OnChange` callbacks are notified, and about 1 second after the last change a background thread writes it to `sr2mods.json` as `"modules": { "<module>": { "<key>": value } }`.

- **Comments are lost on the first save.** The writer re-reads the current file, sets the changed keys and rewrites the whole document as plain JSON (2-space indent, key order kept). All other keys survive, including unknown ones, but `//` and `/* */` comments and trailing commas do not. Keep a copy of the commented `sr2mods.json` if you want it.
- If the file on disk is not valid JSON at save time, it is left untouched and an error is logged.
- A value is written only if it was already in the file or was changed at runtime, so later changes to a module's defaults still take effect.
- The write is atomic (`sr2mods.json.tmp`, then rename).
- Every module automatically gets an `enabled` setting (label "Enabled", default `Module::DefaultEnabled()`). Toggling it calls `ModuleManager::SetEnabled`. A module that was disabled at startup runs `Setup()` the first time it is enabled.
- A setting can name a visibility parent in the same module (`Def::parent`, optional `parentValue` for an Enum index or 0 = "while off"; `settings::SetParent` sets it after registering). Both UIs hide it while the parent does not allow it, and hide every setting of a module except `enabled` while the module is off. `settings::Visible(module, key)` applies the rule.
- A setting can also list "any of these on" parents (`Def::anyOf`, or `settings::SetAnyParent` after registering): it shows only while at least one of them is on and itself shown. Spawn Scaling uses it for the "Per resource (advanced)" multipliers (any source in advanced mode) and for the ore amount multiplier and its cap (any source applied).
- Float/Int settings can carry a display unit (`Def::unit`, e.g. "%", " m", "x"), a text for 0 (`Def::zeroText`, e.g. "auto") and a display scale (`Def::displayScale`); `settings::SetDisplay` sets them after registering. They change only what the UIs show.

### Profiles

A profile is a named set of mod settings that you can create, switch between and share as a file. Profiles live in a `profiles` folder next to `version.dll`, one file each: `profiles\<name>.json`.

```json
{
  "description": "Twice the slimes, HL2 movement on",
  "modules": {
    "hl2_movement": { "accelerate": 10, "enabled": true },
    "spawn_scaling": { "enabled": true, "spawn_rate": 2 }
  }
}
```

- The file has the same shape as the `modules` section of `sr2mods.json`, plus an optional `description`. Comments and trailing commas are allowed when a file is read. Saved files are sorted by module and key and pretty-printed.
- **Loading** applies every value with `settings::Set` on the Unity main thread, as if you had changed each one in the overlay. A value the profile does not contain is reset to its default. Unknown modules or keys are ignored. Module `enabled` flags are applied last, so modules start or stop with their final values in place. If a module has never been set up (it was off at startup), its values are stored in `sr2mods.json` and used when it starts.
- **Saving** writes the current value of every setting. That includes values in `sr2mods.json` for settings a module has not registered yet. The write is atomic (`.tmp`, then rename). An existing file is only replaced after you confirm.
- **Not in profiles:** the `ui` settings (overlay hotkey, hint and theme) and `sandbox` (its `enabled`). A shared profile can never move your hotkey or turn on the sandbox, which the overlay only enables after a confirmation dialog.
- **Names** may contain letters, digits, spaces, `-` and `_`, up to 40 characters. Other characters are dropped. Files you copy in with other names still show up and can be loaded, renamed or deleted.
- **Active profile:** `sr2mods.json` records it as `"active_profile"` (empty means none). The overlay shows it, with "(modified)" when the current values differ from its file. Startup does not re-apply it, because the live values in `sr2mods.json` are always the ones used.
- **Starter profiles:** the first time the folder is created, two profiles are written: **Vanilla** (every module off, multiplier settings at 1) and **Default** (every setting at its default). They are ordinary files: edit or delete them. They are only written when the folder does not exist, and an existing file is never overwritten.
- **Sharing:** copy the `.json` file. To use a shared profile, put it in the `profiles` folder ("Open folder" on the overlay's Profiles page opens it). The list refreshes every time the overlay opens.

Overlay controls (Profiles in the sidebar): the profile list (the active one is marked ACTIVE or MODIFIED, broken files BROKEN; double-click loads), **Load**, **Save** (overwrite the active profile, after a confirmation), **Save as...** (name plus optional description; an existing name needs an explicit "Overwrite"), **Rename**, **Delete** (after a confirmation), **Open folder**, a refresh icon in the tab bar, and a one-line status. The in-game Options "Mods" tab has no profile row.

API: `core/profiles.h` (`List`, `Refresh`, `ActiveProfile`, `IsModified`, `Load`, `SaveCurrentAs`, `Delete`, `Rename`, `SanitizeName`, `Status`).

## How startup works

1. `DllMain` only calls `CreateThread`. It does no other work while the loader lock is held.
2. The proxy stubs jump through a table. The first call to an export loads the real `version.dll` from `GetSystemDirectoryW()` (never by bare name) and fills the table. Two exports, `VerLanguageNameA/W`, stay as the same `KERNEL32` forwarders the system DLL uses.
3. The worker exits immediately unless the host process is `SlimeRancher2.exe`. This matters because other programs in the game folder, such as `UnityCrashHandler64.exe`, may load the proxy too. Setting `SR2MODS_ANY_HOST=1` overrides the check for testing.
4. The worker waits for `GameAssembly.dll`, then resolves every `il2cpp_*` function by export name.
5. It waits for `il2cpp_get_corlib() != NULL`, then for `il2cpp_domain_get()` to list `Assembly-CSharp.dll`, then waits `settle_delay_ms`. It then calls `il2cpp_thread_attach`. `il2cpp_domain_get` alone cannot be used as the ready signal: in this build it creates the domain lazily if none exists yet (confirmed by disassembly).
6. While IL2CPP initialises, the worker computes the SHA-256 of `GameAssembly.dll` and compares it with the known 1.3.0 build (`786006ed…385b7`).
7. The worker dumps the images and resolves the `UnityEngine.Application` getters. It starts MinHook and installs the main-thread dispatcher, then registers and sets up the modules. It queues the `Application.version` check for the main thread, then calls `sr2::ui::StartOptionsTab()` and `sr2::ui::StartOverlay()`. Each UI call fails on its own: a `false` return or a crash is logged and does not stop the other. Finally the worker detaches from IL2CPP and exits. The hooks stay active.

## Threading model

| thread | runs | may touch |
|---|---|---|
| **worker** (from `DllMain`, attached to IL2CPP during startup) | config, waiting, hashing, `Module::Setup`, UI `Start*` | IL2CPP metadata (resolver), hook creation. No Unity object APIs. |
| **Unity main thread** | detours of game methods, `main_thread::Post` jobs, frame callbacks, runtime module toggles (`OnEnable`/`OnDisable`, lazy `Setup`) once the dispatcher is live | everything, including Unity APIs and `core/unity.h` |
| **render / other game threads** | e.g. a `Present` hook | no Unity APIs; hand work to the main thread with `main_thread::Post` |
| **settings saver** | debounced `sr2mods.json` writes | nothing game-related |

The **dispatcher** (`core/main_thread.h`) uses two hooks:

- `UnityEngine.EventSystems.EventSystem::Update` (UnityEngine.UI). This is a real per-frame `MonoBehaviour.Update` with its own 0x260-byte body; its RVA is not shared with any other method. SR2 keeps an EventSystem alive both in menus and in-game.
- `UnityEngine.Canvas::SendWillRenderCanvases` (UnityEngine.UIModule). This static method is called by the engine every rendered frame while a canvas exists. It covers loading screens.

Both hooks call one tick. The tick is de-duplicated with `Time.frameCount`, so it runs exactly once per frame. Each tick runs the posted jobs (FIFO) and then the frame callbacks with `Time.unscaledDeltaTime`. Every job and callback runs inside `Try()` (C++/managed and SEH). A failing job is logged and dropped. A failing frame callback is logged and removed. The game method itself always runs unguarded, so game exceptions follow their normal path. `main_thread::Ready()` becomes true at the first tick.

`settings` is fully thread-safe. `OnChange` callbacks run on the thread that called `Set()`, outside the settings lock. If a callback must touch Unity, it should `Post`. `ModuleManager::SetEnabled` from any thread other than the main thread is posted to the main thread once the dispatcher is live.

## Writing a module

Subclass `sr2::core::Module` (see `core/module.h`) in `modules/<name>/module.cpp`, and expose a factory `std::unique_ptr<sr2::core::Module> sr2::modules::Create<Name>()`. Add one line to `core/module_registry.cpp`. Resolve everything in `Setup(ctx)` by name only, with `ctx.HookMethod(...)`, `ctx.Class(...)` and `ctx.FieldOffset(...)`. When overloads share a parameter count, use `Resolver::MethodByParams(klass, name, {"System.Type", ...})`. Register settings in `Setup` with `settings::Register`. Use `main_thread::Post` and `AddFrameCallback` for per-frame work outside your hooks, and `core/unity.h` for common Unity calls. Unity helpers must be called on the main thread. Hold managed objects you cache across frames with `unity::NewGCHandle`.

### Guarding detours (`core/detour_guard.h`)

Wrap the mod logic of every detour in a `DetourGuard`. Leave the call to the original outside the guard:

```cpp
static sr2::core::DetourGuard g_awakeGuard{"minimap", "MapUI.Awake"};   // constant-initialised

void MapUI_Awake_Detour(Il2CppObject* self, const MethodInfo* mi) {
    g_origAwake(self, mi);                           // game code, always runs, unguarded
    g_awakeGuard.Run([&] { OnMapUIAwake(self); });   // mod logic, guarded
}

// Replacing / changing a result:
float Foo_Detour(Il2CppObject* self, float x, const MethodInfo* mi) {
    float r = 0;
    if (g_fooGuard.Run([&] { r = MyFoo(self, x); })) return r;
    return g_origFoo(self, x, mi);                   // faulted or threw -> pass-through
}
```

A native fault inside `Run()` (an access violation, for example) marks the module faulted. One ERROR line is logged with the fault address as `GameAssembly.dll+0x…`. From then on every `DetourGuard` of that module skips its lambda, so the module's detours pass straight through. `ModuleManager` lists the module as FAILED ("faulted: …"), and its hooks are disabled at the next main-thread tick. `OnDisable` is not called. A C++ or managed exception skips only that call and is logged (rate-limited). After 50 exceptions in one hook the module is marked faulted. `MarkModuleFaulted(module, reason)` and `ModuleFaulted(module)` are also public. `Try()` / `GuardedCallEx()` errors now include the fault address.

If any resolve or hook in `Setup` fails, only that module is disabled. Its hooks are removed and the reason is logged. At runtime, `ModuleManager::SetEnabled(name, bool)` turns a module's hooks on or off as one MinHook batch.

## Version guard

`sr2::core::version_guard::Mismatch()` and `Status()` (`core/version_guard.h`) give the UI a one-line status. The guard combines three checks:

- the SHA-256 of `GameAssembly.dll`, computed at startup
- `Application.unityVersion`, read on the worker
- `Application.version`, read on the main thread through `main_thread::Post`, because it is not thread-safe

A mismatch is only a warning, because modules resolve by name and may still work.

## Layout assumptions

There is one struct-layout assumption: `MethodInfo::methodPointer` is the first field.

`Il2CppGCHandle` is pointer-sized in this IL2CPP version (`typedef void*`, checked against the 1.3.0 dump). This is enforced with a `static_assert`. At startup the worker also runs a self-test: it creates a string, takes a normal and a pinned handle, checks that `get_target` returns the same object, then frees both, all guarded. If the self-test fails, `unity::NewGCHandle` returns `nullptr` and every module is marked FAILED. This has held in every IL2CPP version up to and including Unity 6. Each pointer read this way is checked to be inside `GameAssembly.dll`.

Two IL2CPP runtime ABI constants are used for managed arrays in `core/unity.h`: the element data starts at `4 * sizeof(void*)` (`kIl2CppSizeOfArray`), and the length comes from the `il2cpp_array_length` export.
