#pragma once
// custom_upgrades: several programs per ranch drone station (up to kMaxPrograms). A station runs one
// program at a time (DroneStationGadgetModel._taskData); the others wait in the mod and take turns:
//   * "When idle" (default): the next program runs when the drone has nothing to do with the current
//     one (it rests at its station with empty hands for ~2 s).
//   * "After each delivery": also after every load the drone brought somewhere (hands empty again).
// The switch is the game's own DroneStation.AssignRanchTask and happens only while the drone holds
// nothing (a drone with cargo and a program whose destination can't take it would dump the cargo).
// While every program is idle the drone tries the next one only every 20 s.
//
// Save (vanilla-safe, like the Hen / Elder Collector sources): the vanilla save always holds program 1
// (DroneStationGadgetModel.Pull hands the save a copy of it, its source mapped to Coop / Silo when it
// is one of ours), whichever program runs at that moment. The per-save sidecar keeps every program
// with its true source: "drone_programs.station.<actor id>" = "v1;<mode>;<program>|<program>|...",
// program = "<target>:<ident asset name or ->:<source>:<sink>". Without the mod the station simply
// has program 1. After a load the first ~1 Hz tick checks that the save's program is the sidecar's
// program 1 (else the extra programs are dropped, logged) and runs program 1 with its true source.
//
// The UI (the "Programs" action and dialog in the drone station menu) lives in drone_price_limit's
// station_dialog.cpp, which owns the menu hooks; it calls the functions below. Details and findings:
// drone_programs.cpp and modules/custom_upgrades/NOTES.md ("Several programs per drone").
//
// Threading: Setup on the worker thread; everything else on the Unity main thread unless noted.

#include <cstdint>
#include <string>
#include <vector>

#include "modules/custom_upgrades/drone_source.h"

struct Il2CppObject;

namespace sr2::il2cpp {
class Resolver;
}

namespace sr2::modules::custom_upgrades::drone_programs {

inline constexpr int kMaxPrograms = 3;

enum class Mode : int { kWhenIdle = 0, kAfterDelivery = 1 };

struct View {
    std::vector<drones::ProgramData> programs;  // at least one; [0] = the program the vanilla save keeps
    int running = 0;                            // index of the program the drone runs now
    Mode mode = Mode::kWhenIdle;
};

// custom_upgrades on and the rotation set up (the UI shows its action only then).
bool Available();
// The station's programs (a station without extra programs: its one live program).
bool Get(Il2CppObject* stationModel, View& out);
// A copy of the running program is appended and becomes the running one (the page already shows it,
// so the player edits it there and presses Start Program). False: full, no valid program, not available.
bool Add(Il2CppObject* stationModel);
// Removes program `index` (not the last one). The running one removed: the next one runs.
bool Remove(Il2CppObject* stationModel, int index);
// Runs program `index` now (the drone station menu then shows it for editing).
bool Run(Il2CppObject* stationModel, int index);
bool SetMode(Il2CppObject* stationModel, Mode m);
// The station's menu is open: its rotation waits (call every frame or so while open).
void NoteMenuOpen(int64_t station);

const char* ModeName(Mode m);

class Rotation {
public:
    // Worker thread. false = unavailable (`why`); stations keep one program.
    bool Setup(sr2::il2cpp::Resolver& R, const char* module, std::string& why);
    bool Ready() const { return ready_; }
    // Main thread, ~1 Hz while custom upgrades are on and a save is loaded (after the drone sources).
    void Tick();
    // A save was loaded / the module enabled again: forget the stations (the sidecar is read again).
    // Any thread.
    void ResetSeen();
    // Custom upgrades going off / faulted: every station runs program 1 again (source mapped to the
    // vanilla type). With gameCalls (main thread, module not faulted) the drones are re-programmed,
    // without only the program data (plain field writes, any thread).
    void Revert(bool gameCalls, const char* why);

private:
    bool ready_ = false;
};

}  // namespace sr2::modules::custom_upgrades::drone_programs
