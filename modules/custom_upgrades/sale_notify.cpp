// custom_upgrades: HUD notices for the silo Sell-All (see sale_notify.h). SR2 1.3.0.

#include "modules/custom_upgrades/sale_notify.h"

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
    // The HUD list and its drone provider (nullptr if the HUD is not up).
    Il2CppObject* List() {
        Il2CppObject* list = unity::FindObjectOfType(listClass);
        return unity::IsAlive(list) && IsA(list, listClass) ? list : nullptr;
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
        Il2CppObject* list = List();
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
    Il2CppObject* list = impl_->List();
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
