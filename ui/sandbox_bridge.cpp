#include "sandbox_bridge.h"

#include <windows.h>

#include <atomic>
#include <mutex>

#include "core/main_thread.h"

namespace sr2::ui::sandbox {
namespace {

constexpr unsigned long long kViewedMs = 600;

std::mutex g_mutex;
std::shared_ptr<const Snapshot> g_snap;  // guarded by g_mutex
Status g_status;                          // guarded by g_mutex
std::atomic<void (*)(const Action&)> g_handler{nullptr};
std::atomic<unsigned long long> g_viewed{0};

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
        else SetStatus("Sandbox is off.", true);
    });
    return true;
}

}  // namespace sr2::ui::sandbox
