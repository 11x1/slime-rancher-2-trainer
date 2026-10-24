# discord_rpc

Discord Rich Presence, native: the mod talks to the Discord desktop app over its local IPC pipe. No Discord SDK, no game SDK DLL, no network access of its own. Module key `discord_rpc`, **off by default**. UI: the Discord Rich Presence card on the overlay's Settings page (`ui/NOTES.md`).

| setting | kind | default | what it does |
|---|---|---|---|
| `enabled` | Bool | off | the module switch (the card's heading switch) |
| `official_app` | Bool | on | "Show as Slime Rancher 2": use Slime Rancher 2's own Discord application id (below) instead of `application_id` |
| `show_zone` | Bool | on | first line is the zone's name; off: "Playing" |
| `show_newbucks` | Bool | off | appends "· 12,345 Newbucks" to the second line |
| `member_list` | Enum | Area | "Member list shows": Game name / Area / Activity, sent as `status_display_type` 0 / 2 / 1 |
| `application_id` | string | `""` | the user's own Discord application id, used while `official_app` is off (`settings::GetString` / `SetString`, edited in the card) |

Not in profiles and not on the in-game Options "Mods" tab (personal, and the id can only be edited in the overlay).

## What it shows

| where | details (line 1) | state (line 2) | timer |
|---|---|---|---|
| title screen / loading without a player | In the main menu | - | none |
| in a save | zone name (`show_zone`), else "Playing" | "On the ranch" or "Exploring" (+ Newbucks) | elapsed since the save was loaded (`timestamps.start`) |
| game symbols missing | Playing | - | none |

- "In a save" = `SceneContextUtility.TryGetPlayerState` returns a live `PlayerState`. The timer starts the first time that is true after it was false (unix seconds), so it restarts when you go back to the menu and load again. Leaving needs 3 s (`kLeaveMs`) without a player state: a shorter gap keeps the save's presence and its timer (nothing is published meanwhile), so a missed read or a load inside the save can't restart the timer or flash "In the main menu". Logged: `in a save: the elapsed timer starts now`, `left the save: ...`.
- Zone: `SceneContext.PlayerZoneTracker.GetCurrentZone()`, else `GetLatestZone()` (the last non-null zone). Name: `ZoneDefinition._localizedName.GetLocalizedString()` (rich text stripped), else `_nameKey`. Cached per zone asset.
- Ranch: the zone's `_nameKey` or `_richPresenceId` contains "ranch" or "conservatory" (case-insensitive). **A guess:** every new zone is logged once as `[discord_rpc] zone "<name>" (name key "...", rich presence id "...")[: the ranch]`, so the first session shows whether the Conservatory is recognised. Fix the test if not.
- Newbucks: `CurrencyUtility.DefaultCurrency` (must be the `CurrencyDefinition` with `_persistenceId == 1`) and `PlayerState.GetCurrency`, as in the sandbox. Kept apart from the state in `Presence::extra` (ipc.cpp appends it when sending) so a Newbucks-only change can be told apart (see "Update rate").
- **Image:** `assets.large_image` is an https URL (Discord accepts external URLs for activity assets, no upload to the application needed): Slime Rancher 2's square cover art from its official Discord application, `https://cdn.discordapp.com/app-icons/1124351953024077845/3bcb29b4113fd3799b70218aafe5385d.png?size=512` (233 x 233 PNG, checked 2026-10-24; the app's icon hash `a772cec6b28fcd37794f8f51f0a7e92a` is only 32 x 32, the Steam header / capsule images are not square). `large_text` "Slime Rancher 2". If Discord answers with an ERROR naming `large_image` / `assets`, the presence is sent again without them for the rest of the connection.
- **Title:** Discord always shows the application's name ("Playing <name>"); it can't be set per activity. With `official_app` on that is "Slime Rancher 2". `status_display_type` (newer Discord clients) picks what the member list shows after "Playing": 0 the name, 1 `state`, 2 `details`. Sent only when not 0; if Discord answers with an ERROR naming `status_display_type`, it is left out for the rest of the connection (logged once).

## Threading

- Main thread: a frame callback 4 times a second (every 250 ms) while the module is on reads the state above (4-6 `runtime_invoke` calls, zone names cached per zone asset; native fault -> module faulted) and calls `discord_rpc::Publish(Presence)`, which returns at once when nothing changed, else copies it under a mutex and signals an event.
- Pipe thread (`ipc.cpp`): created on the first enable, lives for the rest of the process (never joined, so turning the module off never waits on it). It wakes on the event, every second, or when a waiting change is due. All pipe I/O happens there.
- `OnDisable` only flips a flag: the pipe thread sends a clearing `SET_ACTIVITY` (activity `null`), waits up to 0.5 s for Discord's answer (closing at once made Discord log `Socket Error: write EPIPE`) and closes the pipe.
- **Watchdog** (`discord_rpc::Watchdog`, called from the frame callback every frame, also while off; two atomic loads): the pipe thread stamps a heartbeat on every loop pass (at least once a second) and records what it is doing (waiting, opening the pipe, writing, reading, reading the settings). No heartbeat for 20 s = stuck: one WARN line naming the step, then `CancelSynchronousIo` on the thread (a blocked `ReadFile` / `WriteFile` / `CreateFile` fails with `ERROR_OPERATION_ABORTED`) and a flag that makes it drop the pipe and connect again; repeated every 20 s while it stays stuck, and "running again" is logged once it recovers. Stuck outside a pipe call (a lock) can't be cancelled; that is logged too. Not restarted as a new thread: the only non-I/O waits are locks the new thread would block on as well.
- **Unanswered updates:** every `SET_ACTIVITY` is answered by Discord (in order). An update without an answer for 10 s = the connection is dead even if the pipe is open: logged, dropped, reconnected.

## Protocol

- Pipe: the first of `\\.\pipe\discord-ipc-0` .. `-9` that opens (`CreateFileW`, read/write, synchronous handle).
- Frame: `uint32 opcode`, `uint32 length` (little-endian), then UTF-8 JSON. Opcodes 0 HANDSHAKE, 1 FRAME, 2 CLOSE, 3 PING, 4 PONG.
- Handshake: op 0 `{"v": 1, "client_id": "<id>"}`. Reply within 6 s (was 3 s; on 2026-10-24 15:08 Discord's own log shows two sockets we closed after exactly 3 s without an answer): op 1 with `"evt": "READY"` (and `data.user`, whose `global_name` / `username` goes into the status line) = connected; op 2 CLOSE (`{"code": 4000, "message": "Invalid Client ID"}` and the like) = refused.
- Presence: op 1 `{"cmd": "SET_ACTIVITY", "args": {"pid": <game pid>, "activity": {"details", "state", "timestamps": {"start"}, "assets": {"large_image", "large_text"}, "status_display_type"}}, "nonce": "<n>"}`. Fields shorter than 2 characters are left out (Discord rejects them), longer than 128 bytes are cut at a UTF-8 boundary.
- Replies are read without blocking (`PeekNamedPipe`, a frame is read only once its header is there). PING is answered with PONG and the same payload; CLOSE or a broken pipe = disconnected.
- **Results are logged once per change, not per send:** a successful `SET_ACTIVITY` answer logs `Discord shows: <details> | <state>, timer from HH:MM:SS` (or `no timer`) only when that differs from the last confirmed one (Newbucks left out, so selling doesn't log); an `"evt": "ERROR"` answer logs `Discord refused the presence (<what>): <message>` once per distinct message, and again after a success. The card's tooltip has the current "Discord shows: ..." line. A successful answer echoes the activity (`timestamps.start` turned into milliseconds, the image into `mp:external/...`).
- Sent only when the presence changed; see "Update rate" below. After a reconnect the current presence is sent again.

## Update rate

- **The limit:** `SET_ACTIVITY` is rate-limited to **5 updates per 20 seconds** per client. That number is from Discord's Game SDK documentation for `UpdateActivity`, which sends `SET_ACTIVITY` over the same pipe. The RPC docs give no other number, and a web search on 2026-10-24 found no official page that states it for RPC. Discord does not answer an over-limit update with an error: it queues or drops it. A third-party write-up claims one visible update per 15 s (not confirmed). So a presence can't follow the game every second the way a game's own UI does. 5 per 20 s is the ceiling for every game, SDK games included.
- **Discord's client throttles too:** its presence code (the cached web bundle, `didCommit` of the presence state) sends the gateway at most 5 presence updates per 20 s for the *whole* presence (Spotify, custom status and the game share it); extra ones are deferred with the latest state winning, not dropped. Its `SET_ACTIVITY` handler itself has no rate limit and applies every update locally at once (`LOCAL_ACTIVITY_UPDATE`).
- **What the mod does:** the main thread samples 4 times a second. The pipe thread sends a change once it has settled (300 ms without a newer change, but at most 1 s after the first one) and a slot is free in a sliding window of the last 5 sends (the oldest must be 20 s + 250 ms old). Otherwise it waits for that slot, and whatever the presence is then goes out: the newest wins and the states in between are skipped. The clearing `SET_ACTIVITY` (disable, id change) counts too. The window is kept across reconnects.
- **Newbucks are low priority:** a change of only `Presence::extra` goes out at most every 20 s after the previous send (`kMinorGapMs`), so selling never uses up the slots an area change needs; an area / state / timer change is sent as above and carries the current Newbucks with it. Before 2026-10-24 every sale was a send (up to 5 within 3 s, seen in Discord's log), which left later area changes waiting up to 20 s for a slot and filled Discord's own 5-per-20-s presence budget.
- A change that is undone before it is sent (back to what Discord already shows) is not sent.
- In practice an area change shows up within about 0.3 s, Newbucks within 20 s.
- The elapsed timer needs no updates: Discord draws it from `timestamps.start`.

## Reconnecting and the log

- The card's Status line shows only a short state:
  - "Connected as <name>" (green);
  - "Reconnecting..." (the handshake failed or the pipe broke);
  - "Discord not running";
  - "Refused: <Discord's reason, cut at 40 characters>" (red);
  - "Off";
  - with an own app, "No application id" or "Invalid application id".

  The technical detail is the line's hover tooltip (`bridge::Status::detail`): the pipe (`discord-ipc-N`), the handshake result, the CLOSE code and message, the number of updates sent, the last ERROR reply, when a rate-limited change goes out, and "Next try in N s" (the card counts it down from `Status::retryAt`). The text uses ASCII "...", not U+2026, which the overlay fonts may lack.
- Discord not running: retried after 5 s, doubling to every 30 s. The log gets "Discord not running (...)" once, not per retry.
- Discord quits: the next read fails, the pipe is closed and the retry starts again ("Reconnecting...", then "Discord not running" if it is gone).
- Id refused: retried every 2 minutes, or at once when the id changes.
- The id is polled every second, so pasting a new one, or switching `official_app`, reconnects with it (the old presence is cleared first).
- **Reconnect** (the card's button, `discord_bridge::RequestReconnect`): the pipe thread clears the presence, closes the pipe and connects again at once, skipping a retry wait or an id refusal's 2 minutes; it also forgets a refused `status_display_type` / image.
- The log only gets a line when the status text changes (connected, reconnecting, refused, ...; the tooltip detail in brackets), plus one per change of what Discord confirms ("Discord shows: ..."), one per distinct ERROR reply, one per new zone, the timer start / leaving a save, and the watchdog's lines. Every sent presence (with Newbucks) is logged at debug level.

## Finding 2026-10-24: "playtime not updating, nor the area"

Report after the feature/discord-polish-2 build (590ef8b, official app id): presence stuck, "maybe crashed the RPC?". What the logs show:

- `sr2mods.log` (16:06-16:47): connected once at 16:06:23, no ERROR reply, no reconnect, zones logged as they were entered; the pipe thread was alive at 16:44:15 (it reacted to "Show as Slime Rancher 2" being switched off within a second). The log had no line per send, so it could not show more; hence the "Discord shows" lines and the watchdog now.
- **Discord's own log** (`%APPDATA%\discord\logs\renderer_js.log`, `[RPCServer:IPC] Socket Message / Socket Emit`) settles it: the game's socket delivered **95 `SET_ACTIVITY` between 16:06:23 and 16:44:15**, each answered within ~20 ms; zone changes reached Discord 0.3 s after the zone log line (Rainbow Fields 16:12:50.256 -> 16:12:50.567, Ember Valley 16:31:58.112 -> .412, Starlight Strand 16:34:19.790 -> 16:34:20.100). The same payload sent by a test program got successful answers echoing details, state and `timestamps.start`. So: no crash, no deadlock, no stuck rate window, no refused payload, the timer start was sent unchanged.
- What did go wrong on our side: bursts. With Newbucks on, every sale was a send: up to 5 within 3 s (16:36:46.866-16:36:49.615 in Discord's log), so a following area change waited up to 20 s for a slot, and those bursts also used up Discord's own 5-per-20-s presence budget (shared with Spotify and the rest), so what other people see lagged further. A brief loss of the player state would also have reset the timer and flashed "In the main menu" (not proven to have happened, now impossible).
- Discord's renderer code (cached bundle): `SET_ACTIVITY` -> `LOCAL_ACTIVITY_UPDATE` with no throttling; the detected `SlimeRancher2.exe` game is hidden when an RPC activity has the same application id or name (it does); the gateway update is throttled 5 per 20 s, deferred not dropped.

## Why an application id is needed, and the official one

Discord's RPC handshake requires the `client_id` of an existing Discord application; the presence is shown under that application's name and icon.

- **Slime Rancher 2's own application (default, `official_app`):** id **`1124351953024077845`**, name "Slime Rancher 2". Found 2026-10-24 in Discord's public list of detectable games, `https://discord.com/api/v9/applications/detectable` (the entry with executable `slime rancher 2/slimerancher2.exe` and Steam SKU 1657630; Xbox, Epic, GOG and PlayStation SKUs too). `https://discord.com/api/v9/applications/1124351953024077845/rpc` returns the same app (icon `a772cec6b28fcd37794f8f51f0a7e92a`, cover image `3bcb29b4113fd3799b70218aafe5385d`). It is the application Discord already uses for "Playing Slime Rancher 2" through process detection, so the RPC presence replaces that line with the same name and icon plus our details. Nothing to create for the user. Untested: that Discord accepts an RPC handshake for an application the user doesn't own (it does for other games' detectable ids in common use; if refused, the status says "Discord refused the application id: ..." and an own app is the fallback). The original Slime Rancher is `363411395854991400`, not used.
- **The user's own app (`official_app` off):** free, two minutes in the Discord Developer Portal (the card's button opens https://discord.com/developers/applications); the profile shows that app's name after "Playing".

## Not seen in game

Everything: written from the documented protocol and the dump. To check: the official id's handshake, the cover image, `status_display_type` in the member list, the handshake and READY reply, that `SET_ACTIVITY` without assets shows both lines and the timer, the ranch detection (zone log line), the zone names, reconnecting when Discord starts after the game, and that turning the module off clears the profile at once.
