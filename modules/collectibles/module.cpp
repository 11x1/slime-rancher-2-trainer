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
//   * Research drones: no static per-zone list exists; counted from the drones
//     loaded so far this session (ResearchDroneController, low-rate scan),
//     marked with '*' (known so far).
//
// UI: our own uGUI objects parented to the minimap's root GameObject, so they
// follow its position, opacity and visibility (hidden with the HUD, menus,
// the full map, loads). No per-frame work besides an "is the parent still
// ours" check; counts are recomputed once per second while shown.
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
constexpr float kDroneScanInterval = 10.0f;
constexpr int kCols = 3;  // compact mode cells per row

enum Cat { kPods = 0, kGordos, kShadowDepositors, kShadowDoors, kPlortDoors, kTeleporters, kStabilizers, kDrones, kCatCount };
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
    {"cat_research_drones", "Research drones (read)", "Drones", false,
     "Read / known research drones. The game keeps no static list per zone, so only drones loaded at least once "
     "this session are known (marked with *)"},
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
    bool cats[kCatCount] = {true, true, true, true, true, true, true, true};
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
    def("compact", "Compact (icons + numbers)", "Display", 1,
        "Icon and count per category (the game's map icons); off: one text line per category");
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
    const MethodInfo* counterGetState = nullptr;  // MapCounterDefinition.GetState (abstract; virtual slot per object)
    const MethodInfo* canBeShown = nullptr;       // IFilterableMapMarker.get_CanBeShown
    const MethodInfo* getDescriptor = nullptr;    // IMapMarkerDescriptorProvider.GetMapMarkerDescriptor
    const MethodInfo* descMapIcon = nullptr;      // IMapMarkerDescriptor.get_MapIcon
    ugui::Fn tryGetGameModel, getGordoModel, gordoHasPopped, puzzleSolved, puzzleShadow, depSolved, depShadow;
    size_t mdCounters = kNoOff, counterIcon = kNoOff;
    size_t areaZoneData = kNoOff, areaPoints = kNoOff, zmdPrimary = kNoOff, zoneMarkerData = kNoOff,
           labZoneMarkerData = kNoOff;
    size_t gordoZone = kNoOff, gordoId = kNoOff, gordoIcon = kNoOff, puzzleIcon = kNoOff, depIcon = kNoOff;
    size_t droneEntryF = kNoOff, droneModelF = kNoOff, entryRefId = kNoOff, droneRead = kNoOff;
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

        // Nothing works without the page / zone basics; everything else degrades per category.
        if (!s_.zoneDef || !ugui::HasTmp())
            return ctx.Fail(!s_.zoneDef ? "ZoneDefinition not found" : "TextMeshPro unavailable");
        for (const auto& m : c.missing) SR2_INFO("collectibles: optional (degraded): {}", m);

        RegisterSettings();
        if (!onChange_) onChange_ = settings::OnChange(kModule, [this](const std::string&) { cfgDirty_.store(true); });
        SR2_INFO("collectibles: resolved (counters {}, gordos {}, plort doors {}, depositors {}, teleporters {}, "
                 "gates {}, drones {}, zone areas {}, {} optional symbols missing)",
                 (Has(s_.mdCounters) && s_.counterGetState) ? "yes" : "no",
                 (s_.gordoMarker && s_.gordoHasPopped && s_.getGordoModel && s_.tryGetGameModel) ? "yes" : "no",
                 s_.puzzleSolved ? "yes" : "no", s_.depSolved ? "yes" : "no",
                 (s_.teleMarker && s_.canBeShown) ? "yes" : "no", (s_.stabMarker && s_.canBeShown) ? "yes" : "no",
                 (s_.droneCtrl && Has(s_.droneModelF) && Has(s_.droneRead)) ? "yes" : "no",
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
    };
    struct Count {
        int done = 0, total = 0, discovered = 0;
        bool partial = false;  // totals only from what was loaded (drones)
        bool known = true;     // discovery information exists (spoiler mode)
    };
    struct Cell {
        Ref go, icon, iconGo, text;
    };
    struct Ui {
        Ref root, rootT, bg, listText;
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
    bool pageHasZoneInfo_ = false;
    Ref catIcon_[kCatCount];
    float scaleX_ = 0, scaleY_ = 0, offX_ = 0, offY_ = 0;

    // live state
    Ref zone_;
    Il2CppObject* lastGameModel_ = nullptr;
    std::unordered_map<std::string, Drone> drones_;
    float recomputeAcc_ = 1e9f, droneAcc_ = 1e9f, retryAcc_ = 0;
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
            for (Ref* r : {&u.root, &u.rootT, &u.bg, &u.listText, &u.parent}) r->Abandon();
            for (Cell& c : u.cells)
                for (Ref* r : {&c.go, &c.icon, &c.iconGo, &c.text}) r->Abandon();
        };
        abandonUi(ui_);
        for (Entry& e : entries_) e.comp.Abandon();
        for (CounterRef& c : counters_) c.counter.Abandon();
        for (Area& a : areas_) a.t.Abandon();
        for (Ref& r : zonePins_) r.Abandon();
        for (Ref& r : catIcon_) r.Abandon();
        for (Ref* r : {&pagePrefab_, &mapT_, &mapDirector_, &zone_}) r->Abandon();
        entries_.clear();
        counters_.clear();
        areas_.clear();
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
            droneAcc_ = 1e9f;
            recomputeAcc_ = 1e9f;
        }

        Step("zone");
        if (snap.zone != zone_.get()) {
            zone_.Reset(snap.zone);
            recomputeAcc_ = 1e9f;
            droneAcc_ = 1e9f;
            SR2_DEBUG("collectibles: zone -> '{}'", unity::GetName(snap.zone));
        }

        droneAcc_ += dt;
        if (droneAcc_ >= kDroneScanInterval && cfg_.cats[kDrones]) {
            droneAcc_ = 0;
            Step("research drones");
            ScanDrones(gm);
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

        // the game's map counters (all zones)
        Il2CppObject* dir = snap.mapDirector;
        if (ugui::IsA(dir, s_.mapDirector) && Has(s_.mdCounters) && s_.counterGetState) {
            mapDirector_.Reset(dir);
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

        LogTotals(byOwn, byPoly, byLabel, unassigned);
        recomputeAcc_ = 1e9f;
        shownSig_.clear();
    }

    // One-shot (per page build) summary of every zone's totals.
    void LogTotals(int byOwn, int byPoly, int byLabel, int unassigned) {
        Il2CppObject* gm = GameModel();
        std::vector<Il2CppObject*> zones;
        for (const Ref& r : zonePins_) zones.push_back(r.get());
        std::string out;
        for (Il2CppObject* z : zones) {
            Count c[kCatCount];
            Tally(z, gm, c, true);
            std::string line;
            for (int i = 0; i < kCatCount; ++i)
                if (c[i].total > 0)
                    line += std::format("{}{} {}/{}", line.empty() ? "" : ", ", kCats[i].shortLabel, c[i].done, c[i].total);
            if (!line.empty()) out += std::format(" | '{}': {}", unity::GetName(z), line);
        }
        std::string icons;
        for (int i = 0; i < kCatCount; ++i)
            if (catIcon_[i]) icons += std::format("{}{}", icons.empty() ? "" : ",", kCats[i].shortLabel);
        SR2_INFO("collectibles: page '{}': {} markers (zone by own field {}, polygon {}, nearest zone label {}, "
                 "unassigned {}), {} zone areas, {} zone labels, {} map counters, icons [{}]; per zone (done/total):{}",
                 unity::GetName(pagePrefab_.get()), entries_.size(), byOwn, byPoly, byLabel, unassigned, areas_.size(),
                 zoneMarkers_.size(), counters_.size(), icons, out.empty() ? " (none)" : out);
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
        if (!hasCounter[kDrones] && (forLog || cfg_.cats[kDrones])) {
            for (const auto& [id, d] : drones_) {
                if (d.zone != zone) continue;
                Count& k = c[kDrones];
                ++k.total;
                ++k.discovered;
                if (d.read) ++k.done;
                k.partial = true;
            }
        }
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
            Il2CppObject* model = F<Il2CppObject*>(c, s_.droneModelF);
            bool read = ugui::IsA(model, s_.droneModel) && F<bool>(model, s_.droneRead);
            auto it = drones_.find(id);
            if (it == drones_.end()) {
                int method = 0;
                Il2CppObject* z = ZoneAtWorld(ugui::GetPosition(unity::TransformOf(c)), method);
                if (!z && !pageHasZoneInfo_) z = zone_.get() ? PinZone(zone_.get()) : nullptr;
                if (!z) return true;
                drones_.emplace(id, Drone{z, read});
                SR2_DEBUG("collectibles: research drone '{}' in '{}' ({}), read {}", id, unity::GetName(z),
                          method == 1 ? "polygon" : method == 2 ? "nearest zone label" : "current zone", read);
            } else {
                it->second.read = it->second.read || read;
            }
            return true;
        });
        (void)seen;
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
        Il2CppObject* bg = ugui::AddComponent(root, ugui::ImageClass());
        ugui::GraphicSetRaycastTarget(bg, false);
        ugui::GraphicSetColor(bg, {0.05f, 0.06f, 0.08f, 0.45f});
        ui_.bg.Reset(bg);
        if (Il2CppObject* lgo = NewChild(ui_.rootT.get(), "List", layer)) ui_.listText.Reset(NewText(lgo, fontSrc));
        for (int i = 0; i < kCatCount; ++i) {
            Cell& c = ui_.cells[i];
            Il2CppObject* go = NewChild(ui_.rootT.get(), kCats[i].key, layer);
            if (!go) continue;
            c.go.Reset(go);
            if (Il2CppObject* ig = NewChild(unity::TransformOf(go), "Icon", layer)) {
                Il2CppObject* img = ugui::AddComponent(ig, ugui::ImageClass());
                ugui::GraphicSetRaycastTarget(img, false);
                ugui::ImageSetPreserveAspect(img, true);
                c.icon.Reset(img);
                c.iconGo.Reset(ig);
            }
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
        const bool top = snap.position < 2;  // top corners: below the minimap; bottom corners: above it
        const float gap = std::round(S * 0.03f) + 2;
        const float pad = std::round(S * 0.02f) + 2;
        float height = 0;

        if (cfg_.compact) {
            const float h = std::max(16.0f, std::round(S * 0.115f));
            const int cols = std::min<int>(kCols, static_cast<int>(shown.size()));
            const int rows = (static_cast<int>(shown.size()) + cols - 1) / cols;
            const float cellW = (S - 2 * pad) / cols;
            height = rows * h + 2 * pad;
            if (ui_.listText.Alive()) unity::SetActive(unity::GameObjectOf(ui_.listText.get()), false);
            std::vector<bool> used(kCatCount, false);
            for (size_t n = 0; n < shown.size(); ++n) {
                int i = shown[n];
                used[i] = true;
                Cell& c = ui_.cells[i];
                if (!c.go.Alive()) continue;
                int row = static_cast<int>(n) / cols, col = static_cast<int>(n) % cols;
                int inRow = std::min<int>(cols, static_cast<int>(shown.size()) - row * cols);
                float x0 = pad + (S - 2 * pad - inRow * cellW) * 0.5f + col * cellW;
                Il2CppObject* t = unity::TransformOf(c.go.get());
                ugui::SetAnchors(t, {0, 1}, {0, 1});
                ugui::SetPivot(t, {0, 1});
                ugui::SetSizeDelta(t, {cellW, h});
                ugui::SetAnchoredPosition(t, {x0, -(pad + row * h)});
                const Count& k = counts_[i];
                bool complete = k.done >= k.total;
                float alpha = complete ? 0.5f : 1.0f;
                Il2CppObject* spr = catIcon_[i].Alive() ? catIcon_[i].get() : nullptr;
                float iconW = 0;
                if (c.iconGo.Alive()) {
                    unity::SetActive(c.iconGo.get(), spr != nullptr);
                    if (spr) {
                        iconW = h * 0.9f;
                        Il2CppObject* it = unity::TransformOf(c.iconGo.get());
                        ugui::SetAnchors(it, {0, 0.5f}, {0, 0.5f});
                        ugui::SetPivot(it, {0, 0.5f});
                        ugui::SetSizeDelta(it, {iconW, iconW});
                        ugui::SetAnchoredPosition(it, {h * 0.05f, 0});
                        ugui::ImageSetSprite(c.icon.get(), spr);
                        ugui::GraphicSetColor(c.icon.get(), {1, 1, 1, alpha});
                    }
                }
                if (c.text.Alive()) {
                    Il2CppObject* tt = unity::TransformOf(c.text.get());
                    float left = spr ? iconW + h * 0.1f : 0;
                    ugui::SetAnchors(tt, {0, 0}, {1, 1});
                    ugui::SetPivot(tt, {0.5f, 0.5f});
                    ugui::SetSizeDelta(tt, {-left, 0});
                    ugui::SetAnchoredPosition(tt, {left * 0.5f, 0});
                    std::string txt = spr ? Numbers(i) : std::format("{} {}", kCats[i].shortLabel, Numbers(i));
                    ugui::TmpSetFontSize(c.text.get(), std::max(9.0f, h * (spr ? 0.62f : 0.48f)));
                    ugui::TmpSetText(c.text.get(), txt);
                    ugui::TmpSetColor(c.text.get(), complete ? Color{0.70f, 1.0f, 0.70f, 0.6f}
                                                             : Color{0.98f, 0.94f, 0.82f, 1.0f});
                }
                unity::SetActive(c.go.get(), true);
            }
            for (int i = 0; i < kCatCount; ++i)
                if (!used[i] && ui_.cells[i].go.Alive()) unity::SetActive(ui_.cells[i].go.get(), false);
        } else {
            for (Cell& c : ui_.cells)
                if (c.go.Alive()) unity::SetActive(c.go.get(), false);
            const float font = std::max(10.0f, std::round(S * 0.062f));
            std::string txt;
            for (size_t n = 0; n < shown.size(); ++n) {
                int i = shown[n];
                bool complete = counts_[i].done >= counts_[i].total;
                if (n) txt += "\n";
                txt += std::format("{}{}  {}{}", complete ? "<alpha=#80>" : "", kCats[i].label, Numbers(i),
                                   complete ? "<alpha=#FF>" : "");
            }
            height = shown.size() * font * 1.25f + 2 * pad;
            if (ui_.listText.Alive()) {
                Il2CppObject* tt = unity::TransformOf(ui_.listText.get());
                ugui::SetAnchors(tt, {0, 0}, {1, 1});
                ugui::SetPivot(tt, {0.5f, 0.5f});
                ugui::SetSizeDelta(tt, {-2 * pad, -2 * pad});
                ugui::SetAnchoredPosition(tt, {0, 0});
                ugui::TmpSetFontSize(ui_.listText.get(), font);
                ugui::TmpSetText(ui_.listText.get(), txt);
                unity::SetActive(unity::GameObjectOf(ui_.listText.get()), true);
            }
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
        drones_.clear();
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
