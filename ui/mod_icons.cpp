#include "ui/mod_icons.h"

#include <windows.h>

#include <cstdint>
#include <cstring>
#include <format>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/log.h"
#include "core/main_thread.h"
#include "core/resolver.h"
#include "core/unity.h"

#pragma warning(push, 0)
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#define STBI_NO_FAILURE_STRINGS
#include "third_party/stb/stb_image.h"
#pragma warning(pop)

namespace sr2::ui::mod_icons {
namespace {

namespace unity = sr2::core::unity;
using sr2::il2cpp::api;
using sr2::il2cpp::Resolver;

struct EmbeddedPng {
    const char* name;
    const unsigned char* data;
    size_t size;
};
// Generated at build time (CMake runs assets/icons/src/gen_inc.py into build/generated); the
// array ends with a {nullptr, nullptr, 0} entry.
#include "sr2mods_icons_data.inc"

constexpr const char* kCore = "UnityEngine.CoreModule";
constexpr int32_t kTextureFormatRGBA32 = 4;
constexpr int32_t kFilterTrilinear = 2;
// The game's own Fabricator full art (e.g. iconDecorBoombox: 512 px DXT5, 10 mips, trilinear) has mip bias -1:
// the 440 px details box samples mip 0 only. With bias 0 a 512 px drawing there blends ~20 % of its half-size
// mip in (round 12: "blurry").
constexpr float kMipBias = -1.0f;
constexpr int32_t kWrapClamp = 1;
constexpr int32_t kHideFlagsDontUnloadUnusedAsset = 32;
constexpr float kDefaultPpu = 100.0f;
constexpr int kMaxAttempts = 3;  // per icon (e.g. a sprite destroyed by a scene unload is rebuilt)

struct Vec2 { float x, y; };
struct Rect { float x, y, w, h; };

struct Api {
    bool tried = false, ok = false;
    Il2CppClass *texture2d = nullptr, *sprite = nullptr, *color32 = nullptr;
    const MethodInfo *texCtor = nullptr, *setPixels32 = nullptr, *apply = nullptr, *setFilter = nullptr,
                     *setWrap = nullptr, *setMipBias = nullptr, *spriteCreate = nullptr, *spriteRect = nullptr, *spritePpu = nullptr,
                     *setHideFlags = nullptr, *setName = nullptr;
    using ArrayNew = Il2CppArray* (*)(Il2CppClass*, uintptr_t);
    ArrayNew arrayNew = nullptr;
    using ObjectNew = Il2CppObject* (*)(Il2CppClass*);
    ObjectNew objectNew = nullptr;
} A;

struct Entry {
    Il2CppObject* sprite = nullptr;  // pinned by a never-freed GC handle
    int attempts = 0;
};
std::unordered_map<std::string, Entry> g_cache;
bool g_faulted = false;  // a native exception inside Unity: no more icons this session

bool Resolve() {
    if (A.tried) return A.ok;
    A.tried = true;
    Resolver& r = Resolver::Get();
    std::string err, missing;
    auto need = [&](const void* p, const char* what) {
        if (!p) missing += std::format("{}{}", missing.empty() ? "" : ", ", what);
    };
    A.texture2d = r.Class(kCore, "UnityEngine", "Texture2D", &err);
    A.sprite = r.Class(kCore, "UnityEngine", "Sprite", &err);
    A.color32 = r.Class(kCore, "UnityEngine", "Color32", &err);
    Il2CppClass* texture = r.Class(kCore, "UnityEngine", "Texture", &err);
    Il2CppClass* object = r.Class(kCore, "UnityEngine", "Object", &err);
    need(A.texture2d, "Texture2D");
    need(A.sprite, "Sprite");
    need(A.color32, "Color32");
    need(texture, "Texture");
    need(object, "Object");
    if (A.texture2d) {
        A.texCtor = r.MethodByParams(A.texture2d, ".ctor",
                                     {"System.Int32", "System.Int32", "UnityEngine.TextureFormat", "System.Boolean"}, &err);
        A.setPixels32 = r.MethodByParams(A.texture2d, "SetPixels32", {"UnityEngine.Color32[]"}, &err);
        A.apply = r.MethodByParams(A.texture2d, "Apply", {"System.Boolean", "System.Boolean"}, &err);
    }
    if (texture) {
        A.setFilter = r.Method(texture, "set_filterMode", 1, &err);
        A.setWrap = r.Method(texture, "set_wrapMode", 1, &err);
        A.setMipBias = r.Method(texture, "set_mipMapBias", 1, &err);         // optional (sharpness)
    }
    if (A.sprite) {
        A.spriteCreate = r.MethodByParams(
            A.sprite, "Create", {"UnityEngine.Texture2D", "UnityEngine.Rect", "UnityEngine.Vector2", "System.Single"}, &err);
        A.spriteRect = r.Method(A.sprite, "get_rect", 0, &err);              // optional (size matching)
        A.spritePpu = r.Method(A.sprite, "get_pixelsPerUnit", 0, &err);      // optional (size matching)
    }
    if (object) {
        A.setHideFlags = r.Method(object, "set_hideFlags", 1, &err);
        A.setName = r.Method(object, "set_name", 1, &err);                   // optional (diagnostics)
    }
    if (HMODULE gm = api().module) {
        A.arrayNew = reinterpret_cast<Api::ArrayNew>(GetProcAddress(gm, "il2cpp_array_new"));
        A.objectNew = reinterpret_cast<Api::ObjectNew>(GetProcAddress(gm, "il2cpp_object_new"));
    }
    need(A.texCtor, "Texture2D..ctor(int,int,TextureFormat,bool)");
    need(A.setPixels32, "Texture2D.SetPixels32(Color32[])");
    need(A.apply, "Texture2D.Apply(bool,bool)");
    need(A.setFilter, "Texture.set_filterMode");
    need(A.setWrap, "Texture.set_wrapMode");
    need(A.spriteCreate, "Sprite.Create(Texture2D,Rect,Vector2,float)");
    need(A.setHideFlags, "Object.set_hideFlags");
    need(reinterpret_cast<const void*>(A.arrayNew), "il2cpp_array_new");
    need(reinterpret_cast<const void*>(A.objectNew), "il2cpp_object_new");
    A.ok = missing.empty();
    if (!A.ok) SR2_WARN("ui: mod icons unavailable (missing {}) - the game's own icons stay", missing);
    return A.ok;
}

// Runtime invoke; false on a managed exception or a native fault (the latter stops all
// further icon creation - it is never retried).
bool Call(const MethodInfo* m, void* obj, std::initializer_list<void*> args, const char* what,
          Il2CppObject** ret = nullptr) {
    std::vector<void*> params(args);
    std::string err;
    Il2CppObject* r = Resolver::Get().Invoke(m, obj, params.empty() ? nullptr : params.data(), &err);
    if (!err.empty()) {
        if (err.find("native exception") != std::string::npos) {
            g_faulted = true;
            SR2_ERROR("ui: mod icons: {} faulted ({}) - no more mod icons this session", what, err);
        } else {
            SR2_WARN("ui: mod icons: {} threw: {}", what, err);
        }
        return false;
    }
    if (ret) *ret = r;
    return true;
}

template <class T>
bool Unboxed(Il2CppObject* boxed, T& out) {
    if (!boxed) return false;
    std::memcpy(&out, reinterpret_cast<char*>(boxed) + 2 * sizeof(void*), sizeof(T));  // object header
    return true;
}

void Name(Il2CppObject* obj, const std::string& name) {
    if (A.setName) Call(A.setName, obj, {unity::NewString(name)}, "Object.set_name");
}

// pixelsPerUnit that gives our sprite the native size of `ref` (its longer side).
float PpuLike(Il2CppObject* ref, int size) {
    if (!ref || !A.spriteRect || !A.spritePpu || !unity::IsAlive(ref)) return kDefaultPpu;
    Il2CppClass* k = api().il2cpp_object_get_class ? api().il2cpp_object_get_class(ref) : nullptr;
    if (!k || !api().il2cpp_class_is_assignable_from || !api().il2cpp_class_is_assignable_from(A.sprite, k))
        return kDefaultPpu;
    Il2CppObject* boxedRect = nullptr;
    Il2CppObject* boxedPpu = nullptr;
    Rect rc{};
    float ppu = 0;
    if (!Call(A.spriteRect, ref, {}, "Sprite.get_rect", &boxedRect) || !Unboxed(boxedRect, rc)) return kDefaultPpu;
    if (!Call(A.spritePpu, ref, {}, "Sprite.get_pixelsPerUnit", &boxedPpu) || !Unboxed(boxedPpu, ppu)) return kDefaultPpu;
    const float side = rc.w > rc.h ? rc.w : rc.h;
    if (!(side > 1.0f && side < 16384.0f) || !(ppu > 0.01f && ppu < 100000.0f)) return kDefaultPpu;
    return static_cast<float>(size) * ppu / side;
}

// Greyed variant: desaturated and pulled towards a light grey, alpha unchanged.
void Grey(std::vector<uint32_t>& rgba) {
    for (uint32_t& p : rgba) {
        const int r = p & 0xFF, g = (p >> 8) & 0xFF, b = (p >> 16) & 0xFF;
        const int l = (r * 77 + g * 150 + b * 29) >> 8;
        const uint32_t v = static_cast<uint32_t>(96 + l * 5 / 8);  // 96..255
        p = (p & 0xFF000000u) | (v << 16) | (v << 8) | v;
    }
}

Il2CppObject* Create(const EmbeddedPng& png, Il2CppObject* sizeLike, bool greyed) {
    int w = 0, h = 0, n = 0;
    stbi_uc* px = stbi_load_from_memory(png.data, static_cast<int>(png.size), &w, &h, &n, 4);
    if (!px || w <= 0 || h <= 0 || w > 4096 || h > 4096) {
        if (px) stbi_image_free(px);
        SR2_WARN("ui: mod icons: '{}' could not be decoded", png.name);
        return nullptr;
    }
    // Unity textures start with the bottom row.
    const size_t count = static_cast<size_t>(w) * h;
    std::vector<uint32_t> rgba(count);
    for (int y = 0; y < h; ++y)
        std::memcpy(&rgba[static_cast<size_t>(h - 1 - y) * w], px + static_cast<size_t>(y) * w * 4,
                    static_cast<size_t>(w) * 4);
    stbi_image_free(px);
    if (greyed) Grey(rgba);
    const std::string objName = std::string("sr2mods_icon_") + png.name + (greyed ? "_grey" : "");

    Il2CppObject* tex = A.objectNew(A.texture2d);
    if (!tex) return nullptr;
    void* texHandle = unity::NewGCHandle(tex);  // pinned from here on (never freed: the icon lives for the session)
    if (!texHandle) return nullptr;
    int32_t iw = w, ih = h, fmt = kTextureFormatRGBA32;
    bool mips = true;
    if (!Call(A.texCtor, tex, {&iw, &ih, &fmt, &mips}, "Texture2D..ctor") || !unity::IsAlive(tex)) return nullptr;
    Il2CppArray* arr = A.arrayNew(A.color32, count);
    if (!arr || unity::ArrayLength(arr) != count) return nullptr;
    std::memcpy(unity::ArrayData<uint32_t>(arr), rgba.data(), count * sizeof(uint32_t));
    if (!Call(A.setPixels32, tex, {arr}, "Texture2D.SetPixels32")) return nullptr;
    int32_t filter = kFilterTrilinear, wrap = kWrapClamp, flags = kHideFlagsDontUnloadUnusedAsset;
    Call(A.setFilter, tex, {&filter}, "Texture.set_filterMode");
    Call(A.setWrap, tex, {&wrap}, "Texture.set_wrapMode");
    float bias = kMipBias;
    const bool biased = A.setMipBias && Call(A.setMipBias, tex, {&bias}, "Texture.set_mipMapBias");
    bool updateMips = true, noLongerReadable = true;
    if (!Call(A.apply, tex, {&updateMips, &noLongerReadable}, "Texture2D.Apply")) return nullptr;
    Call(A.setHideFlags, tex, {&flags}, "Object.set_hideFlags");
    Name(tex, objName);
    if (g_faulted) return nullptr;

    Rect rc{0, 0, static_cast<float>(w), static_cast<float>(h)};
    Vec2 pivot{0.5f, 0.5f};
    float ppu = PpuLike(sizeLike, w > h ? w : h);
    Il2CppObject* sprite = nullptr;
    if (!Call(A.spriteCreate, nullptr, {tex, &rc, &pivot, &ppu}, "Sprite.Create", &sprite)) return nullptr;
    if (!unity::IsAlive(sprite)) return nullptr;
    if (!unity::NewGCHandle(sprite)) return nullptr;
    Call(A.setHideFlags, sprite, {&flags}, "Object.set_hideFlags");
    Name(sprite, objName);
    if (g_faulted) return nullptr;
    SR2_INFO("ui: mod icon '{}' created ({}x{} RGBA32, mips, trilinear, mip bias {}, {:.1f} px/unit)", objName, w, h,
             biased ? "-1" : "0 (set_mipMapBias missing)", ppu);
    return sprite;
}

}  // namespace

Il2CppObject* Get(std::string_view name, Il2CppObject* sizeLike, bool greyed) {
    if (g_faulted || !core::main_thread::IsMainThread()) return nullptr;
    auto [it, fresh] = g_cache.try_emplace(std::string(name) + (greyed ? "#grey" : ""));
    Entry& e = it->second;
    if (e.sprite && unity::IsAlive(e.sprite)) return e.sprite;
    e.sprite = nullptr;
    if (e.attempts >= kMaxAttempts) return nullptr;
    ++e.attempts;
    if (!Resolve()) return nullptr;
    const EmbeddedPng* png = nullptr;
    for (const EmbeddedPng& p : kEmbeddedIcons)
        if (p.name && name == p.name) png = &p;
    if (!png) {
        SR2_WARN("ui: mod icons: no embedded icon named '{}'", name);
        e.attempts = kMaxAttempts;
        return nullptr;
    }
    e.sprite = Create(*png, sizeLike, greyed);
    if (!e.sprite && e.attempts == 1 && !g_faulted)
        SR2_WARN("ui: mod icon '{}' could not be created - using the game's icon", name);
    return e.sprite;
}

bool Has(std::string_view name) {
    for (const EmbeddedPng& p : kEmbeddedIcons)
        if (p.name && name == p.name) return true;
    return false;
}

}  // namespace sr2::ui::mod_icons
