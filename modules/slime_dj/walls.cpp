// slime_dj: "Walls dance to the music" (see walls.h, NOTES.md).
//
// Corral prefab "patchCorral" (asset bundle dump, game 1.3.0): the plot root carries LandPlot,
// WallUpgrader, MusicBoxUpgrader, AirNetUpgrader, ... (all on the same GameObject).
//   * WallUpgrader.StandardWalls = "Base Corral", .UpgradeWalls = "High Wall Corral" (one of them
//     active). Each has Frame/techCorralPostNN (4, turned 90 deg apart) / empty_postBase (at the
//     corner) / "corralField": one MeshRenderer, an L-shaped energy field covering half of the two
//     sides at that corner (mesh bounds 4.9 x 3.2 x 4.9, y 0.17..3.39). High walls add
//     "corralField (1)" at y 3.38 on top. So 4 fields (standard) or 8 (high walls). Material
//     "CorralBarrier" (transparent HDRP shader graph): _Intensity 3, _Color white, _EmissionColor,
//     ripple props. The fields have no collider (the barriers are separate "corralBarrier" boxes),
//     so scaling them changes nothing in play.
//   * AirNetUpgrader.AirNets = [Base Air Net, High Air Net]; the owned one (for the walls) is
//     active: "NetTrigger" = MeshRenderer + AirNet + BoxCollider (the net's collider: never moved
//     or scaled here). Material "AirNet" (same shader family): _Intensity 1. AirNet.Awake/Update
//     write "_Health" on its own material instance and OnCollisionEnter "_HitTime"; we only set
//     _Intensity and _Color, in a property block, so a hit / broken net still shows.
//   * The barrier ripples (CorralBarrierRippleCollision) go through Shader.SetGlobalVectorArray,
//     not through the renderers.
//
// Animation per frame, for every cached renderer (direct native calls, no searches):
//   Renderer.GetPropertyBlock(work) -> SetFloat(_Intensity) / SetColor(_Color) -> SetPropertyBlock,
//   and for wall fields Transform.set_localScale (taller on the beat). Restore: the renderer's own
//   block as it was before the first change (snapshot MaterialPropertyBlock per renderer), and the
//   original localScale.

#include "modules/slime_dj/walls.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <format>

#include "core/detour_guard.h"
#include "core/guard.h"
#include "core/log.h"
#include "core/module.h"
#include "core/resolver.h"
#include "core/unity.h"

namespace sr2::modules::slime_dj {

namespace {
namespace unity = sr2::core::unity;
using il2cpp::api;
using il2cpp::Resolver;

constexpr const char* kModule = "slime_dj";
constexpr const char* kImage = "Assembly-CSharp";
constexpr const char* kCore = "UnityEngine.CoreModule";
constexpr size_t kMaxPlots = 12;
constexpr size_t kMaxPartsPerPlot = 24;
constexpr float kPi = 3.14159265358979f;
// Look
constexpr float kAttack = 0.03f, kRelease = 0.15f;  // bass level smoothing (s)
constexpr float kPulseDecay = 0.2f;                  // beat pulse time constant (s)
constexpr float kChaseTime = 0.09f;                  // the chase turns a quarter per beat, this fast
constexpr float kHueStep = 0.11f;                    // colour wheel step per beat
constexpr float kGlowLevel = 0.8f, kGlowBeat = 2.0f; // _Intensity x (1 + I * (level*a + pulse*b))
constexpr float kLiftLevel = 0.04f, kLiftBeat = 0.10f;  // wall height x (1 + I * (...))
constexpr float kColourMix = 0.6f;                   // colour share at intensity 1
constexpr float kRest = 0.004f;                      // below this the walls count as settled

struct Vec3 {
    float x, y, z;
};
struct Color {
    float r, g, b, a;
};

template <class Fn>
struct M {
    Fn fn = nullptr;
    const MethodInfo* mi = nullptr;
    explicit operator bool() const { return fn != nullptr; }
};

Color Hsv(float h, float s, float v) {
    h -= std::floor(h);
    const float i = std::floor(h * 6.0f), f = h * 6.0f - i;
    const float p = v * (1 - s), q = v * (1 - s * f), t = v * (1 - s * (1 - f));
    switch (static_cast<int>(i) % 6) {
        case 0: return {v, t, p, 1};
        case 1: return {q, v, p, 1};
        case 2: return {p, v, t, 1};
        case 3: return {p, q, v, 1};
        case 4: return {t, p, v, 1};
        default: return {v, p, q, 1};
    }
}
}  // namespace

struct WallDancer::Impl {
    // ---- resolved
    bool ready = false;
    Il2CppClass* wallUpgrader = nullptr;
    Il2CppClass* airNetUpgrader = nullptr;
    Il2CppClass* meshRenderer = nullptr;
    Il2CppClass* mpbClass = nullptr;
    size_t offStandard = 0, offUpgrade = 0, offAirNets = 0;
    using ObjectNewFn = Il2CppObject* (*)(Il2CppClass*);
    ObjectNewFn objectNew = nullptr;
    const MethodInfo* compsInChildren = nullptr;  // Component.GetComponentsInChildren(Type, bool)
    const MethodInfo* mpbCtor = nullptr;
    M<void (*)(Il2CppObject*, const MethodInfo*)> mpbClear;
    M<void (*)(Il2CppObject*, int32_t, float, const MethodInfo*)> mpbSetFloat;
    M<void (*)(Il2CppObject*, int32_t, Color, const MethodInfo*)> mpbSetColor;
    M<void (*)(Il2CppObject*, Il2CppObject*, const MethodInfo*)> rendGetBlock, rendSetBlock;
    M<Il2CppObject* (*)(Il2CppObject*, const MethodInfo*)> rendSharedMaterial;
    M<bool (*)(Il2CppObject*, int32_t, const MethodInfo*)> matHasProperty;
    M<float (*)(Il2CppObject*, int32_t, const MethodInfo*)> matGetFloat;
    M<Color (*)(Il2CppObject*, int32_t, const MethodInfo*)> matGetColor;
    M<int32_t (*)(Il2CppString*, const MethodInfo*)> propertyToId;
    M<Vec3 (*)(Il2CppObject*, const MethodInfo*)> trGetLocalScale, trGetPosition;
    M<void (*)(Il2CppObject*, Vec3, const MethodInfo*)> trSetLocalScale;
    int32_t idIntensity = 0, idColor = 0;

    // ---- state
    struct Part {
        Il2CppGCHandle rend = nullptr, tr = nullptr, saved = nullptr;  // saved: the renderer's own block
        Vec3 scale{1, 1, 1};
        float baseIntensity = 1.0f;
        Color baseColor{1, 1, 1, 1};
        bool hasIntensity = false, hasColor = false;
        bool wall = false;  // wall field (scaled) or net (glow only)
        float angle = 0;    // around the plot centre (rad)
        int tier = 0;       // 0 = lower field, 1 = the high walls' upper field
    };
    struct Plot {
        Il2CppGCHandle up = nullptr;    // MusicBoxUpgrader
        Il2CppObject* walls = nullptr;  // active walls root / net (identity only: rebuilt when they change)
        Il2CppObject* net = nullptr;
        std::vector<Part> parts;
        bool touched = false;           // something is changed right now
    };
    std::vector<Plot> plots;
    Il2CppGCHandle work = nullptr;  // shared MaterialPropertyBlock
    float env = 0, pulse = 0, rot = 0, rotTarget = 0, hue = 0;
    int builtLogs = 0;

    template <class Fn>
    static bool Bind(M<Fn>& m, const MethodInfo* mi) {
        void* p = mi ? Resolver::Get().MethodPointer(mi) : nullptr;
        if (!p) return false;
        m.fn = reinterpret_cast<Fn>(p);
        m.mi = mi;
        return true;
    }
    static Il2CppObject* Target(Il2CppGCHandle h) {
        Il2CppObject* o = h ? unity::GCHandleTarget(h) : nullptr;
        return unity::IsAlive(o) ? o : nullptr;
    }
    static void Free(Il2CppGCHandle& h) {
        if (h) unity::FreeGCHandle(h);
        h = nullptr;
    }
    bool IsA(Il2CppObject* obj, Il2CppClass* k) const {
        const auto& a = api();
        if (!obj || !k || !a.il2cpp_object_get_class) return false;
        Il2CppClass* c = a.il2cpp_object_get_class(obj);
        if (c == k) return true;
        return c && a.il2cpp_class_is_assignable_from && a.il2cpp_class_is_assignable_from(k, c);
    }
    Il2CppObject* Invoke(const MethodInfo* m, void* obj, std::initializer_list<void*> args, const char* what, bool* ok) {
        std::vector<void*> params(args);
        std::string err;
        Il2CppObject* r = Resolver::Get().Invoke(m, obj, params.empty() ? nullptr : params.data(), &err);
        *ok = err.empty();
        if (!*ok) {
            if (err.rfind("native exception", 0) == 0) core::MarkModuleFaulted(kModule, std::format("{}: {}", what, err));
            else SR2_WARN("slime_dj: walls: {} failed: {}", what, err);
        }
        return r;
    }

    Il2CppObject* NewBlock() {
        Il2CppObject* b = objectNew(mpbClass);
        bool ok = false;
        if (b) Invoke(mpbCtor, b, {}, "MaterialPropertyBlock..ctor", &ok);
        return ok ? b : nullptr;
    }

    std::vector<Il2CppObject*> Renderers(Il2CppObject* go) {
        std::vector<Il2CppObject*> out;
        Il2CppObject* tr = unity::TransformOf(go);
        if (!tr) return out;
        bool inactive = false, ok = false;
        Il2CppObject* arr = Invoke(compsInChildren, tr, {unity::TypeOf(meshRenderer), &inactive}, "GetComponentsInChildren", &ok);
        for (size_t i = 0, n = ok ? std::min<size_t>(unity::ArrayLength(arr), 64) : 0; i < n; ++i) {
            auto* r = static_cast<Il2CppObject*>(unity::ArrayAt(arr, i));
            if (unity::IsAlive(r) && IsA(r, meshRenderer)) out.push_back(r);
        }
        return out;
    }

    // The plot's active walls root and owned net (nullptr = none).
    void Find(Il2CppObject* up, Il2CppObject*& walls, Il2CppObject*& net) {
        walls = net = nullptr;
        Il2CppObject* wu = unity::GetComponent(up, wallUpgrader);
        if (wu && IsA(wu, wallUpgrader)) {
            for (size_t off : {offUpgrade, offStandard}) {
                Il2CppObject* go = unity::Field<Il2CppObject*>(wu, off);
                if (unity::IsAlive(go) && unity::ActiveInHierarchy(go)) {
                    walls = go;
                    break;
                }
            }
        }
        Il2CppObject* nu = unity::GetComponent(up, airNetUpgrader);
        if (nu && IsA(nu, airNetUpgrader)) {
            void* arr = unity::Field<void*>(nu, offAirNets);
            for (size_t i = 0, n = std::min<size_t>(unity::ArrayLength(arr), 8); i < n; ++i) {
                auto* go = static_cast<Il2CppObject*>(unity::ArrayAt(arr, i));
                if (unity::IsAlive(go) && unity::ActiveInHierarchy(go)) {
                    net = go;
                    break;
                }
            }
        }
    }

    bool AddPart(Plot& plot, Il2CppObject* r, bool wall, const Vec3& centre) {
        if (plot.parts.size() >= kMaxPartsPerPlot) return false;
        Part pt;
        pt.wall = wall;
        Il2CppObject* tr = unity::TransformOf(r);
        if (!tr) return false;
        if (Il2CppObject* mat = rendSharedMaterial.fn(r, rendSharedMaterial.mi); unity::IsAlive(mat)) {
            pt.hasIntensity = matHasProperty.fn(mat, idIntensity, matHasProperty.mi);
            pt.hasColor = matHasProperty.fn(mat, idColor, matHasProperty.mi);
            if (pt.hasIntensity) pt.baseIntensity = matGetFloat.fn(mat, idIntensity, matGetFloat.mi);
            if (pt.hasColor) pt.baseColor = matGetColor.fn(mat, idColor, matGetColor.mi);
            if (!std::isfinite(pt.baseIntensity)) pt.hasIntensity = false;
        }
        Il2CppObject* saved = NewBlock();
        if (!saved) return false;
        // Snapshot of the renderer's own block (usually empty; this build has no isEmpty to tell),
        // set back on restore.
        mpbClear.fn(saved, mpbClear.mi);
        rendGetBlock.fn(r, saved, rendGetBlock.mi);
        if (wall) {
            pt.scale = trGetLocalScale.fn(tr, trGetLocalScale.mi);
            if (!std::isfinite(pt.scale.x) || !std::isfinite(pt.scale.y) || !std::isfinite(pt.scale.z)) return false;
            const std::string name = unity::GetName(unity::GameObjectOf(r));
            pt.tier = name.find("(1)") != std::string::npos ? 1 : 0;
        }
        const Vec3 pos = trGetPosition.fn(tr, trGetPosition.mi);
        pt.angle = std::isfinite(pos.x) && std::isfinite(pos.z) ? std::atan2(pos.z - centre.z, pos.x - centre.x) : 0.0f;
        pt.rend = unity::NewGCHandle(r);
        pt.tr = unity::NewGCHandle(tr);
        pt.saved = unity::NewGCHandle(saved);
        if (!pt.rend || !pt.tr || !pt.saved) {
            Free(pt.rend);
            Free(pt.tr);
            Free(pt.saved);
            return false;
        }
        plot.parts.push_back(pt);
        return true;
    }

    void Build(Plot& plot, Il2CppObject* up) {
        Find(up, plot.walls, plot.net);
        Il2CppObject* root = unity::TransformOf(up);
        Vec3 centre{0, 0, 0};
        if (root) centre = trGetPosition.fn(root, trGetPosition.mi);
        size_t fields = 0, nets = 0;
        std::string other;
        if (plot.walls)
            for (Il2CppObject* r : Renderers(plot.walls)) {
                const std::string name = unity::GetName(unity::GameObjectOf(r));
                if (name.rfind("corralField", 0) != 0) {
                    if (other.size() < 160) other += (other.empty() ? "" : ", ") + name;
                    continue;
                }
                if (AddPart(plot, r, true, centre)) ++fields;
            }
        if (plot.net)
            for (Il2CppObject* r : Renderers(plot.net))
                if (AddPart(plot, r, false, centre)) ++nets;
        if (builtLogs++ < 24) {
            const Part* w = nullptr;
            const Part* n = nullptr;
            for (const Part& p : plot.parts) {
                if (p.wall && !w) w = &p;
                if (!p.wall && !n) n = &p;
            }
            SR2_INFO("slime_dj: walls dance at '{}': {} wall field(s) under '{}', {} net renderer(s) under '{}'; "
                     "wall _Intensity {} _Color {}, net _Intensity {}{}",
                     unity::GetName(unity::GameObjectOf(up)), fields, plot.walls ? unity::GetName(plot.walls) : "-", nets,
                     plot.net ? unity::GetName(plot.net) : "-",
                     w && w->hasIntensity ? std::format("{:.2f}", w->baseIntensity) : std::string("n/a"),
                     w && w->hasColor ? std::format("({:.2f}, {:.2f}, {:.2f})", w->baseColor.r, w->baseColor.g, w->baseColor.b)
                                      : std::string("n/a"),
                     n && n->hasIntensity ? std::format("{:.2f}", n->baseIntensity) : std::string("n/a"),
                     other.empty() ? std::string() : std::format(" (other renderers skipped: {})", other));
        }
    }

    void Restore(Plot& plot) {
        if (!plot.touched) return;
        plot.touched = false;
        for (Part& pt : plot.parts) {
            Il2CppObject* r = Target(pt.rend);
            Il2CppObject* saved = Target(pt.saved);
            if (r && saved) rendSetBlock.fn(r, saved, rendSetBlock.mi);
            if (pt.wall)
                if (Il2CppObject* tr = Target(pt.tr)) trSetLocalScale.fn(tr, pt.scale, trSetLocalScale.mi);
        }
    }

    void Drop(Plot& plot) {
        Restore(plot);
        for (Part& pt : plot.parts) {
            Free(pt.rend);
            Free(pt.tr);
            Free(pt.saved);
        }
        plot.parts.clear();
        Free(plot.up);
    }

    void Animate(Plot& plot, float intensity) {
        Il2CppObject* work0 = Target(work);
        if (!work0) return;
        plot.touched = true;
        const float mix = std::min(1.0f, kColourMix * intensity);
        for (Part& pt : plot.parts) {
            Il2CppObject* r = Target(pt.rend);
            if (!r) continue;
            // The chase: brightest where the beat "is" on its way around the corral; the upper
            // field trails the lower one a little (a wave going up).
            const float c = 0.5f + 0.5f * std::cos(pt.angle - rot - 0.6f * static_cast<float>(pt.tier));
            const float w = c * c;
            const float beat = pulse * w;
            if (pt.hasIntensity || pt.hasColor) {
                mpbClear.fn(work0, mpbClear.mi);
                rendGetBlock.fn(r, work0, rendGetBlock.mi);  // keeps whatever else the game put there
                if (pt.hasIntensity) {
                    const float g = pt.baseIntensity * (1.0f + intensity * (kGlowLevel * env + kGlowBeat * beat));
                    mpbSetFloat.fn(work0, idIntensity, g, mpbSetFloat.mi);
                }
                if (pt.hasColor) {
                    const Color h = Hsv(hue + pt.angle / (2 * kPi) * 0.3f, 0.85f, 1.0f);
                    const float k = mix * (0.35f + 0.65f * std::max(env, beat));
                    const Color b = pt.baseColor;
                    mpbSetColor.fn(work0, idColor,
                                   Color{b.r + (h.r - b.r) * k, b.g + (h.g - b.g) * k, b.b + (h.b - b.b) * k, b.a},
                                   mpbSetColor.mi);
                }
                rendSetBlock.fn(r, work0, rendSetBlock.mi);
            }
            if (pt.wall)
                if (Il2CppObject* tr = Target(pt.tr)) {
                    const float lift = 1.0f + intensity * (kLiftLevel * env + kLiftBeat * beat);
                    trSetLocalScale.fn(tr, Vec3{pt.scale.x, pt.scale.y * lift, pt.scale.z}, trSetLocalScale.mi);
                }
        }
    }
};

WallDancer::WallDancer() : p_(std::make_unique<Impl>()) {}
WallDancer::~WallDancer() = default;  // never runs for the module's instance (leaked on purpose)

bool WallDancer::Ready() const { return p_->ready; }
size_t WallDancer::Plots() const { return p_->plots.size(); }

bool WallDancer::Resolve(std::string& why) {
    Impl& P = *p_;
    Resolver& R = Resolver::Get();
    auto parentOf = [](Il2CppClass* c) { return api().il2cpp_class_get_parent ? api().il2cpp_class_get_parent(c) : nullptr; };
    auto cls = [&](const char* image, const char* ns, const char* name) -> Il2CppClass* {
        std::string err;
        Il2CppClass* k = R.Class(image, ns, name, &err);
        if (!k && why.empty()) why = std::format("class {}.{}: {}", ns, name, err);
        return k;
    };
    auto off = [&](Il2CppClass* k, const char* field, size_t& out) {
        for (Il2CppClass* c = k; c; c = parentOf(c))
            if (auto o = R.FieldOffset(c, field)) {
                out = *o;
                return;
            }
        if (why.empty()) why = std::format("field {} not found", field);
    };
    auto byParams = [&](Il2CppClass* k, const char* name, std::initializer_list<std::string_view> params) -> const MethodInfo* {
        std::string err;
        const MethodInfo* m = k ? R.MethodByParams(k, name, params, &err) : nullptr;
        if (!m && why.empty()) why = std::format("{}: {}", name, err);
        return m;
    };
    auto bind = [&](auto& m, const MethodInfo* mi, const char* what) {
        if (!Impl::Bind(m, mi) && why.empty()) why = std::format("{}: no native body", what);
    };

    HMODULE ga = api().module;
    P.objectNew = reinterpret_cast<Impl::ObjectNewFn>(ga ? GetProcAddress(ga, "il2cpp_object_new") : nullptr);
    if (!P.objectNew) why = "il2cpp_object_new export missing";
    P.wallUpgrader = cls(kImage, "", "WallUpgrader");
    P.airNetUpgrader = cls(kImage, "", "AirNetUpgrader");
    Il2CppClass* component = cls(kCore, "UnityEngine", "Component");
    Il2CppClass* transform = cls(kCore, "UnityEngine", "Transform");
    Il2CppClass* renderer = cls(kCore, "UnityEngine", "Renderer");
    Il2CppClass* material = cls(kCore, "UnityEngine", "Material");
    Il2CppClass* shader = cls(kCore, "UnityEngine", "Shader");
    P.meshRenderer = cls(kCore, "UnityEngine", "MeshRenderer");
    P.mpbClass = cls(kCore, "UnityEngine", "MaterialPropertyBlock");
    if (!why.empty()) return false;
    off(P.wallUpgrader, "StandardWalls", P.offStandard);
    off(P.wallUpgrader, "UpgradeWalls", P.offUpgrade);
    off(P.airNetUpgrader, "AirNets", P.offAirNets);
    P.compsInChildren = byParams(component, "GetComponentsInChildren", {"System.Type", "System.Boolean"});
    P.mpbCtor = byParams(P.mpbClass, ".ctor", {});
    bind(P.mpbClear, byParams(P.mpbClass, "Clear", {}), "MaterialPropertyBlock.Clear");
    bind(P.mpbSetFloat, byParams(P.mpbClass, "SetFloat", {"System.Int32", "System.Single"}), "MaterialPropertyBlock.SetFloat");
    bind(P.mpbSetColor, byParams(P.mpbClass, "SetColor", {"System.Int32", "UnityEngine.Color"}), "MaterialPropertyBlock.SetColor");
    bind(P.rendGetBlock, byParams(renderer, "GetPropertyBlock", {"UnityEngine.MaterialPropertyBlock"}), "Renderer.GetPropertyBlock");
    bind(P.rendSetBlock, byParams(renderer, "SetPropertyBlock", {"UnityEngine.MaterialPropertyBlock"}), "Renderer.SetPropertyBlock");
    bind(P.rendSharedMaterial, byParams(renderer, "get_sharedMaterial", {}), "Renderer.get_sharedMaterial");
    bind(P.matHasProperty, byParams(material, "HasProperty", {"System.Int32"}), "Material.HasProperty");
    bind(P.matGetFloat, byParams(material, "GetFloat", {"System.Int32"}), "Material.GetFloat");
    bind(P.matGetColor, byParams(material, "GetColor", {"System.Int32"}), "Material.GetColor");
    bind(P.propertyToId, byParams(shader, "PropertyToID", {"System.String"}), "Shader.PropertyToID");
    bind(P.trGetLocalScale, byParams(transform, "get_localScale", {}), "Transform.get_localScale");
    bind(P.trSetLocalScale, byParams(transform, "set_localScale", {"UnityEngine.Vector3"}), "Transform.set_localScale");
    bind(P.trGetPosition, byParams(transform, "get_position", {}), "Transform.get_position");
    if (!why.empty()) return false;
    P.ready = true;
    return true;
}

void WallDancer::Sync(const std::vector<Il2CppObject*>& upgraders) {
    Impl& P = *p_;
    if (!P.ready) return;
    if (!P.idIntensity) {  // main thread, first use
        P.idIntensity = P.propertyToId.fn(unity::NewString("_Intensity"), P.propertyToId.mi);
        P.idColor = P.propertyToId.fn(unity::NewString("_Color"), P.propertyToId.mi);
    }
    if (!Impl::Target(P.work)) {
        Impl::Free(P.work);
        if (Il2CppObject* b = P.NewBlock()) P.work = unity::NewGCHandle(b);
        if (!P.work) return;
    }
    // Gone, out of range, or their walls / net changed (bought or sold): restore and drop (rebuilt below).
    for (size_t i = 0; i < P.plots.size();) {
        Impl::Plot& plot = P.plots[i];
        Il2CppObject* up = Impl::Target(plot.up);
        bool keep = up && std::find(upgraders.begin(), upgraders.end(), up) != upgraders.end();
        if (keep) {
            Il2CppObject *walls = nullptr, *net = nullptr;
            P.Find(up, walls, net);
            keep = walls == plot.walls && net == plot.net;
        }
        if (keep) {
            ++i;
            continue;
        }
        P.Drop(plot);
        P.plots.erase(P.plots.begin() + static_cast<std::ptrdiff_t>(i));
    }
    for (Il2CppObject* up : upgraders) {
        if (P.plots.size() >= kMaxPlots) break;
        if (!unity::IsAlive(up)) continue;
        const bool have = std::any_of(P.plots.begin(), P.plots.end(), [&](const Impl::Plot& pl) { return Impl::Target(pl.up) == up; });
        if (have) continue;
        Impl::Plot plot;
        plot.up = unity::NewGCHandle(up);
        if (!plot.up) continue;
        P.Build(plot, up);
        P.plots.push_back(std::move(plot));
    }
}

void WallDancer::Tick(float dt, const BeatTrack::Sample* beat, float intensity) {
    Impl& P = *p_;
    if (!P.ready || P.plots.empty()) return;
    dt = std::clamp(dt, 0.0f, 0.1f);
    const float level = beat ? beat->level : 0.0f;
    P.env += (level - P.env) * (1.0f - std::exp(-dt / (level > P.env ? kAttack : kRelease)));
    P.pulse *= std::exp(-dt / kPulseDecay);
    if (beat && beat->beat > 0) {
        P.pulse = std::max(P.pulse, beat->beat);
        P.rotTarget += kPi / 2;
        P.hue += kHueStep;
    }
    P.rot += (P.rotTarget - P.rot) * (1.0f - std::exp(-dt / kChaseTime));
    if (P.rotTarget > 64 * kPi) {  // keep the angles small
        P.rotTarget -= 64 * kPi;
        P.rot -= 64 * kPi;
    }
    P.hue -= std::floor(P.hue);
    const bool settled = !beat && P.env < kRest && P.pulse < kRest;
    for (Impl::Plot& plot : P.plots) {
        if (settled) P.Restore(plot);
        else P.Animate(plot, std::clamp(intensity, 0.0f, 2.0f));
    }
}

void WallDancer::Clear() {
    Impl& P = *p_;
    for (Impl::Plot& plot : P.plots) P.Drop(plot);
    P.plots.clear();
    P.env = P.pulse = 0;
}

}  // namespace sr2::modules::slime_dj
