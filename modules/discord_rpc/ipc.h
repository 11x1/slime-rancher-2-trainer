#pragma once
// discord_rpc: the Discord client's local IPC (named pipe \\.\pipe\discord-ipc-0..9),
// spoken natively on one background thread. No Discord SDK, no extra DLL.
//
// Framing (Discord RPC): little-endian uint32 opcode, uint32 payload length, then
// UTF-8 JSON. Opcodes: 0 HANDSHAKE {"v":1,"client_id":...}, 1 FRAME (commands and
// replies), 2 CLOSE, 3 PING, 4 PONG.
//
// The game's main thread only calls Publish() (a mutex and a copy) and Watchdog()
// (two atomic loads); every pipe call happens on the worker, which reconnects
// quietly when Discord starts later.

#include <cstdint>
#include <string>

namespace sr2::modules::discord_rpc {

struct Presence {
    bool inGame = false;       // false: main menu (or loading)
    std::string details;       // first line ("In the main menu", the area name)
    std::string state;         // second line ("On the ranch", "Exploring")
    std::string extra;         // appended to the second line (" · 12,345 Newbucks"); a change of only this
                               // is minor and goes out at most every 20 s, so it never delays an area change
    int64_t start = 0;         // unix seconds the save was loaded (0 = no timer)
    int displayType = 0;       // activity.status_display_type: 0 app name, 1 state, 2 details (member list)

    bool operator==(const Presence&) const = default;
};

// Any thread. Starts the worker on first use; on=false clears the activity and closes the pipe.
void SetActive(bool on);
// Any thread (the main thread in practice). Sent when it differs from the last one, rate-limited.
void Publish(const Presence& p);
// Main thread, every frame is fine (two atomic loads). If the pipe thread has made no progress for
// a while: logs it, cancels its blocked pipe call and makes it reconnect.
void Watchdog();

}  // namespace sr2::modules::discord_rpc
