#include "unity.h"

#include <atomic>
#include <format>
#include <mutex>
#include <unordered_map>

#include "guard.h"
#include "log.h"
#include "resolver.h"

// Direct calls through IL2CPP method pointers. Convention (x64 IL2CPP):
//   instance:  R fn(this, args..., const MethodInfo*)
//   static:    R fn(args..., const MethodInfo*)
// Every call is wrapped in Try() so a managed exception (e.g. Unity's
// NullReferenceException for destroyed objects) cannot unwind into our caller.

namespace sr2::core::unity {
namespace {

using il2cpp::api;
using il2cpp::Resolver;

template <class Fn>
struct M {
    Fn fn = nullptr;
    const MethodInfo* mi = nullptr;
    explicit operator bool() const { return fn != nullptr; }
};

template <class Fn>
M<Fn> Bind(const MethodInfo* mi, const char* what) {
    M<Fn> m;
    if (!mi) {
        SR2_WARN("unity: {} not resolved", what);
        return m;
    }
    std::string err;
    m.fn = reinterpret_cast<Fn>(Resolver::Get().MethodPointer(mi, &err));
    if (m.fn) m.mi = mi;
    else SR2_WARN("unity: {}: {}", what, err);
    return m;
}

struct Api {
    Il2CppClass* object = nullptr;
    Il2CppClass* gameObject = nullptr;
    Il2CppClass* component = nullptr;
    Il2CppClass* transform = nullptr;
    size_t cachedPtrOffset = 0;  // 0 = unknown

    M<Il2CppArray* (*)(Il2CppObject*, const MethodInfo*)> findObjectsOfTypeAll;
    M<Il2CppObject* (*)(Il2CppObject*, const MethodInfo*)> findFirstObjectByType;
    M<Il2CppObject* (*)(Il2CppObject*, Il2CppObject*, const MethodInfo*)> goGetComponent;
    M<Il2CppObject* (*)(Il2CppObject*, Il2CppObject*, const MethodInfo*)> compGetComponent;
    M<Il2CppObject* (*)(Il2CppObject*, const MethodInfo*)> compGetGameObject;
    M<Il2CppObject* (*)(Il2CppObject*, const MethodInfo*)> compGetTransform;
    M<Il2CppObject* (*)(Il2CppObject*, const MethodInfo*)> goGetTransform;
    M<Il2CppObject* (*)(Il2CppObject*, Il2CppObject*, const MethodInfo*)> instantiateParent;
    M<Il2CppObject* (*)(Il2CppObject*, const MethodInfo*)> instantiate;
    M<void (*)(Il2CppObject*, bool, const MethodInfo*)> setActive;
    M<bool (*)(Il2CppObject*, const MethodInfo*)> activeInHierarchy;
    M<void (*)(Il2CppObject*, Il2CppString*, const MethodInfo*)> setName;
    M<Il2CppString* (*)(Il2CppObject*, const MethodInfo*)> getName;
    M<Il2CppObject* (*)(const MethodInfo*)> cameraMain;
    M<float (*)(const MethodInfo*)> deltaTime, unscaledDeltaTime, fixedDeltaTime;
};

const Api& U() {
    static const Api a = [] {
        Api u;
        Resolver& r = Resolver::Get();
        std::string err;
        const char* core = "UnityEngine.CoreModule";
        u.object = r.Class(core, "UnityEngine", "Object", &err);
        u.gameObject = r.Class(core, "UnityEngine", "GameObject", &err);
        u.component = r.Class(core, "UnityEngine", "Component", &err);
        u.transform = r.Class(core, "UnityEngine", "Transform", &err);
        Il2CppClass* resources = r.Class(core, "UnityEngine", "Resources", &err);
        Il2CppClass* camera = r.Class(core, "UnityEngine", "Camera", &err);
        Il2CppClass* time = r.Class(core, "UnityEngine", "Time", &err);

        if (auto off = r.FieldOffset(u.object, "m_CachedPtr", &err)) u.cachedPtrOffset = *off;
        else SR2_WARN("unity: UnityEngine.Object.m_CachedPtr: {}", err);

        auto mp = [&](Il2CppClass* k, const char* name, std::initializer_list<std::string_view> params) {
            std::string e;
            const MethodInfo* m = r.MethodByParams(k, name, params, &e);
            if (!m) SR2_WARN("unity: {}", e);
            return m;
        };
        auto m0 = [&](Il2CppClass* k, const char* name) {
            std::string e;
            const MethodInfo* m = r.Method(k, name, 0, &e);
            if (!m) SR2_WARN("unity: {}", e);
            return m;
        };

        u.findObjectsOfTypeAll = Bind<decltype(u.findObjectsOfTypeAll.fn)>(
            mp(resources, "FindObjectsOfTypeAll", {"System.Type"}), "Resources.FindObjectsOfTypeAll(Type)");
        {
            std::string e;
            const MethodInfo* m = r.MethodByParams(u.object, "FindFirstObjectByType", {"System.Type"}, &e);
            if (!m) m = r.MethodByParams(u.object, "FindObjectOfType", {"System.Type"}, &e);
            u.findFirstObjectByType =
                Bind<decltype(u.findFirstObjectByType.fn)>(m, "Object.FindFirstObjectByType(Type)");
        }
        u.goGetComponent = Bind<decltype(u.goGetComponent.fn)>(mp(u.gameObject, "GetComponent", {"System.Type"}),
                                                               "GameObject.GetComponent(Type)");
        u.compGetComponent = Bind<decltype(u.compGetComponent.fn)>(mp(u.component, "GetComponent", {"System.Type"}),
                                                                   "Component.GetComponent(Type)");
        u.compGetGameObject =
            Bind<decltype(u.compGetGameObject.fn)>(m0(u.component, "get_gameObject"), "Component.get_gameObject");
        u.compGetTransform =
            Bind<decltype(u.compGetTransform.fn)>(m0(u.component, "get_transform"), "Component.get_transform");
        u.goGetTransform =
            Bind<decltype(u.goGetTransform.fn)>(m0(u.gameObject, "get_transform"), "GameObject.get_transform");
        u.instantiateParent = Bind<decltype(u.instantiateParent.fn)>(
            mp(u.object, "Instantiate", {"UnityEngine.Object", "UnityEngine.Transform"}),
            "Object.Instantiate(Object, Transform)");
        u.instantiate = Bind<decltype(u.instantiate.fn)>(mp(u.object, "Instantiate", {"UnityEngine.Object"}),
                                                         "Object.Instantiate(Object)");
        u.setActive = Bind<decltype(u.setActive.fn)>(mp(u.gameObject, "SetActive", {"System.Boolean"}),
                                                     "GameObject.SetActive(bool)");
        u.activeInHierarchy = Bind<decltype(u.activeInHierarchy.fn)>(m0(u.gameObject, "get_activeInHierarchy"),
                                                                     "GameObject.get_activeInHierarchy");
        u.setName = Bind<decltype(u.setName.fn)>(mp(u.object, "set_name", {"System.String"}), "Object.set_name");
        u.getName = Bind<decltype(u.getName.fn)>(m0(u.object, "get_name"), "Object.get_name");
        u.cameraMain = Bind<decltype(u.cameraMain.fn)>(m0(camera, "get_main"), "Camera.get_main");
        u.deltaTime = Bind<decltype(u.deltaTime.fn)>(m0(time, "get_deltaTime"), "Time.get_deltaTime");
        u.unscaledDeltaTime =
            Bind<decltype(u.unscaledDeltaTime.fn)>(m0(time, "get_unscaledDeltaTime"), "Time.get_unscaledDeltaTime");
        u.fixedDeltaTime =
            Bind<decltype(u.fixedDeltaTime.fn)>(m0(time, "get_fixedDeltaTime"), "Time.get_fixedDeltaTime");
        return u;
    }();
    return a;
}

bool IsGameObject(Il2CppObject* o) {
    const Api& u = U();
    return o && u.gameObject && ClassOf(o) == u.gameObject;  // GameObject is sealed
}

template <class R, class F>
R Call(F&& f, R fallback = R{}) {
    R out = fallback;
    if (!Try([&] { out = f(); })) return fallback;
    return out;
}

struct ListFields {
    size_t items = 0, size = 0;
    bool ok = false;
};

}  // namespace

Il2CppString* NewString(std::string_view utf8) {
    auto fn = api().il2cpp_string_new;
    if (!fn) return nullptr;
    std::string s(utf8);
    return fn(s.c_str());
}

std::string ToUtf8(Il2CppString* s) { return Resolver::Get().ToUtf8(s); }

Il2CppObject* TypeOf(Il2CppClass* k) {
    auto& a = api();
    if (!k || !a.il2cpp_class_get_type || !a.il2cpp_type_get_object) return nullptr;
    const Il2CppType* t = a.il2cpp_class_get_type(k);
    return t ? a.il2cpp_type_get_object(t) : nullptr;
}

std::vector<Il2CppObject*> FindObjectsOfTypeAll(Il2CppClass* k) {
    std::vector<Il2CppObject*> out;
    const Api& u = U();
    Il2CppObject* type = TypeOf(k);
    if (!type || !u.findObjectsOfTypeAll) return out;
    Il2CppArray* arr = Call<Il2CppArray*>([&] { return u.findObjectsOfTypeAll.fn(type, u.findObjectsOfTypeAll.mi); });
    size_t n = ArrayLength(arr);
    out.reserve(n);
    for (size_t i = 0; i < n; ++i)
        if (void* e = ArrayAt(arr, i)) out.push_back(static_cast<Il2CppObject*>(e));
    return out;
}

Il2CppObject* FindObjectOfType(Il2CppClass* k) {
    const Api& u = U();
    Il2CppObject* type = TypeOf(k);
    if (!type || !u.findFirstObjectByType) return nullptr;
    return Call<Il2CppObject*>([&] { return u.findFirstObjectByType.fn(type, u.findFirstObjectByType.mi); });
}

bool IsAlive(Il2CppObject* obj) {
    if (!obj) return false;
    const Api& u = U();
    if (!u.cachedPtrOffset) return true;  // cannot tell; assume alive
    return Field<void*>(obj, u.cachedPtrOffset) != nullptr;
}

Il2CppObject* GetComponent(Il2CppObject* goOrComponent, Il2CppClass* k) {
    if (!IsAlive(goOrComponent) || !k) return nullptr;
    const Api& u = U();
    Il2CppObject* type = TypeOf(k);
    if (!type) return nullptr;
    Il2CppObject* r = nullptr;
    if (IsGameObject(goOrComponent)) {
        if (u.goGetComponent) r = Call<Il2CppObject*>([&] { return u.goGetComponent.fn(goOrComponent, type, u.goGetComponent.mi); });
    } else if (u.compGetComponent) {
        r = Call<Il2CppObject*>([&] { return u.compGetComponent.fn(goOrComponent, type, u.compGetComponent.mi); });
    }
    return IsAlive(r) ? r : nullptr;  // Unity returns "fake null" objects in some paths
}

Il2CppObject* GameObjectOf(Il2CppObject* component) {
    if (!IsAlive(component)) return nullptr;
    if (IsGameObject(component)) return component;
    const Api& u = U();
    if (!u.compGetGameObject) return nullptr;
    return Call<Il2CppObject*>([&] { return u.compGetGameObject.fn(component, u.compGetGameObject.mi); });
}

Il2CppObject* TransformOf(Il2CppObject* goOrComponent) {
    if (!IsAlive(goOrComponent)) return nullptr;
    const Api& u = U();
    if (IsGameObject(goOrComponent)) {
        if (!u.goGetTransform) return nullptr;
        return Call<Il2CppObject*>([&] { return u.goGetTransform.fn(goOrComponent, u.goGetTransform.mi); });
    }
    if (!u.compGetTransform) return nullptr;
    return Call<Il2CppObject*>([&] { return u.compGetTransform.fn(goOrComponent, u.compGetTransform.mi); });
}

Il2CppObject* Instantiate(Il2CppObject* original, Il2CppObject* parentTransform) {
    if (!IsAlive(original)) return nullptr;
    const Api& u = U();
    if (parentTransform && IsAlive(parentTransform)) {
        if (!u.instantiateParent) return nullptr;
        return Call<Il2CppObject*>(
            [&] { return u.instantiateParent.fn(original, parentTransform, u.instantiateParent.mi); });
    }
    if (!u.instantiate) return nullptr;
    return Call<Il2CppObject*>([&] { return u.instantiate.fn(original, u.instantiate.mi); });
}

void SetActive(Il2CppObject* go, bool active) {
    if (!IsAlive(go)) return;
    const Api& u = U();
    if (!u.setActive) return;
    Try([&] { u.setActive.fn(go, active, u.setActive.mi); });
}

void SetName(Il2CppObject* obj, std::string_view name) {
    if (!IsAlive(obj)) return;
    const Api& u = U();
    Il2CppString* s = NewString(name);
    if (!u.setName || !s) return;
    Try([&] { u.setName.fn(obj, s, u.setName.mi); });
}

float DeltaTime() {
    const Api& u = U();
    return u.deltaTime ? Call<float>([&] { return u.deltaTime.fn(u.deltaTime.mi); }) : 0.0f;
}
float UnscaledDeltaTime() {
    const Api& u = U();
    return u.unscaledDeltaTime ? Call<float>([&] { return u.unscaledDeltaTime.fn(u.unscaledDeltaTime.mi); }) : 0.0f;
}
float FixedDeltaTime() {
    const Api& u = U();
    return u.fixedDeltaTime ? Call<float>([&] { return u.fixedDeltaTime.fn(u.fixedDeltaTime.mi); }) : 0.0f;
}

size_t ArrayLength(void* arr) {
    if (!arr) return 0;
    if (auto fn = api().il2cpp_array_length) return fn(static_cast<Il2CppArray*>(arr));
    // Fallback: Il2CppArray::max_length (il2cpp_array_size_t) right before the data.
    return Field<size_t>(arr, kArrayDataOffset - sizeof(size_t));
}

void* ArrayAt(void* arr, size_t i) {
    if (!arr || i >= ArrayLength(arr)) return nullptr;
    return ArrayData<void*>(arr)[i];
}

namespace {
ListFields FieldsOf(void* list) {
    static std::mutex mx;
    static std::unordered_map<Il2CppClass*, ListFields> cache;
    ListFields f;
    Il2CppClass* k = ClassOf(static_cast<Il2CppObject*>(list));
    if (!k) return f;
    std::lock_guard lock(mx);
    auto it = cache.find(k);
    if (it != cache.end()) return it->second;
    std::string err;
    auto items = Resolver::Get().FieldOffset(k, "_items", &err);
    auto size = items ? Resolver::Get().FieldOffset(k, "_size", &err) : std::nullopt;
    if (items && size) f = {*items, *size, true};
    else SR2_WARN("unity: List fields: {}", err);
    cache.emplace(k, f);
    return f;
}

size_t ListCountWith(void* list, const ListFields& f) {
    if (!f.ok) return 0;
    int n = Field<int>(list, f.size);
    void* items = Field<void*>(list, f.items);
    if (n <= 0 || !items) return 0;
    size_t cap = ArrayLength(items);
    return static_cast<size_t>(n) < cap ? static_cast<size_t>(n) : cap;
}
}  // namespace

size_t ListCount(void* list) { return ListCountWith(list, FieldsOf(list)); }

void* ListAt(void* list, size_t i) {
    ListFields f = FieldsOf(list);
    if (i >= ListCountWith(list, f)) return nullptr;
    return ArrayAt(Field<void*>(list, f.items), i);
}

Il2CppClass* ClassOf(Il2CppObject* obj) {
    auto fn = api().il2cpp_object_get_class;
    if (!obj || !fn) return nullptr;
    Il2CppClass* k = nullptr;
    if (!Try([&] { k = fn(obj); })) return nullptr;  // bogus / freed pointer
    return k;
}

std::string ClassName(Il2CppObject* obj) {
    auto& a = api();
    Il2CppClass* k = ClassOf(obj);
    if (!k || !a.il2cpp_class_get_name) return {};
    const char* n = a.il2cpp_class_get_name(k);
    const char* ns = a.il2cpp_class_get_namespace ? a.il2cpp_class_get_namespace(k) : nullptr;
    std::string out = (ns && *ns) ? std::string(ns) + "." : std::string();
    return out + (n ? n : "?");
}

std::string GetName(Il2CppObject* obj) {
    if (!IsAlive(obj)) return {};
    const Api& u = U();
    if (!u.getName) return {};
    return ToUtf8(Call<Il2CppString*>([&] { return u.getName.fn(obj, u.getName.mi); }));
}

bool ActiveInHierarchy(Il2CppObject* go) {
    if (!IsAlive(go)) return false;
    const Api& u = U();
    if (!IsGameObject(go)) go = GameObjectOf(go);
    if (!go || !u.activeInHierarchy) return false;
    return Call<bool>([&] { return u.activeInHierarchy.fn(go, u.activeInHierarchy.mi); });
}

Il2CppObject* Camera_main() {
    const Api& u = U();
    if (!u.cameraMain) return nullptr;
    Il2CppObject* c = Call<Il2CppObject*>([&] { return u.cameraMain.fn(u.cameraMain.mi); });
    return IsAlive(c) ? c : nullptr;
}

Il2CppClass* GameObjectClass() { return U().gameObject; }
Il2CppClass* TransformClass() { return U().transform; }
Il2CppClass* ComponentClass() { return U().component; }

namespace {
std::atomic<int> g_gcState{0};  // 0 = untested, 1 = ok, -1 = failed
}  // namespace

Il2CppGCHandle NewGCHandle(Il2CppObject* obj, bool pinned) {
    auto fn = api().il2cpp_gchandle_new;
    if (g_gcState.load(std::memory_order_relaxed) < 0) return nullptr;
    return (obj && fn) ? fn(obj, pinned) : nullptr;
}

bool GCHandlesUsable() { return g_gcState.load() >= 0; }

bool GCHandleSelfTest(std::string* report) {
    static_assert(sizeof(Il2CppGCHandle) == sizeof(void*), "Il2CppGCHandle must be pointer-sized (Unity 6 IL2CPP)");
    auto& a = api();
    auto fail = [&](std::string why) {
        g_gcState = -1;
        if (report) *report = std::format("GC handle self-test FAILED ({}-bit handles): {}", sizeof(Il2CppGCHandle) * 8, why);
        return false;
    };
    if (!a.il2cpp_string_new || !a.il2cpp_gchandle_new || !a.il2cpp_gchandle_get_target || !a.il2cpp_gchandle_free)
        return fail("il2cpp_string_new / il2cpp_gchandle_* exports missing");
    Il2CppObject* obj = nullptr;
    Il2CppGCHandle h = nullptr, hp = nullptr;
    Il2CppObject* back = nullptr;
    Il2CppObject* backPinned = nullptr;
    std::string err;
    bool ok = Try([&] {
        obj = reinterpret_cast<Il2CppObject*>(a.il2cpp_string_new("sr2mods gc handle self-test"));
        if (!obj) return;
        h = a.il2cpp_gchandle_new(obj, false);
        hp = a.il2cpp_gchandle_new(obj, true);
        back = h ? a.il2cpp_gchandle_get_target(h) : nullptr;
        backPinned = hp ? a.il2cpp_gchandle_get_target(hp) : nullptr;
        if (h) a.il2cpp_gchandle_free(h);
        if (hp) a.il2cpp_gchandle_free(hp);
    }, &err);
    if (!ok) return fail(err);
    if (!obj) return fail("il2cpp_string_new returned null");
    if (!h || !hp) return fail("il2cpp_gchandle_new returned 0");
    if (back != obj || backPinned != obj)
        return fail(std::format("get_target round trip mismatch (object {}, normal {} -> {}, pinned {} -> {})",
                                static_cast<void*>(obj), h, static_cast<void*>(back), hp,
                                static_cast<void*>(backPinned)));
    g_gcState = 1;
    if (report)
        *report = std::format("GC handle self-test OK: {}-bit handles (normal {}, pinned {}) round-trip to {}",
                              sizeof(Il2CppGCHandle) * 8, h, hp, static_cast<void*>(obj));
    return true;
}

Il2CppObject* GCHandleTarget(Il2CppGCHandle handle) {
    auto fn = api().il2cpp_gchandle_get_target;
    return (handle && fn) ? fn(handle) : nullptr;
}

void FreeGCHandle(Il2CppGCHandle handle) {
    auto fn = api().il2cpp_gchandle_free;
    if (handle && fn) fn(handle);
}

}  // namespace sr2::core::unity
