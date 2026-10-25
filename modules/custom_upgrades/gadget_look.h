#pragma once
// custom_upgrades: how the mod's gadget definitions look in the game UI, for every runtime clone (the
// Quantum Storage Link, the Slime DJ buildables). SR2 1.3.0, all lookups by name.
//
// Our definitions are Object.Instantiate copies of a vanilla GadgetDefinition: same prefab, same pedia
// entry, own localizedName / _localizedDescription (core/text_override instances). Three vanilla paths
// ignore those (disassembly, RVAs for reference):
//   * GadgetDefinition.get_LocalizedDescription (0x51B440) walks VariantOf to the root definition and
//     returns ITS description: a variant's Fabricator card (GadgetFabricatableItem.get_Description ->
//     FabricatorItemDetails.SetItem) showed the base gadget's text.
//   * The gadget-mode label (GadgetLabelDisplay.UpdateLabels 0x75F820) comes from
//     GadgetDirector.GetTargetGadgetInfo(def, includeCount, isInRanch, droneType) (0x868E80):
//     DisplayName = def._pediaLink's title (shared with the base), SubName = Ranch/RangeLocalizedName.
//     For the held gadget the def is GadgetDirector.SelectedSlottedGadget (our variant after the toggle);
//     for an aimed-at placed gadget it is Gadget.IdentTypeAsDefinition (the vanilla definition: a placed
//     clone IS a vanilla gadget).
//   * The placement preview: GadgetItem.SetHeldGadget (0xA46810) destroys the old placeholder and builds
//     `_gadgetPlaceholderInstance` from GetGadgetDefinitionToPlace(def).prefab (CopyPlaceholderGameObject
//     + CopyMeshComponents / CopyGadgetComponents / CopySpecialComponents): the base's prefab, so the base's
//     look. Its only native callers: PickupGadget, OnItemActivated, PlaceGadgetEvent,
//     HandleSelectedSlottedGadgetChanged (the variant toggle lands here through SetFavorite).
//   * The Fabricator card's details picture (FabricatorItemDetails.SetItem 0x8089B0): a
//     UIImageDynamicLoader loads the definition's full art (IFullArtAndIconSource.FullArtReference, the
//     base's addressable sprite, copied by Instantiate); without a valid reference it leaves the image
//     alone (RequiresFullArt false).
// This file owns those four hooks (MinHook: one hook per address - nobody else may hook them) and asks
// what Register()ed: the description, label and card picture of a registered definition, and its preview
// dresser after SetHeldGadget. Placed-gadget labels: AddPlacedLabel() providers, asked when the label is for an aimed-at
// placed gadget (not for the held one).
//
// Helpers for the looks: TintTree (instanced materials tinted, like the placed quantum links) and
// TintedSprite (a recoloured copy of a sprite, read back from the GPU: game textures are not readable).
//
// Threading: Setup on the worker thread; everything else on the Unity main thread. The description
// detour also answers on other threads (atomic pointer compares only).

#include <functional>
#include <string>

struct Il2CppObject;

namespace sr2::core {
class ModuleContext;
}

namespace sr2::modules::custom_upgrades::gadget_look {

struct Rgb {
    float r = 1, g = 1, b = 1;
};

struct Look {
    Il2CppObject* name = nullptr;         // LocalizedString: the gadget-mode label while held (nullptr: vanilla)
    Il2CppObject* description = nullptr;  // LocalizedString: GadgetDefinition.LocalizedDescription (Fabricator card)
    // Sprite for the Fabricator card's details picture (nullptr: the base's full art). Register() then also
    // clears the definition's full-art reference (IdentifiableType._fullArt / _requiresFullArt) so the
    // details' UIImageDynamicLoader loads nothing over it. The grid tile and hotbar use
    // GadgetDefinition.icon: set that field yourself (get_Icon returns it when not null).
    Il2CppObject* icon = nullptr;
    // artAmount > 0 (with `icon` set): the details picture becomes the base's full art (the 512 px addressable
    // sprite, copied by Instantiate) recoloured `artAmount` towards `artTint` (TintedSprite), instead of
    // `icon` stretched up from its 128 px. The full-art reference stays until the loader has loaded it once
    // (the first time the card is shown: `icon` shows until then); the recoloured copy is kept for the
    // session and the reference is cleared. Falls back to `icon` when the art does not load in 10 s.
    Rgb artTint;
    float artAmount = 0;
    // After GadgetItem.SetHeldGadget built the placement preview for this definition (the preview's root
    // GameObject; inside the hook's DetourGuard). nullptr: the base's preview.
    std::function<void(Il2CppObject* placeholder)> dressPreview;
};

// Worker thread, before the hooks are enabled. false = unavailable (`why`): vanilla labels / previews /
// descriptions (everything else of the buildables keeps working). `report` gets " name@0xRVA" per hook.
bool Setup(sr2::core::ModuleContext& ctx, const char* module, std::string& why, std::string& report);
bool Ready();

// Main thread. `def` must stay alive for the session (pinned by the caller); the LocalizedStrings too.
// Registering again replaces the look. At most 24 definitions.
void Register(Il2CppObject* def, Look look);
void Unregister(Il2CppObject* def);

// A label for an aimed-at placed gadget (`gadget` = the Gadget component, `def` = its vanilla definition):
// a LocalizedString, or nullptr for "not mine". Providers are asked in the order added.
using PlacedLabel = std::function<Il2CppObject*(Il2CppObject* gadget, Il2CppObject* def)>;
void AddPlacedLabel(PlacedLabel provider);  // worker or main thread, before / during play (not from a hook)

// Main thread helpers ------------------------------------------------------------------------------

// Instances every material under `root` (Renderer.materials, once: instances get " [sr2look]" in their
// name) and tints colour properties (color / tint / albedo) `amount` towards `tint`, emission
// properties (emiss / glow) to `glow` x their brightness (HDR). Returns the number of materials tinted.
// `what` names the object in the one log line per new shader.
int TintTree(Il2CppObject* root, Rgb tint, float amount, Rgb glow, const char* what);

// A recoloured copy of `sprite` (same rect size, pivot and pixels per unit; a trimmed atlas sprite keeps
// its full rect with transparent padding): every pixel towards `tint` by `amount`, keeping its brightness
// and alpha. `square`: the rect centred on a square canvas (for UI images that stretch the sprite to a
// square box, e.g. the drone station's source icons). The texture is copied on the GPU (Graphics.Blit
// into a RenderTexture, Texture2D.ReadPixels), so the source needs no Read/Write flag. Kept from
// unloading for the session. nullptr and `why` on failure.
Il2CppObject* TintedSprite(Il2CppObject* sprite, Rgb tint, float amount, std::string& why, bool square = false);

}  // namespace sr2::modules::custom_upgrades::gadget_look
