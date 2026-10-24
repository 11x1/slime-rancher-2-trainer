// sandbox: icons rendered from prefab meshes (see prefab_icons.h). SR2 1.3.0.

#include "modules/sandbox/prefab_icons.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <format>
#include <initializer_list>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/detour_guard.h"
#include "core/il2cpp_api.h"
#include "core/log.h"
#include "core/module.h"
#include "core/resolver.h"
#include "core/unity.h"

namespace sr2::modules::sandbox {
namespace {

namespace unity = sr2::core::unity;
using sr2::il2cpp::api;
using sr2::il2cpp::Resolver;

constexpr const char* kCore = "UnityEngine.CoreModule";
constexpr int kSize = 128;          // icon texture edge
constexpr int kLayer = 31;          // layer the copies are drawn on (camera and light cull everything else)
constexpr size_t kMaxParts = 64;    // renderers copied per prefab
constexpr float kOriginY = 10000;   // where the copies are built (far above any scene)
constexpr int kHideDontUnload = 32; // HideFlags.DontUnloadUnusedAsset

struct V3 {
    float x, y, z;
};
struct Quat {
    float x, y, z, w;
};
struct Color {
    float r, g, b, a;
};
struct Rect {
    float x, y, w, h;
};
struct V2 {
    float x, y;
};
struct Bounds {
    V3 center, extents;
};
struct Color32 {
    uint8_t r, g, b, a;
};

V3 Sub(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
V3 Add(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
V3 Mul(V3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
V3 Cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
float Len(V3 a) { return std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z); }
V3 Norm(V3 a) {
    const float l = Len(a);
    return l > 1e-6f ? Mul(a, 1.0f / l) : V3{0, 0, 1};
}
Quat Conj(Quat q) { return {-q.x, -q.y, -q.z, q.w}; }
Quat QMul(Quat a, Quat b) {
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}
V3 Rotate(Quat q, V3 v) {
    const Quat p{v.x, v.y, v.z, 0};
    const Quat r = QMul(QMul(q, p), Conj(q));
    return {r.x, r.y, r.z};
}
// Quaternion.LookRotation(forward, up = Vector3.up).
Quat LookRotation(V3 f) {
    f = Norm(f);
    V3 r = Cross(V3{0, 1, 0}, f);
    if (Len(r) < 1e-4f) r = V3{1, 0, 0};
    r = Norm(r);
    const V3 u = Cross(f, r);
    // Rotation matrix columns r, u, f -> quaternion.
    const float m00 = r.x, m01 = u.x, m02 = f.x, m10 = r.y, m11 = u.y, m12 = f.y, m20 = r.z, m21 = u.z, m22 = f.z;
    Quat q{};
    const float tr = m00 + m11 + m22;
    if (tr > 0) {
        const float s = std::sqrt(tr + 1.0f) * 2;
        q = {(m21 - m12) / s, (m02 - m20) / s, (m10 - m01) / s, 0.25f * s};
    } else if (m00 > m11 && m00 > m22) {
        const float s = std::sqrt(1.0f + m00 - m11 - m22) * 2;
        q = {0.25f * s, (m01 + m10) / s, (m02 + m20) / s, (m21 - m12) / s};
    } else if (m11 > m22) {
        const float s = std::sqrt(1.0f + m11 - m00 - m22) * 2;
        q = {(m01 + m10) / s, 0.25f * s, (m12 + m21) / s, (m02 - m20) / s};
    } else {
        const float s = std::sqrt(1.0f + m22 - m00 - m11) * 2;
        q = {(m02 + m20) / s, (m12 + m21) / s, 0.25f * s, (m10 - m01) / s};
    }
    return q;
}
Quat AxisAngle(V3 axis, float deg) {
    const float h = deg * 3.14159265f / 360.0f;
    return {axis.x * std::sin(h), axis.y * std::sin(h), axis.z * std::sin(h), std::cos(h)};
}

std::mutex g_modIconsMu;
std::unordered_map<std::string, uint32_t> g_modIcons;

}  // namespace

uint32_t ModIcon(const std::string& id) {
    std::lock_guard<std::mutex> lk(g_modIconsMu);
    const auto it = g_modIcons.find(id);
    return it != g_modIcons.end() ? it->second : 0;
}

void SetModIcon(const std::string& id, uint32_t icon) {
    std::lock_guard<std::mutex> lk(g_modIconsMu);
    g_modIcons[id] = icon;
}

struct PrefabIcons::Impl {
    const char* module = "sandbox";
    Il2CppClass *goClass = nullptr, *objectClass = nullptr, *meshRendererClass = nullptr, *skinnedClass = nullptr,
                *meshFilterClass = nullptr, *cameraClass = nullptr, *lightClass = nullptr, *rtClass = nullptr,
                *tex2dClass = nullptr;
    const MethodInfo *goCtor = nullptr, *addComponent = nullptr, *getInChildren = nullptr, *activeSelf = nullptr,
                     *setLayer = nullptr, *destroyImmediate = nullptr, *setHideFlags = nullptr;
    const MethodInfo *trGetPos = nullptr, *trGetRot = nullptr, *trGetScale = nullptr, *trParent = nullptr,
                     *trSetPos = nullptr, *trSetRot = nullptr, *trSetScale = nullptr, *trFind = nullptr;
    const MethodInfo *rendEnabled = nullptr, *rendBounds = nullptr, *rendGetMats = nullptr, *rendSetMats = nullptr,
                     *mfGetMesh = nullptr, *mfSetMesh = nullptr, *smrGetMesh = nullptr;
    const MethodInfo *camEnabled = nullptr, *camClear = nullptr, *camBg = nullptr, *camMask = nullptr, *camOrtho = nullptr,
                     *camOrthoSize = nullptr, *camNear = nullptr, *camFar = nullptr, *camHdr = nullptr, *camMsaa = nullptr,
                     *camTarget = nullptr, *camRender = nullptr;
    const MethodInfo *lightType = nullptr, *lightIntensity = nullptr, *lightMask = nullptr, *lightColor = nullptr;
    const MethodInfo *rtCtor = nullptr, *rtCreate = nullptr, *rtRelease = nullptr, *rtGetActive = nullptr,
                     *rtSetActive = nullptr;
    const MethodInfo *texCtor = nullptr, *texRead = nullptr, *texGetPixels = nullptr, *texSetPixels = nullptr,
                     *texApply = nullptr, *spriteCreate = nullptr;
    int failures = 0;

    Il2CppObject* Call(const MethodInfo* m, void* obj, std::initializer_list<void*> args, const char* what, bool* ok) {
        if (ok) *ok = false;
        if (!m || sr2::core::ModuleFaulted(module)) return nullptr;
        std::vector<void*> params(args);
        std::string err;
        Il2CppObject* r = Resolver::Get().Invoke(m, obj, params.empty() ? nullptr : params.data(), &err);
        if (!err.empty()) {
            if (err.rfind("native exception", 0) == 0) sr2::core::MarkModuleFaulted(module, std::format("{}: {}", what, err));
            else SR2_WARN("[{}] icon render: {} failed: {}", module, what, err);
            return nullptr;
        }
        if (ok) *ok = true;
        return r;
    }
    bool Do(const MethodInfo* m, void* obj, std::initializer_list<void*> args, const char* what) {
        bool ok = false;
        Call(m, obj, args, what, &ok);
        return ok;
    }
    template <class T>
    bool Get(const MethodInfo* m, void* obj, const char* what, T& out) {
        bool ok = false;
        Il2CppObject* r = Call(m, obj, {}, what, &ok);
        if (!ok || !r) return false;
        out = *reinterpret_cast<T*>(reinterpret_cast<char*>(r) + 2 * sizeof(void*));
        return true;
    }
    using ObjectNew = Il2CppObject* (*)(Il2CppClass*);
    ObjectNew objectNew = nullptr;  // il2cpp_object_new (not in core's api table)
    Il2CppObject* New(Il2CppClass* k) const { return objectNew && k ? objectNew(k) : nullptr; }
    Il2CppObject* NewGameObject(const char* name) {
        Il2CppObject* go = New(goClass);
        if (!go) return nullptr;
        Il2CppString* s = unity::NewString(name);
        if (!Do(goCtor, go, {s}, "GameObject..ctor") || !unity::IsAlive(go)) return nullptr;
        return go;
    }
    Il2CppObject* AddComponent(Il2CppObject* go, Il2CppClass* k, const char* what) {
        bool ok = false;
        Il2CppObject* c = Call(addComponent, go, {unity::TypeOf(k)}, what, &ok);
        return ok && c && unity::IsAlive(c) ? c : nullptr;
    }
    void SetPose(Il2CppObject* go, V3 pos, Quat rot, const V3* scale) {
        Il2CppObject* tr = unity::TransformOf(go);
        if (!tr) return;
        Do(trSetPos, tr, {&pos}, "Transform.set_position");
        Do(trSetRot, tr, {&rot}, "Transform.set_rotation");
        if (scale) Do(trSetScale, tr, {const_cast<V3*>(scale)}, "Transform.set_localScale");
    }
    // The GameObject and every parent up to the prefab root are activeSelf.
    bool ActiveChain(Il2CppObject* go) {
        Il2CppObject* tr = unity::TransformOf(go);
        for (int depth = 0; tr && depth < 32; ++depth) {
            bool on = false;
            if (!Get(activeSelf, unity::GameObjectOf(tr), "GameObject.activeSelf", on) || !on) return false;
            bool ok = false;
            tr = Call(trParent, tr, {}, "Transform.parent", &ok);
            if (!ok) return false;
        }
        return true;
    }
    // `go` or a parent of it below `stop` (the piece's root) has one of the `skip` names.
    bool Skipped(Il2CppObject* go, Il2CppObject* stop, const std::vector<std::string>& skip) {
        if (skip.empty()) return false;
        Il2CppObject* tr = unity::TransformOf(go);
        Il2CppObject* stopTr = unity::TransformOf(stop);
        for (int depth = 0; tr && tr != stopTr && depth < 32; ++depth) {
            const std::string name = unity::GetName(unity::GameObjectOf(tr));
            for (const std::string& s : skip)
                if (name == s) return true;
            bool ok = false;
            tr = Call(trParent, tr, {}, "Transform.parent", &ok);
            if (!ok) return false;
        }
        return false;
    }
    static bool SkipLod(const std::string& name) {
        const size_t p = name.find("LOD");
        return p != std::string::npos && p + 3 < name.size() && name[p + 3] >= '1' && name[p + 3] <= '9';
    }
};

PrefabIcons::PrefabIcons() = default;
PrefabIcons::~PrefabIcons() { delete impl_; }

bool PrefabIcons::Setup(Resolver& R, const char* module, std::string& why) {
    auto* p = new Impl();
    p->module = module;
    std::string err;
    auto cls = [&](const char* name) { return err.empty() ? R.Class(kCore, "UnityEngine", name, &err) : nullptr; };
    p->goClass = cls("GameObject");
    p->objectClass = cls("Object");
    Il2CppClass* transform = cls("Transform");
    Il2CppClass* renderer = cls("Renderer");
    p->meshRendererClass = cls("MeshRenderer");
    p->skinnedClass = cls("SkinnedMeshRenderer");
    p->meshFilterClass = cls("MeshFilter");
    p->cameraClass = cls("Camera");
    p->lightClass = cls("Light");
    p->rtClass = cls("RenderTexture");
    p->tex2dClass = cls("Texture2D");
    Il2CppClass* sprite = cls("Sprite");
    Il2CppClass* behaviour = cls("Behaviour");
    if (!err.empty()) {
        why = err;
        delete p;
        return false;
    }
    auto m = [&](Il2CppClass* k, const char* name, int argc) -> const MethodInfo* {
        if (!err.empty()) return nullptr;
        const MethodInfo* r = nullptr;
        for (Il2CppClass* c = k; c && !r; c = api().il2cpp_class_get_parent ? api().il2cpp_class_get_parent(c) : nullptr)
            r = R.Method(c, name, argc);
        if (!r) err = std::format("method {}({}) not found", name, argc);
        return r;
    };
    auto mp = [&](Il2CppClass* k, const char* name, std::initializer_list<std::string_view> params) -> const MethodInfo* {
        if (!err.empty()) return nullptr;
        const MethodInfo* r = R.MethodByParams(k, name, params, &err);
        if (!r && err.empty()) err = std::format("method {} not found", name);
        return r;
    };
    p->goCtor = mp(p->goClass, ".ctor", {"System.String"});
    p->addComponent = mp(p->goClass, "AddComponent", {"System.Type"});
    p->getInChildren = mp(p->goClass, "GetComponentsInChildren", {"System.Type", "System.Boolean"});
    p->activeSelf = m(p->goClass, "get_activeSelf", 0);
    p->setLayer = m(p->goClass, "set_layer", 1);
    p->destroyImmediate = mp(p->objectClass, "DestroyImmediate", {"UnityEngine.Object"});
    p->setHideFlags = m(p->objectClass, "set_hideFlags", 1);
    p->trGetPos = m(transform, "get_position", 0);
    p->trGetRot = m(transform, "get_rotation", 0);
    p->trGetScale = m(transform, "get_lossyScale", 0);
    p->trParent = m(transform, "get_parent", 0);
    p->trSetPos = m(transform, "set_position", 1);
    p->trSetRot = m(transform, "set_rotation", 1);
    p->trSetScale = m(transform, "set_localScale", 1);
    p->trFind = mp(transform, "Find", {"System.String"});
    p->rendEnabled = m(renderer, "get_enabled", 0);
    p->rendBounds = m(renderer, "get_bounds", 0);
    p->rendGetMats = m(renderer, "get_sharedMaterials", 0);
    p->rendSetMats = m(renderer, "set_sharedMaterials", 1);
    p->mfGetMesh = m(p->meshFilterClass, "get_sharedMesh", 0);
    p->mfSetMesh = m(p->meshFilterClass, "set_sharedMesh", 1);
    p->smrGetMesh = m(p->skinnedClass, "get_sharedMesh", 0);
    p->camEnabled = m(behaviour, "set_enabled", 1);
    p->camClear = m(p->cameraClass, "set_clearFlags", 1);
    p->camBg = m(p->cameraClass, "set_backgroundColor", 1);
    p->camMask = m(p->cameraClass, "set_cullingMask", 1);
    p->camOrtho = m(p->cameraClass, "set_orthographic", 1);
    p->camOrthoSize = m(p->cameraClass, "set_orthographicSize", 1);
    p->camNear = m(p->cameraClass, "set_nearClipPlane", 1);
    p->camFar = m(p->cameraClass, "set_farClipPlane", 1);
    p->camHdr = m(p->cameraClass, "set_allowHDR", 1);
    p->camMsaa = m(p->cameraClass, "set_allowMSAA", 1);
    p->camTarget = m(p->cameraClass, "set_targetTexture", 1);
    p->camRender = m(p->cameraClass, "Render", 0);
    p->lightType = m(p->lightClass, "set_type", 1);
    p->lightIntensity = m(p->lightClass, "set_intensity", 1);
    p->lightMask = m(p->lightClass, "set_cullingMask", 1);
    p->lightColor = m(p->lightClass, "set_color", 1);
    p->rtCtor = mp(p->rtClass, ".ctor", {"System.Int32", "System.Int32", "System.Int32"});
    p->rtCreate = m(p->rtClass, "Create", 0);
    p->rtRelease = m(p->rtClass, "Release", 0);
    p->rtGetActive = m(p->rtClass, "get_active", 0);
    p->rtSetActive = m(p->rtClass, "set_active", 1);
    p->texCtor = mp(p->tex2dClass, ".ctor", {"System.Int32", "System.Int32", "UnityEngine.TextureFormat", "System.Boolean"});
    p->texRead = mp(p->tex2dClass, "ReadPixels", {"UnityEngine.Rect", "System.Int32", "System.Int32"});
    p->texGetPixels = m(p->tex2dClass, "GetPixels32", 0);
    p->texSetPixels = mp(p->tex2dClass, "SetPixels32", {"UnityEngine.Color32[]"});
    p->texApply = mp(p->tex2dClass, "Apply", {"System.Boolean", "System.Boolean"});
    p->spriteCreate = mp(sprite, "Create", {"UnityEngine.Texture2D", "UnityEngine.Rect", "UnityEngine.Vector2", "System.Single"});
    if (api().module)
        p->objectNew = reinterpret_cast<Impl::ObjectNew>(GetProcAddress(api().module, "il2cpp_object_new"));
    if (!err.empty() || !p->objectNew) {
        why = err.empty() ? "il2cpp_object_new not exported" : err;
        delete p;
        return false;
    }
    delete impl_;
    impl_ = p;
    ready_ = true;
    return true;
}

Il2CppObject* PrefabIcons::Render(Il2CppObject* prefab, std::string& why) {
    Piece p;
    p.prefab = prefab;
    return RenderPieces({p}, nullptr, why);
}

Il2CppObject* PrefabIcons::RenderPieces(const std::vector<Piece>& pieces, const float* tint, std::string& why, int* partsOut) {
    if (partsOut) *partsOut = 0;
    if (!ready_) {
        why = "renderer unavailable";
        return nullptr;
    }
    Impl& I = *impl_;
    if (I.failures >= 6) {
        why = "stopped after repeated failures";
        return nullptr;
    }
    // ---- what to copy: enabled mesh renderers on active objects, posed in the composition ----
    struct Part {
        Il2CppObject* mesh;
        Il2CppObject* mats;  // Material[]
        V3 pos, scale;
        Quat rot;
    };
    std::vector<Part> parts;
    // A later piece that can't be used is left out (logged); the first one is the look itself.
    auto leaveOut = [&](const Piece& pc) {
        if (&pc == &pieces.front()) return false;
        SR2_INFO("[{}] icon render: a piece left out ({})", I.module, why);
        return true;
    };
    for (const Piece& pc : pieces) {
        Il2CppObject* prefab = pc.prefab;
        if (!prefab || !unity::IsAlive(prefab) || unity::ClassOf(prefab) != I.goClass) {
            why = "no prefab";
            if (!leaveOut(pc)) return nullptr;
            continue;
        }
        V3 rootPos{};
        Quat rootRot{0, 0, 0, 1};
        Il2CppObject* rootTr = unity::TransformOf(prefab);
        if (!rootTr || !I.Get(I.trGetPos, rootTr, "Transform.position", rootPos) || !I.Get(I.trGetRot, rootTr, "Transform.rotation", rootRot)) {
            why = "prefab transform unreadable";
            if (!leaveOut(pc)) return nullptr;
            continue;
        }
        // The piece's root: the prefab, or its `sub` child.
        Il2CppObject* top = prefab;
        V3 subPos = rootPos;
        Quat subRot = rootRot;
        if (pc.sub) {
            bool ok = false;
            auto* subTr = I.Call(I.trFind, rootTr, {unity::NewString(pc.sub)}, "Transform.Find", &ok);
            if (!ok || !subTr || !unity::IsAlive(subTr) || !I.Get(I.trGetPos, subTr, "Transform.position", subPos) ||
                !I.Get(I.trGetRot, subTr, "Transform.rotation", subRot)) {
                why = std::format("'{}' has no '{}'", unity::GetName(prefab), pc.sub);
                if (!leaveOut(pc)) return nullptr;
                continue;
            }
            top = unity::GameObjectOf(subTr);
        }
        // Where the piece's root goes: its place in the prefab (keepPlace) or the origin, then pos / yaw / tilt
        // (yaw * place * lean-back, as the dressing sets localRotation * AngleAxis(-tilt, x)).
        const Quat invRoot = Conj(rootRot), invSub = Conj(subRot);
        const bool keep = pc.sub && pc.keepPlace;
        const V3 anchor = keep ? Rotate(invRoot, Sub(subPos, rootPos)) : V3{0, 0, 0};
        const Quat anchorRot = keep ? QMul(invRoot, subRot) : Quat{0, 0, 0, 1};
        const Quat pieceRot = QMul(QMul(AxisAngle({0, 1, 0}, pc.yaw), anchorRot), AxisAngle({1, 0, 0}, -pc.tilt));
        const V3 at = Add(anchor, V3{pc.pos[0], pc.pos[1], pc.pos[2]});
        const float sc = pc.scale > 0 ? pc.scale : 1.0f;
        for (Il2CppClass* k : {I.meshRendererClass, I.skinnedClass}) {
            bool include = true, ok = false;
            void* arr = I.Call(I.getInChildren, top, {unity::TypeOf(k), &include}, "GameObject.GetComponentsInChildren", &ok);
            for (size_t i = 0, n = ok ? unity::ArrayLength(arr) : 0; i < n && parts.size() < kMaxParts; ++i) {
                auto* r = static_cast<Il2CppObject*>(unity::ArrayAt(arr, i));
                if (!r || !unity::IsAlive(r)) continue;
                bool enabled = false;
                if (!I.Get(I.rendEnabled, r, "Renderer.enabled", enabled) || !enabled) continue;
                Il2CppObject* go = unity::GameObjectOf(r);
                if (!go || Impl::SkipLod(unity::GetName(go)) || !I.ActiveChain(go) || I.Skipped(go, top, pc.skip)) continue;
                Il2CppObject* mesh = nullptr;
                if (k == I.skinnedClass) mesh = I.Call(I.smrGetMesh, r, {}, "SkinnedMeshRenderer.sharedMesh", &ok);
                else if (Il2CppObject* mf = unity::GetComponent(go, I.meshFilterClass))
                    mesh = I.Call(I.mfGetMesh, mf, {}, "MeshFilter.sharedMesh", &ok);
                if (!mesh || !unity::IsAlive(mesh)) continue;
                Il2CppObject* mats = I.Call(I.rendGetMats, r, {}, "Renderer.sharedMaterials", &ok);
                if (!mats) continue;
                Part p{mesh, mats, {}, {1, 1, 1}, {0, 0, 0, 1}};
                Il2CppObject* tr = unity::TransformOf(go);
                if (!I.Get(I.trGetPos, tr, "Transform.position", p.pos) || !I.Get(I.trGetRot, tr, "Transform.rotation", p.rot) ||
                    !I.Get(I.trGetScale, tr, "Transform.lossyScale", p.scale))
                    continue;
                // Into the composition: relative to the piece's root, scaled, turned, moved.
                p.pos = Add(at, Rotate(pieceRot, Mul(Rotate(invSub, Sub(p.pos, subPos)), sc)));
                p.rot = QMul(pieceRot, QMul(invSub, p.rot));
                p.scale = Mul(p.scale, sc);
                parts.push_back(p);
            }
        }
    }
    if (partsOut) *partsOut = static_cast<int>(parts.size());
    if (parts.empty()) {
        why = "the prefab has no visible mesh";
        return nullptr;
    }

    // ---- build the copies, the camera and the light; everything is destroyed below ----
    std::vector<Il2CppObject*> made;
    auto cleanup = [&] {
        for (auto it = made.rbegin(); it != made.rend(); ++it)
            if (unity::IsAlive(*it)) I.Do(I.destroyImmediate, nullptr, {*it}, "Object.DestroyImmediate");
        made.clear();
    };
    const V3 origin{0, kOriginY, 0};
    int layer = kLayer;
    bool haveBounds = false;
    V3 lo{}, hi{};
    for (const Part& p : parts) {
        Il2CppObject* go = I.NewGameObject("sr2mods_icon_part");
        if (!go) continue;
        made.push_back(go);
        I.Do(I.setLayer, go, {&layer}, "GameObject.set_layer");
        I.SetPose(go, Add(origin, p.pos), p.rot, &p.scale);
        Il2CppObject* mf = I.AddComponent(go, I.meshFilterClass, "AddComponent(MeshFilter)");
        Il2CppObject* mr = I.AddComponent(go, I.meshRendererClass, "AddComponent(MeshRenderer)");
        if (!mf || !mr || !I.Do(I.mfSetMesh, mf, {p.mesh}, "MeshFilter.set_sharedMesh") ||
            !I.Do(I.rendSetMats, mr, {p.mats}, "Renderer.set_sharedMaterials"))
            continue;
        Bounds b{};
        if (!I.Get(I.rendBounds, mr, "Renderer.bounds", b)) continue;
        const V3 a = Sub(b.center, b.extents), c = Add(b.center, b.extents);
        if (!std::isfinite(a.x + a.y + a.z + c.x + c.y + c.z)) continue;
        if (!haveBounds) lo = a, hi = c, haveBounds = true;
        else {
            lo = {std::min(lo.x, a.x), std::min(lo.y, a.y), std::min(lo.z, a.z)};
            hi = {std::max(hi.x, c.x), std::max(hi.y, c.y), std::max(hi.z, c.z)};
        }
    }
    const V3 center = Mul(Add(lo, hi), 0.5f);
    const float radius = Len(Mul(Sub(hi, lo), 0.5f));
    if (!haveBounds || !(radius > 1e-3f) || radius > 500.0f) {
        cleanup();
        why = "no usable bounds";
        ++I.failures;
        return nullptr;
    }

    // Front right, a little above (prefabs face +Z).
    const V3 dir = Norm(V3{0.55f, 0.42f, 1.0f});
    Il2CppObject* camGo = I.NewGameObject("sr2mods_icon_camera");
    Il2CppObject* cam = camGo ? I.AddComponent(camGo, I.cameraClass, "AddComponent(Camera)") : nullptr;
    if (camGo) made.push_back(camGo);
    Il2CppObject* lightGo = I.NewGameObject("sr2mods_icon_light");
    Il2CppObject* light = lightGo ? I.AddComponent(lightGo, I.lightClass, "AddComponent(Light)") : nullptr;
    if (lightGo) made.push_back(lightGo);
    Il2CppObject* rt = I.New(I.rtClass);
    if (!cam || !light || !rt) {
        cleanup();
        why = "could not create the camera / light / render texture";
        ++I.failures;
        return nullptr;
    }
    int sz = kSize, depth = 24;
    if (!I.Do(I.rtCtor, rt, {&sz, &sz, &depth}, "RenderTexture..ctor")) {
        cleanup();
        why = "RenderTexture..ctor failed";
        ++I.failures;
        return nullptr;
    }
    made.insert(made.begin(), rt);  // destroyed last (after the camera lets go of it)
    bool created = false;
    I.Get(I.rtCreate, rt, "RenderTexture.Create", created);

    bool no = false, yes = true;
    int clear = 2;  // CameraClearFlags.SolidColor
    Color key{1, 0, 1, 0};
    int mask = 1 << kLayer;
    float size = radius * 1.04f, nearP = 0.05f, farP = radius * 8.0f + 1.0f;
    I.Do(I.camEnabled, cam, {&no}, "Camera.set_enabled");
    I.Do(I.camClear, cam, {&clear}, "Camera.set_clearFlags");
    I.Do(I.camBg, cam, {&key}, "Camera.set_backgroundColor");
    I.Do(I.camMask, cam, {&mask}, "Camera.set_cullingMask");
    I.Do(I.camOrtho, cam, {&yes}, "Camera.set_orthographic");
    I.Do(I.camOrthoSize, cam, {&size}, "Camera.set_orthographicSize");
    I.Do(I.camNear, cam, {&nearP}, "Camera.set_nearClipPlane");
    I.Do(I.camFar, cam, {&farP}, "Camera.set_farClipPlane");
    I.Do(I.camHdr, cam, {&no}, "Camera.set_allowHDR");
    I.Do(I.camMsaa, cam, {&no}, "Camera.set_allowMSAA");
    I.SetPose(camGo, Add(center, Mul(dir, radius * 4.0f)), LookRotation(Mul(dir, -1.0f)), nullptr);
    int directional = 0;
    float intensity = 1.15f;
    Color white{1, 0.97f, 0.92f, 1};
    I.Do(I.lightType, light, {&directional}, "Light.set_type");
    I.Do(I.lightIntensity, light, {&intensity}, "Light.set_intensity");
    I.Do(I.lightMask, light, {&mask}, "Light.set_cullingMask");
    I.Do(I.lightColor, light, {&white}, "Light.set_color");
    I.SetPose(lightGo, center, LookRotation(V3{-0.35f, -0.75f, -0.55f}), nullptr);

    bool rendered = I.Do(I.camTarget, cam, {rt}, "Camera.set_targetTexture") && I.Do(I.camRender, cam, {}, "Camera.Render");

    // ---- read back, make the background transparent ----
    Il2CppObject* tex = rendered ? I.New(I.tex2dClass) : nullptr;
    int fmt = 4;  // TextureFormat.RGBA32
    bool ok = tex && I.Do(I.texCtor, tex, {&sz, &sz, &fmt, &no}, "Texture2D..ctor");
    if (ok) {
        bool got = false;
        Il2CppObject* prev = I.Call(I.rtGetActive, nullptr, {}, "RenderTexture.active", &got);
        Rect r{0, 0, static_cast<float>(kSize), static_cast<float>(kSize)};
        int zero = 0;
        ok = I.Do(I.rtSetActive, nullptr, {rt}, "RenderTexture.set_active") &&
             I.Do(I.texRead, tex, {&r, &zero, &zero}, "Texture2D.ReadPixels");
        I.Do(I.rtSetActive, nullptr, {got ? prev : nullptr}, "RenderTexture.set_active");
    }
    Il2CppObject* nullRt = nullptr;
    I.Do(I.camTarget, cam, {nullRt}, "Camera.set_targetTexture");
    I.Do(I.rtRelease, rt, {}, "RenderTexture.Release");
    cleanup();
    if (!ok) {
        why = rendered ? "read back failed" : "Camera.Render failed";
        ++I.failures;
        return nullptr;
    }
    bool got = false;
    void* px = I.Call(I.texGetPixels, tex, {}, "Texture2D.GetPixels32", &got);
    const size_t n = got ? unity::ArrayLength(px) : 0;
    if (n != static_cast<size_t>(kSize * kSize)) {
        why = "no pixels";
        ++I.failures;
        return nullptr;
    }
    Color32* c = unity::ArrayData<Color32>(px);
    size_t clearAlpha = 0, opaque = 0;
    for (size_t i = 0; i < n; ++i) {
        clearAlpha += c[i].a < 8 ? 1 : 0;
        opaque += c[i].a > 247 ? 1 : 0;
    }
    // The pipeline kept the clear alpha if part of the image is transparent; else key out the magenta.
    const bool alphaKept = clearAlpha > n / 20 && opaque > 0;
    size_t shown = 0;
    for (size_t i = 0; i < n; ++i) {
        Color32& p = c[i];
        const bool bg = alphaKept ? p.a < 8 : (p.r > 235 && p.g < 24 && p.b > 235);
        if (bg) p = Color32{0, 0, 0, 0};
        else {
            if (!alphaKept) p.a = 255;
            if (tint) {
                // Towards the tint colour, keeping the shading (brightness).
                const float l = (0.30f * p.r + 0.59f * p.g + 0.11f * p.b) / 255.0f;
                const float v = std::min(1.0f, l * 1.5f + 0.15f) * 255.0f;
                auto mix = [v](uint8_t c, float t) {
                    return static_cast<uint8_t>(std::clamp(c + (t * v - c) * 0.65f, 0.0f, 255.0f));
                };
                p.r = mix(p.r, tint[0]), p.g = mix(p.g, tint[1]), p.b = mix(p.b, tint[2]);
            }
            ++shown;
        }
    }
    if (shown < 16 || shown == n) {
        why = shown ? "nothing but background was drawn over" : "nothing was drawn";
        ++I.failures;
        return nullptr;
    }
    bool updateMips = false, noLongerReadable = true;
    if (!I.Do(I.texSetPixels, tex, {px}, "Texture2D.SetPixels32") ||
        !I.Do(I.texApply, tex, {&updateMips, &noLongerReadable}, "Texture2D.Apply")) {
        why = "upload failed";
        ++I.failures;
        return nullptr;
    }
    Rect rect{0, 0, static_cast<float>(kSize), static_cast<float>(kSize)};
    V2 pivot{0.5f, 0.5f};
    float ppu = 100.0f;
    Il2CppObject* spr = I.Call(I.spriteCreate, nullptr, {tex, &rect, &pivot, &ppu}, "Sprite.Create", &got);
    if (!got || !spr) {
        why = "Sprite.Create failed";
        ++I.failures;
        return nullptr;
    }
    int hide = kHideDontUnload;
    I.Do(I.setHideFlags, tex, {&hide}, "Object.set_hideFlags");
    I.Do(I.setHideFlags, spr, {&hide}, "Object.set_hideFlags");
    I.failures = 0;
    return spr;
}

}  // namespace sr2::modules::sandbox
