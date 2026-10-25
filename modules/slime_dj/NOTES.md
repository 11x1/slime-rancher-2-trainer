# slime_dj

Plays your own songs from the corral Music Box ("slime speakers"). Overlay tab: Ranch > Slime DJ
(`ui/NOTES.md`). Default on; it does nothing until you press Play or queue a song.

RE: `re/audio.md` (SECTR audio, AudioSource / AudioClip / AudioLowPassFilter), `re/plots.md`
(MusicBoxUpgrader.Apply, patchCorral prefab, CorralBarrier shader, CorralBarrierRipples), `re/unity.md`.

## Adding songs

Put `.ogg` (Vorbis), `.mp3` or `.wav` files in `<game folder>\sr2mods_music\` (next to
`SlimeRancher2.exe`; created on the module's first start). Subfolders are read too, up to 4 levels.
Then press Rescan on the Songs card (or the refresh icon in the tab bar), or reopen the game.
"Open folder" opens it in Explorer.

The list shows "Artist - Title" from the tags (Vorbis comments, ID3v2.2/2.3/2.4 `TIT2`/`TPE1`, WAV
`LIST/INFO` `INAM`/`IART`), else the file name. Files that can't be decoded are listed dimmed with the
reason. At most 2000 songs per folder.

## Files

| file | what |
|---|---|
| `module.cpp` | the module: music boxes, speakers, channels (global queue, Corral Radios), the clip ring, queue, muffle, requests, snapshot |
| `api.h` | public emitter API for other modules (`RegisterEmitter` / `UnregisterEmitter`, `SongPlaying`, `SongName`) |
| `emitters.cpp/.h` | the emitter registry behind `api.h` (module side: `Collect`, `Generation`, `SetPlaying`) |
| `library.cpp/.h` | folder scan on a background thread, per-file cache (path + size + write time), ID3v2 reader |
| `stream.cpp/.h` | one song decoded on a background thread into 0.5 s chunks (bounded queue) |
| `beat.cpp/.h` | beat analysis of the decoded PCM on the decoder thread (bass level + onsets per 10 ms) |
| `walls.cpp/.h` | "Walls dance to the music": beat ripples round the corral energy walls, walls and Air Net glow |
| `radio_menu.cpp/.h` | the Corral Radio playlist menu (runtime uGUI, game-styled), opened by the blue corral button |
| `codecs.c/.h` | the only TU that compiles stb_vorbis, dr_mp3, dr_wav (CMake target `sr2_codecs`, C, `/W0`) behind a small streaming C API |
| `ui/slime_dj_bridge.cpp/.h` | snapshot / request exchange with the overlay tab (same pattern as the sandbox) |

## Design

**Threads.** Library scan and decoding run on their own threads and never touch the game. Everything
IL2CPP runs in one frame callback (`main_thread::AddFrameCallback`) or in posted requests from the tab,
both on the main thread inside `GuardedCallEx`. The one hook, `MusicBoxUpgrader.Apply`, only records its
`this`. A native fault marks the module faulted for the session (handles leaked on purpose); managed
exceptions are logged (3 lines) and the module stops after 20.

**Music boxes (box scan).** Every 2 s while a song is loaded or the tab is open (and on the next frame
after an emitter (un)register or an Apply record). A box counts while `MusicBoxUpgrader.MusicBox` is
active in the hierarchy (corral owns the upgrade, plot loaded). Music Box II (custom_upgrades) keeps the
same GameObject. Perf rule: never `FindObjectsByType` periodically (it walked the whole scene, 8-14 ms on
the ranch). The module keeps GC handles to known upgraders (`ups_`, max 256), and a scan only reads
`MusicBox` + `activeInHierarchy`. The list is filled by
- one scene search (`FindObjectsByType(..., Include, None)`, inactive too) on the first scan after enable
  and when a song starts or the tab opens after being idle;
- the `MusicBoxUpgrader.Apply(Upgrade)` post-hook: every plot passes there for each owned upgrade on load
  and on purchase. The detour records the raw `this` (DetourGuard, deduplicated, max 512); the tick pins it
  with a GC handle next frame. Records from before a disable are dropped on enable.
- Gone upgraders (`m_CachedPtr` 0) are freed at the next scan; a sold / re-bought box is seen through
  `activeInHierarchy`. Without the hook every scan searches the scene (warning).

**Speakers, all in sync.** Every owned box nearby (max 12, `kMaxSpeakers`) and every registered emitter
(max 48) gets a speaker: a GameObject `sr2dj_Speaker` with an `AudioSource`, parented
(`SetParent(…, false)`) to the box's first `SECTR_AudioSource` or the box itself. All speakers play the
same clip, started together, so walking between corrals never restarts or doubles the song. A box that
appears later joins at the clock's `timeSamples`; a speaker whose box is gone is destroyed. If every
speaker of a channel goes (you leave the ranch), the song pauses and resumes when one is back.
- *Clock* (`Master()`): the first live speaker that `isPlaying` (or is paused with the others). A new
  clock continues the play head from the old one's position.
- *Drift check* (every box scan): each speaker's `timeSamples` vs the clock's; more than 30 ms off on two
  scans in a row -> set back to the clock (logged, first 5). A later join can trail by ~20 ms.
- *Switched-off speakers*: Unity refuses `Play()` on a disabled AudioSource and switching an object off
  stops its sources for good. So each scan: speaker GameObject inactive -> `live` false (stopped,
  `clipGen` 0, never the clock, no drift check); active again -> rejoins at the clock. A live speaker that
  should play but `isPlaying` is false (switched off and on between scans) rejoins too.
- *Clip kept through zone changes*: the clip gets `HideFlags.DontUnloadUnusedAsset` (32), otherwise the
  scene load's `Resources.UnloadUnusedAssets` destroyed it.

**Amplify (`amplify`, default off; round 11).** Boxes in sync already add up physically where ranges
overlap. The toggle adds, per speaker, for each other *audible* speaker of the same channel within its
hearing range (positions from `Transform.get_position` every box scan; round 12: silent emitters and
speakers of another radio no longer count): +15 % volume (up to 1.6x, +4 dB) and +10 % range (up to 1.3x).
An amplified speaker has `minDistance` = a tenth of its range (instead of a fifth), so each speaker is
loudest next to it and the song is heard from every speaker instead of one wide full-volume blob (round 10
had +25 % range up to 2x, which made the full-volume zones overlap). Volume above 1 (Unity clamps
`AudioSource.volume`): every channel has a PCM gain (`Current::gain`) baked into the chunks `Fill` writes,
= the loudest wanted speaker volume of the channel (`vol x volScale x boost x muffle x sum cap`, 1..2).

*Round 12, "sound is crackling a bit"*. Two causes, both fixed:
- The old gain was soft-limited above 0.89 by a tanh knee: on mastered songs (peaks near 0 dBFS) every
  loud peak was bent (audible distortion), and the gain stepped at chunk edges while the speakers divided
  it out a frame off. Now `Fill` holds one decoded chunk back (`pend`), so it sees the next chunk too:
  the gain ramps linearly across the chunk from where the last one ended (`chunkG0` -> `chunkG1` per ring
  slot) to a value that keeps this chunk and the next under 0.97 (`kPeakCeiling`), rises at most 0.05 per
  chunk, falls as fast as needed, never below 1. No sample is shaped or clipped. Speakers divide by the
  ramp's value at the exact play position (`GainAt`). A song mastered at full scale simply gets no gain.
- Speakers of one channel play the same samples in sync, so at the listener they add up as amplitudes
  (five speakers at full volume nearby = 5x; the mixer clips). *Listener sum cap*: every frame `TickMix`
  estimates the sum at the camera (each speaker's wanted volume x Unity's linear rolloff at its distance,
  panning ignored) per channel and scales all its speakers so the sum stays under 1 (`kSumMax`); the cut is
  immediate, the release 0.6 s. Logged once: `N speaker(s) of '...' add up to X.XXx full scale here;
  turned down to stay clean`.

**Silent emitters stop (round 12).** An emitter registered with volume 0 (a booth or speaker stack
switched off, a Boombox playing its own track) does not play at all (`Silent`): stopped, clip dropped,
never the clock, not counted by Amplify, the sum cap or `SongPlaying`; it rejoins at the clock when it is
registered with more (the register bumps the generation: next frame's box scan). Before, those speakers
played at volume 0 and still boosted their neighbours ("sound seems to come from dj table only ... not
enabled and has no slime but still plays").

**3D settings.** `maxDistance` = `range` (default 60 m, 10-200) x emitter `rangeScale` x Amplify boost;
`minDistance` = a fifth of it; rolloff Linear (Logarithmic never reaches silence), `spatialBlend` 1,
doppler 0, priority 32. The vanilla cue's Min/MaxDistance / Falloff are only logged (far too short).

**Volume.** `AudioSource.volume = volume x music bus x emitter volumeScale x Amplify x muffle`, clamped to
1 by Unity (setting tops out at 100 %). Music bus = first `SECTR_AudioBus` whose name contains "music"
under `SECTR_AudioSystem.MasterBus` (system found with `FindFirstObjectByType`, retried every 5 s; bus tree
logged); its `EffectiveVolume` follows the game's music slider, 0 while muted. No music bus -> master bus.

**Pause.** Speakers pause while the vanilla cue's bus reports `Paused` (pause menu), while you pause,
while buffering, and while the channel has no speaker.

**Vanilla tune (`mute_vanilla`, default on).** While a song is loaded and not paused by you, every
`SECTR_AudioSource` under the box gets `Volume = 0` (old value restored) and a plain `AudioSource` under it
`mute = true` (only if not muted already). Back on Pause, Stop, queue end, box losing its speaker, module
off. Silent between two songs. Per speaker: silenced only while its own channel has a song.

### Streaming clip

One `AudioClip.Create(name, 16 chunks, channels, rate, stream: false)` per song, looping: an 8 s ring
(`kRingChunks` 16 x 0.5 s). The decoder keeps up to 20 chunks (10 s) ahead; the main thread copies chunks
in with `AudioClip.SetData(float[], offset)` (one managed `float[]` per song, reused, GC handle), up to 3
per frame, never into the chunk the play head is in. Play head = the clock's `timeSamples`, unwrapped
(delta modulo the ring). Before the first `Play` the ring is filled (at least 2 s). After the decoder is
done the ring is padded with silence and the song ends when the play head passes the last real frame.
Less than 1/8 s written ahead -> speakers pause ("Buffering...") until 1 s is ready.

Limits:
- Memory per song is constant: ~8 s in the clip + up to 10.5 s decoded floats (48 kHz stereo: 3 + 4 MB).
  No length or size limit.
- More than 2 channels mixed down to stereo (front pair + centre at -3 dB); mono stays mono; 8-192 kHz.
- A main-thread stall over ~7 s would replay stale ring audio (loading screens still tick the dispatcher).
- MP3 length comes from a header-only scan at folder read (`drmp3_get_pcm_frame_count`, no synthesis);
  slow the first time for a big folder, cached afterwards.

**Seek** (global song only). The Now playing bar sends `Op::Seek` (seconds) on release. A new decoder
starts at that frame (`Stream::Start(path, startFrame)` -> `sr2_decoder_seek`: `stb_vorbis_seek`,
`drmp3_seek_to_pcm_frame`, `drwav_seek_to_pcm_frame`) with a new clip; `Current::base` = the song frame
the stream started at, shown position `base + playPos`. All speakers restart on the new clip in the same
frame. The player's pause is kept; a seek lands at most 0.5 s before the end. MP3 has no seek table, so a
far seek in a long MP3 takes a moment ("Loading...").

**Queue and shuffle.** Click a song: plays now (queue kept). +: appended (starts it if nothing plays).
Song end: the queue's first; empty queue + `shuffle` (default on) -> a random playable song (not the one
that just played); else stop. Play with nothing loaded starts the same way. Three failed songs in a row
stop playback.

### Emitters (`api.h`, stable API for other modules)

```cpp
#include "modules/slime_dj/api.h"
namespace dj = sr2::modules::slime_dj;
dj::RegisterEmitter(go, dj::EmitterKind::kBooth);            // or kSpeaker / kRadio
dj::RegisterEmitter(go, dj::EmitterKind::kRadio, 0.5f, 0.8f); // half the hearing range, 80 % volume
dj::UnregisterEmitter(go);
bool on = dj::SongPlaying(); std::string name = dj::SongName();  // any thread; the global song
std::string why; dj::OpenRadioMenu(corralLandPlot, &why);       // main thread; the Corral Radio menu
```

- `obj` is a GameObject or any Component on it. Slime DJ adds a child `sr2dj_Speaker` (same 3D setup as a
  box speaker) in sync with the boxes. Emitters play anywhere (also off the ranch).
- `rangeScale` (0.1-4) x Hearing range, `volumeScale` (0-2; above 1 through the channel gain) x volume;
  0 = the speaker stops. The kind is only logged.
- Register again to change kind / scales. Destroyed objects are dropped at the next scan anyway.
- Main thread, not inside a detour. Register takes a GC handle; Unregister only queues, the tick frees it.
  Registrations survive the module being off. Max 48 emitter speakers, 256 registrations.
- An emitter may be registered while inactive; its speaker joins when the object is active.
- Users: custom_upgrades `plot_props.cpp` (Music Box II second speaker, the cloned `Music` model whose
  vanilla audio was stripped), `dj_buildables.cpp` (DJ Booth's two speakers, DJ Speakers, Slime Radios,
  every placed Boombox's `model_boombox`, volume 0 while the Boombox plays its own track).

### Channels: global queue and Corral Radios

Playback is split into `Channel`s: `global_` (the queue) and one per loaded corral that owns the **Corral
Radio** (custom_upgrades plot upgrade `kCorralRadio`, corral, needs the Music Box, 1500). Each channel has
its own `Current` (stream, clip ring, clock, end / buffering); `clipGen_` counts across channels. Every
speaker has `ch` and `corral` (index into `corrals_`), assigned every box scan by `AssignSpeakers`: a box
speaker belongs to its corral, an emitter to the corral whose footprint holds it (`kEmitterHalf` 5.5 m
around the plot root along the plot axes); radio corral -> that radio's channel, else global. A speaker
that changes channel stops, drops its clip and joins the other channel at its clock in `TickSong`.

- *Corrals* (`BuildCorrals`, every scan): per active box its `MusicBoxUpgrader` (plot root): position +
  `Transform.right / forward`, High Walls (`WallUpgrader.UpgradeWalls` active), Air Net (one of
  `AirNetUpgrader.AirNets` active), and with custom upgrades the `LandPlot` (`GetComponentInParent`),
  `custom_upgrades::Owns(plot, kCorralRadio)` and `custom_upgrades::PlotKey`.
- *Radios* (`SyncRadios`): a channel per radio corral found; dropped (speakers back to global) when the
  corral is not found (zone unloaded, radio sold, custom upgrades off). Max 32 radios.
- *Playlist storage*: custom_upgrades per-plot sidecar value `slime_dj.radio`, lines `v=1`, `on=0|1`,
  `shuffle=0|1`, `index=N`, then `song=<file relative to the music folder>` per entry (up to 500). Read
  when the radio appears, written after every change and song change, saved with the next game save.
- *Slots* (`AllocateSlots`): at most `kMaxRadioStreams` = 4 radios decode at once (global not counted). A
  radio keeps its slot while on, non-empty and loaded; free slots go to the nearest waiting radios. A radio
  with a slot and no song resumes at its last entry (from the song's start). Waiting radios show "Waiting:
  4 radios play already". Nothing starts before the music folder has been read.
- *Order*: next entry, wrapping; radio shuffle -> a random other entry. Missing / unplayable files are
  skipped; none playable or three failures in a row -> the radio switches off.
- *After a load*: with custom upgrades on, an Apply record triggers one probe scan even while idle, so a
  radio left on in the save starts.
- Not per radio: `SongPlaying` / `SongName` follow the global song only. Walls dance per channel (round 12).

### Corral Radio menu (round 11, `radio_menu.cpp`)

User: "Add a button in the last empty corner (color it blue; like you did for silo's button) and let the
button open a menu to build a queue from". custom_upgrades builds the button (a second `SiloButtons` with
`RadioButtonLook`: clone of the corral Plort Collector button, blue, on every corral that owns the Corral
Radio, at the free (+x, -z) corner; see `modules/custom_upgrades/NOTES.md`). Its press calls `api.h
OpenRadioMenu(plot, &why)`: it only records the plot key (`RequestMenu`; refused with a reason when Slime
DJ is off, the menu unavailable or already open; custom_upgrades shows the reason above the button). The
next tick counts as active (box scan now, so the radio channel exists), then opens the menu.

- *Look*: a runtime uGUI panel like custom_upgrades' Sell-All receipt (same palette, SDF shapes, the HUD's
  TMP font, overlay Canvas 1920 x 1080 + UIInputLocker: game paused, cursor free). Title, corral name,
  status line, two virtual lists (Songs sorted by name; the playlist numbered, the playing entry pink with
  `>`), a feedback line, buttons Radio on/off, Shuffle, Clear (press twice), Done under the songs and Play,
  Up, Down, Remove for the selected entry under the playlist, key hints. 5..11 rows per list (canvas
  height); only changed texts / colours are written.
- *Input*: polled (InputSystem devices, Toggle clicks), 0.3 s grace; see the `radio_menu.h` header for the
  key map (keyboard, pad, mouse wheel over a list, click a song = add, click an entry = select).
- *Edits* go through `HandleRadio` (the tab's requests), so the sidecar is written and playback reacts the
  same way; the menu's state refreshes every 0.25 s and after each action.
- While open the game is paused, so the speakers pause too (status says so); the radio plays after Done.
- No search box (a TMP input field was not attempted); Page Up / Down (pad LT / RT), Home / End and the
  wheel move through long lists.
- Closed on Done / Esc / pad B / backdrop click, module off, and before a fault stops the module.

### Muffle behind High Walls (`muffle_walls`, default on)

Every speaker gets a Unity `AudioLowPassFilter` on its GameObject (off, cutoff 22 kHz). Every frame
(`TickMuffle`): camera position (the AudioListener) against each walled corral of the last scan. Outside
(more than `kCorralHalf` 4.9 m from the centre along plot x or z, or below -3 m / above +8 m of the plot
root): High Walls + Air Net 750 Hz / 60 % volume, High Walls 2 kHz / 80 %, Air Net alone 6.5 kHz / 92 %;
inside, or no walls / net: clear. Smoothed (log cutoff and volume, tau 0.35 s); filter disabled
(`Behaviour.enabled`) while back at 22 kHz. Volume factor applied in `TickMix`. Walls / net read every
box scan (a purchase takes effect within 2 s). Emitters in the corral footprint are muffled with it. If
`AddComponent` fails, Muffle only lowers the volume (no PCM filter: the global clip is shared by speakers
inside and outside a corral). Why the cutoff can't be overridden by a distance curve: `re/audio.md`.

### Walls dance to the music (`dance_walls`, default off; `dance_intensity` 10-200 %)

*Analysis* (`beat.cpp`, no libraries, no game calls; constants there and in `beat.h`). The decoder thread
feeds every decoded chunk (after the down-mix) into the stream's `BeatTrack`, seconds ahead of the play
head: mono mix, Hann window ~23 ms (1024 frames at 44.1/48 kHz), one radix-2 FFT per 10 ms hop, one 16-bit
entry per hop (4 bytes: level, onset, energy; 24 KB per minute, capped at 6 hours; after a seek `Reset(rate, startFrame)`): a 0..1 bass
level (35-160 Hz, adaptive peak / floor window, 0 in breaks) and an onset strength (spectral flux + bass
rise above an adaptive threshold, at least 180 ms apart). Tuned offline on the user's hardtekk songs
(170-205 onsets/min = the kick); a synthetic 120 BPM track gives every kick.

*Reading it* (`TickDancer` -> `DanceSample` per channel, every frame): song frame = `base + playPos` minus
40 ms (`kDanceLagSeconds`; `timeSamples` runs ahead of what is heard); `Query(last, now)` gives the level at
`now` and the strongest onset in between (no beat lost at low fps, none repeated while paused). Nothing
read while paused or past the end: the walls settle and are restored.

*Which corrals* (`SyncDancer`, every box scan and when the setting goes on): music box speakers (not
emitters) playing their channel's clip whose box is within hearing range (Amplify included) of the camera.
Their `MusicBoxUpgrader` is the plot root, which also carries `WallUpgrader` / `AirNetUpgrader`. Max 12.
Round 12 ("the walls dont dance to the corral's specific radio"): each corral follows its box's channel,
the global queue or its own Corral Radio. `WallDancer::Target{up, group, boost}`: `group` = the channel's
address (only compared, never dereferenced); the dancer keeps one `Groove` (level, pulse, chase, hue,
energy, tempo) per group, so corrals of the same song move together and a radio corral has its own beat.
*Speakers boost the walls* (round 12 idea: "If we add speaker buildables/toy to a plot, it should amplify
the walls moving aswell"): every audible emitter inside the corral's footprint on the same channel
(speaker stacks, Boomboxes, Slime Radios, DJ Booth speakers, the Music Box II prop) adds 0.2 to an
intensity factor (max 2x); effective intensity = setting x factor, at most 2.5 (or the setting if
higher).

*Calm parts* (round 11; "on a chill part of the songs, the walls still freak out like its a BANGER part").
The bass level is relative (falling peak / rising floor), so a quiet part swung 0..1 like a drop. The
analysis now also stores an energy per hop (byte 2 of the entry, entries are 4 bytes): bass (35-160 Hz) and
broadband loudness, each a 1 s power average in dB, placed between 12 dB and 2 dB under a reference = the
louder of the song's loudest part so far (falls 0.02 dB/s) and an absolute one (bass -8 dB, broadband
-11 dB mean square: a quiet intro is calm before the drop has been heard); the lower of the two counts.
`WallDancer::Tick` smooths it (0.4 s) and scales the level, the beat pulse and the colour step by
0.15 + 0.85 x energy; below energy 0.35 no beat ripples. Checked offline (scratch harness over the user's
songs): hardstyle drops 1.0, breakdowns / piano intros 0.0, quieter verses 0.3-0.7.

*What is animated*: the active walls root's renderers named `corralField*` (4, or 8 with High Walls; other
renderers skipped and named in the log) and the active Air Net's "NetTrigger" renderer (never moved or
scaled: it carries the net's collider). `_Health` / `_HitTime` are never touched. Prefab and shader
details: `re/plots.md`.

On every beat of its song: one ripple per animated corral (max 4 per frame, the global ring has 4 slots;
more corrals -> round robin) through `CorralBarrierRipples.SpawnRippleHere` (found with `FindObjectOfType` at a box scan,
at most every 10 s while unknown, GC handle). Centre: middle of one side (between two neighbouring lower
field corners), 1 m inside, 1.7 m up (3.4 m with High Walls), so the side bulges outwards; the side
follows the chase angle (a quarter turn per beat). Without 4 lower fields the plot gets no ripples.

Per frame, per cached renderer: `MaterialPropertyBlock.Clear` + `Renderer.GetPropertyBlock(work)` (keeps
what the game put there) + `SetFloat` / `SetColor` + `SetPropertyBlock` (direct native calls, one shared
block). Driven by the smoothed bass level, a beat pulse and a chase angle (a quarter turn round the corral
per beat): `_Intensity` (glow), `_Color` (towards a colour wheel, hue step per beat), and on wall fields
`_RipplePushScale` / `_RippleStrength` / `_RippleSize` (bulges pump with the bass) and
`_RippleFadeDuration` (follows the tempo so a ripple fades before its ring slot is reused). Drive D =
I x (1 + 0.75 max(0, I - 1)) for intensity I (100 % = 1, 200 % = 3.5). Formulas and constants:
`walls.cpp` (`kHueStep`, `kPush*`, ...). Vacpack shots through a dancing wall use the same block and bulge
more.
Cached per plot (GC handles for renderers, a per-renderer snapshot block from `GetPropertyBlock` before the
first change, the 4 ripple points); rebuilt when the active walls root or net changes. Restore =
`SetPropertyBlock(snapshot)` when the song pauses (after settling), stops, the plot leaves range, the
setting goes off, or the module is disabled. Ripples in flight fade on their own. After a native fault the
walls are left as they are.

## Settings

| key | default | |
|---|---|---|
| `volume` | 1.0 (100 %) | 0-100 %, on top of the game's music volume |
| `range` | 60 m | hearing range: full volume to a fifth of it, silent at it (10-200 m) |
| `amplify` | off | each other audible speaker of the channel in range: +15 % volume (to 1.6x) and +10 % range (to 1.3x); speakers loudest up close; capped at full scale at the listener |
| `shuffle` | on | random songs when the queue is empty |
| `mute_vanilla` | on | silence the music box's own tune while a song plays |
| `muffle_walls` | on | music from a corral with High Walls (and/or Air Net) sounds muffled from outside |
| `dance_walls` | off | a ripple runs round the corral walls on every beat of the song the corral plays (queue or its Corral Radio); walls and Air Net glow and change colour (corrals within hearing range); speakers in the corral make it stronger |
| `dance_intensity` | 1.0 (100 %) | 10-200 %, steeper above 100 % (shown while `dance_walls` is on) |

## Log lines

All prefixed `slime_dj:`; grep for these when reading `sr2mods.log`:
- folder / bus: `music folder '…': N song(s) (M newly read, K not playable) in X ms`, `music bus '…' (buses: …)`
- speakers: `speaker on music box '…' at '…': N vanilla source(s), cue '…' a-b m …` (first; later ones
  shorter), `speaker on emitter '…' (kind K, range xA, volume xB[, switched off for now])`, `N music box(es),
  M emitter(s) registered; speakers: …` (on a change), `N speaker(s) had stopped …; rejoined in sync`,
  `speaker N ms off the others, moved back in sync`
- playback: `playing '…' (file)`, `seek to N s in '…'`
- perf: `box scans in the last 60 s: N (avg X ms, max Y ms); scene searches K (max Z ms); C corral(s) known,
  S speaker(s)` (once a minute while scans ran), `no MusicBoxUpgrader.Apply hook (…)`
- walls dance: `walls dance at 'patchCorral…': N wall field(s) …, M net renderer(s) …; wall _Intensity … ;
  K ripple point(s)` (per corral built, first 24), `walls dance: first beat ripple(s) spawned …` (once),
  `… no CorralBarrierRipples in the scene yet`, `walls dance without beat ripples (…)`, `walls dance at N
  corral(s) (M to their Corral Radio, K boosted by speakers inside)`, `walls dance stopped (setting off|no
  song); walls restored`, `'Walls dance to the music' unavailable (…)`
- mix: `N speaker(s) of '...' add up to X.XXx full scale here; turned down to stay clean (logged once)`
- radio menu: `radio menu ready[; NOT available: ...]`, `radio menu for 'Corral NNNN' (plot '...')[: its radio is not
  found yet (scanning)]`, `radio menu opened (N rows per list, M song(s), font ...)` (first), `radio menu closed (why)`,
  `radio menu not opened: ...`
- corrals / muffle / radio: `N corral(s) with a music box, M with High Walls / Air Net …`, `'Muffle behind High
  Walls' unavailable (…)`, `AudioLowPassFilter could not be added; …`, `Corral Radio unavailable (…)`,
  `radio 'Corral NNNN' (plot '…'): N song(s) in its playlist, on|off[, shuffle]`, `radio '…' playing|switched
  on|off|gone|stopped …`, `N speaker(s) switched between the global queue and a corral radio` (first 20)

## Not verified in game yet

Decoders tested offline (Vorbis, MP3 with ID3v2.3/2.4, mono 22 kHz WAV with INFO, 5.1 WAV, a broken file).
To check in game: SetData into the playing clip is click-free; the tune comes from a `SECTR_AudioSource`
and `Volume = 0` silences it; music bus name / slider / pause-menu pause; 60 m linear range, Amplify, the
Music Box II second speaker, seeking + sync; box cache (no `frame:slime_dj took` warnings, minute line
`scene searches 0`, new boxes within ~2 s); muffle audible, footprint test, transition speed; Corral Radio
sidecar round trip, four radios + global (five decoder threads), slot hand-over, channel switch on buy /
sell; walls dance ripples bulge and travel, `_Intensity` / `_Color` visible on both shaders, beat in time
(40 ms lag), everything restored after Stop / Pause / setting off / leaving the ranch. Round 11: calm walls in
quiet parts, Amplify above full volume (PCM gain, no clipping / pumping at chunk edges), the Corral Radio menu
(build, input, input lock released on every close path).

## History

- Round 7: switched-off speakers fix (inactive Music Box II props after a teleport froze the clock).
- Round 8: walls dance first version (y-scale bob, since replaced by beat ripples).
- Round 9: beat analysis retuned for hardtekk; beat ripples through `CorralBarrierRipples`.
- Perf round (2026-10-24): box scan from scene search every 2 s to the Apply-hook cache.
- Corral Radio channels and Muffle behind High Walls (round 4 ideas).
- Round 11: calm walls in quiet parts (energy), Amplify louder than full volume and spatial, the Corral Radio
  menu behind the blue corral button.
- Round 12: crackle-free gain (look-ahead ramp, peak ceiling) and the listener sum cap; silent emitters stop;
  walls dance to each corral's own Corral Radio; speakers in a corral boost its walls.
