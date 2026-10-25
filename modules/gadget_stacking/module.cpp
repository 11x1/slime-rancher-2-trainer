// gadget_stacking: gadget mode places decorations and gadgets on top of each other (a lamp on a
// pillar), using the game's own stacking system. Default on (Misc > General card). Details and the
// RE in NOTES.md, re/fabricator.md "Gadget stacking".
//
// The game already stacks (SR2 1.3.0, decompiled in IDA):
//   * GadgetItem.UpdateFootprint: of the footprint ray's hits, a hit on a placed gadget (CanBePickedUp,
//     enabled) is only eligible as the surface when that gadget has AllowGadgetStackingOnTop (instance
//     field, false on nearly every prefab) and a gadget is held; otherwise the ray goes through it. Any
//     hit gadget with the flag becomes _baseGadget (not only the chosen hit's: ray order decides).
//   * IsPlacementValid: with a _baseGadget the placeholder needs CanStackOnStackableGadgets (Gadget ctor
//     default true, copied from the prefab by CopyGadgetComponents), then distance, slope and
//     IsCompletelyGrounded (down rays from the bounds' corners).
//   * _isPlacementBlocked = Gadget.IsOverlapping(...) -> GadgetOverlapHelpers.IsGadgetOverlapping:
//     an overlap query with the placeholder's Bounds; GadgetOverlapInfo keeps the blocking colliders.
//   * PlaceGadgetEvent: _baseGadget.AddStackedGadget(new) -> GadgetModel._stackedGadgetsIds, saved as
//     PlacedGadgetV07.StackedGadgetIds and relinked after a load (Gadget.ProcessPendingStackedGadgets);
//     the position and full rotation are saved like any gadget's. PickupGadget ->
//     PickupOrStoreStackedGadgets picks up the whole stack above the gadget, recursively.
//
// What this module does:
//   * Gadget.SetModel (every placed or loaded gadget) sets AllowGadgetStackingOnTop. Turning the module
//     on sweeps the gadgets already in the scene once, turning it off restores the ones it changed.
//   * GadgetItem.IsPlacementValid (called inside UpdateFootprint after the ray): sets the placeholder's
//     CanStackOnStackableGadgets, and sets _baseGadget to the gadget of the CHOSEN hit (or null), so a
//     gadget is linked to the one it stands on and not to one the ray passed through. On a stack target
//     the grounded test is skipped (a lamp on a narrow pillar top); distance and slope stay. A trigger
//     collider of a gadget as the hit surface is refused (it is no surface).
//   * GadgetOverlapHelpers.IsGadgetOverlapping (only the placement test of the held placeholder, same
//     tick, the metadata's GadgetOverlapLayers): when every blocking collider belongs to another placed
//     gadget, not blocked. Plots, walls, slimes and the terrain still block.
//   * PickupOrStoreStackedGadgets: logs when a gadget with others on top is picked up or stored; the
//     vanilla rule stays (the stacked gadgets go back with it).
// Free Placement owns Gadget.IsOverlapping / IsCompletelyGrounded / UpdateFootprint; this module hooks
// only the static helper and IsPlacementValid below them, so both work together.

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <format>
#include <memory>
#include <string>
#include <unordered_set>

#include "core/detour_guard.h"
#include "core/il2cpp_api.h"
#include "core/log.h"
#include "core/main_thread.h"
#include "core/module.h"
#include "core/unity.h"

namespace sr2::modules {
namespace {

namespace unity = sr2::core::unity;
namespace main_thread = sr2::core::main_thread;
using sr2::il2cpp::api;

constexpr const char* kModule = "gadget_stacking";
constexpr const char* kImage = "Assembly-CSharp";
constexpr const char* kItemsNs = "MonomiPark.SlimeRancher.Player.PlayerItems";
constexpr const char* kWorldNs = "MonomiPark.SlimeRancher.World";
constexpr float kDegToRad = 0.017453292f;

struct Vec3 {
    float x, y, z;
};
struct Ray {  // UnityEngine.Ray (passed by pointer)
    Vec3 origin, direction;
};
struct Hit {  // UnityEngine.RaycastHit (passed by pointer); only the leading fields are read
    Vec3 point, normal;
};
struct OverlapInfo {  // GadgetOverlapInfo (ref)
    Il2CppObject* colliders;  // Collider[]
    int32_t count;
};

class GadgetStacking final : public sr2::core::Module {
public:
    const char* Name() const override { return kModule; }
    bool Setup(sr2::core::ModuleContext& ctx) override;
    void OnEnable() override;
    void OnDisable() override;
    bool Enabled() const { return enabled_.load(std::memory_order_relaxed); }

    // Offsets (resolved by name in Setup).
    size_t offMeta = 0;           // GadgetItem._gadgetItemMetadata
    size_t offPlaceholderG = 0;   // GadgetItem._gadgetPlaceholderInstanceGadget
    size_t offBase = 0;           // GadgetItem._baseGadget
    size_t offMaxDist = 0;        // GadgetItemMetadata.MaxValidPlacementDist
    size_t offMaxSlope = 0;       // GadgetItemMetadata.MaxValidPlacementSlope
    size_t offOverlapLayers = 0;  // GadgetItemMetadata.GadgetOverlapLayers (LayerMask = int)
    size_t offPickable = 0;       // Gadget.CanBePickedUp
    size_t offStackTop = 0;       // Gadget.AllowGadgetStackingOnTop
    size_t offCanStack = 0;       // Gadget.CanStackOnStackableGadgets
    size_t offModel = 0;          // Gadget._model
    size_t offStacked = 0;        // Gadget._stackedGadgets (List<Gadget>)
    Il2CppClass* itemClass = nullptr;
    Il2CppClass* metaClass = nullptr;
    Il2CppClass* gadgetClass = nullptr;

    using ColliderFn = Il2CppObject* (*)(const Hit*, const MethodInfo*);           // RaycastHit.get_collider
    using InParentFn = Il2CppObject* (*)(Il2CppObject*, Il2CppObject*, bool, const MethodInfo*);  // Component.GetComponentInParent(Type, bool)
    using BoolGetFn = bool (*)(Il2CppObject*, const MethodInfo*);                  // Behaviour.get_enabled, Collider.get_isTrigger
    using WBarrierFn = void (*)(Il2CppObject*, void**, void*);
    ColliderFn getCollider = nullptr;
    const MethodInfo* getColliderMi = nullptr;
    InParentFn inParent = nullptr;
    const MethodInfo* inParentMi = nullptr;
    BoolGetFn getEnabled = nullptr;
    const MethodInfo* getEnabledMi = nullptr;
    BoolGetFn isTrigger = nullptr;
    const MethodInfo* isTriggerMi = nullptr;
    WBarrierFn wbarrier = nullptr;

    // Main thread only.
    std::unordered_set<const void*> flipped;  // gadgets whose AllowGadgetStackingOnTop this module set (identity only)
    const void* armedTransform = nullptr;     // placeholder transform of the running placement test
    int armedMask = 0;
    unsigned long long armedTick = 0;
    const void* lastBase = nullptr;           // last stack target (log on change)
    bool loggedRelax = false;
    std::unordered_set<std::string> loggedTriggers;

    Il2CppObject* GadgetOf(Il2CppObject* collider) const {
        if (!collider || !inParent || !unity::IsAlive(collider)) return nullptr;
        return inParent(collider, unity::TypeOf(gadgetClass), false, inParentMi);
    }
    bool Placed(Il2CppObject* gadget) const {
        return gadget && unity::IsAlive(gadget) && unity::Field<void*>(gadget, offModel) != nullptr;
    }

private:
    std::atomic<bool> enabled_{false};
};

GadgetStacking* g_self = nullptr;

bool Active() { return g_self && g_self->Enabled(); }

bool IsA(Il2CppObject* obj, Il2CppClass* k) {
    const auto& a = api();
    if (!obj || !k || !a.il2cpp_object_get_class) return false;
    Il2CppClass* c = a.il2cpp_object_get_class(obj);
    if (c == k) return true;
    return c && a.il2cpp_class_is_assignable_from && a.il2cpp_class_is_assignable_from(k, c);
}

// One FindObjectsOfTypeAll per switch (never per frame): turning on marks the placed gadgets already
// in the scene, turning off restores the ones this module marked. Prefabs (no model) are skipped.
void Sweep(bool on) {
    if (!g_self) return;
    GadgetStacking& s = *g_self;
    if (s.Enabled() != on) return;  // switched again before this ran
    int changed = 0;
    for (Il2CppObject* g : unity::FindObjectsOfTypeAll(s.gadgetClass)) {
        if (!s.Placed(g)) continue;
        bool& top = unity::Field<bool>(g, s.offStackTop);
        if (on && !top) {
            top = true;
            s.flipped.insert(g);
            ++changed;
        } else if (!on && top && s.flipped.count(g)) {
            top = false;
            ++changed;
        }
    }
    if (!on) s.flipped.clear();
    s.armedTransform = nullptr;
    s.lastBase = nullptr;
    SR2_INFO("[{}] {}: {} placed gadget(s) {}", kModule, on ? "on" : "off", changed,
             on ? "now take gadgets on top" : "restored to the game's stacking rule");
}

void GadgetStacking::OnEnable() {
    enabled_.store(true);
    main_thread::Post([] { Sweep(true); });
}

void GadgetStacking::OnDisable() {
    enabled_.store(false);
    main_thread::Post([] { Sweep(false); });
}

// ---------------------------------------------------------------- hooks --

sr2::core::DetourGuard g_modelGuard{kModule, "Gadget.SetModel"};
sr2::core::DetourGuard g_validGuard{kModule, "GadgetItem.IsPlacementValid"};
sr2::core::DetourGuard g_overlapGuard{kModule, "GadgetOverlapHelpers.IsGadgetOverlapping"};
sr2::core::DetourGuard g_pickupGuard{kModule, "GadgetItem.PickupOrStoreStackedGadgets"};

using SetModelFn = void (*)(Il2CppObject*, Il2CppObject*, const MethodInfo*);
using ValidFn = bool (*)(Il2CppObject*, const Ray*, const Hit*, const MethodInfo*);
using OverlapFn = bool (*)(Il2CppObject*, Il2CppObject*, Il2CppObject*, OverlapInfo*, float, int32_t, bool,
                           const MethodInfo*);
using PickupFn = void (*)(Il2CppObject*, Il2CppObject*, bool, const MethodInfo*);
SetModelFn g_origSetModel = nullptr;
ValidFn g_origValid = nullptr;
OverlapFn g_origOverlap = nullptr;
PickupFn g_origPickup = nullptr;

// Every gadget that gets a model (placed now or loaded from the save) accepts gadgets on top.
void SetModelDetour(Il2CppObject* self, Il2CppObject* model, const MethodInfo* mi) {
    g_origSetModel(self, model, mi);
    if (!self || !model || !Active() || !main_thread::IsMainThread()) return;
    g_modelGuard.Run([&] {
        GadgetStacking& s = *g_self;
        bool& top = unity::Field<bool>(self, s.offStackTop);
        if (top) {
            s.flipped.erase(self);  // stackable in the game itself (or a reused address)
            return;
        }
        top = true;
        s.flipped.insert(self);
    });
}

bool ValidDetour(Il2CppObject* self, const Ray* ray, const Hit* hit, const MethodInfo* mi) {
    if (!self || !ray || !hit || !Active() || !main_thread::IsMainThread()) return g_origValid(self, ray, hit, mi);
    GadgetStacking& s = *g_self;
    Il2CppObject* base = nullptr;
    char* meta = nullptr;
    bool refuse = false;
    g_validGuard.Run([&] {
        if (!IsA(self, s.itemClass)) return;
        auto* m = unity::Field<Il2CppObject*>(self, s.offMeta);
        if (!IsA(m, s.metaClass)) return;
        meta = reinterpret_cast<char*>(m);
        auto* ph = unity::Field<Il2CppObject*>(self, s.offPlaceholderG);
        if (ph && unity::IsAlive(ph)) {
            unity::Field<bool>(ph, s.offCanStack) = true;  // a fresh copy per held gadget (SetHeldGadget)
            s.armedTransform = unity::TransformOf(ph);
            s.armedMask = unity::Field<int32_t>(meta, s.offOverlapLayers);
            s.armedTick = main_thread::TickCount();
        }
        // The stack target is the gadget of the chosen hit, as the game's ray loop means it.
        Il2CppObject* col = s.getCollider(hit, s.getColliderMi);
        Il2CppObject* g = s.GadgetOf(col);
        if (g && s.Placed(g) && g != ph && unity::Field<bool>(g, s.offPickable) &&
            unity::Field<bool>(g, s.offStackTop) && s.getEnabled(g, s.getEnabledMi)) {
            if (s.isTrigger(col, s.isTriggerMi)) {
                refuse = true;
                std::string name = unity::GetName(g);
                if (s.loggedTriggers.insert(name).second)
                    SR2_INFO("[{}] the outline hit a trigger volume of '{}' ('{}'), not a surface: no stacking there",
                             kModule, name, unity::GetName(col));
            } else {
                base = g;
            }
        }
        void** slot = reinterpret_cast<void**>(reinterpret_cast<char*>(self) + s.offBase);
        if (*slot != base) {
            if (base) s.wbarrier(self, slot, base);
            else *slot = nullptr;  // null store: no write barrier needed
        }
        if (base != s.lastBase) {
            s.lastBase = base;
            if (base) SR2_DEBUG("[{}] stack target '{}'", kModule, unity::GetName(base));
        }
    });

    bool r = g_origValid(self, ray, hit, mi);
    if (refuse) return false;
    if (r || !base || !meta) return r;

    // On a gadget: the game refused for distance, slope or "not completely grounded" (corners of the
    // bounds over the edge of a small top). Keep distance and slope, skip the grounded test.
    g_validGuard.Run([&] {
        const float dx = ray->origin.x - hit->point.x, dy = ray->origin.y - hit->point.y,
                    dz = ray->origin.z - hit->point.z;
        if (std::sqrt(dx * dx + dy * dy + dz * dz) > unity::Field<float>(meta, s.offMaxDist)) return;
        if (std::sin(unity::Field<float>(meta, s.offMaxSlope) * kDegToRad) > hit->normal.y) return;
        r = true;
    });
    return r;
}

// Only the placement overlap test of the held placeholder (armed by ValidDetour in the same tick, the
// metadata's mask): other gadgets in the way don't block. The blockers are what the last overlap query
// left in overlapInfo (box / sphere / capsule / slime-space queries: exactly the blockers; convex mesh:
// the candidates, a superset), so "all of them are placed gadgets" never hides anything else.
bool OverlapDetour(Il2CppObject* tr, Il2CppObject* bounds, Il2CppObject* ident, OverlapInfo* info, float floorTol,
                   int32_t mask, bool skipIgnored, const MethodInfo* mi) {
    bool r = g_origOverlap(tr, bounds, ident, info, floorTol, mask, skipIgnored, mi);
    if (!r || !tr || !info || !Active() || !main_thread::IsMainThread()) return r;
    GadgetStacking& s = *g_self;
    if (tr != s.armedTransform || mask != s.armedMask || s.armedTick != main_thread::TickCount()) return r;
    g_overlapGuard.Run([&] {
        s.armedTransform = nullptr;  // one use
        const int n = info->count;
        if (n <= 0 || !info->colliders || static_cast<size_t>(n) > unity::ArrayLength(info->colliders)) return;
        Il2CppObject* first = nullptr;
        for (int i = 0; i < n; ++i) {
            auto* col = static_cast<Il2CppObject*>(unity::ArrayAt(info->colliders, static_cast<size_t>(i)));
            Il2CppObject* g = s.GadgetOf(col);
            if (!s.Placed(g) || unity::TransformOf(g) == tr) return;  // not a placed gadget: still blocked
            if (!first) first = g;
        }
        r = false;
        if (!s.loggedRelax) {
            s.loggedRelax = true;
            SR2_INFO("[{}] overlap with placed gadgets ignored (first: '{}'); other obstacles still block", kModule,
                     unity::GetName(first));
        }
    });
    return r;
}

void PickupDetour(Il2CppObject* self, Il2CppObject* gadget, bool isRoot, const MethodInfo* mi) {
    if (gadget && isRoot && Active() && main_thread::IsMainThread())
        g_pickupGuard.Run([&] {
            auto* list = unity::Field<void*>(gadget, g_self->offStacked);
            const size_t n = list ? unity::ListCount(list) : 0;
            if (n > 0)
                SR2_INFO("[{}] '{}' picked up or stored with {} gadget(s) stacked on it: they go back with it "
                         "(the game's stacking rule)",
                         kModule, unity::GetName(gadget), n);
        });
    g_origPickup(self, gadget, isRoot, mi);
}

// ---------------------------------------------------------------- setup --

bool GadgetStacking::Setup(sr2::core::ModuleContext& ctx) {
    auto& R = ctx.resolver();
    std::string why;
    auto cls = [&](const char* image, const char* ns, const char* name) -> Il2CppClass* {
        std::string err;
        Il2CppClass* k = R.Class(image, ns, name, &err);
        if (!k && why.empty()) why = err.empty() ? std::format("class {}.{} not found", ns, name) : err;
        return k;
    };
    auto off = [&](Il2CppClass* k, const char* field, size_t& out) -> bool {
        if (!k) return false;
        std::string err;
        auto o = R.FieldOffset(k, field, &err);
        if (!o) {
            if (why.empty()) why = err.empty() ? std::format("field {} not found", field) : err;
            return false;
        }
        out = *o;
        return true;
    };
    auto ptr = [&](const MethodInfo* m, const char* what) -> void* {
        if (!m) {
            if (why.empty()) why = std::format("{} not found", what);
            return nullptr;
        }
        std::string err;
        void* p = R.MethodPointer(m, &err);
        if (!p && why.empty()) why = std::format("{} has no code: {}", what, err);
        return p;
    };

    itemClass = cls(kImage, kItemsNs, "GadgetItem");
    metaClass = cls(kImage, kItemsNs, "GadgetItemMetadata");
    gadgetClass = cls(kImage, kWorldNs, "Gadget");
    Il2CppClass* helpers = cls(kImage, kWorldNs, "GadgetOverlapHelpers");
    Il2CppClass* hitClass = cls("UnityEngine.PhysicsModule", "UnityEngine", "RaycastHit");
    Il2CppClass* colliderClass = cls("UnityEngine.PhysicsModule", "UnityEngine", "Collider");
    bool ok = off(itemClass, "_gadgetItemMetadata", offMeta) &&
              off(itemClass, "_gadgetPlaceholderInstanceGadget", offPlaceholderG) &&
              off(itemClass, "_baseGadget", offBase) && off(metaClass, "MaxValidPlacementDist", offMaxDist) &&
              off(metaClass, "MaxValidPlacementSlope", offMaxSlope) &&
              off(metaClass, "GadgetOverlapLayers", offOverlapLayers) && off(gadgetClass, "CanBePickedUp", offPickable) &&
              off(gadgetClass, "AllowGadgetStackingOnTop", offStackTop) &&
              off(gadgetClass, "CanStackOnStackableGadgets", offCanStack) && off(gadgetClass, "_model", offModel) &&
              off(gadgetClass, "_stackedGadgets", offStacked) && helpers && hitClass && colliderClass;
    if (!ok) return ctx.Fail("resolve failed: " + why);

    std::string err;
    void* setModel = ptr(R.Method(gadgetClass, "SetModel", 1, &err), "Gadget.SetModel");
    void* valid = ptr(R.Method(itemClass, "IsPlacementValid", 2, &err), "GadgetItem.IsPlacementValid");
    // The ref GadgetOverlapInfo overload (7 parameters); the 6-parameter one only forwards to it.
    void* overlap = ptr(R.Method(helpers, "IsGadgetOverlapping", 7, &err), "GadgetOverlapHelpers.IsGadgetOverlapping(7)");
    void* pickup = ptr(R.Method(itemClass, "PickupOrStoreStackedGadgets", 2, &err),
                       "GadgetItem.PickupOrStoreStackedGadgets");
    getColliderMi = R.Method(hitClass, "get_collider", 0, &err);
    getCollider = reinterpret_cast<ColliderFn>(ptr(getColliderMi, "RaycastHit.get_collider"));
    inParentMi = R.MethodByParams(unity::ComponentClass(), "GetComponentInParent", {"System.Type", "System.Boolean"}, &err);
    inParent = reinterpret_cast<InParentFn>(ptr(inParentMi, "Component.GetComponentInParent(Type, bool)"));
    getEnabledMi = R.MethodByParams(gadgetClass, "get_enabled", {}, &err);
    getEnabled = reinterpret_cast<BoolGetFn>(ptr(getEnabledMi, "Behaviour.get_enabled"));
    isTriggerMi = R.MethodByParams(colliderClass, "get_isTrigger", {}, &err);
    isTrigger = reinterpret_cast<BoolGetFn>(ptr(isTriggerMi, "Collider.get_isTrigger"));
    wbarrier = reinterpret_cast<WBarrierFn>(api().module ? GetProcAddress(api().module, "il2cpp_gc_wbarrier_set_field")
                                                         : nullptr);
    if (!wbarrier && why.empty()) why = "il2cpp_gc_wbarrier_set_field export missing";
    if (!setModel || !valid || !overlap || !pickup || !getCollider || !inParent || !getEnabled || !isTrigger ||
        !wbarrier)
        return ctx.Fail("resolve failed: " + why);

    auto D = [](auto f) { return reinterpret_cast<void*>(f); };
    auto O = [](auto* p) { return reinterpret_cast<void**>(p); };
    if (!ctx.HookAddress("Gadget.SetModel", setModel, D(&SetModelDetour), O(&g_origSetModel)) ||
        !ctx.HookAddress("GadgetItem.IsPlacementValid", valid, D(&ValidDetour), O(&g_origValid)) ||
        !ctx.HookAddress("GadgetOverlapHelpers.IsGadgetOverlapping", overlap, D(&OverlapDetour), O(&g_origOverlap)) ||
        !ctx.HookAddress("GadgetItem.PickupOrStoreStackedGadgets", pickup, D(&PickupDetour), O(&g_origPickup)))
        return false;

    g_self = this;
    SR2_INFO("[{}] setup ok: Gadget.SetModel RVA 0x{:X}, IsPlacementValid RVA 0x{:X}, IsGadgetOverlapping RVA 0x{:X}, "
             "PickupOrStoreStackedGadgets RVA 0x{:X}; picking up a gadget takes the ones stacked on it along "
             "(the game's rule)",
             kModule, sr2::il2cpp::Resolver::Rva(setModel), sr2::il2cpp::Resolver::Rva(valid),
             sr2::il2cpp::Resolver::Rva(overlap), sr2::il2cpp::Resolver::Rva(pickup));
    return true;
}

}  // namespace

std::unique_ptr<sr2::core::Module> CreateGadgetStacking() { return std::make_unique<GadgetStacking>(); }

}  // namespace sr2::modules
