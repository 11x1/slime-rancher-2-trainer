// debug_tools: switches on developer tools the game still ships (Misc > General card, everything off by default).
// See NOTES.md and re/debug-mode.md for what survived the 1.3.0 release build and what did not.
//
//   * Font debug overlay: MonomiPark.SlimeRancher.UI.Debug.FontsUsedDebugUI, an IMGUI overlay the developers used
//     to check font fallback. Its code is fully intact in the release build, only nothing calls it. The static
//     FontsUsedDebugUI.StartDebugUI() creates a GameObject "FontsUsedDebugUI" (RectTransform, Canvas, the component);
//     OnEnable collects every TMP_Text once (FindObjectsOfType(true)) and subscribes to TMPro's TEXT_CHANGED_EVENT for
//     later ones; OnGUI (Repaint only) draws, over each visible text, a dark box listing the TMP font assets its
//     characters were rendered with (fallback fonts included), brighter near the mouse. Dead texts are dropped by the
//     component itself. The module calls StartDebugUI, marks the object DontDestroyOnLoad so it survives scene-group
//     changes, and destroys it (Object.Destroy) when the option or the module is turned off.
//
//   * Entity hitboxes (hitboxes.h): wireframes of the colliders of entities near the camera through the overlay's
//     world line segments (ui/hud.h); a physics query every 0.5 s, the shapes every frame while it is on.
//
//   * Highlight what the mod touches (highlight.h): labelled boxes of what other modules publish to
//     modules/common/debug_marks.h (slime_dj: pulsing lamps, other lamps, glowing ice, speakers), same drawing.
//
// Fault policy: all game calls run in a main-thread frame callback, polled every kPollSeconds (no per-frame work, no
// FindObjects of our own); the GameObject is held by a GC handle and checked with IsAlive before use. Direct method
// pointers; no hooks.

#include <windows.h>

#include <atomic>
#include <format>
#include <memory>
#include <string>

#include "core/il2cpp_api.h"
#include "core/log.h"
#include "core/main_thread.h"
#include "core/module.h"
#include "core/settings.h"
#include "core/unity.h"
#include "modules/debug_tools/highlight.h"
#include "modules/debug_tools/hitboxes.h"

namespace sr2::modules {
namespace {

namespace unity = sr2::core::unity;
namespace settings = sr2::core::settings;
namespace main_thread = sr2::core::main_thread;

constexpr const char* kModule = "debug_tools";
constexpr const char* kFontOverlay = "font_overlay";
constexpr const char* kHitboxes = "hitboxes";
constexpr const char* kHitboxRange = "hitbox_range";
constexpr const char* kHitboxTriggers = "hitbox_triggers";
constexpr const char* kHighlight = "highlight";
constexpr float kPollSeconds = 0.5f;  // reaction time to the toggle; also the liveness check interval
constexpr int kMaxStartFailures = 3;  // stop retrying StartDebugUI after this many failed starts in a row

std::atomic<bool> g_enabled{false};
std::atomic<bool> g_fontOverlay{false};
std::atomic<bool> g_hitboxes{false};
std::atomic<float> g_hitboxRange{12.0f};
std::atomic<bool> g_hitboxTriggers{false};
std::atomic<bool> g_highlight{false};

class DebugTools final : public sr2::core::Module {
public:
    const char* Name() const override { return kModule; }
    bool Setup(sr2::core::ModuleContext& ctx) override;
    void OnEnable() override { g_enabled.store(true); }
    void OnDisable() override { g_enabled.store(false); }  // the frame callback tears the overlay down

private:
    void Tick(float dt);
    void StartFontOverlay();
    void StopFontOverlay(const char* why);

    using StartFn = Il2CppObject* (*)(const MethodInfo*);         // static FontsUsedDebugUI StartDebugUI()
    using ObjFn = void (*)(Il2CppObject*, const MethodInfo*);    // static Object.Destroy / DontDestroyOnLoad
    StartFn start_ = nullptr;
    const MethodInfo* startMi_ = nullptr;
    ObjFn destroy_ = nullptr;
    const MethodInfo* destroyMi_ = nullptr;
    ObjFn keep_ = nullptr;  // optional: without it the overlay is recreated after a scene change
    const MethodInfo* keepMi_ = nullptr;

    Il2CppGCHandle overlay_ = nullptr;  // the "FontsUsedDebugUI" GameObject
    float sincePoll_ = kPollSeconds;
    int startFailures_ = 0;
    int starts_ = 0;
    int frameCb_ = 0;
    int onChange_ = 0;
    bool hitboxesOk_ = false;
};

bool DebugTools::Setup(sr2::core::ModuleContext& ctx) {
    auto& R = ctx.resolver();
    std::string err;
    Il2CppClass* fonts = R.Class("Assembly-CSharp", "MonomiPark.SlimeRancher.UI.Debug", "FontsUsedDebugUI", &err);
    if (!fonts) return ctx.Fail("FontsUsedDebugUI: " + err);
    startMi_ = R.Method(fonts, "StartDebugUI", 0, &err);
    start_ = startMi_ ? reinterpret_cast<StartFn>(R.MethodPointer(startMi_, &err)) : nullptr;
    if (!start_) return ctx.Fail("FontsUsedDebugUI.StartDebugUI: " + (err.empty() ? std::string("no native body") : err));
    Il2CppClass* uobject = R.Class("UnityEngine.CoreModule", "UnityEngine", "Object", &err);
    if (!uobject) return ctx.Fail("UnityEngine.Object: " + err);
    destroyMi_ = R.MethodByParams(uobject, "Destroy", {"UnityEngine.Object"}, &err);
    destroy_ = destroyMi_ ? reinterpret_cast<ObjFn>(R.MethodPointer(destroyMi_, &err)) : nullptr;
    if (!destroy_) return ctx.Fail("Object.Destroy(Object): " + (err.empty() ? std::string("no native body") : err));
    std::string kerr;
    keepMi_ = R.Method(uobject, "DontDestroyOnLoad", 1, &kerr);
    keep_ = keepMi_ ? reinterpret_cast<ObjFn>(R.MethodPointer(keepMi_, &kerr)) : nullptr;
    if (!keep_) SR2_INFO("[{}] Object.DontDestroyOnLoad not found ({}): the overlay is recreated after scene changes", kModule, kerr);

    settings::Def d;
    d.module = kModule;
    d.key = kFontOverlay;
    d.label = "Font debug overlay";
    d.group = "Developer tools";
    d.kind = settings::Kind::Bool;
    d.def = 0;
    d.min = 0;
    d.max = 1;
    d.step = 1;
    d.tooltip = "The developers' own font checker, left in the game: every text on screen gets a small dark label "
                "naming the font(s) it is drawn with, including fallback fonts for missing characters. Labels near "
                "the mouse are brighter. Purely visual; turn it off to remove it";
    settings::Register(d);
    // Entity hitboxes (hitboxes.h; optional: the font overlay works without it).
    std::string herr;
    hitboxesOk_ = debug_tools::hitboxes::Setup(ctx, herr);
    if (!hitboxesOk_) SR2_WARN("[{}] entity hitbox view unavailable ({})", kModule, herr);
    {
        settings::Def h;
        h.module = kModule;
        h.group = "Developer tools";
        h.key = kHitboxes;
        h.label = "Show entity hitboxes";
        h.kind = settings::Kind::Bool;
        h.def = 0;
        h.min = 0;
        h.max = 1;
        h.step = 1;
        h.tooltip = "Draws the colliders of things near you as wireframes: slimes, toys, food, plorts, gadgets and the "
                    "mod's buildables. Green = solid, yellow = trigger zones, blue = mesh colliders (their bounding box). "
                    "Hidden while this menu is open";
        settings::Register(h);
        settings::Def r;
        r.module = kModule;
        r.group = "Developer tools";
        r.key = kHitboxRange;
        r.label = "Hitbox range";
        r.kind = settings::Kind::Float;
        r.def = 12;
        r.min = 2;
        r.max = 40;
        r.step = 1;
        r.unit = " m";
        r.parent = kHitboxes;
        r.tooltip = "How far from the camera colliders are shown (the nearest 160 at most)";
        settings::Register(r);
        settings::Def t;
        t.module = kModule;
        t.group = "Developer tools";
        t.key = kHitboxTriggers;
        t.label = "Include trigger zones";
        t.kind = settings::Kind::Bool;
        t.def = 0;
        t.min = 0;
        t.max = 1;
        t.step = 1;
        t.parent = kHitboxes;
        t.tooltip = "Also the invisible trigger volumes (pickup and interaction zones, slime attach spots)";
        settings::Register(t);
        settings::Def l;
        l.module = kModule;
        l.group = "Developer tools";
        l.key = kHighlight;
        l.label = "Highlight what the mod touches";
        l.kind = settings::Kind::Bool;
        l.def = 0;
        l.min = 0;
        l.max = 1;
        l.step = 1;
        l.tooltip = "Draws a labelled box round the things the mod's features act on, so you can see what to check: "
                    "lamps that pulse to the music (pink), lamps with no playing speaker near (grey), the glowing ice "
                    "crystals of Powderfall Bluffs (cyan, also without music), and Slime DJ speakers (green while "
                    "playing, with the lamp range as a circle on the ground; grey when silent). Labels show within "
                    "80 m, boxes within 250 m. Hidden while this menu is open";
        settings::Register(l);
    }
    auto readAll = [] {
        g_fontOverlay.store(settings::GetBool(kModule, kFontOverlay));
        g_hitboxes.store(settings::GetBool(kModule, kHitboxes));
        g_hitboxRange.store(static_cast<float>(settings::GetFloat(kModule, kHitboxRange)));
        g_hitboxTriggers.store(settings::GetBool(kModule, kHitboxTriggers));
        g_highlight.store(settings::GetBool(kModule, kHighlight));
    };
    readAll();
    if (!onChange_) onChange_ = settings::OnChange(kModule, [readAll](const std::string&) { readAll(); });
    if (!frameCb_) frameCb_ = main_thread::AddFrameCallback(kModule, [this](float dt) { Tick(dt); });
    SR2_INFO("[{}] setup ok (FontsUsedDebugUI.StartDebugUI at RVA 0x{:X}; font overlay {}; entity hitboxes {})", kModule,
             sr2::il2cpp::Resolver::Rva(reinterpret_cast<void*>(start_)), g_fontOverlay.load() ? "on" : "off",
             !hitboxesOk_ ? "unavailable" : g_hitboxes.load() ? "on" : "off");
    return true;
}

void DebugTools::Tick(float dt) {
    if (hitboxesOk_)
        debug_tools::hitboxes::Tick(dt, g_enabled.load() && g_hitboxes.load(), g_hitboxRange.load(), g_hitboxTriggers.load());
    debug_tools::highlight::Tick(g_enabled.load() && g_highlight.load());
    sincePoll_ += dt;
    if (sincePoll_ < kPollSeconds) return;
    sincePoll_ = 0;
    const bool want = g_enabled.load() && g_fontOverlay.load();
    Il2CppObject* go = overlay_ ? unity::GCHandleTarget(overlay_) : nullptr;
    const bool alive = go && unity::IsAlive(go);
    if (overlay_ && !alive) {  // destroyed by us or by the game (scene unload without DontDestroyOnLoad)
        unity::FreeGCHandle(overlay_);
        overlay_ = nullptr;
    }
    if (want && !alive) {
        if (startFailures_ < kMaxStartFailures) StartFontOverlay();
    } else if (!want && alive) {
        StopFontOverlay(g_enabled.load() ? "option off" : "module off");
    }
    if (!want) startFailures_ = 0;  // a fresh toggle gets fresh attempts
}

void DebugTools::StartFontOverlay() {
    Il2CppObject* comp = start_(startMi_);
    Il2CppObject* go = comp && unity::IsAlive(comp) ? unity::GameObjectOf(comp) : nullptr;
    if (!go || !unity::IsAlive(go)) {
        ++startFailures_;
        SR2_WARN("[{}] FontsUsedDebugUI.StartDebugUI returned no live object (attempt {}/{})", kModule, startFailures_,
                 kMaxStartFailures);
        return;
    }
    if (keep_) keep_(go, keepMi_);
    overlay_ = unity::NewGCHandle(go);
    if (!overlay_) {  // cannot track it: remove it again rather than leave an overlay we cannot turn off
        destroy_(go, destroyMi_);
        ++startFailures_;
        SR2_WARN("[{}] font overlay: no GC handle, removed again (attempt {}/{})", kModule, startFailures_, kMaxStartFailures);
        return;
    }
    startFailures_ = 0;
    ++starts_;
    SR2_INFO("[{}] font overlay on (FontsUsedDebugUI started{}, start #{})", kModule,
             keep_ ? ", kept across scene loads" : "", starts_);
}

void DebugTools::StopFontOverlay(const char* why) {
    Il2CppObject* go = overlay_ ? unity::GCHandleTarget(overlay_) : nullptr;
    if (go && unity::IsAlive(go)) destroy_(go, destroyMi_);
    if (overlay_) unity::FreeGCHandle(overlay_);
    overlay_ = nullptr;
    SR2_INFO("[{}] font overlay off ({})", kModule, why);
}

}  // namespace

std::unique_ptr<sr2::core::Module> CreateDebugTools() { return std::make_unique<DebugTools>(); }

}  // namespace sr2::modules
