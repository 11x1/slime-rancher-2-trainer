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

namespace sr2::modules {
namespace {

namespace unity = sr2::core::unity;
namespace settings = sr2::core::settings;
namespace main_thread = sr2::core::main_thread;

constexpr const char* kModule = "debug_tools";
constexpr const char* kFontOverlay = "font_overlay";
constexpr float kPollSeconds = 0.5f;  // reaction time to the toggle; also the liveness check interval
constexpr int kMaxStartFailures = 3;  // stop retrying StartDebugUI after this many failed starts in a row

std::atomic<bool> g_enabled{false};
std::atomic<bool> g_fontOverlay{false};

class DebugTools final : public sr2::core::Module {
public:
    const char* Name() const override { return kModule; }
    bool DefaultEnabled() const override { return true; }  // every tool is off by default
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
    g_fontOverlay.store(settings::GetBool(kModule, kFontOverlay));
    if (!onChange_)
        onChange_ = settings::OnChange(kModule, [](const std::string&) { g_fontOverlay.store(settings::GetBool(kModule, kFontOverlay)); });
    if (!frameCb_) frameCb_ = main_thread::AddFrameCallback(kModule, [this](float dt) { Tick(dt); });
    SR2_INFO("[{}] setup ok (FontsUsedDebugUI.StartDebugUI at RVA 0x{:X}; font overlay {})", kModule,
             sr2::il2cpp::Resolver::Rva(reinterpret_cast<void*>(start_)), g_fontOverlay.load() ? "on" : "off");
    return true;
}

void DebugTools::Tick(float dt) {
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
