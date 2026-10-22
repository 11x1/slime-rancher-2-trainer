#include "detour_guard.h"

#include <format>
#include <map>
#include <memory>
#include <mutex>

#include "log.h"
#include "module_manager.h"

namespace sr2::core {
namespace {

std::mutex& FlagMutex() {
    static std::mutex m;
    return m;
}

std::map<std::string, std::unique_ptr<std::atomic<bool>>, std::less<>>& Flags() {
    static std::map<std::string, std::unique_ptr<std::atomic<bool>>, std::less<>> m;
    return m;
}

}  // namespace

std::atomic<bool>& ModuleFaultFlag(std::string_view module) {
    std::lock_guard lock(FlagMutex());
    auto& m = Flags();
    auto it = m.find(module);
    if (it == m.end()) it = m.emplace(std::string(module), std::make_unique<std::atomic<bool>>(false)).first;
    return *it->second;
}

bool ModuleFaulted(std::string_view module) { return ModuleFaultFlag(module).load(); }

void MarkModuleFaulted(std::string_view module, std::string_view reason) {
    if (ModuleFaultFlag(module).exchange(true)) return;  // already faulted: stay quiet
    SR2_ERROR("module {}: FAULTED - {}. Its hooks now pass straight through to the game; module disabled.", module,
              reason);
    ModuleManager::Get().MarkFaulted(module, std::string(reason));
}

std::atomic<bool>& DetourGuard::Flag() const {
    std::atomic<bool>* f = flag_.load(std::memory_order_acquire);
    if (!f) {
        f = &ModuleFaultFlag(module_);
        flag_.store(f, std::memory_order_release);
    }
    return *f;
}

bool DetourGuard::RunThunk(void (*thunk)(void*), void* ctx) {
    std::string err;
    FaultInfo fault;
    if (GuardedCallEx(thunk, ctx, &err, &fault)) return true;
    if (fault.code) {
        MarkModuleFaulted(module_, std::format("{} in hook {}", err, hook_));
        return false;
    }
    int n = ++exceptions_;
    if (n <= 3 || n % 100 == 0)
        SR2_WARN("module {}: exception in hook {} (#{}): {} - call skipped", module_, hook_, n, err);
    if (n >= kMaxExceptions)
        MarkModuleFaulted(module_, std::format("{} exceptions in hook {} (last: {})", n, hook_, err));
    return false;
}

}  // namespace sr2::core
