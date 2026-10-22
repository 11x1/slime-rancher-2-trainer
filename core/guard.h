#pragma once
// Crash guards for calls into game code / module callbacks. A guarded call
// catches C++ exceptions (including IL2CPP managed exceptions, which IL2CPP
// raises as C++ exceptions) and SEH exceptions (access violations etc.).
//
//   std::string err;
//   if (!sr2::core::Try([&] { DoRiskyThing(); }, &err)) SR2_WARN("failed: {}", err);
//
// Notes: catching an SEH exception in the middle of game code can leave game
// state inconsistent; this is a "log and keep the process alive" net, not a
// substitute for null checks. Stack overflows are not recoverable.

#include <string>
#include <type_traits>
#include <utility>

namespace sr2::core {

// Runs thunk(ctx) under both a C++ try/catch and an SEH __try.
// Returns false (and fills *err if given) when anything was thrown.
bool GuardedCall(void (*thunk)(void*), void* ctx, std::string* err = nullptr);
struct FaultInfo {
    unsigned long code = 0;  // SEH exception code; 0 = C++ / managed exception (or none)
    void* address = nullptr; // faulting instruction (SEH only)
};
// Same, also reports SEH details. For SEH faults *err includes the address as
// "<module>+0x<offset>" (e.g. "GameAssembly.dll+0x44AF0B").
bool GuardedCallEx(void (*thunk)(void*), void* ctx, std::string* err, FaultInfo* fault);
// "<module file>+0x<offset>" for any code address ("0x<addr>" if not in a module).
std::string DescribeAddress(const void* p);

template <class F>
bool Try(F&& f, std::string* err = nullptr) {
    using Fn = std::remove_reference_t<F>;
    return GuardedCall([](void* p) { (*static_cast<Fn*>(p))(); }, static_cast<void*>(&f), err);
}

}  // namespace sr2::core
