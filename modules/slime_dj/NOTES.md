# slime_dj

Plays your own songs from the corral Music Box ("slime speakers"). Overlay tab: Ranch > Slime DJ
(`ui/NOTES.md`). Default on; it does nothing until you press Play or queue a song.

## Adding songs

Put `.ogg` (Vorbis), `.mp3` or `.wav` files in `<game folder>\sr2mods_music\` (next to
`SlimeRancher2.exe`; the module creates the folder on its first start). Subfolders are read too,
up to 4 levels. Then press Rescan on the Songs card (or the refresh icon in the tab bar), or reopen the game.
"Open folder" next to it opens the folder in Explorer.

The list shows "Artist - Title" from the file's tags (Vorbis comments, ID3v2.2/2.3/2.4
`TIT2`/`TPE1`, WAV `LIST/INFO` `INAM`/`IART`), else the file name. Files that can't be decoded
are listed dimmed with the reason.

## Files

| file | what |
|---|---|
| `module.cpp` | the module: music boxes, speakers, the clip ring, queue, requests, snapshot |
| `api.h` | public emitter API for other modules (`RegisterEmitter` / `UnregisterEmitter`, `SongPlaying`, `SongName`) |
| `emitters.cpp/.h` | the emitter registry behind `api.h` (module side: `Collect`, `Generation`, `SetPlaying`) |
| `library.cpp/.h` | folder scan on a background thread, per-file cache (path + size + write time), ID3v2 reader |
| `stream.cpp/.h` | one song being decoded on a background thread into 0.5 s chunks (bounded queue) |
| `beat.cpp/.h` | beat analysis of the decoded PCM on the decoder thread (bass level + onsets per 10 ms) |
| `walls.cpp/.h` | "Walls dance to the music": beat ripples round the corral energy walls, walls and Air Net glow with the analysis |
| `codecs.c/.h` | the only TU that compiles stb_vorbis, dr_mp3, dr_wav (CMake target `sr2_codecs`, C, `/W0`) behind a small streaming C API |
| `ui/slime_dj_bridge.cpp/.h` | snapshot / request exchange with the overlay tab (same pattern as the sandbox) |

## Design

**Threads.** Library scan and decoding run on their own threads and never touch the game.
Everything IL2CPP runs in one frame callback (`main_thread::AddFrameCallback`) or in posted
requests from the tab, both on the Unity main thread inside `GuardedCallEx`. One hook,
`MusicBoxUpgrader.Apply`, only records its `this` (next section).
A native fault marks the module faulted and stops it for the session (handles are leaked then,
on purpose); managed exceptions are logged (3 lines) and the module stops after 20.

**Music boxes.** A box scan runs every 2 s while a song is loaded or the tab is open (and right
away after an emitter (un)register or an upgrade applied). A box counts while
`MusicBoxUpgrader.MusicBox` (the GameObject the vanilla Music Box upgrade switches on) is active in
the hierarchy, i.e. the corral owns the upgrade and the plot is loaded. Music Box II
(custom_upgrades) keeps the same GameObject, so it plays too.

*Where the upgraders come from (perf round, 2026-10-24).* The box scan used to call
`Object.FindObjectsByType(MusicBoxUpgrader, Exclude, None)` every time. That walks every object of
the loaded scenes: the user log had `perf: frame:slime_dj took 8-14 ms` every 2 s on the ranch (with
1 box + 1 emitter as much as with 9 emitters) and ~2.5 ms off the ranch, i.e. the cost followed the
scene size, not the speaker count. Now the module keeps GC handles to the known upgraders (`ups_`,
at most 256) and the scan only reads `MusicBox` + `activeInHierarchy` of each (microseconds).
The list is filled by
- one scene search (`FindObjectsByType(..., Include, None)`, inactive ones too, so plots that are
  switched off and on are kept) on the first scan after the module is enabled and whenever a song
  starts or the tab opens after being idle (a click, so a one-off hitch is fine);
- the `MusicBoxUpgrader.Apply(Upgrade)` hook (0x98A810, its own body, 0x2A bytes; nothing else hooks
  it). `LandPlot.ApplyUpgrades` (0x9887E0, checked in IDA) calls `Apply` on every `PlotUpgrader` of
  the plot for every owned upgrade (and `MusicBoxUpgrader.Apply` itself does
  `if (upgrade == MUSIC_BOX) MusicBox.SetActive(true)`), on load and on every purchase. So a corral
  with a music box always passes here when its plot appears (zone load, new plot built + box
  bought), and a purchase triggers a scan in the next frame. The detour calls the original, then
  records the raw `this` (DetourGuard, main thread, deduplicated, at most 512); the tick pins it with
  a GC handle in the next frame. Records from before a disable are dropped on enable.
- Gone upgraders (plot demolished, scene unloaded: `m_CachedPtr` 0) are freed at the next scan.
- A box sold (MusicBox switched off) or switched back on (Music Box II purchase) is seen at the
  next scan through `activeInHierarchy`, as before.
Without the hook (not created) every scan searches the scene, as before (a warning says so).
`slime_dj: box scans in the last 60 s: N (avg X ms, max Y ms); scene searches K (max Z ms); C
corral(s) known, S speaker(s)` is logged once a minute while scans ran.

**Several boxes: all in sync.** Every owned box nearby (at most 12) and every registered emitter
(at most 48, see below) gets a speaker: a new
GameObject `sr2dj_Speaker` with a Unity `AudioSource`, parented (`SetParent(…, false)`) to the
box's first `SECTR_AudioSource` (where the vanilla tune comes from) or to the box itself. All
speakers play the same clip and were started together, so the song is positional at every corral
and walking between corrals never restarts or doubles it. A box that appears later (sold and
bought again, region loaded) joins at the current `timeSamples`; a speaker whose box is gone is
destroyed. The first live speaker is the clock (see below); if it goes, the next one takes over.
If every box goes (you leave the ranch), the song pauses where it was and resumes when a box is
back.

Drift check (every box scan): every speaker's `timeSamples` is compared with the clock speaker's
(all readings come from the same audio update). `Play()` starts at the next audio update, so a
speaker that joined later can trail by a mix buffer (~20 ms); one that is more than 30 ms off on
two scans in a row is set back to the clock's position (logged, first 5 times).

**Amplify (setting `amplify`, default off).** Why a toggle and not something else: boxes in sync
already add up physically where their ranges overlap (two coherent sources), so "play all in
sync" is always on. The toggle adds an explicit boost on top: each other speaker within a
speaker's hearing range (positions from `Transform.get_position` every box scan) adds 25 % to its
volume and its hearing range, up to 2x. The volume part only helps while the speaker is below
full volume (Unity clamps `AudioSource.volume` to 1, so Volume x the game's music slider must be
under 100 %); the range part always works, which with linear rolloff also makes it louder at
any given distance.

**Emitters (`api.h`, stable API for other modules).** Any GameObject can play the song. Users: the
Music Box II prop (custom_upgrades `plot_props`) and the buildable DJ Booth (2 emitters), DJ Speakers and Slime
Radio (custom_upgrades `dj_buildables.cpp`, NOTES "Slime DJ buildables"):

```cpp
#include "modules/slime_dj/api.h"
namespace dj = sr2::modules::slime_dj;
dj::RegisterEmitter(go, dj::EmitterKind::kBooth);            // or kSpeaker / kRadio
dj::RegisterEmitter(go, dj::EmitterKind::kRadio, 0.5f, 0.8f); // half the hearing range, 80 % volume
dj::UnregisterEmitter(go);
bool on = dj::SongPlaying(); std::string name = dj::SongName();  // any thread
```

- `obj` is a GameObject or any Component on it. Slime DJ adds a child `sr2dj_Speaker` with its own
  `AudioSource` (same 3D setup as a music box speaker) that plays the shared song clip in sync
  with every music box: started together, or joined at the clock speaker's `timeSamples`, and
  kept there by the drift check. Emitters play anywhere (also off the ranch), so a song keeps
  playing while at least one emitter or music box exists.
- `rangeScale` (0.1-4) multiplies the Hearing range setting, `volumeScale` (0-1) the volume. The
  kind is informative (logged); it does not change the sound today.
- Register again with the same object to change kind / scales. Unregister when the object goes;
  objects Unity destroyed are dropped at the next scan anyway (every 2 s while a song plays or
  the tab is open; a (un)register triggers a scan on the next frame).
- Main thread, not inside a detour. Register takes a GC handle; Unregister only queues it, the
  slime_dj tick frees it. Registrations survive the module being off and apply when it is on.
- At most 48 emitter speakers (plus 12 music boxes); 256 registrations.
- Current users: custom_upgrades `plot_props.cpp` registers the Music Box II second speaker
  (the cloned `Music` model, whose vanilla audio was stripped with the game scripts);
  `dj_buildables.cpp` the DJ Booth's two speakers, DJ Speakers, Slime Radios and every plain placed
  Boombox (its `model_boombox`; volume 0 while the Boombox plays its own track).
- An emitter may be registered while its object is inactive (a plot or actor built while its scene
  is still off). Its speaker waits and joins when the object is active (next section).

**Switched-off speakers (round 7: "on one pen only from the initial speaker and on the other from
the second").** Unity refuses `Play()` on a disabled AudioSource, and switching an object off stops
its sources for good (`playOnAwake` is off on ours, so nothing starts them again). The user log
showed the Music Box II props rebuilt with `activeInHierarchy false` after a teleport back to the
ranch, then `speaker 3989 ms off the others` every 4 s: a speaker that never played, compared with
the clock every 2 s (4 s = two scans). As the clock it even dragged the playing speakers back to its
frozen position. Which pen's speaker was stuck depended on what was built / toggled when, hence
"inconsistent". Now every box scan checks each speaker: its GameObject inactive in the hierarchy =
`live` false: stopped, out of the song (`clipGen` 0), never the clock, no drift check; once active
again it rejoins at the clock's `timeSamples`. A live speaker that should play but
`AudioSource.isPlaying` is false (switched off and on between two scans, e.g. the Music Box
GameObject during a Music Box -> Music Box II sale + purchase) rejoins too (checked from the scan
after its join). The clock (`Master()`) is the first live speaker that `isPlaying` (or is paused
with the others), so the play head never freezes on a stopped one; a new clock continues the play
head from the old one's position (all speakers are in sync).

**Clip kept through zone changes.** `AudioClip.Create`'s clip gets `HideFlags.DontUnloadUnusedAsset`
(32): the user log had `failed: the AudioClip was destroyed` right after zone changes (the scene
load's `Resources.UnloadUnusedAssets`), which skipped the song.

**3D settings.** Setting `range` (Hearing range, default 60 m, 10-200 m) x the emitter's
`rangeScale` x Amplify's boost = `maxDistance`; `minDistance` = a fifth of it; rolloff Linear (full
volume up to minDistance, silent at maxDistance; Unity's Logarithmic never reaches silence and
stays at its maxDistance level forever beyond it), `spatialBlend` 1, doppler 0, priority 32. The
vanilla cue's (`SECTR_AudioSource.Cue`) MinDistance / MaxDistance / Falloff are only logged now:
users found the copied range far too short for a song.

**Streaming clip (large files).** One `AudioClip.Create(name, 16 chunks, channels, rate,
stream: false)` per song, looping: an 8 s ring. The decoder thread keeps up to 20 chunks (10 s)
decoded ahead; the main thread copies chunks into the ring with `AudioClip.SetData(float[],
offset)` (one managed `float[]` per song, reused, held by a GC handle), up to 3 per frame, never
into the 0.5 s chunk the play head is in. The play head is the first speaker's
`AudioSource.timeSamples`, unwrapped (delta modulo the ring). Before the first `Play` the ring is
filled completely (at least 2 s). After the decoder is done the ring is padded with silence and
the song ends when the play head passes the last real frame. If the decoder ever falls behind
(less than 1/8 s written ahead) the speakers pause ("Buffering...") until 1 s is ready.

Limits that follow:
- Memory per song is constant: about 8 s in the clip plus up to 10.5 s of decoded floats (at
  48 kHz stereo: 3 MB + 4 MB), whatever the length. No length or file-size limit.
- More than 2 channels are mixed down to stereo (front pair plus centre at -3 dB); mono stays
  mono. Sample rates 8-192 kHz (Unity resamples).
- A main-thread stall longer than ~7 s (the ring minus the guard chunk) would replay stale ring
  audio; the game's loading screens still tick the dispatcher, so this should not happen.
- MP3 length comes from a header-only scan when the folder is read (`drmp3_get_pcm_frame_count`
  with minimp3's no-synthesis path), so a big folder of MP3s takes a moment the first time; the
  results are cached for rescans.
- At most 2000 songs per folder.

**Vanilla tune.** While a song is loaded and not paused by you (setting `mute_vanilla`, default
on), every `SECTR_AudioSource` under the box gets `Volume = 0` (the old value is restored), and a
plain `AudioSource` under it `mute = true` (only if it was not muted already). It comes back on
Pause, Stop, when the queue ends, when the box loses its speaker, and when the module is turned
off. It stays silent between two songs.

**Volume.** `AudioSource.volume = volume setting x music bus x emitter volumeScale x Amplify boost`,
clamped to 1 by Unity (so the setting tops out at 100 %; it used to allow 150 %, which never did
anything). The music bus is the first
`SECTR_AudioBus` whose name contains "music" under `SECTR_AudioSystem.MasterBus` (the system is
found with `FindFirstObjectByType` once, retried every 5 s until found; the bus tree is logged).
Its `EffectiveVolume` is what the game's music volume slider drives
(`ChannelVolumeController._targetChannel`); 0 while the bus is muted. Without a music bus the
master bus volume is used.

**Pause.** The speakers pause while the vanilla cue's bus reports `Paused` (game pause menu),
while you pause, while buffering, and while no box is nearby.

**Seek.** The Now playing bar is a seek bar (tab sends `Op::Seek` with seconds on release). The
module starts a new decoder at that frame (`Stream::Start(path, startFrame)` ->
`sr2_decoder_seek`: `stb_vorbis_seek`, `drmp3_seek_to_pcm_frame`, `drwav_seek_to_pcm_frame`) and a
new clip; `Current::base` is the song frame the stream started at, so ring positions stay
relative and the shown position is `base + playPos`. All speakers restart on the new clip in the
same frame, so they stay in sync. The player's pause is kept. A seek lands at most 0.5 s before
the end. MP3 has no seek table: drmp3 decodes up to the target on the decoder thread, so a far
seek in a long MP3 takes a moment ("Loading...").

**Queue and shuffle.** Click a song: plays now (the queue stays). +: appended to the queue (starts
it if nothing plays). When a song ends: the queue's first; if the queue is empty and `shuffle` is
on (default), a random playable song (not the one that just played); otherwise playback stops.
Play with nothing loaded starts the same way. Three failed songs in a row stop playback.

**Walls dance to the music (setting `dance_walls`, default off; `dance_intensity` 10-200 %).**
User request: "make the plot futuristic walls wave based on beat from the speakers on custom music
... same for the roof net if bought, i want to have a party with my slimes".

*Analysis* (`beat.cpp`, no libraries, no game calls). The decoder thread feeds every decoded chunk
(after the down-mix) into the stream's `BeatTrack`, so it runs seconds ahead of the play head and
is ready long before the walls need it. Mono mix, Hann window of ~23 ms rounded to a power of two
(1024 frames at 44.1/48 kHz), one radix-2 FFT per 10 ms hop. Per hop one 16-bit entry:
- bass level: energy of the 35-160 Hz bins in dB, mapped to 0..1 between a peak that falls 6 dB/s
  and a floor that rises 10 dB/s (follows the dips between kicks), span clamped to 8-24 dB; 0 while
  a slow peak (falls 2 dB/s) is under -60 dB or the bass is 30 dB under it (breaks);
- onset: detection function = half spectral flux (rise of `log1p(100 x amplitude)` per bin up to
  3 kHz, bass bins x3) + half bass level rise (12 dB per hop = 1), a local maximum above a running
  mean + 1.5 deviations (~0.5 s), at least 180 ms after the previous one (up to 333 BPM), not in
  the first 0.3 s; strength 0.3..1 from how far above the threshold relative to the recent
  strongest onset (that peak falls 33 %/s).

*Round 9 ("with hardtekk its very chill even on 200%").* Offline harness (scratchpad, the user's
own songs from `sr2mods_music`, 60 s each from 0:40): with the old analysis hardtekk / uptempo
(Septum - The Final Countdown, 102 Boyz - Tokyo Drift hardtekk edit, 200BPM Volksmusik, 1001
Uptempo Nights, BAD HABIT) gave 250-310 onsets/min (median gap 0.15-0.20 s: kicks plus offbeat
hats / bass stabs, strength 0.3-1 at random, mean 0.6) and a bass level that sat at 0.7-0.9 with
short dips (fixed 24 dB window under a slowly falling peak: compressed music never leaves the top).
After the changes above: 170-205 onsets/min, median gap 0.26-0.34 s (= the 180-230 BPM kick),
mean strength 0.7-0.8, level per beat swinging from ~0.1 to ~0.95 (0.35-0.9 on the most
compressed one). The synthetic 120 BPM test track still gives every kick at 0.500 s, strength 1.
Memory 2 bytes per hop (7 KB per minute, capped at 6 hours). After a seek the track starts at the
seek frame (`Reset(rate, startFrame)`), entries stay in song frames. Offline test (scratchpad
harness, synthetic 120 BPM kick/snare/hat/pad track at 44.1 kHz): 39 of 39 kicks after the warm-up
found at exactly 0.500 s spacing, level 0.8 on the kick decaying to 0 between; plain tones and the
5.1 test WAV give no onsets.

*Reading it* (`module.cpp` `TickDancer`, every frame): song frame = `base + playPos` minus 40 ms
(`timeSamples` runs ahead of what is heard by about the output buffer); `Query(last, now)` gives
the level at `now` and the strongest onset in between, so no beat is lost at low frame rates and
none repeats while the play head stands still. Nothing is read while the song is paused (you, the
pause menu, buffering) or past its end: the walls settle and are restored.

*Which corrals* (`SyncDancer`, every box scan = 2 s, also right after the setting goes on): music
box speakers (not emitters) that play the current clip, whose box is within the speaker's hearing
range (Amplify included) of the main camera. Their `MusicBoxUpgrader` (from the box scan) is the
plot root, which also carries `WallUpgrader` and `AirNetUpgrader` (prefab dump: `patchCorral` root
components). At most 12 corrals.

*What is animated* (corral prefab `patchCorral`, asset bundle dump):
- `WallUpgrader.StandardWalls` = "Base Corral", `.UpgradeWalls` = "High Wall Corral"; the active
  one is used. Under `Frame/techCorralPostNN` (4, turned 90 deg apart) `/empty_postBase` (the
  corner) sits "corralField": one MeshRenderer, an L-shaped energy field covering half of the two
  sides at that corner (bounds 4.9 x 3.2 x 4.9, y 0.17-3.39). High Walls add "corralField (1)" on
  top (y 3.38). So 4 or 8 renderers, only those named `corralField*` (other renderers under the
  walls root, posts, the net, the solar shield, are skipped and named in the log). Material
  "CorralBarrier" (HDRP transparent shader graph): `_Intensity` 3, `_Color` white, `_EmissionColor`,
  ripple / `_HitTime` / `_Health` props. The fields have no collider (the barriers are the
  separate "corralBarrier" boxes, layer 29), so scaling them changes nothing in play.
- `AirNetUpgrader.AirNets` = [Base Air Net (under Base Corral), High Air Net (under High Wall
  Corral)]; the active one = the owned net for the current walls. "NetTrigger": MeshRenderer +
  `AirNet` + BoxCollider on the same object, so the net is never moved or scaled. Material "AirNet"
  (same shader family, `_Intensity` 1). `AirNet.Awake/Update` set `_Health` on its own material
  instance (0x982840 loads `StringLiteral__Health`), `OnCollisionEnter` sets `_HitTime`; we touch
  neither, so a hit or broken net still shows.
- The barrier ripples (the "morph" when something is shot through a wall; round 9: "when shooting
  something through walls (eg chicken coop), it gets morphed differently than the current impl").
  IDA: `CorralBarrierRipples` (MonoBehaviour on the FXDirector, `SceneContext.FXDirector` 0xD0 ->
  `corralBarrierRipples` 0x40) keeps `Vector4[4] worldRippleArray` (0x20), `nextRipple` (0x28) and
  the id of `_WorldRippleArray` (0x2C). `ResetRipples` (Awake, OnDestroy) fills w with -10000;
  `SpawnRippleHere(Vector3)` (0xB65EB0) advances the ring, writes (x, y, z, `Time.time`) and calls
  `Shader.SetGlobalVectorArray`. `CorralBarrierRippleCollision.OnTriggerEnter` (0xB65A40) does the
  same inline at `Collider.ClosestPointOnBounds` of the barrier for layer-9 objects with a
  `VacDelaunchTrigger` (vacpack shots).
  Shader "SR/RanchTech/CorralBarrier" (dumped from `shaders_assets_*.bundle`, DXBC disassembled):
  tessellated; the domain stage loops over the 4 ripples: age = _Time.y - w, skipped once age >=
  `_RippleFadeDuration`; t = age / fade; radius = lerp(`_RippleStartSize`, `_RippleSize`, t); every
  vertex within the radius is pushed away from the centre by `_RipplePushScale` x (1 - d / radius)
  x (1 - t^4) (object space). Material CorralBarrier: `_RipplePushScale` 0.5, `_RippleStrength` 1,
  `_RippleFadeDuration` 1, `_RippleSize` 5, `_RippleStartSize` 1 (AirNet: 1 / 5 / 5 / 5 / 0).
  These are per-material floats, so a property block sets them per renderer; the ring is global.

On every beat (onset): one ripple per animated corral (at most 4 corrals per beat: the ring has 4
slots; with more, round robin) through `SpawnRippleHere` on the scene's `CorralBarrierRipples`
(found with `FindObjectOfType` at a box scan, at most every 10 s while unknown, GC handle; used only
while its array exists). The ripple centre is the middle of one side (between two neighbouring
corner posts of the lower fields), 1 m inside the corral, 1.7 m up (3.4 m with High Walls), so the
side bulges outwards; the side is the one nearest the chase angle (a quarter turn per beat), so the
bulge travels round the corral with the music. Without 4 lower fields (no plausible corners) the
plot gets no ripples. The y-scale bob of round 8 is gone.

Per frame, per cached renderer: `MaterialPropertyBlock.Clear` + `Renderer.GetPropertyBlock(work)`
(keeps anything the game put there) + `SetFloat` / `SetColor` + `SetPropertyBlock` (direct native
calls, no searches; one shared work block). With I = intensity, D = drive = I x (1 + 0.75 max(0,
I - 1)) (100 % = 1, 200 % = 3.5), e = smoothed bass level (attack 15 ms, release 100 ms), p = beat
pulse (jumps to the onset strength, decays with 0.15 s), a chase angle that turns a quarter round
the corral per beat (smoothed 60 ms), w = how close a renderer is to the chase (cos^2 of its corner
angle minus the chase, the upper field trailing by 0.6 rad):
- glow: `_Intensity` = base x (1 + D x (0.8 e + 2.0 p w));
- colour: `_Color` = base mixed towards a colour wheel (hue +0.11 per beat, spread around the
  corners) by min(1, 0.6 I) x (0.35 + 0.65 max(e, p w));
- wall fields only: `_RipplePushScale` = base x min(10, 1 + D x (1.0 e + 2.5 p)) (how far the
  bulges push, all ripples in flight pump with the bass); `_RippleStrength` = base x (1 + D x (e +
  2 p)); `_RippleSize` = base x (1 + 0.25 D e); `_RippleFadeDuration` = 0.9 x (4 / corrals per
  beat) x the tempo (smoothed gap between beats), 0.3 s to 1.5 x base: a ripple has faded before
  its slot comes round again, and on fast music the rings grow faster.
Vacpack shots through a dancing wall use the same block, so they bulge more while it dances.
Cached per plot (GC handles for renderer and a per-renderer snapshot block taken with
`GetPropertyBlock` before the first change, the 4 ripple points); rebuilt when the plot's active
walls root or net changes (High Walls / Air Net bought or sold). Restore = `SetPropertyBlock
(snapshot)`: when the song pauses (after the walls settled), stops or changes plot range, the
setting goes off, the module is disabled; ripples in flight fade on their own (material fade 1 s).
After a native fault the module stops for the session and leaves the walls as they are (handles
leaked by design).

## Settings

| key | default | |
|---|---|---|
| `volume` | 1.0 (100 %) | 0-100 %, on top of the game's music volume |
| `range` | 60 m | hearing range: full volume to a fifth of it, silent at it (10-200 m) |
| `amplify` | off | each other speaker in range: +25 % volume and range, up to 2x |
| `shuffle` | on | random songs when the queue is empty |
| `mute_vanilla` | on | silence the music box's own tune while a song plays |
| `dance_walls` | off | a ripple runs round the corral walls on every beat; walls and Air Net glow and change colour (corrals within hearing range) |
| `dance_intensity` | 1.0 (100 %) | 10-200 %, how strongly they ripple, glow and change colour; steeper above 100 % (shown while `dance_walls` is on) |

## Log lines

- `slime_dj: music folder '…': N song(s) (M newly read, K not playable) in X ms`
- `slime_dj: music bus '…' (buses: …)` or a warning with the bus tree
- `slime_dj: speaker on music box '…' at '…': N vanilla source(s), cue '…' a-b m …; hearing range N m` (first speaker;
  the others: `speaker on music box '…' at '…' (N vanilla source(s))`)
- `slime_dj: playing '…' (file)`, `slime_dj: seek to N s in '…'`
- `slime_dj: speaker on emitter '…' (kind K, range xA, volume xB[, switched off for now])`
- `slime_dj: N music box(es), M emitter(s) registered; speakers: B on boxes, E on emitters (K switched off)` (when a count changes)
- `slime_dj: N speaker(s) had stopped (object switched off and on); rejoined in sync` (first 10)
- `slime_dj: box scans in the last 60 s: …` (once a minute while scans ran), `slime_dj: no MusicBoxUpgrader.Apply hook (…)`
- `slime_dj: speaker N ms off the others, moved back in sync`
- `slime_dj: walls dance at 'patchCorral…': N wall field(s) under '…', M net renderer(s) under '…'; wall _Intensity X
  _Color (r, g, b) _RipplePushScale … _RippleStrength … _RippleFadeDuration … _RippleSize …, net _Intensity Y; K ripple
  point(s)[ (other renderers skipped: …)]` (per corral built, first 24)
- `slime_dj: walls dance: first beat ripple(s) spawned through CorralBarrierRipples.SpawnRippleHere (N corral(s) per
  beat)` (once), `slime_dj: walls dance: no CorralBarrierRipples in the scene yet (…)` (once),
  `slime_dj: walls dance without beat ripples (…); glow and colour only` (Setup, warning)
- `slime_dj: walls dance at N corral(s)` (when the count changes), `slime_dj: walls dance stopped (setting off|no
  song); walls restored`
- `slime_dj: 'Walls dance to the music' unavailable (…)` (Setup; a class / method missing)

## Not verified in game yet

Written against the 1.3.0 dump, built, and the decoders tested offline (Vorbis, MP3 with ID3v2.3
and 2.4 tags, mono 22 kHz WAV with INFO title, 5.1 WAV, a broken file); none of it has run in the
game. To check:
- that SetData into a playing looping clip is glitch-free (no clicks at chunk boundaries);
- that the music box's tune really comes from a `SECTR_AudioSource` under `MusicBox` (the log
  line above shows how many were found) and that `Volume = 0` silences it;
- the music bus name and that its `EffectiveVolume` follows the music slider;
- that the vanilla cue's bus pauses in the pause menu (else songs keep playing there);
- the hearing range default (60 m, linear) and Amplify;
- the Music Box II second speaker playing (emitter registered by custom_upgrades plot_props);
- seeking (Vorbis / MP3 / WAV) and that the speakers stay in sync after it.
- box cache: no `frame:slime_dj took` warnings in normal play; a Music Box bought on a new corral,
  and corrals after a teleport back to the ranch, get a speaker within ~2 s without a scene search
  (the minute line shows `scene searches 0`).
- walls dance: round 8 showed the height bob (so property blocks and transforms reach the fields);
  round 9 build: that the beat ripples bulge the walls outwards and travel round the corral, that
  `_RipplePushScale` in the block makes them stronger with the bass (if not, the ripples still show
  at the material's 0.5), that `_Intensity` / `_Color` visibly change the CorralBarrier and AirNet
  shaders, that hardtekk at 200 % is wild, that the beat looks in time
  (lag constant `kDanceLagSeconds` 40 ms), and that everything is back to normal after Stop /
  Pause / setting off / leaving the ranch.
