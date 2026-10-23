// "[modded]" line on the title screen, whenever the mod is loaded.
//
// Main menu prefab (scriptableobjectlookupdata bundle, read with UnityPy, SR2 1.3.0):
//   MainMenu (MainMenuLandingRootUI, CanvasScaler, ...)
//     > MenuContent_OverscanAdjustment
//       > LayoutGroup_Left (Labels)   VerticalLayoutGroup, child alignment LowerLeft, spacing 7,
//                                     anchored bottom-left (30, 30), 490 x 500
//         > SwitchUserButton
//         > Version Label             TMP "v x.y.z" + LocalizeStringEvent + LocalizedVersionText
//         > SupportLabel              TMP "support.slimerancher.com" (19.2 pt, grey-brown) +
//                                     LocalizeStringEvent + LocalizeFontEvent, 24 high
// We clone SupportLabel into the same layout group. Instantiate appends the clone as the last
// child, so the layout puts "[modded]" on the bottom row and the support URL one row up. The clone's
// LocalizeStringEvent is disabled (it would write the support URL back); LocalizeFontEvent stays,
// so the font follows the language like the original. Same font, size and colour as the URL.
//
// One lookup per menu: the MainMenuLandingRootUI.OnEnable post-hook hands us the menu, the next
// frame runs one Transform.Find from it. The clone dies with the menu (scene change); we only drop
// our handles then. Re-enabling the menu (back from a submenu) does nothing while the clone lives.
//
// Always on: hook module "ui" (enabled at start, never switched off). The label logic runs in a
// DetourGuard of its own fault name "modded_label": a fault turns only the label off (the guard
// becomes pass-through; no hooks belong to that name), the hook keeps serving the listener.

#include "ui/modded_label.h"

#include <atomic>
#include <format>
#include <initializer_list>
#include <string>
#include <vector>

#include "core/detour_guard.h"
#include "core/hook_manager.h"
#include "core/log.h"
#include "core/main_thread.h"
#include "core/resolver.h"
#include "core/unity.h"

namespace sr2::ui {
namespace {

namespace unity = sr2::core::unity;
namespace main_thread = sr2::core::main_thread;
using sr2::il2cpp::api;
using sr2::il2cpp::Resolver;

constexpr const char* kFault = "modded_label";  // DetourGuard / fault-flag name (owns no hooks)
constexpr const char* kUiModule = "ui";         // HookManager module of the OnEnable hook
constexpr const char* kHookName = "ui.main_menu_on_enable";
constexpr const char* kImage = "Assembly-CSharp";
constexpr const char* kMenuNs = "MonomiPark.SlimeRancher.UI.MainMenu";
constexpr const char* kSupportPath = "MenuContent_OverscanAdjustment/LayoutGroup_Left (Labels)/SupportLabel";
constexpr const char* kColumnPath = "MenuContent_OverscanAdjustment/LayoutGroup_Left (Labels)";
constexpr const char* kCloneName = "sr2mods_ModdedLabel";
constexpr const char* kText = "[modded]";
constexpr int kReapplyFrames = 10;  // the clone's localisation may still write once after Instantiate

bool IsA(Il2CppObject* obj, Il2CppClass* k) {
    const auto& a = api();
    if (!obj || !k || !a.il2cpp_object_get_class) return false;
    Il2CppClass* c = a.il2cpp_object_get_class(obj);
    if (c == k) return true;
    return c && a.il2cpp_class_is_assignable_from && a.il2cpp_class_is_assignable_from(k, c);
}

Il2CppObject* Target(Il2CppGCHandle h) {
    Il2CppObject* o = h ? unity::GCHandleTarget(h) : nullptr;
    return o && unity::IsAlive(o) ? o : nullptr;
}

void Free(Il2CppGCHandle& h) {
    if (h) unity::FreeGCHandle(h);
    h = nullptr;
}

// Main thread only, except Resolve (worker thread, during Start).
class ModdedLabel {
public:
    // Resolves everything by name. False (with `why`) = label unavailable.
    bool Resolve(std::string& why);
    bool Ok() const { return ok_; }

    // OnEnable post-hook (inside the label's DetourGuard): remembers the menu; the label is built
    // at the next Frame(). Cheap, makes no game calls but a GC handle.
    void OnMenuEnable(Il2CppObject* landing);
    // At start: one FindObjectOfType for a main menu that is already up.
    void FindExisting();
    // Every frame: builds a pending label, re-asserts the text for a few frames, drops the handles
    // once the menu is gone. No scans.
    void Frame();

private:
    void Build(Il2CppObject* landing);
    void ApplyText();
    void ReleaseLanding() { Free(landing_); }
    void ReleaseLabel();
    Il2CppObject* Call(const MethodInfo* m, void* obj, std::initializer_list<void*> args, const char* what,
                       bool* ok = nullptr);

    bool ok_ = false;
    Il2CppClass* landingClass_ = nullptr;  // MainMenuLandingRootUI
    Il2CppClass* tmpClass_ = nullptr;      // TMPro.TMP_Text
    Il2CppClass* lseClass_ = nullptr;      // UnityEngine.Localization.Components.LocalizeStringEvent
    const MethodInfo* trFind_ = nullptr;   // Transform.Find(string)
    const MethodInfo* objDestroy_ = nullptr;
    const MethodInfo* behSetEnabled_ = nullptr;
    const MethodInfo* tmpSetText_ = nullptr;

    Il2CppGCHandle landing_ = nullptr;    // menu waiting for its label
    Il2CppGCHandle label_ = nullptr;      // our clone (GameObject)
    Il2CppGCHandle labelText_ = nullptr;  // its TMP_Text
    int reapply_ = 0;
    bool loggedBuilt_ = false, loggedFail_ = false;
};

ModdedLabel g_label;
std::atomic<bool> g_hooked{false};
std::atomic<modded_label::MenuEnableFn> g_listener{nullptr};

sr2::core::DetourGuard g_enableGuard{kFault, "MainMenuLandingRootUI.OnEnable"};
sr2::core::DetourGuard g_frameGuard{kFault, "frame"};

using VoidFn = void (*)(Il2CppObject*, const MethodInfo*);
VoidFn g_origMenuEnable = nullptr;

void MenuEnableDetour(Il2CppObject* self, const MethodInfo* mi) {
    g_origMenuEnable(self, mi);
    if (!self || !main_thread::IsMainThread()) return;
    if (g_label.Ok()) g_enableGuard.Run([&] { g_label.OnMenuEnable(self); });
    if (auto fn = g_listener.load(std::memory_order_acquire)) fn(self);  // guarded by the listener
}

bool ModdedLabel::Resolve(std::string& why) {
    Resolver& R = Resolver::Get();
    std::string err;
    auto cls = [&](const char* image, const char* ns, const char* name) {
        Il2CppClass* k = R.Class(image, ns, name, &err);
        if (!k && why.empty()) why = err.empty() ? std::format("class {}.{} not found", ns, name) : err;
        return k;
    };
    landingClass_ = cls(kImage, kMenuNs, "MainMenuLandingRootUI");
    tmpClass_ = cls("Unity.TextMeshPro", "TMPro", "TMP_Text");
    lseClass_ = cls("Unity.Localization", "UnityEngine.Localization.Components", "LocalizeStringEvent");
    Il2CppClass* uobject = cls("UnityEngine.CoreModule", "UnityEngine", "Object");
    Il2CppClass* behaviour = cls("UnityEngine.CoreModule", "UnityEngine", "Behaviour");
    Il2CppClass* transform = unity::TransformClass();
    if (!transform && why.empty()) why = "class UnityEngine.Transform not found";
    if (!why.empty()) return false;
    auto need = [&](const MethodInfo* m, const char* what) {
        if (!m && why.empty()) why = err.empty() ? std::format("method {} not found", what) : err;
        return m;
    };
    trFind_ = need(R.MethodByParams(transform, "Find", {"System.String"}, &err), "Transform.Find");
    objDestroy_ = need(R.MethodByParams(uobject, "Destroy", {"UnityEngine.Object"}, &err), "Object.Destroy");
    behSetEnabled_ = need(R.Method(behaviour, "set_enabled", 1, &err), "Behaviour.set_enabled");
    tmpSetText_ = need(R.Method(tmpClass_, "set_text", 1, &err), "TMP_Text.set_text");
    ok_ = why.empty();
    return ok_;
}

Il2CppObject* ModdedLabel::Call(const MethodInfo* m, void* obj, std::initializer_list<void*> args, const char* what,
                                bool* ok) {
    if (ok) *ok = false;
    if (!m || sr2::core::ModuleFaulted(kFault)) return nullptr;
    std::vector<void*> params(args);
    std::string err;
    Il2CppObject* r = Resolver::Get().Invoke(m, obj, params.empty() ? nullptr : params.data(), &err);
    if (!err.empty()) {
        if (err.starts_with("native exception"))
            sr2::core::MarkModuleFaulted(kFault, std::format("modded label: {}: {}", what, err));
        else
            SR2_WARN("ui: modded label: {} failed: {}", what, err);
        return nullptr;
    }
    if (ok) *ok = true;
    return r;
}

void ModdedLabel::OnMenuEnable(Il2CppObject* landing) {
    if (!ok_ || landing_ || !IsA(landing, landingClass_) || !unity::IsAlive(landing)) return;
    landing_ = unity::NewGCHandle(landing);
}

void ModdedLabel::FindExisting() {
    if (!ok_ || landing_ || Target(label_)) return;
    Il2CppObject* m = unity::FindObjectOfType(landingClass_);
    if (m && IsA(m, landingClass_) && unity::IsAlive(m)) landing_ = unity::NewGCHandle(m);
}

void ModdedLabel::Frame() {
    if (!ok_) return;
    if (landing_) {
        Il2CppObject* m = Target(landing_);
        if (m && IsA(m, landingClass_) && !Target(label_)) Build(m);
        ReleaseLanding();
    }
    if (label_ && !Target(label_)) {  // died with the menu
        ReleaseLabel();
        return;
    }
    if (reapply_ > 0) {
        --reapply_;
        ApplyText();
    }
}

void ModdedLabel::Build(Il2CppObject* landing) {
    ReleaseLabel();
    auto fail = [&](const char* what) {
        if (!loggedFail_) SR2_WARN("ui: modded label: {} - no [modded] line on the title screen", what);
        loggedFail_ = true;
    };
    bool ok = false;
    Il2CppObject* support = Call(trFind_, unity::TransformOf(landing), {unity::NewString(kSupportPath)},
                                 "Transform.Find", &ok);
    if (!ok || !support || !unity::IsAlive(support)) return fail("SupportLabel not found under the main menu");
    Il2CppObject* parent = Call(trFind_, unity::TransformOf(landing), {unity::NewString(kColumnPath)},
                                "Transform.Find", &ok);
    if (!ok || !parent || !unity::IsAlive(parent)) return fail("the label column was not found");
    Il2CppObject* clone = unity::Instantiate(unity::GameObjectOf(support), parent);
    Il2CppObject* go = clone && IsA(clone, unity::GameObjectClass()) ? clone : unity::GameObjectOf(clone);
    if (!go || !unity::IsAlive(go)) return fail("cloning SupportLabel failed");
    unity::SetName(go, kCloneName);
    if (Il2CppObject* lse = unity::GetComponent(go, lseClass_); lse && unity::IsAlive(lse)) {
        bool off = false;
        Call(behSetEnabled_, lse, {&off}, "Behaviour.set_enabled");
    }
    Il2CppObject* text = unity::GetComponent(go, tmpClass_);
    if (!text || !unity::IsAlive(text) || !IsA(text, tmpClass_)) {
        Call(objDestroy_, nullptr, {go}, "Object.Destroy");
        return fail("the clone has no text component");
    }
    label_ = unity::NewGCHandle(go);
    labelText_ = unity::NewGCHandle(text);
    reapply_ = kReapplyFrames;
    ApplyText();
    if (!loggedBuilt_) {
        loggedBuilt_ = true;
        SR2_INFO("ui: modded label: \"{}\" added below the support URL on the title screen", kText);
    }
}

void ModdedLabel::ApplyText() {
    if (Il2CppObject* t = Target(labelText_)) Call(tmpSetText_, t, {unity::NewString(kText)}, "TMP_Text.set_text");
}

void ModdedLabel::ReleaseLabel() {
    Free(label_);
    Free(labelText_);
    reapply_ = 0;
}

}  // namespace

namespace modded_label {

bool SetMenuEnableListener(MenuEnableFn fn) {
    g_listener.store(fn, std::memory_order_release);
    return g_hooked.load(std::memory_order_acquire);
}

}  // namespace modded_label

// Worker thread, before the modules are set up (quick_start registers its listener in Setup).
bool StartModdedLabel() {
    static bool started = false;
    if (started) return true;
    started = true;

    std::string why;
    if (!g_label.Resolve(why)) SR2_WARN("ui: modded label: unavailable: {}", why);

    // The hook also serves the listener, so it goes in even when the label did not resolve.
    std::string err;
    void* target = Resolver::Get().MethodPointer(kImage, kMenuNs, "MainMenuLandingRootUI", "OnEnable", 0, &err);
    auto& hm = sr2::core::HookManager::Get();
    if (!target) {
        SR2_ERROR("ui: modded label: MainMenuLandingRootUI.OnEnable not found: {}", err);
        return false;
    }
    if (!hm.Create(kUiModule, kHookName, target, reinterpret_cast<void*>(&MenuEnableDetour),
                   reinterpret_cast<void**>(&g_origMenuEnable), &err) ||
        !hm.Enable(kHookName, &err)) {
        hm.Remove(kHookName);
        SR2_ERROR("ui: modded label: hook installation failed: {}", err);
        return false;
    }
    g_hooked.store(true, std::memory_order_release);
    if (!g_label.Ok()) return true;  // hook only (for the listener)

    if (main_thread::AddFrameCallback("ui.modded_label", [](float) { g_frameGuard.Run([] { g_label.Frame(); }); }) ==
        0) {
        SR2_WARN("ui: modded label: frame callback unavailable - no [modded] line on the title screen");
        return true;
    }
    main_thread::Post([] { g_frameGuard.Run([] { g_label.FindExisting(); }); });
    return true;
}

}  // namespace sr2::ui
