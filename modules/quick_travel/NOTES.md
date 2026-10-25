# quick_travel

This module makes respawning after a knockout faster and removes the artificial waits around teleporter loads. It is native only and resolves everything by name (namespace, class, method and argument count, fields by name). It uses no RVAs. Module key: `quick_travel`, on by default.

| setting | default | what it does |
|---|---|---|
| `fast_respawn` | on | While a knockout is being handled: the death screen fades take 0.02 s instead of `_fadeTime` and the minimum delay is 0 s. In the ranch zone, the death loading screen is skipped. Anywhere else, the ranch-zone load runs without loading-screen padding. The house screen still runs the time skip, then closes by itself, the same way Esc closes it. |
| `fast_teleport` | on | For a teleporter into another zone: the loading screen's 1 s "fade-in" wait is 0 s and its fade to black lasts one frame. The zone load itself is unchanged. Teleports inside one zone are already instant in the game. |

Every scene-group load is timed, from `SceneLoader.LoadSceneGroup` until `IsSceneLoadInProgress` turns false, and logged together with what was skipped.

## Game flows (1.3.0)

Full call chains: RE: re/player.md (PlayerDeathHandler knockout flow), re/map.md (TeleportNetwork, TeleporterNode), re/world.md (SceneLoader queue). In short:

- **Knockout:** `PlayerDeathHandler.OnDeath` (penalties, `unlockWorldTime`, `LockOnDeath`, `ResetPlayer`) -> `DeathObscurer.StartObscuring(true, _fadeTime)` -> `TeleportNetwork.Teleport_Death(player, _delayTime)`. In the ranch zone an in-scene queue: death loading scene, `TeleportTo` wake-up point, "Wait For Min Delay On Death" (`_delayTime`), unload with "Wait for Death Loading Outro" (1 s). Elsewhere a real `LoadSceneGroup` of the default group (the "reload"). Then the `RanchHouseUI` sleeps until `unlockWorldTime` (`LockOnDeath.LockUntil` -> `TimeDirector.FastForwardTo`, **the time skip**), saves (`AutoSaveDirector.SaveGameAndFlush`), fades back in, and **stays open until the player leaves it** (`Close()` / Esc -> `ResetPlayerLocation`).
- **Teleporter:** `TeleporterNode.OnTriggerEnter` -> `TeleportNetwork.Teleport` -> `TeleportToDestinationImpl`: destination registered and not `ForceSceneTransition` = in-scene and **already instant**; otherwise `LoadSceneGroup` with a loading screen.
- **Loading queue padding** (`SceneLoader.EnqueueLoadSceneGroup`, every load): "Wait For Loading Scene Fade In" = `EnqueueDelaySeconds(1.0)` before the real work, `LoadingScreenView.FadeToBlack` (alpha += `dt / _fadeOutTime`) after it; the 1 s "Wait for Loading Outro" only for the death screen.

## Where the time goes (evidence)

Source: `Player.log` (session of 2026-10-22 21:xx). The "Scene Transition from A to B took N seconds" values are **cumulative**: `SceneLoader` calls `System.Diagnostics.Stopwatch.Start()`/`Stop()` without `Reset` (`MonomiPark.SlimeRancher.Performance.Stopwatch.Start` disassembled). Per-load durations are the differences between consecutive lines.

| load | per-load time (stopwatch) | padding inside it |
|---|---|---|
| teleport Fields → Rumbling Gorge | 4.59 s | 1.0 s fade-in wait + `_fadeOutTime` |
| teleport Gorge → Powderfall Bluffs | 2.98 s | same |
| teleport Bluffs → Gorge | 4.39 s | same |
| teleport Gorge (rear) → Fields | 4.92 s | same |
| main menu → Conservatory (save load) | 7.1-9.1 s | not touched by this module |

- So ≥ 1.0 s of every 3-5 s teleport load (20-35 %) is a fixed wait, plus the fade to black, whose serialized length is logged at runtime. The rest is asset streaming, scene activation, GC and unloading, which the module does not touch.
- No knockout happened in the available logs. Knockout timings come from the module's own log lines on the first run (vanilla `_fadeTime` / `_delayTime`, phase times, time-skip hours, fast-forward seconds).

## What fast_respawn keeps and skips

**Kept** (the game's own code runs unchanged):
- everything in `OnDeath` / `ResetPlayer`:
  - drop vacuumed items, leave liquids
  - currency share `PctCurrencyLostOnDeath`
  - health/energy reset, rad reset
  - ammo clear and radiant-slime loss (unless in a boss fight)
- boss fight reset, analytics, death events
- `LockOnDeath` lock
- the **time skip** to `unlockWorldTime`. `_fadeTime` / `_delayTime` are never changed, so `MinDeathTime` and the penalty are identical. The game fast-forwards it itself.
- `PlayerItemController.ResetAfterDeath` and the **save** after the skip
- the house-close path (`ResetPlayerLocation`: a second `ResetPlayer(false)` + teleport to the wake-up point), exactly as when the player presses Esc

**Skipped / shortened:**
- the fade to black (`_fadeTime` → 0.02 s) and the fade back in (it ran behind the house screen anyway)
- "Wait For Min Delay On Death" (`_delayTime` → 0)
- in the ranch zone: the death loading screen (scene load, "Setup Death Transition", 1 s outro wait, unload). These are skipped as a pair inside one `Teleport_Death` call. The screen is already black from the obscurer.
- outside the ranch zone: the zone load still happens (the spawn point is in another scene group, so a real load cannot be avoided), minus its 1 s fade-in wait and fade to black
- waiting for the player to press Esc in the house screen. The module calls `RanchHouseUI.Close()` 2 frames after the time skip ends, once `_sleeping` is false and `_isClosing` is false.

**Not skipped:** the house screen itself and the fast-forward while it is open. This is the game's mechanism for the time-skip penalty; it unpauses and pauses time, runs the lock and the save, and invokes `AfterSleepAction`. Replacing it would mean re-creating managed delegates natively, and if that failed the player could be left "dead". The real seconds it takes are logged.

## Hooks (all logic in `DetourGuard`s; the original call is always outside the guard)

| hook | kind | purpose |
|---|---|---|
| `PlayerDeathHandler.OnDeath` (3) | pre | starts tracking a knockout, logs the vanilla `_fadeTime` / `_delayTime` |
| `DeathObscurer.StartObscuring` (3) | pre, changes the arg | fade time → 0.02 s while a knockout is tracked (> 0 so `1/fade` stays finite and a paused frame cannot produce NaN) |
| `TeleportNetwork.Teleport_Death` (3) | wrap | sets the "respawn" context for the SceneLoader hooks; logs in-scene vs reload |
| `TeleportNetwork.Teleport_ResetPlayer` (2) | wrap | "respawn" context (house close) |
| `TeleportNetwork.TeleportToDestinationImpl` (5) | wrap | "teleport" context; logs same-zone (instant) vs scene load |
| `SceneLoader.LoadSceneGroup` (2) | pre | starts timing a load (every load; only teleport/respawn loads are changed) |
| `SceneLoader.EnqueueDelaySeconds` (2) | pre, changes the arg | 0 s for "Wait For Loading Scene Fade In" (teleport/respawn context), "Wait For Min Delay On Death" and "Wait for Death Loading Outro" (respawn) |
| `SceneLoader.EnqueueLoadDeathLoadingScene` / `EnqueueUnloadDeathLoadingScene` (0) | replace when skipping | skip the death loading screen as a pair |
| `LoadingScreenView.FadeToBlack` (0) | pre | `_fadeOutTime` → 0.0001 for teleport/respawn loads (the alpha is clamped to 1, so it is done in one frame) |
| `RanchHouseUI.Sleep` (3) | post | notes the death sleep (UNTIL_WORLD_TIME) and its game hours (`TimeDirector.HoursUntil`) |
| frame callback | | times loads until `IsSceneLoadInProgress` is false; polls the house screen at 10 Hz and closes it after the time skip |

All targets have unique bodies (checked against the Cpp2IL RVA index: one method per address).

## Names resolved (Assembly-CSharp)

- `MonomiPark.SlimeRancher.SceneManagement.SceneLoader`
  - methods: `LoadSceneGroup`(2), `EnqueueDelaySeconds`(2), `EnqueueLoadDeathLoadingScene`(0), `EnqueueUnloadDeathLoadingScene`(0)
  - fields: `<IsSceneLoadInProgress>k__BackingField`, `_currentSceneGroup` (optional, log only)
- `MonomiPark.SlimeRancher.UI.Loading.LoadingScreenView`: `FadeToBlack`(0), field `_fadeOutTime` (optional)
- `PlayerDeathHandler` (global namespace): `OnDeath`(3), fields `_fadeTime`, `_delayTime`
- `DeathObscurer` (global namespace): `StartObscuring`(3)
- `MonomiPark.SlimeRancher.World.Teleportation.TeleportNetwork`: `Teleport_Death`(3), `Teleport_ResetPlayer`(2), `TeleportToDestinationImpl`(5)
- `MonomiPark.SlimeRancher.UI.RanchHouseUI`: `Sleep`(3), `Close`(0), fields `_sleeping`, `_isClosing`, `_timeDir` (optional)
- `TimeDirector` (global namespace): `HoursUntil`(1) (optional, log only)
- Queue step names compared as strings: "Wait For Loading Scene Fade In", "Wait For Min Delay On Death", "Wait for Death Loading Outro"

Feature groups: the SceneLoader core is required. If the respawn set or the teleport set fails to resolve, only that feature is switched off (logged). The module fails if both fail or if a hook cannot be created.

## Safety

- All hooks run on the main thread (checked). Objects handed in or found are checked with a class test and `IsAlive` before fields are read.
- The house screen is re-found with `FindObjectOfType` on every poll; a cached pointer is only compared, never dereferenced.
- One GC handle is used, for the `SceneLoader`. It is created in the `LoadSceneGroup` hook; stale handles are freed only in the frame callback, never in a hook.
- A native fault inside a guard marks the module faulted: hooks are disabled and the detours pass through. The same happens for a native fault reported by `runtime_invoke` (`RanchHouseUI.Close`, `TimeDirector.HoursUntil`).
- Delay values are matched by step name, so only the named waits change. Respawn loads are only sped up while a knockout is tracked.

## Risks / limits

- **Not tested in game** (the game was not launched for this work). The first knockout and the first teleport write the vanilla values and measured times to `sr2mods.log`.
- **The time skip still takes real seconds** (the game's fast-forward speed, `FfSecsPerGameDay`, is serialized). It is logged as "time skip done after X s of fast-forward". Removing it would mean changing world time directly, which this module does not do (penalty and world simulation stay vanilla).
- **Auto-closing the house screen** means the death visit to the house menu cannot be used. Turn `fast_respawn` off to keep it.
- **Skipping the death loading screen**: the death screen is black from the obscurer during the queued teleport. If a future version uses that scene for something else, behaviour could differ. In that case, turn `fast_respawn` off.
- **Zeroed fade-in wait**: the loading screen may pop in instead of fading over the last game frame. This is cosmetic.
- **Forced teleporters** (`ForceSceneTransition`) still do a scene load even within one zone. The module does not override a designer decision; the log line says "forced load".
- **Fragile names**: compiler-generated `<IsSceneLoadInProgress>k__BackingField` and the queue step strings. A rename only disables the matching shortcut; the step then keeps its vanilla wait.
- **ABI assumptions**: `LoadingScreenView.FadeToBlack` returns a 16-byte `Continuation` through the hidden return buffer (`rcx`) with `this` in `rdx`, as seen in the disassembly. `EnqueueDelaySeconds` returns an 8-byte `SceneLoaderQueueHandle` in `rax`. The boxed `double` from `HoursUntil` is read after the 16-byte object header, after its class name is checked.
