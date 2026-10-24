#include "discord_bridge.h"

#include <atomic>
#include <mutex>

namespace sr2::ui::discord {
namespace {

std::mutex g_mutex;
Status g_status{"Off", Level::Dim, {}, 0};  // guarded by g_mutex
std::atomic<bool> g_reconnect{false};

}  // namespace

void SetStatus(Status s) {
    std::lock_guard lock(g_mutex);
    g_status = std::move(s);
}

Status GetStatus() {
    std::lock_guard lock(g_mutex);
    return g_status;
}

void RequestReconnect() { g_reconnect.store(true); }

bool TakeReconnect() { return g_reconnect.exchange(false); }

}  // namespace sr2::ui::discord
