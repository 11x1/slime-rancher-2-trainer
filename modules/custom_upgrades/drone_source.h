#pragma once
// custom_upgrades: the drone program source "Hen Collector" (next to the vanilla "Coop").
//
// In memory a program that uses it carries our own DroneTaskSourceType value kHenSourceType. The
// vanilla save never does: DroneStationGadgetModel.Pull hands the save a copy that says COOP, and the
// per-save sidecar keeps the list of stations whose program really is "Hen Collector"
// ("drone_hen_source.stations"). After a load the ~1 Hz tick switches those programs back. Removing the
// mod therefore leaves plain Coop programs. Details and findings: drone_source.cpp (header) and
// modules/custom_upgrades/NOTES.md ("Drone source Hen Collector").
//
// Threading: Setup on the worker thread; Tick / Revert(gameCalls) on the Unity main thread;
// RevertMemory from any thread (no game calls).

#include <cstdint>
#include <string>
#include <vector>

struct Il2CppObject;

namespace sr2::core {
class ModuleContext;
}

namespace sr2::modules::custom_upgrades::drones {

// Our DroneTaskSourceType value (never written to the vanilla save). "SR" << 16 like the custom
// upgrade ids, so it can't collide with a future vanilla source type.
inline constexpr int32_t kHenSourceType = 0x53520101;

class HenSource {
public:
    // Worker thread. false = not available (`why`); the Coop linking of the Hen Collector still works.
    bool Setup(sr2::core::ModuleContext& ctx, const char* module, std::string& why, std::string& report);
    bool Ready() const { return ready_; }

    // Main thread, ~1 Hz while custom upgrades are on and a save is loaded. `henStorages` = the live
    // Hen Collector SiloStorages (identities, this tick only).
    void Tick(const std::vector<Il2CppObject*>& henStorages);
    // Custom upgrades going off / faulted: every program with our source becomes Coop again. With
    // gameCalls (main thread, module not faulted) the running drones get the Coop program too;
    // without, only the program data (plain field writes, any thread).
    void Revert(bool gameCalls, const char* why);
    // A save was loaded / the module enabled again: map the sidecar's stations again.
    void ResetSeen();

private:
    bool ready_ = false;
};

}  // namespace sr2::modules::custom_upgrades::drones
