// custom_upgrades: HUD notices for the silo Sell-All (see sale_notify.h). SR2 1.3.0.

#include "modules/custom_upgrades/sale_notify.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <format>
#include <initializer_list>
#include <optional>
#include <string>
#include <vector>

#include "core/detour_guard.h"
#include "core/il2cpp_api.h"
#include "core/log.h"
#include "core/resolver.h"
#include "core/text_override.h"
#include "core/unity.h"

namespace sr2::modules::custom_upgrades::ui {
namespace {

namespace unity = sr2::core::unity;
using sr2::il2cpp::api;
using sr2::il2cpp::Resolver;

constexpr const char* kImage = "Assembly-CSharp";
constexpr const char* kHudNs = "MonomiPark.SlimeRancher.UI.HUD";
constexpr const char* kNotifNs = "MonomiPark.SlimeRancher.UI.HUD.Notification";
constexpr float kRetrySeconds = 0.25f;
constexpr float kMaxWaitSeconds = 120.0f;
constexpr size_t kMaxQueued = 64;

}  // namespace

struct SaleNotices::Impl {
    const char* module = "custom_upgrades";
    const char* label = "sell-all";
    Il2CppClass* listClass = nullptr;      // ItemAcquisitionNotificationList
    Il2CppClass* providerClass = nullptr;  // DroneActivityNotificationProvider
    Il2CppClass* sceneClass = nullptr;     // DroneActivitySceneNotifications
    Il2CppClass* identClass = nullptr;     // IdentifiableType
    size_t offProviders = 0;               // ItemAcquisitionNotificationList._providers
    size_t offActive = 0;                  // DroneActivityNotificationProvider._activeNotifications
    const MethodInfo* canQueueAny = nullptr;   // ItemAcquisitionNotificationList.get_CanQueueAny
    const MethodInfo* handleAvail = nullptr;   // ItemAcquisitionNotificationList.HandleNotificationAvailable
    const MethodInfo* addPlortSold = nullptr;  // DroneActivitySceneNotifications.AddPlortSold(IdentifiableType, int)
    const MethodInfo* addCollected = nullptr;  // DroneActivitySceneNotifications.AddResourceCollected(IdentifiableType, int) (optional)

    struct Entry {
        Il2CppObject* type;
        int count;
        Notice kind;
    };
    std::vector<Entry> queue;

    // free-text notices
    Il2CppClass* entryClass = nullptr;         // NotificationEntry
    Il2CppClass* zoneClass = nullptr;          // ZoneDefinition (owner of the LocalizedStrings we clone)
    const MethodInfo* entryCtor = nullptr;     // NotificationEntry(provider, group, icon, name, style)
    const MethodInfo* styleDefault = nullptr;  // static NotificationEntryStyle.get_Default
    size_t offPending = 0;                     // ItemAcquisitionNotificationList._pendingNotifications
    size_t offDisplay = 0, offAvailable = 0;   // its _displayEntries (the page's slots) / _availableDisplayCount (optional)
    size_t offZoneName = 0;                    // ZoneDefinition._localizedName
    Il2CppObject* (*objectNew)(Il2CppClass*) = nullptr;  // il2cpp_object_new
    struct Text {
        Il2CppObject* icon;  // a Sprite asset (IsAlive-checked again before use)
        std::string text;
    };
    std::vector<Text> texts;
    struct Name {
        Il2CppGCHandle owner = nullptr, str = nullptr;  // the cloned ZoneDefinition and its LocalizedString
        Il2CppObject* ls = nullptr;
    };
    static constexpr size_t kNames = 16;  // more than a page plus the next one (each entry keeps its string until shown)
    Name names[kNames];
    size_t nextName = 0;
    bool loggedText = false, loggedNoSink = false;
    Il2CppClass* gadgetClass = nullptr;  // GadgetDefinition
    size_t offGadgetType = 0, offIdentIcon = 0;
    Il2CppGCHandle teleIcon = nullptr;
    unsigned long long teleIconTried = 0;
    float retry = 0.0f;
    float waited = 0.0f;
    bool loggedFirst = false;
    bool loggedMissing = false;
    bool loggedScan = false;
    unsigned long long lastScan = 0;  // GetTickCount64 of the last all-instances scan

    bool IsA(Il2CppObject* obj, Il2CppClass* k) const {
        const auto& a = api();
        if (!obj || !k || !a.il2cpp_object_get_class) return false;
        Il2CppClass* c = a.il2cpp_object_get_class(obj);
        if (c == k) return true;
        return c && a.il2cpp_class_is_assignable_from && a.il2cpp_class_is_assignable_from(k, c);
    }
    Il2CppObject* Call(const MethodInfo* m, void* obj, std::initializer_list<void*> args, const char* what,
                       bool* ok = nullptr) {
        std::vector<void*> params(args);
        std::string err;
        Il2CppObject* r = Resolver::Get().Invoke(m, obj, params.empty() ? nullptr : params.data(), &err);
        const bool good = err.empty();
        if (ok) *ok = good;
        if (!good) {
            if (err.rfind("native exception", 0) == 0) sr2::core::MarkModuleFaulted(module, std::format("{}: {}", what, err));
            else SR2_WARN("[{}] {} failed: {}", module, what, err);
        }
        return r;
    }
    // The HUD list that holds the drone provider (nullptr if the HUD is not up). The first active
    // list is not always that one (test n7, 2026-10-23: a world-button sale fell back to the text
    // message without any notice log), so when it lacks the provider every loaded list is checked
    // (active ones first, then a started inactive one), at most once per second (a press, or the
    // 0.25 s retry while something is queued). Why nothing was found is logged once.
    Il2CppObject* List(bool allowInactive) {
        Il2CppObject* first = unity::FindObjectOfType(listClass);
        if (unity::IsAlive(first) && IsA(first, listClass) && Provider(first)) return first;
        const unsigned long long now = GetTickCount64();
        if (now - lastScan < 1000) return nullptr;
        lastScan = now;
        size_t seen = 0, active = 0;
        Il2CppObject* inactive = nullptr;  // a list with the provider whose object is inactive right now
        for (Il2CppObject* l : unity::FindObjectsOfTypeAll(listClass)) {
            if (!unity::IsAlive(l) || !IsA(l, listClass)) continue;
            ++seen;
            const bool on = unity::ActiveInHierarchy(unity::GameObjectOf(l));
            active += on ? 1 : 0;
            if (!Provider(l)) continue;  // prefabs / lists whose Start never ran have no providers
            if (on) return Found(l, true, seen);
            if (!inactive) inactive = l;
        }
        // Inactive but started (its HUD canvas hidden for now): the notices wait in its pending
        // queue and show when it is active again. Not for Available(): the world button then shows
        // its text, since such a list may stay hidden.
        if (inactive && allowInactive) return Found(inactive, false, seen);
        if (!loggedMissing) {
            loggedMissing = true;
            std::string providers;
            if (unity::IsAlive(first)) {
                void* arr = unity::Field<void*>(first, offProviders);
                for (size_t i = 0, n = unity::ArrayLength(arr); i < n && i < 16; ++i)
                    if (auto* p = static_cast<Il2CppObject*>(unity::ArrayAt(arr, i)))
                        providers += (providers.empty() ? "" : ", ") + unity::ClassName(p);
            }
            SR2_INFO("[{}] {} notices: no HUD notification list with the drone provider (first active list {}, "
                     "providers [{}]; {} list(s) loaded, {} active) - text feedback instead",
                     module, label, unity::IsAlive(first) ? "found" : "none", providers, seen, active);
        }
        return nullptr;
    }
    Il2CppObject* Found(Il2CppObject* list, bool active, size_t seen) {
        if (!loggedScan) {
            loggedScan = true;
            SR2_INFO("[{}] {} notices: drone provider found on another HUD notification list ({}, list #{} of "
                     "the loaded ones)", module, label, active ? "active" : "inactive", seen);
        }
        return list;
    }
    Il2CppObject* Provider(Il2CppObject* list) {
        void* arr = unity::Field<void*>(list, offProviders);
        for (size_t i = 0, n = unity::ArrayLength(arr); i < n && i < 16; ++i) {
            auto* p = static_cast<Il2CppObject*>(unity::ArrayAt(arr, i));
            if (p && IsA(p, providerClass)) return p;
        }
        return nullptr;
    }
    // true = done with the queue (shown or given up), false = try again later.
    bool TryFlush() {
        Il2CppObject* list = List(true);
        if (!list) return false;
        Il2CppObject* provider = Provider(list);
        Il2CppObject* scene = provider ? unity::Field<Il2CppObject*>(provider, offActive) : nullptr;
        if (!scene || !IsA(scene, sceneClass)) {
            SR2_WARN("[{}] {} notices: HUD drone notification provider not found - {} notice(s) dropped", module, label,
                     queue.size());
            return true;
        }
        bool ok = false;
        Il2CppObject* boxed = Call(canQueueAny, list, {}, "ItemAcquisitionNotificationList.get_CanQueueAny", &ok);
        if (!ok) return true;
        if (!boxed || !*reinterpret_cast<bool*>(reinterpret_cast<char*>(boxed) + 2 * sizeof(void*)))
            return false;  // HUD suppressed (menu open): it would discard them
        int added = 0;
        for (Entry& e : queue) {
            if (e.count <= 0 || !unity::IsAlive(e.type) || !IsA(e.type, identClass)) continue;
            const bool collected = e.kind == Notice::kCollected;
            if (collected && !addCollected) continue;
            int32_t n = e.count;
            Call(collected ? addCollected : addPlortSold, scene, {e.type, &n},
                 collected ? "DroneActivitySceneNotifications.AddResourceCollected"
                           : "DroneActivitySceneNotifications.AddPlortSold",
                 &ok);
            if (!ok) return true;
            ++added;
        }
        if (added) Call(handleAvail, list, {provider}, "ItemAcquisitionNotificationList.HandleNotificationAvailable", &ok);
        if (!loggedFirst && added) {
            loggedFirst = true;
            SR2_INFO("[{}] {}: {} notice(s) handed to the HUD (drone notifications)", module, label, added);
        }
        return true;
    }
};

// ------------------------------------------------------------ free-text notices --
namespace {

// HUD suppression query (menus open): -1 call failed, 0 suppressed, 1 notices allowed.
int CanQueue(SaleNotices::Impl& p, Il2CppObject* list) {
    bool ok = false;
    Il2CppObject* boxed = p.Call(p.canQueueAny, list, {}, "ItemAcquisitionNotificationList.get_CanQueueAny", &ok);
    if (!ok) return -1;
    return boxed && *reinterpret_cast<bool*>(reinterpret_cast<char*>(boxed) + 2 * sizeof(void*)) ? 1 : 0;
}

// One of our LocalizedStrings (round robin), created on first use from a loaded ZoneDefinition.
Il2CppObject* NextName(SaleNotices::Impl& p) {
    auto& n = p.names[p.nextName++ % SaleNotices::Impl::kNames];
    Il2CppObject* owner = n.owner ? unity::GCHandleTarget(n.owner) : nullptr;
    if (owner && unity::IsAlive(owner) && n.ls) return n.ls;
    if (n.ls) sr2::core::text_override::Remove(n.ls);
    if (n.owner) unity::FreeGCHandle(n.owner);
    if (n.str) unity::FreeGCHandle(n.str);
    n = {};
    Il2CppObject* tpl = nullptr;
    for (Il2CppObject* z : unity::FindObjectsOfTypeAll(p.zoneClass))
        if (unity::IsAlive(z) && p.IsA(z, p.zoneClass) && unity::Field<Il2CppObject*>(z, p.offZoneName)) {
            tpl = z;
            break;
        }
    if (!tpl) return nullptr;
    Il2CppObject* clone = unity::Instantiate(tpl, nullptr);  // a fresh ScriptableObject: its own LocalizedString
    if (!unity::IsAlive(clone) || !p.IsA(clone, p.zoneClass)) return nullptr;
    Il2CppObject* ls = unity::Field<Il2CppObject*>(clone, p.offZoneName);
    if (!ls || ls == unity::Field<Il2CppObject*>(tpl, p.offZoneName)) return nullptr;
    unity::SetName(clone, "SR2Mods.NoticeText");
    n.owner = unity::NewGCHandle(clone, false);
    n.str = unity::NewGCHandle(ls, false);
    if (!n.owner || !n.str) return nullptr;
    n.ls = ls;
    return ls;
}

// true = done with the text queue (shown or given up), false = try again later.
// The list shows its pending entries a page at a time (ShowNextPage takes up to one page once every
// shown entry has faded out), and an entry's text is read from its LocalizedString only when it is shown.
// So notices are handed over a page at a time, only while the list is idle (nothing pending, every display
// slot free): each page's strings stay untouched until it was shown, and a burst of notices (one per
// spawned item type) shows page after page instead of reusing strings that are still waiting.
bool TryFlushText(SaleNotices::Impl& p) {
    Il2CppObject* list = p.List(true);
    if (!list) return false;
    Il2CppObject* provider = p.Provider(list);
    if (!provider) return false;
    const int can = CanQueue(p, list);
    if (can < 0) return true;
    if (can == 0) return false;  // HUD suppressed (menu open)
    auto* pending = unity::Field<Il2CppObject*>(list, p.offPending);
    if (pending && unity::ListCount(pending) > 0) return false;  // the list is still busy with earlier entries
    size_t page = 4;
    if (p.offDisplay) {
        void* slots = unity::Field<void*>(list, p.offDisplay);
        const size_t n = slots ? unity::ArrayLength(slots) : 0;
        if (n > 0) {
            page = n;
            if (p.offAvailable && unity::Field<int32_t>(list, p.offAvailable) < static_cast<int32_t>(n))
                return false;  // a page is still showing
        }
    }
    page = std::min(page, SaleNotices::Impl::kNames / 2);
    const auto& a = api();
    Il2CppClass* pk = pending && a.il2cpp_object_get_class ? a.il2cpp_object_get_class(pending) : nullptr;
    const MethodInfo* add = pk ? Resolver::Get().Method(pk, "Add", 1) : nullptr;
    if (!add) {
        SR2_WARN("[{}] text notices: ItemAcquisitionNotificationList._pendingNotifications.Add not found - {} dropped",
                 p.module, p.texts.size());
        return true;
    }
    bool ok = false;
    Il2CppObject* boxedStyle = p.Call(p.styleDefault, nullptr, {}, "NotificationEntryStyle.get_Default", &ok);
    if (!ok || !boxedStyle) return true;
    void* style = reinterpret_cast<char*>(boxedStyle) + 2 * sizeof(void*);
    int added = 0;
    size_t used = 0;
    for (; used < p.texts.size() && used < page; ++used) {
        auto& t = p.texts[used];
        Il2CppObject* ls = NextName(p);
        if (!ls) {
            SR2_WARN("[{}] text notices: no LocalizedString to carry the text - '{}' not shown", p.module, t.text);
            continue;
        }
        sr2::core::text_override::Set(ls, t.text);
        Il2CppObject* icon = t.icon && unity::IsAlive(t.icon) ? t.icon : nullptr;
        Il2CppObject* entry = p.objectNew(p.entryClass);
        if (!entry) return true;
        p.Call(p.entryCtor, entry, {provider, nullptr, icon, ls, style}, "NotificationEntry..ctor", &ok);
        if (!ok) return true;
        p.Call(add, pending, {entry}, "List<INotificationEntry>.Add", &ok);
        if (!ok) return true;
        ++added;
    }
    if (!p.loggedText && added) {
        p.loggedText = true;
        SR2_INFO("[{}] text notices: {} notice(s) added to the HUD's pending notifications", p.module, added);
    }
    p.texts.erase(p.texts.begin(), p.texts.begin() + static_cast<std::ptrdiff_t>(used));
    if (added) p.waited = 0.0f;  // a long sequence is not dropped as "HUD not available"
    return p.texts.empty();
}

}  // namespace

SaleNotices::SaleNotices() = default;
SaleNotices::~SaleNotices() { delete impl_; }

bool SaleNotices::Setup(Resolver& R, const char* module, std::string& why, const char* label) {
    auto* p = new Impl();
    p->module = module;
    p->label = label;
    std::string err;
    p->listClass = R.Class(kImage, kHudNs, "ItemAcquisitionNotificationList", &err);
    p->providerClass = R.Class(kImage, kNotifNs, "DroneActivityNotificationProvider", &err);
    p->sceneClass = R.Class(kImage, kNotifNs, "DroneActivitySceneNotifications", &err);
    p->identClass = R.Class(kImage, "", "IdentifiableType", &err);
    if (!p->listClass || !p->providerClass || !p->sceneClass || !p->identClass) {
        why = err.empty() ? "notification classes not found" : err;
        delete p;
        return false;
    }
    auto offProviders = R.FieldOffset(p->listClass, "_providers", &err);
    auto offActive = R.FieldOffset(p->providerClass, "_activeNotifications", &err);
    p->canQueueAny = R.Method(p->listClass, "get_CanQueueAny", 0, &err);
    p->handleAvail = R.Method(p->listClass, "HandleNotificationAvailable", 1, &err);
    p->addPlortSold = R.MethodByParams(p->sceneClass, "AddPlortSold", {"IdentifiableType", "System.Int32"}, &err);
    if (!offProviders || !offActive || !p->canQueueAny || !p->handleAvail || !p->addPlortSold) {
        why = err.empty() ? "notification fields / methods not found" : err;
        delete p;
        return false;
    }
    p->addCollected = R.MethodByParams(p->sceneClass, "AddResourceCollected", {"IdentifiableType", "System.Int32"}, nullptr);
    p->offProviders = *offProviders;
    p->offActive = *offActive;
    delete impl_;
    impl_ = p;
    ready_ = true;
    return true;
}

bool SaleNotices::Available() {
    if (!ready_) return false;
    Il2CppObject* list = impl_->List(false);
    return list && impl_->Provider(list);
}

bool SaleNotices::CanCollect() const { return ready_ && impl_->addCollected; }

void SaleNotices::Queue(const std::vector<std::pair<Il2CppObject*, int>>& sold, Notice kind) {
    if (!ready_ || sold.empty()) return;
    auto& q = impl_->queue;
    for (const auto& e : sold) {
        bool merged = false;
        for (Impl::Entry& x : q)
            if (x.type == e.first && x.kind == kind) {
                x.count += e.second;
                merged = true;
                break;
            }
        if (!merged && q.size() < kMaxQueued) q.push_back({e.first, e.second, kind});
    }
    impl_->waited = 0.0f;
    impl_->retry = 0.0f;
    if (impl_->TryFlush()) q.clear();
}

void SaleNotices::Frame(float dt) {
    if (!ready_ || (impl_->queue.empty() && impl_->texts.empty())) return;
    if (!std::isfinite(dt) || dt < 0.0f) dt = 0.0f;
    dt = dt > 1.0f ? 1.0f : dt;
    impl_->waited += dt;
    impl_->retry += dt;
    if (impl_->retry < kRetrySeconds) return;
    impl_->retry = 0.0f;
    if (!impl_->texts.empty() && TryFlushText(*impl_)) impl_->texts.clear();
    if (impl_->queue.empty()) {
        if (impl_->waited > kMaxWaitSeconds) impl_->texts.clear();
        return;
    }
    if (impl_->TryFlush()) {
        impl_->queue.clear();
    } else if (impl_->waited > kMaxWaitSeconds) {
        SR2_INFO("[{}] {} notices: HUD not available for {:.0f} s - {} notice(s) dropped", impl_->module, impl_->label,
                 kMaxWaitSeconds, impl_->queue.size());
        impl_->queue.clear();
        impl_->texts.clear();
    }
}

void SaleNotices::Clear() {
    if (impl_) {
        impl_->queue.clear();
        impl_->texts.clear();
    }
}

bool SaleNotices::SetupText(Resolver& R, std::string& why) {
    if (!ready_ || !impl_) {
        why = "sale notices not set up";
        return false;
    }
    Impl& p = *impl_;
    std::string err;
    p.entryClass = R.Class(kImage, kNotifNs, "NotificationEntry", &err);
    Il2CppClass* style = R.Class(kImage, kNotifNs, "NotificationEntryStyle", &err);
    p.zoneClass = R.Class(kImage, "MonomiPark.SlimeRancher.World", "ZoneDefinition", &err);
    if (p.entryClass)
        p.entryCtor = R.MethodByParams(p.entryClass, ".ctor",
                                       {"MonomiPark.SlimeRancher.UI.HUD.Notification.INotificationProvider",
                                        "MonomiPark.SlimeRancher.UI.HUD.Notification.INotificationGroup",
                                        "UnityEngine.Sprite", "UnityEngine.Localization.LocalizedString",
                                        "MonomiPark.SlimeRancher.UI.HUD.Notification.NotificationEntryStyle"},
                                       &err);
    if (style) p.styleDefault = R.Method(style, "get_Default", 0, &err);
    auto pending = R.FieldOffset(p.listClass, "_pendingNotifications", &err);
    std::optional<size_t> zoneName;
    if (p.zoneClass) zoneName = R.FieldOffset(p.zoneClass, "_localizedName", &err);
    const auto& a = api();
    p.objectNew = a.module ? reinterpret_cast<Il2CppObject* (*)(Il2CppClass*)>(
                                 reinterpret_cast<void*>(GetProcAddress(a.module, "il2cpp_object_new")))
                           : nullptr;
    if (!p.entryCtor || !p.styleDefault || !pending || !zoneName || !p.objectNew) {
        why = err.empty() ? "notification entry / LocalizedString source not found" : err;
        return false;
    }
    p.offPending = *pending;
    p.offZoneName = *zoneName;
    auto display = R.FieldOffset(p.listClass, "_displayEntries", nullptr);
    auto available = R.FieldOffset(p.listClass, "_availableDisplayCount", nullptr);
    p.offDisplay = display ? *display : 0;
    p.offAvailable = available ? *available : 0;
    p.gadgetClass = R.Class(kImage, "", "GadgetDefinition", nullptr);
    if (p.gadgetClass) {
        auto t = R.FieldOffset(p.gadgetClass, "Type", nullptr);
        auto i = R.FieldOffset(p.identClass, "icon", nullptr);
        if (t && i) {
            p.offGadgetType = *t;
            p.offIdentIcon = *i;
        } else {
            p.gadgetClass = nullptr;
        }
    }
    textReady_ = true;
    return true;
}

Il2CppObject* SaleNotices::TeleporterIcon() {
    if (!textReady_ || !impl_->gadgetClass) return nullptr;
    Impl& p = *impl_;
    if (p.teleIcon) {
        Il2CppObject* s = unity::GCHandleTarget(p.teleIcon);
        if (s && unity::IsAlive(s)) return s;
        unity::FreeGCHandle(p.teleIcon);
        p.teleIcon = nullptr;
    }
    const unsigned long long now = GetTickCount64();
    if (p.teleIconTried && now - p.teleIconTried < 30000) return nullptr;  // one asset scan per 30 s at most
    p.teleIconTried = now;
    Il2CppObject* best = nullptr;
    std::string bestName;
    for (Il2CppObject* g : unity::FindObjectsOfTypeAll(p.gadgetClass)) {
        if (!unity::IsAlive(g) || !p.IsA(g, p.gadgetClass)) continue;
        if (unity::Field<int>(g, p.offGadgetType) != 1) continue;  // GadgetDefinition.Types.TELEPORTER
        Il2CppObject* icon = unity::Field<Il2CppObject*>(g, p.offIdentIcon);
        if (!icon || !unity::IsAlive(icon)) continue;
        std::string name = unity::GetName(g);
        // prefer the plain home teleporter (blue) over colour variants, if present
        if (!best || name.find("Blue") != std::string::npos) {
            best = icon;
            bestName = name;
        }
        if (name.find("Blue") != std::string::npos) break;
    }
    if (!best) return nullptr;
    p.teleIcon = unity::NewGCHandle(best, false);
    SR2_INFO("[{}] text notices: teleporter icon '{}' (from {})", p.module, unity::GetName(best), bestName);
    return best;
}

bool SaleNotices::QueueText(Il2CppObject* icon, const std::string& text) {
    if (!textReady_ || text.empty()) return false;
    if (!sr2::core::text_override::SinkActive()) {
        if (!impl_->loggedNoSink) {
            impl_->loggedNoSink = true;
            SR2_INFO("[{}] text notices: the UI's text sink is off - notices only in the log", impl_->module);
        }
        return false;
    }
    if (impl_->texts.size() < kMaxQueued) impl_->texts.push_back({icon, text});
    impl_->waited = 0.0f;
    impl_->retry = 0.0f;
    if (TryFlushText(*impl_)) impl_->texts.clear();
    return true;
}

}  // namespace sr2::modules::custom_upgrades::ui
