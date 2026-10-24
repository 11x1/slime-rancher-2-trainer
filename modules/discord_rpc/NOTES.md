# discord_rpc

Discord Rich Presence, native: the mod talks to the Discord desktop app over its local IPC pipe. No Discord SDK, no game SDK DLL, no network access of its own. Module key `discord_rpc`, **off by default**. UI: the Discord Rich Presence card on the overlay's Settings page (`ui/NOTES.md`).

| setting | kind | default | what it does |
|---|---|---|---|
| `enabled` | Bool | off | the module switch (the card's heading switch) |
| `show_zone` | Bool | on | first line is the zone's name; off: "Playing" |
| `show_newbucks` | Bool | off | appends "· 12,345 Newbucks" to the second line |
| `application_id` | string | `""` | the Discord application id (`settings::GetString` / `SetString`, edited in the card) |

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
- No image assets are sent (they would have to be uploaded to the user's own application first); Discord shows the application's name ("Playing <name>") and its icon if it has one.

## Threading

- Main thread: a frame callback every 2 s while the module is on reads the state above (a few `runtime_invoke` calls, native fault -> module faulted) and calls `discord_rpc::Publish(Presence)`, which only copies it under a mutex and signals an event.
- Pipe thread (`ipc.cpp`): created on the first enable, lives for the rest of the process (never joined, so turning the module off never waits on it). It wakes on the event or every second. All pipe I/O happens there.
- `OnDisable` only flips a flag: the pipe thread sends a clearing `SET_ACTIVITY` (activity `null`) and closes the pipe within a second.

## Protocol

- Pipe: the first of `\\.\pipe\discord-ipc-0` .. `-9` that opens (`CreateFileW`, read/write, synchronous handle).
- Frame: `uint32 opcode`, `uint32 length` (little-endian), then UTF-8 JSON. Opcodes 0 HANDSHAKE, 1 FRAME, 2 CLOSE, 3 PING, 4 PONG.
- Handshake: op 0 `{"v": 1, "client_id": "<id>"}`. Reply within 3 s: op 1 with `"evt": "READY"` (and `data.user`, whose `global_name` / `username` goes into the status line) = connected; op 2 CLOSE (`{"code": 4000, "message": "Invalid Client ID"}` and the like) = refused.
- Presence: op 1 `{"cmd": "SET_ACTIVITY", "args": {"pid": <game pid>, "activity": {"details", "state", "timestamps": {"start"}}}, "nonce": "<n>"}`. Fields shorter than 2 characters are left out (Discord rejects them), longer than 128 bytes are cut at a UTF-8 boundary.
- Replies are read without blocking (`PeekNamedPipe`, a frame is read only once its header is there). PING is answered with PONG and the same payload; CLOSE or a broken pipe = disconnected; an `"evt": "ERROR"` reply is logged once per distinct message.
- Sent only when the presence changed, at most once every 5 s (Discord allows 5 updates per 20 s); a change inside that window goes out when it ends. After a reconnect the current presence is sent again.

## Reconnecting and the log

- Discord not running: retried after 5 s, doubling to every 30 s. The status says "Waiting for Discord (the desktop app is not running)." and the log gets that line once, not per retry.
- Discord quits: the next read fails, the pipe is closed and the retry starts again ("Discord closed the connection; waiting for it.").
- Id refused: retried every 2 minutes, or at once when the id changes.
- The id is polled every second, so pasting a new one reconnects with it (the old presence is cleared first).
- The log only gets a line when the status text changes (connected, waiting, refused, ...), plus one per distinct ERROR reply and one per new zone. The presence text itself is logged at debug level.

## Why an application id is needed

Discord's RPC handshake requires the `client_id` of an existing Discord application; the presence is shown under that application's name. The mod can't ship its own without the author registering one (that id would then be a constant in `ipc.cpp`'s caller and the card's field would become optional). Options:
- **User's own app (what ships):** free, two minutes at discord.com/developers/applications; name it "Slime Rancher 2" and the profile reads "Playing Slime Rancher 2".
- **The game's own Discord application id**, if Slime Rancher 2 has one in Discord's detectable-games list (Discord already shows "Playing Slime Rancher 2" through process detection): the handshake works with any existing application id, and the presence would then show the game's official name and icon. Not done because the id could not be looked up offline here, and using another company's application for our presence is a judgement call for the user. If wanted: put that id into `application_id`, nothing else changes.

## Not seen in game

Everything: written from the documented protocol and the dump. To check: the handshake and READY reply, that `SET_ACTIVITY` without assets shows both lines and the timer, the ranch detection (zone log line), the zone names, reconnecting when Discord starts after the game, and that turning the module off clears the profile at once.
