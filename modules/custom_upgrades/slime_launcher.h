#pragma once
// custom_upgrades: the effect of the Fabricator upgrade "Slime Launcher" I..III (bought in
// vacpack_upgrades.cpp, own card). Each level shoots slimes out of the vacpack faster, so they fly
// further: slimes from the ammo slots and largos (or any slime) held at the nozzle. Food, plorts,
// toys and every other item keep the vanilla speed. Not owned = vanilla.
//
// How the vacpack shoots (SR2 1.3.0, IDA): VacuumItem.Update -> Expel(float volume) (0xA66C00, hooked
// by misc_tweaks) picks one of two paths:
//   * held object (_held, largos and toys): ExpelHeld(float) 0xA66730, velocity = nozzle up x EjectSpeed
//     (+0x28) + the player's velocity, Rigidbody.set_linearVelocity, Vacuumable.Launch(PLAYER);
//   * ammo slot (inlined ExpelAmmo): AmmoSlotManager.PopFromSelectedSlot, then the public
//     Expel(AmmoMetadata, bool ignoreEmotions, float velocityOverride, bool forceUncollected) 0xA66E40:
//     speed = velocityOverride if > 0, else EjectSpeed (x3 for ValleyAmmoTypeGroup; GetSpeed inlined).
//     Also called by BossFightController.<ExpelAmmo>d__106 (the boss fight's vacpack shots).
// Both read the VacuumItem's EjectSpeed field, so the two detours scale that field for the one call when
// the shot is a slime (AmmoMetadata.Id / the held object's Identifiable.identType is a SlimeDefinition,
// largos included) and put the vanilla value back right after. The vanilla value is remembered per
// VacuumItem, so a call that unwinds through the detour (managed exception) can't leave a scaled
// speed behind for long: the next shot starts from the remembered value again.
//
// Threading: Setup on the worker thread; detours and everything else on the Unity main thread.

#include <string>

struct Il2CppObject;

namespace sr2::core {
class ModuleContext;
}

namespace sr2::modules::custom_upgrades::launcher {

inline constexpr int kLevels = 3;
// Shot speed factor per level (index 0 = not owned). The flight distance grows about with its square
// (1.44x / 1.96x / 2.56x on flat ground).
inline constexpr float kSpeedFactor[kLevels + 1] = {1.0f, 1.2f, 1.4f, 1.6f};

// Shared with vacpack_upgrades.cpp (main thread writes, any thread reads): the owned level 0..kLevels.
void SetLevel(int level);
int Level();

// Main thread: the "Pink" SlimeDefinition (the card's vanilla icon and pedia picture), nullptr until it is
// loaded (looked for at most every ~10 calls; vacpack_upgrades.cpp asks ~1 Hz until it has it).
Il2CppObject* CardSlime();

class SlimeLauncher {
public:
    // Worker thread: resolve by name, create the hooks (toggled with the module). false = no effect
    // (logged); the Fabricator card still sells and changes nothing.
    bool Setup(sr2::core::ModuleContext& ctx, std::string& why, std::string& report);
    bool Ready() const { return ready_; }

private:
    bool ready_ = false;
};

}  // namespace sr2::modules::custom_upgrades::launcher
