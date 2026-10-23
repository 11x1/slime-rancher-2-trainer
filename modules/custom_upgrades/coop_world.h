#pragma once
// custom_upgrades: world objects of the coop upgrades (cloned from vanilla plot prefabs).
//
// Owned by the custom_upgrades module (module.cpp), which decides WHAT a coop should have
// (sidecar ownership); this class only builds, fits and removes the clones. All calls on the
// Unity main thread, at most ~1 Hz (plus buy / sell / disable). Nothing is cached across ticks
// except plain values: clones are found again every tick by name under the plot transform, the
// coop plots through the game's own registry (CoopRegion._allCoops). No GC handles on plots or
// clones, no FindObjectsOfTypeAll per tick.

#include <cstdint>
#include <functional>
#include <string>

struct Il2CppObject;

namespace sr2::il2cpp {
class Resolver;
}

namespace sr2::modules::custom_upgrades::world {

// What the framework wants on one coop plot.
struct CoopRequest {
    bool airNet = false;  // "Coop Air Net": the corral's Air Net, fitted to the coop
};

// query(plot, out) -> false for plots the framework does not know (no save game loaded,
// no LandPlotLocation id): they keep whatever they have until the next tick that knows them.
using QueryFn = std::function<bool(Il2CppObject* plot, CoopRequest& out)>;
// The LookupDirector (plot prefabs); only called when a clone has to be built.
using LookupFn = std::function<Il2CppObject*()>;

class CoopWorld {
public:
    // Worker thread: resolve classes / fields / methods by name. false = world objects off
    // (the upgrades stay purchasable; `why` is logged by the caller).
    bool Setup(sr2::il2cpp::Resolver& R, const char* module, std::string& why);
    bool Ready() const { return ready_; }

    // Main thread, ~1 Hz: create missing clones, refit them, remove unwanted / stale ones.
    void Tick(const QueryFn& query, const LookupFn& lookup);
    // Main thread: remove our clones from every coop (module disabled / faulted).
    void RemoveAll();
    // Main thread: remove the given clone(s) from one plot right away (sold).
    void Remove(Il2CppObject* plot, bool airNet);
    // Save game loaded / new game: clones created before belong to the old game.
    void Reset() { ++generation_; }

private:
    struct Impl;
    Impl* impl_ = nullptr;
    bool ready_ = false;
    unsigned generation_ = 1;
};

}  // namespace sr2::modules::custom_upgrades::world
