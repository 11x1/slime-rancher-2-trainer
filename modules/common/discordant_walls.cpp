#include "modules/common/discordant_walls.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <format>
#include <mutex>
#include <unordered_map>

#include "core/il2cpp_api.h"
#include "core/resolver.h"
#include "core/unity.h"

namespace sr2::modules::walls {
namespace {

using il2cpp::api;
using il2cpp::Resolver;

struct Vec3 { float x, y, z; };
struct Bounds { Vec3 center, extents; };  // UnityEngine.Bounds (by hidden pointer, like IL2CPP's own code)

template <class Fn>
struct M {
    Fn fn = nullptr;
    const MethodInfo* mi = nullptr;
};

struct Syms {
    Il2CppClass* wall = nullptr;       // MonomiPark.SlimeRancher.Labyrinth.DiscordantWall
    Il2CppClass* listClass = nullptr;  // List<DiscordantWall> (declared type of _activeWalls)
    Il2CppClass* collider = nullptr;   // UnityEngine.Collider (optional)
    size_t staticOff = 0;              // _activeWalls inside the class's static data
    size_t offTarget = 0;              // _targetHarmonizationAmount (float)
    size_t offCollider = 0;            // _harmonizerDetectionCollider (MeshCollider)
    bool haveCollider = false;
    void* (*staticData)(Il2CppClass*) = nullptr;  // il2cpp_class_get_static_field_data
    M<bool (*)(Il2CppObject*, const MethodInfo*)> activeAndEnabled;      // Behaviour.get_isActiveAndEnabled
    M<Il2CppObject* (*)(Il2CppObject*, const MethodInfo*)> getTransform;  // Component.get_transform
    M<Vec3 (*)(Il2CppObject*, const MethodInfo*)> getPosition;            // Transform.get_position
    M<Bounds (*)(Il2CppObject*, const MethodInfo*)> getBounds;            // Collider.get_bounds (optional)
};

Syms g_s;
// Wall -> collider-bounds centre seen while it was active (main thread only). Walls live as long as their
// scene; the map is dropped when the registry empties (scene group unloaded) and kept small otherwise.
std::unordered_map<Il2CppObject*, Vec3> g_centres;
std::mutex g_resolveMutex;
std::atomic<int> g_state{0};  // 0 not tried, 1 ok, -1 failed
std::string g_err;

template <class Fn>
bool Bind(M<Fn>& m, Il2CppClass* k, const char* name, std::string& err) {
    if (!k) return false;
    std::string e;
    const MethodInfo* mi = Resolver::Get().Method(k, name, 0, &e);
    void* p = mi ? Resolver::Get().MethodPointer(mi, &e) : nullptr;
    if (!p) {
        err = std::format("{}: {}", name, e.empty() ? "no native body" : e);
        return false;
    }
    m.fn = reinterpret_cast<Fn>(p);
    m.mi = mi;
    return true;
}

bool ResolveImpl(std::string& err) {
    Resolver& r = Resolver::Get();
    std::string e;
    Syms s;
    s.wall = r.Class("Assembly-CSharp", "MonomiPark.SlimeRancher.Labyrinth", "DiscordantWall", &e);
    if (!s.wall) return err = "DiscordantWall: " + e, false;

    FieldInfo* listField = r.Field(s.wall, "_activeWalls", &e);
    if (!listField) return err = "DiscordantWall._activeWalls: " + e, false;
    HMODULE ga = api().module;
    auto fieldType = ga ? reinterpret_cast<const Il2CppType* (*)(FieldInfo*)>(GetProcAddress(ga, "il2cpp_field_get_type"))
                        : nullptr;
    auto fieldFlags = ga ? reinterpret_cast<int (*)(FieldInfo*)>(GetProcAddress(ga, "il2cpp_field_get_flags")) : nullptr;
    s.staticData = ga ? reinterpret_cast<void* (*)(Il2CppClass*)>(GetProcAddress(ga, "il2cpp_class_get_static_field_data"))
                      : nullptr;
    if (!fieldType || !s.staticData || !api().il2cpp_class_from_type || !api().il2cpp_object_get_class ||
        !api().il2cpp_class_is_assignable_from)
        return err = "il2cpp_field_get_type / il2cpp_class_get_static_field_data / class exports missing", false;
    constexpr int kFieldStatic = 0x10;  // FIELD_ATTRIBUTE_STATIC
    if (fieldFlags && !(fieldFlags(listField) & kFieldStatic)) return err = "DiscordantWall._activeWalls is not static", false;
    const Il2CppType* lt = fieldType(listField);
    s.listClass = lt ? api().il2cpp_class_from_type(lt) : nullptr;
    if (!s.listClass) return err = "type of DiscordantWall._activeWalls not resolved", false;
    s.staticOff = api().il2cpp_field_get_offset(listField);
    if (s.staticOff > 0x1000) return err = std::format("implausible static offset 0x{:X}", s.staticOff), false;

    auto off = [&](Il2CppClass* k, const char* name, size_t& out) {
        auto o = r.FieldOffset(k, name, &e);
        if (!o) return err = std::format("{}: {}", name, e), false;
        out = *o;
        return true;
    };
    if (!off(s.wall, "_targetHarmonizationAmount", s.offTarget) || !off(s.wall, "_harmonizerDetectionCollider", s.offCollider))
        return false;

    const char* core = "UnityEngine.CoreModule";
    Il2CppClass* behaviour = r.Class(core, "UnityEngine", "Behaviour", &e);
    Il2CppClass* component = r.Class(core, "UnityEngine", "Component", &e);
    Il2CppClass* transform = r.Class(core, "UnityEngine", "Transform", &e);
    if (!Bind(s.activeAndEnabled, behaviour, "get_isActiveAndEnabled", err) ||
        !Bind(s.getTransform, component, "get_transform", err) || !Bind(s.getPosition, transform, "get_position", err))
        return false;
    // optional: wall centre from the detection collider's world AABB (the transform may sit at a corner)
    s.collider = r.Class("UnityEngine.PhysicsModule", "UnityEngine", "Collider", &e);
    std::string optErr;
    s.haveCollider = s.collider && Bind(s.getBounds, s.collider, "get_bounds", optErr);
    g_s = s;
    return true;
}

bool IsA(Il2CppObject* o, Il2CppClass* k) {
    if (!o || !k) return false;
    Il2CppClass* c = api().il2cpp_object_get_class(o);
    return c && (c == k || api().il2cpp_class_is_assignable_from(k, c));
}

bool Finite(const Vec3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

bool Plausible(const void* p) {
    auto v = reinterpret_cast<uintptr_t>(p);
    return v >= 0x10000 && v < 0x00007FFFFFFFFFFFull && (v & 7) == 0;
}

}  // namespace

bool Resolve(std::string& err) {
    std::lock_guard lk(g_resolveMutex);
    if (g_state.load() == 0) g_state.store(ResolveImpl(g_err) ? 1 : -1);
    err = g_err;
    return g_state.load() == 1;
}

bool Resolved() { return g_state.load() == 1; }

bool Collect(std::vector<Wall>& out, Stats* stats, bool includeInactive) {
    if (!Resolved()) return false;
    const Syms& s = g_s;
    Stats st;
    // Static data is allocated when the class is initialised; null (or a null list) = no wall has
    // ever woken up (no Labyrinth loaded yet). We never run the class constructor ourselves.
    auto* data = static_cast<char*>(s.staticData(s.wall));
    Il2CppObject* list = data ? *reinterpret_cast<Il2CppObject**>(data + s.staticOff) : nullptr;
    if (!list) {
        g_centres.clear();
        if (stats) *stats = st;
        return true;
    }
    if (!Plausible(list) || api().il2cpp_object_get_class(list) != s.listClass) return false;  // layout mismatch
    size_t n = core::unity::ListCount(list);
    if (n > 4096) return false;
    st.listed = static_cast<int>(n);
    if (n == 0 || g_centres.size() > 4 * n + 64) g_centres.clear();
    for (size_t i = 0; i < n; ++i) {
        auto* w = static_cast<Il2CppObject*>(core::unity::ListAt(list, i));
        if (!Plausible(w) || !IsA(w, s.wall) || !core::unity::IsAlive(w)) continue;
        ++st.valid;
        Wall wall;
        wall.active = s.activeAndEnabled.fn(w, s.activeAndEnabled.mi);
        if (!wall.active && !includeInactive) continue;
        if (wall.active) ++st.active;
        else ++st.inactive;
        float target = core::unity::Field<float>(w, s.offTarget);
        wall.intact = !(target >= 0.5f);  // NaN counts as intact
        bool placed = false;
        if (!wall.active) {
            auto it = g_centres.find(w);
            if (it != g_centres.end()) {
                wall.x = it->second.x;
                wall.y = it->second.y;
                wall.z = it->second.z;
                placed = true;
                ++st.fromCache;
            }
        }
        if (!placed && s.haveCollider) {
            auto* col = core::unity::Field<Il2CppObject*>(w, s.offCollider);
            if (Plausible(col) && IsA(col, s.collider) && core::unity::IsAlive(col)) {
                Bounds b = s.getBounds.fn(col, s.getBounds.mi);
                if (Finite(b.center) && Finite(b.extents) && (b.extents.x + b.extents.y + b.extents.z) > 0.01f) {
                    wall.x = b.center.x;
                    wall.y = b.center.y;
                    wall.z = b.center.z;
                    placed = true;
                    ++st.fromBounds;
                    if (wall.active) g_centres[w] = b.center;
                }
            }
        }
        if (!placed) {
            Il2CppObject* t = s.getTransform.fn(w, s.getTransform.mi);
            if (!Plausible(t) || !core::unity::IsAlive(t)) continue;
            Vec3 p = s.getPosition.fn(t, s.getPosition.mi);
            if (!Finite(p)) continue;
            wall.x = p.x;
            wall.y = p.y;
            wall.z = p.z;
        }
        if (wall.intact && wall.active) ++st.intact;
        out.push_back(wall);
    }
    if (stats) *stats = st;
    return true;
}

}  // namespace sr2::modules::walls
