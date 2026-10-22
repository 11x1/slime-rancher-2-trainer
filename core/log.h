#pragma once
// Thread-safe line logger: sr2mods.log next to the DLL (UTF-8, flushed per
// line so it survives crashes) + optional console in Debug builds.

#include <filesystem>
#include <format>
#include <string>
#include <string_view>

namespace sr2::log {

enum class Level { Trace = 0, Debug, Info, Warn, Error, Off };

Level ParseLevel(std::string_view s, Level fallback);
const char* LevelName(Level l);

// Opens (truncates) the log file. `console` is honoured only in Debug builds.
void Init(const std::filesystem::path& file, Level minLevel, bool console);
void SetLevel(Level minLevel);
bool Enabled(Level l);
void Write(Level l, std::string_view msg);
void Shutdown();

template <class... Args>
void Logf(Level l, std::format_string<Args...> fmt, Args&&... args) {
    if (!Enabled(l)) return;
    Write(l, std::format(fmt, std::forward<Args>(args)...));
}

// UTF-16 -> UTF-8 helper, used all over the place for paths / Il2CppStrings.
std::string Narrow(std::wstring_view w);

}  // namespace sr2::log

#define SR2_TRACE(...) ::sr2::log::Logf(::sr2::log::Level::Trace, __VA_ARGS__)
#define SR2_DEBUG(...) ::sr2::log::Logf(::sr2::log::Level::Debug, __VA_ARGS__)
#define SR2_INFO(...)  ::sr2::log::Logf(::sr2::log::Level::Info, __VA_ARGS__)
#define SR2_WARN(...)  ::sr2::log::Logf(::sr2::log::Level::Warn, __VA_ARGS__)
#define SR2_ERROR(...) ::sr2::log::Logf(::sr2::log::Level::Error, __VA_ARGS__)
