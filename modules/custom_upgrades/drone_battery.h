#pragma once
// custom_upgrades: the effect of the Fabricator upgrade "Drone Battery Saver" I..IV (bought in
// vacpack_upgrades.cpp, global: every drone station). Level I..IV makes every station's drone use
// 1/1.25, 1/1.5, 1/1.75, 1/2 of the normal battery. The level is the vacpack upgrade's per-save value
// (vacpack::kDroneBatteryKey); the vanilla save never holds anything of ours.
//
// How the battery works (SR2 1.3.0, disassembly): a station's battery is ONE number,
// DroneStationGadgetModel._energyDepleteTime (ObservableValue<double>, world seconds, saved in the
// vanilla save). Energy = max(0, depleteTime - worldTime) / 3600 x DroneDirector.EnergyDepletedPerHour
// (GetCurrEnergy / GetEnergyAtTime / IsEnergyFull; max 1000). Fuel goes in through
// AddEnergy(timeDir, rate, value) = depleteTime -> now + min(energy + value, 1000) / rate hours. Every
// caller passes the global DroneDirector.EnergyDepletedPerHour; the drain is pure world time, the
// same while working or resting, loaded or not (the fast-forwarder only reads depleteTime).
// So the saver never touches the rate or the representation: every ~2 s it gives back what each
// station drained too much since its last look, through the game's own AddEnergy:
//   R0 = depleteTime - t0 (battery seconds left at the last look, as if depleteTime held since then),
//   R1 = max(0, R0 - dt / factor), refund = R1 - max(0, depleteTime - now) in energy units.
// A refund below 0.01 energy is not lost: t0 stays until the accumulated one is worth adding.
// Log: the first refund per station and level, then one summary line a minute (energy given back,
// each station's battery), so a test can see it work.
// The battery always stays a plain vanilla value: a save loads the same with or without the mod,
// turning custom upgrades off just stops the refunds. Known approximation: across a sleep the
// game's fast-forward runs the drones on the vanilla battery first and the refund comes after the
// jump (a drone that ran empty during the night stopped working early; its battery is right after).
//
// Also answers the Fabricator card's prerequisite (the Drone Station blueprint) and hands out the
// Drone Station gadget definition (the card's icon and pedia entry).
//
// Threading: Setup on the worker thread, everything else on the Unity main thread.

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>

struct Il2CppObject;
struct Il2CppClass;
struct MethodInfo;

namespace sr2::il2cpp {
class Resolver;
}

namespace sr2::modules::custom_upgrades::drones {

inline constexpr int kBatteryLevels = 4;
// Drain divisor per level (index 0 = not owned).
inline constexpr float kBatteryFactor[kBatteryLevels + 1] = {1.0f, 1.25f, 1.5f, 1.75f, 2.0f};

// "Drone Battery Saver II".
std::string BatteryLevelName(int level);

class BatterySaver {
public:
    // Worker thread. `level` (main thread): the owned level 0..kBatteryLevels (0 = none / no save).
    // false = not available (`why`); the rest of custom_upgrades works without it.
    bool Setup(sr2::il2cpp::Resolver& R, const char* module, std::function<int()> level, std::string& why);
    bool Ready() const { return ready_; }

    // Main thread, every frame while custom upgrades are on (works every ~2 s).
    void Tick(float unscaledDt);
    // Save loaded / module re-enabled: forget the last looks (world time jumps).
    void Reset();

    // Main thread. The Drone Station GadgetDefinition (DroneDirector._droneStationType), or nullptr.
    Il2CppObject* StationDefinition();
    // Main thread. 1 = the player owns the Drone Station blueprint (GadgetDirector.HasBlueprint),
    // 0 = not, -1 = unknown (no scene / lookup failed).
    int HasStationBlueprint();

private:
    struct Look {
        double time = 0.0;      // world time the next refund counts from (kept while a refund is too small)
        int level = 0;          // level logged as active
        double refunded = 0.0;  // energy given back since the last summary line
        bool first = true;      // the first refund at this level is logged
    };
    Il2CppObject* SceneContext();
    Il2CppObject* Call(const MethodInfo* m, void* obj, void** args, const char* what, bool* ok);

    const char* module_ = "custom_upgrades";
    bool ready_ = false;
    std::function<int()> level_;
    Il2CppClass* stationModelClass_ = nullptr;
    Il2CppClass* gadgetDefClass_ = nullptr;
    const MethodInfo* sceneCtxInst_ = nullptr;  // SRSingleton<SceneContext>.get_Instance
    const MethodInfo* worldTime_ = nullptr;     // TimeDirector.WorldTime()
    const MethodInfo* addEnergy_ = nullptr;     // DroneStationGadgetModel.AddEnergy(TimeDirector, float, float)
    const MethodInfo* hasBlueprint_ = nullptr;  // GadgetDirector.HasBlueprint(GadgetDefinition) (optional)
    size_t offScGame_ = 0, offScDrones_ = 0, offScTime_ = 0, offScGadgets_ = 0;  // SceneContext backing fields
    size_t offByIdent_ = 0;                                  // GameModel.identifiablesByIdent
    size_t offStationType_ = 0, offRate_ = 0;                // DroneDirector._droneStationType / EnergyDepletedPerHour
    size_t offDeplete_ = 0, offActorId_ = 0;                 // DroneStationGadgetModel._energyDepleteTime / actorId
    size_t offObsValue_ = 0;                                 // ObservableValue<double>._value (found at runtime)
    const MethodInfo* tryGetValue_ = nullptr;                // Dictionary<IdentifiableType, List<..>>.TryGetValue
    const void* tryGetValueClass_ = nullptr;

    float timer_ = 0.0f;
    float summaryTimer_ = 0.0f;  // real seconds since the last summary line
    const void* gameModel_ = nullptr;  // identity: a new GameModel = a loaded save
    std::unordered_map<int64_t, Look> looks_;
    bool loggedFail_ = false;
};

}  // namespace sr2::modules::custom_upgrades::drones
