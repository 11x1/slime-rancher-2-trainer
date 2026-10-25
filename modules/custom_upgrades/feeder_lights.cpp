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
constexpr uint64_t kRescanMs = 10000;  // FindObjectsOfTypeAll(SlimeFeeder) at most this often
constexpr int kLevels = 20;            // fill steps; a block is written only when the step changes

struct Color {
    float r, g, b, a;
};

struct Light {
    Il2CppGCHandle rend = nullptr;
    Color high{}, low{};  // the material's own colours (restore)
};
struct Feeder {
    Il2CppObject* key = nullptr;  // SlimeFeeder (identity only; read through `feeder`)
    Il2CppGCHandle feeder = nullptr, storage = nullptr;
    std::vector<Light> lights;
    int level = -1;  // last written fill step, -1 = not tinted
};

struct Impl {
    const char* module = "custom_upgrades";
    using ObjectNewFn = Il2CppObject* (*)(Il2CppClass*);
    ObjectNewFn objectNew = nullptr;
    Il2CppClass* feederClass = nullptr;
    Il2CppClass* storageClass = nullptr;
    Il2CppClass* meshRenderer = nullptr;
    Il2CppClass* mpbClass = nullptr;
    size_t offFeederStorage = 0;  // SlimeFeeder._storage
    const MethodInfo* slotCount = nullptr;      // SiloStorage.GetSlotCount(int)
    const MethodInfo* slotMax = nullptr;        // SiloStorage.GetSlotMaxCount(int)
    const MethodInfo* compsInChildren = nullptr;  // Component.GetComponentsInChildren(Type, bool)
    const MethodInfo* sharedMaterial = nullptr;   // Renderer.get_sharedMaterial
    const MethodInfo* matHas = nullptr;           // Material.HasProperty(int)
    const MethodInfo* matGetColor = nullptr;      // Material.GetColor(int)
    const MethodInfo* mpbCtor = nullptr;
    const MethodInfo* mpbSetColor = nullptr;   // MaterialPropertyBlock.SetColor(int, Color)
    const MethodInfo* getBlock = nullptr;      // Renderer.GetPropertyBlock(MaterialPropertyBlock)
    const MethodInfo* setBlock = nullptr;      // Renderer.SetPropertyBlock(MaterialPropertyBlock)
    const MethodInfo* propToId = nullptr;      // Shader.PropertyToID(string)
    int idHigh = -1, idLow = -1;               // _EmissionGlowHigh / _EmissionGlowLow
    Il2CppGCHandle work = nullptr;             // shared MaterialPropertyBlock
    std::vector<Feeder> feeders;
    uint64_t lastScan = 0;
    bool scanned = false;
    bool loggedSetup = false;

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

    // Main thread, once: property ids and the shared block (needs the game's runtime up).
    bool Init() {
        if (work) return true;
        bool ok = false;
        auto id = [&](const char* name) {
            Il2CppString* s = unity::NewString(name);
            Il2CppObject* b = s ? Call(propToId, nullptr, {s}, "Shader.PropertyToID", &ok) : nullptr;
            return ok ? Value<int32_t>(b, -1) : -1;
        };
        idHigh = id("_EmissionGlowHigh");
        idLow = id("_EmissionGlowLow");
        Il2CppObject* b = objectNew ? objectNew(mpbClass) : nullptr;
        if (b) Call(mpbCtor, b, {}, "MaterialPropertyBlock..ctor", &ok);
        if (b && ok && idHigh != -1 && idLow != -1) work = unity::NewGCHandle(b);
        return work != nullptr;
    }

    void Write(Il2CppObject* r, const Color& high, const Color& low) {
        Il2CppObject* w = Target(work);
        if (!w) return;
        bool ok = false;
        Call(getBlock, r, {w}, "Renderer.GetPropertyBlock", &ok);  // keeps what the game put in the block
        if (!ok) return;
        Color h = high, l = low;
        Call(mpbSetColor, w, {&idHigh, &h}, "MaterialPropertyBlock.SetColor", &ok);
        if (ok) Call(mpbSetColor, w, {&idLow, &l}, "MaterialPropertyBlock.SetColor", &ok);
        if (ok) Call(setBlock, r, {w}, "Renderer.SetPropertyBlock", &ok);
    }

    void Restore(Feeder& f) {
        if (f.level < 0) return;
        for (const Light& l : f.lights)
            if (Il2CppObject* r = Target(l.rend)) Write(r, l.high, l.low);
        f.level = -1;
    }

    static void FreeFeeder(Feeder& f) {
        for (Light& l : f.lights) Free(l.rend);
        f.lights.clear();
        Free(f.feeder);
        Free(f.storage);
    }

    // The tintable lights under the storage object (the feeder's root: both ports hang below it).
    std::vector<Light> Lights(Il2CppObject* storage) {
        std::vector<Light> out;
        Il2CppObject* tr = unity::TransformOf(storage);
        if (!tr) return out;
        bool inactive = true, ok = false;
        Il2CppObject* arr = Call(compsInChildren, tr, {unity::TypeOf(meshRenderer), &inactive},
                                 "GetComponentsInChildren(MeshRenderer)", &ok);
        if (!ok || !arr) return out;
        for (size_t i = 0, n = unity::ArrayLength(arr); i < n && i < 256; ++i) {
            auto* r = static_cast<Il2CppObject*>(unity::ArrayAt(arr, i));
            if (!unity::IsAlive(r) || !IsA(r, meshRenderer)) continue;
            Il2CppObject* mat = Call(sharedMaterial, r, {}, "Renderer.get_sharedMaterial", &ok);
            if (!ok || !unity::IsAlive(mat)) continue;
            int32_t idh = idHigh, idl = idLow;
            if (!Value<bool>(Call(matHas, mat, {&idh}, "Material.HasProperty", &ok)) || !ok) continue;
            if (!Value<bool>(Call(matHas, mat, {&idl}, "Material.HasProperty", &ok)) || !ok) continue;
            Light l;
            l.high = Value<Color>(Call(matGetColor, mat, {&idh}, "Material.GetColor", &ok));
            if (!ok) continue;
            l.low = Value<Color>(Call(matGetColor, mat, {&idl}, "Material.GetColor", &ok));
            if (!ok) continue;
            l.rend = unity::NewGCHandle(r);
            if (l.rend) out.push_back(l);
        }
        return out;
    }

    void Scan() {
        lastScan = GetTickCount64();
        scanned = true;
        std::vector<Feeder> next;
        int added = 0;
        for (Il2CppObject* f : unity::FindObjectsOfTypeAll(feederClass)) {
            if (!unity::IsAlive(f) || !IsA(f, feederClass)) continue;
            Il2CppObject* go = unity::GameObjectOf(f);
            if (!go || !unity::ActiveInHierarchy(go)) continue;  // prefabs, unbought feeders
            Il2CppObject* s = unity::Field<Il2CppObject*>(f, offFeederStorage);
            if (!unity::IsAlive(s) || !IsA(s, storageClass)) continue;  // Awake not run yet
            auto old = std::find_if(feeders.begin(), feeders.end(), [&](const Feeder& e) { return e.key == f; });
            if (old != feeders.end() && Target(old->feeder) == f) {
                next.push_back(std::move(*old));
                feeders.erase(old);
                continue;
            }
            Feeder n;
            n.key = f;
            n.lights = Lights(s);
            if (n.lights.empty()) continue;
            n.feeder = unity::NewGCHandle(f);
            n.storage = unity::NewGCHandle(s);
            next.push_back(std::move(n));
            ++added;
            if (sr2::core::ModuleFaulted(module)) break;
        }
        for (Feeder& f : feeders) FreeFeeder(f);  // gone or no longer active
        const size_t dropped = feeders.size();
        feeders = std::move(next);
        if (added || dropped)
            SR2_INFO("[{}] auto-feeder fill lights: {} feeder(s) ({} new, {} gone)", module, feeders.size(), added,
                     dropped);
    }
};

Impl* g = nullptr;

// Empty red -> half yellow -> full vanilla green; Low keeps vanilla's softer tone of the same colour.
void LevelColors(int level, Color& high, Color& low) {
    const float t = std::clamp(static_cast<float>(level) / kLevels, 0.0f, 1.0f);
    constexpr Color kRed{1.0f, 0.04f, 0.0f, 1.0f}, kYellow{1.0f, 0.85f, 0.0f, 1.0f}, kGreen{0.368f, 1.0f, 0.0f, 1.0f};
    auto mix = [](const Color& a, const Color& b, float k) {
        return Color{a.r + (b.r - a.r) * k, a.g + (b.g - a.g) * k, a.b + (b.b - a.b) * k, 1.0f};
    };
    high = t < 0.5f ? mix(kRed, kYellow, t * 2.0f) : mix(kYellow, kGreen, (t - 0.5f) * 2.0f);
    low = Color{high.r * 0.6f + 0.15f, high.g * 0.6f + 0.15f, high.b * 0.6f + 0.15f, 1.0f};
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
    auto* p = new Impl();
    p->module = module;
    HMODULE ga = GetModuleHandleW(L"GameAssembly.dll");
    p->objectNew = reinterpret_cast<Impl::ObjectNewFn>(ga ? GetProcAddress(ga, "il2cpp_object_new") : nullptr);
    if (!p->objectNew) why = "il2cpp_object_new export missing";
    p->feederClass = cls(kImage, "", "SlimeFeeder");
    p->storageClass = cls(kImage, "", "SiloStorage");
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
    std::string err;
    if (auto o = R.FieldOffset(p->feederClass, "_storage", &err)) p->offFeederStorage = *o;
    else why = "field SlimeFeeder._storage not found";
    p->slotCount = byParams(p->storageClass, "GetSlotCount", {"System.Int32"});
    p->slotMax = byParams(p->storageClass, "GetSlotMaxCount", {"System.Int32"});
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
    d.group = "Corrals";
    d.kind = settings::Kind::Bool;
    d.def = 1;
    d.tooltip = "Colours the small arrow lights on each corral's Auto-Feeder by how full it is: red when empty, "
                "yellow at half, green when full. Updated about once a second.";
    settings::Register(d);

    g = p;
    ready_ = true;
    return true;
}

void FeederLights::Tick() {
    if (!ready_ || !g || !main_thread::IsMainThread()) return;
    if (!settings::GetBool(g->module, kLightsSetting)) {
        if (!g->feeders.empty()) Cleanup("fill lights switched off");
        return;
    }
    if (!g->Init()) return;
    bool dead = false;
    for (const Feeder& f : g->feeders) dead |= !Impl::Target(f.feeder) || !Impl::Target(f.storage);
    if (!g->scanned || dead || GetTickCount64() - g->lastScan >= kRescanMs) g->Scan();
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
        Color high{}, low{};
        LevelColors(level, high, low);
        for (const Light& l : f.lights)
            if (Il2CppObject* r = Impl::Target(l.rend)) g->Write(r, high, low);
        if (f.level < 0)
            SR2_DEBUG("[{}] auto-feeder fill lights: feeder {}/{} -> step {}/{} ({} light(s))", g->module, count, max,
                      level, kLevels, f.lights.size());
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
