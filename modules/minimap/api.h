#pragma once
// Read-only view of the minimap's live state for other modules (e.g. the
// collectibles tracker drawn under the minimap). Main thread only. The raw
// pointers are owned (pinned) by the minimap and are only valid during the
// current frame: callers must not keep them across frames without their own
// GC handle, and must re-validate (IsAlive / class checks) before use.

#include "core/il2cpp_api.h"

namespace sr2::modules::minimap::api {

struct Snapshot {
    Il2CppObject* root = nullptr;         // "SR2Mods.Minimap" GameObject (our uGUI root, corner-anchored RectTransform)
    Il2CppObject* zone = nullptr;         // current ZoneDefinition (MapUIUtilities.GetPlayerCurrentZone)
    Il2CppObject* mapDefinition = nullptr;// MapDefinition of the current map (when a page was built)
    Il2CppObject* pagePrefab = nullptr;   // map page prefab GameObject (read-only asset; never instantiate/modify)
    Il2CppObject* mapComponent = nullptr; // its MonomiPark.SlimeRancher.UI.Map.Map component
    Il2CppObject* mapDirector = nullptr;  // MapDirector (ScriptableObject)
    // world (x, z) -> page-local (x, y): page = world * scale + offset (Map._anchor1/_anchor2, see NOTES.md);
    // page-local is the local space of mapComponent's RectTransform. Valid when mapComponent != nullptr.
    float scaleX = 0, scaleY = 0, offsetX = 0, offsetY = 0;
    bool visible = false;                 // minimap root currently shown
    bool loading = false;                 // scene load in progress (minimap hidden)
    float size = 0;                       // minimap edge length in canvas units (RectTransform sizeDelta)
    int position = 1;                     // 0 TL, 1 TR, 2 BL, 3 BR (minimap setting)
};

// False when the minimap module is not running (disabled, faulted, not set up).
bool Get(Snapshot& out);

}  // namespace sr2::modules::minimap::api
