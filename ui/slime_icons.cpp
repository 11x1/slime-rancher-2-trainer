#if defined(SR2_HAVE_IMGUI)

#include "slime_icons.h"

#include <windows.h>

#include <d3d11.h>
#include <d3dcompiler.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <map>
#include <mutex>
#include <random>
#include <string>
#include <vector>

#include "backends/imgui_impl_dx11.h"
#include "core/il2cpp_api.h"
#include "core/log.h"
#include "core/main_thread.h"
#include "core/resolver.h"
#include "core/unity.h"
#include "il2cpp_util.h"

namespace sr2::ui::slime_icons {
namespace {

namespace unity = ::sr2::core::unity;

constexpr const char* kImage = "Assembly-CSharp";
constexpr const char* kCore = "UnityEngine.CoreModule";
constexpr const char* kPedia = "MonomiPark.SlimeRancher.Pedia";

// ------------------------------------------------ main thread: symbols ----
// Everything by name (see re/symbols.md, "UI: Slimepedia"). Resolved once; if
// anything required is missing the logo stays the drawn fallback.
struct Syms {
    const MethodInfo* sceneInstance = nullptr;  // SRSingleton<SceneContext>.get_Instance()
    size_t scPedia = 0, scGameModel = 0;
    Il2CppClass* director = nullptr;
    size_t dirConfig = 0, dirModel = 0, modelUnlocked = 0;
    const MethodInfo* isUnlocked = nullptr;  // PediaDirector.IsUnlocked(PediaEntry)
    Il2CppClass* category = nullptr;
    size_t cfgCategories = 0, catItems = 0;
    Il2CppClass* identEntry = nullptr;  // IdentifiablePediaEntry
    size_t entryDetails = 0, entryTitle = 0, entryDesc = 0, entryIdent = 0;
    Il2CppClass* detail = nullptr;
    size_t detailSection = 0, detailText = 0;
    Il2CppClass* section = nullptr;
    size_t sectionTitle = 0;
    Il2CppClass* slimeDef = nullptr;
    size_t slimeIsLargo = 0, identIcon = 0;
    size_t identLocName = 0;
    bool hasIdentLocName = false;
    Il2CppClass* locString = nullptr;
    const MethodInfo* locIsEmpty = nullptr;  // optional
    const MethodInfo* locGet = nullptr;
    const MethodInfo* sprTexture = nullptr;
    const MethodInfo* sprRect = nullptr;
    const MethodInfo* sprPacked = nullptr;       // optional (with the two below)
    const MethodInfo* sprPackingMode = nullptr;  // SpritePackingMode: Tight = 0
    const MethodInfo* sprUv = nullptr;
    const MethodInfo* texWidth = nullptr;
    const MethodInfo* texHeight = nullptr;
    const MethodInfo* texNative = nullptr;
};

Syms S;
bool g_symsTried = false, g_symsOk = false;

bool Resolve() {
    if (g_symsTried) return g_symsOk;
    g_symsTried = true;
    std::string err;
    auto cls = [&](const char* img, const char* ns, const char* name) -> Il2CppClass* {
        return err.empty() ? il::Class(img, ns, name, &err) : nullptr;
    };
    auto off = [&](Il2CppClass* k, const char* name, size_t& out) {
        if (err.empty()) il::FieldOffsetInHierarchy(k, name, out, &err);
    };
    auto method = [&](Il2CppClass* k, const char* name, int argc) -> const MethodInfo* {
        return err.empty() ? il::MethodInHierarchy(k, name, argc, &err) : nullptr;
    };

    Il2CppClass* scene = cls(kImage, "", "SceneContext");
    S.director = cls(kImage, "", "PediaDirector");
    Il2CppClass* model = cls(kImage, "MonomiPark.SlimeRancher.DataModel", "PediaModel");
    Il2CppClass* config = cls(kImage, kPedia, "PediaConfiguration");
    S.category = cls(kImage, kPedia, "PediaCategory");
    S.identEntry = cls(kImage, kPedia, "IdentifiablePediaEntry");
    S.detail = cls(kImage, kPedia, "PediaEntryDetail");
    S.section = cls(kImage, kPedia, "PediaDetailSection");
    S.slimeDef = cls(kImage, "", "SlimeDefinition");
    Il2CppClass* ident = cls(kImage, "", "IdentifiableType");
    S.locString = cls("Unity.Localization", "UnityEngine.Localization", "LocalizedString");
    Il2CppClass* sprite = cls(kCore, "UnityEngine", "Sprite");
    Il2CppClass* texture = cls(kCore, "UnityEngine", "Texture");
    auto parentOf = il2cpp::api().il2cpp_class_get_parent;
    if (err.empty() && (!parentOf || !parentOf(scene))) err = "SceneContext parent class not found";

    off(scene, "<PediaDirector>k__BackingField", S.scPedia);
    off(scene, "<GameModel>k__BackingField", S.scGameModel);
    off(S.director, "_pediaConfiguration", S.dirConfig);
    off(S.director, "_pediaModel", S.dirModel);
    off(model, "unlocked", S.modelUnlocked);
    off(config, "_categoryList", S.cfgCategories);
    off(S.category, "_items", S.catItems);
    off(S.identEntry, "_details", S.entryDetails);
    off(S.identEntry, "_title", S.entryTitle);
    off(S.identEntry, "_description", S.entryDesc);
    off(S.identEntry, "_identifiableType", S.entryIdent);
    off(S.detail, "Section", S.detailSection);
    off(S.detail, "Text", S.detailText);
    off(S.section, "_title", S.sectionTitle);
    off(S.slimeDef, "IsLargo", S.slimeIsLargo);
    off(ident, "icon", S.identIcon);

    S.sceneInstance = method(err.empty() ? parentOf(scene) : nullptr, "get_Instance", 0);
    if (err.empty()) {
        S.isUnlocked = il2cpp::Resolver::Get().MethodByParams(S.director, "IsUnlocked", {"MonomiPark.SlimeRancher.Pedia.PediaEntry"}, &err);
    }
    S.locGet = method(S.locString, "GetLocalizedString", 0);
    S.sprTexture = method(sprite, "get_texture", 0);
    S.sprRect = method(sprite, "get_textureRect", 0);
    S.texWidth = method(texture, "get_width", 0);
    S.texHeight = method(texture, "get_height", 0);
    S.texNative = method(texture, "GetNativeTexturePtr", 0);
    if (!err.empty()) {
        SR2_WARN("ui: Slimepedia logo unavailable, using the drawn slime: {}", err);
        return false;
    }

    // Optional: localized slime name as a fallback title, the empty check before
    // reading a LocalizedString, and the UV path for tightly packed sprites.
    S.hasIdentLocName = il::FieldOffsetInHierarchy(ident, "localizedName", S.identLocName);
    S.locIsEmpty = il::MethodInHierarchy(S.locString, "get_IsEmpty", 0);
    S.sprPacked = il::Method(sprite, "get_packed", 0);
    S.sprPackingMode = il::Method(sprite, "get_packingMode", 0);
    S.sprUv = il::Method(sprite, "get_uv", 0);
    g_symsOk = true;
    return true;
}

// ------------------------------------------- main thread: reading entries ----
// The picked entry's icon as Unity describes it. The texture is kept alive by a
// GC handle; its D3D11 resource by our shader resource view (render thread).
struct Snapshot {
    unsigned serial = 0;
    bool has = false;
    Entry entry;
    void* nativeTex = nullptr;  // ID3D11Resource* (Texture.GetNativeTexturePtr)
    float x = 0, y = 0, w = 0, h = 0;  // sprite rect in texels, origin bottom-left
    int texW = 0, texH = 0;
};

struct UnityRect {
    float x, y, w, h;
};
struct Vec2 {
    float x, y;
};

std::mutex g_mutex;
Snapshot g_snap;                       // guarded by g_mutex
std::atomic<bool> g_busy{false};       // a refresh is posted or running
std::vector<void*> g_keptTextures;     // main thread: native textures already held by a GC handle
Il2CppObject* g_lastPick = nullptr;    // main thread: identity only, never dereferenced
std::mt19937 g_pickRng{std::random_device{}()};
bool g_loggedCount = false;
size_t g_lastCount = static_cast<size_t>(-1);

// Drops rich-text tags (<b>, <color=...>, <sprite ...>) and trims.
std::string CleanText(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '<') {
            const size_t end = s.find('>', i + 1);
            if (end != std::string::npos && end - i <= 96 && s.find('\n', i) > end) {
                i = end;
                continue;
            }
        }
        if (s[i] != '\r') out += s[i];
    }
    const size_t a = out.find_first_not_of(" \t\n");
    if (a == std::string::npos) return {};
    const size_t b = out.find_last_not_of(" \t\n");
    return out.substr(a, b - a + 1);
}

std::string Localized(Il2CppObject* ls) {
    if (!ls || !il::IsA(ls, S.locString)) return {};
    bool empty = false;
    if (S.locIsEmpty && il::UnboxTo(il::Invoke(S.locIsEmpty, ls, {}), empty) && empty) return {};
    Il2CppObject* r = il::Invoke(S.locGet, ls, {});
    return r ? CleanText(unity::ToUtf8(reinterpret_cast<Il2CppString*>(r))) : std::string();
}

bool Unlocked(Il2CppObject* director, Il2CppObject* entry) {
    bool yes = false;
    return il::UnboxTo(il::Invoke(S.isUnlocked, director, {entry}), yes) && yes;
}

// A slime the logo can show: an IdentifiablePediaEntry for a non-largo SlimeDefinition.
bool IsSlimeEntry(Il2CppObject* e) {
    if (!e || !unity::IsAlive(e) || !il::IsA(e, S.identEntry)) return false;
    Il2CppObject* id = unity::Field<Il2CppObject*>(e, S.entryIdent);
    return id && unity::IsAlive(id) && il::IsA(id, S.slimeDef) && !unity::Field<bool>(id, S.slimeIsLargo);
}

void AddUnique(std::vector<Il2CppObject*>& v, Il2CppObject* o) {
    if (std::find(v.begin(), v.end(), o) == v.end()) v.push_back(o);
}

// The slime entries the Slimepedia lists: its categories' item arrays. If that
// finds none, every IdentifiablePediaEntry asset loaded.
std::vector<Il2CppObject*> SlimeEntries(Il2CppObject* director) {
    std::vector<Il2CppObject*> out;
    Il2CppObject* cfg = unity::Field<Il2CppObject*>(director, S.dirConfig);
    void* cats = (cfg && unity::IsAlive(cfg)) ? unity::Field<void*>(cfg, S.cfgCategories) : nullptr;
    for (size_t i = 0, n = unity::ArrayLength(cats); i < n; ++i) {
        auto* cat = static_cast<Il2CppObject*>(unity::ArrayAt(cats, i));
        if (!cat || !unity::IsAlive(cat) || !il::IsA(cat, S.category)) continue;
        void* items = unity::Field<void*>(cat, S.catItems);
        for (size_t j = 0, m = unity::ArrayLength(items); j < m; ++j) {
            auto* e = static_cast<Il2CppObject*>(unity::ArrayAt(items, j));
            if (IsSlimeEntry(e)) AddUnique(out, e);
        }
    }
    if (out.empty()) {
        for (Il2CppObject* e : unity::FindObjectsOfTypeAll(S.identEntry))
            if (IsSlimeEntry(e)) AddUnique(out, e);
    }
    return out;
}

// Fills the icon part of `s` from the entry's list icon. False if it can't be drawn.
bool ReadIcon(Il2CppObject* entry, Snapshot& s) {
    Il2CppObject* spr = nullptr;
    if (const MethodInfo* getIcon = il::MethodInHierarchy(il::ClassOf(entry), "get_Icon", 0))
        spr = il::Invoke(getIcon, entry, {});
    if (!spr || !unity::IsAlive(spr)) {
        Il2CppObject* id = unity::Field<Il2CppObject*>(entry, S.entryIdent);
        spr = id ? unity::Field<Il2CppObject*>(id, S.identIcon) : nullptr;
    }
    if (!spr || !unity::IsAlive(spr)) return false;
    Il2CppObject* tex = il::Invoke(S.sprTexture, spr, {});
    if (!tex || !unity::IsAlive(tex)) return false;
    void* native = nullptr;
    if (!il::UnboxTo(il::Invoke(S.texWidth, tex, {}), s.texW) || !il::UnboxTo(il::Invoke(S.texHeight, tex, {}), s.texH)) return false;
    if (!il::UnboxTo(il::Invoke(S.texNative, tex, {}), native) || !native || s.texW <= 0 || s.texH <= 0) return false;

    // textureRect throws for tightly packed atlas sprites; for those the bounds of
    // the mesh UVs give the same rectangle (plus whatever transparent margin).
    bool packed = false;
    int mode = 1;
    const bool tight = S.sprPacked && S.sprPackingMode && S.sprUv && il::UnboxTo(il::Invoke(S.sprPacked, spr, {}), packed) &&
                       packed && il::UnboxTo(il::Invoke(S.sprPackingMode, spr, {}), mode) && mode == 0;
    if (tight) {
        void* uv = il::Invoke(S.sprUv, spr, {});
        const size_t n = unity::ArrayLength(uv);
        if (n == 0) return false;
        const Vec2* p = unity::ArrayData<Vec2>(uv);
        float u0 = 1, v0 = 1, u1 = 0, v1 = 0;
        for (size_t i = 0; i < n; ++i) {
            u0 = std::min(u0, p[i].x), v0 = std::min(v0, p[i].y);
            u1 = std::max(u1, p[i].x), v1 = std::max(v1, p[i].y);
        }
        s.x = u0 * s.texW, s.y = v0 * s.texH, s.w = (u1 - u0) * s.texW, s.h = (v1 - v0) * s.texH;
    } else {
        UnityRect r{};
        if (!il::UnboxTo(il::Invoke(S.sprRect, spr, {}), r)) return false;
        s.x = r.x, s.y = r.y, s.w = r.w, s.h = r.h;
    }
    if (s.w <= 0 || s.h <= 0) return false;
    s.nativeTex = native;
    if (std::find(g_keptTextures.begin(), g_keptTextures.end(), native) == g_keptTextures.end()) {
        il::Keep(tex);  // never unloaded while we draw from it
        g_keptTextures.push_back(native);
    }
    return true;
}

// Name and sections as the Slimepedia page shows them, titles from the data.
Entry ReadEntry(Il2CppObject* entry) {
    Entry out;
    out.name = Localized(unity::Field<Il2CppObject*>(entry, S.entryTitle));
    Il2CppObject* id = unity::Field<Il2CppObject*>(entry, S.entryIdent);
    if (out.name.empty() && id && S.hasIdentLocName) out.name = Localized(unity::Field<Il2CppObject*>(id, S.identLocName));
    if (out.name.empty() && id) {
        out.name = unity::GetName(id);
        if (out.name.find("Slime") == std::string::npos) out.name += " Slime";
    }
    if (std::string intro = Localized(unity::Field<Il2CppObject*>(entry, S.entryDesc)); !intro.empty())
        out.sections.push_back({std::string(), std::move(intro)});
    void* details = unity::Field<void*>(entry, S.entryDetails);
    for (size_t i = 0, n = unity::ArrayLength(details); i < n; ++i) {
        auto* d = static_cast<Il2CppObject*>(unity::ArrayAt(details, i));
        if (!d || !il::IsA(d, S.detail)) continue;
        Section sec;
        sec.text = Localized(unity::Field<Il2CppObject*>(d, S.detailText));
        Il2CppObject* sc = unity::Field<Il2CppObject*>(d, S.detailSection);
        if (sc && unity::IsAlive(sc) && il::IsA(sc, S.section)) sec.title = Localized(unity::Field<Il2CppObject*>(sc, S.sectionTitle));
        if (!sec.text.empty()) out.sections.push_back(std::move(sec));
    }
    return out;
}

// The loaded save's PediaDirector with a model, or nullptr (title screen, loading).
Il2CppObject* SaveDirector() {
    Il2CppObject* sc = il::Invoke(S.sceneInstance, nullptr, {});
    if (!sc || !unity::IsAlive(sc)) return nullptr;
    Il2CppObject* gm = unity::Field<Il2CppObject*>(sc, S.scGameModel);
    if (!gm || !unity::IsAlive(gm)) return nullptr;
    Il2CppObject* dir = unity::Field<Il2CppObject*>(sc, S.scPedia);
    if (!dir || !unity::IsAlive(dir) || !il::IsA(dir, S.director)) return nullptr;
    Il2CppObject* model = unity::Field<Il2CppObject*>(dir, S.dirModel);
    if (!model || !unity::Field<Il2CppObject*>(model, S.modelUnlocked)) return nullptr;  // IsUnlocked would throw
    return dir;
}

void Publish(Snapshot s) {
    std::lock_guard lk(g_mutex);
    s.serial = g_snap.serial + 1;
    g_snap = std::move(s);
}

// Main thread, on every overlay open: re-reads what the save has discovered
// (it changes during play) and picks one slime that differs from the last pick.
void Refresh() {
    Snapshot s;
    if (il::AnyFault() || !Resolve()) return Publish(std::move(s));
    Il2CppObject* dir = SaveDirector();
    if (!dir) return Publish(std::move(s));

    std::vector<Il2CppObject*> found;
    for (Il2CppObject* e : SlimeEntries(dir)) {
        if (il::AnyFault()) return Publish(Snapshot{});
        if (Unlocked(dir, e)) found.push_back(e);
    }
    if (found.size() != g_lastCount) {
        g_lastCount = found.size();
        if (!g_loggedCount || found.empty()) SR2_INFO("ui: overlay logo: {} discovered Slimepedia slimes", found.size());
        g_loggedCount = true;
    }
    std::shuffle(found.begin(), found.end(), g_pickRng);
    if (found.size() > 1) {
        auto last = std::find(found.begin(), found.end(), g_lastPick);
        if (last != found.end()) std::rotate(last, last + 1, found.end());  // last pick goes to the back
    }
    for (Il2CppObject* e : found) {
        if (il::AnyFault()) break;
        if (!ReadIcon(e, s)) continue;
        s.entry = ReadEntry(e);
        s.has = true;
        g_lastPick = e;
        break;
    }
    Publish(std::move(s));
}

// ------------------------------------------------ render thread: drawing ----
struct View {
    ID3D11ShaderResourceView* srv = nullptr;
    bool srgb = false;  // sampled as linear: needs the gamma shader
};

struct Icon {
    View view;
    ImVec2 uv0, uv1;
    float aspect = 1.0f;
};

ID3D11Device* g_device = nullptr;
std::map<void*, View> g_views;  // per native texture, kept for later picks
bool g_hasIcon = false;
Icon g_icon;
Entry g_entry;
unsigned g_synced = 0;          // snapshot serial already applied
bool g_waiting = false;         // OnOpen asked for a new pick that has not arrived yet
unsigned long long g_waitStart = 0;
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

// Shader resource view for a native texture, created once and reused.
const View* ViewFor(void* nativeTex) {
    if (auto it = g_views.find(nativeTex); it != g_views.end()) return it->second.srv ? &it->second : nullptr;
    View& v = g_views[nativeTex];  // a failure is remembered as an empty view
    ID3D11Texture2D* tex = nullptr;
    auto* res = static_cast<ID3D11Resource*>(nativeTex);
    if (FAILED(res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&tex))) || !tex) return nullptr;
    D3D11_TEXTURE2D_DESC td{};
    tex->GetDesc(&td);
    D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format = ViewFormat(td.Format, v.srgb);
    sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    sd.Texture2D.MostDetailedMip = 0;
    sd.Texture2D.MipLevels = td.MipLevels ? td.MipLevels : static_cast<UINT>(-1);
    if (td.ArraySize != 1 || FAILED(g_device->CreateShaderResourceView(tex, &sd, &v.srv))) v.srv = nullptr;
    tex->Release();
    return v.srv ? &v : nullptr;
}

// Applies a newer snapshot from the main thread.
void Sync() {
    Snapshot s;
    {
        std::lock_guard lk(g_mutex);
        if (g_snap.serial == g_synced) return;
        s = g_snap;
        g_synced = g_snap.serial;
    }
    g_waiting = false;
    g_hasIcon = false;
    g_entry = std::move(s.entry);
    if (!s.has || !g_device) return;
    const View* v = ViewFor(s.nativeTex);
    if (!v) return;
    g_icon.view = *v;
    // Unity uploads rows bottom-up, so Unity's v (up from the bottom) is the D3D v here:
    // the top edge of the sprite is (y + h) / texH. Half a texel in, against atlas bleeding.
    const float tw = static_cast<float>(s.texW), th = static_cast<float>(s.texH);
    g_icon.uv0 = ImVec2((s.x + 0.5f) / tw, (s.y + s.h - 0.5f) / th);
    g_icon.uv1 = ImVec2((s.x + s.w - 0.5f) / tw, (s.y + 0.5f) / th);
    g_icon.aspect = s.w / s.h;
    g_hasIcon = true;
}

}  // namespace

void Init(ID3D11Device* device) { g_device = device; }

void OnOpen() {
    Sync();
    if (g_busy.load() || !core::main_thread::Ready() || il::AnyFault()) {
        // No main thread to ask: no way to know which save is loaded.
        if (!g_busy.load()) g_hasIcon = false, g_entry = {};
        return;
    }
    g_busy = true;
    g_waiting = true;
    g_waitStart = GetTickCount64();
    core::main_thread::Post([] {
        Refresh();
        g_busy = false;
    });
}

bool Draw(ImDrawList* dl, ImVec2 min, ImVec2 max) {
    Sync();
    if (g_waiting) {
        if (GetTickCount64() - g_waitStart < 500) return true;  // the new pick is a frame or two away
        g_waiting = false;
        g_hasIcon = false;
        g_entry = {};
    }
    if (!g_hasIcon) return false;
    const float bw = max.x - min.x, bh = max.y - min.y;
    float w = bw, h = bw / g_icon.aspect;
    if (h > bh) h = bh, w = bh * g_icon.aspect;
    const ImVec2 a(min.x + (bw - w) * 0.5f, min.y + (bh - h) * 0.5f), b(a.x + w, a.y + h);
    if (g_icon.view.srgb && !g_psTried) CreateGammaShader();
    const bool gamma = g_icon.view.srgb && g_gammaPs;
    if (gamma) dl->AddCallback(&SetGammaShader, nullptr);
    dl->AddImage(ImTextureRef(static_cast<ImTextureID>(reinterpret_cast<intptr_t>(g_icon.view.srv))), a, b, g_icon.uv0, g_icon.uv1);
    if (gamma) dl->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
    return true;
}

const Entry* CurrentEntry() { return (g_hasIcon && !g_waiting && !g_entry.name.empty()) ? &g_entry : nullptr; }

}  // namespace sr2::ui::slime_icons

#endif  // SR2_HAVE_IMGUI
