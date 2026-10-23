// custom_upgrades: coop world objects (see coop_world.h). SR2 1.3.0, all lookups by name.
//
// ---------------------------------------------------------------- Coop Air Net -------
//   Source: LookupDirector.GetPlotPrefab(CORRAL) -> AirNetUpgrader.AirNets (GameObject[]):
//   [0] "Base Air Net" (inactive child of the corral prefab), [1] "High Air Net". Each holds
//   one "NetTrigger": MeshRenderer (the net), AirNet (HitForceToDestroy 100, breaks under hard
//   impacts and recovers after 0.05 + 0.1 game hours) and a BoxCollider 9.5 x 0.6 x 9.5 on
//   layer 29 (the layer of the corral / coop barriers, which keep slimes and chickens in).
//   All plot prefabs share the 9.5 x 9.5 footprint; the base corral net sits at y 3.39..3.99,
//   the coop walls are lower (base coop barrier top y 2.0, high-wall coop 2.8).
//   We Instantiate "Base Air Net" under the coop plot (same local transform as in the corral)
//   and fit it to the coop: the net collider's bottom goes to the top of the coop's active
//   CoopRegion trigger (base coop 2.0, high walls 2.75), X/Z scale and centre to the region's
//   footprint. Refit every tick (cheap; only moved when off by > 2 cm), so buying High Walls
//   later lifts the net. Nothing of the AirNet itself is changed: it behaves like the corral
//   net (blocks what collides with layer 29 from above: chickens launched by slimes / vac,
//   flying things; breaks under hard hits and recovers).
//
// Clones are named "sr2mods_<kind>#<generation>" and found again every tick under the plot
// transform (no stored pointers). A clone of another generation (an older save game / before a
// load) is destroyed without looking at it. They are never part of the vanilla save: the game
// does not save plot children, and the clones are rebuilt from the sidecar after every load.

#include "modules/custom_upgrades/coop_world.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <format>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

#include "core/detour_guard.h"
#include "core/il2cpp_api.h"
#include "core/log.h"
#include "core/resolver.h"
#include "core/unity.h"

namespace sr2::modules::custom_upgrades::world {
namespace {

namespace unity = sr2::core::unity;
using sr2::il2cpp::api;
using sr2::il2cpp::Resolver;

constexpr const char* kImage = "Assembly-CSharp";
constexpr const char* kCore = "UnityEngine.CoreModule";
constexpr const char* kPhysics = "UnityEngine.PhysicsModule";
constexpr int32_t kPlotCorral = 2;
constexpr int32_t kPlotCoop = 3;
constexpr std::string_view kPrefix = "sr2mods_";
constexpr std::string_view kAirNetKind = "CoopAirNet";
constexpr float kFitTolerance = 0.02f;

struct Vec3 {
    float x, y, z;
};
struct Bounds {
    Vec3 center, extents;
};

}  // namespace

struct CoopWorld::Impl {
    const char* module = "custom_upgrades";

    Il2CppClass* plotClass = nullptr;
    Il2CppClass* coopRegionClass = nullptr;
    Il2CppClass* airNetUpgraderClass = nullptr;
    Il2CppClass* colliderClass = nullptr;
    Il2CppClass* gameObjectClass = nullptr;
    FieldInfo* allCoopsField = nullptr;
    using StaticGetFn = void (*)(FieldInfo*, void*);
    StaticGetFn staticGet = nullptr;
    size_t offPlotType = 0, offAirNets = 0;

    const MethodInfo* getPlotPrefab = nullptr;     // LookupDirector.GetPlotPrefab(Id)
    const MethodInfo* compInParent = nullptr;      // Component.GetComponentInParent(Type, bool)
    const MethodInfo* compInChildren = nullptr;    // Component.GetComponentInChildren(Type, bool)
    const MethodInfo* trChildCount = nullptr;      // Transform.get_childCount
    const MethodInfo* trGetChild = nullptr;        // Transform.GetChild(int)
    const MethodInfo* trGetPosition = nullptr;     // Transform.get_position
    const MethodInfo* trSetPosition = nullptr;     // Transform.set_position
    const MethodInfo* trGetLocalScale = nullptr;   // Transform.get_localScale
    const MethodInfo* trSetLocalScale = nullptr;   // Transform.set_localScale
    const MethodInfo* colBounds = nullptr;         // Collider.get_bounds
    const MethodInfo* colEnabled = nullptr;        // Collider.get_enabled
    const MethodInfo* objDestroy = nullptr;        // Object.Destroy(Object)
    const MethodInfo* syncTransforms = nullptr;    // Physics.SyncTransforms() (optional)

    bool IsA(Il2CppObject* obj, Il2CppClass* k) const {
        const auto& a = api();
        if (!obj || !k || !a.il2cpp_object_get_class) return false;
        Il2CppClass* c = a.il2cpp_object_get_class(obj);
        if (c == k) return true;
        return c && a.il2cpp_class_is_assignable_from && a.il2cpp_class_is_assignable_from(k, c);
    }

    Il2CppObject* Call(const MethodInfo* m, void* obj, std::initializer_list<void*> args, const char* what,
                       bool* ok = nullptr) {
        if (!m) {
            if (ok) *ok = false;
            return nullptr;
        }
        std::vector<void*> params(args);
        std::string err;
        Il2CppObject* r = Resolver::Get().Invoke(m, obj, params.empty() ? nullptr : params.data(), &err);
        const bool good = err.empty();
        if (ok) *ok = good;
        if (!good) {
            if (err.rfind("native exception", 0) == 0)
                sr2::core::MarkModuleFaulted(module, std::format("{}: {}", what, err));
            else SR2_WARN("[{}] {} failed: {}", module, what, err);
        }
        return r;
    }
    template <class T>
    T Value(Il2CppObject* boxed, T def = T{}) const {
        return boxed ? *reinterpret_cast<T*>(reinterpret_cast<char*>(boxed) + 2 * sizeof(void*)) : def;
    }

    // ---- small game helpers ----
    Il2CppObject* InParent(Il2CppObject* comp, Il2CppClass* k) {
        bool inactive = true;
        bool ok = false;
        Il2CppObject* r = Call(compInParent, comp, {unity::TypeOf(k), &inactive}, "GetComponentInParent", &ok);
        return ok && unity::IsAlive(r) && IsA(r, k) ? r : nullptr;
    }
    Il2CppObject* InChildren(Il2CppObject* comp, Il2CppClass* k, bool inactive) {
        bool ok = false;
        Il2CppObject* r = Call(compInChildren, comp, {unity::TypeOf(k), &inactive}, "GetComponentInChildren", &ok);
        return ok && unity::IsAlive(r) && IsA(r, k) ? r : nullptr;
    }
    bool ColliderBounds(Il2CppObject* col, Bounds& out) {
        if (!unity::IsAlive(col) || !IsA(col, colliderClass)) return false;
        bool ok = false;
        if (!Value<bool>(Call(colEnabled, col, {}, "Collider.get_enabled", &ok)) || !ok) return false;
        Il2CppObject* b = Call(colBounds, col, {}, "Collider.get_bounds", &ok);
        if (!ok || !b) return false;
        out = Value<Bounds>(b);
        return std::isfinite(out.center.y) && out.extents.x > 0.01f && out.extents.z > 0.01f;
    }
    Vec3 GetVec(const MethodInfo* m, Il2CppObject* tr, const char* what) {
        bool ok = false;
        Il2CppObject* b = Call(m, tr, {}, what, &ok);
        return ok && b ? Value<Vec3>(b) : Vec3{0, 0, 0};
    }
    void SetVec(const MethodInfo* m, Il2CppObject* tr, Vec3 v, const char* what) { Call(m, tr, {&v}, what); }
    void Destroy(Il2CppObject* go) {
        if (unity::IsAlive(go)) Call(objDestroy, nullptr, {go}, "Object.Destroy");
    }
    void Sync() {
        if (syncTransforms) Call(syncTransforms, nullptr, {}, "Physics.SyncTransforms");
    }

    struct CoopInfo {
        Il2CppObject* plot = nullptr;
        bool hasRegion = false;
        Bounds region{};  // the active CoopRegion trigger with the highest top
    };
    std::vector<CoopInfo> Coops();

    struct Children {
        Il2CppObject* airNet = nullptr;  // current-generation clones (GameObjects)
    };
    Children Scan(Il2CppObject* plot, unsigned generation, bool destroyStale, bool destroyAll = false);

    Il2CppObject* AirNetSource(Il2CppObject* lookup);
    void CreateAirNet(Il2CppObject* plot, const CoopInfo& c, Il2CppObject* lookup, unsigned generation);
    void FitAirNet(Il2CppObject* clone, const CoopInfo& c, bool log);
    bool loggedNoSource = false;
};

// ---------------------------------------------------------------- setup --

bool CoopWorld::Setup(Resolver& R, const char* module, std::string& why) {
    auto* p = new Impl();
    p->module = module;
    auto parentOf = [](Il2CppClass* c) { return api().il2cpp_class_get_parent ? api().il2cpp_class_get_parent(c) : nullptr; };
    auto cls = [&](const char* image, const char* ns, const char* name) -> Il2CppClass* {
        std::string err;
        Il2CppClass* k = R.Class(image, ns, name, &err);
        if (!k && why.empty()) why = err.empty() ? std::format("class {}.{} not found", ns, name) : err;
        return k;
    };
    auto off = [&](Il2CppClass* k, const char* field, size_t& out) {
        std::string err;
        for (Il2CppClass* c = k; c; c = parentOf(c))
            if (auto o = R.FieldOffset(c, field, &err)) {
                out = *o;
                return;
            }
        if (why.empty()) why = std::format("field {} not found", field);
    };
    auto method = [&](Il2CppClass* k, const char* name, int argc) -> const MethodInfo* {
        std::string err;
        for (Il2CppClass* c = k; c; c = parentOf(c))
            if (const MethodInfo* m = R.Method(c, name, argc, &err)) return m;
        if (why.empty()) why = std::format("method {}({}) not found", name, argc);
        return nullptr;
    };
    auto byParams = [&](Il2CppClass* k, const char* name, std::initializer_list<std::string_view> params) {
        std::string err;
        const MethodInfo* m = R.MethodByParams(k, name, params, &err);
        if (!m && why.empty()) why = std::format("{}: {}", name, err);
        return m;
    };

    p->staticGet = reinterpret_cast<Impl::StaticGetFn>(
        api().module ? GetProcAddress(api().module, "il2cpp_field_static_get_value") : nullptr);
    if (!p->staticGet) why = "il2cpp_field_static_get_value export missing";

    p->plotClass = cls(kImage, "", "LandPlot");
    p->coopRegionClass = cls(kImage, "", "CoopRegion");
    p->airNetUpgraderClass = cls(kImage, "", "AirNetUpgrader");
    Il2CppClass* lookup = cls(kImage, "", "LookupDirector");
    p->colliderClass = cls(kPhysics, "UnityEngine", "Collider");
    Il2CppClass* physics = cls(kPhysics, "UnityEngine", "Physics");
    Il2CppClass* component = cls(kCore, "UnityEngine", "Component");
    Il2CppClass* transform = cls(kCore, "UnityEngine", "Transform");
    Il2CppClass* uobject = cls(kCore, "UnityEngine", "Object");
    p->gameObjectClass = cls(kCore, "UnityEngine", "GameObject");
    if (!why.empty()) {
        delete p;
        return false;
    }
    {
        std::string err;
        p->allCoopsField = R.Field(p->coopRegionClass, "_allCoops", &err);
        if (!p->allCoopsField && why.empty()) why = "CoopRegion._allCoops: " + err;
    }
    off(p->plotClass, "TypeId", p->offPlotType);
    off(p->airNetUpgraderClass, "AirNets", p->offAirNets);
    p->getPlotPrefab = method(lookup, "GetPlotPrefab", 1);
    p->compInParent = byParams(component, "GetComponentInParent", {"System.Type", "System.Boolean"});
    p->compInChildren = byParams(component, "GetComponentInChildren", {"System.Type", "System.Boolean"});
    p->trChildCount = method(transform, "get_childCount", 0);
    p->trGetChild = method(transform, "GetChild", 1);
    p->trGetPosition = method(transform, "get_position", 0);
    p->trSetPosition = method(transform, "set_position", 1);
    p->trGetLocalScale = method(transform, "get_localScale", 0);
    p->trSetLocalScale = method(transform, "set_localScale", 1);
    p->colBounds = method(p->colliderClass, "get_bounds", 0);
    p->colEnabled = method(p->colliderClass, "get_enabled", 0);
    p->objDestroy = byParams(uobject, "Destroy", {"UnityEngine.Object"});
    if (!why.empty()) {
        delete p;
        return false;
    }
    {
        std::string err;
        p->syncTransforms = R.Method(physics, "SyncTransforms", 0, &err);  // optional
    }
    impl_ = p;
    ready_ = true;
    return true;
}

// ---------------------------------------------------------------- helpers --

std::vector<CoopWorld::Impl::CoopInfo> CoopWorld::Impl::Coops() {
    std::vector<CoopInfo> out;
    void* list = nullptr;
    staticGet(allCoopsField, &list);
    if (!list) return out;
    for (size_t i = 0, n = unity::ListCount(list); i < n && i < 64; ++i) {
        auto* region = static_cast<Il2CppObject*>(unity::ListAt(list, i));
        if (!unity::IsAlive(region) || !IsA(region, coopRegionClass)) continue;
        Il2CppObject* plot = InParent(region, plotClass);
        if (!plot || unity::Field<int32_t>(plot, offPlotType) != kPlotCoop) continue;
        auto it = std::find_if(out.begin(), out.end(), [&](const CoopInfo& c) { return c.plot == plot; });
        if (it == out.end()) {
            out.push_back({plot, false, {}});
            it = out.end() - 1;
        }
        Il2CppObject* go = unity::GameObjectOf(region);
        if (!go || !unity::ActiveInHierarchy(go)) continue;
        Bounds b{};
        if (!ColliderBounds(unity::GetComponent(region, colliderClass), b)) continue;
        if (!it->hasRegion || b.center.y + b.extents.y > it->region.center.y + it->region.extents.y) {
            it->region = b;
            it->hasRegion = true;
        }
    }
    return out;
}

CoopWorld::Impl::Children CoopWorld::Impl::Scan(Il2CppObject* plot, unsigned generation, bool destroyStale,
                                               bool destroyAll) {
    Children c;
    Il2CppObject* tr = unity::TransformOf(plot);
    if (!unity::IsAlive(tr)) return c;
    bool ok = false;
    const int n = Value<int32_t>(Call(trChildCount, tr, {}, "Transform.get_childCount", &ok));
    if (!ok) return c;
    const std::string suffix = std::format("#{}", generation);
    std::vector<Il2CppObject*> doomed;
    for (int32_t i = 0; i < n && i < 128; ++i) {
        Il2CppObject* child = Call(trGetChild, tr, {&i}, "Transform.GetChild", &ok);
        if (!ok || !unity::IsAlive(child)) continue;
        Il2CppObject* go = unity::GameObjectOf(child);
        if (!go) continue;
        const std::string name = unity::GetName(go);
        if (name.rfind(kPrefix, 0) != 0) continue;
        const bool current = name.size() > suffix.size() && name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0;
        if (destroyAll || !current) {
            if (destroyAll || destroyStale) doomed.push_back(go);
            continue;
        }
        const std::string_view kind = std::string_view(name).substr(kPrefix.size(), name.size() - kPrefix.size() - suffix.size());
        if (kind == kAirNetKind && !c.airNet) c.airNet = go;
        else doomed.push_back(go);  // duplicate / unknown: never keep two
    }
    for (Il2CppObject* go : doomed) Destroy(go);
    return c;
}

Il2CppObject* CoopWorld::Impl::AirNetSource(Il2CppObject* lookup) {
    if (!unity::IsAlive(lookup)) return nullptr;
    int32_t id = kPlotCorral;
    bool ok = false;
    Il2CppObject* prefab = Call(getPlotPrefab, lookup, {&id}, "LookupDirector.GetPlotPrefab", &ok);
    if (!ok || !unity::IsAlive(prefab) || !IsA(prefab, gameObjectClass)) return nullptr;
    Il2CppObject* up = unity::GetComponent(prefab, airNetUpgraderClass);
    if (!unity::IsAlive(up) || !IsA(up, airNetUpgraderClass)) return nullptr;
    void* nets = unity::Field<void*>(up, offAirNets);
    Il2CppObject* pick = nullptr;
    for (size_t i = 0, n = unity::ArrayLength(nets); i < n && i < 8; ++i) {
        auto* go = static_cast<Il2CppObject*>(unity::ArrayAt(nets, i));
        if (!unity::IsAlive(go) || !IsA(go, gameObjectClass)) continue;
        if (!pick) pick = go;  // [0] = "Base Air Net"
        if (unity::GetName(go).find("Base") != std::string::npos) {
            pick = go;
            break;
        }
    }
    return pick;
}

void CoopWorld::Impl::FitAirNet(Il2CppObject* clone, const CoopInfo& c, bool log) {
    if (!c.hasRegion) return;
    Il2CppObject* tr = unity::TransformOf(clone);
    Il2CppObject* col = unity::IsAlive(tr) ? InChildren(tr, colliderClass, false) : nullptr;
    Bounds net{};
    if (!col || !ColliderBounds(col, net)) return;  // broken net (collider off) -> refit later
    const Bounds& r = c.region;
    bool moved = false;
    // Footprint: uniform X/Z scale around the clone root (the plot centre).
    const float sx = r.extents.x / net.extents.x, sz = r.extents.z / net.extents.z;
    const float s = 0.5f * (sx + sz);
    if (std::isfinite(s) && std::fabs(s - 1.0f) > kFitTolerance && s > 0.5f && s < 2.0f) {
        Vec3 ls = GetVec(trGetLocalScale, tr, "Transform.get_localScale");
        ls.x *= s;
        ls.z *= s;
        SetVec(trSetLocalScale, tr, ls, "Transform.set_localScale");
        Sync();
        if (!ColliderBounds(col, net)) return;
        moved = true;
    }
    // Height: net bottom on the coop region's top; centre on the region's centre.
    const float dx = r.center.x - net.center.x;
    const float dy = (r.center.y + r.extents.y) - (net.center.y - net.extents.y);
    const float dz = r.center.z - net.center.z;
    if (std::fabs(dx) > kFitTolerance || std::fabs(dy) > kFitTolerance || std::fabs(dz) > kFitTolerance) {
        if (std::fabs(dx) > 3.0f || std::fabs(dz) > 3.0f || std::fabs(dy) > 6.0f) {
            SR2_WARN("[{}] coop air net: fit offset ({:.2f}, {:.2f}, {:.2f}) too large - left unfitted", module, dx, dy, dz);
            return;
        }
        Vec3 pos = GetVec(trGetPosition, tr, "Transform.get_position");
        pos.x += dx;
        pos.y += dy;
        pos.z += dz;
        SetVec(trSetPosition, tr, pos, "Transform.set_position");
        Sync();
        moved = true;
    }
    if (moved || log)
        SR2_INFO("[{}] coop air net fitted: region top {:.2f}, footprint {:.2f} x {:.2f} (net scale x{:.3f}, moved "
                 "{:.2f}, {:.2f}, {:.2f})",
                 module, r.center.y + r.extents.y, 2 * r.extents.x, 2 * r.extents.z, s, dx, dy, dz);
}

void CoopWorld::Impl::CreateAirNet(Il2CppObject* plot, const CoopInfo& c, Il2CppObject* lookup, unsigned generation) {
    Il2CppObject* src = AirNetSource(lookup);
    if (!src) {
        if (!loggedNoSource) SR2_WARN("[{}] coop air net: corral prefab / AirNetUpgrader.AirNets not found", module);
        loggedNoSource = true;
        return;
    }
    Il2CppObject* clone = unity::Instantiate(src, unity::TransformOf(plot));
    if (!unity::IsAlive(clone) || !IsA(clone, gameObjectClass)) {
        SR2_WARN("[{}] coop air net: Instantiate failed", module);
        return;
    }
    unity::SetName(clone, std::format("{}{}#{}", kPrefix, kAirNetKind, generation));
    unity::SetActive(clone, true);  // AirNet.Awake: collider, material, TimeDirector
    Sync();
    SR2_INFO("[{}] coop air net created from corral '{}'", module, unity::GetName(src));
    FitAirNet(clone, c, true);
}

// ---------------------------------------------------------------- api --

void CoopWorld::Tick(const QueryFn& query, const LookupFn& lookup) {
    if (!ready_ || !impl_) return;
    Impl& p = *impl_;
    Il2CppObject* lookupDirector = nullptr;
    bool lookupTried = false;
    auto getLookup = [&] {
        if (!lookupTried) {
            lookupTried = true;
            lookupDirector = lookup();
        }
        return lookupDirector;
    };
    for (const Impl::CoopInfo& c : p.Coops()) {
        CoopRequest req;
        const bool known = query(c.plot, req);
        Impl::Children have = p.Scan(c.plot, generation_, true);
        if (!known) continue;
        if (req.airNet && !have.airNet) p.CreateAirNet(c.plot, c, getLookup(), generation_);
        else if (!req.airNet && have.airNet) {
            p.Destroy(have.airNet);
            SR2_INFO("[{}] coop air net removed (not owned)", p.module);
        } else if (have.airNet) p.FitAirNet(have.airNet, c, false);
        if (sr2::core::ModuleFaulted(p.module)) return;
    }
}

void CoopWorld::RemoveAll() {
    if (!ready_ || !impl_) return;
    for (const Impl::CoopInfo& c : impl_->Coops()) impl_->Scan(c.plot, generation_, true, true);
}

void CoopWorld::Remove(Il2CppObject* plot, bool airNet) {
    if (!ready_ || !impl_ || !unity::IsAlive(plot)) return;
    Impl::Children have = impl_->Scan(plot, generation_, true);
    if (airNet && have.airNet) {
        impl_->Destroy(have.airNet);
        SR2_INFO("[{}] coop air net removed (sold)", impl_->module);
    }
}

}  // namespace sr2::modules::custom_upgrades::world
