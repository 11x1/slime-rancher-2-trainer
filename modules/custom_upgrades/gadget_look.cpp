// custom_upgrades: game-UI looks of the mod's gadget definitions (see gadget_look.h).

#include "modules/custom_upgrades/gadget_look.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <format>
#include <initializer_list>
#include <map>
#include <mutex>
#include <set>
#include <vector>

#include "core/detour_guard.h"
#include "core/il2cpp_api.h"
#include "core/log.h"
#include "core/main_thread.h"
#include "core/module.h"
#include "core/resolver.h"
#include "core/unity.h"

namespace sr2::modules::custom_upgrades::gadget_look {
namespace {

namespace unity = sr2::core::unity;
namespace main_thread = sr2::core::main_thread;
using sr2::il2cpp::api;
using sr2::il2cpp::Resolver;

constexpr const char* kImage = "Assembly-CSharp";
constexpr const char* kCore = "UnityEngine.CoreModule";
constexpr const char* kTag = " [sr2look]";
constexpr size_t kMax = 24;

struct Vec2 {
    float x, y;
};
struct Rect {
    float x, y, w, h;
};
struct Color {
    float r, g, b, a;
};
struct Color32 {
    uint8_t r, g, b, a;
};
// GadgetDirector.TargetGadgetInfo {LocalizedString DisplayName, SubName; Sprite Icon; int ItemCount}
struct TargetInfo {
    Il2CppObject* displayName;
    Il2CppObject* subName;
    Il2CppObject* icon;
    int32_t itemCount;
};

// Registered definitions: pointer compares from any thread (description detour).
struct Slot {
    std::atomic<Il2CppObject*> def{nullptr};
    std::atomic<Il2CppObject*> name{nullptr};
    std::atomic<Il2CppObject*> desc{nullptr};
    std::atomic<Il2CppObject*> icon{nullptr};
};
Slot g_slots[kMax];

Slot* Find(Il2CppObject* def) {
    if (!def) return nullptr;
    for (Slot& s : g_slots)
        if (s.def.load(std::memory_order_acquire) == def) return &s;
    return nullptr;
}

using DescFn = Il2CppObject* (*)(Il2CppObject*, const MethodInfo*);
using InfoFn = TargetInfo* (*)(TargetInfo*, Il2CppObject*, Il2CppObject*, bool, bool, uint64_t, const MethodInfo*);
using HeldFn = void (*)(Il2CppObject*, Il2CppObject*, const MethodInfo*);
using SetItemFn = void (*)(Il2CppObject*, Il2CppObject*, const MethodInfo*);
SetItemFn g_origSetItem = nullptr;
DescFn g_origDesc = nullptr;
InfoFn g_origInfo = nullptr;
HeldFn g_origHeld = nullptr;

struct Impl {
    const char* module = "custom_upgrades";
    bool armed = false;
    // main thread state
    std::map<Il2CppObject*, std::function<void(Il2CppObject*)>> dress;
    std::mutex placedMu;  // AddPlacedLabel may come from the worker thread
    std::vector<PlacedLabel> placed;
    std::set<std::string> loggedShaders;
    // offsets
    size_t offFullArt = 0, offRequiresFullArt = 0, offDetailsIcon = 0, offFabDef = 0;
    Il2CppClass* fabItemClass = nullptr;
    const MethodInfo* imageSetSprite = nullptr;
    size_t offHeld = 0, offPlaceholder = 0, offTargeted = 0, offTargetingPlaceholder = 0, offSelected = 0;
    const void* obsClass = nullptr;
    size_t obsValue = 0;
    // methods
    const MethodInfo* identTypeAsDef = nullptr;
    const MethodInfo* rendGetMaterials = nullptr, *matPropNames = nullptr, *matGetColor = nullptr, *matSetColor = nullptr,
                      *matGetShader = nullptr, *compsInChildren = nullptr;
    const MethodInfo* spriteTexture = nullptr, *spriteTexRect = nullptr, *spriteTexOffset = nullptr, *spriteRect = nullptr, *spritePivot = nullptr,
                      *spritePpu = nullptr, *spriteCreate = nullptr, *texWidth = nullptr, *texHeight = nullptr,
                      *rtGetTemp = nullptr, *rtRelease = nullptr, *rtGetActive = nullptr, *rtSetActive = nullptr,
                      *blit = nullptr, *tex2dCtor = nullptr, *texRead = nullptr, *texGetPixels = nullptr,
                      *texSetPixels = nullptr, *texApply = nullptr, *setHideFlags = nullptr;
    Il2CppClass* rendererClass = nullptr, *tex2dClass = nullptr;
    using ObjectNewFn = Il2CppObject* (*)(Il2CppClass*);
    ObjectNewFn objectNew = nullptr;

    Il2CppObject* Call(const MethodInfo* m, void* obj, std::initializer_list<void*> args, const char* what,
                       bool* ok = nullptr) {
        if (!m) {
            if (ok) *ok = false;
            return nullptr;
        }
        std::vector<void*> params(args);
        std::string err;
        Il2CppObject* r = Resolver::Get().Invoke(m, obj, params.empty() ? nullptr : params.data(), &err);
        if (ok) *ok = err.empty();
        if (!err.empty()) {
            if (err.rfind("native exception", 0) == 0)
                sr2::core::MarkModuleFaulted(module, std::format("gadget look: {}: {}", what, err));
            else SR2_WARN("[{}] gadget look: {} failed: {}", module, what, err);
        }
        return r;
    }
    template <class T>
    static T Value(Il2CppObject* boxed, T def = T{}) {
        return boxed ? *reinterpret_cast<T*>(reinterpret_cast<char*>(boxed) + 2 * sizeof(void*)) : def;
    }
    bool Faulted() const { return sr2::core::ModuleFaulted(module); }
    // ObservableValue<T>._value (generic class: offset looked up per class).
    Il2CppObject* ObsValue(Il2CppObject* obs) {
        if (!obs) return nullptr;
        Il2CppClass* k = api().il2cpp_object_get_class(obs);
        if (k != obsClass) {
            auto o = Resolver::Get().FieldOffset(k, "_value");
            obsClass = k;
            obsValue = o ? *o : 0;
        }
        return obsValue ? unity::Field<Il2CppObject*>(obs, obsValue) : nullptr;
    }
    Il2CppObject* PlacedLabelFor(Il2CppObject* director, Il2CppObject* def);
};

Impl* g = nullptr;
sr2::core::DetourGuard g_guard{"custom_upgrades", "gadget look"};

// GadgetDefinition.get_LocalizedDescription: a registered definition answers with its own text (the
// original returns the root variant's). Pointer compares only.
Il2CppObject* DescDetour(Il2CppObject* self, const MethodInfo* mi) {
    if (g && g->armed)
        if (Slot* s = Find(self))
            if (Il2CppObject* d = s->desc.load(std::memory_order_acquire)) return d;
    return g_origDesc(self, mi);
}

TargetInfo* InfoDetour(TargetInfo* ret, Il2CppObject* self, Il2CppObject* def, bool includeCount, bool isInRanch,
                       uint64_t droneType, const MethodInfo* mi) {
    TargetInfo* r = g_origInfo(ret, self, def, includeCount, isInRanch, droneType, mi);
    if (!r || !def || !g || !g->armed || !main_thread::IsMainThread() || g->Faulted()) return r;
    if (Slot* s = Find(def)) {
        if (Il2CppObject* n = s->name.load(std::memory_order_acquire)) r->displayName = n;  // stack struct: no barrier
        return r;
    }
    if (!r->displayName || !self) return r;
    Il2CppObject* label = nullptr;
    g_guard.Run([&] { label = g->PlacedLabelFor(self, def); });
    if (label) r->displayName = label;
    return r;
}

// The label is for an aimed-at placed gadget (not the held one, GadgetLabelDisplay.UpdateLabels: held
// when SelectedSlottedGadget is set and _isTargetingPlaceholder) whose definition is `def`.
Il2CppObject* Impl::PlacedLabelFor(Il2CppObject* director, Il2CppObject* def) {
    std::vector<PlacedLabel> providers;
    {
        std::lock_guard<std::mutex> lock(placedMu);
        if (placed.empty()) return nullptr;
        providers = placed;
    }
    if (unity::Field<bool>(director, offTargetingPlaceholder) &&
        unity::IsAlive(ObsValue(unity::Field<Il2CppObject*>(director, offSelected))))
        return nullptr;  // the held gadget's label
    Il2CppObject* gadget = ObsValue(unity::Field<Il2CppObject*>(director, offTargeted));
    if (!unity::IsAlive(gadget)) return nullptr;
    bool ok = false;
    if (Call(identTypeAsDef, gadget, {}, "Gadget.get_IdentTypeAsDefinition", &ok) != def || !ok) return nullptr;
    for (const PlacedLabel& p : providers)
        if (Il2CppObject* l = p(gadget, def)) return l;
    return nullptr;
}

void HeldDetour(Il2CppObject* self, Il2CppObject* def, const MethodInfo* mi) {
    g_origHeld(self, def, mi);
    if (!self || !g || !g->armed || !main_thread::IsMainThread() || g->Faulted() || g->dress.empty()) return;
    g_guard.Run([&] {
        Il2CppObject* held = unity::Field<Il2CppObject*>(self, g->offHeld);
        auto it = held ? g->dress.find(held) : g->dress.end();
        if (it == g->dress.end() || !it->second) return;
        Il2CppObject* ph = unity::Field<Il2CppObject*>(self, g->offPlaceholder);
        if (unity::IsAlive(ph)) it->second(ph);
    });
}

// FabricatorItemDetails.SetItem: a registered definition's card shows its own picture (its full-art
// reference is cleared on Register, so the dynamic loader does not load the base's over it).
void SetItemDetour(Il2CppObject* self, Il2CppObject* item, const MethodInfo* mi) {
    g_origSetItem(self, item, mi);
    if (!self || !item || !g || !g->armed || !main_thread::IsMainThread() || g->Faulted()) return;
    if (api().il2cpp_object_get_class(item) != g->fabItemClass) return;
    Slot* s = Find(unity::Field<Il2CppObject*>(item, g->offFabDef));
    Il2CppObject* icon = s ? s->icon.load(std::memory_order_acquire) : nullptr;
    if (!icon) return;
    g_guard.Run([&] {
        Il2CppObject* image = unity::Field<Il2CppObject*>(self, g->offDetailsIcon);
        if (unity::IsAlive(image)) g->Call(g->imageSetSprite, image, {icon}, "Image.set_sprite");
    });
}

}  // namespace

bool Setup(sr2::core::ModuleContext& ctx, const char* module, std::string& why, std::string& report) {
    Resolver& R = ctx.resolver();
    auto* p = new Impl();
    p->module = module;
    auto parentOf = [](Il2CppClass* c) { return api().il2cpp_class_get_parent ? api().il2cpp_class_get_parent(c) : nullptr; };
    auto cls = [&](const char* image, const char* ns, const char* name) -> Il2CppClass* {
        std::string err;
        Il2CppClass* k = R.Class(image, ns, name, &err);
        if (!k && why.empty()) why = err.empty() ? std::format("class {}.{} not found", ns, name) : err;
        return k;
    };
    auto off = [&](Il2CppClass* k, const char* field, size_t& out) {
        for (Il2CppClass* c = k; c; c = parentOf(c))
            if (auto o = R.FieldOffset(c, field)) {
                out = *o;
                return;
            }
        if (why.empty()) why = std::format("field {} not found", field);
    };
    auto method = [&](Il2CppClass* k, const char* name, int argc) -> const MethodInfo* {
        for (Il2CppClass* c = k; c; c = parentOf(c))
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
    auto fail = [&] {
        delete p;
        return false;
    };
    HMODULE ga = api().module;
    p->objectNew = reinterpret_cast<Impl::ObjectNewFn>(ga ? GetProcAddress(ga, "il2cpp_object_new") : nullptr);
    if (!p->objectNew) why = "il2cpp_object_new export missing";

    Il2CppClass* gadgetDef = cls(kImage, "", "GadgetDefinition");
    Il2CppClass* gadgetDir = cls(kImage, "", "GadgetDirector");
    Il2CppClass* gadgetItem = cls(kImage, "MonomiPark.SlimeRancher.Player.PlayerItems", "GadgetItem");
    Il2CppClass* gadget = cls(kImage, "MonomiPark.SlimeRancher.World", "Gadget");
    p->rendererClass = cls(kCore, "UnityEngine", "Renderer");
    Il2CppClass* material = cls(kCore, "UnityEngine", "Material");
    Il2CppClass* component = cls(kCore, "UnityEngine", "Component");
    Il2CppClass* sprite = cls(kCore, "UnityEngine", "Sprite");
    Il2CppClass* texture = cls(kCore, "UnityEngine", "Texture");
    p->tex2dClass = cls(kCore, "UnityEngine", "Texture2D");
    Il2CppClass* rt = cls(kCore, "UnityEngine", "RenderTexture");
    Il2CppClass* graphics = cls(kCore, "UnityEngine", "Graphics");
    Il2CppClass* uobject = cls(kCore, "UnityEngine", "Object");
    Il2CppClass* identType = cls(kImage, "", "IdentifiableType");
    const char* kFab = "MonomiPark.SlimeRancher.UI.Fabricator";
    p->fabItemClass = cls(kImage, kFab, "GadgetFabricatableItem");
    Il2CppClass* details = cls(kImage, kFab, "FabricatorItemDetails");
    Il2CppClass* image = cls("UnityEngine.UI", "UnityEngine.UI", "Image");
    if (!why.empty()) return fail();
    off(identType, "_fullArt", p->offFullArt);
    off(identType, "_requiresFullArt", p->offRequiresFullArt);
    off(details, "_icon", p->offDetailsIcon);
    off(p->fabItemClass, "<Definition>k__BackingField", p->offFabDef);
    p->imageSetSprite = method(image, "set_sprite", 1);
    off(gadgetItem, "_heldGadget", p->offHeld);
    off(gadgetItem, "_gadgetPlaceholderInstance", p->offPlaceholder);
    off(gadgetDir, "<TargetedGadget>k__BackingField", p->offTargeted);
    off(gadgetDir, "<SelectedSlottedGadget>k__BackingField", p->offSelected);
    off(gadgetDir, "_isTargetingPlaceholder", p->offTargetingPlaceholder);
    p->identTypeAsDef = method(gadget, "get_IdentTypeAsDefinition", 0);
    p->rendGetMaterials = method(p->rendererClass, "get_materials", 0);
    p->matPropNames = byParams(material, "GetPropertyNames", {"UnityEngine.MaterialPropertyType"});
    p->matGetColor = byParams(material, "GetColor", {"System.String"});
    p->matSetColor = byParams(material, "SetColor", {"System.String", "UnityEngine.Color"});
    p->matGetShader = method(material, "get_shader", 0);
    p->compsInChildren = byParams(component, "GetComponentsInChildren", {"System.Type", "System.Boolean"});
    p->spriteTexture = method(sprite, "get_texture", 0);
    p->spriteTexRect = method(sprite, "get_textureRect", 0);
    p->spriteTexOffset = R.Method(sprite, "get_textureRectOffset", 0);  // optional: trimmed sprites keep their size
    p->spriteRect = method(sprite, "get_rect", 0);
    p->spritePivot = method(sprite, "get_pivot", 0);
    p->spritePpu = method(sprite, "get_pixelsPerUnit", 0);
    p->spriteCreate = byParams(sprite, "Create", {"UnityEngine.Texture2D", "UnityEngine.Rect", "UnityEngine.Vector2", "System.Single"});
    p->texWidth = method(texture, "get_width", 0);
    p->texHeight = method(texture, "get_height", 0);
    p->rtGetTemp = byParams(rt, "GetTemporary", {"System.Int32", "System.Int32", "System.Int32",
                                                 "UnityEngine.RenderTextureFormat", "UnityEngine.RenderTextureReadWrite"});
    p->rtRelease = method(rt, "ReleaseTemporary", 1);
    p->rtGetActive = method(rt, "get_active", 0);
    p->rtSetActive = method(rt, "set_active", 1);
    p->blit = byParams(graphics, "Blit", {"UnityEngine.Texture", "UnityEngine.RenderTexture"});
    p->tex2dCtor = byParams(p->tex2dClass, ".ctor",
                            {"System.Int32", "System.Int32", "UnityEngine.TextureFormat", "System.Boolean", "System.Boolean"});
    p->texRead = byParams(p->tex2dClass, "ReadPixels", {"UnityEngine.Rect", "System.Int32", "System.Int32"});
    p->texGetPixels = method(p->tex2dClass, "GetPixels32", 0);
    p->texSetPixels = byParams(p->tex2dClass, "SetPixels32", {"UnityEngine.Color32[]"});
    p->texApply = method(p->tex2dClass, "Apply", 0);
    p->setHideFlags = method(uobject, "set_hideFlags", 1);
    // hooks
    const MethodInfo* descMi = method(gadgetDef, "get_LocalizedDescription", 0);
    const MethodInfo* infoMi = method(gadgetDir, "GetTargetGadgetInfo", 4);
    const MethodInfo* heldMi = method(gadgetItem, "SetHeldGadget", 1);
    const MethodInfo* setItemMi = method(details, "SetItem", 1);
    if (!why.empty()) return fail();
    struct H {
        const char* name;
        const MethodInfo* mi;
        void* detour;
        void** orig;
    };
    const H hooks[] = {
        {"GadgetDefinition.get_LocalizedDescription", descMi, reinterpret_cast<void*>(&DescDetour),
         reinterpret_cast<void**>(&g_origDesc)},
        {"GadgetDirector.GetTargetGadgetInfo", infoMi, reinterpret_cast<void*>(&InfoDetour), reinterpret_cast<void**>(&g_origInfo)},
        {"GadgetItem.SetHeldGadget", heldMi, reinterpret_cast<void*>(&HeldDetour), reinterpret_cast<void**>(&g_origHeld)},
        {"FabricatorItemDetails.SetItem", setItemMi, reinterpret_cast<void*>(&SetItemDetour),
         reinterpret_cast<void**>(&g_origSetItem)},
    };
    void* ptrs[std::size(hooks)] = {};
    for (size_t i = 0; i < std::size(hooks); ++i) {
        std::string err;
        ptrs[i] = R.MethodPointer(hooks[i].mi, &err);
        if (!ptrs[i]) {
            why = std::format("{}: no native body ({})", hooks[i].name, err);
            return fail();
        }
    }
    g = p;  // the detours pass through until armed
    for (size_t i = 0; i < std::size(hooks); ++i) {
        if (!ctx.HookAddress(hooks[i].name, ptrs[i], hooks[i].detour, hooks[i].orig)) {
            why = std::format("hook {} failed", hooks[i].name);
            return false;  // hooks made so far only pass through (never armed)
        }
        report += std::format(" {}@0x{:X}", hooks[i].name, Resolver::Rva(ptrs[i]));
    }
    p->armed = true;
    return true;
}

bool Ready() { return g && g->armed; }

void Register(Il2CppObject* def, Look look) {
    if (!def || !g) return;
    Slot* s = Find(def);
    if (!s)
        for (Slot& e : g_slots)
            if (!e.def.load(std::memory_order_acquire)) {
                s = &e;
                break;
            }
    if (!s) {
        SR2_WARN("[{}] gadget look: no slot left for '{}' (at most {})", g->module, unity::GetName(def), kMax);
        return;
    }
    s->name.store(look.name, std::memory_order_release);
    s->desc.store(look.description, std::memory_order_release);
    s->icon.store(look.icon, std::memory_order_release);
    if (look.icon) {  // the card's picture is ours: no full art loaded over it
        unity::Field<Il2CppObject*>(def, g->offFullArt) = nullptr;
        unity::Field<bool>(def, g->offRequiresFullArt) = false;
    }
    s->def.store(def, std::memory_order_release);
    if (look.dressPreview) g->dress[def] = std::move(look.dressPreview);
    else g->dress.erase(def);
}

void Unregister(Il2CppObject* def) {
    if (!g) return;
    if (Slot* s = Find(def)) s->def.store(nullptr, std::memory_order_release);
    g->dress.erase(def);
}

void AddPlacedLabel(PlacedLabel provider) {
    if (!g || !provider) return;
    std::lock_guard<std::mutex> lock(g->placedMu);
    g->placed.push_back(std::move(provider));
}

int TintTree(Il2CppObject* root, Rgb tint, float amount, Rgb glow, const char* what) {
    if (!g || !g->armed || !main_thread::IsMainThread() || g->Faulted()) return 0;
    Impl& p = *g;
    Il2CppObject* tr = unity::TransformOf(root);
    if (!tr) return 0;
    bool inactive = true, ok = false;
    Il2CppObject* arr = p.Call(p.compsInChildren, tr, {unity::TypeOf(p.rendererClass), &inactive}, "GetComponentsInChildren", &ok);
    int tinted = 0;
    for (size_t ri = 0, rn = ok ? unity::ArrayLength(arr) : 0; ri < rn && ri < 256; ++ri) {
        auto* r = static_cast<Il2CppObject*>(unity::ArrayAt(arr, ri));
        if (!unity::IsAlive(r)) continue;
        Il2CppObject* mats = p.Call(p.rendGetMaterials, r, {}, "Renderer.get_materials", &ok);  // instances
        for (size_t mi = 0, mn = ok && mats ? unity::ArrayLength(mats) : 0; mi < mn && mi < 16; ++mi) {
            auto* m = static_cast<Il2CppObject*>(unity::ArrayAt(mats, mi));
            if (!unity::IsAlive(m)) continue;
            const std::string mname = unity::GetName(m);
            if (mname.find(kTag) != std::string::npos) continue;  // tinted already
            int32_t vec = 2;  // MaterialPropertyType.Vector
            Il2CppObject* names = p.Call(p.matPropNames, m, {&vec}, "Material.GetPropertyNames", &ok);
            int colors = 0, emissions = 0;
            for (size_t i = 0, n = ok ? unity::ArrayLength(names) : 0; i < n && i < 64; ++i) {
                auto* s = static_cast<Il2CppString*>(unity::ArrayAt(names, i));
                std::string low = unity::ToUtf8(s);
                for (char& c : low) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                const bool emission = low.find("emiss") != std::string::npos || low.find("glow") != std::string::npos;
                const bool color = !emission && (low.find("color") != std::string::npos || low.find("tint") != std::string::npos ||
                                                 low.find("albedo") != std::string::npos);
                if (!emission && !color) continue;
                Color c = Impl::Value<Color>(p.Call(p.matGetColor, m, {s}, "Material.GetColor", &ok));
                if (!ok) continue;
                Color out;
                if (emission) {
                    const float k = std::clamp(std::max({c.r, c.g, c.b, 0.0f}), 0.6f, 4.0f) * 1.5f;
                    out = {glow.r * k, glow.g * k, glow.b * k, c.a};
                    ++emissions;
                } else {
                    out = {c.r + (tint.r - c.r) * amount, c.g + (tint.g - c.g) * amount, c.b + (tint.b - c.b) * amount, c.a};
                    ++colors;
                }
                p.Call(p.matSetColor, m, {s, &out}, "Material.SetColor", &ok);
            }
            Il2CppObject* shader = p.Call(p.matGetShader, m, {}, "Material.get_shader", &ok);
            const std::string sh = ok && shader ? unity::GetName(shader) : std::string("?");
            if (p.loggedShaders.insert(std::string(what) + "|" + sh).second)
                SR2_INFO("[{}] gadget look: {}: shader '{}': {} colour / {} emission propert(ies) tinted", p.module, what, sh,
                         colors, emissions);
            unity::SetName(m, mname + kTag);
            ++tinted;
            if (p.Faulted()) return tinted;
        }
    }
    return tinted;
}

Il2CppObject* TintedSprite(Il2CppObject* sprite, Rgb tint, float amount, std::string& why, bool square) {
    if (!g || !g->armed || !main_thread::IsMainThread() || g->Faulted()) {
        why = "not available";
        return nullptr;
    }
    Impl& p = *g;
    if (!unity::IsAlive(sprite)) {
        why = "no sprite";
        return nullptr;
    }
    bool ok = false;
    Il2CppObject* tex = p.Call(p.spriteTexture, sprite, {}, "Sprite.get_texture", &ok);
    if (!ok || !unity::IsAlive(tex)) {
        why = "the sprite has no texture";
        return nullptr;
    }
    const int tw = Impl::Value<int32_t>(p.Call(p.texWidth, tex, {}, "Texture.get_width", &ok));
    const int th = ok ? Impl::Value<int32_t>(p.Call(p.texHeight, tex, {}, "Texture.get_height", &ok)) : 0;
    if (!ok || tw <= 0 || th <= 0 || tw > 8192 || th > 8192) {
        why = std::format("texture size {}x{}", tw, th);
        return nullptr;
    }
    // Where the sprite is in its texture (textureRect throws for tightly packed atlas sprites).
    std::string err;
    Il2CppObject* boxed = Resolver::Get().Invoke(p.spriteTexRect, sprite, nullptr, &err);
    Rect src = err.empty() ? Impl::Value<Rect>(boxed) : Rect{0, 0, 0, 0};
    const Rect full = Impl::Value<Rect>(p.Call(p.spriteRect, sprite, {}, "Sprite.get_rect", &ok));
    if (!err.empty() || src.w < 1 || src.h < 1) src = full;
    // A trimmed atlas sprite (textureRect smaller than rect, e.g. iconGadgetRefineryLink_small: 99x125 of a
    // square rect) draws its pixels at textureRectOffset inside the full rect. The copy keeps the full rect
    // with the pixels at that offset: a copy of only the trimmed pixels was stretched to the square icon box
    // (round 11: the quantum drone source icon "is wider than normally").
    Vec2 off{0, 0};
    if (err.empty() && p.spriteTexOffset && (src.w < full.w - 0.5f || src.h < full.h - 0.5f)) {
        std::string offErr;
        Il2CppObject* bo = Resolver::Get().Invoke(p.spriteTexOffset, sprite, nullptr, &offErr);
        if (offErr.empty()) off = Impl::Value<Vec2>(bo);
    }
    const int x = static_cast<int>(std::lround(src.x)), y = static_cast<int>(std::lround(src.y));
    const int w = static_cast<int>(std::lround(src.w)), h = static_cast<int>(std::lround(src.h));
    if (w < 1 || h < 1 || x < 0 || y < 0 || x + w > tw || y + h > th) {
        why = std::format("sprite rect ({}, {}, {}, {}) outside its {}x{} texture", x, y, w, h, tw, th);
        return nullptr;
    }
    // The copy's size (the sprite's full rect when trimmed) and where the pixels go in it.
    int cwFull = w, chFull = h, ox = 0, oy = 0;
    if (off.x != 0 || off.y != 0 || full.w > w + 0.5f || full.h > h + 0.5f) {
        cwFull = std::max(w, static_cast<int>(std::lround(full.w)));
        chFull = std::max(h, static_cast<int>(std::lround(full.h)));
        ox = std::clamp(static_cast<int>(std::lround(off.x)), 0, cwFull - w);
        oy = std::clamp(static_cast<int>(std::lround(off.y)), 0, chFull - h);
        if (cwFull > 4096 || chFull > 4096) cwFull = w, chFull = h, ox = oy = 0;
    }
    int padX = 0, padY = 0;  // square: the full rect centred on a square canvas
    if (square && cwFull != chFull) {
        const int side = std::max(cwFull, chFull);
        padX = (side - cwFull) / 2, padY = (side - chFull) / 2;
        ox += padX, oy += padY;
        cwFull = chFull = side;
    }
    const Vec2 pivotPx = Impl::Value<Vec2>(p.Call(p.spritePivot, sprite, {}, "Sprite.get_pivot", &ok));
    const float ppu = Impl::Value<float>(p.Call(p.spritePpu, sprite, {}, "Sprite.get_pixelsPerUnit", &ok), 100.0f);
    // GPU copy: Blit into an sRGB RenderTexture (sampled like the UI samples it), ReadPixels the rect.
    int32_t rw = tw, rh = th, depth = 0, fmt = 0 /* ARGB32 */, rwMode = 2 /* sRGB */;
    Il2CppObject* rtex = p.Call(p.rtGetTemp, nullptr, {&rw, &rh, &depth, &fmt, &rwMode}, "RenderTexture.GetTemporary", &ok);
    if (!ok || !rtex) {
        why = "no RenderTexture";
        return nullptr;
    }
    bool prevOk = false;
    Il2CppObject* prev = p.Call(p.rtGetActive, nullptr, {}, "RenderTexture.get_active", &prevOk);
    Il2CppObject* copy = nullptr;
    bool read = false;
    p.Call(p.blit, nullptr, {tex, rtex}, "Graphics.Blit", &ok);
    if (ok) p.Call(p.rtSetActive, nullptr, {rtex}, "RenderTexture.set_active", &ok);
    if (ok) copy = p.objectNew(p.tex2dClass);
    if (copy) {
        int32_t cw = cwFull, ch = chFull, cf = 4 /* RGBA32 */;
        bool mip = false, linear = false;
        p.Call(p.tex2dCtor, copy, {&cw, &ch, &cf, &mip, &linear}, "Texture2D..ctor", &ok);
        Rect rr{static_cast<float>(x), static_cast<float>(y), static_cast<float>(w), static_cast<float>(h)};
        int32_t dx = ox, dy = oy;
        if (ok) p.Call(p.texRead, copy, {&rr, &dx, &dy}, "Texture2D.ReadPixels", &ok);
        read = ok;
    }
    if (prevOk) p.Call(p.rtSetActive, nullptr, {prev}, "RenderTexture.set_active");
    p.Call(p.rtRelease, nullptr, {rtex}, "RenderTexture.ReleaseTemporary");
    if (!read || p.Faulted()) {
        why = "GPU read back failed";
        return nullptr;
    }
    // Recolour: towards tint x brightness, alpha kept.
    void* px = p.Call(p.texGetPixels, copy, {}, "Texture2D.GetPixels32", &ok);
    const size_t n = static_cast<size_t>(cwFull) * static_cast<size_t>(chFull);
    if (!ok || unity::ArrayLength(px) != n) {
        why = "GetPixels32 failed";
        return nullptr;
    }
    Color32* c = unity::ArrayData<Color32>(px);
    size_t opaque = 0;
    for (size_t i = 0; i < n; ++i) {
        const int px_ = static_cast<int>(i % static_cast<size_t>(cwFull)), py_ = static_cast<int>(i / static_cast<size_t>(cwFull));
        if (px_ < ox || px_ >= ox + w || py_ < oy || py_ >= oy + h) {  // the padding round a trimmed sprite: clear
            c[i] = {0, 0, 0, 0};
            continue;
        }
        if (c[i].a) ++opaque;
        const float lum = (0.30f * c[i].r + 0.59f * c[i].g + 0.11f * c[i].b) / 255.0f;
        const float k = std::min(lum * 1.35f + 0.08f, 1.0f);
        auto mix = [&](uint8_t v, float t) {
            const float target = std::clamp(t * k + (k > 0.85f ? (k - 0.85f) * 2.0f : 0.0f), 0.0f, 1.0f) * 255.0f;
            return static_cast<uint8_t>(std::clamp(v + (target - v) * amount, 0.0f, 255.0f));
        };
        c[i] = {mix(c[i].r, tint.r), mix(c[i].g, tint.g), mix(c[i].b, tint.b), c[i].a};
    }
    p.Call(p.texSetPixels, copy, {px}, "Texture2D.SetPixels32", &ok);
    if (ok) p.Call(p.texApply, copy, {}, "Texture2D.Apply", &ok);
    Rect rect{0, 0, static_cast<float>(cwFull), static_cast<float>(chFull)};
    Vec2 pivot{full.w > 0 ? (pivotPx.x + static_cast<float>(padX)) / static_cast<float>(cwFull) : 0.5f,
               full.h > 0 ? (pivotPx.y + static_cast<float>(padY)) / static_cast<float>(chFull) : 0.5f};
    float useppu = ppu > 0 ? ppu : 100.0f;
    Il2CppObject* out = ok ? p.Call(p.spriteCreate, nullptr, {copy, &rect, &pivot, &useppu}, "Sprite.Create", &ok) : nullptr;
    if (!ok || !unity::IsAlive(out)) {
        why = "Sprite.Create failed";
        return nullptr;
    }
    int32_t flags = 32;  // HideFlags.DontUnloadUnusedAsset
    p.Call(p.setHideFlags, copy, {&flags}, "Object.set_hideFlags");
    p.Call(p.setHideFlags, out, {&flags}, "Object.set_hideFlags");
    (void)unity::NewGCHandle(copy);  // session objects, never freed
    (void)unity::NewGCHandle(out);
    unity::SetName(out, unity::GetName(sprite) + " (sr2 tint)");
    SR2_INFO("[{}] gadget look: sprite '{}' recoloured ({}x{} px from its {}x{} texture{}, {} opaque)", p.module,
             unity::GetName(sprite), w, h, tw, th,
             cwFull != w || chFull != h ? std::format(", padded to its {}x{} rect at ({}, {})", cwFull, chFull, ox, oy) : std::string(),
             opaque);
    return out;
}

}  // namespace sr2::modules::custom_upgrades::gadget_look
