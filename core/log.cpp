#include "log.h"

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <mutex>

namespace sr2::log {
namespace {

std::mutex g_mutex;
FILE* g_file = nullptr;
std::atomic<int> g_level{static_cast<int>(Level::Info)};
bool g_console = false;

}  // namespace

Level ParseLevel(std::string_view s, Level fallback) {
    auto eq = [&](const char* k) { return _strnicmp(s.data(), k, s.size()) == 0 && k[s.size()] == '\0'; };
    if (s.empty()) return fallback;
    if (eq("trace")) return Level::Trace;
    if (eq("debug")) return Level::Debug;
    if (eq("info")) return Level::Info;
    if (eq("warn") || eq("warning")) return Level::Warn;
    if (eq("error")) return Level::Error;
    if (eq("off") || eq("none")) return Level::Off;
    return fallback;
}

const char* LevelName(Level l) {
    switch (l) {
        case Level::Trace: return "TRACE";
        case Level::Debug: return "DEBUG";
        case Level::Info: return "INFO ";
        case Level::Warn: return "WARN ";
        case Level::Error: return "ERROR";
        default: return "?????";
    }
}

std::string Narrow(std::wstring_view w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), out.data(), n, nullptr, nullptr);
    return out;
}

void Init(const std::filesystem::path& file, Level minLevel, bool console) {
    std::lock_guard lock(g_mutex);
    g_level = static_cast<int>(minLevel);
    if (!g_file) {
        // Keep the previous run's log (e.g. the one that ended in a crash):
        // sr2mods.log -> sr2mods.prev.log (replacing the older prev).
        std::filesystem::path prev = file;
        prev.replace_extension(L".prev" + file.extension().wstring());
        if (GetFileAttributesW(file.c_str()) != INVALID_FILE_ATTRIBUTES &&
            !MoveFileExW(file.c_str(), prev.c_str(), MOVEFILE_REPLACE_EXISTING))
            CopyFileW(file.c_str(), prev.c_str(), FALSE);  // e.g. someone is tailing it
        // Truncate on every start; other processes may read (tail) but not write.
        g_file = _wfsopen(file.c_str(), L"wb", _SH_DENYWR);
    }
#ifdef _DEBUG
    if (console && !g_console && AllocConsole()) {
        SetConsoleTitleW(L"sr2mods");
        SetConsoleOutputCP(CP_UTF8);
        g_console = true;
    }
#else
    (void)console;
#endif
}

void SetLevel(Level minLevel) { g_level = static_cast<int>(minLevel); }

bool Enabled(Level l) { return static_cast<int>(l) >= g_level.load(std::memory_order_relaxed) && l != Level::Off; }

void Write(Level l, std::string_view msg) {
    if (!Enabled(l)) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    std::string line = std::format("{:04}-{:02}-{:02} {:02}:{:02}:{:02}.{:03} [{}] [{:5}] {}\r\n", t.wYear, t.wMonth,
                                   t.wDay, t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, LevelName(l),
                                   GetCurrentThreadId(), msg);
    std::lock_guard lock(g_mutex);
    if (g_file) {
        fwrite(line.data(), 1, line.size(), g_file);
        fflush(g_file);
    }
    if (g_console) {
        DWORD written = 0;
        WriteConsoleA(GetStdHandle(STD_OUTPUT_HANDLE), line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
    }
    OutputDebugStringA(line.c_str());
}

void Shutdown() {
    std::lock_guard lock(g_mutex);
    if (g_file) {
        fclose(g_file);
        g_file = nullptr;
    }
}

}  // namespace sr2::log
