// custom_upgrades: Hen Collector drone source icon (see drone_icon.h). SR2 1.3.0, all by name.
//
// Asset data (UnityPy, bundle duplicateassetisolation_assets__54b00e46...): the drone menu's
// DroneSourceDefinitions (scene MenuLayer_DroneStationMenu) gives PLORT_COLLECTOR (7) the sprite
// iconPatchCorralCollector (512 x 512 texture, sprite rect = the whole texture, not readable): a cream
// collector with a dark round opening (green glow inside) and a pink plort leaving it to the upper
// right. The opening's dark ring spans x 141..314, y 147..320 of the 512 canvas (Unity y up), centre
// (228, 233). The Hen icon iconBirdHen (512 x 512, not readable) is scaled so its opaque box is 200
// wide and centred on the opening, over the green glow; the plort stays. Offline preview:
// scratchpad/hen-source-icon.png (same numbers, PIL).

#include "modules/custom_upgrades/drone_icon.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <format>
#include <initializer_list>
#include <vector>

#include "core/detour_guard.h"
#include "core/il2cpp_api.h"
#include "core/log.h"
#include "core/resolver.h"
#include "core/unity.h"

namespace sr2::modules::custom_upgrades::drones {
namespace {

namespace unity = sr2::core::unity;
using sr2::il2cpp::api;
using sr2::il2cpp::Resolver;

constexpr const char* kCore = "UnityEngine.CoreModule";
constexpr int kHideDontUnload = 32;  // HideFlags.DontUnloadUnusedAsset
constexpr int kFormatRgba32 = 4;     // TextureFormat.RGBA32
// The opening, in fractions of the base sprite (Unity y up), and the hen's width there.
constexpr float kCentreX = 228.0f / 512.0f, kCentreY = 233.0f / 512.0f, kHenBox = 200.0f / 512.0f;
constexpr int kMaxSide = 1024;

struct Rect {
    float x, y, w, h;
};
struct V2 {
    float x, y;
};
struct Color32 {
    uint8_t r, g, b, a;
};

template <class T>
T Unbox(Il2CppObject* boxed, T def = T{}) {
    return boxed ? *reinterpret_cast<T*>(reinterpret_cast<char*>(boxed) + 2 * sizeof(void*)) : def;
}

}  // namespace

struct HenSourceIcon::Impl {
    const char* module = "custom_upgrades";
    Il2CppClass* tex2dClass = nullptr;
    const MethodInfo* sprTexture = nullptr, *sprRect = nullptr, *texWidth = nullptr, *texHeight = nullptr;
    const MethodInfo* rtGetTemp = nullptr, *rtRelease = nullptr, *rtGetActive = nullptr, *rtSetActive = nullptr;
    const MethodInfo* blit = nullptr, *texCtor = nullptr, *texRead = nullptr, *texGet = nullptr, *texSet = nullptr;
    const MethodInfo* texApply = nullptr, *spriteCreate = nullptr, *setHideFlags = nullptr, *objDestroy = nullptr;
    using ObjectNewFn = Il2CppObject* (*)(Il2CppClass*);
    ObjectNewFn objectNew = nullptr;

    Il2CppObject* Call(const MethodInfo* m, void* obj, std::initializer_list<void*> args, const char* what, bool* ok) {
        std::vector<void*> params(args);
        std::string err;
        Il2CppObject* r = Resolver::Get().Invoke(m, obj, params.empty() ? nullptr : params.data(), &err);
        if (ok) *ok = err.empty();
        if (!err.empty()) {
            if (err.rfind("native exception", 0) == 0)
                sr2::core::MarkModuleFaulted(module, std::format("{}: {}", what, err));
            else SR2_WARN("[{}] hen source icon: {} failed: {}", module, what, err);
        }
        return r;
    }
    Il2CppObject* NewTexture(int w, int h) {
        Il2CppObject* t = objectNew(tex2dClass);
        int fmt = kFormatRgba32;
        bool mips = false, ok = false;
        if (t) Call(texCtor, t, {&w, &h, &fmt, &mips}, "Texture2D..ctor", &ok);
        return t && ok ? t : nullptr;
    }
    // The sprite's rect of its texture as RGBA32, rows bottom-up (Unity), through a RenderTexture.
    bool ReadSprite(Il2CppObject* sprite, std::vector<Color32>& out, int& w, int& h, std::string& why) {
        bool ok = false;
        Il2CppObject* tex = Call(sprTexture, sprite, {}, "Sprite.get_texture", &ok);
        const Rect r = Unbox<Rect>(ok ? Call(sprRect, sprite, {}, "Sprite.get_rect", &ok) : nullptr);
        if (!ok || !unity::IsAlive(tex)) return why = "sprite has no texture", false;
        int tw = Unbox<int32_t>(Call(texWidth, tex, {}, "Texture.get_width", &ok));
        int th = ok ? Unbox<int32_t>(Call(texHeight, tex, {}, "Texture.get_height", &ok)) : 0;
        w = static_cast<int>(std::lround(r.w));
        h = static_cast<int>(std::lround(r.h));
        if (!ok || tw <= 0 || th <= 0 || tw > 4096 || th > 4096 || w <= 0 || h <= 0 || w > kMaxSide || h > kMaxSide)
            return why = std::format("unexpected texture / sprite size {}x{} / {}x{}", tw, th, w, h), false;
        int depth = 0;
        Il2CppObject* rt = Call(rtGetTemp, nullptr, {&tw, &th, &depth}, "RenderTexture.GetTemporary", &ok);
        if (!ok || !unity::IsAlive(rt)) return why = "no RenderTexture", false;
        Il2CppObject* dst = NewTexture(w, h);
        bool read = false;
        if (dst) {
            bool got = false;
            Il2CppObject* prev = Call(rtGetActive, nullptr, {}, "RenderTexture.get_active", &got);
            Call(blit, nullptr, {tex, rt}, "Graphics.Blit", &ok);
            Rect src{r.x, r.y, static_cast<float>(w), static_cast<float>(h)};
            int zero = 0;
            if (ok) Call(rtSetActive, nullptr, {rt}, "RenderTexture.set_active", &ok);
            if (ok) Call(texRead, dst, {&src, &zero, &zero}, "Texture2D.ReadPixels", &read);
            Call(rtSetActive, nullptr, {got ? prev : nullptr}, "RenderTexture.set_active", &got);
        }
        Call(rtRelease, nullptr, {rt}, "RenderTexture.ReleaseTemporary", &ok);
        void* px = read ? Call(texGet, dst, {}, "Texture2D.GetPixels32", &ok) : nullptr;
        const size_t n = px ? unity::ArrayLength(px) : 0;
        if (dst) Call(objDestroy, nullptr, {dst}, "Object.Destroy", &ok);  // the readable copy is ours
        if (n != static_cast<size_t>(w) * h) return why = "read back failed", false;
        const Color32* c = unity::ArrayData<Color32>(px);
        out.assign(c, c + n);
        return true;
    }
};

bool HenSourceIcon::Setup(Resolver& R, const char* module, std::string& why) {
    auto* p = new Impl();
    p->module = module;
    std::string err;
    auto cls = [&](const char* name) { return R.Class(kCore, "UnityEngine", name, &err); };
    Il2CppClass* sprite = cls("Sprite");
    Il2CppClass* texture = cls("Texture");
    p->tex2dClass = cls("Texture2D");
    Il2CppClass* rtClass = cls("RenderTexture");
    Il2CppClass* graphics = cls("Graphics");
    Il2CppClass* uobject = cls("Object");
    if (!sprite || !texture || !p->tex2dClass || !rtClass || !graphics || !uobject) {
        why = err.empty() ? "UnityEngine classes not found" : err;
        delete p;
        return false;
    }
    p->sprTexture = R.Method(sprite, "get_texture", 0, &err);
    p->sprRect = R.Method(sprite, "get_rect", 0, &err);
    p->texWidth = R.Method(texture, "get_width", 0, &err);
    p->texHeight = R.Method(texture, "get_height", 0, &err);
    p->rtGetTemp = R.MethodByParams(rtClass, "GetTemporary", {"System.Int32", "System.Int32", "System.Int32"}, &err);
    p->rtRelease = R.Method(rtClass, "ReleaseTemporary", 1, &err);
    p->rtGetActive = R.Method(rtClass, "get_active", 0, &err);
    p->rtSetActive = R.Method(rtClass, "set_active", 1, &err);
    p->blit = R.MethodByParams(graphics, "Blit", {"UnityEngine.Texture", "UnityEngine.RenderTexture"}, &err);
    p->texCtor = R.MethodByParams(p->tex2dClass, ".ctor", {"System.Int32", "System.Int32", "UnityEngine.TextureFormat", "System.Boolean"}, &err);
    p->texRead = R.MethodByParams(p->tex2dClass, "ReadPixels", {"UnityEngine.Rect", "System.Int32", "System.Int32"}, &err);
    p->texGet = R.Method(p->tex2dClass, "GetPixels32", 0, &err);
    p->texSet = R.MethodByParams(p->tex2dClass, "SetPixels32", {"UnityEngine.Color32[]"}, &err);
    p->texApply = R.MethodByParams(p->tex2dClass, "Apply", {"System.Boolean", "System.Boolean"}, &err);
    p->spriteCreate = R.MethodByParams(sprite, "Create", {"UnityEngine.Texture2D", "UnityEngine.Rect", "UnityEngine.Vector2", "System.Single"}, &err);
    p->setHideFlags = R.Method(uobject, "set_hideFlags", 1, &err);
    p->objDestroy = R.MethodByParams(uobject, "Destroy", {"UnityEngine.Object"}, &err);
    p->objectNew = reinterpret_cast<Impl::ObjectNewFn>(api().module ? GetProcAddress(api().module, "il2cpp_object_new") : nullptr);
    if (!p->sprTexture || !p->sprRect || !p->texWidth || !p->texHeight || !p->rtGetTemp || !p->rtRelease ||
        !p->rtGetActive || !p->rtSetActive || !p->blit || !p->texCtor || !p->texRead || !p->texGet || !p->texSet ||
        !p->texApply || !p->spriteCreate || !p->setHideFlags || !p->objDestroy || !p->objectNew) {
        why = err.empty() ? "texture methods not found" : err;
        delete p;
        return false;
    }
    impl_ = p;
    ready_ = true;
    return true;
}

Il2CppObject* HenSourceIcon::Compose(Il2CppObject* baseSprite, Il2CppObject* henSprite, std::string& why) {
    if (!ready_) return why = "not set up", nullptr;
    if (!unity::IsAlive(baseSprite) || !unity::IsAlive(henSprite)) return why = "no sprite", nullptr;
    Impl& I = *impl_;
    std::vector<Color32> base, hen;
    int bw = 0, bh = 0, hw = 0, hh = 0;
    if (!I.ReadSprite(baseSprite, base, bw, bh, why) || !I.ReadSprite(henSprite, hen, hw, hh, why)) return nullptr;
    // The hen's opaque box.
    int x0 = hw, y0 = hh, x1 = -1, y1 = -1;
    for (int y = 0; y < hh; ++y)
        for (int x = 0; x < hw; ++x)
            if (hen[static_cast<size_t>(y) * hw + x].a > 8) {
                x0 = std::min(x0, x);
                y0 = std::min(y0, y);
                x1 = std::max(x1, x);
                y1 = std::max(y1, y);
            }
    size_t baseOpaque = 0;
    for (const Color32& c : base) baseOpaque += c.a > 8 ? 1 : 0;
    if (x1 < x0 || y1 < y0 || baseOpaque < 64) return why = "an icon read back empty (blit / readback)", nullptr;
    const float sw = static_cast<float>(x1 - x0 + 1), sh = static_cast<float>(y1 - y0 + 1);
    const float scale = kHenBox * bw / std::max(sw, sh);
    const float dw = sw * scale, dh = sh * scale;
    const float dx0 = kCentreX * bw - dw * 0.5f, dy0 = kCentreY * bh - dh * 0.5f;
    // Bilinear sample of the hen (premultiplied), composited alpha-over.
    auto at = [&](int x, int y) -> const Color32& {
        x = std::clamp(x, 0, hw - 1);
        y = std::clamp(y, 0, hh - 1);
        return hen[static_cast<size_t>(y) * hw + x];
    };
    for (int y = std::max(0, static_cast<int>(dy0)); y < std::min(bh, static_cast<int>(std::ceil(dy0 + dh))); ++y)
        for (int x = std::max(0, static_cast<int>(dx0)); x < std::min(bw, static_cast<int>(std::ceil(dx0 + dw))); ++x) {
            const float u = x0 + (x + 0.5f - dx0) / scale - 0.5f, v = y0 + (y + 0.5f - dy0) / scale - 0.5f;
            const int ix = static_cast<int>(std::floor(u)), iy = static_cast<int>(std::floor(v));
            const float fx = u - ix, fy = v - iy;
            float pr = 0, pg = 0, pb = 0, pa = 0;
            const float wts[4] = {(1 - fx) * (1 - fy), fx * (1 - fy), (1 - fx) * fy, fx * fy};
            const Color32* s[4] = {&at(ix, iy), &at(ix + 1, iy), &at(ix, iy + 1), &at(ix + 1, iy + 1)};
            for (int k = 0; k < 4; ++k) {
                const float a = s[k]->a / 255.0f * wts[k];
                pr += s[k]->r * a;
                pg += s[k]->g * a;
                pb += s[k]->b * a;
                pa += a;
            }
            if (pa <= 0.002f) continue;
            Color32& d = base[static_cast<size_t>(y) * bw + x];
            const float da = d.a / 255.0f, oa = pa + da * (1 - pa);
            auto mix = [&](float src, uint8_t dst) {
                return static_cast<uint8_t>(std::clamp((src + dst * da * (1 - pa)) / oa, 0.0f, 255.0f));
            };
            d = Color32{mix(pr, d.r), mix(pg, d.g), mix(pb, d.b), static_cast<uint8_t>(std::lround(oa * 255.0f))};
        }
    // Upload: a new texture + sprite, kept for the session.
    Il2CppObject* tex = I.NewTexture(bw, bh);
    Il2CppObject* arr = nullptr;
    if (tex) {
        bool ok = false;
        Il2CppObject* px = I.Call(I.texGet, tex, {}, "Texture2D.GetPixels32", &ok);  // a Color32[] of the right size
        if (ok && px && unity::ArrayLength(px) == base.size()) {
            std::copy(base.begin(), base.end(), unity::ArrayData<Color32>(px));
            arr = px;
        }
    }
    bool ok = false;
    bool mips = false, noLongerReadable = true;
    if (arr) I.Call(I.texSet, tex, {arr}, "Texture2D.SetPixels32", &ok);
    if (ok) I.Call(I.texApply, tex, {&mips, &noLongerReadable}, "Texture2D.Apply", &ok);
    if (!ok) return why = "upload failed", nullptr;
    Rect rect{0, 0, static_cast<float>(bw), static_cast<float>(bh)};
    V2 pivot{0.5f, 0.5f};
    float ppu = 100.0f;
    Il2CppObject* spr = I.Call(I.spriteCreate, nullptr, {tex, &rect, &pivot, &ppu}, "Sprite.Create", &ok);
    if (!ok || !unity::IsAlive(spr)) return why = "Sprite.Create failed", nullptr;
    int hide = kHideDontUnload;
    I.Call(I.setHideFlags, tex, {&hide}, "Object.set_hideFlags", &ok);
    I.Call(I.setHideFlags, spr, {&hide}, "Object.set_hideFlags", &ok);
    unity::NewGCHandle(spr);  // never freed: the UI keeps showing it
    unity::NewGCHandle(tex);
    return spr;
}

}  // namespace sr2::modules::custom_upgrades::drones
