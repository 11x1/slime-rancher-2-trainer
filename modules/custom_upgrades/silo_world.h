#pragma once
// custom_upgrades: the in-world button of the silo "Sell-All Button" upgrade.
//
// A clone of the corral Plort Collector's press button ("techActivator": Animator, stand, button
// mesh, child "triggerActivate" = PlortCollectorActivator + TechUIInteractable), parented to the
// silo plot. module.cpp decides WHICH silos own the upgrade (sidecar) and runs the sale; this
// class only builds, finds, removes the clones and gives press feedback. Main thread only,
// ~1 Hz ticks (plus press / sell / disable). Clones are found again every tick by name under
// the plot ("sr2mods_SiloSellButton#<generation>"); silo plots come from
// SceneContext.GameModel.GetLandPlotModel(id) for the ids module.cpp passes. No GC handles on
// plots or clones, no FindObjectsOfTypeAll.

#include <cstdint>
#include <functional>
#include <set>
#include <string>
#include <vector>

struct Il2CppObject;

namespace sr2::il2cpp {
class Resolver;
}

namespace sr2::modules::custom_upgrades::world {

// What a button looks like and where it goes (defaults: the silo's gold Sell-All button). Round 11:
// a second instance builds the blue Corral Radio button with kRadioButtonLook.
struct ButtonLook {
    const char* name = "sr2mods_SiloSellButton#";  // clone name prefix (+ generation)
    int32_t plotType = 5;                           // LandPlot.Id of the plots it goes on (SILO)
    float pos[3] = {5.58f, 1.20f, -5.58f};          // plot-local: outer (+x, -z) corner
    float rot[4] = {0.0f, 0.92387953f, 0.0f, 0.38268343f};  // 135 deg about y: faces outwards
    float tint[3] = {1.0f, 0.78f, 0.18f};           // palette tint of the base (gold)
    float lit[3] = {1.0f, 0.74f, 0.16f};            // the cap's own HDRP/Lit _BaseColor
    const char* material = "sr2mods_gold";          // name of our materials
    const char* what = "silo sell button";          // in the log
    const char* hudOwner = "custom_upgrades.silo_button";  // ui::hud owner of its world message
};

// The Corral Radio button: corral plots, the free (+x, -z) corner (see silo_world.cpp), blue.
inline ButtonLook RadioButtonLook() {
    ButtonLook l;
    l.name = "sr2mods_RadioButton#";
    l.plotType = 2;  // CORRAL
    l.tint[0] = 0.22f, l.tint[1] = 0.55f, l.tint[2] = 1.0f;
    l.lit[0] = 0.16f, l.lit[1] = 0.48f, l.lit[2] = 1.0f;
    l.material = "sr2mods_blue";
    l.what = "corral radio button";
    l.hudOwner = "custom_upgrades.radio_button";
    return l;
}

class SiloButtons {
public:
    // Worker thread: resolve by name. false = no world button (the menu action still works).
    bool Setup(sr2::il2cpp::Resolver& R, const char* module, std::string& why, const ButtonLook& look = {});
    bool Ready() const { return ready_; }

    // Main thread, ~1 Hz. `wanted` = LandPlotLocation ids of silos that own the upgrade.
    // `lookup` returns the LookupDirector (only called when a button has to be built).
    void Tick(const std::vector<std::string>& wanted, const std::function<Il2CppObject*()>& lookup);
    // Main thread: remove every button we built (module disabled / faulted).
    void RemoveAll();
    // Main thread: remove the button of this silo plot right away (upgrade sold).
    void Remove(Il2CppObject* plot);
    // Save game loaded / new game: buttons built before belong to the old game.
    void Reset();

    // PlortCollectorActivator.Activate detour (main thread): true if `activator` is one of our
    // clones (its Collector is null and it sits under a "sr2mods_SiloSellButton#" object).
    bool IsOurs(Il2CppObject* activator);
    // The silo LandPlot our clone stands on (nullptr if none / not a silo).
    Il2CppObject* OwnerPlot(Il2CppObject* activator);
    // Button animation + press cue, rate limited like vanilla (0.4 s). false = too soon.
    bool Press(Il2CppObject* activator);
    // A short world-space text above the button (ui::hud), `seconds` long.
    void Message(Il2CppObject* activator, const std::string& text, bool good, float seconds = 3.5f);
    // Main thread, every frame: publishes / clears the message (cheap when none is shown).
    void Frame();

private:
    struct Impl;
    Impl* impl_ = nullptr;
    bool ready_ = false;
    unsigned generation_ = 1;
    std::set<std::string> built_;  // plot ids that may carry a button (visited until confirmed gone)
};

}  // namespace sr2::modules::custom_upgrades::world
