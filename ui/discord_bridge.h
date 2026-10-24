#pragma once
// Data exchange between the discord_rpc module (modules/discord_rpc) and its card
// on the overlay's Settings page (menu.cpp). The module's pipe thread sets a
// one-line connection status; the render thread reads it. The application id is
// a free-text value in sr2mods.json (settings::GetString / SetString), which the
// card edits and the pipe thread polls.

#include <string>

namespace sr2::ui::discord {

inline constexpr const char* kModule = "discord_rpc";
inline constexpr const char* kKeyAppId = "application_id";     // settings::GetString: the Discord application id
inline constexpr const char* kKeyNewbucks = "show_newbucks";   // Bool setting
inline constexpr const char* kKeyZone = "show_zone";           // Bool setting

enum class Level { Dim, Ok, Warn, Error };

struct Status {
    std::string text;
    Level level = Level::Dim;
};

void SetStatus(std::string text, Level level);  // any thread
Status GetStatus();                             // any thread

}  // namespace sr2::ui::discord
