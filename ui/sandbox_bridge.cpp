#include "sandbox_bridge.h"

#include <windows.h>

#include <atomic>
#include <mutex>

#include "core/main_thread.h"

namespace sr2::ui::sandbox {
namespace {

constexpr unsigned long long kViewedMs = 600;
constexpr unsigned long long kFreshWaitMs = 400;  // Current: how long a reopened tab waits for a new snapshot

std::mutex g_mutex;
std::shared_ptr<const Snapshot> g_snap;  // guarded by g_mutex
Status g_status;                          // guarded by g_mutex
std::atomic<void (*)(const Action&)> g_handler{nullptr};
std::atomic<unsigned long long> g_viewed{0};
std::atomic<unsigned> g_session{0};
std::atomic<unsigned long long> g_sessionStart{0};
std::atomic<bool> g_cheated{false};

}  // namespace

void SetSaveCheated(bool cheated) { g_cheated.store(cheated, std::memory_order_relaxed); }
bool SaveCheated() { return g_cheated.load(std::memory_order_relaxed); }

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

unsigned ViewSession() { return g_session.load(); }

void MarkViewed() {
    const unsigned long long now = GetTickCount64();
    const unsigned long long v = g_viewed.exchange(now, std::memory_order_relaxed);
    if (v == 0 || now - v >= kViewedMs) {  // shown again: the module has stopped refreshing since then
        g_sessionStart.store(now);
        g_session.fetch_add(1);
    }
}

bool Current(const Snapshot& s) {
    return s.view == g_session.load() || GetTickCount64() - g_sessionStart.load() >= kFreshWaitMs;
}

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
