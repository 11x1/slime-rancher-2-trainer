// minimap: a corner minimap built from the game's own map art.
//
// How it works (details and verification status in NOTES.md):
//   * The full map (MapUI) shows one "map page" prefab per MapDefinition; its
//     MonomiPark.SlimeRancher.UI.Map.Map component defines the linear
//     world(XZ) -> page-local transform via _anchor1/_anchor2
//     (Map.SetupInitialValues / MapUtils.GetSlopeAndOffset):
//         page = (world.x * scale.x + offset.x, world.z * scale.y + offset.y)
//     clamped to the page's RectTransform rect (Map.GetMapPosition(Vector3)).
//   * The page prefab is READ, never instantiated: its Images/RawImages are
//     copied (sprite, colour, type, rect corners in page space) into our own
//     uGUI objects under a masked container in the HUD canvas. The copy is
//     time-sliced over several frames. Fog areas (FogMapElement) and dynamic
//     elements (DynamicMapElement) become groups we show/hide with the game's
//     own queries (ShouldBeRevealed / CanBeShown, pure queries on serialized
//     data). Baked markers (IFilterableMapMarker) become our own icons.
//   * Treasure pods: registry fed by TreasurePod.Awake/OnDestroy hooks.
//     Resource nodes: ResourceNodeDirector.AllResourceDirectors -> spawners ->
//     model state. Teleporter gadgets: MapDirector.Markers. No per-frame or
//     periodic scene scans.
//   * HudUI / PlayerDisplayOnMap / MapUI are captured by Awake hooks; scene
//     lookups (FindFirstObjectByType) are only a fallback, at most every 5 s,
//     and only while needed.
//   * The page prefabs live in the full map's Addressables UI scene. We hold a
//     preload request on it (UIDisplayManager.RequestPreloadDisplay, the
//     game's own path), so the page exists without opening the map and stays
//     loaded after it closes; one Resources.FindObjectsOfTypeAll then finds
//     the MapPrefabMapping.
//   * Any native fault: the module freezes itself for the session (hooks pass
//     through, no further IL2CPP/Unity calls, references are dropped).
//
// Threading: Setup() (worker) resolves names and creates hooks. All Unity work
// happens in a main-thread frame callback.

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <format>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/detour_guard.h"
#include "core/guard.h"
#include "core/hook_manager.h"
#include "core/il2cpp_api.h"
#include "core/log.h"
#include "core/main_thread.h"
#include "core/module.h"
#include "core/perf.h"
#include "core/settings.h"
#include "core/unity.h"
#include "modules/common/discordant_walls.h"
#include "modules/minimap/api.h"
#include "modules/minimap/game.h"
#include "modules/minimap/procgen.h"
#include "modules/minimap/ugui.h"
#include "ui/hud_edit.h"
#include "ui/overlay.h"

namespace sr2::modules {
namespace {

namespace settings = sr2::core::settings;
namespace unity = sr2::core::unity;
namespace main_thread = sr2::core::main_thread;
namespace ugui = sr2::modules::minimap::ugui;
namespace game = sr2::modules::minimap::game;
namespace procgen = sr2::modules::minimap::procgen;
using ugui::Color;
using ugui::Rect;
using ugui::Ref;
using ugui::Vec2;
using ugui::Vec3;

constexpr const char* kModule = "minimap";
constexpr float kBaseRadiusMeters = 160.0f;  // visible radius at zoom 1.0
constexpr float kSlowTick = 0.1f;            // 10 Hz: zone, hide rules, marker state
constexpr float kFogInterval = 2.0f;         // unrevealed / unknown fog areas re-queried
constexpr float kFogFullInterval = 30.0f;    // every area re-queried (safety net; reveals are permanent)
constexpr float kFogSettle = 1.0f;           // save data must stay ready this long before the first query
constexpr float kFogFallback = 10.0f;        // then accept an event record that is not GameModel's (logged)
constexpr float kDynInterval = 10.0f;
constexpr float kScanInterval = 3.0f;
constexpr float kFindInterval = 5.0f;        // scene lookup fallback
constexpr float kMappingScanRetry = 15.0f;    // asset scan while no mapping has the page
constexpr float kMappingScanBackoff = 60.0f;  // ... after kMappingScanFastTries misses
constexpr int kMappingScanFastTries = 6;
constexpr float kPreloadInterval = 10.0f;     // re-assert the map UI preload (idempotent, cheap)
constexpr float kPreloadWait = 30.0f;         // no asset scan while the preload is still loading (up to this)
constexpr const char* kPreloadRequester = "SR2Mods.Minimap.PagePreload";  // HashSet<object> key (string equality)
constexpr float kArrowBaseZoom = 2.5f;        // the arrow has its base size at this zoom
constexpr float kGameMarkerScale = 1.5f;      // the game marker (arrow + face + cone) is this much bigger than the classic arrow
constexpr float kIconBaseZoom = 1.0f;         // the map icons have their base size at this zoom (the default)
constexpr float kRoundBorder = 52;            // 9-slice border (px) of the 128 px rounded-square mask / frame
constexpr int kMarkerQueriesPerTick = 8;      // CanBeShown queries per 10 Hz tick (round robin)
constexpr float kIconRotStep = 0.5f;          // icon counter-rotation re-applied when the map turned this much (deg)
constexpr float kNegRefresh = 0.5f;           // outside-fog mask matrix re-set at least this often (s)
constexpr float kPerfLogInterval = 30.0f;     // "minimap: perf" counter line (perf_log on)
constexpr int kMarkerMaxFailures = 3;
constexpr float kBuildRetry = 5.0f;
constexpr float kPagePendingError = 60.0f;  // "no page prefab yet" is normal while loading; ERROR only after this long
constexpr float kHudWait = 10.0f;
constexpr float kMarkerRecheck = 2.0f;
constexpr int kBuildChunk = 32;              // graphics copied per frame while building
constexpr size_t kMaxIcons = 160;
constexpr float kPi = 3.14159265358979f;

enum Cat {
    kTeleporter = 0, kTreasurePod, kGordo, kResource, kOther, kNullWall,
    kPlaced, kDronePort, kPlacedTele, kPlacedDecor,  // placed gadgets: master toggle + groups
    kCatCount
};
const char* const kCatName[kCatCount] = {"teleporter", "treasure pod", "gordo", "resource node", "other",
                                         "nullifier wall", "placed gadget", "drone port",
                                         "placed teleporter", "placed decoration"};

std::string Lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}
bool Contains(const std::string& hay, const char* needle) { return hay.find(needle) != std::string::npos; }

template <class T>
T& F(void* obj, size_t off) {
    return unity::Field<T>(obj, off);
}

struct NativeFault : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// ------------------------------------------------------------- settings -----
struct Cfg {
    float posX = 98.7f, posY = 2.6f;  // % of the free space (ui/hud_edit.h), set with the HUD edit mode
    float size = 24, opacity = 1, zoom = 1;
    int shape = 0;        // 0 circle, 1 rounded square
    int orientation = 0;  // 0 north up, 1 rotate with player
    int zoomKey = 0;
    int arrowStyle = 1;  // 0 the game's player marker, 1 classic (procedural) arrow
    bool icons[kCatCount] = {true, true, true, false, true, true, true, true, true, false};
    bool respectFog = true;
    bool hideFullMap = true, hideMenus = true, hideConservatory = false;
    bool showAltitude = false;  // "Alt N m" label (Grey Labyrinth)
};

void RegisterSettings() {
    using settings::Kind;
    auto def = [](const char* key, const char* label, const char* group, Kind kind, double d, double mn, double mx,
                  double step, std::vector<std::string> options = {}, const char* tip = "") {
        settings::Def x;
        x.module = kModule;
        x.key = key;
        x.label = label;
        x.group = group;
        x.kind = kind;
        x.def = d;
        x.min = mn;
        x.max = mx;
        x.step = step;
        x.options = std::move(options);
        x.tooltip = tip;
        settings::Register(x);
    };
    // Placement: the overlay shows a "Move" button for these (ui/hud_edit.h). The
    // defaults are the old top-right corner, 2 % of the height from both edges, at 16:9.
    def("pos_x", "Horizontal position (% of free space)", "Layout", Kind::Float, 98.7, 0, 100, 0.01, {},
        "0 = left edge, 100 = right edge. Set with Move.");
    def("pos_y", "Vertical position (% of free space)", "Layout", Kind::Float, 2.6, 0, 100, 0.01, {},
        "0 = top edge, 100 = bottom edge. Set with Move.");
    def("size", "Size (% of screen height)", "Layout", Kind::Float, 24, 10, 50, 0.01, {},
        "Set with Move (drag a corner).");
    def("shape", "Shape", "Layout", Kind::Enum, 0, 0, 1, 1, {"Circle", "Rounded square"});
    def("opacity", "Opacity", "Layout", Kind::Float, 1, 0.2, 1, 0.05);
    def("zoom", "Zoom", "View", Kind::Float, 1, 0.5, 4, 0.25, {}, "1.0 shows about 160 m around the player");
    def("zoom_key", "Zoom cycle hotkey", "View", Kind::Key, VK_OEM_6, 0, 255, 1, {},
        "Cycles the zoom (x2, wraps to the minimum)");
    def("orientation", "Orientation", "View", Kind::Enum, 0, 0, 1, 1, {"North up", "Rotate with player"});
    def("arrow_style", "Player arrow", "View", Kind::Enum, 1, 0, 1, 1, {"Game marker", "Classic arrow"},
        "The arrow and the map icons grow and shrink with the zoom");
    // Units after the slider values (UI only).
    for (const char* k : {"pos_x", "pos_y", "size"}) settings::SetDisplay(kModule, k, "%");
    settings::SetDisplay(kModule, "opacity", "%", {}, 100);
    settings::SetDisplay(kModule, "zoom", "x");
    def("show_altitude", "Show altitude label (Grey Labyrinth)", "View", Kind::Bool, 0, 0, 1, 1, {},
        "Shows the player's height (\"Alt N m\") at the bottom of the minimap in the Grey Labyrinth");
    def("icon_teleporters", "Teleporters (map markers)", "Icons", Kind::Bool, 1, 0, 1, 1, {},
        "Teleporters drawn on the game's map page. Placed teleporter gadgets have their own toggle");
    def("icon_treasure_pods", "Treasure pods", "Icons", Kind::Bool, 1, 0, 1, 1);
    def("icon_gordos", "Gordos", "Icons", Kind::Bool, 1, 0, 1, 1);
    def("icon_resource_nodes", "Resource nodes", "Icons", Kind::Bool, 0, 0, 1, 1);
    def("icon_other", "Other map markers (plort doors, landmarks)", "Icons", Kind::Bool, 1, 0, 1, 1);
    def("icon_null_walls", "Nullifier walls (Grey Labyrinth)", "Icons", Kind::Bool, 1, 0, 1, 1, {},
        "Walls a placed Nullifier opens. Shown while intact, hidden once a Nullifier has opened them");
    def("icon_placed_gadgets", "Placed gadgets", "Icons", Kind::Bool, 1, 0, 1, 1, {},
        "Every gadget you placed, with its own icon. Placed gadgets ignore the fog of war. The three options "
        "below refine this one");
    def("icon_drone_ports", "Placed gadgets: drone ports", "Icons", Kind::Bool, 1, 0, 1, 1, {},
        "Quantum drone stations, with the full map's drone-type icon (ranch / explorer)");
    def("icon_placed_teleporters", "Placed gadgets: teleporters, warp depots, market/refinery links", "Icons", Kind::Bool, 1, 0, 1, 1);
    def("icon_placed_decor", "Placed gadgets: decorations", "Icons", Kind::Bool, 0, 0, 1, 1, {},
        "Purely decorative gadgets (usually many on a ranch). Off by default to keep the minimap readable");
    def("respect_fog", "Respect fog of war", "Icons", Kind::Bool, 1, 0, 1, 1, {},
        "Hide unrevealed map areas and every icon inside them (as the game's map does). Popped gordos, opened "
        "treasure pods, solved plort doors and opened Nullifier walls are never shown, whatever this setting");
    def("hide_full_map", "Full map open", "Hide when", Kind::Bool, 1, 0, 1, 1);
    def("hide_menus", "Menus open", "Hide when", Kind::Bool, 1, 0, 1, 1);
    def("hide_conservatory", "In the Conservatory", "Hide when", Kind::Bool, 0, 0, 1, 1);
    // Visibility parents (settings::Def::parent): the refinements show only while placed gadgets are drawn.
    for (const char* k : {"icon_drone_ports", "icon_placed_teleporters", "icon_placed_decor"})
        settings::SetParent(kModule, k, "icon_placed_gadgets");
}

Cfg LoadCfg() {
    Cfg c;
    c.posX = std::clamp(settings::GetFloat(kModule, "pos_x"), 0.0f, 100.0f);
    c.posY = std::clamp(settings::GetFloat(kModule, "pos_y"), 0.0f, 100.0f);
    c.size = settings::GetFloat(kModule, "size");
    c.shape = std::clamp(settings::GetInt(kModule, "shape"), 0, 1);
    c.opacity = std::clamp(settings::GetFloat(kModule, "opacity"), 0.05f, 1.0f);
    c.zoom = std::max(0.1f, settings::GetFloat(kModule, "zoom"));
    c.zoomKey = settings::GetInt(kModule, "zoom_key");
    c.orientation = std::clamp(settings::GetInt(kModule, "orientation"), 0, 1);
    c.icons[kTeleporter] = settings::GetBool(kModule, "icon_teleporters");
    c.icons[kTreasurePod] = settings::GetBool(kModule, "icon_treasure_pods");
    c.icons[kGordo] = settings::GetBool(kModule, "icon_gordos");
    c.icons[kResource] = settings::GetBool(kModule, "icon_resource_nodes");
    c.icons[kOther] = settings::GetBool(kModule, "icon_other");
    c.icons[kNullWall] = settings::GetBool(kModule, "icon_null_walls");
    c.icons[kPlaced] = settings::GetBool(kModule, "icon_placed_gadgets");
    c.icons[kDronePort] = settings::GetBool(kModule, "icon_drone_ports");
    c.icons[kPlacedTele] = settings::GetBool(kModule, "icon_placed_teleporters");
    c.icons[kPlacedDecor] = settings::GetBool(kModule, "icon_placed_decor");
    c.arrowStyle = std::clamp(settings::GetInt(kModule, "arrow_style"), 0, 1);
    c.respectFog = settings::GetBool(kModule, "respect_fog");
    c.hideFullMap = settings::GetBool(kModule, "hide_full_map");
    c.hideMenus = settings::GetBool(kModule, "hide_menus");
    c.hideConservatory = settings::GetBool(kModule, "hide_conservatory");
    c.showAltitude = settings::GetBool(kModule, "show_altitude");
    return c;
}

// ----------------------------------------------------------- hook state ------
// All detours run on the Unity main thread. Each one calls the original FIRST
// (unguarded), then records the raw `this` pointer inside a core DetourGuard;
// no IL2CPP/GC-handle call ever happens inside a hook. The tick validates and
// pins recorded objects. A native fault inside a guard marks the module
// faulted (core disables its hooks; every guard passes through from then on).
std::atomic<bool> g_dead{false};
std::atomic<bool> g_active{false};

std::atomic<Il2CppObject*> g_mapUI{nullptr}, g_hud{nullptr}, g_player{nullptr};
std::vector<Il2CppObject*> g_podAwoken, g_podDestroyed;  // main thread only, drained by the tick

// Pod registry: pod object -> GC handle (main thread only; tick-owned).
std::unordered_map<Il2CppObject*, Il2CppGCHandle> g_pods;

sr2::core::DetourGuard g_guardMapUI{"minimap", "MapUI.Awake"};
sr2::core::DetourGuard g_guardHud{"minimap", "HudUI.Awake"};
sr2::core::DetourGuard g_guardPlayer{"minimap", "PlayerDisplayOnMap.Awake"};
sr2::core::DetourGuard g_guardPodAwake{"minimap", "TreasurePod.Awake"};
sr2::core::DetourGuard g_guardPodDestroy{"minimap", "TreasurePod.OnDestroy"};

bool Recording() { return !g_dead.load() && g_active.load(); }

using AwakeFn = void (*)(Il2CppObject*, const MethodInfo*);
AwakeFn g_origMapUIAwake = nullptr, g_origHudAwake = nullptr, g_origPlayerAwake = nullptr;
AwakeFn g_origPodAwake = nullptr, g_origPodOnDestroy = nullptr;

void MapUIAwakeDetour(Il2CppObject* self, const MethodInfo* mi) {
    g_origMapUIAwake(self, mi);
    if (Recording()) g_guardMapUI.Run([&] { g_mapUI.store(self); });
}
void HudAwakeDetour(Il2CppObject* self, const MethodInfo* mi) {
    g_origHudAwake(self, mi);
    if (Recording()) g_guardHud.Run([&] { g_hud.store(self); });
}
void PlayerAwakeDetour(Il2CppObject* self, const MethodInfo* mi) {
    g_origPlayerAwake(self, mi);
    if (Recording()) g_guardPlayer.Run([&] { g_player.store(self); });
}
void PodAwakeDetour(Il2CppObject* self, const MethodInfo* mi) {
    g_origPodAwake(self, mi);
    if (Recording())
        g_guardPodAwake.Run([&] {
            if (self && g_podAwoken.size() < 4096) g_podAwoken.push_back(self);
        });
}
void PodOnDestroyDetour(Il2CppObject* self, const MethodInfo* mi) {
    g_origPodOnDestroy(self, mi);
    if (Recording())
        g_guardPodDestroy.Run([&] {
            if (self && g_pods.count(self) && g_podDestroyed.size() < 4096) g_podDestroyed.push_back(self);
        });
}

// =============================================================== module ======
class Minimap {
public:
    bool Setup(core::ModuleContext& ctx) {
        std::string err;
        if (!ugui::Resolve(err)) return ctx.Fail(err);
        std::vector<std::string> optional;
        if (!game::Resolve(sym_, err, optional))
            return ctx.Fail(err + " - the minimap needs the game's map page prefab and its projection");
        for (const auto& m : optional) SR2_INFO("minimap: optional (degraded): {}", m);
        if (!ugui::HasSpriteGen()) SR2_WARN("minimap: Texture2D/Sprite.Create unavailable - shapes need game sprites");
        {
            std::string werr;
            if (!walls::Resolve(werr)) SR2_INFO("minimap: optional (degraded): Nullifier wall icons unavailable: {}", werr);
        }

        auto hook = [&](const char* name, void* target, void* detour, AwakeFn* orig) {
            if (!target) return;
            std::string herr;
            if (!ctx.hooks().Create(kModule, std::string("minimap.") + name, target, detour,
                                    reinterpret_cast<void**>(orig), &herr))
                SR2_WARN("minimap: hook {} not created ({})", name, herr);
        };
        hook("MapUI.Awake", sym_.mapUIAwake, reinterpret_cast<void*>(&MapUIAwakeDetour), &g_origMapUIAwake);
        hook("HudUI.Awake", sym_.hudAwake, reinterpret_cast<void*>(&HudAwakeDetour), &g_origHudAwake);
        hook("PlayerDisplayOnMap.Awake", sym_.playerAwake, reinterpret_cast<void*>(&PlayerAwakeDetour),
             &g_origPlayerAwake);
        if (sym_.podAwake && sym_.podOnDestroy) {  // only as a pair
            hook("TreasurePod.Awake", sym_.podAwake, reinterpret_cast<void*>(&PodAwakeDetour), &g_origPodAwake);
            hook("TreasurePod.OnDestroy", sym_.podOnDestroy, reinterpret_cast<void*>(&PodOnDestroyDetour),
                 &g_origPodOnDestroy);
        }

        const auto& js = ctx.settings();
        frameSpriteName_ = js["frame_sprite"].asString();
        logSprites_ = js["log_sprite_candidates"].isBool() && js["log_sprite_candidates"].asBool();

        // Old corner + offset placement (before the HUD edit mode): migrated once, at the
        // first layout, when the canvas size (screen aspect) is known.
        migratePlacement_ = !settings::Saved(kModule, "pos_x") && !settings::Saved(kModule, "pos_y") &&
                            (settings::Saved(kModule, "position") || settings::Saved(kModule, "offset_x") ||
                             settings::Saved(kModule, "offset_y"));
        // The arrow rotation offset slider is gone (fixed offsets, see UpdateArrow). Its saved value is
        // never read; core keeps it in sr2mods.json like any unregistered key.
        if (settings::Saved(kModule, "arrow_rotation_offset"))
            SR2_INFO("minimap: setting arrow_rotation_offset ({}) is no longer used - the arrow rotation is fixed",
                     settings::GetFloat(kModule, "arrow_rotation_offset"));
        RegisterSettings();
        if (!onChange_)
            onChange_ = settings::OnChange(kModule, [this](const std::string& key) {
                if (key == "pos_x" || key == "pos_y") migratePlacement_ = false;  // placed since: keep that
                cfgDirty_.store(true);
            });
        SR2_INFO("minimap: resolved (fog {}, fog readiness {}, markers {}, pods {}, nodes {}, gadgets {}, load guard {})",
                 sym_.fogShouldReveal ? "yes" : "no",
                 (game::Has(sym_.edModel) && sym_.eventDirector)
                     ? (sym_.tryGetGameModel && game::Has(sym_.gmEventRecord) ? "EventDirector._model == GameModel record"
                                                                              : "EventDirector._model set")
                     : "unchecked",
                 sym_.canBeShown ? "yes" : "no",
                 (sym_.podAwake && sym_.podGetState) ? "yes" : "no", sym_.allNodeDirectors ? "yes" : "no",
                 game::Has(sym_.mdMarkers) ? "yes" : "no", sym_.sceneLoadInProgress ? "yes" : "no");
        return true;
    }

    void OnEnable() {
        if (g_dead.load()) {
            SR2_WARN("minimap: stays off for this session after a native fault");
            return;
        }
        cfgDirty_.store(true);
        podsSeeded_ = false;
        g_active.store(true);
        if (!frameCb_) frameCb_ = main_thread::AddFrameCallback(kModule, [this](float dt) { Tick(dt); });
    }

    void OnDisable() {
        g_active.store(false);
        if (frameCb_) main_thread::RemoveFrameCallback(frameCb_);
        frameCb_ = 0;
        if (g_dead.load()) return;  // frozen: no runtime calls at all
        if (main_thread::IsMainThread()) Teardown();
        else main_thread::Post([this] {
            if (!g_dead.load()) Teardown();
        });
    }

    // Read-only state for other modules (modules/minimap/api.h). Main thread; raw pointers, this frame only.
    bool ApiSnapshot(minimap::api::Snapshot& s) {
        if (frozen_ || g_dead.load() || !g_active.load() || !main_thread::IsMainThread()) return false;
        s = {};
        s.root = ui_.root.Alive() ? ui_.root.get() : nullptr;
        s.zone = zone_.get();
        s.mapDefinition = (view_.ok && view_.mapDef.Alive()) ? view_.mapDef.get() : nullptr;
        s.pagePrefab = (view_.ok && view_.prefab.Alive()) ? view_.prefab.get() : nullptr;
        s.mapComponent = (view_.ok && view_.mapComp.Alive()) ? view_.mapComp.get() : nullptr;
        s.mapDirector = mapDirector_.Alive() ? mapDirector_.get() : nullptr;
        if (s.mapComponent) {
            s.scaleX = view_.scale.x;
            s.scaleY = view_.scale.y;
            s.offsetX = view_.offset.x;
            s.offsetY = view_.offset.y;
        }
        s.visible = s.root && ui_.visible && ui_.built;
        s.loading = wasLoading_;
        s.size = ui_.size;
        const sr2::ui::hud_edit::Placement pl = EffectivePlacement();
        s.position = (pl.y >= 50 ? 2 : 0) + (pl.x >= 50 ? 1 : 0);  // nearest corner (free placement)
        return true;
    }

    // Every icon of the current page for the full map (modules/minimap/api.h): the minimap's own scanners over
    // the whole page instead of its view, without the fog test (the full map draws its fog over them), plus the
    // page markers the full map does not show yet if asked. Same fault policy as the tick.
    bool ApiCollectPageIcons(const minimap::api::PageIconQuery& q, std::vector<minimap::api::PageIcon>& out,
                             Il2CppObject** mapDef) {
        out.clear();
        if (mapDef) *mapDef = nullptr;
        if (frozen_ || g_dead.load() || !g_active.load() || !main_thread::IsMainThread()) return false;
        if (!view_.ok || build_.active || wasLoading_ || !view_.mapDef.Alive() || !view_.page.Alive()) return false;
        if (mapDef) *mapDef = view_.mapDef.get();
        const char* prevStep = step_;
        std::string err, fault;
        bool native = false;
        bool ok = core::Try([&] {
            if (!ugui::GuardNative([&] { CollectPageIconsImpl(q, out); }, &fault)) native = true;
        }, &err);
        scanCats_ = cfg_.icons;
        scanMax_ = kMaxIcons;
        if (ok && !native) {
            step_ = prevStep;
            return true;
        }
        out.clear();
        if (mapDef) *mapDef = nullptr;
        Die(std::format("step '{}' (full map icons): {}", step_, native ? fault : err));
        return false;
    }

private:
    static constexpr size_t kMaxPageIcons = 2000;  // full map: the whole page

    void CollectPageIconsImpl(const minimap::api::PageIconQuery& q, std::vector<minimap::api::PageIcon>& out) {
        static_assert(int{minimap::api::kIconCatCount} == int{kCatCount} &&
                      int{minimap::api::kIconPlacedDecor} == int{kPlacedDecor} &&
                      int{minimap::api::kIconNullWall} == int{kNullWall});
        Step("full map icons: world scan");
        bool cats[kCatCount];
        for (int c = 0; c < kCatCount; ++c) cats[c] = q.cats[c];
        const IconStats saved = stats_;
        stats_ = IconStats{};  // counted fresh for the page (the minimap's own counts are restored below)
        std::vector<IconItem> items;
        scanCats_ = cats;
        scanMax_ = kMaxPageIcons;
        auto onPage = [&](Vec3 wp, Vec2& p) {
            p = ProjectPage(wp, false);
            return OnPage(p);
        };
        bool ignoreFog = false;
        if (cats[kTreasurePod] && !view_.hasPagePods && sym_.treasurePod && sym_.podGetState) ScanPods(items, onPage);
        if (cats[kNullWall] && walls::Resolved()) {
            // the whole page: also the walls of proxied (unloaded) regions, which are registered but inactive
            wallsInactive_ = true;
            ScanNullWalls(items, onPage);
            wallsInactive_ = false;
            if (stats_.wallsActive + stats_.wallsInactive != pageWallsLogged_) {
                pageWallsLogged_ = stats_.wallsActive + stats_.wallsInactive;
                SR2_INFO("minimap: full map: Nullifier walls {} active + {} inactive (unloaded regions, {} positioned "
                         "from when they were loaded), {} shown",
                         stats_.wallsActive, stats_.wallsInactive, stats_.wallsFromCache, stats_.wallsShown);
            }
        }
        if (cats[kPlaced] && sym_.tryGetGameModel && sym_.gameModel && sym_.gadgetModel &&
            game::Has(sym_.gmIdentsByIdent) && game::Has(sym_.imLastPos) && game::Has(sym_.imIdent))
            ScanPlacedGadgets(items, onPage, ignoreFog);
        if (cats[kResource] && !view_.hasPageNodes && sym_.allNodeDirectors && game::Has(sym_.dirSpawners) &&
            game::Has(sym_.spawnerModel) && game::Has(sym_.modelNodeState))
            ScanNodes(items, onPage);
        stats_ = saved;
        scanCats_ = cfg_.icons;
        scanMax_ = kMaxIcons;
        out.reserve(items.size());
        for (const IconItem& it : items)
            if (it.sprite) out.push_back({it.cat, it.pos.x, it.pos.y, it.sprite, it.tint.r, it.tint.g, it.tint.b, it.tint.a});
        if (!q.undiscoveredMarkers) return;
        Step("full map icons: undiscovered markers");
        for (Marker& m : view_.markers) {
            // no CanBeShown bound: the full map always shows it, nothing to add
            if (!m.go || !cats[m.cat] || m.broken || !m.canBeShown || !m.sprite.Alive() || !m.comp.Alive()) continue;
            bool shown = true;
            if (!Managed([&] { shown = ugui::Call<bool>(m.canBeShown, m.comp.get()); }) || shown) continue;
            bool notDone = false;
            if (!QueryNotDone(m, notDone) || !notDone) continue;
            out.push_back({m.cat, m.pos.x, m.pos.y, m.sprite.get(), m.tint.r, m.tint.g, m.tint.b, m.tint.a});
        }
    }

    // ---------------------------------------------------------------- state --
    struct Marker {      // baked page marker (from the prefab) -> our icon
        Ref comp;        // prefab component (queries only)
        Ref go, t, img;  // our icon
        ugui::Fn canBeShown;
        Ref sprite;
        Color tint{1, 1, 1, 1};
        int cat = kOther;
        int kind = 0;    // MarkerKind: how its "done" state is read when the fog is ignored
        Vec2 pos{};
        float nextCheck = 0;
        int fails = 0;
        bool can = false, queried = false, active = false, broken = false;
    };
    enum MarkerKind { kMkOther = 0, kMkTeleporter, kMkGordo, kMkPuzzle };
    struct Quad {        // parallelogram in page space: c0 + u*ex + v*ey, u,v in [0,1]
        Vec2 c0, ex, ey;
        float det = 0;
        bool Contains(Vec2 p) const {
            if (std::fabs(det) < 1e-6f) return false;
            float dx = p.x - c0.x, dy = p.y - c0.y;
            float u = (dx * ey.y - dy * ey.x) / det, v = (ex.x * dy - ex.y * dx) / det;
            return u >= 0 && u <= 1 && v >= 0 && v <= 1;
        }
    };
    struct Group {       // fog area / dynamic element: our copied images
        Ref comp;        // prefab FogMapElement / DynamicMapElement
        std::vector<Ref> gos;
        std::vector<Quad> quads;  // fog: page-space shape of the area (one per image, or the element's own rect)
        float minX = 1e30f, minY = 1e30f, maxX = -1e30f, maxY = -1e30f;
        int state = -1;  // fog: the game's answer, 1 revealed / 0 unrevealed / -1 unknown (= fogged); dyn: 1 shown
        int vis = -1;    // fog: our images active (1) / inactive (0); only touched when this changes
        std::string name;         // fog: reveal event key (or the element's GameObject name), for the logs
        bool fromRect = false;    // fog: no art, area = the FogMapElement's own RectTransform
        void AddQuad(Vec2 c0, Vec2 ex, Vec2 ey) {
            quads.push_back({c0, ex, ey, ex.x * ey.y - ex.y * ey.x});
            for (Vec2 c : {c0, Vec2{c0.x + ex.x, c0.y + ex.y}, Vec2{c0.x + ey.x, c0.y + ey.y},
                           Vec2{c0.x + ex.x + ey.x, c0.y + ex.y + ey.y}}) {
                minX = std::min(minX, c.x);
                maxX = std::max(maxX, c.x);
                minY = std::min(minY, c.y);
                maxY = std::max(maxY, c.y);
            }
        }
        bool Contains(Vec2 p) const {
            if (p.x < minX || p.x > maxX || p.y < minY || p.y > maxY) return false;
            for (const Quad& q : quads)
                if (q.Contains(p)) return true;
            return false;
        }
    };
    struct Icon {        // pods / nodes / teleporter gadgets
        Ref go, t, img;
        bool active = false;
        // last values set (ApplyIcons sets only what changed)
        Il2CppObject* sprite = nullptr;  // identity only (the item's sprite is pinned by its owner)
        Color tint{-1, -1, -1, -1};
        Vec2 pos{1e9f, 1e9f};
        float oS = -1, oR = 1e9f;        // Orient: scale / rotation applied
    };
    // The game's player marker (the full map's PlayerMarker prefab), read once: one entry per visible Image.
    // Geometry is in the space of the prefab root (upright parts) or of PlayerMapMarker._arrowRect (parts
    // that turn with the heading), so our copy can rotate the facing parts like PlayerMapMarker.Rotate does.
    enum MarkerAnim { kAnimNone = 0, kAnimArrowPulse, kAnimCone };
    struct MarkerPartDef {
        Ref sprite;
        Color color{1, 1, 1, 1};
        int type = 0;
        float ppu = 1;
        bool preserveAspect = false;
        bool facing = false;  // under _arrowRect: turns with the heading
        bool below = false;   // under _moveBelowMarkersTransform: drawn under the map icons (the view cone)
        Vec2 pos{}, size{}, pivot{0.5f, 0.5f};  // pivot point in the reference space, rect size, pivot
        Vec2 up{0, 1};        // one unit of the parent's +y in the reference space (cone animation)
        float angle = 0;
        bool mirrored = false;
        int anim = kAnimNone;
        std::string name;
    };
    struct GameMarker {
        std::vector<MarkerPartDef> parts;
        Vec2 facingOrigin{};  // _arrowRect's origin in the prefab root's space
        float rootH = 50;     // the prefab root's rect height: drawn at the arrow size
        float rotOffset = 0;  // PlayerMapMarker._rotationOffset
        bool ok = false;
    };
    struct MarkerPart {   // one of our copies of a MarkerPartDef
        Ref t, img;
        int anim = kAnimNone;
        Vec2 pos{}, up{0, 1};
        Color color{1, 1, 1, 1};
        float sy = 1;     // -1 when mirrored
    };
    struct View {
        Ref mapDef, prefab, mapComp, mapT;
        Vec2 scale{}, offset{};
        Rect clampRect{};
        bool clamp = false;
        float unitsPerMeter = 1;
        Ref page;  // our page container (RectTransform), child of the rotator
        std::vector<Marker> markers;
        std::vector<Group> fog, dyn;
        // Fog-shader art outside every FogMapElement ("fog_static": the "covering up static fog" patches and
        // the negative-masked outside fog). Nothing in the game toggles them; drawn while the fog is respected.
        Group staticFog;
        // The outside fog's mask: our copy of the prefab's NegativeMaskMap transform (child of our page).
        struct NegMask {
            Ref t, canvasT;        // our mask transform; the canvas whose space the shader sees (re-resolved)
            float noiseScale = 0, noiseStrength = 0, softness = 0, roundness = 0;
            bool ok = false, overlay = true;
            int copies = 0;        // NegativeMask-shader images copied
            std::string info;      // build log
        } neg;
        std::vector<Icon> icons;
        // The game marker's view cone: in our page, after the art and before the icons, so the icons
        // are drawn over it as in the full map (PlayerMapMarker.SetBelowMarkerLayer). Own nested Canvas.
        Ref coneT;
        std::vector<MarkerPart> coneParts;
        bool ok = false;
        bool hasPagePods = false, hasPageNodes = false;
    };
    // Time-sliced page copy.
    struct Build {
        bool active = false;
        Ref rootT, zoneT;
        std::vector<Ref> graphics;
        std::vector<Ref> pins;  // keeps group-root transforms' wrappers (pointer identity)
        std::unordered_map<Il2CppObject*, int> roots;  // transform -> code (kind << 24 | index)
        std::unordered_map<Il2CppObject*, float> groupAlpha;  // transform -> CanvasGroup alpha
        std::unordered_map<Il2CppObject*, bool> hiddenMaskGraphic;  // transform with Mask.showMaskGraphic == false
        std::unordered_map<Il2CppObject*, bool> forceActive;  // GameObjects Map.Awake activates (_forceActiveOnAwake)
        std::unordered_map<Il2CppObject*, bool> filterSeen;   // textures already checked for point filtering
        int forcedActive = 0, pointFiltered = 0;
        struct MatInfo {
            bool custom = false, stencil = false;
            Ref tex;
            Rect uv{0, 0, 1, 1};
            std::string label;
        };
        std::unordered_map<Il2CppObject*, MatInfo> materials;  // keys pinned by `pins`
        std::unordered_map<Il2CppObject*, bool> markerSprites;  // the player marker's sprites (pinned by gm_)
        size_t next = 0;
        int copied = 0, skipped = 0;
        // diagnostics (one log line per build)
        int skipInactive = 0, skipDisabled = 0, skipMaskGraphic = 0, skipPlaceholder = 0, skipMarkerArt = 0,
            skipTransparent = 0, skipOther = 0;
        int matKept = 0, matAsTexture = 0, matDropped = 0, noTexture = 0;
        struct Note {
            float area;
            std::string text;
        };
        std::vector<Note> notes;
        std::string placeholders;
        int skipNegMask = 0;         // outside-fog images skipped (their mask cannot be driven)
        int fogOpaque = 0;           // fog-shader copies shielded from the root CanvasGroup's opacity
        int staticFog = 0;           // fog-shader images outside FogMapElements (always drawn)
        std::string staticFogNames;
    };
    enum RootKind { kRootMarker = 1, kRootFog = 2, kRootDyn = 3, kRootZone = 4, kRootSkip = 5 };

    struct Ui {
        Ref root, rootT, group, parentT, ownCanvas;
        Ref maskImg, bg, overlay, rotator, arrowHolder, arrowImg, arrowT, frame, label;
        Ref gameMarkerT, gameFacingT;      // our copy of the game's player marker (child of arrowHolder)
        std::vector<MarkerPart> gameParts;
        float markerK = 1;                 // game marker scale: prefab units -> minimap pixels
        int arrowStyleApplied = -1;        // 0 game marker, 1 classic arrow
        bool visible = true;
        bool built = false;
        float size = 200;
    };

    game::Symbols sym_;
    Cfg cfg_;
    std::atomic<bool> cfgDirty_{true};
    std::atomic<bool> migratePlacement_{false};  // old corner placement still to convert (first layout)
    sr2::ui::hud_edit::Placement shown_{-1, -1, -1};  // placement of the last ApplyLayout
    static bool SamePlace(const sr2::ui::hud_edit::Placement& a, const sr2::ui::hud_edit::Placement& b) {
        return a.x == b.x && a.y == b.y && a.size == b.size;
    }
    int frameCb_ = 0, onChange_ = 0;
    std::string frameSpriteName_;
    bool logSprites_ = false;

    Ref playerDisplay_, playerT_, sceneLoader_, mapDirector_, hud_, mapUI_, prefabMapping_;
    Ref zone_, wantedMap_;
    std::string zoneName_;
    bool inConservatory_ = false, inLabyrinth_ = false, unknownArea_ = false, hidden_ = true, mapOpen_ = false;
    float findPlayerCd_ = 0, findLoaderCd_ = 0, findHudCd_ = 0, findDirectorCd_ = 0, mappingScanCd_ = 0;
    int mappingScanMisses_ = 0;
    size_t markerRR_ = 0;
    bool markerStatsLogged_ = false;
    float lastArrowZ_ = 1e9f;
    GameMarker gm_;                                       // kept across page rebuilds while its sprites live
    Vec2 coneP_{1e9f, 1e9f};                              // last cone placement (page space)
    float coneZ_ = 1e9f, coneS_ = -1;
    bool fogMode_ = true;  // cfg_.respectFog the marker answers belong to
    int worldResets_ = 0;
    float buildCd_ = 0, hudWait_ = 0, time_ = 0;
    bool podsSeeded_ = false;
    std::string lastBuildError_;
    float pagePendingSince_ = -1;  // time_ of the first "no page prefab yet" of this map (-1: none)
    bool pagePendingReported_ = false;

    // Map UI preload: the full map's UI scene (MapUI prefab -> MapPrefabMapping -> page prefabs) is an
    // Addressables scene the game loads only while the map is open and unloads after it closes. We hold
    // a preload request on it through the game's own UIDisplayManager.RequestPreloadDisplay.
    Ref mapUIRoot_, preloadEntry_;
    std::string mapDisplayName_;
    float preloadCd_ = 0, preloadT0_ = 0;
    bool preloadDone_ = false, preloadFailLogged_ = false, preloadWaitLogged_ = false;
    bool fogLogged_ = false, iconsLogged_ = false;

    // Fog readiness: the areas' states belong to one event record (the loaded save's EventRecordModel,
    // compared by identity only, never dereferenced through this pointer). Until the record is the
    // loaded save's and has stayed so for kFogSettle seconds, every area is UNKNOWN = fogged.
    Il2CppObject* fogRecord_ = nullptr;
    Il2CppObject* fogDirLogged_ = nullptr;  // director named in the last "reveal source" line (identity)
    float fogReadySince_ = -1, fogWaitSince_ = -1, fogFullAcc_ = 0;
    bool fogFullDue_ = true, fogReadyLogged_ = false, fogFallbackLogged_ = false;
    std::string fogWaitReason_;
    int fogLastRevealed_ = -1, fogLastHidden_ = -1, fogLastUnknown_ = -1;
    bool fogSummaryLogged_ = false;  // "fog summary (save loaded)": once per event record / world

    Ui ui_;
    View view_;
    Build build_;
    Ref sprDisc_, sprRound_, sprCircleFrame_, sprRoundFrame_, sprArrow_, sprDot_, sprPod_, sprFrameOverride_, sprWall_;
    Ref sprDronePort_;      // procedural fallback (drone ports)
    Ref sprGadget_;         // procedural fallback (other placed gadgets)
    Ref sprDroneType_[2];   // the full map's drone-type sprites (0 ranch, 1 explorer), from the page's marker mapping
    float droneIconRetry_[2] = {0, 0};
    bool droneIconLogged_[2] = {false, false};
    int lastGadgetSig_ = -1;  // placed-gadget counts of the last change log
    bool spritesTried_ = false, podIconTried_ = false, frameOverrideTried_ = false;

    float lastS_ = -1, lastR_ = 1e9f, slowAcc_ = 0, fogAcc_ = 0, dynAcc_ = 0, scanAcc_ = 0;
    float lastIconR_ = 1e9f;                 // icon counter-rotation applied (kIconRotStep steps)
    float pageS_ = -1;                       // page scale / position last set (skip same-value sets)
    Vec2 pageP_{1e9f, 1e9f};
    bool negDirty_ = true;                   // outside-fog mask matrix must be set again
    float negAge_ = 0;                       // seconds since it was last set (safety refresh)
    // Work counters for the periodic "minimap: perf" line (only while perf_log is on).
    struct PerfCounters {
        int frames = 0, pageSets = 0, negSets = 0, orients = 0, iconSets = 0, iconSame = 0, podsFar = 0, podsNear = 0;
    } pc_;
    float pcAcc_ = 0;
    bool mapWasOpen_ = false, keyDown_ = false, wasLoading_ = false;
    Vec2 playerP_{};
    int lastAlt_ = 1 << 30;
    const char* step_ = "";

    struct DescInfo {
        Ref pin;  // keeps the key object alive (the map key is its raw pointer)
        bool teleporter = false;
        Ref icon;
    };
    std::map<Il2CppObject*, DescInfo> descCache_;
    struct GadgetInfo {   // per GadgetDefinition (cached until the world resets)
        Ref pin;          // keeps the key object alive (the map key is its raw pointer)
        int group = kPlaced;
        Ref icon;         // _optionalMapIcon, else IdentifiableType.icon
    };
    std::map<Il2CppObject*, GadgetInfo> gadgetCache_;
    std::set<std::string> loggedDefs_;  // gadget definition names already logged (kept across world resets)

    void Step(const char* s) { step_ = s; }

    // Runs a call into game code. Managed exceptions are contained (returns
    // false); a native fault aborts the frame and freezes the module.
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

    // ---------------------------------------------------------------- tick ---
    void Tick(float dt) {
        if (!frozen_ && core::ModuleFaulted(kModule)) {
            Die("module marked faulted (fault inside a hook detour, see the line above)");
            return;
        }
        if (g_dead.load()) {
            if (!frozen_) Die("module frozen");
            return;
        }
        std::string err, fault;
        bool native = false;
        bool ok = core::Try([&] {
            if (!ugui::GuardNative([&] { TickImpl(dt); }, &fault)) native = true;
        }, &err);
        if (ok && !native) return;
        Die(std::format("step '{}': {}", step_, native ? fault : err));
    }

    // Permanent shutdown after a fault: no teardown calls into Unity, hooks pass
    // through, every reference is dropped without touching IL2CPP.
    bool frozen_ = false;

    void Die(const std::string& why) {
        frozen_ = true;
        g_dead.store(true);
        g_active.store(false);
        ugui::FreezeRuntime();
        SR2_ERROR("minimap: {} - minimap disabled for the rest of this session (no further game calls)", why);
        core::MarkModuleFaulted(kModule, why);  // ModuleManager: FAILED; hooks disabled next tick
        ui_ = Ui{};
        view_ = View{};
        build_ = Build{};
        gm_ = GameMarker{};
        descCache_.clear();
        gadgetCache_.clear();
        for (Ref* r : {&playerDisplay_, &playerT_, &sceneLoader_, &mapDirector_, &hud_, &mapUI_, &prefabMapping_,
                       &zone_, &wantedMap_, &sprDisc_, &sprRound_, &sprCircleFrame_, &sprRoundFrame_, &sprArrow_,
                       &sprDot_, &sprPod_, &sprFrameOverride_, &sprWall_, &sprDronePort_, &sprGadget_, &sprDroneType_[0],
                       &sprDroneType_[1], &mapUIRoot_, &preloadEntry_})
            r->Abandon();
        g_pods.clear();  // handles leaked on purpose
        podPos_.clear();
        g_podAwoken.clear();
        g_podDestroyed.clear();
        if (frameCb_) main_thread::RemoveFrameCallback(frameCb_);
        frameCb_ = 0;
    }

    void TickImpl(float dt) {
        time_ += dt;
        pcAcc_ += dt;
        if (pcAcc_ >= kPerfLogInterval) {  // how much per-frame work the caches saved (perf_log on)
            if (sr2::core::perf::Enabled() && pc_.frames > 0)
                SR2_INFO("minimap: perf ({:.0f} s): {} frames shown, page transform set in {} of them; outside-fog "
                         "mask set {} times; icon orient calls {}; icon property sets {} ({} icon updates with "
                         "nothing to set); pods {} in view / {} skipped without a game call",
                         pcAcc_, pc_.frames, pc_.pageSets, pc_.negSets, pc_.orients, pc_.iconSets, pc_.iconSame,
                         pc_.podsNear, pc_.podsFar);
            pcAcc_ = 0;
            pc_ = PerfCounters{};
        }
        Step("settings");
        if (cfgDirty_.exchange(false)) {
            cfg_ = LoadCfg();
            if (ui_.built) ApplyLayout();
            fogAcc_ = kFogInterval;
            fogFullDue_ = true;  // fog shown / hidden by the setting at once, every area re-queried
            slowAcc_ = kSlowTick;
            scanAcc_ = kScanInterval;
            if (cfg_.respectFog != fogMode_) {  // other visibility rule: re-query every marker
                fogMode_ = cfg_.respectFog;
                markerStatsLogged_ = false;
                for (Marker& m : view_.markers) m.queried = false;
            }
            for (Marker& m : view_.markers) m.nextCheck = 0;
        }
        Step("edit preview");
        if (ui_.built && !SamePlace(EffectivePlacement(), shown_)) ApplyLayout();  // HUD edit mode: follow the drag
        Step("hotkey");
        HandleHotkey();
        Step("captures");
        TakeCaptures();

        // 1. never touch map data during scene/zone loads
        Step("scene load check");
        if (IsLoading(dt)) {
            if (!wasLoading_) {
                SR2_DEBUG("minimap: scene load in progress - hidden");
                ResetWorld("scene load started");
            }
            wasLoading_ = true;
            SetVisible(false);
            return;
        }
        if (wasLoading_) {
            wasLoading_ = false;
            slowAcc_ = kSlowTick;
        }

        // 2. zone / player / UI / map at 10 Hz
        slowAcc_ += dt;
        bool slow = slowAcc_ >= kSlowTick;
        if (slow) {
            float sdt = slowAcc_;
            slowAcc_ = 0;
            SR2_PERF_DETAIL("minimap.slow (10 Hz)");
            if (!SlowUpdate(sdt)) {
                SetVisible(false);
                return;
            }
        }
        if (!ui_.built || !zone_ || !wantedMap_) {
            SetVisible(false);
            return;
        }

        // 3. time-sliced page build
        if (build_.active) {
            Step("build: copy page");
            SR2_PERF_DETAIL("minimap.build (page copy)");
            BuildStep();
            SetVisible(false);
            return;
        }
        if (!view_.ok || hidden_) {
            SetVisible(false);
            return;
        }
        Step("show");
        SetVisible(true);

        // 4. per frame: transform only
        {
            Step("transform");
            SR2_PERF_DETAIL("minimap.transform");
            UpdateTransform();
        }
        {
            Step("negative mask");
            SR2_PERF_DETAIL("minimap.negmask");
            UpdateNegativeMask(dt);
        }

        // 5. low rate
        fogFullAcc_ += dt;
        if (fogFullAcc_ >= kFogFullInterval) {
            fogFullAcc_ = 0;
            fogFullDue_ = true;
        }
        fogAcc_ += dt;
        if (fogAcc_ >= kFogInterval) {
            fogAcc_ = 0;
            Step("fog");
            SR2_PERF_DETAIL("minimap.fog (2 s)");
            UpdateFog("periodic");
        }
        dynAcc_ += dt;
        if (dynAcc_ >= kDynInterval) {
            dynAcc_ = 0;
            Step("dynamic elements");
            SR2_PERF_DETAIL("minimap.dynamic (10 s)");
            UpdateDynamic();
        }
        if (slow) {
            Step("markers");
            SR2_PERF_DETAIL("minimap.markers (10 Hz)");
            UpdateMarkers();
            Step("label");
            UpdateLabel();
        }
        scanAcc_ += dt;
        if (scanAcc_ >= kScanInterval) {
            scanAcc_ = 0;
            Step("icons");
            SR2_PERF_DETAIL("minimap.icons (3 s)");
            ScanIcons();
        }
        Step("idle");
    }

    // Returns false when the minimap must stay hidden this tick.
    bool SlowUpdate(float sdt) {
        Step("zone");
        Il2CppObject* z = CurrentZone();
        if (!z) {  // main menu / no player
            if (zone_) {
                zone_.Reset(nullptr);
                SR2_DEBUG("minimap: no current zone");
            }
            return false;
        }
        Step("player");
        if (!EnsurePlayer(sdt)) return false;
        Step("ui");
        if (!EnsureUi(sdt)) return false;
        Step("map director");
        EnsureDirector(sdt);
        Step("map UI preload");
        EnsurePreload(sdt);
        Step("fog readiness");
        if (cfg_.respectFog && sym_.fogShouldReveal && !FogSettled()) {  // until settled only (then: 2 s rate)
            FogSource src;
            FogReadiness(src);
        }
        Step("zone change");
        UpdateZone(z);
        Step("map build");
        if (buildCd_ > 0) buildCd_ -= sdt;
        if (mappingScanCd_ > 0) mappingScanCd_ -= sdt;
        if (view_.ok && (!view_.mapComp.Alive() || !view_.mapDef.Alive())) {
            SR2_INFO("minimap: map page prefab unloaded - rebuilding the copy");
            DestroyView();
        }
        bool needBuild = wantedMap_ && !build_.active &&
                         (!view_.ok || view_.mapDef.get() != wantedMap_.get() || !view_.page.Alive());
        if (needBuild && buildCd_ <= 0) {
            SR2_PERF_DETAIL("minimap.page build start");
            StartBuild();
        }

        Step("hide rules");
        mapOpen_ = FullMapOpen();
        if (mapWasOpen_ && !mapOpen_) {
            // reveals animate in the full map: refresh fog (every area) / markers / dynamic elements now
            fogAcc_ = kFogInterval;
            fogFullDue_ = true;
            dynAcc_ = kDynInterval;
            for (Marker& m : view_.markers) m.nextCheck = 0;
            negDirty_ = true;
        }
        mapWasOpen_ = mapOpen_;
        unknownArea_ = false;
        if (cfg_.respectFog && sym_.playerUnknownArea && playerDisplay_.Alive())
            Managed([&] { unknownArea_ = ugui::Call<bool>(sym_.playerUnknownArea, playerDisplay_.get()); });
        hidden_ = ShouldHide();
        return true;
    }

    // ------------------------------------------------------------ teardown ---
    // One teardown step; a native fault freezes the module (no further calls).
    template <class Fn>
    void TeardownStep(const char* what, Fn&& fn) {
        if (frozen_) return;
        std::string err, fault;
        bool native = false;
        bool ok = core::Try([&] {
            if (!ugui::GuardNative(fn, &fault)) native = true;
        }, &err);
        if (native) Die(std::format("teardown step '{}': {}", what, fault));
        else if (!ok) SR2_WARN("minimap: teardown step '{}' failed: {}", what, err);
    }

    void Teardown() {
        // Destroy OUR objects only (never game objects), then drop each reference independently.
        Ref root = std::move(ui_.root), canvas = std::move(ui_.ownCanvas);
        TeardownStep("destroy root", [&] { if (root.Alive()) ugui::Destroy(root.get()); });
        TeardownStep("destroy own canvas", [&] { if (canvas.Alive()) ugui::Destroy(canvas.get()); });
        TeardownStep("release root", [&] { root.Reset(nullptr); });
        TeardownStep("release canvas", [&] { canvas.Reset(nullptr); });
        TeardownStep("release view", [&] { view_ = View{}; });
        TeardownStep("release build", [&] { build_ = Build{}; });
        TeardownStep("release ui", [&] { ui_ = Ui{}; });
        TeardownStep("release game marker", [&] { gm_ = GameMarker{}; });
        // Give the map UI scene back to the game (it unloads it again when nothing else needs it).
        if (!mapDisplayName_.empty() && sym_.udmInstance && sym_.udmReleasePreload)  // no-op if not registered
            TeardownStep("release map UI preload", [&] {
                Il2CppObject* udm = ugui::Call<Il2CppObject*>(sym_.udmInstance);
                if (ugui::IsA(udm, sym_.uiDisplayManager))
                    ugui::Call<void>(sym_.udmReleasePreload, udm, unity::NewString(mapDisplayName_),
                                     unity::NewString(kPreloadRequester));
            });
        for (Ref* r : {&playerDisplay_, &playerT_, &sceneLoader_, &mapDirector_, &hud_, &mapUI_, &prefabMapping_,
                       &zone_, &wantedMap_, &mapUIRoot_, &preloadEntry_})
            TeardownStep("release reference", [&] { r->Reset(nullptr); });
        TeardownStep("release descriptors", [&] {
            descCache_.clear();
            gadgetCache_.clear();
        });
        for (auto& [obj, h] : g_pods) {
            Il2CppGCHandle hh = h;
            TeardownStep("release pod", [&] { unity::FreeGCHandle(hh); });
        }
        g_pods.clear();
        podPos_.clear();
        g_podAwoken.clear();
        g_podDestroyed.clear();
        g_mapUI.store(nullptr);
        g_hud.store(nullptr);
        g_player.store(nullptr);
        ResetTransformCache();
        mapWasOpen_ = false;
        hudWait_ = 0;
        preloadDone_ = false;
        preloadCd_ = 0;
    }

    // Cached transform state: everything is set again on the next frame.
    void ResetTransformCache() {
        lastS_ = -1;
        lastR_ = lastIconR_ = 1e9f;
        pageS_ = -1;
        pageP_ = {1e9f, 1e9f};
        negDirty_ = true;
    }

    void DestroyView() {
        if (view_.page.Alive()) ugui::Destroy(unity::GameObjectOf(view_.page.get()));
        view_ = View{};
        build_ = Build{};
        ResetTransformCache();
        lastArrowZ_ = 1e9f;
        coneS_ = -1;
        markerRR_ = 0;
        markerStatsLogged_ = false;
        fogLastRevealed_ = fogLastHidden_ = fogLastUnknown_ = -1;  // the next evaluation is logged
        fogFullDue_ = true;
    }

    // Save change / scene unload: drop everything that belongs to the old world
    // and rebuild from scratch. Only our own objects are destroyed; game
    // references are simply released (fresh lookups follow).
    void ResetWorld(const char* reason) {
        ++worldResets_;
        SR2_INFO("minimap: world reset #{} ({}) - cached map objects dropped, rebuilding", worldResets_, reason);
        DestroyView();
        descCache_.clear();
        gadgetCache_.clear();
        zone_.Reset(nullptr);
        wantedMap_.Reset(nullptr);
        zoneName_.clear();
        mapDirector_.Reset(nullptr);
        findDirectorCd_ = 0;
        // The UI load manager may drop preload requests on a scene change: request again after the load
        // (idempotent; the display name is constant).
        mapUIRoot_.Reset(nullptr);
        preloadEntry_.Reset(nullptr);
        preloadDone_ = false;
        preloadCd_ = 0;
        preloadWaitLogged_ = false;
        if (!mapUI_.Alive()) mapUI_.Reset(nullptr);
        if (!prefabMapping_.Alive()) prefabMapping_.Reset(nullptr);
        if (!sprPod_.Alive()) {
            sprPod_.Reset(nullptr);
            podIconTried_ = false;
        }
        PrunePods();
        podPos_.clear();
        podsSeeded_ = false;
        buildCd_ = 0;
        mappingScanCd_ = 0;
        mappingScanMisses_ = 0;
        lastBuildError_.clear();
        pagePendingSince_ = -1;
        unknownArea_ = false;
        mapWasOpen_ = false;
        lastAlt_ = 1 << 30;
        ResetFogReadiness();
    }

    // Save change / scene load: the fog states and the readiness belong to the old world.
    void ResetFogReadiness() {
        fogRecord_ = nullptr;
        fogDirLogged_ = nullptr;
        fogReadySince_ = -1;
        fogWaitSince_ = -1;
        fogFullAcc_ = 0;
        fogFullDue_ = true;
        fogReadyLogged_ = false;
        fogFallbackLogged_ = false;
        fogWaitReason_.clear();
        fogLastRevealed_ = fogLastHidden_ = fogLastUnknown_ = -1;
        fogSummaryLogged_ = false;
        for (Group& f : view_.fog) f.state = -1;  // view_ is normally already gone (DestroyView)
    }

    // Pods whose native object is gone (OnDestroy normally removes them).
    static void PrunePods() {
        for (auto it = g_pods.begin(); it != g_pods.end();) {
            if (!unity::IsAlive(it->first)) {
                unity::FreeGCHandle(it->second);
                it = g_pods.erase(it);
            } else {
                ++it;
            }
        }
    }

    void SetVisible(bool v) {
        if (!ui_.root.Alive() || ui_.visible == v) return;
        ui_.visible = v;
        if (v) negDirty_ = true;  // the full map may have set the outside-fog global while we were hidden
        unity::SetActive(ui_.root.get(), v);
    }

    // ---------------------------------------------------------- captures -----
    void TakeCaptures() {
        // Pod queues first (removals, then additions).
        for (Il2CppObject* pod : g_podDestroyed) {
            auto it = g_pods.find(pod);
            if (it != g_pods.end()) {
                unity::FreeGCHandle(it->second);
                g_pods.erase(it);
                podPos_.erase(pod);
            }
        }
        g_podDestroyed.clear();
        for (Il2CppObject* pod : g_podAwoken)
            if (!g_pods.count(pod) && ugui::IsA(pod, sym_.treasurePod))
                if (Il2CppGCHandle h = unity::NewGCHandle(pod, false)) g_pods.emplace(pod, h);
        g_podAwoken.clear();

        if (Il2CppObject* o = g_mapUI.exchange(nullptr); ugui::IsA(o, sym_.mapUI)) {
            mapUI_.Reset(o);
            Il2CppObject* m = F<Il2CppObject*>(o, sym_.mapUIPrefabMapping);
            if (ugui::IsA(m, sym_.mapPrefabMapping) && m != prefabMapping_.get()) {
                prefabMapping_.Reset(m);
                buildCd_ = 0;  // a page that failed before may build now
                SR2_DEBUG("minimap: map prefab mapping taken from MapUI");
            }
            if (game::Has(sym_.mapUIDirector) && !mapDirector_.Alive()) {
                Il2CppObject* d = F<Il2CppObject*>(o, sym_.mapUIDirector);
                if (ugui::IsA(d, sym_.mapDirector)) mapDirector_.Reset(d);
            }
        }
        if (Il2CppObject* o = g_hud.exchange(nullptr); ugui::IsA(o, sym_.hudUI) && o != hud_.get()) {
            hud_.Reset(o);
            SR2_DEBUG("minimap: HudUI captured (Awake)");
        }
        if (Il2CppObject* o = g_player.exchange(nullptr); ugui::IsA(o, sym_.playerDisplay) && o != playerDisplay_.get()) {
            if (playerDisplay_) ResetWorld("new player object (save loaded)");
            playerDisplay_.Reset(o);
            playerT_.Reset(unity::TransformOf(o));
        }
    }

    // --------------------------------------------------------- environment ---
    bool IsLoading(float dt) {
        if (!sym_.sceneLoader || !sym_.sceneLoadInProgress) return false;
        if (!sceneLoader_.Alive()) {
            findLoaderCd_ -= dt;
            if (findLoaderCd_ > 0) return false;
            findLoaderCd_ = kFindInterval;
            sceneLoader_.Reset(ugui::FindFirst(sym_.sceneLoader, false));
            if (!sceneLoader_.Alive()) return false;
        }
        bool loading = false;
        Managed([&] { loading = ugui::Call<bool>(sym_.sceneLoadInProgress, sceneLoader_.get()); });
        return loading;
    }

    Il2CppObject* CurrentZone() {
        Il2CppObject* z = nullptr;
        if (sym_.getPlayerCurrentZone) Managed([&] { z = ugui::Call<Il2CppObject*>(sym_.getPlayerCurrentZone); });
        if (!z && sym_.playerGetZone && playerDisplay_.Alive())
            Managed([&] { z = ugui::Call<Il2CppObject*>(sym_.playerGetZone, playerDisplay_.get()); });
        return unity::IsAlive(z) ? z : nullptr;
    }

    bool EnsurePlayer(float dt) {
        if (playerDisplay_.Alive() && playerT_.Alive()) return true;
        if (playerDisplay_) {  // had one: it was destroyed (scene unload / save change)
            playerDisplay_.Reset(nullptr);
            playerT_.Reset(nullptr);
            ResetWorld("player object destroyed");
            findPlayerCd_ = 0;
        }
        findPlayerCd_ -= dt;
        if (findPlayerCd_ > 0) return false;
        findPlayerCd_ = kFindInterval;  // fallback only (normally captured by the Awake hook)
        Il2CppObject* pd = ugui::FindFirst(sym_.playerDisplay, false);
        if (!pd) return false;
        if (playerDisplay_ && pd != playerDisplay_.get()) ResetWorld("new player object (lookup)");
        playerDisplay_.Reset(pd);
        playerT_.Reset(unity::TransformOf(pd));
        SR2_INFO("minimap: player found by lookup ({})", unity::GetName(unity::GameObjectOf(pd)));
        return playerT_.Alive();
    }

    void EnsureDirector(float dt) {
        if (mapDirector_.Alive()) return;
        findDirectorCd_ -= dt;
        if (findDirectorCd_ > 0) return;
        findDirectorCd_ = kFindInterval;
        Il2CppObject* root = ugui::FindFirst(sym_.mapUIRoot, true);
        if (ugui::IsA(root, sym_.mapUIRoot)) mapUIRoot_.Reset(root);
        Il2CppObject* d = root ? F<Il2CppObject*>(root, sym_.mapUIRootDirector) : nullptr;
        if (ugui::IsA(d, sym_.mapDirector)) {
            mapDirector_.Reset(d);
            SR2_INFO("minimap: MapDirector '{}' (from MapUIRoot)", unity::GetName(d));
        }
    }

    bool PreloadSupported() const {
        return sym_.udmInstance && sym_.udmRequestPreload && sym_.uiDisplayManager && game::Has(sym_.mapUIRootDisplay);
    }
    // The preload was requested and is still loading (asset scans would find nothing yet).
    bool PreloadPending() const { return preloadEntry_ && !preloadDone_ && time_ - preloadT0_ < kPreloadWait; }

    // Keeps the full map's UI scene loaded: UIDisplayManager.RequestPreloadDisplay(MapUIRoot._mapDisplay,
    // requester) - the game's own preload path (UIDisplayPreloadOnDisplayOpenClose uses it), which adds a
    // requester to the display's UIDisplaySceneLoadEntry. While any requester holds the entry, the scene
    // (UIDisplaySceneTag -> MapUI prefab -> MapPrefabMapping -> page prefabs) stays loaded, so the page is
    // available without opening the map and is not unloaded when the map closes. Re-asserted every
    // kPreloadInterval s (idempotent: an existing requester is not added twice).
    void EnsurePreload(float sdt) {
        if (!PreloadSupported()) return;
        if (preloadEntry_ && !preloadDone_) CheckPreloadDone();
        preloadCd_ -= sdt;
        if (preloadCd_ > 0) return;
        preloadCd_ = kPreloadInterval;
        if (mapDisplayName_.empty()) {
            if (!mapUIRoot_.Alive()) {  // scene lookup at most every kPreloadInterval s, only until found
                Il2CppObject* root = ugui::FindFirst(sym_.mapUIRoot, true);
                if (ugui::IsA(root, sym_.mapUIRoot)) mapUIRoot_.Reset(root);
                if (!mapUIRoot_.Alive()) return;
            }
            Il2CppObject* s = F<Il2CppObject*>(mapUIRoot_.get(), sym_.mapUIRootDisplay);  // struct{string _name}
            if (ugui::PlausiblePointer(s) && unity::ClassName(s) == "System.String")
                mapDisplayName_ = unity::ToUtf8(reinterpret_cast<Il2CppString*>(s));
            if (mapDisplayName_.empty()) {
                if (!preloadFailLogged_) SR2_WARN("minimap: MapUIRoot._mapDisplay has no display name - no map UI preload");
                preloadFailLogged_ = true;
                return;
            }
        }
        Il2CppObject* udm = nullptr;
        Managed([&] { udm = ugui::Call<Il2CppObject*>(sym_.udmInstance); });
        if (!ugui::IsA(udm, sym_.uiDisplayManager)) return;
        Il2CppObject* entry = nullptr;
        Managed([&] {
            entry = ugui::Call<Il2CppObject*>(sym_.udmRequestPreload, udm, unity::NewString(mapDisplayName_),
                                              unity::NewString(kPreloadRequester));
        });
        if (!entry) {
            if (!preloadFailLogged_)
                SR2_WARN("minimap: map display '{}' has no UI scene to preload (or the UI layers are not set up yet) - "
                         "retrying every {:.0f} s; the page then comes from the full map / asset scan",
                         mapDisplayName_, kPreloadInterval);
            preloadFailLogged_ = true;
            return;
        }
        if (entry == preloadEntry_.get()) return;
        preloadEntry_.Reset(entry);
        preloadDone_ = false;
        preloadT0_ = time_;
        preloadWaitLogged_ = false;
        SR2_INFO("minimap: map UI preload requested for display '{}' ({}) - the page no longer needs the full map",
                 mapDisplayName_, unity::ClassName(entry));
        CheckPreloadDone();
    }

    void CheckPreloadDone() {
        Il2CppObject* e = preloadEntry_.get();
        bool done = false;
        if (sym_.entryIsDone && ugui::IsA(e, sym_.sceneLoadEntry))
            Managed([&] { done = ugui::Call<bool>(sym_.entryIsDone, e); });
        else
            done = true;  // unknown handle type: do not wait for it
        if (!done) {
            if (!preloadWaitLogged_ && time_ - preloadT0_ >= kPreloadWait) {
                preloadWaitLogged_ = true;
                SR2_WARN("minimap: map UI scene still loading after {:.0f} s - asset scans resume", kPreloadWait);
            }
            return;
        }
        preloadDone_ = true;
        SR2_INFO("minimap: map UI scene loaded ({:.1f} s after the request){}", time_ - preloadT0_,
                 view_.ok ? "" : " - building the page");
        if (!view_.ok) {  // look for the page now (one asset scan) instead of waiting for the retry timers
            mappingScanCd_ = 0;
            mappingScanMisses_ = 0;
            buildCd_ = 0;
        }
    }

    bool FullMapOpen() {
        if (!mapUI_.Alive()) return false;
        if (!unity::ActiveInHierarchy(mapUI_.get()) || !ugui::BehaviourEnabled(mapUI_.get())) return false;
        // ASSUMED: the open map frees the cursor or pauses (guards against a
        // display that hides MapUI without deactivating it).
        return ugui::CursorLockState() < 0 || MenusOpen();
    }

    bool MenusOpen() {
        int lock = ugui::CursorLockState();
        float ts = ugui::TimeScale();
        return (lock >= 0 && lock != 1 /* Locked */) || ts == 0.0f;
    }

    bool ShouldHide() {
        if (cfg_.hideFullMap && mapOpen_) return true;
        // The overlay's HUD edit mode unlocks the cursor too, but the minimap is what is being placed.
        if (cfg_.hideMenus && MenusOpen() && !sr2::ui::hud_edit::Editing(sr2::ui::hud_edit::Element::Minimap))
            return true;
        if (cfg_.hideConservatory && inConservatory_) return true;
        if (hud_.Alive() && !unity::ActiveInHierarchy(hud_.get())) return true;
        if (cfg_.respectFog && unknownArea_) return true;  // the game hides the player's position here
        return false;
    }

    void HandleHotkey() {
        int vk = cfg_.zoomKey;
        if (vk <= 0 || vk > 255) return;
        if (sr2::ui::overlay::IsOpen()) {  // typing in the overlay (profile names etc.) must not zoom
            keyDown_ = true;               // a key held while the overlay closes does not fire either
            return;
        }
        bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
        bool edge = down && !keyDown_;
        keyDown_ = down;
        if (!edge) return;
        DWORD pid = 0;
        GetWindowThreadProcessId(GetForegroundWindow(), &pid);
        if (pid != GetCurrentProcessId() || hidden_) return;
        float next = cfg_.zoom * 2.0f;
        if (next > 4.0f + 1e-3f) next = 0.5f;
        settings::Set(kModule, "zoom", next);  // persists + notifies (cfgDirty_)
    }

    // ------------------------------------------------------------ sprites ----
    void EnsureSprites() {
        if (spritesTried_) {
            if (sprDisc_ && !sprDisc_.Alive()) spritesTried_ = false;  // unloaded on a scene change
            else return;
        }
        spritesTried_ = true;
        if (!ugui::HasSpriteGen()) return;
        // High resolution + mip chain + trilinear: smooth, antialiased edges at any minimap size (the
        // frame and mask used to be 256 / 64 px and were magnified on large screens).
        constexpr int kDiscPx = 512;
        sprDisc_.Reset(ugui::MakeSprite(procgen::Disc(kDiscPx), kDiscPx, kDiscPx, {}, "SR2Mods.Minimap.Disc", true));
        constexpr int kRs = 128;
        constexpr float kRadius = 44, kBorder = kRoundBorder;
        sprRound_.Reset(ugui::MakeSprite(procgen::RoundSquare(kRs, kRadius), kRs, kRs, {kBorder, kBorder, kBorder, kBorder},
                                         "SR2Mods.Minimap.RoundSquare", true));
        sprCircleFrame_.Reset(ugui::MakeSprite(procgen::CircleFrame(kDiscPx, 20), kDiscPx, kDiscPx, {},
                                               "SR2Mods.Minimap.CircleFrame", true));
        sprRoundFrame_.Reset(ugui::MakeSprite(procgen::RoundSquareFrame(kRs, kRadius, 12), kRs, kRs,
                                              {kBorder, kBorder, kBorder, kBorder}, "SR2Mods.Minimap.RoundFrame", true));
        sprArrow_.Reset(ugui::MakeSprite(procgen::Arrow(128), 128, 128, {}, "SR2Mods.Minimap.Arrow", true));
        sprDot_.Reset(ugui::MakeSprite(procgen::Dot(64), 64, 64, {}, "SR2Mods.Minimap.Dot", true));
        sprWall_.Reset(ugui::MakeSprite(procgen::CrackedWall(64), 64, 64, {}, "SR2Mods.Minimap.NullifierWall", true));
        sprDronePort_.Reset(ugui::MakeSprite(procgen::DronePort(64), 64, 64, {}, "SR2Mods.Minimap.DronePort", true));
        sprGadget_.Reset(ugui::MakeSprite(procgen::Gadget(64), 64, 64, {}, "SR2Mods.Minimap.Gadget", true));
        if (!sprDisc_) SR2_WARN("minimap: procedural sprites could not be created");
    }

    void FindFrameOverride() {
        if (frameOverrideTried_) return;
        frameOverrideTried_ = true;
        if (frameSpriteName_.empty() && !logSprites_) return;
        // opt-in only (sr2mods.json): one asset scan
        std::vector<std::string> candidates;
        ugui::ForEachOfType(ugui::SpriteClass(), [&](Il2CppObject* sp) {
            std::string n = unity::GetName(sp);
            if (!frameSpriteName_.empty() && n == frameSpriteName_ && !sprFrameOverride_) sprFrameOverride_.Reset(sp);
            if (logSprites_) {
                std::string l = Lower(n);
                if (Contains(l, "circle") || Contains(l, "ring") || Contains(l, "round") || Contains(l, "frame") ||
                    Contains(l, "border") || Contains(l, "outline"))
                    candidates.push_back(n);
            }
            return true;
        });
        if (!frameSpriteName_.empty())
            SR2_INFO("minimap: frame sprite '{}' {}", frameSpriteName_, sprFrameOverride_ ? "found" : "NOT found");
        if (logSprites_) {
            std::string all;
            for (const auto& c : candidates) all += c + ", ";
            SR2_INFO("minimap: frame sprite candidates ({}): {}", candidates.size(), all);
        }
    }

    // ------------------------------------------------------------------ UI ---
    bool EnsureUi(float dt) {
        if (ui_.built && ui_.root.Alive() && ui_.rotator.Alive()) return true;
        if (ui_.built) {
            SR2_DEBUG("minimap: UI destroyed externally (scene change) - rebuilding");
            DestroyView();
            if (ui_.ownCanvas.Alive()) ugui::Destroy(ui_.ownCanvas.get());
            ui_ = Ui{};
        }
        Il2CppObject* parentT = nullptr;
        int layer = -1;
        if (sym_.hudUI) {
            if (!hud_.Alive()) {
                findHudCd_ -= dt;
                if (findHudCd_ <= 0) {
                    findHudCd_ = kFindInterval;  // fallback only (normally captured by HudUI.Awake)
                    // HudUI deactivates its own GameObject while hidden, so include inactive objects.
                    hud_.Reset(ugui::FindFirst(sym_.hudUI, true));
                }
            }
            if (hud_.Alive()) parentT = HudParent(layer);
        }
        if (!parentT) {
            hudWait_ += dt;
            if (hudWait_ < kHudWait) return false;
            if (!ui_.ownCanvas.Alive()) {
                Il2CppObject* go = ugui::NewGameObject("SR2Mods.MinimapCanvas");
                Il2CppObject* canvas = ugui::AddCanvas(go);
                ugui::CanvasSetOverlay(canvas, 60);
                ui_.ownCanvas.Reset(go);
                SR2_WARN("minimap: HudUI canvas not found - using an own overlay canvas");
            }
            parentT = unity::TransformOf(ui_.ownCanvas.get());
            layer = 5;  // UI
        }
        if (!parentT) return false;
        SR2_PERF_DETAIL("minimap.ui build (once)");
        BuildUi(parentT, layer);
        return ui_.built;
    }

    // HudUI.Get/SetSortOrder use a Canvas on the HudUI GameObject itself; parent
    // there so the minimap shows/hides with the HUD (HudUI.UpdateHudVisibility
    // toggles that GameObject). Otherwise the root canvas above it.
    Il2CppObject* HudParent(int& layer) {
        Il2CppObject* hudGo = unity::GameObjectOf(hud_.get());
        Il2CppObject* target = unity::GetComponent(hudGo, ugui::CanvasClass()) ? hudGo : nullptr;
        if (!target) {
            Il2CppObject* parentCanvas = ugui::ComponentInParent(hudGo, ugui::CanvasClass());
            if (parentCanvas) target = unity::GameObjectOf(ugui::CanvasRoot(parentCanvas));
        }
        if (!target) return nullptr;
        Il2CppObject* t = unity::TransformOf(target);
        if (!ugui::IsRectTransform(t)) return nullptr;
        layer = ugui::GetLayer(target);
        return t;
    }

    Il2CppObject* Child(Il2CppObject* parentT, const char* name, int layer) {
        Il2CppObject* go = ugui::NewGameObject(name);
        if (!go) return nullptr;
        ugui::SetLayer(go, layer);
        ugui::SetParent(unity::TransformOf(go), parentT, false);
        return go;
    }

    Il2CppObject* AddImage(Il2CppObject* go) {
        Il2CppObject* img = ugui::AddComponent(go, ugui::ImageClass());
        ugui::GraphicSetRaycastTarget(img, false);
        return img;
    }

    void BuildUi(Il2CppObject* parentT, int layer) {
        Step("ui: sprites");
        {
            SR2_PERF_DETAIL("minimap.ui sprites (procedural, once)");
            EnsureSprites();
        }
        FindFrameOverride();
        Step("ui: root");
        ui_.parentT.Reset(parentT);
        Il2CppObject* root = Child(parentT, "SR2Mods.Minimap", layer);
        if (!root) return;
        ui_.root.Reset(root);
        ui_.rootT.Reset(unity::TransformOf(root));
        ugui::AddCanvas(root);  // nested canvas: our per-frame moves only rebatch this canvas
        ui_.group.Reset(ugui::AddCanvasGroup(root));
        ugui::CanvasGroupSetBlocksRaycasts(ui_.group.get(), false);
        ugui::SetAsLastSibling(ui_.rootT.get());

        Step("ui: mask");
        Il2CppObject* mask = Child(ui_.rootT.get(), "Mask", layer);
        ugui::Fill(unity::TransformOf(mask));
        ui_.maskImg.Reset(AddImage(mask));
        ugui::MaskSetShowGraphic(ugui::AddMask(mask), true);
        ugui::GraphicSetColor(ui_.maskImg.get(), {0.10f, 0.22f, 0.27f, 1});

        Step("ui: background");
        Il2CppObject* bg = Child(unity::TransformOf(mask), "Background", layer);
        ugui::Fill(unity::TransformOf(bg));
        ui_.bg.Reset(AddImage(bg));
        unity::SetActive(bg, false);
        Il2CppObject* ov = Child(unity::TransformOf(mask), "BackgroundOverlay", layer);
        ugui::Fill(unity::TransformOf(ov));
        ui_.overlay.Reset(AddImage(ov));
        unity::SetActive(ov, false);

        Step("ui: rotator");
        Il2CppObject* rot = Child(unity::TransformOf(mask), "Rotator", layer);
        ui_.rotator.Reset(unity::TransformOf(rot));
        ugui::Center(ui_.rotator.get(), {0, 0});

        Step("ui: arrow");
        Il2CppObject* holder = Child(ui_.rootT.get(), "Arrow", layer);
        ui_.arrowHolder.Reset(unity::TransformOf(holder));
        ugui::Center(ui_.arrowHolder.get(), {0, 0});
        Il2CppObject* arrow = Child(ui_.arrowHolder.get(), "PlayerArrow", layer);
        ui_.arrowT.Reset(unity::TransformOf(arrow));
        ui_.arrowImg.Reset(AddImage(arrow));
        ugui::ImageSetSprite(ui_.arrowImg.get(), sprArrow_.get());
        ugui::ImageSetPreserveAspect(ui_.arrowImg.get(), true);
        ugui::Center(ui_.arrowT.get(), {64, 64});

        Step("ui: frame");
        Il2CppObject* frame = Child(ui_.rootT.get(), "Frame", layer);
        ugui::Fill(unity::TransformOf(frame));
        ui_.frame.Reset(AddImage(frame));

        if (ugui::HasTmp()) {
            Step("ui: level label");
            Il2CppObject* lbl = Child(ui_.rootT.get(), "Level", layer);
            unity::SetActive(lbl, false);
            Il2CppObject* t = nullptr;
            if (Managed([&] { t = ugui::AddTmpText(lbl); }) && t) {
                ugui::GraphicSetRaycastTarget(t, false);
                Il2CppObject* src = (hud_.Alive() && game::Has(sym_.hudDebugText))
                                        ? F<Il2CppObject*>(hud_.get(), sym_.hudDebugText) : nullptr;
                if (ugui::IsA(src, ugui::TmpTextClass())) ugui::TmpCopyFont(t, src);
                ugui::TmpSetCenter(t);
                ugui::TmpSetColor(t, {0.98f, 0.94f, 0.82f, 1});
                ui_.label.Reset(t);
            }
        }

        ui_.built = true;
        ui_.visible = true;
        Step("ui: layout");
        ApplyLayout();
        SR2_INFO("minimap: UI built under '{}'", unity::GetName(unity::GameObjectOf(parentT)));
    }

    // The placement to show: the HUD edit mode's live one while the minimap is
    // being moved (and until the commit has landed), else the settings.
    sr2::ui::hud_edit::Placement EffectivePlacement() const {
        sr2::ui::hud_edit::Placement p{cfg_.posX, cfg_.posY, cfg_.size};
        sr2::ui::hud_edit::Preview(sr2::ui::hud_edit::Element::Minimap, p);
        return p;
    }

    // Old placement (corner, offsets in % of the height) -> % of the free space,
    // for a canvas of w x h. Writes the new settings once.
    void MigratePlacement(float w, float h) {
        if (w <= 1 || h <= 1) return;  // canvas not laid out yet: at the next layout
        migratePlacement_ = false;
        const int corner = settings::Saved(kModule, "position") ? std::clamp(settings::GetInt(kModule, "position"), 0, 3) : 1;
        const float ox = (settings::Saved(kModule, "offset_x") ? settings::GetFloat(kModule, "offset_x") : 2.0f) * h / 100.0f;
        const float oy = (settings::Saved(kModule, "offset_y") ? settings::GetFloat(kModule, "offset_y") : 2.0f) * h / 100.0f;
        const float s = std::max(32.0f, h * cfg_.size / 100.0f);
        const bool right = corner == 1 || corner == 3, top = corner < 2;
        const float x = sr2::ui::hud_edit::ToPct(right ? w - s - ox : ox, w, s);
        const float y = sr2::ui::hud_edit::ToPct(top ? oy : h - s - oy, h, s);
        settings::Set(kModule, "pos_x", x);
        settings::Set(kModule, "pos_y", y);
        cfg_.posX = std::clamp(settings::GetFloat(kModule, "pos_x"), 0.0f, 100.0f);
        cfg_.posY = std::clamp(settings::GetFloat(kModule, "pos_y"), 0.0f, 100.0f);
        SR2_INFO("minimap: placement migrated (corner {}, offsets {:.1f}/{:.1f} px on {:.0f}x{:.0f}) -> {:.1f}% / {:.1f}%",
                 corner, ox, oy, w, h, cfg_.posX, cfg_.posY);
    }

    void ApplyLayout() {
        if (!ui_.built || !ui_.rootT.Alive()) return;
        const ugui::Rect canvas = ugui::GetRect(ui_.parentT.get());
        float canvasH = canvas.h;
        if (canvasH <= 1) canvasH = static_cast<float>(std::max(ugui::ScreenHeight(), 720));
        if (migratePlacement_) MigratePlacement(canvas.w, canvas.h);
        const sr2::ui::hud_edit::Placement place = EffectivePlacement();
        shown_ = place;
        float size = std::max(32.0f, canvasH * place.size / 100.0f);
        ui_.size = size;
        // Anchor and pivot at the same fraction of the parent and of the minimap: the
        // left edge is then x% of (parent width - size), i.e. exactly the free-space percent.
        const Vec2 a{place.x / 100.0f, 1.0f - place.y / 100.0f};
        Il2CppObject* rt = ui_.rootT.get();
        ugui::SetAnchors(rt, a, a);
        ugui::SetPivot(rt, a);
        ugui::SetSizeDelta(rt, {size, size});
        ugui::SetAnchoredPosition(rt, {0, 0});
        ugui::SetLocalScale(rt, {1, 1, 1});
        ugui::CanvasGroupSetAlpha(ui_.group.get(), cfg_.opacity);

        bool round = cfg_.shape == 1;
        ugui::ImageSetSprite(ui_.maskImg.get(), round ? sprRound_.get() : sprDisc_.get());
        ugui::ImageSetType(ui_.maskImg.get(), round ? 1 : 0);
        float ppuMul = kRoundBorder / std::max(1.0f, size * 0.16f);  // rendered 9-slice border = 16% of the size
        if (round) ugui::ImageSetPpuMultiplier(ui_.maskImg.get(), ppuMul);

        if (sprFrameOverride_.Alive()) {
            ugui::ImageSetSprite(ui_.frame.get(), sprFrameOverride_.get());
            ugui::Vec4 b = ugui::SpriteBorder(sprFrameOverride_.get());
            ugui::ImageSetType(ui_.frame.get(), (b.x + b.y + b.z + b.w) > 0 ? 1 : 0);
        } else {
            ugui::ImageSetSprite(ui_.frame.get(), round ? sprRoundFrame_.get() : sprCircleFrame_.get());
            ugui::ImageSetType(ui_.frame.get(), round ? 1 : 0);
            if (round) ugui::ImageSetPpuMultiplier(ui_.frame.get(), ppuMul);
        }
        unity::SetActive(unity::GameObjectOf(ui_.frame.get()), ugui::ImageGetSprite(ui_.frame.get()) != nullptr);

        ApplyArrowStyle();
        // Mask inset: the stencil edge (any mask pixel with alpha > 0) must lie
        // inside the frame's opaque band, or map pixels bleed through the
        // frame's anti-aliased outer edge (white fringe on a bright map).
        // Both frames are ~3.8% of the size thick with an opaque cream band from ~0.9% to ~3%: the
        // (binary, stencil) mask edge sits in the middle of that band, so its stair-steps are never visible.
        if (Il2CppObject* maskT = unity::TransformOf(ui_.maskImg.get())) {
            float inset = std::max(2.0f, std::round(size * 0.0195f));
            ugui::SetSizeDelta(maskT, {-2 * inset, -2 * inset});
        }

        if (ui_.label.Alive()) {
            Il2CppObject* lt = unity::TransformOf(ui_.label.get());
            ugui::SetAnchors(lt, {0.5f, 0}, {0.5f, 0});
            ugui::SetPivot(lt, {0.5f, 0});
            ugui::SetSizeDelta(lt, {size * 0.7f, size * 0.14f});
            ugui::SetAnchoredPosition(lt, {0, size * 0.07f});
            ugui::TmpSetFontSize(ui_.label.get(), std::max(10.0f, size * 0.075f));
        }
        // icon sizes
        float iconPx = IconPx();
        for (Marker& m : view_.markers)
            if (m.t.Alive()) ugui::SetSizeDelta(m.t.get(), {iconPx, iconPx});
        for (Icon& i : view_.icons)
            if (i.t.Alive()) ugui::SetSizeDelta(i.t.get(), {iconPx, iconPx});
        lastS_ = -1;
        negDirty_ = true;  // the minimap moved / resized: the outside-fog mask moved with it
        lastAlt_ = 1 << 30;
    }

    float IconPx() const { return ui_.size * 0.085f; }

    // ------------------------------------------------------------ zone / map ---
    void UpdateZone(Il2CppObject* z) {
        bool changed = z != zone_.get();
        if (!changed && wantedMap_) return;
        if (!changed && !mapDirector_.Alive()) return;  // waiting for the director
        if (changed) {
            zone_.Reset(z);
            zoneName_ = unity::GetName(z);
            std::string l = Lower(zoneName_);
            inConservatory_ = Contains(l, "conservatory") || Contains(l, "ranch");
            inLabyrinth_ = Contains(l, "labyrinth");
            lastAlt_ = 1 << 30;
            // Zone change: cached reveal answers are dropped and every area is queried again this tick
            // (UpdateFog runs right after; areas stay fogged if the save data is not ready).
            for (Group& f : view_.fog) f.state = -1;
            fogFullDue_ = true;
            fogAcc_ = kFogInterval;
        }
        if (!mapDirector_.Alive()) return;
        Il2CppObject* mapDef = nullptr;
        Managed([&] { mapDef = ugui::Call<Il2CppObject*>(sym_.getMapForZone, mapDirector_.get(), z); });
        if (!ugui::IsA(mapDef, sym_.mapDefinition)) mapDef = nullptr;
        if (changed || mapDef)
            SR2_INFO("minimap: zone -> '{}' (map '{}'{}{})", zoneName_, mapDef ? unity::GetName(mapDef) : "<none>",
                     inConservatory_ ? ", conservatory" : "", inLabyrinth_ ? ", labyrinth" : "");
        if (mapDef != wantedMap_.get()) {
            wantedMap_.Reset(mapDef);
            buildCd_ = 0;
            lastBuildError_.clear();
        pagePendingSince_ = -1;
        }
    }

    // Page prefab for mapDef from one MapPrefabMapping: the game's own
    // GetPrefabFor, then the serialized _entries list (in case the runtime
    // dictionary is stale). Only a GameObject with a Map component counts.
    Il2CppObject* PrefabFrom(Il2CppObject* mapping, Il2CppObject* mapDef) {
        if (!ugui::IsA(mapping, sym_.mapPrefabMapping) || !ugui::IsA(mapDef, sym_.mapDefinition)) return nullptr;
        auto valid = [&](Il2CppObject* p) {
            return ugui::IsA(p, unity::GameObjectClass()) && ugui::IsA(unity::GetComponent(p, sym_.map), sym_.map);
        };
        Il2CppObject* p = nullptr;
        Managed([&] { p = ugui::Call<Il2CppObject*>(sym_.getPrefabFor, mapping, mapDef); });
        if (valid(p)) return p;
        p = nullptr;
        if (game::Has(sym_.mpmEntries)) {
            Il2CppObject* list = F<Il2CppObject*>(mapping, sym_.mpmEntries);
            if (ugui::PlausiblePointer(list))
                ugui::ForEachListStructPair(list, "Map", "Prefab", [&](Il2CppObject* m, Il2CppObject* pf) {
                    if (m != mapDef) return true;
                    p = pf;
                    return false;
                });
        }
        return valid(p) ? p : nullptr;
    }

    // Sources, in order: the mapping we already have (MapUI.Awake or an earlier
    // scan); MapDirector._viewModel._prefabMapping; every loaded
    // MapPrefabMapping asset (Resources.FindObjectsOfTypeAll, rate limited),
    // keeping the first one that actually has a page for this map.
    Il2CppObject* FindPagePrefab(Il2CppObject* mapDef) {
        if (Il2CppObject* p = PrefabFrom(prefabMapping_.get(), mapDef)) return p;
        if (mapDirector_.Alive() && game::Has(sym_.mdViewModel) && game::Has(sym_.vmPrefabMapping) && sym_.mapViewModel) {
            Il2CppObject* vm = F<Il2CppObject*>(mapDirector_.get(), sym_.mdViewModel);
            if (ugui::PlausiblePointer(vm) && ugui::IsA(vm, sym_.mapViewModel)) {
                Il2CppObject* m = F<Il2CppObject*>(vm, sym_.vmPrefabMapping);
                if (ugui::PlausiblePointer(m))
                    if (Il2CppObject* p = PrefabFrom(m, mapDef)) {
                        prefabMapping_.Reset(m);
                        SR2_INFO("minimap: map prefab mapping '{}' from MapDirector's view model", unity::GetName(m));
                        return p;
                    }
            }
        }
        if (PreloadPending()) return nullptr;  // the map UI scene is loading: scan once it is done
        if (mappingScanCd_ > 0) return nullptr;
        LARGE_INTEGER t0, t1, f;
        QueryPerformanceCounter(&t0);
        int loaded = 0;
        std::string names;
        Il2CppObject* hit = nullptr;
        Il2CppObject* page = nullptr;
        ugui::ForEachOfType(sym_.mapPrefabMapping, [&](Il2CppObject* m) {
            if (!ugui::IsA(m, sym_.mapPrefabMapping)) return true;
            ++loaded;
            if (names.size() < 200) names += (names.empty() ? "'" : ", '") + unity::GetName(m) + "'";
            if (Il2CppObject* p = PrefabFrom(m, mapDef)) {
                hit = m;
                page = p;
                return false;
            }
            return true;
        });
        QueryPerformanceCounter(&t1);
        QueryPerformanceFrequency(&f);
        double ms = 1000.0 * static_cast<double>(t1.QuadPart - t0.QuadPart) / static_cast<double>(f.QuadPart);
        if (hit) {
            prefabMapping_.Reset(hit);
            mappingScanMisses_ = 0;
            mappingScanCd_ = 0;
            SR2_INFO("minimap: MapPrefabMapping asset scan: {} loaded, '{}' has page '{}' for map '{}' ({:.1f} ms)",
                     loaded, unity::GetName(hit), unity::GetName(page), unity::GetName(mapDef), ms);
            return page;
        }
        ++mappingScanMisses_;
        mappingScanCd_ = mappingScanMisses_ < kMappingScanFastTries ? kMappingScanRetry : kMappingScanBackoff;
        SR2_INFO("minimap: MapPrefabMapping asset scan: {} loaded ({}), none has a page for map '{}' ({:.1f} ms; next "
                 "scan in {:.0f} s)",
                 loaded, loaded ? names : std::string("-"), unity::GetName(mapDef), ms, mappingScanCd_);
        return nullptr;
    }

    void BuildFail(std::string why, float retry = kBuildRetry) {
        if (why != lastBuildError_) SR2_ERROR("minimap: cannot show map '{}': {}", unity::GetName(wantedMap_.get()), why);
        lastBuildError_ = std::move(why);
        buildCd_ = retry;
        DestroyView();
    }

    // No page prefab yet: normal on every load (the map UI scene loads after the world), so INFO;
    // ERROR once only if the map still has no page after kPagePendingError seconds.
    void BuildPending(const char* reason) {
        if (pagePendingSince_ < 0) {
            pagePendingSince_ = time_;
            pagePendingReported_ = false;
        }
        std::string why = std::format("no map page prefab yet ({})", reason);
        const float waited = time_ - pagePendingSince_;
        if (waited >= kPagePendingError && !pagePendingReported_) {
            pagePendingReported_ = true;
            SR2_ERROR("minimap: cannot show map '{}': {} - still none after {:.0f} s (also taken from the full map "
                      "when it opens)",
                      unity::GetName(wantedMap_.get()), why, waited);
        } else if (why != lastBuildError_) {
            SR2_INFO("minimap: map '{}': {} - retrying every second", unity::GetName(wantedMap_.get()), why);
        }
        lastBuildError_ = std::move(why);
        buildCd_ = 1.0f;
        DestroyView();
    }

    // Phase 0 of the page copy (one frame): read the prefab's structure.
    void StartBuild() {
        DestroyView();
        Il2CppObject* mapDef = wantedMap_.get();
        if (!ugui::IsA(mapDef, sym_.mapDefinition)) return;
        Il2CppObject* prefab = FindPagePrefab(mapDef);
        if (!prefab) {
            const char* why = !PreloadSupported() ? "map UI preload unavailable"
                              : !preloadEntry_    ? "map UI preload not requested yet"
                              : PreloadPending()  ? "map UI scene still loading"
                                                  : "map UI scene loaded but no MapPrefabMapping has this map";
            return BuildPending(why);  // re-checked every second (asset scans are rate limited separately)
        }
        pagePendingSince_ = -1;
        Il2CppObject* mapComp = unity::GetComponent(prefab, sym_.map);
        if (!ugui::IsA(mapComp, sym_.map)) return BuildFail("map page prefab has no Map component on its root");
        Il2CppObject* mapT = unity::TransformOf(mapComp);
        if (!ugui::IsRectTransform(mapT)) return BuildFail("map page root has no RectTransform");
        view_.mapDef.Reset(mapDef);
        view_.prefab.Reset(prefab);
        view_.mapComp.Reset(mapComp);
        view_.mapT.Reset(mapT);
        if (!ReadProjection(mapComp)) return BuildFail("world->map projection unavailable");
        view_.clampRect = ugui::GetRect(mapT);
        view_.clamp = view_.clampRect.w > 1 && view_.clampRect.h > 1;
        view_.unitsPerMeter = 0.5f * (std::fabs(view_.scale.x) + std::fabs(view_.scale.y));
        if (!(view_.unitsPerMeter > 1e-6f)) return BuildFail("degenerate projection scale");
        SR2_INFO("minimap: page '{}' rect ({:.0f},{:.0f} {:.0f}x{:.0f}) scale ({:.5f},{:.5f}) offset ({:.1f},{:.1f})",
                 unity::GetName(prefab), view_.clampRect.x, view_.clampRect.y, view_.clampRect.w, view_.clampRect.h,
                 view_.scale.x, view_.scale.y, view_.offset.x, view_.offset.y);

        // our page container: local space == the prefab page's local space
        int layer = ugui::GetLayer(ui_.root.get());
        Il2CppObject* page = Child(ui_.rotator.get(), "Page", layer);
        if (!page) return BuildFail("could not create the page container");
        view_.page.Reset(unity::TransformOf(page));
        ugui::Center(view_.page.get(), {0, 0});

        Build& b = build_;
        b = Build{};
        b.rootT.Reset(unity::TransformOf(prefab));
        if (game::Has(sym_.mapForceActive)) {
            // Map.Awake: foreach (go in _forceActiveOnAwake) go.SetActive(true) (verified by disassembly).
            // Containers the designers left disabled in the prefab are active in game - so are our copies.
            auto* arr = F<Il2CppArray*>(mapComp, sym_.mapForceActive);
            if (ugui::PlausiblePointer(arr)) {
                Ref pin(reinterpret_cast<Il2CppObject*>(arr));
                for (size_t i = 0, n = std::min<size_t>(ugui::ArrayCount(arr), 256); i < n; ++i) {
                    Il2CppObject* go = ugui::ArrayItem(arr, i);
                    if (!ugui::IsA(go, unity::GameObjectClass())) continue;
                    b.pins.emplace_back(go);
                    b.forceActive[go] = true;
                }
            }
        }
        LoadGameMarker();  // before the copy: page art showing a player marker sprite is skipped
        for (const MarkerPartDef& p : gm_.parts) b.markerSprites[p.sprite.get()] = true;
        if (game::Has(sym_.mapZoneMarkers)) {
            Il2CppObject* zt = F<Il2CppObject*>(mapComp, sym_.mapZoneMarkers);
            if (ugui::IsA(zt, ugui::TransformClass())) AddRoot(zt, kRootZone, 0);
        }
        if (sym_.filterable) {
            ugui::ForEachInChildren(prefab, sym_.filterable, true, [&](Il2CppObject* c) {
                Marker m;
                m.comp.Reset(c);
                view_.markers.push_back(std::move(m));
                AddRoot(unity::TransformOf(c), kRootMarker, static_cast<int>(view_.markers.size() - 1));
                return true;
            });
        }
        if (sym_.mapMarkerBase) {  // player / navigation marker art etc.: never copied as page art
            ugui::ForEachInChildren(prefab, sym_.mapMarkerBase, true, [&](Il2CppObject* c) {
                AddRoot(unity::TransformOf(c), kRootSkip, 0);  // filterable markers keep their own root
                return true;
            });
        }
        if (sym_.fogElement) {
            ugui::ForEachInChildren(prefab, sym_.fogElement, true, [&](Il2CppObject* c) {
                Group g;
                g.comp.Reset(c);
                g.name = FogName(c);
                view_.fog.push_back(std::move(g));
                AddRoot(unity::TransformOf(c), kRootFog, static_cast<int>(view_.fog.size() - 1));
                return true;
            });
        }
        SetupNegativeMask(prefab);
        if (sym_.dynamicElement) {
            ugui::ForEachInChildren(prefab, sym_.dynamicElement, true, [&](Il2CppObject* c) {
                Group g;
                g.comp.Reset(c);
                view_.dyn.push_back(std::move(g));
                AddRoot(unity::TransformOf(c), kRootDyn, static_cast<int>(view_.dyn.size() - 1));
                return true;
            });
        }
        ugui::ForEachInChildren(prefab, ugui::CanvasGroupClass(), true, [&](Il2CppObject* c) {
            Il2CppObject* t = unity::TransformOf(c);
            if (t) {
                b.pins.emplace_back(t);
                b.groupAlpha[t] = ugui::CanvasGroupGetAlpha(c);
            }
            return true;
        });
        ugui::ForEachInChildren(prefab, ugui::MaskClass(), true, [&](Il2CppObject* m) {
            // a Mask that hides its own graphic: that Image is a stencil shape, not art (in game it is invisible)
            if (!ugui::MaskShowsGraphic(m))
                if (Il2CppObject* t = unity::TransformOf(m)) {
                    b.pins.emplace_back(t);
                    b.hiddenMaskGraphic[t] = true;
                }
            return true;
        });
        ugui::ForEachInChildren(prefab, ugui::GraphicClass(), true, [&](Il2CppObject* g) {
            b.graphics.emplace_back(g);
            return true;
        });
        b.active = true;
        SR2_DEBUG("minimap: page copy started: {} graphics, {} markers, {} fog, {} dynamic", b.graphics.size(),
                  view_.markers.size(), view_.fog.size(), view_.dyn.size());
    }

    void AddRoot(Il2CppObject* t, int kind, int index) {
        if (!t) return;
        build_.pins.emplace_back(t);
        build_.roots.emplace(t, (kind << 24) | index);
    }

    // Phases 1..n: copy kBuildChunk graphics per frame, then create markers.
    void BuildStep() {
        Build& b = build_;
        if (!view_.page.Alive() || !view_.mapT.Alive()) return BuildFail("page container lost during the build");
        size_t end = std::min(b.graphics.size(), b.next + kBuildChunk);
        for (; b.next < end; ++b.next) CopyGraphic(b.graphics[b.next].get());
        if (b.next < b.graphics.size()) return;
        SR2_PERF_DETAIL("minimap.build finish (markers, first fog)");
        FinishBuild();
    }

    // Material of a page graphic: default UI material, or a custom one. A
    // custom material is kept only if its shader can be stencil-masked (it has
    // _Stencil); otherwise its main texture is shown through the default
    // material (RawImage) or the material is dropped (sprite art).
    const Build::MatInfo& MaterialInfo(Il2CppObject* g) {
        static const Build::MatInfo kDefault{};
        Il2CppObject* mat = ugui::GraphicGetMaterial(g);
        if (!mat) return kDefault;
        auto it = build_.materials.find(mat);
        if (it != build_.materials.end()) return it->second;
        Build::MatInfo info;
        Il2CppObject* def = ugui::GraphicGetDefaultMaterial(g);
        std::string shader = ugui::MaterialShaderName(mat);
        info.custom = mat != def && shader != "UI/Default";
        if (info.custom) {
            info.stencil = ugui::MaterialHasProperty(mat, "_Stencil");
            info.tex.Reset(ugui::MaterialMainTexture(mat));
            info.uv = ugui::MaterialMainUv(mat);
            info.label = std::format("{}({})", unity::GetName(mat), shader);
        }
        build_.pins.emplace_back(mat);
        return build_.materials.emplace(mat, std::move(info)).first->second;
    }

    void CopyGraphic(Il2CppObject* g) {
        Build& b = build_;
        bool isImage = ugui::IsA(g, ugui::ImageClass());
        bool isRaw = !isImage && ugui::RawImageClass() && ugui::IsA(g, ugui::RawImageClass());
        if (!isImage && !isRaw) return;  // texts etc.
        Il2CppObject* t = unity::TransformOf(g);
        if (!ugui::IsRectTransform(t)) return;

        // walk up to the page root: active state, group, CanvasGroup alpha
        int code = 0;
        float alpha = 1;
        bool active = true, forced = false;
        Il2CppObject* cur = t;
        for (int depth = 0; cur && depth < 64; ++depth) {
            Il2CppObject* curGo = unity::GameObjectOf(cur);
            if (!ugui::ActiveSelf(curGo)) {
                if (!b.forceActive.count(curGo)) {
                    active = false;
                    break;
                }
                forced = true;  // inactive in the prefab, activated by Map.Awake
            }
            auto r = b.roots.find(cur);
            int rootKind = r != b.roots.end() ? (r->second >> 24) : 0;
            if (!code && rootKind) code = r->second;
            if (rootKind != kRootFog && rootKind != kRootDyn)  // fog/dynamic alpha is ours to control
                if (auto a = b.groupAlpha.find(cur); a != b.groupAlpha.end()) alpha *= a->second;
            if (cur == b.rootT.get()) break;
            cur = ugui::GetParent(cur);
        }
        int kind = code >> 24, index = code & 0xFFFFFF;
        bool group = kind == kRootFog || kind == kRootDyn;
        auto skip = [&](int& counter) {
            ++counter;
            ++b.skipped;
        };
        if (!active) return skip(b.skipInactive);
        if (kind == kRootZone || kind == kRootSkip) return skip(b.skipMarkerArt);
        Il2CppObject* sprite = isImage ? ugui::ImageGetSprite(g) : nullptr;
        if (!unity::IsAlive(sprite)) sprite = nullptr;
        Color color = ugui::GraphicGetColor(g);
        if (kind == kRootMarker) {  // first sprite of a marker is its icon (fallback)
            Marker& m = view_.markers[static_cast<size_t>(index)];
            if (!m.sprite && sprite) {
                m.sprite.Reset(sprite);
                m.tint = color;
            }
            return;
        }
        if (!ugui::BehaviourEnabled(g)) return skip(b.skipDisabled);  // disabled Image: invisible in game
        if (b.hiddenMaskGraphic.count(t)) return skip(b.skipMaskGraphic);
        if (sprite && b.markerSprites.count(sprite)) return skip(b.skipMarkerArt);
        color.a *= alpha;
        if (color.a <= 0.003f && !group) return skip(b.skipTransparent);
        Il2CppObject* tex = isRaw ? ugui::RawImageGetTexture(g) : nullptr;
        if (isRaw && !unity::IsAlive(tex)) return skip(b.skipOther);

        // material / texture source
        const Build::MatInfo& mi = MaterialInfo(g);
        bool fogShader = mi.custom && Contains(Lower(mi.label), "fogofwar");  // Custom/PosterizedFogOfWar*
        // Outside fog (Custom/PosterizedFogOfWarNegativeMask): its mask is a global matrix set from the
        // FULL map's transforms. Without our own mask it would be stale or wrong on the minimap: skip it.
        bool negMask = fogShader && Contains(Lower(mi.label), "negativemask");
        if (negMask && (!view_.neg.ok || group)) return skip(b.skipNegMask);
        Il2CppObject* useMat = nullptr;  // custom material to assign
        Il2CppObject* matTex = nullptr;  // null-sprite Image drawn as RawImage(material main texture)
        if (mi.custom) {
            if (mi.stencil) {
                useMat = ugui::GraphicGetMaterial(g);
                ++b.matKept;
            } else if (isImage && !sprite && mi.tex.Alive()) {
                matTex = mi.tex.get();
                ++b.matAsTexture;
            } else {
                ++b.matDropped;
            }
        }
        if (isImage && !sprite && !useMat && !matTex && !group) {
            // No sprite and no texture source: a white placeholder / raycast blocker or art the
            // game assigns at runtime. Copying it paints a white rectangle - skip.
            if (b.placeholders.size() < 160)
                b.placeholders += (b.placeholders.empty() ? "'" : ", '") + unity::GetName(unity::GameObjectOf(g)) + "'";
            return skip(b.skipPlaceholder);
        }
        Il2CppObject* artTex = matTex ? matTex : (isRaw ? tex : nullptr);
        if (sprite && ugui::HasSpriteTexture()) {
            artTex = ugui::SpriteTexture(sprite);
            if (!artTex) ++b.noTexture;  // late-bound atlas
        }
        if (artTex && !b.filterSeen.count(artTex)) {
            // Point-filtered map art looks blocky when the minimap magnifies it: use bilinear.
            b.pins.emplace_back(artTex);
            b.filterSeen[artTex] = true;
            if (ugui::TextureGetFilter(artTex) == 0) {
                ugui::TextureSetFilter(artTex, 1);
                ++b.pointFiltered;
            }
        }

        // corners in page space
        Rect r = ugui::GetRect(t);
        Il2CppObject* mapT = view_.mapT.get();
        auto corner = [&](float x, float y) {
            Vec3 w = ugui::TransformPoint(t, {x, y, 0});
            Vec3 l = ugui::InverseTransformPoint(mapT, w);
            return Vec2{l.x, l.y};
        };
        Vec2 c0 = corner(r.x, r.y), c1 = corner(r.x + r.w, r.y), c2 = corner(r.x, r.y + r.h);
        Vec2 ex{c1.x - c0.x, c1.y - c0.y}, ey{c2.x - c0.x, c2.y - c0.y};
        float wdt = std::hypot(ex.x, ex.y), hgt = std::hypot(ey.x, ey.y);
        if (wdt < 0.01f || hgt < 0.01f) return skip(b.skipOther);
        Vec2 center{c0.x + 0.5f * (ex.x + ey.x), c0.y + 0.5f * (ex.y + ey.y)};
        float angle = std::atan2(ex.y, ex.x) * 180.0f / kPi;
        bool mirrored = (ex.x * ey.y - ex.y * ey.x) < 0;

        Il2CppObject* go = Child(view_.page.get(), "Art", ugui::GetLayer(ui_.root.get()));
        if (!go) return;
        Il2CppObject* gt = unity::TransformOf(go);
        if (isImage && !matTex) {
            Il2CppObject* img = AddImage(go);
            ugui::ImageSetSprite(img, sprite);
            int type = ugui::ImageGetType(g);
            if (type != 0 && sprite) {
                ugui::ImageSetType(img, type);
                ugui::ImageSetPpuMultiplier(img, ugui::ImageGetPpuMultiplier(g));
            }
            if (useMat) ugui::GraphicSetMaterial(img, useMat);
            ugui::GraphicSetColor(img, color);
            if (useMat && fogShader) {
                // The fog shaders read the vertex alpha (Graphic colour x every CanvasGroup above) as their
                // reveal threshold, not as opacity: under the root CanvasGroup a minimap opacity < 1 eats
                // holes into the fog. This group stops the inheritance: fog keeps its full shape (opaque).
                if (Il2CppObject* cg = ugui::HasIgnoreParentGroups() ? ugui::AddCanvasGroup(go) : nullptr) {
                    ugui::CanvasGroupSetIgnoreParentGroups(cg, true);
                    ugui::CanvasGroupSetBlocksRaycasts(cg, false);
                    ++b.fogOpaque;
                }
            }
        } else {
            Il2CppObject* ri = ugui::AddComponent(go, ugui::RawImageClass());
            ugui::GraphicSetRaycastTarget(ri, false);
            ugui::RawImageSetTexture(ri, matTex ? matTex : tex);
            ugui::RawImageSetUvRect(ri, matTex ? mi.uv : ugui::RawImageGetUvRect(g));
            ugui::GraphicSetColor(ri, color);
        }
        ugui::Center(gt, {wdt, hgt});
        ugui::SetAnchoredPosition(gt, center);
        ugui::SetLocalEuler(gt, {0, 0, angle});
        if (mirrored) ugui::SetLocalScale(gt, {1, -1, 1});
        ++b.copied;
        if (forced) ++b.forcedActive;
        std::string src = sprite ? "sprite '" + unity::GetName(sprite) + "'"
                                 : std::string(matTex ? "material texture" : (isRaw ? "raw texture" : "no sprite"));
        b.notes.push_back({wdt * hgt, std::format("'{}' {} {:.0f}x{:.0f} rgba({:.2f},{:.2f},{:.2f},{:.2f}){}{}",
                                                  unity::GetName(unity::GameObjectOf(g)), src, wdt, hgt, color.r,
                                                  color.g, color.b, color.a, mi.custom ? " mat " + mi.label : std::string(),
                                                  kind == kRootFog ? " [fog]" : (kind == kRootDyn ? " [dynamic]" : ""))});

        if (kind != kRootFog && fogShader) {
            // fog-shader art outside every FogMapElement: drawn as in the full map (nothing in the game
            // toggles it), hidden only with "Respect fog of war" off
            if (!group) view_.staticFog.gos.emplace_back(go);
            if (negMask) ++view_.neg.copies;
            ++b.staticFog;
            if (b.staticFogNames.size() < 400) {
                Il2CppObject* parent = ugui::GetParent(t);
                b.staticFogNames += std::format("{}'{}'{}", b.staticFogNames.empty() ? "" : ", ",
                                                unity::GetName(unity::GameObjectOf(g)),
                                                parent ? " in '" + unity::GetName(unity::GameObjectOf(parent)) + "'"
                                                       : std::string());
            }
        }
        if (group) {
            Group& grp = (kind == kRootFog ? view_.fog : view_.dyn)[static_cast<size_t>(index)];
            grp.gos.emplace_back(go);
            grp.AddQuad(c0, ex, ey);  // the area's shape for "is this icon under fog" (per image, not one bbox)
        }
    }

    void FinishBuild() {
        Build& b = build_;
        Step("build: player cone");
        BuildCone();  // after the art, before every icon: the icons are drawn over it
        Step("build: markers");
        int perCat[kCatCount] = {};
        std::map<std::string, int> histogram;
        int layer = ugui::GetLayer(ui_.root.get());
        float iconPx = IconPx();
        for (Marker& m : view_.markers) {
            Il2CppObject* comp = m.comp.get();
            if (!m.comp.Alive()) continue;
            std::string label;
            m.cat = Classify(comp, label);
            m.kind = (sym_.gordoMarker && ugui::IsA(comp, sym_.gordoMarker))             ? kMkGordo
                     : (sym_.puzzleMarker && ugui::IsA(comp, sym_.puzzleMarker))         ? kMkPuzzle
                     : (sym_.teleporterMarker && ugui::IsA(comp, sym_.teleporterMarker)) ? kMkTeleporter
                                                                                         : kMkOther;
            ++histogram[label + " -> " + kCatName[m.cat]];
            ++perCat[m.cat];
            // preferred icon: the marker's own Image field
            Il2CppObject* imgField = nullptr;
            if (game::Has(sym_.mapMarkerImage) && ugui::IsA(comp, sym_.mapMarker))
                imgField = F<Il2CppObject*>(comp, sym_.mapMarkerImage);
            else if (game::Has(sym_.gordoIconImage) && ugui::IsA(comp, sym_.gordoMarker))
                imgField = F<Il2CppObject*>(comp, sym_.gordoIconImage);
            if (ugui::IsA(imgField, ugui::ImageClass())) {
                Il2CppObject* sp = ugui::ImageGetSprite(imgField);
                if (unity::IsAlive(sp)) {
                    m.sprite.Reset(sp);
                    m.tint = ugui::GraphicGetColor(imgField);
                }
            }
            if (!m.sprite) continue;
            m.tint.a = std::max(m.tint.a, 0.5f);
            Vec3 lp = ugui::InverseTransformPoint(view_.mapT.get(), ugui::GetPosition(unity::TransformOf(comp)));
            m.pos = {lp.x, lp.y};
            if (sym_.canBeShown) m.canBeShown = ugui::BindFn(ugui::VirtualMethod(comp, sym_.canBeShown));
            Il2CppObject* go = Child(view_.page.get(), "Marker", layer);
            if (!go) continue;
            m.go.Reset(go);
            m.t.Reset(unity::TransformOf(go));
            m.img.Reset(AddImage(go));
            ugui::ImageSetSprite(m.img.get(), m.sprite.get());
            ugui::ImageSetPreserveAspect(m.img.get(), true);
            ugui::GraphicSetColor(m.img.get(), m.tint);
            ugui::Center(m.t.get(), {iconPx, iconPx});
            ugui::SetAnchoredPosition(m.t.get(), m.pos);
            unity::SetActive(go, false);
            m.active = false;
            m.nextCheck = 0;
        }
        view_.hasPagePods = perCat[kTreasurePod] > 0;
        view_.hasPageNodes = perCat[kResource] > 0;
        std::string summary;
        for (const auto& [k, n] : histogram) summary += std::format("{} x{}; ", k, n);
        SR2_INFO("minimap: page copied: {} images ({} skipped: {} inactive, {} disabled, {} hidden mask graphic, {} "
                 "placeholder without sprite, {} marker art, {} transparent, {} other), {} fog, {} dynamic, markers "
                 "(teleporters {}, pods {}, gordos {}, resources {}, other {}): {}",
                 b.copied, b.skipped, b.skipInactive, b.skipDisabled, b.skipMaskGraphic, b.skipPlaceholder,
                 b.skipMarkerArt, b.skipTransparent, b.skipOther, view_.fog.size(), view_.dyn.size(), perCat[kTeleporter],
                 perCat[kTreasurePod], perCat[kGordo], perCat[kResource], perCat[kOther], summary);
        SR2_INFO("minimap: page activation: {} images copied from containers Map.Awake activates "
                 "(_forceActiveOnAwake: {} objects); {} map textures were point-filtered (switched to bilinear)",
                 b.forcedActive, b.forceActive.size(), b.pointFiltered);
        if (b.matKept + b.matAsTexture + b.matDropped + b.noTexture > 0)
            SR2_INFO("minimap: page materials: {} custom kept (stencil-capable), {} shown via their main texture, {} "
                     "dropped (no stencil support); {} sprites without a texture (sprite atlas not loaded yet)",
                     b.matKept, b.matAsTexture, b.matDropped, b.noTexture);
        // Fog areas without copied art: their own RectTransform is the area (better than no area at all).
        for (Group& f : view_.fog) {
            if (!f.quads.empty() || !f.comp.Alive()) continue;
            Il2CppObject* ft = unity::TransformOf(f.comp.get());
            if (!ugui::IsRectTransform(ft)) continue;
            Rect r = ugui::GetRect(ft);
            auto corner = [&](float x, float y) {
                Vec3 l = ugui::InverseTransformPoint(view_.mapT.get(), ugui::TransformPoint(ft, {x, y, 0}));
                return Vec2{l.x, l.y};
            };
            Vec2 c0 = corner(r.x, r.y), c1 = corner(r.x + r.w, r.y), c2 = corner(r.x, r.y + r.h);
            Vec2 ex{c1.x - c0.x, c1.y - c0.y}, ey{c2.x - c0.x, c2.y - c0.y};
            if (std::hypot(ex.x, ex.y) < 1 || std::hypot(ey.x, ey.y) < 1) continue;
            f.AddQuad(c0, ex, ey);
            f.fromRect = true;
        }
        fogLogged_ = false;
        iconsLogged_ = false;
        if (!b.placeholders.empty()) SR2_INFO("minimap: page placeholders skipped: {}", b.placeholders);
        if (b.fogOpaque > 0)
            SR2_INFO("minimap: fog: {} fog-shader images drawn at full alpha (CanvasGroup.ignoreParentGroups): "
                     "the shader uses vertex alpha as its reveal threshold, so the minimap opacity does not apply "
                     "to fog",
                     b.fogOpaque);
        if (view_.neg.copies > 0 || b.skipNegMask > 0)
            SR2_INFO("minimap: fog: outside fog (NegativeMask shader): {} image(s) copied, {} skipped; {}",
                     view_.neg.copies, b.skipNegMask, view_.neg.info);
        if (b.staticFog > 0)
            SR2_INFO("minimap: fog: {} fog-shader images outside every FogMapElement (static fog: no game "
                     "component toggles them, the full map always draws them; shown while fog of war is respected): {}",
                     b.staticFog, b.staticFogNames);
        {  // the largest copies: what the minimap actually draws
            std::sort(b.notes.begin(), b.notes.end(), [](const auto& x, const auto& y) { return x.area > y.area; });
            std::string top;
            for (size_t i = 0; i < b.notes.size() && i < 8; ++i) top += (i ? "; " : "") + b.notes[i].text;
            if (!top.empty()) SR2_INFO("minimap: page art (largest first): {}", top);
        }

        Step("build: background");
        ApplyBackground(view_.mapDef.get());
        build_ = Build{};  // drops the build pins
        view_.ok = true;
        lastBuildError_.clear();
        pagePendingSince_ = -1;
        ResetTransformCache();
        fogAcc_ = kFogInterval;
        dynAcc_ = kDynInterval;
        scanAcc_ = kScanInterval;
        slowAcc_ = kSlowTick;
        Step("build: first fog");
        fogFullDue_ = true;
        UpdateFog("page built");
        UpdateDynamic();
        LogFog();
    }

    void LogFog() {
        if (fogLogged_) return;
        fogLogged_ = true;
        int art = 0, rect = 0, empty = 0, revealed = 0, hidden = 0, unknown = 0;
        for (const Group& f : view_.fog) {
            if (f.quads.empty()) ++empty;
            else if (f.fromRect) ++rect;
            else ++art;
            if (f.state == 1) ++revealed;
            else if (f.state == 0) ++hidden;
            else ++unknown;
        }
        SR2_INFO("minimap: fog: {} areas ({} shaped by their art, {} by their own rect, {} without an area); {} revealed, "
                 "{} unrevealed, {} unknown (fogged until the save data is ready){}",
                 view_.fog.size(), art, rect, empty, revealed, hidden, unknown,
                 cfg_.respectFog ? " - icons inside unrevealed / unknown areas are hidden"
                                 : " (fog of war ignored by setting: no fog drawn, nothing queried)");
    }

    bool ReadProjection(Il2CppObject* mapComp) {
        // The prefab's serialized anchors: exactly what Map.SetupInitialValues
        // feeds MapUtils.GetSlopeAndOffset to build the MapProjectionHelper the
        // full map uses (verified by disassembly). The helper itself is not
        // serialized (null on the prefab), so it is only a fallback.
        Vec2 w1 = F<Vec2>(mapComp, sym_.mapAnchor1 + sym_.anchorWorld);
        Vec2 m1 = F<Vec2>(mapComp, sym_.mapAnchor1 + sym_.anchorMap);
        Vec2 w2 = F<Vec2>(mapComp, sym_.mapAnchor2 + sym_.anchorWorld);
        Vec2 m2 = F<Vec2>(mapComp, sym_.mapAnchor2 + sym_.anchorMap);
        if (w2.x != w1.x && w2.y != w1.y) {
            view_.scale.x = (m2.x - m1.x) / (w2.x - w1.x);
            view_.offset.x = m2.x - view_.scale.x * w2.x;
            view_.scale.y = (m2.y - m1.y) / (w2.y - w1.y);
            view_.offset.y = m2.y - view_.scale.y * w2.y;
            if (std::isfinite(view_.scale.x) && std::isfinite(view_.scale.y) && std::isfinite(view_.offset.x) &&
                std::isfinite(view_.offset.y)) {
                SR2_INFO("minimap: projection from anchors: world ({:.1f},{:.1f})->map ({:.1f},{:.1f}), world "
                         "({:.1f},{:.1f})->map ({:.1f},{:.1f})",
                         w1.x, w1.y, m1.x, m1.y, w2.x, w2.y, m2.x, m2.y);
                return true;
            }
        }
        Il2CppObject* helper = game::Has(sym_.mapProjection) ? F<Il2CppObject*>(mapComp, sym_.mapProjection) : nullptr;
        if (ugui::IsA(helper, sym_.projectionHelper) && game::Has(sym_.projScale) && game::Has(sym_.projOffset)) {
            view_.scale = F<Vec2>(helper, sym_.projScale);
            view_.offset = F<Vec2>(helper, sym_.projOffset);
            if (std::isfinite(view_.scale.x) && std::isfinite(view_.scale.y)) {
                SR2_INFO("minimap: projection from the page's MapProjectionHelper (anchors degenerate)");
                return true;
            }
        }
        return false;
    }

    // world -> page local (same as Map.GetMapPosition, incl. clamping)
    Vec2 ProjectPage(Vec3 wp, bool clamp = true) const {
        Vec2 p{wp.x * view_.scale.x + view_.offset.x, wp.z * view_.scale.y + view_.offset.y};
        if (clamp && view_.clamp) {
            const Rect& r = view_.clampRect;
            p.x = std::clamp(p.x, r.x, r.x + r.w);
            p.y = std::clamp(p.y, r.y, r.y + r.h);
        }
        return p;
    }
    bool OnPage(Vec2 p) const {
        const Rect& r = view_.clampRect;
        return !view_.clamp || (p.x >= r.x && p.x <= r.x + r.w && p.y >= r.y && p.y <= r.y + r.h);
    }

    void ApplyBackground(Il2CppObject* mapDef) {
        auto apply = [&](Il2CppObject* img, size_t field) {
            if (!img || !game::Has(field) || !game::Has(sym_.imgSetupImage)) return;
            Il2CppObject* sprite = F<Il2CppObject*>(mapDef, field + sym_.imgSetupImage);
            bool ok = ugui::IsA(sprite, ugui::SpriteClass());
            if (ok) {
                ugui::ImageSetSprite(img, sprite);
                Color tint{1, 1, 1, 1};
                if (game::Has(sym_.imgSetupTint)) tint = F<Color>(mapDef, field + sym_.imgSetupTint);
                if (tint.a <= 0.001f) tint.a = 1;
                ugui::GraphicSetColor(img, tint);
            }
            unity::SetActive(unity::GameObjectOf(img), ok);
        };
        apply(ui_.bg.get(), sym_.defBackground);
        apply(ui_.overlay.get(), sym_.defOverlay);
    }

    // The game's player marker, read from the page's marker prefab mapping
    // (Map._markerPrefabMapping -> _playerMarkerPrefab, a PlayerMapMarker). In 1.3.0 (UnityPy):
    //   PlayerMarker (50x50, pivot 0.5/0.45)
    //     FacingFrame = _arrowRect           turned by PlayerMapMarker.Rotate
    //       Cone = _moveBelowMarkersTransform (scale 0.7)  > Cone  Image fxPlayerMarkerCone (pulses)
    //       FacingArrow                       Image framePlayerMarker (pulses)
    //     BeaIcon                             Image player_face (upright)
    // Every visible Image is copied with its rect corners (like the page art), in _arrowRect space when it
    // turns with the heading, otherwise in the root's space.
    bool GameMarkerAlive() const {
        if (!gm_.ok) return false;
        for (const MarkerPartDef& p : gm_.parts)
            if (!p.sprite.Alive()) return false;
        return true;
    }

    void LoadGameMarker() {
        if (GameMarkerAlive()) return;
        if (gm_.ok) {  // the prefab (and its sprites) was unloaded: drop our copy, read it again
            SR2_INFO("minimap: player marker sprites unloaded - reading the marker again");
            DestroyGameMarker();
        }
        gm_ = GameMarker{};
        if (!game::Has(sym_.mapMarkerMapping) || !game::Has(sym_.mpmPlayerPrefab) ||
            !game::Has(sym_.pmmArrow) || !view_.mapComp.Alive())
            return;
        Il2CppObject* mpm = F<Il2CppObject*>(view_.mapComp.get(), sym_.mapMarkerMapping);
        if (!ugui::IsA(mpm, sym_.markerPrefabMapping)) return;
        Il2CppObject* prefab = F<Il2CppObject*>(mpm, sym_.mpmPlayerPrefab);
        if (!ugui::IsA(prefab, unity::GameObjectClass())) return;
        Il2CppObject* pmm = unity::GetComponent(prefab, sym_.playerMapMarker);
        if (!ugui::IsA(pmm, sym_.playerMapMarker)) return;
        Il2CppObject* arrowRect = F<Il2CppObject*>(pmm, sym_.pmmArrow);
        if (!ugui::IsA(arrowRect, ugui::TransformClass())) return;
        Il2CppObject* below = game::Has(sym_.pmmBelow) ? F<Il2CppObject*>(pmm, sym_.pmmBelow) : nullptr;
        if (!ugui::IsA(below, ugui::TransformClass())) below = nullptr;
        Il2CppObject* rootT = unity::TransformOf(prefab);
        if (!ugui::IsRectTransform(rootT)) return;
        std::vector<Ref> pins;  // keeps the transforms used as map keys (pointer identity)
        pins.emplace_back(arrowRect);
        pins.emplace_back(rootT);
        if (below) pins.emplace_back(below);

        Rect rr = ugui::GetRect(rootT);
        gm_.rootH = rr.h > 1 ? rr.h : 50.0f;
        Vec3 fo = ugui::InverseTransformPoint(rootT, ugui::TransformPoint(arrowRect, {0, 0, 0}));
        gm_.facingOrigin = {fo.x, fo.y};
        gm_.rotOffset = game::Has(sym_.pmmRotOffset) ? F<float>(pmm, sym_.pmmRotOffset) : 0.0f;
        if (!std::isfinite(gm_.rotOffset)) gm_.rotOffset = 0;

        std::unordered_map<Il2CppObject*, float> groupAlpha;
        ugui::ForEachInChildren(prefab, ugui::CanvasGroupClass(), true, [&](Il2CppObject* c) {
            if (Il2CppObject* t = unity::TransformOf(c)) {
                pins.emplace_back(t);
                groupAlpha[t] = ugui::CanvasGroupGetAlpha(c);
            }
            return true;
        });
        std::string desc;
        ugui::ForEachInChildren(prefab, ugui::ImageClass(), true, [&](Il2CppObject* img) {
            Il2CppObject* t = unity::TransformOf(img);
            if (!ugui::IsRectTransform(t) || !ugui::BehaviourEnabled(img)) return true;
            Il2CppObject* sprite = ugui::ImageGetSprite(img);
            if (!unity::IsAlive(sprite)) return true;
            MarkerPartDef p;
            bool active = true;
            float alpha = 1;
            for (Il2CppObject* cur = t; cur; cur = ugui::GetParent(cur)) {
                if (!ugui::ActiveSelf(unity::GameObjectOf(cur))) active = false;
                if (cur == arrowRect) p.facing = true;
                if (cur == below) p.below = true;
                if (auto a = groupAlpha.find(cur); a != groupAlpha.end()) alpha *= a->second;
                if (cur == rootT) break;
            }
            p.color = ugui::GraphicGetColor(img);
            p.color.a *= alpha;
            if (!active || p.color.a <= 0.003f) return true;
            Il2CppObject* ref = p.facing ? arrowRect : rootT;
            Rect r = ugui::GetRect(t);
            if (r.w < 0.01f || r.h < 0.01f) return true;
            auto local = [&](Il2CppObject* from, float x, float y) {
                Vec3 l = ugui::InverseTransformPoint(ref, ugui::TransformPoint(from, {x, y, 0}));
                return Vec2{l.x, l.y};
            };
            Vec2 c0 = local(t, r.x, r.y), c1 = local(t, r.x + r.w, r.y), c2 = local(t, r.x, r.y + r.h);
            Vec2 ex{c1.x - c0.x, c1.y - c0.y}, ey{c2.x - c0.x, c2.y - c0.y};
            p.size = {std::hypot(ex.x, ex.y), std::hypot(ey.x, ey.y)};
            if (p.size.x < 0.01f || p.size.y < 0.01f) return true;
            p.pivot = {-r.x / r.w, -r.y / r.h};  // GetRect is relative to the pivot
            p.pos = {c0.x + p.pivot.x * ex.x + p.pivot.y * ey.x, c0.y + p.pivot.x * ex.y + p.pivot.y * ey.y};
            p.angle = std::atan2(ex.y, ex.x) * 180.0f / kPi;
            p.mirrored = (ex.x * ey.y - ex.y * ey.x) < 0;
            if (Il2CppObject* parent = ugui::GetParent(t)) {
                Vec2 o = local(parent, 0, 0), u = local(parent, 0, 1);
                p.up = {u.x - o.x, u.y - o.y};
            }
            p.sprite.Reset(sprite);
            p.type = ugui::ImageGetType(img);
            p.ppu = ugui::ImageGetPpuMultiplier(img);
            p.preserveAspect = ugui::ImageGetPreserveAspect(img);
            p.name = unity::GetName(unity::GameObjectOf(img));
            // The prefab's Animator clips (decoded from the bundle, see NOTES.md): FacingArrow scale pulse,
            // Cone (the Image object inside the cone holder) position + alpha pulse.
            p.anim = p.name == "FacingArrow" ? kAnimArrowPulse : (p.name == "Cone" ? kAnimCone : kAnimNone);
            desc += std::format("{}'{}' sprite '{}' {:.0f}x{:.0f} at ({:.1f},{:.1f}){}{}{}", desc.empty() ? "" : "; ",
                                p.name, unity::GetName(sprite), p.size.x, p.size.y, p.pos.x, p.pos.y,
                                p.facing ? " turns" : " upright", p.below ? ", under the icons" : "",
                                p.anim ? ", pulses" : "");
            gm_.parts.push_back(std::move(p));
            return true;
        });
        gm_.ok = !gm_.parts.empty();
        ui_.arrowStyleApplied = -1;
        if (!gm_.ok) {
            SR2_WARN("minimap: the game's player marker has no visible image - using the classic arrow");
            return;
        }
        SR2_INFO("minimap: player marker from the game ({} images, root {:.0f} high, facing origin ({:.1f},{:.1f}), "
                 "prefab rotation offset {}): {}",
                 gm_.parts.size(), gm_.rootH, gm_.facingOrigin.x, gm_.facingOrigin.y, gm_.rotOffset, desc);
        ApplyArrowStyle();
    }

    // One of our copies of a marker part, child of parentT.
    MarkerPart MakeMarkerPart(Il2CppObject* parentT, const MarkerPartDef& d, int layer) {
        MarkerPart m;
        Il2CppObject* go = Child(parentT, d.name.c_str(), layer);
        if (!go) return m;
        m.t.Reset(unity::TransformOf(go));
        Il2CppObject* img = AddImage(go);
        m.img.Reset(img);
        ugui::ImageSetSprite(img, d.sprite.get());
        if (d.type != 0) {
            ugui::ImageSetType(img, d.type);
            ugui::ImageSetPpuMultiplier(img, d.ppu);
        }
        if (d.preserveAspect) ugui::ImageSetPreserveAspect(img, true);
        ugui::GraphicSetColor(img, d.color);
        ugui::Center(m.t.get(), d.size);
        ugui::SetPivot(m.t.get(), d.pivot);
        ugui::SetAnchoredPosition(m.t.get(), d.pos);
        ugui::SetLocalEuler(m.t.get(), {0, 0, d.angle});
        m.sy = d.mirrored ? -1.0f : 1.0f;
        if (d.mirrored) ugui::SetLocalScale(m.t.get(), {1, -1, 1});
        m.anim = d.anim;
        m.pos = d.pos;
        m.up = d.up;
        m.color = d.color;
        return m;
    }

    // On-screen copy (child of the arrow holder): upright parts + a "Facing" container (= _arrowRect)
    // that turns with the heading. Its own nested Canvas: the pulse animation rebuilds only this batch.
    void BuildGameMarker() {
        if (ui_.gameMarkerT.Alive() || !gm_.ok || !ui_.arrowHolder.Alive()) return;
        int layer = ugui::GetLayer(ui_.root.get());
        Il2CppObject* go = Child(ui_.arrowHolder.get(), "GameMarker", layer);
        if (!go) return;
        ui_.gameMarkerT.Reset(unity::TransformOf(go));
        ugui::Center(ui_.gameMarkerT.get(), {gm_.rootH, gm_.rootH});  // origin = the player point
        ugui::AddCanvas(go);
        // _arrowRect comes first in the prefab, the upright face after it (draw order = sibling order).
        Il2CppObject* f = Child(ui_.gameMarkerT.get(), "Facing", layer);
        ui_.gameFacingT.Reset(unity::TransformOf(f));
        ugui::Center(ui_.gameFacingT.get(), {0, 0});
        ugui::SetAnchoredPosition(ui_.gameFacingT.get(), gm_.facingOrigin);
        for (const MarkerPartDef& d : gm_.parts)
            if (!d.below)
                ui_.gameParts.push_back(MakeMarkerPart(d.facing ? ui_.gameFacingT.get() : ui_.gameMarkerT.get(), d, layer));
        lastArrowZ_ = 1e9f;
    }

    // The view cone, inside our page (built with the page, see FinishBuild).
    void BuildCone() {
        if (view_.coneT || !gm_.ok || !view_.page.Alive()) return;
        bool any = false;
        for (const MarkerPartDef& d : gm_.parts) any |= d.below;
        if (!any) return;
        int layer = ugui::GetLayer(ui_.root.get());
        Il2CppObject* go = Child(view_.page.get(), "PlayerCone", layer);
        if (!go) return;
        view_.coneT.Reset(unity::TransformOf(go));
        ugui::Center(view_.coneT.get(), {0, 0});
        ugui::AddCanvas(go);  // its pulse rebuilds only this batch, not the whole page
        for (const MarkerPartDef& d : gm_.parts)
            if (d.below) view_.coneParts.push_back(MakeMarkerPart(view_.coneT.get(), d, layer));
        unity::SetActive(go, UseGameArrow());
        coneS_ = -1;
    }

    void DestroyGameMarker() {
        if (ui_.gameMarkerT.Alive()) ugui::Destroy(unity::GameObjectOf(ui_.gameMarkerT.get()));
        ui_.gameMarkerT.Reset(nullptr);
        ui_.gameFacingT.Reset(nullptr);
        ui_.gameParts.clear();
        if (view_.coneT.Alive()) ugui::Destroy(unity::GameObjectOf(view_.coneT.get()));
        view_.coneT.Reset(nullptr);
        view_.coneParts.clear();
        ui_.arrowStyleApplied = -1;
    }

    // Cheap per-frame test: the style was applied with a loaded game marker (aliveness is checked when
    // the page is (re)built, LoadGameMarker).
    bool UseGameArrow() const { return cfg_.arrowStyle == 0 && gm_.ok && ui_.arrowStyleApplied == 0; }

    // Which player arrow is shown, and its size.
    void ApplyArrowStyle() {
        if (!ui_.arrowImg.Alive() || !ui_.arrowT.Alive()) return;
        bool game = cfg_.arrowStyle == 0 && gm_.ok;
        if (game) {
            BuildGameMarker();
            game = ui_.gameMarkerT.Alive();
        }
        // Base size (13% of the minimap) at zoom 2.5x, proportional to the zoom, clamped to 0.4x..1.6x.
        // Game marker: its root rect (50 units) gets kGameMarkerScale times this height.
        float px = std::max(10.0f, ui_.size * 0.13f * ArrowZoomFactor());
        unity::SetActive(unity::GameObjectOf(ui_.arrowT.get()), !game);
        if (ui_.gameMarkerT.Alive()) unity::SetActive(unity::GameObjectOf(ui_.gameMarkerT.get()), game);
        if (view_.coneT.Alive()) unity::SetActive(unity::GameObjectOf(view_.coneT.get()), game);
        if (game) {
            ui_.markerK = px * kGameMarkerScale / gm_.rootH;  // the cone follows markerK
            ugui::SetLocalScale(ui_.gameMarkerT.get(), {ui_.markerK, ui_.markerK, 1});
        } else {
            ugui::ImageSetSprite(ui_.arrowImg.get(), sprArrow_.get());
            ugui::SetPivot(ui_.arrowT.get(), {0.5f, 0.5f});
            ugui::SetSizeDelta(ui_.arrowT.get(), {px, px});
            ugui::SetAnchoredPosition(ui_.arrowT.get(), {0, 0});
        }
        ui_.arrowStyleApplied = game ? 0 : 1;
        lastArrowZ_ = 1e9f;
        coneS_ = -1;
    }

    // Pulse curves of the marker's Animator clips (1.3.0, decoded from the bundle: per-segment cubics, 60 Hz
    // source, both loop every 4 s through the controller):
    //   MapMarkerPulse      FacingArrow localScale.xy 1 -> 0.75 -> 1 -> 0.75 -> 1 at t = 0,1,2,3,4
    //   MapMarkerPulse_Cone Cone anchoredPosition.y -16 -> -14 -> -16, Image alpha 1 -> 0.75 -> 1 at t = 0,2,4
    // (MapMarkerPulse_Rare, picked at random instead of MapMarkerPulse, also wobbles BeaIcon's rotation once;
    // not reproduced.)
    static float ArrowPulse(float t) {
        float u = std::fmod(t, 4.0f);
        int seg = std::clamp(static_cast<int>(u), 0, 3);
        u -= static_cast<float>(seg);
        static const float k[4][4] = {{0.245f, -0.488f, -0.007f, 1.0f},
                                      {-0.268f, 0.267f, 0.251f, 0.75f},
                                      {0.233f, -0.464f, -0.019f, 1.0f},
                                      {-0.268f, 0.267f, 0.251f, 0.75f}};
        const float* c = k[seg];
        return ((c[0] * u + c[1]) * u + c[2]) * u + c[3];
    }
    // 0 at rest .. 1 at the cone's peak (t = 2 s): smoothstep both ways (the clip's cubics are exactly that).
    static float ConePhase(float t) {
        float u = std::fmod(t, 4.0f) * 0.5f;
        float x = u < 1 ? u : 2 - u;
        return x * x * (3 - 2 * x);
    }

    void AnimateMarkerParts(std::vector<MarkerPart>& parts) {
        for (MarkerPart& m : parts) {
            if (m.anim == kAnimNone || !m.t) continue;
            if (m.anim == kAnimArrowPulse) {
                float sc = ArrowPulse(time_);
                ugui::SetLocalScale(m.t.get(), {sc, sc * m.sy, 1});
            } else {
                float a = ConePhase(time_);
                float dy = 2.0f * a;  // -16 -> -14 in the cone holder's space
                ugui::SetAnchoredPosition(m.t.get(), {m.pos.x + m.up.x * dy, m.pos.y + m.up.y * dy});
                Color c = m.color;
                c.a *= 1.0f - 0.25f * a;
                ugui::GraphicSetColor(m.img.get(), c);
            }
        }
    }

    int Classify(Il2CppObject* comp, std::string& label) {
        std::string cn = unity::ClassName(comp);
        label = cn.substr(cn.rfind('.') == std::string::npos ? 0 : cn.rfind('.') + 1);
        std::string l = Lower(label);
        if (Contains(l, "teleporter")) return kTeleporter;
        if (Contains(l, "gordo")) return kGordo;
        if (sym_.getDescriptor) {
            Il2CppObject* desc = nullptr;
            ugui::Fn f = ugui::BindFn(ugui::VirtualMethod(comp, sym_.getDescriptor));
            if (f) Managed([&] { desc = ugui::Call<Il2CppObject*>(f, comp); });
            if (desc) {
                std::string dn = unity::IsAlive(desc) ? unity::GetName(desc) : unity::ClassName(desc);
                label += ":" + dn;
                std::string d = Lower(dn);
                if (Contains(d, "teleport")) return kTeleporter;
                if (Contains(d, "treasure") || Contains(d, "pod")) return kTreasurePod;
                if (Contains(d, "gordo")) return kGordo;
                if (Contains(d, "resource")) return kResource;
            }
        }
        return kOther;
    }

    // ---------------------------------------------------------- fog / dyn -----
    static void SetGroup(Group& g, bool show) {
        for (Ref& r : g.gos)
            if (r.Alive()) unity::SetActive(r.get(), show);
    }

    // ---- fog of war ------------------------------------------------------
    // Reveal source (what the full map does, disassembly): MapUIFogReveal.RevealKnownZones calls
    // FogMapElement.SetRevealState(ShouldBeRevealed(_eventDirector)) for every element.
    // ShouldBeRevealed = StaticEventQueryComponent(_fogRevealEvent).IsSatisfied(director), i.e. the
    // director's EventRecordModel (`_model`) holds the reveal event that MapNodeActivator.Activate
    // raises. `_model` is set by EventDirector.SetModel when a save's GameModel registers it, and set
    // to null when the session ends; while it is null or still another save's, the answers are wrong.
    // So we query only once the director's record IS the loaded save's (GameModel.eventRecordModel)
    // and stayed so for kFogSettle seconds; before that every area is UNKNOWN = fogged.
    struct FogSource {
        Il2CppObject* director = nullptr;
        Il2CppObject* record = nullptr;  // the director's EventRecordModel (identity)
        const char* from = "";
        bool matchesSave = false;        // record == GameModel.eventRecordModel
    };

    std::string FogName(Il2CppObject* fogComp) {
        if (game::Has(sym_.fogEvent) && game::Has(sym_.sgeKey)) {
            Il2CppObject* ev = F<Il2CppObject*>(fogComp, sym_.fogEvent);
            if (ugui::IsA(ev, sym_.staticGameEvent)) {
                auto* key = F<Il2CppString*>(ev, sym_.sgeKey);
                if (ugui::PlausiblePointer(key)) {
                    std::string k = unity::ToUtf8(key);
                    if (!k.empty()) return k;
                }
            }
        }
        return unity::GetName(unity::GameObjectOf(fogComp));
    }

    Il2CppObject* RecordOf(Il2CppObject* director) const {
        if (!game::Has(sym_.edModel) || !ugui::IsA(director, sym_.eventDirector)) return nullptr;
        Il2CppObject* r = F<Il2CppObject*>(director, sym_.edModel);
        if (sym_.eventRecordModel ? !ugui::IsA(r, sym_.eventRecordModel) : !ugui::PlausiblePointer(r)) return nullptr;
        return r;
    }

    // The director to query and whether the save's event data is there. Returns false (with
    // fogWaitReason_) while the areas must stay UNKNOWN.
    bool FogSourceNow(FogSource& out) {
        // 1. the loaded save's record
        Il2CppObject* saveRecord = nullptr;
        bool saveKnown = sym_.tryGetGameModel && game::Has(sym_.gmEventRecord);
        if (saveKnown) {
            Il2CppObject* gm = nullptr;
            Managed([&] {
                if (!ugui::Call<bool>(sym_.tryGetGameModel, &gm)) gm = nullptr;
            });
            if (!ugui::IsA(gm, sym_.gameModel)) {
                fogWaitReason_ = "no GameModel (no save loaded)";
                return false;
            }
            saveRecord = F<Il2CppObject*>(gm, sym_.gmEventRecord);
            if (sym_.eventRecordModel ? !ugui::IsA(saveRecord, sym_.eventRecordModel) : !ugui::PlausiblePointer(saveRecord)) {
                fogWaitReason_ = "GameModel has no event record yet";
                return false;
            }
        }
        // 2. candidate directors: the one the reveal events raise into (StaticGameEvent.RaiseEvent ->
        //    _eventDirector.RaiseEvent, so the reveals are recorded in ITS model), then the map director's
        struct Cand {
            Il2CppObject* d;
            const char* from;
        } cands[2] = {};
        if (game::Has(sym_.fogEvent) && game::Has(sym_.sgeDirector))
            for (const Group& f : view_.fog) {
                if (!f.comp.Alive()) continue;
                Il2CppObject* ev = F<Il2CppObject*>(f.comp.get(), sym_.fogEvent);
                if (!ugui::IsA(ev, sym_.staticGameEvent)) continue;
                cands[0] = {F<Il2CppObject*>(ev, sym_.sgeDirector), "FogMapElement._fogRevealEvent._eventDirector"};
                break;
            }
        if (mapDirector_.Alive() && game::Has(sym_.mdEventDirector))
            cands[1] = {F<Il2CppObject*>(mapDirector_.get(), sym_.mdEventDirector), "MapDirector._eventDirector"};
        if (!game::Has(sym_.edModel) || !sym_.eventDirector) {  // readiness not checkable: trust any director
            for (const Cand& c : cands)
                if (sym_.eventDirector ? ugui::IsA(c.d, sym_.eventDirector) : ugui::PlausiblePointer(c.d)) {
                    out = {c.d, nullptr, c.from, false};
                    return true;
                }
            fogWaitReason_ = "no EventDirector";
            return false;
        }
        const Cand* stale = nullptr;
        Il2CppObject* staleRecord = nullptr;
        for (const Cand& c : cands) {
            Il2CppObject* r = RecordOf(c.d);
            if (!r) continue;
            if (!saveKnown || r == saveRecord) {
                out = {c.d, r, c.from, saveKnown};
                return true;
            }
            if (!stale) {
                stale = &c;
                staleRecord = r;
            }
        }
        if (stale && fogWaitSince_ >= 0 && time_ - fogWaitSince_ >= kFogFallback) {
            // The director has a record, just not GameModel's (duplicate asset instance?). Better than
            // fog forever; logged once.
            if (!fogFallbackLogged_) {
                fogFallbackLogged_ = true;
                SR2_WARN("minimap: fog: {}'s event record is not GameModel.eventRecordModel after {:.0f} s - "
                         "querying it anyway",
                         stale->from, kFogFallback);
            }
            out = {stale->d, staleRecord, stale->from, false};
            return true;
        }
        fogWaitReason_ = stale ? "EventDirector._model is not the loaded save's event record (previous session)"
                               : (cands[0].d || cands[1].d ? "EventDirector._model is null (save data not set yet)"
                                                           : "no EventDirector (MapDirector not found yet)");
        return false;
    }

    // Readiness bookkeeping (10 Hz until settled, then with every fog evaluation).
    bool FogReadiness(FogSource& src) {
        std::string prevReason = fogWaitReason_;
        if (!FogSourceNow(src)) {
            fogReadySince_ = -1;
            if (fogWaitSince_ < 0) fogWaitSince_ = time_;
            if (fogWaitReason_ != prevReason)
                SR2_INFO("minimap: fog: waiting for the save's map data: {} - every area fogged (unknown) until then",
                         fogWaitReason_);
            return false;
        }
        if (src.record != fogRecord_) {  // another save / session: every cached answer is void
            if (fogRecord_)
                SR2_INFO("minimap: fog: event record changed (save / session change) - all {} areas unknown again",
                         view_.fog.size());
            for (Group& f : view_.fog) f.state = -1;
            if (fogRecord_) fogSummaryLogged_ = false;  // another save: summarise it again
            fogRecord_ = src.record;
            fogReadySince_ = -1;
            fogReadyLogged_ = false;
            fogFullDue_ = true;
        }
        if (fogReadySince_ < 0) fogReadySince_ = time_;
        if (src.director != fogDirLogged_) {
            fogDirLogged_ = src.director;
            Il2CppObject* mdEd = mapDirector_.Alive() && game::Has(sym_.mdEventDirector)
                                     ? F<Il2CppObject*>(mapDirector_.get(), sym_.mdEventDirector)
                                     : nullptr;
            SR2_INFO("minimap: fog: reveal source FogMapElement.ShouldBeRevealed(EventDirector '{}' from {}{}) = the "
                     "full map's MapUIFogReveal.RevealKnownZones query; readiness: {}",
                     ugui::IsA(src.director, sym_.eventDirector) ? unity::GetName(src.director) : std::string("?"),
                     src.from, !mdEd ? "" : (mdEd == src.director ? ", same instance as MapDirector._eventDirector"
                                                        : ", NOT MapDirector._eventDirector's instance"),
                     src.matchesSave ? "EventDirector._model == GameModel.eventRecordModel"
                                     : (src.record ? "EventDirector._model set (GameModel record not comparable)"
                                                   : "unchecked (EventDirector._model not resolvable)"));
        }
        if (!FogSettled()) return false;
        if (!fogReadyLogged_) {
            fogReadyLogged_ = true;
            SR2_INFO("minimap: fog: save data ready{} (event record stable for {:.1f} s) - fog areas are queried from now on",
                     fogWaitSince_ >= 0 ? std::format(" after waiting {:.1f} s", time_ - fogWaitSince_) : std::string(),
                     time_ - fogReadySince_);
            fogFullDue_ = true;
            fogAcc_ = kFogInterval;  // query right away (next frame), not at the next 2 s step
        }
        fogWaitSince_ = -1;
        fogWaitReason_.clear();
        return true;
    }

    bool FogSettled() const { return fogReadySince_ >= 0 && time_ - fogReadySince_ >= kFogSettle; }

    // Re-query (unrevealed + unknown areas, or every area when fogFullDue_), then show / hide.
    void UpdateFog(const char* reason) {
        view_.neg.canvasT.Reset(nullptr);  // re-resolved next frame (the minimap may have changed canvas)
        if (!view_.ok) return;
        int failures = 0;
        std::string revealedNow, fogged;
        bool full = fogFullDue_, queried = false;
        if (cfg_.respectFog && sym_.fogShouldReveal && !view_.fog.empty()) {
            FogSource src;
            if (FogReadiness(src)) {
                queried = true;
                fogFullDue_ = false;
                for (Group& f : view_.fog) {
                    if (!full && f.state == 1) continue;  // reveals are permanent within one save
                    bool r = false;
                    int before = f.state;
                    if (!f.comp.Alive() ||
                        !Managed([&] { r = ugui::Call<bool>(sym_.fogShouldReveal, f.comp.get(), src.director); })) {
                        ++failures;
                        f.state = -1;  // unknown -> fogged, asked again in 2 s
                        continue;
                    }
                    f.state = r ? 1 : 0;
                    if (f.state == 1 && before != 1) revealedNow += (revealedNow.empty() ? "'" : ", '") + f.name + "'";
                    if (f.state == 0 && before == 1) fogged += (fogged.empty() ? "'" : ", '") + f.name + "'";
                }
            }
        }
        ApplyFogVisibility();
        int revealed = 0, hidden = 0, unknown = 0;
        for (const Group& f : view_.fog) (f.state == 1 ? revealed : (f.state == 0 ? hidden : unknown))++;
        if (revealed != fogLastRevealed_ || hidden != fogLastHidden_ || unknown != fogLastUnknown_ || failures ||
            !fogged.empty()) {
            fogLastRevealed_ = revealed;
            fogLastHidden_ = hidden;
            fogLastUnknown_ = unknown;
            SR2_INFO("minimap: fog: {} revealed / {} unrevealed / {} unknown ({}{}){}{}{}{}", revealed, hidden, unknown,
                     reason, full ? ", all areas queried" : "",
                     revealedNow.empty() ? "" : "; revealed: " + revealedNow,
                     fogged.empty() ? "" : "; fogged again: " + fogged,
                     failures ? std::format("; {} queries failed (kept fogged)", failures) : std::string(),
                     cfg_.respectFog ? "" : " - fog of war ignored by setting");
        }
        // One summary per loaded save (event record): after its first complete query, or at once when the
        // fog is ignored by the setting.
        if (!fogSummaryLogged_ && !view_.fog.empty() && ((queried && full) || !cfg_.respectFog)) {
            fogSummaryLogged_ = true;
            const Group& st = view_.staticFog;
            std::string outside = view_.neg.copies > 0 ? std::format("outside fog {} image(s), mask driven by the minimap",
                                                                      view_.neg.copies)
                                                       : std::string("outside fog not copied");
            if (cfg_.respectFog)
                SR2_INFO("minimap: fog summary (save loaded): {}/{} areas revealed, {} unrevealed, {} unknown{}; static "
                         "fog: {} image(s) {}; {}",
                         revealed, view_.fog.size(), hidden, unknown,
                         failures ? std::format(" ({} queries failed)", failures) : std::string(), st.gos.size(),
                         st.vis == 1 ? "shown (as in the full map)" : "hidden", outside);
            else
                SR2_INFO("minimap: fog summary (save loaded): fog of war ignored by setting - {} areas not queried, "
                         "no fog drawn; static fog: {} image(s) hidden; {}",
                         view_.fog.size(), st.gos.size(), outside);
        }
    }

    // ---- outside fog ------------------------------------------------------
    // NegativeMaskMap.UpdateBeforeRender (0x6347A0, Canvas.willRenderCanvases, updateConstantly):
    //   M = transform.worldToLocalMatrix * canvasTransform.localToWorldMatrix (canvasTransform =
    //   GetComponentInParent<Canvas>().transform, OnEnable 0x6346A0); M[3] = maskEdgeNoiseScale,
    //   M[7] = maskEdgeNoiseStrength, M[11] = softness, M[15] = roundness; Shader.SetGlobalMatrix(_NegativeMask, M).
    // The shader turns the vertex position into mask space with M (rounded box |2p| < 1, softness/noise
    // edge) and draws fog only OUTSIDE it. We place our own mask transform in our page where the prefab's
    // sits relative to the Map and set the same global every frame while our outside fog is visible.
    int negMaskId_ = -1;
    bool negCanvasLogged_ = false;

    void SetupNegativeMask(Il2CppObject* prefab) {
        View::NegMask& n = view_.neg;
        n = View::NegMask{};
        if (!sym_.negativeMask || !game::Has(sym_.nmSoftness) || !game::Has(sym_.nmRoundness) ||
            !game::Has(sym_.nmNoiseScale) || !game::Has(sym_.nmNoiseStrength) || !ugui::HasShaderGlobals()) {
            n.info = "skipped: NegativeMaskMap fields or Shader.SetGlobalMatrix / matrix getters not resolved";
            return;
        }
        if (negMaskId_ == -1) negMaskId_ = ugui::ShaderPropertyId("_NegativeMask");
        if (negMaskId_ == -1) {
            n.info = "skipped: Shader.PropertyToID(_NegativeMask) failed";
            return;
        }
        Il2CppObject* mapT = view_.mapT.get();
        int layer = ugui::GetLayer(ui_.root.get());
        n.info = "skipped: the page has no NegativeMaskMap";
        ugui::ForEachInChildren(prefab, sym_.negativeMask, true, [&](Il2CppObject* c) {
            Il2CppObject* t = unity::TransformOf(c);
            if (!t || !mapT) return false;
            Vec3 o = ugui::InverseTransformPoint(mapT, ugui::TransformPoint(t, {0, 0, 0}));
            auto axis = [&](Vec3 u) {
                Vec3 p = ugui::InverseTransformPoint(mapT, ugui::TransformPoint(t, u));
                return Vec3{p.x - o.x, p.y - o.y, p.z - o.z};
            };
            Vec3 ax = axis({1, 0, 0}), ay = axis({0, 1, 0}), az = axis({0, 0, 1});
            float sx = std::hypot(ax.x, ax.y), sy = std::hypot(ay.x, ay.y);
            float sz = std::sqrt(az.x * az.x + az.y * az.y + az.z * az.z);
            if (!std::isfinite(o.x) || !std::isfinite(o.y) || !std::isfinite(sx) || !std::isfinite(sy) ||
                !(sx > 1e-3f) || !(sy > 1e-3f)) {
                n.info = "skipped: the NegativeMaskMap transform is degenerate";
                return false;
            }
            if (!std::isfinite(sz) || !(sz > 1e-3f)) sz = 1;
            float angle = std::atan2(ax.y, ax.x) * 180.0f / kPi;
            bool mirrored = (ax.x * ay.y - ax.y * ay.x) < 0;
            Il2CppObject* go = Child(view_.page.get(), "NegativeMask", layer);
            Il2CppObject* gt = go ? unity::TransformOf(go) : nullptr;
            if (!ugui::IsRectTransform(gt)) {
                n.info = "skipped: could not create the mask object";
                return false;
            }
            ugui::Center(gt, {0, 0});
            ugui::SetAnchoredPosition(gt, {o.x, o.y});
            ugui::SetLocalEuler(gt, {0, 0, angle});
            ugui::SetLocalScale(gt, {sx, mirrored ? -sy : sy, sz});
            n.t.Reset(gt);
            n.softness = F<float>(c, sym_.nmSoftness);
            n.roundness = F<float>(c, sym_.nmRoundness);
            n.noiseScale = F<float>(c, sym_.nmNoiseScale);
            n.noiseStrength = F<float>(c, sym_.nmNoiseStrength);
            n.ok = true;
            n.info = std::format("mask = our copy of NegativeMaskMap '{}' at page ({:.0f}, {:.0f}), {:.0f} x {:.0f}, "
                                 "{:.1f} deg (softness {:.3f}, roundness {:.2f}, edge noise {:.2f} / {:.2f}); "
                                 "Shader.SetGlobalMatrix(_NegativeMask) every frame while shown",
                                 unity::GetName(unity::GameObjectOf(c)), o.x, o.y, sx, sy, angle, n.softness,
                                 n.roundness, n.noiseScale, n.noiseStrength);
            return false;  // one per page
        });
    }

    void UpdateNegativeMask(float dt) {
        View::NegMask& n = view_.neg;
        // The full map's own NegativeMaskMap owns the global while it is open.
        if (!n.ok || n.copies == 0 || view_.staticFog.vis != 1 || mapOpen_ || !n.t.Alive()) {
            negDirty_ = true;  // set again as soon as it is ours to drive
            return;
        }
        // The matrix only changes when the page moves / turns / scales (negDirty_, from UpdateTransform) or
        // the canvas changes (re-resolved below); it is re-set at least every kNegRefresh s anyway, in case
        // something else changed the global.
        negAge_ += dt;
        if (!negDirty_ && n.canvasT.Alive() && negAge_ < kNegRefresh) return;
        negDirty_ = false;
        negAge_ = 0;
        ++pc_.negSets;
        if (!n.canvasT.Alive()) {
            // The shader sees vertices in the space of the canvas the geometry is batched in: the root canvas
            // (our nested Canvas has no overrideSorting). Overlay: shader position = canvas-local, so the
            // canvas matrix is applied as the game does; Camera / World: it already is the world position.
            Il2CppObject* c =
                ugui::CanvasRoot(ugui::ComponentInParent(unity::GameObjectOf(n.t.get()), ugui::CanvasClass()));
            if (!ugui::IsA(c, ugui::CanvasClass())) return;
            int mode = ugui::CanvasRenderMode(c);
            n.overlay = mode != 1 && mode != 2;
            n.canvasT.Reset(unity::TransformOf(c));
            if (!negCanvasLogged_) {
                negCanvasLogged_ = true;
                SR2_INFO("minimap: fog: outside-fog mask relative to root canvas '{}' (render mode {}: {})",
                         unity::GetName(unity::GameObjectOf(c)), mode,
                         n.overlay ? "worldToLocal(mask) * localToWorld(canvas), as the game" : "worldToLocal(mask)");
            }
        }
        ugui::Mat4 m{};
        if (!ugui::WorldToLocal(n.t.get(), m)) return;
        if (n.overlay) {
            ugui::Mat4 c{};
            if (!ugui::LocalToWorld(n.canvasT.get(), c)) return;
            m = ugui::Mul(m, c);
        }
        m.m[3] = n.noiseScale;
        m.m[7] = n.noiseStrength;
        m.m[11] = n.softness;
        m.m[15] = n.roundness;
        ugui::ShaderSetGlobalMatrix(negMaskId_, m);
    }

    // Fog images are SetActive per area: fully shown or fully hidden. The user opacity (CanvasGroup on
    // the minimap root) does not reach fog-shader copies: each has its own CanvasGroup with
    // ignoreParentGroups, because those shaders treat vertex alpha as the reveal threshold.
    void ApplyFogVisibility() {
        Group& st = view_.staticFog;  // static fog: as in the full map, unless the fog is ignored
        int svis = cfg_.respectFog ? 1 : 0;
        if (svis != st.vis) {
            st.vis = svis;
            SetGroup(st, svis != 0);
        }
        for (Group& f : view_.fog) {
            int vis = (cfg_.respectFog && f.state != 1) ? 1 : 0;  // unknown counts as unrevealed
            if (vis != f.vis) {
                f.vis = vis;
                SetGroup(f, vis != 0);
            }
        }
    }

    void UpdateDynamic() {
        if (!view_.ok) return;
        for (Group& d : view_.dyn) {
            bool show = true;
            if (sym_.dynCanBeShown && d.comp.Alive())
                if (!Managed([&] { show = ugui::Call<bool>(sym_.dynCanBeShown, d.comp.get()); })) show = true;
            int s = show ? 1 : 0;
            if (s != d.state) {
                d.state = s;
                SetGroup(d, show);
            }
        }
    }

    bool UnderFog(Vec2 p) const {
        // the page's fog element(s) whose shape contains p; under fog while that area is not revealed
        // (unrevealed or still unknown)
        for (const Group& f : view_.fog)
            if (f.state != 1 && f.Contains(p)) return true;
        return false;
    }

    // ------------------------------------------------------------ per frame ---
    float CurrentScale() const {
        float rv = ui_.size * 0.5f;
        float rw = kBaseRadiusMeters / std::max(0.1f, cfg_.zoom);
        return rv / (rw * view_.unitsPerMeter);
    }

    void UpdateTransform() {
        if (!view_.ok || !playerT_.Alive() || !view_.page.Alive()) return;
        Vec3 wp = ugui::GetPosition(playerT_.get());
        ugui::Quat q = ugui::GetRotation(playerT_.get());
        playerP_ = ProjectPage(wp);
        float s = CurrentScale();
        ++pc_.frames;
        // Only real changes: every Transform set dirties the minimap canvas (its batch is rebuilt by Unity
        // later in the frame, outside our timing), even with the same value. Standing still = no page sets.
        bool pageMoved = false;
        if (s != pageS_) {
            ugui::SetLocalScale(view_.page.get(), {s, s, 1});
            pageS_ = s;
            pageMoved = true;
        }
        Vec2 pp{-playerP_.x * s, -playerP_.y * s};
        if (pp.x != pageP_.x || pp.y != pageP_.y) {
            ugui::SetLocalPosition(view_.page.get(), {pp.x, pp.y, 0});
            pageP_ = pp;
            pageMoved = true;
        }

        // yaw like Quaternion.eulerAngles.y (forward = q * +Z)
        float fx = 2.0f * (q.x * q.z + q.w * q.y);
        float fz = 1.0f - 2.0f * (q.x * q.x + q.y * q.y);
        float yaw = std::atan2(fx, fz) * 180.0f / kPi;
        float rot = cfg_.orientation == 1 ? yaw : 0.0f;
        if (std::fabs(rot - lastR_) > 0.05f) {
            ugui::SetLocalEuler(ui_.rotator.get(), {0, 0, rot});
            lastR_ = rot;
            pageMoved = true;
        }
        if (pageMoved) {
            negDirty_ = true;  // the outside-fog mask follows the page
            ++pc_.pageSets;
        }
        UpdateArrow(rot, yaw, s);

        // Icons are counter-scaled / counter-rotated: one or two calls per visible icon. The rotation is
        // re-applied in kIconRotStep steps (an icon tilted < 0.5 deg is invisible), not on every 0.05 deg.
        bool scaleChanged = std::fabs(s - lastS_) > lastS_ * 0.005f;
        bool rotChanged = std::fabs(std::remainder(rot - lastIconR_, 360.0f)) > kIconRotStep || lastIconR_ > 1e8f;
        if (scaleChanged || rotChanged) {
            for (Marker& m : view_.markers)
                if (m.active && m.t.Alive()) Orient(m.t.get(), s, rot, scaleChanged, rotChanged);
            for (Icon& i : view_.icons)
                if (i.active && i.t.Alive()) Orient(i.t.get(), s, rot, scaleChanged, rotChanged);
            if (scaleChanged) lastS_ = s;
            if (rotChanged) lastIconR_ = rot;
            for (Icon& i : view_.icons) {  // what ApplyIcons compares against
                if (!i.active) continue;
                if (scaleChanged) i.oS = s;
                if (rotChanged) i.oR = rot;
            }
        }
    }

    // GTA-style: the arrow stays centred, the page moves/rotates under it. PlayerMapMarker.Rotate turns
    // _arrowRect to z = -(eulerY + _rotationOffset). Fixed offsets (the old slider's values, confirmed in
    // game): 0 for the game marker (plus the prefab's _rotationOffset, 0 in 1.3.0), 180 for the classic
    // arrow on top of the old +180 base of its texture = 0 in total: both point like the game's marker.
    // Rotate-with-player: rot == yaw, so the arrow points up.
    void UpdateArrow(float rot, float yaw, float s) {
        const bool game = UseGameArrow();
        float arrowZ = std::remainder(rot - yaw - (game ? gm_.rotOffset : 0.0f), 360.0f);
        Il2CppObject* turn = game ? ui_.gameFacingT.get() : ui_.arrowT.get();
        if (std::fabs(std::remainder(arrowZ - lastArrowZ_, 360.0f)) > 0.05f || lastArrowZ_ > 1e8f) {
            ugui::SetLocalEuler(turn, {0, 0, arrowZ});
            lastArrowZ_ = arrowZ;
        }
        if (!game) return;
        AnimateMarkerParts(ui_.gameParts);
        if (!view_.coneT) return;
        // The cone sits in the page (under the icons) where _arrowRect sits on screen: the player point plus
        // the facing origin (screen space, k px per prefab unit), turned with the heading, counter-scaled.
        float k = ui_.markerK, zc = std::remainder(arrowZ - rot, 360.0f);
        float rr = -rot * kPi / 180.0f, cs = std::cos(rr), sn = std::sin(rr);
        Vec2 o{gm_.facingOrigin.x * k / s, gm_.facingOrigin.y * k / s};
        Vec2 p{playerP_.x + cs * o.x - sn * o.y, playerP_.y + sn * o.x + cs * o.y};
        if (p.x != coneP_.x || p.y != coneP_.y || coneS_ < 0) {
            ugui::SetLocalPosition(view_.coneT.get(), {p.x, p.y, 0});
            coneP_ = p;
        }
        if (std::fabs(std::remainder(zc - coneZ_, 360.0f)) > 0.05f || coneS_ < 0) {
            ugui::SetLocalEuler(view_.coneT.get(), {0, 0, zc});
            coneZ_ = zc;
        }
        float cscale = k / s;
        if (std::fabs(cscale - coneS_) > coneS_ * 0.002f || coneS_ < 0) {
            ugui::SetLocalScale(view_.coneT.get(), {cscale, cscale, 1});
            coneS_ = cscale;
        }
        AnimateMarkerParts(view_.coneParts);
    }

    // Size factor of the arrow: 1 at zoom 2.5x, proportional to the zoom, clamped to 0.4x..1.6x.
    float ArrowZoomFactor() const { return std::clamp(cfg_.zoom / kArrowBaseZoom, 0.4f, 1.6f); }

    // Size factor of the icons: 1 at the default zoom 1x (their old fixed size), proportional to the
    // zoom like the arrow, clamped to 0.6x..1.75x so they neither vanish at 0.5x nor get huge at 4x.
    float IconZoomFactor() const { return std::clamp(cfg_.zoom / kIconBaseZoom, 0.6f, 1.75f); }

    // icons grow and shrink with the zoom (not with the page scale) and stay upright
    void Orient(Il2CppObject* t, float s, float rot, bool scale, bool rotate) {
        const float k = IconZoomFactor() / s;
        if (scale) ugui::SetLocalScale(t, {k, k, 1});
        if (rotate) ugui::SetLocalEuler(t, {0, 0, -rot});
        pc_.orients += (scale ? 1 : 0) + (rotate ? 1 : 0);
    }
    // The counter-rotation the visible icons currently have.
    float IconRot() const { return lastIconR_ < 1e8f ? lastIconR_ : 0.0f; }

    // ------------------------------------------------------------- markers ----
    float ViewRadiusPage() const { return ui_.size * 0.5f / std::max(1e-6f, CurrentScale()) * 1.3f; }

    // The game's own "would the full map show this" check:
    // IFilterableMapMarker.CanBeShown on the page's marker component, e.g.
    //   AncientTeleporterMarkerUI: discovery query / show query,
    //   GordoMarkerUI: gordo seen and not popped (GameModel),
    //   PuzzleSlotMarkerUI: discovered and not solved,
    //   StaticMapMarker: its DynamicMapElement's CanBeShown.
    // Queried round robin (kMarkerQueriesPerTick per 10 Hz tick, each marker at
    // most every kMarkerRecheck s). Unknown / failing -> hidden.
    // Fog ignored: only the "done" state hides a marker (popped gordo, solved plort door); discovery
    // (seen / discovered) no longer matters. Returns false if the state cannot be read.
    bool QueryNotDone(Marker& m, bool& notDone) {
        Il2CppObject* comp = m.comp.get();
        switch (m.kind) {
            case kMkTeleporter:  // discovery only: always shown when the fog is ignored
                notDone = true;
                return true;
            case kMkGordo: {
                if (!sym_.tryGetGameModel || !sym_.getGordoModel || !sym_.gordoHasPopped || !game::Has(sym_.gordoId))
                    return false;
                Il2CppObject* id = F<Il2CppObject*>(comp, sym_.gordoId);
                if (!ugui::PlausiblePointer(id)) return false;
                bool ok = false, popped = false;
                Managed([&] {
                    Il2CppObject* gm = nullptr;
                    if (!ugui::Call<bool>(sym_.tryGetGameModel, &gm) || !gm) return;
                    Il2CppObject* model = ugui::Call<Il2CppObject*>(sym_.getGordoModel, gm, id);
                    if (!model) {  // no model for this id: never fed, not popped
                        ok = true;
                        return;
                    }
                    if (!ugui::IsA(model, sym_.gordoModel)) return;
                    popped = ugui::Call<bool>(sym_.gordoHasPopped, model);  // eat count == -1
                    ok = true;
                });
                notDone = !popped;
                return ok;
            }
            case kMkPuzzle: {
                if (!sym_.puzzleIsSolved) return false;
                bool solved = false;
                if (!Managed([&] { solved = ugui::Call<bool>(sym_.puzzleIsSolved, comp); })) return false;
                notDone = !solved;
                return true;
            }
            default:  // static markers etc.: their CanBeShown is a world state (DynamicMapElement), not discovery
                if (!m.canBeShown) {
                    notDone = true;
                    return true;
                }
                return Managed([&] { notDone = ugui::Call<bool>(m.canBeShown, comp); });
        }
    }

    void QueryMarkers() {
        size_t n = view_.markers.size();
        if (!n) return;
        int budget = kMarkerQueriesPerTick;
        for (size_t step = 0; step < n && budget > 0; ++step) {
            Marker& m = view_.markers[markerRR_ % n];
            ++markerRR_;
            if (!m.go || !cfg_.icons[m.cat] || time_ < m.nextCheck) continue;
            m.nextCheck = time_ + kMarkerRecheck;
            if (m.broken) {
                m.can = false;
                m.queried = true;
                continue;
            }
            if (cfg_.respectFog && !m.canBeShown) {  // no game check bound: the game always shows it (fog still applies)
                m.can = true;
                m.queried = true;
                continue;
            }
            if (!m.comp.Alive()) {
                m.can = false;
                m.queried = true;
                continue;
            }
            --budget;
            bool can = false;
            bool answered = cfg_.respectFog ? Managed([&] { can = ugui::Call<bool>(m.canBeShown, m.comp.get()); })
                                            : QueryNotDone(m, can);
            if (!answered && !cfg_.respectFog && m.canBeShown)  // done state unreadable: the game's own check
                answered = Managed([&] { can = ugui::Call<bool>(m.canBeShown, m.comp.get()); });
            if (answered) {
                m.can = can;
                m.fails = 0;
            } else {
                m.can = false;
                if (++m.fails >= kMarkerMaxFailures) m.broken = true;  // keeps failing: hidden for this build
            }
            m.queried = true;
        }
        if (!markerStatsLogged_) {
            int total[kCatCount] = {}, shown[kCatCount] = {}, failed = 0;
            for (const Marker& m : view_.markers) {
                if (!m.go || !cfg_.icons[m.cat]) continue;
                if (!m.queried) return;  // first pass not complete yet
                ++total[m.cat];
                if (m.can) ++shown[m.cat];
                if (m.fails > 0 || m.broken) ++failed;
            }
            markerStatsLogged_ = true;
            std::string parts;
            for (int c = 0; c < kCatCount; ++c)
                if (total[c]) parts += std::format("{}{} {}/{}", parts.empty() ? "" : ", ", kCatName[c], shown[c], total[c]);
            SR2_INFO("minimap: marker visibility ({}): {} shown ({} query failures -> hidden)",
                     cfg_.respectFog ? "fog respected: the game's CanBeShown + fog areas"
                                     : "fog ignored: done state only - popped gordos / solved plort doors hidden",
                     parts.empty() ? std::string("no markers enabled") : parts, failed);
        }
    }

    void UpdateMarkers() {
        if (!view_.ok) return;
        QueryMarkers();
        float vr = ViewRadiusPage();
        float s = CurrentScale();
        float rot = IconRot();
        for (Marker& m : view_.markers) {
            if (!m.go) continue;
            bool show = cfg_.icons[m.cat] && std::hypot(m.pos.x - playerP_.x, m.pos.y - playerP_.y) <= vr;
            if (show) show = m.queried && m.can && !(cfg_.respectFog && UnderFog(m.pos));
            if (show != m.active && m.go.Alive()) {
                unity::SetActive(m.go.get(), show);
                m.active = show;
                if (show) Orient(m.t.get(), s, rot, true, true);
            }
        }
    }

    void HideIcons(size_t from = 0) {
        for (size_t i = from; i < view_.icons.size(); ++i) {
            Icon& ic = view_.icons[i];
            if (ic.active && ic.go.Alive()) unity::SetActive(ic.go.get(), false);
            ic.active = false;
        }
    }

    struct IconItem {
        Vec2 pos;
        Il2CppObject* sprite;
        Color tint;
        int cat = kOther;
    };
    // What the scanners collect: the minimap's own settings and icon budget, or a caller's
    // (CollectPageIcons, the full map's icons through api.h).
    const bool* scanCats_ = nullptr;
    size_t scanMax_ = kMaxIcons;

    Il2CppObject* PodIcon() {
        if (sprPod_.Alive()) return sprPod_.get();
        if (podIconTried_ || !mapDirector_.Alive()) return nullptr;
        podIconTried_ = true;
        if (game::Has(sym_.mdCounters) && game::Has(sym_.counterIcon) && sym_.podCounterDef) {
            Il2CppObject* list = F<Il2CppObject*>(mapDirector_.get(), sym_.mdCounters);
            ugui::ForEachListStructRef(list, "Counter", [&](Il2CppObject* counter) {
                if (!ugui::IsA(counter, sym_.podCounterDef)) return true;
                Il2CppObject* icon = F<Il2CppObject*>(counter, sym_.counterIcon);
                if (!ugui::IsA(icon, ugui::SpriteClass())) return true;
                sprPod_.Reset(icon);
                return false;
            });
        }
        SR2_INFO("minimap: treasure pod icon {}", sprPod_ ? unity::GetName(sprPod_.get()) : std::string("not found - dot"));
        return sprPod_.get();
    }

    void ScanIcons() {
        if (!view_.ok || !view_.page.Alive()) return;
        bool pods = cfg_.icons[kTreasurePod] && !view_.hasPagePods && sym_.treasurePod && sym_.podGetState;
        bool nodes = cfg_.icons[kResource] && !view_.hasPageNodes && sym_.allNodeDirectors && game::Has(sym_.dirSpawners) &&
                     game::Has(sym_.spawnerModel) && game::Has(sym_.modelNodeState);
        bool gadgets = cfg_.icons[kPlaced] && sym_.tryGetGameModel && sym_.gameModel && sym_.gadgetModel &&
                       game::Has(sym_.gmIdentsByIdent) && game::Has(sym_.imLastPos) && game::Has(sym_.imIdent);
        bool nullWalls = cfg_.icons[kNullWall] && walls::Resolved();
        std::vector<IconItem> items;
        scanCats_ = cfg_.icons;
        scanMax_ = kMaxIcons;
        float vr = ViewRadiusPage() * 1.5f;
        stats_ = IconStats{};
        bool ignoreFog = false;  // placed gadgets: the player put them there
        auto inView = [&](Vec3 wp, Vec2& p) {
            p = ProjectPage(wp, false);
            if (!OnPage(p)) return false;
            if (std::hypot(p.x - playerP_.x, p.y - playerP_.y) > vr) return false;
            if (cfg_.respectFog && !ignoreFog && UnderFog(p)) {  // inside an unrevealed fog area of the page
                ++stats_.fogged;
                return false;
            }
            return true;
        };
        if (pods) ScanPods(items, inView);
        if (nullWalls) ScanNullWalls(items, inView);
        if (gadgets) ScanPlacedGadgets(items, inView, ignoreFog);
        if (nodes) ScanNodes(items, inView);
        ApplyIcons(items);
        if (gadgets) {  // low volume: one line whenever the placed-gadget counts change
            const IconStats& t = stats_;
            int sig = ((t.gDrone * 31 + t.gTele) * 31 + t.gDecor) * 31 + t.gOther + t.gPre * 7919 + t.gDroneRanch * 104729;
            if (sig != lastGadgetSig_) {
                lastGadgetSig_ = sig;
                SR2_INFO("minimap: placed gadgets (GameModel): {} total - drone ports {} (ranch {}, explorer {}), "
                         "teleporters/warp depots {}, decorations {}{}, other {}; {} pre-placed by the level (fog "
                         "applies to those); {} in view now",
                         t.gDrone + t.gTele + t.gDecor + t.gOther, t.gDrone, t.gDroneRanch, t.gDrone - t.gDroneRanch,
                         t.gTele, t.gDecor, cfg_.icons[kPlacedDecor] ? "" : " (hidden)", t.gOther, t.gPre, t.gShown);
            }
        }
        if (!iconsLogged_) {
            iconsLogged_ = true;
            SR2_INFO("minimap: icons (first scan): pods {} shown of {} registered; resource nodes {} READY spawners, {} "
                     "with a live node, {} shown{}; Nullifier walls {} intact / {} active, {} shown{}; {} icons hidden "
                     "under unrevealed fog; {} total",
                     stats_.podsShown, g_pods.size(), stats_.nodesReady, stats_.nodesLive, stats_.nodesShown,
                     nodes ? "" : " (off)", stats_.wallsIntact, stats_.wallsActive, stats_.wallsShown,
                     nullWalls ? "" : " (off)", stats_.fogged, items.size());
        }
    }

    struct IconStats {
        int podsShown = 0, nodesReady = 0, nodesLive = 0, nodesShown = 0, fogged = 0;
        int wallsActive = 0, wallsIntact = 0, wallsShown = 0, wallsInactive = 0, wallsFromCache = 0;
        int gDrone = 0, gDroneRanch = 0, gTele = 0, gDecor = 0, gOther = 0, gPre = 0, gShown = 0;
    };
    IconStats stats_;

    template <class Near>
    void ScanPods(std::vector<IconItem>& items, Near& nearFn) {
        if (!podsSeeded_ && sym_.podAwake) {
            // Pods that woke before the module was enabled: one scene query per enable.
            podsSeeded_ = true;
            ugui::ForEachSceneObject(sym_.treasurePod, false, [&](Il2CppObject* pod) {
                if (!g_pods.count(pod))
                    if (Il2CppGCHandle h = unity::NewGCHandle(pod, false)) g_pods.emplace(pod, h);
                return true;
            });
            SR2_DEBUG("minimap: pod registry seeded ({} pods)", g_pods.size());
        }
        Il2CppObject* icon = PodIcon();
        // Pods never move: their world position is read once per pod (two calls), so a pod far from the
        // player costs no game call at all; only pods in view are checked for active + LOCKED.
        for (auto& [pod, h] : g_pods) {
            if (items.size() >= scanMax_) break;
            if (!ugui::IsA(pod, sym_.treasurePod)) continue;
            auto pp = podPos_.find(pod);
            if (pp == podPos_.end()) pp = podPos_.emplace(pod, ugui::GetPosition(unity::TransformOf(pod))).first;
            Vec2 p{};
            if (!nearFn(pp->second, p)) {
                ++pc_.podsFar;
                continue;
            }
            ++pc_.podsNear;
            if (!unity::ActiveInHierarchy(pod)) continue;
            int state = 1;
            if (!Managed([&] { state = ugui::Call<int>(sym_.podGetState, pod); }) || state != 0 /* LOCKED */) continue;
            ++stats_.podsShown;  // opened pods never show, whatever the fog setting
            items.push_back({p, icon ? icon : sprDot_.get(), icon ? Color{1, 1, 1, 1} : Color{1.0f, 0.78f, 0.25f, 1},
                             kTreasurePod});
        }
    }

    // Nullifier walls (DiscordantWall): the game's static registry, active walls only; harmonized
    // ("opened") walls are not shown. Fog applies through nearFn like every world icon.
    template <class Near>
    void ScanNullWalls(std::vector<IconItem>& items, Near& nearFn) {
        wallBuf_.clear();
        walls::Stats st;
        if (!walls::Collect(wallBuf_, &st, wallsInactive_)) {
            if (!wallLayoutWarned_) {
                wallLayoutWarned_ = true;
                SR2_WARN("minimap: DiscordantWall._activeWalls has an unexpected type - no Nullifier wall icons");
            }
            return;
        }
        stats_.wallsActive = st.active;
        stats_.wallsIntact = st.intact;
        stats_.wallsInactive = st.inactive;
        stats_.wallsFromCache = st.fromCache;
        Il2CppObject* icon = sprWall_.Alive() ? sprWall_.get() : sprDot_.get();
        for (const walls::Wall& w : wallBuf_) {
            if (items.size() >= scanMax_) break;
            if (!w.intact) continue;
            Vec2 p{};
            if (!nearFn(Vec3{w.x, w.y, w.z}, p)) continue;
            items.push_back({p, icon, Color{0.88f, 0.40f, 1.0f, 1}, kNullWall});
            ++stats_.wallsShown;
        }
    }
    std::vector<walls::Wall> wallBuf_;
    bool wallsInactive_ = false;  // full map scan: inactive walls too (walls::Collect includeInactive)
    int pageWallsLogged_ = 0;     // last wall count logged for the full map
    std::unordered_map<Il2CppObject*, Vec3> podPos_;  // registered pod -> world position (pods never move)
    bool wallLayoutWarned_ = false;

    template <class Near>
    void ScanNodes(std::vector<IconItem>& items, Near& nearFn) {
        Il2CppObject* directors = nullptr;
        if (!Managed([&] { directors = ugui::Call<Il2CppObject*>(sym_.allNodeDirectors); }) || !directors) return;
        Ref pin(directors);
        for (size_t d = 0, nd = unity::ListCount(directors); d < nd; ++d) {
            auto* dir = static_cast<Il2CppObject*>(unity::ListAt(directors, d));
            if (!ugui::IsA(dir, sym_.nodeDirector)) continue;
            Il2CppObject* spawners = F<Il2CppObject*>(dir, sym_.dirSpawners);
            for (size_t i = 0, ns = unity::ListCount(spawners); i < ns && items.size() < scanMax_; ++i) {
                auto* sp = static_cast<Il2CppObject*>(unity::ListAt(spawners, i));
                if (!ugui::IsA(sp, sym_.nodeSpawner)) continue;
                Il2CppObject* model = F<Il2CppObject*>(sp, sym_.spawnerModel);
                if (!ugui::IsA(model, sym_.nodeModel) || F<int>(model, sym_.modelNodeState) != 1 /* READY */) continue;
                ++stats_.nodesReady;
                // Only a node that exists right now: the spawner's spawned ResourceNode object, alive and
                // active, at ITS position. A READY model alone can be an empty spawn point (no node object,
                // or one in an unloaded / deactivated cell).
                Il2CppObject* node = nullptr;
                if (game::Has(sym_.spawnerNode) && sym_.resourceNode) {
                    node = F<Il2CppObject*>(sp, sym_.spawnerNode);
                    if (!ugui::PlausiblePointer(node) || !ugui::IsA(node, sym_.resourceNode) ||
                        !unity::ActiveInHierarchy(unity::GameObjectOf(node)))
                        continue;
                }
                Il2CppObject* resources =
                    game::Has(sym_.modelResources) ? F<Il2CppObject*>(model, sym_.modelResources) : nullptr;
                if (game::Has(sym_.modelResources) && (!ugui::PlausiblePointer(resources) || unity::ListCount(resources) == 0))
                    continue;  // nothing left to harvest
                ++stats_.nodesLive;
                Vec2 p{};
                if (!nearFn(ugui::GetPosition(unity::TransformOf(node ? node : sp)), p)) continue;
                Il2CppObject* icon = nullptr;
                if (resources && game::Has(sym_.identIcon)) {
                    auto* ident = static_cast<Il2CppObject*>(unity::ListAt(resources, 0));
                    if (ugui::IsA(ident, sym_.identType)) icon = F<Il2CppObject*>(ident, sym_.identIcon);
                    if (!ugui::IsA(icon, ugui::SpriteClass())) icon = nullptr;
                }
                items.push_back({p, icon ? icon : sprDot_.get(), icon ? Color{1, 1, 1, 1} : Color{0.45f, 0.85f, 1.0f, 1},
                                 kResource});
                ++stats_.nodesShown;
            }
        }
    }

    // `wantIcon`: also read the map icon (drone stations: the fallback icon). A descriptor (gadget
    // definition) always belongs to the same kind of source, so the cached answer stays valid.
    const DescInfo& Describe(Il2CppObject* desc, bool wantIcon = false) {
        auto it = descCache_.find(desc);
        if (it != descCache_.end()) return it->second;
        DescInfo info;
        info.pin.Reset(desc);
        if (unity::IsAlive(desc)) {
            info.teleporter = Contains(Lower(unity::GetName(desc)), "teleport");
            if ((info.teleporter || wantIcon) && sym_.descMapIcon) {
                ugui::Fn f = ugui::BindFn(ugui::VirtualMethod(desc, sym_.descMapIcon));
                Il2CppObject* icon = nullptr;
                if (f) Managed([&] { icon = ugui::Call<Il2CppObject*>(f, desc); });
                if (ugui::IsA(icon, ugui::SpriteClass())) info.icon.Reset(icon);
            }
        }
        return descCache_.emplace(desc, std::move(info)).first->second;
    }

    // The full map's drone-type sprite (DroneStationMapMarker.UpdateIcon: _image.sprite =
    // _displayConfiguration.PickDroneTypeSprite(model._type), disassembly 0x704740 / 0x8247E0), read from the
    // page's Map._markerPrefabMapping._droneStationPrefab. Field reads only; retried every 10 s while missing
    // (the prefab lives in the preloaded map UI scene).
    Il2CppObject* DroneTypeIcon(int type) {
        if (type != 0 && type != 1) return nullptr;
        Ref& slot = sprDroneType_[type];
        if (slot.Alive()) return slot.get();
        if (time_ < droneIconRetry_[type]) return nullptr;
        droneIconRetry_[type] = time_ + 10.0f;
        if (!game::Has(sym_.mapMarkerMapping) || !game::Has(sym_.mpmDronePrefab) || !sym_.droneStationMarker ||
            !view_.mapComp.Alive())
            return nullptr;
        Il2CppObject* mpm = F<Il2CppObject*>(view_.mapComp.get(), sym_.mapMarkerMapping);
        if (!ugui::IsA(mpm, sym_.markerPrefabMapping)) return nullptr;
        Il2CppObject* prefab = F<Il2CppObject*>(mpm, sym_.mpmDronePrefab);
        if (!ugui::IsA(prefab, unity::GameObjectClass())) return nullptr;
        Il2CppObject* dsm = unity::GetComponent(prefab, sym_.droneStationMarker);
        if (!ugui::IsA(dsm, sym_.droneStationMarker))
            ugui::ForEachInChildren(prefab, sym_.droneStationMarker, true, [&](Il2CppObject* c) {
                dsm = c;
                return false;
            });
        if (!ugui::IsA(dsm, sym_.droneStationMarker)) return nullptr;
        Il2CppObject* sprite = nullptr;
        const char* from = "";
        if (game::Has(sym_.dsmDisplayConfig) && sym_.droneDisplayConfig) {
            Il2CppObject* conf = F<Il2CppObject*>(dsm, sym_.dsmDisplayConfig);
            size_t off = type == 1 ? sym_.ddcExplorerIcon : sym_.ddcRancherIcon;
            if (ugui::IsA(conf, sym_.droneDisplayConfig) && game::Has(off)) {
                sprite = F<Il2CppObject*>(conf, off);
                from = "drone type display configuration";
            }
        }
        if (!ugui::IsA(sprite, ugui::SpriteClass()) && game::Has(sym_.mapMarkerImage)) {  // the prefab's own image
            sprite = nullptr;
            Il2CppObject* img = F<Il2CppObject*>(dsm, sym_.mapMarkerImage);
            if (ugui::IsA(img, ugui::ImageClass())) sprite = ugui::ImageGetSprite(img);
            from = "marker prefab image";
        }
        if (!ugui::IsA(sprite, ugui::SpriteClass()) || !unity::IsAlive(sprite)) return nullptr;
        slot.Reset(sprite);
        if (!droneIconLogged_[type]) {
            droneIconLogged_[type] = true;
            SR2_INFO("minimap: drone port icon ({}) '{}' from the {}", type == 1 ? "explorer" : "ranch",
                     unity::GetName(sprite), from);
        }
        return slot.get();
    }

    // Gadget definition -> group + icon (cached). GadgetDefinition.Type alone does not separate the groups: the
    // game's decorations are all DEFAULT (0) with no groupType and no "deco" in the asset name (ConservatoryLamp,
    // SimpleChair, RockClump, ...; log of 1.3.0), and a variant gadget may leave Type at DEFAULT while its base
    // (VariantOf) carries it. So: Type of the gadget, else of its base; DRONE_STATION (10) -> drone port,
    // TELEPORTER (1) / WARP_DEPOT (2) -> teleporter; else names: "dronestation" -> drone port, "teleport" /
    // "warpdepot" / "marketlink" / "refinerylink" -> teleporter (the market and refinery link terminals are
    // DEFAULT gadgets, MarketLink / RefineryLink with iconGadgetMarketLink_small / iconGadgetRefineryLink_small,
    // log of 1.3.0; they count as warp depots), "decor" in the asset, group or icon names (the decorations' icons are all
    // iconDecor*, e.g. iconDecorSingleChair_small; SpringPad is iconGadgetSpringPad_small) -> decoration;
    // anything else -> other. Each definition name is logged once per session with its group.
    const GadgetInfo& DescribeGadget(Il2CppObject* def, std::string& newDefs) {
        auto it = gadgetCache_.find(def);
        if (it != gadgetCache_.end()) return it->second;
        GadgetInfo info;
        info.pin.Reset(def);
        int type = -1, baseType = -1;
        std::string name = unity::GetName(def), groupName, iconNames;
        if (ugui::IsA(def, sym_.gadgetDef)) {
            if (game::Has(sym_.gdType)) type = F<int>(def, sym_.gdType);
            if (game::Has(sym_.gdVariantOf) && game::Has(sym_.gdType)) {
                Il2CppObject* base = F<Il2CppObject*>(def, sym_.gdVariantOf);
                if (base != def && ugui::IsA(base, sym_.gadgetDef) && unity::IsAlive(base))
                    baseType = F<int>(base, sym_.gdType);
            }
            if (game::Has(sym_.itGroup)) {
                Il2CppObject* g = F<Il2CppObject*>(def, sym_.itGroup);
                if (ugui::PlausiblePointer(g) && unity::IsAlive(g)) groupName = unity::GetName(g);
            }
            Il2CppObject* mapIcon = game::Has(sym_.gdMapIcon) ? F<Il2CppObject*>(def, sym_.gdMapIcon) : nullptr;
            Il2CppObject* pediaIcon = game::Has(sym_.identIcon) ? F<Il2CppObject*>(def, sym_.identIcon) : nullptr;
            if (!ugui::IsA(mapIcon, ugui::SpriteClass())) mapIcon = nullptr;
            if (!ugui::IsA(pediaIcon, ugui::SpriteClass())) pediaIcon = nullptr;
            if (Il2CppObject* icon = mapIcon ? mapIcon : pediaIcon) info.icon.Reset(icon);
            if (mapIcon) iconNames += unity::GetName(mapIcon);
            if (pediaIcon) iconNames += " " + unity::GetName(pediaIcon);
        }
        int t = type > 0 ? type : baseType;
        std::string ln = Lower(name), lg = Lower(groupName), li = Lower(iconNames);
        auto named = [&](const char* k) { return Contains(ln, k) || Contains(li, k); };
        if (t == 10 || (t <= 0 && named("dronestation"))) info.group = kDronePort;
        else if (t == 1 || t == 2 ||
                 (t <= 0 && (named("teleport") || named("warpdepot") || named("marketlink") || named("refinerylink"))))
            info.group = kPlacedTele;
        else if (t <= 0 && (named("decor") || Contains(lg, "decor"))) info.group = kPlacedDecor;
        else info.group = kPlaced;
        if (loggedDefs_.insert(name).second) {
            if (!newDefs.empty()) newDefs += "; ";
            newDefs += std::format("{}[type {}{}, group '{}', {}, icon '{}']", name, type,
                                   baseType >= 0 ? std::format(", base type {}", baseType) : std::string(), groupName,
                                   kCatName[info.group],
                                   info.icon.Alive() ? unity::GetName(info.icon.get()) : std::string("-"));
        }
        return gadgetCache_.emplace(def, std::move(info)).first->second;
    }

    // Every placed gadget, from the save's data model (GameModel.identifiablesByIdent: the lists whose models are
    // GadgetModels), at IdentifiableModel.lastPosition (= GadgetModel's map position). Field reads only, every
    // kScanInterval; no scene scan. Player-placed gadgets ignore the fog; gadgets pre-placed by the level
    // (GadgetModel._isPrePlaced) follow it like other world objects. Only gadgets in the player's scene group.
    // Drone ports and teleporters are listed before other gadgets, decorations last (kMaxIcons).
    template <class Near>
    void ScanPlacedGadgets(std::vector<IconItem>& items, Near& nearFn, bool& ignoreFog) {
        Il2CppObject* gm = nullptr;
        if (!Managed([&] {
                if (!ugui::Call<bool>(sym_.tryGetGameModel, &gm)) gm = nullptr;
            }) ||
            !ugui::IsA(gm, sym_.gameModel))
            return;
        Ref gmPin(gm);
        Il2CppObject* byIdent = F<Il2CppObject*>(gm, sym_.gmIdentsByIdent);
        if (!ugui::PlausiblePointer(byIdent)) return;
        Il2CppObject* playerScene = nullptr;
        if (game::Has(sym_.gmPlayer) && game::Has(sym_.imSceneGroup)) {
            Il2CppObject* pm = F<Il2CppObject*>(gm, sym_.gmPlayer);
            if (ugui::IsA(pm, sym_.identModel)) playerScene = F<Il2CppObject*>(pm, sym_.imSceneGroup);
        }
        std::vector<IconItem> bucket[3];  // 0 drone ports + teleporters, 1 other, 2 decorations
        std::string newDefs;
        ugui::ForEachDictValue(byIdent, [&](Il2CppObject* list) {
            size_t n = unity::ListCount(list);
            if (n == 0) return true;
            // one identifiable type per list: a list of non-gadgets is skipped after one class check
            if (!ugui::IsA(static_cast<Il2CppObject*>(unity::ListAt(list, 0)), sym_.gadgetModel)) return true;
            for (size_t i = 0; i < n; ++i) {
                auto* model = static_cast<Il2CppObject*>(unity::ListAt(list, i));
                if (!ugui::IsA(model, sym_.gadgetModel)) continue;
                Il2CppObject* def = F<Il2CppObject*>(model, sym_.imIdent);
                if (!ugui::IsA(def, sym_.identType)) continue;
                const GadgetInfo& info = DescribeGadget(def, newDefs);
                bool drone = info.group == kDronePort || (sym_.droneStationModel && ugui::IsA(model, sym_.droneStationModel));
                // the model class settles teleporters / warp depots whatever the definition says
                bool tele = (sym_.teleporterModel && ugui::IsA(model, sym_.teleporterModel)) ||
                            (sym_.warpDepotModel && ugui::IsA(model, sym_.warpDepotModel));
                int group = drone ? kDronePort : tele ? kPlacedTele : info.group;
                bool pre = game::Has(sym_.gmdPrePlaced) && F<bool>(model, sym_.gmdPrePlaced);
                int droneType = -1;
                switch (group) {
                    case kDronePort:
                        ++stats_.gDrone;
                        if (sym_.droneStationModel && game::Has(sym_.dsgmType) && ugui::IsA(model, sym_.droneStationModel))
                            droneType = F<int>(model, sym_.dsgmType);
                        if (droneType == 0) ++stats_.gDroneRanch;
                        break;
                    case kPlacedTele: ++stats_.gTele; break;
                    case kPlacedDecor: ++stats_.gDecor; break;
                    default: ++stats_.gOther; break;
                }
                if (pre) ++stats_.gPre;
                if (group != kPlaced && !scanCats_[group]) continue;
                if (playerScene && game::Has(sym_.imSceneGroup) && F<Il2CppObject*>(model, sym_.imSceneGroup) != playerScene)
                    continue;
                Vec3 wp = F<Vec3>(model, sym_.imLastPos);
                if (!std::isfinite(wp.x) || !std::isfinite(wp.y) || !std::isfinite(wp.z)) continue;
                Vec2 p{};
                ignoreFog = !pre;
                bool visible = nearFn(wp, p);
                ignoreFog = false;
                if (!visible) continue;
                Il2CppObject* icon = nullptr;
                Color tint{1, 1, 1, 1};
                if (group == kDronePort) {
                    icon = DroneTypeIcon(droneType);  // the full map's drone-type sprite
                    if (!icon && info.icon.Alive()) icon = info.icon.get();
                    if (!icon) {
                        icon = sprDronePort_.Alive() ? sprDronePort_.get() : sprDot_.get();
                        tint = {0.35f, 0.90f, 0.75f, 1};
                    }
                } else if (info.icon.Alive()) {
                    icon = info.icon.get();
                } else {
                    icon = sprGadget_.Alive() ? sprGadget_.get() : sprDot_.get();
                    tint = group == kPlacedTele ? Color{0.70f, 0.55f, 1.0f, 1} : Color{0.95f, 0.80f, 0.45f, 1};
                }
                int b = (group == kDronePort || group == kPlacedTele) ? 0 : group == kPlacedDecor ? 2 : 1;
                if (bucket[b].size() < scanMax_) bucket[b].push_back({p, icon, tint, group});
            }
            return true;
        });
        for (auto& bk : bucket)
            for (const IconItem& it : bk) {
                if (items.size() >= scanMax_) break;
                items.push_back(it);
                ++stats_.gShown;
            }
        // each definition name once per session, in chunks: the full list is the classification's evidence
        for (size_t at = 0; at < newDefs.size();) {
            size_t end = newDefs.size();
            if (end - at > 1500) {
                end = newDefs.find("; ", at + 1500);
                if (end == std::string::npos) end = newDefs.size();
            }
            SR2_INFO("minimap: placed gadget kinds (new): {}", newDefs.substr(at, end - at));
            at = end < newDefs.size() ? end + 2 : end;
        }
    }

    void ApplyIcons(const std::vector<IconItem>& items) {
        int layer = ugui::GetLayer(ui_.root.get());
        float s = CurrentScale();
        float rot = IconRot();
        float px = IconPx();
        while (view_.icons.size() < items.size()) {
            Il2CppObject* go = Child(view_.page.get(), "Icon", layer);
            if (!go) break;
            Icon ic;
            ic.go.Reset(go);
            ic.t.Reset(unity::TransformOf(go));
            ic.img.Reset(AddImage(go));
            ugui::ImageSetPreserveAspect(ic.img.get(), true);
            ugui::Center(ic.t.get(), {px, px});
            ic.active = true;
            view_.icons.push_back(std::move(ic));
        }
        for (size_t i = 0; i < view_.icons.size(); ++i) {
            Icon& ic = view_.icons[i];
            if (!ic.go.Alive() || !ic.t.Alive()) continue;
            bool show = i < items.size() && items[i].sprite;
            if (show) {
                // Only what changed: most icons are the same object at the same place every 3 s, and each
                // set dirties the graphic (mesh + canvas batch rebuild).
                const IconItem& it = items[i];
                int sets = 0;
                if (it.sprite != ic.sprite) {
                    ugui::ImageSetSprite(ic.img.get(), it.sprite);
                    ic.sprite = it.sprite;
                    ++sets;
                }
                if (it.tint.r != ic.tint.r || it.tint.g != ic.tint.g || it.tint.b != ic.tint.b || it.tint.a != ic.tint.a) {
                    ugui::GraphicSetColor(ic.img.get(), it.tint);
                    ic.tint = it.tint;
                    ++sets;
                }
                if (it.pos.x != ic.pos.x || it.pos.y != ic.pos.y) {
                    ugui::SetAnchoredPosition(ic.t.get(), it.pos);
                    ic.pos = it.pos;
                    ++sets;
                }
                bool os = ic.oS != s, orr = ic.oR != rot;
                if (os || orr) {
                    Orient(ic.t.get(), s, rot, os, orr);
                    ic.oS = s;
                    ic.oR = rot;
                }
                pc_.iconSets += sets;
                if (!sets && !os && !orr) ++pc_.iconSame;
            }
            if (show != ic.active) {
                unity::SetActive(ic.go.get(), show);
                ic.active = show;
            }
        }
    }

    // --------------------------------------------- Grey Labyrinth indicator ---
    void UpdateLabel() {
        if (!ui_.label.Alive()) return;
        Il2CppObject* go = unity::GameObjectOf(ui_.label.get());
        if (!cfg_.showAltitude || !inLabyrinth_ || !playerT_.Alive()) {
            if (ugui::ActiveSelf(go)) unity::SetActive(go, false);
            lastAlt_ = 1 << 30;  // text is set again when the label reappears
            return;
        }
        // Single flat page for the Labyrinth (no per-floor art): show the altitude.
        int alt = static_cast<int>(std::lround(ugui::GetPosition(playerT_.get()).y));
        if (!ugui::ActiveSelf(go)) unity::SetActive(go, true);
        if (alt != lastAlt_) {
            lastAlt_ = alt;
            ugui::TmpSetText(ui_.label.get(), std::format("Alt {} m", alt));
        }
    }
};

// The module object is destroyed at process exit, possibly after the IL2CPP
// runtime has shut down; the impl (which owns GC handles) is therefore
// intentionally never deleted.
class MinimapModule final : public core::Module {
public:
    const char* Name() const override { return kModule; }
    bool DefaultEnabled() const override { return true; }
    bool Setup(core::ModuleContext& ctx) override { return impl_->Setup(ctx); }
    void OnEnable() override { impl_->OnEnable(); }
    void OnDisable() override { impl_->OnDisable(); }

private:
    Minimap* impl_ = new Minimap();  // leaked on purpose (see above)

public:
    Minimap* impl() const { return impl_; }
};

MinimapModule* g_minimapModule = nullptr;  // set by CreateMinimap (one instance per process)

}  // namespace

namespace minimap::api {
bool Get(Snapshot& out) {
    out = {};
    return g_minimapModule && g_minimapModule->impl()->ApiSnapshot(out);
}
bool CollectPageIcons(const PageIconQuery& query, std::vector<PageIcon>& out, Il2CppObject** mapDefinition) {
    out.clear();
    if (mapDefinition) *mapDefinition = nullptr;
    return g_minimapModule && g_minimapModule->impl()->ApiCollectPageIcons(query, out, mapDefinition);
}
}  // namespace minimap::api

std::unique_ptr<sr2::core::Module> CreateMinimap() {
    auto m = std::make_unique<MinimapModule>();
    g_minimapModule = m.get();
    return m;
}

}  // namespace sr2::modules
