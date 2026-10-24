#pragma once
// Data exchange between the sandbox module's upgrades manager
// (modules/sandbox/upgrades.cpp) and the "Upgrades & Blueprints" card on the
// Sandbox page (ui/sandbox_upgrades.cpp). Kept apart from sandbox_bridge.h like
// the player extras (sandbox_extras_bridge.h).
//
// Threading is the same as sandbox_bridge.h: the module publishes a State on
// the Unity main thread (~2 Hz while the page is open, and right after each
// change); the render thread only reads it. Requests are posted to the main
// thread and handled there. Results go to the shared status line
// (sandbox::SetStatus) and the log.
//
// The blueprint tabs list the crafter's recipes from the main snapshot
// (sandbox_bridge.h: Snapshot::recipes, recipeCategories); their ids are the
// recipe ids, which the module looks up in the crafter's own list.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace sr2::ui::sandbox::upgrades {

// One player upgrade (UpgradeModel.upgradeDefinitions: Tank Booster, Extra Tank, Jetpack, ...).
struct Upgrade {
    uintptr_t id = 0;        // opaque (the UpgradeDefinition's address; checked against the live list on use)
    std::string name;        // base name without the tier ("Tank Booster")
    uint32_t icon = 0;       // ui/item_icons.h id (UpgradeDefinition._icon), 0 = none
    int level = -1;          // UpgradeModel.GetUpgradeLevel: -1 = not bought, 0 = tier I, ...
    int levels = 0;          // UpgradeDefinition.LevelCount (with custom_upgrades' tiers appended to the vanilla card)
    int vanillaLevels = 0;   // levels the vanilla game has (0 = all of them); the rest come from custom_upgrades
    bool discoverable = false;  // hidden in the Fabricator until found (_requireComponentToBeVisibleToPlayer + a level-0 component)
    bool discovered = true;     // not hidden right now (bought, or the player holds level 0's components)
    bool slotTier = false;      // the vacpack slot count upgrade ("AmmoSlots", Extra Tank)
};

struct State {
    unsigned long long tick = 0;  // GetTickCount64 when it was taken
    bool inGame = false;
    std::string why;              // !inGame / no upgrade list: what is missing
    std::vector<Upgrade> upgrades;  // in the Fabricator's order
    std::string blueprintWhy;     // blueprint lock / unlock unavailable: why ("" = available)
};

enum class Op {
    SetLevel,       // id, value = new level (-1 .. levels - 1)
    Discover,       // id: gives level 0's upgrade components (as finding it does); no level bought
    Undiscover,     // id: back to "not bought" and level 0's components taken away (hidden again)
    DiscoverAll,    // every discoverable upgrade
    MaxAll,         // every upgrade to its last level
    SetBlueprints,  // ids (recipe ids), value = 1 unlock / 0 lock
};

struct Action {
    Op op = Op::SetLevel;
    uintptr_t id = 0;
    long long value = 0;
    std::vector<uintptr_t> ids;
};

// ---- module side (Unity main thread) ----
void Publish(State s);
void SetHandler(void (*fn)(const Action&));  // nullptr detaches (module off). Any thread.

// ---- render thread ----
std::shared_ptr<const State> Get();  // nullptr until the first publish
bool Request(Action a);

}  // namespace sr2::ui::sandbox::upgrades
