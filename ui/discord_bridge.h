#pragma once
// Data exchange between the discord_rpc module (modules/discord_rpc) and its card
// on the overlay's Settings page (menu.cpp). The module's pipe thread sets a
// short connection status plus its technical detail (the Status line's tooltip);
// the render thread reads it. The application id is
// a free-text value in sr2mods.json (settings::GetString / SetString), which the
// card edits and the pipe thread polls.

#include <string>

namespace sr2::ui::discord {

inline constexpr const char* kModule = "discord_rpc";
inline constexpr const char* kKeyAppId = "application_id";     // settings::GetString: the Discord application id
inline constexpr const char* kKeyNewbucks = "show_newbucks";   // Bool setting
inline constexpr const char* kKeyZone = "show_zone";           // Bool setting
inline constexpr const char* kKeyOfficial = "official_app";    // Bool setting: use kOfficialAppId instead of the field
inline constexpr const char* kKeyMemberList = "member_list";   // Enum setting: what the member list shows
// Slime Rancher 2's own Discord application (Discord's detectable-games list, see
// modules/discord_rpc/NOTES.md): name and icon then come from Discord.
inline constexpr const char* kOfficialAppId = "1124351953024077845";
inline constexpr const char* kDevPortalUrl = "https://discord.com/developers/applications";

enum class Level { Dim, Ok, Warn, Error };

struct Status {
    std::string text;                // short: "Connected as <name>", "Reconnecting...", "Discord not running", ...
    Level level = Level::Dim;
    std::string detail;              // the Status line's tooltip: pipe, handshake, error codes
    unsigned long long retryAt = 0;  // GetTickCount64 of the next connection attempt (0 = none planned)
};

void SetStatus(Status s);  // any thread
Status GetStatus();                             // any thread

// The card's Reconnect button (any thread): the pipe thread drops the connection
// and connects again at once, skipping any retry wait.
void RequestReconnect();
bool TakeReconnect();  // pipe thread: true once per request

}  // namespace sr2::ui::discord
