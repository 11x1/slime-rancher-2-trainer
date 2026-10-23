#pragma once
// quick_start: "[modded]" line on the title screen (see modded_label.cpp).
//
// Main thread only, except Resolve (worker thread, during Setup).

#include <string>

#include "core/il2cpp_api.h"

namespace sr2::modules::quick_start {

class ModdedLabel {
public:
    // Resolves everything by name. False (with `why`) = feature unavailable.
    bool Resolve(std::string& why);
    bool Ok() const { return ok_; }

    // MainMenuLandingRootUI.OnEnable (post-hook, inside the module's DetourGuard): remembers the
    // menu; the label is built at the next Frame(). Cheap, makes no game calls but a GC handle.
    void OnMenuEnable(Il2CppObject* landing);
    // Module switched on: one FindObjectOfType for a main menu that is already up.
    void OnModuleEnable();
    // Every frame while the module is on: builds a pending label, re-asserts the text for a few
    // frames, drops the handles once the menu is gone. No scans.
    void Frame();
    // Destroys the label (module off). Safe when there is none.
    void Remove();

private:
    void Build(Il2CppObject* landing);
    void ApplyText();
    void ReleaseLanding();
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

    Il2CppGCHandle landing_ = nullptr;  // menu waiting for its label
    Il2CppGCHandle label_ = nullptr;    // our clone (GameObject)
    Il2CppGCHandle labelText_ = nullptr;  // its TMP_Text
    int reapply_ = 0;
    bool loggedBuilt_ = false, loggedFail_ = false;
};

}  // namespace sr2::modules::quick_start
