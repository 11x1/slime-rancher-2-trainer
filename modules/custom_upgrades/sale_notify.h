#pragma once
// custom_upgrades: HUD feedback for the silo Sell-All: one game notification per plort type sold
// (icon, "<plort> sold"-style name, count), the same popups the quantum drones raise when they sell
// plorts at the market. SR2 1.3.0, all lookups by name.
//
// Game side (disassembly, RVAs for reference): the HUD's ItemAcquisitionNotificationList
// (MonomiPark.SlimeRancher.UI.HUD) polls INotificationProviders (`_providers`). The drone one,
// DroneActivityNotificationProvider (.UI.HUD.Notification), keeps its ready-to-show items in
// `_activeNotifications` (a DroneActivitySceneNotifications); its private AddPlortSold(
// IdentifiableType, int) (0x75A9D0) adds / merges a per-type entry, and DequeuePlortsSold
// (0x75B200) turns it into a NotificationEntry {icon = type.icon, name = PlortSoldFormat with the
// type's localized name as {0}, count, DefaultStyle}. The list's private
// HandleNotificationAvailable(provider) (0x76B7B0) dequeues every item into its pending queue and
// shows them page by page, unless its HUD-suppression query is satisfied (menus open: get_
// CanQueueAny false, 0x76D2B0) - then it drains and DISCARDS them. So we only hand items over
// while CanQueueAny is true: a sale from the plot menu waits until the menu is closed.
//
// Main thread only; every game call through Resolver::Invoke (managed exceptions caught, a native
// fault marks the module faulted). No GC handles: queued IdentifiableTypes are persistent assets
// and are class- and IsAlive-checked again before use. While something is queued, one
// FindObjectOfType(ItemAcquisitionNotificationList) every 0.25 s; nothing otherwise. If that first
// active list lacks the drone provider, every loaded list is checked (FindObjectsOfTypeAll, active
// ones only), at most once per second; why none fits is logged once.

#include <string>
#include <utility>
#include <vector>

struct Il2CppObject;

namespace sr2::il2cpp {
class Resolver;
}

namespace sr2::modules::custom_upgrades::ui {

class SaleNotices {
public:
    SaleNotices();
    ~SaleNotices();
    SaleNotices(const SaleNotices&) = delete;
    SaleNotices& operator=(const SaleNotices&) = delete;

    // Worker thread: resolve by name. false = unavailable (`why`); callers keep their text feedback.
    bool Setup(sr2::il2cpp::Resolver& R, const char* module, std::string& why);
    bool Ready() const { return ready_; }

    // Main thread: true if the HUD list with the drone provider exists right now (the notices will
    // be shown, maybe after a menu closes). false = caller shows its own feedback.
    bool Available();
    // Main thread: queue one notice per (IdentifiableType, count); shown at once if the HUD
    // allows it, else when it does (dropped after 2 minutes).
    void Queue(const std::vector<std::pair<Il2CppObject*, int>>& sold);
    // Main thread, every frame (one branch while nothing is queued).
    void Frame(float dt);
    // Drop anything queued (save loaded / new game / module off).
    void Clear();

private:
    struct Impl;
    Impl* impl_ = nullptr;
    bool ready_ = false;
};

}  // namespace sr2::modules::custom_upgrades::ui
