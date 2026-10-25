#pragma once
// custom_upgrades: Auto-Feeder "Leave last food item in" (round 16; round 15 was a food picker, user on the
// round 15 test: "No no no no no, it should be just a toggle, 'Leave last food item in'"). Design:
// modules/custom_upgrades/NOTES.md "Auto-Feeder: Leave last food item in"; RE: re/plots.md "SlimeFeeder" and
// re/drones.md "Feeder sink".
//
// A corral's feeder (SlimeFeeder, the FEEDER upgrade) stores its food in a SiloStorage on a parent object
// (SlimeFeeder._storage, set in Awake by GetComponentInParent); one slot, one food type at a time.
//
// Feeding out: every feed operation is SlimeFeeder.ProcessFeedOperation(ejectFood) - FixedUpdate (true:
// spawns the item) and ProcessFeedOperationFastForward (false: RanchCellFastForwarder's AutoFeeder source,
// whose budget is RemainingFeedOperationsFastForward = min(pending operations, food count)). While the
// toggle is on and the feeder holds at most one item, the ProcessFeedOperation detour does what vanilla
// does for an empty feeder (pending operations - 1, nothing fed) and the fast-forward budget is
// min(original, count - 1). So the last item always stays in.
//
// Putting food in: the feeder keeps that food type (one slot), and while kept, five SiloStorage methods
// (CanSpecificSlotAccept, CanAnySlotAccept, MaybeAddToSpecificSlot, MaybeAddToAnySlot, MaybeAddAsResource)
// answer false for any other food on that storage (round 15's hooks; the locked food is now the feeder's
// current food, read with the ~1 Hz tick). So the player's shots and drones (RanchDroneSinkProgramFeeder.
// GetSinks -> CanSpecificSlotAccept) can only add more of the same food. An empty feeder (toggle switched
// on before the first food) takes any food; the first one becomes the kept food.
//
// Toggle off: everything goes straight to the originals (one atomic load while no feeder keeps one).
//
// The toggle is a per-plot value in the sidecar (api.h PlotValue, key kKeepKey; non-empty = on, "" = off /
// vanilla; round 15 stored a food's asset name under the same key, which reads as on). module.cpp hands the
// corrals with the toggle on to Tick() with the ~1 Hz world tick; SetNow() applies a change at once.
//
// Threading: Setup on the worker thread; Tick / SetNow / Cleanup and the SlimeFeeder detours on the Unity
// main thread. The table is read by the SiloStorage detours under a shared lock (any thread).

#include <string>
#include <vector>

struct Il2CppObject;

namespace sr2::core {
class ModuleContext;
}

namespace sr2::modules::custom_upgrades::feeder {

inline constexpr const char* kKeepKey = "custom_upgrades.feeder_lock";

struct KeepRequest {
    Il2CppObject* plot = nullptr;  // live LandPlot (corral)
    std::string id;                // plot id (log)
};

class FeederLock {
public:
    // Worker thread: resolve by name and create the seven hooks (all or nothing). false = no toggle
    // (logged by the caller); vanilla feeders.
    bool Setup(sr2::core::ModuleContext& ctx, std::string& why, std::string& report);
    bool Ready() const { return ready_; }

    // Main thread. true = the corral has an active feeder (bought; the toggle is offered).
    bool HasFeeder(Il2CppObject* plot);
    // Main thread, ~1 Hz: the corrals with the toggle on (the others are off). Rebuilds the table.
    void Tick(const std::vector<KeepRequest>& keep);
    // Main thread: switch `plot`'s feeder now, until the next Tick confirms it.
    void SetNow(Il2CppObject* plot, bool on, const std::string& id);
    // Main thread or any: forget every feeder (module off, load, faulted). Feeders behave as vanilla.
    void Cleanup(const char* why);

private:
    bool ready_ = false;
};

}  // namespace sr2::modules::custom_upgrades::feeder
