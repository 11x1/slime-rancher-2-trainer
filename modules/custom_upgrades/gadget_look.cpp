// custom_upgrades: game-UI looks of the mod's gadget definitions (see gadget_look.h).

#include "modules/custom_upgrades/gadget_look.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <format>
#include <initializer_list>
#include <map>
#include <mutex>
#include <set>
#include <tuple>
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
constexpr float kMinFullArt = 256.0f;  // a loaded Fabricator full art is 512 px; anything smaller is still loading

struct Vec2 {
    float x, y;
};
struct Vec3 {
    float x, y, z;
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
    std::atomic<Il2CppObject*> art{nullptr};  // the recoloured full art (Look.artAmount), once made
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
    // Look.artAmount: definitions whose recoloured full art is still to be made, and the details image
    // waiting for the loader (SetItemDetour -> ArtFrame).
    struct ArtTint {
        Rgb tint;
        float amount = 0;
    };
    std::map<Il2CppObject*, ArtTint> artTints;
    std::set<Il2CppObject*> pinnedImages;  // details images we watch (pinned once, never freed)
    std::map<Il2CppObject*, std::string> loggedPictures;  // definition -> what its details picture log said last
    Il2CppObject* artImage = nullptr;
    Il2CppObject* artDef = nullptr;
    float artWait = 0;
    int artCallback = 0;
    // Round 14: the details picture is drawn from a copy at its exact on-screen size (ScreenSized).
    std::map<std::tuple<Il2CppObject*, int, int, bool>, Il2CppObject*> sized;  // (source, w, h, point) -> copy
    std::set<Il2CppObject*> made;  // every sprite this file made (TintedSprite, ScreenSized): never "the loaded full art"
    // One look at a vanilla card's details picture (how the game's own full art reaches the screen).
    Il2CppObject* probeImage = nullptr;
    float probeWait = 0;
    int probes = 0;
    // offsets
    size_t offFullArt = 0, offRequiresFullArt = 0, offDetailsIcon = 0, offFabDef = 0, offLoaderSource = 0;
    Il2CppClass* fabItemClass = nullptr, *loaderClass = nullptr;
    const MethodInfo* imageSetSprite = nullptr, *imageGetSprite = nullptr;
    // on-screen size and diagnostics (all optional: without them the picture is shown unscaled)
    const MethodInfo* rtGetRect = nullptr, *trLossy = nullptr, *graphicCanvas = nullptr, *graphicMaterial = nullptr,
                      *imagePreserve = nullptr, *canvasScale = nullptr, *canvasRoot = nullptr, *canvasMode = nullptr,
                      *canvasPixelPerfect = nullptr, *screenW = nullptr, *screenH = nullptr, *texGetFilter = nullptr,
                      *texMipCount = nullptr, *texGetMipBias = nullptr, *texSetWrap = nullptr, *tex2dFormat = nullptr,
                      *objDestroy = nullptr;
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
                      *texSetPixels = nullptr, *texApply = nullptr, *setHideFlags = nullptr, *texSetFilter = nullptr,
                      *texSetMipBias = nullptr;
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
    void EnsureCallback();
    void ArtFrame(float dt);
    void ProbeFrame(float dt);
    void DropArt(Il2CppObject* def, Slot* s, const std::string& why);
    bool IsOurs(Il2CppObject* sprite);
    struct Screen {
        bool ok = false;      // the on-screen size is known
        float w = 0, h = 0;   // the sprite's drawn size in screen pixels (preserveAspect fit into the box)
        bool exact = false;   // no scaled parent between the image and its root canvas: a copy at this size draws 1:1
        std::string text;     // for the log
    };
    Screen OnScreen(Il2CppObject* image, float spriteW, float spriteH);
    std::string TextureText(Il2CppObject* sprite);
    void ShowPicture(Il2CppObject* image, Il2CppObject* def, Il2CppObject* sprite);
};

Impl* g = nullptr;

// A copy of `sprite` resampled (Lanczos-3, linear light, premultiplied alpha) to w x h, no mips, point or
// bilinear filter: drawn 1:1 the GPU does no scaling. Cached per (sprite, size, filter). Defined below.
Il2CppObject* ScreenSized(Il2CppObject* sprite, int w, int h, bool point, std::string& why);
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
    const bool live = self && item && g && g->armed && main_thread::IsMainThread() && !g->Faulted();
    const bool gadgetCard = live && api().il2cpp_object_get_class(item) == g->fabItemClass;
    Slot* s = gadgetCard ? Find(unity::Field<Il2CppObject*>(item, g->offFabDef)) : nullptr;
    Il2CppObject* icon = s ? s->icon.load(std::memory_order_acquire) : nullptr;
    if (icon && !s->art.load(std::memory_order_acquire) && g->artTints.count(s->def.load(std::memory_order_acquire)))
        g_guard.Run([&] {
            // Full art still to recolour. Round 13: the Quantum Storage Link's card picture was the DJ Booth's
            // drawing recoloured: the image still showed the previous card's picture (ours) and the loader put
            // nothing over it, so ArtFrame took it for the loaded full art. Now the image shows our icon before
            // the loader runs, and the loader forgets its source so SetFullArtSource (re)loads for this item
            // (it skips an unchanged source with a valid handle).
            Il2CppObject* image = unity::Field<Il2CppObject*>(self, g->offDetailsIcon);
            if (!unity::IsAlive(image)) return;
            g->Call(g->imageSetSprite, image, {icon}, "Image.set_sprite");
            Il2CppObject* loader = g->loaderClass && g->offLoaderSource ? unity::GetComponent(image, g->loaderClass) : nullptr;
            if (unity::IsAlive(loader)) unity::Field<Il2CppObject*>(loader, g->offLoaderSource) = nullptr;
        });
    g_origSetItem(self, item, mi);
    if (!live) return;
    g->artImage = nullptr;  // another card: stop waiting
    g->probeImage = nullptr;
    if (!icon) {
        if (gadgetCard && !s && g->probes < 2)  // a vanilla gadget card: look at how its full art is drawn
            g_guard.Run([&] {
                Il2CppObject* image = unity::Field<Il2CppObject*>(self, g->offDetailsIcon);
                if (!unity::IsAlive(image)) return;
                if (g->pinnedImages.insert(image).second) (void)unity::NewGCHandle(image);
                g->probeImage = image;
                g->probeWait = 0;
                g->EnsureCallback();
            });
        return;
    }
    g_guard.Run([&] {
        Il2CppObject* image = unity::Field<Il2CppObject*>(self, g->offDetailsIcon);
        if (!unity::IsAlive(image)) return;
        Il2CppObject* art = s->art.load(std::memory_order_acquire);
        Il2CppObject* def = s->def.load(std::memory_order_acquire);
        if (art || !g->artTints.count(def) || !g->imageGetSprite) {
            g->ShowPicture(image, def, art ? art : icon);
            return;
        }
        // Full art still to recolour: the loader shows our icon (the item's) and puts the base's full art on
        // this image once loaded (maybe already, inside the original). ArtFrame picks it up from there.
        if (g->pinnedImages.insert(image).second) (void)unity::NewGCHandle(image);
        g->artImage = image;
        g->artDef = def;
        g->artWait = 0;
        g->EnsureCallback();
    });
}

void Impl::EnsureCallback() {
    if (!artCallback)
        artCallback = main_thread::AddFrameCallback("custom_upgrades", [](float dt) {
            if (g && g->armed && !g->Faulted()) g_guard.Run([&] {
                    g->ArtFrame(dt);
                    g->ProbeFrame(dt);
                });
        });
}

// No recoloured full art for `def`: its picture is the icon from now on (the reference cleared).
void Impl::DropArt(Il2CppObject* def, Slot* s, const std::string& why) {
    artTints.erase(def);
    unity::Field<Il2CppObject*>(def, offFullArt) = nullptr;
    unity::Field<bool>(def, offRequiresFullArt) = false;
    Il2CppObject* icon = s ? s->icon.load(std::memory_order_acquire) : nullptr;
    SR2_INFO("[{}] gadget look: '{}' card picture stays the icon ({})", module, unity::GetName(def), why);
    if (icon && unity::IsAlive(artImage)) ShowPicture(artImage, def, icon);
    artImage = nullptr;
}

// A sprite this file or mod_icons made, or one registered as a picture: never the loader's full art.
bool Impl::IsOurs(Il2CppObject* sprite) {
    if (!sprite) return false;
    if (made.count(sprite)) return true;
    for (Slot& e : g_slots)
        if (e.def.load(std::memory_order_acquire) &&
            (e.icon.load(std::memory_order_acquire) == sprite || e.art.load(std::memory_order_acquire) == sprite))
            return true;
    return unity::GetName(sprite).rfind("sr2mods_icon_", 0) == 0;
}

// Where the details picture really lands on screen (round 14: "still pixelated" after the round 13 mip bias):
// the Image's RectTransform box (canvas units) x the root canvas's scale factor, the sprite fitted into it
// (preserveAspect). Also logged: parent scales between image and canvas, render mode, pixel perfect, screen
// size, and the material / shader the Image draws with.
Impl::Screen Impl::OnScreen(Il2CppObject* image, float sw, float sh) {
    Screen out;
    if (!rtGetRect || !graphicCanvas || !canvasScale || sw < 1 || sh < 1) {
        out.text = "on-screen size unknown (RectTransform / Canvas methods missing)";
        return out;
    }
    bool ok = false;
    Il2CppObject* tr = unity::TransformOf(image);
    const Rect box = tr ? Value<Rect>(Call(rtGetRect, tr, {}, "RectTransform.get_rect", &ok)) : Rect{0, 0, 0, 0};
    Il2CppObject* canvas = ok ? Call(graphicCanvas, image, {}, "Graphic.get_canvas", &ok) : nullptr;
    if (!ok || !unity::IsAlive(canvas) || box.w < 1 || box.h < 1) {
        out.text = std::format("on-screen size unknown (box {:.0f}x{:.0f}, canvas {})", box.w, box.h, unity::IsAlive(canvas) ? "yes" : "none");
        return out;
    }
    Il2CppObject* root = canvasRoot ? Call(canvasRoot, canvas, {}, "Canvas.get_rootCanvas", &ok) : nullptr;
    if (!unity::IsAlive(root)) root = canvas;
    const float scale = Value<float>(Call(canvasScale, root, {}, "Canvas.get_scaleFactor", &ok));
    if (!ok || !(scale > 0.01f && scale < 100.0f)) {
        out.text = std::format("on-screen size unknown (canvas scale factor {})", scale);
        return out;
    }
    const int mode = canvasMode ? Value<int32_t>(Call(canvasMode, root, {}, "Canvas.get_renderMode"), -1) : -1;
    const bool pixelPerfect = canvasPixelPerfect && Value<bool>(Call(canvasPixelPerfect, root, {}, "Canvas.get_pixelPerfect"));
    // Scales between the image and the root canvas (an opening tween, a scaled panel): 1 at rest.
    float rel = 1.0f;
    if (trLossy)
        if (Il2CppObject* rootTr = unity::TransformOf(root)) {
            const Vec3 li = Value<Vec3>(Call(trLossy, tr, {}, "Transform.get_lossyScale"));
            const Vec3 lr = Value<Vec3>(Call(trLossy, rootTr, {}, "Transform.get_lossyScale"));
            if (std::fabs(lr.x) > 1e-6f) rel = li.x / lr.x;
        }
    const bool preserve = imagePreserve ? Value<bool>(Call(imagePreserve, image, {}, "Image.get_preserveAspect"), true) : true;
    const float bw = box.w * scale, bh = box.h * scale;
    if (preserve) {
        const float f = std::min(bw / sw, bh / sh);
        out.w = sw * f, out.h = sh * f;
    } else {
        out.w = bw, out.h = bh;
    }
    out.ok = true;
    out.exact = std::fabs(rel - 1.0f) < 0.005f;
    Il2CppObject* mat = graphicMaterial ? Call(graphicMaterial, image, {}, "Graphic.get_materialForRendering", &ok) : nullptr;
    Il2CppObject* shader = unity::IsAlive(mat) && matGetShader ? Call(matGetShader, mat, {}, "Material.get_shader") : nullptr;
    const int scrW = screenW ? Value<int32_t>(Call(screenW, nullptr, {}, "Screen.get_width")) : 0;
    const int scrH = screenH ? Value<int32_t>(Call(screenH, nullptr, {}, "Screen.get_height")) : 0;
    static constexpr const char* kModes[] = {"overlay", "camera", "world"};
    out.text = std::format(
        "on screen {:.1f}x{:.1f} px (box {:.0f}x{:.0f} units x canvas scale {:.3f}{}, parent scale {:.3f}; root canvas '{}' {}{}; "
        "screen {}x{}; material '{}' shader '{}')",
        out.w, out.h, box.w, box.h, scale, preserve ? ", aspect kept" : "", rel, unity::GetName(root),
        mode >= 0 && mode < 3 ? kModes[mode] : "mode ?", pixelPerfect ? ", pixel perfect" : "", scrW, scrH,
        unity::IsAlive(mat) ? unity::GetName(mat) : std::string("?"), unity::IsAlive(shader) ? unity::GetName(shader) : std::string("?"));
    return out;
}

// "'name' WxH px of a TWxTH texture (format F, N mips, filter, bias B)".
std::string Impl::TextureText(Il2CppObject* sprite) {
    bool ok = false;
    const Rect rc = Value<Rect>(Call(spriteRect, sprite, {}, "Sprite.get_rect", &ok));
    Il2CppObject* tex = ok ? Call(spriteTexture, sprite, {}, "Sprite.get_texture", &ok) : nullptr;
    if (!ok || !unity::IsAlive(tex)) return std::format("'{}' {:.0f}x{:.0f} px (no texture)", unity::GetName(sprite), rc.w, rc.h);
    const int tw = Value<int32_t>(Call(texWidth, tex, {}, "Texture.get_width"));
    const int th = Value<int32_t>(Call(texHeight, tex, {}, "Texture.get_height"));
    const int fmt = tex2dFormat ? Value<int32_t>(Call(tex2dFormat, tex, {}, "Texture2D.get_format"), -1) : -1;
    const int mips = texMipCount ? Value<int32_t>(Call(texMipCount, tex, {}, "Texture.get_mipmapCount"), -1) : -1;
    const int filter = texGetFilter ? Value<int32_t>(Call(texGetFilter, tex, {}, "Texture.get_filterMode"), -1) : -1;
    const float bias = texGetMipBias ? Value<float>(Call(texGetMipBias, tex, {}, "Texture.get_mipMapBias")) : 0.0f;
    static constexpr const char* kFilters[] = {"point", "bilinear", "trilinear"};
    return std::format("'{}' {:.0f}x{:.0f} px of a {}x{} texture (format {}, {} mip(s), {}, bias {:.1f})", unity::GetName(sprite),
                       rc.w, rc.h, tw, th, fmt, mips, filter >= 0 && filter < 3 ? kFilters[filter] : "filter ?", bias);
}

// Puts `sprite` on the details image, drawn from a copy at its exact on-screen size when that is known (round
// 14: a 512 px picture minified to the 440 px box by the GPU looked "pixelated"; with mips it was "blurry").
// Logs what it did once per definition (again when it changes).
void Impl::ShowPicture(Il2CppObject* image, Il2CppObject* def, Il2CppObject* sprite) {
    if (!unity::IsAlive(image) || !unity::IsAlive(sprite)) return;
    bool ok = false;
    const Rect rc = Value<Rect>(Call(spriteRect, sprite, {}, "Sprite.get_rect", &ok));
    const Screen sc = OnScreen(image, rc.w, rc.h);
    Il2CppObject* show = sprite;
    std::string note = "shown as is";
    if (sc.ok && ok) {
        const int w = static_cast<int>(std::lround(sc.w)), h = static_cast<int>(std::lround(sc.h));
        if (std::fabs(static_cast<float>(w) - rc.w) < 1.0f && std::fabs(static_cast<float>(h) - rc.h) < 1.0f) {
            note = "already its on-screen size: shown as is";
        } else if (w < 16 || h < 16 || w > 2048 || h > 2048) {
            note = "on-screen size out of range: shown as is";
        } else {
            std::string why;
            if (Il2CppObject* copy = ScreenSized(sprite, w, h, sc.exact, why)) {
                show = copy;
                note = std::format("drawn from a {}x{} copy (Lanczos, no mips, {})", w, h,
                                   sc.exact ? "point filter: 1:1 on screen" : "bilinear: a parent is scaled");
            } else {
                note = "no screen-sized copy (" + why + "): shown as is";
            }
        }
    }
    Call(imageSetSprite, image, {show}, "Image.set_sprite");
    std::string line = std::format("{}; {}; {}", TextureText(sprite), sc.text, note);
    std::string& last = loggedPictures[def];
    if (last != line) {
        SR2_INFO("[{}] gadget look: '{}' details picture shown = {}", module, unity::GetName(def), line);
        last = std::move(line);
    }
}

// Waits (up to 10 s) for the details image to show something other than our icon: the loaded full art.
void Impl::ArtFrame(float dt) {
    if (!artImage) return;
    Slot* s = Find(artDef);
    auto it = artTints.find(artDef);
    if (!s || it == artTints.end() || s->art.load(std::memory_order_acquire)) {
        artImage = nullptr;
        return;
    }
    if (!unity::IsAlive(artImage)) {
        artImage = nullptr;  // the menu closed before the art came: the next SetItem waits again
        return;
    }
    artWait += std::isfinite(dt) && dt > 0 ? std::min(dt, 1.0f) : 0.0f;
    bool ok = false;
    Il2CppObject* shown = Call(imageGetSprite, artImage, {}, "Image.get_sprite", &ok);
    Il2CppObject* icon = s->icon.load(std::memory_order_acquire);
    // The full art is the bigger picture (512 px); anything else means still loading: our icon, nothing yet, or
    // the loader's own 160 px placeholder 'iconEmpty' it shows while loading (round 12: the Slime Radio's card
    // picture was 'iconEmpty' recoloured, a small grey box stretched to the 440 px details box). Never one of
    // our own pictures (round 13: another card's drawing left on the image was taken for it).
    const float iconW = icon ? Value<Rect>(Call(spriteRect, icon, {}, "Sprite.get_rect")).w : 0.0f;
    const float shownW = ok && unity::IsAlive(shown) && !IsOurs(shown) ? Value<Rect>(Call(spriteRect, shown, {}, "Sprite.get_rect", &ok)).w : 0.0f;
    if (!ok || shownW <= iconW || shownW < kMinFullArt || unity::GetName(shown) == "iconEmpty") {
        if (artWait >= 10.0f) DropArt(artDef, s, "the full art did not load within 10 s");
        return;
    }
    std::string why;
    Il2CppObject* art = TintedSprite(shown, it->second.tint, it->second.amount, why);
    if (!art) return DropArt(artDef, s, std::format("recolouring '{}' failed: {}", unity::GetName(shown), why));
    s->art.store(art, std::memory_order_release);
    artTints.erase(it);
    // Ours from now on: the loader loads nothing over it.
    unity::Field<Il2CppObject*>(artDef, offFullArt) = nullptr;
    unity::Field<bool>(artDef, offRequiresFullArt) = false;
    SR2_INFO("[{}] gadget look: '{}' card picture = its full art '{}' recoloured (after {:.1f} s)", module,
             unity::GetName(artDef), unity::GetName(shown), artWait);
    ShowPicture(artImage, artDef, art);
    artImage = nullptr;
}

// A vanilla gadget card, 1 s after it was shown (the loader is done): how the game's own full art is set up
// and drawn, to compare with ours (round 14). Twice per session.
void Impl::ProbeFrame(float dt) {
    if (!probeImage) return;
    if (!unity::IsAlive(probeImage)) {
        probeImage = nullptr;
        return;
    }
    probeWait += std::isfinite(dt) && dt > 0 ? std::min(dt, 1.0f) : 0.0f;
    if (probeWait < 1.0f) return;
    Il2CppObject* image = probeImage;
    probeImage = nullptr;
    bool ok = false;
    Il2CppObject* shown = imageGetSprite ? Call(imageGetSprite, image, {}, "Image.get_sprite", &ok) : nullptr;
    if (!ok || !unity::IsAlive(shown)) return;
    ++probes;
    const Rect rc = Value<Rect>(Call(spriteRect, shown, {}, "Sprite.get_rect"));
    SR2_INFO("[{}] gadget look: a vanilla card's details picture = {}; {}", module, TextureText(shown), OnScreen(image, rc.w, rc.h).text);
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
    p->imageGetSprite = R.Method(image, "get_sprite", 0);  // optional: Look.artAmount
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
    p->texSetFilter = R.Method(texture, "set_filterMode", 1);   // optional (sharpness, TintedSprite)
    p->texSetMipBias = R.Method(texture, "set_mipMapBias", 1);  // optional (sharpness, TintedSprite)
    p->setHideFlags = method(uobject, "set_hideFlags", 1);
    // Optional (round 14): the details picture's on-screen size, screen-sized copies, and the diagnostics.
    auto opt = [&](Il2CppClass* k, const char* name, int argc) -> const MethodInfo* {
        for (Il2CppClass* c = k; c; c = parentOf(c))
            if (const MethodInfo* m = R.Method(c, name, argc)) return m;
        return nullptr;
    };
    auto optCls = [&](const char* img, const char* ns, const char* name) { return R.Class(img, ns, name); };
    Il2CppClass* rectTr = optCls(kCore, "UnityEngine", "RectTransform");
    Il2CppClass* transform = optCls(kCore, "UnityEngine", "Transform");
    Il2CppClass* canvas = optCls("UnityEngine.UIModule", "UnityEngine", "Canvas");
    Il2CppClass* screen = optCls(kCore, "UnityEngine", "Screen");
    p->rtGetRect = opt(rectTr, "get_rect", 0);
    p->trLossy = opt(transform, "get_lossyScale", 0);
    p->graphicCanvas = opt(image, "get_canvas", 0);  // Graphic, through the parents
    p->graphicMaterial = opt(image, "get_materialForRendering", 0);
    p->imagePreserve = opt(image, "get_preserveAspect", 0);
    p->canvasScale = opt(canvas, "get_scaleFactor", 0);
    p->canvasRoot = opt(canvas, "get_rootCanvas", 0);
    p->canvasMode = opt(canvas, "get_renderMode", 0);
    p->canvasPixelPerfect = opt(canvas, "get_pixelPerfect", 0);
    p->screenW = opt(screen, "get_width", 0);
    p->screenH = opt(screen, "get_height", 0);
    p->texGetFilter = opt(texture, "get_filterMode", 0);
    p->texMipCount = opt(texture, "get_mipmapCount", 0);
    p->texGetMipBias = opt(texture, "get_mipMapBias", 0);
    p->texSetWrap = opt(texture, "set_wrapMode", 1);
    p->tex2dFormat = opt(p->tex2dClass, "get_format", 0);
    p->objDestroy = opt(uobject, "Destroy", 1);
    p->loaderClass = optCls(kImage, "MonomiPark.SlimeRancher.UI.Framework.Components", "UIImageDynamicLoader");
    if (p->loaderClass)
        if (auto o = R.FieldOffset(p->loaderClass, "_iconSource")) p->offLoaderSource = *o;
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
    const bool tintArt = look.icon && look.artAmount > 0 && !s->art.load(std::memory_order_acquire) && g->imageGetSprite &&
                         unity::Field<Il2CppObject*>(def, g->offFullArt) && unity::Field<bool>(def, g->offRequiresFullArt);
    if (tintArt) {
        g->artTints[def] = {look.artTint, look.artAmount};  // the full art loads once, then is recoloured
    } else if (look.icon) {  // the card's picture is ours: no full art loaded over it
        g->artTints.erase(def);
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

namespace {

// A sprite's pixels read back from the GPU (game textures are not readable): `tex` is a new readable RGBA32
// Texture2D of cw x ch holding the sprite's full rect (a trimmed atlas sprite's pixels at (ox, oy), w x h;
// the rest is undefined until cleared). `pivot` in pixels of that canvas.
struct ReadBack {
    Il2CppObject* tex = nullptr;
    int cw = 0, ch = 0, ox = 0, oy = 0, w = 0, h = 0, tw = 0, th = 0;
    Vec2 pivot{0, 0};
    float ppu = 100.0f;
};

bool ReadSprite(Impl& p, Il2CppObject* sprite, bool square, bool mips, ReadBack& out, std::string& why) {
    bool ok = false;
    Il2CppObject* tex = p.Call(p.spriteTexture, sprite, {}, "Sprite.get_texture", &ok);
    if (!ok || !unity::IsAlive(tex)) {
        why = "the sprite has no texture";
        return false;
    }
    const int tw = Impl::Value<int32_t>(p.Call(p.texWidth, tex, {}, "Texture.get_width", &ok));
    const int th = ok ? Impl::Value<int32_t>(p.Call(p.texHeight, tex, {}, "Texture.get_height", &ok)) : 0;
    if (!ok || tw <= 0 || th <= 0 || tw > 8192 || th > 8192) {
        why = std::format("texture size {}x{}", tw, th);
        return false;
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
        return false;
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
        return false;
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
        bool mip = mips, linear = false;
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
        return false;
    }
    out.tex = copy;
    out.cw = cwFull, out.ch = chFull, out.ox = ox, out.oy = oy, out.w = w, out.h = h, out.tw = tw, out.th = th;
    out.pivot = {pivotPx.x + static_cast<float>(padX), pivotPx.y + static_cast<float>(padY)};
    out.ppu = ppu > 0 ? ppu : 100.0f;
    return true;
}

// The finished texture (pixels applied) as a session sprite: pivot in pixels, never unloaded or freed.
Il2CppObject* MakeSprite(Impl& p, Il2CppObject* tex, int w, int h, Vec2 pivotPx, float ppu, const std::string& name, std::string& why) {
    bool ok = false;
    Rect rect{0, 0, static_cast<float>(w), static_cast<float>(h)};
    Vec2 pivot{w > 0 ? pivotPx.x / static_cast<float>(w) : 0.5f, h > 0 ? pivotPx.y / static_cast<float>(h) : 0.5f};
    Il2CppObject* out = p.Call(p.spriteCreate, nullptr, {tex, &rect, &pivot, &ppu}, "Sprite.Create", &ok);
    if (!ok || !unity::IsAlive(out)) {
        why = "Sprite.Create failed";
        return nullptr;
    }
    int32_t flags = 32;  // HideFlags.DontUnloadUnusedAsset
    p.Call(p.setHideFlags, tex, {&flags}, "Object.set_hideFlags");
    p.Call(p.setHideFlags, out, {&flags}, "Object.set_hideFlags");
    (void)unity::NewGCHandle(tex);  // session objects, never freed
    (void)unity::NewGCHandle(out);
    unity::SetName(tex, name);
    unity::SetName(out, name);
    p.made.insert(out);
    return out;
}

// sRGB <-> linear light.
float ToLinear(uint8_t v) {
    static const auto lut = [] {
        std::array<float, 256> t{};
        for (int i = 0; i < 256; ++i) {
            const float c = static_cast<float>(i) / 255.0f;
            t[static_cast<size_t>(i)] = c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
        }
        return t;
    }();
    return lut[v];
}
uint8_t ToSrgb(float c) {
    c = std::clamp(c, 0.0f, 1.0f);
    const float s = c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
    return static_cast<uint8_t>(std::clamp(std::lround(s * 255.0f), 0L, 255L));
}

// Lanczos-3 weights of every output sample along one axis (src -> dst samples); the kernel widens by the
// reduction so a downscale averages every source pixel (no skipped pixels: no aliasing).
struct Taps {
    std::vector<int> first, count;
    std::vector<float> weights;  // count[i] per output i, in order
};
Taps MakeTaps(int src, int dst) {
    constexpr float kPi = 3.14159265358979f;
    auto lanczos = [&](float x) {
        x = std::fabs(x);
        if (x < 1e-5f) return 1.0f;
        if (x >= 3.0f) return 0.0f;
        return 3.0f * std::sin(kPi * x) * std::sin(kPi * x / 3.0f) / (kPi * kPi * x * x);
    };
    Taps t;
    const float scale = static_cast<float>(dst) / static_cast<float>(src);
    const float widen = std::max(1.0f, 1.0f / scale);
    const float support = 3.0f * widen;
    for (int i = 0; i < dst; ++i) {
        const float centre = (static_cast<float>(i) + 0.5f) / scale - 0.5f;
        const int lo = static_cast<int>(std::ceil(centre - support)), hi = static_cast<int>(std::floor(centre + support));
        t.first.push_back(lo);
        t.count.push_back(hi - lo + 1);
        float sum = 0;
        const size_t at = t.weights.size();
        for (int j = lo; j <= hi; ++j) {
            const float wgt = lanczos((static_cast<float>(j) - centre) / widen);
            t.weights.push_back(wgt);
            sum += wgt;
        }
        if (std::fabs(sum) > 1e-6f)
            for (size_t k = at; k < t.weights.size(); ++k) t.weights[k] /= sum;
    }
    return t;
}

// RGBA8 sRGB sw x sh -> dw x dh, filtered in linear light on premultiplied alpha (transparent pixels do not
// darken the edges), edges clamped.
std::vector<Color32> Resample(const Color32* src, int sw, int sh, int dw, int dh) {
    std::vector<float> lin(static_cast<size_t>(sw) * sh * 4);
    for (size_t i = 0, n = static_cast<size_t>(sw) * sh; i < n; ++i) {
        const float a = src[i].a / 255.0f;
        lin[i * 4 + 0] = ToLinear(src[i].r) * a;
        lin[i * 4 + 1] = ToLinear(src[i].g) * a;
        lin[i * 4 + 2] = ToLinear(src[i].b) * a;
        lin[i * 4 + 3] = a;
    }
    const Taps tx = MakeTaps(sw, dw), ty = MakeTaps(sh, dh);
    std::vector<float> mid(static_cast<size_t>(dw) * sh * 4, 0.0f);  // horizontal pass
    for (int y = 0; y < sh; ++y) {
        size_t wi = 0;
        for (int x = 0; x < dw; ++x) {
            float acc[4] = {0, 0, 0, 0};
            for (int k = 0; k < tx.count[static_cast<size_t>(x)]; ++k, ++wi) {
                const int sx = std::clamp(tx.first[static_cast<size_t>(x)] + k, 0, sw - 1);
                const float* s = &lin[(static_cast<size_t>(y) * sw + sx) * 4];
                const float wgt = tx.weights[wi];
                for (int c = 0; c < 4; ++c) acc[c] += s[c] * wgt;
            }
            for (int c = 0; c < 4; ++c) mid[(static_cast<size_t>(y) * dw + x) * 4 + c] = acc[c];
        }
    }
    std::vector<Color32> out(static_cast<size_t>(dw) * dh);
    size_t wi = 0;
    for (int y = 0; y < dh; ++y) {  // vertical pass
        const size_t at = wi;
        for (int x = 0; x < dw; ++x) {
            wi = at;
            float acc[4] = {0, 0, 0, 0};
            for (int k = 0; k < ty.count[static_cast<size_t>(y)]; ++k, ++wi) {
                const int sy = std::clamp(ty.first[static_cast<size_t>(y)] + k, 0, sh - 1);
                const float* s = &mid[(static_cast<size_t>(sy) * dw + x) * 4];
                const float wgt = ty.weights[wi];
                for (int c = 0; c < 4; ++c) acc[c] += s[c] * wgt;
            }
            const float a = std::clamp(acc[3], 0.0f, 1.0f);
            Color32& o = out[static_cast<size_t>(y) * dw + x];
            if (a < 0.5f / 255.0f) {
                o = {0, 0, 0, 0};
                continue;
            }
            o = {ToSrgb(acc[0] / a), ToSrgb(acc[1] / a), ToSrgb(acc[2] / a), static_cast<uint8_t>(std::lround(a * 255.0f))};
        }
    }
    return out;
}

Il2CppObject* ScreenSized(Il2CppObject* sprite, int w, int h, bool point, std::string& why) {
    if (!g || !g->armed || !main_thread::IsMainThread() || g->Faulted()) {
        why = "not available";
        return nullptr;
    }
    Impl& p = *g;
    const auto key = std::make_tuple(sprite, w, h, point);
    if (auto it = p.sized.find(key); it != p.sized.end() && unity::IsAlive(it->second)) return it->second;
    ReadBack rb;
    if (!ReadSprite(p, sprite, false, false, rb, why)) return nullptr;
    bool ok = false;
    void* px = p.Call(p.texGetPixels, rb.tex, {}, "Texture2D.GetPixels32", &ok);
    const size_t n = static_cast<size_t>(rb.cw) * static_cast<size_t>(rb.ch);
    Il2CppObject* tex = ok && unity::ArrayLength(px) == n ? p.objectNew(p.tex2dClass) : nullptr;
    if (!tex) {
        why = "GetPixels32 failed";
        return nullptr;
    }
    Color32* c = unity::ArrayData<Color32>(px);
    for (size_t i = 0; i < n; ++i) {  // the padding round a trimmed sprite: clear
        const int x = static_cast<int>(i % static_cast<size_t>(rb.cw)), y = static_cast<int>(i / static_cast<size_t>(rb.cw));
        if (x < rb.ox || x >= rb.ox + rb.w || y < rb.oy || y >= rb.oy + rb.h) c[i] = {0, 0, 0, 0};
    }
    const std::vector<Color32> scaled = Resample(c, rb.cw, rb.ch, w, h);
    if (p.objDestroy) p.Call(p.objDestroy, nullptr, {rb.tex}, "Object.Destroy");  // the full-size read-back copy
    int32_t tw = w, th = h, cf = 4 /* RGBA32 */;
    bool mip = false, linear = false;  // drawn 1:1: no mips, nothing to blend
    p.Call(p.tex2dCtor, tex, {&tw, &th, &cf, &mip, &linear}, "Texture2D..ctor", &ok);
    void* arr = ok ? p.Call(p.texGetPixels, tex, {}, "Texture2D.GetPixels32", &ok) : nullptr;
    if (!ok || unity::ArrayLength(arr) != scaled.size()) {
        why = "new texture failed";
        return nullptr;
    }
    std::memcpy(unity::ArrayData<Color32>(arr), scaled.data(), scaled.size() * sizeof(Color32));
    p.Call(p.texSetPixels, tex, {arr}, "Texture2D.SetPixels32", &ok);
    if (ok) p.Call(p.texApply, tex, {}, "Texture2D.Apply", &ok);
    int32_t filter = point ? 0 : 1, clamp = 1;
    if (ok && p.texSetFilter) p.Call(p.texSetFilter, tex, {&filter}, "Texture.set_filterMode");
    if (ok && p.texSetWrap) p.Call(p.texSetWrap, tex, {&clamp}, "Texture.set_wrapMode");
    if (!ok || p.Faulted()) {
        why = "texture upload failed";
        return nullptr;
    }
    const float sx = static_cast<float>(w) / static_cast<float>(rb.cw), sy = static_cast<float>(h) / static_cast<float>(rb.ch);
    Il2CppObject* out = MakeSprite(p, tex, w, h, {rb.pivot.x * sx, rb.pivot.y * sy}, rb.ppu * sx,
                                   std::format("{} ({}x{} screen)", unity::GetName(sprite), w, h), why);
    if (!out) return nullptr;
    p.sized[key] = out;
    SR2_INFO("[{}] gadget look: sprite '{}' resampled {}x{} -> {}x{} (Lanczos-3, linear light; no mips, {} filter)", p.module,
             unity::GetName(sprite), rb.cw, rb.ch, w, h, point ? "point" : "bilinear");
    return out;
}

}  // namespace

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
    ReadBack rb;
    // mips + trilinear + bias -1 below: sampled like the game's full art
    if (!ReadSprite(p, sprite, square, true, rb, why)) return nullptr;
    Il2CppObject* copy = rb.tex;
    const int cwFull = rb.cw, chFull = rb.ch, ox = rb.ox, oy = rb.oy, w = rb.w, h = rb.h;
    bool ok = false;
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
    if (ok) p.Call(p.texApply, copy, {}, "Texture2D.Apply", &ok);  // Apply(): mips too
    int32_t trilinear = 2;
    float bias = -1.0f;
    if (ok && p.texSetFilter) p.Call(p.texSetFilter, copy, {&trilinear}, "Texture.set_filterMode");
    if (ok && p.texSetMipBias) p.Call(p.texSetMipBias, copy, {&bias}, "Texture.set_mipMapBias");
    Il2CppObject* out = ok ? MakeSprite(p, copy, cwFull, chFull, rb.pivot, rb.ppu, unity::GetName(sprite) + " (sr2 tint)", why) : nullptr;
    if (!out) {
        if (why.empty()) why = "texture upload failed";
        return nullptr;
    }
    SR2_INFO("[{}] gadget look: sprite '{}' recoloured ({}x{} px from its {}x{} texture{}, {} opaque)", p.module,
             unity::GetName(sprite), w, h, rb.tw, rb.th,
             cwFull != w || chFull != h ? std::format(", padded to its {}x{} rect at ({}, {})", cwFull, chFull, ox, oy) : std::string(),
             opaque);
    return out;
}

}  // namespace sr2::modules::custom_upgrades::gadget_look
