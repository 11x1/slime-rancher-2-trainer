#include "sandbox_extras_bridge.h"

#include <atomic>
#include <mutex>

#include "core/main_thread.h"
#include "ui/sandbox_bridge.h"

namespace sr2::ui::sandbox::extras {
namespace {

std::mutex g_mutex;
std::shared_ptr<const State> g_state;  // guarded by g_mutex
std::atomic<void (*)(Op, long long)> g_handler{nullptr};

}  // namespace

void Publish(State s) {
    auto p = std::make_shared<const State>(std::move(s));
    std::lock_guard lock(g_mutex);
    g_state = std::move(p);
}

void SetHandler(void (*fn)(Op, long long)) { g_handler.store(fn); }

std::shared_ptr<const State> Get() {
    std::lock_guard lock(g_mutex);
    return g_state;
}

bool Request(Op op, long long value) {
    namespace main_thread = ::sr2::core::main_thread;
    if (!main_thread::Ready()) {
        SetStatus("The game is not ready yet (main-thread dispatcher not running).", true);
        return false;
    }
    main_thread::Post([op, value] {
        if (auto fn = g_handler.load()) fn(op, value);
        else SetStatus("Sandbox is off.", true);
    });
    return true;
}

}  // namespace sr2::ui::sandbox::extras
