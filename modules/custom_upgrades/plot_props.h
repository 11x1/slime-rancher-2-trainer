#pragma once
// custom_upgrades: decorative props that show an owned tier upgrade on its plot (checklist u5,
// u6, u9, u11): hay bales in the coop (Faster Hens I), extra nests, a flower pillow and a stuffed
// chicken (Faster Hens II), a third sprinkler in the garden (Fast Growth; II adds a fourth), glow
// and embers in the incinerator's fire (Ash Booster).
//
// Every prop is a clone of a vanilla model (no asset files of ours): children of decoration /
// toy prefabs (LookupDirector._gadgetDefinitions / _toyDefinitions -> IdentifiableType.prefab,
// matched by prefab name) and of plot prefabs (LookupDirector.GetPlotPrefab). The clones are
// made under an INACTIVE holder (no Awake runs), stripped of colliders, joints, rigidbodies and
// game scripts (every MonoBehaviour outside the UnityEngine namespace), then activated: pure
// renderers + particle systems, nothing to bump into, vac, pick up or save.
//
// module.cpp decides WHICH plots want WHICH props (sidecar ownership); this class builds, finds
// and removes them. Main thread only, ~1 Hz ticks (plus disable / load). Holders are found again
// every tick by name under the plot ("sr2props_<Kind>#<generation>"; not "sr2mods_", which
// coop_world's scan treats as its own). Plots come from SceneContext.GameModel.GetLandPlotModel
// (id) like the silo buttons. Prefab sub-objects are looked up once and pinned (re-resolved
// only if Unity destroyed them). No FindObjectsOfTypeAll, nothing per frame.

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

enum PropKind : uint32_t {
    kPropCoopHay = 1u << 0,         // coop, Faster Hens I or II: two small hay bales
    kPropCoopNests = 1u << 1,       // coop, Faster Hens II: two nests, a flower pillow, a stuffed chicken
    kPropGardenSprinkler = 1u << 2, // garden, Fast Growth I or II: a sprinkler on the free +x side
    kPropIncinGlow = 1u << 3,       // incinerator, Ash Booster: glow + embers in the fire
    kPropGardenSprinkler2 = 1u << 4, // garden, Fast Growth II: a fourth sprinkler on the -x side
};

struct PropRequest {
    std::string id;      // LandPlotLocation id
    uint32_t kinds = 0;  // PropKind bits
};

class PlotProps {
public:
    // Worker thread: resolve by name. false = no props (the upgrades work without them).
    bool Setup(sr2::il2cpp::Resolver& R, const char* module, std::string& why);
    bool Ready() const { return ready_; }

    // Main thread, ~1 Hz. `wanted`: plots with at least one prop; every other plot we built on
    // loses its props. `lookup` returns the LookupDirector (only called when a prop is built).
    void Tick(const std::vector<PropRequest>& wanted, const std::function<Il2CppObject*()>& lookup);
    // Main thread: remove every prop we built (module disabled / faulted).
    void RemoveAll();
    // Save game loaded / new game: props built before belong to the old game.
    void Reset();

private:
    struct Impl;
    Impl* impl_ = nullptr;
    bool ready_ = false;
    unsigned generation_ = 1;
    std::set<std::string> built_;  // plot ids that may carry props (visited until confirmed bare)
};

}  // namespace sr2::modules::custom_upgrades::world
