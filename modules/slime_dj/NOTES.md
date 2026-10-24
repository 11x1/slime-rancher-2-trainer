# slime_dj

Plays your own songs from the corral Music Box ("slime speakers"). Overlay tab: Ranch > Slime DJ
(`ui/NOTES.md`). Default on; it does nothing until you press Play or queue a song.

## Adding songs

Put `.ogg` (Vorbis), `.mp3` or `.wav` files in `<game folder>\sr2mods_music\` (next to
`SlimeRancher2.exe`; the module creates the folder on its first start). Subfolders are read too,
up to 4 levels. Then press the refresh icon in the tab bar (Rescan), or reopen the game. "Open
folder" on the Songs card opens it in Explorer.

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

**Several boxes: all in sync.** Every owned box nearby (at most 12) gets a speaker: a new
GameObject `sr2dj_Speaker` with a Unity `AudioSource`, parented (`SetParent(…, false)`) to the
box's first `SECTR_AudioSource` (where the vanilla tune comes from) or to the box itself. All
speakers play the same clip and were started together, so the song is positional at every corral
and walking between corrals never restarts or doubles it. A box that appears later (sold and
bought again, region loaded) joins at the current `timeSamples`; a speaker whose box is gone is
destroyed. The first live speaker is the clock (see below); if it goes, the next one takes over.
If every box goes (you leave the ranch), the song pauses where it was and resumes when a box is
back.

**Emitters (`api.h`, stable API for other modules).** Any GameObject can play the song:

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
- At most 24 emitter speakers (plus 12 music boxes); 256 registrations.
- Current users: custom_upgrades `plot_props.cpp` registers the Music Box II second speaker
  (the cloned `Music` model, whose vanilla audio was stripped with the game scripts).

**3D settings.** Copied from the vanilla cue (`SECTR_AudioSource.Cue`): `MinDistance`,
`MaxDistance`, `Falloff` (SECTR Linear 0 / Logrithmic 1 -> Unity Linear / Logarithmic),
`spatialBlend` 1, doppler 0, priority 32. Without a cue: 3-40 m logarithmic.

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

**Volume.** `AudioSource.volume = volume setting x music bus`. The music bus is the first
`SECTR_AudioBus` whose name contains "music" under `SECTR_AudioSystem.MasterBus` (the system is
found with `FindFirstObjectByType` once, retried every 5 s until found; the bus tree is logged).
Its `EffectiveVolume` is what the game's music volume slider drives
(`ChannelVolumeController._targetChannel`); 0 while the bus is muted. Without a music bus the
master bus volume is used.

**Pause.** The speakers pause while the vanilla cue's bus reports `Paused` (game pause menu),
while you pause, while buffering, and while no box is nearby.

**Queue and shuffle.** Click a song: plays now (the queue stays). +: appended to the queue (starts
it if nothing plays). When a song ends: the queue's first; if the queue is empty and `shuffle` is
on (default), a random playable song (not the one that just played); otherwise playback stops.
Play with nothing loaded starts the same way. Three failed songs in a row stop playback.

## Settings

| key | default | |
|---|---|---|
| `volume` | 1.0 (100 %) | 0-150 %, on top of the game's music volume |
| `shuffle` | on | random songs when the queue is empty |
| `mute_vanilla` | on | silence the music box's own tune while a song plays |

## Log lines

- `slime_dj: music folder '…': N song(s) (M newly read, K not playable) in X ms`
- `slime_dj: music bus '…' (buses: …)` or a warning with the bus tree
- `slime_dj: speaker on music box '…' at '…': N vanilla source(s), cue '…' a-b m …` (first speaker only)
- `slime_dj: playing '…' (file)`

## Not verified in game yet

Written against the 1.3.0 dump, built, and the decoders tested offline (Vorbis, MP3 with ID3v2.3
and 2.4 tags, mono 22 kHz WAV with INFO title, 5.1 WAV, a broken file); none of it has run in the
game. To check:
- that SetData into a playing looping clip is glitch-free (no clicks at chunk boundaries);
- that the music box's tune really comes from a `SECTR_AudioSource` under `MusicBox` (the log
  line above shows how many were found) and that `Volume = 0` silences it;
- the music bus name and that its `EffectiveVolume` follows the music slider;
- that the vanilla cue's bus pauses in the pause menu (else songs keep playing there);
- the 3D range copied from the cue (maybe too short for a song; the vanilla tune is quiet).
