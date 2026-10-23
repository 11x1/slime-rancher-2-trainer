// collectibles: a compact per-zone collectible tracker drawn directly under
// (or, for a bottom-corner minimap, above) the minimap.
//
// Data sources:
//   * Treasure pods / shadow plort depositors: the game's own map counters,
//     MapDirector._counters (List<ZoneToCounterMapping{Zone, Counter}>), via
//     the virtual MapCounterDefinition.GetState() -> MapCounterState{Current,
//     Maximum, Image}. The counters hold static ID lists, so the totals include
//     unvisited / unloaded areas (that is what the full map's counter shows).
//   * Gordos, plort doors, shadow plort doors, shadow depositors (if no counter),
//     ancient teleporters, stabilizer gates: the baked markers of the current
//     map page prefab (read only, the prefab the minimap already holds through
//     modules/minimap/api.h). The prefab covers the whole map, so totals include
//     unvisited areas. Zone of a marker: GordoMarkerUI._zone, else the page's
//     ZoneDetectionArea polygons, else the nearest zone label marker.
//     Done state: GameModel gordo model popped / CalculateIsSolved() / the
//     marker's discovery query (IFilterableMapMarker.CanBeShown).
//   * Map data nodes: the page prefab's FogMapElements (one fog area per reveal
//     event = per node; distinct StaticGameEvents per zone, zone by polygon of
//     the area's art), so totals include unvisited areas. Done: the full map's
//     own reveal query FogMapElement.ShouldBeRevealed(EventDirector), only once
//     the director's record IS the loaded save's (GameModel.eventRecordModel).
//   * Research drones: no static per-zone list exists (GameModel.researchDrones
//     only holds drones whose scene registered them; ResearchDroneEntry has no
//     zone); counted from the drones loaded this session (ResearchDroneController,
//     scene scan). A zone is loaded as a whole (ZoneSceneGroupMapping: zone ->
//     SceneGroup[], a SceneGroup loads all its scenes), so once a scan ran while
//     the player stood in a zone that maps to exactly ONE scene group, that
//     zone's total is complete; otherwise it is marked with '*' (known so far).
//     Read state: the controller's model, re-read at 1 Hz while it is loaded.
//     Scan cost: one Object.FindObjectsByType(includeInactive) walks every
//     scene object (~15-20 ms each in the open world), so the scan runs once
//     per zone, kZoneSettle s after entering it, drones and ghosts on two
//     different frames; only zones that are not loaded as a whole are rescanned
//     (every kPartialRescan s) to pick up scenes that stream in later.
//   * Ghostly drones: ComponentAcqDroneSpawner sits on a TreasurePod (InitDrone:
//     SRGetComponent<TreasurePod>); the drone carries that pod and opening it
//     awards it. Spawners are scene objects (no static list), so they are
//     collected by the same low-rate scan as research drones (same '*' rule
//     for zones not fully scanned, same once-per-zone schedule). Zone: the pod's
//     GetZoneId() (CellDirector), else the zone polygons. Done: pod OPEN
//     (TreasurePod.get_CurrState), re-read at 1 Hz while the pod is loaded.
//
// UI: our own uGUI objects parented to the minimap's root GameObject, so they
// follow its position, opacity and visibility (hidden with the HUD, menus,
// the full map, loads). No per-frame work besides an "is the parent still
// ours" check; counts are recomputed once per second while shown. There is no
// background panel: compact mode is one row of "icon X/Y" per category,
// spread over the minimap's width (a second row only when it does not fit),
// list mode one text line per category; every icon and text has a dark copy
// offset down-right as a drop shadow, so it stays readable on bright terrain.
// Icons: the game's own sprite where one exists (map marker / counter /
// identifiable icon); map data nodes, research drones and ghostly drones have
// none (the ghostly drone's IdentifiableType has no icon), so they use the
// mod's own icons (assets/icons/map_node, research_drone, ghost_drone, via
// ui/mod_icons).
//
// Fault policy: every game call runs under core::Try + a native-fault guard;
// managed exceptions of single queries are contained, any native fault
// disables the module for the session (references abandoned, no teardown
// calls). No hooks.

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <format>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "core/detour_guard.h"
#include "core/guard.h"
#include "core/il2cpp_api.h"
#include "core/log.h"
#include "core/main_thread.h"
#include "core/module.h"
#include "core/resolver.h"
#include "core/settings.h"
#include "core/unity.h"
#include "modules/minimap/api.h"
#include "modules/minimap/ugui.h"
#include "ui/mod_icons.h"

namespace sr2::modules {
namespace {

namespace settings = sr2::core::settings;
namespace unity = sr2::core::unity;
namespace main_thread = sr2::core::main_thread;
namespace ugui = sr2::modules::minimap::ugui;
namespace mapi = sr2::modules::minimap::api;
using il2cpp::Resolver;
using ugui::Color;
using ugui::Ref;
using ugui::Vec2;
using ugui::Vec3;

constexpr const char* kModule = "collectibles";
constexpr const char* kAsm = "Assembly-CSharp";
constexpr size_t kNoOff = static_cast<size_t>(-1);
constexpr float kRecomputeInterval = 1.0f;
constexpr float kPartialRescan = 30.0f;  // scene rescan of a zone that is not loaded as a whole
constexpr float kZoneSettle = 3.0f;  // a zone's scenes count as fully loaded this long after entering it

enum Cat {
    kPods = 0, kGordos, kShadowDepositors, kShadowDoors, kPlortDoors, kTeleporters, kStabilizers, kMapNodes, kDrones,
    kGhostDrones, kCatCount
};
struct CatInfo {
    const char* key;
    const char* label;   // list mode / settings
    const char* shortLabel;  // compact mode without an icon
    bool discoveryOnly;  // "done" == discovered (no other state)
    const char* tip;
};
const CatInfo kCats[kCatCount] = {
    {"cat_treasure_pods", "Treasure pods", "Pods", false,
     "Opened / total, from the game's own map counter (includes unvisited areas)"},
    {"cat_gordos", "Gordos", "Gordos", false, "Popped / total gordos on the map page of this zone"},
    {"cat_shadow_depositors", "Shadow plort depositors", "Depositors", false,
     "Filled / total shadow plort depositors (Grey Labyrinth), from the game's map counter"},
    {"cat_shadow_doors", "Shadow plort doors", "Shadow doors", false, "Opened / total plort doors that take shadow plorts"},
    {"cat_plort_doors", "Plort doors", "Doors", false, "Opened / total plort doors"},
    {"cat_teleporters", "Ancient teleporters (found)", "Teleporters", true, "Discovered / total ancient teleporters"},
    {"cat_stabilizers", "Stabilizer gates (found)", "Gates", true,
     "Discovered / total prisma stabilizer gates (Grey Labyrinth)"},
    {"cat_map_nodes", "Map data nodes", "Nodes", false,
     "Activated / total map data nodes: the fog areas of the full map in this zone (one per node), revealed or not. "
     "Includes unvisited areas"},
    {"cat_research_drones", "Research drones (read)", "Drones", false,
     "Read / total research drones. The game keeps no static list, so drones are counted from the loaded scenes: "
     "complete once you have been in the zone (its scenes load as a whole), marked with * until then"},
    {"cat_ghostly_drones", "Ghostly drones (opened)", "Ghosts", false,
     "Opened / total ghostly drones (each carries a treasure pod). Counted from the loaded scenes like research "
     "drones: complete once you have been in the zone, marked with * until then"},
};

struct NativeFault : std::runtime_error {
    using std::runtime_error::runtime_error;
};

template <class T>
T& F(void* obj, size_t off) {
    return unity::Field<T>(obj, off);
}
bool Has(size_t off) { return off != kNoOff; }

// ------------------------------------------------------------- settings -----
struct Cfg {
    bool cats[kCatCount];
    Cfg() { std::fill(std::begin(cats), std::end(cats), true); }
    bool showCompleted = true;
    bool compact = true;
    bool spoilerFree = false;
};

void RegisterSettings() {
    using settings::Kind;
    auto def = [](const char* key, const char* label, const char* group, double d, const char* tip) {
        settings::Def x;
        x.module = kModule;
        x.key = key;
        x.label = label;
        x.group = group;
        x.kind = Kind::Bool;
        x.def = d;
        x.min = 0;
        x.max = 1;
        x.step = 1;
        x.tooltip = tip;
        settings::Register(x);
    };
    def("compact", "Compact row (icons + numbers)", "Display", 1,
        "One row under the minimap: the category's icon (the game's map icon where it has one) and the count, "
        "spread over the minimap's width (wraps to a second row only when needed). Off: one text line per category");
    def("show_completed_categories", "Show completed categories (dimmed)", "Display", 1,
        "Off: categories with everything collected are hidden");
    def("hide_totals_until_discovered", "Spoiler-free totals", "Display", 0,
        "Totals count only what you have discovered: '3/4+' means more exist that you have not found yet; "
        "pods and depositors show '?' until complete");
    for (const CatInfo& c : kCats) def(c.key, c.label, "Categories", 1, c.tip);
}

Cfg LoadCfg() {
    Cfg c;
    for (int i = 0; i < kCatCount; ++i) c.cats[i] = settings::GetBool(kModule, kCats[i].key);
    c.showCompleted = settings::GetBool(kModule, "show_completed_categories");
    c.compact = settings::GetBool(kModule, "compact");
    c.spoilerFree = settings::GetBool(kModule, "hide_totals_until_discovered");
    return c;
}

// --------------------------------------------------------------- symbols ----
struct Sym {
    Il2CppClass *mapDirector = nullptr, *counterDef = nullptr, *podCounter = nullptr, *shadowCounter = nullptr;
    Il2CppClass *zoneDef = nullptr, *zoneMapData = nullptr, *detectArea = nullptr, *zoneMarker = nullptr,
                *labZoneMarker = nullptr;
    Il2CppClass *gordoMarker = nullptr, *puzzleMarker = nullptr, *depositorMarker = nullptr, *stabMarker = nullptr,
                *teleMarker = nullptr;
    Il2CppClass *gameModel = nullptr, *gordoModel = nullptr;
    Il2CppClass *droneCtrl = nullptr, *droneEntry = nullptr, *droneModel = nullptr;
    Il2CppClass* zoneSgMapping = nullptr;  // ZoneSceneGroupMapping (asset)
    Il2CppClass *ghostSpawner = nullptr, *treasurePod = nullptr, *identType = nullptr;
    Il2CppClass *fogElement = nullptr, *staticGameEvent = nullptr, *eventDirector = nullptr, *eventRecordModel = nullptr;
    const MethodInfo* counterGetState = nullptr;  // MapCounterDefinition.GetState (abstract; virtual slot per object)
    const MethodInfo* canBeShown = nullptr;       // IFilterableMapMarker.get_CanBeShown
    const MethodInfo* getDescriptor = nullptr;    // IMapMarkerDescriptorProvider.GetMapMarkerDescriptor
    const MethodInfo* descMapIcon = nullptr;      // IMapMarkerDescriptor.get_MapIcon
    ugui::Fn tryGetGameModel, getGordoModel, gordoHasPopped, puzzleSolved, puzzleShadow, depSolved, depShadow;
    ugui::Fn fogShouldReveal;  // FogMapElement.ShouldBeRevealed(EventDirector)
    ugui::Fn podGetState, podGetZone;  // TreasurePod.get_CurrState (0 LOCKED, 1 OPEN), GetZoneId
    size_t mdCounters = kNoOff, counterIcon = kNoOff;
    size_t areaZoneData = kNoOff, areaPoints = kNoOff, zmdPrimary = kNoOff, zoneMarkerData = kNoOff,
           labZoneMarkerData = kNoOff;
    size_t gordoZone = kNoOff, gordoId = kNoOff, gordoIcon = kNoOff, puzzleIcon = kNoOff, depIcon = kNoOff;
    size_t droneEntryF = kNoOff, droneModelF = kNoOff, entryRefId = kNoOff, droneRead = kNoOff;
    size_t fogEvent = kNoOff, sgeDirector = kNoOff, sgeKey = kNoOff, edModel = kNoOff, gmEventRecord = kNoOff,
           mdEventDirector = kNoOff;
    size_t sgMappings = kNoOff;
    size_t spawnerId = kNoOff, spawnerIdent = kNoOff, identIcon = kNoOff, podModel = kNoOff;
};

struct ResolveCtx {
    std::vector<std::string> missing;
    Il2CppClass* Class(const char* ns, const char* name) {
        std::string e;
        Il2CppClass* k = Resolver::Get().Class(kAsm, ns, name, &e);
        if (!k) missing.push_back(e.empty() ? std::format("class {}.{}", ns, name) : e);
        return k;
    }
    const MethodInfo* Method(Il2CppClass* k, const char* name, int argc) {
        if (!k) return nullptr;
        std::string e;
        const MethodInfo* m = Resolver::Get().Method(k, name, argc, &e);
        if (!m) missing.push_back(e.empty() ? std::format("method {}", name) : e);
        return m;
    }
    ugui::Fn Bind(Il2CppClass* k, const char* name, int argc) {
        const MethodInfo* m = Method(k, name, argc);
        if (!m) return {};
        ugui::Fn f = ugui::BindFn(m);
        if (!f) missing.push_back(std::format("{}: no native body", name));
        return f;
    }
    size_t Field(Il2CppClass* k, const char* name) {
        if (!k) return kNoOff;
        std::string e;
        auto o = Resolver::Get().FieldOffset(k, name, &e);
        if (!o) {
            missing.push_back(e.empty() ? std::format("field {}", name) : e);
            return kNoOff;
        }
        return *o;
    }
};

// ============================================================== module ======
class Collectibles {
public:
    bool Setup(core::ModuleContext& ctx) {
        std::string err;
        if (!ugui::Resolve(err)) return ctx.Fail("uGUI bindings: " + err);
        ResolveCtx c;
        const char* nsMap = "MonomiPark.SlimeRancher.Map";
        const char* nsUiMap = "MonomiPark.SlimeRancher.UI.Map";
        const char* nsData = "MonomiPark.SlimeRancher.DataModel";
        s_.mapDirector = c.Class(nsMap, "MapDirector");
        s_.mdCounters = c.Field(s_.mapDirector, "_counters");
        s_.counterDef = c.Class(nsMap, "MapCounterDefinition");
        s_.counterGetState = c.Method(s_.counterDef, "GetState", 0);
        s_.counterIcon = c.Field(s_.counterDef, "Icon");
        s_.podCounter = c.Class(nsMap, "MapTreasurePodCounterDefinition");
        s_.shadowCounter = c.Class(nsMap, "MapShadowDepositorCounterDefinition");
        s_.zoneDef = c.Class("MonomiPark.SlimeRancher.World", "ZoneDefinition");
        s_.zoneMapData = c.Class("MonomiPark.SlimeRancher.World", "ZoneMapData");
        s_.zmdPrimary = c.Field(s_.zoneMapData, "_primaryZone");
        s_.detectArea = c.Class(nsUiMap, "ZoneDetectionArea");
        s_.areaZoneData = c.Field(s_.detectArea, "_zoneMapData");
        s_.areaPoints = c.Field(s_.detectArea, "_points");
        s_.zoneMarker = c.Class(nsUiMap, "ZoneMarker");
        s_.zoneMarkerData = c.Field(s_.zoneMarker, "_zoneMapData");
        s_.labZoneMarker = c.Class(nsUiMap, "LabyrinthZoneMarker");
        s_.labZoneMarkerData = c.Field(s_.labZoneMarker, "_zoneMapData");
        s_.gordoMarker = c.Class(nsUiMap, "GordoMarkerUI");
        s_.gordoZone = c.Field(s_.gordoMarker, "_zone");
        s_.gordoId = c.Field(s_.gordoMarker, "_gordoIdentifier");
        s_.gordoIcon = c.Field(s_.gordoMarker, "_iconImage");
        s_.puzzleMarker = c.Class(nsUiMap, "PuzzleSlotMarkerUI");
        s_.puzzleSolved = c.Bind(s_.puzzleMarker, "CalculateIsSolved", 0);
        s_.puzzleShadow = c.Bind(s_.puzzleMarker, "CheckIsShadowPlort", 0);
        s_.puzzleIcon = c.Field(s_.puzzleMarker, "_iconElement");
        s_.depositorMarker = c.Class(nsUiMap, "PlortDepositorMarkerUI");
        s_.depSolved = c.Bind(s_.depositorMarker, "CalculateIsSolved", 0);
        s_.depShadow = c.Bind(s_.depositorMarker, "CheckIsShadowPlort", 0);
        s_.depIcon = c.Field(s_.depositorMarker, "_iconElement");
        s_.stabMarker = c.Class(nsUiMap, "StabilizingMarkerUI");
        s_.teleMarker = c.Class(nsUiMap, "AncientTeleporterMarkerUI");
        if (Il2CppClass* k = c.Class(nsUiMap, "IFilterableMapMarker")) s_.canBeShown = c.Method(k, "get_CanBeShown", 0);
        if (Il2CppClass* k = c.Class(nsUiMap, "IMapMarkerDescriptorProvider"))
            s_.getDescriptor = c.Method(k, "GetMapMarkerDescriptor", 0);
        if (Il2CppClass* k = c.Class(nsUiMap, "IMapMarkerDescriptor")) s_.descMapIcon = c.Method(k, "get_MapIcon", 0);
        s_.gameModel = c.Class(nsData, "GameModel");
        s_.getGordoModel = c.Bind(s_.gameModel, "GetGordoModel", 1);
        s_.gordoModel = c.Class(nsData, "GordoModel");
        s_.gordoHasPopped = c.Bind(s_.gordoModel, "HasPopped", 0);
        if (Il2CppClass* scu = c.Class("MonomiPark.SlimeRancher.Util", "SceneContextUtility"))
            s_.tryGetGameModel = c.Bind(scu, "TryGetGameModel", 1);
        s_.droneCtrl = c.Class("MonomiPark.SlimeRancher.World.ResearchDrone", "ResearchDroneController");
        s_.droneEntryF = c.Field(s_.droneCtrl, "_researchDroneEntry");
        s_.droneModelF = c.Field(s_.droneCtrl, "_model");
        s_.droneEntry = c.Class("MonomiPark.SlimeRancher.Dialogue.ResearchDrone", "ResearchDroneEntry");
        s_.entryRefId = c.Field(s_.droneEntry, "referenceId");
        s_.droneModel = c.Class(nsData, "ResearchDroneModel");
        s_.droneRead = c.Field(s_.droneModel, "read");
        // zone -> scene groups (whole-zone loading: completeness of the scene-scanned categories)
        s_.zoneSgMapping = c.Class(nsMap, "ZoneSceneGroupMapping");
        s_.sgMappings = c.Field(s_.zoneSgMapping, "_mappings");
        // ghostly drones: ComponentAcqDroneSpawner on a TreasurePod
        s_.ghostSpawner = c.Class("MonomiPark.SlimeRancher.Drone", "ComponentAcqDroneSpawner");
        s_.spawnerId = c.Field(s_.ghostSpawner, "_id");  // IdHandler._id
        s_.spawnerIdent = c.Field(s_.ghostSpawner, "_ident");
        s_.identType = c.Class("", "IdentifiableType");
        s_.identIcon = c.Field(s_.identType, "icon");
        s_.treasurePod = c.Class("", "TreasurePod");
        s_.podModel = c.Field(s_.treasurePod, "_model");
        s_.podGetState = c.Bind(s_.treasurePod, "get_CurrState", 0);
        s_.podGetZone = c.Bind(s_.treasurePod, "GetZoneId", 0);
        // map data nodes = fog areas (MapUIFogReveal.RevealKnownZones: SetRevealState(ShouldBeRevealed(director)))
        const char* nsEvent = "MonomiPark.SlimeRancher.Event";
        s_.fogElement = c.Class(nsUiMap, "FogMapElement");
        s_.fogShouldReveal = c.Bind(s_.fogElement, "ShouldBeRevealed", 1);
        s_.fogEvent = c.Field(s_.fogElement, "_fogRevealEvent");
        s_.staticGameEvent = c.Class(nsEvent, "StaticGameEvent");
        s_.sgeDirector = c.Field(s_.staticGameEvent, "_eventDirector");
        s_.sgeKey = c.Field(s_.staticGameEvent, "_eventKey");
        s_.eventDirector = c.Class(nsEvent, "EventDirector");
        s_.edModel = c.Field(s_.eventDirector, "_model");
        s_.eventRecordModel = c.Class(nsEvent, "EventRecordModel");
        s_.gmEventRecord = c.Field(s_.gameModel, "eventRecordModel");
        s_.mdEventDirector = c.Field(s_.mapDirector, "_eventDirector");

        // Nothing works without the page / zone basics; everything else degrades per category.
        if (!s_.zoneDef || !ugui::HasTmp())
            return ctx.Fail(!s_.zoneDef ? "ZoneDefinition not found" : "TextMeshPro unavailable");
        for (const auto& m : c.missing) SR2_INFO("collectibles: optional (degraded): {}", m);

        RegisterSettings();
        if (!onChange_) onChange_ = settings::OnChange(kModule, [this](const std::string&) { cfgDirty_.store(true); });
        SR2_INFO("collectibles: resolved (counters {}, gordos {}, plort doors {}, depositors {}, teleporters {}, "
                 "gates {}, map nodes {}, drones {}, ghostly drones {}, zone areas {}, {} optional symbols missing)",
                 (Has(s_.mdCounters) && s_.counterGetState) ? "yes" : "no",
                 (s_.gordoMarker && s_.gordoHasPopped && s_.getGordoModel && s_.tryGetGameModel) ? "yes" : "no",
                 s_.puzzleSolved ? "yes" : "no", s_.depSolved ? "yes" : "no",
                 (s_.teleMarker && s_.canBeShown) ? "yes" : "no", (s_.stabMarker && s_.canBeShown) ? "yes" : "no",
                 MapNodesResolved() ? "yes" : "no",
                 (s_.droneCtrl && Has(s_.droneModelF) && Has(s_.droneRead)) ? "yes" : "no",
                 GhostsResolved() ? "yes" : "no",
                 (Has(s_.areaPoints) && Has(s_.zmdPrimary)) ? "yes" : "no", c.missing.size());
        return true;
    }

    void OnEnable() {
        if (dead_.load()) {
            SR2_WARN("collectibles: stays off for this session after a native fault");
            return;
        }
        cfgDirty_.store(true);
        active_.store(true);
        if (!frameCb_) frameCb_ = main_thread::AddFrameCallback(kModule, [this](float dt) { Tick(dt); });
    }

    void OnDisable() {
        active_.store(false);
        if (frameCb_) main_thread::RemoveFrameCallback(frameCb_);
        frameCb_ = 0;
        if (dead_.load()) return;
        auto td = [this] {
            if (!dead_.load()) Teardown();
        };
        if (main_thread::IsMainThread()) td();
        else main_thread::Post(td);
    }

private:
    // ---------------------------------------------------------------- state --
    struct Entry {
        Cat cat;
        Ref comp;                 // prefab marker component (queries only)
        Il2CppObject* zone;       // ZoneDefinition (pinned in zonePins_) or nullptr
        ugui::Fn canBeShown;      // its IFilterableMapMarker.get_CanBeShown implementation
        int queryFailures = 0;
    };
    struct CounterRef {
        Cat cat;
        Ref counter;
        Il2CppObject* zone;
        ugui::Fn getState;
    };
    struct Area {
        Il2CppObject* zone;
        Ref t;
        std::vector<Vec2> pts;
    };
    struct ZoneMk {
        Il2CppObject* zone;
        Vec3 pos;
    };
    struct Drone {
        Il2CppObject* zone = nullptr;
        bool read = false;
        Ref ctrl;  // ResearchDroneController while its scene is loaded (read state re-read at 1 Hz)
    };
    struct Ghost {
        Il2CppObject* zone = nullptr;
        bool done = false;
        Ref pod;  // TreasurePod while its scene is loaded (state re-read at 1 Hz)
    };
    // One map data node = one reveal event of the page's fog areas.
    struct FogNode {
        Ref event;          // StaticGameEvent (asset)
        Ref comp;           // a FogMapElement of the prefab with that event (queries only)
        Il2CppObject* zone; // pinned in zonePins_ or nullptr
        std::string name;   // event key (logs)
        int failures = 0;
        bool revealed = false;  // reveals are permanent within one save (reset on a record change)
    };
    struct Count {
        int done = 0, total = 0, discovered = 0;
        bool partial = false;  // totals only from what was loaded (drones)
        bool known = true;     // discovery information exists (spoiler mode)
    };
    // No background panel: every graphic gets a dark copy one or two pixels down-right
    // (drawn first, so behind it), which keeps icons and numbers readable on bright terrain.
    struct Cell {
        Ref go, icon, iconGo, iconShadow, iconShadowGo, text, textShadow;
    };
    struct Ui {
        Ref root, rootT, listText, listShadow;
        Ref parent;  // the minimap root we are parented to
        Cell cells[kCatCount];
        bool visible = false;
    };

    struct MapCounterState {
        int current;
        int maximum;
        void* image;
    };

    Sym s_;
    Cfg cfg_;
    std::atomic<bool> cfgDirty_{true}, active_{false}, dead_{false};
    int frameCb_ = 0, onChange_ = 0;
    const char* step_ = "idle";
    bool frozen_ = false;

    // page index
    Ref pagePrefab_, mapT_, mapDirector_;
    std::vector<Ref> zonePins_;
    std::unordered_set<Il2CppObject*> zoneSet_;
    std::vector<Entry> entries_;
    std::vector<CounterRef> counters_;
    std::vector<Area> areas_;
    std::vector<ZoneMk> zoneMarkers_;
    std::vector<FogNode> fogNodes_;
    Il2CppObject* fogRecord_ = nullptr;  // identity only: the event record the reveal cache belongs to
    bool fogReady_ = false;              // last Tally: the save's event record was queryable
    bool pageHasZoneInfo_ = false;
    Ref catIcon_[kCatCount];
    float scaleX_ = 0, scaleY_ = 0, offX_ = 0, offY_ = 0;

    // live state
    Ref zone_;
    Il2CppObject* lastGameModel_ = nullptr;
    std::unordered_map<std::string, Drone> drones_;
    std::unordered_map<std::string, Ghost> ghosts_;
    Ref ghostIcon_;  // IdentifiableType.icon of the spawners' drone (kept across page rebuilds)
    // zone -> number of scene groups (ZoneSceneGroupMapping; -1 = also a SceneGroupList asset)
    std::unordered_map<Il2CppObject*, int> zoneGroups_;
    // zones scanned while current and settled (identity only): their scene-scanned totals are complete
    std::unordered_set<Il2CppObject*> dronesComplete_, ghostsComplete_;
    float zoneAge_ = 0;
    bool settleScanDue_ = false;
    int scanStage_ = 0;  // 1: research drone scan this frame, 2: ghostly drone scan this frame, 0: idle
    float recomputeAcc_ = 1e9f, droneAcc_ = 0, retryAcc_ = 0;
    Count counts_[kCatCount];
    std::string shownSig_;
    Ui ui_;
    float lastSize_ = -1;
    int lastPos_ = -1;

    void Step(const char* s) { step_ = s; }

    template <class Fn>
    bool Managed(Fn&& fn) {
        std::string fault;
        bool native = false;
        bool ok = core::Try([&] {
            if (!ugui::GuardNative(fn, &fault)) native = true;
        });
        if (native) throw NativeFault(fault);
        return ok;
    }

    // ----------------------------------------------------------------- tick --
    void Tick(float dt) {
        if (dead_.load()) return;
        std::string err, fault;
        bool native = false;
        bool ok = core::Try([&] {
            if (!ugui::GuardNative([&] { TickImpl(dt); }, &fault)) native = true;
        }, &err);
        if (ok && !native) return;
        Die(std::format("step '{}': {}", step_, native ? fault : err));
    }

    void Die(const std::string& why) {
        frozen_ = true;
        dead_.store(true);
        active_.store(false);
        SR2_ERROR("collectibles: {} - collectible tracker disabled for the rest of this session", why);
        core::MarkModuleFaulted(kModule, why);
        // Drop every reference WITHOUT calling into IL2CPP (handles leaked on purpose).
        auto abandonUi = [](Ui& u) {
            for (Ref* r : {&u.root, &u.rootT, &u.listText, &u.listShadow, &u.parent}) r->Abandon();
            for (Cell& c : u.cells)
                for (Ref* r : {&c.go, &c.icon, &c.iconGo, &c.iconShadow, &c.iconShadowGo, &c.text, &c.textShadow})
                    r->Abandon();
        };
        abandonUi(ui_);
        for (Entry& e : entries_) e.comp.Abandon();
        for (CounterRef& c : counters_) c.counter.Abandon();
        for (Area& a : areas_) a.t.Abandon();
        for (FogNode& n : fogNodes_) {
            n.event.Abandon();
            n.comp.Abandon();
        }
        for (Ref& r : zonePins_) r.Abandon();
        for (Ref& r : catIcon_) r.Abandon();
        for (auto& [id, g] : ghosts_) g.pod.Abandon();
        for (auto& [id, d] : drones_) d.ctrl.Abandon();
        ghostIcon_.Abandon();
        for (Ref* r : {&pagePrefab_, &mapT_, &mapDirector_, &zone_}) r->Abandon();
        entries_.clear();
        counters_.clear();
        areas_.clear();
        fogNodes_.clear();
        ghosts_.clear();
        drones_.clear();
        zonePins_.clear();
        if (frameCb_) main_thread::RemoveFrameCallback(frameCb_);
        frameCb_ = 0;
    }

    void TickImpl(float dt) {
        if (ugui::RuntimeFrozen()) {  // the minimap froze the shared bindings after a fault: stop calling
            if (!frozen_) Die("shared uGUI bindings frozen by the minimap");
            return;
        }
        Step("settings");
        if (cfgDirty_.exchange(false)) {
            cfg_ = LoadCfg();
            recomputeAcc_ = 1e9f;
            settleScanDue_ = true;  // a category may just have been switched on
            shownSig_.clear();
        }
        Step("minimap state");
        mapi::Snapshot snap;
        if (!mapi::Get(snap) || snap.loading || !snap.root || !snap.zone) {
            SetVisible(false);
            if (snap.loading || !snap.zone) ResetZone();
            return;
        }
        Step("ui");
        if (!EnsureUi(snap)) return;
        if (!snap.visible) {  // our root is a child of the minimap root: hidden with it anyway
            return;
        }

        Step("page index");
        if (pagePrefab_ && !pagePrefab_.Alive()) {  // page asset unloaded: drop the index (rebuilt from the next page)
            SR2_DEBUG("collectibles: map page prefab unloaded - index dropped");
            ClearIndex();
        }
        if (snap.pagePrefab && snap.pagePrefab != pagePrefab_.get()) BuildIndex(snap);
        if (!snap.pagePrefab && !pagePrefab_) {
            SetVisible(false);
            return;
        }

        Step("save check");
        Il2CppObject* gm = GameModel();
        if (gm != lastGameModel_) {  // save change: session drone knowledge belongs to the old save
            lastGameModel_ = gm;
            drones_.clear();
            ghosts_.clear();
            dronesComplete_.clear();
            ghostsComplete_.clear();
            settleScanDue_ = true;
            recomputeAcc_ = 1e9f;
        }

        Step("zone");
        if (snap.zone != zone_.get()) {
            zone_.Reset(snap.zone);
            recomputeAcc_ = 1e9f;
            zoneAge_ = 0;
            settleScanDue_ = true;  // one scan once the zone's scenes are in (counts + completeness)
            SR2_DEBUG("collectibles: zone -> '{}'", unity::GetName(snap.zone));
        }

        // Scene scans (each one FindObjectsByType over every scene object): once per zone after it
        // settled, drones and ghosts on separate frames; rescans only for zones not loaded as a whole.
        droneAcc_ += dt;
        zoneAge_ += dt;
        if (scanStage_ == 0 && (cfg_.cats[kDrones] || cfg_.cats[kGhostDrones])) {
            if (settleScanDue_ && zoneAge_ >= kZoneSettle) scanStage_ = 1;
            else if (!settleScanDue_ && droneAcc_ >= kPartialRescan && !CurrentZoneFullyLoaded()) scanStage_ = 1;
            if (scanStage_) {
                settleScanDue_ = false;
                droneAcc_ = 0;
            }
        }
        if (scanStage_ == 1) {
            scanStage_ = 2;
            if (cfg_.cats[kDrones]) {
                Step("research drones");
                ScanDrones(gm);
                recomputeAcc_ = 1e9f;
            }
        } else if (scanStage_ == 2) {
            scanStage_ = 0;
            if (cfg_.cats[kGhostDrones]) {
                Step("ghostly drones");
                ScanGhosts();
                recomputeAcc_ = 1e9f;
            }
        }
        recomputeAcc_ += dt;
        if (recomputeAcc_ >= kRecomputeInterval) {
            recomputeAcc_ = 0;
            Step("recompute");
            Recompute(gm);
            Step("render");
            Render(snap);
        } else if (snap.size != lastSize_ || snap.position != lastPos_) {
            Step("layout");
            shownSig_.clear();
            Render(snap);
        }
    }

    void ResetZone() {
        if (zone_) zone_.Reset(nullptr);
        recomputeAcc_ = 1e9f;
    }

    Il2CppObject* GameModel() {
        if (!s_.tryGetGameModel) return nullptr;
        Il2CppObject* gm = nullptr;
        Managed([&] {
            if (!ugui::Call<bool>(s_.tryGetGameModel, &gm)) gm = nullptr;
        });
        return ugui::IsA(gm, s_.gameModel) ? gm : nullptr;
    }

    // --------------------------------------------------------- page index ----
    Il2CppObject* PinZone(Il2CppObject* z) {
        if (!ugui::IsA(z, s_.zoneDef)) return nullptr;
        if (zoneSet_.insert(z).second) zonePins_.emplace_back(z);
        return z;
    }
    Il2CppObject* ZoneOfMapData(Il2CppObject* zmd) {
        if (!ugui::IsA(zmd, s_.zoneMapData) || !Has(s_.zmdPrimary)) return nullptr;
        return PinZone(F<Il2CppObject*>(zmd, s_.zmdPrimary));
    }

    static bool PointInPoly(const std::vector<Vec2>& p, float x, float y) {
        bool in = false;
        for (size_t i = 0, j = p.size() - 1; i < p.size(); j = i++) {
            if (((p[i].y > y) != (p[j].y > y)) &&
                (x < (p[j].x - p[i].x) * (y - p[i].y) / (p[j].y - p[i].y + 1e-12f) + p[i].x))
                in = !in;
        }
        return in;
    }

    // Zone of a point in the page prefab's world space. method: 1 polygon, 2 nearest zone label, 0 none.
    Il2CppObject* ZoneAtPrefabPoint(Vec3 w, int& method) {
        method = 0;
        for (Area& a : areas_) {
            if (a.pts.size() < 3 || !a.t.Alive()) continue;
            Vec3 l = ugui::InverseTransformPoint(a.t.get(), w);
            if (PointInPoly(a.pts, l.x, l.y)) {
                method = 1;
                return a.zone;
            }
        }
        float best = 1e30f;
        Il2CppObject* z = nullptr;
        for (const ZoneMk& m : zoneMarkers_) {
            float dx = m.pos.x - w.x, dy = m.pos.y - w.y, d = dx * dx + dy * dy;
            if (d < best) {
                best = d;
                z = m.zone;
            }
        }
        if (z) method = 2;
        return z;
    }

    Il2CppObject* ZoneAtWorld(Vec3 world, int& method) {
        method = 0;
        if (!mapT_.Alive()) return nullptr;
        Vec3 page{world.x * scaleX_ + offX_, world.z * scaleY_ + offY_, 0};
        return ZoneAtPrefabPoint(ugui::TransformPoint(mapT_.get(), page), method);
    }

    Il2CppObject* MarkerIcon(Il2CppObject* comp, Cat cat) {
        Il2CppObject* spr = nullptr;
        if (s_.getDescriptor && s_.descMapIcon) {
            Managed([&] {
                ugui::Fn gd = ugui::BindFn(ugui::VirtualMethod(comp, s_.getDescriptor));
                Il2CppObject* desc = gd ? ugui::Call<Il2CppObject*>(gd, comp) : nullptr;
                if (!desc || !ugui::PlausiblePointer(desc)) return;
                ugui::Fn gi = ugui::BindFn(ugui::VirtualMethod(desc, s_.descMapIcon));
                Il2CppObject* s = gi ? ugui::Call<Il2CppObject*>(gi, desc) : nullptr;
                if (ugui::IsA(s, ugui::SpriteClass())) spr = s;
            });
        }
        if (spr) return spr;
        size_t off = cat == kGordos ? s_.gordoIcon
                     : (cat == kShadowDepositors && ugui::IsA(comp, s_.depositorMarker)) ? s_.depIcon
                     : ugui::IsA(comp, s_.puzzleMarker) ? s_.puzzleIcon
                                                        : kNoOff;
        if (Has(off)) {
            Il2CppObject* img = F<Il2CppObject*>(comp, off);
            if (ugui::IsA(img, ugui::ImageClass())) {
                Il2CppObject* s = ugui::ImageGetSprite(img);
                if (ugui::IsA(s, ugui::SpriteClass())) return s;
            }
        }
        return nullptr;
    }

    void ClearIndex() {
        entries_.clear();
        counters_.clear();
        areas_.clear();
        zoneMarkers_.clear();
        fogNodes_.clear();
        fogRecord_ = nullptr;
        zoneGroups_.clear();
        zonePins_.clear();
        zoneSet_.clear();
        for (Ref& r : catIcon_) r.Reset(nullptr);
        pagePrefab_.Reset(nullptr);
        mapT_.Reset(nullptr);
        mapDirector_.Reset(nullptr);
        pageHasZoneInfo_ = false;
    }

    void BuildIndex(const mapi::Snapshot& snap) {
        ClearIndex();
        Il2CppObject* prefab = snap.pagePrefab;
        if (!unity::IsAlive(prefab)) return;
        pagePrefab_.Reset(prefab);
        if (snap.mapComponent) mapT_.Reset(unity::TransformOf(snap.mapComponent));
        scaleX_ = snap.scaleX;
        scaleY_ = snap.scaleY;
        offX_ = snap.offsetX;
        offY_ = snap.offsetY;

        // zone polygons (ZoneDetectionArea: _points in the area's local space; ContainsPoint
        // does InverseTransformPoint(_transform) + point-in-polygon, 1.3.0 RVA 0x739800)
        if (s_.detectArea && Has(s_.areaZoneData) && Has(s_.areaPoints)) {
            ugui::ForEachInChildren(prefab, s_.detectArea, true, [&](Il2CppObject* a) {
                if (!ugui::IsA(a, s_.detectArea)) return true;
                Il2CppObject* z = ZoneOfMapData(F<Il2CppObject*>(a, s_.areaZoneData));
                Il2CppObject* list = F<Il2CppObject*>(a, s_.areaPoints);
                if (!z || !list || !ugui::PlausiblePointer(list)) return true;
                ugui::ListLayout l = ugui::ListStructLayout(list, "x");
                if (!l.ok || l.stride < 8) return true;
                auto* items = F<Il2CppArray*>(list, l.items);
                int n = F<int>(list, l.size);
                size_t cap = ugui::ArrayCount(items);
                if (!items || n < 3 || static_cast<size_t>(n) > cap || n > 4096) return true;
                Area ar{z, Ref(unity::TransformOf(a)), {}};
                const char* data = reinterpret_cast<const char*>(items) + unity::kArrayDataOffset;
                for (int i = 0; i < n; ++i) {
                    const float* v = reinterpret_cast<const float*>(data + static_cast<size_t>(i) * l.stride + l.field);
                    ar.pts.push_back({v[0], v[1]});
                }
                areas_.push_back(std::move(ar));
                return true;
            });
        }
        // zone label markers (fallback attribution: nearest)
        auto addZoneMarkers = [&](Il2CppClass* k, size_t off) {
            if (!k || !Has(off)) return;
            ugui::ForEachInChildren(prefab, k, true, [&](Il2CppObject* m) {
                if (!ugui::IsA(m, k)) return true;
                if (Il2CppObject* z = ZoneOfMapData(F<Il2CppObject*>(m, off)))
                    zoneMarkers_.push_back({z, ugui::GetPosition(unity::TransformOf(m))});
                return true;
            });
        };
        addZoneMarkers(s_.zoneMarker, s_.zoneMarkerData);
        addZoneMarkers(s_.labZoneMarker, s_.labZoneMarkerData);
        pageHasZoneInfo_ = !areas_.empty() || !zoneMarkers_.empty();

        // baked markers
        int byOwn = 0, byPoly = 0, byLabel = 0, unassigned = 0;
        auto addMarkers = [&](Il2CppClass* k, auto classify) {
            if (!k) return;
            ugui::ForEachInChildren(prefab, k, true, [&](Il2CppObject* comp) {
                if (!ugui::IsA(comp, k)) return true;
                int cat = classify(comp);
                if (cat < 0) return true;
                Il2CppObject* z = nullptr;
                if (k == s_.gordoMarker && Has(s_.gordoZone)) z = PinZone(F<Il2CppObject*>(comp, s_.gordoZone));
                int method = 0;
                if (z) ++byOwn;
                else {
                    z = ZoneAtPrefabPoint(ugui::GetPosition(unity::TransformOf(comp)), method);
                    if (method == 1) ++byPoly;
                    else if (method == 2) ++byLabel;
                    else ++unassigned;
                }
                Entry e{static_cast<Cat>(cat), Ref(comp), z, {}, 0};
                if (s_.canBeShown) e.canBeShown = ugui::BindFn(ugui::VirtualMethod(comp, s_.canBeShown));
                if (!catIcon_[cat])
                    if (Il2CppObject* spr = MarkerIcon(comp, static_cast<Cat>(cat))) catIcon_[cat].Reset(spr);
                entries_.push_back(std::move(e));
                return true;
            });
        };
        addMarkers(s_.gordoMarker, [](Il2CppObject*) { return static_cast<int>(kGordos); });
        addMarkers(s_.puzzleMarker, [&](Il2CppObject* comp) {
            bool shadow = false;
            if (s_.puzzleShadow) Managed([&] { shadow = ugui::Call<bool>(s_.puzzleShadow, comp); });
            return static_cast<int>(shadow ? kShadowDoors : kPlortDoors);
        });
        addMarkers(s_.depositorMarker, [&](Il2CppObject* comp) {
            bool shadow = true;
            if (s_.depShadow) Managed([&] { shadow = ugui::Call<bool>(s_.depShadow, comp); });
            return static_cast<int>(shadow ? kShadowDepositors : kPlortDoors);
        });
        addMarkers(s_.teleMarker, [](Il2CppObject*) { return static_cast<int>(kTeleporters); });
        addMarkers(s_.stabMarker, [](Il2CppObject*) { return static_cast<int>(kStabilizers); });

        int fogAreas = 0, fogNoEvent = 0;
        IndexFogNodes(prefab, fogAreas, fogNoEvent);

        // the game's map counters (all zones)
        Il2CppObject* dir = snap.mapDirector;
        if (ugui::IsA(dir, s_.mapDirector)) mapDirector_.Reset(dir);
        if (ugui::IsA(dir, s_.mapDirector) && Has(s_.mdCounters) && s_.counterGetState) {
            Il2CppObject* list = F<Il2CppObject*>(dir, s_.mdCounters);
            if (list && ugui::PlausiblePointer(list))
                ugui::ForEachListStructPair(list, "Zone", "Counter", [&](Il2CppObject* zone, Il2CppObject* counter) {
                    Il2CppObject* z = PinZone(zone);
                    if (!z || !ugui::IsA(counter, s_.counterDef)) return true;
                    Cat cat = ugui::IsA(counter, s_.podCounter)      ? kPods
                              : ugui::IsA(counter, s_.shadowCounter) ? kShadowDepositors
                                                                     : kCatCount;
                    if (cat == kCatCount) {
                        SR2_INFO("collectibles: map counter of unknown type {} for zone '{}' ignored",
                                 unity::ClassName(counter), unity::GetName(z));
                        return true;
                    }
                    CounterRef c{cat, Ref(counter), z, ugui::BindFn(ugui::VirtualMethod(counter, s_.counterGetState))};
                    if (!c.getState) return true;
                    if (Has(s_.counterIcon)) {
                        Il2CppObject* icon = F<Il2CppObject*>(counter, s_.counterIcon);
                        if (ugui::IsA(icon, ugui::SpriteClass())) catIcon_[cat].Reset(icon);  // counter icon wins
                    }
                    counters_.push_back(std::move(c));
                    return true;
                });
        }

        ReadZoneGroups();
        LogTotals(byOwn, byPoly, byLabel, unassigned, fogAreas, fogNoEvent);
        recomputeAcc_ = 1e9f;
        shownSig_.clear();
    }

    bool MapNodesResolved() const {
        return s_.fogElement && s_.fogShouldReveal && Has(s_.fogEvent) && s_.eventDirector && Has(s_.edModel) &&
               Has(s_.gmEventRecord) && s_.tryGetGameModel;
    }

    // Map data nodes: every FogMapElement of the page is one fog area that a node's reveal event
    // (MapNodeActivator.Activate -> _fogRevealEvent.RaiseEvent) uncovers; areas sharing an event
    // count once. Zone: majority of the polygon hits of the area's rect centre and its art's centres.
    void IndexFogNodes(Il2CppObject* prefab, int& areas, int& noEvent) {
        if (!MapNodesResolved()) return;
        std::unordered_map<Il2CppObject*, size_t> byEvent;
        ugui::ForEachInChildren(prefab, s_.fogElement, true, [&](Il2CppObject* f) {
            if (!ugui::IsA(f, s_.fogElement)) return true;
            ++areas;
            Il2CppObject* ev = F<Il2CppObject*>(f, s_.fogEvent);
            if (s_.staticGameEvent ? !ugui::IsA(ev, s_.staticGameEvent) : !ugui::PlausiblePointer(ev)) {
                ++noEvent;
                return true;
            }
            std::unordered_map<Il2CppObject*, int> votes;
            Il2CppObject* labelZone = nullptr;
            auto vote = [&](Il2CppObject* rt) {
                if (!rt || !ugui::IsRectTransform(rt)) return;
                ugui::Rect r = ugui::GetRect(rt);
                if (!std::isfinite(r.x) || !std::isfinite(r.y) || !std::isfinite(r.w) || !std::isfinite(r.h)) return;
                Vec3 w = ugui::TransformPoint(rt, {r.x + r.w * 0.5f, r.y + r.h * 0.5f, 0});
                int m = 0;
                Il2CppObject* z = ZoneAtPrefabPoint(w, m);
                if (m == 1) ++votes[z];
                else if (m == 2 && !labelZone) labelZone = z;
            };
            vote(unity::TransformOf(f));
            if (Il2CppObject* go = unity::GameObjectOf(f))
                ugui::ForEachInChildren(go, ugui::ImageClass(), true, [&](Il2CppObject* img) {
                    vote(unity::TransformOf(img));
                    return true;
                });
            Il2CppObject* z = labelZone;
            int best = 0;
            for (const auto& [vz, n] : votes)
                if (n > best) {
                    best = n;
                    z = vz;
                }
            auto it = byEvent.find(ev);
            if (it != byEvent.end()) {  // second area of the same node: only fills a missing zone
                if (!fogNodes_[it->second].zone) fogNodes_[it->second].zone = z;
                return true;
            }
            std::string name;
            if (Has(s_.sgeKey)) {
                auto* key = F<Il2CppString*>(ev, s_.sgeKey);
                if (key && ugui::PlausiblePointer(key)) name = unity::ToUtf8(key);
            }
            if (name.empty()) name = unity::GetName(ev);
            byEvent.emplace(ev, fogNodes_.size());
            FogNode n{Ref(ev), Ref(f), z, std::move(name)};
            fogNodes_.push_back(std::move(n));
            return true;
        });
    }

    // The director whose record IS the loaded save's (else the reveal answers belong to no / another
    // save): the one the reveal events raise into, then MapDirector._eventDirector.
    Il2CppObject* FogDirector(Il2CppObject* gm, Il2CppObject*& record) {
        record = nullptr;
        if (!MapNodesResolved() || !gm) return nullptr;
        Il2CppObject* saveRec = F<Il2CppObject*>(gm, s_.gmEventRecord);
        if (s_.eventRecordModel ? !ugui::IsA(saveRec, s_.eventRecordModel) : !ugui::PlausiblePointer(saveRec))
            return nullptr;
        Il2CppObject* cands[2] = {};
        if (Has(s_.sgeDirector))
            for (FogNode& n : fogNodes_)
                if (n.event.Alive()) {
                    cands[0] = F<Il2CppObject*>(n.event.get(), s_.sgeDirector);
                    break;
                }
        if (mapDirector_.Alive() && Has(s_.mdEventDirector))
            cands[1] = F<Il2CppObject*>(mapDirector_.get(), s_.mdEventDirector);
        for (Il2CppObject* d : cands) {
            if (!ugui::IsA(d, s_.eventDirector)) continue;
            if (F<Il2CppObject*>(d, s_.edModel) == saveRec) {
                record = saveRec;
                return d;
            }
        }
        return nullptr;
    }

    // One-shot (per page build) summary of every zone's totals.
    void LogTotals(int byOwn, int byPoly, int byLabel, int unassigned, int fogAreas, int fogNoEvent) {
        Il2CppObject* gm = GameModel();
        std::vector<Il2CppObject*> zones;
        for (const Ref& r : zonePins_) zones.push_back(r.get());
        std::string out;
        for (Il2CppObject* z : zones) {
            Count c[kCatCount];
            Tally(z, gm, c, true);
            std::string line;
            for (int i = 0; i < kCatCount; ++i) {
                if (c[i].total <= 0) continue;
                const bool unknownDone = i == kMapNodes && !fogReady_;
                line += unknownDone ? std::format("{}{} ?/{}", line.empty() ? "" : ", ", kCats[i].shortLabel, c[i].total)
                                    : std::format("{}{} {}/{}{}", line.empty() ? "" : ", ", kCats[i].shortLabel,
                                                  c[i].done, c[i].total, c[i].partial ? "*" : "");
            }
            auto g = zoneGroups_.find(z);
            if (g != zoneGroups_.end())
                line += std::format("{}scene groups {}", line.empty() ? "" : ", ",
                                    g->second < 0 ? std::string("list") : std::to_string(g->second));
            if (!line.empty()) out += std::format(" | '{}': {}", unity::GetName(z), line);
        }
        std::string icons;
        for (int i = 0; i < kCatCount; ++i)
            if (IconFor(i)) icons += std::format("{}{}", icons.empty() ? "" : ",", kCats[i].shortLabel);
        int nodesNoZone = 0;
        for (const FogNode& n : fogNodes_) nodesNoZone += n.zone ? 0 : 1;
        SR2_INFO("collectibles: page '{}': {} markers (zone by own field {}, polygon {}, nearest zone label {}, "
                 "unassigned {}), {} zone areas, {} zone labels, {} map counters, {} map nodes ({} fog areas, {} without "
                 "a reveal event, {} without a zone; reveal state {}), icons [{}]; per zone (done/total):{}",
                 unity::GetName(pagePrefab_.get()), entries_.size(), byOwn, byPoly, byLabel, unassigned, areas_.size(),
                 zoneMarkers_.size(), counters_.size(), fogNodes_.size(), fogAreas, fogNoEvent, nodesNoZone,
                 fogReady_ ? "read" : "not readable yet (save's event record not set)", icons,
                 out.empty() ? " (none)" : out);
    }

    // ------------------------------------------------------------- counting --
    // Done state of a page marker. Returns false if it cannot be read.
    bool QueryDone(Entry& e, Il2CppObject* gm, bool& done) {
        Il2CppObject* comp = e.comp.get();
        if (!e.comp.Alive()) return false;
        switch (e.cat) {
            case kGordos: {
                if (!gm || !s_.getGordoModel || !s_.gordoHasPopped || !Has(s_.gordoId)) return false;
                Il2CppObject* id = F<Il2CppObject*>(comp, s_.gordoId);
                if (!id || !ugui::PlausiblePointer(id)) return false;
                bool ok = false;
                done = false;
                Managed([&] {
                    Il2CppObject* model = ugui::Call<Il2CppObject*>(s_.getGordoModel, gm, id);
                    if (!model) {  // never fed / not registered yet: not popped
                        ok = true;
                        return;
                    }
                    if (!ugui::IsA(model, s_.gordoModel)) return;
                    done = ugui::Call<bool>(s_.gordoHasPopped, model);
                    ok = true;
                });
                return ok;
            }
            case kShadowDoors:
            case kPlortDoors:
            case kShadowDepositors: {
                const ugui::Fn& f = ugui::IsA(comp, s_.depositorMarker) ? s_.depSolved : s_.puzzleSolved;
                if (!f) return false;
                return Managed([&] { done = ugui::Call<bool>(f, comp); });
            }
            case kTeleporters:
            case kStabilizers:
                if (!e.canBeShown) return false;
                return Managed([&] { done = ugui::Call<bool>(e.canBeShown, comp); });
            default:
                return false;
        }
    }

    bool QueryCanBeShown(Entry& e, bool& can) {
        if (!e.canBeShown || !e.comp.Alive()) return false;
        return Managed([&] { can = ugui::Call<bool>(e.canBeShown, e.comp.get()); });
    }

    // Counts for one zone. forLog: every category, no discovery queries.
    void Tally(Il2CppObject* zone, Il2CppObject* gm, Count* c, bool forLog) {
        bool hasCounter[kCatCount] = {};
        for (const CounterRef& cr : counters_)
            if (cr.zone == zone) hasCounter[cr.cat] = true;
        for (CounterRef& cr : counters_) {
            if (cr.zone != zone || (!forLog && !cfg_.cats[cr.cat]) || !cr.counter.Alive()) continue;
            MapCounterState st{0, 0, nullptr};
            // MapCounterState GetState(): 16-byte struct returned through a hidden pointer (rcx), this in rdx.
            if (!Managed([&] { ugui::Call<void*>(cr.getState, static_cast<void*>(&st), cr.counter.get()); })) continue;
            if (st.maximum < 0 || st.maximum > 100000 || st.current < 0) continue;
            c[cr.cat].done += std::min(st.current, st.maximum);
            c[cr.cat].total += st.maximum;
            c[cr.cat].known = false;  // no discovery info for counter categories
        }
        bool attributeUnassigned = !pageHasZoneInfo_ && zone == zone_.get();
        for (Entry& e : entries_) {
            Il2CppObject* ez = e.zone ? e.zone : (attributeUnassigned ? zone : nullptr);
            if (ez != zone || hasCounter[e.cat] || (!forLog && !cfg_.cats[e.cat])) continue;
            if (e.queryFailures >= 3) continue;  // broken marker: left out (logged once)
            bool done = false;
            if (!QueryDone(e, gm, done)) {
                if (++e.queryFailures == 3)
                    SR2_WARN("collectibles: {} marker '{}' cannot be queried - left out of the counts",
                             kCats[e.cat].label, unity::GetName(e.comp.get()));
                continue;
            }
            Count& k = c[e.cat];
            ++k.total;
            if (done) ++k.done;
            if (!forLog && cfg_.spoilerFree) {
                bool disc = done;
                if (!disc && !kCats[e.cat].discoveryOnly) {
                    bool can = false;
                    disc = QueryCanBeShown(e, can) && can;  // gordo seen / door discovered (and not done)
                } else if (!disc && kCats[e.cat].discoveryOnly) {
                    disc = false;
                }
                if (disc) ++k.discovered;
            }
        }
        if (!hasCounter[kMapNodes] && (forLog || cfg_.cats[kMapNodes]) && !fogNodes_.empty()) {
            Il2CppObject* rec = nullptr;
            Il2CppObject* dir = FogDirector(gm, rec);
            fogReady_ = dir != nullptr;
            if (dir && rec != fogRecord_) {  // another save / session: cached reveals are void
                fogRecord_ = rec;
                for (FogNode& n : fogNodes_) n.revealed = false;
            }
            for (FogNode& n : fogNodes_) {
                Il2CppObject* nz = n.zone ? n.zone : (attributeUnassigned ? zone : nullptr);
                if (nz != zone || n.failures >= 3) continue;
                Count& k = c[kMapNodes];
                k.known = false;  // no discovery information: spoiler-free shows "done/?"
                if (!dir) {       // save data not there yet: totals only in the log, nothing shown
                    if (forLog) ++k.total;
                    continue;
                }
                bool r = n.revealed;
                if (!r && (!n.comp.Alive() ||
                           !Managed([&] { r = ugui::Call<bool>(s_.fogShouldReveal, n.comp.get(), dir); }))) {
                    if (++n.failures == 3)
                        SR2_WARN("collectibles: map node '{}' cannot be queried - left out of the counts", n.name);
                    continue;
                }
                n.revealed = r;
                ++k.total;
                if (r) ++k.done;
            }
        }
        if (!hasCounter[kDrones] && (forLog || cfg_.cats[kDrones])) {
            for (auto& [id, d] : drones_) {
                if (d.zone != zone) continue;
                if (!forLog && !d.read && d.ctrl.Alive()) d.read = DroneRead(d.ctrl.get());
                Count& k = c[kDrones];
                ++k.total;
                ++k.discovered;
                if (d.read) ++k.done;
                k.partial = !dronesComplete_.count(zone);
            }
        }
        if (!hasCounter[kGhostDrones] && (forLog || cfg_.cats[kGhostDrones])) {
            for (auto& [id, g] : ghosts_) {
                if (g.zone != zone) continue;
                if (!forLog && !g.done && g.pod.Alive()) {
                    bool done = false;
                    if (PodOpen(g.pod.get(), done)) g.done = done;
                }
                Count& k = c[kGhostDrones];
                ++k.total;
                ++k.discovered;
                if (g.done) ++k.done;
                k.partial = !ghostsComplete_.count(zone);
            }
        }
    }

    bool GhostsResolved() const {
        return s_.ghostSpawner && s_.treasurePod && s_.podGetState && Has(s_.podModel);
    }

    // TreasurePod.get_CurrState() == OPEN; get_CurrState throws while the pod has no model yet.
    bool PodOpen(Il2CppObject* pod, bool& open) {
        if (!ugui::IsA(pod, s_.treasurePod) || !s_.podGetState || !Has(s_.podModel)) return false;
        Il2CppObject* model = F<Il2CppObject*>(pod, s_.podModel);
        if (!model || !ugui::PlausiblePointer(model)) return false;
        int state = 0;
        if (!Managed([&] { state = ugui::Call<int>(s_.podGetState, pod); })) return false;
        open = state == 1;
        return true;
    }

    void ScanGhosts() {
        if (!GhostsResolved()) return;
        ugui::ForEachSceneObject(s_.ghostSpawner, true, [&](Il2CppObject* sp) {
            if (!ugui::IsA(sp, s_.ghostSpawner)) return true;
            Il2CppObject* go = unity::GameObjectOf(sp);
            Il2CppObject* pod = nullptr;
            if (go)
                ugui::ForEachInChildren(go, s_.treasurePod, true, [&](Il2CppObject* p) {
                    if (ugui::IsA(p, s_.treasurePod)) pod = p;
                    return pod == nullptr;
                });
            if (!pod) return true;  // nothing to count without its pod
            std::string id;
            if (Has(s_.spawnerId)) {
                auto* str = F<Il2CppString*>(sp, s_.spawnerId);
                if (str && ugui::PlausiblePointer(str)) id = unity::ToUtf8(str);
            }
            if (id.empty()) id = std::format("obj:{}", static_cast<const void*>(sp));
            if (!ghostIcon_.Alive() && Has(s_.spawnerIdent) && Has(s_.identIcon)) {
                Il2CppObject* ident = F<Il2CppObject*>(sp, s_.spawnerIdent);
                if (ugui::IsA(ident, s_.identType)) {
                    Il2CppObject* icon = F<Il2CppObject*>(ident, s_.identIcon);
                    if (ugui::IsA(icon, ugui::SpriteClass())) ghostIcon_.Reset(icon);
                }
            }
            bool done = false;
            bool known = PodOpen(pod, done);
            auto it = ghosts_.find(id);
            if (it != ghosts_.end()) {
                if (!it->second.pod.Alive() || it->second.pod.get() != pod) it->second.pod.Reset(pod);
                if (known) it->second.done = it->second.done || done;
                return true;
            }
            Il2CppObject* z = nullptr;
            const char* how = "pod zone";
            if (s_.podGetZone) {
                Il2CppObject* pz = nullptr;
                Managed([&] { pz = ugui::Call<Il2CppObject*>(s_.podGetZone, pod); });
                z = PinZone(pz);
            }
            if (!z) {
                int method = 0;
                z = ZoneAtWorld(ugui::GetPosition(unity::TransformOf(sp)), method);
                how = method == 1 ? "polygon" : "nearest zone label";
            }
            if (!z && !pageHasZoneInfo_) {
                z = zone_.get() ? PinZone(zone_.get()) : nullptr;
                how = "current zone";
            }
            if (!z) return true;
            Ghost g;
            g.zone = z;
            g.done = known && done;
            g.pod.Reset(pod);
            ghosts_.emplace(id, std::move(g));
            SR2_DEBUG("collectibles: ghostly drone '{}' in '{}' ({}), opened {}", id, unity::GetName(z), how, known && done);
            return true;
        });
        if (CurrentZoneFullyLoaded() && ghostsComplete_.insert(zone_.get()).second)
            SR2_DEBUG("collectibles: ghostly drones of '{}' complete (zone scenes loaded)", unity::GetName(zone_.get()));
    }

    // A scan of the loaded scenes saw every object of the current zone: the zone maps to exactly one
    // scene group (loaded as a whole) and the player has been in it for kZoneSettle seconds.
    bool CurrentZoneFullyLoaded() const {
        if (!zone_ || zoneAge_ < kZoneSettle) return false;
        auto it = zoneGroups_.find(zone_.get());
        return it != zoneGroups_.end() && it->second == 1;
    }

    void ReadZoneGroups() {
        zoneGroups_.clear();
        if (!s_.zoneSgMapping || !Has(s_.sgMappings)) return;
        ugui::ForEachOfType(s_.zoneSgMapping, [&](Il2CppObject* m) {
            if (!ugui::IsA(m, s_.zoneSgMapping)) return true;
            Il2CppObject* list = F<Il2CppObject*>(m, s_.sgMappings);
            if (!list || !ugui::PlausiblePointer(list)) return true;
            ugui::ForEachListStructPair(list, "Zone", "SceneGroups", [&](Il2CppObject* zone, Il2CppObject* groups) {
                Il2CppObject* z = PinZone(zone);
                if (!z) return true;
                int n = groups ? static_cast<int>(ugui::ArrayCount(reinterpret_cast<Il2CppArray*>(groups))) : 0;
                int& slot = zoneGroups_[z];
                slot = slot < 0 ? -1 : slot + n;
                return true;
            });
            ugui::ForEachListStructPair(list, "Zone", "SceneGroupListAsset", [&](Il2CppObject* zone, Il2CppObject* lst) {
                if (lst && zoneSet_.count(zone)) zoneGroups_[zone] = -1;  // groups from a list asset: not counted
                return true;
            });
            return true;
        });
    }

    // ResearchDroneController._model.read (false while the model is not there yet).
    bool DroneRead(Il2CppObject* ctrl) {
        if (!ugui::IsA(ctrl, s_.droneCtrl)) return false;
        Il2CppObject* model = F<Il2CppObject*>(ctrl, s_.droneModelF);
        return ugui::IsA(model, s_.droneModel) && F<bool>(model, s_.droneRead);
    }

    void ScanDrones(Il2CppObject* gm) {
        if (!s_.droneCtrl || !Has(s_.droneModelF) || !Has(s_.droneRead) || !gm) return;
        int seen = 0;
        ugui::ForEachSceneObject(s_.droneCtrl, true, [&](Il2CppObject* c) {
            if (!ugui::IsA(c, s_.droneCtrl)) return true;
            ++seen;
            std::string id;
            if (Has(s_.droneEntryF) && Has(s_.entryRefId)) {
                Il2CppObject* entry = F<Il2CppObject*>(c, s_.droneEntryF);
                if (ugui::IsA(entry, s_.droneEntry)) {
                    auto* str = F<Il2CppString*>(entry, s_.entryRefId);
                    if (str && ugui::PlausiblePointer(str)) id = unity::ToUtf8(str);
                }
            }
            if (id.empty()) id = std::format("obj:{}", static_cast<const void*>(c));
            const bool read = DroneRead(c);
            auto it = drones_.find(id);
            if (it == drones_.end()) {
                int method = 0;
                Il2CppObject* z = ZoneAtWorld(ugui::GetPosition(unity::TransformOf(c)), method);
                if (!z && !pageHasZoneInfo_) z = zone_.get() ? PinZone(zone_.get()) : nullptr;
                if (!z) return true;
                Drone d;
                d.zone = z;
                d.read = read;
                d.ctrl.Reset(c);
                drones_.emplace(id, std::move(d));
                SR2_DEBUG("collectibles: research drone '{}' in '{}' ({}), read {}", id, unity::GetName(z),
                          method == 1 ? "polygon" : method == 2 ? "nearest zone label" : "current zone", read);
            } else {
                it->second.read = it->second.read || read;
                if (!it->second.ctrl.Alive() || it->second.ctrl.get() != c) it->second.ctrl.Reset(c);
            }
            return true;
        });
        (void)seen;
        if (CurrentZoneFullyLoaded() && dronesComplete_.insert(zone_.get()).second)
            SR2_DEBUG("collectibles: research drones of '{}' complete (zone scenes loaded)", unity::GetName(zone_.get()));
    }

    void Recompute(Il2CppObject* gm) {
        for (Count& c : counts_) c = Count{};
        if (!zone_) return;
        Tally(zone_.get(), gm, counts_, false);
    }

    // ------------------------------------------------------------------- UI --
    Il2CppObject* NewChild(Il2CppObject* parentT, const char* name, int layer) {
        Il2CppObject* go = ugui::NewGameObject(name);
        if (!go) return nullptr;
        ugui::SetLayer(go, layer);
        ugui::SetParent(unity::TransformOf(go), parentT, false);
        return go;
    }
    Il2CppObject* NewText(Il2CppObject* go, Il2CppObject* fontSrc) {
        Il2CppObject* t = nullptr;
        if (!Managed([&] { t = ugui::AddTmpText(go); }) || !t) return nullptr;
        ugui::GraphicSetRaycastTarget(t, false);
        if (fontSrc) ugui::TmpCopyFont(t, fontSrc);
        ugui::TmpSetCenter(t);
        ugui::TmpSetColor(t, {0.98f, 0.94f, 0.82f, 1});
        return t;
    }

    bool EnsureUi(const mapi::Snapshot& snap) {
        if (ui_.root.Alive() && ui_.parent.get() == snap.root && ui_.parent.Alive()) return true;
        if (ui_.root.Alive()) ugui::Destroy(ui_.root.get());
        ui_ = Ui{};
        shownSig_.clear();
        lastSize_ = -1;
        Il2CppObject* parentT = unity::TransformOf(snap.root);
        if (!parentT) return false;
        int layer = ugui::GetLayer(snap.root);
        // font: the minimap's own TMP label (copied from the HUD)
        Il2CppObject* fontSrc = nullptr;
        ugui::ForEachInChildren(snap.root, ugui::TmpTextClass(), true, [&](Il2CppObject* t) {
            fontSrc = t;
            return false;
        });
        Il2CppObject* root = NewChild(parentT, "SR2Mods.Collectibles", layer);
        if (!root) return false;
        ui_.parent.Reset(snap.root);
        ui_.root.Reset(root);
        ui_.rootT.Reset(unity::TransformOf(root));
        // Shadows are created first: uGUI draws siblings in order, so they end up behind.
        if (Il2CppObject* sgo = NewChild(ui_.rootT.get(), "ListShadow", layer)) ui_.listShadow.Reset(NewText(sgo, fontSrc));
        if (Il2CppObject* lgo = NewChild(ui_.rootT.get(), "List", layer)) ui_.listText.Reset(NewText(lgo, fontSrc));
        auto newIcon = [&](Il2CppObject* parentGo, const char* name, Ref& img, Ref& go) {
            Il2CppObject* ig = NewChild(unity::TransformOf(parentGo), name, layer);
            if (!ig) return;
            Il2CppObject* im = ugui::AddComponent(ig, ugui::ImageClass());
            ugui::GraphicSetRaycastTarget(im, false);
            ugui::ImageSetPreserveAspect(im, true);
            img.Reset(im);
            go.Reset(ig);
        };
        for (int i = 0; i < kCatCount; ++i) {
            Cell& c = ui_.cells[i];
            Il2CppObject* go = NewChild(ui_.rootT.get(), kCats[i].key, layer);
            if (!go) continue;
            c.go.Reset(go);
            newIcon(go, "IconShadow", c.iconShadow, c.iconShadowGo);
            newIcon(go, "Icon", c.icon, c.iconGo);
            if (Il2CppObject* sg = NewChild(unity::TransformOf(go), "CountShadow", layer))
                c.textShadow.Reset(NewText(sg, fontSrc));
            if (Il2CppObject* tg = NewChild(unity::TransformOf(go), "Count", layer)) c.text.Reset(NewText(tg, fontSrc));
            unity::SetActive(go, false);
        }
        unity::SetActive(root, false);
        ui_.visible = false;
        SR2_INFO("collectibles: tracker UI created under the minimap (font {})", fontSrc ? "copied" : "TMP default");
        return true;
    }

    void SetVisible(bool v) {
        if (!ui_.root.Alive() || ui_.visible == v) return;
        ui_.visible = v;
        unity::SetActive(ui_.root.get(), v);
    }

    // The game's own sprite (map marker / counter / identifiable icon) when it has one, else the
    // mod's icon for categories without any game sprite (map data nodes, research drones, ghostly
    // drones: ComponentAcqDrone has no icon), else nullptr (text label).
    Il2CppObject* IconFor(int cat) {
        if (catIcon_[cat].Alive()) return catIcon_[cat].get();
        if (cat == kGhostDrones && ghostIcon_.Alive()) return ghostIcon_.get();
        const char* own = cat == kMapNodes ? "map_node" : cat == kDrones ? "research_drone"
                        : cat == kGhostDrones ? "ghost_drone" : nullptr;
        return own ? sr2::ui::mod_icons::Get(own) : nullptr;
    }

    std::string Numbers(int cat) const {
        const Count& c = counts_[cat];
        std::string star = c.partial ? "*" : "";
        if (!cfg_.spoilerFree || c.done >= c.total) return std::format("{}/{}{}", c.done, c.total, star);
        if (!c.known) return std::format("{}/?", c.done);
        if (c.discovered < c.total) return std::format("{}/{}+{}", c.done, c.discovered, star);
        return std::format("{}/{}{}", c.done, c.total, star);
    }

    void Render(const mapi::Snapshot& snap) {
        // what to show
        std::vector<int> shown;
        for (int i = 0; i < kCatCount; ++i) {
            const Count& c = counts_[i];
            if (!cfg_.cats[i] || c.total <= 0) continue;
            if (!cfg_.showCompleted && c.done >= c.total) continue;
            shown.push_back(i);
        }
        std::string sig = std::format("{}|{}|{}|{:.1f}|", cfg_.compact, cfg_.spoilerFree, snap.position, snap.size);
        for (int i : shown) sig += std::format("{}:{}/{}/{}{};", i, counts_[i].done, counts_[i].total,
                                               counts_[i].discovered, counts_[i].partial ? "*" : "");
        lastSize_ = snap.size;
        lastPos_ = snap.position;
        if (sig == shownSig_) return;
        shownSig_ = sig;
        if (shown.empty() || !ui_.root.Alive()) {
            SetVisible(false);
            return;
        }

        const float S = std::max(60.0f, snap.size);
        const bool top = snap.position < 2;    // top corners: below the minimap; bottom corners: above it
        const bool right = (snap.position % 2) == 1;  // right corners: list text hugs the right edge
        const float gap = std::round(S * 0.03f) + 2;
        const Color kTextCol{0.98f, 0.94f, 0.82f, 1.0f}, kDoneCol{0.70f, 1.0f, 0.70f, 0.6f};
        auto shadowCol = [](float a) { return Color{0, 0, 0, 0.6f * a}; };
        float height = 0;

        if (cfg_.compact) {
            for (Ref* t : {&ui_.listText, &ui_.listShadow})
                if (t->Alive()) unity::SetActive(unity::GameObjectOf(t->get()), false);
            // One row of "icon X/Y" per category, as wide as the minimap; wraps only when it does not fit.
            const float h = std::max(14.0f, std::round(S * 0.095f));  // row height = icon size
            const float font = std::max(9.0f, std::round(h * 0.7f));
            const float iconGap = std::max(2.0f, std::round(h * 0.15f));
            const float spacing = std::max(4.0f, std::round(h * 0.55f));  // between two categories
            const float rowGap = std::max(1.0f, std::round(h * 0.2f));
            const float d = std::max(1.0f, std::round(h * 0.07f));  // shadow offset
            struct Item {
                int cat;
                Il2CppObject* spr;
                float textW, w;
            };
            std::vector<Item> items;
            for (int i : shown) {
                Cell& c = ui_.cells[i];
                if (!c.go.Alive()) continue;
                Item it{i, IconFor(i), 0, 0};
                const std::string txt = it.spr ? Numbers(i) : std::format("{} {}", kCats[i].shortLabel, Numbers(i));
                for (Ref* t : {&c.text, &c.textShadow}) {
                    if (!t->Alive()) continue;
                    ugui::TmpSetFontSize(t->get(), font);
                    ugui::TmpSetText(t->get(), txt);
                }
                float tw = c.text.Alive() ? ugui::TmpPreferredWidth(c.text.get()) : -1.0f;
                if (!std::isfinite(tw) || tw <= 0 || tw > S * 4) tw = static_cast<float>(txt.size()) * font * 0.56f;
                it.textW = std::ceil(tw) + 2;
                it.w = (it.spr ? h + iconGap : 0) + it.textW;
                items.push_back(it);
            }
            if (items.empty()) {
                SetVisible(false);
                return;
            }
            std::vector<std::pair<size_t, size_t>> rows;  // [begin, end) into items
            for (size_t b = 0; b < items.size();) {
                size_t e = b + 1;
                float w = items[b].w;
                while (e < items.size() && w + spacing + items[e].w <= S) w += spacing + items[e++].w;
                rows.push_back({b, e});
                b = e;
            }
            std::vector<bool> used(kCatCount, false);
            for (size_t r = 0; r < rows.size(); ++r) {
                const size_t b = rows[r].first, e = rows[r].second, n = e - b;
                float total = 0;
                for (size_t k = b; k < e; ++k) total += items[k].w;
                // Spread the row over the minimap's width; a short row is centred instead of pulled apart.
                float sp = n > 1 ? (S - total) / static_cast<float>(n - 1) : spacing, x = 0;
                if (n == 1 || sp > spacing * 2.5f) {
                    sp = spacing;
                    x = std::round((S - total - sp * static_cast<float>(n - 1)) * 0.5f);
                }
                const float y = -static_cast<float>(r) * (h + rowGap);
                for (size_t k = b; k < e; ++k) {
                    const Item& it = items[k];
                    Cell& c = ui_.cells[it.cat];
                    used[it.cat] = true;
                    const Count& cnt = counts_[it.cat];
                    const bool complete = cnt.done >= cnt.total;
                    const float alpha = complete ? 0.5f : 1.0f;  // completed categories are dimmed
                    Il2CppObject* t = unity::TransformOf(c.go.get());
                    ugui::SetAnchors(t, {0, 1}, {0, 1});
                    ugui::SetPivot(t, {0, 1});
                    ugui::SetSizeDelta(t, {it.w, h});
                    ugui::SetAnchoredPosition(t, {std::round(x), y});
                    auto place = [&](Ref& go, Ref& img, Vec2 pos, Color col) {
                        if (!go.Alive()) return;
                        unity::SetActive(go.get(), it.spr != nullptr);
                        if (!it.spr) return;
                        Il2CppObject* gt = unity::TransformOf(go.get());
                        ugui::SetAnchors(gt, {0, 0.5f}, {0, 0.5f});
                        ugui::SetPivot(gt, {0, 0.5f});
                        ugui::SetSizeDelta(gt, {h, h});
                        ugui::SetAnchoredPosition(gt, pos);
                        ugui::ImageSetSprite(img.get(), it.spr);
                        ugui::GraphicSetColor(img.get(), col);
                    };
                    place(c.iconShadowGo, c.iconShadow, {d, -d}, shadowCol(alpha));
                    place(c.iconGo, c.icon, {0, 0}, {1, 1, 1, alpha});
                    const float left = it.spr ? h + iconGap : 0;
                    auto text = [&](Ref& tr, Vec2 pos, Color col) {
                        if (!tr.Alive()) return;
                        Il2CppObject* tt = unity::TransformOf(tr.get());
                        ugui::SetAnchors(tt, {0, 0.5f}, {0, 0.5f});
                        ugui::SetPivot(tt, {0, 0.5f});
                        ugui::SetSizeDelta(tt, {it.textW, h});
                        ugui::SetAnchoredPosition(tt, pos);
                        ugui::TmpSetColor(tr.get(), col);
                    };
                    text(c.textShadow, {left + d, -d}, shadowCol(complete ? 0.6f : 1.0f));
                    text(c.text, {left, 0}, complete ? kDoneCol : kTextCol);
                    unity::SetActive(c.go.get(), true);
                    x += it.w + sp;
                }
            }
            height = static_cast<float>(rows.size()) * h + static_cast<float>(rows.size() - 1) * rowGap;
            for (int i = 0; i < kCatCount; ++i)
                if (!used[i] && ui_.cells[i].go.Alive()) unity::SetActive(ui_.cells[i].go.get(), false);
        } else {
            for (Cell& c : ui_.cells)
                if (c.go.Alive()) unity::SetActive(c.go.get(), false);
            const float font = std::max(10.0f, std::round(S * 0.062f));
            const float d = std::max(1.0f, std::round(font * 0.08f));
            std::string txt, shadow;
            for (size_t n = 0; n < shown.size(); ++n) {
                int i = shown[n];
                bool complete = counts_[i].done >= counts_[i].total;
                if (n) txt += "\n", shadow += "\n";
                const std::string line = std::format("{}  {}", kCats[i].label, Numbers(i));
                // <alpha> replaces the vertex alpha, so the shadow copy carries its own values.
                txt += std::format("{}{}", complete ? "<alpha=#80>" : "<alpha=#FF>", line);
                shadow += std::format("{}{}", complete ? "<alpha=#4D>" : "<alpha=#99>", line);
            }
            height = static_cast<float>(shown.size()) * font * 1.25f;
            auto text = [&](Ref& tr, Vec2 pos, const std::string& s, Color col) {
                if (!tr.Alive()) return;
                Il2CppObject* tt = unity::TransformOf(tr.get());
                ugui::SetAnchors(tt, {0, 0}, {1, 1});
                ugui::SetPivot(tt, {0.5f, 0.5f});
                ugui::SetSizeDelta(tt, {0, 0});
                ugui::SetAnchoredPosition(tt, pos);
                ugui::TmpSetFontSize(tr.get(), font);
                ugui::TmpSetAlignment(tr.get(), right ? 516 /* Right */ : 513 /* Left */);
                ugui::TmpSetColor(tr.get(), col);
                ugui::TmpSetText(tr.get(), s);
                unity::SetActive(unity::GameObjectOf(tr.get()), true);
            };
            text(ui_.listShadow, {d, -d}, shadow, {0, 0, 0, 1});
            text(ui_.listText, {0, 0}, txt, kTextCol);
        }

        Il2CppObject* rt = ui_.rootT.get();
        if (top) {
            ugui::SetAnchors(rt, {0, 0}, {1, 0});
            ugui::SetPivot(rt, {0.5f, 1});
            ugui::SetAnchoredPosition(rt, {0, -gap});
        } else {
            ugui::SetAnchors(rt, {0, 1}, {1, 1});
            ugui::SetPivot(rt, {0.5f, 0});
            ugui::SetAnchoredPosition(rt, {0, gap});
        }
        ugui::SetSizeDelta(rt, {0, height});
        SetVisible(true);
    }

    // ------------------------------------------------------------ teardown ---
    template <class Fn>
    void TeardownStep(const char* what, Fn&& fn) {
        if (frozen_) return;
        std::string err, fault;
        bool native = false;
        bool ok = core::Try([&] {
            if (!ugui::GuardNative(fn, &fault)) native = true;
        }, &err);
        if (native) Die(std::format("teardown step '{}': {}", what, fault));
        else if (!ok) SR2_WARN("collectibles: teardown step '{}' failed: {}", what, err);
    }

    void Teardown() {
        if (ugui::RuntimeFrozen()) {
            if (!frozen_) Die("shared uGUI bindings frozen");
            return;
        }
        TeardownStep("destroy ui", [&] {
            if (ui_.root.Alive()) ugui::Destroy(ui_.root.get());
        });
        TeardownStep("release ui", [&] { ui_ = Ui{}; });
        TeardownStep("release index", [&] { ClearIndex(); });
        TeardownStep("release zone", [&] { zone_.Reset(nullptr); });
        TeardownStep("release drones", [&] { drones_.clear(); });
        TeardownStep("release ghosts", [&] {
            ghosts_.clear();
            ghostIcon_.Reset(nullptr);
        });
        dronesComplete_.clear();
        ghostsComplete_.clear();
        lastGameModel_ = nullptr;
        shownSig_.clear();
        lastSize_ = -1;
    }
};

// The impl owns GC handles and is never deleted (process exit may happen after the IL2CPP runtime is gone).
class CollectiblesModule final : public core::Module {
public:
    const char* Name() const override { return kModule; }
    bool DefaultEnabled() const override { return true; }
    bool Setup(core::ModuleContext& ctx) override { return impl_->Setup(ctx); }
    void OnEnable() override { impl_->OnEnable(); }
    void OnDisable() override { impl_->OnDisable(); }

private:
    Collectibles* impl_ = new Collectibles();  // leaked on purpose
};

}  // namespace

std::unique_ptr<sr2::core::Module> CreateCollectibles() { return std::make_unique<CollectiblesModule>(); }

}  // namespace sr2::modules
