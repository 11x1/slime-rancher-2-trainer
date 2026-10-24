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
// type's localized name as {0}, count, DefaultStyle}. That is the drones' cloud-upload notice too:
// DequeueResourceCollected (0x75B430, for AddResourceCollected) is the same code with
// QuantumCloudResourceCollectedFormat as the text, same entry class and DefaultStyle (both
// decompiled), so the sale notices look exactly like it and only say "sold". The list's private
// HandleNotificationAvailable(provider) (0x76B7B0) dequeues every item into its pending queue and
// shows them page by page, unless its HUD-suppression query is satisfied (menus open: get_
// CanQueueAny false, 0x76D2B0) - then it drains and DISCARDS them. So we only hand items over
// while CanQueueAny is true: a sale from the plot menu waits until the menu is closed.
//
// The sandbox module (modules/sandbox) uses the same class for its spawn / craft feedback with
// Notice::kCollected: DroneActivitySceneNotifications.AddResourceCollected(IdentifiableType, int),
// the quantum drones' cloud-upload notice (icon, name, count), resolved as optional.
//
// Main thread only; every game call through Resolver::Invoke (managed exceptions caught, a native
// fault marks the module faulted). No GC handles: queued IdentifiableTypes are persistent assets
// and are class- and IsAlive-checked again before use. While something is queued, one
// FindObjectOfType(ItemAcquisitionNotificationList) every 0.25 s; nothing otherwise. If that first
// active list lacks the drone provider, every loaded list is checked (FindObjectsOfTypeAll; active
// ones first, else a started inactive one for queued notices, never for Available()), at most once
// per second; why none fits is logged once.

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
    // Which drone notice is raised: "plorts sold" (Sell-All) or "resource collected" (sandbox).
    enum class Notice { kPlortSold, kCollected };

    SaleNotices();
    ~SaleNotices();
    SaleNotices(const SaleNotices&) = delete;
    SaleNotices& operator=(const SaleNotices&) = delete;

    // Worker thread: resolve by name. false = unavailable (`why`); callers keep their text feedback.
    // `label` starts the log lines ("sell-all notices: ...").
    bool Setup(sr2::il2cpp::Resolver& R, const char* module, std::string& why, const char* label = "sell-all");
    // Notice::kCollected can be queued (AddResourceCollected resolved).
    bool CanCollect() const;
    bool Ready() const { return ready_; }

    // Main thread: true if the HUD list with the drone provider exists right now (the notices will
    // be shown, maybe after a menu closes). false = caller shows its own feedback.
    bool Available();
    // Main thread: queue one notice per (IdentifiableType, count); shown at once if the HUD
    // allows it, else when it does (dropped after 2 minutes).
    void Queue(const std::vector<std::pair<Il2CppObject*, int>>& sold, Notice kind = Notice::kPlortSold);
    // Main thread, every frame (one branch while nothing is queued).
    void Frame(float dt);
    // Drop anything queued (save loaded / new game / module off).
    void Clear();

    // ---- free-text notices (map_tools teleport, sandbox teleport to waypoint) ----
    // The same drone-style pop-up with any sprite and text: our own NotificationEntry(provider, null group,
    // icon, name, NotificationEntryStyle.Default) appended to the list's _pendingNotifications (its Update
    // shows pending entries page by page). The name is a LocalizedString of our own (the _localizedName of
    // an Instantiate'd ZoneDefinition, a pool of 4) whose text core/text_override replaces, so it needs the
    // UI's LocalizeStringEvent sink (text_override::SinkActive()).
    // Worker thread, after Setup: false = unavailable (`why`).
    bool SetupText(sr2::il2cpp::Resolver& R, std::string& why);
    bool TextReady() const { return textReady_; }
    // Main thread: queue one notice; false = not possible now (no text sink) - callers just log.
    bool QueueText(Il2CppObject* icon, const std::string& text);
    // Main thread: the game's teleporter sprite for such notices (the icon of a TELEPORTER gadget definition,
    // home teleporter, found once among the loaded GadgetDefinitions and kept); nullptr if none.
    Il2CppObject* TeleporterIcon();

    struct Impl;  // public for the free-text helpers in the .cpp

private:

    Impl* impl_ = nullptr;
    bool ready_ = false;
    bool textReady_ = false;
};

}  // namespace sr2::modules::custom_upgrades::ui
