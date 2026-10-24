#pragma once
// custom_upgrades: Slime DJ buildables - the DJ Booth and the DJ Speakers (Fabricator, Decorations) and
// the Slime Radio toy. All three play the Slime DJ song through slime_dj's emitter API.
//
// Save model (the vanilla save only ever sees vanilla ids), the Quantum Storage Link's scheme
// (quantum_link.h), on its hooks (variant_ext.h):
//   * DJ Booth = a variant of the Scientist Desk decoration (same prefab); DJ Speaker / DJ Speaker
//     (tilted) / DJ Speaker (facing up) = variants of the Boombox decoration. Placed, they ARE the vanilla
//     decoration (saved as one); the sidecar lists which actors are booths ("dj.booths") and speakers
//     with their tilt ("dj.speakers", "id:kind"). The ~1 Hz tick dresses those: extra meshes cloned from
//     the game's own prefabs, the emitter, the dance animation while a song plays.
//   * Fabricating a DJ Booth / DJ Speaker adds the vanilla Scientist Desk / Boombox to the inventory and
//     one kit ("dj.booth_kits" / "dj.speaker_kits"); placing the variant uses a kit, picking it up gives
//     it back. Our definitions never enter the saved inventory, blueprints or favourites.
//   * Slime Radio = a vanilla Night Light toy actor (spawned in front of you when fabricated) listed in
//     "dj.radios"; dressed as a small boombox.
//   * Without the mod: plain Scientist Desks, Boomboxes and Night Lights where they stood; unused kits
//     are gone, the decorations fabricated with them stay in the inventory.
//
// Details: dj_buildables.cpp (header) and modules/custom_upgrades/NOTES.md ("Slime DJ buildables").
// Threading: Setup on the worker thread, everything else on the Unity main thread.

#include <functional>
#include <string>

struct Il2CppObject;

namespace sr2::core {
class ModuleContext;
}

namespace sr2::modules::custom_upgrades::dj {

struct Host {
    std::function<bool()> saveReady;                                // a save game is loaded and its sidecar read
    std::function<std::string(const std::string&)> value;           // per-save sidecar value
    std::function<bool(const std::string&, const std::string&)> setValue;
    std::function<Il2CppObject*()> lookup;                          // LookupDirector
};

class DjBuildables {
public:
    // Worker thread, after the Quantum Storage Link's Setup succeeded (its hooks carry ours).
    bool Setup(sr2::core::ModuleContext& ctx, Host host, std::string& why);
    bool Ready() const { return ready_; }
    // Main thread, ~1 Hz while custom upgrades are on and a save is loaded.
    void Tick();
    // Main thread: custom upgrades off / faulted. Everything undressed, our definitions out of the
    // favourite slots, the held gadget and the variant lists.
    void Cleanup(const char* why);
    // A save was loaded / new game.
    void Reset();

private:
    bool ready_ = false;
};

// Any thread: `def` is one of our runtime definitions (never saved as itself).
bool Owns(Il2CppObject* def);

}  // namespace sr2::modules::custom_upgrades::dj
