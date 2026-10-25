// debug_tools: entity hitbox view (see hitboxes.h).

#include "modules/debug_tools/hitboxes.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <initializer_list>
#include <vector>

#include "core/detour_guard.h"
#include "core/il2cpp_api.h"
#include "core/log.h"
#include "core/module.h"
#include "core/resolver.h"
#include "core/unity.h"
#include "ui/hud.h"
#include "ui/overlay.h"

namespace sr2::modules::debug_tools::hitboxes {
namespace {

namespace unity = sr2::core::unity;
using sr2::il2cpp::api;
using sr2::il2cpp::Resolver;

constexpr const char* kModule = "debug_tools";
constexpr const char* kHudOwner = "debug_tools.hitboxes";
constexpr float kRefreshSeconds = 0.5f;
constexpr size_t kMaxColliders = 160;
constexpr int kCircleSegments = 20;

struct Vec3 {
    float x, y, z;
};
struct Mat4 {
    float m[16];  // Unity Matrix4x4 memory order (column-major): element (r, c) at [c * 4 + r]
};
struct Bounds {
    Vec3 center, extents;
};

Vec3 Mul(const Mat4& a, Vec3 p) {
    return {a.m[0] * p.x + a.m[4] * p.y + a.m[8] * p.z + a.m[12], a.m[1] * p.x + a.m[5] * p.y + a.m[9] * p.z + a.m[13],
            a.m[2] * p.x + a.m[6] * p.y + a.m[10] * p.z + a.m[14]};
}

// IM_COL32 layout without including imgui here: A<<24 | B<<16 | G<<8 | R
constexpr uint32_t Rgba(uint32_t r, uint32_t g, uint32_t b, uint32_t a) { return (a << 24) | (b << 16) | (g << 8) | r; }
constexpr uint32_t kSolid = Rgba(90, 255, 120, 230), kTrigger = Rgba(255, 210, 60, 190), kAabb = Rgba(120, 190, 255, 190);

enum class Shape { kBox, kSphere, kCapsule, kAabb };

struct Kept {
    Il2CppGCHandle h = nullptr;
    Shape shape = Shape::kAabb;
    bool trigger = false;
    Vec3 center{}, size{};    // box: centre, size; sphere / capsule: centre
    float radius = 0, height = 0;
    int direction = 1;        // capsule axis (0 x, 1 y, 2 z)
};

struct State {
    bool ready = false, faulted = false, shown = false, loggedFirst = false;
    Il2CppClass* boxClass = nullptr, *sphereClass = nullptr, *capsuleClass = nullptr, *identClass = nullptr, *camClass = nullptr;
    const MethodInfo* overlap = nullptr, *inParent = nullptr, *colEnabled = nullptr, *colTrigger = nullptr, *colBounds = nullptr;
    const MethodInfo* boxCenter = nullptr, *boxSize = nullptr, *sphCenter = nullptr, *sphRadius = nullptr;
    const MethodInfo* capCenter = nullptr, *capRadius = nullptr, *capHeight = nullptr, *capDir = nullptr;
    const MethodInfo* trMatrix = nullptr, *trPosition = nullptr, *camView = nullptr, *camProj = nullptr;
    std::vector<Kept> kept;
    float sinceRefresh = kRefreshSeconds;
    float lastRange = -1;
    bool lastTriggers = false;
};
State g;

template <class T>
T Value(Il2CppObject* boxed, T def = T{}) {
    return boxed ? *reinterpret_cast<T*>(reinterpret_cast<char*>(boxed) + 2 * sizeof(void*)) : def;
}

// A guarded invoke; a native fault stops the view for the session and marks the module faulted.
Il2CppObject* Call(const MethodInfo* m, void* obj, std::initializer_list<void*> args, const char* what, bool* ok = nullptr) {
    if (ok) *ok = false;
    if (!m || g.faulted) return nullptr;
    std::vector<void*> params(args);
    std::string err;
    Il2CppObject* r = Resolver::Get().Invoke(m, obj, params.empty() ? nullptr : params.data(), &err);
    if (!err.empty()) {
        if (err.rfind("native exception", 0) == 0) {
            g.faulted = true;
            sr2::core::MarkModuleFaulted(kModule, std::format("hitboxes: {}: {}", what, err));
        } else {
            SR2_WARN("[{}] hitboxes: {} failed: {}", kModule, what, err);
        }
        return nullptr;
    }
    if (ok) *ok = true;
    return r;
}

void Release() {
    for (Kept& k : g.kept)
        if (k.h) unity::FreeGCHandle(k.h);
    g.kept.clear();
}

void Hide() {
    if (g.shown) sr2::ui::hud::Clear(kHudOwner);
    g.shown = false;
}

bool IsExactly(Il2CppObject* o, Il2CppClass* k) { return o && k && api().il2cpp_object_get_class(o) == k; }

bool CameraPos(Vec3& out) {
    Il2CppObject* cam = unity::Camera_main();
    Il2CppObject* tr = cam ? unity::TransformOf(cam) : nullptr;
    bool ok = false;
    if (tr) out = Value<Vec3>(Call(g.trPosition, tr, {}, "Transform.get_position", &ok));
    return ok;
}

// The colliders of entities around the camera, nearest first.
void Refresh(float range, bool triggers) {
    Release();
    Vec3 eye{};
    if (!CameraPos(eye)) return;
    int32_t mask = -1, query = triggers ? 2 : 1;  // QueryTriggerInteraction.Collide / Ignore
    bool ok = false;
    Il2CppObject* arr = Call(g.overlap, nullptr, {&eye, &range, &mask, &query}, "Physics.OverlapSphere", &ok);
    if (!ok || !arr) return;
    struct Found {
        Il2CppObject* col;
        float d2;
        Bounds b;
    };
    std::vector<Found> found;
    for (size_t i = 0, n = std::min<size_t>(unity::ArrayLength(arr), 1024); i < n; ++i) {
        auto* c = static_cast<Il2CppObject*>(unity::ArrayAt(arr, i));
        if (!unity::IsAlive(c)) continue;
        if (!Value<bool>(Call(g.colEnabled, c, {}, "Collider.get_enabled", &ok)) || !ok) continue;
        bool inactive = false;
        if (!Call(g.inParent, c, {unity::TypeOf(g.identClass), &inactive}, "GetComponentInParent(Identifiable)", &ok) || !ok) continue;
        const Bounds b = Value<Bounds>(Call(g.colBounds, c, {}, "Collider.get_bounds", &ok));
        if (!ok) continue;
        const float dx = b.center.x - eye.x, dy = b.center.y - eye.y, dz = b.center.z - eye.z;
        if (std::fabs(dx) <= b.extents.x && std::fabs(dy) <= b.extents.y && std::fabs(dz) <= b.extents.z) continue;  // the player
        found.push_back({c, dx * dx + dy * dy + dz * dz, b});
        if (g.faulted) return;
    }
    std::sort(found.begin(), found.end(), [](const Found& a, const Found& b) { return a.d2 < b.d2; });
    if (found.size() > kMaxColliders) found.resize(kMaxColliders);
    for (const Found& f : found) {
        Kept k;
        k.trigger = Value<bool>(Call(g.colTrigger, f.col, {}, "Collider.get_isTrigger", &ok));
        if (IsExactly(f.col, g.boxClass)) {
            k.shape = Shape::kBox;
            k.center = Value<Vec3>(Call(g.boxCenter, f.col, {}, "BoxCollider.get_center", &ok));
            k.size = Value<Vec3>(Call(g.boxSize, f.col, {}, "BoxCollider.get_size", &ok));
        } else if (IsExactly(f.col, g.sphereClass)) {
            k.shape = Shape::kSphere;
            k.center = Value<Vec3>(Call(g.sphCenter, f.col, {}, "SphereCollider.get_center", &ok));
            k.radius = Value<float>(Call(g.sphRadius, f.col, {}, "SphereCollider.get_radius", &ok));
        } else if (IsExactly(f.col, g.capsuleClass)) {
            k.shape = Shape::kCapsule;
            k.center = Value<Vec3>(Call(g.capCenter, f.col, {}, "CapsuleCollider.get_center", &ok));
            k.radius = Value<float>(Call(g.capRadius, f.col, {}, "CapsuleCollider.get_radius", &ok));
            k.height = Value<float>(Call(g.capHeight, f.col, {}, "CapsuleCollider.get_height", &ok));
            k.direction = std::clamp(Value<int32_t>(Call(g.capDir, f.col, {}, "CapsuleCollider.get_direction", &ok)), 0, 2);
        }
        if (g.faulted) return;
        k.h = unity::NewGCHandle(f.col);
        if (k.h) g.kept.push_back(k);
    }
    if (!g.loggedFirst && !g.kept.empty()) {
        g.loggedFirst = true;
        SR2_INFO("[{}] hitboxes: showing {} entity collider(s) within {:.0f} m of the camera ({} found by the physics query)",
                 kModule, g.kept.size(), range, unity::ArrayLength(arr));
    }
}

void AddLine(std::vector<sr2::ui::hud::Line>& out, Vec3 a, Vec3 b, uint32_t color) {
    out.push_back({a.x, a.y, a.z, b.x, b.y, b.z, color});
}

// A circle of radius r round c in the plane of the unit vectors u, v (world or local, by `xf`).
template <class Xf>
void Circle(std::vector<sr2::ui::hud::Line>& out, Vec3 c, Vec3 u, Vec3 v, float r, uint32_t color, Xf xf, float from = 0,
            float to = 6.2831853f, int segments = kCircleSegments) {
    Vec3 prev{};
    for (int i = 0; i <= segments; ++i) {
        const float t = from + (to - from) * static_cast<float>(i) / static_cast<float>(segments);
        const float cs = std::cos(t) * r, sn = std::sin(t) * r;
        const Vec3 p = xf({c.x + u.x * cs + v.x * sn, c.y + u.y * cs + v.y * sn, c.z + u.z * cs + v.z * sn});
        if (i) AddLine(out, prev, p, color);
        prev = p;
    }
}

void Lines(const Kept& k, Il2CppObject* col, std::vector<sr2::ui::hud::Line>& out) {
    const uint32_t color = k.shape == Shape::kAabb ? kAabb : k.trigger ? kTrigger : kSolid;
    bool ok = false;
    if (k.shape == Shape::kAabb) {
        const Bounds b = Value<Bounds>(Call(g.colBounds, col, {}, "Collider.get_bounds", &ok));
        if (!ok) return;
        const Vec3 h = b.extents, c = b.center;  // world axes
        Vec3 p[8];
        for (int i = 0; i < 8; ++i)
            p[i] = {c.x + ((i & 1) ? h.x : -h.x), c.y + ((i & 2) ? h.y : -h.y), c.z + ((i & 4) ? h.z : -h.z)};
        for (int i = 0; i < 8; ++i)
            for (int bit : {1, 2, 4})
                if (!(i & bit)) AddLine(out, p[i], p[i | bit], color);
        return;
    }
    Il2CppObject* tr = unity::TransformOf(col);
    const Mat4 m = Value<Mat4>(tr ? Call(g.trMatrix, tr, {}, "Transform.get_localToWorldMatrix", &ok) : nullptr);
    if (!ok) return;
    auto local = [&](Vec3 p) { return Mul(m, p); };
    if (k.shape == Shape::kBox) {
        const Vec3 h{k.size.x * 0.5f, k.size.y * 0.5f, k.size.z * 0.5f};
        Vec3 p[8];
        for (int i = 0; i < 8; ++i)
            p[i] = local({k.center.x + ((i & 1) ? h.x : -h.x), k.center.y + ((i & 2) ? h.y : -h.y), k.center.z + ((i & 4) ? h.z : -h.z)});
        for (int i = 0; i < 8; ++i)
            for (int bit : {1, 2, 4})
                if (!(i & bit)) AddLine(out, p[i], p[i | bit], color);
        return;
    }
    if (k.shape == Shape::kSphere) {  // Unity scales the radius by the largest axis scale
        const Vec3 c = local(k.center);
        auto len = [&](int col3) {
            return std::sqrt(m.m[col3 * 4] * m.m[col3 * 4] + m.m[col3 * 4 + 1] * m.m[col3 * 4 + 1] + m.m[col3 * 4 + 2] * m.m[col3 * 4 + 2]);
        };
        const float r = k.radius * std::max({len(0), len(1), len(2)});
        auto world = [](Vec3 p) { return p; };
        Circle(out, c, {1, 0, 0}, {0, 1, 0}, r, color, world);
        Circle(out, c, {1, 0, 0}, {0, 0, 1}, r, color, world);
        Circle(out, c, {0, 1, 0}, {0, 0, 1}, r, color, world);
        return;
    }
    // Capsule (local space, then the transform): two rings, four side lines, two half arcs per cap.
    Vec3 axis{0, 0, 0}, u{0, 0, 0}, v{0, 0, 0};
    (k.direction == 0 ? axis.x : k.direction == 1 ? axis.y : axis.z) = 1.0f;
    (k.direction == 0 ? u.y : k.direction == 1 ? u.z : u.x) = 1.0f;
    (k.direction == 0 ? v.z : k.direction == 1 ? v.x : v.y) = 1.0f;
    const float half = std::max(0.0f, k.height * 0.5f - k.radius), r = k.radius;
    const Vec3 top{k.center.x + axis.x * half, k.center.y + axis.y * half, k.center.z + axis.z * half};
    const Vec3 bot{k.center.x - axis.x * half, k.center.y - axis.y * half, k.center.z - axis.z * half};
    Circle(out, top, u, v, r, color, local);
    Circle(out, bot, u, v, r, color, local);
    for (Vec3 d : {u, v, Vec3{-u.x, -u.y, -u.z}, Vec3{-v.x, -v.y, -v.z}})
        AddLine(out, local({top.x + d.x * r, top.y + d.y * r, top.z + d.z * r}), local({bot.x + d.x * r, bot.y + d.y * r, bot.z + d.z * r}),
                color);
    for (Vec3 side : {u, v}) {
        Circle(out, top, side, axis, r, color, local, 0.0f, 3.14159265f, kCircleSegments / 2);
        const Vec3 down{-axis.x, -axis.y, -axis.z};
        Circle(out, bot, side, down, r, color, local, 0.0f, 3.14159265f, kCircleSegments / 2);
    }
}

}  // namespace

bool Setup(sr2::core::ModuleContext& ctx, std::string& why) {
    Resolver& R = ctx.resolver();
    auto cls = [&](const char* image, const char* ns, const char* name) {
        std::string err;
        Il2CppClass* k = R.Class(image, ns, name, &err);
        if (!k && why.empty()) why = std::format("class {}.{}: {}", ns, name, err);
        return k;
    };
    auto method = [&](Il2CppClass* k, const char* name, int argc) -> const MethodInfo* {
        for (Il2CppClass* c = k; c; c = api().il2cpp_class_get_parent ? api().il2cpp_class_get_parent(c) : nullptr)
            if (const MethodInfo* m = R.Method(c, name, argc)) return m;
        if (why.empty()) why = std::format("method {}({}) not found", name, argc);
        return nullptr;
    };
    auto byParams = [&](Il2CppClass* k, const char* name, std::initializer_list<std::string_view> params) {
        std::string err;
        const MethodInfo* m = k ? R.MethodByParams(k, name, params, &err) : nullptr;
        if (!m && why.empty()) why = std::format("{}: {}", name, err);
        return m;
    };
    const char* kPhys = "UnityEngine.PhysicsModule";
    const char* kCore = "UnityEngine.CoreModule";
    Il2CppClass* physics = cls(kPhys, "UnityEngine", "Physics");
    Il2CppClass* collider = cls(kPhys, "UnityEngine", "Collider");
    g.boxClass = cls(kPhys, "UnityEngine", "BoxCollider");
    g.sphereClass = cls(kPhys, "UnityEngine", "SphereCollider");
    g.capsuleClass = cls(kPhys, "UnityEngine", "CapsuleCollider");
    g.identClass = cls("Assembly-CSharp", "", "Identifiable");
    g.camClass = cls(kCore, "UnityEngine", "Camera");
    Il2CppClass* component = cls(kCore, "UnityEngine", "Component");
    Il2CppClass* transform = cls(kCore, "UnityEngine", "Transform");
    if (!why.empty()) return false;
    g.overlap = byParams(physics, "OverlapSphere",
                         {"UnityEngine.Vector3", "System.Single", "System.Int32", "UnityEngine.QueryTriggerInteraction"});
    // 1.3.0 strips Component.GetComponentInParent(Type); only the (Type, bool includeInactive) overload is left.
    g.inParent = byParams(component, "GetComponentInParent", {"System.Type", "System.Boolean"});
    g.colEnabled = method(collider, "get_enabled", 0);
    g.colTrigger = method(collider, "get_isTrigger", 0);
    g.colBounds = method(collider, "get_bounds", 0);
    g.boxCenter = method(g.boxClass, "get_center", 0);
    g.boxSize = method(g.boxClass, "get_size", 0);
    g.sphCenter = method(g.sphereClass, "get_center", 0);
    g.sphRadius = method(g.sphereClass, "get_radius", 0);
    g.capCenter = method(g.capsuleClass, "get_center", 0);
    g.capRadius = method(g.capsuleClass, "get_radius", 0);
    g.capHeight = method(g.capsuleClass, "get_height", 0);
    g.capDir = method(g.capsuleClass, "get_direction", 0);
    g.trMatrix = method(transform, "get_localToWorldMatrix", 0);
    g.trPosition = method(transform, "get_position", 0);
    g.camView = method(g.camClass, "get_worldToCameraMatrix", 0);
    g.camProj = method(g.camClass, "get_projectionMatrix", 0);
    if (!why.empty()) return false;
    g.ready = true;
    return true;
}

void Tick(float dt, bool want, float range, bool triggers) {
    if (!g.ready || g.faulted || !want || sr2::ui::overlay::IsOpen()) {
        Hide();
        if (!want && !g.kept.empty()) Release();
        if (!want) g.loggedFirst = false;
        return;
    }
    range = std::clamp(range, 1.0f, 50.0f);
    g.sinceRefresh += std::isfinite(dt) && dt > 0 ? std::min(dt, 1.0f) : 0.0f;
    if (g.sinceRefresh >= kRefreshSeconds || range != g.lastRange || triggers != g.lastTriggers) {
        g.sinceRefresh = 0;
        g.lastRange = range;
        g.lastTriggers = triggers;
        Refresh(range, triggers);
        if (g.faulted) return Hide();
    }
    Il2CppObject* cam = unity::Camera_main();
    if (!cam || g.kept.empty()) return Hide();
    bool ok = false;
    const Mat4 v = Value<Mat4>(Call(g.camView, cam, {}, "Camera.get_worldToCameraMatrix", &ok));
    const Mat4 p = ok ? Value<Mat4>(Call(g.camProj, cam, {}, "Camera.get_projectionMatrix", &ok)) : Mat4{};
    if (!ok) return Hide();
    sr2::ui::hud::Frame f;
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r) {
            float s = 0;
            for (int k = 0; k < 4; ++k) s += p.m[k * 4 + r] * v.m[c * 4 + k];
            f.vp[c * 4 + r] = s;
        }
    for (float x : f.vp)
        if (!std::isfinite(x)) return Hide();
    for (const Kept& k : g.kept) {
        Il2CppObject* col = unity::GCHandleTarget(k.h);
        if (!unity::IsAlive(col)) continue;
        Lines(k, col, f.lines);
        if (g.faulted) return Hide();
    }
    if (f.lines.empty()) return Hide();
    sr2::ui::hud::Publish(kHudOwner, std::move(f));
    g.shown = true;
}

}  // namespace sr2::modules::debug_tools::hitboxes
