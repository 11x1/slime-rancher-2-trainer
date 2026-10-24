#include "discord_bridge.h"

#include <mutex>

namespace sr2::ui::discord {
namespace {

std::mutex g_mutex;
Status g_status{"Off.", Level::Dim};  // guarded by g_mutex

}  // namespace

void SetStatus(std::string text, Level level) {
    std::lock_guard lock(g_mutex);
    g_status.text = std::move(text);
    g_status.level = level;
}

Status GetStatus() {
    std::lock_guard lock(g_mutex);
    return g_status;
}

}  // namespace sr2::ui::discord
