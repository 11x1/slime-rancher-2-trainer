#pragma once
// discord_rpc: the Discord client's local IPC (named pipe \\.\pipe\discord-ipc-0..9),
// spoken natively on one background thread. No Discord SDK, no extra DLL.
//
// Framing (Discord RPC): little-endian uint32 opcode, uint32 payload length, then
// UTF-8 JSON. Opcodes: 0 HANDSHAKE {"v":1,"client_id":...}, 1 FRAME (commands and
// replies), 2 CLOSE, 3 PING, 4 PONG.
//
// The game's main thread only calls Publish() (a mutex and a copy); every pipe
// call happens on the worker, which reconnects quietly when Discord starts later.

#include <cstdint>
#include <string>

namespace sr2::modules::discord_rpc {

struct Presence {
    bool inGame = false;       // false: main menu (or loading)
    std::string details;       // first line ("In the main menu", the area name)
    std::string state;         // second line ("On the ranch", "Exploring", Newbucks)
    int64_t start = 0;         // unix seconds the save was loaded (0 = no timer)
    int displayType = 0;       // activity.status_display_type: 0 app name, 1 state, 2 details (member list)

    bool operator==(const Presence&) const = default;
};

// Any thread. Starts the worker on first use; on=false clears the activity and closes the pipe.
void SetActive(bool on);
// Any thread (the main thread in practice). Sent when it differs from the last one, rate-limited.
void Publish(const Presence& p);

}  // namespace sr2::modules::discord_rpc
