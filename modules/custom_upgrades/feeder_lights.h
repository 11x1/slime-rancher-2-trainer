#pragma once
// custom_upgrades: Auto-Feeder fill lights (round 17; user on the round 16 test: "for the feeder models
// ingame, color them red -> yellow -> green based on how full the are (the smaller lights besides the button
// that are green)"). Design: modules/custom_upgrades/NOTES.md "Auto-Feeder fill lights"; RE: re/plots.md
// "SlimeFeeder".
//
// The lights are the LED arrows of the feeder's two depositor ports (asset bundle dump of patchCorral >
// SlimeFeeder: techDepositor > ... > techDepositorPort01_inputOutput, the input port beside the speed button,
// and techChute > ... the output port up at the distributor), material "TechDepositor_ArrowsLed", shader
// "SR/UI/Digital Monitor Gradient Animated". Round 17 tinted _EmissionGlowHigh / _EmissionGlowLow: stale saved
// values the shader does not have (Material.HasProperty false), so no light was ever found ("Nope, doesnt
// change"). The green is baked into the texture (dif_techDepositor01) and multiplied by _Color (white; not
// animated: the port Animator drives _GradientAnimationMultiply, _Intensity, _ExposureMultiply,
// _GradientOffset), so a MaterialPropertyBlock with _Color as a multiplier tints it.
//
// Fill = SiloStorage.GetSlotCount(0) / GetSlotMaxCount(0) of the feeder's storage: empty red, half yellow,
// full green (vanilla). Setting "FeederFillLights" (group "Plot upgrades", default on); off / module off: the
// material's own _Color goes back into the block.
//
// Finding feeders (round 18; round 17's Resources.FindObjectsOfTypeAll(SlimeFeeder) cost 22-32 ms every 10 s):
// GameModel.landPlots (the save's plots) -> corrals whose LandPlotModel.upgrades contain FEEDER and whose
// plot object is loaded -> GetComponentInChildren(SlimeFeeder) once per corral (cached; again only when the
// feeder died or went inactive). Every 3 s: field reads plus one HashSet.Contains per corral. Per second two
// int calls per feeder; a block is written only when the level (1/20 steps) changes.
//
// Threading: Setup on the worker thread; everything else on the Unity main thread.

#include <string>

struct Il2CppObject;

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
    // gameModel = SceneContext.GameModel (null: nothing to do yet).
    void Tick(Il2CppObject* gameModel);
    // Main thread: the material colours back on every tinted light; forget the feeders (module off, fault).
    void Cleanup(const char* why);
    // A save is loading: forget the feeders without touching them (their scene goes).
    void Reset();

private:
    bool ready_ = false;
};

}  // namespace sr2::modules::custom_upgrades::feeder
