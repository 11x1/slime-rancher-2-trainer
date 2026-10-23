// custom_upgrades: decorative props for owned tier upgrades (see plot_props.h). SR2 1.3.0, all
// lookups by name.
//
// Sources (asset bundle dumps, game 1.3.0):
//   * decorHayBales (decoration gadget, identifiabletypes bundle): children "hayBale_2" and
//     "hayBale_0 (1)" (each one "hayBaleA": MeshRenderer, MeshCollider, a collision-FX script),
//     "hayStack", "pitchfork", two grass tufts, plus the gadget's Bounds / environment
//     interaction objects. We take the two bales at half size: the coop's "small hay beds".
//   * decorFlowerPillowSmall (decoration gadget): child "mesh" (LODGroup + 3 LOD renderers, no
//     collider; the colliders and the slime attachment point are siblings we do not take).
//   * toyStuffedChicken (toy, root scale 1.33): child "model_stuffedChicken" (one MeshRenderer).
//   * coop prefab "patchCoop": "Deco/nestPlain01" = the vanilla nest in the middle of every coop
//     (nestChicken01 mesh + a leaves decal).
//   * garden prefab "patchGarden": "Sprinkler/SprinklerBase" = one of the Sprinkler upgrade's two
//     heads (Animator (disabled), sprinkler + sprinklerpart meshes, 3 spray particle systems;
//     the loop sound sits on "Sprinkler", which we do not take). Vanilla heads stand in the middle
//     of the -z / +z sides (Sprinkler at y 0.11 turned 90 deg, heads at (+-4.38, -0.44, 0)).
//   * incinerator prefab "patchIncinerator": "objIncinerator/Meshes/meshIncinerator/FireTest"
//     (the fire particle system at (0, 3.55, 0), scale (0.75, 1.5, 0.75)) has two INACTIVE
//     children "Glow" (scale 0.67) and "Embers" (at -0.96, turned -90 deg about x, scale 1.4 /
//     1.2 / 1.2) that vanilla never shows. We clone and switch them on at the same place.
//   * decorFireFlower (decoration gadget): child "FX Fireflower" (two ember particle systems,
//     "Embers_01" / "Embers_02" at y 2.59 / 1.17 above the flower), set so they rise out of
//     the incinerator's opening.
//
// Placement (plot-local, plot root = the plot prefab root). Kept away from the corners: coop
// corners hold the plot menu terminal (-x, -z), the vitamizer (-x, +z), the vanilla elder
// collector (+x, +z) and, by coop_world's PickCorner, the Hen Collector (the free one); garden
// corners hold the terminal, the deluxe planters (+x, -z) / (-x, +z) and the scareslime (+x, +z),
// the deposit port stands at (-5.44, -3.14). Coop: hay bales along the +z side, nests in the
// middle of the -x / +x sides (stuffed chicken in the +x nest), pillow in the middle of the -z
// side. Garden: a third sprinkler head in the middle of the +x side (Fast Growth I or II) and a
// fourth in the middle of the -x side (Fast Growth II; the deposit port at (-5.44, -3.14) is
// outside the soil), spraying inwards like the vanilla ones. Nothing has a collider, so nothing blocks chickens, crops, the vac or the player.
//
// Holders: "sr2props_<Kind>#<generation>", a plain GameObject under the plot (identity local
// transform), made inactive before the clones go in, activated after stripping.

#include "modules/custom_upgrades/plot_props.h"

#include <windows.h>

#include <algorithm>
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
constexpr int32_t kPlotCoop = 3;
constexpr int32_t kPlotGarden = 4;
constexpr int32_t kPlotIncinerator = 7;
constexpr std::string_view kPrefix = "sr2props_";

struct Vec3 {
    float x, y, z;
};
struct Quat {
    float x, y, z, w;
};

enum class Src { kPlot, kGadget, kToy };

// One cloned piece: a child of a plot prefab (by path from the prefab root) or of a gadget / toy
// prefab (matched by the prefab's name), with its plot-local transform.
struct Part {
    Src src;
    int32_t plotType;    // kPlot: LandPlot.Id of the prefab
    const char* prefab;  // kGadget / kToy: prefab GameObject name
    const char* path;    // Transform.Find path below the prefab root
    Vec3 pos;
    Quat rot;
    Vec3 scale;
    bool activate;       // the source is inactive in its prefab: switch the clone on
};

constexpr Quat kId{0.0f, 0.0f, 0.0f, 1.0f};
constexpr Quat YawQ(float s, float c) { return {0.0f, s, 0.0f, c}; }  // (sin, cos) of half the yaw
constexpr Vec3 S(float s) { return {s, s, s}; }

const Part kHay[] = {
    {Src::kGadget, 0, "decorHayBales", "hayBale_2", {-1.3f, 0.0f, 3.8f}, YawQ(0.70710678f, 0.70710678f), S(0.5f), false},
    {Src::kGadget, 0, "decorHayBales", "hayBale_0 (1)", {1.3f, 0.0f, 3.85f}, YawQ(0.60876143f, 0.79335334f), S(0.5f),
     false},
};
const Part kNests[] = {
    {Src::kPlot, kPlotCoop, nullptr, "Deco/nestPlain01", {-3.7f, 0.0f, 0.0f}, YawQ(0.25881905f, 0.96592583f), S(1.0f),
     false},
    {Src::kPlot, kPlotCoop, nullptr, "Deco/nestPlain01", {3.7f, 0.0f, 0.0f}, YawQ(-0.34202014f, 0.93969262f), S(1.0f),
     false},
    {Src::kToy, 0, "toyStuffedChicken", "model_stuffedChicken", {3.7f, 0.1f, 0.0f}, YawQ(-0.70710678f, 0.70710678f),
     S(1.33f), false},
    {Src::kGadget, 0, "decorFlowerPillowSmall", "mesh", {0.0f, 0.0f, -3.7f}, kId, S(0.7f), false},
};
const Part kSprinkler[] = {
    {Src::kPlot, kPlotGarden, nullptr, "Sprinkler/SprinklerBase", {4.38f, -0.33f, 0.0f}, kId, S(1.0f), false},
};
const Part kSprinkler2[] = {  // the same head mirrored onto the -x side (turned 180 deg: sprays inwards)
    {Src::kPlot, kPlotGarden, nullptr, "Sprinkler/SprinklerBase", {-4.38f, -0.33f, 0.0f}, YawQ(1.0f, 0.0f), S(1.0f),
     false},
};
const Part kGlow[] = {
    {Src::kPlot, kPlotIncinerator, nullptr, "objIncinerator/Meshes/meshIncinerator/FireTest/Glow", {0.0f, 3.55f, 0.0f},
     kId, {0.5f, 1.0f, 0.5f}, true},
    {Src::kPlot, kPlotIncinerator, nullptr, "objIncinerator/Meshes/meshIncinerator/FireTest/Embers",
     {0.0f, 2.11f, 0.0f}, {-0.70710678f, 0.0f, 0.0f, 0.70710678f}, {1.05f, 0.9f, 1.8f}, true},
    {Src::kGadget, 0, "decorFireFlower", "FX Fireflower", {0.0f, 2.4f, 0.0f}, kId, S(1.0f), false},
};

struct KindDef {
    PropKind kind;
    const char* name;  // holder name part
    int32_t plotType;
    const Part* parts;
    size_t count;
};
const KindDef kKinds[] = {
    {kPropCoopHay, "CoopHay", kPlotCoop, kHay, std::size(kHay)},
    {kPropCoopNests, "CoopNests", kPlotCoop, kNests, std::size(kNests)},
    {kPropGardenSprinkler, "GardenSprinkler", kPlotGarden, kSprinkler, std::size(kSprinkler)},
    {kPropGardenSprinkler2, "GardenSprinklerII", kPlotGarden, kSprinkler2, std::size(kSprinkler2)},
    {kPropIncinGlow, "IncineratorGlow", kPlotIncinerator, kGlow, std::size(kGlow)},
};

}  // namespace

struct PlotProps::Impl {
    const char* module = "custom_upgrades";
    Il2CppClass* plotClass = nullptr;
    Il2CppClass* gameObjectClass = nullptr;
    Il2CppClass* groupClass = nullptr;      // IdentifiableTypeGroup
    Il2CppClass* identTypeClass = nullptr;  // IdentifiableType
    // stripped from every clone (game scripts = MonoBehaviours outside the UnityEngine namespace)
    Il2CppClass* monoClass = nullptr;
    Il2CppClass* jointClass = nullptr;
    Il2CppClass* rigidbodyClass = nullptr;
    Il2CppClass* colliderClass = nullptr;
    size_t offPlotType = 0, offScGameModel = 0, offModelGameObj = 0;
    size_t offGadgets = 0, offToys = 0, offMembers = 0, offSubGroups = 0, offPrefab = 0;

    const MethodInfo* sceneInstance = nullptr;  // SRSingleton<SceneContext>.get_Instance
    const MethodInfo* getPlotModel = nullptr;   // GameModel.GetLandPlotModel(string)
    const MethodInfo* getPlotPrefab = nullptr;  // LookupDirector.GetPlotPrefab(Id)
    const MethodInfo* compInChildren = nullptr;   // Component.GetComponentInChildren(Type, bool)
    const MethodInfo* compsInChildren = nullptr;  // Component.GetComponentsInChildren(Type, bool)
    const MethodInfo* trChildCount = nullptr;
    const MethodInfo* trGetChild = nullptr;
    const MethodInfo* trFind = nullptr;          // Transform.Find(string)
    const MethodInfo* trSetParent = nullptr;     // Transform.SetParent(Transform, bool)
    const MethodInfo* trSetLocalPosition = nullptr;
    const MethodInfo* trSetLocalRotation = nullptr;
    const MethodInfo* trSetLocalScale = nullptr;
    const MethodInfo* goCtor = nullptr;          // GameObject..ctor(string)
    const MethodInfo* objDestroy = nullptr;
    const MethodInfo* objDestroyImmediate = nullptr;
    using ObjectNewFn = Il2CppObject* (*)(Il2CppClass*);
    ObjectNewFn objectNew = nullptr;

    // Prefab sub-objects found so far (pinned, never freed), and sources not found (logged once,
    // not searched again this session).
    std::map<std::string, Il2CppObject*> sources;
    std::map<std::string, bool> missing;
    bool broken = false;

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
            if (err.rfind("native exception", 0) == 0) sr2::core::MarkModuleFaulted(module, std::format("{}: {}", what, err));
            else SR2_WARN("[{}] {} failed: {}", module, what, err);
        }
        return r;
    }
    template <class T>
    T Value(Il2CppObject* boxed, T def = T{}) const {
        return boxed ? *reinterpret_cast<T*>(reinterpret_cast<char*>(boxed) + 2 * sizeof(void*)) : def;
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
    void Destroy(Il2CppObject* obj) {
        if (unity::IsAlive(obj)) Call(objDestroy, nullptr, {obj}, "Object.Destroy");
    }
    void DestroyNow(Il2CppObject* obj) {
        if (unity::IsAlive(obj)) Call(objDestroyImmediate, nullptr, {obj}, "Object.DestroyImmediate");
    }
    int32_t PlotType(Il2CppObject* plot) const { return unity::Field<int32_t>(plot, offPlotType); }

    Il2CppObject* GameModel() {
        bool ok = false;
        Il2CppObject* sc = Call(sceneInstance, nullptr, {}, "SceneContext.Instance", &ok);
        if (!ok || !unity::IsAlive(sc)) return nullptr;
        Il2CppObject* gm = unity::Field<Il2CppObject*>(sc, offScGameModel);
        return unity::IsAlive(gm) ? gm : nullptr;
    }
    Il2CppObject* PlotFor(Il2CppObject* gm, const std::string& id) {
        bool ok = false;
        Il2CppObject* model = Call(getPlotModel, gm, {unity::NewString(id)}, "GameModel.GetLandPlotModel", &ok);
        if (!ok || !model) return nullptr;
        Il2CppObject* loc = unity::Field<Il2CppObject*>(model, offModelGameObj);
        if (!unity::IsAlive(loc) || !IsA(loc, gameObjectClass)) return nullptr;
        Il2CppObject* tr = unity::TransformOf(loc);
        if (!unity::IsAlive(tr)) return nullptr;
        bool inactive = true;
        Il2CppObject* plot = Call(compInChildren, tr, {unity::TypeOf(plotClass), &inactive}, "GetComponentInChildren", &ok);
        return ok && unity::IsAlive(plot) && IsA(plot, plotClass) ? plot : nullptr;
    }
    // Current-generation holders under the plot (kind bits); destroys stale / unknown / duplicate
    // holders and those of kinds not in `keep`.
    uint32_t Scan(Il2CppObject* plot, unsigned generation, uint32_t keep) {
        Il2CppObject* tr = unity::TransformOf(plot);
        if (!unity::IsAlive(tr)) return 0;
        const std::string suffix = std::format("#{}", generation);
        uint32_t have = 0;
        for (Il2CppObject* child : ChildTransforms(tr)) {
            Il2CppObject* go = unity::GameObjectOf(child);
            if (!go) continue;
            const std::string name = unity::GetName(go);
            if (name.rfind(kPrefix, 0) != 0) continue;
            const bool current = name.size() > suffix.size() + kPrefix.size() &&
                                 name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0;
            const KindDef* def = nullptr;
            if (current) {
                const std::string_view kind =
                    std::string_view(name).substr(kPrefix.size(), name.size() - kPrefix.size() - suffix.size());
                for (const KindDef& k : kKinds)
                    if (kind == k.name) def = &k;
            }
            if (!def || !(keep & def->kind) || (have & def->kind)) {
                Destroy(go);
                if (def && (keep & def->kind) == 0) SR2_INFO("[{}] {} props removed (not owned / module off)", module, def->name);
                continue;
            }
            have |= def->kind;
        }
        return have;
    }

    Il2CppObject* FindInGroup(Il2CppObject* group, const char* prefabName, int depth, int& budget);
    Il2CppObject* Source(const Part& part, Il2CppObject* lookup);
    void Strip(Il2CppObject* root);
    void Create(Il2CppObject* plot, const KindDef& def, Il2CppObject* lookup, const std::string& id, unsigned generation);
};

// ---------------------------------------------------------------- setup --

bool PlotProps::Setup(Resolver& R, const char* module, std::string& why) {
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
        const MethodInfo* m = k ? R.MethodByParams(k, name, params, &err) : nullptr;
        if (!m && why.empty()) why = std::format("{}: {}", name, err);
        return m;
    };
    HMODULE ga = api().module;
    p->objectNew = reinterpret_cast<Impl::ObjectNewFn>(ga ? GetProcAddress(ga, "il2cpp_object_new") : nullptr);
    if (!p->objectNew) why = "il2cpp_object_new export missing";

    p->plotClass = cls(kImage, "", "LandPlot");
    p->groupClass = cls(kImage, "", "IdentifiableTypeGroup");
    p->identTypeClass = cls(kImage, "", "IdentifiableType");
    Il2CppClass* sceneCtx = cls(kImage, "", "SceneContext");
    Il2CppClass* gameModel = cls(kImage, "MonomiPark.SlimeRancher.DataModel", "GameModel");
    Il2CppClass* plotModel = cls(kImage, "MonomiPark.SlimeRancher.DataModel", "LandPlotModel");
    Il2CppClass* lookup = cls(kImage, "", "LookupDirector");
    Il2CppClass* component = cls(kCore, "UnityEngine", "Component");
    Il2CppClass* transform = cls(kCore, "UnityEngine", "Transform");
    Il2CppClass* uobject = cls(kCore, "UnityEngine", "Object");
    p->gameObjectClass = cls(kCore, "UnityEngine", "GameObject");
    p->monoClass = cls(kCore, "UnityEngine", "MonoBehaviour");
    p->jointClass = cls(kPhysics, "UnityEngine", "Joint");
    p->rigidbodyClass = cls(kPhysics, "UnityEngine", "Rigidbody");
    p->colliderClass = cls(kPhysics, "UnityEngine", "Collider");
    if (!why.empty()) {
        delete p;
        return false;
    }
    off(p->plotClass, "TypeId", p->offPlotType);
    off(sceneCtx, "<GameModel>k__BackingField", p->offScGameModel);
    off(plotModel, "gameObj", p->offModelGameObj);
    off(lookup, "_gadgetDefinitions", p->offGadgets);
    off(lookup, "_toyDefinitions", p->offToys);
    off(p->groupClass, "_memberTypes", p->offMembers);
    off(p->groupClass, "_memberGroups", p->offSubGroups);
    off(p->identTypeClass, "prefab", p->offPrefab);
    p->sceneInstance = method(parentOf(sceneCtx), "get_Instance", 0);
    p->getPlotModel = method(gameModel, "GetLandPlotModel", 1);
    p->getPlotPrefab = method(lookup, "GetPlotPrefab", 1);
    p->compInChildren = byParams(component, "GetComponentInChildren", {"System.Type", "System.Boolean"});
    p->compsInChildren = byParams(component, "GetComponentsInChildren", {"System.Type", "System.Boolean"});
    p->trChildCount = method(transform, "get_childCount", 0);
    p->trGetChild = method(transform, "GetChild", 1);
    p->trFind = byParams(transform, "Find", {"System.String"});
    p->trSetParent = byParams(transform, "SetParent", {"UnityEngine.Transform", "System.Boolean"});
    p->trSetLocalPosition = method(transform, "set_localPosition", 1);
    p->trSetLocalRotation = method(transform, "set_localRotation", 1);
    p->trSetLocalScale = method(transform, "set_localScale", 1);
    p->goCtor = byParams(p->gameObjectClass, ".ctor", {"System.String"});
    p->objDestroy = byParams(uobject, "Destroy", {"UnityEngine.Object"});
    p->objDestroyImmediate = byParams(uobject, "DestroyImmediate", {"UnityEngine.Object"});
    if (!why.empty()) {
        delete p;
        return false;
    }
    impl_ = p;
    ready_ = true;
    return true;
}

// ---------------------------------------------------------------- sources --

// A gadget / toy prefab by its GameObject name: the group's member types, then its sub-groups.
Il2CppObject* PlotProps::Impl::FindInGroup(Il2CppObject* group, const char* prefabName, int depth, int& budget) {
    if (!unity::IsAlive(group) || !IsA(group, groupClass) || depth > 4) return nullptr;
    void* members = unity::Field<void*>(group, offMembers);
    for (size_t i = 0, n = unity::ListCount(members); i < n && budget > 0; ++i, --budget) {
        auto* t = static_cast<Il2CppObject*>(unity::ListAt(members, i));
        if (!unity::IsAlive(t) || !IsA(t, identTypeClass)) continue;
        auto* prefab = unity::Field<Il2CppObject*>(t, offPrefab);
        if (unity::IsAlive(prefab) && IsA(prefab, gameObjectClass) && unity::GetName(prefab) == prefabName) return prefab;
    }
    void* groups = unity::Field<void*>(group, offSubGroups);
    for (size_t i = 0, n = unity::ListCount(groups); i < n && budget > 0; ++i, --budget)
        if (Il2CppObject* r = FindInGroup(static_cast<Il2CppObject*>(unity::ListAt(groups, i)), prefabName, depth + 1, budget))
            return r;
    return nullptr;
}

Il2CppObject* PlotProps::Impl::Source(const Part& part, Il2CppObject* lookup) {
    const std::string key = part.src == Src::kPlot ? std::format("plot{}/{}", part.plotType, part.path)
                                                   : std::format("{}/{}", part.prefab, part.path);
    if (auto it = sources.find(key); it != sources.end() && unity::IsAlive(it->second)) return it->second;
    if (missing.count(key) || !unity::IsAlive(lookup)) return nullptr;
    Il2CppObject* root = nullptr;
    bool ok = false;
    if (part.src == Src::kPlot) {
        int32_t id = part.plotType;
        root = Call(getPlotPrefab, lookup, {&id}, "LookupDirector.GetPlotPrefab", &ok);
        if (!ok || !IsA(root, gameObjectClass)) root = nullptr;
    } else {
        int budget = 4000;
        root = FindInGroup(unity::Field<Il2CppObject*>(lookup, part.src == Src::kToy ? offToys : offGadgets), part.prefab, 0,
                           budget);
    }
    Il2CppObject* rootTr = unity::IsAlive(root) ? unity::TransformOf(root) : nullptr;
    Il2CppObject* tr = rootTr ? Call(trFind, rootTr, {unity::NewString(part.path)}, "Transform.Find", &ok) : nullptr;
    Il2CppObject* go = ok && unity::IsAlive(tr) ? unity::GameObjectOf(tr) : nullptr;
    if (!unity::IsAlive(go)) {
        missing[key] = true;
        SR2_WARN("[{}] plot props: model '{}' not found ({}) - that piece is left out", module, key,
                 root ? "no such child" : "prefab not found");
        return nullptr;
    }
    unity::NewGCHandle(go);  // pinned for the session (never freed)
    sources[key] = go;
    return go;
}

// Pure decoration: game scripts, joints, rigidbodies and colliders go (in that order, so nothing
// that requires one of them is left behind). Unity's own behaviours (LODGroup, Animator, decal
// projectors, light data) stay.
void PlotProps::Impl::Strip(Il2CppObject* root) {
    Il2CppObject* tr = unity::TransformOf(root);
    for (Il2CppClass* k : {monoClass, jointClass, rigidbodyClass, colliderClass}) {
        bool inactive = true, ok = false;
        Il2CppObject* arr = Call(compsInChildren, tr, {unity::TypeOf(k), &inactive}, "GetComponentsInChildren", &ok);
        for (size_t i = 0, n = ok ? unity::ArrayLength(arr) : 0; i < n && i < 512; ++i) {
            auto* c = static_cast<Il2CppObject*>(unity::ArrayAt(arr, i));
            if (!unity::IsAlive(c) || !IsA(c, k)) continue;
            if (k == monoClass && unity::ClassName(c).rfind("UnityEngine.", 0) == 0) continue;
            DestroyNow(c);
        }
        if (sr2::core::ModuleFaulted(module)) return;
    }
}

void PlotProps::Impl::Create(Il2CppObject* plot, const KindDef& def, Il2CppObject* lookup, const std::string& id,
                             unsigned generation) {
    if (broken) return;
    std::vector<std::pair<const Part*, Il2CppObject*>> parts;
    for (size_t i = 0; i < def.count; ++i)
        if (Il2CppObject* src = Source(def.parts[i], lookup)) parts.emplace_back(&def.parts[i], src);
    if (parts.empty() || sr2::core::ModuleFaulted(module)) return;  // logged by Source (once per model)
    Il2CppObject* holder = objectNew(gameObjectClass);
    bool ok = false;
    if (holder) Call(goCtor, holder, {unity::NewString(std::format("{}{}#{}", kPrefix, def.name, generation))}, "GameObject..ctor",
                     &ok);
    if (!ok || !unity::IsAlive(holder)) {
        SR2_WARN("[{}] plot props: holder GameObject not created", module);
        broken = true;
        return;
    }
    unity::SetActive(holder, false);  // clones must not wake up before they are stripped
    Il2CppObject* holderTr = unity::TransformOf(holder);
    bool stays = false;
    Call(trSetParent, holderTr, {unity::TransformOf(plot), &stays}, "Transform.SetParent", &ok);
    Vec3 zero{0.0f, 0.0f, 0.0f}, one{1.0f, 1.0f, 1.0f};
    Quat id0 = kId;
    if (ok) Call(trSetLocalPosition, holderTr, {&zero}, "Transform.set_localPosition", &ok);
    if (ok) Call(trSetLocalRotation, holderTr, {&id0}, "Transform.set_localRotation", &ok);
    if (ok) Call(trSetLocalScale, holderTr, {&one}, "Transform.set_localScale", &ok);
    if (!ok) {
        DestroyNow(holder);
        return;
    }
    int built = 0;
    for (const auto& [part, src] : parts) {
        Il2CppObject* clone = unity::Instantiate(src, holderTr);
        if (!unity::IsAlive(clone) || !IsA(clone, gameObjectClass)) continue;
        Il2CppObject* tr = unity::TransformOf(clone);
        Vec3 pos = part->pos, scale = part->scale;
        Quat rot = part->rot;
        Call(trSetLocalPosition, tr, {&pos}, "Transform.set_localPosition");
        Call(trSetLocalRotation, tr, {&rot}, "Transform.set_localRotation");
        Call(trSetLocalScale, tr, {&scale}, "Transform.set_localScale");
        if (part->activate) unity::SetActive(clone, true);  // still inactive in the hierarchy (holder)
        ++built;
    }
    if (sr2::core::ModuleFaulted(module)) {
        DestroyNow(holder);
        return;
    }
    Strip(holder);
    if (sr2::core::ModuleFaulted(module)) {
        DestroyNow(holder);
        return;
    }
    unity::SetActive(holder, true);
    SR2_INFO("[{}] {} props built on plot '{}' ({} of {} piece(s))", module, def.name, id, built, def.count);
}

// ---------------------------------------------------------------- api --

void PlotProps::Tick(const std::vector<PropRequest>& wanted, const std::function<Il2CppObject*()>& lookup) {
    if (!ready_ || !impl_) return;
    Impl& p = *impl_;
    if (wanted.empty() && built_.empty()) return;
    Il2CppObject* gm = p.GameModel();
    if (!gm) return;
    std::map<std::string, uint32_t> ids;
    for (const std::string& id : built_) ids[id] = 0;
    for (const PropRequest& r : wanted) ids[r.id] |= r.kinds;
    Il2CppObject* lookupDirector = nullptr;
    bool lookupTried = false;
    for (const auto& [id, kinds] : ids) {
        Il2CppObject* plot = p.PlotFor(gm, id);
        if (!plot) {
            built_.erase(id);  // demolished / not loaded: its props went with the old plot object
            continue;
        }
        const int32_t type = p.PlotType(plot);
        uint32_t want = 0;
        for (const KindDef& k : kKinds)
            if ((kinds & k.kind) && k.plotType == type) want |= k.kind;
        const uint32_t have = p.Scan(plot, generation_, want);
        for (const KindDef& k : kKinds) {
            if (!(want & k.kind) || (have & k.kind)) continue;
            if (!lookupTried) {
                lookupTried = true;
                lookupDirector = lookup();
            }
            p.Create(plot, k, lookupDirector, id, generation_);
            if (sr2::core::ModuleFaulted(p.module)) return;
        }
        if (want) built_.insert(id);
        else built_.erase(id);
    }
}

void PlotProps::RemoveAll() {
    if (!ready_ || !impl_) return;
    Il2CppObject* gm = built_.empty() ? nullptr : impl_->GameModel();
    if (gm)
        for (const std::string& id : built_)
            if (Il2CppObject* plot = impl_->PlotFor(gm, id)) impl_->Scan(plot, generation_, 0);
    built_.clear();
}

void PlotProps::Reset() {
    ++generation_;  // built_ is kept: those plots are visited once more and stale props destroyed
}

}  // namespace sr2::modules::custom_upgrades::world
