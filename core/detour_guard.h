#pragma once
// Guarded hook detours. Wrap a detour's MOD LOGIC (not the call to the
// original) in a DetourGuard:
//
//   static sr2::core::DetourGuard g_awakeGuard{"minimap", "MapUI.Awake"};
//
//   void MapUI_Awake_Detour(Il2CppObject* self, const MethodInfo* mi) {
//       g_origAwake(self, mi);                          // game code: always runs, unguarded
//       g_awakeGuard.Run([&] { OnMapUIAwake(self); });  // mod logic: guarded
//   }
//
//   // Detour that replaces / changes the original's result:
//   float Foo_Detour(Il2CppObject* self, float x, const MethodInfo* mi) {
//       float r = 0;
//       if (g_fooGuard.Run([&] { r = MyFoo(self, x); })) return r;
//       return g_origFoo(self, x, mi);                  // module faulted -> pure pass-through
//   }
//
// On a native fault (access violation etc.) inside Run():
//   - the module is marked faulted (ONE error line with the fault address),
//   - every DetourGuard of that module skips its logic from then on (Run()
//     returns false immediately, so the detours become pass-through),
//   - ModuleManager shows the module as FAILED with the reason, and the
//     module's hooks are disabled on the main thread at the next tick.
// C++ / managed exceptions skip that one call and are logged (rate-limited);
// after kMaxExceptions of them the module is marked faulted as well.
//
// Do not call the original from inside Run() unless you accept that a fault
// after that point makes the caller call it a second time (replace pattern).
// DetourGuard objects are constant-initialised (safe as namespace statics).

#include <atomic>
#include <string>
#include <string_view>

#include "guard.h"

namespace sr2::core {

// Per-module fault flag (created on demand, stable address, any thread).
std::atomic<bool>& ModuleFaultFlag(std::string_view module);
bool ModuleFaulted(std::string_view module);
// Marks a module faulted (idempotent; logs once): flag, ModuleManager state ->
// Failed(reason), hooks disabled on the main thread. Any thread.
void MarkModuleFaulted(std::string_view module, std::string_view reason);

class DetourGuard {
public:
    static constexpr int kMaxExceptions = 50;

    constexpr DetourGuard(const char* module, const char* hook) noexcept : module_(module), hook_(hook) {}
    DetourGuard(const DetourGuard&) = delete;
    DetourGuard& operator=(const DetourGuard&) = delete;

    // True while this module's logic may run (module not faulted).
    bool Active() const { return !Flag().load(std::memory_order_relaxed); }

    // Runs f() guarded. true = ran and returned normally; false = skipped
    // (module faulted) or failed (fault / exception, already logged).
    template <class F>
    bool Run(F&& f) {
        if (!Active()) return false;
        using Fn = std::remove_reference_t<F>;
        return RunThunk([](void* p) { (*static_cast<Fn*>(p))(); }, static_cast<void*>(&f));
    }

    const char* module() const { return module_; }
    const char* hook() const { return hook_; }

private:
    std::atomic<bool>& Flag() const;
    bool RunThunk(void (*thunk)(void*), void* ctx);

    const char* module_;
    const char* hook_;
    mutable std::atomic<std::atomic<bool>*> flag_{nullptr};
    std::atomic<int> exceptions_{0};
};

}  // namespace sr2::core
