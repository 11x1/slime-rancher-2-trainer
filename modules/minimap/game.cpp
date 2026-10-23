#include "modules/minimap/game.h"

#include <format>

#include "core/resolver.h"

namespace sr2::modules::minimap::game {
namespace {

using il2cpp::Resolver;

constexpr const char* kAsm = "Assembly-CSharp";

struct Ctx {
    std::vector<std::string> req;
    std::vector<std::string>* opt;

    std::vector<std::string>& list(bool required) { return required ? req : *opt; }

    Il2CppClass* Class(const char* ns, const char* name, bool required) {
        std::string e;
        Il2CppClass* k = Resolver::Get().Class(kAsm, ns, name, &e);
        if (!k) list(required).push_back(e);
        return k;
    }
    const MethodInfo* Method(Il2CppClass* k, const char* name, int argc, bool required) {
        if (!k) return nullptr;
        std::string e;
        const MethodInfo* m = Resolver::Get().Method(k, name, argc, &e);
        if (!m) list(required).push_back(e);
        return m;
    }
    ugui::Fn Bind(const MethodInfo* m, const char* what, bool required) {
        if (!m) return {};
        ugui::Fn f = ugui::BindFn(m);
        if (!f) list(required).push_back(std::format("{}: no native method body", what));
        return f;
    }
    void* Target(Il2CppClass* k, const char* name, int argc) {
        const MethodInfo* m = Method(k, name, argc, false);
        void* p = m ? Resolver::Get().MethodPointer(m) : nullptr;
        if (m && !p) opt->push_back(std::format("{}: no native body (hook skipped)", name));
        return p;
    }
    size_t Field(Il2CppClass* k, const char* name, bool required) {
        if (!k) return kNoOffset;
        std::string e;
        auto off = Resolver::Get().FieldOffset(k, name, &e);
        if (!off) {
            list(required).push_back(e);
            return kNoOffset;
        }
        return *off;
    }
    size_t Inner(Il2CppClass* valueType, const char* name, bool required) {
        size_t out = 0;
        if (valueType && ugui::StructFieldOffset(valueType, name, out)) return out;
        list(required).push_back(std::format("struct field '{}' not resolvable", name));
        return kNoOffset;
    }
};

}  // namespace

bool Resolve(Symbols& s, std::string& err, std::vector<std::string>& optionalMissing) {
    Ctx c{{}, &optionalMissing};
    const char* nsMap = "MonomiPark.SlimeRancher.Map";
    const char* nsUiMap = "MonomiPark.SlimeRancher.UI.Map";

    // ---- required: map data, the map page prefab and its world->map projection
    s.mapDirector = c.Class(nsMap, "MapDirector", true);
    s.mapDefinition = c.Class(nsMap, "MapDefinition", true);
    s.mapPrefabMapping = c.Class(nsMap, "MapPrefabMapping", true);
    s.map = c.Class(nsUiMap, "Map", true);
    s.projectionHelper = c.Class(nsUiMap, "MapProjectionHelper", true);
    s.playerDisplay = c.Class("", "PlayerDisplayOnMap", true);
    s.mapUI = c.Class(nsUiMap, "MapUI", true);
    s.mapUIRoot = c.Class(nsUiMap, "MapUIRoot", true);

    s.getMapForZone = c.Bind(c.Method(s.mapDirector, "GetMapForZone", 1, true), "MapDirector.GetMapForZone", true);
    s.getPrefabFor = c.Bind(c.Method(s.mapPrefabMapping, "GetPrefabFor", 1, true), "MapPrefabMapping.GetPrefabFor", true);
    s.mapAnchor1 = c.Field(s.map, "_anchor1", true);
    s.mapAnchor2 = c.Field(s.map, "_anchor2", true);
    if (Il2CppClass* anchor = ugui::NestedClass(s.map, "MapAnchorCoordinates")) {
        s.anchorWorld = c.Inner(anchor, "WorldPosition", true);
        s.anchorMap = c.Inner(anchor, "MapPosition", true);
    } else {
        c.req.push_back("nested struct Map.MapAnchorCoordinates not found");
    }
    s.defBackground = c.Field(s.mapDefinition, "MapBackground", true);
    if (Il2CppClass* imageSetup = ugui::NestedClass(s.mapDefinition, "ImageSetup")) {
        s.imgSetupImage = c.Inner(imageSetup, "Image", true);
        s.imgSetupTint = c.Inner(imageSetup, "TintColor", false);
    } else {
        c.req.push_back("nested struct MapDefinition.ImageSetup not found");
    }
    s.mapUIPrefabMapping = c.Field(s.mapUI, "_mapPrefabMapping", true);
    s.mapUIDirector = c.Field(s.mapUI, "_mapDirector", false);
    s.mapUIRootDirector = c.Field(s.mapUIRoot, "_mapDirector", true);

    if (Il2CppClass* utils = c.Class(nsUiMap, "MapUIUtilities", false))
        s.getPlayerCurrentZone =
            c.Bind(c.Method(utils, "GetPlayerCurrentZone", 0, false), "MapUIUtilities.GetPlayerCurrentZone", false);
    s.playerGetZone =
        c.Bind(c.Method(s.playerDisplay, "GetZoneDefinition", 0, false), "PlayerDisplayOnMap.GetZoneDefinition", false);
    if (!s.getPlayerCurrentZone && !s.playerGetZone)
        c.req.push_back("no current-zone source (MapUIUtilities.GetPlayerCurrentZone / PlayerDisplayOnMap.GetZoneDefinition)");

    // ---- optional
    s.mapProjection = c.Field(s.map, "_projectionHelper", false);
    s.projScale = c.Field(s.projectionHelper, "_worldToMapScale", false);
    s.projOffset = c.Field(s.projectionHelper, "_worldToMapOffset", false);
    s.defOverlay = c.Field(s.mapDefinition, "MapBackgroundOverlay", false);
    s.mapZoneMarkers = c.Field(s.map, "_zoneMarkersContainer", false);
    s.mapMarkerMapping = c.Field(s.map, "_markerPrefabMapping", false);
    s.mdEventDirector = c.Field(s.mapDirector, "_eventDirector", false);
    s.mdMarkers = c.Field(s.mapDirector, "Markers", false);
    s.mdCounters = c.Field(s.mapDirector, "_counters", false);
    if (Il2CppClass* marker = ugui::NestedClass(s.mapDirector, "Marker")) {
        s.dirMarker = marker;
        s.markerShouldDisplay = c.Bind(c.Method(marker, "ShouldDisplay", 0, false), "MapDirector.Marker.ShouldDisplay", false);
        s.markerGetPosition = c.Bind(c.Method(marker, "GetPosition", 0, false), "MapDirector.Marker.GetPosition", false);
        s.markerSource = c.Field(marker, "_mapMarkerSource", false);
    } else {
        optionalMissing.push_back("nested class MapDirector.Marker not found (teleporter gadgets off)");
    }
    s.playerUnknownArea =
        c.Bind(c.Method(s.playerDisplay, "IsInUnknownArea", 0, false), "PlayerDisplayOnMap.IsInUnknownArea", false);

    s.fogElement = c.Class(nsUiMap, "FogMapElement", false);
    s.fogShouldReveal = c.Bind(c.Method(s.fogElement, "ShouldBeRevealed", 1, false), "FogMapElement.ShouldBeRevealed", false);
    s.dynamicElement = c.Class(nsUiMap, "DynamicMapElement", false);
    s.dynCanBeShown = c.Bind(c.Method(s.dynamicElement, "get_CanBeShown", 0, false), "DynamicMapElement.CanBeShown", false);

    s.filterable = c.Class(nsUiMap, "IFilterableMapMarker", false);
    s.canBeShown = c.Method(s.filterable, "get_CanBeShown", 0, false);
    if (Il2CppClass* provider = c.Class(nsUiMap, "IMapMarkerDescriptorProvider", false))
        s.getDescriptor = c.Method(provider, "GetMapMarkerDescriptor", 0, false);
    if (Il2CppClass* descriptor = c.Class(nsUiMap, "IMapMarkerDescriptor", false))
        s.descMapIcon = c.Method(descriptor, "get_MapIcon", 0, false);
    s.mapMarker = c.Class(nsUiMap, "MapMarker", false);
    s.mapMarkerImage = c.Field(s.mapMarker, "_image", false);
    s.gordoMarker = c.Class(nsUiMap, "GordoMarkerUI", false);
    s.gordoIconImage = c.Field(s.gordoMarker, "_iconImage", false);

    s.mapMarkerBase = c.Class(nsUiMap, "MapMarkerBase", false);
    s.mdViewModel = c.Field(s.mapDirector, "_viewModel", false);
    s.mapViewModel = c.Class(nsUiMap, "MapViewModel", false);
    s.vmPrefabMapping = c.Field(s.mapViewModel, "_prefabMapping", false);
    s.mpmEntries = c.Field(s.mapPrefabMapping, "_entries", false);
    s.markerPrefabMapping = c.Class(nsUiMap, "MapMarkerPrefabMapping", false);
    s.mpmPlayerPrefab = c.Field(s.markerPrefabMapping, "_playerMarkerPrefab", false);
    s.playerMapMarker = c.Class(nsUiMap, "PlayerMapMarker", false);
    s.pmmArrow = c.Field(s.playerMapMarker, "_arrowRect", false);
    s.pmmRotOffset = c.Field(s.playerMapMarker, "_rotationOffset", false);

    s.sceneLoader = c.Class("MonomiPark.SlimeRancher.SceneManagement", "SceneLoader", false);
    s.sceneLoadInProgress = c.Bind(c.Method(s.sceneLoader, "get_IsSceneLoadInProgress", 0, false),
                                   "SceneLoader.get_IsSceneLoadInProgress", false);

    s.hudUI = c.Class("MonomiPark.SlimeRancher.UI", "HudUI", false);
    s.hudDebugText = c.Field(s.hudUI, "_debugText", false);

    s.treasurePod = c.Class("", "TreasurePod", false);
    s.podGetState = c.Bind(c.Method(s.treasurePod, "get_CurrState", 0, false), "TreasurePod.get_CurrState", false);
    s.podCounterDef = c.Class(nsMap, "MapTreasurePodCounterDefinition", false);
    s.counterIcon = c.Field(c.Class(nsMap, "MapCounterDefinition", false), "Icon", false);

    s.nodeDirector = c.Class("MonomiPark.SlimeRancher.World.ResourceNode", "ResourceNodeDirector", false);
    s.allNodeDirectors = c.Bind(c.Method(s.nodeDirector, "get_AllResourceDirectors", 0, false),
                                "ResourceNodeDirector.get_AllResourceDirectors", false);
    s.dirSpawners = c.Field(s.nodeDirector, "_nodeSpawners", false);
    s.nodeSpawner = c.Class("", "ResourceNodeSpawner", false);
    s.spawnerModel = c.Field(s.nodeSpawner, "_model", false);
    s.nodeModel = c.Class("MonomiPark.SlimeRancher.DataModel", "ResourceNodeSpawnerModel", false);
    s.modelNodeState = c.Field(s.nodeModel, "nodeState", false);
    s.modelResources = c.Field(s.nodeModel, "resourcesToSpawn", false);
    s.identType = c.Class("", "IdentifiableType", false);
    s.identIcon = c.Field(s.identType, "icon", false);

    s.resourceNode = c.Class("", "ResourceNode", false);
    s.spawnerNode = c.Field(s.nodeSpawner, "_attachedResourceNode", false);
    s.mapForceActive = c.Field(s.map, "_forceActiveOnAwake", false);

    // ---- drone ports (quantum drone stations)
    s.droneStationModel = c.Class("MonomiPark.SlimeRancher.DataModel", "DroneStationGadgetModel", false);
    s.dsgmType = c.Field(s.droneStationModel, "_type", false);
    s.teleporterModel = c.Class("MonomiPark.SlimeRancher.DataModel", "TeleporterGadgetModel", false);
    s.warpDepotModel = c.Class("MonomiPark.SlimeRancher.DataModel", "WarpDepotModel", false);
    s.droneStationMarker = c.Class(nsUiMap, "DroneStationMapMarker", false);
    s.dsmDisplayConfig = c.Field(s.droneStationMarker, "_displayConfiguration", false);
    s.mpmDronePrefab = c.Field(s.markerPrefabMapping, "_droneStationPrefab", false);
    s.droneDisplayConfig = c.Class("MonomiPark.SlimeRancher.UI.Drone", "DroneTypeDisplayConfiguration", false);
    s.ddcRancherIcon = c.Field(s.droneDisplayConfig, "_rancherDroneIcon", false);
    s.ddcExplorerIcon = c.Field(s.droneDisplayConfig, "_explorerDroneIcon", false);

    // ---- placed gadgets (GameModel)
    // (s.gameModel is resolved further down; resolve it here too, same class)
    Il2CppClass* gm = Resolver::Get().Class(kAsm, "MonomiPark.SlimeRancher.DataModel", "GameModel", nullptr);
    s.gmIdentsByIdent = c.Field(gm, "identifiablesByIdent", false);
    s.gmPlayer = c.Field(gm, "player", false);
    s.identModel = c.Class("MonomiPark.SlimeRancher.DataModel", "IdentifiableModel", false);
    s.imSceneGroup = c.Field(s.identModel, "sceneGroup", false);
    s.imIdent = c.Field(s.identModel, "_ident", false);
    s.imLastPos = c.Field(s.identModel, "lastPosition", false);
    s.gadgetModel = c.Class("MonomiPark.SlimeRancher.DataModel", "GadgetModel", false);
    s.gmdPrePlaced = c.Field(s.gadgetModel, "_isPrePlaced", false);
    s.gadgetDef = c.Class("", "GadgetDefinition", false);
    s.gdType = c.Field(s.gadgetDef, "Type", false);
    s.gdMapIcon = c.Field(s.gadgetDef, "_optionalMapIcon", false);
    s.gdVariantOf = c.Field(s.gadgetDef, "VariantOf", false);
    s.itGroup = c.Field(s.identType, "groupType", false);

    // ---- map UI preload (the game's own UI display loader)
    const char* nsDisplays = "MonomiPark.SlimeRancher.UI.Framework.Displays";
    s.uiDisplayManager = c.Class(nsDisplays, "UIDisplayManager", false);
    s.udmInstance = c.Bind(c.Method(s.uiDisplayManager, "get_Instance", 0, false), "UIDisplayManager.get_Instance", false);
    s.udmRequestPreload = c.Bind(c.Method(s.uiDisplayManager, "RequestPreloadDisplay", 2, false),
                                 "UIDisplayManager.RequestPreloadDisplay", false);
    s.udmReleasePreload = c.Bind(c.Method(s.uiDisplayManager, "ReleasePreloadDisplay", 2, false),
                                 "UIDisplayManager.ReleasePreloadDisplay", false);
    s.sceneLoadEntry = c.Class(nsDisplays, "UIDisplaySceneLoadEntry", false);
    s.entryIsDone = c.Bind(c.Method(s.sceneLoadEntry, "get_IsDone", 0, false), "UIDisplaySceneLoadEntry.get_IsDone", false);
    s.mapUIRootDisplay = c.Field(s.mapUIRoot, "_mapDisplay", false);

    // ---- marker "done" state
    s.puzzleMarker = c.Class(nsUiMap, "PuzzleSlotMarkerUI", false);
    s.puzzleIsSolved = c.Bind(c.Method(s.puzzleMarker, "CalculateIsSolved", 0, false),
                              "PuzzleSlotMarkerUI.CalculateIsSolved", false);
    s.teleporterMarker = c.Class(nsUiMap, "AncientTeleporterMarkerUI", false);
    s.gordoId = c.Field(s.gordoMarker, "_gordoIdentifier", false);
    s.gordoModel = c.Class("MonomiPark.SlimeRancher.DataModel", "GordoModel", false);
    s.gordoHasPopped = c.Bind(c.Method(s.gordoModel, "HasPopped", 0, false), "GordoModel.HasPopped", false);
    s.gameModel = c.Class("MonomiPark.SlimeRancher.DataModel", "GameModel", false);
    if (s.gameModel)
        s.getGordoModel = c.Bind(c.Method(s.gameModel, "GetGordoModel", 1, false), "GameModel.GetGordoModel", false);

    // ---- fog of war: FogMapElement.ShouldBeRevealed(director) = StaticEventQueryComponent(_fogRevealEvent)
    //      .IsSatisfied(director) = director._model (the save's EventRecordModel) has the event.
    const char* nsEvent = "MonomiPark.SlimeRancher.Event";
    s.fogEvent = c.Field(s.fogElement, "_fogRevealEvent", false);
    s.staticGameEvent = c.Class(nsEvent, "StaticGameEvent", false);
    s.sgeDirector = c.Field(s.staticGameEvent, "_eventDirector", false);
    s.sgeKey = c.Field(s.staticGameEvent, "_eventKey", false);
    s.eventDirector = c.Class(nsEvent, "EventDirector", false);
    s.edModel = c.Field(s.eventDirector, "_model", false);
    s.eventRecordModel = c.Class(nsEvent, "EventRecordModel", false);
    s.gmEventRecord = c.Field(s.gameModel, "eventRecordModel", false);
    s.negativeMask = c.Class("MonomiPark.SlimeRancher", "NegativeMaskMap", false);
    s.nmSoftness = c.Field(s.negativeMask, "softness", false);
    s.nmRoundness = c.Field(s.negativeMask, "roundness", false);
    s.nmNoiseScale = c.Field(s.negativeMask, "maskEdgeNoiseScale", false);
    s.nmNoiseStrength = c.Field(s.negativeMask, "maskEdgeNoiseStrength", false);
    if (Il2CppClass* scu = c.Class("MonomiPark.SlimeRancher.Util", "SceneContextUtility", false))
        s.tryGetGameModel = c.Bind(c.Method(scu, "TryGetGameModel", 1, false), "SceneContextUtility.TryGetGameModel", false);

    // ---- hook targets (record-only detours; optional)
    s.mapUIAwake = c.Target(s.mapUI, "Awake", 0);
    s.hudAwake = c.Target(s.hudUI, "Awake", 0);
    s.playerAwake = c.Target(s.playerDisplay, "Awake", 0);
    s.podAwake = c.Target(s.treasurePod, "Awake", 0);
    s.podOnDestroy = c.Target(s.treasurePod, "OnDestroy", 0);

    if (!c.req.empty()) {
        err = "map symbols missing: ";
        for (size_t i = 0; i < c.req.size(); ++i) err += (i ? "; " : "") + c.req[i];
        return false;
    }
    return true;
}

}  // namespace sr2::modules::minimap::game
