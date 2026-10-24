#pragma once
// custom_upgrades: how the mod's runtime gadget definitions look in gadget (placement) mode - shared by every
// buildable that is a runtime clone of a vanilla gadget (the Slime DJ buildables today; the Quantum Storage Link can
// register the same way).
//
// Round 9: "custom buildables (mod ones) use the default label and the preview is also default". Both come from the
// vanilla gadget the clone shares its prefab and pedia entry with:
//   * The ghost: GadgetItem.SetHeldGadget(def) (0xA46810) destroys the old placeholder and builds a new one from
//     def.prefab (CopyPlaceholderGameObject: a GameObject tree with the prefab's names, its meshes copied, every
//     object put on layer 26 = the gadget overlay pass). A clone's prefab is the vanilla one, so the ghost is too.
//   * The label: GadgetLabelDisplay.UpdateLabels (0x75F820) shows GadgetDirector.GetTargetGadgetInfo(def): the
//     DisplayName is def._pediaLink's title, again the vanilla gadget's. The def is SelectedSlottedGadget while
//     gadget mode is on and GadgetDirector._isTargetingPlaceholder, else the targeted placed gadget's definition.
// What this does: SetHeldGadget is hooked (after the original): when a provider wants the held definition, the
// placeholder is handed to it on the next frame (once per placeholder: it builds under a child named kPreviewChild).
// UpdateLabels is hooked (after the original): the name label gets the provider's LocalizedString for the selected /
// held definition, or for the targeted placed gadget. Nothing else hooks either method (MinHook: one hook per
// address). Both detours run the original first and only add to it.
//
// Threading: Add on the worker thread before Setup; the hooks and providers on the Unity main thread.

#include <string>

struct Il2CppObject;

namespace sr2::core {
class ModuleContext;
}

namespace sr2::modules::custom_upgrades::placement_look {

constexpr const char* kPreviewChild = "sr2look";  // the provider's objects under the placeholder
constexpr int kPreviewLayer = 26;                 // the gadget overlay layer the placeholder is drawn on

class Provider {
public:
    virtual ~Provider() = default;
    // Any thread, pointer compares: `def` is one of the provider's definitions.
    virtual bool Wants(Il2CppObject* def) const = 0;
    // Main thread: dress the placeholder (a GameObject named like the prefab's root) for `def`: build a child named
    // kPreviewChild under it, script- and collider-free, then SetLayerRecursive(child, kPreviewLayer).
    virtual void DressPreview(Il2CppObject* def, Il2CppObject* placeholder) = 0;
    // Main thread: the name for `def` (a LocalizedString), nullptr = keep the vanilla label.
    virtual Il2CppObject* Label(Il2CppObject* def) = 0;
    // Main thread: the name for a placed gadget the player aims at (a Gadget component), nullptr = vanilla.
    virtual Il2CppObject* TargetLabel(Il2CppObject* gadget) = 0;
};

// Worker thread, before Setup (at most 8).
void Add(Provider* provider);
// Worker thread: resolves and hooks. false + why: no label / preview changes (the buildables still work).
bool Setup(sr2::core::ModuleContext& ctx, std::string& why);
// Main thread: `goOrComp`'s GameObject and everything under it on `layer`.
void SetLayerRecursive(Il2CppObject* goOrComp, int layer);

}  // namespace sr2::modules::custom_upgrades::placement_look
