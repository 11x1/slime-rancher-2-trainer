#pragma once
// custom_upgrades: the Slime Radio in the shop's Daily Deals - at one restock in four it is the daily toy
// (instead of a vanilla toy), bought with Newbucks (one per restock) and dispensed like any toy.
//
// Save model (the vanilla save only ever sees vanilla ids), like the Fabricator path (dj_buildables.h):
//   * The shop entry is a runtime ShopRuntimeItem of our own in the Daily Deals category's item list. Its
//     asset reference is the vanilla Night Light toy's (GUID, reference id and unique id
//     "ToyDefinition.NightLight"); only the display (title, icon, picture, description) is the radio's.
//   * The vanilla toy it stands in for is only taken out of the list the shop UI reads (its data is untouched).
//   * Buying it spends the Newbucks and queues one vanilla Night Light at the shop's spawner
//     (ShopDirector.QueueToSpawnItem, the game's own pending-spawn queue, saved as Night Lights). The
//     purchase is NOT tracked in the shop's purchase records (nothing of ours is written there).
//   * The sidecar counts the radios still to come ("dj.shop_radios_pending"); each Night Light the
//     shop's spawner dispenses while that count is above 0 is listed as a Slime Radio ("dj.radios") and
//     dressed by the DJ buildables' tick.
//   * Without the mod: Night Lights come out of the shop where radios would have; radios already
//     dispensed load as Night Lights (as Fabricator radios do).
//
// Details: radio_shop.cpp (header) and modules/custom_upgrades/NOTES.md ("Slime Radio in the shop").
// Threading: Setup on the worker thread, everything else on the Unity main thread.

#include <functional>
#include <string>

namespace sr2::core {
class ModuleContext;
}

namespace sr2::modules::custom_upgrades::radio_shop {

struct Host {
    std::function<bool()> on;                                       // custom upgrades on (not faulted)
    std::function<bool()> saveReady;                                // a save game is loaded and its sidecar read
    std::function<std::string(const std::string&)> value;           // per-save sidecar value
    std::function<bool(const std::string&, const std::string&)> setValue;
};

class RadioShop {
public:
    // Worker thread, after the DJ buildables' Setup succeeded.
    bool Setup(sr2::core::ModuleContext& ctx, Host host, std::string& why);
    bool Ready() const { return ready_; }
    // Main thread, ~1 Hz while custom upgrades are on and a save is loaded: unlock state, pending count.
    void Tick();
    // Main thread: custom upgrades off / faulted. Our shop entry leaves the item list.
    void Cleanup(const char* why);
    // A save was loaded / new game.
    void Reset();

private:
    bool ready_ = false;
};

}  // namespace sr2::modules::custom_upgrades::radio_shop
