// map_tools: extensions for the game's own full-screen map (MapUI). See NOTES.md for the research.
//
//   * Reveal everything: FogMapElement.SetRevealState(bool) is forced to "revealed" (hook; its only caller is
//     MapUIFogReveal.RevealKnownZones, which runs when a map page opens) and MapDirector.IsZoneHidden is
//     answered "not hidden" (hook; called only by the full map's MapUI / ZoneMarkerUI / LabyrinthZoneMarkerUI
//     LateUpdates). The fog-shader art outside every FogMapElement (the static fog covers and the outside
//     fog) is switched off (Behaviour.enabled) on the live page and switched back on afterwards. Display
//     only: nothing is written to the save; with the switch off the next map open shows the real fog again.
//   * Icons: the minimap's page icon scan (modules/minimap/api.h CollectPageIcons) drawn as our own uGUI
//     Images INSIDE the live map page (a container under MapUI._map, page-local coordinates = the page's
//     own space), so the game's pan / zoom / mask apply to them exactly. Their size stays constant on
//     screen (scale = root canvas scale / page scale, per frame). With "hide icons under fog" the container
//     sits right below the page's fog layer, so the game's fog covers them like the map art.
//   * Right-click teleport: MapClickPlaceMarker.OnPointerClick (the map surface's click handler, left
//     button = waypoint) is hooked to notice right clicks. The cursor's page point is the one the game's
//     MapUI.LateUpdate uses (-MapViewModel.CursorPosition), turned into a world position with the view
//     model's map / world rects (the inverse of Map.GetMapPosition) of the tab shown. In the player's zone
//     (MapUI.TryFindZoneAt, ZoneDetectionArea._zoneMapData) or a zone of the player's scene group: the shared
//     ground teleport (ground_teleport.h: far spots are streamed out, so their regions are loaded in place while
//     the player stays put, then he lands on a safe floor; TeleportablePlayer.TeleportTo, the game's in-scene
//     teleport; never while knocked out). With Reveal off, spots under the page's fog are refused (FogMapElement.
//     ShouldBeRevealed per fog area). In another discovered zone (any zone, the Labyrinth and the Sanctuary
//     included, with "Reveal everything" on; that switch also lists every map tab): the game's zone-to-zone travel
//     TeleportNetwork.TeleportToDestinationImpl into a node of the static teleport network that arrives in one
//     of the zone's scene groups (ZoneSceneGroupMapping._mappings), then the same ground teleport. The map closes
//     (UIDisplayManager.CloseDisplayByObjectInHierarchy). A refused click plays the map's "failed to place
//     waypoint" cue. A "[RMB] Teleport" hint is cloned from the map's input legend; every teleport and refusal
//     also shows as a drone-style HUD pop-up with the teleporter icon (custom_upgrades/sale_notify.h).
//
// Fault policy: hooks only record / flip an argument inside DetourGuards; every game call runs on the main
// thread under core::Try + a native-fault guard; a native fault disables the module for the session.

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <format>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
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
#include "modules/common/discordant_walls.h"
#include "modules/custom_upgrades/api.h"
#include "modules/custom_upgrades/sale_notify.h"
#include "modules/map_tools/ground_teleport.h"
#include "modules/minimap/api.h"
#include "modules/minimap/ugui.h"

namespace sr2::modules {
namespace {

namespace settings = sr2::core::settings;
namespace unity = sr2::core::unity;
namespace main_thread = sr2::core::main_thread;
namespace ugui = sr2::modules::minimap::ugui;
namespace mapi = sr2::modules::minimap::api;
namespace ground_teleport = sr2::modules::ground_teleport;
using il2cpp::Resolver;
using ugui::Color;
using ugui::Rect;
using ugui::Ref;
using ugui::Vec2;
using ugui::Vec3;

constexpr const char* kModule = "map_tools";
constexpr const char* kAsm = "Assembly-CSharp";
constexpr size_t kNoOff = static_cast<size_t>(-1);
inline bool Has(size_t off) { return off != kNoOff; }
constexpr float kIconRefresh = 2.0f;   // full map icons re-collected this often while the map is open
constexpr int kClosedTicks = 3;        // no MapUI.LateUpdate for this many frames = map closed
constexpr int kReapplyFrames = 4;      // re-assert the cloned legend hint's key / label for a few frames
constexpr float kHintDiagDelay = 3.0f; // no hint this long after the map opened: log why (once per session)
constexpr float kTravelTimeout = 90.0f;  // zone change: give up if the player is not in the new zone by then
constexpr float kTravelPoll = 0.5f;      // zone change: arrival checked this often
constexpr float kWallPoll = 3.0f;        // Nullifier wall registry read this often (any time)
constexpr const char* kWallKey = "map_tools.null_walls";  // per-save sidecar value: the walls seen

template <class T>
T& F(void* obj, size_t off) {
    return unity::Field<T>(obj, off);
}

struct NativeFault : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// ------------------------------------------------------------- settings -----
struct IconKind {
    const char* key;
    const char* sizeKey;
    const char* label;
    double def, size;
    const char* tip;
};
// Order = mapi::IconCat.
const IconKind kKinds[mapi::kIconCatCount] = {
    {"icon_teleporters", "size_teleporters", "Undiscovered ancient teleporters", 0, 30,
     "Adds the ones you have not found yet (spoilers)."},
    {"icon_treasure_pods", "size_treasure_pods", "Treasure pods", 1, 26, "Unopened treasure pods"},
    {"icon_gordos", "size_gordos", "Undiscovered gordos", 0, 30,
     "Adds the ones you have not seen yet (spoilers)."},
    {"icon_resource_nodes", "size_resource_nodes", "Resource nodes", 0, 22,
     "Resource nodes ready to harvest (only in loaded areas)"},
    {"icon_other", "size_other", "Undiscovered plort doors and landmarks", 0, 28,
     "Adds the ones you have not found yet (spoilers)."},
    {"icon_null_walls", "size_null_walls", "Nullifier walls (Grey Labyrinth)", 1, 24, "Intact Nullifier walls"},
    {"icon_placed_gadgets", "size_placed_gadgets", "Placed gadgets", 1, 24,
     "Every gadget you placed. The options below pick which kinds."},
    {"icon_drone_ports", "size_drone_ports", "Placed gadgets: drone ports", 1, 28,
     "Drone stations, drawn at your size."},
    {"icon_placed_teleporters", "size_placed_teleporters",
     "Placed gadgets: teleporters, warp depots, market/refinery links", 1, 26, ""},
    {"icon_placed_decor", "size_placed_decor", "Placed gadgets: decorations", 0, 20, "Purely decorative gadgets"},
};

struct Cfg {
    bool reveal = false;
    bool icons[mapi::kIconCatCount] = {};
    float size[mapi::kIconCatCount] = {};
    bool underFog = true;
    bool teleport = false;
    bool AnyIcon() const {
        for (bool b : icons)
            if (b) return true;
        return false;
    }
};

void RegisterSettings() {
    using settings::Kind;
    auto def = [](const char* key, const char* label, const char* group, Kind kind, double d, double mn, double mx,
                  double step, const char* tip) {
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
        x.tooltip = tip;
        settings::Register(x);
    };
    def("reveal_all", "Reveal the whole map", "Fog of war", Kind::Bool, 0, 0, 1, 1,
        "Shows the full map without fog, with every map tab. Display only: nothing counts as discovered. With "
        "right-click teleport, every zone can be reached, the Grey Labyrinth and The Sanctuary too (may skip parts "
        "of the story).");
    for (const IconKind& k : kKinds) {
        def(k.key, k.label, "Icons", Kind::Bool, k.def, 0, 1, 1, k.tip);
        def(k.sizeKey, (std::string(k.label) + ": size").c_str(), "Icons", Kind::Float, k.size, 12, 64, 1,
            "Icon size in pixels at 1080p, the same at every zoom.");
        settings::SetDisplay(kModule, k.sizeKey, " px");
        settings::SetParent(kModule, k.sizeKey, k.key);
    }
    for (const char* k : {"icon_drone_ports", "icon_placed_teleporters", "icon_placed_decor"})
        settings::SetParent(kModule, k, "icon_placed_gadgets");
    def("icons_under_fog", "Hide icons under fog", "Icons", Kind::Bool, 1, 0, 1, 1,
        "Unexplored areas hide the icons in them. Off: icons show on top of the fog.");
    def("rmb_teleport", "Right-click to teleport", "Teleport", Kind::Bool, 0, 0, 1, 1,
        "Right-click a spot on the open map to teleport there. Other zones you have visited work too, after a "
        "loading screen. Spots under the map's fog are refused unless Reveal the whole map is on.");
}

Cfg LoadCfg() {
    Cfg c;
    c.reveal = settings::GetBool(kModule, "reveal_all");
    for (int i = 0; i < mapi::kIconCatCount; ++i) {
        c.icons[i] = settings::GetBool(kModule, kKinds[i].key);
        float s = settings::GetFloat(kModule, kKinds[i].sizeKey);
        c.size[i] = std::isfinite(s) ? std::clamp(s, 8.0f, 96.0f) : static_cast<float>(kKinds[i].size);
    }
    // the gadget groups refine the master switch
    if (!c.icons[mapi::kIconPlaced])
        for (int g : {mapi::kIconDronePort, mapi::kIconPlacedTele, mapi::kIconPlacedDecor}) c.icons[g] = false;
    c.underFog = settings::GetBool(kModule, "icons_under_fog");
    c.teleport = settings::GetBool(kModule, "rmb_teleport");
    return c;
}

// --------------------------------------------------------------- symbols ----
struct Sym {
    Il2CppClass *mapUI = nullptr, *map = nullptr, *viewModel = nullptr, *fogReveal = nullptr, *fogElement = nullptr;
    Il2CppClass *mapDirector = nullptr, *zoneArea = nullptr, *clickMarker = nullptr, *pointerEvent = nullptr;
    Il2CppClass *legend = nullptr, *hint = nullptr, *inputIcon = nullptr, *eventDisplay = nullptr, *invokeOnClick = nullptr;
    Il2CppClass *componentIface = nullptr, *lse = nullptr, *selectable = nullptr, *registrar = nullptr;
    Il2CppClass *teleportable = nullptr, *displayManager = nullptr;
    // hook targets
    void *lateUpdate = nullptr, *setRevealState = nullptr, *isZoneHidden = nullptr, *onPointerClick = nullptr;
    // calls
    ugui::Fn currentMapDef, tryFindZoneAt, revealZonesInMap, getMapForZone, playerCurrentZone;
    ugui::Fn playerSceneGroup, dmInstance, dmClose, playCue;
    ugui::Fn behSetEnabled, setSiblingIndex, getSiblingIndex;
    // cross-zone travel
    Il2CppClass *teleportNetwork = nullptr, *zoneSgMapping = nullptr, *sceneLoaderClass = nullptr;
    ugui::Fn tpToDestImpl, nodeId, zsgTryGet, loadInProgress, isZoneHiddenFn, lsGetLocalized;
    size_t tnDefinition = kNoOff, listItems = kNoOff, tsdDestinations = kNoOff, tddNode = kNoOff, tddGroup = kNoOff;
    size_t zmdPrimary = kNoOff, zoneLocName = kNoOff, zoneRanch = kNoOff;  // ZoneDefinition._isRanchZone
    ugui::Fn defaultGroup;  // SceneLoader.get_DefaultGameplaySceneGroup (the ranch's scene group)
    size_t zsgMappings = kNoOff, sglItems = kNoOff;  // ZoneSceneGroupMapping._mappings, SceneGroupList.items
    // MapUI
    size_t uiMap = kNoOff, uiViewModel = kNoOff, uiFogReveal = kNoOff, uiLegend = kNoOff, uiDirector = kNoOff,
           uiFailCue = kNoOff;
    // Map
    size_t mapBelowMarkers = kNoOff, mapStaticMarkers = kNoOff;
    // MapViewModel
    size_t vmAnchor = kNoOff, vmCursor = kNoOff, vmZoom = kNoOff, vmMapRect = kNoOff, vmWorldRect = kNoOff,
           vmFullRect = kNoOff;
    size_t areaZoneData = kNoOff;
    size_t clickDragging = kNoOff, peButton = kNoOff;
    // legend
    size_t ciObject = kNoOff, legendHints = kNoOff, regPassThrough = kNoOff;
    size_t iconButton = kNoOff, iconKey = kNoOff, iconKeyText = kNoOff, iconKeyImage = kNoOff, iconKeyLse = kNoOff,
           iconDouble = kNoOff, iconButtonImage = kNoOff;
    // the game's mouse button glyphs: KeyboardMouseIconSet (InputIconSet<KeyboardMouseIcon>._items),
    // KeyboardMouseIcon._control ("<Mouse>/rightButton") / ._buttonSprite
    Il2CppClass* kmIconSet = nullptr;
    size_t kmItems = kNoOff, kmControl = kNoOff, kmSprite = kNoOff;
    // the key bar's Place Waypoint hint: InputHint._configuration (InputHintConfiguration).InputEvent, _labelText
    size_t hintConfig = kNoOff, hcEvent = kNoOff, hintLabelText = kNoOff;
    ugui::Fn tmpGetText;  // TMP_Text.get_text
    // the real fog (teleport refused under fog with Reveal off): FogMapElement.ShouldBeRevealed(EventDirector),
    // MapUIFogReveal._eventDirector, FogMapElement._canvasGroup (fallback: its alpha)
    ugui::Fn fogShouldReveal;
    size_t frDirector = kNoOff, fogCanvasGroup = kNoOff;
    // map tabs with Reveal on: MapListConfiguration.<>c__DisplayClass1_0.<GetAvailableMaps>b__0(MapDefinition)
    void* mapAvailable = nullptr;
};

struct ResolveCtx {
    std::vector<std::string> missing;
    Il2CppClass* Class(const char* image, const char* ns, const char* name) {
        std::string e;
        Il2CppClass* k = Resolver::Get().Class(image, ns, name, &e);
        if (!k) missing.push_back(e.empty() ? std::format("class {}.{}", ns, name) : e);
        return k;
    }
    Il2CppClass* Class(const char* ns, const char* name) { return Class(kAsm, ns, name); }
    const MethodInfo* Method(Il2CppClass* k, const char* name, int argc) {
        if (!k) return nullptr;
        std::string e;
        const MethodInfo* m = Resolver::Get().Method(k, name, argc, &e);
        if (!m) missing.push_back(e.empty() ? std::format("method {}", name) : e);
        return m;
    }
    ugui::Fn Bind(const MethodInfo* m, const char* name) {
        if (!m) return {};
        ugui::Fn f = ugui::BindFn(m);
        if (!f) missing.push_back(std::format("{}: no native body", name));
        return f;
    }
    ugui::Fn Bind(Il2CppClass* k, const char* name, int argc) { return Bind(Method(k, name, argc), name); }
    void* Target(Il2CppClass* k, const char* name, int argc) {
        const MethodInfo* m = Method(k, name, argc);
        if (!m) return nullptr;
        void* p = ugui::MethodPtr(m);
        if (!p) missing.push_back(std::format("{}: no native body to hook", name));
        return p;
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

// ----------------------------------------------------------- hook state ------
// All detours run on the Unity main thread. Each one calls the original (unguarded) and does its own small
// logic inside a DetourGuard: record a pointer / a flag, or flip an argument / result. No IL2CPP calls.
std::atomic<bool> g_dead{false};
std::atomic<bool> g_active{false};
std::atomic<bool> g_reveal{false}, g_teleport{false};
std::atomic<Il2CppObject*> g_mapUI{nullptr};
std::atomic<uint32_t> g_lateCount{0};
std::atomic<bool> g_rightClick{false};
std::atomic<int> g_forcedMaps{0};  // map pages listed only because of Reveal everything (counted by the hook)
size_t g_offButton = kNoOff, g_offDragging = kNoOff;  // set in Setup, read by the click detour

sr2::core::DetourGuard g_guardLate{"map_tools", "MapUI.LateUpdate"};
sr2::core::DetourGuard g_guardReveal{"map_tools", "FogMapElement.SetRevealState"};
sr2::core::DetourGuard g_guardHidden{"map_tools", "MapDirector.IsZoneHidden"};
sr2::core::DetourGuard g_guardClick{"map_tools", "MapClickPlaceMarker.OnPointerClick"};
sr2::core::DetourGuard g_guardAvail{"map_tools", "MapListConfiguration.GetAvailableMaps filter"};

bool Live() { return !g_dead.load() && g_active.load(); }

using VoidFn = void (*)(Il2CppObject*, const MethodInfo*);
using SetRevealFn = void (*)(Il2CppObject*, bool, const MethodInfo*);
using IsHiddenFn = bool (*)(Il2CppObject*, Il2CppObject*, const MethodInfo*);
using ClickFn = void (*)(Il2CppObject*, Il2CppObject*, const MethodInfo*);
using AvailFn = bool (*)(Il2CppObject*, Il2CppObject*, const MethodInfo*);
VoidFn g_origLate = nullptr;
SetRevealFn g_origSetReveal = nullptr;
IsHiddenFn g_origIsHidden = nullptr;
ClickFn g_origClick = nullptr;
AvailFn g_origAvail = nullptr;

void LateUpdateDetour(Il2CppObject* self, const MethodInfo* mi) {
    g_origLate(self, mi);
    if (Live())
        g_guardLate.Run([&] {
            g_mapUI.store(self);
            g_lateCount.fetch_add(1);
        });
}

void SetRevealStateDetour(Il2CppObject* self, bool isRevealed, const MethodInfo* mi) {
    if (!isRevealed && Live()) g_guardReveal.Run([&] { isRevealed = g_reveal.load(); });
    g_origSetReveal(self, isRevealed, mi);
}

bool IsZoneHiddenDetour(Il2CppObject* self, Il2CppObject* zoneMapData, const MethodInfo* mi) {
    bool hidden = g_origIsHidden(self, zoneMapData, mi);
    if (hidden && Live()) g_guardHidden.Run([&] { hidden = !g_reveal.load(); });
    return hidden;
}

void ClickDetour(Il2CppObject* self, Il2CppObject* eventData, const MethodInfo* mi) {
    g_origClick(self, eventData, mi);  // left button: the game's waypoint; other buttons: nothing
    if (!Live() || !g_teleport.load() || !self || !eventData) return;
    g_guardClick.Run([&] {
        // PointerEventData.InputButton: 0 Left, 1 Right, 2 Middle; no click after a drag
        if (F<int>(eventData, g_offButton) == 1 && !(Has(g_offDragging) && F<bool>(self, g_offDragging)))
            g_rightClick.store(true);
    });
}

// The filter of MapListConfiguration.GetAvailableMaps (MapDefinition.MapAvailableCondition satisfied), called
// once per map page when the full map's MapViewModel is built (its only caller): the map tabs. With Reveal
// everything on, every page is listed (the Grey Labyrinth's tab on a save that has not reached it yet).
bool MapAvailableDetour(Il2CppObject* self, Il2CppObject* map, const MethodInfo* mi) {
    bool available = g_origAvail(self, map, mi);
    if (!available && Live())
        g_guardAvail.Run([&] {
            if (g_reveal.load()) {
                available = true;
                g_forcedMaps.fetch_add(1);
            }
        });
    return available;
}

// =============================================================== module ======
class MapTools {
public:
    bool Setup(core::ModuleContext& ctx) {
        std::string err;
        if (!ugui::Resolve(err)) return ctx.Fail("uGUI bindings: " + err);
        ResolveCtx c;
        const char* nsUiMap = "MonomiPark.SlimeRancher.UI.Map";
        const char* nsCtl = "MonomiPark.SlimeRancher.UI.Framework.CommonControls";
        s_.mapUI = c.Class(nsUiMap, "MapUI");
        s_.map = c.Class(nsUiMap, "Map");
        s_.viewModel = c.Class(nsUiMap, "MapViewModel");
        s_.fogReveal = c.Class(nsUiMap, "MapUIFogReveal");
        s_.fogElement = c.Class(nsUiMap, "FogMapElement");
        s_.zoneArea = c.Class(nsUiMap, "ZoneDetectionArea");
        s_.clickMarker = c.Class(nsUiMap, "MapClickPlaceMarker");
        s_.mapDirector = c.Class("MonomiPark.SlimeRancher.Map", "MapDirector");
        if (!s_.mapUI || !s_.map) return ctx.Fail("MapUI / Map not found");

        s_.uiMap = c.Field(s_.mapUI, "_map");
        s_.uiViewModel = c.Field(s_.mapUI, "_viewModel");
        s_.uiFogReveal = c.Field(s_.mapUI, "_fogReveal");
        s_.uiLegend = c.Field(s_.mapUI, "_inputLegend");
        s_.uiDirector = c.Field(s_.mapUI, "_mapDirector");
        s_.uiFailCue = c.Field(s_.mapUI, "_failedPlaceWaypointCue");
        s_.currentMapDef = c.Bind(s_.mapUI, "get_CurrentMapDefinition", 0);
        s_.tryFindZoneAt = c.Bind(s_.mapUI, "TryFindZoneAt", 1);
        s_.lateUpdate = c.Target(s_.mapUI, "LateUpdate", 0);
        if (!Has(s_.uiMap) || !s_.lateUpdate) return ctx.Fail("MapUI._map / MapUI.LateUpdate not found");
        s_.mapBelowMarkers = c.Field(s_.map, "_belowMarkersContainer");
        s_.mapStaticMarkers = c.Field(s_.map, "_staticMarkerContainer");
        s_.vmAnchor = c.Field(s_.viewModel, "_mapAnchor");
        s_.vmCursor = c.Field(s_.viewModel, "_cursorOffset");
        s_.vmZoom = c.Field(s_.viewModel, "_mapZoom");
        s_.vmMapRect = c.Field(s_.viewModel, "_mapRect");
        s_.vmWorldRect = c.Field(s_.viewModel, "_worldRect");
        s_.vmFullRect = c.Field(s_.viewModel, "_fullMapRect");
        s_.areaZoneData = c.Field(s_.zoneArea, "_zoneMapData");
        s_.revealZonesInMap = c.Bind(s_.fogReveal, "RevealZonesInMap", 1);
        s_.setRevealState = c.Target(s_.fogElement, "SetRevealState", 1);
        s_.isZoneHidden = c.Target(s_.mapDirector, "IsZoneHidden", 1);
        s_.getMapForZone = c.Bind(s_.mapDirector, "GetMapForZone", 1);
        s_.playerCurrentZone = c.Bind(c.Class(nsUiMap, "MapUIUtilities"), "GetPlayerCurrentZone", 0);
        s_.onPointerClick = c.Target(s_.clickMarker, "UnityEngine.EventSystems.IPointerClickHandler.OnPointerClick", 1);
        s_.clickDragging = c.Field(s_.clickMarker, "_isDragging");
        s_.pointerEvent = c.Class("UnityEngine.UI", "UnityEngine.EventSystems", "PointerEventData");
        s_.peButton = c.Field(s_.pointerEvent, "<button>k__BackingField");

        // input legend ("[RMB] Teleport")
        s_.componentIface = c.Class("MonomiPark.SlimeRancher.UI.Framework", "ComponentInterfaceBase");
        s_.ciObject = c.Field(s_.componentIface, "_object");
        s_.legend = c.Class(nsCtl, "InputLegend");
        s_.legendHints = c.Field(s_.legend, "_hints");
        // MapUI._inputLegend may hold an InputLegendFocusRegistrar that forwards (_passThrough) to the real legend
        s_.registrar = c.Class(nsCtl, "InputLegendFocusRegistrar");
        s_.regPassThrough = c.Field(s_.registrar, "_passThrough");
        s_.hint = c.Class(nsCtl, "InputHint");
        s_.inputIcon = c.Class(nsCtl, "InputIcon");
        s_.eventDisplay = c.Class(nsCtl, "InputEventDisplay");
        s_.invokeOnClick = c.Class(nsCtl, "InvokeInputEventOnPointerClick");
        s_.iconButton = c.Field(s_.inputIcon, "_buttonRoot");
        s_.iconKey = c.Field(s_.inputIcon, "_keyRoot");
        s_.iconKeyText = c.Field(s_.inputIcon, "_keyText");
        s_.iconKeyImage = c.Field(s_.inputIcon, "_keyImage");
        s_.iconKeyLse = c.Field(s_.inputIcon, "_keyLocalizeStringEvent");
        s_.iconDouble = c.Field(s_.inputIcon, "_doubleTapRoot");
        s_.iconButtonImage = c.Field(s_.inputIcon, "_buttonImage");
        s_.kmIconSet = c.Class("MonomiPark.SlimeRancher.Input", "KeyboardMouseIconSet");
        s_.kmItems = c.Field(s_.kmIconSet, "_items");
        Il2CppClass* kmIcon = c.Class("MonomiPark.SlimeRancher.Input", "KeyboardMouseIcon");
        s_.kmControl = c.Field(kmIcon, "_control");
        s_.kmSprite = c.Field(kmIcon, "_buttonSprite");
        s_.lse = c.Class("Unity.Localization", "UnityEngine.Localization.Components", "LocalizeStringEvent");
        s_.hintConfig = c.Field(s_.hint, "_configuration");
        s_.hintLabelText = c.Field(s_.hint, "_labelText");
        s_.hcEvent = c.Field(c.Class(nsCtl, "InputHintConfiguration"), "InputEvent");
        s_.tmpGetText = c.Bind(c.Class("Unity.TextMeshPro", "TMPro", "TMP_Text"), "get_text", 0);
        // the real fog under a click
        s_.fogShouldReveal = c.Bind(s_.fogElement, "ShouldBeRevealed", 1);
        s_.frDirector = c.Field(s_.fogReveal, "_eventDirector");
        s_.fogCanvasGroup = c.Field(s_.fogElement, "_canvasGroup");
        // map tabs (Reveal everything)
        if (Il2CppClass* mlc = c.Class("MonomiPark.SlimeRancher.Map", "MapListConfiguration")) {
            Il2CppClass* dc = ugui::NestedClass(mlc, "<>c__DisplayClass1_0");
            if (dc) s_.mapAvailable = c.Target(dc, "<GetAvailableMaps>b__0", 1);
            else c.missing.push_back("class MapListConfiguration.<>c__DisplayClass1_0");
        }
        s_.selectable = c.Class("UnityEngine.UI", "UnityEngine.UI", "Selectable");

        // teleport
        s_.teleportable = c.Class("", "TeleportablePlayer");
        s_.playerSceneGroup = c.Bind(s_.teleportable, "get_SceneGroup", 0);
        s_.displayManager = c.Class("MonomiPark.SlimeRancher.UI.Framework.Displays", "UIDisplayManager");
        s_.dmInstance = c.Bind(s_.displayManager, "get_Instance", 0);
        if (s_.displayManager) {
            std::string e;
            s_.dmClose = c.Bind(Resolver::Get().MethodByParams(s_.displayManager, "CloseDisplayByObjectInHierarchy",
                                                               {"UnityEngine.Component"}, &e),
                                "UIDisplayManager.CloseDisplayByObjectInHierarchy(Component)");
            if (!s_.dmClose && !e.empty()) c.missing.push_back(e);
        }
        s_.playCue = c.Bind(c.Class("MonomiPark.SlimeRancher.UI.Framework.Audio", "UIAudioCueHandle"), "PlayAudioCue", 1);
        {
            std::string werr;  // Nullifier walls for the full map's other tabs (cached per save)
            if (!walls::Resolve(werr)) c.missing.push_back("Nullifier walls: " + werr);
        }
        {
            std::string gerr;  // the ground teleport shared with the sandbox's "teleport to waypoint"
            if (!ground_teleport::Resolve(gerr)) c.missing.push_back("ground teleport: " + gerr);
        }

        // Unity
        Il2CppClass* behaviour = c.Class("UnityEngine.CoreModule", "UnityEngine", "Behaviour");
        Il2CppClass* transform = c.Class("UnityEngine.CoreModule", "UnityEngine", "Transform");
        s_.behSetEnabled = c.Bind(behaviour, "set_enabled", 1);
        s_.setSiblingIndex = c.Bind(transform, "SetSiblingIndex", 1);
        s_.getSiblingIndex = c.Bind(transform, "GetSiblingIndex", 0);

        // cross-zone travel (the game's teleporter path)
        const char* nsTp = "MonomiPark.SlimeRancher.World.Teleportation";
        s_.teleportNetwork = c.Class(nsTp, "TeleportNetwork");
        s_.tpToDestImpl = c.Bind(s_.teleportNetwork, "TeleportToDestinationImpl", 5);
        s_.tnDefinition = c.Field(s_.teleportNetwork, "_networkDefinition");
        s_.listItems = c.Field(c.Class(nsTp, "StaticTeleportNetworkDefinition"), "items");  // ListAsset<T>.items
        Il2CppClass* tsd = c.Class(nsTp, "TeleportSystemDefinition");
        s_.tsdDestinations = c.Field(tsd, "DestinationNodes");
        Il2CppClass* tdd = c.Class(nsTp, "TeleportDestinationDefinition");
        s_.tddNode = c.Field(tdd, "DestinationNode");
        s_.tddGroup = c.Field(tdd, "DestinationSceneGroup");
        s_.nodeId = c.Bind(c.Class(nsTp, "TeleporterNodeDefinition"), "get_NodeId", 0);
        s_.zoneSgMapping = c.Class("MonomiPark.SlimeRancher.Map", "ZoneSceneGroupMapping");
        s_.zsgTryGet = c.Bind(s_.zoneSgMapping, "TryGetValue", 2);
        s_.zsgMappings = c.Field(s_.zoneSgMapping, "_mappings");
        s_.sglItems = c.Field(c.Class("MonomiPark.SlimeRancher.SceneManagement", "SceneGroupList"), "items");
        s_.zmdPrimary = c.Field(c.Class("MonomiPark.SlimeRancher.World", "ZoneMapData"), "_primaryZone");
        Il2CppClass* zoneDef = c.Class("MonomiPark.SlimeRancher.World", "ZoneDefinition");
        s_.zoneLocName = c.Field(zoneDef, "_localizedName");
        s_.zoneRanch = c.Field(zoneDef, "_isRanchZone");
        s_.lsGetLocalized = c.Bind(c.Class("Unity.Localization", "UnityEngine.Localization", "LocalizedString"),
                                   "GetLocalizedString", 0);
        s_.isZoneHiddenFn = c.Bind(s_.mapDirector, "IsZoneHidden", 1);
        s_.sceneLoaderClass = c.Class("MonomiPark.SlimeRancher.SceneManagement", "SceneLoader");
        s_.loadInProgress = c.Bind(s_.sceneLoaderClass, "get_IsSceneLoadInProgress", 0);
        s_.defaultGroup = c.Bind(s_.sceneLoaderClass, "get_DefaultGameplaySceneGroup", 0);
        {
            std::string nerr;
            if (!notices_.Setup(Resolver::Get(), kModule, nerr, "teleport") || !notices_.SetupText(Resolver::Get(), nerr))
                c.missing.push_back("HUD notices: " + nerr);
        }

        for (const auto& m : c.missing) SR2_INFO("map_tools: optional (degraded): {}", m);
        g_offButton = s_.peButton;
        g_offDragging = s_.clickDragging;

        auto hook = [&](const char* name, void* target, void* detour, void** orig) {
            if (!target) return false;
            std::string herr;
            if (ctx.hooks().Create(kModule, std::string("map_tools.") + name, target, detour, orig, &herr)) return true;
            SR2_WARN("map_tools: hook {} not created ({})", name, herr);
            return false;
        };
        if (!hook("MapUI.LateUpdate", s_.lateUpdate, reinterpret_cast<void*>(&LateUpdateDetour),
                  reinterpret_cast<void**>(&g_origLate)))
            return ctx.Fail("hook MapUI.LateUpdate failed");
        hookReveal_ = hook("FogMapElement.SetRevealState", s_.setRevealState, reinterpret_cast<void*>(&SetRevealStateDetour),
                           reinterpret_cast<void**>(&g_origSetReveal));
        hookHidden_ = hook("MapDirector.IsZoneHidden", s_.isZoneHidden, reinterpret_cast<void*>(&IsZoneHiddenDetour),
                           reinterpret_cast<void**>(&g_origIsHidden));
        hookAvail_ = hook("MapListConfiguration.GetAvailableMaps filter", s_.mapAvailable,
                          reinterpret_cast<void*>(&MapAvailableDetour), reinterpret_cast<void**>(&g_origAvail));
        hookClick_ = Has(s_.peButton) &&
                     hook("MapClickPlaceMarker.OnPointerClick", s_.onPointerClick, reinterpret_cast<void*>(&ClickDetour),
                          reinterpret_cast<void**>(&g_origClick));

        RegisterSettings();
        if (!onChange_) onChange_ = settings::OnChange(kModule, [this](const std::string&) { cfgDirty_.store(true); });
        SR2_INFO("map_tools: resolved (reveal {}, zone names {}, map tabs {}, static fog {}, icons {}, teleport {}, "
                 "cross-zone {}, fog check {}, legend hint {}, notices {}, {} optional symbols missing)",
                 hookReveal_ ? "yes" : "no", hookHidden_ ? "yes" : "no", hookAvail_ ? "yes" : "no",
                 s_.behSetEnabled ? "yes" : "no", s_.setSiblingIndex ? "yes" : "no", TeleportResolved() ? "yes" : "no",
                 TravelResolved() ? "yes" : "no",
                 s_.fogShouldReveal && Has(s_.frDirector) ? "yes" : Has(s_.fogCanvasGroup) ? "alpha only" : "no",
                 LegendResolved() ? "yes" : "no", notices_.TextReady() ? "yes" : "no", c.missing.size());
        return true;
    }

    void OnEnable() {
        if (g_dead.load()) {
            SR2_WARN("map_tools: stays off for this session after a native fault");
            return;
        }
        cfgDirty_.store(true);
        g_active.store(true);
        if (!frameCb_) frameCb_ = main_thread::AddFrameCallback(kModule, [this](float dt) { Tick(dt); });
    }

    void OnDisable() {
        g_active.store(false);
        g_rightClick.store(false);
        if (frameCb_) main_thread::RemoveFrameCallback(frameCb_);
        frameCb_ = 0;
        if (g_dead.load()) return;
        auto td = [this] {
            if (!g_dead.load()) Teardown();
        };
        if (main_thread::IsMainThread()) td();
        else main_thread::Post(td);
    }

private:
    struct Icon {
        Ref go, t, img;
        int cat = -1;
        bool active = false;
    };

    Sym s_;
    Cfg cfg_;
    std::atomic<bool> cfgDirty_{true};
    int frameCb_ = 0, onChange_ = 0;
    bool hookReveal_ = false, hookHidden_ = false, hookClick_ = false, hookAvail_ = false;
    int forcedLogged_ = 0;  // g_forcedMaps last logged
    bool frozen_ = false;
    const char* step_ = "";

    // the open map
    uint32_t lastLate_ = 0;
    int closedTicks_ = kClosedTicks;
    bool open_ = false;
    Ref mapUI_, map_, mapT_;
    float time_ = 0;

    // reveal
    bool revealApplied_ = false;       // static fog switched off on this page
    std::vector<Ref> hiddenStatic_;    // fog-shader graphics we disabled
    bool revealShown_ = false;         // cfg the page's fog state belongs to

    // icons
    Ref container_, canvasT_;  // canvasT_: the root canvas above the page (icon scale reference)
    std::vector<Icon> icons_;
    bool containerFog_ = true;
    float iconAcc_ = 1e9f, lastK_ = -1;
    Il2CppObject* iconMapDef_ = nullptr;  // identity only
    bool iconsLogged_ = false, iconMismatchLogged_ = false, minimapOffLogged_ = false;
    // Nullifier walls seen this session / in the save's sidecar (kWallKey), for maps the walls are not loaded on
    struct CachedWall {
        float x = 0, y = 0, z = 0;
        bool intact = true;
    };
    std::vector<CachedWall> wallCache_;
    std::string wallSaved_;   // the sidecar value last read or written
    float wallPollAt_ = 0;
    std::vector<walls::Wall> wallLive_;
    Il2CppObject* wallsLoggedDef_ = nullptr;  // identity only: page the cached walls were last logged for
    std::vector<mapi::PageIcon> buf_;

    // legend
    Ref hintGo_, hintLabel_, hintKey_;
    std::vector<Ref> hintIcons_;
    int hintReapply_ = 0;
    float hintRetryAt_ = 0;
    bool hintFailLogged_ = false, hintLogged_ = false;
    float hintSince_ = -1;  // first try of this map open (diagnostics after kHintDiagDelay)
    bool hintDiagLogged_ = false;
    Ref hintLegend_;           // the InputLegend the hint was cloned into
    bool hintPlaced_ = false;  // moved next to the Place Waypoint hint
    bool hintPlaceLogged_ = false;
    float hintPlaceAt_ = 0;
    Ref mouseGlyph_;           // the game's right mouse button sprite (or the left one, drawn mirrored)
    bool mouseGlyphMirror_ = false, mouseGlyphSearched_ = false;

    // teleport
    Ref player_, net_, zoneMap_, sceneLoader_;
    bool clickLogged_ = false;
    float zoneMapRetry_ = 0;
    std::map<Il2CppObject*, std::string> zoneNames_;
    std::vector<Ref> zonePins_;
    std::map<Il2CppObject*, std::vector<Ref>> zoneGroups_;  // ZoneDefinition -> its scene groups (BuildZoneGroups)
    bool zoneGroupsBuilt_ = false, routesLogged_ = false, ranchLogged_ = false;
    std::map<Il2CppObject*, std::string> nodeNames_;        // TeleporterNodeDefinition (asset) -> node id
    struct Travel {  // a zone change in progress (cross-zone click)
        bool active = false;
        float x = 0, z = 0;
        Ref group;    // destination SceneGroup
        Ref origin;   // the player's scene group at the click
        Ref zoneDef;  // destination ZoneDefinition
        std::string zone, node;
        float started = 0, nextTry = 0, arrivedAt = -1;
        bool sawLoading = false;
    } travel_;
    sr2::modules::custom_upgrades::ui::SaleNotices notices_;  // the HUD's drone-style pop-ups (free text)

    bool TeleportResolved() const {
        return hookClick_ && ground_teleport::Resolved() && s_.playerSceneGroup &&
               Has(s_.uiViewModel) && Has(s_.vmAnchor) && Has(s_.vmCursor) && Has(s_.vmZoom) && Has(s_.vmMapRect) &&
               Has(s_.vmWorldRect) && s_.teleportable;
    }
    bool TravelResolved() const {
        return s_.teleportNetwork && s_.tpToDestImpl && s_.nodeId && s_.zsgTryGet && Has(s_.tnDefinition) &&
               Has(s_.listItems) && Has(s_.tsdDestinations) && Has(s_.tddNode) && Has(s_.tddGroup) &&
               Has(s_.zmdPrimary) && s_.zoneSgMapping;
    }
    bool LegendResolved() const {
        return Has(s_.uiLegend) && Has(s_.ciObject) && Has(s_.legendHints) && s_.hint && s_.inputIcon &&
               Has(s_.iconKey) && Has(s_.iconKeyText) && s_.behSetEnabled;
    }

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

    void SetEnabled(Il2CppObject* behaviour, bool on) {
        if (!s_.behSetEnabled || !unity::IsAlive(behaviour)) return;
        Managed([&] { ugui::Call<void>(s_.behSetEnabled, behaviour, on); });
    }

    // ----------------------------------------------------------------- tick --
    void Tick(float dt) {
        if (!frozen_ && core::ModuleFaulted(kModule)) {
            Die("module marked faulted (fault inside a hook detour, see the line above)");
            return;
        }
        if (g_dead.load()) return;
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
        g_dead.store(true);
        g_active.store(false);
        SR2_ERROR("map_tools: {} - map tools disabled for the rest of this session", why);
        core::MarkModuleFaulted(kModule, why);
        for (Ref* r : {&mapUI_, &map_, &mapT_, &container_, &canvasT_, &hintGo_, &hintLabel_, &hintKey_, &hintLegend_, &mouseGlyph_, &player_, &net_,
                       &zoneMap_, &sceneLoader_, &travel_.group, &travel_.origin, &travel_.zoneDef})
            r->Abandon();
        for (auto& [z, groups] : zoneGroups_)
            for (Ref& r : groups) r.Abandon();
        zoneGroups_.clear();
        for (Ref& r : zonePins_) r.Abandon();
        zonePins_.clear();
        zoneNames_.clear();
        travel_.active = false;
        for (Icon& i : icons_)
            for (Ref* r : {&i.go, &i.t, &i.img}) r->Abandon();
        for (Ref& r : hiddenStatic_) r.Abandon();
        for (Ref& r : hintIcons_) r.Abandon();
        icons_.clear();
        hiddenStatic_.clear();
        hintIcons_.clear();
        if (frameCb_) main_thread::RemoveFrameCallback(frameCb_);
        frameCb_ = 0;
    }

    void TickImpl(float dt) {
        if (ugui::RuntimeFrozen()) {  // the minimap froze the shared bindings after a fault: stop calling
            if (!frozen_) Die("shared uGUI bindings frozen by the minimap");
            return;
        }
        time_ += dt;
        Step("settings");
        bool cfgChanged = false;
        if (cfgDirty_.exchange(false)) {
            Cfg old = cfg_;
            cfg_ = LoadCfg();
            g_reveal.store(cfg_.reveal && hookReveal_);
            g_teleport.store(cfg_.teleport);
            cfgChanged = true;
            iconAcc_ = 1e9f;
            if (cfg_.underFog != old.underFog) DestroyContainer();  // re-placed below / above the fog
        }

        Step("notices");
        if (notices_.Ready()) notices_.Frame(dt);
        Step("walls");
        PollWalls();
        Step("travel");
        TickTravel();

        Step("map open");
        uint32_t late = g_lateCount.load();
        if (late != lastLate_) {
            lastLate_ = late;
            closedTicks_ = 0;
        } else if (closedTicks_ < kClosedTicks) {
            ++closedTicks_;
        }
        bool open = closedTicks_ < kClosedTicks;
        if (!open) {
            if (open_) OnMapClosed();
            g_rightClick.store(false);
            return;
        }
        Il2CppObject* ui = g_mapUI.load();
        if (!ugui::IsA(ui, s_.mapUI)) return;
        if (ui != mapUI_.get()) {
            OnMapClosed();
            mapUI_.Reset(ui);
            SR2_DEBUG("map_tools: full map opened");
        }
        open_ = true;
        Il2CppObject* map = F<Il2CppObject*>(ui, s_.uiMap);
        if (!ugui::IsA(map, s_.map)) return;
        if (map != map_.get()) OnNewPage(map);

        Step("reveal");
        UpdateReveal(cfgChanged);
        if (int forced = g_forcedMaps.load(); forced != forcedLogged_) {
            SR2_INFO("map_tools: reveal: {} map tab(s) listed that the save has not unlocked yet ({} so far this session)",
                     forced - forcedLogged_, forced);
            forcedLogged_ = forced;
        }
        Step("icons");
        iconAcc_ += dt;
        if (iconAcc_ >= kIconRefresh) {
            iconAcc_ = 0;
            RefreshIcons();
        }
        Step("icon scale");
        UpdateIconScale();
        Step("legend");
        UpdateLegend();
        Step("teleport");
        if (g_rightClick.exchange(false) && cfg_.teleport) Teleport();
        Step("idle");
    }

    void OnNewPage(Il2CppObject* map) {
        RestoreStaticFog();  // dies with the old page anyway; restores what is still alive
        DestroyContainer();
        map_.Reset(map);
        mapT_.Reset(unity::TransformOf(map));
        revealApplied_ = false;
        revealShown_ = cfg_.reveal;  // the page's RevealKnownZones ran with the current hook state
        iconAcc_ = 1e9f;
        iconMapDef_ = nullptr;
        iconMismatchLogged_ = false;
    }

    void OnMapClosed() {
        Step("map closed");
        hintSince_ = -1;
        RestoreStaticFog();
        DestroyContainer();
        DestroyHint();
        map_.Reset(nullptr);
        mapT_.Reset(nullptr);
        mapUI_.Reset(nullptr);
        open_ = false;
        revealApplied_ = false;
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
        else if (!ok) SR2_WARN("map_tools: teardown step '{}' failed: {}", what, err);
    }

    void Teardown() {
        if (ugui::RuntimeFrozen()) {
            if (!frozen_) Die("shared uGUI bindings frozen");
            return;
        }
        g_reveal.store(false);
        g_teleport.store(false);
        TeardownStep("restore static fog", [&] { RestoreStaticFog(); });
        TeardownStep("re-evaluate fog", [&] {
            if (revealShown_ && open_) ReapplyFog();
        });
        TeardownStep("destroy icons", [&] { DestroyContainer(); });
        TeardownStep("destroy hint", [&] { DestroyHint(); });
        TeardownStep("release", [&] {
            map_.Reset(nullptr);
            mapT_.Reset(nullptr);
            mapUI_.Reset(nullptr);
            player_.Reset(nullptr);
            net_.Reset(nullptr);
            sceneLoader_.Reset(nullptr);
            travel_ = {};
            notices_.Clear();
        });
        open_ = false;
        revealShown_ = false;
        closedTicks_ = kClosedTicks;
    }

    // -------------------------------------------------------------- reveal ---
    // The game's fog state comes from MapUIFogReveal.RevealKnownZones (hooked SetRevealState). When the
    // setting changes while the map is open, the page is re-evaluated through RevealZonesInMap.
    void ReapplyFog() {
        if (!s_.revealZonesInMap || !mapUI_.Alive() || !map_.Alive() || !Has(s_.uiFogReveal)) return;
        Il2CppObject* fr = F<Il2CppObject*>(mapUI_.get(), s_.uiFogReveal);
        if (!ugui::IsA(fr, s_.fogReveal)) return;
        Managed([&] { ugui::Call<void>(s_.revealZonesInMap, fr, map_.get()); });
    }

    void UpdateReveal(bool cfgChanged) {
        if (cfgChanged && cfg_.reveal != revealShown_) {
            revealShown_ = cfg_.reveal;
            ReapplyFog();
            SR2_INFO("map_tools: reveal {} on the open map", cfg_.reveal ? "on" : "off");
        }
        if (cfg_.reveal && !revealApplied_) HideStaticFog();
        else if (!cfg_.reveal && revealApplied_) RestoreStaticFog();
    }

    static bool IsFogShader(const std::string& shader) { return shader.find("FogOfWar") != std::string::npos; }

    // Fog-shader art outside every FogMapElement ("fog_static": the static covers and the outside fog, see
    // modules/minimap/NOTES.md): nothing in the game ever hides it, so "reveal" switches the Graphics off.
    void HideStaticFog() {
        revealApplied_ = true;
        if (!s_.behSetEnabled || !map_.Alive() || !mapT_.Alive()) return;
        Il2CppObject* mapGo = unity::GameObjectOf(map_.get());
        std::vector<Il2CppObject*> fogRoots;
        if (s_.fogElement)
            ugui::ForEachInChildren(mapGo, s_.fogElement, true, [&](Il2CppObject* f) {
                fogRoots.push_back(unity::TransformOf(f));
                return true;
            });
        std::vector<Ref> pins;  // fogRoots stay reachable while we allocate
        for (Il2CppObject* t : fogRoots) pins.emplace_back(t);
        int hidden = 0, inAreas = 0;
        std::string names;
        ugui::ForEachInChildren(mapGo, ugui::GraphicClass(), true, [&](Il2CppObject* g) {
            Il2CppObject* mat = ugui::GraphicGetMaterial(g);
            if (!mat || !IsFogShader(ugui::MaterialShaderName(mat))) return true;
            for (Il2CppObject* t = unity::TransformOf(g); t && t != mapT_.get(); t = ugui::GetParent(t))
                if (std::find(fogRoots.begin(), fogRoots.end(), t) != fogRoots.end()) {
                    ++inAreas;
                    return true;
                }
            if (!ugui::BehaviourEnabled(g)) return true;
            SetEnabled(g, false);
            hiddenStatic_.emplace_back(g);
            ++hidden;
            if (names.size() < 300) names += (names.empty() ? "" : ", ") + unity::GetName(unity::GameObjectOf(g));
            return true;
        });
        SR2_INFO("map_tools: reveal: {} fog areas forced revealed by the game's own fog pass, {} static fog image(s) "
                 "switched off{}{}",
                 fogRoots.size(), hidden, names.empty() ? "" : ": ", names);
    }

    void RestoreStaticFog() {
        for (Ref& r : hiddenStatic_)
            if (r.Alive()) SetEnabled(r.get(), true);
        hiddenStatic_.clear();
        revealApplied_ = false;
    }

    // --------------------------------------------------------------- icons ---
    void DestroyContainer() {
        if (container_.Alive()) ugui::Destroy(container_.get());
        container_.Reset(nullptr);
        canvasT_.Reset(nullptr);
        icons_.clear();
        lastK_ = -1;
    }

    // Direct child of the page that contains `t` (nullptr if `t` is not under the page).
    Il2CppObject* TopChild(Il2CppObject* t) {
        for (int guard = 0; t && guard < 64; ++guard) {
            Il2CppObject* p = ugui::GetParent(t);
            if (p == mapT_.get()) return t;
            t = p;
        }
        return nullptr;
    }
    int SiblingIndex(Il2CppObject* t) {
        int i = -1;
        if (t && s_.getSiblingIndex) Managed([&] { i = ugui::Call<int>(s_.getSiblingIndex, t); });
        return i;
    }

    // Our container: a child of the live page in its own space. Below the page's fog layer when icons hide
    // under fog (the first direct child that holds a FogMapElement), else right below the marker containers.
    bool EnsureContainer() {
        if (container_.Alive()) return true;
        container_.Reset(nullptr);
        icons_.clear();
        if (!mapT_.Alive()) return false;
        Il2CppObject* mapGo = unity::GameObjectOf(map_.get());
        int fogIdx = -1, markerIdx = -1;
        std::string fogName, markerName;
        if (s_.fogElement)
            ugui::ForEachInChildren(mapGo, s_.fogElement, true, [&](Il2CppObject* f) {
                Il2CppObject* top = TopChild(unity::TransformOf(f));
                int i = SiblingIndex(top);
                if (i >= 0 && (fogIdx < 0 || i < fogIdx)) {
                    fogIdx = i;
                    fogName = unity::GetName(top);
                }
                return true;
            });
        for (size_t off : {s_.mapBelowMarkers, s_.mapStaticMarkers}) {
            if (!Has(off)) continue;
            Il2CppObject* c = F<Il2CppObject*>(map_.get(), off);
            if (!ugui::IsA(c, ugui::TransformClass())) continue;
            Il2CppObject* top = (c == mapT_.get()) ? nullptr : TopChild(c);
            int i = SiblingIndex(top);
            if (i >= 0 && (markerIdx < 0 || i < markerIdx)) {
                markerIdx = i;
                markerName = unity::GetName(top);
            }
        }
        Il2CppObject* go = ugui::NewGameObject("SR2Mods.MapIcons");
        if (!go) return false;
        container_.Reset(go);
        ugui::SetLayer(go, ugui::GetLayer(mapGo));
        Il2CppObject* t = unity::TransformOf(go);
        ugui::SetParent(t, mapT_.get(), false);
        ugui::SetAnchors(t, {0.5f, 0.5f}, {0.5f, 0.5f});
        ugui::SetPivot(t, {0.5f, 0.5f});
        ugui::SetSizeDelta(t, {0, 0});
        ugui::SetLocalPosition(t, {0, 0, 0});  // the page's own origin: icon positions are page-local
        ugui::SetLocalScale(t, {1, 1, 1});
        containerFog_ = cfg_.underFog;
        int idx = cfg_.underFog && fogIdx >= 0 ? fogIdx : markerIdx;
        const char* where = cfg_.underFog && fogIdx >= 0 ? "below the fog layer" : markerIdx >= 0 ? "below the markers" : "on top";
        if (idx >= 0 && s_.setSiblingIndex) Managed([&] { ugui::Call<void>(s_.setSiblingIndex, t, idx); });
        SR2_INFO("map_tools: icons: container on page '{}' at child index {} ({}; fog layer '{}' at {}, markers '{}' at {})",
                 unity::GetName(mapGo), idx, where, fogName, fogIdx, markerName, markerIdx);
        lastK_ = -1;
        return true;
    }

    void HideIcons() {
        for (Icon& ic : icons_) {
            if (ic.active && ic.go.Alive()) unity::SetActive(ic.go.get(), false);
            ic.active = false;
        }
    }

    void RefreshIcons() {
        if (!cfg_.AnyIcon()) {
            if (container_) DestroyContainer();
            return;
        }
        mapi::PageIconQuery q;
        for (int i = 0; i < mapi::kIconCatCount; ++i) q.cats[i] = cfg_.icons[i];
        q.undiscoveredMarkers = cfg_.icons[mapi::kIconTeleporter] || cfg_.icons[mapi::kIconGordo] || cfg_.icons[mapi::kIconOther];
        Il2CppObject* pageDef = nullptr;
        if (!mapi::CollectPageIcons(q, buf_, &pageDef)) {
            if (!minimapOffLogged_) {
                minimapOffLogged_ = true;
                SR2_INFO("map_tools: icons: the minimap has no map page right now (it must be on) - no full map icons");
            }
            HideIcons();
            return;
        }
        minimapOffLogged_ = false;
        // The icons belong to the page of the player's zone; the full map may show another map (tabs).
        Il2CppObject* shownDef = nullptr;
        if (s_.currentMapDef) Managed([&] { shownDef = ugui::Call<Il2CppObject*>(s_.currentMapDef, mapUI_.get()); });
        if (!shownDef || shownDef != pageDef) {
            if (!iconMismatchLogged_) {
                iconMismatchLogged_ = true;
                SR2_INFO("map_tools: icons: the full map shows '{}', the icons belong to '{}' - hidden on this map{}",
                         shownDef ? unity::GetName(shownDef) : std::string("?"), unity::GetName(pageDef),
                         cfg_.icons[mapi::kIconNullWall] ? " (only the cached Nullifier walls are drawn here)" : "");
            }
            // Another tab (e.g. the Grey Labyrinth while the player is elsewhere): the cached walls only.
            if (!shownDef || !CachedWallIcons(shownDef)) {
                HideIcons();
                return;
            }
            pageDef = shownDef;
        }
        if (!EnsureContainer()) return;
        int layer = ugui::GetLayer(container_.get());
        while (icons_.size() < buf_.size()) {
            Il2CppObject* go = ugui::NewGameObject("Icon");
            if (!go) break;
            ugui::SetLayer(go, layer);
            Il2CppObject* t = unity::TransformOf(go);
            ugui::SetParent(t, unity::TransformOf(container_.get()), false);
            Icon ic;
            ic.go.Reset(go);
            ic.t.Reset(t);
            ic.img.Reset(ugui::AddComponent(go, ugui::ImageClass()));
            ugui::GraphicSetRaycastTarget(ic.img.get(), false);  // clicks and drags reach the map
            ugui::ImageSetPreserveAspect(ic.img.get(), true);
            ugui::SetAnchors(t, {0.5f, 0.5f}, {0.5f, 0.5f});
            ugui::SetPivot(t, {0.5f, 0.5f});
            ic.active = true;
            icons_.push_back(std::move(ic));
        }
        int perCat[mapi::kIconCatCount] = {};
        for (size_t i = 0; i < icons_.size(); ++i) {
            Icon& ic = icons_[i];
            if (!ic.go.Alive() || !ic.t.Alive()) continue;
            bool show = i < buf_.size() && buf_[i].sprite && unity::IsAlive(buf_[i].sprite);
            if (show) {
                const mapi::PageIcon& p = buf_[i];
                int cat = std::clamp(p.cat, 0, mapi::kIconCatCount - 1);
                ++perCat[cat];
                ugui::ImageSetSprite(ic.img.get(), p.sprite);
                ugui::GraphicSetColor(ic.img.get(), Color{p.r, p.g, p.b, p.a});
                ugui::SetSizeDelta(ic.t.get(), {cfg_.size[cat], cfg_.size[cat]});
                ugui::SetLocalPosition(ic.t.get(), {p.x, p.y, 0});
                if (lastK_ > 0) ugui::SetLocalScale(ic.t.get(), {lastK_, lastK_, 1});
                ic.cat = cat;
            }
            if (show != ic.active) {
                unity::SetActive(ic.go.get(), show);
                ic.active = show;
            }
        }
        iconMapDef_ = pageDef;
        if (!iconsLogged_) {
            iconsLogged_ = true;
            std::string parts;
            for (int c = 0; c < mapi::kIconCatCount; ++c)
                if (perCat[c]) parts += std::format("{}{} {}", parts.empty() ? "" : ", ", kKinds[c].label, perCat[c]);
            SR2_INFO("map_tools: icons (first map): {} on '{}'{}{}", buf_.size(), unity::GetName(pageDef),
                     parts.empty() ? "" : ": ", parts);
        }
    }

    // ------------------------------------------------------------ walls ---
    // The full map's other tabs have no live wall data: the Labyrinth's walls exist only while its scene group is
    // loaded (they register in DiscordantWall._activeWalls when it loads, all of them, and are destroyed when it
    // unloads), and nothing else in the game knows them (no save data per wall: harmonized = a Nullifier gadget
    // next to it; no map marker). So every wall seen is remembered: position and intact flag, merged from the live
    // registry (modules/common/discordant_walls.h, inactive walls of proxied regions included) every kWallPoll
    // seconds, kept in the save's sidecar (custom_upgrades per-save value kWallKey, written with the next game
    // save), read back when another save is loaded. One visit to the Labyrinth fills it.
    void PollWalls() {
        if (time_ < wallPollAt_) return;
        wallPollAt_ = time_ + kWallPoll;
        if (custom_upgrades::Active()) {
            std::string v = custom_upgrades::SaveValue(kWallKey);
            if (v != wallSaved_) {  // another save (or its sidecar) loaded
                wallSaved_ = v;
                wallCache_ = ParseWalls(v);
                SR2_INFO("map_tools: Nullifier walls: {} cached in the loaded save", wallCache_.size());
            }
        }
        if (!walls::Resolved()) return;
        wallLive_.clear();
        if (!walls::Collect(wallLive_, nullptr, true) || wallLive_.empty()) return;
        int added = 0, updated = 0;
        for (const walls::Wall& w : wallLive_) {
            if (!std::isfinite(w.x) || !std::isfinite(w.y) || !std::isfinite(w.z)) continue;
            CachedWall* hit = nullptr;
            for (CachedWall& c : wallCache_)
                if (std::hypot(c.x - w.x, c.z - w.z) < 4.0f) {
                    hit = &c;
                    break;
                }
            if (!hit) {
                wallCache_.push_back({w.x, w.y, w.z, w.intact});
                ++added;
                continue;
            }
            if (hit->intact != w.intact || std::fabs(hit->x - w.x) > 0.5f || std::fabs(hit->y - w.y) > 0.5f ||
                std::fabs(hit->z - w.z) > 0.5f) {
                *hit = {w.x, w.y, w.z, w.intact};
                ++updated;
            }
        }
        if (!added && !updated) return;
        std::string v;
        for (const CachedWall& c : wallCache_)
            v += std::format("{}{:.1f},{:.1f},{:.1f},{}", v.empty() ? "" : ";", c.x, c.y, c.z, c.intact ? 1 : 0);
        bool stored = custom_upgrades::Active() && custom_upgrades::SetSaveValue(kWallKey, v);
        if (stored) wallSaved_ = v;
        SR2_INFO("map_tools: Nullifier walls: {} new, {} changed, {} remembered{}", added, updated, wallCache_.size(),
                 stored ? " (kept in the save's sidecar with the next save)" : " (this session only: custom_upgrades is off)");
    }

    static std::vector<CachedWall> ParseWalls(const std::string& v) {
        std::vector<CachedWall> out;
        size_t pos = 0;
        while (pos < v.size() && out.size() < 256) {
            size_t end = v.find(';', pos);
            std::string item = v.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
            pos = end == std::string::npos ? v.size() : end + 1;
            CachedWall c;
            int intact = 1;
            if (sscanf_s(item.c_str(), "%f,%f,%f,%d", &c.x, &c.y, &c.z, &intact) != 4) continue;
            if (!std::isfinite(c.x) || !std::isfinite(c.y) || !std::isfinite(c.z)) continue;
            c.intact = intact != 0;
            out.push_back(c);
        }
        return out;
    }

    // The cached intact walls on the page shown (`shownDef`), projected with the view model's rects (the page's
    // anchors, as the right-click teleport's inverse): buf_. False when there is none to draw.
    bool CachedWallIcons(Il2CppObject* shownDef) {
        buf_.clear();
        if (!cfg_.icons[mapi::kIconNullWall] || wallCache_.empty()) return false;
        float rgba[4] = {1, 1, 1, 1};
        Il2CppObject* sprite = mapi::NullWallSprite(rgba);
        Il2CppObject* vm = F<Il2CppObject*>(mapUI_.get(), s_.uiViewModel);
        if (!sprite || !Has(s_.vmMapRect) || !Has(s_.vmWorldRect) || !ugui::IsA(vm, s_.viewModel)) return false;
        Rect mr = F<Rect>(vm, s_.vmMapRect), wr = F<Rect>(vm, s_.vmWorldRect);
        if (std::fabs(wr.w) < 1e-6f || std::fabs(wr.h) < 1e-6f) return false;
        bool clip = Has(s_.vmFullRect);
        Rect fr = clip ? F<Rect>(vm, s_.vmFullRect) : Rect{};
        float x0 = std::min(fr.x, fr.x + fr.w), x1 = std::max(fr.x, fr.x + fr.w);
        float y0 = std::min(fr.y, fr.y + fr.h), y1 = std::max(fr.y, fr.y + fr.h);
        int intact = 0;
        for (const CachedWall& c : wallCache_) {
            if (!c.intact) continue;
            ++intact;
            float px = mr.x + (c.x - wr.x) * mr.w / wr.w, py = mr.y + (c.z - wr.y) * mr.h / wr.h;
            if (!std::isfinite(px) || !std::isfinite(py) || (clip && (px < x0 || px > x1 || py < y0 || py > y1))) continue;
            buf_.push_back({mapi::kIconNullWall, px, py, sprite, rgba[0], rgba[1], rgba[2], rgba[3]});
        }
        if (wallsLoggedDef_ != shownDef) {
            wallsLoggedDef_ = shownDef;
            SR2_INFO("map_tools: icons: {} of {} cached intact Nullifier wall(s) on '{}' ({} remembered)", buf_.size(), intact,
                     unity::GetName(shownDef), wallCache_.size());
        }
        return !buf_.empty();
    }

    // World units per local unit along x (uniform scale assumed, as uGUI pages are).
    static float Scale(Il2CppObject* t) {
        Vec3 a = ugui::TransformPoint(t, {0, 0, 0}), b = ugui::TransformPoint(t, {1, 0, 0});
        return std::hypot(b.x - a.x, b.y - a.y, b.z - a.z);
    }

    // Constant screen size: icon local scale = root canvas scale / page scale (the page carries the zoom).
    void UpdateIconScale() {
        if (!container_.Alive() || icons_.empty() || !mapT_.Alive()) return;
        if (!canvasT_.Alive()) {
            Il2CppObject* canvas = ugui::ComponentInParent(container_.get(), ugui::CanvasClass());
            Il2CppObject* root = canvas ? ugui::CanvasRoot(canvas) : nullptr;
            if (!root) return;
            canvasT_.Reset(unity::TransformOf(root));
        }
        float page = Scale(mapT_.get()), ref = Scale(canvasT_.get());
        if (!(page > 1e-9f) || !(ref > 1e-9f) || !std::isfinite(page) || !std::isfinite(ref)) return;
        float k = ref / page;
        if (lastK_ > 0 && std::fabs(k - lastK_) <= lastK_ * 0.005f) return;
        lastK_ = k;
        for (Icon& ic : icons_)
            if (ic.t.Alive()) ugui::SetLocalScale(ic.t.get(), {k, k, 1});
    }

    // -------------------------------------------------------------- legend ---
    void DestroyHint() {
        if (hintGo_.Alive()) ugui::Destroy(hintGo_.get());
        hintGo_.Reset(nullptr);
        hintLabel_.Reset(nullptr);
        hintKey_.Reset(nullptr);
        hintIcons_.clear();
        hintLegend_.Reset(nullptr);
        hintPlaced_ = false;
    }

    // The key bar lays its hints out left to right in sibling order (the clone, last, showed on the far right).
    // "[RMB] Teleport" goes right before the Place Waypoint hint: the legend's InputHint whose input event
    // (InputHint._configuration.InputEvent) or label names a waypoint / marker. Logged once per session.
    void PlaceHint() {
        if (!hintGo_.Alive() || !hintLegend_.Alive() || !s_.setSiblingIndex || !s_.getSiblingIndex) {
            hintPlaced_ = true;  // nothing to do
            return;
        }
        auto* hints = F<Il2CppArray*>(hintLegend_.get(), s_.legendHints);
        if (!ugui::PlausiblePointer(hints)) return;
        Il2CppObject* tr = unity::TransformOf(hintGo_.get());
        Il2CppObject* parent = ugui::GetParent(tr);
        Il2CppObject *target = nullptr, *fallback = nullptr;
        std::string seen, targetDesc, fallbackDesc;
        for (size_t i = 0, n = std::min<size_t>(ugui::ArrayCount(hints), 32); i < n && !target; ++i) {
            Il2CppObject* h = ugui::ArrayItem(hints, i);
            if (!ugui::IsA(h, s_.hint) || !unity::ActiveInHierarchy(unity::GameObjectOf(h))) continue;
            std::string ev, label;
            Il2CppObject* cfg = Has(s_.hintConfig) ? F<Il2CppObject*>(h, s_.hintConfig) : nullptr;
            Il2CppObject* e = ugui::PlausiblePointer(cfg) && Has(s_.hcEvent) ? F<Il2CppObject*>(cfg, s_.hcEvent) : nullptr;
            if (ugui::PlausiblePointer(e) && unity::IsAlive(e)) ev = unity::GetName(e);
            Il2CppObject* lt = Has(s_.hintLabelText) ? F<Il2CppObject*>(h, s_.hintLabelText) : nullptr;
            if (s_.tmpGetText && ugui::PlausiblePointer(lt) && unity::IsAlive(lt)) {
                Il2CppString* str = nullptr;
                Managed([&] { str = ugui::Call<Il2CppString*>(s_.tmpGetText, lt); });
                label = unity::ToUtf8(str);
            }
            std::string desc = std::format("'{}' (event '{}')", label, ev);
            seen += (seen.empty() ? "" : ", ") + desc;
            std::string key = ev + " " + label;
            for (char& ch : key) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            bool waypoint = key.find("waypoint") != std::string::npos;
            if (!waypoint && (fallback || key.find("marker") == std::string::npos)) continue;
            Il2CppObject* ht = unity::TransformOf(unity::GameObjectOf(h));
            if (ugui::GetParent(ht) != parent) continue;
            if (waypoint) {
                target = ht;
                targetDesc = desc;
            } else {
                fallback = ht;  // "marker": only when no hint says "waypoint"
                fallbackDesc = desc;
            }
        }
        if (!target && fallback) {
            target = fallback;
            targetDesc = fallbackDesc;
        }
        if (!target) {
            if (!hintPlaceLogged_ && time_ - hintSince_ >= kHintDiagDelay) {
                hintPlaceLogged_ = true;
                SR2_INFO("map_tools: [RMB] Teleport hint: no Place Waypoint hint in the key bar (active hints: {}) - kept on "
                         "the right",
                         seen.empty() ? "none" : seen);
            }
            return;
        }
        int idx = -1, mine = -1;
        Managed([&] {
            idx = ugui::Call<int>(s_.getSiblingIndex, target);
            mine = ugui::Call<int>(s_.getSiblingIndex, tr);
        });
        if (idx < 0) return;
        if (mine >= 0 && mine < idx) --idx;  // our own slot frees up when we move
        Managed([&] { ugui::Call<void>(s_.setSiblingIndex, tr, idx); });
        hintPlaced_ = true;
        if (!hintPlaceLogged_) {
            hintPlaceLogged_ = true;
            SR2_INFO("map_tools: [RMB] Teleport hint placed left of the key bar's {} (sibling index {}; active hints: {})",
                     targetDesc, idx, seen);
        }
    }

    static bool IsUnder(Il2CppObject* t, Il2CppObject* ancestor) {
        for (int guard = 0; t && guard < 64; ++guard, t = ugui::GetParent(t))
            if (t == ancestor) return true;
        return false;
    }

    void UpdateLegend() {
        if (!cfg_.teleport || !TeleportResolved() || !LegendResolved()) {
            if (hintGo_) DestroyHint();
            return;
        }
        if (hintGo_.Alive()) {
            if (hintReapply_ > 0) {
                --hintReapply_;
                ApplyHintLook();
            }
            if (!hintPlaced_ && time_ >= hintPlaceAt_) {  // the Place Waypoint hint may show up later
                hintPlaceAt_ = time_ + 0.5f;
                PlaceHint();
            }
            return;
        }
        if (hintGo_) DestroyHint();  // died with the legend
        if (time_ < hintRetryAt_) return;
        hintRetryAt_ = time_ + 0.5f;  // the legend may not show its hints yet
        if (hintSince_ < 0) hintSince_ = time_;
        std::string why;
        if (EnsureHint(why) || hintDiagLogged_ || time_ - hintSince_ < kHintDiagDelay) return;
        hintDiagLogged_ = true;
        SR2_INFO("map_tools: no [RMB] Teleport hint {:.0f} s after the map opened: {}", kHintDiagDelay, why);
    }

    // The map's key bar: MapUI._inputLegend (ComponentInterface<IInputLegend>)._object. That can be an
    // InputLegendFocusRegistrar, which forwards to another IInputLegend through its own _passThrough
    // (ComponentInterface<IInputLegend>), possibly several hops; the chain ends at the InputLegend. Fallback: the
    // first active InputLegend under the last component of the chain, then under the MapUI.
    Il2CppObject* FindLegend(std::string& chain) {
        auto under = [&](Il2CppObject* comp) -> Il2CppObject* {
            if (!comp || !unity::IsAlive(comp)) return nullptr;
            Il2CppObject* found = nullptr;
            ugui::ForEachInChildren(unity::GameObjectOf(comp), s_.legend, false, [&](Il2CppObject* l) {
                found = l;
                return false;
            });
            return found;
        };
        Il2CppObject* iface = F<Il2CppObject*>(mapUI_.get(), s_.uiLegend);
        Il2CppObject* obj = ugui::PlausiblePointer(iface) ? F<Il2CppObject*>(iface, s_.ciObject) : nullptr;
        Il2CppObject* last = nullptr;
        chain = "MapUI._inputLegend";
        for (int hop = 0; hop < 8; ++hop) {
            if (!ugui::PlausiblePointer(obj)) {
                chain += " -> null";
                break;
            }
            chain += " -> " + unity::ClassName(obj);
            if (ugui::IsA(obj, s_.legend)) return obj;
            if (!ugui::IsA(obj, unity::ComponentClass()) || !unity::IsAlive(obj)) break;
            last = obj;
            if (!s_.registrar || !Has(s_.regPassThrough) || !ugui::IsA(obj, s_.registrar)) break;
            Il2CppObject* ci = F<Il2CppObject*>(obj, s_.regPassThrough);
            obj = ugui::PlausiblePointer(ci) ? F<Il2CppObject*>(ci, s_.ciObject) : nullptr;
        }
        if (Il2CppObject* l = under(last)) {
            chain += " (InputLegend found under '" + unity::GetName(unity::GameObjectOf(last)) + "')";
            return l;
        }
        if (Il2CppObject* l = under(mapUI_.get())) {
            chain += " (InputLegend found under the MapUI)";
            return l;
        }
        return nullptr;
    }

    // "[RMB] Teleport": a clone of one of the legend's own hints with its behaviour removed (no input event,
    // no localisation, no clicks), our key text and label. A child of the same layout, so it lines up.
    bool EnsureHint(std::string& why) {
        std::string chain;
        Il2CppObject* legend = FindLegend(chain);
        if (!legend) {
            why = "no InputLegend (" + chain + ")";
            return false;
        }
        auto* hints = F<Il2CppArray*>(legend, s_.legendHints);
        if (!ugui::PlausiblePointer(hints)) {
            why = "the legend has no _hints array (" + chain + ")";
            return false;
        }
        Il2CppObject* tmpl = nullptr;
        size_t count = std::min<size_t>(ugui::ArrayCount(hints), 32);
        for (size_t i = 0; i < count && !tmpl; ++i) {
            Il2CppObject* h = ugui::ArrayItem(hints, i);
            if (ugui::IsA(h, s_.hint) && unity::ActiveInHierarchy(unity::GameObjectOf(h))) tmpl = h;
        }
        if (!tmpl) {  // the legend has not shown its hints yet
            why = std::format("none of the legend's {} hints is active ({})", count, chain);
            return false;
        }
        Il2CppObject* tmplGo = unity::GameObjectOf(tmpl);
        Il2CppObject* parent = ugui::GetParent(unity::TransformOf(tmplGo));
        Il2CppObject* clone = unity::Instantiate(tmplGo, parent);
        if (!unity::IsAlive(clone)) {
            if (!hintFailLogged_) SR2_WARN("map_tools: cloning a legend hint failed - no [RMB] Teleport hint");
            hintFailLogged_ = true;
            why = "Object.Instantiate failed";
            return false;
        }
        Il2CppObject* go = ugui::IsA(clone, unity::GameObjectClass()) ? clone : unity::GameObjectOf(clone);
        hintGo_.Reset(go);
        unity::SetName(go, "SR2Mods.TeleportHint");
        Il2CppObject* tr = unity::TransformOf(go);
        ugui::SetAsLastSibling(tr);
        hintLegend_.Reset(legend);
        hintPlaced_ = false;
        hintPlaceAt_ = 0;
        // Behaviour off: the hint would configure itself from the template's input event and fire it on click.
        if (Il2CppObject* h = unity::GetComponent(go, s_.hint)) ugui::Destroy(h);
        if (s_.invokeOnClick)
            ugui::ForEachInChildren(go, s_.invokeOnClick, true, [&](Il2CppObject* c) {
                ugui::Destroy(c);
                return true;
            });
        for (Il2CppClass* k : {s_.eventDisplay, s_.lse, s_.selectable}) {
            if (!k) continue;
            ugui::ForEachInChildren(go, k, true, [&](Il2CppObject* c) {
                SetEnabled(c, false);
                return true;
            });
        }
        ugui::ForEachInChildren(go, ugui::GraphicClass(), true, [&](Il2CppObject* g) {
            ugui::GraphicSetRaycastTarget(g, false);
            return true;
        });
        // InputHint fades its CanvasGroup with a spring: the clone keeps the template's alpha of this frame
        if (Il2CppClass* cgk = ugui::CanvasGroupClass())
            if (Il2CppObject* cg = unity::GetComponent(go, cgk)) ugui::CanvasGroupSetAlpha(cg, 1.0f);
        // key icons and the label (the TMP text outside every icon)
        std::vector<Il2CppObject*> iconTs;
        ugui::ForEachInChildren(go, s_.inputIcon, true, [&](Il2CppObject* ic) {
            hintIcons_.emplace_back(ic);
            iconTs.push_back(unity::TransformOf(ic));
            return true;
        });
        if (Il2CppClass* tmp = ugui::TmpTextClass())
            ugui::ForEachInChildren(go, tmp, true, [&](Il2CppObject* t) {
                Il2CppObject* tt = unity::TransformOf(t);
                for (Il2CppObject* it : iconTs)
                    if (IsUnder(tt, it)) return true;
                hintLabel_.Reset(t);
                return false;
            });
        FindMouseGlyph();
        hintReapply_ = kReapplyFrames;
        ApplyHintLook();
        if (!hintLogged_) {
            hintLogged_ = true;
            SR2_INFO("map_tools: [RMB] Teleport hint cloned from the map legend's '{}' ({} key icon(s), label {}, icon {}; {})",
                     unity::GetName(tmplGo), hintIcons_.size(), hintLabel_ ? "ok" : "MISSING",
                     !mouseGlyph_.Alive()      ? "key cap \"RMB\" (no mouse glyph)"
                     : mouseGlyphMirror_ ? "the game's left mouse button glyph, mirrored"
                                         : "the game's right mouse button glyph",
                     chain);
        }
        return true;
    }

    // The game's own mouse button glyphs: every KeyboardMouseIconSet asset (the key bar's icon set for keyboard
    // and mouse; InputIconSet<KeyboardMouseIcon>._items), the entry whose input control path is the right mouse
    // button ("<Mouse>/rightButton") with a sprite; else the left button's, drawn mirrored. One asset scan per map
    // open until found.
    void FindMouseGlyph() {
        if (mouseGlyph_.Alive() || !s_.kmIconSet || !Has(s_.kmItems) || !Has(s_.kmControl) || !Has(s_.kmSprite) ||
            !Has(s_.iconButtonImage))
            return;
        Il2CppObject *right = nullptr, *left = nullptr;
        std::string rightCtl, leftCtl;
        int sets = 0, entries = 0;
        ugui::ForEachOfType(s_.kmIconSet, [&](Il2CppObject* set) {
            if (!ugui::IsA(set, s_.kmIconSet)) return true;
            ++sets;
            Il2CppObject* items = F<Il2CppObject*>(set, s_.kmItems);
            if (!ugui::PlausiblePointer(items)) return true;
            for (size_t i = 0, n = std::min<size_t>(unity::ListCount(items), 512); i < n; ++i) {
                auto* icon = static_cast<Il2CppObject*>(unity::ListAt(items, i));
                if (!ugui::PlausiblePointer(icon)) continue;
                ++entries;
                Il2CppObject* sprite = F<Il2CppObject*>(icon, s_.kmSprite);
                if (!sprite || !unity::IsAlive(sprite)) continue;
                std::string ctl = unity::ToUtf8(F<Il2CppString*>(icon, s_.kmControl));
                if (ctl.find("<Mouse>") == std::string::npos) continue;
                if (!right && ctl.find("rightButton") != std::string::npos) {
                    right = sprite;
                    rightCtl = ctl;
                }
                if (!left && ctl.find("leftButton") != std::string::npos) {
                    left = sprite;
                    leftCtl = ctl;
                }
            }
            return !right;
        });
        mouseGlyph_.Reset(right ? right : left);
        mouseGlyphMirror_ = !right && left;
        if (!mouseGlyphSearched_ || mouseGlyph_) {
            mouseGlyphSearched_ = true;
            SR2_INFO("map_tools: mouse glyphs: {} KeyboardMouseIconSet(s), {} entries; right button {}, left button {}{}",
                     sets, entries, right ? "'" + unity::GetName(right) + "' (" + rightCtl + ")" : std::string("none"),
                     left ? "'" + unity::GetName(left) + "' (" + leftCtl + ")" : std::string("none"),
                     mouseGlyph_ ? (mouseGlyphMirror_ ? " - the hint uses the left one mirrored" : "")
                                 : " - the hint keeps the \"RMB\" key cap");
        }
    }

    // InputIcon.SetKey by hand (see modules/drone_price_limit/station_dialog.cpp): the first icon shows the
    // key cap with our text, the others are hidden.
    void ApplyHintLook() {
        for (size_t i = 0; i < hintIcons_.size(); ++i) {
            Il2CppObject* icon = hintIcons_[i].get();
            if (!unity::IsAlive(icon)) continue;
            Il2CppObject* go = unity::GameObjectOf(icon);
            if (i > 0) {
                unity::SetActive(go, false);
                continue;
            }
            unity::SetActive(go, true);
            auto obj = [&](size_t off) -> Il2CppObject* {
                if (!Has(off)) return nullptr;
                Il2CppObject* o = F<Il2CppObject*>(icon, off);
                return unity::IsAlive(o) ? o : nullptr;
            };
            if (Il2CppObject* d = obj(s_.iconDouble)) unity::SetActive(d, false);
            // The game's mouse glyph, as InputIcon.SetButtonImage: _buttonRoot on, _keyRoot off, _buttonImage.sprite.
            Il2CppObject* glyph = mouseGlyph_.Alive() ? mouseGlyph_.get() : nullptr;
            Il2CppObject *button = obj(s_.iconButton), *image = obj(s_.iconButtonImage);
            if (glyph && button && image) {
                unity::SetActive(button, true);
                if (Il2CppObject* k = obj(s_.iconKey)) unity::SetActive(k, false);
                SetEnabled(image, true);
                ugui::ImageSetSprite(image, glyph);
                ugui::SetLocalScale(unity::TransformOf(image), {mouseGlyphMirror_ ? -1.0f : 1.0f, 1, 1});
                continue;
            }
            if (button) unity::SetActive(button, false);
            if (Il2CppObject* k = obj(s_.iconKey)) unity::SetActive(k, true);
            SetEnabled(obj(s_.iconKeyImage), false);
            SetEnabled(obj(s_.iconKeyLse), false);
            if (Il2CppObject* text = obj(s_.iconKeyText)) {
                SetEnabled(text, true);
                ugui::TmpSetText(text, "RMB");
            }
        }
        if (hintLabel_.Alive()) ugui::TmpSetText(hintLabel_.get(), "Teleport");
    }

    // ------------------------------------------------------------ teleport ---
    void PlayFailCue() {
        if (!s_.playCue || !Has(s_.uiFailCue) || !mapUI_.Alive()) return;
        void* cue = reinterpret_cast<char*>(mapUI_.get()) + s_.uiFailCue;  // UIAudioCueHandle (struct field)
        Managed([&] { ugui::Call<bool>(s_.playCue, cue, 3 /* NO_WARNING */); });
    }

    // Log line + (when the HUD allows it) the game's drone-style pop-up with the teleporter icon.
    void Notice(const std::string& text) {
        if (!notices_.TextReady()) return;
        Il2CppObject* icon = notices_.TeleporterIcon();
        notices_.QueueText(icon, text);
    }

    // `why` for the log, `shortWhy` for the pop-up. The fail cue is for targets that cannot be reached at all.
    void Refuse(const std::string& why, const std::string& shortWhy = {}, bool cue = true) {
        SR2_INFO("map_tools: teleport refused: {}", why);
        Notice("Teleport refused: " + (shortWhy.empty() ? why : shortWhy));
        if (cue) PlayFailCue();
    }

    Il2CppObject* ZoneAt(Vec2 page) {
        if (!s_.tryFindZoneAt || !Has(s_.areaZoneData) || !mapT_.Alive()) return nullptr;
        Vec3 w = ugui::TransformPoint(mapT_.get(), {page.x, page.y, 0});  // the UI world position the game uses
        Il2CppObject* area = nullptr;
        Managed([&] { area = ugui::Call<Il2CppObject*>(s_.tryFindZoneAt, mapUI_.get(), Vec2{w.x, w.y}); });
        if (!ugui::IsA(area, s_.zoneArea)) return nullptr;
        Il2CppObject* zmd = F<Il2CppObject*>(area, s_.areaZoneData);
        return ugui::PlausiblePointer(zmd) ? zmd : nullptr;
    }

    // The Sanctuary (reached only at the end of the Grey Labyrinth's story: the scripted
    // TeleporterSanctuaryDestination / the boat ride) and the Labyrinth itself: arriving early may skip parts of the
    // story. Only logged; allowed with Reveal everything. Matched by the zone's asset or display name.
    bool StoryZone(Il2CppObject* zone) {
        if (!zone) return false;
        for (const std::string& n : {unity::GetName(zone), ZoneName(zone)})
            if (n.find("Sanctuary") != std::string::npos || n.find("Labyrinth") != std::string::npos) return true;
        return false;
    }

    // ZoneMapData -> its primary ZoneDefinition.
    Il2CppObject* PrimaryZone(Il2CppObject* zmd) {
        if (!zmd || !Has(s_.zmdPrimary)) return nullptr;
        Il2CppObject* z = F<Il2CppObject*>(zmd, s_.zmdPrimary);
        return ugui::PlausiblePointer(z) && unity::IsAlive(z) ? z : nullptr;
    }

    // The zone's localized name (ZoneDefinition._localizedName.GetLocalizedString()), else its asset name.
    std::string ZoneName(Il2CppObject* zone) {
        if (!zone) return "?";
        auto it = zoneNames_.find(zone);
        if (it != zoneNames_.end()) return it->second;
        std::string name;
        if (Has(s_.zoneLocName) && s_.lsGetLocalized) {
            Il2CppObject* ls = F<Il2CppObject*>(zone, s_.zoneLocName);
            Il2CppString* str = nullptr;
            if (ugui::PlausiblePointer(ls)) Managed([&] { str = ugui::Call<Il2CppString*>(s_.lsGetLocalized, ls); });
            name = unity::ToUtf8(str);
        }
        if (name.empty()) name = unity::GetName(zone);
        zonePins_.emplace_back(zone);  // the map key stays the same object
        return zoneNames_.emplace(zone, name).first->second;
    }

    // Has the player been in this zone (the map's own test: MapDirector.IsZoneHidden, called past our hook)?
    bool ZoneDiscovered(Il2CppObject* zmd) {
        if (!zmd || !Has(s_.uiDirector)) return true;
        Il2CppObject* director = F<Il2CppObject*>(mapUI_.get(), s_.uiDirector);
        if (!ugui::IsA(director, s_.mapDirector)) return true;
        bool hidden = false;
        if (hookHidden_ && g_origIsHidden) Managed([&] { hidden = g_origIsHidden(director, zmd, nullptr); });
        else if (s_.isZoneHiddenFn) Managed([&] { hidden = ugui::Call<bool>(s_.isZoneHiddenFn, director, zmd); });
        return !hidden;
    }

    struct Route {
        Ref node;              // TeleporterNodeDefinition (its NodeId); empty when inScene
        Ref group;             // destination SceneGroup
        bool inScene = false;  // the zone is in the player's own scene group: no load
        std::string name;      // node id
        std::string others;    // the other candidate nodes (log)
    };

    // Scene groups of a zone: ZoneSceneGroupMapping._mappings (List<ZoneMapping{Zone, SceneGroups[],
    // SceneGroupListAsset}>), every entry of that zone. One scene group can hold several zones (The Conservatory
    // and Rainbow Fields are one group, ConservatoryFields), and the mapping's TryGetValue(group) answers only the
    // first zone of a group, so the route search goes zone -> groups. Built once per mapping asset.
    void BuildZoneGroups() {
        if (zoneGroupsBuilt_ || !zoneMap_.Alive() || !Has(s_.zsgMappings)) return;
        zoneGroupsBuilt_ = true;
        zoneGroups_.clear();
        Il2CppObject* list = F<Il2CppObject*>(zoneMap_.get(), s_.zsgMappings);
        if (!ugui::PlausiblePointer(list)) return;
        Ref pin(list);
        auto add = [&](Il2CppObject* zone, Il2CppObject* group) {
            if (!zone || !group || !unity::IsAlive(zone) || !unity::IsAlive(group)) return;
            auto& v = zoneGroups_[zone];
            for (const Ref& r : v)
                if (r.get() == group) return;
            if (v.empty()) zonePins_.emplace_back(zone);  // the map key stays the same object
            v.emplace_back(group);
        };
        ugui::ForEachListStructPair(list, "Zone", "SceneGroups", [&](Il2CppObject* zone, Il2CppObject* arr) {
            auto* a = reinterpret_cast<Il2CppArray*>(arr);
            for (size_t i = 0, n = std::min<size_t>(ugui::ArrayCount(a), 64); i < n; ++i) add(zone, ugui::ArrayItem(a, i));
            return true;
        });
        if (Has(s_.sglItems))
            ugui::ForEachListStructPair(list, "Zone", "SceneGroupListAsset", [&](Il2CppObject* zone, Il2CppObject* asset) {
                if (!asset || !unity::IsAlive(asset)) return true;
                Il2CppObject* items = F<Il2CppObject*>(asset, s_.sglItems);
                if (!ugui::PlausiblePointer(items)) return true;
                for (size_t i = 0, n = std::min<size_t>(unity::ListCount(items), 64); i < n; ++i)
                    add(zone, static_cast<Il2CppObject*>(unity::ListAt(items, i)));
                return true;
            });
        SR2_INFO("map_tools: zone <-> scene group mapping: {} zone(s) with scene groups", zoneGroups_.size());
    }

    // The game's default gameplay scene group (SceneLoader.DefaultGameplaySceneGroup): the ranch's.
    Il2CppObject* DefaultGroup() {
        if (!s_.sceneLoaderClass || !s_.defaultGroup) return nullptr;
        if (!sceneLoader_.Alive()) sceneLoader_.Reset(ugui::FindFirst(s_.sceneLoaderClass, false));
        Il2CppObject* g = nullptr;
        if (sceneLoader_.Alive()) Managed([&] { g = ugui::Call<Il2CppObject*>(s_.defaultGroup, sceneLoader_.get()); });
        return g && unity::IsAlive(g) ? g : nullptr;
    }

    // A ranch zone (ZoneDefinition._isRanchZone) with no mapping entry: its scene group is the default gameplay
    // group. "No way into The Conservatory" (in-game test): the mapping has entries for the six other zones only
    // (the route log listed Rainbow Fields with ConservatoryFields, no Conservatory), so the zone -> groups search
    // found nothing for it from any zone. The first lookup is logged.
    Il2CppObject* RanchGroup(Il2CppObject* zone) {
        if (!zone || !Has(s_.zoneRanch) || !F<bool>(zone, s_.zoneRanch)) return nullptr;
        Il2CppObject* g = DefaultGroup();
        if (!ranchLogged_) {
            ranchLogged_ = true;
            SR2_INFO("map_tools: zone '{}' has no scene group mapping entry; it is the ranch zone, so its scene group is the "
                     "default gameplay group '{}'",
                     ZoneName(zone), g ? unity::GetName(g) : std::string("(none)"));
        }
        return g;
    }

    // Is `group` one of the zone's scene groups? Without a mapping entry for the zone: the ranch's default group
    // for a ranch zone, else the mapping's own TryGetValue(group) == zone.
    bool GroupInZone(Il2CppObject* zone, Il2CppObject* group) {
        if (!zone || !group) return false;
        BuildZoneGroups();
        auto it = zoneGroups_.find(zone);
        if (it != zoneGroups_.end()) {
            for (const Ref& r : it->second)
                if (r.get() == group) return true;
            return false;
        }
        if (Il2CppObject* ranch = RanchGroup(zone)) return ranch == group;
        if (!zoneMap_.Alive() || !s_.zsgTryGet) return false;
        Il2CppObject* z = nullptr;
        bool got = false;
        Managed([&] { got = ugui::Call<bool>(s_.zsgTryGet, zoneMap_.get(), group, &z); });
        return got && z == zone;
    }

    std::string NodeName(Il2CppObject* node) {
        auto it = nodeNames_.find(node);
        if (it != nodeNames_.end()) return it->second;
        Il2CppString* id = nullptr;
        Managed([&] { id = ugui::Call<Il2CppString*>(s_.nodeId, node); });
        std::string n = id ? unity::ToUtf8(id) : std::string();
        if (n.empty()) n = unity::GetName(node);
        return nodeNames_.emplace(node, n).first->second;
    }

    // The teleport network and the zone mapping (found lazily; the mapping with one asset scan, re-tried every 30 s).
    bool EnsureNetwork(std::string& why) {
        if (!net_.Alive()) net_.Reset(ugui::FindFirst(s_.teleportNetwork, false));
        if (!ugui::IsA(net_.get(), s_.teleportNetwork)) {
            why = "no TeleportNetwork";
            return false;
        }
        if (!zoneMap_.Alive() && time_ >= zoneMapRetry_) {
            zoneMapRetry_ = time_ + 30.0f;
            ugui::ForEachOfType(s_.zoneSgMapping, [&](Il2CppObject* m) {  // one asset scan (the map UI holds it)
                if (!ugui::IsA(m, s_.zoneSgMapping)) return true;
                zoneMap_.Reset(m);
                return false;
            });
            zoneGroupsBuilt_ = false;
            SR2_INFO("map_tools: zone <-> scene group mapping {}", zoneMap_ ? "found" : "not loaded");
        }
        if (!zoneMap_.Alive()) {
            why = "the zone <-> scene group mapping is not loaded";
            return false;
        }
        return true;
    }

    // Every destination of the game's static teleport network (TeleportNetwork._networkDefinition:
    // TeleportSystemDefinition.DestinationNodes = {DestinationNode, DestinationSceneGroup}): the nodes the game's
    // own zone-to-zone teleporters arrive at, registered once their scene group is loaded.
    template <class Fn>
    int ForEachDestination(Fn&& fn) {
        Il2CppObject* def = F<Il2CppObject*>(net_.get(), s_.tnDefinition);
        if (!ugui::PlausiblePointer(def) || !unity::IsAlive(def)) return -1;
        Il2CppObject* systems = F<Il2CppObject*>(def, s_.listItems);
        Ref pinSystems(systems);
        int checked = 0;
        for (size_t i = 0, n = std::min<size_t>(unity::ListCount(systems), 512); i < n; ++i) {
            auto* sys = static_cast<Il2CppObject*>(unity::ListAt(systems, i));
            if (!ugui::PlausiblePointer(sys)) continue;
            Il2CppObject* dests = F<Il2CppObject*>(sys, s_.tsdDestinations);
            for (size_t j = 0, m = std::min<size_t>(unity::ListCount(dests), 64); j < m; ++j) {
                auto* d = static_cast<Il2CppObject*>(unity::ListAt(dests, j));
                if (!ugui::PlausiblePointer(d)) continue;
                Il2CppObject* node = F<Il2CppObject*>(d, s_.tddNode);
                Il2CppObject* group = F<Il2CppObject*>(d, s_.tddGroup);
                if (!unity::IsAlive(node) || !unity::IsAlive(group)) continue;
                ++checked;
                fn(node, group);
            }
        }
        return checked;
    }

    // A way into `zone`: in-scene when the player's scene group is one of the zone's groups (the Conservatory from
    // Rainbow Fields), else a destination of the static teleport network that arrives in one of its groups. Nodes
    // named "OneWay" come last (the Grey Labyrinth's 'TeleporterOneWayLabyrinthHub' arrival never completed in a
    // test: the load ran and the player was back in the start zone).
    bool FindRoute(Il2CppObject* zone, Il2CppObject* myGroup, Route& out, std::string& why) {
        if (!EnsureNetwork(why)) return false;
        if (myGroup && GroupInZone(zone, myGroup)) {
            out.inScene = true;
            out.group.Reset(myGroup);
            return true;
        }
        Il2CppObject *best = nullptr, *bestGroup = nullptr;
        bool bestOneWay = true;
        std::vector<std::string> names;
        int checked = ForEachDestination([&](Il2CppObject* node, Il2CppObject* group) {
            if (!GroupInZone(zone, group)) return;
            std::string n = NodeName(node);
            bool oneWay = n.find("OneWay") != std::string::npos;
            names.push_back(n);
            if (!best || (bestOneWay && !oneWay)) {
                best = node;
                bestGroup = group;
                bestOneWay = oneWay;
                out.name = n;
            }
        });
        if (checked < 0) {
            why = "no static teleport network";
            return false;
        }
        if (!best) {
            std::string groups;
            if (auto it = zoneGroups_.find(zone); it != zoneGroups_.end())
                for (const Ref& r : it->second) groups += (groups.empty() ? "" : ", ") + unity::GetName(r.get());
            else if (Il2CppObject* ranch = RanchGroup(zone))
                groups = unity::GetName(ranch) + " (ranch zone: default group)";
            why = groups.empty() ? std::format("none of the {} teleport destinations leads into it (no scene group mapping "
                                               "entry for the zone)",
                                               checked)
                                 : std::format("none of the {} teleport destinations arrives in its scene group(s) {}", checked,
                                               groups);
            return false;
        }
        for (const std::string& n : names)
            if (n != out.name) out.others += (out.others.empty() ? "" : ", ") + n;
        out.node.Reset(best);
        out.group.Reset(bestGroup);
        return true;
    }

    // Once per session (first cross-zone click): which arrival point every zone of the mapping would use.
    void LogRoutes() {
        if (routesLogged_ || !zoneMap_.Alive()) return;
        routesLogged_ = true;
        BuildZoneGroups();
        std::vector<Il2CppObject*> zones;
        for (auto& [z, groups] : zoneGroups_) zones.push_back(z);
        for (Il2CppObject* z : zones) {
            if (!unity::IsAlive(z)) continue;
            std::string groups;
            for (const Ref& r : zoneGroups_[z]) groups += (groups.empty() ? "" : ", ") + unity::GetName(r.get());
            Route r;
            std::string why;
            if (FindRoute(z, nullptr, r, why))
                SR2_INFO("map_tools: route: '{}' (scene groups {}): arrival '{}' in '{}'{}{}", ZoneName(z), groups, r.name,
                         unity::GetName(r.group.get()), r.others.empty() ? "" : "; other arrivals: ", r.others);
            else
                SR2_INFO("map_tools: route: '{}' (scene groups {}): no arrival ({})", ZoneName(z), groups, why);
        }
    }

    // The shared ground teleport (ground_teleport.h) at the clicked point; the result comes now or a few seconds
    // later (far away: the area is loaded first). `afterTravel`: this ends a zone change (age = seconds since the
    // click).
    bool StartGround(float wx, float wz, const std::string& zoneName, bool hasZone, bool afterTravel, float age,
                     std::string& why) {
        return ground_teleport::Start(
            kModule, wx, wz,
            [this, zoneName, hasZone, afterTravel, age, wx, wz](const ground_teleport::Result& r) {
                if (g_dead.load()) return;
                if (r.ok) {
                    SR2_INFO("map_tools: {}teleported from ({:.1f}, {:.1f}, {:.1f}) to ({:.1f}, {:.1f}, {:.1f}) (zone '{}'{}){}",
                             afterTravel ? "arrived and " : "", r.from.x, r.from.y, r.from.z, r.to.x, r.to.y, r.to.z, zoneName,
                             r.loadedFirst ? std::format(", area loaded first in {:.1f} s", r.seconds) : std::string(),
                             afterTravel ? std::format(", {:.1f} s after the click", age + r.seconds) : std::string());
                    Notice(hasZone ? "Teleported to " + zoneName : std::string("Teleported"));
                } else if (afterTravel) {
                    SR2_INFO("map_tools: arrived in '{}', but not teleported to ({:.0f}, {:.0f}) ({}) - staying at the "
                             "arrival point",
                             zoneName, wx, wz, r.why);
                    Notice("Teleported to " + zoneName + " (at the teleporter: no safe spot there)");
                } else {
                    SR2_INFO("map_tools: teleport refused: ({:.0f}, {:.0f}): {}", wx, wz, r.why);
                    Notice("Teleport refused: no safe spot there");
                }
            },
            &why);
    }

    Il2CppObject* Player() {
        if (!player_.Alive()) player_.Reset(ugui::FindFirst(s_.teleportable, false));
        return ugui::IsA(player_.get(), s_.teleportable) ? player_.get() : nullptr;
    }

    void CloseMap() {
        if (!s_.dmInstance || !s_.dmClose || !mapUI_.Alive()) return;
        Managed([&] {
            Il2CppObject* dm = ugui::Call<Il2CppObject*>(s_.dmInstance);
            if (dm) ugui::Call<Il2CppObject*>(s_.dmClose, dm, mapUI_.get());
        });
    }

    // Is the page point `pm` under the map's fog on the live page? 1 yes (`area` names it), 0 no, -1 cannot tell.
    // Fog areas: every FogMapElement of the page with its real state, FogMapElement.ShouldBeRevealed(
    // MapUIFogReveal._eventDirector) (what RevealKnownZones shows; fallback: its CanvasGroup alpha); its shape =
    // the rect of each of its Graphics (page-space parallelograms, as the minimap's fog test), else its own rect.
    // Static fog (fog-shader art outside every FogMapElement, except the outside-of-the-island negative mask)
    // counts as fog too: the game never reveals it. A point in a revealed area is clear even when a neighbouring
    // fogged area's rect also covers it (the rects are larger than the cloud shapes).
    int FogAt(Vec2 pm, std::string& area) {
        if (!s_.fogElement || !map_.Alive() || !mapT_.Alive()) return -1;
        Il2CppObject* director = nullptr;
        if (s_.fogShouldReveal && Has(s_.frDirector) && Has(s_.uiFogReveal)) {
            Il2CppObject* fr = F<Il2CppObject*>(mapUI_.get(), s_.uiFogReveal);
            Il2CppObject* d = ugui::IsA(fr, s_.fogReveal) ? F<Il2CppObject*>(fr, s_.frDirector) : nullptr;
            if (ugui::PlausiblePointer(d) && unity::IsAlive(d)) director = d;
        }
        Il2CppObject* mapGo = unity::GameObjectOf(map_.get());
        std::vector<Ref> elems;
        ugui::ForEachInChildren(mapGo, s_.fogElement, true, [&](Il2CppObject* f) {
            elems.emplace_back(f);
            return true;
        });
        auto inside = [&](Il2CppObject* t) {
            if (!ugui::IsRectTransform(t)) return false;
            Rect r = ugui::GetRect(t);
            const float xs[4] = {r.x, r.x + r.w, r.x + r.w, r.x}, ys[4] = {r.y, r.y, r.y + r.h, r.y + r.h};
            Vec2 q[4];
            for (int i = 0; i < 4; ++i) {
                Vec3 l = ugui::InverseTransformPoint(mapT_.get(), ugui::TransformPoint(t, {xs[i], ys[i], 0}));
                q[i] = {l.x, l.y};
            }
            int sign = 0;
            for (int i = 0; i < 4; ++i) {
                Vec2 a = q[i], b = q[(i + 1) % 4];
                float c = (b.x - a.x) * (pm.y - a.y) - (b.y - a.y) * (pm.x - a.x);
                int sg = c > 0 ? 1 : (c < 0 ? -1 : 0);
                if (!sg) continue;
                if (!sign) sign = sg;
                else if (sg != sign) return false;
            }
            return sign != 0;
        };
        auto contains = [&](Il2CppObject* root) {
            bool any = false, hit = false;
            ugui::ForEachInChildren(unity::GameObjectOf(root), ugui::GraphicClass(), false, [&](Il2CppObject* g) {
                any = true;
                hit = inside(unity::TransformOf(g));
                return !hit;
            });
            return any ? hit : inside(unity::TransformOf(root));
        };
        int fogged = 0, revealed = 0, unknown = 0, byAlpha = 0;
        bool inFog = false, inClear = false;
        for (Ref& e : elems) {
            Il2CppObject* f = e.get();
            if (!unity::IsAlive(f)) continue;
            int state = -1;  // 1 revealed, 0 fogged
            if (director) {
                bool rev = false;
                if (Managed([&] { rev = ugui::Call<bool>(s_.fogShouldReveal, f, director); })) state = rev ? 1 : 0;
            }
            if (state < 0 && Has(s_.fogCanvasGroup)) {
                Il2CppObject* cg = F<Il2CppObject*>(f, s_.fogCanvasGroup);
                if (ugui::PlausiblePointer(cg) && unity::IsAlive(cg)) {
                    state = ugui::CanvasGroupGetAlpha(cg) < 0.5f ? 1 : 0;
                    ++byAlpha;
                }
            }
            if (state < 0) {
                ++unknown;
                continue;
            }
            ++(state ? revealed : fogged);
            if (state ? inClear : inFog) continue;
            if (!contains(f)) continue;
            if (state) {
                inClear = true;
            } else {
                inFog = true;
                area = "fog area '" + unity::GetName(unity::GameObjectOf(f)) + "'";
            }
        }
        if (!inFog && !inClear) {  // static fog
            std::vector<Il2CppObject*> roots;
            for (Ref& e : elems)
                if (e.Alive()) roots.push_back(unity::TransformOf(e.get()));
            ugui::ForEachInChildren(mapGo, ugui::GraphicClass(), false, [&](Il2CppObject* g) {
                Il2CppObject* mat = ugui::GraphicGetMaterial(g);
                std::string shader = mat ? ugui::MaterialShaderName(mat) : std::string();
                if (!IsFogShader(shader) || shader.find("Negative") != std::string::npos || !ugui::BehaviourEnabled(g))
                    return true;
                for (Il2CppObject* t = unity::TransformOf(g); t && t != mapT_.get(); t = ugui::GetParent(t))
                    if (std::find(roots.begin(), roots.end(), t) != roots.end()) return true;
                if (!inside(unity::TransformOf(g))) return true;
                inFog = true;
                area = "static fog '" + unity::GetName(unity::GameObjectOf(g)) + "'";
                return false;
            });
        }
        int result = inFog && !inClear ? 1 : 0;
        SR2_INFO("map_tools: fog at map point ({:.0f}, {:.0f}): {}; {} fog area(s): {} fogged, {} revealed, {} unknown (state from "
                 "{})",
                 pm.x, pm.y,
                 result      ? "under " + area
                 : inClear   ? std::string("in a revealed area")
                             : std::string("outside every fog area"),
                 elems.size(), fogged, revealed, unknown,
                 byAlpha ? std::format("the CanvasGroup alpha for {}", byAlpha) : std::string("ShouldBeRevealed"));
        return elems.empty() && !inFog ? -1 : result;
    }

    void Teleport() {
        if (!clickLogged_) {
            clickLogged_ = true;
            SR2_INFO("map_tools: first right click on the full map received");
        }
        if (!TeleportResolved()) return Refuse("teleport symbols missing (see the 'resolved' line)", "not available");
        if (travel_.active) return Refuse("a zone change for the last click is still running", "still travelling", false);
        if (ground_teleport::Busy()) return Refuse("the last teleport is still loading its spot", "still teleporting", false);
        if (std::string busy; ground_teleport::PlayerUnavailable(&busy)) return Refuse(busy, busy, false);
        Il2CppObject* vm = F<Il2CppObject*>(mapUI_.get(), s_.uiViewModel);
        if (!ugui::IsA(vm, s_.viewModel)) return Refuse("no map view model", "map not ready");
        // The cursor's page point, as MapUI.LateUpdate computes it: -(mapAnchor + cursorOffset / zoom). The view
        // model is bound to the page shown (any tab), so are its rects below.
        Vec2 anchor = F<Vec2>(vm, s_.vmAnchor), cur = F<Vec2>(vm, s_.vmCursor);
        float zoom = F<float>(vm, s_.vmZoom);
        Rect mr = F<Rect>(vm, s_.vmMapRect), wr = F<Rect>(vm, s_.vmWorldRect);
        if (!(zoom > 1e-6f) || std::fabs(mr.w) < 1e-6f || std::fabs(mr.h) < 1e-6f || std::fabs(wr.w) < 1e-6f ||
            std::fabs(wr.h) < 1e-6f)
            return Refuse("map projection not ready", "map not ready");
        Vec2 pm{-(anchor.x + cur.x / zoom), -(anchor.y + cur.y / zoom)};
        if (Has(s_.vmFullRect)) {
            Rect fr = F<Rect>(vm, s_.vmFullRect);
            float x0 = std::min(fr.x, fr.x + fr.w), x1 = std::max(fr.x, fr.x + fr.w);
            float y0 = std::min(fr.y, fr.y + fr.h), y1 = std::max(fr.y, fr.y + fr.h);
            if (pm.x < x0 || pm.x > x1 || pm.y < y0 || pm.y > y1)
                return Refuse("the click is outside the map page", "outside the map");
        }
        // inverse of Map.GetMapPosition: map = mapMin + (world - worldMin) * mapSize / worldSize
        float wx = wr.x + (pm.x - mr.x) * wr.w / mr.w;
        float wz = wr.y + (pm.y - mr.y) * wr.h / mr.h;
        if (!std::isfinite(wx) || !std::isfinite(wz)) return Refuse("bad map position", "bad position");
        // Reveal off: no target under the map's fog (the player's real map, not the zone-visited flag).
        if (!cfg_.reveal) {
            std::string area;
            if (FogAt(pm, area) == 1)
                return Refuse(std::format("({:.0f}, {:.0f}) is under the map's fog ({}); turn on Reveal everything to go there",
                                          wx, wz, area),
                              "that spot is not on your map yet");
        }

        Il2CppObject* player = Player();
        if (!player) return Refuse("no player", "no player");
        Il2CppObject* myGroup = nullptr;
        Managed([&] { myGroup = ugui::Call<Il2CppObject*>(s_.playerSceneGroup, player); });
        // is the page shown the map of the player's zone?
        bool myMap = true;
        if (s_.currentMapDef && s_.getMapForZone && s_.playerCurrentZone && Has(s_.uiDirector)) {
            Il2CppObject* director = F<Il2CppObject*>(mapUI_.get(), s_.uiDirector);
            Il2CppObject *shown = nullptr, *mine = nullptr;
            if (ugui::IsA(director, s_.mapDirector))
                Managed([&] {
                    shown = ugui::Call<Il2CppObject*>(s_.currentMapDef, mapUI_.get());
                    Il2CppObject* zone = ugui::Call<Il2CppObject*>(s_.playerCurrentZone);
                    if (zone) mine = ugui::Call<Il2CppObject*>(s_.getMapForZone, director, zone);
                });
            myMap = !(shown && mine && shown != mine);
        }
        Vec3 pp = ugui::GetPosition(unity::TransformOf(player));
        Il2CppObject* zTarget = ZoneAt(pm);
        Il2CppObject* zPlayer = nullptr;
        if (myMap) {
            Vec2 ppage{mr.x + (pp.x - wr.x) * mr.w / wr.w, mr.y + (pp.z - wr.y) * mr.h / wr.h};
            zPlayer = ZoneAt(ppage);
        }
        Il2CppObject* targetZone = PrimaryZone(zTarget);
        std::string zoneName = targetZone ? ZoneName(targetZone) : std::string("-");

        // Same zone (or outside every zone area on the player's own map, e.g. the sea next to the island):
        // in-scene teleport.
        bool sameZone = myMap && (!zTarget || !zPlayer || zTarget == zPlayer);
        Route route;
        if (!sameZone) {
            if (!zTarget) return Refuse("the click is outside every zone of this map", "outside every zone");
            if (!ZoneDiscovered(zTarget)) {
                // "Reveal everything" also opens the undiscovered zones, the story zones included (the Grey
                // Labyrinth, The Sanctuary: the user accepts that arriving early may skip parts of the story).
                if (!cfg_.reveal)
                    return Refuse(std::format("'{}' is not discovered yet (enter it once first)", zoneName),
                                  zoneName + " not discovered yet");
                SR2_INFO("map_tools: '{}' is not discovered yet - allowed, Reveal everything is on{}", zoneName,
                         StoryZone(targetZone) ? " (a story zone: arriving early may skip parts of the story)" : "");
            }
            std::string why;
            if (!TravelResolved())
                return Refuse("zone travel symbols missing (see the 'resolved' line)", "zone travel not available");
            if (EnsureNetwork(why)) LogRoutes();
            if (!FindRoute(targetZone, myGroup, route, why))
                return Refuse(std::format("no way into '{}': {}", zoneName, why), "no way into " + zoneName);
            if (route.inScene) sameZone = true;  // another zone of the player's scene group: no load
        }
        if (sameZone) {
            std::string why;
            SR2_INFO("map_tools: teleport to ({:.1f}, {:.1f}) (map point ({:.1f}, {:.1f}), zone '{}') from ({:.1f}, {:.1f}, "
                     "{:.1f})",
                     wx, wz, pm.x, pm.y, zoneName, pp.x, pp.y, pp.z);
            if (!StartGround(wx, wz, zoneName, targetZone != nullptr, false, 0, why))
                return Refuse("ground teleport: " + why, why);
            CloseMap();
            return;
        }

        // Another scene group: the game's own zone-to-zone travel (loading screen), into the route's node, then
        // the clicked point once the zone is loaded (TickTravel).
        Il2CppString* nodeId = nullptr;
        Managed([&] { nodeId = ugui::Call<Il2CppString*>(s_.nodeId, route.node.get()); });
        if (!nodeId) return Refuse(std::format("the route into '{}' has no node id", zoneName), "no way into " + zoneName);
        bool ok = Managed([&] {
            ugui::Call<void>(s_.tpToDestImpl, net_.get(), player, nodeId, route.group.get(), false, false);
        });
        if (!ok) return Refuse("TeleportNetwork.TeleportToDestinationImpl threw (see above)", "travel failed");
        travel_ = {};
        travel_.active = true;
        travel_.x = wx;
        travel_.z = wz;
        travel_.group.Reset(route.group.get());
        travel_.origin.Reset(myGroup);
        travel_.zoneDef.Reset(targetZone);
        travel_.zone = zoneName;
        travel_.node = route.name;
        travel_.started = time_;
        SR2_INFO("map_tools: travelling to '{}' (scene group '{}', arrival node '{}'{}{}), then to ({:.1f}, {:.1f})", zoneName,
                 unity::GetName(route.group.get()), route.name, route.others.empty() ? "" : "; other arrivals: ",
                 route.others, wx, wz);
        CloseMap();
    }

    // After a zone change: wait for the load and the player in one of the zone's scene groups, then the ground
    // teleport at the clicked point (which loads the spot's area first if needed; no ground: the player stays at
    // the arrival node). A load that ends with the player back in the start group = the game's travel failed.
    void TickTravel() {
        if (!travel_.active) return;
        float age = time_ - travel_.started;
        if (age > kTravelTimeout) {
            SR2_WARN("map_tools: travel to '{}' (arrival node '{}'): not arrived after {:.0f} s - given up", travel_.zone,
                     travel_.node, kTravelTimeout);
            Notice("Teleport to " + travel_.zone + " did not finish");
            travel_ = {};
            return;
        }
        if (time_ < travel_.nextTry) return;
        travel_.nextTry = time_ + kTravelPoll;
        bool knockedOut = false;
        if (std::string why; ground_teleport::PlayerUnavailable(&why, &knockedOut) && knockedOut) {
            SR2_INFO("map_tools: travel to '{}' dropped: {} (the game's respawn takes over)", travel_.zone, why);
            Notice("Teleport to " + travel_.zone + " cancelled");
            travel_ = {};
            return;
        }
        if (s_.sceneLoaderClass && s_.loadInProgress) {
            if (!sceneLoader_.Alive()) sceneLoader_.Reset(ugui::FindFirst(s_.sceneLoaderClass, false));
            bool loading = false;
            if (sceneLoader_.Alive()) Managed([&] { loading = ugui::Call<bool>(s_.loadInProgress, sceneLoader_.get()); });
            if (loading) {
                travel_.sawLoading = true;
                travel_.arrivedAt = -1;
                return;
            }
        }
        Il2CppObject* player = Player();
        if (!player) return;
        Il2CppObject* group = nullptr;
        Managed([&] { group = ugui::Call<Il2CppObject*>(s_.playerSceneGroup, player); });
        bool there = group && (group == travel_.group.get() ||
                               (group != travel_.origin.get() && GroupInZone(travel_.zoneDef.get(), group)));
        if (!there) {
            if (travel_.sawLoading && group && group == travel_.origin.get()) {
                SR2_WARN("map_tools: travel to '{}' failed: the load ended with the player back in '{}' (arrival node '{}' "
                         "in '{}' not reached)",
                         travel_.zone, unity::GetName(group), travel_.node, unity::GetName(travel_.group.get()));
                Notice("Teleport to " + travel_.zone + " failed");
                travel_ = {};
            }
            return;
        }
        if (travel_.arrivedAt < 0) {
            travel_.arrivedAt = time_;  // let the arrival settle one step before the ground teleport
            return;
        }
        float wx = travel_.x, wz = travel_.z;
        std::string zone = travel_.zone, why;
        travel_ = {};
        if (!StartGround(wx, wz, zone, true, true, age, why)) {
            SR2_INFO("map_tools: arrived in '{}', no ground teleport ({}) - staying at the arrival point", zone, why);
            Notice("Teleported to " + zone + " (arrival point)");
        }
    }
};

// The impl owns GC handles and is never deleted (process exit may happen after the IL2CPP runtime is gone).
class MapToolsModule final : public core::Module {
public:
    const char* Name() const override { return kModule; }
    bool DefaultEnabled() const override { return true; }
    bool Setup(core::ModuleContext& ctx) override { return impl_->Setup(ctx); }
    void OnEnable() override { impl_->OnEnable(); }
    void OnDisable() override { impl_->OnDisable(); }

private:
    MapTools* impl_ = new MapTools();  // leaked on purpose
};

}  // namespace

std::unique_ptr<sr2::core::Module> CreateMapTools() { return std::make_unique<MapToolsModule>(); }

}  // namespace sr2::modules
