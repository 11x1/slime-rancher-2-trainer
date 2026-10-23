#pragma once
// Read-only view of the minimap's live state for other modules (e.g. the
// collectibles tracker drawn under the minimap). Main thread only. The raw
// pointers are owned (pinned) by the minimap and are only valid during the
// current frame: callers must not keep them across frames without their own
// GC handle, and must re-validate (IsAlive / class checks) before use.

#include <vector>

#include "core/il2cpp_api.h"

namespace sr2::modules::minimap::api {

struct Snapshot {
    Il2CppObject* root = nullptr;         // "SR2Mods.Minimap" GameObject (our uGUI root, freely placed RectTransform)
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
    int position = 1;                     // nearest corner of the placement: 0 TL, 1 TR, 2 BL, 3 BR
};

// False when the minimap module is not running (disabled, faulted, not set up).
bool Get(Snapshot& out);

// Icon categories, in the order of the minimap's own "Icons" settings.
enum IconCat {
    kIconTeleporter = 0,  // page markers: ancient teleporters
    kIconTreasurePod,
    kIconGordo,           // page markers
    kIconResource,
    kIconOther,           // page markers: plort doors, landmarks, ...
    kIconNullWall,
    kIconPlaced,          // placed gadgets (master switch; the three groups below refine it)
    kIconDronePort,
    kIconPlacedTele,
    kIconPlacedDecor,
    kIconCatCount
};

struct PageIcon {
    int cat = kIconOther;
    float x = 0, y = 0;              // page-local position (see Snapshot::scaleX)
    Il2CppObject* sprite = nullptr;  // pinned by the minimap; valid this frame only
    float r = 1, g = 1, b = 1, a = 1;
};

struct PageIconQuery {
    bool cats[kIconCatCount] = {};
    // Page markers (teleporters, gordos, other) are drawn by the full map itself once discovered. With this
    // on, the ones the full map does NOT show yet (not discovered) are returned, unless they are done
    // (popped gordo, solved plort door). Off: no page markers at all.
    bool undiscoveredMarkers = false;
};

// Every icon of the minimap's current map page (the whole page, not only the minimap's view): treasure pods,
// resource nodes, Nullifier walls and placed gadgets from the minimap's world scanners (done states applied,
// fog NOT applied), plus undiscovered page markers if asked. `mapDefinition` = the page's MapDefinition (the
// icons only fit that map). Main thread; costs a full scan, so call it every few seconds at most. False when
// the minimap is not running or has no page right now.
bool CollectPageIcons(const PageIconQuery& query, std::vector<PageIcon>& out, Il2CppObject** mapDefinition);

}  // namespace sr2::modules::minimap::api
