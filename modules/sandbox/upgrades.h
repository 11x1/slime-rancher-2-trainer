#pragma once
// sandbox: the upgrades manager on the Sandbox page's "Upgrades & Blueprints" card (see NOTES.md,
// "Upgrades manager"): every player upgrade with its tier, raised or lowered with
// UpgradeModel.SetUpgradeLevel (the game's own level change: ActorUpgradeHandler applies that
// level's modifiers, or none at -1); discover / undiscover the upgrades the Fabricator hides until
// found; gadget blueprints locked / unlocked per Fabricator category. After a change an open
// Fabricator is rebuilt the way it builds itself when it opens. SR2 1.3.0.
//
// Nothing here is required: a missing symbol turns only that part off (logged, the card says why).
// No hooks of its own (the open Fabricator comes from module.cpp's FabricatorUI hooks).

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "ui/sandbox_bridge.h"  // CostLine

struct Il2CppObject;

namespace sr2::core {
class ModuleContext;
}

namespace sr2::modules::sandbox {

// What the upgrades manager needs from module.cpp (all main thread).
struct UpgradesHost {
    std::function<Il2CppObject*(uintptr_t)> recipe;  // crafter recipe id -> its live GadgetDefinition (nullptr: unknown)
    std::function<void(bool)> refreshFabricator;     // the open Fabricator (true: rebuild its grid, RebuildFabricator)
    std::function<void()> changed;                   // levels / blueprints changed: the main snapshot's counts now
    std::function<std::string(bool)> vacpackState;   // "slot 1 holds up to 60, ..." (log; true: maxima follow AmmoLimit)
    // The Newbucks and refinery item lines of a PurchaseCost struct in place (module.cpp CostOf; nothing while the
    // crafter's cost offsets are missing), for the upgrade entries' tooltips (the next level's cost).
    std::function<void(const char*, std::vector<sr2::ui::sandbox::CostLine>&)> costLines;
};

// Worker thread, from Sandbox::Setup. Never fails the module.
void SetupUpgrades(sr2::core::ModuleContext& ctx, UpgradesHost host);
// Module on / off (any thread): attaches the request handler.
void UpgradesEnabled(bool on);
// Main thread, every frame while the module is live. `snapshot`: the page is open.
void UpgradesTick(bool snapshot);

// Main thread. Rebuilds the open Fabricator `fabricatorUi` (a FabricatorUI) as it builds itself when
// it opens: new category runtimes (FabricatorCategoryList.FindCategoriesToShow, which makes new
// fabricatable items: icons, tiers, placeholders, blueprints), the category buttons, then the item grid
// on the same category, page and selection. False if it could not (nothing open, symbols missing).
bool RebuildFabricator(Il2CppObject* fabricatorUi);

}  // namespace sr2::modules::sandbox
