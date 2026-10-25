#pragma once
// custom_upgrades: Auto-Feeder fill lights (round 17; user on the round 16 test: "for the feeder models
// ingame, color them red -> yellow -> green based on how full the are (the smaller lights besides the button
// that are green)"). Design: modules/custom_upgrades/NOTES.md "Auto-Feeder fill lights"; RE: re/plots.md
// "SlimeFeeder".
//
// The lights are the LED arrows of the feeder's two depositor ports (asset bundle dump of patchCorral >
// SlimeFeeder: techDepositor > ... > techDepositorPort01_inputOutput, the input port beside the speed button,
// and techChute > ... the output port up at the distributor), material "TechDepositor_ArrowsLed": its green
// is _EmissionGlowHigh (0.368, 1, 0) / _EmissionGlowLow (0.406, 0.679, 0.363). The port Animator
// (AnimCtrl_DepositorPort) animates _GradientAnimationMultiply, _Intensity, _ExposureMultiply and
// _GradientOffset on that renderer, never the two glow colours, so a MaterialPropertyBlock with the two
// colours holds.
//
// Fill = SiloStorage.GetSlotCount(0) / GetSlotMaxCount(0) of the feeder's storage: empty red, half yellow,
// full green (vanilla's green). Every renderer under the storage object whose material has
// _EmissionGlowHigh is tinted. Setting "FeederFillLights" (default on); off / module off: the material's own
// colours go back into the block.
//
// Cost: one Resources.FindObjectsOfTypeAll(SlimeFeeder) every 10 s (and at once after a load or a dead
// feeder); per second two int calls per feeder; a block is written only when the level (1/20 steps) changes.
//
// Threading: Setup on the worker thread; everything else on the Unity main thread.

#include <string>

namespace sr2::il2cpp {
class Resolver;
}

namespace sr2::modules::custom_upgrades::feeder {

inline constexpr const char* kLightsSetting = "FeederFillLights";

class FeederLights {
public:
    // Worker thread: resolve by name and register the setting. false = no tint (logged by the caller).
    bool Setup(sr2::il2cpp::Resolver& R, const char* module, std::string& why);
    bool Ready() const { return ready_; }

    // Main thread, ~1 Hz (world tick): tint every live feeder by its fill level, or restore while off.
    void Tick();
    // Main thread: the material colours back on every tinted light; forget the feeders (module off, fault).
    void Cleanup(const char* why);
    // A save is loading: forget the feeders without touching them (their scene goes).
    void Reset();

private:
    bool ready_ = false;
};

}  // namespace sr2::modules::custom_upgrades::feeder
