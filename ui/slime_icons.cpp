#if defined(SR2_HAVE_IMGUI)

#include "slime_icons.h"

#include <windows.h>

#include <d3d11.h>
#include <d3dcompiler.h>

#include <atomic>
#include <cstring>
#include <mutex>
#include <random>
#include <string>
#include <vector>

#include "backends/imgui_impl_dx11.h"
#include "core/log.h"
#include "core/main_thread.h"
#include "core/unity.h"
#include "il2cpp_util.h"

namespace sr2::ui::slime_icons {
namespace {

namespace unity = ::sr2::core::unity;

// ------------------------------------------------- main thread: discovery ----
// One slime icon as Unity describes it. The texture is kept alive by a GC
// handle; its D3D11 resource by our shader resource view (render thread).
struct Found {
    std::string name;
    void* nativeTex = nullptr;  // ID3D11Resource* (Texture.GetNativeTexturePtr)
    float x = 0, y = 0, w = 0, h = 0;  // Sprite.textureRect in pixels, origin bottom-left
    int texW = 0, texH = 0;
};

std::mutex g_mutex;
std::vector<Found> g_found;            // guarded by g_mutex
std::atomic<bool> g_searching{false};  // a search is posted or running
std::atomic<unsigned long long> g_lastSearch{0};

struct UnityRect {
    float x, y, w, h;
};

std::string DisplayName(std::string asset) {
    if (asset.find("Slime") == std::string::npos) asset += " Slime";
    return asset;
}

// Every SlimeDefinition with an icon (largos skipped: the logo is one slime).
void Search() {
    std::string err;
    Il2CppClass* ident = il::Class("Assembly-CSharp", "", "IdentifiableType", &err);
    Il2CppClass* sprite = il::Class("UnityEngine.CoreModule", "UnityEngine", "Sprite", &err);
    Il2CppClass* texture = il::Class("UnityEngine.CoreModule", "UnityEngine", "Texture", &err);
    size_t offIcon = 0;
    if (!ident || !sprite || !texture || !il::FieldOffsetInHierarchy(ident, "icon", offIcon, &err)) {
        SR2_WARN("ui: slime icons unavailable: {}", err.empty() ? "classes not found" : err);
        return;
    }
    const MethodInfo* getTexture = il::Method(sprite, "get_texture", 0, &err);
    const MethodInfo* getRect = il::Method(sprite, "get_textureRect", 0, &err);
    const MethodInfo* getWidth = il::MethodInHierarchy(texture, "get_width", 0, &err);
    const MethodInfo* getHeight = il::MethodInHierarchy(texture, "get_height", 0, &err);
    const MethodInfo* getNative = il::Method(texture, "GetNativeTexturePtr", 0, &err);
    if (!getTexture || !getRect || !getWidth || !getHeight || !getNative) {
        SR2_WARN("ui: slime icons unavailable: {}", err);
        return;
    }

    std::vector<Found> out;
    std::vector<void*> textures;  // dedupe GC handles per texture
    for (Il2CppObject* o : unity::FindObjectsOfTypeAll(ident)) {
        if (il::AnyFault()) break;
        const std::string cls = unity::ClassName(o);
        if (cls != "SlimeDefinition" && !(cls.size() > 16 && cls.ends_with(".SlimeDefinition"))) continue;
        size_t offLargo = 0;
        if (il::FieldOffsetInHierarchy(il::ClassOf(o), "IsLargo", offLargo) && unity::Field<bool>(o, offLargo)) continue;
        Il2CppObject* spr = unity::Field<Il2CppObject*>(o, offIcon);
        if (!spr || !unity::IsAlive(spr)) continue;
        Il2CppObject* tex = il::Invoke(getTexture, spr, {});
        if (!tex || !unity::IsAlive(tex)) continue;
        Found f;
        UnityRect r{};
        void* native = nullptr;
        // textureRect throws for tightly packed atlas sprites; those are skipped.
        if (!il::UnboxTo(il::Invoke(getRect, spr, {}), r) || r.w <= 0 || r.h <= 0) continue;
        if (!il::UnboxTo(il::Invoke(getWidth, tex, {}), f.texW) || !il::UnboxTo(il::Invoke(getHeight, tex, {}), f.texH)) continue;
        if (!il::UnboxTo(il::Invoke(getNative, tex, {}), native) || !native || f.texW <= 0 || f.texH <= 0) continue;
        f.name = DisplayName(unity::GetName(o));
        f.nativeTex = native;
        f.x = r.x, f.y = r.y, f.w = r.w, f.h = r.h;
        bool known = false;
        for (void* t : textures) known |= t == native;
        if (!known) {
            il::Keep(tex);  // never unloaded while we draw from it
            textures.push_back(native);
        }
        out.push_back(std::move(f));
    }
    SR2_INFO("ui: {} slime icons found for the overlay logo", out.size());
    std::lock_guard lk(g_mutex);
    g_found = std::move(out);
}

// ------------------------------------------------ render thread: drawing ----
struct Icon {
    std::string name;
    ID3D11ShaderResourceView* srv = nullptr;
    ImVec2 uv0, uv1;
    float aspect = 1.0f;
    bool srgb = false;  // sampled as linear: needs the gamma shader
};

ID3D11Device* g_device = nullptr;
std::vector<Icon> g_icons;
size_t g_synced = 0;  // g_found entries already turned into icons
int g_current = -1;
std::mt19937 g_rng{std::random_device{}()};
ID3D11PixelShader* g_gammaPs = nullptr;
bool g_psTried = false;

// View format for a texture: typeless -> UNORM (no conversion), typed sRGB
// stays (then `srgb`: the texels arrive linear and the gamma shader undoes it).
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
void CreateGammaShader() {
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
        SR2_WARN("ui: slime icon shader did not compile; sRGB icons will look darker");
        if (errors) errors->Release();
        return;
    }
    if (FAILED(g_device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &g_gammaPs))) g_gammaPs = nullptr;
    blob->Release();
}

void SetGammaShader(const ImDrawList*, const ImDrawCmd*) {
    auto* rs = static_cast<ImGui_ImplDX11_RenderState*>(ImGui::GetPlatformIO().Renderer_RenderState);
    if (rs && g_gammaPs) rs->DeviceContext->PSSetShader(g_gammaPs, nullptr, 0);
}

void Pick() {
    if (g_icons.empty()) {
        g_current = -1;
        return;
    }
    if (g_icons.size() == 1) {
        g_current = 0;
        return;
    }
    int next = g_current;
    while (next == g_current) next = static_cast<int>(g_rng() % g_icons.size());
    g_current = next;
}

// Turns newly found icons into shader resource views.
void Sync() {
    std::vector<Found> fresh;
    {
        std::lock_guard lk(g_mutex);
        if (g_found.size() <= g_synced) return;
        fresh.assign(g_found.begin() + static_cast<std::ptrdiff_t>(g_synced), g_found.end());
        g_synced = g_found.size();
    }
    if (!g_device) return;
    for (const Found& f : fresh) {
        ID3D11Texture2D* tex = nullptr;
        auto* res = static_cast<ID3D11Resource*>(f.nativeTex);
        if (FAILED(res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&tex))) || !tex) continue;
        D3D11_TEXTURE2D_DESC td{};
        tex->GetDesc(&td);
        Icon icon;
        D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format = ViewFormat(td.Format, icon.srgb);
        sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        sd.Texture2D.MostDetailedMip = 0;
        sd.Texture2D.MipLevels = td.MipLevels ? td.MipLevels : static_cast<UINT>(-1);
        const bool ok = td.ArraySize == 1 && SUCCEEDED(g_device->CreateShaderResourceView(tex, &sd, &icon.srv));
        tex->Release();
        if (!ok || !icon.srv) continue;
        // Unity uploads rows bottom-up, so Unity's v (up from the bottom) is the D3D v here:
        // the top edge of the sprite is (y + h) / texH. Half a texel in, against atlas bleeding.
        const float tw = static_cast<float>(f.texW), th = static_cast<float>(f.texH);
        icon.uv0 = ImVec2((f.x + 0.5f) / tw, (f.y + f.h - 0.5f) / th);
        icon.uv1 = ImVec2((f.x + f.w - 0.5f) / tw, (f.y + 0.5f) / th);
        icon.aspect = f.w / f.h;
        icon.name = f.name;
        g_icons.push_back(std::move(icon));
    }
    if (g_current < 0) Pick();
}

}  // namespace

void Init(ID3D11Device* device) { g_device = device; }

void OnOpen() {
    Sync();
    Pick();
    const unsigned long long now = GetTickCount64();
    if (!g_icons.empty() || g_searching.load() || !core::main_thread::Ready() || il::AnyFault()) return;
    if (g_lastSearch.load() != 0 && now - g_lastSearch.load() < 15000) return;  // retry later (the definitions load with the game)
    g_lastSearch = now;
    g_searching = true;
    core::main_thread::Post([] {
        Search();
        g_searching = false;
    });
}

bool Draw(ImDrawList* dl, ImVec2 min, ImVec2 max) {
    Sync();
    if (g_current < 0 || g_current >= static_cast<int>(g_icons.size())) return false;
    const Icon& icon = g_icons[static_cast<size_t>(g_current)];
    const float bw = max.x - min.x, bh = max.y - min.y;
    float w = bw, h = bw / icon.aspect;
    if (h > bh) h = bh, w = bh * icon.aspect;
    const ImVec2 a(min.x + (bw - w) * 0.5f, min.y + (bh - h) * 0.5f), b(a.x + w, a.y + h);
    if (icon.srgb && !g_psTried) CreateGammaShader();
    const bool gamma = icon.srgb && g_gammaPs;
    if (gamma) dl->AddCallback(&SetGammaShader, nullptr);
    dl->AddImage(ImTextureRef(static_cast<ImTextureID>(reinterpret_cast<intptr_t>(icon.srv))), a, b, icon.uv0, icon.uv1);
    if (gamma) dl->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
    return true;
}

std::string CurrentName() {
    return g_current >= 0 && g_current < static_cast<int>(g_icons.size()) ? g_icons[static_cast<size_t>(g_current)].name
                                                                           : std::string();
}

}  // namespace sr2::ui::slime_icons

#endif  // SR2_HAVE_IMGUI
