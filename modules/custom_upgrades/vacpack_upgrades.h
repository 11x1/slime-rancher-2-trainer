#pragma once
// custom_upgrades: vacpack upgrades sold in the Fabricator's Upgrades category, in the game's own
// style (SR2 1.3.0). Design + findings: modules/custom_upgrades/NOTES.md "Vacpack upgrades".
//
//   upgrade              levels  effect                                              shown
//   Tank Booster IX-XII   4      125 / 150 / 175 / 200 items per slot                on the vanilla Tank Booster
//                                                                                    card, after VIII
//   Extra Tank III-IV     2      a 7th / 8th vacpack slot, before the water slot     on the vanilla Extra Tank
//                                                                                    card, after II
//   Drone Battery Saver   4      every drone station's drone uses 1/1.25 .. 1/2 of   own card, once the Drone
//     I-IV                       the battery (effect: drone_battery.cpp)             Station blueprint is owned
//
// Tiers: four / two more UpgradeLevelDefinitions (deep copies from Object.Instantiate of
// "AmmoCapacity", with our values, costs and texts) appended to the vanilla definitions' _levels at
// runtime, so the vanilla card simply has more levels (fabricator_levels.cpp draws the extra
// circles). Levels replace each other in vanilla, so each tier's modifier is absolute.
// The vanilla save never holds a level past the vanilla max: UpgradeModel.Pull is detoured to clamp
// Tank Booster / Extra Tank to VIII / II in the save's copy; the real level is a per-save value in
// the custom_upgrades sidecar (written by the SetUpgradeLevel detour) and the ~1 Hz tick puts it back
// on the model after a load (only on top of a maxed vanilla level in the save).
// The Drone Battery Saver is its own card: a clone of "AmmoCapacity" with its own _uniqueId,
// appended to the player's UpgradeDefinitionList (+ the Fabricator category's list); its id is
// removed from the save's copy, its level lives in the sidecar.
// The extra slots are two more AmmoSlotDefinitions (clones of "ExpansionAmmoSlot 2", thresholds 7
// and 8 on AmmoSlotsAvailable) inserted before the water slot in PlayerState._ammoSlotDefinitions
// before the game builds the player's AmmoSlotManager (PlayerState.InitModel / Reset). The save
// keeps the vanilla layout: AmmoModel.Pull hands it the vanilla slots in the vanilla order (the
// extra slots' contents go to the sidecar), AmmoModel.Push spreads a loaded vanilla array into our
// layout (water back into the water slot). Number keys (settings, default 8 / 9) select the extra
// slots; the game's key 7 still selects the water slot; the HUD row is widened so it stays centred.
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
    // Worker thread: resolve by name and create the hooks (disabled until the module is enabled),
    // register the extra slots' key settings. false = no vacpack upgrades (logged); the rest of
    // custom_upgrades keeps working.
    bool Setup(sr2::core::ModuleContext& ctx, Host host, std::string& why, std::string& report);
    bool Ready() const { return ready_; }

    // Main thread, ~1 Hz while the module is on: register the upgrades / extend the vanilla ones,
    // put the saved levels and the extra slots' contents back after a load, keep the HUD row wide.
    void Tick();
    // Main thread, every frame while the module is on: the extra slots' number keys.
    void Frame();
    // Main thread: module disabled / faulted. Levels off or back to the vanilla max, our levels and
    // definitions out of the game's lists, the live slots back in the vanilla order and the player's
    // slot definitions back to vanilla for the next load. The sidecar keeps the levels.
    void Cleanup(const char* why);
    // Save game loaded / new game started (before the game builds the new player model).
    void Reset();

    // Main thread: the owned Drone Battery Saver level, 0 (none / no save / off) .. 4.
    int DroneBatteryLevel() const;

private:
    bool ready_ = false;
};

}  // namespace sr2::modules::custom_upgrades::vacpack
