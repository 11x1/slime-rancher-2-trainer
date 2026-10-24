#pragma once
// custom_upgrades: more save-safe gadget variants on the Quantum Storage Link's hooks.
//
// The Quantum Storage Link (quantum_link.cpp) owns the hooks that make a runtime GadgetDefinition
// clone save-safe: SaveReferenceTranslation.GetPersistenceId, GadgetDirector.HasBlueprint /
// GetItemCount, GadgetDirectorHelper.HasBlueprint, GadgetFabricateCategory.GetItems,
// GadgetFabricatableItem.FabricateAndSpendCost, GadgetItem.PlaceGadgetEvent / PickupGadget and
// Gadget.ChangeVariant. MinHook allows one hook per address, so other clones (the Slime DJ buildables,
// dj_buildables.cpp) plug into those detours through this interface instead of hooking again. Each
// quantum detour asks the extension first (only for definitions it Owns) and passes the vanilla
// original as a callback where the answer depends on it. Everything works only while the quantum
// hooks are set up and armed (the extension is told nothing otherwise).
//
// Threading: SaveAs from any thread (pointer compares only); everything else on the Unity main thread,
// inside the quantum detours' DetourGuard.

#include <cstdint>
#include <functional>

struct Il2CppObject;

namespace sr2::modules::custom_upgrades::variant_ext {

struct PlaceCtx {
    bool ours = false;               // the held gadget is the extension's and gets listed after the placement
    Il2CppObject* before = nullptr;  // GadgetItem._lastPlacedGadget before the original ran
    int family = -1, kind = 0;       // extension data
};

class Extension {
public:
    virtual ~Extension() = default;
    // Any thread, pointer compares only: the vanilla definition `type` is saved as, else nullptr.
    virtual Il2CppObject* SaveAs(Il2CppObject* type) const = 0;
    // The definition is one of the extension's (variants and Fabricator card definitions).
    virtual bool Owns(Il2CppObject* def) const = 0;
    // GadgetDirector.HasBlueprint(def); `vanilla(d)` runs the original for d.
    virtual bool HasBlueprint(Il2CppObject* def, const std::function<bool(Il2CppObject*)>& vanilla) = 0;
    // GadgetDirectorHelper.HasBlueprint(def) (Fabricator card unlocked).
    virtual bool HelperBlueprint(Il2CppObject* def, const std::function<bool(Il2CppObject*)>& vanilla) = 0;
    // GadgetDirector.GetItemCount(def); `vanilla` = the original's answer.
    virtual int32_t ItemCount(Il2CppObject* def, int32_t vanilla) = 0;
    // After GadgetFabricateCategory.GetItems built `list` (any category; the quantum card is in already).
    virtual void InsertCards(Il2CppObject* category, Il2CppObject* list, Il2CppObject* helper, Il2CppObject* factory) = 0;
    // GadgetFabricatableItem.FabricateAndSpendCost for an owned card definition (never the original).
    virtual int32_t Fabricate(Il2CppObject* item, Il2CppObject* def, int32_t count) = 0;
    // GadgetItem.PlaceGadgetEvent: before / after the original (After only when ctx.ours).
    virtual void BeforePlace(Il2CppObject* gadgetItem, Il2CppObject* held, PlaceCtx& ctx) = 0;
    virtual void AfterPlace(Il2CppObject* gadgetItem, const PlaceCtx& ctx) = 0;
    // GadgetItem.PickupGadget: true = the gadget is one of the extension's; AfterPickup then runs after
    // the original with the same gadget item.
    virtual bool BeforePickup(Il2CppObject* gadgetItem, Il2CppObject* gadget) = 0;
    virtual void AfterPickup(Il2CppObject* gadgetItem) = 0;
    // Gadget.ChangeVariant(variant) for an owned variant: handled in place, the original is skipped.
    virtual void ChangeVariant(Il2CppObject* gadget, Il2CppObject* variant) = 0;
};

// Set once on the worker thread during Setup (before the hooks are enabled), never cleared.
void Set(Extension* ext);
Extension* Get();

}  // namespace sr2::modules::custom_upgrades::variant_ext
