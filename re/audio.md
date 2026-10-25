# Music and audio

Tags: **code** = decompiled; **data** = asset bundles; **assumed** = not verified. Mod use: `modules/slime_dj/` (streamed music, emitters, Corral Radio, muffle), `modules/custom_upgrades/dj_buildables.cpp`, `modules/plot_upgrades/` (music box). Hooks: re/hooks.md.

- `SECTR_AudioSystem.PauseNonUiSfx` (called by `UIInputLocker.OnEnable`).
- `SECTR_AudioSource` (abstract; resolve `Play()` / `Stop(bool)` / `get_IsPlaying` on the concrete class); fields Cue, volume, pitch, Loop, PlayOnStart.
- Corral music box: `MusicBoxUpgrader.MusicBox` -> `Music Box/techCorralMusicBox/MusicBox` (SECTR_AudioSource) in the CORRAL prefab (re/plots.md); `MusicBoxRegion`.
- `MusicDefinition` (priority, `_cue` SECTR cue, loops); `MusicPriority` includes HOUSE 900 (shop songs), `SLIME_HOOP` 101. Shop music selection: re/shop.md.
- `GadgetMusicPlayer` (Boombox): `OnInteract`, `ShowDeactivatePrompt`, `_gadgetActive` animator bool (static hash), `gadgetMusic`, `onInteractCue`, cooldown; one gadget track at a time (`GadgetDirector.CurrentGadgetMusicPlayer`). Hooked by dj_buildables.
- `MusicDebugOverlay` is stripped (re/debug-mode.md).

## From the custom_upgrades notes

### GadgetMusicPlayer (Boombox)
- methods: `OnInteract` 0x747930 - when it is `GadgetDirector.CurrentGadgetMusicPlayer` it stops (Dequeue, OnStopPlaying); else queues `gadgetMusic` on the MusicController (one global gadget track, previous player stops), sets the animator bool and notes FX; then the 3 s cooldown
- methods: `ShowDeactivatePrompt` 0x748180 = Current == this; `Awake` 0x747680 caches MusicController, GadgetDirector, GetComponentInParent<Gadget>; `Update` 0x7481F0 only stops its own track (nothing resets the Animator bool)
- fields: static int `_gadgetActive` (animator bool hash), `_gadgetActiveFX`, gadgetMusic, onInteractCue, cooldown
- mod: dj_buildables.cpp (`dj.` hooks on OnInteract / ShowDeactivatePrompt for listed records only)

### Corral Music Box audio
- `LookupDirector.GetPlotPrefab(CORRAL)` -> `Music Box/techCorralMusicBox/MusicBox` -> `SECTR_AudioSource` (abstract; resolve `Play()` / `Stop(bool)` / `get_IsPlaying` on the concrete class); Cue, volume, pitch, Loop, PlayOnStart fields
- mod: dj_buildables.cpp (`sr2dj_standby` component copy)

### SECTR_AudioSystem
- `Play(cue, pos, loop)`; `PauseNonUiSfx` (UIInputLocker)

## From the slime_dj, spawn_scaling, hl2_movement, quick_start, free_placement, misc_tweaks, plot_upgrades, drone_price_limit notes

### Unity AudioSource volume above 1 (slime_dj Amplify)
- `AudioSource.volume` is clamped to 0..1 (Unity docs; the setter clamps). slime_dj gets louder than full volume by scaling the PCM it writes into its own clip (`AudioClip.SetData`) and dividing every speaker's volume by that gain; samples are soft-limited (tanh knee at 0.89) so they never pass 1 (whether Unity would keep float samples > 1 in a created clip was not checked).
- mod: slime_dj `module.cpp` (`Current::gain`, `chunkGain`, `ApplyGain`, `TickMix`)

### SECTR_AudioSource (global)  [: MonoBehaviour]
- fields: `Cue` (SECTR_AudioCue) - offset by name (dump)
- methods: `get_Volume()`, `set_Volume(float)` (dump)
- gotchas: the corral music box's vanilla tune is assumed to come from a SECTR_AudioSource under `MusicBoxUpgrader.MusicBox`; `Volume = 0` assumed to silence it (not verified in game; slime_dj logs how many it finds)
- mod: slime_dj `module.cpp` (speaker parent = the box's first SECTR_AudioSource; mute_vanilla sets Volume 0, restores the old value)
- fields (dump): +0x20 volume, +0x24 pitch, +0x28 Cue, +0x30 Loop, +0x31 PlayOnStart, +0x32 RestartLoopsOnEnabled; SECTR_PointSource +0x38 instance (SECTR_AudioCueInstance)
- `Start` 0x4EE4E0 (**IDA**): `Application.isPlaying && PlayOnStart && !IsPlaying` -> `Play()` (vtable)
- `OnEnable` 0x4EE180 (**IDA**): while playing, `Cue != null && RestartLoopsOnEnabled && (Cue.Loops (+0x58) || Loop)` -> `Play()` at once, or, while the SceneLoader is loading (+0x130), subscribes `PostSceneLoad` (0x4EE380: `Play()` if not playing, then unsubscribes). `OnDisable` 0x4EDFF0 unsubscribes.
- `SECTR_PointSource.Play` 0x505910 (**IDA**): stops a playing loop first; only plays while `isActiveAndEnabled` with a Cue; `SECTR_AudioSystem.Play(cue, transform, zero, Loop, ...)` then sets the instance's Volume / Pitch
- gotcha: a source added at runtime with `Loop` + `RestartLoopsOnEnabled` plays every time its GameObject goes active, whatever `PlayOnStart` says (round 12: the switched-off DJ Booth's standby tune played when placed / loaded). dj_buildables sets `RestartLoopsOnEnabled = false` and calls Play / Stop itself.

### SECTR_AudioCue (global)
- fields: `MinDistance`, `MaxDistance`, `Falloff`, `bus` (SECTR_AudioBus) - by name (dump)
- gotchas: the music box cue's Min/MaxDistance are far too short for a song (users); slime_dj only logs them
- mod: slime_dj (cue bus `Paused` = game pause menu -> speakers pause; assumed, not verified)

### SECTR_AudioBus (global)
- fields: `children` (bus tree) (dump)
- methods: `get_EffectiveVolume()`, `get_Muted()`, `get_Paused()` (dump)
- gotchas: the music bus = first bus whose name contains "music" under `SECTR_AudioSystem.MasterBus`; its `EffectiveVolume` is what the game's music slider drives (`ChannelVolumeController._targetChannel`), 0 while muted (inferred, not verified in game). Paused while the pause menu is open (assumed).
- mod: slime_dj (speaker volume = setting x bus EffectiveVolume)

### SECTR_AudioSystem (global)
- fields: `MasterBus` (static/instance by name) (dump)
- methods: `PauseNonUiSfx(bool)` (used by IntroSequenceUIRoot, see re/ui.md)
- gotchas: slime_dj finds it with `FindFirstObjectByType` once, retried every 5 s until found
- mod: slime_dj

### ChannelVolumeController
- fields: `_targetChannel` - the SECTR bus a volume slider drives (dump, inferred)

### UnityEngine.AudioSource (UnityEngine.AudioModule)
- methods used: `set_clip`, `set_loop`, `set_playOnAwake`, `set_spatialBlend`, `set_rolloffMode`, `set_min/maxDistance`, `set_dopplerLevel`, `set_priority`, `set_volume`, `set/get_mute`, `get/set_timeSamples`, `get_isPlaying`, `Play()`, `Pause`, `UnPause`, `Stop`
- gotchas:
  - `Play()` on a disabled AudioSource is refused; switching its GameObject off stops it for good (with playOnAwake off nothing restarts it) (log: Music Box II props rebuilt inactive after a teleport)
  - `Play()` starts at the next audio update, so a source started later trails by about one mix buffer (~20 ms) (log)
  - `timeSamples` runs ahead of what is heard by about the output buffer (~40 ms used as lag) (inferred)
  - `volume` is clamped to 1 by Unity (values > 1 do nothing)
  - Logarithmic rolloff never reaches silence (stays at its maxDistance level beyond it); Linear = full to minDistance, silent at maxDistance
- mod: slime_dj (speakers `sr2dj_Speaker`, spatialBlend 1, doppler 0, priority 32, Linear)

### UnityEngine.AudioClip (UnityEngine.AudioModule)
- methods: `Create(string, int lengthSamples, int channels, int frequency, bool stream)`, `SetData(float[], int offsetSamples)` (by params)
- gotchas: a clip from `AudioClip.Create` is destroyed by the scene load's `Resources.UnloadUnusedAssets` (log: "the AudioClip was destroyed" after zone changes); `Object.set_hideFlags(32 = DontUnloadUnusedAsset)` keeps it. SetData into a playing looping clip used as a ring (glitch-free not verified)
- mod: slime_dj (16 x 0.5 s ring, looping)

### UnityEngine.AudioLowPassFilter (UnityEngine.AudioModule)  [: Behaviour]
- methods: managed `set_cutoffFrequency`, `set_lowpassResonanceQ` kept; `customCutoffCurve` stripped (dump)
- native (UnityPlayer.dll, IDA): all `AudioLowPassFilter::*_Injected` icalls registered (name table 0x181B26EE0, function table right before it at 0x181B263E0, 352 entries). `set_cutoffFrequency` (0x180150110) maps f to `clamp((f - 10) / 21990, 0, 1)` and writes it as the single key of the filter's lowpass-level curve (+0x58); the getter reads key 0
- gotchas: the frequency IS the constant curve, so no default distance curve (the editor's AudioSource "Low-Pass" curve) overrides it. The curve setter refuses an empty curve ("customCutoffCurve must be passed an AnimationCurve with at least 1 element") and throws on null. `Behaviour.set_enabled` switches it off at 22 kHz.
- mod: slime_dj (Muffle: added to every speaker GameObject via `GameObject.AddComponent(Type)`)

