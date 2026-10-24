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

- "In a save" = `SceneContextUtility.TryGetPlayerState` returns a live `PlayerState`. The timer starts the first time that is true after it was false (unix seconds), so it restarts when you go back to the menu and load again.
- Zone: `SceneContext.PlayerZoneTracker.GetCurrentZone()`, else `GetLatestZone()` (the last non-null zone). Name: `ZoneDefinition._localizedName.GetLocalizedString()` (rich text stripped), else `_nameKey`. Cached per zone asset.
- Ranch: the zone's `_nameKey` or `_richPresenceId` contains "ranch" or "conservatory" (case-insensitive). **A guess:** every new zone is logged once as `[discord_rpc] zone "<name>" (name key "...", rich presence id "...")[: the ranch]`, so the first session shows whether the Conservatory is recognised. Fix the test if not.
- Newbucks: `CurrencyUtility.DefaultCurrency` (must be the `CurrencyDefinition` with `_persistenceId == 1`) and `PlayerState.GetCurrency`, as in the sandbox.
- **Image:** `assets.large_image` is an https URL (Discord accepts external URLs for activity assets, no upload to the application needed): Slime Rancher 2's square cover art from its official Discord application, `https://cdn.discordapp.com/app-icons/1124351953024077845/3bcb29b4113fd3799b70218aafe5385d.png?size=512` (233 x 233 PNG, checked 2026-10-24; the app's icon hash `a772cec6b28fcd37794f8f51f0a7e92a` is only 32 x 32, the Steam header / capsule images are not square). `large_text` "Slime Rancher 2". If Discord answers with an ERROR naming `large_image` / `assets`, the presence is sent again without them for the rest of the connection.
- **Title:** Discord always shows the application's name ("Playing <name>"); it can't be set per activity. With `official_app` on that is "Slime Rancher 2". `status_display_type` (newer Discord clients) picks what the member list shows after "Playing": 0 the name, 1 `state`, 2 `details`. Sent only when not 0; if Discord answers with an ERROR naming `status_display_type`, it is left out for the rest of the connection (logged once).

## Threading

- Main thread: a frame callback every 2 s while the module is on reads the state above (a few `runtime_invoke` calls, native fault -> module faulted) and calls `discord_rpc::Publish(Presence)`, which only copies it under a mutex and signals an event.
- Pipe thread (`ipc.cpp`): created on the first enable, lives for the rest of the process (never joined, so turning the module off never waits on it). It wakes on the event or every second. All pipe I/O happens there.
- `OnDisable` only flips a flag: the pipe thread sends a clearing `SET_ACTIVITY` (activity `null`) and closes the pipe within a second.

## Protocol

- Pipe: the first of `\\.\pipe\discord-ipc-0` .. `-9` that opens (`CreateFileW`, read/write, synchronous handle).
- Frame: `uint32 opcode`, `uint32 length` (little-endian), then UTF-8 JSON. Opcodes 0 HANDSHAKE, 1 FRAME, 2 CLOSE, 3 PING, 4 PONG.
- Handshake: op 0 `{"v": 1, "client_id": "<id>"}`. Reply within 3 s: op 1 with `"evt": "READY"` (and `data.user`, whose `global_name` / `username` goes into the status line) = connected; op 2 CLOSE (`{"code": 4000, "message": "Invalid Client ID"}` and the like) = refused.
- Presence: op 1 `{"cmd": "SET_ACTIVITY", "args": {"pid": <game pid>, "activity": {"details", "state", "timestamps": {"start"}, "assets": {"large_image", "large_text"}, "status_display_type"}}, "nonce": "<n>"}`. Fields shorter than 2 characters are left out (Discord rejects them), longer than 128 bytes are cut at a UTF-8 boundary.
- Replies are read without blocking (`PeekNamedPipe`, a frame is read only once its header is there). PING is answered with PONG and the same payload; CLOSE or a broken pipe = disconnected; an `"evt": "ERROR"` reply is logged once per distinct message.
- Sent only when the presence changed, at most once every 5 s (Discord allows 5 updates per 20 s); a change inside that window goes out when it ends. After a reconnect the current presence is sent again.

## Reconnecting and the log

- Discord not running: retried after 5 s, doubling to every 30 s. The status says "Waiting for Discord (the desktop app is not running)." and the log gets that line once, not per retry.
- Discord quits: the next read fails, the pipe is closed and the retry starts again ("Discord closed the connection; waiting for it.").
- Id refused: retried every 2 minutes, or at once when the id changes.
- The id is polled every second, so pasting a new one, or switching `official_app`, reconnects with it (the old presence is cleared first).
- **Reconnect** (the card's button, `discord_bridge::RequestReconnect`): the pipe thread clears the presence, closes the pipe and connects again at once, skipping a retry wait or an id refusal's 2 minutes; it also forgets a refused `status_display_type` / image.
- The log only gets a line when the status text changes (connected, waiting, refused, ...), plus one per distinct ERROR reply and one per new zone. The presence text itself is logged at debug level.

## Why an application id is needed, and the official one

Discord's RPC handshake requires the `client_id` of an existing Discord application; the presence is shown under that application's name and icon.

- **Slime Rancher 2's own application (default, `official_app`):** id **`1124351953024077845`**, name "Slime Rancher 2". Found 2026-10-24 in Discord's public list of detectable games, `https://discord.com/api/v9/applications/detectable` (the entry with executable `slime rancher 2/slimerancher2.exe` and Steam SKU 1657630; Xbox, Epic, GOG and PlayStation SKUs too). `https://discord.com/api/v9/applications/1124351953024077845/rpc` returns the same app (icon `a772cec6b28fcd37794f8f51f0a7e92a`, cover image `3bcb29b4113fd3799b70218aafe5385d`). It is the application Discord already uses for "Playing Slime Rancher 2" through process detection, so the RPC presence replaces that line with the same name and icon plus our details. Nothing to create for the user. Untested: that Discord accepts an RPC handshake for an application the user doesn't own (it does for other games' detectable ids in common use; if refused, the status says "Discord refused the application id: ..." and an own app is the fallback). The original Slime Rancher is `363411395854991400`, not used.
- **The user's own app (`official_app` off):** free, two minutes in the Discord Developer Portal (the card's button opens https://discord.com/developers/applications); the profile shows that app's name after "Playing".

## Not seen in game

Everything: written from the documented protocol and the dump. To check: the official id's handshake, the cover image, `status_display_type` in the member list, the handshake and READY reply, that `SET_ACTIVITY` without assets shows both lines and the timer, the ranch detection (zone log line), the zone names, reconnecting when Discord starts after the game, and that turning the module off clears the profile at once.
