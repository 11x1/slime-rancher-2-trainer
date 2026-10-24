#pragma once
// custom_upgrades: vacpack upgrades sold in the Fabricator's Upgrades category, in the game's own
// style (SR2 1.3.0). Design + findings: modules/custom_upgrades/NOTES.md "Vacpack upgrades".
//
//   upgrade            levels  effect (stacks on the maxed vanilla upgrade)        shown once
//   Tank Booster IX-XII  4     +25 / +50 / +75 / +100 items per slot (125..200)    Tank Booster VIII owned
//   Extra Tank III-IV    2     a 7th / 8th vacpack slot                            Extra Tank II owned
//   Drone Battery Saver  4     every drone station's drone uses 1/1.25 .. 1/2 of   Drone Station blueprint
//     I-IV                     the battery (effect: drone_battery.cpp)             owned
//
// Each is a clone of the vanilla "AmmoCapacity" UpgradeDefinition (Object.Instantiate: its levels,
// effects, costs and texts are deep copies) with its own _uniqueId in the custom range, appended to
// the player's UpgradeDefinitionList (+ the Fabricator category's list if that is another asset).
// Buying it is the vanilla flow (PlayerUpgradeFabricatableItem: spend, UpgradeModel.
// IncrementUpgradeLevel -> ActorUpgradeHandler adds the level's FloatModifier). Levels never reach
// the vanilla save: UpgradeModel.Pull is detoured to drop our ids; the level lives in the
// custom_upgrades sidecar (per-save value) and the ~1 Hz tick sets it on the model after a load.
// The extra slots are two more AmmoSlotDefinitions (clones of "ExpansionAmmoSlot 2", thresholds 7
// and 8 on AmmoSlotsAvailable) appended to PlayerState._ammoSlotDefinitions before the game builds
// the player's AmmoSlotManager (PlayerState.InitModel / Reset). Their contents are kept out of the
// vanilla save too (AmmoModel.Pull detour: the player's slots past the vanilla count go to the
// sidecar and are put back after the next load).
//
// Main thread only. All game objects come from the live player (SceneContextUtility.
// TryGetPlayerState), our clones are pinned by never-freed GC handles (a handful per session).

#include <functional>
#include <string>
#include <utility>
#include <vector>

struct Il2CppObject;

namespace sr2::core {
class ModuleContext;
}

namespace sr2::modules::custom_upgrades::vacpack {

// Per-save value of the Drone Battery Saver: the owned level, 0-based ("" = none).
inline constexpr const char* kDroneBatteryKey = "custom_upgrades.vacpack.drone_battery";
// Old builds sold the Battery Saver per drone station: "<prefix><actor id>" = tier 1..4. Migrated
// once into the global level (the highest tier wins) and then erased.
inline constexpr const char* kOldBatteryPrefix = "drone_battery.station.";

// What the vacpack upgrades need from module.cpp.
struct Host {
    std::function<bool()> on;         // module enabled, not faulted
    std::function<bool()> saveReady;  // a save game is loaded and its sidecar state is read
    std::function<std::string(const std::string&)> value;                 // per-save value (sidecar)
    std::function<bool(const std::string&, const std::string&)> setValue;
    std::function<Il2CppObject*()> lookup;                                // LookupDirector
    // Per-save values whose key starts with the prefix (key, value).
    std::function<std::vector<std::pair<std::string, std::string>>(const std::string&)> valuesWithPrefix;
    // Drone Battery Saver: the Drone Station GadgetDefinition (icon, pedia entry; nullptr = unknown)
    // and whether its blueprint is owned (1 / 0, -1 = unknown: the card is shown). Optional.
    std::function<Il2CppObject*()> droneStation;
    std::function<int()> droneBlueprint;
};

class VacpackUpgrades {
public:
    // Worker thread: resolve by name and create the hooks (disabled until the module is enabled).
    // false = no vacpack upgrades (logged); the rest of custom_upgrades keeps working.
    bool Setup(sr2::core::ModuleContext& ctx, Host host, std::string& why, std::string& report);
    bool Ready() const { return ready_; }

    // Main thread, ~1 Hz while the module is on: register the upgrades, put the saved levels and
    // the extra slots' contents back after a load.
    void Tick();
    // Main thread: module disabled / faulted. Levels off (modifiers removed), our definitions out
    // of the game's lists, the player's slot definitions back to vanilla for the next load. The
    // sidecar keeps the levels.
    void Cleanup(const char* why);
    // Save game loaded / new game started (before the game builds the new player model).
    void Reset();

    // Main thread: the owned Drone Battery Saver level, 0 (none / no save / off) .. 4.
    int DroneBatteryLevel() const;

private:
    bool ready_ = false;
};

}  // namespace sr2::modules::custom_upgrades::vacpack
