#pragma once
// Extra entries in the game's own key bar (InputLegend), shared by map_tools ("[RMB] Teleport" on the full
// map), sandbox ("[G] Add materials" in the Fabricator) and drone_price_limit ("[P] Programs" in the drone
// station's program page; it adds its own click target as a child of the clone).
//
// Game 1.3.0 (dump + IDA, see modules/map_tools/NOTES.md "[RMB] Teleport in the key bar"):
//   * InputLegend._hints (InputHint[]) are the bar's entries, children of one layout. Each InputHint has an
//     InputHintConfiguration (_configuration: Label, InputEvent, AdditionalInputEvent, ...).
//   * A ComponentInterface<IInputLegend> (ComponentInterfaceBase._object) may hold an
//     InputLegendFocusRegistrar that forwards to another IInputLegend through its own _passThrough.
//   * An entry of ours is a clone of one of the bar's hints (same parent, so it lines up) with its behaviour
//     removed: InputHint and InvokeInputEventOnPointerClick destroyed (they would reconfigure it from the
//     template's input event and fire it on click), InputEventDisplay / LocalizeStringEvent / Selectables
//     disabled, no raycast targets, CanvasGroup alpha 1. The first InputIcon shows our key cap (as
//     InputIcon.SetKey) or a sprite (as InputIcon.SetButtonImage); the label TMP (outside every icon) our text.
//
// Everything runs on the Unity main thread inside the caller's fault guard (core::Try + ugui::GuardNative):
// the calls here are not guarded one by one. Needs ugui::Resolve() first.

#include <string>
#include <string_view>
#include <vector>

#include "core/il2cpp_api.h"
#include "modules/minimap/ugui.h"

namespace sr2::modules::legend_hint {

// Worker thread, idempotent. False + reason when the key bar classes / fields are missing.
bool Resolve(std::string& err);
bool Resolved();

// The InputLegend behind `obj`: an InputLegend itself, or a ComponentInterface<IInputLegend> / an
// InputLegendFocusRegistrar (followed through _passThrough, up to 8 hops). Fallback: the first active
// InputLegend under the chain's last component, then under `root` (may be null). `chain` describes the walk
// (starts with `start`), for the log.
Il2CppObject* FollowLegend(Il2CppObject* obj, Il2CppObject* root, std::string_view start, std::string& chain);

// The legend's hints (at most 32). `active`: only those active in the hierarchy.
std::vector<Il2CppObject*> Hints(Il2CppObject* legend, bool active);
// A hint to clone: the first active one that takes input (InputHint._inputEnabled; a disabled entry is drawn
// dimmed), else the first active one; nullptr when none is shown yet.
Il2CppObject* Template(Il2CppObject* legend);
// The active hint whose configuration's InputEvent (or AdditionalInputEvent) is `inputEvent`, else nullptr.
Il2CppObject* HintFor(Il2CppObject* legend, Il2CppObject* inputEvent);

class Clone {
public:
    // Clones the InputHint `tmpl` next to it (last sibling), named `name`. False + why.
    bool Create(Il2CppObject* tmpl, std::string_view name, std::string& why);
    // What the clone shows: `key` in a key cap, or `glyph` (a Sprite; mirrored = local scale x -1) when set,
    // and `label`. Applied now and re-applied for a few frames (the layout may still configure itself).
    void SetLook(std::string_view key, std::string_view label, Il2CppObject* glyph = nullptr, bool mirrored = false);
    void Tick();  // the re-applies; call every frame while the clone exists
    // Sibling order: right before `hint` (one of the bar's entries); nullptr = last.
    void PlaceBefore(Il2CppObject* hint);
    void Destroy();
    void Abandon();  // after a native fault: forget everything without touching IL2CPP

    bool Exists() const { return static_cast<bool>(go_); }  // created (it may have died with the bar)
    bool Alive() const { return go_.Alive(); }
    Il2CppObject* GameObject() const { return go_.get(); }
    size_t IconCount() const { return icons_.size(); }
    bool HasLabel() const { return static_cast<bool>(label_); }

private:
    void Apply();
    minimap::ugui::Ref go_, label_, glyph_;
    std::vector<minimap::ugui::Ref> icons_;
    std::string key_, text_;
    bool mirrored_ = false;
    int reapply_ = 0;
};

}  // namespace sr2::modules::legend_hint
