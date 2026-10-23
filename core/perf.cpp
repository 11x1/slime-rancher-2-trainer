#include "perf.h"

#include <windows.h>

#include <atomic>
#include <cstring>
#include <format>
#include <mutex>
#include <string>

#include "guard.h"
#include "il2cpp_api.h"
#include "log.h"
#include "main_thread.h"
#include "resolver.h"

namespace sr2::core::perf {
namespace {

constexpr double kWarnMs = 2.0;          // single item above this -> WARN (rate-limited per owner)
constexpr double kSummarySeconds = 10.0;  // INFO summary period
constexpr int kMaxSlots = 64;
constexpr size_t kNameLen = 56;

struct SlotData {
    char name[kNameLen] = {};
    std::atomic<int64_t> calls{0}, total{0}, max{0}, slow{0};
    std::atomic<int64_t> lastWarn{0}, suppressed{0};
    std::atomic<int64_t> frame{0};  // ticks since the last dispatcher tick (hitch attribution)
};

std::atomic<bool> g_enabled{false};
int64_t g_freq = 1;  // QPC ticks per second
int64_t g_warnTicks = 0;
SlotData g_slots[kMaxSlots];
std::atomic<int> g_slotCount{0};
std::mutex g_slotMutex;
std::atomic<int64_t> g_frameWork{0};  // mod work (all threads) since the last dispatcher tick

// System.GC.CollectionCount(int) (mscorlib icall wrapper) and IL2CPP heap exports.
using GcCountFn = int32_t (*)(int32_t, const MethodInfo*);
using GcSizeFn = int64_t (*)();
GcCountFn g_gcCount = nullptr;
const MethodInfo* g_gcCountMi = nullptr;
GcSizeFn g_gcUsed = nullptr, g_gcHeap = nullptr;

// Main-thread-only state (OnTick).
struct TickState {
    int64_t lastTick = 0, windowStart = 0;
    double emaMs = 0;
    int64_t frames = 0, sumTicks = 0, maxTicks = 0;
    int hitches = 0, hitchesSuppressed = 0;
    int64_t lastHitchLog = 0;
    int32_t gcLast = -1, gcLogged = -1, gcWindowStart = -1;
    int64_t lastGcLog = 0;
} g_t;

double Ms(int64_t ticks) { return static_cast<double>(ticks) * 1000.0 / static_cast<double>(g_freq); }

int32_t GcCount() {
    if (!g_gcCount) return -1;
    int32_t n = -1;
    if (!Try([&] { n = g_gcCount(0, g_gcCountMi); })) {
        g_gcCount = nullptr;  // never again
        SR2_WARN("perf: GC.CollectionCount failed - GC tracking off");
        return -1;
    }
    return n;
}

std::string HeapText() {
    if (!g_gcUsed || !g_gcHeap) return "n/a";
    int64_t used = 0, heap = 0;
    Try([&] {
        used = g_gcUsed();
        heap = g_gcHeap();
    });
    return std::format("{:.1f}/{:.1f} MB", static_cast<double>(used) / 1048576.0,
                       static_cast<double>(heap) / 1048576.0);
}

void Summary(int64_t now) {
    const double secs = static_cast<double>(now - g_t.windowStart) / static_cast<double>(g_freq);
    const double avg = g_t.frames ? Ms(g_t.sumTicks) / static_cast<double>(g_t.frames) : 0.0;
    const int32_t gc = g_t.gcLast;
    std::string gcText = gc < 0 ? std::string("n/a")
                                : std::format("+{} (total {})", g_t.gcWindowStart >= 0 ? gc - g_t.gcWindowStart : 0, gc);
    SR2_INFO("perf: {:.0f} s summary: {} frames, avg {:.2f} ms ({:.0f} fps), max {:.1f} ms, {} hitch(es), GC gen0 {}, "
             "managed heap {}",
             secs, g_t.frames, avg, avg > 0 ? 1000.0 / avg : 0.0, Ms(g_t.maxTicks), g_t.hitches, gcText, HeapText());
    const int n = g_slotCount.load(std::memory_order_acquire);
    for (int i = 0; i < n; ++i) {
        SlotData& s = g_slots[i];
        const int64_t calls = s.calls.exchange(0, std::memory_order_relaxed);
        const int64_t total = s.total.exchange(0, std::memory_order_relaxed);
        const int64_t mx = s.max.exchange(0, std::memory_order_relaxed);
        const int64_t slow = s.slow.exchange(0, std::memory_order_relaxed);
        if (!calls) continue;
        SR2_INFO("perf:   {:<30} {:>7} calls  avg {:.3f} ms  max {:.2f} ms  {} > {:.0f} ms  total {:.1f} ms", s.name,
                 calls, Ms(total) / static_cast<double>(calls), Ms(mx), slow, kWarnMs, Ms(total));
    }
    g_t.windowStart = now;
    g_t.frames = g_t.sumTicks = g_t.maxTicks = 0;
    g_t.hitches = 0;
    g_t.gcWindowStart = gc;
}

// " (top: a 40.12, b 3.20)": the owners with the most time in this frame (> 0.5 ms, up to 4).
// Render-thread owners (hook:ui.Present*) overlap the main thread's frame, they are not inside it.
std::string TopOwners(const int64_t* perSlot, int n) {
    const int64_t minTicks = g_freq / 2000;  // 0.5 ms
    int top[4] = {-1, -1, -1, -1};
    for (int i = 0; i < n; ++i) {
        if (perSlot[i] < minTicks) continue;
        for (int k = 0; k < 4; ++k) {
            if (top[k] < 0 || perSlot[i] > perSlot[top[k]]) {
                for (int m = 3; m > k; --m) top[m] = top[m - 1];
                top[k] = i;
                break;
            }
        }
    }
    if (top[0] < 0) return {};
    std::string s = " (top:";
    for (int k = 0; k < 4 && top[k] >= 0; ++k)
        s += std::format("{} {} {:.2f}", k ? "," : "", g_slots[top[k]].name, Ms(perSlot[top[k]]));
    return s + ")";
}

}  // namespace

void Init(bool enabled) {
    LARGE_INTEGER f{};
    QueryPerformanceFrequency(&f);
    g_freq = f.QuadPart > 0 ? f.QuadPart : 1;
    g_warnTicks = static_cast<int64_t>(kWarnMs * static_cast<double>(g_freq) / 1000.0);
    if (!enabled) {
        SR2_INFO("perf: off (\"perf_log\": false)");
        return;
    }
    std::string err;
    auto& r = il2cpp::Resolver::Get();
    if (Il2CppClass* gc = r.Class("mscorlib", "System", "GC", &err)) {
        if ((g_gcCountMi = r.Method(gc, "CollectionCount", 1, &err)))
            g_gcCount = reinterpret_cast<GcCountFn>(r.MethodPointer(g_gcCountMi, &err));
    }
    if (HMODULE ga = il2cpp::api().module) {
        g_gcUsed = reinterpret_cast<GcSizeFn>(GetProcAddress(ga, "il2cpp_gc_get_used_size"));
        g_gcHeap = reinterpret_cast<GcSizeFn>(GetProcAddress(ga, "il2cpp_gc_get_heap_size"));
    }
    g_enabled.store(true, std::memory_order_release);
    SR2_INFO("perf: on - WARN per item > {:.0f} ms, frame hitches, {:.0f} s summaries; GC tracking {}{}", kWarnMs,
             kSummarySeconds, g_gcCount ? "via System.GC.CollectionCount(0)" : "unavailable",
             g_gcCount ? "" : std::format(" ({})", err));
}

bool Enabled() { return g_enabled.load(std::memory_order_relaxed); }

int64_t Now() {
    if (!g_enabled.load(std::memory_order_relaxed)) return 0;
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return c.QuadPart;
}

int Slot(std::string_view owner) {
    if (!Enabled()) return -1;
    std::lock_guard lock(g_slotMutex);
    const int n = g_slotCount.load(std::memory_order_relaxed);
    for (int i = 0; i < n; ++i)
        if (owner.size() < kNameLen && std::strncmp(g_slots[i].name, owner.data(), owner.size()) == 0 &&
            g_slots[i].name[owner.size()] == '\0')
            return i;
    if (n >= kMaxSlots) return -1;
    const size_t len = owner.size() < kNameLen - 1 ? owner.size() : kNameLen - 1;
    std::memcpy(g_slots[n].name, owner.data(), len);
    g_slots[n].name[len] = '\0';
    g_slotCount.store(n + 1, std::memory_order_release);
    return n;
}

void Add(int slot, int64_t ticks, bool detail) {
    if (slot < 0 || slot >= kMaxSlots || ticks < 0) return;
    SlotData& s = g_slots[slot];
    s.calls.fetch_add(1, std::memory_order_relaxed);
    s.total.fetch_add(ticks, std::memory_order_relaxed);
    int64_t m = s.max.load(std::memory_order_relaxed);
    while (ticks > m && !s.max.compare_exchange_weak(m, ticks, std::memory_order_relaxed)) {
    }
    s.frame.fetch_add(ticks, std::memory_order_relaxed);
    if (!detail) g_frameWork.fetch_add(ticks, std::memory_order_relaxed);
    if (ticks <= g_warnTicks) return;
    s.slow.fetch_add(1, std::memory_order_relaxed);
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    int64_t last = s.lastWarn.load(std::memory_order_relaxed);
    if (c.QuadPart - last < g_freq || !s.lastWarn.compare_exchange_strong(last, c.QuadPart)) {
        s.suppressed.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const int64_t more = s.suppressed.exchange(0, std::memory_order_relaxed);
    SR2_WARN("perf: {} took {:.2f} ms (frame {}){}", s.name, Ms(ticks), main_thread::TickCount(),
             more ? std::format(" - {} more > {:.0f} ms since the last report", more, kWarnMs) : std::string());
}

void OnTick() {
    if (!Enabled()) return;
    const int64_t now = Now();
    const int64_t work = g_frameWork.exchange(0, std::memory_order_relaxed);
    // Per-owner time of this frame (read and cleared every tick, formatted only on a hitch).
    int64_t perSlot[kMaxSlots];
    const int nSlots = g_slotCount.load(std::memory_order_acquire);
    for (int i = 0; i < nSlots; ++i) perSlot[i] = g_slots[i].frame.exchange(0, std::memory_order_relaxed);
    const int32_t gc = GcCount();
    const int32_t gcDelta = (gc >= 0 && g_t.gcLast >= 0) ? gc - g_t.gcLast : 0;
    if (gc >= 0) g_t.gcLast = gc;
    if (g_t.gcWindowStart < 0) g_t.gcWindowStart = gc;

    if (!g_t.lastTick) {
        g_t.lastTick = g_t.windowStart = now;
        g_t.gcLogged = gc;
        return;
    }
    const int64_t ft = now - g_t.lastTick;
    g_t.lastTick = now;
    const double ftMs = Ms(ft);
    ++g_t.frames;
    g_t.sumTicks += ft;
    if (ft > g_t.maxTicks) g_t.maxTicks = ft;

    // Hitch: much longer than the running average (loading screens / alt-tab
    // frames > 1 s are not averaged in, but still reported).
    const bool warm = g_t.emaMs > 0;
    const bool hitch = warm && ftMs > 2.5 * g_t.emaMs && ftMs - g_t.emaMs > 10.0;
    if (hitch) {
        ++g_t.hitches;
        if (now - g_t.lastHitchLog >= g_freq / 4) {
            g_t.lastHitchLog = now;
            const int more = g_t.hitchesSuppressed;
            g_t.hitchesSuppressed = 0;
            SR2_WARN("perf: frame hitch {:.1f} ms (avg {:.1f} ms) at frame {} | mod work in it {:.2f} ms{} | GC {}{}",
                     ftMs, g_t.emaMs, main_thread::TickCount(), Ms(work), TopOwners(perSlot, nSlots),
                     gcDelta > 0 ? std::format("+{} collection(s) in this frame", gcDelta) : std::string("none"),
                     more ? std::format(" | {} more hitch(es) not logged", more) : std::string());
        } else {
            ++g_t.hitchesSuppressed;
        }
    }
    if (ftMs < 1000.0) {
        const double sample = warm && ftMs > 3.0 * g_t.emaMs ? 3.0 * g_t.emaMs : ftMs;
        g_t.emaMs = warm ? g_t.emaMs + (sample - g_t.emaMs) * 0.05 : ftMs;
    }

    // GC: report increments (aggregated, max 1 line/s) with the frame time.
    if (gcDelta > 0 && g_t.gcLogged >= 0 && now - g_t.lastGcLog >= g_freq) {
        SR2_INFO("perf: GC gen0 +{} in this frame (+{} since the last GC line, count {}) | this frame {:.1f} ms (avg "
                 "{:.1f} ms), mod work {:.2f} ms | managed heap {}",
                 gcDelta, gc - g_t.gcLogged, gc, ftMs, g_t.emaMs, Ms(work), HeapText());
        g_t.gcLogged = gc;
        g_t.lastGcLog = now;
    }

    if (static_cast<double>(now - g_t.windowStart) >= kSummarySeconds * static_cast<double>(g_freq)) Summary(now);
}

}  // namespace sr2::core::perf
