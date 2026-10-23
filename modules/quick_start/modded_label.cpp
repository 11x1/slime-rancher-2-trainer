// quick_start: "[modded]" line on the title screen.
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
// One lookup per menu: MainMenuLandingRootUI.OnEnable (the module's post-hook) hands us the menu,
// the next frame runs one Transform.Find from it. The clone dies with the menu (scene change);
// we only drop our handles then. Module off: the clone is destroyed.

#include "modules/quick_start/modded_label.h"

#include <format>
#include <initializer_list>
#include <vector>

#include "core/detour_guard.h"
#include "core/log.h"
#include "core/main_thread.h"
#include "core/resolver.h"
#include "core/unity.h"

namespace sr2::modules::quick_start {
namespace {

namespace unity = sr2::core::unity;
namespace main_thread = sr2::core::main_thread;
using sr2::il2cpp::api;
using sr2::il2cpp::Resolver;

constexpr const char* kModule = "quick_start";
constexpr const char* kSupportPath = "MenuContent_OverscanAdjustment/LayoutGroup_Left (Labels)/SupportLabel";
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

}  // namespace

bool ModdedLabel::Resolve(std::string& why) {
    Resolver& R = Resolver::Get();
    std::string err;
    auto cls = [&](const char* image, const char* ns, const char* name) {
        Il2CppClass* k = R.Class(image, ns, name, &err);
        if (!k && why.empty()) why = err.empty() ? std::format("class {}.{} not found", ns, name) : err;
        return k;
    };
    landingClass_ = cls("Assembly-CSharp", "MonomiPark.SlimeRancher.UI.MainMenu", "MainMenuLandingRootUI");
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
    if (!m || sr2::core::ModuleFaulted(kModule)) return nullptr;
    std::vector<void*> params(args);
    std::string err;
    Il2CppObject* r = Resolver::Get().Invoke(m, obj, params.empty() ? nullptr : params.data(), &err);
    if (!err.empty()) {
        if (err.starts_with("native exception"))
            sr2::core::MarkModuleFaulted(kModule, std::format("modded label: {}: {}", what, err));
        else
            SR2_WARN("[{}] modded label: {} failed: {}", kModule, what, err);
        return nullptr;
    }
    if (ok) *ok = true;
    return r;
}

void ModdedLabel::OnMenuEnable(Il2CppObject* landing) {
    if (!ok_ || landing_ || !IsA(landing, landingClass_) || !unity::IsAlive(landing)) return;
    landing_ = unity::NewGCHandle(landing);
}

void ModdedLabel::OnModuleEnable() {
    if (!ok_) return;
    main_thread::Post([this] {
        if (landing_ || Target(label_)) return;
        Il2CppObject* m = unity::FindObjectOfType(landingClass_);
        if (m && IsA(m, landingClass_) && unity::IsAlive(m)) landing_ = unity::NewGCHandle(m);
    });
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
        if (!loggedFail_) SR2_WARN("[{}] modded label: {} - no [modded] line on the title screen", kModule, what);
        loggedFail_ = true;
    };
    bool ok = false;
    Il2CppObject* support = Call(trFind_, unity::TransformOf(landing), {unity::NewString(kSupportPath)},
                                 "Transform.Find", &ok);
    if (!ok || !support || !unity::IsAlive(support)) return fail("SupportLabel not found under the main menu");
    Il2CppObject* parent = Call(trFind_, unity::TransformOf(landing),
                                {unity::NewString("MenuContent_OverscanAdjustment/LayoutGroup_Left (Labels)")},
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
        SR2_INFO("[{}] modded label: \"{}\" added below the support URL on the title screen", kModule, kText);
    }
}

void ModdedLabel::ApplyText() {
    if (Il2CppObject* t = Target(labelText_)) Call(tmpSetText_, t, {unity::NewString(kText)}, "TMP_Text.set_text");
}

void ModdedLabel::Remove() {
    ReleaseLanding();
    if (Il2CppObject* go = Target(label_)) Call(objDestroy_, nullptr, {go}, "Object.Destroy");
    ReleaseLabel();
}

void ModdedLabel::ReleaseLanding() { Free(landing_); }

void ModdedLabel::ReleaseLabel() {
    Free(label_);
    Free(labelText_);
    reapply_ = 0;
}

}  // namespace sr2::modules::quick_start
