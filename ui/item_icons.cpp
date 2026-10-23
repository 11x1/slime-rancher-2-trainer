#include "item_icons.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/il2cpp_api.h"
#include "core/log.h"
#include "core/unity.h"
#include "il2cpp_util.h"

namespace sr2::ui::item_icons {
namespace {

namespace unity = ::sr2::core::unity;

constexpr const char* kCore = "UnityEngine.CoreModule";

// One registered icon: the D3D11 resource of its texture and its rectangle in
// D3D texture coordinates (v down).
struct Icon {
    void* nativeTex = nullptr;  // ID3D11Resource* (Texture.GetNativeTexturePtr)
    float u0 = 0, v0 = 0, u1 = 1, v1 = 1;
    float aspect = 1.0f;
};

std::mutex g_mutex;
std::vector<Icon> g_icons;           // id = index + 1; guarded by g_mutex, append only
std::atomic<uint32_t> g_count{0};    // g_icons.size(), for the render thread's cheap "anything new?" check

// ------------------------------------------------ main thread: symbols ----
struct Syms {
    Il2CppClass* sprite = nullptr;
    Il2CppClass* texture = nullptr;
    const MethodInfo* sprTexture = nullptr;
    const MethodInfo* sprRect = nullptr;
    const MethodInfo* sprPacked = nullptr;       // optional (with the two below)
    const MethodInfo* sprPackingMode = nullptr;  // SpritePackingMode: Tight = 0
    const MethodInfo* sprUv = nullptr;
    const MethodInfo* texWidth = nullptr;
    const MethodInfo* texHeight = nullptr;
    // Texture.GetNativeTexturePtr() is stripped from the managed code; the engine
    // icall takes the native object (Object.m_CachedPtr). See slime_icons.cpp.
    using NativePtrFn = void* (*)(void* nativeSelf);
    NativePtrFn texNative = nullptr;
    size_t objCachedPtr = 0;
};

Syms S;
bool g_symsTried = false, g_symsOk = false;
std::unordered_map<Il2CppObject*, uint32_t> g_bySprite;  // main thread; 0 = could not be read
std::unordered_map<void*, bool> g_keptTex;               // main thread: textures already held

bool Resolve() {
    if (g_symsTried) return g_symsOk;
    g_symsTried = true;
    std::string err;
    auto cls = [&](const char* ns, const char* name) -> Il2CppClass* {
        return err.empty() ? il::Class(kCore, ns, name, &err) : nullptr;
    };
    auto method = [&](Il2CppClass* k, const char* name, int argc) -> const MethodInfo* {
        return err.empty() ? il::MethodInHierarchy(k, name, argc, &err) : nullptr;
    };
    S.sprite = cls("UnityEngine", "Sprite");
    S.texture = cls("UnityEngine", "Texture");
    Il2CppClass* object = cls("UnityEngine", "Object");
    S.sprTexture = method(S.sprite, "get_texture", 0);
    S.sprRect = method(S.sprite, "get_textureRect", 0);
    S.texWidth = method(S.texture, "get_width", 0);
    S.texHeight = method(S.texture, "get_height", 0);
    if (err.empty()) il::FieldOffsetInHierarchy(object, "m_CachedPtr", S.objCachedPtr, &err);
    if (err.empty()) {
        using ResolveIcall = void* (*)(const char*);
        auto resolve = reinterpret_cast<ResolveIcall>(il::Export("il2cpp_resolve_icall"));
        if (!resolve) err = "il2cpp_resolve_icall not exported";
        else if (!(S.texNative = reinterpret_cast<Syms::NativePtrFn>(resolve("UnityEngine.Texture::GetNativeTexturePtr_Injected"))))
            err = "icall UnityEngine.Texture::GetNativeTexturePtr_Injected not registered";
    }
    if (!err.empty()) {
        SR2_WARN("ui: item icons unavailable (item grids show names only): {}", err);
        return false;
    }
    S.sprPacked = il::Method(S.sprite, "get_packed", 0);
    S.sprPackingMode = il::Method(S.sprite, "get_packingMode", 0);
    S.sprUv = il::Method(S.sprite, "get_uv", 0);
    g_symsOk = true;
    return true;
}

// The icall under SEH like il::Invoke: a fault puts the UI into fault mode.
void* CallNativePtr(Syms::NativePtrFn fn, void* self, DWORD* code) {
    __try {
        return fn(self);
    } __except (*code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

// ID3D11Resource* of a live Texture; class and m_CachedPtr are checked first,
// so the icall never sees a destroyed object.
void* NativeTexture(Il2CppObject* tex) {
    if (!tex || !il::IsA(tex, S.texture) || il::AnyFault()) return nullptr;
    void* self = unity::Field<void*>(tex, S.objCachedPtr);
    if (!self) return nullptr;
    DWORD code = 0;
    void* out = CallNativePtr(S.texNative, self, &code);
    if (code) {
        il::MarkFault("Texture::GetNativeTexturePtr_Injected", code);
        return nullptr;
    }
    return out;
}

struct UnityRect {
    float x, y, w, h;
};
struct Vec2 {
    float x, y;
};

bool ReadSprite(Il2CppObject* spr, Icon& out) {
    if (!spr || !unity::IsAlive(spr) || !il::IsA(spr, S.sprite)) return false;
    Il2CppObject* tex = il::Invoke(S.sprTexture, spr, {});
    if (!tex || !unity::IsAlive(tex)) return false;
    int tw = 0, th = 0;
    if (!il::UnboxTo(il::Invoke(S.texWidth, tex, {}), tw) || !il::UnboxTo(il::Invoke(S.texHeight, tex, {}), th)) return false;
    if (tw <= 0 || th <= 0) return false;
    void* native = NativeTexture(tex);
    if (!native) return false;

    // textureRect throws for tightly packed atlas sprites; the bounds of the mesh
    // UVs give the same rectangle for those.
    float x = 0, y = 0, w = 0, h = 0;
    bool packed = false;
    int mode = 1;
    const bool tight = S.sprPacked && S.sprPackingMode && S.sprUv && il::UnboxTo(il::Invoke(S.sprPacked, spr, {}), packed) &&
                       packed && il::UnboxTo(il::Invoke(S.sprPackingMode, spr, {}), mode) && mode == 0;
    if (tight) {
        void* uv = il::Invoke(S.sprUv, spr, {});
        const size_t n = unity::ArrayLength(uv);
        if (n == 0) return false;
        const Vec2* p = unity::ArrayData<Vec2>(uv);
        float a0 = 1, b0 = 1, a1 = 0, b1 = 0;
        for (size_t i = 0; i < n; ++i) {
            a0 = std::min(a0, p[i].x), b0 = std::min(b0, p[i].y);
            a1 = std::max(a1, p[i].x), b1 = std::max(b1, p[i].y);
        }
        x = a0 * tw, y = b0 * th, w = (a1 - a0) * tw, h = (b1 - b0) * th;
    } else {
        UnityRect r{};
        if (!il::UnboxTo(il::Invoke(S.sprRect, spr, {}), r)) return false;
        x = r.x, y = r.y, w = r.w, h = r.h;
    }
    if (w <= 0 || h <= 0) return false;

    // Unity uploads rows bottom-up, so Unity's v is the D3D v: the sprite's top edge
    // is (y + h) / th. Half a texel in, against atlas bleeding.
    const float ftw = static_cast<float>(tw), fth = static_cast<float>(th);
    out.nativeTex = native;
    out.u0 = (x + 0.5f) / ftw, out.v0 = (y + h - 0.5f) / fth;
    out.u1 = (x + w - 0.5f) / ftw, out.v1 = (y + 0.5f) / fth;
    out.aspect = w / h;
    if (!g_keptTex[native]) {
        il::Keep(tex);  // never unloaded while the overlay samples it
        g_keptTex[native] = true;
    }
    return true;
}

}  // namespace

uint32_t Register(Il2CppObject* sprite) {
    if (!sprite) return 0;
    if (auto it = g_bySprite.find(sprite); it != g_bySprite.end()) return it->second;
    if (il::AnyFault() || !Resolve()) return 0;
    Icon icon;
    uint32_t id = 0;
    if (ReadSprite(sprite, icon)) {
        il::Keep(sprite);  // the cache key stays this sprite for the process lifetime
        std::lock_guard lk(g_mutex);
        g_icons.push_back(icon);
        id = static_cast<uint32_t>(g_icons.size());
        g_count.store(id);
    }
    g_bySprite[sprite] = id;
    return id;
}

}  // namespace sr2::ui::item_icons

// ------------------------------------------------ render thread: drawing ----
#if defined(SR2_HAVE_IMGUI)

#include <d3d11.h>
#include <d3dcompiler.h>

#include <cstring>
#include <map>

#include "backends/imgui_impl_dx11.h"

namespace sr2::ui::item_icons {
namespace {

struct View {
    ID3D11ShaderResourceView* srv = nullptr;
    bool srgb = false;  // sampled as linear: needs the gamma shader
};

std::vector<Icon> g_local;          // render thread copy of g_icons
std::map<void*, View> g_views;      // per native texture; a failure is remembered as an empty view
ID3D11PixelShader* g_gammaPs = nullptr;
bool g_psTried = false;

// Typeless -> UNORM (no conversion); typed sRGB stays, then the gamma shader undoes the decode.
DXGI_FORMAT ViewFormat(DXGI_FORMAT f, bool& srgb) {
    srgb = false;
    switch (f) {
        case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
        case DXGI_FORMAT_BC1_TYPELESS: return DXGI_FORMAT_BC1_UNORM;
        case DXGI_FORMAT_BC2_TYPELESS: return DXGI_FORMAT_BC2_UNORM;
        case DXGI_FORMAT_BC3_TYPELESS: return DXGI_FORMAT_BC3_UNORM;
        case DXGI_FORMAT_BC7_TYPELESS: return DXGI_FORMAT_BC7_UNORM;
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        case DXGI_FORMAT_BC1_UNORM_SRGB:
        case DXGI_FORMAT_BC2_UNORM_SRGB:
        case DXGI_FORMAT_BC3_UNORM_SRGB:
        case DXGI_FORMAT_BC7_UNORM_SRGB: srgb = true; return f;
        default: return f;
    }
}

// ImGui's DX11 pixel shader with the sRGB curve put back on the texel.
void CreateGammaShader(ID3D11Device* device) {
    g_psTried = true;
    static const char kSource[] =
        "struct PS_INPUT { float4 pos : SV_POSITION; float4 col : COLOR0; float2 uv : TEXCOORD0; };\n"
        "sampler sampler0;\n"
        "Texture2D texture0;\n"
        "float4 main(PS_INPUT input) : SV_Target {\n"
        "  float4 t = texture0.Sample(sampler0, input.uv);\n"
        "  t.rgb = pow(saturate(t.rgb), 1.0 / 2.2);\n"
        "  return input.col * t;\n"
        "}\n";
    ID3DBlob* blob = nullptr;
    ID3DBlob* errors = nullptr;
    if (FAILED(D3DCompile(kSource, std::strlen(kSource), nullptr, nullptr, nullptr, "main", "ps_4_0", 0, 0, &blob, &errors))) {
        SR2_WARN("ui: item icon shader did not compile; sRGB icons will look darker");
        if (errors) errors->Release();
        return;
    }
    if (FAILED(device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &g_gammaPs))) g_gammaPs = nullptr;
    blob->Release();
}

void SetGammaShader(const ImDrawList*, const ImDrawCmd*) {
    auto* rs = static_cast<ImGui_ImplDX11_RenderState*>(ImGui::GetPlatformIO().Renderer_RenderState);
    if (rs && g_gammaPs) rs->DeviceContext->PSSetShader(g_gammaPs, nullptr, 0);
}

// Shader resource view for a native texture, created once (on the texture's own device).
const View* ViewFor(void* nativeTex) {
    if (auto it = g_views.find(nativeTex); it != g_views.end()) return it->second.srv ? &it->second : nullptr;
    View& v = g_views[nativeTex];
    ID3D11Texture2D* tex = nullptr;
    auto* res = static_cast<ID3D11Resource*>(nativeTex);
    if (FAILED(res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&tex))) || !tex) return nullptr;
    ID3D11Device* device = nullptr;
    tex->GetDevice(&device);
    D3D11_TEXTURE2D_DESC td{};
    tex->GetDesc(&td);
    D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format = ViewFormat(td.Format, v.srgb);
    sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    sd.Texture2D.MostDetailedMip = 0;
    sd.Texture2D.MipLevels = td.MipLevels ? td.MipLevels : static_cast<UINT>(-1);
    if (!device || td.ArraySize != 1 || FAILED(device->CreateShaderResourceView(tex, &sd, &v.srv))) v.srv = nullptr;
    if (device && v.srgb && v.srv && !g_psTried) CreateGammaShader(device);
    if (device) device->Release();
    tex->Release();
    return v.srv ? &v : nullptr;
}

void Sync() {
    const uint32_t n = g_count.load();
    if (n == g_local.size()) return;
    std::lock_guard lk(g_mutex);
    g_local.assign(g_icons.begin(), g_icons.end());
}

}  // namespace

bool Draw(ImDrawList* dl, uint32_t id, ImVec2 min, ImVec2 max, ImU32 tint) {
    if (id == 0) return false;
    Sync();
    if (id > g_local.size()) return false;
    const Icon& ic = g_local[id - 1];
    const View* v = ViewFor(ic.nativeTex);
    if (!v) return false;
    const float bw = max.x - min.x, bh = max.y - min.y;
    float w = bw, h = bw / ic.aspect;
    if (h > bh) h = bh, w = bh * ic.aspect;
    const ImVec2 a(min.x + (bw - w) * 0.5f, min.y + (bh - h) * 0.5f), b(a.x + w, a.y + h);
    const bool gamma = v->srgb && g_gammaPs;
    if (gamma) dl->AddCallback(&SetGammaShader, nullptr);
    dl->AddImage(ImTextureRef(static_cast<ImTextureID>(reinterpret_cast<intptr_t>(v->srv))), a, b, ImVec2(ic.u0, ic.v0),
                 ImVec2(ic.u1, ic.v1), tint);
    if (gamma) dl->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
    return true;
}

}  // namespace sr2::ui::item_icons

#endif  // SR2_HAVE_IMGUI
