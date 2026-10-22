// Central list of built-in modules. Explicit registration (instead of static
// self-registering objects) keeps static constructors - which run under the
// loader lock - trivial, and makes the load order obvious.

#include <memory>

#include "module_manager.h"

namespace sr2::modules {
std::unique_ptr<sr2::core::Module> CreateSpawnScaling();  // modules/spawn_scaling/module.cpp
std::unique_ptr<sr2::core::Module> CreateHl2Movement();   // modules/hl2_movement/module.cpp
std::unique_ptr<sr2::core::Module> CreateMinimap();       // modules/minimap/module.cpp
std::unique_ptr<sr2::core::Module> CreateCollectibles();  // modules/collectibles/module.cpp
std::unique_ptr<sr2::core::Module> CreateQuickStart();    // modules/quick_start/module.cpp
std::unique_ptr<sr2::core::Module> CreateQuickTravel();   // modules/quick_travel/module.cpp
std::unique_ptr<sr2::core::Module> CreateInstantBuild();  // modules/instant_build/module.cpp
std::unique_ptr<sr2::core::Module> CreateWallMarkers();   // modules/wall_markers/module.cpp
std::unique_ptr<sr2::core::Module> CreatePlotUpgrades();  // modules/plot_upgrades/module.cpp
std::unique_ptr<sr2::core::Module> CreateSellUpgrades();  // modules/sell_upgrades/module.cpp
std::unique_ptr<sr2::core::Module> CreateCustomUpgrades();  // modules/custom_upgrades/module.cpp
}  // namespace sr2::modules

namespace sr2::core {

void RegisterBuiltinModules(ModuleManager& mm) {
    mm.Register(modules::CreateSpawnScaling());
    mm.Register(modules::CreateHl2Movement());
    mm.Register(modules::CreateMinimap());
    mm.Register(modules::CreateCollectibles());
    mm.Register(modules::CreateQuickStart());
    mm.Register(modules::CreateQuickTravel());
    mm.Register(modules::CreateInstantBuild());
    mm.Register(modules::CreateWallMarkers());
    mm.Register(modules::CreatePlotUpgrades());
    mm.Register(modules::CreateSellUpgrades());
    mm.Register(modules::CreateCustomUpgrades());
}

}  // namespace sr2::core
