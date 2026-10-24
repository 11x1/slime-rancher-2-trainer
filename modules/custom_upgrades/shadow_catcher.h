#pragma once
// custom_upgrades: the effect of the Fabricator upgrade "Shadow Catcher" (bought in vacpack_upgrades.cpp,
// one level, per-save value vacpack::kShadowCatcherKey). Research: re/shadow-slimes.md; design and test
// notes: modules/custom_upgrades/NOTES.md "Shadow Catcher".
//
// While the upgrade is owned:
//   * the vacpack can hold Shadow slimes: AmmoSlotDefinition.IsAllowed answers true for the "Shadow"
//     SlimeDefinition on the player's regular slot definitions (the ones sharing AmmoSlot1's type group,
//     so also the Extra Tank III / IV clones; never the water slot, silos, drones or feeders). Vanilla's
//     "VaccableNonLiquids" group leaves Shadow out, so the consume coroutine found no slot;
//   * shadow slimes no longer notice the player walking up (ShadowSlimeScatter.ScatterIfPlayerNear is
//     skipped and Relevancy says 0 while the slime is not alerted). Hitting one (ProducePlortsOnHit ->
//     IFleeBehavior.StartFleeing) still alerts it and its neighbours as in vanilla;
//   * on the player's ranch (CellDirector.IsOnRanch on the slime's RegionMember, the test the game's own
//     feral lifetime uses) they stay: no alert at all (DoAlert skipped, an alert from before is cleared,
//     Action never reaches its Destroyer.DestroyActor), the hit-plort counter never reaches the
//     "despawn after 10 plorts" limit (ProducePlortsOnHit.ProducePlort resets _numPlortsSpawned), and
//     SlimeFeral already never poofs a slime on the ranch;
//   * kept slimes eat: the Shadow diet's EatMap gets copies of the Pink slime's food entries (fruit,
//     veggies, meat) that produce Shadow plorts, and each kept slime's SlimeEat gets KeepEatingWhenFull
//     (Shadow never gets hungry: its hunger recovery rate is 0) and recomputes its eats (InitFood).
//
// Save: a shadow slime on the ranch is a plain actor of type Shadow. The save code stores every actor
// except GameModel.ignoreOnLoad ("SceneObjectGroup" = the player) and Shadow has a persistence id, so
// it saves and loads natively. Shadow slimes in the vacpack never reach the vanilla save: vacpack's
// AmmoModel.Pull detour moves them into the sidecar (see vacpack_upgrades.cpp).
//
// Threading: Setup on the worker thread, everything else on the Unity main thread. The shared flags
// below are written by the vacpack tick and read by the detours.

#include <string>
#include <vector>

struct Il2CppObject;

namespace sr2::core {
class ModuleContext;
}

namespace sr2::modules::custom_upgrades::shadow {

// Shared with vacpack_upgrades.cpp (main thread writes, any thread reads).
void SetOwned(bool owned);  // the Shadow Catcher is owned in the loaded save
bool Owned();
// The player's regular vacpack slot definitions (at most 16; empty = none known).
void SetPlayerSlots(const std::vector<Il2CppObject*>& defs);
// The "Shadow" SlimeDefinition and its plort, nullptr until found (main thread).
Il2CppObject* ShadowType();
Il2CppObject* ShadowPlort();

class ShadowCatcher {
public:
    // Worker thread: resolve by name, create the hooks. false = no Shadow Catcher effect (logged); the
    // Fabricator card still sells (it then changes nothing).
    bool Setup(sr2::core::ModuleContext& ctx, std::string& why, std::string& report);
    bool Ready() const { return ready_; }

    // Main thread, ~1 Hz while custom upgrades are on: find the Shadow types once, keep the diet, decide
    // which tracked shadow slimes stand on the ranch, switch their eating on / off.
    void Tick();
    // Main thread: module off / faulted. Diet back to vanilla, kept slimes back to vanilla eating,
    // tracked slimes forgotten.
    void Cleanup(const char* why);

private:
    bool ready_ = false;
};

}  // namespace sr2::modules::custom_upgrades::shadow
