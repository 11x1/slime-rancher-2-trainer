// custom_upgrades: HUD notices for the silo Sell-All (see sale_notify.h). SR2 1.3.0.

#include "modules/custom_upgrades/sale_notify.h"

#include <windows.h>

#include <cmath>
#include <format>
#include <initializer_list>
#include <string>
#include <vector>

#include "core/detour_guard.h"
#include "core/il2cpp_api.h"
#include "core/log.h"
#include "core/resolver.h"
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
    Il2CppClass* listClass = nullptr;      // ItemAcquisitionNotificationList
    Il2CppClass* providerClass = nullptr;  // DroneActivityNotificationProvider
    Il2CppClass* sceneClass = nullptr;     // DroneActivitySceneNotifications
    Il2CppClass* identClass = nullptr;     // IdentifiableType
    size_t offProviders = 0;               // ItemAcquisitionNotificationList._providers
    size_t offActive = 0;                  // DroneActivityNotificationProvider._activeNotifications
    const MethodInfo* canQueueAny = nullptr;   // ItemAcquisitionNotificationList.get_CanQueueAny
    const MethodInfo* handleAvail = nullptr;   // ItemAcquisitionNotificationList.HandleNotificationAvailable
    const MethodInfo* addPlortSold = nullptr;  // DroneActivitySceneNotifications.AddPlortSold(IdentifiableType, int)

    std::vector<std::pair<Il2CppObject*, int>> queue;
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
            SR2_INFO("[{}] sell-all notices: no HUD notification list with the drone provider (first active list {}, "
                     "providers [{}]; {} list(s) loaded, {} active) - text feedback instead",
                     module, unity::IsAlive(first) ? "found" : "none", providers, seen, active);
        }
        return nullptr;
    }
    Il2CppObject* Found(Il2CppObject* list, bool active, size_t seen) {
        if (!loggedScan) {
            loggedScan = true;
            SR2_INFO("[{}] sell-all notices: drone provider found on another HUD notification list ({}, list #{} of "
                     "the loaded ones)", module, active ? "active" : "inactive", seen);
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
            SR2_WARN("[{}] sell-all notices: HUD drone notification provider not found - {} notice(s) dropped", module,
                     queue.size());
            return true;
        }
        bool ok = false;
        Il2CppObject* boxed = Call(canQueueAny, list, {}, "ItemAcquisitionNotificationList.get_CanQueueAny", &ok);
        if (!ok) return true;
        if (!boxed || !*reinterpret_cast<bool*>(reinterpret_cast<char*>(boxed) + 2 * sizeof(void*)))
            return false;  // HUD suppressed (menu open): it would discard them
        int added = 0;
        for (auto& [type, count] : queue) {
            if (count <= 0 || !unity::IsAlive(type) || !IsA(type, identClass)) continue;
            int32_t n = count;
            Call(addPlortSold, scene, {type, &n}, "DroneActivitySceneNotifications.AddPlortSold", &ok);
            if (!ok) return true;
            ++added;
        }
        if (added) Call(handleAvail, list, {provider}, "ItemAcquisitionNotificationList.HandleNotificationAvailable", &ok);
        if (!loggedFirst && added) {
            loggedFirst = true;
            SR2_INFO("[{}] sell-all: {} plort notice(s) handed to the HUD (drone 'plorts sold' notifications)", module,
                     added);
        }
        return true;
    }
};

SaleNotices::SaleNotices() = default;
SaleNotices::~SaleNotices() { delete impl_; }

bool SaleNotices::Setup(Resolver& R, const char* module, std::string& why) {
    auto* p = new Impl();
    p->module = module;
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

void SaleNotices::Queue(const std::vector<std::pair<Il2CppObject*, int>>& sold) {
    if (!ready_ || sold.empty()) return;
    auto& q = impl_->queue;
    for (const auto& e : sold) {
        bool merged = false;
        for (auto& [type, count] : q)
            if (type == e.first) {
                count += e.second;
                merged = true;
                break;
            }
        if (!merged && q.size() < kMaxQueued) q.push_back(e);
    }
    impl_->waited = 0.0f;
    impl_->retry = 0.0f;
    if (impl_->TryFlush()) q.clear();
}

void SaleNotices::Frame(float dt) {
    if (!ready_ || impl_->queue.empty()) return;
    if (!std::isfinite(dt) || dt < 0.0f) dt = 0.0f;
    dt = dt > 1.0f ? 1.0f : dt;
    impl_->waited += dt;
    impl_->retry += dt;
    if (impl_->retry < kRetrySeconds) return;
    impl_->retry = 0.0f;
    if (impl_->TryFlush()) {
        impl_->queue.clear();
    } else if (impl_->waited > kMaxWaitSeconds) {
        SR2_INFO("[{}] sell-all notices: HUD not available for {:.0f} s - {} notice(s) dropped", impl_->module,
                 kMaxWaitSeconds, impl_->queue.size());
        impl_->queue.clear();
    }
}

void SaleNotices::Clear() {
    if (impl_) impl_->queue.clear();
}

}  // namespace sr2::modules::custom_upgrades::ui
