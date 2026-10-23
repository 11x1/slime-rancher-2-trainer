#include "slime_dj_bridge.h"

#include <windows.h>

#include <atomic>
#include <mutex>

#include "core/log.h"
#include "core/main_thread.h"

namespace sr2::ui::slime_dj {
namespace {

constexpr unsigned long long kViewedMs = 600;

std::mutex g_mutex;
std::shared_ptr<const Snapshot> g_snap;  // guarded by g_mutex
Status g_status;                          // guarded by g_mutex
std::atomic<void (*)(const Action&)> g_handler{nullptr};
std::atomic<unsigned long long> g_viewed{0};

std::wstring Widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n > 0 ? n : 0), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

// explorer.exe on the folder, started from its own thread so the caller (the render thread)
// never waits for process creation. Only kernel32: shell32 is delay-loaded and only linked in
// ImGui builds.
DWORD WINAPI OpenFolderThread(LPVOID p) {
    std::wstring* dir = static_cast<std::wstring*>(p);
    std::wstring cmd = L"explorer.exe \"" + *dir + L"\"";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    } else {
        SR2_WARN("slime_dj: cannot open the music folder (CreateProcess error {})", GetLastError());
    }
    delete dir;
    return 0;
}

}  // namespace

void Publish(Snapshot s) {
    auto p = std::make_shared<const Snapshot>(std::move(s));
    std::lock_guard lock(g_mutex);
    g_snap = std::move(p);
}

void SetStatus(std::string text, bool error) {
    std::lock_guard lock(g_mutex);
    g_status.text = std::move(text);
    g_status.error = error;
    g_status.tick = GetTickCount64();
}

void SetHandler(void (*fn)(const Action&)) { g_handler.store(fn); }

bool WantsSnapshot() {
    const unsigned long long v = g_viewed.load(std::memory_order_relaxed);
    return v != 0 && GetTickCount64() - v < kViewedMs;
}

std::shared_ptr<const Snapshot> Get() {
    std::lock_guard lock(g_mutex);
    return g_snap;
}

Status GetStatus() {
    std::lock_guard lock(g_mutex);
    return g_status;
}

void MarkViewed() { g_viewed.store(GetTickCount64(), std::memory_order_relaxed); }

bool Request(const Action& a) {
    namespace main_thread = ::sr2::core::main_thread;
    if (!main_thread::Ready()) {
        SetStatus("The game is not ready yet (main-thread dispatcher not running).", true);
        return false;
    }
    main_thread::Post([a] {
        if (auto fn = g_handler.load()) fn(a);
        else SetStatus("Slime DJ is off.", true);
    });
    return true;
}

void OpenFolder(const std::string& utf8Path) {
    if (utf8Path.empty()) return;
    auto* dir = new std::wstring(Widen(utf8Path));
    HANDLE t = CreateThread(nullptr, 0, &OpenFolderThread, dir, 0, nullptr);
    if (t) CloseHandle(t);
    else delete dir;
}

}  // namespace sr2::ui::slime_dj
