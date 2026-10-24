// custom_upgrades: the silo Sell-All world button (see silo_world.h). SR2 1.3.0, all lookups by name.
//
// Source (asset bundle dump of the CORRAL prefab "patchCorral"): PlortCollectorUpgrader.Collector
// = the inactive "PlortCollector" part (rotated -90 deg). Its button is the child "techActivator"
// at (5.58, 1.20, -5.58) / 135 deg in that part's space = the plot's outer (+x, +z) corner, facing
// outwards: Animator (trigger "ButtonPressed"), SphereCollider, ApplyPalette; children
// "triggerActivate" (PlortCollectorActivator + TechUIInteractable: the "press E" prompt, which
// calls ITechActivator.Activate on the same object), "button_stand" (down to the ground, local
// y -1.16), "button_base" / "button_button".
//
// PlortCollectorActivator.Activate (0x52DD60): returns if Time.timeAsDouble < _nextAllowed;
// Collector.StartCollection(); _buttonAnimator.SetTrigger(_buttonPressedTriggerId);
// SECTR_AudioSystem.Play(PressButtonCue, transform.position, false); _nextAllowed = now + 0.4.
// Awake: _buttonAnimator = GetComponentInParent<Animator>() (the techActivator object, which we
// clone together with it), _buttonPressedTriggerId = Animator.StringToHash("ButtonPressed").
//
// Our clone: Instantiate("techActivator", siloPlot.transform) (the object is active in the prefab,
// so Awake runs right away and only caches the animator), renamed
// "sr2mods_SiloSellButton#<generation>", its activator's Collector set to null (the original
// would start the corral PREFAB's collector; with null it can never run anything: module.cpp's
// Activate detour never calls the original for an activator without a Collector). Placed at the
// silo plot's outer (+x, -z) corner (5.58, 1.20, -5.58), turned 135 deg so it faces outwards:
// the same front side as the plot menu terminal at (-5.54, -5.54), clear of the slot panels at
// the middle of each side (silo prefab "patchSilo": Addition 0..3 at 3.05 m from the centre,
// corner posts at 4.75).
//
// Tint (user request: "modify the sell button model colors so I can see it's not the menu-open one"):
// the clone looks like the plot menu terminal's button, so it gets a gold accent. The prefab's
// ApplyPalette swaps the renderers' shared materials for recoloured ones in its Start (the frame after
// Instantiate, ColorPaletteApplicator.SetupForTarget: sharedMaterials -> the palette's material map, a
// material not in the map is kept), so the tint waits for the next ~1 Hz tick: then Renderer.get_materials
// (instanced copies of the palette's materials) and every Color property of each material's shader
// (Shader.GetPropertyCount / GetPropertyType == Color / GetPropertyNameId) blended towards gold, alpha kept.
// The material is the palette shader "RanchTech01" (corral prefab, UnityPy): 24 colour slots _Color00 ..
// _Color72 picked per vertex through a UV2 colour mask - the cap ("button_button") uses all 24, which is
// what the first log showed ("24 colour(s) on button_button").
// Round 7 test: "button is NOT gold" although that log said it was tinted (kTintCap 0.6, cap only). Two
// weak spots, both fixed: (1) the tint ran once per button object, remembered by the raw GameObject
// pointer (no GC handle - a rebuilt button could get the old wrapper's address and stay untinted), and
// nothing noticed if anything later put other materials on the renderers; (2) colour props on the material
// lose against a MaterialPropertyBlock (the game's StaticColorizer, ColorPalette.SetColors(Renderer), and
// animated material curves write those; also renderers drawn by the GPU Resident Drawer). Now:
//   * our instanced materials are named kGoldMat; every tick checks the cap's shared material name and
//     tints again when it is not ours (log "materials were replaced (was '...') - tinted again"), so
//     whatever happens to the materials, the button turns gold within a second;
//   * the same gold colours also go into a MaterialPropertyBlock on each tinted single-material renderer
//     (Renderer.GetPropertyBlock / SetPropertyBlock), which wins over material values at draw time;
//   * stronger: the cap 85 % gold, the base ("button_base") 35 %; without a cap every renderer 40 %.
// The log line lists the renderers, colour counts and whether the property block was set. A later
// palette change finds our instanced materials in no palette map and leaves them as they are. The
// instanced materials are not destroyed with the clone (a few per rebuild; the next scene unload frees
// them).
//
// Silo plots: the plot ids module.cpp passes -> SceneContext.GameModel.GetLandPlotModel(id)
// (a TryGetValue, null if unknown) -> LandPlotModel.gameObj (the LandPlotLocation object) ->
// GetComponentInChildren<LandPlot>. A demolished / rebuilt plot is a new object: its button went
// with the old one and is rebuilt at the next tick.

#include "modules/custom_upgrades/silo_world.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
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
#include "ui/hud.h"

namespace sr2::modules::custom_upgrades::world {
namespace {

namespace unity = sr2::core::unity;
using sr2::il2cpp::api;
using sr2::il2cpp::Resolver;

constexpr const char* kImage = "Assembly-CSharp";
constexpr const char* kCore = "UnityEngine.CoreModule";
constexpr int32_t kPlotCorral = 2;
constexpr int32_t kPlotSilo = 5;
constexpr std::string_view kName = "sr2mods_SiloSellButton#";
constexpr const char* kHudOwner = "custom_upgrades.silo_button";
constexpr unsigned long long kPressGapMs = 400;  // vanilla TIME_BETWEEN_ACTIVATIONS
constexpr float kMessageRange = 40.0f;
constexpr float kTintCap = 0.85f;   // blend towards kTintColor: the cap
constexpr float kTintBase = 0.35f;  // ... the button's base
constexpr float kTintAll = 0.4f;    // ... every renderer (no cap found)
constexpr std::string_view kGoldMat = "sr2mods_gold";  // name of our instanced (tinted) materials
constexpr int kShaderPropColor = 0;  // UnityEngine.Rendering.ShaderPropertyType.Color

struct Vec3 {
    float x, y, z;
};
struct Quat {
    float x, y, z, w;
};
struct Mat4 {
    float m[16];
};
struct Color {
    float r, g, b, a;
};
constexpr Color kTintColor{1.0f, 0.78f, 0.18f, 1.0f};  // gold (the Newbucks coin)

}  // namespace

struct SiloButtons::Impl {
    const char* module = "custom_upgrades";
    Il2CppClass* plotClass = nullptr;
    Il2CppClass* upgraderClass = nullptr;   // PlortCollectorUpgrader
    Il2CppClass* activatorClass = nullptr;  // PlortCollectorActivator
    Il2CppClass* gameObjectClass = nullptr;
    Il2CppClass* cameraClass = nullptr;
    size_t offPlotType = 0, offUpgCollector = 0, offActCollector = 0, offActCue = 0, offActAnim = 0, offActTrigger = 0;
    size_t offScGameModel = 0, offModelGameObj = 0;
    using WBarrierFn = void (*)(Il2CppObject*, void**, void*);
    WBarrierFn wbarrier = nullptr;

    const MethodInfo* sceneInstance = nullptr;   // SRSingleton<SceneContext>.get_Instance
    const MethodInfo* getPlotModel = nullptr;    // GameModel.GetLandPlotModel(string)
    const MethodInfo* getPlotPrefab = nullptr;   // LookupDirector.GetPlotPrefab(Id)
    const MethodInfo* compInParent = nullptr;
    const MethodInfo* compInChildren = nullptr;
    const MethodInfo* trChildCount = nullptr;
    const MethodInfo* trGetChild = nullptr;
    const MethodInfo* trGetParent = nullptr;
    const MethodInfo* trGetPosition = nullptr;
    const MethodInfo* trSetLocalPosition = nullptr;
    const MethodInfo* trSetLocalRotation = nullptr;
    const MethodInfo* trSetLocalScale = nullptr;
    const MethodInfo* objDestroy = nullptr;
    const MethodInfo* objDestroyImmediate = nullptr;
    const MethodInfo* animSetTrigger = nullptr;  // Animator.SetTrigger(int)
    const MethodInfo* audioPlay = nullptr;       // SECTR_AudioSystem.Play(cue, Vector3, bool) (optional)
    using MatFn = Mat4 (*)(Il2CppObject*, const MethodInfo*);
    MatFn worldToCamera = nullptr, projection = nullptr;
    const MethodInfo* worldToCameraMi = nullptr;
    const MethodInfo* projectionMi = nullptr;

    // tint (optional: without these the button keeps the prefab's colours)
    Il2CppClass* rendererClass = nullptr;
    const MethodInfo* compsInChildren = nullptr;  // Component.GetComponentsInChildren(Type, bool)
    const MethodInfo* rendMaterials = nullptr;    // Renderer.get_materials (instanced copies)
    const MethodInfo* matShader = nullptr;        // Material.get_shader
    const MethodInfo* matGetColor = nullptr;      // Material.GetColor(int)
    const MethodInfo* matSetColor = nullptr;      // Material.SetColor(int, Color)
    const MethodInfo* shPropCount = nullptr;      // Shader.GetPropertyCount()
    const MethodInfo* shPropType = nullptr;       // Shader.GetPropertyType(int)
    const MethodInfo* shPropNameId = nullptr;     // Shader.GetPropertyNameId(int)
    const MethodInfo* rendSharedMaterial = nullptr;  // Renderer.get_sharedMaterial
    // optional: the property block (missing -> material colours only)
    Il2CppClass* mpbClass = nullptr;              // MaterialPropertyBlock
    const MethodInfo* mpbCtor = nullptr;
    const MethodInfo* mpbSetColor = nullptr;      // MaterialPropertyBlock.SetColor(int, Color)
    const MethodInfo* rendGetBlock = nullptr;     // Renderer.GetPropertyBlock(MaterialPropertyBlock)
    const MethodInfo* rendSetBlock = nullptr;     // Renderer.SetPropertyBlock(MaterialPropertyBlock)
    using ObjectNewFn = Il2CppObject* (*)(Il2CppClass*);
    ObjectNewFn objectNew = nullptr;
    std::map<std::string, int> tintCount;  // plot id -> tints so far (a re-tint after the first is logged)
    bool loggedTint = false;
    int loggedRetint = 0;

    bool loggedNoSource = false;
    bool broken = false;
    unsigned long long lastPress = 0;

    // world message (ui::hud)
    bool msgActive = false;
    unsigned long long msgUntil = 0;
    Vec3 msgPos{};
    std::string msgText;
    uint32_t msgColor = 0;

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
    Il2CppObject* InParent(Il2CppObject* comp, Il2CppClass* k) {
        bool inactive = true, ok = false;
        Il2CppObject* r = Call(compInParent, comp, {unity::TypeOf(k), &inactive}, "GetComponentInParent", &ok);
        return ok && unity::IsAlive(r) && IsA(r, k) ? r : nullptr;
    }
    Il2CppObject* InChildren(Il2CppObject* comp, Il2CppClass* k) {
        bool inactive = true, ok = false;
        Il2CppObject* r = Call(compInChildren, comp, {unity::TypeOf(k), &inactive}, "GetComponentInChildren", &ok);
        return ok && unity::IsAlive(r) && IsA(r, k) ? r : nullptr;
    }
    Il2CppObject* Parent(Il2CppObject* tr) {
        bool ok = false;
        Il2CppObject* p = Call(trGetParent, tr, {}, "Transform.get_parent", &ok);
        return ok && unity::IsAlive(p) ? p : nullptr;
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
    void Destroy(Il2CppObject* go) {
        if (unity::IsAlive(go)) Call(objDestroy, nullptr, {go}, "Object.Destroy");
    }
    void DestroyNow(Il2CppObject* go) {
        if (unity::IsAlive(go)) Call(objDestroyImmediate, nullptr, {go}, "Object.DestroyImmediate");
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
        return unity::IsAlive(tr) ? InChildren(tr, plotClass) : nullptr;
    }
    // Our current-generation button under the plot; destroys stale / duplicate ones (or all).
    Il2CppObject* Scan(Il2CppObject* plot, unsigned generation, bool destroyAll) {
        Il2CppObject* tr = unity::TransformOf(plot);
        if (!unity::IsAlive(tr)) return nullptr;
        const std::string current = std::format("{}{}", kName, generation);
        Il2CppObject* found = nullptr;
        for (Il2CppObject* child : ChildTransforms(tr)) {
            Il2CppObject* go = unity::GameObjectOf(child);
            if (!go) continue;
            const std::string name = unity::GetName(go);
            if (name.rfind(kName, 0) != 0) continue;
            if (destroyAll || name != current || found) Destroy(go);
            else found = go;
        }
        return found;
    }
    Il2CppObject* Source(Il2CppObject* lookup) {
        if (!unity::IsAlive(lookup)) return nullptr;
        int32_t id = kPlotCorral;
        bool ok = false;
        Il2CppObject* prefab = Call(getPlotPrefab, lookup, {&id}, "LookupDirector.GetPlotPrefab", &ok);
        if (!ok || !unity::IsAlive(prefab) || !IsA(prefab, gameObjectClass)) return nullptr;
        Il2CppObject* up = unity::GetComponent(prefab, upgraderClass);
        if (!unity::IsAlive(up) || !IsA(up, upgraderClass)) return nullptr;
        Il2CppObject* part = unity::Field<Il2CppObject*>(up, offUpgCollector);
        if (!unity::IsAlive(part) || !IsA(part, gameObjectClass)) return nullptr;
        Il2CppObject* partTr = unity::TransformOf(part);
        Il2CppObject* act = InChildren(partTr, activatorClass);
        Il2CppObject* actTr = act ? unity::TransformOf(act) : nullptr;
        Il2CppObject* button = actTr ? Parent(actTr) : nullptr;  // "techActivator" (Animator + meshes)
        if (!button || button == partTr) return nullptr;
        Il2CppObject* go = unity::GameObjectOf(button);
        return unity::IsAlive(go) ? go : nullptr;
    }
    void Create(Il2CppObject* plot, Il2CppObject* lookup, const std::string& id, unsigned generation);
    void Tint(Il2CppObject* button, const std::string& id);
};

// ---------------------------------------------------------------- setup --

bool SiloButtons::Setup(Resolver& R, const char* module, std::string& why) {
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
    p->wbarrier = reinterpret_cast<Impl::WBarrierFn>(ga ? GetProcAddress(ga, "il2cpp_gc_wbarrier_set_field") : nullptr);

    p->plotClass = cls(kImage, "", "LandPlot");
    p->upgraderClass = cls(kImage, "", "PlortCollectorUpgrader");
    p->activatorClass = cls(kImage, "", "PlortCollectorActivator");
    Il2CppClass* sceneCtx = cls(kImage, "", "SceneContext");
    Il2CppClass* gameModel = cls(kImage, "MonomiPark.SlimeRancher.DataModel", "GameModel");
    Il2CppClass* plotModel = cls(kImage, "MonomiPark.SlimeRancher.DataModel", "LandPlotModel");
    Il2CppClass* lookup = cls(kImage, "", "LookupDirector");
    Il2CppClass* animator = cls("UnityEngine.AnimationModule", "UnityEngine", "Animator");
    Il2CppClass* component = cls(kCore, "UnityEngine", "Component");
    Il2CppClass* transform = cls(kCore, "UnityEngine", "Transform");
    Il2CppClass* uobject = cls(kCore, "UnityEngine", "Object");
    p->gameObjectClass = cls(kCore, "UnityEngine", "GameObject");
    p->cameraClass = cls(kCore, "UnityEngine", "Camera");
    if (!why.empty()) {
        delete p;
        return false;
    }
    off(p->plotClass, "TypeId", p->offPlotType);
    off(p->upgraderClass, "Collector", p->offUpgCollector);
    off(p->activatorClass, "Collector", p->offActCollector);
    off(p->activatorClass, "PressButtonCue", p->offActCue);
    off(p->activatorClass, "_buttonAnimator", p->offActAnim);
    off(p->activatorClass, "_buttonPressedTriggerId", p->offActTrigger);
    off(sceneCtx, "<GameModel>k__BackingField", p->offScGameModel);
    off(plotModel, "gameObj", p->offModelGameObj);
    p->sceneInstance = method(parentOf(sceneCtx), "get_Instance", 0);
    p->getPlotModel = method(gameModel, "GetLandPlotModel", 1);
    p->getPlotPrefab = method(lookup, "GetPlotPrefab", 1);
    p->compInParent = byParams(component, "GetComponentInParent", {"System.Type", "System.Boolean"});
    p->compInChildren = byParams(component, "GetComponentInChildren", {"System.Type", "System.Boolean"});
    p->trChildCount = method(transform, "get_childCount", 0);
    p->trGetChild = method(transform, "GetChild", 1);
    p->trGetParent = method(transform, "get_parent", 0);
    p->trGetPosition = method(transform, "get_position", 0);
    p->trSetLocalPosition = method(transform, "set_localPosition", 1);
    p->trSetLocalRotation = method(transform, "set_localRotation", 1);
    p->trSetLocalScale = method(transform, "set_localScale", 1);
    p->objDestroy = byParams(uobject, "Destroy", {"UnityEngine.Object"});
    p->objDestroyImmediate = byParams(uobject, "DestroyImmediate", {"UnityEngine.Object"});
    p->animSetTrigger = byParams(animator, "SetTrigger", {"System.Int32"});
    p->worldToCameraMi = method(p->cameraClass, "get_worldToCameraMatrix", 0);
    p->projectionMi = method(p->cameraClass, "get_projectionMatrix", 0);
    if (why.empty()) {
        std::string err;
        p->worldToCamera = reinterpret_cast<Impl::MatFn>(R.MethodPointer(p->worldToCameraMi, &err));
        p->projection = reinterpret_cast<Impl::MatFn>(R.MethodPointer(p->projectionMi, &err));
        if (!p->worldToCamera || !p->projection) why = "Camera matrix getters: " + err;
    }
    if (!why.empty()) {
        delete p;
        return false;
    }
    {
        std::string err;  // optional: the press sound
        if (Il2CppClass* audio = R.Class(kImage, "", "SECTR_AudioSystem", &err))
            p->audioPlay = R.MethodByParams(audio, "Play", {"SECTR_AudioCue", "UnityEngine.Vector3", "System.Boolean"}, &err);
    }
    {
        std::string err;  // optional: the gold tint
        p->rendererClass = R.Class(kCore, "UnityEngine", "Renderer", &err);
        Il2CppClass* material = R.Class(kCore, "UnityEngine", "Material", &err);
        Il2CppClass* shader = R.Class(kCore, "UnityEngine", "Shader", &err);
        if (p->rendererClass && material && shader) {
            p->compsInChildren = R.MethodByParams(component, "GetComponentsInChildren", {"System.Type", "System.Boolean"}, &err);
            p->rendMaterials = R.Method(p->rendererClass, "get_materials", 0, &err);
            p->matShader = R.Method(material, "get_shader", 0, &err);
            p->matGetColor = R.MethodByParams(material, "GetColor", {"System.Int32"}, &err);
            p->matSetColor = R.MethodByParams(material, "SetColor", {"System.Int32", "UnityEngine.Color"}, &err);
            p->shPropCount = R.Method(shader, "GetPropertyCount", 0, &err);
            p->shPropType = R.MethodByParams(shader, "GetPropertyType", {"System.Int32"}, &err);
            p->shPropNameId = R.MethodByParams(shader, "GetPropertyNameId", {"System.Int32"}, &err);
            p->rendSharedMaterial = R.Method(p->rendererClass, "get_sharedMaterial", 0, &err);
            std::string berr;  // optional: the property block
            p->objectNew = reinterpret_cast<Impl::ObjectNewFn>(ga ? GetProcAddress(ga, "il2cpp_object_new") : nullptr);
            p->mpbClass = R.Class(kCore, "UnityEngine", "MaterialPropertyBlock", &berr);
            if (p->mpbClass) {
                p->mpbCtor = R.Method(p->mpbClass, ".ctor", 0, &berr);
                p->mpbSetColor = R.MethodByParams(p->mpbClass, "SetColor", {"System.Int32", "UnityEngine.Color"}, &berr);
                p->rendGetBlock = R.MethodByParams(p->rendererClass, "GetPropertyBlock", {"UnityEngine.MaterialPropertyBlock"}, &berr);
                p->rendSetBlock = R.MethodByParams(p->rendererClass, "SetPropertyBlock", {"UnityEngine.MaterialPropertyBlock"}, &berr);
            }
            if (!p->objectNew || !p->mpbClass || !p->mpbCtor || !p->mpbSetColor || !p->rendGetBlock || !p->rendSetBlock) {
                p->mpbClass = nullptr;
                SR2_WARN("[{}] silo sell button: no MaterialPropertyBlock ({}) - material colours only", module,
                         berr.empty() ? "a method is missing" : berr);
            }
        }
        if (!p->compsInChildren || !p->rendMaterials || !p->matShader || !p->matGetColor || !p->matSetColor ||
            !p->shPropCount || !p->shPropType || !p->shPropNameId || !p->rendSharedMaterial) {
            p->rendererClass = nullptr;
            SR2_WARN("[{}] silo sell button tint unavailable ({}) - the button keeps the prefab's colours", module,
                     err.empty() ? "a Renderer / Material / Shader method is missing" : err);
        }
    }
    impl_ = p;
    ready_ = true;
    return true;
}

// ---------------------------------------------------------------- build --

void SiloButtons::Impl::Create(Il2CppObject* plot, Il2CppObject* lookup, const std::string& id, unsigned generation) {
    if (broken) return;
    Il2CppObject* src = Source(lookup);
    if (!src) {
        if (!loggedNoSource) SR2_WARN("[{}] silo sell button: corral Plort Collector button not found in the prefab", module);
        loggedNoSource = true;
        return;
    }
    Il2CppObject* plotTr = unity::TransformOf(plot);
    Il2CppObject* clone = unity::Instantiate(src, plotTr);
    if (!unity::IsAlive(clone) || !IsA(clone, gameObjectClass)) {
        SR2_WARN("[{}] silo sell button: Instantiate failed", module);
        broken = true;
        return;
    }
    unity::SetName(clone, std::format("{}{}", kName, generation));
    Il2CppObject* root = unity::TransformOf(clone);
    Il2CppObject* act = InChildren(root, activatorClass);
    if (!act || unity::Field<Il2CppObject*>(act, offActCollector) == nullptr) {
        // Never expected: no activator, or the prefab's own (shared) one.
        SR2_WARN("[{}] silo sell button: cloned button has no own PlortCollectorActivator - not built", module);
        DestroyNow(clone);
        broken = true;
        return;
    }
    // Never let vanilla code start the corral prefab's collector from our clone.
    void** slot = reinterpret_cast<void**>(reinterpret_cast<char*>(act) + offActCollector);
    if (wbarrier) wbarrier(act, slot, nullptr);
    else *slot = nullptr;
    Vec3 pos{5.58f, 1.20f, -5.58f};                // outer (+x, -z) corner, front side of the silo
    Quat rot{0.0f, 0.92387953f, 0.0f, 0.38268343f};  // 135 deg: faces outwards (diagonal)
    Vec3 one{1.0f, 1.0f, 1.0f};
    Call(trSetLocalPosition, root, {&pos}, "Transform.set_localPosition");
    Call(trSetLocalRotation, root, {&rot}, "Transform.set_localRotation");
    Call(trSetLocalScale, root, {&one}, "Transform.set_localScale");
    if (sr2::core::ModuleFaulted(module)) {
        DestroyNow(clone);
        broken = true;
        return;
    }
    unity::SetActive(clone, true);
    SR2_INFO("[{}] silo sell button built on plot '{}' (clone of corral '{}')", module, id, unity::GetName(src));
}

// Gold accent (see the header comment). Called on every tick for a built button: returns at once
// while the cap still carries our gold material.
void SiloButtons::Impl::Tint(Il2CppObject* button, const std::string& id) {
    if (!rendererClass) return;
    Il2CppObject* root = unity::TransformOf(button);
    if (!unity::IsAlive(root)) return;
    bool inactive = true, ok = false;
    Il2CppObject* arr = Call(compsInChildren, root, {unity::TypeOf(rendererClass), &inactive},
                             "GetComponentsInChildren(Renderer)", &ok);
    if (!ok || !arr) return;
    struct Target {
        Il2CppObject* r;
        std::string name;
        float t;
    };
    std::vector<Target> renderers;
    bool anyCap = false;
    for (size_t i = 0, n = unity::ArrayLength(arr); i < n && i < 32; ++i) {
        auto* r = static_cast<Il2CppObject*>(unity::ArrayAt(arr, i));
        if (!unity::IsAlive(r) || !IsA(r, rendererClass)) continue;
        Il2CppObject* go = unity::GameObjectOf(r);
        std::string name = go ? unity::GetName(go) : std::string();
        anyCap |= name.find("button_button") != std::string::npos;
        renderers.push_back({r, std::move(name), kTintAll});
    }
    if (anyCap) {
        std::erase_if(renderers, [](const Target& t) {
            return t.name.find("button_button") == std::string::npos && t.name.find("button_base") == std::string::npos;
        });
        for (Target& t : renderers) t.t = t.name.find("button_button") != std::string::npos ? kTintCap : kTintBase;
    }
    // Still ours? The first target's shared material keeps our name until something replaces it.
    if (renderers.empty()) return;
    Il2CppObject* shared = Call(rendSharedMaterial, renderers.front().r, {}, "Renderer.get_sharedMaterial", &ok);
    const std::string was = ok && unity::IsAlive(shared) ? unity::GetName(shared) : std::string();
    if (was.rfind(kGoldMat, 0) == 0) return;
    std::string done;
    int colours = 0, blocks = 0;
    for (const Target& tg : renderers) {
        Il2CppObject* mats = Call(rendMaterials, tg.r, {}, "Renderer.get_materials", &ok);
        if (!ok || !mats) continue;
        const size_t nm = unity::ArrayLength(mats);
        // One property block per single-material renderer, filled with the same gold colours.
        Il2CppObject* mpb = nullptr;
        if (nm == 1 && mpbClass && objectNew) {
            mpb = objectNew(mpbClass);
            if (mpb) Call(mpbCtor, mpb, {}, "MaterialPropertyBlock..ctor", &ok);
            if (mpb && ok) Call(rendGetBlock, tg.r, {mpb}, "Renderer.GetPropertyBlock", &ok);
            if (!ok) mpb = nullptr;
        }
        int here = 0;
        for (size_t m = 0; m < nm && m < 8; ++m) {
            auto* mat = static_cast<Il2CppObject*>(unity::ArrayAt(mats, m));
            if (!unity::IsAlive(mat)) continue;
            Il2CppObject* sh = Call(matShader, mat, {}, "Material.get_shader", &ok);
            if (!ok || !unity::IsAlive(sh)) continue;
            const int32_t count = Value<int32_t>(Call(shPropCount, sh, {}, "Shader.GetPropertyCount", &ok));
            for (int32_t k = 0; ok && k < count && k < 64; ++k) {
                if (Value<int32_t>(Call(shPropType, sh, {&k}, "Shader.GetPropertyType", &ok), -1) != kShaderPropColor) continue;
                int32_t nameId = Value<int32_t>(Call(shPropNameId, sh, {&k}, "Shader.GetPropertyNameId", &ok));
                if (!ok) break;
                Color c = Value<Color>(Call(matGetColor, mat, {&nameId}, "Material.GetColor", &ok));
                if (!ok || !std::isfinite(c.r) || !std::isfinite(c.g) || !std::isfinite(c.b)) continue;
                // HDR colours (emission > 1) keep their intensity: blend the hue, scale by the brightest channel.
                const float peak = std::max({c.r, c.g, c.b, 1.0f});
                c.r += (kTintColor.r * peak - c.r) * tg.t;
                c.g += (kTintColor.g * peak - c.g) * tg.t;
                c.b += (kTintColor.b * peak - c.b) * tg.t;
                Call(matSetColor, mat, {&nameId, &c}, "Material.SetColor", &ok);
                if (ok) ++here;
                if (ok && mpb) Call(mpbSetColor, mpb, {&nameId, &c}, "MaterialPropertyBlock.SetColor", &ok);
                ok = true;  // a failed block write only drops the block
            }
            unity::SetName(mat, kGoldMat);
            if (sr2::core::ModuleFaulted(module)) return;
        }
        if (mpb && here) {
            Call(rendSetBlock, tg.r, {mpb}, "Renderer.SetPropertyBlock", &ok);
            if (ok) ++blocks;
        }
        colours += here;
        done += std::format("{}{} ({} at {:.0f} %)", done.empty() ? "" : ", ", tg.name, here, tg.t * 100.0f);
    }
    const int n = ++tintCount[id];
    if (!loggedTint) {
        loggedTint = true;
        SR2_INFO("[{}] silo sell button on plot '{}' tinted gold ({}): {} colour(s) on {}; property block on {} "
                 "renderer(s)",
                 module, id, anyCap ? "cap + base" : "no cap found - whole button", colours,
                 done.empty() ? "nothing" : done, blocks);
    } else if (n > 1 && loggedRetint < 5) {
        ++loggedRetint;
        SR2_INFO("[{}] silo sell button on plot '{}': materials were replaced (was '{}') - tinted again ({} colour(s))",
                 module, id, was, colours);
    }
}

// ---------------------------------------------------------------- api --

void SiloButtons::Tick(const std::vector<std::string>& wanted, const std::function<Il2CppObject*()>& lookup) {
    if (!ready_ || !impl_) return;
    Impl& p = *impl_;
    if (wanted.empty() && built_.empty()) return;
    Il2CppObject* gm = p.GameModel();
    if (!gm) return;
    std::set<std::string> ids(built_);
    ids.insert(wanted.begin(), wanted.end());
    Il2CppObject* lookupDirector = nullptr;
    bool lookupTried = false;
    for (const std::string& id : ids) {
        Il2CppObject* plot = p.PlotFor(gm, id);
        if (!plot) {
            built_.erase(id);
            continue;
        }
        Il2CppObject* btn = p.Scan(plot, generation_, false);
        const bool want = p.PlotType(plot) == kPlotSilo && std::find(wanted.begin(), wanted.end(), id) != wanted.end();
        if (want && !btn) {
            if (!lookupTried) {
                lookupTried = true;
                lookupDirector = lookup();
            }
            p.Create(plot, lookupDirector, id, generation_);
        } else if (want && btn) {
            p.Tint(btn, id);  // from a tick after the build on (ApplyPalette.Start has run); no-op while gold
        } else if (!want && btn) {
            p.tintCount.erase(id);
            p.Destroy(btn);
            SR2_INFO("[{}] silo sell button removed from plot '{}' (not owned)", p.module, id);
        }
        if (want) built_.insert(id);
        else built_.erase(id);
        if (sr2::core::ModuleFaulted(p.module)) return;
    }
}

void SiloButtons::RemoveAll() {
    if (!ready_ || !impl_) return;
    sr2::ui::hud::Clear(kHudOwner);
    impl_->msgActive = false;
    Il2CppObject* gm = built_.empty() ? nullptr : impl_->GameModel();
    if (gm)
        for (const std::string& id : built_)
            if (Il2CppObject* plot = impl_->PlotFor(gm, id)) impl_->Scan(plot, generation_, true);
    built_.clear();
    impl_->tintCount.clear();
}

void SiloButtons::Remove(Il2CppObject* plot) {
    if (!ready_ || !impl_ || !unity::IsAlive(plot) || !impl_->IsA(plot, impl_->plotClass)) return;
    if (Il2CppObject* btn = impl_->Scan(plot, generation_, false)) {
        impl_->Destroy(btn);
        SR2_INFO("[{}] silo sell button removed (sold)", impl_->module);
    }
}

void SiloButtons::Reset() {
    ++generation_;  // built_ is kept: those plots are visited once more and stale buttons destroyed
    if (impl_) {
        impl_->msgActive = false;
        sr2::ui::hud::Clear(kHudOwner);
    }
}

bool SiloButtons::IsOurs(Il2CppObject* activator) {
    if (!ready_ || !impl_ || !unity::IsAlive(activator) || !impl_->IsA(activator, impl_->activatorClass)) return false;
    if (unity::Field<Il2CppObject*>(activator, impl_->offActCollector) != nullptr) return false;  // vanilla
    Il2CppObject* tr = unity::TransformOf(activator);
    for (int depth = 0; tr && depth < 4; ++depth, tr = impl_->Parent(tr)) {
        Il2CppObject* go = unity::GameObjectOf(tr);
        if (go && unity::GetName(go).rfind(kName, 0) == 0) return true;
    }
    return false;
}

Il2CppObject* SiloButtons::OwnerPlot(Il2CppObject* activator) {
    if (!ready_ || !impl_) return nullptr;
    Il2CppObject* plot = impl_->InParent(activator, impl_->plotClass);
    return plot && impl_->PlotType(plot) == kPlotSilo ? plot : nullptr;
}

bool SiloButtons::Press(Il2CppObject* activator) {
    if (!ready_ || !impl_) return false;
    Impl& p = *impl_;
    const unsigned long long now = GetTickCount64();
    if (p.lastPress && now - p.lastPress < kPressGapMs) return false;
    p.lastPress = now;
    Il2CppObject* anim = unity::Field<Il2CppObject*>(activator, p.offActAnim);
    int32_t trigger = unity::Field<int32_t>(activator, p.offActTrigger);
    if (unity::IsAlive(anim)) p.Call(p.animSetTrigger, anim, {&trigger}, "Animator.SetTrigger");
    Il2CppObject* cue = unity::Field<Il2CppObject*>(activator, p.offActCue);
    Il2CppObject* tr = unity::TransformOf(activator);
    if (p.audioPlay && cue && unity::IsAlive(tr)) {
        bool ok = false;
        Il2CppObject* b = p.Call(p.trGetPosition, tr, {}, "Transform.get_position", &ok);
        Vec3 pos = ok && b ? p.Value<Vec3>(b) : Vec3{0, 0, 0};
        bool loop = false;
        if (ok) p.Call(p.audioPlay, nullptr, {cue, &pos, &loop}, "SECTR_AudioSystem.Play");
    }
    return true;
}

void SiloButtons::Message(Il2CppObject* activator, const std::string& text, bool good, float seconds) {
    if (!ready_ || !impl_) return;
    Impl& p = *impl_;
    Il2CppObject* tr = unity::TransformOf(activator);
    if (!unity::IsAlive(tr)) return;
    bool ok = false;
    Il2CppObject* b = p.Call(p.trGetPosition, tr, {}, "Transform.get_position", &ok);
    if (!ok || !b) return;
    p.msgPos = p.Value<Vec3>(b);
    p.msgPos.y += 0.9f;
    p.msgText = text;
    p.msgColor = good ? 0xFF9BF58Cu : 0xFF5AB4FFu;  // IM_COL32 order (0xAABBGGRR): green / orange
    p.msgUntil = GetTickCount64() + static_cast<unsigned long long>(std::clamp(seconds, 0.5f, 10.0f) * 1000.0f);
    p.msgActive = true;
}

void SiloButtons::Frame() {
    if (!ready_ || !impl_ || !impl_->msgActive) return;
    Impl& p = *impl_;
    if (GetTickCount64() > p.msgUntil) {
        p.msgActive = false;
        sr2::ui::hud::Clear(kHudOwner);
        return;
    }
    Il2CppObject* cam = unity::Camera_main();
    if (!unity::IsAlive(cam) || !p.IsA(cam, p.cameraClass)) return;  // stale frame expires by itself
    const Mat4 v = p.worldToCamera(cam, p.worldToCameraMi);
    const Mat4 pr = p.projection(cam, p.projectionMi);
    sr2::ui::hud::Frame f;
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r) {
            float s = 0;
            for (int k = 0; k < 4; ++k) s += pr.m[k * 4 + r] * v.m[c * 4 + k];
            f.vp[c * 4 + r] = s;
        }
    for (float x : f.vp)
        if (!std::isfinite(x)) return;
    const float tx = v.m[12], ty = v.m[13], tz = v.m[14];
    const float cx = -(v.m[0] * tx + v.m[1] * ty + v.m[2] * tz);
    const float cy = -(v.m[4] * tx + v.m[5] * ty + v.m[6] * tz);
    const float cz = -(v.m[8] * tx + v.m[9] * ty + v.m[10] * tz);
    const float dx = p.msgPos.x - cx, dy = p.msgPos.y - cy, dz = p.msgPos.z - cz;
    const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (!std::isfinite(d) || d > kMessageRange) {
        sr2::ui::hud::Clear(kHudOwner);
        return;
    }
    f.markers.push_back({p.msgPos.x, p.msgPos.y, p.msgPos.z, d});
    f.range = kMessageRange;
    f.showDistance = false;
    f.textOnly = true;
    f.tag = p.msgText;
    f.color = p.msgColor;
    sr2::ui::hud::Publish(kHudOwner, std::move(f));
}

}  // namespace sr2::modules::custom_upgrades::world
