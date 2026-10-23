#pragma once
// Lightweight timing of the mod's own work ("perf_log" in sr2mods.json,
// default on for now). Everything is a few atomics + QueryPerformanceCounter
// per measured item; nothing allocates on the hot path.
//
// What gets logged (all lines start with "perf:"):
//   WARN  perf: <owner> took X ms ...          one item > 2 ms (max 1 line/s per owner)
//   WARN  perf: frame hitch X ms ...           a frame much longer than the running average,
//                                              with the mod's work in that frame, the owners
//                                              that spent the most of it ("top: a 40.1, b 3.2") and GC info
//   INFO  perf: GC gen0 +N ...                 managed GC count went up (max 1 line/s)
//   INFO  perf: 10 s summary ...               frames / avg / max / hitches / GC / heap,
//                                              then one line per owner: calls, avg, max
//
// Owners: "frame:<name>" (main-thread frame callbacks), "main_thread.posted"
// (posted jobs), "hook:<name>" (per-frame / per-tick hook detours).

#include <cstdint>
#include <string_view>

namespace sr2::core::perf {

// Worker thread, once, after the IL2CPP resolver is ready (resolves GC.CollectionCount).
void Init(bool enabled);
bool Enabled();

// QueryPerformanceCounter ticks; 0 when profiling is disabled.
int64_t Now();

// Stable id for an owner name (the name is copied). -1 when disabled / table full.
int Slot(std::string_view owner);

// Record one measured item (any thread). detail: a sub-part of another owner's item (statistics
// and WARN lines only, not added to the frame's mod work a second time).
void Add(int slot, int64_t ticks, bool detail = false);

// Main-thread dispatcher, once per frame: frame time, hitches, GC, 10 s summary.
void OnTick();

class Scope {
public:
    explicit Scope(int slot, bool detail = false) : slot_(slot), t0_(slot >= 0 ? Now() : 0), detail_(detail) {}
    ~Scope() {
        if (t0_) Add(slot_, Now() - t0_, detail_);
    }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

private:
    int slot_;
    int64_t t0_;
    bool detail_;
};

}  // namespace sr2::core::perf

#define SR2_PERF_CAT2_(a, b) a##b
#define SR2_PERF_CAT_(a, b) SR2_PERF_CAT2_(a, b)
// Times the rest of the enclosing block under `name` (a string literal).
#define SR2_PERF_SCOPE(name)                                                                       \
    static const int SR2_PERF_CAT_(sr2PerfSlot_, __LINE__) = ::sr2::core::perf::Slot(name);         \
    ::sr2::core::perf::Scope SR2_PERF_CAT_(sr2PerfScope_, __LINE__)(SR2_PERF_CAT_(sr2PerfSlot_, __LINE__))
// Same, for a part of an item that is already timed by an enclosing SR2_PERF_SCOPE.
#define SR2_PERF_DETAIL(name)                                                                          static const int SR2_PERF_CAT_(sr2PerfSlot_, __LINE__) = ::sr2::core::perf::Slot(name);             ::sr2::core::perf::Scope SR2_PERF_CAT_(sr2PerfScope_, __LINE__)(SR2_PERF_CAT_(sr2PerfSlot_, __LINE__), true)
