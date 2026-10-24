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
| `codecs.c/.h` | the only TU that compiles stb_vorbis, dr_mp3, dr_wav (CMake target `sr2_codecs`, C, `/W0`) behind a small streaming C API |
| `ui/slime_dj_bridge.cpp/.h` | snapshot / request exchange with the overlay tab (same pattern as the sandbox) |

## Design

**Threads.** Library scan and decoding run on their own threads and never touch the game.
Everything IL2CPP runs in one frame callback (`main_thread::AddFrameCallback`) or in posted
requests from the tab, both on the Unity main thread inside `GuardedCallEx`. There are no hooks.
A native fault marks the module faulted and stops it for the session (handles are leaked then,
on purpose); managed exceptions are logged (3 lines) and the module stops after 20.

**Music boxes.** `Object.FindObjectsByType(MusicBoxUpgrader, FindObjectsInactive.Exclude, None)`
every 2 s, only while a song is loaded or the tab is open (never `FindObjectsOfTypeAll`, never
per frame). A box counts while `MusicBoxUpgrader.MusicBox` (the GameObject the vanilla Music Box
upgrade switches on) is active in the hierarchy, i.e. the corral owns the upgrade and the plot is
loaded. Music Box II (custom_upgrades) keeps the same GameObject, so it plays too.

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

## Settings

| key | default | |
|---|---|---|
| `volume` | 1.0 (100 %) | 0-100 %, on top of the game's music volume |
| `range` | 60 m | hearing range: full volume to a fifth of it, silent at it (10-200 m) |
| `amplify` | off | each other speaker in range: +25 % volume and range, up to 2x |
| `shuffle` | on | random songs when the queue is empty |
| `mute_vanilla` | on | silence the music box's own tune while a song plays |

## Log lines

- `slime_dj: music folder '…': N song(s) (M newly read, K not playable) in X ms`
- `slime_dj: music bus '…' (buses: …)` or a warning with the bus tree
- `slime_dj: speaker on music box '…' at '…': N vanilla source(s), cue '…' a-b m …; hearing range N m` (first speaker;
  the others: `speaker on music box '…' at '…' (N vanilla source(s))`)
- `slime_dj: playing '…' (file)`, `slime_dj: seek to N s in '…'`
- `slime_dj: speaker on emitter '…' (kind K, range xA, volume xB[, switched off for now])`
- `slime_dj: N music box(es), M emitter(s) registered; speakers: B on boxes, E on emitters (K switched off)` (when a count changes)
- `slime_dj: N speaker(s) had stopped (object switched off and on); rejoined in sync` (first 10)
- `slime_dj: speaker N ms off the others, moved back in sync`

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
