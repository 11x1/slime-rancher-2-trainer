#pragma once
// Game-side symbols used by the minimap, resolved by class / method / field
// NAME from Assembly-CSharp (the 1.3.0 layout is documented in NOTES.md; no
// offset or RVA from the dump is used at runtime).

#include <cstddef>
#include <string>
#include <vector>

#include "core/il2cpp_api.h"
#include "modules/minimap/ugui.h"

namespace sr2::modules::minimap::game {

inline constexpr size_t kNoOffset = static_cast<size_t>(-1);
inline bool Has(size_t off) { return off != kNoOffset; }

struct Symbols {
    // ---- classes
    Il2CppClass* mapDirector = nullptr;          // MonomiPark.SlimeRancher.Map.MapDirector (ScriptableObject)
    Il2CppClass* mapDefinition = nullptr;        // MonomiPark.SlimeRancher.Map.MapDefinition
    Il2CppClass* mapPrefabMapping = nullptr;     // MonomiPark.SlimeRancher.Map.MapPrefabMapping
    Il2CppClass* map = nullptr;                  // MonomiPark.SlimeRancher.UI.Map.Map (map page component)
    Il2CppClass* projectionHelper = nullptr;     // MonomiPark.SlimeRancher.UI.Map.MapProjectionHelper
    Il2CppClass* playerDisplay = nullptr;        // PlayerDisplayOnMap (global ns)
    Il2CppClass* fogElement = nullptr;           // MonomiPark.SlimeRancher.UI.Map.FogMapElement
    Il2CppClass* dynamicElement = nullptr;       // MonomiPark.SlimeRancher.UI.Map.DynamicMapElement
    Il2CppClass* filterable = nullptr;           // MonomiPark.SlimeRancher.UI.Map.IFilterableMapMarker
    Il2CppClass* mapMarker = nullptr;            // MonomiPark.SlimeRancher.UI.Map.MapMarker
    Il2CppClass* gordoMarker = nullptr;          // MonomiPark.SlimeRancher.UI.Map.GordoMarkerUI
    Il2CppClass* markerPrefabMapping = nullptr;  // MonomiPark.SlimeRancher.UI.Map.MapMarkerPrefabMapping
    Il2CppClass* playerMapMarker = nullptr;      // MonomiPark.SlimeRancher.UI.Map.PlayerMapMarker
    Il2CppClass* mapUI = nullptr;                // MonomiPark.SlimeRancher.UI.Map.MapUI
    Il2CppClass* mapUIRoot = nullptr;            // MonomiPark.SlimeRancher.UI.Map.MapUIRoot
    Il2CppClass* sceneLoader = nullptr;          // MonomiPark.SlimeRancher.SceneManagement.SceneLoader
    Il2CppClass* hudUI = nullptr;                // MonomiPark.SlimeRancher.UI.HudUI
    Il2CppClass* treasurePod = nullptr;          // TreasurePod (global ns)
    Il2CppClass* nodeDirector = nullptr;         // MonomiPark.SlimeRancher.World.ResourceNode.ResourceNodeDirector
    Il2CppClass* nodeSpawner = nullptr;          // ResourceNodeSpawner (global ns)
    Il2CppClass* nodeModel = nullptr;            // MonomiPark.SlimeRancher.DataModel.ResourceNodeSpawnerModel
    Il2CppClass* identType = nullptr;            // IdentifiableType (global ns)
    Il2CppClass* podCounterDef = nullptr;        // MonomiPark.SlimeRancher.Map.MapTreasurePodCounterDefinition
    Il2CppClass* dirMarker = nullptr;            // MapDirector.Marker (nested; validates dictionary values)
    Il2CppClass* mapMarkerBase = nullptr;        // MonomiPark.SlimeRancher.UI.Map.MapMarkerBase
    Il2CppClass* mapViewModel = nullptr;         // MonomiPark.SlimeRancher.UI.Map.MapViewModel

    // ---- methods (called)
    ugui::Fn getMapForZone;         // MapDirector.GetMapForZone(ZoneDefinition) -> MapDefinition
    ugui::Fn getPrefabFor;          // MapPrefabMapping.GetPrefabFor(MapDefinition) -> GameObject
    ugui::Fn getPlayerCurrentZone;  // static MapUIUtilities.GetPlayerCurrentZone() -> ZoneDefinition
    ugui::Fn playerGetZone;         // PlayerDisplayOnMap.GetZoneDefinition()
    ugui::Fn playerUnknownArea;     // PlayerDisplayOnMap.IsInUnknownArea()
    ugui::Fn fogShouldReveal;       // FogMapElement.ShouldBeRevealed(EventDirector) (pure query on serialized data)
    ugui::Fn dynCanBeShown;         // DynamicMapElement.get_CanBeShown
    ugui::Fn sceneLoadInProgress;   // SceneLoader.get_IsSceneLoadInProgress()
    ugui::Fn podGetState;           // TreasurePod.get_CurrState() (0 LOCKED, 1 OPEN)
    ugui::Fn allNodeDirectors;      // static ResourceNodeDirector.get_AllResourceDirectors() -> List
    const MethodInfo* canBeShown = nullptr;     // IFilterableMapMarker.get_CanBeShown (interface slot)
    const MethodInfo* getDescriptor = nullptr;  // IMapMarkerDescriptorProvider.GetMapMarkerDescriptor
    const MethodInfo* descMapIcon = nullptr;    // IMapMarkerDescriptor.get_MapIcon
    ugui::Fn markerShouldDisplay;               // MapDirector.Marker.ShouldDisplay()
    ugui::Fn markerGetPosition;                 // MapDirector.Marker.GetPosition() -> Vector3

    // ---- hook targets (all optional; each only records `this`)
    void* mapUIAwake = nullptr;       // MapUI.Awake: full map open + its prefab mappings / director
    void* hudAwake = nullptr;         // HudUI.Awake
    void* playerAwake = nullptr;      // PlayerDisplayOnMap.Awake
    void* podAwake = nullptr;         // TreasurePod.Awake
    void* podOnDestroy = nullptr;     // TreasurePod.OnDestroy

    // ---- field offsets (object fields include the object header; struct
    //      inner offsets are raw)
    size_t mdEventDirector = kNoOffset;     // MapDirector._eventDirector
    size_t mdMarkers = kNoOffset;           // MapDirector.Markers (Dictionary<string, Marker>)
    size_t mdCounters = kNoOffset;          // MapDirector._counters (List<ZoneToCounterMapping>)
    size_t markerSource = kNoOffset;        // MapDirector.Marker._mapMarkerSource
    size_t defBackground = kNoOffset;       // MapDefinition.MapBackground (ImageSetup)
    size_t defOverlay = kNoOffset;          // MapDefinition.MapBackgroundOverlay (ImageSetup)
    size_t imgSetupImage = kNoOffset;       // ImageSetup.Image (Sprite), inner
    size_t imgSetupTint = kNoOffset;        // ImageSetup.TintColor (Color), inner
    size_t mapProjection = kNoOffset;       // Map._projectionHelper
    size_t mapZoneMarkers = kNoOffset;      // Map._zoneMarkersContainer
    size_t mapMarkerMapping = kNoOffset;    // Map._markerPrefabMapping
    size_t mapAnchor1 = kNoOffset;          // Map._anchor1 (MapAnchorCoordinates)
    size_t mapAnchor2 = kNoOffset;          // Map._anchor2
    size_t anchorWorld = kNoOffset;         // MapAnchorCoordinates.WorldPosition, inner
    size_t anchorMap = kNoOffset;           // MapAnchorCoordinates.MapPosition, inner
    size_t projScale = kNoOffset;           // MapProjectionHelper._worldToMapScale
    size_t projOffset = kNoOffset;          // MapProjectionHelper._worldToMapOffset
    size_t mpmPlayerPrefab = kNoOffset;     // MapMarkerPrefabMapping._playerMarkerPrefab
    size_t pmmArrow = kNoOffset;            // PlayerMapMarker._arrowRect
    size_t pmmRotOffset = kNoOffset;        // PlayerMapMarker._rotationOffset
    size_t mapMarkerImage = kNoOffset;      // MapMarker._image
    size_t gordoIconImage = kNoOffset;      // GordoMarkerUI._iconImage
    size_t mapUIPrefabMapping = kNoOffset;  // MapUI._mapPrefabMapping
    size_t mapUIDirector = kNoOffset;       // MapUI._mapDirector
    size_t mapUIRootDirector = kNoOffset;   // MapUIRoot._mapDirector
    size_t hudDebugText = kNoOffset;        // HudUI._debugText (TMP font source)
    size_t dirSpawners = kNoOffset;         // ResourceNodeDirector._nodeSpawners
    size_t spawnerModel = kNoOffset;        // ResourceNodeSpawner._model
    size_t modelNodeState = kNoOffset;      // ResourceNodeSpawnerModel.nodeState
    size_t modelResources = kNoOffset;      // ResourceNodeSpawnerModel.resourcesToSpawn
    size_t identIcon = kNoOffset;           // IdentifiableType.icon
    size_t counterIcon = kNoOffset;         // MapCounterDefinition.Icon
    size_t mdViewModel = kNoOffset;         // MapDirector._viewModel (MapViewModel, set once the full map was used)
    size_t vmPrefabMapping = kNoOffset;     // MapViewModel._prefabMapping (IMapPrefabMapping)
    size_t mpmEntries = kNoOffset;          // MapPrefabMapping._entries (List<MappingEntry{Map, Prefab}>)
    size_t mapForceActive = kNoOffset;      // Map._forceActiveOnAwake (GameObject[]; Map.Awake activates them)

    // ---- map UI preload (keeps the full map's UI scene, and with it the
    //      MapPrefabMapping + page prefabs, loaded without opening the map)
    Il2CppClass* uiDisplayManager = nullptr;     // MonomiPark.SlimeRancher.UI.Framework.Displays.UIDisplayManager
    Il2CppClass* sceneLoadEntry = nullptr;       // ...Displays.UIDisplaySceneLoadEntry (IUISceneLoadHandle)
    ugui::Fn udmInstance;                        // static UIDisplayManager.get_Instance()
    ugui::Fn udmRequestPreload;                  // RequestPreloadDisplay(UIDisplayHandle{string}, object) -> IUISceneLoadHandle
    ugui::Fn udmReleasePreload;                  // ReleasePreloadDisplay(UIDisplayHandle{string}, object)
    ugui::Fn entryIsDone;                        // UIDisplaySceneLoadEntry.get_IsDone()
    size_t mapUIRootDisplay = kNoOffset;         // MapUIRoot._mapDisplay (UIDisplayHandleWithData<MapDirector>{string _name})

    // ---- "done" state of page markers (independent of the fog setting)
    Il2CppClass* puzzleMarker = nullptr;         // MonomiPark.SlimeRancher.UI.Map.PuzzleSlotMarkerUI
    Il2CppClass* teleporterMarker = nullptr;     // MonomiPark.SlimeRancher.UI.Map.AncientTeleporterMarkerUI
    ugui::Fn puzzleIsSolved;                     // PuzzleSlotMarkerUI.CalculateIsSolved() (GameModel query)
    size_t gordoId = kNoOffset;                  // GordoMarkerUI._gordoIdentifier (string)
    Il2CppClass* gordoModel = nullptr;           // MonomiPark.SlimeRancher.DataModel.GordoModel
    ugui::Fn tryGetGameModel;                    // static SceneContextUtility.TryGetGameModel(out GameModel)
    ugui::Fn getGordoModel;                      // GameModel.GetGordoModel(string) -> GordoModel
    ugui::Fn gordoHasPopped;                     // GordoModel.HasPopped()

    // ---- fog of war: the reveal source and its readiness (the save's event record)
    size_t fogEvent = kNoOffset;                 // FogMapElement._fogRevealEvent (StaticGameEvent)
    Il2CppClass* staticGameEvent = nullptr;      // MonomiPark.SlimeRancher.Event.StaticGameEvent
    size_t sgeDirector = kNoOffset;              // StaticGameEvent._eventDirector (the director RaiseEvent uses)
    size_t sgeKey = kNoOffset;                   // StaticGameEvent._eventKey (string, logs only)
    Il2CppClass* eventDirector = nullptr;        // MonomiPark.SlimeRancher.Event.EventDirector
    size_t edModel = kNoOffset;                  // EventDirector._model (EventRecordModel; null between sessions)
    Il2CppClass* eventRecordModel = nullptr;     // MonomiPark.SlimeRancher.Event.EventRecordModel
    Il2CppClass* gameModel = nullptr;            // MonomiPark.SlimeRancher.DataModel.GameModel
    size_t gmEventRecord = kNoOffset;            // GameModel.eventRecordModel (the loaded save's record)

    // ---- outside fog: NegativeMaskMap sets the global _NegativeMask matrix the NegativeMask fog shader reads
    Il2CppClass* negativeMask = nullptr;         // MonomiPark.SlimeRancher.NegativeMaskMap
    size_t nmSoftness = kNoOffset;               // NegativeMaskMap.softness (float)
    size_t nmRoundness = kNoOffset;              // NegativeMaskMap.roundness (float)
    size_t nmNoiseScale = kNoOffset;             // NegativeMaskMap.maskEdgeNoiseScale (float)
    size_t nmNoiseStrength = kNoOffset;          // NegativeMaskMap.maskEdgeNoiseStrength (float)

    // ---- resource nodes: the node object a spawner actually spawned
    Il2CppClass* resourceNode = nullptr;         // ResourceNode (global ns)
    size_t spawnerNode = kNoOffset;              // ResourceNodeSpawner._attachedResourceNode

    // ---- drone ports (quantum drone stations): player-placed gadgets registered in MapDirector.Markers
    //      (GadgetModel.SetupMapMarker when GadgetDefinition.DisplayOnMap). The full map draws them with
    //      DroneStationMapMarker, whose icon is DroneTypeDisplayConfiguration.PickDroneTypeSprite(model._type).
    Il2CppClass* droneStationModel = nullptr;    // MonomiPark.SlimeRancher.DataModel.DroneStationGadgetModel
    size_t dsgmType = kNoOffset;                 // DroneStationGadgetModel._type (DroneType, int: 0 ranch, 1 explorer)
    // ---- teleporters / warp depots: their own GadgetModel subclasses (both `: GadgetModel, LinkDestroyer`)
    Il2CppClass* teleporterModel = nullptr;      // MonomiPark.SlimeRancher.DataModel.TeleporterGadgetModel
    Il2CppClass* warpDepotModel = nullptr;       // MonomiPark.SlimeRancher.DataModel.WarpDepotModel
    Il2CppClass* droneStationMarker = nullptr;   // MonomiPark.SlimeRancher.UI.Map.DroneStationMapMarker
    size_t dsmDisplayConfig = kNoOffset;         // DroneStationMapMarker._displayConfiguration
    size_t mpmDronePrefab = kNoOffset;           // MapMarkerPrefabMapping._droneStationPrefab
    Il2CppClass* droneDisplayConfig = nullptr;   // MonomiPark.SlimeRancher.UI.Drone.DroneTypeDisplayConfiguration
    size_t ddcRancherIcon = kNoOffset;           // DroneTypeDisplayConfiguration._rancherDroneIcon (Sprite)
    size_t ddcExplorerIcon = kNoOffset;          // DroneTypeDisplayConfiguration._explorerDroneIcon (Sprite)

    // ---- every placed gadget: GameModel.identifiablesByIdent (Dictionary<IdentifiableType, List<IdentifiableModel>>),
    //      the lists whose models are GadgetModels (GameModel.AllGadgets does the same with the key type)
    size_t gmIdentsByIdent = kNoOffset;          // GameModel.identifiablesByIdent
    size_t gmPlayer = kNoOffset;                 // GameModel.player (PlayerModel : IdentifiableModel)
    Il2CppClass* identModel = nullptr;           // MonomiPark.SlimeRancher.DataModel.IdentifiableModel
    size_t imSceneGroup = kNoOffset;             // IdentifiableModel.sceneGroup
    size_t imIdent = kNoOffset;                  // IdentifiableModel._ident (IdentifiableType)
    size_t imLastPos = kNoOffset;                // IdentifiableModel.lastPosition (Vector3; GadgetModel's IMapMarkerSource.MapPosition)
    Il2CppClass* gadgetModel = nullptr;          // MonomiPark.SlimeRancher.DataModel.GadgetModel
    size_t gmdPrePlaced = kNoOffset;             // GadgetModel._isPrePlaced (bool: placed by the level, not the player)
    Il2CppClass* gadgetDef = nullptr;            // GadgetDefinition (global ns, : IdentifiableType)
    size_t gdType = kNoOffset;                   // GadgetDefinition.Type (Types enum, int: 1 TELEPORTER, 2 WARP_DEPOT, 10 DRONE_STATION)
    size_t gdMapIcon = kNoOffset;                // GadgetDefinition._optionalMapIcon (Sprite)
    size_t gdVariantOf = kNoOffset;              // GadgetDefinition.VariantOf (GadgetDefinition; null on a base gadget)
    size_t itGroup = kNoOffset;                  // IdentifiableType.groupType (IdentifiableTypeGroup, decor heuristic)
};

// Required parts fail with `err`; optional parts are appended to `optionalMissing`.
bool Resolve(Symbols& s, std::string& err, std::vector<std::string>& optionalMissing);

}  // namespace sr2::modules::minimap::game
