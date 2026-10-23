#include "modules/minimap/ugui.h"

#include <windows.h>

#include <atomic>
#include <cstring>
#include <format>
#include <map>
#include <unordered_map>
#include <utility>

#include "core/log.h"
#include "core/resolver.h"
#include "core/unity.h"

namespace sr2::modules::minimap::ugui {
namespace {

using il2cpp::api;
using il2cpp::Resolver;
namespace unity = sr2::core::unity;

struct Exports {
    Il2CppObject* (*object_new)(Il2CppClass*) = nullptr;
    Il2CppArray* (*array_new)(Il2CppClass*, uintptr_t) = nullptr;
    const MethodInfo* (*object_get_virtual_method)(Il2CppObject*, const MethodInfo*) = nullptr;
    Il2CppClass* (*class_get_nested_types)(Il2CppClass*, void**) = nullptr;
    int32_t (*class_instance_size)(Il2CppClass*) = nullptr;
    int32_t (*class_value_size)(Il2CppClass*, uint32_t*) = nullptr;
    const Il2CppType* (*field_get_type)(FieldInfo*) = nullptr;
    Il2CppClass* (*class_get_element_class)(Il2CppClass*) = nullptr;
    int (*class_array_element_size)(const Il2CppClass*) = nullptr;
    void (*gc_wbarrier_set_field)(Il2CppObject*, void**, void*) = nullptr;
};

struct State {
    Exports x;
    // classes
    Il2CppClass *object = nullptr, *gameObject = nullptr, *transform = nullptr, *rectTransform = nullptr;
    Il2CppClass *behaviour = nullptr, *texture2d = nullptr, *sprite = nullptr, *color32 = nullptr;
    Il2CppClass *canvas = nullptr, *canvasGroup = nullptr;
    Il2CppClass *image = nullptr, *graphic = nullptr, *mask = nullptr;
    Il2CppClass *tmpUgui = nullptr, *tmpText = nullptr;
    // GameObject / Object
    Fn findAll, goCtorTypes, goGetScene, findFirst, findByType, getParent;
    Fn gfxGetColor, imgGetType, imgGetPpuMul, cgGetAlpha, riGetTex, riGetUv, riSetTex, riSetUv;
    Il2CppClass* rawImage = nullptr;
    Il2CppClass* systemType = nullptr;
    Fn goCtor, addComponent, compsInChildren, compInParent, getLayer, setLayer, activeSelf, destroy, setHideFlags,
        getHideFlags;
    // Transform
    Fn setParent, setAsLastSibling, setLocalPosition, setLocalScale, setLocalEuler, getPosition, getRotation,
        transformPoint, inverseTransformPoint, getL2W, getW2L;
    Fn shaderPropId, shaderSetGlobalMatrix, canvasGetRenderMode;
    // RectTransform
    Fn setAnchorMin, setAnchorMax, setPivot, setSizeDelta, setAnchoredPosition, getAnchorMin, getAnchorMax,
        getSizeDelta, getRect;
    // uGUI
    Fn imgSetSprite, imgGetSprite, imgSetType, imgSetPreserveAspect, imgSetPpuMul, gfxSetColor, gfxSetRaycast,
        maskShowGraphic;
    Fn cgSetAlpha, cgSetBlocks, cgSetInteractable, cgSetIgnoreParent;
    Fn canvasRoot, canvasSetRenderMode, canvasSetSorting, canvasSetOverrideSorting;
    Fn behEnabled, maskGetShow;
    Il2CppClass* material = nullptr;
    Fn gfxGetMaterial, gfxGetDefaultMaterial, gfxSetMaterial, matHasProp, matMainTex, matMainOffset, matMainScale,
        matGetShader, spriteGetTexture, spriteGetPivot;
    Fn spriteBorder, spriteRect;
    // textures
    Fn texCtor, texSetPixels32, texApply, texSetFilter, texGetFilter, texSetWrap, spriteCreate;
    // TMP
    Fn tmpSetText, tmpSetFontSize, tmpSetAlignment, tmpSetColor, tmpGetFont, tmpSetFont, tmpSetWrap, tmpPreferredWidth;
    // misc
    Fn cursorLockState, timeScale, screenHeight;
    bool resolved = false;
};

State S;

template <class F>
F Export(const char* name) {
    HMODULE m = api().module;
    return m ? reinterpret_cast<F>(GetProcAddress(m, name)) : nullptr;
}

Fn Bind(const MethodInfo* mi, const std::string& what, std::vector<std::string>* missing) {
    Fn f;
    std::string err;
    if (mi) {
        f.p = Resolver::Get().MethodPointer(mi, &err);
        if (f.p) f.mi = mi;
    }
    if (!f.p && missing) missing->push_back(what + (err.empty() ? "" : " (" + err + ")"));
    return f;
}

std::string ClassLabel(Il2CppClass* k) {
    if (!k) return "?";
    const char* n = api().il2cpp_class_get_name ? api().il2cpp_class_get_name(k) : nullptr;
    return n ? n : "?";
}

// name + exact parameter type list
Fn P(Il2CppClass* k, const char* name, std::initializer_list<std::string_view> params,
     std::vector<std::string>* missing) {
    const MethodInfo* mi = k ? Resolver::Get().MethodByParams(k, name, params) : nullptr;
    return Bind(mi, ClassLabel(k) + "." + name, missing);
}
// name + argc (only for names that are not overloaded at that argc)
Fn A(Il2CppClass* k, const char* name, int argc, std::vector<std::string>* missing) {
    const MethodInfo* mi = k ? Resolver::Get().Method(k, name, argc) : nullptr;
    // Method() does not walk parents; do it here so inherited setters resolve.
    for (Il2CppClass* c = k; !mi && c && api().il2cpp_class_get_parent;) {
        c = api().il2cpp_class_get_parent(c);
        if (c) mi = Resolver::Get().Method(c, name, argc);
    }
    return Bind(mi, ClassLabel(k) + "." + name, missing);
}

bool Alive(Il2CppObject* o) { return o && unity::IsAlive(o); }

}  // namespace

// ---------------------------------------------------------------- Ref --------
Ref& Ref::operator=(Ref&& o) noexcept {
    if (this != &o) {
        Reset(nullptr);
        p_ = o.p_;
        h_ = o.h_;
        o.p_ = nullptr;
        o.h_ = 0;
    }
    return *this;
}

namespace {
std::atomic<bool> g_frozen{false};
}

void FreezeRuntime() { g_frozen.store(true); }
bool RuntimeFrozen() { return g_frozen.load(); }

void Ref::Reset(Il2CppObject* o) {
    bool frozen = g_frozen.load();
    if (h_ && !frozen) unity::FreeGCHandle(h_);  // after a fault: leak, never call into IL2CPP
    h_ = nullptr;
    p_ = frozen ? nullptr : o;
    if (p_) {
        h_ = unity::NewGCHandle(p_, false);
        if (!h_) p_ = nullptr;  // never keep an unpinned pointer (e.g. GC-handle self-test failed)
    }
}

bool Ref::Alive() const { return p_ && !g_frozen.load() && unity::IsAlive(p_); }

// ------------------------------------------------------------- Resolve --------
bool Resolve(std::string& err) {
    if (S.resolved) return true;
    Resolver& r = Resolver::Get();
    State& s = S;
    std::vector<std::string> req, opt;

    s.x.object_new = Export<decltype(s.x.object_new)>("il2cpp_object_new");
    s.x.array_new = Export<decltype(s.x.array_new)>("il2cpp_array_new");
    s.x.object_get_virtual_method =
        Export<decltype(s.x.object_get_virtual_method)>("il2cpp_object_get_virtual_method");
    s.x.class_get_nested_types = Export<decltype(s.x.class_get_nested_types)>("il2cpp_class_get_nested_types");
    s.x.class_instance_size = Export<decltype(s.x.class_instance_size)>("il2cpp_class_instance_size");
    s.x.class_value_size = Export<decltype(s.x.class_value_size)>("il2cpp_class_value_size");
    s.x.field_get_type = Export<decltype(s.x.field_get_type)>("il2cpp_field_get_type");
    s.x.class_get_element_class = Export<decltype(s.x.class_get_element_class)>("il2cpp_class_get_element_class");
    s.x.class_array_element_size = Export<decltype(s.x.class_array_element_size)>("il2cpp_class_array_element_size");
    s.x.gc_wbarrier_set_field = Export<decltype(s.x.gc_wbarrier_set_field)>("il2cpp_gc_wbarrier_set_field");
    if (!s.x.object_new) req.push_back("export il2cpp_object_new");
    if (!api().il2cpp_gchandle_new || !api().il2cpp_gchandle_free) req.push_back("export il2cpp_gchandle_new/free");
    if (!api().il2cpp_class_get_type || !api().il2cpp_type_get_object) req.push_back("export il2cpp_class_get_type");

    const char* core = "UnityEngine.CoreModule";
    const char* uim = "UnityEngine.UIModule";
    const char* ui = "UnityEngine.UI";
    auto cls = [&](const char* img, const char* ns, const char* name, std::vector<std::string>& miss) {
        std::string e;
        Il2CppClass* k = r.Class(img, ns, name, &e);
        if (!k) miss.push_back(e);
        return k;
    };
    s.object = cls(core, "UnityEngine", "Object", req);
    s.gameObject = cls(core, "UnityEngine", "GameObject", req);
    s.transform = cls(core, "UnityEngine", "Transform", req);
    s.rectTransform = cls(core, "UnityEngine", "RectTransform", req);
    s.behaviour = cls(core, "UnityEngine", "Behaviour", opt);
    s.sprite = cls(core, "UnityEngine", "Sprite", req);
    s.texture2d = cls(core, "UnityEngine", "Texture2D", opt);
    s.color32 = cls(core, "UnityEngine", "Color32", opt);
    Il2CppClass* texture = cls(core, "UnityEngine", "Texture", opt);
    Il2CppClass* cursor = cls(core, "UnityEngine", "Cursor", opt);
    Il2CppClass* time = cls(core, "UnityEngine", "Time", opt);
    Il2CppClass* screen = cls(core, "UnityEngine", "Screen", opt);
    Il2CppClass* resources = cls(core, "UnityEngine", "Resources", req);
    s.canvas = cls(uim, "UnityEngine", "Canvas", req);
    s.canvasGroup = cls(uim, "UnityEngine", "CanvasGroup", req);
    s.image = cls(ui, "UnityEngine.UI", "Image", req);
    s.graphic = cls(ui, "UnityEngine.UI", "Graphic", req);
    s.mask = cls(ui, "UnityEngine.UI", "Mask", req);
    s.tmpUgui = cls("Unity.TextMeshPro", "TMPro", "TextMeshProUGUI", opt);
    s.tmpText = cls("Unity.TextMeshPro", "TMPro", "TMP_Text", opt);

    const char* V2 = "UnityEngine.Vector2";
    const char* V3 = "UnityEngine.Vector3";
    s.findAll = P(resources, "FindObjectsOfTypeAll", {"System.Type"}, &req);
    // GameObject / Object
    s.goCtor = P(s.gameObject, ".ctor", {"System.String"}, &req);
    // new GameObject(name, typeof(RectTransform)) - the canonical way to get a
    // RectTransform; AddComponent(RectTransform) on an existing Transform is
    // not reliable (can return null and leave a plain Transform).
    s.goCtorTypes = P(s.gameObject, ".ctor", {"System.String", "System.Type[]"}, &opt);
    s.systemType = r.Class(il2cpp::api().il2cpp_get_corlib(), "System", "Type");
    if (!s.systemType) opt.push_back("System.Type class");
    s.goGetScene = A(s.gameObject, "get_scene", 0, &opt);
    s.findFirst = P(s.object, "FindFirstObjectByType", {"System.Type", "UnityEngine.FindObjectsInactive"}, &req);
    s.findByType = P(s.object, "FindObjectsByType",
                     {"System.Type", "UnityEngine.FindObjectsInactive", "UnityEngine.FindObjectsSortMode"}, &opt);
    s.getParent = A(s.transform, "get_parent", 0, &req);
    s.rawImage = cls(ui, "UnityEngine.UI", "RawImage", opt);
    s.gfxGetColor = A(s.graphic, "get_color", 0, &req);
    s.imgGetType = A(s.image, "get_type", 0, &opt);
    s.imgGetPpuMul = A(s.image, "get_pixelsPerUnitMultiplier", 0, &opt);
    s.cgGetAlpha = A(s.canvasGroup, "get_alpha", 0, &opt);
    if (s.rawImage) {
        s.riGetTex = A(s.rawImage, "get_texture", 0, &opt);
        s.riGetUv = A(s.rawImage, "get_uvRect", 0, &opt);
        s.riSetTex = P(s.rawImage, "set_texture", {"UnityEngine.Texture"}, &opt);
        s.riSetUv = P(s.rawImage, "set_uvRect", {"UnityEngine.Rect"}, &opt);
    }
    s.addComponent = P(s.gameObject, "AddComponent", {"System.Type"}, &req);
    s.compsInChildren = P(s.gameObject, "GetComponentsInChildren", {"System.Type", "System.Boolean"}, &req);
    s.compInParent = P(s.gameObject, "GetComponentInParent", {"System.Type", "System.Boolean"}, &opt);
    s.getLayer = A(s.gameObject, "get_layer", 0, &opt);
    s.setLayer = A(s.gameObject, "set_layer", 1, &opt);
    s.activeSelf = A(s.gameObject, "get_activeSelf", 0, &opt);
    s.destroy = P(s.object, "Destroy", {"UnityEngine.Object"}, &req);
    s.setHideFlags = A(s.object, "set_hideFlags", 1, &opt);
    s.getHideFlags = A(s.object, "get_hideFlags", 0, &opt);
    // Transform
    s.setParent = P(s.transform, "SetParent", {"UnityEngine.Transform", "System.Boolean"}, &req);
    s.setAsLastSibling = A(s.transform, "SetAsLastSibling", 0, &opt);
    s.setLocalPosition = P(s.transform, "set_localPosition", {V3}, &req);
    s.setLocalScale = P(s.transform, "set_localScale", {V3}, &req);
    s.setLocalEuler = P(s.transform, "set_localEulerAngles", {V3}, &req);
    s.getPosition = A(s.transform, "get_position", 0, &req);
    s.getRotation = A(s.transform, "get_rotation", 0, &req);
    s.transformPoint = P(s.transform, "TransformPoint", {V3}, &opt);
    s.inverseTransformPoint = P(s.transform, "InverseTransformPoint", {V3}, &opt);
    s.getL2W = A(s.transform, "get_localToWorldMatrix", 0, &opt);
    s.getW2L = A(s.transform, "get_worldToLocalMatrix", 0, &opt);
    if (Il2CppClass* shader = cls(core, "UnityEngine", "Shader", opt)) {
        s.shaderPropId = P(shader, "PropertyToID", {"System.String"}, &opt);
        s.shaderSetGlobalMatrix = P(shader, "SetGlobalMatrix", {"System.Int32", "UnityEngine.Matrix4x4"}, &opt);
    }
    // RectTransform
    s.setAnchorMin = P(s.rectTransform, "set_anchorMin", {V2}, &req);
    s.setAnchorMax = P(s.rectTransform, "set_anchorMax", {V2}, &req);
    s.setPivot = P(s.rectTransform, "set_pivot", {V2}, &req);
    s.setSizeDelta = P(s.rectTransform, "set_sizeDelta", {V2}, &req);
    s.setAnchoredPosition = P(s.rectTransform, "set_anchoredPosition", {V2}, &req);
    s.getAnchorMin = A(s.rectTransform, "get_anchorMin", 0, &opt);
    s.getAnchorMax = A(s.rectTransform, "get_anchorMax", 0, &opt);
    s.getSizeDelta = A(s.rectTransform, "get_sizeDelta", 0, &opt);
    s.getRect = A(s.rectTransform, "get_rect", 0, &req);
    // uGUI
    s.imgSetSprite = P(s.image, "set_sprite", {"UnityEngine.Sprite"}, &req);
    s.imgGetSprite = A(s.image, "get_sprite", 0, &opt);
    s.imgSetType = A(s.image, "set_type", 1, &opt);
    s.imgSetPreserveAspect = A(s.image, "set_preserveAspect", 1, &opt);
    s.imgSetPpuMul = A(s.image, "set_pixelsPerUnitMultiplier", 1, &opt);
    s.gfxSetColor = P(s.graphic, "set_color", {"UnityEngine.Color"}, &req);
    s.gfxSetRaycast = A(s.graphic, "set_raycastTarget", 1, &opt);
    s.maskShowGraphic = A(s.mask, "set_showMaskGraphic", 1, &opt);
    s.cgSetAlpha = A(s.canvasGroup, "set_alpha", 1, &req);
    s.cgSetBlocks = A(s.canvasGroup, "set_blocksRaycasts", 1, &opt);
    s.cgSetInteractable = A(s.canvasGroup, "set_interactable", 1, &opt);
    s.cgSetIgnoreParent = A(s.canvasGroup, "set_ignoreParentGroups", 1, &opt);
    s.canvasRoot = A(s.canvas, "get_rootCanvas", 0, &opt);
    s.canvasGetRenderMode = A(s.canvas, "get_renderMode", 0, &opt);
    s.canvasSetRenderMode = A(s.canvas, "set_renderMode", 1, &opt);
    s.canvasSetSorting = A(s.canvas, "set_sortingOrder", 1, &opt);
    s.canvasSetOverrideSorting = A(s.canvas, "set_overrideSorting", 1, &opt);
    s.behEnabled = A(s.behaviour, "get_enabled", 0, &opt);
    s.maskGetShow = A(s.mask, "get_showMaskGraphic", 0, &opt);
    s.material = cls(core, "UnityEngine", "Material", opt);
    s.gfxGetMaterial = A(s.image, "get_material", 0, &opt);  // Image overrides Graphic.material
    s.gfxGetDefaultMaterial = A(s.graphic, "get_defaultMaterial", 0, &opt);
    s.gfxSetMaterial = A(s.graphic, "set_material", 1, &opt);
    if (s.material) {
        s.matHasProp = P(s.material, "HasProperty", {"System.String"}, &opt);
        s.matMainTex = A(s.material, "get_mainTexture", 0, &opt);
        s.matMainOffset = A(s.material, "get_mainTextureOffset", 0, &opt);
        s.matMainScale = A(s.material, "get_mainTextureScale", 0, &opt);
        s.matGetShader = A(s.material, "get_shader", 0, &opt);
    }
    s.spriteGetTexture = A(s.sprite, "get_texture", 0, &opt);
    s.spriteGetPivot = A(s.sprite, "get_pivot", 0, &opt);
    s.spriteBorder = A(s.sprite, "get_border", 0, &opt);
    s.spriteRect = A(s.sprite, "get_rect", 0, &opt);
    // Textures / sprites
    s.texCtor = P(s.texture2d, ".ctor", {"System.Int32", "System.Int32", "UnityEngine.TextureFormat", "System.Boolean"},
                  &opt);
    s.texSetPixels32 = P(s.texture2d, "SetPixels32", {"UnityEngine.Color32[]"}, &opt);
    s.texApply = P(s.texture2d, "Apply", {"System.Boolean", "System.Boolean"}, &opt);
    s.texSetFilter = A(texture, "set_filterMode", 1, &opt);
    s.texGetFilter = A(texture, "get_filterMode", 0, &opt);
    s.texSetWrap = A(texture, "set_wrapMode", 1, &opt);
    s.spriteCreate = P(s.sprite, "Create",
                       {"UnityEngine.Texture2D", "UnityEngine.Rect", V2, "System.Single", "System.UInt32",
                        "UnityEngine.SpriteMeshType", "UnityEngine.Vector4"},
                       &opt);
    if (!s.x.array_new) opt.push_back("export il2cpp_array_new");
    // TMP
    if (s.tmpText) {
        s.tmpSetText = P(s.tmpText, "set_text", {"System.String"}, &opt);
        s.tmpSetFontSize = A(s.tmpText, "set_fontSize", 1, &opt);
        s.tmpSetAlignment = A(s.tmpText, "set_alignment", 1, &opt);
        s.tmpSetColor = P(s.tmpText, "set_color", {"UnityEngine.Color"}, &opt);
        s.tmpGetFont = A(s.tmpText, "get_font", 0, &opt);
        s.tmpSetFont = A(s.tmpText, "set_font", 1, &opt);
        s.tmpSetWrap = A(s.tmpText, "set_enableWordWrapping", 1, nullptr);
        s.tmpPreferredWidth = A(s.tmpText, "get_preferredWidth", 0, nullptr);
    }
    // Misc
    s.cursorLockState = A(cursor, "get_lockState", 0, &opt);
    s.timeScale = A(time, "get_timeScale", 0, &opt);
    s.screenHeight = A(screen, "get_height", 0, &opt);

    for (const auto& m : opt) SR2_INFO("minimap: optional Unity symbol missing: {}", m);
    if (!req.empty()) {
        err = "required Unity symbol(s) missing: ";
        for (size_t i = 0; i < req.size(); ++i) err += (i ? "; " : "") + req[i];
        return false;
    }
    s.resolved = true;
    return true;
}

bool HasTmp() { return S.tmpUgui && S.tmpSetText; }
bool HasSpriteGen() {
    return S.texture2d && S.color32 && S.x.array_new && S.texCtor && S.texSetPixels32 && S.texApply && S.spriteCreate;
}

// ---------------------------------------------------------------- exports -----
Il2CppObject* ObjectNew(Il2CppClass* k) { return (k && S.x.object_new) ? S.x.object_new(k) : nullptr; }

const MethodInfo* VirtualMethod(Il2CppObject* obj, const MethodInfo* m) {
    if (!obj || !m || !S.x.object_get_virtual_method) return nullptr;
    return S.x.object_get_virtual_method(obj, m);
}

void* MethodPtr(const MethodInfo* m) { return m ? Resolver::Get().MethodPointer(m) : nullptr; }

Fn BindFn(const MethodInfo* mi) {
    Fn f;
    if (void* p = MethodPtr(mi)) {
        f.p = p;
        f.mi = mi;
    }
    return f;
}

Il2CppClass* NestedClass(Il2CppClass* outer, std::string_view name) {
    if (!outer || !S.x.class_get_nested_types || !api().il2cpp_class_get_name) return nullptr;
    void* iter = nullptr;
    while (Il2CppClass* n = S.x.class_get_nested_types(outer, &iter)) {
        const char* nn = api().il2cpp_class_get_name(n);
        if (nn && name == nn) return n;
    }
    return nullptr;
}

bool StructFieldOffset(Il2CppClass* valueType, std::string_view field, size_t& out) {
    if (!valueType || !S.x.class_instance_size || !S.x.class_value_size) return false;
    auto off = Resolver::Get().FieldOffset(valueType, field);
    if (!off) return false;
    // il2cpp_field_get_offset reports value-type fields as if boxed (object
    // header included). The header size is instance_size - value_size.
    uint32_t align = 0;
    int32_t inst = S.x.class_instance_size(valueType);
    int32_t val = S.x.class_value_size(valueType, &align);
    if (inst <= 0 || val <= 0 || inst < val) return false;
    size_t header = static_cast<size_t>(inst - val);
    if (*off < header) return false;
    out = *off - header;
    return true;
}

namespace {
// Element stride + layout check for an array-of-struct field (Dictionary._entries,
// List<T>._items). NOTE: il2cpp_class_array_element_size takes the ELEMENT
// class (like mono_class_array_element_size); passed the array class it
// returns sizeof(void*), which made the 2026-10-22 build walk 24-byte entries
// with an 8-byte stride and read hashCode|next as a pointer.
bool ElementLayout(FieldInfo* arrayField, Il2CppClass*& elem, size_t& stride) {
    elem = nullptr;
    stride = 0;
    if (!arrayField || !S.x.field_get_type || !api().il2cpp_class_from_type || !S.x.class_get_element_class ||
        !S.x.class_array_element_size || !S.x.class_value_size)
        return false;
    const Il2CppType* t = S.x.field_get_type(arrayField);
    Il2CppClass* arr = t ? api().il2cpp_class_from_type(t) : nullptr;
    elem = arr ? S.x.class_get_element_class(arr) : nullptr;
    if (!elem) return false;
    int es = S.x.class_array_element_size(elem);
    uint32_t align = 0;
    int vs = S.x.class_value_size(elem, &align);
    if (es <= 0 || vs <= 0 || es != vs || es > 4096) return false;  // must be an inline struct element
    stride = static_cast<size_t>(es);
    return true;
}
}  // namespace

DictLayout DictLayoutOf(Il2CppObject* dict) {
    static std::unordered_map<Il2CppClass*, DictLayout> cache;  // main thread only
    DictLayout l;
    Il2CppClass* k = dict && api().il2cpp_object_get_class ? api().il2cpp_object_get_class(dict) : nullptr;
    if (!k) return l;
    if (auto it = cache.find(k); it != cache.end()) return it->second;
    Resolver& r = Resolver::Get();
    FieldInfo* fe = r.Field(k, "_entries");
    auto ce = r.FieldOffset(k, "_entries");
    auto cc = r.FieldOffset(k, "_count");
    Il2CppClass* elem = nullptr;
    size_t stride = 0, hash = 0, value = 0;
    if (fe && ce && cc && ElementLayout(fe, elem, stride) && StructFieldOffset(elem, "hashCode", hash) &&
        StructFieldOffset(elem, "value", value) && hash + sizeof(int) <= stride && value + sizeof(void*) <= stride &&
        value != hash && value % sizeof(void*) == 0) {
        l = {*ce, *cc, stride, hash, value, true};
        SR2_INFO("minimap: Dictionary layout {}: entry stride {}, hashCode +{}, value +{}", ClassLabel(k), stride, hash,
                 value);
    }
    if (!l.ok) SR2_WARN("minimap: Dictionary layout not resolvable on {} (walk skipped)", ClassLabel(k));
    cache.emplace(k, l);
    return l;
}

Il2CppClass* SpriteClass() { return S.sprite; }
Il2CppClass* RawImageClass() { return S.rawImage; }
Il2CppClass* CanvasGroupClass() { return S.canvasGroup; }
Il2CppClass* TransformClass() { return S.transform; }

bool IsA(Il2CppObject* obj, Il2CppClass* k) {
    if (!obj || !k || !api().il2cpp_object_get_class) return false;
    Il2CppClass* c = api().il2cpp_object_get_class(obj);
    if (!c) return false;
    auto assignable = [](Il2CppClass* to, Il2CppClass* from) {
        return to == from || (api().il2cpp_class_is_assignable_from && api().il2cpp_class_is_assignable_from(to, from));
    };
    if (!assignable(k, c)) return false;
    // UnityEngine.Object-derived: must still have its native object
    return !(S.object && assignable(S.object, c)) || unity::IsAlive(obj);
}

Il2CppObject* FindFirst(Il2CppClass* k, bool includeInactive) {
    if (!k || !S.findFirst) return nullptr;
    Il2CppObject* type = unity::TypeOf(k);
    if (!type) return nullptr;
    Il2CppObject* o = Call<Il2CppObject*>(S.findFirst, type, includeInactive ? 1 : 0);
    return IsA(o, k) ? o : nullptr;
}

Il2CppArray* FindObjectsByTypeArray(Il2CppClass* k, bool includeInactive) {
    if (!k || !S.findByType) return nullptr;
    Il2CppObject* type = unity::TypeOf(k);
    return type ? Call<Il2CppArray*>(S.findByType, type, includeInactive ? 1 : 0, 0 /* None */) : nullptr;
}

ListLayout ListStructLayout(Il2CppObject* list, std::string_view field) {
    static std::map<std::pair<Il2CppClass*, std::string>, ListLayout> cache;  // main thread only
    ListLayout l;
    Il2CppClass* k = list && api().il2cpp_object_get_class ? api().il2cpp_object_get_class(list) : nullptr;
    if (!k) return l;
    auto key = std::make_pair(k, std::string(field));
    if (auto it = cache.find(key); it != cache.end()) return it->second;
    Resolver& r = Resolver::Get();
    FieldInfo* fi = r.Field(k, "_items");
    auto ci = r.FieldOffset(k, "_items");
    auto cs = r.FieldOffset(k, "_size");
    Il2CppClass* elem = nullptr;
    size_t stride = 0, off = 0;
    if (fi && ci && cs && ElementLayout(fi, elem, stride) && StructFieldOffset(elem, field, off) &&
        off + sizeof(void*) <= stride && off % sizeof(void*) == 0)
        l = {*ci, *cs, stride, off, true};
    if (!l.ok) SR2_WARN("minimap: List<struct> layout not resolvable on {} ({})", ClassLabel(k), field);
    cache.emplace(std::move(key), l);
    return l;
}

bool PlausiblePointer(const void* p) {
    auto v = reinterpret_cast<uintptr_t>(p);
    return v >= 0x10000 && v < 0x00007FFFFFFF0000ull && (v & (sizeof(void*) - 1)) == 0;
}

Il2CppObject* GetParent(Il2CppObject* t) {
    if (!Alive(t) || !S.getParent) return nullptr;
    Il2CppObject* p = Call<Il2CppObject*>(S.getParent, t);
    return Alive(p) ? p : nullptr;
}

Color GraphicGetColor(Il2CppObject* g) { return Alive(g) ? Call<Color>(S.gfxGetColor, g) : Color{1, 1, 1, 1}; }
int ImageGetType(Il2CppObject* img) { return (Alive(img) && S.imgGetType) ? Call<int>(S.imgGetType, img) : 0; }
float ImageGetPpuMultiplier(Il2CppObject* img) {
    return (Alive(img) && S.imgGetPpuMul) ? Call<float>(S.imgGetPpuMul, img) : 1.0f;
}
float CanvasGroupGetAlpha(Il2CppObject* cg) {
    return (Alive(cg) && S.cgGetAlpha) ? Call<float>(S.cgGetAlpha, cg) : 1.0f;
}
Il2CppObject* RawImageGetTexture(Il2CppObject* ri) {
    return (Alive(ri) && S.riGetTex) ? Call<Il2CppObject*>(S.riGetTex, ri) : nullptr;
}
Rect RawImageGetUvRect(Il2CppObject* ri) {
    return (Alive(ri) && S.riGetUv) ? Call<Rect>(S.riGetUv, ri) : Rect{0, 0, 1, 1};
}
void RawImageSetTexture(Il2CppObject* ri, Il2CppObject* tex) {
    if (Alive(ri) && S.riSetTex) Call<void>(S.riSetTex, ri, Alive(tex) ? tex : nullptr);
}
void RawImageSetUvRect(Il2CppObject* ri, Rect r) {
    if (Alive(ri) && S.riSetUv) Call<void>(S.riSetUv, ri, r);
}
Il2CppClass* RectTransformClass() { return S.rectTransform; }
Il2CppClass* CanvasClass() { return S.canvas; }
Il2CppClass* ImageClass() { return S.image; }
Il2CppClass* GraphicClass() { return S.graphic; }
Il2CppClass* TmpTextClass() { return S.tmpText; }

// ------------------------------------------------------- GameObject / Transform
bool IsRectTransform(Il2CppObject* t) {
    if (!t || !S.rectTransform || !api().il2cpp_object_get_class) return false;
    Il2CppClass* k = api().il2cpp_object_get_class(t);
    if (k == S.rectTransform) return true;
    return api().il2cpp_class_is_assignable_from && api().il2cpp_class_is_assignable_from(S.rectTransform, k);
}

Il2CppObject* NewGameObject(std::string_view name) {
    if (!S.resolved) return nullptr;
    Il2CppObject* go = ObjectNew(S.gameObject);
    Il2CppString* n = unity::NewString(name);
    if (!go || !n) return nullptr;
    Il2CppArray* types = (S.goCtorTypes && S.systemType && S.x.array_new) ? S.x.array_new(S.systemType, 1) : nullptr;
    Il2CppObject* rtType = types ? unity::TypeOf(S.rectTransform) : nullptr;
    if (types && rtType) {
        void** slot = unity::ArrayData<void*>(types);
        if (S.x.gc_wbarrier_set_field) S.x.gc_wbarrier_set_field(reinterpret_cast<Il2CppObject*>(types), slot, rtType);
        else *slot = rtType;
        Call<void>(S.goCtorTypes, go, n, types);
    } else {
        Call<void>(S.goCtor, go, n);
    }
    if (!Alive(go)) return nullptr;
    if (!IsRectTransform(unity::TransformOf(go))) {
        AddComponent(go, S.rectTransform);
        if (!IsRectTransform(unity::TransformOf(go))) {
            SR2_ERROR("minimap: could not give '{}' a RectTransform", name);
            Destroy(go);
            return nullptr;
        }
    }
    return go;
}

int SceneHandle(Il2CppObject* go) {
    struct Scene {
        int handle;
    };
    if (!Alive(go) || !S.goGetScene) return -1;
    return Call<Scene>(S.goGetScene, go).handle;
}

Il2CppObject* AddComponent(Il2CppObject* go, Il2CppClass* k) {
    if (!Alive(go) || !k || !S.addComponent) return nullptr;
    Il2CppObject* type = unity::TypeOf(k);
    if (!type) return nullptr;
    Il2CppObject* c = Call<Il2CppObject*>(S.addComponent, go, type);
    return Alive(c) ? c : nullptr;
}

Il2CppArray* ComponentsInChildrenArray(Il2CppObject* go, Il2CppClass* k, bool includeInactive) {
    if (!Alive(go) || !k || !S.compsInChildren) return nullptr;
    Il2CppObject* type = unity::TypeOf(k);
    return type ? Call<Il2CppArray*>(S.compsInChildren, go, type, includeInactive) : nullptr;
}

Il2CppArray* FindObjectsOfTypeAllArray(Il2CppClass* k) {
    if (!k || !S.findAll) return nullptr;
    Il2CppObject* type = unity::TypeOf(k);
    return type ? Call<Il2CppArray*>(S.findAll, type) : nullptr;
}

size_t ArrayCount(Il2CppArray* a) { return unity::ArrayLength(a); }

Il2CppObject* ArrayItem(Il2CppArray* a, size_t i) {
    auto* e = static_cast<Il2CppObject*>(unity::ArrayAt(a, i));
    return Alive(e) ? e : nullptr;
}

// Only for MonoBehaviour-derived results (their managed wrappers are held
// strongly by the engine); otherwise use ForEachInChildren.
std::vector<Il2CppObject*> ComponentsInChildren(Il2CppObject* go, Il2CppClass* k, bool includeInactive) {
    std::vector<Il2CppObject*> out;
    ForEachInChildren(go, k, includeInactive, [&](Il2CppObject* e) {
        out.push_back(e);
        return true;
    });
    return out;
}

Il2CppObject* ComponentInParent(Il2CppObject* go, Il2CppClass* k) {
    if (!Alive(go) || !k || !S.compInParent) return nullptr;
    Il2CppObject* type = unity::TypeOf(k);
    if (!type) return nullptr;
    Il2CppObject* c = Call<Il2CppObject*>(S.compInParent, go, type, true);
    return Alive(c) ? c : nullptr;
}

int GetLayer(Il2CppObject* go) { return (Alive(go) && S.getLayer) ? Call<int>(S.getLayer, go) : -1; }
void SetLayer(Il2CppObject* go, int layer) {
    if (Alive(go) && S.setLayer && layer >= 0) Call<void>(S.setLayer, go, layer);
}
bool ActiveSelf(Il2CppObject* go) { return (Alive(go) && S.activeSelf) ? Call<bool>(S.activeSelf, go) : false; }

void Destroy(Il2CppObject* obj) {
    if (Alive(obj) && S.destroy) Call<void>(S.destroy, obj);
}

void DontUnload(Il2CppObject* obj) {
    if (!Alive(obj) || !S.setHideFlags || !S.getHideFlags) return;
    int flags = Call<int>(S.getHideFlags, obj);
    Call<void>(S.setHideFlags, obj, flags | 32 /* HideFlags.DontUnloadUnusedAsset */);
}

void SetParent(Il2CppObject* t, Il2CppObject* parent, bool worldPositionStays) {
    if (Alive(t)) Call<void>(S.setParent, t, Alive(parent) ? parent : nullptr, worldPositionStays);
}
void SetAsLastSibling(Il2CppObject* t) {
    if (Alive(t) && S.setAsLastSibling) Call<void>(S.setAsLastSibling, t);
}
void SetLocalPosition(Il2CppObject* t, Vec3 v) {
    if (Alive(t)) Call<void>(S.setLocalPosition, t, v);
}
void SetLocalScale(Il2CppObject* t, Vec3 v) {
    if (Alive(t)) Call<void>(S.setLocalScale, t, v);
}
void SetLocalEuler(Il2CppObject* t, Vec3 v) {
    if (Alive(t)) Call<void>(S.setLocalEuler, t, v);
}
Vec3 GetPosition(Il2CppObject* t) { return Alive(t) ? Call<Vec3>(S.getPosition, t) : Vec3{}; }
Quat GetRotation(Il2CppObject* t) { return Alive(t) ? Call<Quat>(S.getRotation, t) : Quat{0, 0, 0, 1}; }
Vec3 TransformPoint(Il2CppObject* t, Vec3 p) {
    return (Alive(t) && S.transformPoint) ? Call<Vec3>(S.transformPoint, t, p) : p;
}
Vec3 InverseTransformPoint(Il2CppObject* t, Vec3 p) {
    return (Alive(t) && S.inverseTransformPoint) ? Call<Vec3>(S.inverseTransformPoint, t, p) : p;
}
bool LocalToWorld(Il2CppObject* t, Mat4& out) {
    if (!Alive(t) || !S.getL2W) return false;
    out = Call<Mat4>(S.getL2W, t);
    return true;
}
bool WorldToLocal(Il2CppObject* t, Mat4& out) {
    if (!Alive(t) || !S.getW2L) return false;
    out = Call<Mat4>(S.getW2L, t);
    return true;
}
Mat4 Mul(const Mat4& a, const Mat4& b) {
    Mat4 c{};
    for (int col = 0; col < 4; ++col)
        for (int row = 0; row < 4; ++row) {
            float v = 0;
            for (int k = 0; k < 4; ++k) v += a.m[row + 4 * k] * b.m[k + 4 * col];
            c.m[row + 4 * col] = v;
        }
    return c;
}
bool HasShaderGlobals() { return S.shaderPropId && S.shaderSetGlobalMatrix && S.getL2W && S.getW2L; }
int ShaderPropertyId(std::string_view name) {
    if (!S.shaderPropId) return -1;
    Il2CppString* n = unity::NewString(name);
    return n ? Call<int>(S.shaderPropId, n) : -1;
}
void ShaderSetGlobalMatrix(int id, const Mat4& m) {
    if (S.shaderSetGlobalMatrix && id != -1) Call<void>(S.shaderSetGlobalMatrix, id, m);
}

// ------------------------------------------------------------ RectTransform ---
namespace {
// RectTransform icalls on a plain Transform corrupt native memory: refuse.
bool RT(Il2CppObject* rt, const char* what) {
    if (!Alive(rt)) return false;
    if (IsRectTransform(rt)) return true;
    static bool logged = false;
    if (!logged) {
        logged = true;
        SR2_ERROR("minimap: {} called on a non-RectTransform ({}) - skipped", what, unity::ClassName(rt));
    }
    return false;
}
}  // namespace

void SetAnchors(Il2CppObject* rt, Vec2 min, Vec2 max) {
    if (!RT(rt, "SetAnchors")) return;
    Call<void>(S.setAnchorMin, rt, min);
    Call<void>(S.setAnchorMax, rt, max);
}
void SetPivot(Il2CppObject* rt, Vec2 p) {
    if (RT(rt, "SetPivot")) Call<void>(S.setPivot, rt, p);
}
void SetSizeDelta(Il2CppObject* rt, Vec2 v) {
    if (RT(rt, "SetSizeDelta")) Call<void>(S.setSizeDelta, rt, v);
}
void SetAnchoredPosition(Il2CppObject* rt, Vec2 p) {
    if (RT(rt, "SetAnchoredPosition")) Call<void>(S.setAnchoredPosition, rt, p);
}
Vec2 GetAnchorMin(Il2CppObject* rt) {
    return (S.getAnchorMin && RT(rt, "GetAnchorMin")) ? Call<Vec2>(S.getAnchorMin, rt) : Vec2{};
}
Vec2 GetAnchorMax(Il2CppObject* rt) {
    return (S.getAnchorMax && RT(rt, "GetAnchorMax")) ? Call<Vec2>(S.getAnchorMax, rt) : Vec2{};
}
Vec2 GetSizeDelta(Il2CppObject* rt) {
    return (S.getSizeDelta && RT(rt, "GetSizeDelta")) ? Call<Vec2>(S.getSizeDelta, rt) : Vec2{};
}
Rect GetRect(Il2CppObject* rt) { return RT(rt, "GetRect") ? Call<Rect>(S.getRect, rt) : Rect{}; }

void Fill(Il2CppObject* rt) {
    SetAnchors(rt, {0, 0}, {1, 1});
    SetPivot(rt, {0.5f, 0.5f});
    SetSizeDelta(rt, {0, 0});
    SetAnchoredPosition(rt, {0, 0});
}

void Center(Il2CppObject* rt, Vec2 size) {
    SetAnchors(rt, {0.5f, 0.5f}, {0.5f, 0.5f});
    SetPivot(rt, {0.5f, 0.5f});
    SetSizeDelta(rt, size);
    SetAnchoredPosition(rt, {0, 0});
}

// --------------------------------------------------------------------- uGUI ---
void ImageSetSprite(Il2CppObject* img, Il2CppObject* sprite) {
    if (Alive(img)) Call<void>(S.imgSetSprite, img, Alive(sprite) ? sprite : nullptr);
}
Il2CppObject* ImageGetSprite(Il2CppObject* img) {
    return (Alive(img) && S.imgGetSprite) ? Call<Il2CppObject*>(S.imgGetSprite, img) : nullptr;
}
void ImageSetType(Il2CppObject* img, int type) {
    if (Alive(img) && S.imgSetType) Call<void>(S.imgSetType, img, type);
}
void ImageSetPreserveAspect(Il2CppObject* img, bool v) {
    if (Alive(img) && S.imgSetPreserveAspect) Call<void>(S.imgSetPreserveAspect, img, v);
}
void ImageSetPpuMultiplier(Il2CppObject* img, float v) {
    if (Alive(img) && S.imgSetPpuMul) Call<void>(S.imgSetPpuMul, img, v);
}
void GraphicSetColor(Il2CppObject* g, Color c) {
    if (Alive(g)) Call<void>(S.gfxSetColor, g, c);
}
void GraphicSetRaycastTarget(Il2CppObject* g, bool v) {
    if (Alive(g) && S.gfxSetRaycast) Call<void>(S.gfxSetRaycast, g, v);
}
void MaskSetShowGraphic(Il2CppObject* mask, bool v) {
    if (Alive(mask) && S.maskShowGraphic) Call<void>(S.maskShowGraphic, mask, v);
}
Il2CppObject* AddMask(Il2CppObject* go) { return AddComponent(go, S.mask); }
Il2CppObject* AddCanvasGroup(Il2CppObject* go) { return AddComponent(go, S.canvasGroup); }
void CanvasGroupSetAlpha(Il2CppObject* cg, float a) {
    if (Alive(cg)) Call<void>(S.cgSetAlpha, cg, a);
}
void CanvasGroupSetBlocksRaycasts(Il2CppObject* cg, bool v) {
    if (!Alive(cg)) return;
    if (S.cgSetBlocks) Call<void>(S.cgSetBlocks, cg, v);
    if (S.cgSetInteractable) Call<void>(S.cgSetInteractable, cg, v);
}
void CanvasGroupSetIgnoreParentGroups(Il2CppObject* cg, bool v) {
    if (Alive(cg) && S.cgSetIgnoreParent) Call<void>(S.cgSetIgnoreParent, cg, v);
}
bool HasIgnoreParentGroups() { return static_cast<bool>(S.cgSetIgnoreParent); }
Il2CppObject* AddCanvas(Il2CppObject* go) { return AddComponent(go, S.canvas); }
Il2CppObject* CanvasRoot(Il2CppObject* canvas) {
    return (Alive(canvas) && S.canvasRoot) ? Call<Il2CppObject*>(S.canvasRoot, canvas) : canvas;
}
int CanvasRenderMode(Il2CppObject* canvas) {
    return (Alive(canvas) && S.canvasGetRenderMode) ? Call<int>(S.canvasGetRenderMode, canvas) : -1;
}
void CanvasSetOverlay(Il2CppObject* canvas, int sortingOrder) {
    if (!Alive(canvas)) return;
    if (S.canvasSetRenderMode) Call<void>(S.canvasSetRenderMode, canvas, 0 /* ScreenSpaceOverlay */);
    if (S.canvasSetSorting) Call<void>(S.canvasSetSorting, canvas, sortingOrder);
}
bool BehaviourEnabled(Il2CppObject* b) { return (Alive(b) && S.behEnabled) ? Call<bool>(S.behEnabled, b) : true; }
bool MaskShowsGraphic(Il2CppObject* mask) {
    return (Alive(mask) && S.maskGetShow) ? Call<bool>(S.maskGetShow, mask) : true;
}
Il2CppClass* MaskClass() { return S.mask; }
Il2CppObject* GraphicGetMaterial(Il2CppObject* g) {
    if (!Alive(g) || !S.gfxGetMaterial || !IsA(g, S.image)) return nullptr;  // bound on Image
    Il2CppObject* m = Call<Il2CppObject*>(S.gfxGetMaterial, g);
    return Alive(m) ? m : nullptr;
}
Il2CppObject* GraphicGetDefaultMaterial(Il2CppObject* g) {
    if (!Alive(g) || !S.gfxGetDefaultMaterial) return nullptr;
    Il2CppObject* m = Call<Il2CppObject*>(S.gfxGetDefaultMaterial, g);
    return Alive(m) ? m : nullptr;
}
void GraphicSetMaterial(Il2CppObject* g, Il2CppObject* mat) {
    if (Alive(g) && S.gfxSetMaterial) Call<void>(S.gfxSetMaterial, g, Alive(mat) ? mat : nullptr);
}
bool MaterialHasProperty(Il2CppObject* mat, std::string_view name) {
    if (!Alive(mat) || !S.matHasProp) return false;
    Il2CppString* n = unity::NewString(name);
    return n && Call<bool>(S.matHasProp, mat, n);
}
Il2CppObject* MaterialMainTexture(Il2CppObject* mat) {
    if (!Alive(mat) || !S.matMainTex) return nullptr;
    Il2CppObject* t = Call<Il2CppObject*>(S.matMainTex, mat);
    return Alive(t) ? t : nullptr;
}
Rect MaterialMainUv(Il2CppObject* mat) {
    if (!Alive(mat) || !S.matMainOffset || !S.matMainScale) return Rect{0, 0, 1, 1};
    Vec2 o = Call<Vec2>(S.matMainOffset, mat), sc = Call<Vec2>(S.matMainScale, mat);
    return Rect{o.x, o.y, sc.x, sc.y};
}
std::string MaterialShaderName(Il2CppObject* mat) {
    if (!Alive(mat) || !S.matGetShader) return {};
    Il2CppObject* sh = Call<Il2CppObject*>(S.matGetShader, mat);
    return Alive(sh) ? unity::GetName(sh) : std::string();
}
Il2CppObject* SpriteTexture(Il2CppObject* sprite) {
    if (!Alive(sprite) || !S.spriteGetTexture) return nullptr;
    Il2CppObject* t = Call<Il2CppObject*>(S.spriteGetTexture, sprite);
    return Alive(t) ? t : nullptr;
}
bool HasSpriteTexture() { return static_cast<bool>(S.spriteGetTexture); }
Vec2 SpritePivot(Il2CppObject* sprite) {
    return (Alive(sprite) && S.spriteGetPivot) ? Call<Vec2>(S.spriteGetPivot, sprite) : Vec2{};
}
Vec4 SpriteBorder(Il2CppObject* sprite) {
    return (Alive(sprite) && S.spriteBorder) ? Call<Vec4>(S.spriteBorder, sprite) : Vec4{};
}
Rect SpriteRect(Il2CppObject* sprite) {
    return (Alive(sprite) && S.spriteRect) ? Call<Rect>(S.spriteRect, sprite) : Rect{};
}

// ----------------------------------------------------------------------- TMP ---
Il2CppObject* AddTmpText(Il2CppObject* go) { return HasTmp() ? AddComponent(go, S.tmpUgui) : nullptr; }
void TmpSetText(Il2CppObject* t, std::string_view utf8) {
    if (!Alive(t) || !S.tmpSetText) return;
    if (Il2CppString* s = unity::NewString(utf8)) Call<void>(S.tmpSetText, t, s);
}
void TmpSetFontSize(Il2CppObject* t, float size) {
    if (Alive(t) && S.tmpSetFontSize) Call<void>(S.tmpSetFontSize, t, size);
}
void TmpSetCenter(Il2CppObject* t) {
    if (!Alive(t)) return;
    if (S.tmpSetAlignment) Call<void>(S.tmpSetAlignment, t, 514 /* TextAlignmentOptions.Center */);
    if (S.tmpSetWrap) Call<void>(S.tmpSetWrap, t, false);
}
void TmpSetAlignment(Il2CppObject* t, int alignment) {
    if (Alive(t) && S.tmpSetAlignment) Call<void>(S.tmpSetAlignment, t, alignment);
}
float TmpPreferredWidth(Il2CppObject* t) {
    return (Alive(t) && S.tmpPreferredWidth) ? Call<float>(S.tmpPreferredWidth, t) : -1.0f;
}
void TmpSetColor(Il2CppObject* t, Color c) {
    if (Alive(t) && S.tmpSetColor) Call<void>(S.tmpSetColor, t, c);
}
void TmpCopyFont(Il2CppObject* dst, Il2CppObject* srcText) {
    if (!Alive(dst) || !Alive(srcText) || !S.tmpGetFont || !S.tmpSetFont) return;
    Il2CppObject* font = Call<Il2CppObject*>(S.tmpGetFont, srcText);
    if (Alive(font)) Call<void>(S.tmpSetFont, dst, font);
}

int TextureGetFilter(Il2CppObject* tex) { return (Alive(tex) && S.texGetFilter) ? Call<int>(S.texGetFilter, tex) : -1; }
void TextureSetFilter(Il2CppObject* tex, int mode) {
    if (Alive(tex) && S.texSetFilter) Call<void>(S.texSetFilter, tex, mode);
}

// ------------------------------------------------------- procedural sprites ---
Il2CppObject* MakeSprite(const std::vector<uint32_t>& rgba, int w, int h, Vec4 border, std::string_view name,
                         bool mipmaps) {
    if (!HasSpriteGen() || w <= 0 || h <= 0 || rgba.size() != static_cast<size_t>(w) * h) return nullptr;
    Il2CppObject* tex = ObjectNew(S.texture2d);
    if (!tex) return nullptr;
    Call<void>(S.texCtor, tex, w, h, 4 /* TextureFormat.RGBA32 */, mipmaps);
    if (!Alive(tex)) return nullptr;
    Il2CppArray* arr = S.x.array_new(S.color32, rgba.size());
    if (!arr) return nullptr;
    std::memcpy(unity::ArrayData<uint32_t>(arr), rgba.data(), rgba.size() * sizeof(uint32_t));
    Call<void>(S.texSetPixels32, tex, arr);
    // Trilinear with mips: smooth when the sprite is drawn smaller than its texture.
    if (S.texSetFilter) Call<void>(S.texSetFilter, tex, mipmaps ? 2 /* Trilinear */ : 1 /* Bilinear */);
    if (S.texSetWrap) Call<void>(S.texSetWrap, tex, 1 /* Clamp */);
    Call<void>(S.texApply, tex, mipmaps /* updateMipmaps */, true);
    unity::SetName(tex, name);
    DontUnload(tex);
    Rect rc{0, 0, static_cast<float>(w), static_cast<float>(h)};
    Il2CppObject* sp = Call<Il2CppObject*>(S.spriteCreate, tex, rc, Vec2{0.5f, 0.5f}, 100.0f, 0u,
                                           0 /* SpriteMeshType.FullRect */, border);
    if (!Alive(sp)) return nullptr;
    unity::SetName(sp, name);
    DontUnload(sp);
    return sp;
}

// ------------------------------------------------------------ fault capture ---
namespace {
struct FaultInfo {
    DWORD code = 0;
    void* addr = nullptr;
    void* av = nullptr;  // accessed address for access violations
    ULONG_PTR rw = 0;
};

int FaultFilter(EXCEPTION_POINTERS* ep, FaultInfo* f) {
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code == 0xE06D7363) return EXCEPTION_CONTINUE_SEARCH;  // C++ / managed exception: not ours
    f->code = code;
    f->addr = ep->ExceptionRecord->ExceptionAddress;
    if (code == EXCEPTION_ACCESS_VIOLATION && ep->ExceptionRecord->NumberParameters >= 2) {
        f->rw = ep->ExceptionRecord->ExceptionInformation[0];
        f->av = reinterpret_cast<void*>(ep->ExceptionRecord->ExceptionInformation[1]);
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

bool RunGuarded(void (*thunk)(void*), void* ctx, FaultInfo* f) {
    __try {
        thunk(ctx);
        return true;
    } __except (FaultFilter(GetExceptionInformation(), f)) {
        return false;
    }
}

std::string DescribeAddress(void* a) {
    HMODULE mod = nullptr;
    if (!a || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                  static_cast<LPCWSTR>(a), &mod))
        return std::format("{}", a);
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(mod, path, MAX_PATH);
    std::wstring_view w(path);
    size_t slash = w.find_last_of(L"\\/");
    if (slash != std::wstring_view::npos) w = w.substr(slash + 1);
    return std::format("{}+0x{:X}", sr2::log::Narrow(w),
                       reinterpret_cast<uintptr_t>(a) - reinterpret_cast<uintptr_t>(mod));
}
}  // namespace

bool GuardNativeImpl(void (*thunk)(void*), void* ctx, std::string* where) {
    FaultInfo f;
    if (RunGuarded(thunk, ctx, &f)) return true;
    if (where) {
        *where = std::format("native exception 0x{:08X} at {}", static_cast<unsigned>(f.code), DescribeAddress(f.addr));
        if (f.code == EXCEPTION_ACCESS_VIOLATION)
            *where += std::format(" ({} {})", f.rw == 0 ? "reading" : f.rw == 1 ? "writing" : "executing", f.av);
    }
    return false;
}

// --------------------------------------------------------------------- misc ---
int CursorLockState() { return S.cursorLockState ? Call<int>(S.cursorLockState) : -1; }
float TimeScale() { return S.timeScale ? Call<float>(S.timeScale) : -1.0f; }
int ScreenHeight() { return S.screenHeight ? Call<int>(S.screenHeight) : 0; }

}  // namespace sr2::modules::minimap::ugui
