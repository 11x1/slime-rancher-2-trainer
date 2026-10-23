#pragma once
// Minimal Unity / uGUI bindings for the minimap, resolved by NAME at runtime
// (UnityEngine.CoreModule, UnityEngine.UIModule, UnityEngine.UI,
// Unity.TextMeshPro). Calls go straight through the IL2CPP method pointers:
//   instance: R fn(this, args..., const MethodInfo*)
//   static:   R fn(args..., const MethodInfo*)
// Value types are plain aggregates so MSVC uses the same x64 ABI as the
// IL2CPP-generated code (8-byte structs in registers, larger ones by hidden
// pointer / reference).
//
// Resolve() runs on the worker thread (name lookups only). Everything else
// must run on the Unity main thread. All wrappers are null-safe.

#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include "core/il2cpp_api.h"

namespace sr2::modules::minimap::ugui {

struct Vec2 { float x, y; };
struct Vec3 { float x, y, z; };
struct Vec4 { float x, y, z, w; };
struct Quat { float x, y, z, w; };
struct Rect { float x, y, w, h; };
struct Color { float r, g, b, a; };

// A bound managed method: native pointer + its MethodInfo (passed last).
struct Fn {
    void* p = nullptr;
    const MethodInfo* mi = nullptr;
    explicit operator bool() const { return p != nullptr; }
};
template <class R, class... A>
R Call(const Fn& f, A... a) {
    return reinterpret_cast<R (*)(A..., const MethodInfo*)>(f.p)(a..., f.mi);
}
Fn BindFn(const MethodInfo* mi);  // validated (pointer inside GameAssembly.dll), empty on failure

// Strong GC handle + raw pointer. IL2CPP's GC does not scan our heap, so any
// managed object kept across frames must be pinned through a GC handle.
class Ref {
public:
    Ref() = default;
    explicit Ref(Il2CppObject* o) { Reset(o); }
    ~Ref() { Reset(nullptr); }
    Ref(const Ref&) = delete;
    Ref& operator=(const Ref&) = delete;
    Ref(Ref&& o) noexcept : p_(o.p_), h_(o.h_) { o.p_ = nullptr; o.h_ = 0; }
    Ref& operator=(Ref&& o) noexcept;
    void Reset(Il2CppObject* o);
    Il2CppObject* get() const { return p_; }
    explicit operator bool() const { return p_ != nullptr; }
    bool Alive() const;  // UnityEngine.Object still has a native object
    // Forget the object WITHOUT touching IL2CPP (the handle is leaked). Used
    // after a native fault, when no further runtime calls are made.
    void Abandon() { p_ = nullptr; h_ = nullptr; }

private:
    Il2CppObject* p_ = nullptr;
    Il2CppGCHandle h_ = nullptr;
};

// After a native fault: every Ref stops calling into IL2CPP (handles are
// leaked, Alive() is false). Irreversible for the session.
void FreezeRuntime();
bool RuntimeFrozen();

// Resolves everything. Returns false (with a reason) only if something that
// the minimap cannot work without is missing; optional parts are logged.
bool Resolve(std::string& err);
bool HasTmp();      // TextMeshProUGUI available
bool HasSpriteGen();// Texture2D + Sprite.Create available (procedural sprites)

// ---- extra il2cpp exports (resolved by export name) ------------------------
Il2CppObject* ObjectNew(Il2CppClass* k);
const MethodInfo* VirtualMethod(Il2CppObject* obj, const MethodInfo* ifaceOrVirtual);
void* MethodPtr(const MethodInfo* m);  // validated method pointer (nullptr if none)
Il2CppClass* NestedClass(Il2CppClass* outer, std::string_view name);
// Values of a System.Collections.Generic.Dictionary<K, V> with a reference-type
// V (fields resolved by name on the object's own generic instance; used
// entries have hashCode >= 0). The dictionary stays pinned while iterating.
template <class F>
void ForEachDictValue(Il2CppObject* dict, F&& fn);
struct DictLayout {
    size_t entries = 0, count = 0, stride = 0, hash = 0, value = 0;
    bool ok = false;
};
DictLayout DictLayoutOf(Il2CppObject* dict);  // cached per class
// Offset of `field` inside a value type's raw data (without object header).
bool StructFieldOffset(Il2CppClass* valueType, std::string_view field, size_t& out);

// ---- classes ---------------------------------------------------------------
Il2CppClass* SpriteClass();
Il2CppClass* RectTransformClass();
Il2CppClass* CanvasClass();
Il2CppClass* ImageClass();
Il2CppClass* GraphicClass();
Il2CppClass* TmpTextClass();
Il2CppClass* RawImageClass();
Il2CppClass* CanvasGroupClass();
Il2CppClass* TransformClass();
// obj's class is k or derives from / implements k; UnityEngine.Objects must
// also still have their native object.
bool IsA(Il2CppObject* obj, Il2CppClass* k);

// ---- GameObject / Transform -------------------------------------------------
Il2CppObject* NewGameObject(std::string_view name);  // new GameObject(name, typeof(RectTransform))
bool IsRectTransform(Il2CppObject* t);
int SceneHandle(Il2CppObject* go);  // GameObject.scene handle: 0 for prefab assets, -1 unknown
Il2CppObject* AddComponent(Il2CppObject* go, Il2CppClass* k);
std::vector<Il2CppObject*> ComponentsInChildren(Il2CppObject* go, Il2CppClass* k, bool includeInactive);
// Iterate while the returned managed array stays pinned (elements stay
// reachable for the GC even if fn allocates). fn returns false to stop.
template <class F>
void ForEachInChildren(Il2CppObject* go, Il2CppClass* k, bool includeInactive, F&& fn);
template <class F>
void ForEachOfType(Il2CppClass* k, F&& fn);  // Resources.FindObjectsOfTypeAll (assets included)
Il2CppArray* ComponentsInChildrenArray(Il2CppObject* go, Il2CppClass* k, bool includeInactive);
Il2CppArray* FindObjectsOfTypeAllArray(Il2CppClass* k);
size_t ArrayCount(Il2CppArray* a);
Il2CppObject* ArrayItem(Il2CppArray* a, size_t i);
Il2CppObject* ComponentInParent(Il2CppObject* go, Il2CppClass* k);
// Object.FindFirstObjectByType(Type, FindObjectsInactive): scene objects only.
Il2CppObject* FindFirst(Il2CppClass* k, bool includeInactive);
// Object.FindObjectsByType(Type, FindObjectsInactive, None): scene objects only, pinned while iterating.
template <class F>
void ForEachSceneObject(Il2CppClass* k, bool includeInactive, F&& fn);
Il2CppArray* FindObjectsByTypeArray(Il2CppClass* k, bool includeInactive);
// Reference-type field `field` of each element of a List<struct> (pinned while iterating).
template <class F>
void ForEachListStructRef(Il2CppObject* list, std::string_view field, F&& fn);
struct ListLayout {
    size_t items = 0, size = 0, stride = 0, field = 0;
    bool ok = false;
};
ListLayout ListStructLayout(Il2CppObject* list, std::string_view field);  // cached per (class, field)
// Cheap sanity check before dereferencing a pointer read from managed memory:
// canonical user-mode address, 8-byte aligned.
bool PlausiblePointer(const void* p);
Il2CppObject* GetParent(Il2CppObject* t);
int GetLayer(Il2CppObject* go);
void SetLayer(Il2CppObject* go, int layer);
bool ActiveSelf(Il2CppObject* go);
void Destroy(Il2CppObject* obj);
void DontUnload(Il2CppObject* obj);  // hideFlags |= DontUnloadUnusedAsset (runtime textures/sprites)

void SetParent(Il2CppObject* t, Il2CppObject* parent, bool worldPositionStays);
void SetAsLastSibling(Il2CppObject* t);
void SetLocalPosition(Il2CppObject* t, Vec3 v);
void SetLocalScale(Il2CppObject* t, Vec3 v);
void SetLocalEuler(Il2CppObject* t, Vec3 v);
Vec3 GetPosition(Il2CppObject* t);
Quat GetRotation(Il2CppObject* t);
Vec3 TransformPoint(Il2CppObject* t, Vec3 p);
Vec3 InverseTransformPoint(Il2CppObject* t, Vec3 p);

// ---- RectTransform -----------------------------------------------------------
void SetAnchors(Il2CppObject* rt, Vec2 min, Vec2 max);
void SetPivot(Il2CppObject* rt, Vec2 p);
void SetSizeDelta(Il2CppObject* rt, Vec2 s);
void SetAnchoredPosition(Il2CppObject* rt, Vec2 p);
Vec2 GetAnchorMin(Il2CppObject* rt);
Vec2 GetAnchorMax(Il2CppObject* rt);
Vec2 GetSizeDelta(Il2CppObject* rt);
Rect GetRect(Il2CppObject* rt);
// Stretch to fill the parent (anchors 0..1, zero offsets).
void Fill(Il2CppObject* rt);
// Centered fixed-size rect.
void Center(Il2CppObject* rt, Vec2 size);

// ---- uGUI ----------------------------------------------------------------------
void ImageSetSprite(Il2CppObject* img, Il2CppObject* sprite);
Il2CppObject* ImageGetSprite(Il2CppObject* img);
void ImageSetType(Il2CppObject* img, int type);  // 0 Simple, 1 Sliced
void ImageSetPreserveAspect(Il2CppObject* img, bool v);
void ImageSetPpuMultiplier(Il2CppObject* img, float v);
void GraphicSetColor(Il2CppObject* g, Color c);
Color GraphicGetColor(Il2CppObject* g);
int ImageGetType(Il2CppObject* img);
float ImageGetPpuMultiplier(Il2CppObject* img);
float CanvasGroupGetAlpha(Il2CppObject* cg);
Il2CppObject* RawImageGetTexture(Il2CppObject* ri);
Rect RawImageGetUvRect(Il2CppObject* ri);
void RawImageSetTexture(Il2CppObject* ri, Il2CppObject* tex);
void RawImageSetUvRect(Il2CppObject* ri, Rect r);
void GraphicSetRaycastTarget(Il2CppObject* g, bool v);
void MaskSetShowGraphic(Il2CppObject* mask, bool v);
Il2CppObject* AddMask(Il2CppObject* go);
Il2CppObject* AddCanvasGroup(Il2CppObject* go);
void CanvasGroupSetAlpha(Il2CppObject* cg, float a);
void CanvasGroupSetBlocksRaycasts(Il2CppObject* cg, bool v);
Il2CppObject* AddCanvas(Il2CppObject* go);  // nested canvas, isolates our rebuilds
Il2CppObject* CanvasRoot(Il2CppObject* canvas);
void CanvasSetOverlay(Il2CppObject* canvas, int sortingOrder);  // ScreenSpaceOverlay root canvas
bool BehaviourEnabled(Il2CppObject* b);
bool MaskShowsGraphic(Il2CppObject* mask);         // Mask.showMaskGraphic (true if unknown)
Il2CppClass* MaskClass();
// Materials (all optional; nullptr / false when unavailable)
Il2CppObject* GraphicGetMaterial(Il2CppObject* g);         // Graphic.material (Image override honoured)
Il2CppObject* GraphicGetDefaultMaterial(Il2CppObject* g);  // Graphic.defaultMaterial
void GraphicSetMaterial(Il2CppObject* g, Il2CppObject* mat);
bool MaterialHasProperty(Il2CppObject* mat, std::string_view name);
Il2CppObject* MaterialMainTexture(Il2CppObject* mat);
Rect MaterialMainUv(Il2CppObject* mat);  // (offset.x, offset.y, scale.x, scale.y); {0,0,1,1} if unknown
std::string MaterialShaderName(Il2CppObject* mat);
Il2CppObject* SpriteTexture(Il2CppObject* sprite);  // nullptr while a late-bound atlas is not loaded
bool HasSpriteTexture();                            // Sprite.get_texture resolved
Vec2 SpritePivot(Il2CppObject* sprite);             // pixels, {0,0} if unknown
Vec4 SpriteBorder(Il2CppObject* sprite);
Rect SpriteRect(Il2CppObject* sprite);

// ---- TextMeshPro (optional) ----------------------------------------------------
Il2CppObject* AddTmpText(Il2CppObject* go);
void TmpSetText(Il2CppObject* t, std::string_view utf8);
void TmpSetFontSize(Il2CppObject* t, float size);
void TmpSetCenter(Il2CppObject* t);
void TmpSetAlignment(Il2CppObject* t, int alignment);  // TMPro.TextAlignmentOptions (513 Left, 514 Center)
// Width the current text needs at the current font size (TMP_Text.preferredWidth); -1 if unavailable.
float TmpPreferredWidth(Il2CppObject* t);
void TmpSetColor(Il2CppObject* t, Color c);
void TmpCopyFont(Il2CppObject* dst, Il2CppObject* srcText);

// ---- procedural sprites -------------------------------------------------------
// RGBA8 pixels, row 0 = bottom. border = (left, bottom, right, top) in pixels.
// mipmaps: full mip chain + trilinear filtering (for sprites drawn smaller than their texture).
Il2CppObject* MakeSprite(const std::vector<uint32_t>& rgba, int w, int h, Vec4 border, std::string_view name,
                         bool mipmaps = false);
int TextureGetFilter(Il2CppObject* tex);            // FilterMode: 0 Point, 1 Bilinear, 2 Trilinear; -1 unknown
void TextureSetFilter(Il2CppObject* tex, int mode);

// ---- misc engine state ----------------------------------------------------------
int CursorLockState();  // 0 None, 1 Locked, 2 Confined, -1 unknown
float TimeScale();      // -1 unknown
int ScreenHeight();     // 0 unknown

// ---- fault capture -------------------------------------------------------------
// Runs fn under an SEH guard that records native faults (access violations
// etc.) with module + offset of the faulting instruction, e.g.
// "0xC0000005 at GameAssembly.dll+0x3459812". C++/IL2CPP managed exceptions
// are NOT caught here (they propagate to the caller's core::Try).
bool GuardNativeImpl(void (*thunk)(void*), void* ctx, std::string* where);
template <class F>
bool GuardNative(F&& fn, std::string* where) {
    using Fn = std::remove_reference_t<F>;
    return GuardNativeImpl([](void* p) { (*static_cast<Fn*>(p))(); }, static_cast<void*>(&fn), where);
}

// ---- template definitions ---------------------------------------------------
template <class F>
void ForEachInChildren(Il2CppObject* go, Il2CppClass* k, bool includeInactive, F&& fn) {
    Il2CppArray* arr = ComponentsInChildrenArray(go, k, includeInactive);
    Ref pin(reinterpret_cast<Il2CppObject*>(arr));
    for (size_t i = 0, n = ArrayCount(arr); i < n; ++i)
        if (Il2CppObject* e = ArrayItem(arr, i))
            if (!fn(e)) break;
}

template <class F>
void ForEachDictValue(Il2CppObject* dict, F&& fn) {
    if (!dict) return;
    DictLayout l = DictLayoutOf(dict);
    if (!l.ok) return;
    Ref pin(dict);
    char* obj = reinterpret_cast<char*>(dict);
    auto* entries = *reinterpret_cast<Il2CppArray**>(obj + l.entries);
    int count = *reinterpret_cast<int*>(obj + l.count);
    size_t cap = ArrayCount(entries);
    if (!entries || count <= 0) return;
    if (static_cast<size_t>(count) > cap) count = static_cast<int>(cap);
    // kIl2CppSizeOfArray: element data follows the 4-pointer array header
    char* data = reinterpret_cast<char*>(entries) + 4 * sizeof(void*);
    for (int i = 0; i < count; ++i) {
        char* e = data + static_cast<size_t>(i) * l.stride;
        if (*reinterpret_cast<int*>(e + l.hash) < 0) continue;  // free slot
        auto* v = *reinterpret_cast<Il2CppObject**>(e + l.value);
        if (!v) continue;
        if (!PlausiblePointer(v)) return;  // layout mismatch: stop rather than dereference garbage
        if (!fn(v)) break;
    }
}

template <class F>
void ForEachSceneObject(Il2CppClass* k, bool includeInactive, F&& fn) {
    Il2CppArray* arr = FindObjectsByTypeArray(k, includeInactive);
    Ref pin(reinterpret_cast<Il2CppObject*>(arr));
    for (size_t i = 0, n = ArrayCount(arr); i < n; ++i)
        if (Il2CppObject* e = ArrayItem(arr, i))
            if (!fn(e)) break;
}

template <class F>
void ForEachListStructRef(Il2CppObject* list, std::string_view field, F&& fn) {
    if (!list) return;
    ListLayout l = ListStructLayout(list, field);
    if (!l.ok) return;
    Ref pin(list);
    char* obj = reinterpret_cast<char*>(list);
    auto* items = *reinterpret_cast<Il2CppArray**>(obj + l.items);
    int count = *reinterpret_cast<int*>(obj + l.size);
    size_t cap = ArrayCount(items);
    if (!items || count <= 0) return;
    if (static_cast<size_t>(count) > cap) count = static_cast<int>(cap);
    char* data = reinterpret_cast<char*>(items) + 4 * sizeof(void*);
    for (int i = 0; i < count; ++i) {
        auto* v = *reinterpret_cast<Il2CppObject**>(data + static_cast<size_t>(i) * l.stride + l.field);
        if (!v) continue;
        if (!PlausiblePointer(v)) return;  // layout mismatch: stop rather than dereference garbage
        if (!fn(v)) break;
    }
}

// Two reference-type fields of each element of a List<struct>: fn(a, b) (either may be null).
template <class F>
void ForEachListStructPair(Il2CppObject* list, std::string_view fieldA, std::string_view fieldB, F&& fn) {
    if (!list) return;
    ListLayout la = ListStructLayout(list, fieldA), lb = ListStructLayout(list, fieldB);
    if (!la.ok || !lb.ok || la.stride != lb.stride) return;
    Ref pin(list);
    char* obj = reinterpret_cast<char*>(list);
    auto* items = *reinterpret_cast<Il2CppArray**>(obj + la.items);
    int count = *reinterpret_cast<int*>(obj + la.size);
    size_t cap = ArrayCount(items);
    if (!items || count <= 0) return;
    if (static_cast<size_t>(count) > cap) count = static_cast<int>(cap);
    char* data = reinterpret_cast<char*>(items) + 4 * sizeof(void*);
    for (int i = 0; i < count; ++i) {
        char* e = data + static_cast<size_t>(i) * la.stride;
        auto* a = *reinterpret_cast<Il2CppObject**>(e + la.field);
        auto* b = *reinterpret_cast<Il2CppObject**>(e + lb.field);
        if ((a && !PlausiblePointer(a)) || (b && !PlausiblePointer(b))) return;
        if (!fn(a, b)) break;
    }
}

template <class F>
void ForEachOfType(Il2CppClass* k, F&& fn) {
    Il2CppArray* arr = FindObjectsOfTypeAllArray(k);
    Ref pin(reinterpret_cast<Il2CppObject*>(arr));
    for (size_t i = 0, n = ArrayCount(arr); i < n; ++i)
        if (Il2CppObject* e = ArrayItem(arr, i))
            if (!fn(e)) break;
}

}  // namespace sr2::modules::minimap::ugui
