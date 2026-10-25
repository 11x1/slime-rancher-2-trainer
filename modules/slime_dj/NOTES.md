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
| `lamps.cpp/.h` | "Lamps pulse to the music": lamp decorations and Powderfall's glowing ice near a playing speaker pulse with its channel's beat |
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
speaker of the global queue goes (you leave the ranch), its song pauses and resumes when one is back. A
Corral Radio plays on in silence instead (round 13, see "Real radio" under Channels).
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
while buffering, and (global queue only) while the channel has no speaker.

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
  with a slot and no song resumes at its last entry (from the song's start, unless it has an Air record,
  below). Waiting radios show "Waiting: 4 radios play already". Nothing starts before the music folder has
  been read.
- *Real radio* (round 13, user: "buying radio II plays the selected music from the start not where it
  continued, so in theory its not really a radio if it doesnt play when no output...?"). Two parts:
  - Channel loaded, no speaker playing it (no box near, all switched off or silent): `TickSong` runs a
    silent play head (`playPos += dt x rate`, fraction in `Current::virt`) while not paused by the game,
    not buffering; `Fill` keeps the ring ahead of it, a song with nobody to play it starts on its own and
    the next entry follows at its end. A speaker that comes (back) joins at `playPos % ring`, mid-song.
    The global queue still pauses without speakers (it is the player, not a radio).
  - Channel gone (Music Box sold, zone unloaded, radio sold): `DropRadio` keeps an `Air` record per plot
    key (entry file + index, playlist hash, seconds into the song, `radioTime_` = unpaused play time of
    the module, max 64 records). When the channel comes back (`SyncRadios`; the record is used only if the
    radio is on and the playlist is unchanged), `NextRadioSong(resume)` -> `CatchUp` plays the time gone by
    through the playlist (library lengths; whole rounds skipped without shuffle; shuffle picks as it would
    have) and `StartSong(ch, id, startAt)` starts the decoder there (as a seek: `Current::base`). Logged:
    `radio '...' back on air after N s: entry K at S s`, `radio '...' playing '...' (...) from S s (it played
    on without a speaker)`. In memory only: after a game load a radio without a channel starts its stored
    entry from the beginning when it gets one (it never was on air).
- *Order*: next entry, wrapping; radio shuffle -> a random other entry. Missing / unplayable files are
  skipped; none playable or three failures in a row -> the radio switches off.
- *After a load*: with custom upgrades on, an Apply record triggers one probe scan even while idle, so a
  radio left on in the save starts.
- Not per radio: `SongPlaying` / `SongName` follow the global song only. Walls dance per channel (round 12).

### Corral Radio menu (rounds 11-13, `radio_menu.cpp`)

User: "Add a button in the last empty corner (color it blue; like you did for silo's button) and let the
button open a menu to build a queue from". custom_upgrades builds the button (a second `SiloButtons` with
`RadioButtonLook`: clone of the corral Plort Collector button, blue, on every corral that owns the Corral
Radio, at the free (+x, -z) corner; see `modules/custom_upgrades/NOTES.md`). Its press calls `api.h
OpenRadioMenu(plot, &why)`: it only records the plot key (`RequestMenu`; refused with a reason when Slime
DJ is off, the menu unavailable or already open; custom_upgrades shows the reason as a game notification,
round 13 without parentheses: "Slime DJ is off, turn it on in the overlay under Ranch > Slime DJ"). The
request keeps a GC handle of the plot (`menuPlot_`). The next tick counts as active (box scan now, so the
radio channel exists), then opens the menu.

- *Look*: a runtime uGUI panel like custom_upgrades' Sell-All receipt (same palette, SDF shapes, the HUD's
  TMP font, overlay Canvas 1920 x 1080 + UIInputLocker: game paused, cursor free). Title with the header
  buttons Radio on/off, Shuffle, Clear (press twice) on its right, corral name (+ warning pill), two virtual
  lists (Songs sorted by name; the playlist numbered, the playing entry teal with `>`), a feedback line.
  5..11 rows per list (canvas height); only changed texts / colours are written.
- *Round 12* (user: "the playlist could use more slime ranchification, we dont need keybinds when we put
  the list items as slime rancher's buttons (or design similiar to it, like Silo's sell all hud), what way
  we can remove the bottom hint, DONE PLAY UP DOWN REMOVE buttons"): every row is a pill in the game
  button's colours (`ButtonAnimator.ColorNormal` / `ColorMouseOver` and its label colour, read from the
  popup's Cancel button at each open; menu palette if unreadable; re/ui.md). Hover lights the row and moves
  the cursor there; the focused song row shows "+ Add", the focused entry "Play". Click a song = add, click
  an entry = play it now; each entry has three small round buttons (move up, move down, remove; glyphs are
  rotated SDF bars, letters without `Transform.set_localRotation`). The header buttons are clones of the
  popup's Cancel button (as the receipt's; `Icon Background` key glyph hidden, LayoutManager, x0.72; the
  key focus colours the label pink), own pills if the clone fails. No key hint bar, no Done / Play / Up /
  Down / Remove buttons.
- *Round 13*:
  - Header buttons through the shared `modules/common/game_button` (user: "Use game's button for the
    overlay please, they have support to show what icon to press to activate the button"): key caps R
    (Radio), F (Shuffle), C (Clear, new key), x0.72, min label width 150, key focus = the helper's teal
    `SetSelected`, disabled = the game's grey (`SetInteractable`). The menu's own clone copy is gone; the
    rows take the helper's colours (`kPink` / `kDeepBlue`, white label) when it resolves.
  - Playing entry (user: "Currently playing (not hovered) song in the queue text is the same color as
    background ... move the '>' outside before the row currently playing"): pink label on the pink row
    before; now the row is the game's selected teal (`gbn::kTeal`) with a white label, and a pink `>` sits
    in the list's left margin before it (hover still wins). The kept selection (cursor in the playlist
    while the songs list has focus) is light blue with a navy label (white was unreadable on it).
  - No status line (user: "Remove the top status row 'Now playing: ... (paused while this menu is open)'
    text please"): `MenuState::warning` instead, an orange pill right of the corral name, shown only when
    the radio can't play: no Music Box (`LandPlot.HasUpgrade(MUSIC_BOX)` false), no channel after 3 s for
    another reason, or waiting for a decoder slot. The lists moved up by the freed row.
  - No Music Box (user: "no speaker upgrade shows the queue as empty in radio dialog, make it grayed out
    with an error warning on top of the dialog"): without a channel the menu reads the stored playlist from
    the plot's sidecar value (`LoadRadio` into a temporary channel) and shows it greyed (the game's
    disabled grey, no hover, no row buttons), header buttons disabled, a song click shows the warning.
- *Round 14*:
  - Key bar at the bottom (user: "the key hints should be at the bottom, aswell as '[ESC] Close' on the left,
    others on the right"): game buttons (game_button, x0.72, key caps). Left: ESC Close. Right: the cursor
    row's actions for the list it is in (user: "make the songs buttons so i can have a hint of what to press to
    queue a song up, play now, remove from queue"): Songs SPACE Queue + ENTER Play now, Playlist DEL Remove +
    ENTER Play now (greyed when the row can't act); then R Radio, F Shuffle, C Clear (moved down from the
    header). `LayoutBar` places them by their laid-out widths (`Button::Size`, re-placed when a label changes).
    The rows stay our pills in the game button's colours (cloning 22 game buttons as list rows, with a time
    column, marquee and inline buttons inside the clone's layout, was not attempted); the focused row no longer
    shows "+ Add" / "Play" (the bar names the actions). Keys: Songs Space / pad A queue, Enter / pad X play now;
    Playlist Space / Enter / pad A play, Delete / Backspace / pad X remove; Down past the last row goes into the
    bar, Up from the bar back to the list.
  - Play now (`MenuOp::kPlayNow`): the song is inserted after the playing entry and played from its start, so
    the radio goes on with what followed.
  - Marquee (user: "song names should scroll when they dont fit"): each row's name sits in a `RectMask2D`
    clip; the focused row's name, when `TMP_Text.get_preferredWidth` is wider than the clip, switches to
    overflow mode Overflow and scrolls (1 s wait, 80 px/s, 1.5 s at the end, jump back). Ellipsis otherwise.
  - "Clear?" (user: "should go back to normal [C] once any other input is pressed"): any key but C
    (`Keyboard.anyKey`), a mouse button pressed anywhere but on Clear, the wheel or a pad button disarms it
    (and clears its note).
  - Warning pill 42 high instead of 34 ("needs a bit more space (margin) on y"), list heads 10 px lower.
  - Refusal notification is just "Slime DJ is off" (custom_upgrades shows the reason without a prefix).
  - Off and on keeps the song (user: "song still restarts if slime radio is disabled and enabled"): switching
    a radio off stores an Air record (`AirOf`) in the channel; switching it on catches up (`CatchUp`), as when
    its channel comes back. The time off counts only while the game runs, so off / on inside the menu
    (game paused) goes on exactly where it was. `AirInList` re-finds the entry if the playlist was edited
    meanwhile (dropped if it was removed); Play at / Clear drop the record. A switched-off radio's record
    also survives its channel going away (`airs_`). (The Slime Radio toy has no switch of its own; it always
    plays.)
  - Toggle spam (user: "'4 radios active, waiting for one to stop' ... when spamming R"): the menu built its
    state right after the toggle, before the next box scan handed out the slot. Now `HandleRadio` ends with
    `AllocateSlots()` (slot freed / taken at once), and the warning (menu and tab) shows only when four
    other radios really hold the slots (`SlotsFull`).
- *Input*: polled (InputSystem devices, Toggle clicks, the clones' `Selectable.IsPressed` + pointer inside),
  0.3 s grace; hover from `Selectable.get_isPointerInside` (the list box, then its rows, then the hovered
  entry's small buttons: about 16 calls per frame). The keys of round 11 still work, unprinted (see the
  `radio_menu.h` header).
- *Edits* go through `HandleRadio` (the tab's requests), so the sidecar is written and playback reacts the
  same way; the menu's state refreshes every 0.25 s and after each action.
- While open the game is paused, so the speakers pause too; the radio plays on after the menu closes.
- No search box (a TMP input field was not attempted); Page Up / Down (pad LT / RT), Home / End and the
  wheel move through long lists.
- Closed on Esc / pad B / backdrop click (no Done button since round 12), module off, and before a fault
  stops the module.

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
emitters) playing their channel's clip, at any distance. Their `MusicBoxUpgrader` is the plot root, which
also carries `WallUpgrader` / `AirNetUpgrader`. Max 12, nearest to the camera first (`WallDancer::Sync`
only looks at the first 12 targets, so a far plot gives way to a near one). Round 14 ("the walls dont
react to music from far away :("): until then only corrals within hearing range (Amplify included) of the
camera danced, so with the user's 30 m range the walls stopped a few steps outside the corral (log:
`walls dance at 0 corral(s)` with 2 music boxes playing). Nothing else limits it: out of range the
speaker's AudioSource plays on silently (linear rolloff, volume 0), so `timeSamples`, the play head and
`DanceSample` keep running, and a box only counts while its plot is loaded (`MusicBox` activeInHierarchy),
which is also when its walls can be seen. Cost per far corral is the same as a near one (about 9 property
block updates per frame, one ripple per beat), bounded by the 12.
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
`SetPropertyBlock(snapshot)` when the song pauses (after settling), stops, the plot unloads, the
setting goes off, or the module is disabled. Ripples in flight fade on their own. After a native fault the
walls are left as they are.

### Lamps pulse to the music (`dance_lamps`, default on; `lamp_range` 5-100 m, default 20 m)

User idea: "maybe (insane idea) we can get the Electric pillar lamps to beat to the music aswell lol".
`lamps.cpp`; the module only hands it data (`SyncLamps` in the box scan, `TickLamps` every frame).

*The lamp* (prefab `decorElectricLampPillar`, definition `ElectricLampPillar`; full tree in
`re/fabricator.md`) has **no Light component**. Its light is two big glow spheres ("Sphere Light",
"Sphere Light (2)", material `SphereLightPillarLightning_02`: `_Color` warm yellow, `_BrightnessMultiplier`
5) and the lightning meshes of `PillarLightningFX` (materials `LightningPillar_*`: HDR `_ColorCore` /
`_ColorEdge`). No script animates them, so per-renderer property blocks are the whole look. Renderers are
picked by property: `_BrightnessMultiplier` (spheres) or `_ColorCore` / `_ColorEdge` (lightning); the glass
pillar, bronze bases and grass quads have none and are never touched.

*Finding lamps* (no scene search): the lamp's `GadgetDefinition` (prefab name match in
`LookupDirector._gadgetDefinitions` and its sub-groups, plus any definition whose `VariantOf` is it; found
once, retried every 10 s until the groups are loaded, GC handles), then
`GameModel.identifiablesByIdent.TryGetValue(def)` = the `GadgetModel` of every placed lamp;
`GadgetModel.transform` alive and active = loaded. This refresh runs at most every 4 s, during a box scan
(2 s) while some speaker plays; between refreshes only the lamps already held are re-weighed. Up to 48
lamps at once, nearest to a speaker first.

*More lamps* (round 13: "Add support for 'Pink Striped Lamp', 'Conservatory Lamp', 'Blue Lava Lamp', 'Ice
Lamp' and the glowing ice things in the snow map if possible"). Lamp kinds by gadget prefab:
`decorElectricLampPillar`, `decorPinkStripedLamp` (+ Blue / Mint / Prismatic variants),
`decorConservatoryLamp`, `decorSlimeLampBlue` (Blue Lava Lamp; Violet / Gold / Green / Grey / Red variants
come along), `decorIceLamp`. Unlike the pillar these have real HDRP Lights (structure in `re/fabricator.md`).
Per lamp the pulse touches:
- renderers with a glowing property: `_BrightnessMultiplier` (glow spheres, the striped lamp's bulb),
  `_IntensityDay` / `_IntensityNight` (lava lamp glow quad), `_ColorCore` / `_ColorEdge` (lightning),
  `_EmissiveColor` / `_InnerEmission` only when brighter than 0.01 (striped lamp bulb and stripes, ice cube;
  black emission on lamp bodies, snow and the conservatory cone is left alone). `_Color` is tinted only on
  glow volumes / lightning (elsewhere it is the albedo).
- up to 4 `Light` components: `Light.intensity` and `Light.color` (HDRP forwards to these, `re/unity.md`).
  The Ice Lamp's light is run by a `LightControl` (gradient colour, writes the light on its own update
  ticks). So each frame the pulse reads the light; a value it did not write itself becomes the new base,
  and restore only puts the base back while the light still shows the pulse's last value. If the
  LightControl runs after the mod's frame callback on its tick, that frame shows the plain light (a
  short dip at most; not seen in game yet).

*Glowing scene ice* (Powderfall Bluffs): the crystals are scene objects `iceCube0N_light` (material
`objIce_IceCube01_lightGradient*`, HDR `_InnerEmission`) with a child "Point Light Gradient" (point Light
8192, range 5, green-cyan, `LightControl` + `CullablePointLight`), the same as the Ice Lamp's cube. Found
with one `FindObjectsOfTypeAll(LightControl)` per scene group: `RegionRegistry.CurrentSceneGroup` is
checked at each lamp refresh (4 s) while a speaker plays; 5 s after it changed the scan keeps every
LightControl whose parent is named `iceCube*` and that has no `Gadget` above it (Ice Lamps are placed
lamps), as a GC handle + position (static geometry; up to 512). Candidates are then the crystals that
are active now (proxied regions are off). The scan is logged with its time.
Round 14 (round 13 test: "didnt see"): the log had two scans, both on the ranch (`0 glowing ice crystal(s)
... (20 LightControl(s) looked at)`: the ranch has no scene ice; the placed Ice Lamp is skipped as a gadget).
While the player was in Powderfall no speaker played (minute line `0 corral(s) known, 0 speaker(s)`), so no
lamp refresh ran there and the zone was never scanned: the scene ice only pulses near a speaker placed in
Powderfall itself. The debug highlighter (below) now runs the scene group check and scan without music, and
the log line names the scene group, the LightControls on placed Ice Lamps and, when 0 crystals match, up to 8
other LightControl parent names (to see whether the crystals are named differently in game).

*Own colours* (round 14: "pulse lamps (electric pillar can stay rgb) in their own color light"): only the
Electric Pillar Lamp (`decorElectricLampPillar*` by name) mixes towards the beat hue; every other lamp and
the scene ice pulse brightness only (k = 0, `Light.color` not written at all).

*Debug highlighter* (`LampPulser::Debug`, every frame while debug_tools' "Highlight what the mod touches"
is on, music or not): every 4 s the ice check and a lamp refresh of its own (renderer bounds of up to 96
loaded lamps, `Renderer.get_bounds` union; ice crystals measured once each, 64 per refresh); every 1 s it
publishes `slime_dj.lamps.pulsing` (pink, the held lamps), `slime_dj.lamps.idle` (grey, loaded lamps that do
not pulse) and `slime_dj.lamps.ice` (cyan, loaded crystals not pulsing) to `modules/common/debug_marks.h`.
The module publishes `slime_dj.speakers.playing` (green, lamp range as a ground circle) / `.silent` (grey)
at each box scan while a song is loaded. Log `lamps: highlighter: N pulsing, M not pulsing, K glowing ice
crystal(s) loaded of T in scene group '...'` on a change.

*Sources* (`SyncLamps`): every live speaker playing its channel's current clip with volume > 0 (so a DJ
Booth / Boombox that is switched off does not count): music boxes, DJ Booth speakers, speaker stacks, Slime
Radios, plain Boomboxes, corral speakers of a Corral Radio. Each lamp takes its nearest source within
`lamp_range` and that source's channel (global queue or a radio). Strength (round 13: "lamps shouldnt be
as bright when only one speaker close and playing"): every speaker of that channel in range adds its
closeness (1 up to 40 % of the range, fading linearly to 0 at the range) x its loudness (emitter volume
scale x muffle factor); w = 1 - exp(-0.45 x sum), so one close speaker gives 0.36 (a gentle pulse), two
0.59, three 0.74, four 0.83. w is eased over 0.5 s (a lamp entering range fades in).

*Beat* (`TickLamps`, every frame): per channel the same read as `TickDancer` (`base + playPos` minus 40 ms,
`BeatTrack::Query(last, now)`; nothing while paused / buffering / pause menu / past the end) and the walls'
smoothing: bass level attack 15 ms / release 100 ms, beat pulse to the onset strength decaying with
0.15 s, energy smoothed 0.4 s scaling level and pulse to 0.15 + 0.85 x energy (calm parts stay calm), hue
+0.11 x that per beat. With e = w x level, p = w x pulse: `_BrightnessMultiplier` = base x (1 + 0.8 e +
1.6 p); `_ColorCore` / `_ColorEdge` = base x (1 + 1.0 e + 2.5 p); every colour (also `_Color`) mixed towards
the hue by 0.7 x max(e, p), keeping its brightness (max channel). No intensity slider (fixed look).

*Restore*: `SetPropertyBlock(snapshot)` (the renderer's own block from `GetPropertyBlock` before the first
change) when its channel settles (song paused / stopped), the lamp leaves range or is gone, its nearest
source switches to another channel (restore, then the new song), the setting goes off, no song is loaded,
or the module is disabled. A lamp whose scene unloaded is simply dropped (its objects are gone).

## Settings

| key | default | |
|---|---|---|
| `volume` | 1.0 (100 %) | 0-100 %, on top of the game's music volume |
| `range` | 60 m | hearing range: full volume to a fifth of it, silent at it (10-200 m) |
| `amplify` | off | each other audible speaker of the channel in range: +15 % volume (to 1.6x) and +10 % range (to 1.3x); speakers loudest up close; capped at full scale at the listener |
| `shuffle` | on | random songs when the queue is empty |
| `mute_vanilla` | on | silence the music box's own tune while a song plays |
| `muffle_walls` | on | music from a corral with High Walls (and/or Air Net) sounds muffled from outside |
| `dance_walls` | off | a ripple runs round the corral walls on every beat of the song the corral plays (queue or its Corral Radio); walls and Air Net glow and change colour (any distance while the plot is loaded, nearest 12); speakers in the corral make it stronger |
| `dance_intensity` | 1.0 (100 %) | 10-200 %, steeper above 100 % (shown while `dance_walls` is on) |
| `dance_lamps` | on | lamps (Electric Pillar, Striped, Conservatory, Lava, Ice) and Powderfall's glowing ice near a playing speaker pulse their glow and lights with the beat |
| `lamp_range` | 20 m | 5-100 m: the distance from a playing speaker within which lamps pulse (tooltip says so, round 14); full strength to 40 % of it (shown while `dance_lamps` is on) |

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
- lamps: `lamps: N lamp definition(s) (K of 5 lamp kinds found): …` (once), `lamps: no gadget definition with
  a lamp prefab ('decorElectricLampPillar', ...) among N …`, `lamps: pulsing lamp '…' at (x, y, z): N glowing
  renderer(s), M light(s): '…' brightness 5.00, '…' core (8.2, 4.5, 2.7), light 1200, …` (first 8), `lamps: N
  glowing ice crystal(s) in scene group 'G' (M LightControl(s) looked at, K on placed Ice Lamps, X ms[; other
  LightControl parents: ...])` (per scene group while music plays or the highlighter is on), `lamps:
  highlighter: …`, `lamps: glowing scene ice unavailable (…)`, `lamps pulse at N lamp(s) (M loaded lamp(s), S
  playing speaker(s), range R m)` (on a change), `lamps pulse stopped; lamps restored`, `'Lamps pulse to the
  music' unavailable (…)`
- radio menu: `radio menu ready[; NOT available: ...]`, `radio menu: game buttons not available (...)`, `radio menu
  for 'Corral NNNN' (plot '...')[: no radio channel (no Music Box|corral not found yet)]`, `radio menu opened (N rows
  per list, M song(s), font ..., key bar the game's popup button (game_button)|own pills, rows in the game
  button's colours|in the menu palette)` (first), `radio menu: game button not cloned (...) - own buttons`, `radio
  menu closed (why)`, `radio menu not opened: ...`
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
Lamps pulse: that `_BrightnessMultiplier` / `_ColorCore` / `_ColorEdge` in a property block visibly change the
lamp (a shader could ignore blocks: SRP Batcher materials take them, but it was not tested), that the pulse
looks in time and not too strong / weak, restore after Stop / Pause / out of range / setting off. Round 13:
the strength curve (one speaker gentle), the new lamps' Lights and emission (property blocks on HDRP Lit /
custom shaders), the Ice Lamp's LightControl not fighting the pulse, the scene ice scan cost and matches.

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
