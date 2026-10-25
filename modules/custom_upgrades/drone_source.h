#pragma once
// custom_upgrades: the drone program sources "Hen Collector" and "Elder Collector" (next to the
// vanilla "Coop"), and "Quantum Storage Link" (next to the vanilla "Silo"). Hen Collector takes only from
// our Hen Collector's storage, Elder Collector only from the deluxe coop's vanilla elder collector; Coop
// keeps taking from everything. Quantum Storage Link takes items out of the Quantum Cloud at a Quantum
// Storage Link (quantum_link.h).
//
// In memory a program that uses one carries our own DroneTaskSourceType value (kHenSourceType /
// kElderSourceType / kQuantumSourceType). The vanilla save never does: DroneStationGadgetModel.Pull
// hands the save a copy that says COOP (SILO for Quantum Storage Link), and the per-save sidecar keeps
// the stations whose program really is ours ("drone_hen_source.stations" / "drone_elder_source.stations"
// / "drone_quantum_source.stations"). After a load the ~1 Hz tick switches those programs back.
// Removing the mod therefore leaves plain Coop / Silo programs. Details and findings: drone_source.cpp
// (header) and modules/custom_upgrades/NOTES.md ("Drone sources", "Quantum Storage Link").
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

// Our DroneTaskSourceType values (never written to the vanilla save). "SR" << 16 like the custom
// upgrade ids, so they can't collide with a future vanilla source type.
inline constexpr int32_t kHenSourceType = 0x53520101;
inline constexpr int32_t kElderSourceType = 0x53520102;
inline constexpr int32_t kQuantumSourceType = 0x53520110;

// A station program as plain data (DroneTaskData: TargetType, TargetIdentType, SourceType, SinkType).
struct ProgramData {
    int32_t target = 0;            // DroneTaskTargetType (0 = NONE)
    Il2CppObject* ident = nullptr; // IdentifiableType for SINGLE, else null (an asset: lives all session)
    int32_t source = 0;            // DroneTaskSourceType, may be one of ours
    int32_t sink = 0;              // DroneTaskSinkType
    bool operator==(const ProgramData&) const = default;
    bool Valid() const { return target != 0 && source != 0 && sink != 0; }
    // An Explorer mode station's program has a target only (what to gather; source / sink NONE).
    bool ValidFor(bool explorer) const { return explorer ? target != 0 : Valid(); }
};

// Helpers for the several-programs rotation (drone_programs.cpp). Main thread unless noted; all of them
// return false / null / 0 while the drone sources are not set up.
int64_t StationIdOf(Il2CppObject* stationModel);                 // any thread (plain field read)
bool ReadProgram(Il2CppObject* stationModel, ProgramData& out);  // any thread (plain field reads)
// The station is in Explorer mode (DroneStationGadgetModel._type == DroneType.EXPLORER_DRONE). Any thread.
bool IsExplorer(Il2CppObject* stationModel);
bool StationModels(std::vector<Il2CppObject*>& out);             // the save's DroneStationGadgetModels
Il2CppObject* DroneOf(Il2CppObject* stationModel);               // its live RanchDrone (verified) or null
// The station's program -> `p`: with its drone the game's own DroneStation.AssignRanchTask (our source
// types through the GatherSourceTask detour), else the program data only. Logs one line when `why`.
bool AssignProgram(Il2CppObject* stationModel, const ProgramData& p, const char* why);
// The program data only (plain field writes, any thread; the drone keeps what it runs until it is
// programmed again). For turning the rotation off without game calls.
bool SetProgramData(Il2CppObject* stationModel, const ProgramData& p);
// Our source type -> its vanilla save type (Coop / Silo); any other type unchanged. Any thread.
int32_t VanillaSource(int32_t source);
// Any thread (the Pull detour): `id`'s program for the save instead of the live one (true = use `out`;
// its source may be ours, it is mapped to the vanilla type like any other).
using SaveFilter = bool (*)(int64_t id, ProgramData& out);
// Main thread (the drone source tick): `id` is managed by the several-programs rotation; its sidecar
// keeps the true sources, so the drone source lists leave it alone.
using ManagedFilter = bool (*)(int64_t id);
void SetProgramHooks(SaveFilter save, ManagedFilter managed);

class HenSource {
public:
    // Worker thread. false = not available (`why`); the Coop linking of the Hen Collector still works.
    bool Setup(sr2::core::ModuleContext& ctx, const char* module, std::string& why, std::string& report);
    bool Ready() const { return ready_; }

    // Main thread, ~1 Hz while custom upgrades are on and a save is loaded. `henStorages` = the live
    // Hen Collector SiloStorages (identities, this tick only); `quantumReady` = a Quantum Storage Link is
    // built with a drone port (the source is offered); `quantumPlaced` = the save has one (loaded or not:
    // saved programs are restored, not dropped, while the ranch is not loaded).
    void Tick(const std::vector<Il2CppObject*>& henStorages, bool quantumReady, bool quantumPlaced);
    // Custom upgrades going off / faulted: every program with one of our sources gets its vanilla
    // source again (Coop / Silo). With gameCalls (main thread, module not faulted) the running drones get
    // the vanilla program too; without, only the program data (plain field writes, any thread).
    void Revert(bool gameCalls, const char* why);
    // A save was loaded / the module enabled again: map the sidecar's stations again.
    void ResetSeen();

private:
    bool ready_ = false;
};

}  // namespace sr2::modules::custom_upgrades::drones
