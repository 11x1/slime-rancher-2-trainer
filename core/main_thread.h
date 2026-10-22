#pragma once
// Unity main-thread dispatcher. A hook on a managed per-frame method (see
// main_thread.cpp for which one) drains a work queue and runs per-frame
// callbacks on the Unity main thread. Every queued job / callback runs inside
// an SEH + C++ exception guard: a failing frame callback is logged and removed,
// a failing posted job is logged and dropped; the game keeps running.

#include <functional>
#include <string>
#include <string_view>

namespace sr2::core::main_thread {

bool Ready();         // dispatcher hook installed and has ticked at least once
bool IsMainThread();  // true on the thread that runs the dispatcher tick

// Run once on the Unity main thread at the next tick (any thread may call).
// Posting from the main thread itself runs it at the NEXT tick, not inline.
void Post(std::function<void()> fn);

// Every tick, on the main thread. Returns an id > 0 (0 on failure).
int AddFrameCallback(std::string_view owner, std::function<void(float unscaledDt)> fn);
void RemoveFrameCallback(int id);  // safe from any thread, also from inside a callback

// ---- additions (core-internal / diagnostics) ----
// Installs the dispatcher hook. Called once by the core worker thread.
bool Install(std::string* err = nullptr);
// Number of ticks seen so far.
unsigned long long TickCount();
// Human readable description of the hooked method ("Image:Ns.Class::Method").
std::string HookDescription();

}  // namespace sr2::core::main_thread
