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
//   * coop prefab "patchCoop": "Deco/decoFieldsGrass01 _Green" / "Deco/decoFieldsGrass02_Green" =
//     the coop's own grass tufts (mesh + LODGroup + two game scripts, no collider).
//   * corral prefab "patchCorral": "Music Box/techCorralMusicBox/MusicBox/Music" = the Music Box
//     upgrade's speaker (Animator, a game script, capsule collider; children: bone_musicBase ->
//     bone_musicEmmit -> "FX Music" note particles + "musicbox (1)" mesh, and the skinned
//     "musicbox" mesh). Its chain (container turned 180 deg, inner offsets cancel) puts it on
//     the (-x, +z) corner post, the speaker head ~0.8 m inwards at 2.5 m.
//   * decorFireFlower (decoration gadget): child "FX Fireflower" (two ember particle systems,
//     "Embers_01" / "Embers_02" at y 2.59 / 1.17 above the flower), set so they rise out of
//     the incinerator's opening.
//   * silo prefab "patchSilo": "Silo Upgrade N/Addition N/Capacity Addon N/techSiloCapacity" = the
//     vanilla Storage Capacity Upgrade's tanks, one per storage section, high on the silo's diagonals
//     (plot-local about (-1.6, 3.3, -1.8) for section 0, the others turned 90 deg each and higher).
//     Their ApplyPalette script is stripped like every game script, so our tanks keep the prefab's
//     default colours (not the silo's palette).
//
// Placement (plot-local, plot root = the plot prefab root). Kept away from the corners: coop
// corners hold the plot menu terminal (-x, -z), the vitamizer (-x, +z), the vanilla elder
// collector (+x, +z) and, by coop_world's PickCorner, the Hen Collector (the free one); garden
// corners hold the terminal, the deluxe planters (+x, -z) / (-x, +z) and the scareslime (+x, +z),
// the deposit port stands at (-5.44, -3.14). Nothing but the silo tanks has a collider, so nothing
// else blocks chickens, crops, the vac or the player.
//
// Coop: hay bales + three grass tufts (Faster Hens I / II), two nests (stuffed chicken in the
// second), the flower pillow and two more tufts (Faster Hens II) stand on scattered "slots"
// (CoopLayout): position, yaw and a small size variation drawn from a generator seeded by the
// plot id (FNV-1a), so a plot always gets the same layout and two coops differ. One layout covers
// both kinds (the Hens II slots stay put when Hens I is bought or sold). A slot keeps within
// +-3.7 m, out of the corner squares (|x| and |z| both > 2.5: terminal, vitamizer, elder
// collector, Hen Collector), off the vanilla nest in the middle and clear of the other slots;
// after 64 failed draws it takes a fixed fallback spot.
//
// Garden: a third sprinkler head in the middle of the +x side (Fast Growth I or II) and a fourth
// in the middle of the -x side (Fast Growth II; the deposit port at (-5.44, -3.14) is outside the
// soil), spraying inwards like the vanilla ones.
//
// Corral: the second speaker goes on the (-x, -z) corner post, above the plot menu terminal
// (which stands outside the post at ground height), head 2.5 m up and leaning inwards; the other
// corners hold the vanilla speaker (-x, +z), the plort collector (+x, +z) and the auto-feeder
// (+x, -z).
//
// Holders: "sr2props_<Kind>#<generation>", a plain GameObject under the plot (identity local
// transform), made inactive before the clones go in, activated after stripping.

#include "modules/custom_upgrades/plot_props.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
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
#include "modules/slime_dj/api.h"

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
constexpr int32_t kPlotGarden = 4;
constexpr int32_t kPlotSilo = 5;
constexpr int32_t kPlotIncinerator = 7;
// The vanilla Storage Capacity Upgrade's tank of the first silo section (inactive in the prefab;
// Transform.Find finds inactive children, the clone goes under our holder and is shown with it).
constexpr const char* kSiloTankPath = "Silo Upgrade 0/Addition 0/Capacity Addon 0/techSiloCapacity";
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
    int slot = -1;       // >= 0: coop layout slot; pos / rot are then offsets from the slot
};

constexpr Quat kId{0.0f, 0.0f, 0.0f, 1.0f};
constexpr Quat YawQ(float s, float c) { return {0.0f, s, 0.0f, c}; }  // (sin, cos) of half the yaw
constexpr Vec3 S(float s) { return {s, s, s}; }

// Coop layout slots (CoopLayout): clearance radius and size variation per slot, and a fallback
// spot (x, z, yaw degrees) used when no random draw fits.
struct SlotDef {
    float radius;
    float scaleLo, scaleHi;
    float fx, fz, fyaw;
};
constexpr SlotDef kCoopSlots[] = {
    {0.9f, 0.9f, 1.1f, -1.3f, 3.6f, 90.0f},    // 0 hay bale       (Hens I / II)
    {0.9f, 0.9f, 1.1f, 1.3f, 3.6f, 75.0f},     // 1 hay bale
    {0.45f, 0.8f, 1.25f, -3.5f, 1.5f, 10.0f},  // 2 grass
    {0.45f, 0.8f, 1.25f, 3.5f, 1.5f, 200.0f},  // 3 grass
    {0.45f, 0.8f, 1.25f, 0.0f, -2.2f, 120.0f}, // 4 grass
    {0.85f, 0.9f, 1.1f, -3.5f, -0.5f, 30.0f},  // 5 nest           (Hens II)
    {0.85f, 0.9f, 1.1f, 3.5f, -0.5f, -40.0f},  // 6 nest + stuffed chicken
    {0.8f, 0.9f, 1.1f, 0.0f, -3.6f, 0.0f},     // 7 flower pillow
    {0.45f, 0.8f, 1.25f, -2.0f, 2.0f, 300.0f}, // 8 grass
    {0.45f, 0.8f, 1.25f, 2.0f, -2.0f, 60.0f},  // 9 grass
};
constexpr size_t kCoopSlotCount = std::size(kCoopSlots);

const Part kHay[] = {
    {Src::kGadget, 0, "decorHayBales", "hayBale_2", {}, kId, S(0.5f), false, 0},
    {Src::kGadget, 0, "decorHayBales", "hayBale_0 (1)", {}, kId, S(0.5f), false, 1},
    {Src::kPlot, kPlotCoop, nullptr, "Deco/decoFieldsGrass01 _Green", {}, kId, S(0.7f), false, 2},
    {Src::kPlot, kPlotCoop, nullptr, "Deco/decoFieldsGrass02_Green", {}, kId, S(0.6f), false, 3},
    {Src::kPlot, kPlotCoop, nullptr, "Deco/decoFieldsGrass01 _Green", {}, kId, S(0.55f), false, 4},
};
const Part kNests[] = {
    {Src::kPlot, kPlotCoop, nullptr, "Deco/nestPlain01", {}, kId, S(1.0f), false, 5},
    {Src::kPlot, kPlotCoop, nullptr, "Deco/nestPlain01", {}, kId, S(1.0f), false, 6},
    {Src::kToy, 0, "toyStuffedChicken", "model_stuffedChicken", {0.0f, 0.1f, 0.0f}, YawQ(-0.70710678f, 0.70710678f),
     S(1.33f), false, 6},
    {Src::kGadget, 0, "decorFlowerPillowSmall", "mesh", {}, kId, S(0.7f), false, 7},
    {Src::kPlot, kPlotCoop, nullptr, "Deco/decoFieldsGrass02_Green", {}, kId, S(0.6f), false, 8},
    {Src::kPlot, kPlotCoop, nullptr, "Deco/decoFieldsGrass01 _Green", {}, kId, S(0.65f), false, 9},
};
const Part kSprinkler[] = {
    {Src::kPlot, kPlotGarden, nullptr, "Sprinkler/SprinklerBase", {4.38f, -0.33f, 0.0f}, kId, S(1.0f), false},
};
const Part kSprinkler2[] = {  // the same head mirrored onto the -x side (turned 180 deg: sprays inwards)
    {Src::kPlot, kPlotGarden, nullptr, "Sprinkler/SprinklerBase", {-4.38f, -0.33f, 0.0f}, YawQ(1.0f, 0.0f), S(1.0f),
     false},
};
const Part kMusic[] = {  // the vanilla speaker, from its (-x, +z) corner post (turned 180 deg) to (-x, -z) (90 deg)
    {Src::kPlot, kPlotCorral, nullptr, "Music Box/techCorralMusicBox/MusicBox/Music", {-4.75f, 0.0f, -4.75f},
     YawQ(0.70710678f, 0.70710678f), S(1.0f), false},
};
const Part kGlow[] = {
    {Src::kPlot, kPlotIncinerator, nullptr, "objIncinerator/Meshes/meshIncinerator/FireTest/Glow", {0.0f, 3.55f, 0.0f},
     kId, {0.5f, 1.0f, 0.5f}, true},
    {Src::kPlot, kPlotIncinerator, nullptr, "objIncinerator/Meshes/meshIncinerator/FireTest/Embers",
     {0.0f, 2.11f, 0.0f}, {-0.70710678f, 0.0f, 0.0f, 0.70710678f}, {1.05f, 0.9f, 1.8f}, true},
    {Src::kGadget, 0, "decorFireFlower", "FX Fireflower", {0.0f, 2.4f, 0.0f}, kId, S(1.0f), false},
};

// Silo tanks: the vanilla capacity tank ("Capacity Addon 0/techSiloCapacity": mesh 2.4 x 2.7 x 2.4
// centred on its origin, child objPipeConnectorWall01 1.59 m below as a foot plate) at 0.8 size,
// standing on the ground on the silo's diagonals at 2.9 / 2.9 (r 4.1: clear of the silo wall at 2.1,
// the vanilla pipes and caps at r <= 3.05, the depositors in the middle of each side and the corner
// posts at 4.75). Yaw = the vanilla tank's yaw on that diagonal (19.6 deg + 90 per quarter turn).
// Tier V takes the last diagonal, (-x, -z): every diagonal has the same low pipes (each storage section's
// Addition is the section-0 layout turned 90 deg), and the plot menu terminal stands outside the fence
// at (-5.54, -5.54), 3.7 m from the tank centre.
// The tanks keep their colliders (SolidKind): the vanilla tank's "collider" MeshCollider and the foot
// plate's MeshCollider, on the prefab's own layer (Default, like the vanilla tanks and pipes), so the
// player walks around them as around the vanilla ones and the vac / drones treat them the same.
const Part kSiloTank2[] = {
    {Src::kPlot, kPlotSilo, nullptr, kSiloTankPath, {2.9f, 1.42f, 2.9f}, YawQ(0.98480775f, -0.17364818f), S(0.8f), false},
};
const Part kSiloTank3[] = {
    {Src::kPlot, kPlotSilo, nullptr, kSiloTankPath, {-2.9f, 1.42f, 2.9f}, YawQ(0.81915204f, 0.57357644f), S(0.8f), false},
};
const Part kSiloTank4[] = {
    {Src::kPlot, kPlotSilo, nullptr, kSiloTankPath, {2.9f, 1.42f, -2.9f}, YawQ(0.57357644f, -0.81915204f), S(0.8f), false},
};
const Part kSiloTank5[] = {
    {Src::kPlot, kPlotSilo, nullptr, kSiloTankPath, {-2.9f, 1.42f, -2.9f}, YawQ(0.17364818f, 0.98480775f), S(0.8f), false},
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
    {kPropCorralMusic, "CorralMusicII", kPlotCorral, kMusic, std::size(kMusic)},
    {kPropSiloTank2, "SiloTankII", kPlotSilo, kSiloTank2, std::size(kSiloTank2)},
    {kPropSiloTank3, "SiloTankIII", kPlotSilo, kSiloTank3, std::size(kSiloTank3)},
    {kPropSiloTank4, "SiloTankIV", kPlotSilo, kSiloTank4, std::size(kSiloTank4)},
    {kPropSiloTank5, "SiloTankV", kPlotSilo, kSiloTank5, std::size(kSiloTank5)},
};

// Props that keep the colliders of their source (solid, like the vanilla part they copy).
bool SolidKind(PropKind k) {
    return k == kPropSiloTank2 || k == kPropSiloTank3 || k == kPropSiloTank4 || k == kPropSiloTank5;
}

// ---------------------------------------------------------------- coop layout --

struct Slot {
    Vec3 pos;
    Quat rot;
    float scale;
};
using CoopSlots = std::array<Slot, kCoopSlotCount>;

Quat Mul(const Quat& a, const Quat& b) {
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}
Quat YawDeg(float deg) {
    const float half = deg * 3.14159265f / 360.0f;
    return YawQ(std::sin(half), std::cos(half));
}

// splitmix64: small, and the same sequence on every run for the same seed.
struct Rng {
    uint64_t state;
    uint64_t Next() {
        uint64_t z = (state += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    float Range(float lo, float hi) { return lo + (hi - lo) * static_cast<float>(Next() >> 40) * (1.0f / 16777216.0f); }
};

// The scattered coop layout of one plot (see the header comment): the same id gives the same slots.
CoopSlots CoopLayout(const std::string& id) {
    uint64_t seed = 1469598103934665603ull;  // FNV-1a of the LandPlotLocation id
    for (unsigned char c : id) {
        seed ^= c;
        seed *= 1099511628211ull;
    }
    Rng rng{seed};
    CoopSlots out{};
    constexpr float kHalf = 3.7f, kCorner = 2.5f, kMiddle = 1.2f;  // vanilla nest: ~1.2 m clear
    for (size_t i = 0; i < kCoopSlotCount; ++i) {
        const SlotDef& d = kCoopSlots[i];
        float x = d.fx, z = d.fz, yaw = d.fyaw;
        for (int attempt = 0; attempt < 64; ++attempt) {
            const float cx = rng.Range(-kHalf, kHalf), cz = rng.Range(-kHalf, kHalf);
            if (std::fabs(cx) > kCorner && std::fabs(cz) > kCorner) continue;
            if (cx * cx + cz * cz < (kMiddle + d.radius) * (kMiddle + d.radius)) continue;
            bool clear = true;
            for (size_t j = 0; j < i && clear; ++j) {
                const float dx = cx - out[j].pos.x, dz = cz - out[j].pos.z, r = d.radius + kCoopSlots[j].radius;
                clear = dx * dx + dz * dz >= r * r;
            }
            if (!clear) continue;
            x = cx;
            z = cz;
            yaw = rng.Range(0.0f, 360.0f);
            break;
        }
        out[i] = {{x, 0.0f, z}, YawDeg(yaw), rng.Range(d.scaleLo, d.scaleHi)};
    }
    return out;
}

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
    void Strip(Il2CppObject* root, bool keepColliders);
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
// projectors, light data) stay. keepColliders (SolidKind): the colliders stay as static colliders
// (no rigidbody left), on the layer the prefab gave them.
void PlotProps::Impl::Strip(Il2CppObject* root, bool keepColliders) {
    Il2CppObject* tr = unity::TransformOf(root);
    for (Il2CppClass* k : {monoClass, jointClass, rigidbodyClass, colliderClass}) {
        if (k == colliderClass && keepColliders) break;
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
    const bool scattered = std::any_of(parts.begin(), parts.end(), [](const auto& p) { return p.first->slot >= 0; });
    const CoopSlots layout = scattered ? CoopLayout(id) : CoopSlots{};
    int built = 0;
    std::vector<Il2CppObject*> clones;
    for (const auto& [part, src] : parts) {
        Il2CppObject* clone = unity::Instantiate(src, holderTr);
        if (!unity::IsAlive(clone) || !IsA(clone, gameObjectClass)) continue;
        Il2CppObject* tr = unity::TransformOf(clone);
        Vec3 pos = part->pos, scale = part->scale;
        Quat rot = part->rot;
        if (part->slot >= 0 && static_cast<size_t>(part->slot) < kCoopSlotCount) {
            // slot offsets are vertical only (the stuffed chicken sits in its nest), so no rotation
            const Slot& sl = layout[static_cast<size_t>(part->slot)];
            pos = {sl.pos.x + pos.x, sl.pos.y + pos.y, sl.pos.z + pos.z};
            rot = Mul(sl.rot, rot);
            scale = {scale.x * sl.scale, scale.y * sl.scale, scale.z * sl.scale};
        }
        Call(trSetLocalPosition, tr, {&pos}, "Transform.set_localPosition");
        Call(trSetLocalRotation, tr, {&rot}, "Transform.set_localRotation");
        Call(trSetLocalScale, tr, {&scale}, "Transform.set_localScale");
        if (part->activate) unity::SetActive(clone, true);  // still inactive in the hierarchy (holder)
        clones.push_back(clone);
        ++built;
    }
    if (sr2::core::ModuleFaulted(module)) {
        DestroyNow(holder);
        return;
    }
    Strip(holder, SolidKind(def.kind));
    if (sr2::core::ModuleFaulted(module)) {
        DestroyNow(holder);
        return;
    }
    unity::SetActive(holder, true);
    // Music Box II: the second speaker plays the Slime DJ song too (its vanilla audio was stripped with
    // the game scripts). Dropped by slime_dj when Unity destroys the holder.
    if (def.kind == kPropCorralMusic)
        for (Il2CppObject* clone : clones)
            if (unity::IsAlive(clone)) sr2::modules::slime_dj::RegisterEmitter(clone, sr2::modules::slime_dj::EmitterKind::kSpeaker);
    SR2_INFO("[{}] {} props built on plot '{}' ({} of {} piece(s){}{})", module, def.name, id, built, def.count,
             scattered ? ", scattered by plot id" : "", SolidKind(def.kind) ? ", colliders kept" : "");
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
