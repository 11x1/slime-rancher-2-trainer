#pragma once
// custom_upgrades: Auto-Feeder food lock (round 15, user: "Give the autofeeder an option to lock 1 item in
// it as always in the buy menu so drones can be filtered to put specific food items in them not just
// everything"). Design: modules/custom_upgrades/NOTES.md "Auto-Feeder food lock"; RE: re/plots.md
// "SlimeFeeder" and re/drones.md "Feeder sink".
//
// A corral's feeder (SlimeFeeder, the FEEDER upgrade) stores its food in a SiloStorage on a parent object
// (SlimeFeeder._storage, set in Awake by GetComponentInParent). Everything that puts food in goes through
// that storage:
//   * the player shooting food into the feeder: SiloCatcher.OnTriggerEnter -> Insert ->
//     storage.MaybeAddToSpecificSlot (virtual) - false = nothing happens, the item stays outside (vanilla's
//     answer for an item the feeder doesn't take);
//   * ranch drones with a Feeder destination: RanchDroneSinkProgramFeeder.GetSinks(id, overflow) lists the
//     feeders whose storage CanSpecificSlotAccept(id) (lambda <GetSinks>b__2, folded with the silo sink's
//     b__1). HasAvailableSpace / GetAvailableSpace / Relevancy / the per-tick re-check / FastForward and the
//     source programs' "any sink has room" test (IsSourceValid) all go through GetSinks, so a feeder that
//     refuses an item is simply no destination for it: the drone fetches only the locked food for it.
//     The deposit itself is StorageMetadata.Increment (only after GetSinks picked the feeder).
// While a feeder is locked, five SiloStorage methods (CanSpecificSlotAccept, CanAnySlotAccept,
// MaybeAddToSpecificSlot, MaybeAddToAnySlot, MaybeAddAsResource) answer false for any other item on that
// storage only; every other storage and every unlocked feeder goes straight to the original (one atomic
// load while nothing is locked). Food already inside stays and is fed out as usual (the slot holds one type
// at a time, so the locked food can come in once it is empty).
//
// The lock is a per-plot value in the sidecar (api.h PlotValue, key kLockKey = the IdentifiableType's asset
// name; "" = unlocked, vanilla). module.cpp hands the locked plots to Tick() with the ~1 Hz world tick,
// which maps their live feeder storages; SetNow() applies a change from the plot menu at once.
//
// Threading: Setup on the worker thread; Foods / Tick / SetNow / Cleanup on the Unity main thread. The
// storage -> food table is read by the detours under a shared lock (any thread).

#include <string>
#include <vector>

struct Il2CppObject;

namespace sr2::core {
class ModuleContext;
}

namespace sr2::modules::custom_upgrades::feeder {

inline constexpr const char* kLockKey = "custom_upgrades.feeder_lock";

struct Food {
    Il2CppObject* type = nullptr;  // IdentifiableType (persistent asset)
    std::string id;                // asset name (the sidecar value)
    std::string name;              // localized display name
};

struct LockRequest {
    Il2CppObject* plot = nullptr;  // live LandPlot (corral)
    std::string id;                // plot id (log)
    std::string food;              // locked food's asset name
};

class FeederLock {
public:
    // Worker thread: resolve by name and create the five SiloStorage hooks (all or nothing). false = no
    // lock (logged by the caller); vanilla feeders.
    bool Setup(sr2::core::ModuleContext& ctx, std::string& why, std::string& report);
    bool Ready() const { return ready_; }

    // Main thread. The foods the corral's feeder takes (its slot definition's group, sub-groups included,
    // minus its block list; group order), cached per group for the session. false = no active feeder.
    bool Foods(Il2CppObject* plot, std::vector<Food>& out);
    // Main thread, ~1 Hz: the locked corrals (others are unlocked). Rebuilds the storage table.
    void Tick(const std::vector<LockRequest>& locks);
    // Main thread: lock `plot`'s feeder to `type` now (nullptr = unlock), until the next Tick confirms it.
    void SetNow(Il2CppObject* plot, Il2CppObject* type, const std::string& id);
    // Main thread or any: forget every lock (module off, load, faulted). Feeders behave as vanilla.
    void Cleanup(const char* why);

private:
    bool ready_ = false;
};

}  // namespace sr2::modules::custom_upgrades::feeder
