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
// ---------------------------------------------------------------- Hen Collector ------
//   Vanilla elder collector (COOP prefab "patchCoop", DeluxeCoopUpgrader.DeluxeStuff = the
//   inactive "Deluxe" child): "ElderCollector" = SiloStorage (AmmoSetReference
//   "CorralPlortCollectorAmmo", AmmoSlotDefinitions = 2 x "ElderCollector": _slotTypeGroup
//   ElderGroup {ElderHen, ElderRooster}, _maxAmount 100) + "Collector Unit" PlortCollector
//   (CollectionArea = the whole coop, CollectPeriod 6 game hours, CollectFX, cyclone Animator)
//   + 2 SiloCatcher output ports (SILO_OUTPUT_ONLY, slot 0 / 1) + 2 SiloSlotUI displays + a
//   PlortCollectorActivator button; plus "coopDeluxe_tv". PlortCollector.DoCollection has NO
//   count limit: it springs every actor in the area that the storage CouldAddToAnySlot and
//   stores it (FixedUpdate: MaybeAddToAnySlot, SpawnAndPlayFX(CollectFX), DestroyActor). So the
//   elder collector's only hardcoded limit is its storage: 2 slots x 100.
//   Save: SiloStorage.Awake -> InitAmmo builds a LOCAL AmmoSlotManager from the slot
//   definitions; only SetModel(LandPlotModel) links it to LandPlotModel.siloAmmo[set guid]
//   (the vanilla save). SetModel / InitModel of plot children are called only by
//   LandPlotModel.NotifyParticipants / Init, i.e. only from InstantiatePlot (a NEW plot object;
//   our clones are never there). Storing hens in the vanilla elder set is NOT vanilla-valid:
//   on a mod-free load the slots get the ElderGroup definitions again
//   (AmmoSlotDefinition.IsAllowed = !blockList.Contains && group.IsMember) and
//   ValidateAndAdjustSlots would drop them.
//   => Our clone: Instantiate(DeluxeStuff) under the coop plot (inactive), TV and button
//   removed, the SiloStorage's two slot definitions replaced by ONE session clone of the
//   elder definition (same _maxAmount 100, _slotTypeGroup = MeatGroup, block list = ElderGroup
//   members + UnstableMeat), PlortCollector disabled (it would NRE without a LandPlotModel and
//   cannot keep a breeding stock), placed in a free corner (below). The storage gets
//   InitAmmo + a session AmmoModel of its own (AmmoSlotManager.SetModel; without a model every slot
//   call NREs: slot displays, ports, our tick). Then activated:
//   local storage only, never linked to the plot model, contents persisted in the sidecar,
//   output ports / slot displays work as vanilla (vac the chickens out).
//   Collecting (our tick, ~1 Hz): adults = Physics.OverlapBox over the coop's active
//   CoopRegion -> Identifiable.identType in {MeatGroup} - {ElderGroup} - UnstableMeat (chicks
//   are in ChickGroup: never counted, never collected), not held by a vac. While more than
//   the coop's keep-limit (CoopRequest.keepAdults: per coop from the sidecar, set with the plot
//   menu slider, kHenKeepMin..kHenKeepMax, default 20) adults are in the coop and the collector's period (its CollectPeriod, 6 game
//   hours, like the elder collector) is due, the excess is vacuumed in, keeping at least
//   kKeepRoosters roosters, until the storage is full.
//   Vacuum (user test of 4d16e38c: chickens just vanished): the vanilla elder collector's own
//   suction. PlortCollector.DoCollection (0x98B670) gives every actor it takes a "CollectJoint"
//   (kinematic Rigidbody under CollectPt, SpringJoint spring = CollectSpringiness, maxDistance =
//   the distance, no auto anchor, connectedAnchor 0), SafeJointReference.AttachSafely(actor,
//   joint, true), connectedBody.WakeUp, Vacuumable.Capture(joint), and a JointReference
//   {AmmoMetadata +0x10 (Identifiable.get_AmmoMetadata), Vacuumable +0x30, Joint +0x38} in
//   _joints; then _forceCollectUntil = HoursFromNow(1/12), _endCollectAt = HoursFromNow(1/6).
//   PlortCollector.FixedUpdate (0x98BE90) does the rest: Region hibernated -> nothing; joints and
//   _endCollectAt reached -> every JointReference.Destroy (joint destroyed, Vacuumable.Release);
//   no joints -> DoCollection once HasReached(_model.collectorNextTime) (the only _model use: a
//   null model NREs here); per joint: storage CouldAddToAnySlot else released, farther than 1 m ->
//   maxDistance -= 5 * dt, else storage.MaybeAddToAnySlot + SpawnAndPlayFX(CollectFX) +
//   Destroyer.DestroyActor; cyclone Animator bool = joints || !HasReached(_forceCollectUntil),
//   vac audio on / off. So the clone's PlortCollector runs with a private LandPlotModel (never
//   registered anywhere, collectorNextTime = 1e300: its own DoCollection never fires) and our tick
//   builds the joints DoCollection would, for the chosen chickens only. Without those parts
//   (names not found, no Region above the plot) the old direct store + puff is the fallback.
//   Placement (asset data of "patchCoop", plot-local = coop prefab root): the "Deluxe" container
//   is NOT identity in the prefab - its localRotation is (0, -0.707, 0, 0.707) = -90 deg about Y,
//   so the vanilla elder collector (CollectionPoint at Deluxe-local (3.75, 0.5, -3.75)) stands in
//   the plot's (+x, +z) corner. Plot menu terminal "techActivator" (-5.54, -5.54) = (-x, -z),
//   vitamizer "Vitamizer/objCoopVitamizer" (-4.75, 4.75) = (-x, +z); (+x, -z) is free. Build
//   4d16e38c turned the clone root -90 deg believing the container was identity: that is exactly
//   the vanilla transform, so the Hen Collector stood INSIDE the vanilla elder collector of a
//   deluxe coop (same meshes, same place: invisible) - its log line "render bounds ... plot-relative
//   centre (-4.97, 1.89, -4.95)" (world axes, plot turned 180 deg) is that corner. Now PickCorner
//   reads the occupied corners from the live plot's own children (by path, plot-local through
//   InverseTransformPoint) and turns the clone by a multiple of 90 deg into the first free one,
//   (+x, -z) preferred; logged once per build ("hen collector corner").
//
// Clones are named "sr2mods_<kind>#<generation>" and found again every tick under the plot
// transform (no stored pointers). A clone of another generation (an older save game / before a
// load) is destroyed without looking at it. They are never part of the vanilla save: the game
// does not save plot children, and the clones are rebuilt from the sidecar after every load.

#include "modules/custom_upgrades/coop_world.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <initializer_list>
#include <map>
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
constexpr std::string_view kHenKind = "HenCollector";
constexpr float kFitTolerance = 0.02f;
constexpr int kHideFlagsDontUnloadUnusedAsset = 32;
constexpr int kKeepRoosters = 2;   // never collect the last roosters (keep-limit: CoopRequest.keepAdults)
constexpr int kWarmupTicks = 5;    // no collecting in the first seconds after a (re)build / load
constexpr int32_t kQueryTriggerIgnore = 1;
constexpr size_t kAmmoMetadataSize = 32;  // AmmoSlot.AmmoMetadata {IdentifiableType Id; float4 Emotions; bool Radiant}
constexpr float kForceCollectHours = 1.0f / 12.0f;  // DoCollection: _forceCollectUntil (cyclone shown at least this long)
constexpr float kEndCollectHours = 1.0f / 6.0f;     // DoCollection: _endCollectAt (unfinished joints released)

struct Vec3 {
    float x, y, z;
};
struct Quat {
    float x, y, z, w;
};
struct Bounds {
    Vec3 center, extents;
};

// v rotated by the unit quaternion q (q * v * q^-1).
Vec3 Rotate(const Quat& q, const Vec3& v) {
    const float tx = 2.0f * (q.y * v.z - q.z * v.y), ty = 2.0f * (q.z * v.x - q.x * v.z),
                tz = 2.0f * (q.x * v.y - q.y * v.x);
    return {v.x + q.w * tx + (q.y * tz - q.z * ty), v.y + q.w * ty + (q.z * tx - q.x * tz),
            v.z + q.w * tz + (q.x * ty - q.y * tx)};
}
Quat Yaw(float degrees) {
    const float h = degrees * 3.14159265f / 360.0f;
    return {0.0f, std::sin(h), 0.0f, std::cos(h)};
}
// Plot corner of a plot-local point: x / z signs, "(+x, -z)".
struct Quadrant {
    int x = 0, z = 0;
    bool operator==(const Quadrant&) const = default;
};
Quadrant QuadrantOf(const Vec3& v) { return {v.x >= 0.0f ? 1 : -1, v.z >= 0.0f ? 1 : -1}; }
std::string Name(Quadrant q) { return std::format("({}x, {}z)", q.x > 0 ? '+' : '-', q.z > 0 ? '+' : '-'); }

}  // namespace

struct CoopWorld::Impl {
    const char* module = "custom_upgrades";

    Il2CppClass* plotClass = nullptr;
    Il2CppClass* coopRegionClass = nullptr;
    Il2CppClass* airNetUpgraderClass = nullptr;
    Il2CppClass* deluxeUpgraderClass = nullptr;
    Il2CppClass* siloStorageClass = nullptr;
    Il2CppClass* plortCollectorClass = nullptr;
    Il2CppClass* collectorActivatorClass = nullptr;
    Il2CppClass* slotDefClass = nullptr;
    Il2CppClass* typeGroupClass = nullptr;
    Il2CppClass* identifiableClass = nullptr;
    Il2CppClass* identTypeClass = nullptr;
    Il2CppClass* vacuumableClass = nullptr;
    Il2CppClass* colliderClass = nullptr;
    Il2CppClass* gameObjectClass = nullptr;
    Il2CppClass* transformClass = nullptr;
    FieldInfo* allCoopsField = nullptr;
    using StaticGetFn = void (*)(FieldInfo*, void*);
    StaticGetFn staticGet = nullptr;
    using WBarrierFn = void (*)(Il2CppObject*, void**, void*);
    WBarrierFn wbarrier = nullptr;
    size_t offPlotType = 0, offAirNets = 0, offDeluxeStuff = 0;
    size_t offStorageDefs = 0, offStorageModel = 0;
    size_t offPcPeriod = 0, offPcAnim = 0, offPcFx = 0, offPcTimeDir = 0, offPcCycloneId = 0;
    size_t offDefGroup = 0, offDefBlock = 0, offDefMax = 0;
    size_t offGroupMembers = 0, offIdentType = 0;

    const MethodInfo* getPlotPrefab = nullptr;     // LookupDirector.GetPlotPrefab(Id)
    const MethodInfo* compInParent = nullptr;      // Component.GetComponentInParent(Type, bool)
    const MethodInfo* compInChildren = nullptr;    // Component.GetComponentInChildren(Type, bool)
    const MethodInfo* goActiveSelf = nullptr;      // GameObject.get_activeSelf
    const MethodInfo* trChildCount = nullptr;      // Transform.get_childCount
    const MethodInfo* trGetChild = nullptr;        // Transform.GetChild(int)
    const MethodInfo* trGetParent = nullptr;       // Transform.get_parent
    const MethodInfo* trGetPosition = nullptr;     // Transform.get_position
    const MethodInfo* trSetPosition = nullptr;     // Transform.set_position
    const MethodInfo* trGetRotation = nullptr;     // Transform.get_rotation
    const MethodInfo* trSetLocalRotation = nullptr;
    const MethodInfo* trGetLocalScale = nullptr;   // Transform.get_localScale
    const MethodInfo* trSetLocalScale = nullptr;   // Transform.set_localScale
    const MethodInfo* colBounds = nullptr;         // Collider.get_bounds
    const MethodInfo* colEnabled = nullptr;        // Collider.get_enabled
    const MethodInfo* objDestroy = nullptr;        // Object.Destroy(Object)
    const MethodInfo* objDestroyImmediate = nullptr;  // Object.DestroyImmediate(Object)
    const MethodInfo* objSetHideFlags = nullptr;   // Object.set_hideFlags (optional)
    const MethodInfo* behSetEnabled = nullptr;     // Behaviour.set_enabled
    const MethodInfo* syncTransforms = nullptr;    // Physics.SyncTransforms() (optional)
    const MethodInfo* overlapBox = nullptr;        // Physics.OverlapBox(c, half, rot, mask, qti)
    const MethodInfo* localAmmo = nullptr;         // SiloStorage.get_LocalAmmo
    const MethodInfo* ammoSlots = nullptr;         // AmmoSlotManager.get_Slots
    const MethodInfo* slotName = nullptr;          // AmmoSlotManager.GetSlotName(int)
    const MethodInfo* slotCount = nullptr;         // AmmoSlotManager.GetSlotCount(int)
    const MethodInfo* slotMax = nullptr;           // AmmoSlotManager.GetSlotMaxCount(int)
    const MethodInfo* addResource = nullptr;       // AmmoSlotManager.MaybeAddResource(id, slot, n, overflow)
    const MethodInfo* isCaptive = nullptr;         // Vacuumable.IsCaptive()
    const MethodInfo* spawnFx = nullptr;           // FXHelpers.SpawnAndPlayFX(GameObject, Vector3, Quaternion)
    const MethodInfo* destroyActor = nullptr;      // Destroyer.DestroyActor(GameObject, string, bool)
    const MethodInfo* hoursFromNow = nullptr;      // TimeDirector.HoursFromNow(float)
    const MethodInfo* hasReached = nullptr;        // TimeDirector.HasReached(double)
    const MethodInfo* animSetBool = nullptr;       // Animator.SetBool(int, bool)
    const MethodInfo* initAmmo = nullptr;          // SiloStorage.InitAmmo() (local AmmoSlotManager)
    const MethodInfo* ammoModelCtor = nullptr;     // AmmoModel..ctor(UnitPropertyTracker)
    const MethodInfo* ammoSetModel = nullptr;      // AmmoSlotManager.SetModel(AmmoModel)
    Il2CppClass* ammoModelClass = nullptr;
    using ObjectNewFn = Il2CppObject* (*)(Il2CppClass*);
    ObjectNewFn objectNew = nullptr;
    // diagnostics (one-shot per build)
    Il2CppClass* rendererClass = nullptr;
    const MethodInfo* compsInChildren = nullptr;   // Component.GetComponentsInChildren(Type, bool)
    const MethodInfo* rendEnabled = nullptr;       // Renderer.get_enabled
    const MethodInfo* rendVisible = nullptr;       // Renderer.get_isVisible
    const MethodInfo* rendBounds = nullptr;        // Renderer.get_bounds
    const MethodInfo* trSetLocalPosition = nullptr;
    // placement (optional: without them the clone keeps the vanilla corner, logged)
    const MethodInfo* trGetLocalRotation = nullptr;  // Transform.get_localRotation
    const MethodInfo* trInverseTransformPoint = nullptr;  // Transform.InverseTransformPoint(Vector3)
    const MethodInfo* trFind = nullptr;            // Transform.Find(string)
    size_t offPcCollectPt = 0;                     // PlortCollector.CollectPt
    // vacuum (optional: without it the chickens are stored directly, with a puff)
    bool vacuum = false;
    Il2CppClass* rigidbodyClass = nullptr;
    Il2CppClass* springJointClass = nullptr;
    Il2CppClass* landPlotModelClass = nullptr;
    Il2CppClass* jointRefClass = nullptr;          // PlortCollector.JointReference (nested)
    const MethodInfo* goCtor = nullptr;            // GameObject..ctor(string)
    const MethodInfo* goAddComponent = nullptr;    // GameObject.AddComponent(Type)
    const MethodInfo* trSetParent = nullptr;       // Transform.SetParent(Transform, bool)
    const MethodInfo* rbSetKinematic = nullptr;    // Rigidbody.set_isKinematic
    const MethodInfo* rbWakeUp = nullptr;          // Rigidbody.WakeUp
    const MethodInfo* sjSetSpring = nullptr;       // SpringJoint.set_spring
    const MethodInfo* sjSetMaxDistance = nullptr;  // SpringJoint.set_maxDistance
    const MethodInfo* jSetAutoAnchor = nullptr;    // Joint.set_autoConfigureConnectedAnchor
    const MethodInfo* jSetAnchor = nullptr;        // Joint.set_connectedAnchor
    const MethodInfo* jGetBody = nullptr;          // Joint.get_connectedBody
    const MethodInfo* attachSafely = nullptr;      // SafeJointReference.AttachSafely(GameObject, Joint, bool)
    const MethodInfo* vacCapture = nullptr;        // Vacuumable.Capture(Joint)
    const MethodInfo* identMeta = nullptr;         // Identifiable.get_AmmoMetadata
    const MethodInfo* lpmCtor = nullptr;           // LandPlotModel..ctor()
    const MethodInfo* jrDestroy = nullptr;         // JointReference.Destroy (joint + Vacuumable.Release)
    size_t offPcModel = 0, offPcRegion = 0, offPcJoints = 0, offPcEnd = 0, offPcForce = 0, offPcSpring = 0;
    size_t offPcStorage = 0, offPcAudio = 0;
    size_t offLpmNext = 0;                         // LandPlotModel.collectorNextTime
    size_t offJrMeta = 0, offJrVac = 0, offJrJoint = 0;
    bool loggedNoVacuum = false;

    // Session-wide Hen Collector assets (built once, pinned, never freed).
    bool henTried = false;
    Il2CppObject* henSlotDef = nullptr;            // our AmmoSlotDefinition clone
    std::vector<Il2CppObject*> adults;             // adult hen / rooster IdentifiableTypes
    std::vector<std::string> adultNames;
    int henMax = 0;

    struct PlotTimer {
        double next = 0.0;  // world time of the next collection (0 = due)
        int age = 0;        // ticks the current-generation collector has been served
        bool diagLogged = false;  // in-world diagnostic written for this build
    };
    std::map<std::string, PlotTimer> timers;

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
    void SetRef(Il2CppObject* obj, void** slot, Il2CppObject* value) {
        if (wbarrier) wbarrier(obj, slot, value);
        else *slot = value;
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
    void DestroyNow(Il2CppObject* go) {
        if (unity::IsAlive(go)) Call(objDestroyImmediate, nullptr, {go}, "Object.DestroyImmediate");
    }
    void Sync() {
        if (syncTransforms) Call(syncTransforms, nullptr, {}, "Physics.SyncTransforms");
    }
    std::vector<Il2CppObject*> ChildTransforms(Il2CppObject* tr) {
        std::vector<Il2CppObject*> out;
        bool ok = false;
        const int n = Value<int32_t>(Call(trChildCount, tr, {}, "Transform.get_childCount", &ok));
        for (int32_t i = 0; ok && i < n && i < 128; ++i) {
            Il2CppObject* child = Call(trGetChild, tr, {&i}, "Transform.GetChild", &ok);
            if (ok && unity::IsAlive(child)) out.push_back(child);
        }
        return out;
    }
    Il2CppObject* PlotPrefab(Il2CppObject* lookup, int32_t id) {
        if (!unity::IsAlive(lookup)) return nullptr;
        bool ok = false;
        Il2CppObject* prefab = Call(getPlotPrefab, lookup, {&id}, "LookupDirector.GetPlotPrefab", &ok);
        return ok && unity::IsAlive(prefab) && IsA(prefab, gameObjectClass) ? prefab : nullptr;
    }

    struct CoopInfo {
        Il2CppObject* plot = nullptr;
        bool hasRegion = false;
        Bounds region{};  // the active CoopRegion trigger with the highest top
    };
    std::vector<CoopInfo> Coops();

    struct Children {
        Il2CppObject* airNet = nullptr;  // current-generation clones (GameObjects)
        Il2CppObject* hen = nullptr;
    };
    Children Scan(Il2CppObject* plot, unsigned generation, bool destroyStale, bool destroyAll = false);

    // Air net
    Il2CppObject* AirNetSource(Il2CppObject* lookup);
    void CreateAirNet(Il2CppObject* plot, const CoopInfo& c, Il2CppObject* lookup, unsigned generation);
    void FitAirNet(Il2CppObject* clone, const CoopInfo& c, bool log);
    bool loggedNoSource = false;

    // Hen collector
    Il2CppObject* DeluxeSource(Il2CppObject* lookup);
    bool EnsureHenAssets(Il2CppObject* lookup);
    Il2CppObject* AdultByName(const std::string& name) const;
    bool IsAdult(Il2CppObject* type) const {
        return std::find(adults.begin(), adults.end(), type) != adults.end();
    }
    void CreateHen(Il2CppObject* plot, CoopRequest& req, Il2CppObject* lookup, unsigned generation);
    bool LinkLocalModel(Il2CppObject* storage, size_t nDefs);
    void LogPlacement(Il2CppObject* plot, Il2CppObject* clone, const char* when);
    struct Corner {
        bool ok = false;
        Quat rot{0, 0, 0, 1};  // clone root localRotation
        std::string log;
    };
    Corner PickCorner(Il2CppObject* plot, Il2CppObject* pc);
    bool SetupVacuum(Resolver& R);
    bool EnableVacuum(Il2CppObject* pc, const std::string& id);
    bool Vacuuming(Il2CppObject* pc) const { return vacuum && unity::Field<Il2CppObject*>(pc, offPcModel) != nullptr; }
    int PendingJoints(Il2CppObject* pc) const {
        return Vacuuming(pc) ? static_cast<int>(unity::ListCount(unity::Field<void*>(pc, offPcJoints))) : 0;
    }
    bool Suck(Il2CppObject* pc, Il2CppObject* ident, Il2CppObject* vac);
    void ReleaseJoints(Il2CppObject* clone);
    bool PlotLocal(Il2CppObject* plotTr, Il2CppObject* tr, Vec3& out);
    void Restore(Il2CppObject* storage, const std::vector<HenSlot>& slots);
    bool ReadSlots(Il2CppObject* clone, std::vector<HenSlot>& out, Il2CppObject** ammoOut = nullptr,
                   Il2CppObject** storageOut = nullptr);
    void SnapshotInto(Il2CppObject* clone, CoopRequest& req);
    void ServeHen(Il2CppObject* clone, const CoopInfo& c, CoopRequest& req);
    int Collect(Il2CppObject* clone, Il2CppObject* pc, Il2CppObject* ammo, const CoopInfo& c, int keep, int* adultsOut);
    bool loggedNoHen = false;
    bool henBroken = false;  // a build failed structurally: not retried this session
    bool loggedLinked = false;
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

    HMODULE ga = api().module;
    p->staticGet = reinterpret_cast<Impl::StaticGetFn>(ga ? GetProcAddress(ga, "il2cpp_field_static_get_value") : nullptr);
    p->wbarrier = reinterpret_cast<Impl::WBarrierFn>(ga ? GetProcAddress(ga, "il2cpp_gc_wbarrier_set_field") : nullptr);
    if (!p->staticGet) why = "il2cpp_field_static_get_value export missing";

    p->plotClass = cls(kImage, "", "LandPlot");
    p->coopRegionClass = cls(kImage, "", "CoopRegion");
    p->airNetUpgraderClass = cls(kImage, "", "AirNetUpgrader");
    p->deluxeUpgraderClass = cls(kImage, "", "DeluxeCoopUpgrader");
    p->siloStorageClass = cls(kImage, "", "SiloStorage");
    p->plortCollectorClass = cls(kImage, "", "PlortCollector");
    p->collectorActivatorClass = cls(kImage, "", "PlortCollectorActivator");
    p->slotDefClass = cls(kImage, "", "AmmoSlotDefinition");
    p->typeGroupClass = cls(kImage, "", "IdentifiableTypeGroup");
    p->identifiableClass = cls(kImage, "", "Identifiable");
    p->identTypeClass = cls(kImage, "", "IdentifiableType");
    p->vacuumableClass = cls(kImage, "MonomiPark.SlimeRancher", "Vacuumable");
    Il2CppClass* lookup = cls(kImage, "", "LookupDirector");
    Il2CppClass* ammoMgr = cls(kImage, "MonomiPark.SlimeRancher.Player", "AmmoSlotManager");
    Il2CppClass* fx = cls(kImage, "", "FXHelpers");
    Il2CppClass* destroyer = cls(kImage, "", "Destroyer");
    Il2CppClass* timeDir = cls(kImage, "", "TimeDirector");
    p->colliderClass = cls(kPhysics, "UnityEngine", "Collider");
    Il2CppClass* physics = cls(kPhysics, "UnityEngine", "Physics");
    Il2CppClass* animator = cls("UnityEngine.AnimationModule", "UnityEngine", "Animator");
    Il2CppClass* component = cls(kCore, "UnityEngine", "Component");
    Il2CppClass* behaviour = cls(kCore, "UnityEngine", "Behaviour");
    p->transformClass = cls(kCore, "UnityEngine", "Transform");
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
    off(p->deluxeUpgraderClass, "DeluxeStuff", p->offDeluxeStuff);
    off(p->siloStorageClass, "AmmoSlotDefinitions", p->offStorageDefs);
    off(p->siloStorageClass, "Model", p->offStorageModel);
    off(p->plortCollectorClass, "CollectPeriod", p->offPcPeriod);
    off(p->plortCollectorClass, "CollectAnim", p->offPcAnim);
    off(p->plortCollectorClass, "CollectFX", p->offPcFx);
    off(p->plortCollectorClass, "_timeDir", p->offPcTimeDir);
    off(p->plortCollectorClass, "_animCycloneActiveId", p->offPcCycloneId);
    off(p->slotDefClass, "_slotTypeGroup", p->offDefGroup);
    off(p->slotDefClass, "_slotBlockList", p->offDefBlock);
    off(p->slotDefClass, "_maxAmount", p->offDefMax);
    off(p->typeGroupClass, "_memberTypes", p->offGroupMembers);
    off(p->identifiableClass, "identType", p->offIdentType);
    p->getPlotPrefab = method(lookup, "GetPlotPrefab", 1);
    p->compInParent = byParams(component, "GetComponentInParent", {"System.Type", "System.Boolean"});
    p->compInChildren = byParams(component, "GetComponentInChildren", {"System.Type", "System.Boolean"});
    p->goActiveSelf = method(p->gameObjectClass, "get_activeSelf", 0);
    p->trChildCount = method(p->transformClass, "get_childCount", 0);
    p->trGetChild = method(p->transformClass, "GetChild", 1);
    p->trGetParent = method(p->transformClass, "get_parent", 0);
    p->trGetPosition = method(p->transformClass, "get_position", 0);
    p->trSetPosition = method(p->transformClass, "set_position", 1);
    p->trGetRotation = method(p->transformClass, "get_rotation", 0);
    p->trSetLocalRotation = method(p->transformClass, "set_localRotation", 1);
    p->trGetLocalScale = method(p->transformClass, "get_localScale", 0);
    p->trSetLocalScale = method(p->transformClass, "set_localScale", 1);
    p->colBounds = method(p->colliderClass, "get_bounds", 0);
    p->colEnabled = method(p->colliderClass, "get_enabled", 0);
    p->objDestroy = byParams(uobject, "Destroy", {"UnityEngine.Object"});
    p->objDestroyImmediate = byParams(uobject, "DestroyImmediate", {"UnityEngine.Object"});
    p->behSetEnabled = method(behaviour, "set_enabled", 1);
    p->overlapBox = byParams(physics, "OverlapBox",
                             {"UnityEngine.Vector3", "UnityEngine.Vector3", "UnityEngine.Quaternion", "System.Int32",
                              "UnityEngine.QueryTriggerInteraction"});
    p->localAmmo = method(p->siloStorageClass, "get_LocalAmmo", 0);
    p->ammoSlots = method(ammoMgr, "get_Slots", 0);
    p->slotName = method(ammoMgr, "GetSlotName", 1);
    p->slotCount = method(ammoMgr, "GetSlotCount", 1);
    p->slotMax = method(ammoMgr, "GetSlotMaxCount", 1);
    p->addResource = method(ammoMgr, "MaybeAddResource", 4);
    p->isCaptive = method(p->vacuumableClass, "IsCaptive", 0);
    p->spawnFx = byParams(fx, "SpawnAndPlayFX", {"UnityEngine.GameObject", "UnityEngine.Vector3", "UnityEngine.Quaternion"});
    p->destroyActor = method(destroyer, "DestroyActor", 3);
    p->hoursFromNow = method(timeDir, "HoursFromNow", 1);
    p->hasReached = method(timeDir, "HasReached", 1);
    p->animSetBool = byParams(animator, "SetBool", {"System.Int32", "System.Boolean"});
    if (!why.empty()) {
        delete p;
        return false;
    }
    {
        std::string err;
        p->syncTransforms = R.Method(physics, "SyncTransforms", 0, &err);  // optional
        p->objSetHideFlags = R.Method(uobject, "set_hideFlags", 1, &err);  // optional
        // Hen Collector storage model (without it the collector is not offered: see CreateHen).
        p->objectNew = reinterpret_cast<Impl::ObjectNewFn>(ga ? GetProcAddress(ga, "il2cpp_object_new") : nullptr);
        p->initAmmo = R.Method(p->siloStorageClass, "InitAmmo", 0, &err);
        p->ammoModelClass = R.Class(kImage, "MonomiPark.SlimeRancher.DataModel", "AmmoModel", &err);
        p->ammoModelCtor = p->ammoModelClass ? R.Method(p->ammoModelClass, ".ctor", 1, &err) : nullptr;
        p->ammoSetModel = R.Method(ammoMgr, "SetModel", 1, &err);
        // Diagnostics (optional).
        p->rendererClass = R.Class(kCore, "UnityEngine", "Renderer", &err);
        p->compsInChildren = R.MethodByParams(component, "GetComponentsInChildren", {"System.Type", "System.Boolean"}, &err);
        p->rendEnabled = p->rendererClass ? R.Method(p->rendererClass, "get_enabled", 0, &err) : nullptr;
        p->rendVisible = p->rendererClass ? R.Method(p->rendererClass, "get_isVisible", 0, &err) : nullptr;
        p->rendBounds = p->rendererClass ? R.Method(p->rendererClass, "get_bounds", 0, &err) : nullptr;
        p->trSetLocalPosition = R.Method(p->transformClass, "set_localPosition", 1, &err);
        // Corner choice (optional).
        p->trGetLocalRotation = R.Method(p->transformClass, "get_localRotation", 0, &err);
        p->trInverseTransformPoint = R.MethodByParams(p->transformClass, "InverseTransformPoint", {"UnityEngine.Vector3"}, &err);
        p->trFind = R.MethodByParams(p->transformClass, "Find", {"System.String"}, &err);
        if (auto o = R.FieldOffset(p->plortCollectorClass, "CollectPt", &err)) p->offPcCollectPt = *o;
        // Vacuum: the vanilla collector's joints (all optional together).
        p->vacuum = p->SetupVacuum(R);
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
    const std::string suffix = std::format("#{}", generation);
    std::vector<Il2CppObject*> doomed;
    for (Il2CppObject* child : ChildTransforms(tr)) {
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
        else if (kind == kHenKind && !c.hen) c.hen = go;
        else doomed.push_back(go);  // duplicate / unknown: never keep two
    }
    for (Il2CppObject* go : doomed) Destroy(go);
    return c;
}

// ---------------------------------------------------------------- air net --

Il2CppObject* CoopWorld::Impl::AirNetSource(Il2CppObject* lookup) {
    Il2CppObject* prefab = PlotPrefab(lookup, kPlotCorral);
    if (!prefab) return nullptr;
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

// ---------------------------------------------------------------- hen collector --

Il2CppObject* CoopWorld::Impl::DeluxeSource(Il2CppObject* lookup) {
    Il2CppObject* prefab = PlotPrefab(lookup, kPlotCoop);
    if (!prefab) return nullptr;
    Il2CppObject* up = unity::GetComponent(prefab, deluxeUpgraderClass);
    if (!unity::IsAlive(up) || !IsA(up, deluxeUpgraderClass)) return nullptr;
    auto* go = unity::Field<Il2CppObject*>(up, offDeluxeStuff);
    return unity::IsAlive(go) && IsA(go, gameObjectClass) ? go : nullptr;
}

Il2CppObject* CoopWorld::Impl::AdultByName(const std::string& name) const {
    for (size_t i = 0; i < adults.size() && i < adultNames.size(); ++i)
        if (adultNames[i] == name && unity::IsAlive(adults[i])) return adults[i];
    return nullptr;
}

bool CoopWorld::Impl::EnsureHenAssets(Il2CppObject* lookup) {
    if (henTried) return henSlotDef != nullptr;
    henTried = true;
    // The vanilla elder slot definition (limit, ElderGroup) from the coop prefab.
    Il2CppObject* deluxe = DeluxeSource(lookup);
    Il2CppObject* storage = deluxe ? InChildren(unity::TransformOf(deluxe), siloStorageClass, true) : nullptr;
    void* defs = storage ? unity::Field<void*>(storage, offStorageDefs) : nullptr;
    auto* elderDef = static_cast<Il2CppObject*>(unity::ArrayAt(defs, 0));
    if (!unity::IsAlive(elderDef) || !IsA(elderDef, slotDefClass)) {
        SR2_WARN("[{}] hen collector: deluxe coop elder collector not found in the coop prefab", module);
        return false;
    }
    Il2CppObject* elderGroup = unity::Field<Il2CppObject*>(elderDef, offDefGroup);
    // MeatGroup: hens, roosters, elders (+ UnstableMeat). One-time scan of the loaded groups.
    Il2CppObject* meat = nullptr;
    for (Il2CppObject* g : unity::FindObjectsOfTypeAll(typeGroupClass))
        if (unity::IsAlive(g) && unity::GetName(g) == "MeatGroup") {
            meat = g;
            break;
        }
    if (!unity::IsAlive(meat) || !unity::IsAlive(elderGroup)) {
        SR2_WARN("[{}] hen collector: MeatGroup / ElderGroup not found", module);
        return false;
    }
    void* meatList = unity::Field<void*>(meat, offGroupMembers);
    void* elderList = unity::Field<void*>(elderGroup, offGroupMembers);
    std::vector<Il2CppObject*> elders, blocked;
    for (size_t i = 0, n = unity::ListCount(elderList); i < n && i < 32; ++i)
        if (auto* t = static_cast<Il2CppObject*>(unity::ListAt(elderList, i)); unity::IsAlive(t)) elders.push_back(t);
    std::string names;
    for (size_t i = 0, n = unity::ListCount(meatList); i < n && i < 64; ++i) {
        auto* t = static_cast<Il2CppObject*>(unity::ListAt(meatList, i));
        if (!unity::IsAlive(t) || !IsA(t, identTypeClass)) continue;
        const std::string nm = unity::GetName(t);
        const bool elder = std::find(elders.begin(), elders.end(), t) != elders.end();
        if (elder || nm.find("Unstable") != std::string::npos) {
            blocked.push_back(t);
            continue;
        }
        adults.push_back(t);
        adultNames.push_back(nm);
        unity::NewGCHandle(t);  // pinned for the session
        names += (names.empty() ? "" : ", ") + nm;
    }
    if (adults.empty()) {
        SR2_WARN("[{}] hen collector: no adult chicken types in MeatGroup", module);
        return false;
    }
    // Our slot definition: a clone of the elder one (same limit), for adult chickens.
    Il2CppObject* def = unity::Instantiate(elderDef, nullptr);
    if (!unity::IsAlive(def) || !IsA(def, slotDefClass)) {
        SR2_WARN("[{}] hen collector: slot definition clone failed", module);
        return false;
    }
    unity::NewGCHandle(def);  // pinned for the session (never freed)
    unity::SetName(def, "sr2mods_HenCollectorSlot");
    if (objSetHideFlags) {
        int32_t flags = kHideFlagsDontUnloadUnusedAsset;
        Call(objSetHideFlags, def, {&flags}, "Object.set_hideFlags");
    }
    SetRef(def, reinterpret_cast<void**>(reinterpret_cast<char*>(def) + offDefGroup), meat);
    // Block list (elders, UnstableMeat): only if the clone owns its list (Unity copies it).
    auto* list = unity::Field<Il2CppObject*>(def, offDefBlock);
    if (list && list != unity::Field<Il2CppObject*>(elderDef, offDefBlock)) {
        std::string err;
        const MethodInfo* add = Resolver::Get().Method(api().il2cpp_object_get_class(list), "Add", 1, &err);
        for (Il2CppObject* t : blocked)
            if (add) Call(add, list, {t}, "List.Add");
    }
    henMax = unity::Field<int32_t>(def, offDefMax);
    henSlotDef = def;
    SR2_INFO("[{}] hen collector: slot = elder collector slot ({} per slot), adults: {} (blocked: {} type(s))", module,
             henMax, names, blocked.size());
    return true;
}

void CoopWorld::Impl::Restore(Il2CppObject* storage, const std::vector<HenSlot>& slots) {
    bool ok = false;
    Il2CppObject* ammo = Call(localAmmo, storage, {}, "SiloStorage.get_LocalAmmo", &ok);
    if (!ok || !ammo) return;
    const int32_t nSlots = static_cast<int32_t>(unity::ArrayLength(Call(ammoSlots, ammo, {}, "AmmoSlotManager.get_Slots", &ok)));
    for (int32_t i = 0; i < static_cast<int32_t>(slots.size()) && i < 8; ++i) {
        const HenSlot& s = slots[i];
        if (s.count <= 0 || s.type.empty()) continue;
        Il2CppObject* type = AdultByName(s.type);
        int32_t count = std::min(s.count, 100000);
        bool overflow = false;
        bool added = false;
        for (int32_t k = 0; type && k < nSlots && !added; ++k) {
            int32_t slot = (i + k) % nSlots;  // its own slot first
            added = Value<bool>(Call(addResource, ammo, {type, &slot, &count, &overflow},
                                     "AmmoSlotManager.MaybeAddResource", &ok));
        }
        if (!added) SR2_WARN("[{}] hen collector: could not restore {} x '{}'", module, s.count, s.type);
    }
}

bool CoopWorld::Impl::ReadSlots(Il2CppObject* clone, std::vector<HenSlot>& out, Il2CppObject** ammoOut,
                                Il2CppObject** storageOut) {
    out.clear();
    Il2CppObject* storage = InChildren(unity::TransformOf(clone), siloStorageClass, true);
    if (!storage) return false;
    if (storageOut) *storageOut = storage;
    bool ok = false;
    Il2CppObject* ammo = Call(localAmmo, storage, {}, "SiloStorage.get_LocalAmmo", &ok);
    if (!ok || !ammo) return false;
    if (ammoOut) *ammoOut = ammo;
    const int32_t n = static_cast<int32_t>(unity::ArrayLength(Call(ammoSlots, ammo, {}, "AmmoSlotManager.get_Slots", &ok)));
    for (int32_t i = 0; ok && i < n && i < 8; ++i) {
        HenSlot s;
        s.count = Value<int32_t>(Call(slotCount, ammo, {&i}, "AmmoSlotManager.GetSlotCount", &ok));
        Il2CppObject* t = s.count > 0 ? Call(slotName, ammo, {&i}, "AmmoSlotManager.GetSlotName", &ok) : nullptr;
        if (s.count > 0 && unity::IsAlive(t)) s.type = unity::GetName(t);
        else s.count = 0;
        out.push_back(std::move(s));
    }
    return ok;
}

void CoopWorld::Impl::SnapshotInto(Il2CppObject* clone, CoopRequest& req) {
    if (!req.hens) return;
    std::vector<HenSlot> live;
    if (ReadSlots(clone, live) && live != *req.hens) *req.hens = std::move(live);
}

// SiloStorage.Awake -> InitAmmo only constructs an AmmoSlotManager from the slot definitions: its
// AmmoModel (the slots array, AmmoSlotManager +0x18) is set only by SiloStorage.SetModel(LandPlotModel)
// -> LandPlotModel.GetAmmoSet -> AmmoSlotManager.SetModel. Without it every slot call NREs
// (StorageSlotUI.Start / Update of the slot displays, our ReadSlots / Collect, the output ports). The
// vanilla model for a plot's ammo set is `new AmmoModel(null)` + SetModel (which Resets it to the
// definitions); we do the same with a model of our own that is never part of the LandPlotModel (so
// never saved by the game). Called while the clone is still inactive, so Awake's InitAmmo skips.
bool CoopWorld::Impl::LinkLocalModel(Il2CppObject* storage, size_t nDefs) {
    if (!initAmmo || !ammoModelClass || !ammoModelCtor || !ammoSetModel || !objectNew) {
        SR2_WARN("[{}] hen collector: AmmoModel / SiloStorage.InitAmmo / AmmoSlotManager.SetModel not found", module);
        return false;
    }
    bool ok = false;
    Call(initAmmo, storage, {}, "SiloStorage.InitAmmo", &ok);
    if (!ok) return false;
    Il2CppObject* ammo = Call(localAmmo, storage, {}, "SiloStorage.get_LocalAmmo", &ok);
    if (!ok || !ammo) {
        SR2_WARN("[{}] hen collector: SiloStorage.InitAmmo built no AmmoSlotManager", module);
        return false;
    }
    Il2CppObject* model = objectNew(ammoModelClass);
    if (!model) return false;
    Call(ammoModelCtor, model, {nullptr}, "AmmoModel..ctor", &ok);  // UnitPropertyTracker null, like vanilla
    if (!ok) return false;
    Call(ammoSetModel, ammo, {model}, "AmmoSlotManager.SetModel", &ok);
    if (!ok) return false;
    const size_t n = unity::ArrayLength(Call(ammoSlots, ammo, {}, "AmmoSlotManager.get_Slots", &ok));
    if (!ok || n != nDefs) {
        SR2_WARN("[{}] hen collector: local storage model has {} slot(s), expected {}", module, n, nDefs);
        return false;
    }
    return true;
}

bool CoopWorld::Impl::PlotLocal(Il2CppObject* plotTr, Il2CppObject* tr, Vec3& out) {
    if (!trInverseTransformPoint || !unity::IsAlive(plotTr) || !unity::IsAlive(tr)) return false;
    Vec3 world = GetVec(trGetPosition, tr, "Transform.get_position");
    bool ok = false;
    Il2CppObject* b = Call(trInverseTransformPoint, plotTr, {&world}, "Transform.InverseTransformPoint", &ok);
    if (!ok || !b) return false;
    out = Value<Vec3>(b);
    return std::isfinite(out.x) && std::isfinite(out.z);
}

// Turn the (inactive, root at identity) clone so that its collector stands in a plot corner no
// vanilla part uses. The occupied corners come from the live plot's own children (names from the
// coop prefab), each with the prefab layout as fallback: vanilla elder collector (+x, +z), plot
// menu terminal (-x, -z), vitamizer (-x, +z).
CoopWorld::Impl::Corner CoopWorld::Impl::PickCorner(Il2CppObject* plot, Il2CppObject* pc) {
    Corner c;
    Il2CppObject* plotTr = unity::TransformOf(plot);
    Il2CppObject* collectPt = offPcCollectPt ? unity::Field<Il2CppObject*>(pc, offPcCollectPt) : nullptr;
    Vec3 own{};
    if (!trFind || !PlotLocal(plotTr, unity::IsAlive(collectPt) ? collectPt : unity::TransformOf(pc), own)) {
        c.log = "placement methods / CollectPt missing - vanilla corner kept";
        return c;
    }
    struct Part {
        const char* what;
        const char* path;
        Quadrant fallback;
    };
    static const Part kParts[] = {
        {"vanilla elder collector", "Deluxe/ElderCollector/Collector Unit/CollectionPoint", {1, 1}},
        {"plot menu", "techActivator", {-1, -1}},
        {"vitamizer", "Vitamizer/objCoopVitamizer", {-1, 1}},
    };
    std::vector<Quadrant> used;
    std::string parts;
    for (const Part& part : kParts) {
        bool ok = false;
        Il2CppObject* tr = Call(trFind, plotTr, {unity::NewString(part.path)}, "Transform.Find", &ok);
        Vec3 v{};
        const bool found = ok && unity::IsAlive(tr) && PlotLocal(plotTr, tr, v);
        const Quadrant q = found ? QuadrantOf(v) : part.fallback;
        used.push_back(q);
        parts += std::format("{}{} {}{}", parts.empty() ? "" : ", ", part.what, Name(q),
                             found ? "" : " (not found, prefab layout)");
    }
    static const Quadrant kOrder[] = {{1, -1}, {-1, 1}, {1, 1}, {-1, -1}};
    Quadrant target = kOrder[0];
    for (const Quadrant& q : kOrder)
        if (std::find(used.begin(), used.end(), q) == used.end()) {
            target = q;
            break;
        }
    for (int k = 0; k < 4; ++k) {
        const Quat r = Yaw(90.0f * k);
        if (QuadrantOf(Rotate(r, own)) == target) {
            c.ok = true;
            c.rot = r;
            c.log = std::format("collector turned {} deg into the {} corner (unturned {}); occupied: {}", 90 * k,
                                Name(target), Name(QuadrantOf(own)), parts);
            return c;
        }
    }
    c.log = "no turn reaches the free corner - vanilla corner kept";
    return c;
}

// One-shot diagnostic: where the clone ended up and whether it renders.
void CoopWorld::Impl::LogPlacement(Il2CppObject* plot, Il2CppObject* clone, const char* when) {
    Il2CppObject* root = unity::TransformOf(clone);
    Il2CppObject* plotTr = unity::TransformOf(plot);
    if (!unity::IsAlive(root) || !unity::IsAlive(plotTr)) return;
    const Vec3 pp = GetVec(trGetPosition, plotTr, "Transform.get_position");
    const Vec3 rp = GetVec(trGetPosition, root, "Transform.get_position");
    const Vec3 rs = GetVec(trGetLocalScale, root, "Transform.get_localScale");
    bool ok = false;
    const int children = Value<int32_t>(Call(trChildCount, root, {}, "Transform.get_childCount", &ok));
    int total = 0, enabled = 0, visible = 0;
    Vec3 lo{1e9f, 1e9f, 1e9f}, hi{-1e9f, -1e9f, -1e9f};
    if (rendererClass && compsInChildren) {
        bool inactive = true;
        Il2CppObject* arr = Call(compsInChildren, root, {unity::TypeOf(rendererClass), &inactive},
                                 "GetComponentsInChildren(Renderer)", &ok);
        for (size_t i = 0, n = ok ? unity::ArrayLength(arr) : 0; i < n && i < 256; ++i) {
            auto* r = static_cast<Il2CppObject*>(unity::ArrayAt(arr, i));
            if (!unity::IsAlive(r) || !IsA(r, rendererClass)) continue;
            ++total;
            if (!Value<bool>(Call(rendEnabled, r, {}, "Renderer.get_enabled", &ok)) || !ok) continue;
            if (!unity::ActiveInHierarchy(unity::GameObjectOf(r))) continue;
            ++enabled;
            if (Value<bool>(Call(rendVisible, r, {}, "Renderer.get_isVisible", &ok)) && ok) ++visible;
            Il2CppObject* b = Call(rendBounds, r, {}, "Renderer.get_bounds", &ok);
            if (!ok || !b) continue;
            const Bounds bb = Value<Bounds>(b);
            lo = {std::min(lo.x, bb.center.x - bb.extents.x), std::min(lo.y, bb.center.y - bb.extents.y),
                  std::min(lo.z, bb.center.z - bb.extents.z)};
            hi = {std::max(hi.x, bb.center.x + bb.extents.x), std::max(hi.y, bb.center.y + bb.extents.y),
                  std::max(hi.z, bb.center.z + bb.extents.z)};
        }
    }
    const bool any = enabled > 0 && lo.x <= hi.x;
    // Plot-local centre (the corner as the plot sees it) and whether the vanilla deluxe parts are built.
    std::string local = "?";
    if (any && trInverseTransformPoint) {
        Vec3 mid{0.5f * (lo.x + hi.x), 0.5f * (lo.y + hi.y), 0.5f * (lo.z + hi.z)};
        Il2CppObject* b = Call(trInverseTransformPoint, plotTr, {&mid}, "Transform.InverseTransformPoint", &ok);
        if (ok && b) {
            const Vec3 v = Value<Vec3>(b);
            local = std::format("({:.2f}, {:.2f}, {:.2f}) {}", v.x, v.y, v.z, Name(QuadrantOf(v)));
        }
    }
    std::string deluxe = "?";
    if (trFind) {
        Il2CppObject* d = Call(trFind, plotTr, {unity::NewString("Deluxe")}, "Transform.Find", &ok);
        if (ok && unity::IsAlive(d)) deluxe = unity::ActiveInHierarchy(unity::GameObjectOf(d)) ? "built" : "not built";
    }
    SR2_INFO("[{}] hen collector placement ({}): plot at ({:.2f}, {:.2f}, {:.2f}), clone root at ({:.2f}, {:.2f}, {:.2f}) "
             "scale ({:.2f}, {:.2f}, {:.2f}), activeInHierarchy {}, {} child(ren), renderers {} (enabled+active {}, "
             "visible to a camera {}), render bounds {}; plot-local centre {}, vanilla deluxe parts {}",
             module, when, pp.x, pp.y, pp.z, rp.x, rp.y, rp.z, rs.x, rs.y, rs.z, unity::ActiveInHierarchy(clone),
             children, total, enabled, visible,
             any ? std::format("({:.2f}, {:.2f}, {:.2f})..({:.2f}, {:.2f}, {:.2f}) = plot-relative centre ({:.2f}, {:.2f}, "
                               "{:.2f})",
                               lo.x, lo.y, lo.z, hi.x, hi.y, hi.z, 0.5f * (lo.x + hi.x) - pp.x, 0.5f * (lo.y + hi.y) - pp.y,
                               0.5f * (lo.z + hi.z) - pp.z)
                 : std::string("none"),
             local, deluxe);
}

void CoopWorld::Impl::CreateHen(Il2CppObject* plot, CoopRequest& req, Il2CppObject* lookup, unsigned generation) {
    if (henBroken || !EnsureHenAssets(lookup)) {
        if (!loggedNoHen) SR2_WARN("[{}] hen collector: not available (see above)", module);
        loggedNoHen = true;
        return;
    }
    Il2CppObject* src = DeluxeSource(lookup);
    if (!src) return;
    Il2CppObject* clone = unity::Instantiate(src, unity::TransformOf(plot));
    if (!unity::IsAlive(clone) || !IsA(clone, gameObjectClass)) {
        SR2_WARN("[{}] hen collector: Instantiate failed", module);
        henBroken = true;
        return;
    }
    unity::SetName(clone, std::format("{}{}#{}", kPrefix, kHenKind, generation));
    bool ok = false;
    if (Value<bool>(Call(goActiveSelf, clone, {}, "GameObject.get_activeSelf", &ok)) || !ok) {
        // The source must be the inactive "Deluxe" object: its scripts may not wake up unmodified.
        SR2_WARN("[{}] hen collector: deluxe parts are active in the prefab - not built", module);
        DestroyNow(clone);
        henBroken = true;
        return;
    }
    Il2CppObject* root = unity::TransformOf(clone);
    Il2CppObject* storage = InChildren(root, siloStorageClass, true);
    Il2CppObject* pc = InChildren(root, plortCollectorClass, true);
    if (!storage || !pc) {
        SR2_WARN("[{}] hen collector: SiloStorage / PlortCollector missing in the deluxe parts", module);
        DestroyNow(clone);
        henBroken = true;
        return;
    }
    // Keep only the collector branch (drops the deluxe TV), and drop its button (it would start
    // the disabled vanilla collector).
    Il2CppObject* storageTr = unity::TransformOf(storage);
    for (Il2CppObject* child : ChildTransforms(root)) {
        bool keep = child == storageTr;
        for (Il2CppObject* t = storageTr; !keep && unity::IsAlive(t);) {
            Il2CppObject* parent = Call(trGetParent, t, {}, "Transform.get_parent", &ok);
            if (!ok || parent == root || !unity::IsAlive(parent)) break;
            keep = parent == child;
            t = parent;
        }
        if (!keep) DestroyNow(unity::GameObjectOf(child));
    }
    if (Il2CppObject* act = InChildren(root, collectorActivatorClass, true)) {
        Il2CppObject* actTr = unity::TransformOf(act);
        Il2CppObject* parent = Call(trGetParent, actTr, {}, "Transform.get_parent", &ok);
        Il2CppObject* doomed = (ok && unity::IsAlive(parent) && parent != storageTr && parent != root) ? parent : actTr;
        DestroyNow(unity::GameObjectOf(doomed));
    }
    // Storage: our slot definition in every slot (local AmmoSlotManager built by Awake).
    void* defs = unity::Field<void*>(storage, offStorageDefs);
    Il2CppObject* srcStorage = InChildren(unity::TransformOf(src), siloStorageClass, true);
    if (!defs || (srcStorage && unity::Field<void*>(srcStorage, offStorageDefs) == defs)) {
        // The array must be the clone's own copy, never the prefab's (that would change the vanilla
        // elder collector of every deluxe coop built afterwards).
        SR2_WARN("[{}] hen collector: cloned storage shares the prefab's slot array - not built", module);
        DestroyNow(clone);
        henBroken = true;
        return;
    }
    const size_t nDefs = unity::ArrayLength(defs);
    Il2CppObject** data = unity::ArrayData<Il2CppObject*>(defs);
    for (size_t i = 0; data && i < nDefs && i < 8; ++i)
        SetRef(static_cast<Il2CppObject*>(defs), reinterpret_cast<void**>(&data[i]), henSlotDef);
    bool off = false;
    Call(behSetEnabled, pc, {&off}, "Behaviour.set_enabled");  // our tick collects, not its FixedUpdate
    // Placement relative to the live plot: root at the plot origin, unturned, then turned into a free
    // corner (PickCorner). Fallback: the vanilla container's -90 deg (the vanilla elder collector's
    // corner), only when no free corner can be worked out.
    Vec3 zero{0.0f, 0.0f, 0.0f}, one{1.0f, 1.0f, 1.0f};
    if (trSetLocalPosition) Call(trSetLocalPosition, root, {&zero}, "Transform.set_localPosition");
    SetVec(trSetLocalScale, root, one, "Transform.set_localScale");
    Quat q{0.0f, 0.0f, 0.0f, 1.0f};
    Call(trSetLocalRotation, root, {&q}, "Transform.set_localRotation");
    const Corner corner = PickCorner(plot, pc);
    q = corner.ok ? corner.rot : Quat{0.0f, -0.70710678f, 0.0f, 0.70710678f};
    Call(trSetLocalRotation, root, {&q}, "Transform.set_localRotation");
    SR2_INFO("[{}] hen collector corner on plot '{}': {}", module, req.id, corner.log);
    if (sr2::core::ModuleFaulted(module) || !LinkLocalModel(storage, nDefs)) {
        if (!sr2::core::ModuleFaulted(module)) SR2_WARN("[{}] hen collector: storage model not set up - not built", module);
        DestroyNow(clone);
        henBroken = true;
        return;
    }
    unity::SetActive(clone, true);  // Awake: collector refs, catchers, slot UIs (storage already has its model)
    const bool vac = EnableVacuum(pc, req.id);
    LogPlacement(plot, clone, "built");
    if (req.hens) Restore(storage, *req.hens);
    int stored = 0;
    if (req.hens)
        for (const HenSlot& s : *req.hens) stored += std::max(s.count, 0);
    timers[req.id] = PlotTimer{};
    SR2_INFO("[{}] hen collector built on plot '{}' ({} slot(s) x {}, {} chicken(s) restored, {})", module, req.id, nDefs,
             henMax, stored, vac ? "vacuum: the elder collector's own suction" : "no vacuum: stored directly");
}

bool CoopWorld::Impl::SetupVacuum(Resolver& R) {
    std::string err, miss;
    auto need = [&](bool ok, const char* what) {
        if (!ok && miss.empty()) miss = what;
    };
    auto off = [&](Il2CppClass* k, const char* field, size_t& out) {
        std::string e;
        auto o = k ? R.FieldOffset(k, field, &e) : std::nullopt;
        if (o) out = *o;
        need(o.has_value(), field);
    };
    rigidbodyClass = R.Class(kPhysics, "UnityEngine", "Rigidbody", &err);
    springJointClass = R.Class(kPhysics, "UnityEngine", "SpringJoint", &err);
    Il2CppClass* joint = R.Class(kPhysics, "UnityEngine", "Joint", &err);
    Il2CppClass* safe = R.Class(kImage, "", "SafeJointReference", &err);
    landPlotModelClass = R.Class(kImage, "MonomiPark.SlimeRancher.DataModel", "LandPlotModel", &err);
    need(rigidbodyClass && springJointClass && joint && safe && landPlotModelClass, "physics / LandPlotModel classes");
    // PlortCollector.JointReference: a private nested class (no name lookup through the image).
    using NestedFn = Il2CppClass* (*)(Il2CppClass*, void**);
    auto nested = reinterpret_cast<NestedFn>(api().module ? GetProcAddress(api().module, "il2cpp_class_get_nested_types") : nullptr);
    void* iter = nullptr;
    for (Il2CppClass* k = nested ? nested(plortCollectorClass, &iter) : nullptr; k && api().il2cpp_class_get_name;
         k = nested(plortCollectorClass, &iter)) {
        const char* n = api().il2cpp_class_get_name(k);
        if (n && std::string_view(n) == "JointReference") {
            jointRefClass = k;
            break;
        }
    }
    need(jointRefClass != nullptr, "PlortCollector.JointReference");
    if (!miss.empty()) {
        SR2_WARN("[{}] hen collector vacuum unavailable ({}) - chickens are stored directly", module, miss);
        return false;
    }
    goCtor = R.MethodByParams(gameObjectClass, ".ctor", {"System.String"}, &err);
    goAddComponent = R.MethodByParams(gameObjectClass, "AddComponent", {"System.Type"}, &err);
    trSetParent = R.MethodByParams(transformClass, "SetParent", {"UnityEngine.Transform", "System.Boolean"}, &err);
    rbSetKinematic = R.Method(rigidbodyClass, "set_isKinematic", 1, &err);
    rbWakeUp = R.Method(rigidbodyClass, "WakeUp", 0, &err);
    sjSetSpring = R.Method(springJointClass, "set_spring", 1, &err);
    sjSetMaxDistance = R.Method(springJointClass, "set_maxDistance", 1, &err);
    jSetAutoAnchor = R.Method(joint, "set_autoConfigureConnectedAnchor", 1, &err);
    jSetAnchor = R.Method(joint, "set_connectedAnchor", 1, &err);
    jGetBody = R.Method(joint, "get_connectedBody", 0, &err);
    attachSafely = R.MethodByParams(safe, "AttachSafely", {"UnityEngine.GameObject", "UnityEngine.Joint", "System.Boolean"}, &err);
    vacCapture = R.MethodByParams(vacuumableClass, "Capture", {"UnityEngine.Joint"}, &err);
    identMeta = R.Method(identifiableClass, "get_AmmoMetadata", 0, &err);
    lpmCtor = R.Method(landPlotModelClass, ".ctor", 0, &err);
    jrDestroy = R.Method(jointRefClass, "Destroy", 0, &err);
    need(goCtor && goAddComponent && trSetParent && rbSetKinematic && rbWakeUp && sjSetSpring && sjSetMaxDistance &&
             jSetAutoAnchor && jSetAnchor && jGetBody && attachSafely && vacCapture && identMeta && lpmCtor && jrDestroy &&
             objectNew && trSetLocalPosition,
         "joint / capture methods");
    off(plortCollectorClass, "_model", offPcModel);
    off(plortCollectorClass, "_region", offPcRegion);
    off(plortCollectorClass, "_joints", offPcJoints);
    off(plortCollectorClass, "_endCollectAt", offPcEnd);
    off(plortCollectorClass, "_forceCollectUntil", offPcForce);
    off(plortCollectorClass, "CollectSpringiness", offPcSpring);
    off(plortCollectorClass, "_storage", offPcStorage);
    off(plortCollectorClass, "_vacAudio", offPcAudio);
    off(landPlotModelClass, "collectorNextTime", offLpmNext);
    off(jointRefClass, "AmmoMetadata", offJrMeta);
    off(jointRefClass, "Vacuumable", offJrVac);
    off(jointRefClass, "Joint", offJrJoint);
    need(offPcCollectPt != 0, "CollectPt");
    if (!miss.empty()) {
        SR2_WARN("[{}] hen collector vacuum unavailable ({}) - chickens are stored directly", module, miss);
        return false;
    }
    return true;
}

// After activation (Awake has set _region / _storage / _timeDir / CollectAnim; the constructor
// _joints): a private LandPlotModel whose collectorNextTime never comes, then the component on, so
// the vanilla FixedUpdate reels in, stores, puffs and animates whatever joints we hand it.
bool CoopWorld::Impl::EnableVacuum(Il2CppObject* pc, const std::string& id) {
    if (!vacuum) return false;
    Il2CppObject* collectPt = unity::Field<Il2CppObject*>(pc, offPcCollectPt);
    // FixedUpdate dereferences all of these without a check (NRE every physics frame otherwise).
    const bool parts = unity::Field<Il2CppObject*>(pc, offPcRegion) && unity::Field<Il2CppObject*>(pc, offPcTimeDir) &&
                       unity::Field<void*>(pc, offPcJoints) && unity::Field<Il2CppObject*>(pc, offPcStorage) &&
                       unity::Field<Il2CppObject*>(pc, offPcAudio) && unity::IsAlive(collectPt);
    if (!parts) {
        if (!loggedNoVacuum) SR2_WARN("[{}] hen collector on plot '{}': collector has no Region / TimeDirector / storage / audio / "
                                      "CollectPt - "
                                      "chickens are stored directly", module, id);
        loggedNoVacuum = true;
        return false;
    }
    Il2CppObject* model = objectNew(landPlotModelClass);
    bool ok = false;
    if (model) Call(lpmCtor, model, {}, "LandPlotModel..ctor", &ok);
    if (!ok) return false;
    unity::Field<double>(model, offLpmNext) = 1e300;  // FixedUpdate's own DoCollection never fires
    SetRef(pc, reinterpret_cast<void**>(reinterpret_cast<char*>(pc) + offPcModel), model);
    bool on = true;
    Call(behSetEnabled, pc, {&on}, "Behaviour.set_enabled", &ok);
    if (!ok) {
        SetRef(pc, reinterpret_cast<void**>(reinterpret_cast<char*>(pc) + offPcModel), nullptr);
        return false;
    }
    return true;
}

// One chicken onto the collector's spring, like DoCollection. false = nothing attached (the caller
// stores it directly instead).
bool CoopWorld::Impl::Suck(Il2CppObject* pc, Il2CppObject* ident, Il2CppObject* vac) {
    Il2CppObject* collectPt = unity::Field<Il2CppObject*>(pc, offPcCollectPt);
    void* joints = unity::Field<void*>(pc, offPcJoints);
    Il2CppObject* birdGo = unity::GameObjectOf(ident);
    if (!unity::IsAlive(collectPt) || !joints || !unity::IsAlive(vac) || !unity::IsAlive(birdGo)) return false;
    std::string err;
    const MethodInfo* listAdd =
        Resolver::Get().Method(api().il2cpp_object_get_class(static_cast<Il2CppObject*>(joints)), "Add", 1, &err);
    // get_AmmoMetadata is virtual (DoCollection calls it through the vtable); runtime_invoke does not
    // dispatch, so take the override of the chicken's own class.
    const MethodInfo* metaMi = nullptr;
    for (Il2CppClass* k = api().il2cpp_object_get_class(ident); k && !metaMi && k != api().il2cpp_class_get_parent(identifiableClass);
         k = api().il2cpp_class_get_parent ? api().il2cpp_class_get_parent(k) : nullptr)
        metaMi = Resolver::Get().Method(k, "get_AmmoMetadata", 0, &err);
    bool ok = false;
    Il2CppObject* meta = Call(metaMi ? metaMi : identMeta, ident, {}, "Identifiable.get_AmmoMetadata", &ok);
    if (!ok || !meta || !listAdd) return false;
    Il2CppObject* go = objectNew(gameObjectClass);
    if (!go) return false;
    Call(goCtor, go, {unity::NewString("CollectJoint")}, "GameObject..ctor", &ok);
    if (!ok || !unity::IsAlive(go)) return false;
    auto add = [&](Il2CppClass* k, const char* what) -> Il2CppObject* {
        bool good = false;
        Il2CppObject* c = Call(goAddComponent, go, {unity::TypeOf(k)}, what, &good);
        return good && unity::IsAlive(c) && IsA(c, k) ? c : nullptr;
    };
    Il2CppObject* rb = add(rigidbodyClass, "AddComponent(Rigidbody)");
    bool kinematic = true, stays = false, autoAnchor = false, canDestroy = true;
    Vec3 zero{0.0f, 0.0f, 0.0f};
    if (rb) Call(rbSetKinematic, rb, {&kinematic}, "Rigidbody.set_isKinematic", &ok);
    Il2CppObject* tr = unity::TransformOf(go);
    if (rb && ok) Call(trSetParent, tr, {collectPt, &stays}, "Transform.SetParent", &ok);
    if (rb && ok) Call(trSetLocalPosition, tr, {&zero}, "Transform.set_localPosition", &ok);
    Il2CppObject* joint = rb && ok ? add(springJointClass, "AddComponent(SpringJoint)") : nullptr;
    if (!joint) {
        Destroy(go);
        return false;
    }
    float spring = unity::Field<float>(pc, offPcSpring);
    const Vec3 a = GetVec(trGetPosition, unity::TransformOf(ident), "Transform.get_position");
    const Vec3 b = GetVec(trGetPosition, collectPt, "Transform.get_position");
    float dist = std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z));
    Call(sjSetSpring, joint, {&spring}, "SpringJoint.set_spring", &ok);
    if (ok) Call(sjSetMaxDistance, joint, {&dist}, "SpringJoint.set_maxDistance", &ok);
    if (ok) Call(jSetAutoAnchor, joint, {&autoAnchor}, "Joint.set_autoConfigureConnectedAnchor", &ok);
    if (ok) Call(jSetAnchor, joint, {&zero}, "Joint.set_connectedAnchor", &ok);
    if (ok) Call(attachSafely, nullptr, {birdGo, joint, &canDestroy}, "SafeJointReference.AttachSafely", &ok);
    if (!ok || sr2::core::ModuleFaulted(module)) {
        Destroy(go);
        return false;
    }
    if (Il2CppObject* body = Call(jGetBody, joint, {}, "Joint.get_connectedBody", &ok); ok && unity::IsAlive(body))
        Call(rbWakeUp, body, {}, "Rigidbody.WakeUp");
    Call(vacCapture, vac, {joint}, "Vacuumable.Capture", &ok);
    if (!ok) {
        Destroy(go);
        return false;
    }
    // The JointReference FixedUpdate works through (its constructor is empty).
    Il2CppObject* ref = objectNew(jointRefClass);
    if (ref) {
        char* base = reinterpret_cast<char*>(ref);
        std::memcpy(base + offJrMeta, reinterpret_cast<char*>(meta) + 2 * sizeof(void*), kAmmoMetadataSize);
        // AmmoMetadata.Id (+0) is its only reference: through the write barrier.
        SetRef(ref, reinterpret_cast<void**>(base + offJrMeta), *reinterpret_cast<Il2CppObject**>(base + offJrMeta));
        SetRef(ref, reinterpret_cast<void**>(base + offJrVac), vac);
        SetRef(ref, reinterpret_cast<void**>(base + offJrJoint), joint);
        Call(listAdd, joints, {ref}, "List<JointReference>.Add", &ok);
    }
    if (!ref || !ok) {
        if (ref) Call(jrDestroy, ref, {}, "JointReference.Destroy");  // joint gone, chicken released
        else Destroy(go);
        return false;
    }
    return true;
}

// Chickens on the clone's springs are let go (JointReference.Destroy: joint destroyed,
// Vacuumable.Release) before the clone goes, so none stays captive on a destroyed joint.
void CoopWorld::Impl::ReleaseJoints(Il2CppObject* clone) {
    if (!vacuum || !unity::IsAlive(clone)) return;
    Il2CppObject* pc = InChildren(unity::TransformOf(clone), plortCollectorClass, true);
    if (!pc || !Vacuuming(pc)) return;
    void* joints = unity::Field<void*>(pc, offPcJoints);
    const size_t n = unity::ListCount(joints);
    if (!n) return;
    std::vector<Il2CppObject*> refs;
    for (size_t i = 0; i < n && i < 512; ++i)
        if (auto* r = static_cast<Il2CppObject*>(unity::ListAt(joints, i))) refs.push_back(r);
    std::string err;
    const MethodInfo* clear =
        Resolver::Get().Method(api().il2cpp_object_get_class(static_cast<Il2CppObject*>(joints)), "Clear", 0, &err);
    if (clear) Call(clear, joints, {}, "List<JointReference>.Clear");
    for (Il2CppObject* r : refs) Call(jrDestroy, r, {}, "JointReference.Destroy");
    SR2_INFO("[{}] hen collector: {} chicken(s) released from the vacuum", module, refs.size());
}

int CoopWorld::Impl::Collect(Il2CppObject* clone, Il2CppObject* pc, Il2CppObject* ammo, const CoopInfo& c, int keep,
                             int* adultsOut) {
    (void)clone;
    *adultsOut = 0;
    if (!c.hasRegion) return 0;
    Vec3 center = c.region.center, half = c.region.extents;
    Quat rot{0, 0, 0, 1};
    int32_t mask = -1, qti = kQueryTriggerIgnore;
    bool ok = false;
    Il2CppObject* cols = Call(overlapBox, nullptr, {&center, &half, &rot, &mask, &qti}, "Physics.OverlapBox", &ok);
    if (!ok) return 0;
    struct Bird {
        Il2CppObject* ident;
        Il2CppObject* type;
        bool rooster;
    };
    std::vector<Bird> birds;
    for (size_t i = 0, n = unity::ArrayLength(cols); i < n && i < 512; ++i) {
        auto* col = static_cast<Il2CppObject*>(unity::ArrayAt(cols, i));
        if (!unity::IsAlive(col)) continue;
        Il2CppObject* ident = InParent(col, identifiableClass);
        if (!ident || std::any_of(birds.begin(), birds.end(), [&](const Bird& b) { return b.ident == ident; })) continue;
        Il2CppObject* type = unity::Field<Il2CppObject*>(ident, offIdentType);
        if (!IsAdult(type)) continue;  // chicks, elders, everything else
        Il2CppObject* vac = unity::GetComponent(ident, vacuumableClass);
        if (vac && Value<bool>(Call(isCaptive, vac, {}, "Vacuumable.IsCaptive", &ok))) continue;  // held by a vac
        birds.push_back({ident, type, unity::GetName(type).find("Rooster") != std::string::npos});
    }
    *adultsOut = static_cast<int>(birds.size());
    int excess = static_cast<int>(birds.size()) - keep;
    if (excess <= 0) return 0;
    int roosters = static_cast<int>(std::count_if(birds.begin(), birds.end(), [](const Bird& b) { return b.rooster; }));
    const int32_t nSlots = static_cast<int32_t>(unity::ArrayLength(Call(ammoSlots, ammo, {}, "AmmoSlotManager.get_Slots", &ok)));
    Il2CppObject* fx = unity::Field<Il2CppObject*>(pc, offPcFx);
    // The slots as they will be once the chickens on their way are stored (vacuum: stored later by
    // the collector's FixedUpdate, into the slot holding that type, else an empty one).
    struct Slot {
        Il2CppObject* type;
        int32_t count, max;
    };
    std::vector<Slot> slots;
    for (int32_t i = 0; i < nSlots && i < 8; ++i) {
        const int32_t cnt = Value<int32_t>(Call(slotCount, ammo, {&i}, "AmmoSlotManager.GetSlotCount", &ok));
        const int32_t max = Value<int32_t>(Call(slotMax, ammo, {&i}, "AmmoSlotManager.GetSlotMaxCount", &ok));
        Il2CppObject* t = cnt > 0 ? Call(slotName, ammo, {&i}, "AmmoSlotManager.GetSlotName", &ok) : nullptr;
        slots.push_back({cnt > 0 ? t : nullptr, cnt, max});
    }
    const bool vac = Vacuuming(pc);
    int collected = 0;
    for (const Bird& b : birds) {
        if (excess <= 0 || sr2::core::ModuleFaulted(module)) break;
        if (b.rooster && roosters <= kKeepRoosters) continue;
        int32_t slot = -1;
        for (int32_t i = 0; i < static_cast<int32_t>(slots.size()); ++i) {
            if (slots[i].count > 0 && slots[i].type == b.type && slots[i].count < slots[i].max) {
                slot = i;
                break;
            }
            if (slots[i].count == 0 && slot < 0) slot = i;
        }
        if (slot < 0) continue;  // storage full for this type
        if (vac && Suck(pc, b.ident, unity::GetComponent(b.ident, vacuumableClass))) {
            slots[slot].type = b.type;
            ++slots[slot].count;
            ++collected;
            --excess;
            if (b.rooster) --roosters;
            continue;
        }
        if (sr2::core::ModuleFaulted(module)) break;
        int32_t one = 1;
        bool overflow = false;
        if (!Value<bool>(Call(addResource, ammo, {b.type, &slot, &one, &overflow}, "AmmoSlotManager.MaybeAddResource", &ok)))
            continue;
        slots[slot].type = b.type;
        ++slots[slot].count;
        Il2CppObject* tr = unity::TransformOf(b.ident);
        Il2CppObject* go = unity::GameObjectOf(b.ident);
        if (unity::IsAlive(fx) && unity::IsAlive(tr)) {
            Vec3 pos = GetVec(trGetPosition, tr, "Transform.get_position");
            Il2CppObject* boxed = Call(trGetRotation, tr, {}, "Transform.get_rotation", &ok);
            Quat r = ok && boxed ? Value<Quat>(boxed) : Quat{0, 0, 0, 1};
            Call(spawnFx, nullptr, {fx, &pos, &r}, "FXHelpers.SpawnAndPlayFX");
        }
        bool okIfNonActor = false;
        Call(destroyActor, nullptr, {go, unity::NewString("sr2mods.HenCollector"), &okIfNonActor}, "Destroyer.DestroyActor",
             &ok);
        ++collected;
        --excess;
        if (b.rooster) --roosters;
    }
    if (vac && PendingJoints(pc) > 0) {
        // DoCollection's tail: how long the cyclone shows at least, when unfinished joints are dropped.
        Il2CppObject* timeDir = unity::Field<Il2CppObject*>(pc, offPcTimeDir);
        float force = kForceCollectHours, end = kEndCollectHours;
        const double untilForce = Value<double>(Call(hoursFromNow, timeDir, {&force}, "TimeDirector.HoursFromNow", &ok));
        if (ok) unity::Field<double>(pc, offPcForce) = untilForce;
        const double untilEnd = Value<double>(Call(hoursFromNow, timeDir, {&end}, "TimeDirector.HoursFromNow", &ok));
        if (ok) unity::Field<double>(pc, offPcEnd) = untilEnd;
    }
    return collected;
}

void CoopWorld::Impl::ServeHen(Il2CppObject* clone, const CoopInfo& c, CoopRequest& req) {
    Il2CppObject* storage = nullptr;
    Il2CppObject* ammo = nullptr;
    std::vector<HenSlot> live;
    if (!ReadSlots(clone, live, &ammo, &storage)) return;
    if (unity::Field<Il2CppObject*>(storage, offStorageModel) != nullptr) {
        // Never expected (see the header): the storage was linked to the plot's saved model.
        if (!loggedLinked) SR2_ERROR("[{}] hen collector on plot '{}': storage got linked to the plot model - not "
                                     "collecting", module, req.id);
        loggedLinked = true;
        return;
    }
    if (req.hens && live != *req.hens) *req.hens = live;
    Il2CppObject* pc = InChildren(unity::TransformOf(clone), plortCollectorClass, true);
    if (!pc) return;
    // Vacuum: the collector's FixedUpdate animates the cyclone; direct storing: off between bursts.
    const bool vac = Vacuuming(pc);
    Il2CppObject* anim = unity::Field<Il2CppObject*>(pc, offPcAnim);
    int32_t cyclone = unity::Field<int32_t>(pc, offPcCycloneId);
    bool flag = false;
    if (!vac && unity::IsAlive(anim)) Call(animSetBool, anim, {&cyclone, &flag}, "Animator.SetBool");

    PlotTimer& t = timers[req.id];
    if (++t.age < kWarmupTicks) return;
    if (!t.diagLogged) {
        t.diagLogged = true;
        LogPlacement(c.plot, clone, "in the world");  // isVisible now reflects the cameras
    }
    Il2CppObject* timeDir = unity::Field<Il2CppObject*>(pc, offPcTimeDir);
    if (!timeDir || PendingJoints(pc) > 0) return;  // chickens still on their way in
    bool ok = false;
    if (t.next > 0.0 && !Value<bool>(Call(hasReached, timeDir, {&t.next}, "TimeDirector.HasReached", &ok))) return;
    int adultsIn = 0;
    const int keep = std::clamp(req.keepAdults, std::max(kHenKeepMin, kKeepRoosters), kHenKeepMax);
    const int n = Collect(clone, pc, ammo, c, keep, &adultsIn);
    if (adultsIn <= keep) return;  // nothing over the limit: check again next tick
    float period = unity::Field<float>(pc, offPcPeriod);
    if (!std::isfinite(period) || period <= 0.0f) period = 6.0f;
    t.next = Value<double>(Call(hoursFromNow, timeDir, {&period}, "TimeDirector.HoursFromNow", &ok));
    if (n > 0 && !vac) {
        flag = true;
        if (unity::IsAlive(anim)) Call(animSetBool, anim, {&cyclone, &flag}, "Animator.SetBool");
        if (req.hens) ReadSlots(clone, *req.hens);
    }
    SR2_INFO("[{}] hen collector on plot '{}': {} adult(s) in the coop (limit {}), {} {}; next in {:.1f} game hours",
             module, req.id, adultsIn, keep, vac ? "vacuuming" : "stored directly", n, period);
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
        if (req.henCollector && !have.hen) p.CreateHen(c.plot, req, getLookup(), generation_);
        else if (!req.henCollector && have.hen) {
            p.ReleaseJoints(have.hen);
            p.Destroy(have.hen);
            SR2_INFO("[{}] hen collector removed from plot '{}' (not owned)", p.module, req.id);
        } else if (have.hen) p.ServeHen(have.hen, c, req);
        if (sr2::core::ModuleFaulted(p.module)) return;
    }
}

void CoopWorld::Snapshot(const QueryFn& query) {
    if (!ready_ || !impl_) return;
    for (const Impl::CoopInfo& c : impl_->Coops()) {
        CoopRequest req;
        if (!query(c.plot, req) || !req.henCollector) continue;
        Impl::Children have = impl_->Scan(c.plot, generation_, false);
        if (have.hen) impl_->SnapshotInto(have.hen, req);
    }
}

void CoopWorld::RemoveAll(const QueryFn* query) {
    if (!ready_ || !impl_) return;
    for (const Impl::CoopInfo& c : impl_->Coops()) {
        if (query) {
            CoopRequest req;
            if ((*query)(c.plot, req) && req.henCollector) {
                Impl::Children have = impl_->Scan(c.plot, generation_, false);
                if (have.hen) impl_->SnapshotInto(have.hen, req);
            }
        }
        if (!sr2::core::ModuleFaulted(impl_->module)) {
            Impl::Children have = impl_->Scan(c.plot, generation_, false);
            if (have.hen) impl_->ReleaseJoints(have.hen);
        }
        impl_->Scan(c.plot, generation_, true, true);
    }
}

int CoopWorld::StoredHens(Il2CppObject* plot) {
    if (!ready_ || !impl_ || !unity::IsAlive(plot)) return -1;
    Impl::Children have = impl_->Scan(plot, generation_, false);
    std::vector<HenSlot> live;
    if (!have.hen || !impl_->ReadSlots(have.hen, live)) return -1;
    int n = 0;
    for (const HenSlot& s : live) n += std::max(s.count, 0);
    return n;
}

void CoopWorld::Remove(Il2CppObject* plot, bool airNet, bool henCollector) {
    if (!ready_ || !impl_ || !unity::IsAlive(plot)) return;
    Impl::Children have = impl_->Scan(plot, generation_, true);
    if (airNet && have.airNet) {
        impl_->Destroy(have.airNet);
        SR2_INFO("[{}] coop air net removed (sold)", impl_->module);
    }
    if (henCollector && have.hen) {
        impl_->ReleaseJoints(have.hen);
        impl_->Destroy(have.hen);
        SR2_INFO("[{}] hen collector removed (sold)", impl_->module);
    }
}

void CoopWorld::Reset() {
    ++generation_;
    if (impl_) impl_->timers.clear();
}

}  // namespace sr2::modules::custom_upgrades::world
