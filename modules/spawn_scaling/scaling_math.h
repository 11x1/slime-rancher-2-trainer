#pragma once
// Pure value math for the spawn_scaling module (no IL2CPP / Unity access).
// Every function returns the vanilla value bit-for-bit when all multipliers
// are 1.0, so "settings back at default" == "vanilla asset values".

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace sr2::modules::spawn_scaling {

// Lower bound for the scaled node update interval (in-game hours). Only used
// when the vanilla value is above it, so it can never shorten a vanilla value.
inline constexpr float kMinIntervalHours = 0.1f;

// Scales a positive count. Rounds to nearest, never drops a positive count to
// 0, and `cap` only limits increases (the result is never pushed below the
// vanilla value by the cap). Non-positive vanilla values are kept as-is.
inline int32_t ScaleCount(int32_t vanilla, double factor, int32_t cap) {
    if (vanilla <= 0) return vanilla;
    double r = std::round(static_cast<double>(vanilla) * factor);
    if (r < 1.0) r = 1.0;
    const int32_t limit = std::max(cap, vanilla);
    if (r > static_cast<double>(limit)) return limit;
    return static_cast<int32_t>(r);
}

// ResourceNodeDirectorConfig._maxActiveNodes.
// hardCap <= 0 means "auto": 5 x vanilla for this zone's config.
inline int32_t ScaleMaxActiveNodes(int32_t vanilla, double maxNodesMult, int32_t hardCap) {
    const int32_t cap = hardCap > 0 ? hardCap : (vanilla > 0 ? vanilla * 5 : 0);
    return ScaleCount(vanilla, maxNodesMult, cap);
}

// ResourceNodeDirectorConfig._updateNodesIntervalHours.
//   interval = vanilla * respawnIntervalMult / spawnRateMult
// spawnRateMult > 1 -> the director refills more often (shorter interval);
// respawnIntervalMult < 1 -> shorter interval (faster respawn), > 1 longer.
inline float ScaleIntervalHours(float vanilla, double spawnRateMult, double respawnIntervalMult) {
    if (!(vanilla > 0.0f) || !(spawnRateMult > 0.0)) return vanilla;
    const double v = static_cast<double>(vanilla) * respawnIntervalMult / spawnRateMult;
    const float floorH = std::min(vanilla, kMinIntervalHours);
    const float out = static_cast<float>(v);
    return out < floorH ? floorH : out;
}

// Generic "time between spawns" scaling by a rate multiplier (rate 2 -> half
// the time). `floorV` only applies when the vanilla value is above it.
inline float ScaleDuration(float vanilla, double rate, float floorV) {
    if (!(vanilla > 0.0f) || !(rate > 0.0) || rate == 1.0) return vanilla;
    const float out = static_cast<float>(static_cast<double>(vanilla) / rate);
    const float f = std::min(vanilla, floorV);
    return out < f ? f : out;
}

// A (min, max) count pair scaled by `factor`, keeping max >= min. factor 1.0
// returns the vanilla pair untouched (even if vanilla had min > max).
inline void ScaleCountPair(int32_t vMin, int32_t vMax, double factor, int32_t cap, int32_t& outMin,
                           int32_t& outMax) {
    if (factor == 1.0) {
        outMin = vMin;
        outMax = vMax;
        return;
    }
    outMin = ScaleCount(vMin, factor, cap);
    outMax = std::max(outMin, ScaleCount(vMax, factor, cap));
}

// Rolled node lists (ResourceNodeSpawnerModel.resourcesToSpawn).
// Total length for a list whose per-type wanted counts sum to `want`: rounded,
// at least 1 for a non-empty list, and the roll cap only limits increases
// (never below the current length `baseN`). `hardMax` is a sanity bound.
inline int64_t ScaledListTotal(double want, size_t baseN, int32_t cap, size_t hardMax) {
    if (baseN == 0) return 0;
    int64_t total = static_cast<int64_t>(std::llround(want));
    if (total < 1) total = 1;
    const int64_t limit = std::max<int64_t>(cap, static_cast<int64_t>(baseN));
    if (total > limit) total = limit;
    if (total > static_cast<int64_t>(hardMax)) total = static_cast<int64_t>(hardMax);
    return total;
}

// Splits `total` over the entries of `want` proportionally (largest remainder;
// ties keep the original order). The result sums to `total` exactly.
inline std::vector<int32_t> SplitLargestRemainder(const std::vector<double>& want, int64_t total) {
    std::vector<int32_t> alloc(want.size(), 0);
    if (want.empty() || total <= 0) return alloc;
    double sum = 0;
    for (double w : want) sum += w > 0 ? w : 0.0;
    const double scale = sum > 0 ? static_cast<double>(total) / sum : 0.0;
    std::vector<std::pair<double, size_t>> frac;
    frac.reserve(want.size());
    int64_t assigned = 0;
    for (size_t i = 0; i < want.size(); ++i) {
        const double w = (want[i] > 0 ? want[i] : 0.0) * scale;
        alloc[i] = static_cast<int32_t>(std::floor(w));
        assigned += alloc[i];
        frac.emplace_back(w - static_cast<double>(alloc[i]), i);
    }
    std::stable_sort(frac.begin(), frac.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    for (size_t k = 0; assigned < total && k < frac.size(); ++k, ++assigned) ++alloc[frac[k].second];
    for (; assigned < total; ++assigned) ++alloc[0];  // only if every weight was 0
    return alloc;
}

}  // namespace sr2::modules::spawn_scaling
