#pragma once
// drone_price_limit: "Drone Battery Saver" tiers, bought per drone station in the station menu
// (station_dialog.cpp). Tier I..IV make the station's drone use 1/1.25, 1/1.5, 1/1.75, 1/2 of the
// normal battery. Ownership: custom_upgrades per-save value "drone_battery.station.<actor id>" = tier
// (only while custom_upgrades is on; the vanilla save never holds anything of ours).
//
// How the battery works (SR2 1.3.0, disassembly): a station's battery is ONE number,
// DroneStationGadgetModel._energyDepleteTime (ObservableValue<double>, world seconds, saved in the
// vanilla save). Energy = max(0, depleteTime - worldTime) / 3600 x DroneDirector.EnergyDepletedPerHour
// (GetCurrEnergy / GetEnergyAtTime / IsEnergyFull; max 1000). Fuel goes in through
// AddEnergy(timeDir, rate, value) = depleteTime -> now + min(energy + value, 1000) / rate hours. Every
// caller passes the global DroneDirector.EnergyDepletedPerHour; the drain is pure world time, the
// same while working or resting, loaded or not (the fast-forwarder only reads depleteTime).
// So the saver never touches the rate or the representation: every ~2 s it gives back what the
// station drained too much since its last look, through the game's own AddEnergy:
//   R0 = depleteTime - t0 (battery seconds left at the last look, as if depleteTime held since then),
//   R1 = max(0, R0 - dt / factor), refund = R1 - max(0, depleteTime - now) in energy units.
// The battery always stays a plain vanilla value: a save loads the same with or without the mod,
// turning the tier / module off just stops the refunds. Known approximation: across a sleep the
// game's fast-forward runs the drones on the vanilla battery first and the refund comes after the
// jump (a drone that ran empty during the night stopped working early; its battery is right after).
//
// Threading: Setup on the worker thread, everything else on the Unity main thread.

#include <cstdint>
#include <string>
#include <unordered_map>

struct Il2CppObject;
struct Il2CppClass;
struct MethodInfo;

namespace sr2::il2cpp {
class Resolver;
}

namespace sr2::modules::drone_price_limit {

inline constexpr int kBatteryTiers = 4;
// Drain divisor per tier (index 0 = no tier).
inline constexpr float kBatteryFactor[kBatteryTiers + 1] = {1.0f, 1.25f, 1.5f, 1.75f, 2.0f};
// Newbucks per tier; selling refunds half (rounded down), like sell_upgrades.
inline constexpr int kBatteryPrice[kBatteryTiers + 1] = {0, 1500, 3000, 5000, 7500};

// "Battery Saver II".
std::string BatteryTierName(int tier);

class BatterySaver {
public:
    // Worker thread. false = not available (`why`); the module works without it.
    bool Setup(sr2::il2cpp::Resolver& R, const char* module, std::string& why);
    bool Ready() const { return ready_; }

    // Main thread. The station's owned tier (0 = none; always 0 while custom_upgrades is off).
    int Tier(int64_t station);
    // Main thread: custom_upgrades on with a save loaded (tiers can be bought and are applied).
    bool Available() const;
    // Main thread. Player's Newbucks (-1 = unknown).
    int Newbucks();
    // Main thread. Buy the next tier / sell the owned one. Returns "" on success, else a short reason
    // for the player ("Not enough Newbucks").
    std::string BuyNext(int64_t station);
    std::string SellTop(int64_t station);
    // Main thread, every frame (works every ~2 s).
    void Tick(float unscaledDt);
    // Save loaded / module re-enabled: forget the last looks (world time jumps).
    void Reset();

private:
    struct Look {
        double time = 0.0;  // world time of the last look
        int tier = 0;       // tier logged as active
    };
    struct CachedTier {
        int tier = 0;
        unsigned long long tick = 0;
    };
    Il2CppObject* SceneContext();
    Il2CppObject* Currency();  // Newbucks (CurrencyDefinition)
    Il2CppObject* Player();    // PlayerState
    bool SetTier(int64_t station, int tier);
    Il2CppObject* Call(const MethodInfo* m, void* obj, void** args, const char* what, bool* ok);

    const char* module_ = "drone_price_limit";
    bool ready_ = false;
    Il2CppClass* stationModelClass_ = nullptr;
    const MethodInfo* sceneCtxInst_ = nullptr;  // SRSingleton<SceneContext>.get_Instance
    const MethodInfo* gcInst_ = nullptr;        // SRSingleton<GameContext>.get_Instance
    const MethodInfo* worldTime_ = nullptr;     // TimeDirector.WorldTime()
    const MethodInfo* addEnergy_ = nullptr;     // DroneStationGadgetModel.AddEnergy(TimeDirector, float, float)
    const MethodInfo* tryGetPlayer_ = nullptr;  // SceneContextUtility.TryGetPlayerState(out PlayerState)
    const MethodInfo* getCurrency_ = nullptr;   // PlayerState.GetCurrency(ICurrency)
    const MethodInfo* spendCurrency_ = nullptr; // PlayerState.SpendCurrency(ICurrency, int, IUIDisplayData)
    const MethodInfo* addCurrency_ = nullptr;   // PlayerState.AddCurrency(ICurrency, int, bool, IUIDisplayData)
    const MethodInfo* defaultCurrency_ = nullptr;  // CurrencyUtility.get_DefaultCurrency (optional)
    size_t offScGame_ = 0, offScDrones_ = 0, offScTime_ = 0;  // SceneContext backing fields
    size_t offByIdent_ = 0;                                  // GameModel.identifiablesByIdent
    size_t offStationType_ = 0, offRate_ = 0;                // DroneDirector._droneStationType / EnergyDepletedPerHour
    size_t offDeplete_ = 0, offActorId_ = 0;                 // DroneStationGadgetModel._energyDepleteTime / actorId
    size_t offObsValue_ = 0;                                 // ObservableValue<double>._value (found at runtime)
    size_t offGcLookup_ = 0, offLookupCurrencies_ = 0, offCurrencyArr_ = 0;
    const MethodInfo* tryGetValue_ = nullptr;                // Dictionary<IdentifiableType, List<..>>.TryGetValue
    const void* tryGetValueClass_ = nullptr;

    float timer_ = 0.0f;
    const void* gameModel_ = nullptr;  // identity: a new GameModel = a loaded save
    std::unordered_map<int64_t, Look> looks_;
    std::unordered_map<int64_t, CachedTier> tiers_;
    bool loggedFail_ = false;
};

}  // namespace sr2::modules::drone_price_limit
