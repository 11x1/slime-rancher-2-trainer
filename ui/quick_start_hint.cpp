#include "quick_start_hint.h"

#include <windows.h>

#include <algorithm>
#include <atomic>

namespace sr2::ui::quick_start_hint {
namespace {

// 0 hidden, 1 pending, 2 staying
std::atomic<int> g_phase{0};
std::atomic<unsigned long long> g_stamp{0};     // GetTickCount64 of the last publish
std::atomic<unsigned long long> g_deadline{0};  // tick at which auto-continue fires, 0 = unknown

}  // namespace

void SetPending(double secondsLeft) {
    const unsigned long long now = GetTickCount64();
    g_deadline.store(secondsLeft < 0 ? 0 : now + static_cast<unsigned long long>(secondsLeft * 1000.0 + 0.5),
                     std::memory_order_relaxed);
    g_stamp.store(now, std::memory_order_relaxed);
    g_phase.store(1, std::memory_order_release);
}

void SetStaying() {
    g_stamp.store(GetTickCount64(), std::memory_order_relaxed);
    g_phase.store(2, std::memory_order_release);
}

void Clear() { g_phase.store(0, std::memory_order_release); }

View Get() {
    View v;
    const int phase = g_phase.load(std::memory_order_acquire);
    if (phase == 0) return v;
    const unsigned long long now = GetTickCount64();
    const unsigned long long stamp = g_stamp.load(std::memory_order_relaxed);
    const unsigned long long age = now > stamp ? now - stamp : 0;
    if (phase == 2) {
        if (age <= kStayingMs) v.phase = Phase::Staying;
        return v;
    }
    if (age > kStaleMs) return v;
    v.phase = Phase::Pending;
    const unsigned long long deadline = g_deadline.load(std::memory_order_relaxed);
    if (deadline != 0) v.secondsLeft = deadline > now ? static_cast<float>(deadline - now) / 1000.0f : 0.0f;
    return v;
}

}  // namespace sr2::ui::quick_start_hint
