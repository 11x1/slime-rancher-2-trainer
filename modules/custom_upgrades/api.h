#pragma once
// custom_upgrades: public surface for other modules (sell_upgrades).
//
// Custom land-plot upgrade ids live in their own range (0x5352xxxx, "SR") of
// the LandPlot.Upgrade int enum. They are NEVER written into
// LandPlotModel.upgrades (the vanilla save): ownership is kept in a sidecar
// file per save game (see module.cpp). LandPlot.HasUpgrade answers for them
// through a detour, so the plot purchase menu greys owned custom upgrades out
// like vanilla ones.
//
// All functions are safe to call when the module is disabled / faulted /
// never set up: they then report "not active" / "can't sell".

#include <cstdint>
#include <string>

struct Il2CppObject;

namespace sr2::modules::custom_upgrades {

inline constexpr int32_t kIdBase = 0x53520000;  // "SR" << 16
inline constexpr int32_t kIdMask = static_cast<int32_t>(0xFFFF0000u);

// Pure range check (any thread).
inline bool IsCustomUpgrade(int32_t upgrade) { return (upgrade & kIdMask) == kIdBase; }

// Module enabled, set up and not faulted.
bool Active();

// Display name ("Sell-All Button"); "custom upgrade" if unknown.
std::string Name(int32_t upgrade);

// Main thread. "" = the owned custom upgrade can be sold back; else the reason.
std::string CanSell(Il2CppObject* landPlot, int32_t upgrade);

// Main thread. Removes the custom upgrade from the plot (sidecar state,
// world objects). Returns false if nothing was removed. `notes` gets details.
bool Remove(Il2CppObject* landPlot, int32_t upgrade, std::string& notes);

}  // namespace sr2::modules::custom_upgrades
