#pragma once
// custom_upgrades: public surface for other modules (sell_upgrades, plot_upgrades).
//
// Custom land-plot upgrade ids live in their own range (0x5352xxxx, "SR") of
// the LandPlot.Upgrade int enum. They are NEVER written into
// LandPlotModel.upgrades (the vanilla save): ownership is kept in a sidecar
// file per .sav file (see module.cpp). LandPlot.HasUpgrade answers for them
// through a detour, so the plot purchase menu greys owned custom upgrades out
// like vanilla ones.
//
// All functions are safe to call when the module is disabled / faulted /
// never set up: they then report "not active" / "can't sell" / multiplier 1.

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

// Any thread, also while the module is off: `def` is one of our runtime GadgetDefinition clones (Quantum
// Storage Link, Slime DJ buildables). The save stores them only as their vanilla gadget, through our
// detours, so nothing else may add them to the inventory or the blueprints (the Sandbox crafter skips them).
bool IsModGadget(Il2CppObject* def);

// Display name ("Sell-All Button"); "custom upgrade" if unknown.
std::string Name(int32_t upgrade);

// Main thread. "" = the owned custom upgrade can be sold back; else the reason.
// `brief` (optional) gets a few-word form of the reason for a button label
// ("holds chickens", "sell Faster Hens II first"); cleared when sellable.
std::string CanSell(Il2CppObject* landPlot, int32_t upgrade, std::string* brief = nullptr);

// Main thread. Removes the custom upgrade from the plot (sidecar state,
// world objects). Returns false if nothing was removed. `notes` gets details.
bool Remove(Il2CppObject* landPlot, int32_t upgrade, std::string& notes);

// Main thread. For a VANILLA upgrade: the name of an owned custom upgrade on
// this plot that needs it (it must be sold first), or "" if none.
std::string VanillaDependent(Il2CppObject* landPlot, int32_t vanillaUpgrade);

// Main thread. A tier chain has one menu card (Faster Hens I -> II, Music Box -> Music Box II):
// once a tier is owned the card shows the next one. For such a card `item` (a plot menu item)
// that shows a tier NOT owned yet: the owned tier below it, which the card's Sell button sells
// (a custom id, or the vanilla upgrade the chain starts on), and the menu item whose price the
// refund is based on. False for every other item (an owned item sells itself).
bool ChainSellTarget(Il2CppObject* landPlot, Il2CppObject* item, int32_t& upgrade, Il2CppObject*& priceItem);

// Purchasable per-plot tiers (plot_upgrades applies the effects).
enum class Family : int {
    kMusicBox = 0,   // corral Music Box II: calming bonus x multiplier
    kCoop,           // coop Faster Hens I / II: laying period divided by the multiplier
    kGarden,         // garden Sprinkler II / III (ids garden_growth_1 / 2): crop growth rate x multiplier
    kAsh,            // incinerator Ash Booster: ash per incinerated food x multiplier
    kCount
};

// Main thread, hot path. `plotOrComponent` is a LandPlot or any component
// below one (collector, feeder, Incinerate, CoopRegion, MusicBoxRegion...).
// Returns the multiplier of the highest tier of `family` the plot owns, 1.0
// if none (or wrong plot type / module off). Cached per object (O(1) map read;
// a miss or an expired entry takes the slow path once).
float Multiplier(Il2CppObject* plotOrComponent, Family family);

// ---- per-save values (stored in the same sidecar as the custom upgrades) ------------------
// Main thread. Small string values that follow the exact .sav file like the upgrades do: kept
// in memory, written with the next game save into that save's sidecar, dropped on load / new
// game and read back from the loaded save's sidecar. Keys: "<module>.<name>". "" = not set;
// setting "" erases. Per-plot values are keyed by the plot's LandPlotLocation id + plot type
// (a plot rebuilt as another type loses them). They never touch the vanilla save. Return
// "" / false when the module is off, faulted or no save game is loaded.
std::string PlotValue(Il2CppObject* landPlot, const std::string& key);
bool SetPlotValue(Il2CppObject* landPlot, const std::string& key, const std::string& value);
std::string SaveValue(const std::string& key);
bool SetSaveValue(const std::string& key, const std::string& value);

}  // namespace sr2::modules::custom_upgrades
