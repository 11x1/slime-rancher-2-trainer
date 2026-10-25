#pragma once
// Debug highlighter registry (round 14: "maybe add a debug menu to highlight what you want me to check, which
// entities/buildables/map parts"). Modules publish world-space boxes of what they touch, by category (slime_dj:
// pulsing lamps, idle lamps, glowing ice crystals, speakers); debug_tools' "Highlight what the mod touches"
// draws them as labelled wireframes through ui/hud.h (modules/debug_tools/highlight.cpp).
//
// Publishers check Wanted() first and do no extra work while the highlighter is off. A category not
// republished for kStaleMs is dropped (the module stopped, was disabled or faulted). Any thread; the data is
// copied under a mutex (in practice both sides run on the Unity main thread).

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace sr2::modules::common::debug_marks {

inline constexpr unsigned long long kStaleMs = 6000;

struct Box {
    float x = 0, y = 0, z = 0;     // centre (world)
    float hx = 0.5f, hy = 0.5f, hz = 0.5f;  // half extents (world axes)
    float ring = 0;                // > 0: also a horizontal circle of this radius round the centre (a range)
};

struct Category {
    std::string key;    // "<module>.<what>"
    std::string label;  // shown above each box
    uint32_t color = 0xFFFFFFFF;  // IM_COL32 order (0xAABBGGRR)
    std::vector<Box> boxes;
    unsigned long long stamp = 0;
};

namespace detail {
struct Store {
    std::mutex m;
    std::map<std::string, Category, std::less<>> cats;
};
inline Store& S() {
    static Store s;
    return s;
}
inline std::atomic<bool>& WantFlag() {
    static std::atomic<bool> f{false};
    return f;
}
}  // namespace detail

// Set by debug_tools (option on and module on); read by publishers.
inline bool Wanted() { return detail::WantFlag().load(std::memory_order_relaxed); }
inline void SetWanted(bool on) { detail::WantFlag().store(on, std::memory_order_relaxed); }

// Replaces the category's boxes (an empty list keeps the category, with nothing to draw).
inline void Publish(std::string_view key, std::string_view label, uint32_t color, std::vector<Box> boxes) {
    auto& s = detail::S();
    std::lock_guard lk(s.m);
    auto it = s.cats.find(key);
    if (it == s.cats.end()) it = s.cats.emplace(std::string(key), Category{}).first;
    Category& c = it->second;
    c.key = std::string(key);
    c.label = std::string(label);
    c.color = color;
    c.boxes = std::move(boxes);
    c.stamp = GetTickCount64();
}

inline void Remove(std::string_view key) {
    auto& s = detail::S();
    std::lock_guard lk(s.m);
    if (auto it = s.cats.find(key); it != s.cats.end()) s.cats.erase(it);
}

// Every category republished within kStaleMs (stale ones are dropped).
inline std::vector<Category> Snapshot() {
    auto& s = detail::S();
    const unsigned long long now = GetTickCount64();
    std::vector<Category> out;
    std::lock_guard lk(s.m);
    for (auto it = s.cats.begin(); it != s.cats.end();) {
        if (now - it->second.stamp > kStaleMs) {
            it = s.cats.erase(it);
        } else {
            out.push_back(it->second);
            ++it;
        }
    }
    return out;
}

}  // namespace sr2::modules::common::debug_marks
