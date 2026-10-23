#pragma once
// custom_upgrades: world objects of the coop upgrades (cloned from vanilla plot prefabs).
//
// Owned by the custom_upgrades module (module.cpp), which decides WHAT a coop should have
// (sidecar ownership); this class only builds, fits, runs and removes the clones. All calls on
// the Unity main thread, at most ~1 Hz (plus buy / sell / save / disable). Nothing is cached
// across ticks except plain values and a few session-wide assets (pinned): clones are found again
// every tick by name under the plot transform, the coop plots through the game's own registry
// (CoopRegion._allCoops). No GC handles on plots or clones, no FindObjectsOfTypeAll per tick.

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

struct Il2CppObject;

namespace sr2::il2cpp {
class Resolver;
}

namespace sr2::modules::custom_upgrades::world {

// Hen Collector keep-limit (adults left in the coop), per coop; set with the plot menu slider.
// 58 = Reproduce.MaxDensityInsideCoops, where breeding stalls anyway (re/custom-upgrades.md).
inline constexpr int kHenKeepDefault = 20;
inline constexpr int kHenKeepMin = 2;  // = the roosters that are never collected
inline constexpr int kHenKeepMax = 58;

// One storage slot of a Hen Collector (sidecar): IdentifiableType name ("" = empty) + count.
struct HenSlot {
    std::string type;
    int count = 0;
    bool operator==(const HenSlot&) const = default;
};

// What the framework wants on one coop plot.
struct CoopRequest {
    std::string id;               // LandPlotLocation id (timers per plot)
    bool airNet = false;          // "Coop Air Net": the corral's Air Net, fitted to the coop
    bool henCollector = false;    // "Hen Collector": the deluxe coop's elder collector, for adults
    // Hen Collector contents in the sidecar state: restored into a newly built collector, and
    // overwritten with the live contents whenever they differ (only while henCollector).
    std::vector<HenSlot>* hens = nullptr;
    int keepAdults = kHenKeepDefault;  // collect only the adults above this (clamped to the range above)
};

// query(plot, out) -> false for plots the framework does not know (no save game loaded,
// no LandPlotLocation id): they keep whatever they have until the next tick that knows them.
using QueryFn = std::function<bool(Il2CppObject* plot, CoopRequest& out)>;
// The LookupDirector (plot prefabs); only called when a clone has to be built.
using LookupFn = std::function<Il2CppObject*()>;

class CoopWorld {
public:
    // Worker thread: resolve classes / fields / methods by name. false = world objects off
    // (the world upgrades are then not offered; `why` is logged by the caller).
    bool Setup(sr2::il2cpp::Resolver& R, const char* module, std::string& why);
    bool Ready() const { return ready_; }

    // Main thread, ~1 Hz: create missing clones, refit them, run the Hen Collectors, copy their
    // contents into the requests, remove unwanted / stale clones.
    void Tick(const QueryFn& query, const LookupFn& lookup);
    // Main thread: copy the live Hen Collector contents into the requests (before a save).
    void Snapshot(const QueryFn& query);
    // Main thread: remove our clones from every coop (module disabled / faulted). With a query,
    // Hen Collector contents are copied into the requests first.
    void RemoveAll(const QueryFn* query);
    // Main thread: chickens stored in this plot's live Hen Collector; -1 = no live collector.
    int StoredHens(Il2CppObject* plot);
    // Main thread: remove the given clone(s) from one plot right away (sold).
    void Remove(Il2CppObject* plot, bool airNet, bool henCollector);
    // Save game loaded / new game: clones created before belong to the old game.
    void Reset();

private:
    struct Impl;
    Impl* impl_ = nullptr;
    bool ready_ = false;
    unsigned generation_ = 1;
};

}  // namespace sr2::modules::custom_upgrades::world
