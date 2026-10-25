// custom_upgrades: Auto-Feeder fill lights (see feeder_lights.h). SR2 1.3.0, all lookups by name.

#include "modules/custom_upgrades/feeder_lights.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <format>
#include <initializer_list>
#include <string>
#include <vector>

#include "core/detour_guard.h"
#include "core/il2cpp_api.h"
#include "core/log.h"
#include "core/main_thread.h"
#include "core/module.h"
#include "core/resolver.h"
#include "core/settings.h"
#include "core/unity.h"

namespace sr2::modules::custom_upgrades::feeder {
namespace {

namespace unity = sr2::core::unity;
namespace settings = sr2::core::settings;
namespace main_thread = sr2::core::main_thread;
using sr2::il2cpp::api;
using sr2::il2cpp::Resolver;

constexpr const char* kImage = "Assembly-CSharp";
constexpr const char* kCore = "UnityEngine.CoreModule";
constexpr const char* kModelNs = "MonomiPark.SlimeRancher.DataModel";
constexpr uint64_t kRescanMs = 3000;  // walk the save's plot list at most this often (field reads + 1 call per corral)
constexpr int kLevels = 20;           // fill steps; a block is written only when the step changes
constexpr int32_t kPlotCorral = 2;    // LandPlot.Id.CORRAL
constexpr int32_t kUpgFeeder = 9;     // LandPlot.Upgrade.FEEDER
constexpr size_t kDictEntry = 24;     // Dictionary<string, LandPlotModel>.Entry {uint hash; int next; key; value}
constexpr const char* kLedMaterial = "TechDepositor_ArrowsLed";

struct Color {
    float r, g, b, a;
};

struct Light {
    Il2CppGCHandle rend = nullptr;
    Color base{};  // the material's own _Color (restore)
};
struct Feeder {
    Il2CppObject* key = nullptr;  // LandPlotModel (identity only; the save's plot dictionary keeps it alive)
    Il2CppGCHandle feeder = nullptr, storage = nullptr;
    std::vector<Light> lights;
    int level = -1;  // last written fill step, -1 = not tinted
};

struct Impl {
    const char* module = "custom_upgrades";
    using ObjectNewFn = Il2CppObject* (*)(Il2CppClass*);
    using ElemSizeFn = int (*)(Il2CppClass*);
    ObjectNewFn objectNew = nullptr;
    ElemSizeFn elemSize = nullptr;
    Il2CppClass* feederClass = nullptr;
    Il2CppClass* storageClass = nullptr;
    Il2CppClass* meshRenderer = nullptr;
    Il2CppClass* mpbClass = nullptr;
    size_t offFeederStorage = 0;  // SlimeFeeder._storage
    size_t offLandPlots = 0;      // GameModel.landPlots (Dictionary<string, LandPlotModel>)
    size_t offModelType = 0, offModelUpgrades = 0, offModelObj = 0;  // LandPlotModel.typeId / upgrades / gameObj
    const MethodInfo* slotCount = nullptr;        // SiloStorage.GetSlotCount(int)
    const MethodInfo* slotMax = nullptr;          // SiloStorage.GetSlotMaxCount(int)
    const MethodInfo* compInChildren = nullptr;   // Component.GetComponentInChildren(Type, bool)
    const MethodInfo* compsInChildren = nullptr;  // Component.GetComponentsInChildren(Type, bool)
    const MethodInfo* sharedMaterial = nullptr;   // Renderer.get_sharedMaterial
    const MethodInfo* matHas = nullptr;           // Material.HasProperty(int)
    const MethodInfo* matGetColor = nullptr;      // Material.GetColor(int)
    const MethodInfo* mpbCtor = nullptr;
    const MethodInfo* mpbSetColor = nullptr;  // MaterialPropertyBlock.SetColor(int, Color)
    const MethodInfo* getBlock = nullptr;     // Renderer.GetPropertyBlock(MaterialPropertyBlock)
    const MethodInfo* setBlock = nullptr;     // Renderer.SetPropertyBlock(MaterialPropertyBlock)
    const MethodInfo* propToId = nullptr;     // Shader.PropertyToID(string)
    int idColor = -1;                         // _Color
    Il2CppGCHandle work = nullptr;            // shared MaterialPropertyBlock
    // Dictionary / HashSet layouts: read from the live objects' classes once
    Il2CppClass* dictClass = nullptr;
    size_t offDictEntries = 0, offDictCount = 0;
    Il2CppClass* setClass = nullptr;
    const MethodInfo* setContains = nullptr;  // HashSet<LandPlot.Upgrade>.Contains
    bool layoutBad = false;
    std::vector<Feeder> feeders;
    uint64_t lastScan = 0;
    bool scanned = false;
    bool loggedNoLights = false;

    template <typename T>
    static T Value(Il2CppObject* boxed, T def = T{}) {
        return boxed ? *reinterpret_cast<T*>(reinterpret_cast<char*>(boxed) + 2 * sizeof(void*)) : def;
    }
    static bool IsA(Il2CppObject* obj, Il2CppClass* k) {
        const auto& a = api();
        if (!obj || !k || !a.il2cpp_object_get_class) return false;
        Il2CppClass* c = a.il2cpp_object_get_class(obj);
        if (c == k) return true;
        return c && a.il2cpp_class_is_assignable_from && a.il2cpp_class_is_assignable_from(k, c);
    }
    static Il2CppObject* Target(Il2CppGCHandle h) {
        Il2CppObject* o = h ? unity::GCHandleTarget(h) : nullptr;
        return unity::IsAlive(o) ? o : nullptr;
    }
    static void Free(Il2CppGCHandle& h) {
        if (h) unity::FreeGCHandle(h);
        h = nullptr;
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
                sr2::core::MarkModuleFaulted(module, std::format("feeder lights: {}: {}", what, err));
            else SR2_WARN("[{}] feeder lights: {} failed: {}", module, what, err);
        }
        return r;
    }

    // Main thread, once: the property id and the shared block (needs the game's runtime up).
    bool Init() {
        if (work) return true;
        bool ok = false;
        Il2CppString* s = unity::NewString("_Color");
        Il2CppObject* b = s ? Call(propToId, nullptr, {s}, "Shader.PropertyToID", &ok) : nullptr;
        idColor = ok ? Value<int32_t>(b, -1) : -1;
        Il2CppObject* blk = objectNew ? objectNew(mpbClass) : nullptr;
        if (blk) Call(mpbCtor, blk, {}, "MaterialPropertyBlock..ctor", &ok);
        if (blk && ok && idColor != -1) work = unity::NewGCHandle(blk);
        return work != nullptr;
    }

    void Write(Il2CppObject* r, const Color& c) {
        Il2CppObject* w = Target(work);
        if (!w) return;
        bool ok = false;
        Call(getBlock, r, {w}, "Renderer.GetPropertyBlock", &ok);  // keeps what the game / Animator put in the block
        if (!ok) return;
        Color v = c;
        Call(mpbSetColor, w, {&idColor, &v}, "MaterialPropertyBlock.SetColor", &ok);
        if (ok) Call(setBlock, r, {w}, "Renderer.SetPropertyBlock", &ok);
    }

    void Restore(Feeder& f) {
        if (f.level < 0) return;
        for (const Light& l : f.lights)
            if (Il2CppObject* r = Target(l.rend)) Write(r, l.base);
        f.level = -1;
    }

    static void FreeFeeder(Feeder& f) {
        for (Light& l : f.lights) Free(l.rend);
        f.lights.clear();
        Free(f.feeder);
        Free(f.storage);
    }

    // The port LEDs under the storage object (the feeder's root: both ports hang below it): every MeshRenderer whose
    // shared material is TechDepositor_ArrowsLed and has _Color. `renderers` = MeshRenderers seen (diagnostics).
    std::vector<Light> Lights(Il2CppObject* storage, int& renderers) {
        std::vector<Light> out;
        renderers = 0;
        Il2CppObject* tr = unity::TransformOf(storage);
        if (!tr) return out;
        bool inactive = true, ok = false;
        Il2CppObject* arr = Call(compsInChildren, tr, {unity::TypeOf(meshRenderer), &inactive},
                                 "GetComponentsInChildren(MeshRenderer)", &ok);
        if (!ok || !arr) return out;
        for (size_t i = 0, n = unity::ArrayLength(arr); i < n && i < 256; ++i) {
            auto* r = static_cast<Il2CppObject*>(unity::ArrayAt(arr, i));
            if (!unity::IsAlive(r) || !IsA(r, meshRenderer)) continue;
            ++renderers;
            Il2CppObject* mat = Call(sharedMaterial, r, {}, "Renderer.get_sharedMaterial", &ok);
            if (!ok || !unity::IsAlive(mat)) continue;
            if (unity::GetName(mat).rfind(kLedMaterial, 0) != 0) continue;  // also "... (Instance)"
            int32_t id = idColor;
            if (!Value<bool>(Call(matHas, mat, {&id}, "Material.HasProperty", &ok)) || !ok) continue;
            Light l;
            l.base = Value<Color>(Call(matGetColor, mat, {&id}, "Material.GetColor", &ok));
            if (!ok) continue;
            l.rend = unity::NewGCHandle(r);
            if (l.rend) out.push_back(l);
        }
        return out;
    }

    // Read the Dictionary / HashSet layouts from the first live objects (generic instances: resolved at run time).
    bool Layout(Il2CppObject* dict, Il2CppObject* set) {
        if (layoutBad) return false;
        auto& R = Resolver::Get();
        Il2CppClass* dk = dict ? api().il2cpp_object_get_class(dict) : nullptr;
        if (dk && dk != dictClass) {
            auto e = R.FieldOffset(dk, "_entries");
            auto c = R.FieldOffset(dk, "_count");
            if (!e || !c) {
                layoutBad = true;
                SR2_WARN("[{}] auto-feeder fill lights: unexpected Dictionary layout (no _entries / _count) - off", module);
                return false;
            }
            dictClass = dk;
            offDictEntries = *e;
            offDictCount = *c;
        }
        Il2CppClass* sk = set ? api().il2cpp_object_get_class(set) : nullptr;
        if (sk && sk != setClass) {
            const MethodInfo* m = R.Method(sk, "Contains", 1);
            if (!m) {
                layoutBad = true;
                SR2_WARN("[{}] auto-feeder fill lights: HashSet<Upgrade>.Contains not found - off", module);
                return false;
            }
            setClass = sk;
            setContains = m;
        }
        return dictClass != nullptr;
    }

    // The corrals with a bought Auto-Feeder, from the save's plot list (GameModel.landPlots): field reads plus one
    // HashSet.Contains per corral; GetComponentInChildren only for a corral not cached yet (or whose feeder died).
    void Scan(Il2CppObject* gameModel) {
        lastScan = GetTickCount64();
        scanned = true;
        Il2CppObject* dict = gameModel ? unity::Field<Il2CppObject*>(gameModel, offLandPlots) : nullptr;
        if (!dict || !Layout(dict, nullptr)) return;
        void* entries = unity::Field<void*>(dict, offDictEntries);
        const int count = unity::Field<int32_t>(dict, offDictCount);
        const size_t len = entries ? unity::ArrayLength(entries) : 0;
        if (entries && elemSize) {
            const int es = elemSize(api().il2cpp_object_get_class(static_cast<Il2CppObject*>(entries)));
            if (es != static_cast<int>(kDictEntry)) {
                layoutBad = true;
                SR2_WARN("[{}] auto-feeder fill lights: unexpected Dictionary entry size {} - off", module, es);
                return;
            }
        }
        auto* base = entries ? unity::ArrayData<unsigned char>(entries) : nullptr;
        std::vector<Feeder> next;
        int added = 0;
        for (int i = 0; base && i < count && static_cast<size_t>(i) < len && i < 512; ++i) {
            const unsigned char* e = base + static_cast<size_t>(i) * kDictEntry;
            if (*reinterpret_cast<const int32_t*>(e + 4) < -1) continue;  // free entry
            auto* model = *reinterpret_cast<Il2CppObject* const*>(e + 16);
            if (!model || unity::Field<int32_t>(model, offModelType) != kPlotCorral) continue;
            Il2CppObject* loc = unity::Field<Il2CppObject*>(model, offModelObj);
            if (!unity::IsAlive(loc) || !unity::ActiveInHierarchy(loc)) continue;  // scene not loaded
            Il2CppObject* set = unity::Field<Il2CppObject*>(model, offModelUpgrades);
            if (!set || !Layout(nullptr, set) || !setContains) continue;
            bool ok = false;
            int32_t upg = kUpgFeeder;
            if (!Value<bool>(Call(setContains, set, {&upg}, "HashSet<Upgrade>.Contains", &ok)) || !ok) continue;
            auto old = std::find_if(feeders.begin(), feeders.end(), [&](const Feeder& f) { return f.key == model; });
            if (old != feeders.end()) {
                Il2CppObject* f = Target(old->feeder);
                if (f && Target(old->storage) && unity::ActiveInHierarchy(unity::GameObjectOf(f))) {
                    next.push_back(std::move(*old));
                    feeders.erase(old);
                    continue;
                }
            }
            // New (or rebuilt) feeder: the active SlimeFeeder under the plot, its storage, its LEDs.
            bool active = false;
            Il2CppObject* tr = unity::TransformOf(loc);
            Il2CppObject* f = tr ? Call(compInChildren, tr, {unity::TypeOf(feederClass), &active},
                                        "GetComponentInChildren(SlimeFeeder)", &ok)
                                 : nullptr;
            if (!ok || !unity::IsAlive(f) || !IsA(f, feederClass)) continue;  // not active yet (just bought)
            Il2CppObject* s = unity::Field<Il2CppObject*>(f, offFeederStorage);
            if (!unity::IsAlive(s) || !IsA(s, storageClass)) continue;  // Awake not run yet
            Feeder n;
            n.key = model;
            int renderers = 0;
            n.lights = Lights(s, renderers);
            if (n.lights.empty()) {
                if (!loggedNoLights)
                    SR2_WARN("[{}] auto-feeder fill lights: no '{}' light with _Color among {} renderer(s) of a feeder", module,
                             kLedMaterial, renderers);
                loggedNoLights = true;
                continue;
            }
            n.feeder = unity::NewGCHandle(f);
            n.storage = unity::NewGCHandle(s);
            next.push_back(std::move(n));
            ++added;
            if (sr2::core::ModuleFaulted(module)) break;
        }
        for (Feeder& f : feeders) FreeFeeder(f);  // gone, sold or no longer loaded
        const size_t dropped = feeders.size();
        feeders = std::move(next);
        if (added || dropped)
            SR2_INFO("[{}] auto-feeder fill lights: {} feeder(s) ({} new, {} gone; {} light(s) on the newest)", module,
                     feeders.size(), added, dropped, feeders.empty() ? 0 : feeders.back().lights.size());
    }
};

Impl* g = nullptr;

// The LEDs' green is baked into their texture (dif_techDepositor01, about (0.33, 0.94, 0.04)) and the shader
// ("SR/UI/Digital Monitor Gradient Animated") multiplies it by _Color (white). So the tint is a multiplier, worked
// out in gamma space (SetColor converts it to linear as it does the texture, and pow is multiplicative): empty
// (3, 0.05, 0) -> red, half (3, 0.9, 0) -> yellow, full (1, 1, 1) = vanilla green. Values above 1 are fine (HDR).
Color LevelColor(int level) {
    const float t = std::clamp(static_cast<float>(level) / kLevels, 0.0f, 1.0f);
    constexpr Color kRed{3.0f, 0.05f, 0.0f, 1.0f}, kYellow{3.0f, 0.9f, 0.0f, 1.0f}, kGreen{1.0f, 1.0f, 1.0f, 1.0f};
    auto mix = [](const Color& a, const Color& b, float k) {
        return Color{a.r + (b.r - a.r) * k, a.g + (b.g - a.g) * k, a.b + (b.b - a.b) * k, 1.0f};
    };
    return t < 0.5f ? mix(kRed, kYellow, t * 2.0f) : mix(kYellow, kGreen, (t - 0.5f) * 2.0f);
}

}  // namespace

bool FeederLights::Setup(Resolver& R, const char* module, std::string& why) {
    auto cls = [&](const char* image, const char* ns, const char* name) -> Il2CppClass* {
        std::string err;
        Il2CppClass* k = R.Class(image, ns, name, &err);
        if (!k && why.empty()) why = err.empty() ? std::format("class {} not found", name) : err;
        return k;
    };
    auto byParams = [&](Il2CppClass* k, const char* name, std::initializer_list<std::string_view> params) {
        std::string err;
        const MethodInfo* m = k ? R.MethodByParams(k, name, params, &err) : nullptr;
        if (!m && why.empty()) why = std::format("{}: {}", name, err);
        return m;
    };
    auto field = [&](Il2CppClass* k, const char* name, size_t& out) {
        std::string err;
        if (auto o = k ? R.FieldOffset(k, name, &err) : std::nullopt) out = *o;
        else if (why.empty()) why = std::format("field {} not found", name);
    };
    auto* p = new Impl();
    p->module = module;
    HMODULE ga = GetModuleHandleW(L"GameAssembly.dll");
    p->objectNew = reinterpret_cast<Impl::ObjectNewFn>(ga ? GetProcAddress(ga, "il2cpp_object_new") : nullptr);
    p->elemSize = reinterpret_cast<Impl::ElemSizeFn>(ga ? GetProcAddress(ga, "il2cpp_class_array_element_size") : nullptr);
    if (!p->objectNew) why = "il2cpp_object_new export missing";
    p->feederClass = cls(kImage, "", "SlimeFeeder");
    p->storageClass = cls(kImage, "", "SiloStorage");
    Il2CppClass* gameModel = cls(kImage, kModelNs, "GameModel");
    Il2CppClass* plotModel = cls(kImage, kModelNs, "LandPlotModel");
    p->meshRenderer = cls(kCore, "UnityEngine", "MeshRenderer");
    p->mpbClass = cls(kCore, "UnityEngine", "MaterialPropertyBlock");
    Il2CppClass* renderer = cls(kCore, "UnityEngine", "Renderer");
    Il2CppClass* material = cls(kCore, "UnityEngine", "Material");
    Il2CppClass* shader = cls(kCore, "UnityEngine", "Shader");
    Il2CppClass* component = cls(kCore, "UnityEngine", "Component");
    if (!why.empty()) {
        delete p;
        return false;
    }
    field(p->feederClass, "_storage", p->offFeederStorage);
    field(gameModel, "landPlots", p->offLandPlots);
    field(plotModel, "typeId", p->offModelType);
    field(plotModel, "upgrades", p->offModelUpgrades);
    field(plotModel, "gameObj", p->offModelObj);
    p->slotCount = byParams(p->storageClass, "GetSlotCount", {"System.Int32"});
    p->slotMax = byParams(p->storageClass, "GetSlotMaxCount", {"System.Int32"});
    p->compInChildren = byParams(component, "GetComponentInChildren", {"System.Type", "System.Boolean"});
    p->compsInChildren = byParams(component, "GetComponentsInChildren", {"System.Type", "System.Boolean"});
    p->sharedMaterial = byParams(renderer, "get_sharedMaterial", {});
    p->matHas = byParams(material, "HasProperty", {"System.Int32"});
    p->matGetColor = byParams(material, "GetColor", {"System.Int32"});
    p->mpbCtor = byParams(p->mpbClass, ".ctor", {});
    p->mpbSetColor = byParams(p->mpbClass, "SetColor", {"System.Int32", "UnityEngine.Color"});
    p->getBlock = byParams(renderer, "GetPropertyBlock", {"UnityEngine.MaterialPropertyBlock"});
    p->setBlock = byParams(renderer, "SetPropertyBlock", {"UnityEngine.MaterialPropertyBlock"});
    p->propToId = byParams(shader, "PropertyToID", {"System.String"});
    if (!why.empty()) {
        delete p;
        return false;
    }

    settings::Def d;
    d.module = module;
    d.key = kLightsSetting;
    d.label = "Auto-Feeder fill lights";
    d.group = "Plot upgrades";  // round 18: "put under 'Plot upgrades'" (was a "Corrals" card)
    d.kind = settings::Kind::Bool;
    d.def = 1;
    d.tooltip = "Colours the small arrow lights on each corral's Auto-Feeder by how full it is: red when empty, "
                "yellow at half, green when full. Updated about once a second.";
    settings::Register(d);

    g = p;
    ready_ = true;
    return true;
}

void FeederLights::Tick(Il2CppObject* gameModel) {
    if (!ready_ || !g || !main_thread::IsMainThread() || g->layoutBad) return;
    if (!settings::GetBool(g->module, kLightsSetting)) {
        if (!g->feeders.empty()) Cleanup("fill lights switched off");
        return;
    }
    if (!gameModel || !g->Init()) return;
    bool dead = false;
    for (const Feeder& f : g->feeders) dead |= !Impl::Target(f.feeder) || !Impl::Target(f.storage);
    if (!g->scanned || dead || GetTickCount64() - g->lastScan >= kRescanMs) g->Scan(gameModel);
    for (Feeder& f : g->feeders) {
        Il2CppObject* s = Impl::Target(f.storage);
        if (!s) continue;
        bool ok = false;
        int32_t slot = 0;
        const int count = Impl::Value<int32_t>(g->Call(g->slotCount, s, {&slot}, "SiloStorage.GetSlotCount", &ok));
        if (!ok) continue;
        const int max = Impl::Value<int32_t>(g->Call(g->slotMax, s, {&slot}, "SiloStorage.GetSlotMaxCount", &ok));
        if (!ok) continue;
        const float fill = max > 0 ? std::clamp(static_cast<float>(count) / static_cast<float>(max), 0.0f, 1.0f) : 0.0f;
        // Any food at all shows at least one step above empty; only a truly empty feeder is pure red.
        int level = static_cast<int>(std::lround(fill * kLevels));
        if (count > 0 && level == 0) level = 1;
        if (level == f.level) continue;
        const Color c = LevelColor(level);
        for (const Light& l : f.lights)
            if (Il2CppObject* r = Impl::Target(l.rend)) g->Write(r, c);
        if (f.level < 0)
            SR2_INFO("[{}] auto-feeder fill lights: feeder {}/{} -> step {}/{}, _Color x({:.2f}, {:.2f}, {:.2f}) on {} light(s)",
                     g->module, count, max, level, kLevels, c.r, c.g, c.b, f.lights.size());
        f.level = level;
        if (sr2::core::ModuleFaulted(g->module)) return;
    }
}

void FeederLights::Cleanup(const char* why) {
    if (!ready_ || !g) return;
    if (g->feeders.empty()) return;
    const bool main = main_thread::IsMainThread() && !sr2::core::ModuleFaulted(g->module);
    for (Feeder& f : g->feeders) {
        if (main) g->Restore(f);
        Impl::FreeFeeder(f);
    }
    g->feeders.clear();
    g->scanned = false;
    SR2_INFO("[{}] {}: auto-feeder fill lights back to vanilla green", g->module, why);
}

void FeederLights::Reset() {
    if (!ready_ || !g) return;
    for (Feeder& f : g->feeders) Impl::FreeFeeder(f);
    g->feeders.clear();
    g->scanned = false;
}

}  // namespace sr2::modules::custom_upgrades::feeder
