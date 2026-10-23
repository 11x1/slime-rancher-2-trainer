# quick_start

This module shortens the time from launch to playing. It is native only and resolves everything by name: namespace, class, method and argument count, plus fields by name. It uses no RVAs. Module key: `quick_start`, on by default.

| setting | default | what it does |
|---|---|---|
| `skip_intro` | on | Makes the Monomi Park logo, the warning screen and the game logo end after one frame each. Answers the "press any key" prompt automatically. Scene loading is never skipped. |
| `auto_continue` | off | Once per launch, when the main menu is ready, does what the **Continue** button does (loads the most recent save). Holding **Shift** while the game window is in front, at any point before it fires, cancels it for that launch. The cancel is logged. |
| `auto_continue_delay` | 0.5 s (0-5) | How long the main menu stays interactive before auto-continue fires. |
| `skip_new_game_intro` | on | When a new save starts, ends the story intro (the text frames shown before the player gets control) right away through the game's own end-of-intro path. The main quest, tutorials and camera are set up exactly as after a watched intro. |

## Boot flow (game 1.3.0, from the dump and a disassembly of `re/input/GameAssembly.dll`)

1. **Unity / IL2CPP start-up**, then `level0` (bootstrap: `BootstrapCompanyLogoScene` fades the logo in while loading). `SceneLoader` loads the scene group **CompanyLogo**.
2. **`CompanyLogoScene.Start`** queues the startup music and starts `StartUpCoroutine`. The coroutine has 16 states. Every wait in it is `WaitForSeconds(<serialized float>)` or a `Time.time` loop:
   - company logo: fade in `logoFadeInTime`, hold `logoHoldTime` (the optional blink `logoBlinkTime` happens inside the hold), fade out `logoFadeOutTime`
   - black screen: `logoBackgroundHoldTime`
   - **warning screen**: fade in `warningFadeInTime`, then hold until `warningHoldTime` runs out or `_skipWarningScreen` is set. A key press sets it through `CompanyLogoScene.OnInteract`. Then fade out `warningFadeOutTime`.
   - black screen again: `logoBackgroundHoldTime`
   - game logo background: `splashFadeInWaitTime`; game logo: `splashFadeInTime`
   - last step: `SceneLoader.LoadSceneGroup(PlatformInfoProvider.standaloneEngagementPrompt)`, which is the **StandaloneStart** group, with the LoadStep `OnMainMenuLoaded`.

   Nothing loads inside the coroutine. It is presentation only.
3. **`CompanyLogoScene.OnMainMenuLoaded`** finds the `PlatformEngagementPrompt` and subscribes `AnimateOut` (to `OnSceneActivated`) and `StartLoadingIndicator` (to `BeforeAnimateOut`). It then calls **`PlatformEngagementPrompt.Onloaded()`**, which sets the texts and `RegisterInput()` (the submit action's `canceled` event). The game now waits for a key ("press any key").
4. **`StandaloneEngagementPrompt.OnInteract(CallbackContext)`**:
   - plays the click, unregisters input and disables the EventSystem
   - `IUserManager` establishes the user, then `SystemContext.InitializePlatformForCurrentUser`, then `OnSuccessfullySetupUser`
   - runs `BeforeAnimateOut`, then `Animate(AnimationDuration)` (scale/fade curve)
   - `LoadSceneGroup(SceneGroupToLoad)`, which is **MainMenuFromBoot**, with LoadStep `OnSceneGroupLoad`. That step optionally fades the EA message (`EaMessageFadeTime`), then `OnSceneActivated` runs `CompanyLogoScene.AnimateOut`, which fades the overlay away and reveals the menu.

   The context argument is never read. In the disassembly `rdx` is overwritten before any use.
5. **Main menu**: `MainMenuLandingRootUI` builds `ButtonBehaviorModel`s from its `ButtonBehaviorConfiguration`. `OnSubmit` calls the selected model's `InvokeBehavior()`.
   - **Continue** is `ContinueGameItemDefinition`. It creates a `ContinueGameBehaviorModel` (a `LoadGameBehaviorModel`) whose `GameDataSummary` is set to `AutoSaveDirector.GetSaveToContinue()`.
   - `InvokeBehavior()` returns early if `SceneLoader.IsSceneLoadInProgress` is set. Otherwise it calls `AutoSaveDirector.BeginLoad(summary.SaveIdentifier, null)`.
   - Visibility condition: `ButtonBehaviorConditionUtility.HasContinueGame()`, which calls `AutoSaveDirector.HasContinue()`.

`StandaloneStartScreen` (which auto-loads its `defaultSceneGroup` on its second frame) is not part of this flow. In this build "StandaloneStart" is the engagement-prompt group.

## Where the time goes (evidence)

Sources: `Player.log` / `Player-prev.log` (Unity prints scene-group load times but no timestamps) and the timestamped `sr2mods.log` of the same 19:00 launch. `sr2mods.log` was only read, never written.

**The `Scene Transition ... took N seconds` figures are cumulative.** The line is logged by `SceneLoader+<>c__DisplayClass99_0.<EnqueueLoadSceneGroup>b__16` (0x8E3150) from `SceneLoader.Stopwatch.ElapsedTimeInSeconds`. That is one `MonomiPark.SlimeRancher.Stopwatch` per `SceneLoader`, created lazily in `get_Stopwatch` (0x8DC6B0); its `Start()`/`Stop()` (0x99EF80/0x99F010) only resume and pause the wrapped `System.Diagnostics.Stopwatch`, and nothing resets it. Each figure is therefore the total scene-load time since launch: it keeps running only while a scene group loads, so time between loads (the logos, the prompt, the menu) is not in it. One step's load time is the difference to the previous line. The logs agree: the values only ever grow (launch of 2026-10-23 02:15: 0.735, 0.875, 9.731, 19.289, 21.799).

| phase | time | evidence | skippable by a mod? |
|---|---|---|---|
| process start to first rendered frame (Unity engine, D3D11, IL2CPP init, bootstrap scene) | **~1.9 s** | proxy loaded 19:00:50.283; GameAssembly 50.337; IL2CPP domain ready 50.551; dispatcher live 52.197 and `Time.frameCount` 77 at 52.753 at ~7 ms/frame, so frame 1 was at ~52.2 | **no**. Native engine start; the mod only runs after IL2CPP is up (+ `settle_delay_ms` 1.5 s) |
| scene group **CompanyLogo** (boot) | **0.75 s** | first transition line, so the total is this step: `CompanyLogo to CompanyLogo took 0.746` (prev launch 0.725) | no (asset load) |
| logo / warning / game-logo sequence | **~11-12.7 s (estimate)** | The serialized durations are in the CompanyLogo scene's addressable bundle and cannot be read offline here; the module logs the exact vanilla values on its first run. Estimate: the sequence started at ~52.95 and the StandaloneStart activation hitch (183.6 ms, frame 1912) came at 19:01:05.79. That 12.8 s window still holds the 0.14 s StandaloneStart load, so 12.7 s is an upper bound | **yes** (`skip_intro`). About 15 frames remain |
| scene group **StandaloneStart** | **0.14 s** | `CompanyLogo to StandaloneStart took 0.885` minus 0.746 = 0.139 (prev 0.863 - 0.725 = 0.138) | no |
| "press any key" prompt | player's reaction time | In this launch the prompt stayed up ~18 s: the load's allocation burst began at 19:01:24, so the player was away | **yes** (`skip_intro`) |
| prompt click animation | `AnimationDuration` (serialized, logged at runtime) | dump | **yes** (zeroed with `skip_intro`) |
| scene group **MainMenuFromBoot** | **8.6-8.9 s** | `StandaloneStart to MainMenuFromBoot took 9.767` minus 0.885 = 8.88 (prev 9.6 - 0.863 ≈ 8.7). About 88k objects loaded (64k to 152k), 10,667 assets unloaded at the end | **no**. Addressables / asset loading and scene activation |
| first seconds of the menu | (menu usable; ~11 s of 40-670 ms hitches, 56 fps average) | `sr2mods.log` perf from 19:01:34 to 45 (shader / streaming warm-up) | no |
| Continue to in-game | **~9.4-9.6 s** load | not in the 19:00 logs. Later launches: `MainMenuFromBoot to ConservatoryFields took 19.289` minus 9.731 = 9.56, and 18.829 - 9.439 = 9.39 | the click and the menu wait are skippable (`auto_continue`); the save/world load is not |

The later launches (2026-10-23) give the same per-step loads: CompanyLogo 0.735 / 0.721 s, StandaloneStart 0.140 / 0.147 s, MainMenuFromBoot 8.856 / 8.571 s.

**Summary.** With an instant key press, vanilla is about 1.9 + 0.75 + ~11-12.7 + 0.14 + ~0.5-1 + 8.9 ≈ 23-25 s to the menu. The logo part is the uncertain one, and the user's ~20 s suggests it is closer to 6-8 s.

With `skip_intro` the floor is about 1.9 + 0.75 + ~0.1 + 0.14 + ~0.05 + 8.9 ≈ **12 s** to an interactive menu. Everything left is engine start-up and real asset loading that a mod cannot remove.

Caveat: anything the game does in the background during the logos (for example platform/Steam initialisation) moves into the following load. The real saving can therefore be slightly smaller than the vanilla wait total. The module logs the measured time.

## How it is done

**skip_intro**
- **Pre-hook `CompanyLogoScene.Start` (0 args)**:
  - validates `this` (class check + `IsAlive`)
  - logs the vanilla timings, zeroes the 11 timing floats listed above and sets `_skipWarningScreen = true`
  - the game's coroutine still runs every step in order (music, `DestroyBootstrapCamera`, the fades with duration 0, the scene-group load)
- **Late install**: the module is set up ~2 s after process start, normally before `Start` runs ~0.75 s later. If the hook arrives too late, the frame callback polls `FindObjectOfType(CompanyLogoScene)` every 0.1 s for up to 60 s and patches the running instance. A wait already in progress still finishes.
- **Post-hook `PlatformEngagementPrompt.Onloaded` (0 args)**: marks the end of the logo phase (it logs the measured time against the vanilla wait total) and starts the auto-answer. On the main thread, it waits for all of these:
  - at least 2 frames have passed
  - the prompt is active and is a `StandaloneEngagementPrompt`
  - `SceneLoader.<IsSceneLoadInProgress>k__BackingField` is false, the same moment a player could press a key in vanilla

  It then zeroes `AnimationDuration` and `EaMessageFadeTime` (the curve evaluation clamps `t/0` to 1, so it lasts one frame). Finally it calls `StandaloneEngagementPrompt.OnInteract` through `il2cpp_runtime_invoke` with a zeroed `CallbackContext`. Done once per process; it gives up after 20 s.
- **Pass-through hook `StandaloneEngagementPrompt.OnInteract` (1 arg)**: only records that a key press happened, so the prompt is never answered twice when the player is quicker.

**auto_continue**
- **`MainMenuLandingRootUI.OnEnable`**: marks "menu seen". The hook itself belongs to the always-on `[modded]` title-screen label (`ui/modded_label.cpp`, see `ui/NOTES.md`), because MinHook allows one hook per target. `Setup` registers a listener with `modded_label::SetMenuEnableListener`; the label's post-hook calls it after the original, and the listener runs inside this module's `DetourGuard`. If that hook is not installed, auto_continue is unavailable (logged).
- **Frame callback (every 0.1 s)**: the menu counts as ready when all of these hold:
  - `SceneLoader` is not loading
  - `MainMenuLandingRootUI` is alive and active in the hierarchy
  - no active `CompanyLogoScene` overlay and no active `PlatformEngagementPrompt`

  After `auto_continue_delay` it checks `ButtonBehaviorConditionUtility.HasContinueGame()`. If that is true, it calls `InvokeBehavior()` on the `ContinueGameBehaviorModel` found in `_models` (or `_subListModels`) that has a `GameDataSummary`.
- **Submenu fallback**: if Continue lives in a submenu that was never opened, its model does not exist. The module then makes the same calls `InvokeBehavior` makes: `AutoSaveDirector.BeginLoad(GetSaveToContinue().SaveIdentifier, null)`.
- **Once per process**: it only fires at the first main menu of the launch. If `auto_continue` is off then, it stays off until the next launch. It cancels if Shift is held while the game window is in front, or if a scene load starts from the menu before it fires (the player acted). It gives up if the menu never becomes ready within 120 s.
- **Overlay note**: while it is pending, the overlay shows "Hold Shift to stay in the main menu" top left (with "Continuing in N s" once the menu is ready), and "Staying in the main menu" for 2.5 s after a Shift cancel. The frame callback publishes the state through `ui/quick_start_hint.h` (atomics only; the render thread makes no game calls).

**skip_new_game_intro**

New-game flow (disassembly, 1.3.0):
- `AutoSaveDirector.StartNewGame` adds a load step `<>c.<StartNewGame>b__29_2` (0x5E4150), which calls `CutsceneDirector.Instance.SpawnIntroSequence()` (0xAC4290). That is the only caller. `SpawnIntroSequence` activates the registered `IntroSequenceUIRoot` (it registers itself in `Awake`) and pushes it on `UIDirector`.
- `IntroSequenceUIRoot.Start`: black screen on (alpha 1), `PauseNonUiSfx(true)`, `_cameraEnabled.Value = false`, starts `StartUpCoroutine`: queue `_introSequenceMusic`, wait `_fadeInWaitTime`, fade the black screen out over `_fadeInTime`, then `StartFrame(_frameIndex)`.
- `StartFrame` tweens the cutscene camera to the frame's target; on completion the frame text is shown and `_isInputEnabled` set. The `_next` input calls `TryToIterateFrame`, which after the last frame does exactly what `EndSequence()` does: `_isInputEnabled = false`, `MusicController.Dequeue(_currentMusic)`, `StartCoroutine(EndCoroutine)`.
- `EndCoroutine`: fade to black over `_fadeOutTime`, wait `_fadeOutWaitTime`, then the virtual close (the UI is popped and destroyed).
- `IntroSequenceUIRoot.OnDestroy` applies the intro's state: `PauseNonUiSfx(false)`, destroy the cutscene camera, `_cameraEnabled.Value = true`, `MetaGameDirector.StartMainQuest(true)`, `FadeIntoGameplay()` (`TutorialDirector.SuppressTutorials(HudUI)`, `LockOnDeath.Freeze()`, `DeathObscurer.StartObscuring(false, _fadeOutTime, cb)` with cb = `LockOnDeath.Unfreeze()` + `UnsuppressTutorials`), then `CutsceneDirector.ClearIntroSequenceUIRoot()`.
- `EndSequence` has no direct caller in the binary (probably the debug skip); its body is the same as the end branch of `TryToIterateFrame`.

What the module does:
- **Pre-hook `IntroSequenceUIRoot.Start` (0 args)**: class check + `IsAlive`, logs the vanilla fade values, then `_fadeInWaitTime = 0`, `_fadeInTime = 0.1 s`, `_fadeOutTime` at most 0.5 s, `_fadeOutWaitTime = 0`. The fades keep a short non-zero length because they also drive tweens, particles and the `DeathObscurer`.
- **Replace hook `IntroSequenceUIRoot.StartFrame` (1 arg)**: on the first call per instance it calls `EndSequence()` through `runtime_invoke` instead of the original. At that point the intro music has been queued (so `EndSequence` dequeues it), no frame tween exists and input is still off, so the next-frame input does nothing. The game's own end path then runs unchanged, including everything `OnDestroy` sets. If `EndSequence` throws, the original `StartFrame` runs and the intro plays as usual; a native fault marks the module faulted (detours become pass-through).
- What the player sees: black, about 0.1 s of the intro camera view while the black screen fades, a fade to black (at most 0.5 s), then the game's own fade from black into gameplay.

**Safety**
- All logic in the detours and the frame callback runs inside `DetourGuard`s. A native fault marks the module faulted for the session: hooks are disabled and the detours become pure pass-through. A native fault reported by `runtime_invoke` does the same (`MarkModuleFaulted`).
- Objects are re-found with `FindObjectOfType` when needed and checked with `IsA` + `IsAlive` (+ `activeInHierarchy`). A cached object pointer is only compared, never dereferenced later. The module holds no GC handles.

## Names resolved (Assembly-CSharp)

- `MonomiPark.SlimeRancher.UI.CompanyLogoScene`
  - method: `Start()`
  - fields: `logoFadeInTime`, `logoHoldTime`, `logoBlinkTime`, `logoFadeOutTime`, `logoBackgroundHoldTime`, `logoBackgroundFadeOutTime`, `warningFadeInTime`, `warningHoldTime`, `warningFadeOutTime`, `splashFadeInTime`, `splashFadeInWaitTime`, `_skipWarningScreen`
- `MonomiPark.SlimeRancher.UI.MainMenu.PlatformEngagementPrompt`
  - method: `Onloaded()`
  - fields: `AnimationDuration`, `EaMessageFadeTime` (optional)
- `MonomiPark.SlimeRancher.UI.MainMenu.StandaloneEngagementPrompt`: `OnInteract` (1 arg)
- `MonomiPark.SlimeRancher.SceneManagement.SceneLoader`: field `<IsSceneLoadInProgress>k__BackingField`
- `MonomiPark.SlimeRancher.UI.MainMenu.MainMenuLandingRootUI`
  - method: `OnEnable()` (hooked by `ui/modded_label.cpp`)
  - fields: `_models`, `_subListModels` (optional)
- `MonomiPark.SlimeRancher.UI.MainMenu.Model.ContinueGameBehaviorModel`: class check only
- `MonomiPark.SlimeRancher.UI.MainMenu.Model.LoadGameBehaviorModel`
  - field: `<GameDataSummary>k__BackingField`
  - method: `InvokeBehavior()`
- `MonomiPark.SlimeRancher.UI.ButtonBehavior.ButtonBehaviorConditionUtility`: `HasContinueGame()`
- `MonomiPark.SlimeRancher.UI.IntroSequence.IntroSequenceUIRoot`
  - methods: `Start()`, `StartFrame` (1 arg), `EndSequence()`
  - fields: `_fadeInWaitTime`, `_fadeInTime`, `_fadeOutTime`, `_fadeOutWaitTime`
- fallback (optional):
  - `MonomiPark.SlimeRancher.AutoSaveDirector`: `GetSaveToContinue()`, `BeginLoad` (2 args)
  - `MonomiPark.SlimeRancher.Persist.Summary`: `get_SaveIdentifier()`

If the skip_intro, auto_continue or skip_new_game_intro set fails to resolve, only that feature is switched off (logged). If all three fail, the module fails.

## Risks / limits

- **Not yet tested in game** (the game was not launched for this work). The first run's `sr2mods.log` shows the vanilla logo timings, the measured logo phase, when the prompt appeared and was answered, and when the menu became interactive (`T+x s after process start`).
- **Warning screen**: its exact text is unknown from the dump. It is a photosensitivity or save notice that vanilla also lets a key press skip. `skip_intro` skips it.
- **The prompt answer is a real "key press"**: Steam user establishment and platform init run as usual. If they fail, the game shows its own error, and the module does not answer again.
- **`OnInteract` with a zeroed `CallbackContext`**: safe in 1.3.0 because the context is not read. If a future version reads it, it would throw a managed exception: caught, logged, and the player presses a key as usual.
- **Fragile names**: `<IsSceneLoadInProgress>k__BackingField` and `<GameDataSummary>k__BackingField` are compiler-generated names. They are stable unless the property is rewritten. Failure only disables the feature.
- **ABI assumption**: the boxed `bool` / `GameSaveIdentifier` returned by `runtime_invoke` is read at `2 * sizeof(void*)` (the Il2CppObject header). This is the same footing as the array-data constant in `core/unity.h`, and the class name of the box is checked first.
- **skip_new_game_intro is not yet tested in game.** Check on a new save: `sr2mods.log` shows "new-game intro starting" with the vanilla fades and "intro ended before frame 0"; the player must get control, the main quest / first tutorial popups must appear as after a watched intro, and the HUD and camera must be normal.
- **Auto-continue loads a save without a click**. Turn it off, or hold Shift at launch, to reach the menu.
