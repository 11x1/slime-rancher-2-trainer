#include "sandbox_upgrades_bridge.h"

#include <atomic>
#include <mutex>

#include "core/main_thread.h"
#include "ui/sandbox_bridge.h"

namespace sr2::ui::sandbox::upgrades {
namespace {

std::mutex g_mutex;
std::shared_ptr<const State> g_state;  // guarded by g_mutex
std::atomic<void (*)(const Action&)> g_handler{nullptr};

}  // namespace

void Publish(State s) {
    auto p = std::make_shared<const State>(std::move(s));
    std::lock_guard lock(g_mutex);
    g_state = std::move(p);
}

void SetHandler(void (*fn)(const Action&)) { g_handler.store(fn); }

std::shared_ptr<const State> Get() {
    std::lock_guard lock(g_mutex);
    return g_state;
}

bool Request(Action a) {
    namespace main_thread = ::sr2::core::main_thread;
    if (!main_thread::Ready()) {
        SetStatus("The game is not ready yet (main-thread dispatcher not running).", true);
        return false;
    }
    main_thread::Post([a = std::move(a)] {
        if (auto fn = g_handler.load()) fn(a);
        else SetStatus("Sandbox is off.", true);
    });
    return true;
}

}  // namespace sr2::ui::sandbox::upgrades
