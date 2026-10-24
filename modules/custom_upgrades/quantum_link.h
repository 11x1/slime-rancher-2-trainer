#pragma once
// custom_upgrades: the Quantum Storage Link - a Refinery Link tuned to the Quantum Cloud, and the
// drone source that takes items out of the cloud there (drone_source.h, kQuantumSourceType).
//
// Save model (the vanilla save never sees anything of ours):
//   * World: a placed Quantum Storage Link IS a vanilla Refinery Link (same prefab, same GadgetModel,
//     saved as PlacedGadgetV07 with the Refinery Link's type). The per-save sidecar lists the actor ids
//     that are quantum ("quantum_link.actors"); the ~1 Hz tick recolours those links, adds the quantum
//     glow and the hidden storage the drones take from.
//   * Inventory: our GadgetDefinition is a runtime clone of the Refinery Link's and a VARIANT of it
//     (GadgetDefinition.VariantOf). Variants share their base gadget's inventory count
//     (GadgetDirector.GetItemCount -> BaseGadget), placing one spends one Refinery Link, so our
//     definition never enters GadgetsModel._itemCounts / blueprints. Fabricating a Quantum Storage Link
//     adds a Refinery Link and one "quantum kit" (sidecar "quantum_link.kits"); placing the variant uses
//     a kit, picking a quantum link up gives it back. The favourite slots may hold the variant: the
//     save gets the Refinery Link's persistence id for it (SaveReferenceTranslation.GetPersistenceId).
//   * Without the mod: every placed Quantum Storage Link loads as a plain Refinery Link; unused kits
//     are gone, the Refinery Links they came with stay in the inventory. Drone programs saved with the
//     Silo source (drone_source.h).
//
// Details and findings: quantum_link.cpp (header) and modules/custom_upgrades/NOTES.md ("Quantum
// Storage Link"). Threading: Setup on the worker thread, everything else on the Unity main thread.

#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "modules/custom_upgrades/buildables.h"

struct Il2CppObject;

namespace sr2::core {
class ModuleContext;
}

namespace sr2::modules::custom_upgrades::quantum {

struct Host {
    std::function<bool()> on;                                       // custom upgrades on (not faulted)
    std::function<bool()> saveReady;                                // a save game is loaded and its sidecar read
    std::function<std::string(const std::string&)> value;           // per-save sidecar value
    std::function<bool(const std::string&, const std::string&)> setValue;
    std::function<Il2CppObject*()> lookup;                          // LookupDirector
    // HUD notices (icon Sprite or nullptr, text), shown as one burst; false = not possible now.
    std::function<bool(const std::vector<std::pair<Il2CppObject*, std::string>>&)> notify;
};

class QuantumLink {
public:
    // Worker thread. false = not available (`why`; nothing of it is offered).
    bool Setup(sr2::core::ModuleContext& ctx, Host host, std::string& why, std::string& report);
    bool Ready() const { return ready_; }

    // Main thread, ~1 Hz while custom upgrades are on and a save is loaded.
    void Tick();
    // Main thread, every frame while custom upgrades are on: the links' glow glitch (field reads only
    // between glitches).
    void Frame();
    // Main thread: custom upgrades off / faulted. Our definition out of the favourite slots, the held
    // gadget and the variant list; every link back to a plain-looking Refinery Link (the sidecar keeps
    // which ones are quantum). Also after a fault (no game calls then besides what is needed).
    void Cleanup(const char* why);
    // A save was loaded / new game: links built before belong to the old game.
    void Reset();

private:
    bool ready_ = false;
};

// ---- for drone_source.cpp (main thread) ----
// At least one Quantum Storage Link is placed, built and has a drone port.
bool Available();
// The loaded save lists at least one Quantum Storage Link (loaded or not).
bool Placed();
// The StorageMetadata[] a Quantum Storage Link program takes from (the links' storages, their slots
// mirroring the cloud amounts of the types `predicate` accepts), or nullptr. `program` keys a short
// cache of the predicate's answers.
Il2CppObject* Sources(Il2CppObject* predicate, Il2CppObject* program);
// Any thread: `def` is the Quantum Storage Link definition (a runtime clone, never saved as itself).
bool IsOurs(Il2CppObject* def);
// The Quantum Storage Link gadget's icon (a Sprite: our purple copy of the Refinery Link's, else the
// Refinery Link's) once known, else nullptr.
Il2CppObject* SourceIcon();

// ---- for buildables.cpp (main thread) ----
// Fills `out` (id kQuantumLink) once the definition is built; false before.
bool Describe(buildables::Info& out);
// `count` Refinery Links into the inventory and `count` kits (as fabricating, free).
bool Grant(int count, std::string& result);

}  // namespace sr2::modules::custom_upgrades::quantum
