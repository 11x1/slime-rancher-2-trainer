// custom_upgrades: the Quantum Storage Link (see quantum_link.h). SR2 1.3.0, all lookups by name; RVAs
// for reference only.
//
// Findings (disassembly):
//   * "Quantum storage" in 1.3.0 is the Quantum Cloud: DroneDirector.GetCloudModel() -> DroneCloudModel
//     {Dictionary<IdentifiableType, int> _amounts (one key per storable type, 0 when empty), int _limit},
//     saved as DroneCloudV01 {IDs, Amounts}. Explorer drones upload into it; the drone station's cloud
//     menu withdraws (DroneStationCloudExtractionController ejects the items, _spawnFX) or sends to the
//     refinery / market. GetAmount (0xA201E0); SetAmount (0xA20BD0) = min(amount, _limit); AddAmount
//     (0xA1FE90) = min(cur + n, _limit) with no lower bound (so we use Get + Set). Unsupported types log
//     an exception.
//   * A placed gadget's model type comes from its PREFAB: GadgetDirector.InstantiateGadget (0x869880) does
//     IdentifiableType.TryGetId(prefab) and GameModel.InstantiateGadget(that definition). GadgetItem.
//     PlaceGadgetEvent (0xA45410) spends {BaseGadget, 1} from the inventory (BaseGadget = VariantOf ?? the
//     definition), then InstantiateGadget(GetGadgetDefinitionToPlace(held).prefab) -> _lastPlacedGadget.
//     So a variant whose prefab is the Refinery Link's places a vanilla Refinery Link model and costs one
//     Refinery Link.
//   * Variants: GadgetDirector._gadgetVariantMap (Dictionary<GadgetDefinition, List<GadgetDefinition>>,
//     filled in Awake; get_Item without a fallback in GetVariantsForGadget 0x869140,
//     GetAvailableVariantsForGadget 0x868750 and HasVariantsForGadget 0x869540, so every definition that
//     may be asked needs a key). Held gadget: SetHeldGadget (0xA46810) = [BaseGadget] +
//     GetVariantsForGadget(base, unlockedOnly: true), unlocked = GadgetDirector.HasBlueprint (0x869360,
//     blueprints HashSet). ToggleVariant (0xA47AC0): held -> GadgetDirector.OnGadgetVariantToggled
//     (0x86A1C0) = GadgetsModel.SetFavorite(selected slot, variant) (favourites ARE saved, as persistence
//     ids); targeted placed gadget -> Gadget.ChangeVariant(variant, regionRegistry) (0x651CD0).
//     GetItemCount (0x8688C0) answers a gadget's BaseGadget count.
//   * Save: GadgetsModel.Pull (0xA91FC0) writes blueprints / availBlueprints / favourites through
//     ISaveReferenceTranslation.GetPersistenceId and _itemCounts through Enumerable.ToDictionary (a
//     duplicate key would throw). SaveReferenceTranslation.GetPersistenceId(IdentifiableType) (0x60DDC0,
//     only reached through the interface) = PersistenceIdLookupTable._reverseIndex[ReferenceId] (no
//     fallback: KeyNotFoundException). ReferenceId (0x8700C0) = "{GetType().Name}.{name}", computed once
//     into the private field referenceId; the save's id table is _primaryIndex (string[]), separate from
//     _reverseIndex. ViewedItems keeps viewed blueprints as IdentifiableType.StableHashedId ints
//     (Animator.StringToHash(name)): harmless for a mod-free load.
//   * Fabricator: GadgetFabricateCategory.GetItems (0x8289E0) builds a GadgetFabricatableItem(helper,
//     recipeFactory, _gadgetPurchaseEvent, def) per member of its group; FabricateAndSpendCost
//     (0x827E20) = CheckCanFabricate (helper: recipe has items, HasBlueprint, CanAddGadget, costs),
//     SpendableDirector.Spend(def.CraftingCosts, n), GadgetDirector.AddItem(def, n), analytics, events.
//     GadgetDirectorHelper.HasBlueprint (0x827B50) -> GadgetDirector.HasBlueprint.
//   * Drones (see drone_source.cpp): a RanchDroneSourceProgramSiloStorage takes from a StorageMetadata
//     {SiloStorage storage, SiloCatcher catcher (its transform is where the drone flies), int index};
//     every take goes through StorageMetadata.Decrement (0x9969C0; callers: RanchDroneSourceProgram
//     SiloStorage.OnAction and DroneFastForwarder.GatherGroup.Storage.Decrement only). SiloStorage.Awake
//     -> InitAmmo (0x9956C0) = `if (Ammo == null) Ammo = new AmmoSlotManager(AmmoSlotDefinitions)`;
//     the manager needs an AmmoModel (AmmoSlotManager.SetModel) before any slot call (as in
//     coop_world.cpp). AmmoSlotManager.Decrement clears the slot (id null) at 0.
//
// What we do:
//   * Definition: Object.Instantiate(LookupDirector.RefineryLink) named "QuantumStorageLink" (ReferenceId
//     "GadgetDefinition.QuantumStorageLink"), VariantOf = Refinery Link, same prefab, variant gradient
//     purple -> cyan, own name / description (text_override), crafting cost = the Refinery Link's with
//     3x the Newbucks and 2x the items. Pinned for the session.
//   * Registered as the Refinery Link's variant (and as a key with no variants of its own) in the live
//     GadgetDirector, and its ReferenceId added to the save's _reverseIndex with the Refinery Link's
//     persistence id (checked every tick; the table is rebuilt when content is added). The
//     GetPersistenceId detour maps it too (covers the moment between a rebuild and the next tick).
//   * Fabricator: a card of its own right after the Refinery Link (GadgetFabricateCategoryRuntime..ctor
//     detour: GetItems is inlined there and has no native caller). Fabricating it
//     (FabricateAndSpendCost detour, our definition only): the item's own CheckCanFabricate, Spend(our
//     cost), AddItem(Refinery Link, n), kits += n. Never the vanilla path (it would AddItem our type).
//   * HasBlueprint(ours) = kits > 0 and the card unlocked (variant shown while a kit is left); the
//     Fabricator helper's HasBlueprint(ours) = the card unlocked: the Refinery Link blueprint, or the
//     sidecar override "buildables.unlock.quantum_link" (buildables.h). get_ShouldShowPlaceholder is false
//     for our cards (name and icon always shown; the description says what unlocks it).
//     GetItemCount(ours) = min(kits, Refinery Links).
//   * Placing (PlaceGadgetEvent detour): holding ours with a kit -> after the placement the new gadget's
//     actor id goes into the sidecar list and a kit is used; without a kit the held gadget becomes the
//     Refinery Link first (a plain one is placed, logged).
//   * PickupGadget detour: picking a quantum link up gives its kit back and holds the Quantum Storage
//     Link variant, so placing it again keeps it quantum. Any other way a listed gadget disappears
//     (stored, ...) is noticed by the tick (kit back).
//   * ChangeVariant detour (toggle-variant key on a placed Refinery Link, ours requested): flips the
//     link between plain and quantum (a kit used / given back) without the original, which would
//     destroy the gadget and place it again (new actor id).
//   * Tick (~1 Hz): every listed link that is loaded gets a holder "sr2qlink#<gen>" (the quantum glow:
//     a script-free clone of a teleporter gadget's active FX, tinted; and "sr2qstore": a SiloStorage
//     with 8 slots of a session AmmoSlotDefinition (no type group, 999 per slot) and its own AmmoModel),
//     its renderers' materials are instanced and tinted purple (colour properties) / cyan (emission
//     properties). The link's own SiloCatcher (the refinery input port) is the drone port. A quantum
//     pulse (the drone station's cloud-withdraw FX) plays every few seconds and on every drone take.
//   * Drone source (quantum::Sources, from drone_source's GetSources detour): the cloud types with an
//     amount > 0 that the program's predicate accepts (cached 1 s per program, at most 4) get a slot
//     each; the slot of every link mirrors min(cloud amount, 999). StorageMetadata.Decrement detour on
//     our storages: the cloud amount goes down by the count taken and every mirror follows, so two
//     drones at two links can't take the same items twice. The tick re-syncs the mirrors with the cloud
//     (withdrawals in the station menu, uploads) and frees slots unused for 30 s.

#include "modules/custom_upgrades/quantum_link.h"
#include "modules/custom_upgrades/gadget_look.h"
#include "modules/custom_upgrades/variant_ext.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <format>
#include <initializer_list>
#include <map>
#include <set>
#include <sstream>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "core/detour_guard.h"
#include "core/il2cpp_api.h"
#include "core/log.h"
#include "core/main_thread.h"
#include "core/module.h"
#include "core/resolver.h"
#include "core/text_override.h"
#include "core/unity.h"

namespace sr2::modules::custom_upgrades::quantum {
namespace {

namespace unity = sr2::core::unity;
namespace main_thread = sr2::core::main_thread;
using sr2::il2cpp::api;
using sr2::il2cpp::Resolver;

constexpr const char* kImage = "Assembly-CSharp";
constexpr const char* kCore = "UnityEngine.CoreModule";
constexpr const char* kPhysics = "UnityEngine.PhysicsModule";
constexpr const char* kKitsKey = "quantum_link.kits";
constexpr const char* kActorsKey = "quantum_link.actors";
constexpr const char* kDefName = "QuantumStorageLink";
constexpr const char* kTitle = "Quantum Storage Link";
constexpr const char* kDescription =
    "A Refinery Link wired to your Quantum Cloud: ranch drones set to the source \"Quantum Storage Link\" pick "
    "up cloud items here and deliver them. To place one: fabricate it, hold a Refinery Link in gadget mode, press "
    "the Variant key until it shows Quantum Storage Link, then place it. Needs the Refinery Link blueprint.";
constexpr const char* kUnlockHint = "Needs the Refinery Link blueprint.";
constexpr const char* kHolderPrefix = "sr2qlink#";
constexpr const char* kMatTag = " [sr2q]";
constexpr int kSlots = 8;            // storage slots per link (cloud types mirrored at once)
constexpr int kSlotMax = 999;        // per slot
constexpr int kTypesPerProgram = 4;  // cloud types one program's source offers at once
constexpr double kCacheSeconds = 1.0;
constexpr double kSlotIdleSeconds = 30.0;
constexpr double kPulseSeconds = 7.0;
constexpr double kTakeFxSeconds = 1.5;
constexpr int kStaleTicks = 5;       // ticks with a readable model list before a never-seen listed id is stale
constexpr int kStaleTicksEmpty = 60; // the same when the save has no Refinery Link at all
constexpr int32_t kTeleporterType = 1;  // GadgetDefinition.Types.TELEPORTER
// The teleporter FX on top (round 9: "the portal effect is too big"; round 11: "teleport effect too big"). Its child
// particle systems simulate in Local scaling mode, so the root's scale never reached them: each of those gets the
// factor on its own transform too (ScaleParticles).
constexpr float kGlowScale = 0.22f;
constexpr float kGlowHeight = 1.0f;
constexpr double kUseCooldown = 1.5;    // Use on a link: the cloud menu / notices at most this often
constexpr double kCloudOpenSeconds = 10.0;  // the Quantum Cloud menu must load within this time
constexpr int kUseLines = 5;            // cloud types listed by Use

struct Vec3 {
    float x, y, z;
};
struct Quat {
    float x, y, z, w;
};
struct Color {
    float r, g, b, a;
};
constexpr Color kPurple{0.50f, 0.20f, 1.00f, 1.0f};
constexpr Color kCyan{0.15f, 0.85f, 1.00f, 1.0f};

double Now() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

bool IsA(Il2CppObject* obj, Il2CppClass* k) {
    const auto& a = api();
    if (!obj || !k || !a.il2cpp_object_get_class) return false;
    Il2CppClass* c = a.il2cpp_object_get_class(obj);
    if (c == k) return true;
    return c && a.il2cpp_class_is_assignable_from && a.il2cpp_class_is_assignable_from(k, c);
}

std::string Lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// detour signatures
using PersistFn = int32_t (*)(Il2CppObject*, Il2CppObject*, const MethodInfo*);
using HasBpFn = bool (*)(Il2CppObject*, Il2CppObject*, const MethodInfo*);
using CountFn = int32_t (*)(Il2CppObject*, Il2CppObject*, const MethodInfo*);
using RuntimeFn = void (*)(Il2CppObject*, Il2CppObject*, Il2CppObject*, Il2CppObject*, const MethodInfo*);
using PlaceholderFn = bool (*)(Il2CppObject*, const MethodInfo*);
using FabFn = int32_t (*)(Il2CppObject*, int32_t, const MethodInfo*);
using PlaceFn = void (*)(Il2CppObject*, void*, const MethodInfo*);
using PickupFn = void (*)(Il2CppObject*, Il2CppObject*, const MethodInfo*);
using VariantFn = void (*)(Il2CppObject*, Il2CppObject*, Il2CppObject*, const MethodInfo*);
using DecFn = void (*)(Il2CppObject*, int32_t, const MethodInfo*);
using InteractFn = void (*)(Il2CppObject*, const MethodInfo*);
PersistFn g_origPersist = nullptr;
HasBpFn g_origHasBp = nullptr;
HasBpFn g_origHelperBp = nullptr;
CountFn g_origCount = nullptr;
RuntimeFn g_origRuntime = nullptr;
PlaceholderFn g_origPlaceholder = nullptr;
FabFn g_origFab = nullptr;
PlaceFn g_origPlace = nullptr;
PickupFn g_origPickup = nullptr;
VariantFn g_origVariant = nullptr;
DecFn g_origDec = nullptr;
InteractFn g_origActivator = nullptr;  // UIActivator.OnInteract
InteractFn g_origDisplay = nullptr;    // UIDisplayInteractable.OnInteract

// Read by the GetPersistenceId detour (any thread; set once on the main thread, never cleared).
std::atomic<Il2CppObject*> g_ours{nullptr};
std::atomic<Il2CppObject*> g_base{nullptr};
// More variants on these hooks (variant_ext.h); set before the hooks are enabled.
std::atomic<variant_ext::Extension*> g_ext{nullptr};
variant_ext::Extension* Ext() { return g_ext.load(std::memory_order_acquire); }

struct Impl {
    Host host;
    const char* module = "custom_upgrades";
    bool armed = false;  // every hook in place: the detours may act

    // classes
    Il2CppClass* gadgetDefClass = nullptr, *identTypeClass = nullptr, *groupClass = nullptr, *lsClass = nullptr;
    Il2CppClass* gadgetClass = nullptr, *gadgetModelClass = nullptr, *siloStorageClass = nullptr, *siloCatcherClass = nullptr;
    Il2CppClass* slotDefClass = nullptr, *ammoModelClass = nullptr, *storageMetaClass = nullptr, *rendererClass = nullptr;
    Il2CppClass* gameObjectClass = nullptr, *monoClass = nullptr, *colliderClass = nullptr, *rigidbodyClass = nullptr;
    Il2CppClass* jointClass = nullptr, *fabItemClass = nullptr, *cloudCtrlClass = nullptr, *teleNodeClass = nullptr;
    Il2CppClass* gadgetItemClass = nullptr, *interactableClass = nullptr;
    Il2CppClass* particleClass = nullptr, *uiDisplayIntClass = nullptr, *droneUiClass = nullptr;
    // offsets
    size_t offLocName = 0, offPrefab = 0, offIcon = 0, offRefId = 0, offHashInit = 0;
    size_t offDesc = 0, offCosts = 0, offVariantOf = 0, offGrad1 = 0, offGrad2 = 0, offGType = 0, offLinked = 0;
    size_t offPcCurrency = 0, offPcIdent = 0, offCostAmount = 0;
    size_t offVariantMap = 0, offGdModel = 0, offFavorites = 0;
    size_t offHeld = 0, offLastPlaced = 0, offGadgetModel = 0, offGmTransform = 0, offActorId = 0;
    size_t offStorageDefs = 0, offSdMax = 0, offSdGroup = 0, offSdMaxProp = 0, offSdUnlockProp = 0;
    size_t offSmStorage = 0, offSmIndex = 0, offFabDef = 0, offFabRecipe = 0, offCatEvent = 0, offAmounts = 0;
    size_t offSpawnFx = 0, offActiveFx = 0, offArriveFx = 0, offStationType = 0;
    size_t offScGame = 0, offScGadgetDir = 0, offScSpendable = 0, offScDrones = 0, offGcAutoSave = 0;
    size_t offAsdTranslation = 0, offSrtIdentTable = 0, offByIdent = 0, offGadgets = 0, offMembers = 0, offSubGroups = 0;
    size_t offRtItems = 0, offRtCount = 0;  // FabricateCategoryRuntime`1 _itemList / _categoryCount
    // methods
    const MethodInfo* getRefinery = nullptr, *sceneInst = nullptr, *gameCtxInst = nullptr, *getRefId = nullptr;
    const MethodInfo* setHideFlags = nullptr, *objDestroy = nullptr, *objDestroyImmediate = nullptr;
    const MethodInfo* lsCtor = nullptr, *lsGetTable = nullptr, *lsGetEntry = nullptr;
    const MethodInfo* goCtor = nullptr, *goAddComponent = nullptr, *compInChildren = nullptr, *compsInChildren = nullptr;
    const MethodInfo* trSetParent = nullptr, *trSetLocalPos = nullptr, *trSetLocalRot = nullptr, *trSetLocalScale = nullptr;
    const MethodInfo* trGetPosition = nullptr, *trChildCount = nullptr, *trGetChild = nullptr;
    const MethodInfo* initAmmo = nullptr, *localAmmo = nullptr, *ammoModelCtor = nullptr, *ammoSetModel = nullptr;
    const MethodInfo* slotName = nullptr, *slotCount = nullptr, *addResource = nullptr, *ammoDecrement = nullptr;
    const MethodInfo* smCtor = nullptr, *cloudGet = nullptr, *cloudSet = nullptr, *getCloudModel = nullptr;
    const MethodInfo* spawnFx = nullptr, *rendGetMaterials = nullptr, *rendGetShared = nullptr, *rendSetShared = nullptr;
    const MethodInfo* matPropNames = nullptr, *matGetColor = nullptr, *matSetColor = nullptr, *matGetShader = nullptr;
    const MethodInfo* fabItemCtor = nullptr, *fabCheck = nullptr, *spend = nullptr, *addItem = nullptr;
    const MethodInfo* setFavorite = nullptr, *setHeld = nullptr, *hasBp = nullptr, *lsGetString = nullptr;
    using ObjectNewFn = Il2CppObject* (*)(Il2CppClass*);
    using ArrayNewFn = Il2CppObject* (*)(Il2CppClass*, uintptr_t);
    using WBarrierFn = void (*)(Il2CppObject*, void**, void*);
    using FieldTypeFn = const Il2CppType* (*)(FieldInfo*);
    using ElemClassFn = Il2CppClass* (*)(Il2CppClass*);
    using ElemSizeFn = int (*)(Il2CppClass*);
    ObjectNewFn objectNew = nullptr;
    ArrayNewFn arrayNew = nullptr;
    WBarrierFn wbarrier = nullptr;
    FieldTypeFn fieldGetType = nullptr;
    ElemClassFn elemClass = nullptr;
    ElemSizeFn elemSize = nullptr;

    // session objects (pinned, never freed)
    Il2CppObject* base = nullptr;   // Refinery Link definition
    Il2CppObject* ours = nullptr;   // our definition
    Il2CppObject* slotDef = nullptr;
    Il2CppObject* cloudFx = nullptr;  // drone station: cloud withdraw FX prefab
    Il2CppObject* glowSrc = nullptr;  // teleporter active FX
    Il2CppObject* icon = nullptr;     // our purple copy of the Refinery Link's icon (nullptr: not made)
    Il2CppObject* squareIcon = nullptr;  // the same on a square canvas: the drone source icon
    bool defFailed = false, slotDefTried = false, fxTried = false;
    std::string baseRefId;

    // per-object caches of live generic classes
    struct DictMethods {
        const void* klass = nullptr;
        const MethodInfo* tryGet = nullptr, *setItem = nullptr, *containsKey = nullptr, *remove = nullptr;
    };
    DictMethods variantDict, aliasDict, modelDict;
    struct ListMethods {
        const void* klass = nullptr;
        const MethodInfo* add = nullptr, *remove = nullptr, *insert = nullptr, *ctor = nullptr;
    };
    ListMethods variantList, itemList;
    const MethodInfo* predInvoke = nullptr;
    const void* predInvokeClass = nullptr;
    struct DictLayout {
        const void* klass = nullptr;
        bool ok = false;
        size_t entries = 0, count = 0, stride = 0, hash = 0, key = 0, value = 0;
    } cloudLayout;

    // registration state
    const void* registeredDir = nullptr;
    bool loggedRegister = false, loggedAlias = false;
    std::string cardState;  // last logged Fabricator card state (logged when it changes)
    bool loggedNoDef = false;
    bool loggedRefineryFallback = false;
    std::map<std::string, int32_t> countLogged;  // category -> entry count last logged by FixPageCount
    int defWaits = 0;  // ticks EnsureDefinition found no Refinery Link (logged once, after a while)

    // links (main thread)
    struct RendRec {
        Il2CppObject* renderer = nullptr;
        Il2CppObject* shared = nullptr;  // the original sharedMaterials array
    };
    struct Rec {
        Il2CppObject* root = nullptr;     // the gadget's GameObject
        Il2CppObject* holder = nullptr;   // our holder under it
        Il2CppObject* storage = nullptr;
        Il2CppObject* ammo = nullptr;
        Il2CppObject* catcher = nullptr;
        Il2CppObject* sms[kSlots] = {};
        int mirror[kSlots] = {};          // count we set in each slot
        std::vector<RendRec> rends;
        std::vector<Il2CppGCHandle> handles;
        double nextPulse = 0.0, nextTakeFx = 0.0;
        // the glitch (Frame): the glow drops out and comes back in a stutter every few seconds
        Il2CppObject* glow = nullptr;     // the glow's GameObject
        std::vector<Il2CppObject*> interactables;  // the link's BaseUIInteractables (Use)
        double glitchAt = 0.0, glitchEnd = 0.0, nextFlip = 0.0;
        bool glitching = false;
        uint32_t rng = 1;
    };
    std::map<int64_t, Rec> recs;
    std::unordered_set<Il2CppObject*> storages;  // identities of our storages (Decrement detour)
    std::unordered_set<Il2CppObject*> interactables;  // the links' Use components (OnInteract detours)
    double lastUse = 0.0;
    // Use opens the game's Quantum Cloud menu: the drone station menu without a station (OpenCloudMenu / CloudFrame).
    const MethodInfo* udmInstance = nullptr, *udmOpen = nullptr, *droneShowData = nullptr, *tabGoTo = nullptr;
    size_t offUdiDisplay = 0, offMenuRoot = 0, offMenuTabs = 0, offMenuTabbed = 0, offMenuCloud = 0, offTabTarget = 0;
    std::string cloudDisplay;          // the drone station menu's display name (from the station prefab)
    bool cloudDisplayLooked = false;
    Il2CppGCHandle cloudReq = nullptr; // UIDisplayRequestHandle of the menu being opened
    double cloudReqAt = 0.0;
    const void* reqClass = nullptr;
    const MethodInfo* reqTryGet = nullptr;
    const void* rtClass = nullptr;
    size_t rtDisplayGo = 0;
    // Local-mode particle systems under the glow (ScaleParticles): icall get_scalingMode(MainModule*)
    using ScalingModeFn = int32_t (*)(void* mainModule);
    ScalingModeFn psScalingMode = nullptr;
    const MethodInfo* trGetLocalScale = nullptr;
    bool loggedParticles = false;
    std::set<std::string> loggedInteractables;
    std::unordered_set<int64_t> seenIds;         // listed ids seen present since the load
    int stableTicks = 0;
    unsigned generation = 1;
    bool loggedNoCatcher = false, loggedGlow = false, loggedPreview = false;
    std::set<std::string> loggedShaders;
    // slots (shared by every link)
    Il2CppObject* slotType[kSlots] = {};
    double slotUsed[kSlots] = {};
    int slotAmount[kSlots] = {};
    // predicate cache per program
    struct Cached {
        double at = 0.0;
        std::vector<Il2CppObject*> types;
    };
    std::unordered_map<Il2CppObject*, Cached> cache;

    // ---- helpers ----
    Il2CppObject* Call(const MethodInfo* m, void* obj, std::initializer_list<void*> args, const char* what,
                       bool* ok = nullptr) {
        if (!m) {
            if (ok) *ok = false;
            return nullptr;
        }
        std::vector<void*> params(args);
        std::string err;
        Il2CppObject* r = Resolver::Get().Invoke(m, obj, params.empty() ? nullptr : params.data(), &err);
        if (ok) *ok = err.empty();
        if (!err.empty()) {
            if (err.rfind("native exception", 0) == 0)
                sr2::core::MarkModuleFaulted(module, std::format("quantum link: {}: {}", what, err));
            else SR2_WARN("[{}] quantum link: {} failed: {}", module, what, err);
        }
        return r;
    }
    template <class T>
    static T Value(Il2CppObject* boxed, T def = T{}) {
        return boxed ? *reinterpret_cast<T*>(reinterpret_cast<char*>(boxed) + 2 * sizeof(void*)) : def;
    }
    static void* UnboxPtr(Il2CppObject* boxed) { return boxed ? reinterpret_cast<char*>(boxed) + 2 * sizeof(void*) : nullptr; }
    void SetRef(Il2CppObject* obj, size_t offset, Il2CppObject* value) {
        void** slot = reinterpret_cast<void**>(reinterpret_cast<char*>(obj) + offset);
        if (wbarrier) wbarrier(obj, slot, value);
        else *slot = value;
    }
    void SetElem(Il2CppObject* arr, size_t i, Il2CppObject* value) {
        Il2CppObject** data = unity::ArrayData<Il2CppObject*>(arr);
        if (!data) return;
        if (wbarrier) wbarrier(arr, reinterpret_cast<void**>(&data[i]), value);
        else data[i] = value;
    }
    bool Faulted() const { return sr2::core::ModuleFaulted(module); }
    // GC handles are never freed inside a hook: a posted job does it.
    static void FreeLater(std::vector<Il2CppGCHandle> handles) {
        if (!handles.empty()) main_thread::Post([handles] {
            for (Il2CppGCHandle h : handles) unity::FreeGCHandle(h);
        });
    }
    Il2CppObject* SceneContext() {
        bool ok = false;
        Il2CppObject* sc = Call(sceneInst, nullptr, {}, "SceneContext.Instance", &ok);
        return ok && unity::IsAlive(sc) ? sc : nullptr;
    }
    Il2CppObject* SceneField(size_t off) {
        Il2CppObject* sc = SceneContext();
        Il2CppObject* v = sc ? unity::Field<Il2CppObject*>(sc, off) : nullptr;
        return v;
    }
    void Destroy(Il2CppObject* obj) {
        if (unity::IsAlive(obj)) Call(objDestroy, nullptr, {obj}, "Object.Destroy");
    }
    void DestroyNow(Il2CppObject* obj) {
        if (unity::IsAlive(obj)) Call(objDestroyImmediate, nullptr, {obj}, "Object.DestroyImmediate");
    }
    Il2CppObject* InChildren(Il2CppObject* goOrComp, Il2CppClass* k) {
        Il2CppObject* tr = unity::TransformOf(goOrComp);
        if (!tr || !k) return nullptr;
        bool inactive = true, ok = false;
        Il2CppObject* r = Call(compInChildren, tr, {unity::TypeOf(k), &inactive}, "GetComponentInChildren", &ok);
        return ok && unity::IsAlive(r) ? r : nullptr;
    }
    std::vector<Il2CppObject*> AllInChildren(Il2CppObject* goOrComp, Il2CppClass* k) {
        std::vector<Il2CppObject*> out;
        Il2CppObject* tr = unity::TransformOf(goOrComp);
        if (!tr || !k) return out;
        bool inactive = true, ok = false;
        Il2CppObject* arr = Call(compsInChildren, tr, {unity::TypeOf(k), &inactive}, "GetComponentsInChildren", &ok);
        for (size_t i = 0, n = ok ? unity::ArrayLength(arr) : 0; i < n && i < 512; ++i)
            if (auto* c = static_cast<Il2CppObject*>(unity::ArrayAt(arr, i)); unity::IsAlive(c)) out.push_back(c);
        return out;
    }
    std::vector<Il2CppObject*> ChildTransforms(Il2CppObject* tr) {
        std::vector<Il2CppObject*> out;
        bool ok = false;
        const int n = Value<int32_t>(Call(trChildCount, tr, {}, "Transform.get_childCount", &ok));
        for (int32_t i = 0; ok && i < n && i < 256; ++i) {
            Il2CppObject* child = Call(trGetChild, tr, {&i}, "Transform.GetChild", &ok);
            if (ok && unity::IsAlive(child)) out.push_back(child);
        }
        return out;
    }
    Il2CppObject* NewLocalized(Il2CppObject* tmpl, const char* text) {
        if (!IsA(tmpl, lsClass)) return nullptr;
        bool ok = false;
        Il2CppObject* table = Call(lsGetTable, tmpl, {}, "LocalizedString.get_TableReference", &ok);
        Il2CppObject* entry = ok ? Call(lsGetEntry, tmpl, {}, "LocalizedString.get_TableEntryReference", &ok) : nullptr;
        Il2CppObject* ls = ok && table && entry ? objectNew(lsClass) : nullptr;
        if (ls) Call(lsCtor, ls, {UnboxPtr(table), UnboxPtr(entry)}, "LocalizedString..ctor", &ok);
        if (!ls || !ok || !unity::NewGCHandle(ls)) return nullptr;
        sr2::core::text_override::Set(ls, text);
        return ls;
    }

    // ---- sidecar ----
    int Kits() {
        try {
            const std::string v = host.value(kKitsKey);
            return v.empty() ? 0 : std::max(0, std::stoi(v));
        } catch (...) {
            return 0;
        }
    }
    void SetKits(int n) { host.setValue(kKitsKey, n > 0 ? std::to_string(n) : std::string()); }
    // Sandbox override of the card's unlock (buildables.h), else the Refinery Link blueprint.
    buildables::Unlock Override() { return buildables::ParseUnlock(host.value(buildables::UnlockKey(buildables::kQuantumLink))); }
    bool CardUnlocked(const std::function<bool(Il2CppObject*)>& vanilla) {
        switch (Override()) {
            case buildables::Unlock::kUnlocked: return true;
            case buildables::Unlock::kLocked: return false;
            default: return base && vanilla(base);
        }
    }
    // The vanilla GadgetDirector.HasBlueprint (through the detour, which passes vanilla definitions on).
    bool BaseBlueprint() {
        Il2CppObject* gd = SceneField(offScGadgetDir);
        bool ok = false;
        return unity::IsAlive(gd) && base && Value<bool>(Call(hasBp, gd, {base}, "GadgetDirector.HasBlueprint", &ok)) && ok;
    }
    std::set<int64_t> Actors() {
        std::set<int64_t> out;
        std::stringstream ss(host.value(kActorsKey));
        std::string part;
        while (std::getline(ss, part, ','))
            try {
                if (!part.empty()) out.insert(std::stoll(part));
            } catch (...) {
            }
        return out;
    }
    void SetActors(const std::set<int64_t>& ids) {
        std::string s;
        for (int64_t id : ids) s += (s.empty() ? "" : ",") + std::to_string(id);
        host.setValue(kActorsKey, s);
    }
    bool Listed(int64_t id) { return id && Actors().count(id) > 0; }
    void List(int64_t id) {
        auto ids = Actors();
        if (ids.insert(id).second) SetActors(ids);
    }
    void Unlist(int64_t id) {
        auto ids = Actors();
        if (ids.erase(id)) SetActors(ids);
    }
    int64_t ActorOfGadget(Il2CppObject* gadget) {
        if (!IsA(gadget, gadgetClass) || !unity::IsAlive(gadget)) return 0;
        Il2CppObject* model = unity::Field<Il2CppObject*>(gadget, offGadgetModel);
        return IsA(model, gadgetModelClass) ? unity::Field<int64_t>(model, offActorId) : 0;
    }

    // ---- live generic helpers ----
    DictMethods& Dict(DictMethods& d, Il2CppObject* dict) {
        Il2CppClass* k = api().il2cpp_object_get_class(dict);
        if (k != d.klass) {
            Resolver& R = Resolver::Get();
            d = {};
            d.klass = k;
            d.tryGet = R.Method(k, "TryGetValue", 2, nullptr);
            d.setItem = R.Method(k, "set_Item", 2, nullptr);
            d.containsKey = R.Method(k, "ContainsKey", 1, nullptr);
            d.remove = R.Method(k, "Remove", 1, nullptr);
        }
        return d;
    }
    ListMethods& List(ListMethods& l, Il2CppObject* list) {
        Il2CppClass* k = api().il2cpp_object_get_class(list);
        if (k != l.klass) {
            Resolver& R = Resolver::Get();
            l = {};
            l.klass = k;
            l.add = R.Method(k, "Add", 1, nullptr);
            l.remove = R.Method(k, "Remove", 1, nullptr);
            l.insert = R.Method(k, "Insert", 2, nullptr);
            l.ctor = R.Method(k, ".ctor", 0, nullptr);
        }
        return l;
    }
    static bool ListHas(void* list, Il2CppObject* item) {
        for (size_t i = 0, n = unity::ListCount(list); i < n && i < 512; ++i)
            if (unity::ListAt(list, i) == item) return true;
        return false;
    }

    // ---- cloud ----
    Il2CppObject* Cloud() {
        Il2CppObject* dd = SceneField(offScDrones);
        if (!unity::IsAlive(dd)) return nullptr;
        bool ok = false;
        Il2CppObject* c = Call(getCloudModel, dd, {}, "DroneDirector.GetCloudModel", &ok);
        return ok ? c : nullptr;
    }
    int CloudAmount(Il2CppObject* cloud, Il2CppObject* type) {
        bool ok = false;
        const int v = Value<int32_t>(Call(cloudGet, cloud, {type}, "DroneCloudModel.GetAmount", &ok));
        return ok ? std::max(v, 0) : 0;
    }
    bool CloudLayout(Il2CppObject* dict);
    std::vector<std::pair<Il2CppObject*, int>> CloudEntries(Il2CppObject* cloud);

    // ---- build ----
    bool EnsureDefinition();
    Il2CppObject* FindRefineryLink(Il2CppObject* lookup);
    void ScaleCost();
    void Register();
    void FindFx();
    bool EnsureSlotDef();
    void Tint(Il2CppObject* mat, int& colors, int& emissions, std::string& props);
    void TintRenderers(Il2CppObject* root, Rec* rec, const char* what);
    void Strip(Il2CppObject* root);
    bool Build(int64_t id, Il2CppObject* root, Rec& rec);
    void Undecorate(Rec& rec, bool restore);
    void DropRec(int64_t id, bool restore);
    void Pulse(Rec& rec, Il2CppObject* at);
    void MakeLook();
    void Glitch(Rec& rec, double now);
    int ScaleParticles(Il2CppObject* glow);
    bool OpenCloudMenu();
    void CloudFrame();
    void EndCloudRequest();
    bool OnUse(Il2CppObject* interactable);
    std::string NameOf(Il2CppObject* type);
    // ---- slots ----
    int SlotOf(Il2CppObject* type) const {
        for (int i = 0; i < kSlots; ++i)
            if (slotType[i] == type) return i;
        return -1;
    }
    int AssignSlot(Il2CppObject* type, double now);
    void SetSlot(Rec& rec, int slot, Il2CppObject* type, int amount);
    void SyncSlot(int slot, Il2CppObject* cloud);
    void RefreshMirrors(double now);
    // ---- tick ----
    void Scan(double now);
    // ---- detour bodies ----
    void InsertItem(Il2CppObject* category, Il2CppObject* list, Il2CppObject* helper, Il2CppObject* factory);
    void FixPageCount(Il2CppObject* runtime, Il2CppObject* category, int added);
    int32_t Fabricate(Il2CppObject* item, int32_t count);
    void OnTaken(Il2CppObject* storage, int slot, int32_t count);
    Il2CppObject* Sources(Il2CppObject* predicate, Il2CppObject* program);
};

Impl* g = nullptr;
sr2::core::DetourGuard g_guard{"custom_upgrades", "quantum storage link"};

// ---------------------------------------------------------------- detours --

// Pure pointer work (no game calls, no fault check): our definition is saved as the Refinery Link.
int32_t PersistDetour(Il2CppObject* self, Il2CppObject* type, const MethodInfo* mi) {
    Il2CppObject* ours = g_ours.load(std::memory_order_acquire);
    if (ours && type == ours) {
        if (Il2CppObject* base = g_base.load(std::memory_order_acquire)) type = base;
    } else if (variant_ext::Extension* e = Ext()) {
        if (Il2CppObject* base = e->SaveAs(type)) type = base;
    }
    return g_origPersist(self, type, mi);
}

bool HasBlueprintDetour(Il2CppObject* self, Il2CppObject* def, const MethodInfo* mi) {
    if (variant_ext::Extension* e = Ext(); e && g && g->armed && def && e->Owns(def)) {
        if (!main_thread::IsMainThread() || g->Faulted()) return false;
        bool r = false;
        g_guard.Run([&] { r = e->HasBlueprint(def, [&](Il2CppObject* d) { return g_origHasBp(self, d, mi); }); });
        return r;
    }
    if (!g || !g->armed || !def || def != g->ours) return g_origHasBp(self, def, mi);
    if (!main_thread::IsMainThread() || g->Faulted()) return false;
    bool r = false;
    g_guard.Run([&] { r = g->Kits() > 0 && g->CardUnlocked([&](Il2CppObject* d) { return g_origHasBp(self, d, mi); }); });
    return r;
}

bool HelperBlueprintDetour(Il2CppObject* self, Il2CppObject* def, const MethodInfo* mi) {
    if (variant_ext::Extension* e = Ext(); e && g && g->armed && def && e->Owns(def)) {
        if (!main_thread::IsMainThread() || g->Faulted()) return false;
        bool r = false;
        g_guard.Run([&] { r = e->HelperBlueprint(def, [&](Il2CppObject* d) { return g_origHelperBp(self, d, mi); }); });
        return r;
    }
    if (!g || !g->armed || !def || def != g->ours || !g->base) return g_origHelperBp(self, def, mi);
    if (!main_thread::IsMainThread() || g->Faulted()) return false;
    bool r = false;
    g_guard.Run([&] { r = g->CardUnlocked([&](Il2CppObject* d) { return g_origHelperBp(self, d, mi); }); });
    return r;
}

int32_t ItemCountDetour(Il2CppObject* self, Il2CppObject* def, const MethodInfo* mi) {
    const int32_t n = g_origCount(self, def, mi);
    if (variant_ext::Extension* e = Ext(); e && g && g->armed && def && main_thread::IsMainThread() && !g->Faulted() &&
                                           e->Owns(def)) {
        int32_t r = n;
        g_guard.Run([&] { r = e->ItemCount(def, n); });
        return r;
    }
    if (!g || !g->armed || !def || def != g->ours || !main_thread::IsMainThread() || g->Faulted()) return n;
    int kits = n;
    g_guard.Run([&] { kits = g->Kits(); });
    return std::min(n, kits);
}

// GadgetFabricateCategoryRuntime..ctor(category, helper, recipeFactory): the Fabricator builds one per
// category each time it opens (FabricatorCategoryList.FindCategoriesToShow; also the Sandbox's rebuild).
// GadgetFabricateCategory.GetItems is INLINED there (no native caller at all in 1.3.0), so the cards go
// into the runtime's own item list after the constructor.
void RuntimeCtorDetour(Il2CppObject* self, Il2CppObject* category, Il2CppObject* helper, Il2CppObject* factory,
                       const MethodInfo* mi) {
    g_origRuntime(self, category, helper, factory, mi);
    if (!self || !category || !g || !g->armed || !main_thread::IsMainThread() || g->Faulted()) return;
    Il2CppObject* list = unity::Field<Il2CppObject*>(self, g->offRtItems);
    if (!list) return;
    const size_t before = unity::ListCount(list);
    g_guard.Run([&] { g->InsertItem(category, list, helper, factory); });
    if (variant_ext::Extension* e = Ext()) g_guard.Run([&] { e->InsertCards(category, list, helper, factory); });
    const size_t after = unity::ListCount(list);
    if (after > before) g_guard.Run([&] { g->FixPageCount(self, category, static_cast<int>(after - before)); });
}

// GadgetFabricatableItem.get_ShouldShowPlaceholder = "the base's pedia entry is not unlocked" (our
// definitions share the base's pedia link). Our cards always show their name and icon, so the locked
// card can say what unlocks it. Pointer compares only.
bool PlaceholderDetour(Il2CppObject* self, const MethodInfo* mi) {
    if (g && g->armed && self && g->offFabDef) {
        Il2CppObject* def = unity::Field<Il2CppObject*>(self, g->offFabDef);
        if (def && (def == g_ours.load(std::memory_order_acquire) || (Ext() && Ext()->Owns(def)))) return false;
    }
    return g_origPlaceholder(self, mi);
}

// Our card never runs the vanilla body (it would AddItem our definition into the saved inventory).
int32_t FabricateDetour(Il2CppObject* self, int32_t count, const MethodInfo* mi) {
    if (variant_ext::Extension* e = Ext(); e && g && g->armed && self) {
        Il2CppObject* def = unity::Field<Il2CppObject*>(self, g->offFabDef);
        if (def && e->Owns(def)) {
            int32_t r = 7;  // FabricationBlockedReason.OPERATION_FAILED
            if (main_thread::IsMainThread() && !g->Faulted()) g_guard.Run([&] { r = e->Fabricate(self, def, count); });
            return r;
        }
    }
    if (!g || !g->armed || !g->ours || !self || unity::Field<Il2CppObject*>(self, g->offFabDef) != g->ours)
        return g_origFab(self, count, mi);
    int32_t r = 7;  // FabricationBlockedReason.OPERATION_FAILED
    if (main_thread::IsMainThread() && !g->Faulted()) g_guard.Run([&] { r = g->Fabricate(self, count); });
    return r;
}

void PlaceDetour(Il2CppObject* self, void* data, const MethodInfo* mi) {
    if (variant_ext::Extension* e = Ext(); e && g && g->armed && main_thread::IsMainThread() && !g->Faulted()) {
        variant_ext::PlaceCtx ctx;
        bool owned = false;
        g_guard.Run([&] {
            Il2CppObject* held = unity::Field<Il2CppObject*>(self, g->offHeld);
            owned = held && e->Owns(held);
            if (owned) e->BeforePlace(self, held, ctx);
        });
        if (owned) {
            g_origPlace(self, data, mi);
            if (ctx.ours) g_guard.Run([&] { e->AfterPlace(self, ctx); });
            return;
        }
    }
    if (!g || !g->armed || !g->ours || !main_thread::IsMainThread() || g->Faulted()) return g_origPlace(self, data, mi);
    Il2CppObject* before = nullptr;
    bool ours = false, fallback = false;
    g_guard.Run([&] {
        ours = unity::Field<Il2CppObject*>(self, g->offHeld) == g->ours;
        if (!ours) return;
        before = unity::Field<Il2CppObject*>(self, g->offLastPlaced);
        if (g->Kits() > 0) return;
        g->SetRef(self, g->offHeld, g->base);  // no kit left: a plain Refinery Link is placed
        fallback = true;
        SR2_INFO("[{}] quantum link: no Quantum Storage Link left to place - placing a plain Refinery Link", g->module);
    });
    g_origPlace(self, data, mi);
    if (!ours || fallback) return;
    g_guard.Run([&] {
        Il2CppObject* last = unity::Field<Il2CppObject*>(self, g->offLastPlaced);
        if (!last || last == before || !unity::IsAlive(last)) return;  // nothing placed (blocked, popup)
        const int64_t id = g->ActorOfGadget(last);
        if (!id) return;
        const int kits = g->Kits();
        g->List(id);
        g->SetKits(kits - 1);
        SR2_INFO("[{}] quantum link: Quantum Storage Link placed (actor {}, saved as a Refinery Link; {} kit(s) left)",
                 g->module, id, std::max(kits - 1, 0));
    });
}

void PickupDetour(Il2CppObject* self, Il2CppObject* gadget, const MethodInfo* mi) {
    if (variant_ext::Extension* e = Ext(); e && g && g->armed && main_thread::IsMainThread() && !g->Faulted()) {
        bool mine = false;
        g_guard.Run([&] { mine = e->BeforePickup(self, gadget); });
        if (mine) {
            g_origPickup(self, gadget, mi);
            g_guard.Run([&] { e->AfterPickup(self); });
            return;
        }
    }
    if (!g || !g->armed || !g->ours || !main_thread::IsMainThread() || g->Faulted()) return g_origPickup(self, gadget, mi);
    bool was = false;
    g_guard.Run([&] {
        const int64_t id = g->ActorOfGadget(gadget);
        if (!id || !g->Listed(id)) return;
        g->Unlist(id);
        g->SetKits(g->Kits() + 1);
        g->DropRec(id, false);  // its GameObject goes with the pickup
        was = true;
        SR2_INFO("[{}] quantum link: Quantum Storage Link picked up (actor {}): kit back, held as the Quantum Storage "
                 "Link variant", g->module, id);
    });
    g_origPickup(self, gadget, mi);
    if (!was) return;
    g_guard.Run([&] {
        if (unity::Field<Il2CppObject*>(self, g->offHeld) == g->base)
            g->Call(g->setHeld, self, {g->ours}, "GadgetItem.SetHeldGadget");
    });
}

// The toggle-variant key on a placed Refinery Link asked for our variant: flip quantum on / off in place.
void ChangeVariantDetour(Il2CppObject* self, Il2CppObject* variant, Il2CppObject* regions, const MethodInfo* mi) {
    if (variant_ext::Extension* e = Ext(); e && g && g->armed && variant && main_thread::IsMainThread() && !g->Faulted() &&
                                           e->Owns(variant)) {
        g_guard.Run([&] { e->ChangeVariant(self, variant); });
        return;
    }
    if (!g || !g->armed || !g->ours || variant != g->ours || !main_thread::IsMainThread() || g->Faulted())
        return g_origVariant(self, variant, regions, mi);
    g_guard.Run([&] {
        const int64_t id = g->ActorOfGadget(self);
        if (!id) return;
        const int kits = g->Kits();
        if (g->Listed(id)) {
            g->Unlist(id);
            g->SetKits(kits + 1);
            g->DropRec(id, true);
            SR2_INFO("[{}] quantum link: actor {} is a plain Refinery Link again (kit back, {} now)", g->module, id, kits + 1);
        } else if (kits > 0) {
            g->List(id);
            g->SetKits(kits - 1);
            SR2_INFO("[{}] quantum link: Refinery Link (actor {}) turned into a Quantum Storage Link ({} kit(s) left)",
                     g->module, id, kits - 1);
        } else {
            SR2_INFO("[{}] quantum link: no Quantum Storage Link kit left - actor {} stays a Refinery Link", g->module, id);
        }
    });
}

void DecrementDetour(Il2CppObject* self, int32_t count, const MethodInfo* mi) {
    Il2CppObject* storage = (g && self && g->offSmStorage) ? unity::Field<Il2CppObject*>(self, g->offSmStorage) : nullptr;
    if (!storage || !main_thread::IsMainThread() || !g->storages.count(storage)) return g_origDec(self, count, mi);
    const int slot = unity::Field<int32_t>(self, g->offSmIndex);
    g_origDec(self, count, mi);
    g_guard.Run([&] { g->OnTaken(storage, slot, count); });
}

// Use (the interact key) on a placed Quantum Storage Link: instead of the Refinery Link's refinery
// screen, HUD notices say what the drones take here (the Quantum Cloud's contents). Round 9: "pressing
// Use on it shows the refinery still". Pointer lookups only for every other interactable. If no notice
// can be shown, the refinery screen opens as before.
template <InteractFn* Orig>
void InteractDetour(Il2CppObject* self, const MethodInfo* mi) {
    if (g && g->armed && self && main_thread::IsMainThread() && !g->Faulted() && g->interactables.count(self)) {
        bool handled = false;
        g_guard.Run([&] { handled = g->OnUse(self); });
        if (handled) return;
    }
    (*Orig)(self, mi);
}

// ---------------------------------------------------------------- definition --

bool Impl::EnsureDefinition() {
    if (ours) return true;
    if (defFailed) return false;
    Il2CppObject* lookup = host.lookup ? host.lookup() : nullptr;
    if (!lookup) return false;
    auto fail = [&](const std::string& what) {
        defFailed = true;
        SR2_WARN("[{}] quantum link: definition not built ({}) - the Quantum Storage Link is not offered", module, what);
        return false;
    };
    bool ok = false;
    Il2CppObject* b = Call(getRefinery, lookup, {}, "LookupDirector.get_RefineryLink", &ok);
    if (!ok || !IsA(b, gadgetDefClass) || !unity::IsAlive(b)) {
        // Round 8: in the user's game the property stayed null for a whole session (no card, no
        // log). The Refinery Link is also a member of GadgetWarpGroup: found there by name.
        Il2CppObject* byName = FindRefineryLink(lookup);
        if (!byName) {
            if (++defWaits == 30)
                SR2_WARN("[{}] quantum link: no Refinery Link definition yet (LookupDirector.RefineryLink {}, none named "
                         "'RefineryLink' in the gadget groups) - still waiting", module, ok ? "null" : "failed");
            return false;  // not ready yet
        }
        if (!loggedRefineryFallback)
            SR2_INFO("[{}] quantum link: LookupDirector.RefineryLink is {} - using '{}' from the gadget groups", module,
                     ok ? "null" : "unreadable", unity::GetName(byName));
        loggedRefineryFallback = true;
        b = byName;
    }
    Il2CppObject* prefab = unity::Field<Il2CppObject*>(b, offPrefab);
    if (!unity::IsAlive(prefab)) return fail("the Refinery Link has no prefab");
    Il2CppObject* d = unity::Instantiate(b, nullptr);
    if (!IsA(d, gadgetDefClass) || !unity::IsAlive(d) || d == b) return fail("Object.Instantiate");
    if (!unity::NewGCHandle(d) || !unity::NewGCHandle(b)) return fail("GC handle");
    int32_t flags = 32;  // HideFlags.DontUnloadUnusedAsset
    Call(setHideFlags, d, {&flags}, "Object.set_hideFlags");
    unity::SetName(d, kDefName);
    // ReferenceId / StableHashedId are cached privately: computed again from the new name.
    unity::Field<Il2CppObject*>(d, offRefId) = nullptr;
    unity::Field<bool>(d, offHashInit) = false;
    SetRef(d, offVariantOf, b);
    SetRef(d, offLinked, nullptr);
    if (unity::Field<Il2CppObject*>(d, offPrefab) != prefab) SetRef(d, offPrefab, prefab);
    unity::Field<Color>(d, offGrad1) = kPurple;
    unity::Field<Color>(d, offGrad2) = kCyan;
    if (Il2CppObject* n = NewLocalized(unity::Field<Il2CppObject*>(b, offLocName), kTitle)) SetRef(d, offLocName, n);
    if (Il2CppObject* n = NewLocalized(unity::Field<Il2CppObject*>(b, offDesc), kDescription)) SetRef(d, offDesc, n);
    Il2CppObject* refId = Call(getRefId, d, {}, "IdentifiableType.get_ReferenceId", &ok);
    const std::string rid = ok ? unity::ToUtf8(reinterpret_cast<Il2CppString*>(refId)) : std::string();
    Il2CppObject* baseRef = Call(getRefId, b, {}, "IdentifiableType.get_ReferenceId", &ok);
    baseRefId = ok ? unity::ToUtf8(reinterpret_cast<Il2CppString*>(baseRef)) : std::string();
    if (rid.empty() || rid == baseRefId || baseRefId.empty())
        return fail(std::format("reference ids '{}' / '{}'", rid, baseRefId));
    base = b;
    ours = d;
    ScaleCost();
    g_base.store(b, std::memory_order_release);
    g_ours.store(d, std::memory_order_release);
    MakeLook();
    SR2_INFO("[{}] quantum link: definition '{}' built from '{}' (variant of the Refinery Link, same prefab '{}')", module,
             rid, baseRefId, unity::GetName(prefab));
    return true;
}

// The gadget definition named "RefineryLink" in LookupDirector._gadgetDefinitions and its member
// groups (GadgetWarpGroup, index 13 in 1.3.0). Only while the definition is not built (once per tick).
Il2CppObject* Impl::FindRefineryLink(Il2CppObject* lookup) {
    std::vector<Il2CppObject*> groups{unity::Field<Il2CppObject*>(lookup, offGadgets)};
    for (size_t gi = 0; gi < groups.size() && gi < 64; ++gi) {
        Il2CppObject* grp = groups[gi];
        if (!IsA(grp, groupClass)) continue;
        void* members = unity::Field<void*>(grp, offMembers);
        for (size_t i = 0, n = unity::ListCount(members); i < n && i < 1000; ++i) {
            auto* t = static_cast<Il2CppObject*>(unity::ListAt(members, i));
            if (IsA(t, gadgetDefClass) && unity::IsAlive(t) && unity::GetName(t) == "RefineryLink") return t;
        }
        void* subs = unity::Field<void*>(grp, offSubGroups);
        for (size_t i = 0, n = unity::ListCount(subs); i < n && i < 64; ++i)
            groups.push_back(static_cast<Il2CppObject*>(unity::ListAt(subs, i)));
    }
    return nullptr;
}

// The Refinery Link's crafting cost, Newbucks x3 and items x2 (on our own deep copies).
void Impl::ScaleCost() {
    char* mine = reinterpret_cast<char*>(ours) + offCosts;
    char* theirs = reinterpret_cast<char*>(base) + offCosts;
    void* cur = *reinterpret_cast<void**>(mine + offPcCurrency);
    void* idents = *reinterpret_cast<void**>(mine + offPcIdent);
    if ((cur && cur == *reinterpret_cast<void**>(theirs + offPcCurrency)) ||
        (idents && idents == *reinterpret_cast<void**>(theirs + offPcIdent))) {
        SR2_WARN("[{}] quantum link: the cost lists are shared with the Refinery Link - its cost is used unchanged", module);
        return;
    }
    std::string paid;
    for (size_t i = 0, n = unity::ListCount(cur); i < n && i < 8; ++i)
        if (auto* e = static_cast<Il2CppObject*>(unity::ListAt(cur, i))) {
            int32_t& a = unity::Field<int32_t>(e, offCostAmount);
            a = std::max(a * 3, 1);
            paid += std::format("{}{} currency", paid.empty() ? "" : ", ", a);
        }
    auto itemsOff = idents ? Resolver::Get().FieldOffset(unity::ClassOf(static_cast<Il2CppObject*>(idents)), "_items")
                           : std::nullopt;
    void* arr = itemsOff ? unity::Field<void*>(idents, *itemsOff) : nullptr;
    for (size_t i = 0, n = unity::ListCount(idents); arr && i < n && i < 16; ++i) {
        char* e = unity::ArrayData<char>(arr) + i * 16;  // IdentCostEntry {int Amount @0; IdentifiableType IdentType @8}
        int32_t& a = *reinterpret_cast<int32_t*>(e);
        a = std::max(a * 2, 1);
        paid += std::format("{}{} {}", paid.empty() ? "" : ", ", a,
                            unity::GetName(*reinterpret_cast<Il2CppObject**>(e + 8)));
    }
    SR2_INFO("[{}] quantum link: Fabricator cost {}", module, paid.empty() ? "unchanged (no entries)" : paid);
}

// Our variant in the live GadgetDirector's map, our ReferenceId in the save's reverse index. Cheap
// when nothing changed (native list walks, one ContainsKey).
void Impl::Register() {
    // variants
    Il2CppObject* gd = SceneField(offScGadgetDir);
    Il2CppObject* map = unity::IsAlive(gd) ? unity::Field<Il2CppObject*>(gd, offVariantMap) : nullptr;
    if (map) {
        DictMethods& d = Dict(variantDict, map);
        bool ok = false;
        Il2CppObject* list = nullptr;
        const bool found = d.tryGet && Value<bool>(Call(d.tryGet, map, {base, &list}, "variant map TryGetValue", &ok));
        if (ok && found && list) {
            ListMethods& l = List(variantList, list);
            if (!ListHas(list, ours) && l.add) Call(l.add, list, {ours}, "variant list Add", &ok);
            // A key of our own (no variants): every variant lookup of our definition finds an entry.
            Il2CppObject* mine = nullptr;
            const bool have = Value<bool>(Call(d.tryGet, map, {ours, &mine}, "variant map TryGetValue", &ok));
            if (ok && !have && l.ctor && objectNew && d.setItem) {
                Il2CppObject* empty = objectNew(api().il2cpp_object_get_class(list));
                if (empty) Call(l.ctor, empty, {}, "List..ctor", &ok);
                if (empty && ok) Call(d.setItem, map, {ours, empty}, "variant map set_Item", &ok);
            }
            if (gd != registeredDir) {
                registeredDir = gd;
                SR2_INFO("[{}] quantum link: registered as the Refinery Link's variant", module);
            }
        } else if (!loggedRegister) {
            loggedRegister = true;
            SR2_WARN("[{}] quantum link: the Refinery Link has no variant entry - the variant can't be held yet", module);
        }
    }
    // save alias
    bool ok = false;
    Il2CppObject* gc = Call(gameCtxInst, nullptr, {}, "GameContext.Instance", &ok);
    Il2CppObject* asd = ok && unity::IsAlive(gc) ? unity::Field<Il2CppObject*>(gc, offGcAutoSave) : nullptr;
    Il2CppObject* srt = unity::IsAlive(asd) ? unity::Field<Il2CppObject*>(asd, offAsdTranslation) : nullptr;
    Il2CppObject* table = srt ? unity::Field<Il2CppObject*>(srt, offSrtIdentTable) : nullptr;
    if (!table) return;
    auto rev = Resolver::Get().FieldOffset(api().il2cpp_object_get_class(table), "_reverseIndex");
    Il2CppObject* dict = rev ? unity::Field<Il2CppObject*>(table, *rev) : nullptr;
    if (!dict) return;
    DictMethods& d = Dict(aliasDict, dict);
    if (!d.containsKey || !d.tryGet || !d.setItem) return;
    Il2CppString* mine = unity::NewString(std::format("GadgetDefinition.{}", kDefName));
    if (Value<bool>(Call(d.containsKey, dict, {mine}, "save index ContainsKey", &ok)) || !ok) return;
    int32_t id = -1;
    const bool found = Value<bool>(Call(d.tryGet, dict, {unity::NewString(baseRefId), &id}, "save index TryGetValue", &ok));
    if (!ok || !found || id < 0) {
        if (!loggedAlias) SR2_WARN("[{}] quantum link: '{}' is not in the save index - no save alias", module, baseRefId);
        loggedAlias = true;
        return;
    }
    Call(d.setItem, dict, {mine, &id}, "save index set_Item", &ok);
    if (ok) SR2_INFO("[{}] quantum link: save alias GadgetDefinition.{} -> persistence id {} ({})", module, kDefName, id, baseRefId);
}

// ---------------------------------------------------------------- fabricator --

void Impl::InsertItem(Il2CppObject* category, Il2CppObject* list, Il2CppObject* helper, Il2CppObject* factory) {
    if (!ours) {
        if (!loggedNoDef) SR2_INFO("[{}] quantum link: Fabricator opened before the definition was built (it is built at the "
                                   "first tick with a save loaded{}) - no Quantum Storage Link card this time", module,
                                   defFailed ? "; building it FAILED, see above" : "");
        loggedNoDef = true;
        return;
    }
    int32_t at = -1;
    const size_t n = unity::ListCount(list);
    for (size_t i = 0; i < n && i < 256; ++i) {
        auto* item = static_cast<Il2CppObject*>(unity::ListAt(list, i));
        if (!IsA(item, fabItemClass)) continue;
        Il2CppObject* def = unity::Field<Il2CppObject*>(item, offFabDef);
        if (def == ours) return;  // already there
        if (def == base) at = static_cast<int32_t>(i) + 1;
    }
    if (at < 0) return;  // not the Refinery Link's category
    ListMethods& l = List(itemList, list);
    Il2CppObject* item = objectNew(fabItemClass);
    bool ok = false;
    if (item) Call(fabItemCtor, item, {helper, factory, unity::Field<Il2CppObject*>(category, offCatEvent), ours},
                   "GadgetFabricatableItem..ctor", &ok);
    if (!item || !ok || !l.insert) return;
    // No pinned recipe for our card: RecipeFactory builds a variant's recipe from its BASE gadget (the
    // Refinery Link's cost, saved as "G:GadgetDefinition.RefineryLink"), which is not what it costs.
    // Without a recipe CanBePinned is false; the card's cost display uses PurchaseCost (ours).
    SetRef(item, offFabRecipe, nullptr);
    Call(l.insert, list, {&at, item}, "Fabricator item list Insert", &ok);
    if (!ok) return;
    // Why the card is locked / unlocked, logged when it changes.
    const buildables::Unlock o = Override();
    const bool bp = BaseBlueprint();
    const bool unlocked = o == buildables::Unlock::kUnlocked || (o == buildables::Unlock::kFollowBase && bp);
    const std::string state = std::format(
        "{} ({}; Refinery Link blueprint {})", unlocked ? "unlocked" : "LOCKED",
        o == buildables::Unlock::kUnlocked ? "Sandbox override: unlocked"
        : o == buildables::Unlock::kLocked ? "Sandbox override: locked"
                                           : "unlocked with the Refinery Link blueprint",
        bp ? "owned" : "not owned");
    if (state != cardState) {
        cardState = state;
        SR2_INFO("[{}] quantum link: Fabricator card added after the Refinery Link (Warp Tech, position {}): {}, {} kit(s)",
                 module, at + 1, state, Kits());
    }
}

// FabricateCategoryRuntime caches the category's member count (_categoryCount = TotalCount) at
// construction: FabricatorItemGrid.SetCategory sizes its pages from it (pages = ceil(TotalCount / grid
// size)) and the category title shows "unlocked / TotalCount". Always raised to the item count (round 8:
// "a 41st blueprint ... can't see what it is"; whatever the grid size, every card gets a page, and the
// title counts the cards the grid shows).
void Impl::FixPageCount(Il2CppObject* runtime, Il2CppObject* category, int added) {
    if (!offRtCount || added <= 0) return;
    int32_t& total = unity::Field<int32_t>(runtime, offRtCount);
    const int32_t items = static_cast<int32_t>(unity::ListCount(unity::Field<void*>(runtime, offRtItems)));
    if (items <= total) return;
    const std::string name = unity::GetName(category);
    auto it = countLogged.find(name);
    if (it == countLogged.end() || it->second != items)
        SR2_INFO("[{}] Fabricator category '{}': entry count raised for {} mod card(s) ({} -> {} entries)", module, name,
                 added, total, items);
    countLogged[name] = items;
    total = items;
}

int32_t Impl::Fabricate(Il2CppObject* item, int32_t count) {
    bool ok = false;
    const int32_t reason = Value<int32_t>(Call(fabCheck, item, {&count}, "GadgetFabricatableItem.CheckCanFabricate", &ok), 7);
    if (!ok) return 7;
    if (reason != 0) return reason;
    Il2CppObject* sd = SceneField(offScSpendable);
    Il2CppObject* gd = SceneField(offScGadgetDir);
    if (!unity::IsAlive(sd) || !unity::IsAlive(gd)) return 7;
    // PurchaseCost is a struct of three references: passed by pointer to our definition's copy.
    void* cost = reinterpret_cast<char*>(ours) + offCosts;
    if (!Value<bool>(Call(spend, sd, {cost, &count}, "SpendableDirector.Spend", &ok)) || !ok) return 3;  // INSUFFICIENT_COST_ITEMS
    const int32_t added = Value<int32_t>(Call(addItem, gd, {base, &count}, "GadgetDirector.AddItem", &ok));
    const int kits = Kits() + count;
    SetKits(kits);
    SR2_INFO("[{}] quantum link: fabricated {} Quantum Storage Link(s): {} Refinery Link(s) added, {} kit(s) now", module,
             count, ok ? added : 0, kits);
    return 0;
}

// ---------------------------------------------------------------- visuals --

void Impl::FindFx() {
    if (fxTried) return;
    fxTried = true;
    // The drone station's cloud withdraw FX.
    if (Il2CppObject* dd = SceneField(offScDrones); unity::IsAlive(dd)) {
        Il2CppObject* st = unity::Field<Il2CppObject*>(dd, offStationType);
        Il2CppObject* prefab = IsA(st, identTypeClass) ? unity::Field<Il2CppObject*>(st, offPrefab) : nullptr;
        Il2CppObject* ctrl = unity::IsAlive(prefab) ? InChildren(prefab, cloudCtrlClass) : nullptr;
        Il2CppObject* fx = ctrl ? unity::Field<Il2CppObject*>(ctrl, offSpawnFx) : nullptr;
        if (unity::IsAlive(fx) && unity::NewGCHandle(fx)) cloudFx = fx;
    }
    // A teleporter gadget's active FX (the glow), found in the gadget definitions group.
    Il2CppObject* lookup = host.lookup ? host.lookup() : nullptr;
    std::vector<Il2CppObject*> groups;
    if (lookup) groups.push_back(unity::Field<Il2CppObject*>(lookup, offGadgets));
    for (size_t gi = 0; gi < groups.size() && gi < 64 && !glowSrc; ++gi) {
        Il2CppObject* grp = groups[gi];
        if (!IsA(grp, groupClass)) continue;
        void* members = unity::Field<void*>(grp, offMembers);
        for (size_t i = 0, n = unity::ListCount(members); i < n && i < 1000 && !glowSrc; ++i) {
            auto* t = static_cast<Il2CppObject*>(unity::ListAt(members, i));
            if (!IsA(t, gadgetDefClass) || unity::Field<int32_t>(t, offGType) != kTeleporterType) continue;
            Il2CppObject* prefab = unity::Field<Il2CppObject*>(t, offPrefab);
            Il2CppObject* node = unity::IsAlive(prefab) ? InChildren(prefab, teleNodeClass) : nullptr;
            if (!node) continue;
            Il2CppObject* fx = unity::Field<Il2CppObject*>(node, offActiveFx);
            if (!unity::IsAlive(fx)) fx = unity::Field<Il2CppObject*>(node, offArriveFx);
            if (unity::IsAlive(fx) && IsA(fx, gameObjectClass) && unity::NewGCHandle(fx)) {
                glowSrc = fx;
                SR2_INFO("[{}] quantum link: glow from teleporter '{}' ('{}')", module, unity::GetName(t), unity::GetName(fx));
            }
        }
        void* subs = unity::Field<void*>(grp, offSubGroups);
        for (size_t i = 0, n = unity::ListCount(subs); i < n && i < 64; ++i)
            groups.push_back(static_cast<Il2CppObject*>(unity::ListAt(subs, i)));
    }
    SR2_INFO("[{}] quantum link: quantum pulse FX {}, glow {}", module, cloudFx ? unity::GetName(cloudFx) : "not found",
             glowSrc ? "found" : "not found (no teleporter FX)");
}

// Colour properties towards quantum purple, emission properties to cyan. Property names from the
// material itself (Material.GetPropertyNames(Vector)), so any shader works.
void Impl::Tint(Il2CppObject* mat, int& colors, int& emissions, std::string& props) {
    int32_t vec = 2;  // MaterialPropertyType.Vector
    bool ok = false;
    Il2CppObject* names = Call(matPropNames, mat, {&vec}, "Material.GetPropertyNames", &ok);
    for (size_t i = 0, n = ok ? unity::ArrayLength(names) : 0; i < n && i < 64; ++i) {
        auto* s = static_cast<Il2CppString*>(unity::ArrayAt(names, i));
        const std::string name = unity::ToUtf8(s);
        const std::string low = Lower(name);
        const bool emission = low.find("emiss") != std::string::npos || low.find("glow") != std::string::npos;
        const bool color = !emission && (low.find("color") != std::string::npos || low.find("tint") != std::string::npos ||
                                         low.find("albedo") != std::string::npos);
        if (!emission && !color) continue;
        Color c = Value<Color>(Call(matGetColor, mat, {s}, "Material.GetColor", &ok));
        if (!ok) continue;
        Color out;
        if (emission) {
            const float lum = std::max({c.r, c.g, c.b, 0.0f});
            const float k = std::clamp(lum, 0.6f, 4.0f) * 1.5f;  // HDR: keep a glow that bright, at least visible
            out = {kCyan.r * k, kCyan.g * k, kCyan.b * k, c.a};
            ++emissions;
        } else {
            constexpr float t = 0.6f;
            out = {c.r + (kPurple.r - c.r) * t, c.g + (kPurple.g - c.g) * t, c.b + (kPurple.b - c.b) * t, c.a};
            ++colors;
        }
        Call(matSetColor, mat, {s, &out}, "Material.SetColor", &ok);
        if (ok && props.size() < 300) props += (props.empty() ? "" : " ") + name;
    }
}

// Instance and tint every material under `root` (once: our instances carry kMatTag). With `rec`, the
// original shared arrays are remembered to put them back.
void Impl::TintRenderers(Il2CppObject* root, Rec* rec, const char* what) {
    int colors = 0, emissions = 0, mats = 0;
    for (Il2CppObject* r : AllInChildren(root, rendererClass)) {
        bool ok = false;
        Il2CppObject* shared = Call(rendGetShared, r, {}, "Renderer.get_sharedMaterials", &ok);
        if (!ok || !shared) continue;
        bool done = false;
        for (size_t i = 0, n = unity::ArrayLength(shared); i < n && i < 16 && !done; ++i)
            if (auto* m = static_cast<Il2CppObject*>(unity::ArrayAt(shared, i)))
                done = unity::GetName(m).find(kMatTag) != std::string::npos;
        if (done) continue;  // tinted already (a holder of an earlier build)
        Il2CppObject* inst = Call(rendGetMaterials, r, {}, "Renderer.get_materials", &ok);
        if (!ok || !inst) continue;
        if (rec) {
            Il2CppGCHandle hr = unity::NewGCHandle(r), hs = unity::NewGCHandle(shared);
            if (hr) rec->handles.push_back(hr);
            if (hs) rec->handles.push_back(hs);
            rec->rends.push_back({r, shared});
        }
        for (size_t i = 0, n = unity::ArrayLength(inst); i < n && i < 16; ++i) {
            auto* m = static_cast<Il2CppObject*>(unity::ArrayAt(inst, i));
            if (!unity::IsAlive(m)) continue;
            std::string props;
            const int c0 = colors, e0 = emissions;
            Tint(m, colors, emissions, props);
            Il2CppObject* shader = Call(matGetShader, m, {}, "Material.get_shader", &ok);
            const std::string sh = ok && shader ? unity::GetName(shader) : std::string("?");
            if (loggedShaders.insert(sh).second)
                SR2_INFO("[{}] quantum link: shader '{}' ({}): {} colour / {} emission propert(ies) tinted [{}]", module, sh,
                         what, colors - c0, emissions - e0, props);
            unity::SetName(m, unity::GetName(m) + kMatTag);
            ++mats;
        }
        if (Faulted()) return;
    }
    (void)mats;
}

void Impl::Strip(Il2CppObject* root) {
    for (Il2CppClass* k : {monoClass, jointClass, rigidbodyClass, colliderClass}) {
        for (Il2CppObject* c : AllInChildren(root, k)) {
            if (k == monoClass && unity::ClassName(c).rfind("UnityEngine.", 0) == 0) continue;
            DestroyNow(c);
        }
        if (Faulted()) return;
    }
}

bool Impl::EnsureSlotDef() {
    if (slotDef || slotDefTried) return slotDef != nullptr;
    slotDefTried = true;
    Il2CppObject* best = nullptr;
    for (Il2CppObject* d : unity::FindObjectsOfTypeAll(slotDefClass)) {  // once per session
        if (!unity::IsAlive(d) || unity::Field<Il2CppObject*>(d, offSdMaxProp) || unity::Field<Il2CppObject*>(d, offSdUnlockProp))
            continue;
        if (!best || unity::Field<int32_t>(d, offSdMax) > unity::Field<int32_t>(best, offSdMax)) best = d;
    }
    Il2CppObject* c = best ? unity::Instantiate(best, nullptr) : nullptr;
    if (!unity::IsAlive(c) || !IsA(c, slotDefClass) || !unity::NewGCHandle(c)) {
        SR2_WARN("[{}] quantum link: no AmmoSlotDefinition to copy - links have no drone storage", module);
        return false;
    }
    int32_t flags = 32;  // HideFlags.DontUnloadUnusedAsset
    Call(setHideFlags, c, {&flags}, "Object.set_hideFlags");
    unity::SetName(c, "sr2QuantumLinkSlot");
    SetRef(c, offSdGroup, nullptr);  // any type (the cloud decides)
    unity::Field<int32_t>(c, offSdMax) = kSlotMax;
    slotDef = c;
    SR2_INFO("[{}] quantum link: drone storage slot from '{}' ({} per slot, no type group)", module, unity::GetName(best),
             kSlotMax);
    return true;
}

// Holder (glow + storage) under the link, the link's materials tinted, the drone port found.
bool Impl::Build(int64_t id, Il2CppObject* root, Rec& rec) {
    FindFx();
    Il2CppObject* rootTr = unity::TransformOf(root);
    if (!rootTr) return false;
    // Holders of an older generation / a lost record go first.
    for (Il2CppObject* child : ChildTransforms(rootTr))
        if (Il2CppObject* go = unity::GameObjectOf(child); go && unity::GetName(go).rfind(kHolderPrefix, 0) == 0) DestroyNow(go);
    rec = Rec{};
    rec.root = root;
    TintRenderers(root, &rec, "link");
    if (Faulted()) return false;
    bool ok = false;
    Il2CppObject* holder = objectNew(gameObjectClass);
    if (holder) Call(goCtor, holder, {unity::NewString(std::format("{}{}", kHolderPrefix, generation))}, "GameObject..ctor", &ok);
    if (!ok || !unity::IsAlive(holder)) return false;
    unity::SetActive(holder, false);  // nothing wakes up before it is set up
    Il2CppObject* holderTr = unity::TransformOf(holder);
    bool stays = false;
    Call(trSetParent, holderTr, {rootTr, &stays}, "Transform.SetParent", &ok);
    Vec3 zero{0.0f, 0.0f, 0.0f}, one{1.0f, 1.0f, 1.0f};
    Quat id0{0.0f, 0.0f, 0.0f, 1.0f};
    Call(trSetLocalPos, holderTr, {&zero}, "Transform.set_localPosition");
    Call(trSetLocalRot, holderTr, {&id0}, "Transform.set_localRotation");
    Call(trSetLocalScale, holderTr, {&one}, "Transform.set_localScale");
    rec.holder = holder;
    if (Il2CppGCHandle h = unity::NewGCHandle(holder)) rec.handles.push_back(h);
    // the glow
    if (glowSrc) {
        Il2CppObject* glow = unity::Instantiate(glowSrc, holderTr);
        if (unity::IsAlive(glow)) {
            Strip(glow);
            Il2CppObject* gtr = unity::TransformOf(glow);
            Vec3 pos{0.0f, kGlowHeight, 0.0f}, scale{kGlowScale, kGlowScale, kGlowScale};
            Call(trSetLocalPos, gtr, {&pos}, "Transform.set_localPosition");
            Call(trSetLocalRot, gtr, {&id0}, "Transform.set_localRotation");
            Call(trSetLocalScale, gtr, {&scale}, "Transform.set_localScale");
            const int scaled = ScaleParticles(glow);
            if (!loggedParticles) {
                loggedParticles = true;
                SR2_INFO("[{}] quantum link: glow at {:.2f} size, {} local-scaled particle system(s) shrunk with it", module,
                         kGlowScale, scaled);
            }
            unity::SetActive(glow, true);  // still inactive in the hierarchy (holder)
            TintRenderers(glow, nullptr, "glow");
            rec.glow = glow;
            if (Il2CppGCHandle h = unity::NewGCHandle(glow)) rec.handles.push_back(h);
            rec.rng = static_cast<uint32_t>(id * 2654435761u) | 1u;
            rec.glitchAt = Now() + 2.0 + static_cast<double>(id % 7) * 0.4;
        }
    } else if (!loggedGlow) {
        loggedGlow = true;
        SR2_INFO("[{}] quantum link: no glow FX - links are tinted only", module);
    }
    // the drone storage
    if (EnsureSlotDef()) {
        Il2CppObject* sgo = objectNew(gameObjectClass);
        if (sgo) Call(goCtor, sgo, {unity::NewString("sr2qstore")}, "GameObject..ctor", &ok);
        if (sgo && ok && unity::IsAlive(sgo)) {
            Call(trSetParent, unity::TransformOf(sgo), {holderTr, &stays}, "Transform.SetParent", &ok);
            Il2CppObject* storage = Call(goAddComponent, sgo, {unity::TypeOf(siloStorageClass)}, "AddComponent(SiloStorage)", &ok);
            Il2CppObject* defs = ok && IsA(storage, siloStorageClass) ? arrayNew(slotDefClass, kSlots) : nullptr;
            if (defs) {
                for (size_t i = 0; i < static_cast<size_t>(kSlots); ++i) SetElem(defs, i, slotDef);
                SetRef(storage, offStorageDefs, defs);
                Call(initAmmo, storage, {}, "SiloStorage.InitAmmo", &ok);
                Il2CppObject* ammo = ok ? Call(localAmmo, storage, {}, "SiloStorage.get_LocalAmmo", &ok) : nullptr;
                Il2CppObject* model = ok && ammo ? objectNew(ammoModelClass) : nullptr;
                if (model) Call(ammoModelCtor, model, {nullptr}, "AmmoModel..ctor", &ok);  // UnitPropertyTracker null, like vanilla
                if (model && ok) Call(ammoSetModel, ammo, {model}, "AmmoSlotManager.SetModel", &ok);
                if (model && ok) {
                    rec.storage = storage;
                    rec.ammo = ammo;
                    for (Il2CppObject* o : {storage, ammo})
                        if (Il2CppGCHandle h = unity::NewGCHandle(o)) rec.handles.push_back(h);
                }
            }
        }
    }
    unity::SetActive(holder, true);  // glow plays; the storage is active in the hierarchy (drones don't cancel)
    // the drone port: the link's own SiloCatcher (refinery input)
    rec.catcher = InChildren(root, siloCatcherClass);
    if (rec.catcher) {
        if (Il2CppGCHandle h = unity::NewGCHandle(rec.catcher)) rec.handles.push_back(h);
    } else if (!loggedNoCatcher) {
        loggedNoCatcher = true;
        SR2_WARN("[{}] quantum link: the Refinery Link has no SiloCatcher - drones can't take from it", module);
    }
    if (rec.catcher && rec.storage)
        for (int i = 0; i < kSlots; ++i) {
            Il2CppObject* sm = objectNew(storageMetaClass);
            int32_t idx = i;
            if (sm) Call(smCtor, sm, {&idx, rec.storage, rec.catcher}, "StorageMetadata..ctor", &ok);
            if (!sm || !ok) break;
            if (Il2CppGCHandle h = unity::NewGCHandle(sm)) rec.handles.push_back(h);
            rec.sms[i] = sm;
        }
    if (rec.storage) storages.insert(rec.storage);
    // Use: the link's interactables (logged once per class, so we know what the Refinery Link uses)
    if (interactableClass)
        for (Il2CppObject* c : AllInChildren(root, interactableClass)) {
            if (Il2CppGCHandle h = unity::NewGCHandle(c)) rec.handles.push_back(h);
            rec.interactables.push_back(c);
            interactables.insert(c);
            const std::string cn = unity::ClassName(c);
            if (loggedInteractables.insert(cn).second)
                SR2_INFO("[{}] quantum link: Use component '{}' on '{}'{}", module, cn, unity::GetName(unity::GameObjectOf(c)),
                         cn == "MonomiPark.SlimeRancher.UI.UIActivator" || IsA(c, uiDisplayIntClass)
                             ? " - Use opens the Quantum Cloud"
                             : " - not hooked, Use keeps its vanilla action");
        }
    if (Il2CppGCHandle h = unity::NewGCHandle(root)) rec.handles.push_back(h);
    SR2_INFO("[{}] quantum link: actor {} built ({} renderer(s) tinted, glow {}, drone storage {}, drone port {})", module, id,
             rec.rends.size(), glowSrc ? "on" : "off", rec.storage ? "ready" : "missing", rec.catcher ? "ready" : "missing");
    return true;
}

// Materials back (restore) and our objects away; handles freed. The GameObject may be gone already.
void Impl::Undecorate(Rec& rec, bool restore) {
    if (rec.storage) storages.erase(rec.storage);
    for (Il2CppObject* c : rec.interactables) interactables.erase(c);
    if (restore && unity::IsAlive(rec.root)) {
        bool ok = false;
        for (const RendRec& rr : rec.rends) {
            if (!unity::IsAlive(rr.renderer)) continue;
            Il2CppObject* inst = Call(rendGetShared, rr.renderer, {}, "Renderer.get_sharedMaterials", &ok);
            Call(rendSetShared, rr.renderer, {rr.shared}, "Renderer.set_sharedMaterials", &ok);
            for (size_t i = 0, n = inst ? unity::ArrayLength(inst) : 0; i < n && i < 16; ++i)
                if (auto* m = static_cast<Il2CppObject*>(unity::ArrayAt(inst, i));
                    unity::IsAlive(m) && unity::GetName(m).find(kMatTag) != std::string::npos)
                    Destroy(m);
        }
        Destroy(rec.holder);
    }
    FreeLater(std::move(rec.handles));
    rec = Rec{};
}

void Impl::DropRec(int64_t id, bool restore) {
    auto it = recs.find(id);
    if (it == recs.end()) return;
    Undecorate(it->second, restore);
    recs.erase(it);
}

void Impl::Pulse(Rec& rec, Il2CppObject* at) {
    if (!cloudFx || !spawnFx) return;
    Il2CppObject* tr = unity::TransformOf(at ? at : rec.root);
    if (!tr) return;
    bool ok = false;
    Vec3 pos = Value<Vec3>(Call(trGetPosition, tr, {}, "Transform.get_position", &ok));
    if (!ok) return;
    Quat id0{0.0f, 0.0f, 0.0f, 1.0f};
    Call(spawnFx, nullptr, {cloudFx, &pos, &id0}, "FXHelpers.SpawnAndPlayFX");
}

// Our icon (a purple copy of the Refinery Link's, also the drone source's) and the gadget looks: own
// description on the Fabricator card (the vanilla getter walks VariantOf to the Refinery Link's), the
// gadget-mode label "Quantum Storage Link" while held, a purple placement preview.
void Impl::MakeLook() {
    std::string why;
    Il2CppObject* baseIcon = unity::Field<Il2CppObject*>(base, offIcon);
    icon = gadget_look::TintedSprite(baseIcon, {kPurple.r, kPurple.g, kPurple.b}, 0.85f, why);
    if (icon) {
        SetRef(ours, offIcon, icon);
        SR2_INFO("[{}] quantum link: icon = a purple copy of the Refinery Link's", module);
        // The drone station's source list stretches each icon to a square box; the Refinery Link sprite is
        // taller than wide, so the source got its own square copy (round 11: "is wider than normally").
        std::string whySq;
        squareIcon = gadget_look::TintedSprite(baseIcon, {kPurple.r, kPurple.g, kPurple.b}, 0.85f, whySq, true);
        if (!squareIcon) SR2_INFO("[{}] quantum link: no square drone source icon ({}) - the card icon is used", module, whySq);
    } else {
        SR2_INFO("[{}] quantum link: no purple icon ({}) - the Refinery Link's icon is used", module, why);
    }
    if (!gadget_look::Ready()) return;
    gadget_look::Look look;
    Il2CppObject* name = unity::Field<Il2CppObject*>(ours, offLocName);
    Il2CppObject* desc = unity::Field<Il2CppObject*>(ours, offDesc);
    look.name = name != unity::Field<Il2CppObject*>(base, offLocName) ? name : nullptr;
    look.description = desc != unity::Field<Il2CppObject*>(base, offDesc) ? desc : nullptr;
    look.icon = icon;
    look.dressPreview = [this](Il2CppObject* placeholder) {
        const int n = gadget_look::TintTree(placeholder, {kPurple.r, kPurple.g, kPurple.b}, 0.6f, {kCyan.r, kCyan.g, kCyan.b},
                                            "Quantum Storage Link preview");
        if (!loggedPreview) SR2_INFO("[{}] quantum link: placement preview tinted ({} material(s))", module, n);
        loggedPreview = true;
    };
    gadget_look::Register(ours, std::move(look));
}

// The glow jitters in place and size (every 30-80 ms) for about half a second every 3.5-8 s. It used to drop
// out too (SetActive off / on), which restarted its particle systems: the user saw the effect "disappear in like
// 6-7 s intervals" (round 11). The ghost drones' look (ComponentAcquisitionDroneController: a material float
// "_PhaseValue" faded 0 -> 1 in Update) is a property of their own shader, so it can't be put on the teleporter
// FX; this jitter only hints at it with the transform.
void Impl::Glitch(Rec& rec, double now) {
    auto rnd = [&rec] {
        rec.rng ^= rec.rng << 13;
        rec.rng ^= rec.rng >> 17;
        rec.rng ^= rec.rng << 5;
        return static_cast<float>(rec.rng & 0xFFFFFF) / static_cast<float>(0x1000000);
    };
    Il2CppObject* tr = unity::TransformOf(rec.glow);
    if (!tr) return;
    if (!rec.glitching) {
        rec.glitching = true;
        rec.glitchEnd = now + 0.35 + rnd() * 0.3;
        rec.nextFlip = now;
    }
    if (now >= rec.glitchEnd) {  // back to normal
        rec.glitching = false;
        rec.glitchAt = now + 3.5 + rnd() * 4.5;
        Vec3 pos{0.0f, kGlowHeight, 0.0f}, scale{kGlowScale, kGlowScale, kGlowScale};
        Call(trSetLocalPos, tr, {&pos}, "Transform.set_localPosition");
        Call(trSetLocalScale, tr, {&scale}, "Transform.set_localScale");
        return;
    }
    if (now < rec.nextFlip) return;
    rec.nextFlip = now + 0.03 + rnd() * 0.05;
    Vec3 pos{(rnd() - 0.5f) * 0.08f, kGlowHeight + (rnd() - 0.5f) * 0.06f, (rnd() - 0.5f) * 0.08f};
    Vec3 scale{kGlowScale * (0.8f + rnd() * 0.35f), kGlowScale * (0.7f + rnd() * 0.45f), kGlowScale * (0.8f + rnd() * 0.35f)};
    Call(trSetLocalPos, tr, {&pos}, "Transform.set_localPosition");
    Call(trSetLocalScale, tr, {&scale}, "Transform.set_localScale");
}

// Every particle system under the glow (not its root) that simulates in Local scaling mode ignores its parents'
// scale: its own transform gets the glow factor. Returns how many were scaled.
int Impl::ScaleParticles(Il2CppObject* glow) {
    if (!particleClass || !psScalingMode || !trGetLocalScale) return 0;
    Il2CppObject* rootGo = unity::GameObjectOf(glow) ? unity::GameObjectOf(glow) : glow;
    int n = 0;
    for (Il2CppObject* ps : AllInChildren(glow, particleClass)) {
        if (unity::GameObjectOf(ps) == rootGo) continue;  // the root carries kGlowScale itself
        void* mainModule = ps;                          // ParticleSystem.MainModule { ParticleSystem m_ParticleSystem }
        if (psScalingMode(&mainModule) != 1) continue;   // ParticleSystemScalingMode.Local
        Il2CppObject* tr = unity::TransformOf(ps);
        bool ok = false;
        Vec3 s = Value<Vec3>(Call(trGetLocalScale, tr, {}, "Transform.get_localScale", &ok));
        if (!ok) continue;
        s = {s.x * kGlowScale, s.y * kGlowScale, s.z * kGlowScale};
        Call(trSetLocalScale, tr, {&s}, "Transform.set_localScale", &ok);
        n += ok ? 1 : 0;
    }
    return n;
}

// Use on a link opens the game's own Quantum Cloud menu (round 11: "[E] should open the quantum cloud GUI menu like it
// shows in drones menu"): the drone station menu (its display name read from the Drone Station prefab's
// UIDisplayInteractable._display) opened with UIDisplayManager.OpenDisplay and, once loaded (CloudFrame), shown
// without a station: DroneUIDisplay's IDisplayShowDataHandler<IDroneTaskAssignable>.UpdateShowData(null) builds the
// view model the map's drone panel uses (cloud view only, no withdraw; DroneMenuUI.SetViewModel drops the programming
// tab), then the Quantum Cloud tab is selected.
bool Impl::OpenCloudMenu() {
    if (!udmInstance || !udmOpen || !droneShowData) return false;
    if (cloudReq) return true;  // already opening
    if (cloudDisplay.empty() && !cloudDisplayLooked) {
        cloudDisplayLooked = true;
        Il2CppObject* dd = SceneField(offScDrones);
        Il2CppObject* st = unity::IsAlive(dd) ? unity::Field<Il2CppObject*>(dd, offStationType) : nullptr;
        Il2CppObject* prefab = IsA(st, identTypeClass) ? unity::Field<Il2CppObject*>(st, offPrefab) : nullptr;
        std::string classes;
        if (unity::IsAlive(prefab))
            for (Il2CppObject* c : AllInChildren(prefab, uiDisplayIntClass)) {
                const std::string name = unity::ToUtf8(unity::Field<Il2CppString*>(c, offUdiDisplay));
                classes += (classes.empty() ? "" : ", ") + unity::ClassName(c) + " '" + name + "'";
                if (cloudDisplay.empty() && !name.empty()) cloudDisplay = name;
            }
        if (cloudDisplay.empty())
            SR2_WARN("[{}] quantum link: the drone station menu's display was not found ({}) - Use shows HUD notices instead",
                     module, !prefab ? "no Drone Station prefab" : classes.empty() ? "no UIDisplayInteractable on it" : classes);
        else SR2_INFO("[{}] quantum link: Quantum Cloud menu = the drone station menu '{}' ({})", module, cloudDisplay, classes);
    }
    if (cloudDisplay.empty()) return false;
    bool ok = false;
    Il2CppObject* udm = Call(udmInstance, nullptr, {}, "UIDisplayManager.get_Instance", &ok);
    if (!ok || !udm) return false;
    Il2CppString* handle = unity::NewString(cloudDisplay);  // UIDisplayHandle = struct { string _name }
    Il2CppObject* req = Call(udmOpen, udm, {&handle}, "UIDisplayManager.OpenDisplay", &ok);
    if (!ok || !req) return false;
    cloudReq = unity::NewGCHandle(req);
    cloudReqAt = Now();
    return cloudReq != nullptr;
}

void Impl::EndCloudRequest() {
    if (cloudReq) unity::FreeGCHandle(cloudReq);  // frame callback, not a hook
    cloudReq = nullptr;
}

// While the menu loads (every frame, at most kCloudOpenSeconds): the request's display -> its GameObject ->
// DroneUIDisplay -> UpdateShowData(null), the Quantum Cloud tab.
void Impl::CloudFrame() {
    Il2CppObject* req = unity::GCHandleTarget(cloudReq);
    if (!req || Now() - cloudReqAt > kCloudOpenSeconds) {
        SR2_WARN("[{}] quantum link: the Quantum Cloud menu did not open within {:.0f} s", module, kCloudOpenSeconds);
        return EndCloudRequest();
    }
    if (api().il2cpp_object_get_class(req) != reqClass) {
        reqClass = api().il2cpp_object_get_class(req);
        reqTryGet = Resolver::Get().Method(api().il2cpp_object_get_class(req), "TryGetRequestedDisplay", 1, nullptr);
    }
    if (!reqTryGet) {
        SR2_WARN("[{}] quantum link: UIDisplayRequestHandle.TryGetRequestedDisplay not found - the menu opens as it is", module);
        return EndCloudRequest();
    }
    Il2CppObject* display = nullptr;
    bool ok = false;
    if (!Value<bool>(Call(reqTryGet, req, {&display}, "UIDisplayRequestHandle.TryGetRequestedDisplay", &ok)) || !ok || !display) return;
    if (api().il2cpp_object_get_class(display) != rtClass) {
        rtClass = api().il2cpp_object_get_class(display);
        auto o = Resolver::Get().FieldOffset(api().il2cpp_object_get_class(display), "_displayGameObject");
        rtDisplayGo = o ? *o : 0;
    }
    Il2CppObject* go = rtDisplayGo ? unity::Field<Il2CppObject*>(display, rtDisplayGo) : nullptr;
    if (!unity::IsAlive(go)) return;  // still loading
    Il2CppObject* dui = InChildren(go, droneUiClass);
    if (!dui) return;
    EndCloudRequest();
    Call(droneShowData, dui, {nullptr}, "DroneUIDisplay.UpdateShowData(null)", &ok);
    if (!ok) return;
    // The Quantum Cloud tab: the DroneTabSetup whose Target is the cloud inventory's transform.
    Il2CppObject* menu = unity::Field<Il2CppObject*>(dui, offMenuRoot);
    Il2CppObject* cloud = unity::IsAlive(menu) ? unity::Field<Il2CppObject*>(menu, offMenuCloud) : nullptr;
    void* tabs = unity::IsAlive(menu) ? unity::Field<void*>(menu, offMenuTabs) : nullptr;
    Il2CppObject* cloudTr = unity::IsAlive(cloud) ? unity::TransformOf(cloud) : nullptr;
    int32_t index = -1;
    for (size_t i = 0, n = unity::ListCount(tabs); cloudTr && i < n && i < 8; ++i)
        if (auto* t = static_cast<Il2CppObject*>(unity::ListAt(tabs, i)); t && unity::Field<Il2CppObject*>(t, offTabTarget) == cloudTr)
            index = static_cast<int32_t>(i);
    Il2CppObject* tabbed = unity::IsAlive(menu) ? unity::Field<Il2CppObject*>(menu, offMenuTabbed) : nullptr;
    bool switched = false;
    if (index >= 0 && unity::IsAlive(tabbed)) switched = Value<bool>(Call(tabGoTo, tabbed, {&index}, "TabbedControl.GoToTab", &ok)) && ok;
    SR2_INFO("[{}] quantum link: Quantum Cloud menu open ({}; view only, no station)", module,
             switched ? std::format("cloud tab {}", index + 1) : index < 0 ? std::string("cloud tab not found") : std::string("already on the cloud tab"));
}

std::string Impl::NameOf(Il2CppObject* type) {
    Il2CppObject* ls = IsA(type, identTypeClass) ? unity::Field<Il2CppObject*>(type, offLocName) : nullptr;
    bool ok = false;
    Il2CppObject* str = ls && lsGetString ? Call(lsGetString, ls, {}, "LocalizedString.GetLocalizedString", &ok) : nullptr;
    std::string name = ok ? unity::ToUtf8(reinterpret_cast<Il2CppString*>(str)) : std::string();
    return name.empty() ? unity::GetName(type) : name;
}

// Use on a link: one burst of HUD notices - what the link is for, then the largest cloud amounts.
bool Impl::OnUse(Il2CppObject* interactable) {
    const double now = Now();
    if (now - lastUse < kUseCooldown) return true;  // a held key / double press: no second burst
    if (OpenCloudMenu()) {
        lastUse = now;
        SR2_INFO("[{}] quantum link: Use on '{}' - opening the Quantum Cloud menu instead of the refinery screen", module,
                 unity::GetName(unity::GameObjectOf(interactable)));
        return true;
    }
    if (Faulted() || !host.notify) return false;
    std::vector<std::pair<Il2CppObject*, std::string>> lines;
    lines.emplace_back(icon ? icon : unity::Field<Il2CppObject*>(base, offIcon),
                       "Quantum Storage Link: ranch drones set to this source take items from your Quantum Cloud here");
    const auto entries = CloudEntries(Cloud());
    int total = 0;
    for (const auto& e : entries) total += e.second;
    for (size_t i = 0; i < entries.size() && static_cast<int>(i) < kUseLines; ++i)
        lines.emplace_back(unity::Field<Il2CppObject*>(entries[i].first, offIcon),
                           std::format("{} in the Quantum Cloud: {}", NameOf(entries[i].first), entries[i].second));
    if (entries.size() > static_cast<size_t>(kUseLines))
        lines.emplace_back(nullptr, std::format("... and {} more type(s)", entries.size() - kUseLines));
    if (entries.empty()) lines.emplace_back(nullptr, "The Quantum Cloud is empty - explorer drones fill it");
    if (!host.notify(lines)) {
        SR2_INFO("[{}] quantum link: Use - HUD notices not available now, the Refinery Link screen opens", module);
        return false;
    }
    lastUse = now;
    SR2_INFO("[{}] quantum link: Use on '{}' - Quantum Cloud shown ({} type(s), {} item(s)) instead of the refinery screen",
             module, unity::GetName(unity::GameObjectOf(interactable)), entries.size(), total);
    return true;
}

// ---------------------------------------------------------------- cloud / slots --

bool Impl::CloudLayout(Il2CppObject* dict) {
    Il2CppClass* k = api().il2cpp_object_get_class(dict);
    if (k == cloudLayout.klass) return cloudLayout.ok;
    cloudLayout = {};
    cloudLayout.klass = k;
    Resolver& R = Resolver::Get();
    FieldInfo* fe = R.Field(k, "_entries");
    auto ce = R.FieldOffset(k, "_entries");
    auto cc = R.FieldOffset(k, "_count");
    const Il2CppType* t = fe && fieldGetType ? fieldGetType(fe) : nullptr;
    Il2CppClass* arr = t && api().il2cpp_class_from_type ? api().il2cpp_class_from_type(t) : nullptr;
    Il2CppClass* elem = arr && elemClass ? elemClass(arr) : nullptr;
    const int stride = elem && elemSize ? elemSize(elem) : 0;
    auto structOff = [&](const char* name, size_t& out) {
        FieldInfo* f = elem ? api().il2cpp_class_get_field_from_name(elem, name) : nullptr;
        if (!f) return false;
        const size_t o = api().il2cpp_field_get_offset(f);
        if (o < 2 * sizeof(void*)) return false;
        out = o - 2 * sizeof(void*);  // value-type field offsets include the object header
        return true;
    };
    size_t hash = 0, key = 0, value = 0;
    if (ce && cc && stride >= 16 && stride <= 64 && structOff("hashCode", hash) && structOff("key", key) &&
        structOff("value", value) && key % sizeof(void*) == 0 && key + sizeof(void*) <= static_cast<size_t>(stride) &&
        value + sizeof(int32_t) <= static_cast<size_t>(stride)) {
        cloudLayout.ok = true;
        cloudLayout.entries = *ce;
        cloudLayout.count = *cc;
        cloudLayout.stride = static_cast<size_t>(stride);
        cloudLayout.hash = hash;
        cloudLayout.key = key;
        cloudLayout.value = value;
        SR2_INFO("[{}] quantum link: cloud dictionary entries {} bytes (key +{}, value +{})", module, stride, key, value);
    } else {
        SR2_WARN("[{}] quantum link: cloud dictionary layout not resolvable - drones find nothing to take", module);
    }
    return cloudLayout.ok;
}

// (type, amount) of every cloud entry with an amount, largest first.
std::vector<std::pair<Il2CppObject*, int>> Impl::CloudEntries(Il2CppObject* cloud) {
    std::vector<std::pair<Il2CppObject*, int>> out;
    Il2CppObject* dict = cloud ? unity::Field<Il2CppObject*>(cloud, offAmounts) : nullptr;
    if (!dict || !CloudLayout(dict)) return out;
    char* obj = reinterpret_cast<char*>(dict);
    void* entries = *reinterpret_cast<void**>(obj + cloudLayout.entries);
    int count = *reinterpret_cast<int*>(obj + cloudLayout.count);
    const size_t cap = entries ? unity::ArrayLength(entries) : 0;
    if (count <= 0 || !cap) return out;
    count = std::min(count, static_cast<int>(std::min<size_t>(cap, 4096)));
    char* data = unity::ArrayData<char>(entries);
    for (int i = 0; i < count; ++i) {
        char* e = data + static_cast<size_t>(i) * cloudLayout.stride;
        if (*reinterpret_cast<int32_t*>(e + cloudLayout.hash) < 0) continue;  // free slot
        auto* type = *reinterpret_cast<Il2CppObject**>(e + cloudLayout.key);
        const int amount = *reinterpret_cast<int32_t*>(e + cloudLayout.value);
        if (type && amount > 0 && IsA(type, identTypeClass)) out.emplace_back(type, amount);
    }
    std::stable_sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    return out;
}

int Impl::AssignSlot(Il2CppObject* type, double now) {
    if (const int s = SlotOf(type); s >= 0) return s;
    for (int i = 0; i < kSlots; ++i)
        if (!slotType[i]) return i;
    int pick = -1;  // else the least recently wanted one, if idle for a few seconds
    for (int i = 0; i < kSlots; ++i)
        if (now - slotUsed[i] > 3.0 && (pick < 0 || slotUsed[i] < slotUsed[pick])) pick = i;
    return pick;
}

// One link's slot to `type` x `amount` (0 clears it). Our own slot calls: the cloud is not touched.
void Impl::SetSlot(Rec& rec, int slot, Il2CppObject* type, int amount) {
    if (!rec.ammo || slot < 0 || slot >= kSlots) return;
    bool ok = false;
    int32_t idx = slot;
    Il2CppObject* cur = Call(slotName, rec.ammo, {&idx}, "AmmoSlotManager.GetSlotName", &ok);
    int count = ok ? Value<int32_t>(Call(slotCount, rec.ammo, {&idx}, "AmmoSlotManager.GetSlotCount", &ok)) : 0;
    if (!ok) return;
    if (count > 0 && cur != type) {
        int32_t n = count;
        Call(ammoDecrement, rec.ammo, {&idx, &n}, "AmmoSlotManager.Decrement", &ok);
        count = 0;
    }
    const int target = type ? std::clamp(amount, 0, kSlotMax) : 0;
    if (target > count) {
        int32_t n = target - count;
        bool overflow = true;
        Call(addResource, rec.ammo, {type, &idx, &n, &overflow}, "AmmoSlotManager.MaybeAddResource", &ok);
    } else if (target < count) {
        int32_t n = count - target;
        Call(ammoDecrement, rec.ammo, {&idx, &n}, "AmmoSlotManager.Decrement", &ok);
    }
    rec.mirror[slot] = target;
}

void Impl::SyncSlot(int slot, Il2CppObject* cloud) {
    Il2CppObject* type = slotType[slot];
    const int amount = type && cloud ? CloudAmount(cloud, type) : 0;
    slotAmount[slot] = amount;
    for (auto& [id, rec] : recs) {
        SetSlot(rec, slot, type, amount);
        if (Faulted()) return;
    }
}

void Impl::RefreshMirrors(double now) {
    Il2CppObject* cloud = nullptr;
    bool cloudTried = false;
    for (int s = 0; s < kSlots; ++s) {
        if (!slotType[s]) continue;
        if (now - slotUsed[s] > kSlotIdleSeconds) {  // nobody wanted it for a while: free it
            slotType[s] = nullptr;
            for (auto& [id, rec] : recs) SetSlot(rec, s, nullptr, 0);
            continue;
        }
        if (!cloudTried) {
            cloudTried = true;
            cloud = Cloud();
        }
        const int amount = cloud ? CloudAmount(cloud, slotType[s]) : 0;
        bool stale = amount != slotAmount[s];
        for (auto& [id, rec] : recs) stale = stale || rec.mirror[s] != std::min(amount, kSlotMax);
        if (stale) SyncSlot(s, cloud);
        if (Faulted()) return;
    }
}

void Impl::OnTaken(Il2CppObject* storage, int slot, int32_t count) {
    if (slot < 0 || slot >= kSlots || count <= 0) return;
    Il2CppObject* type = slotType[slot];
    Il2CppObject* cloud = type ? Cloud() : nullptr;
    if (!cloud) return;
    const int amount = std::max(CloudAmount(cloud, type) - count, 0);
    int32_t v = amount;
    bool ok = false;
    Call(cloudSet, cloud, {type, &v}, "DroneCloudModel.SetAmount", &ok);
    slotAmount[slot] = amount;
    slotUsed[slot] = Now();
    const double now = slotUsed[slot];
    for (auto& [id, rec] : recs) {
        if (rec.storage == storage) {
            rec.mirror[slot] = std::max(rec.mirror[slot] - count, 0);
            if (now >= rec.nextTakeFx) {
                rec.nextTakeFx = now + kTakeFxSeconds;
                Pulse(rec, rec.catcher);
            }
        }
        if (rec.mirror[slot] != std::min(amount, kSlotMax)) SetSlot(rec, slot, type, amount);
    }
}

Il2CppObject* Impl::Sources(Il2CppObject* predicate, Il2CppObject* program) {
    std::vector<Il2CppObject*> out;
    const double now = Now();
    bool any = false;
    for (auto& [id, rec] : recs) any = any || (rec.catcher && rec.storage && unity::IsAlive(rec.storage));
    if (any) {
        Cached& c = cache[program];
        if (now - c.at > kCacheSeconds) {
            c.at = now;
            c.types.clear();
            if (predicate) {
                Il2CppClass* pk = api().il2cpp_object_get_class(predicate);
                if (pk != predInvokeClass) {
                    predInvoke = Resolver::Get().Method(pk, "Invoke", 1, nullptr);
                    predInvokeClass = pk;
                }
            }
            int asked = 0;
            for (const auto& [type, amount] : CloudEntries(Cloud())) {
                if (static_cast<int>(c.types.size()) >= kTypesPerProgram || ++asked > 96) break;
                bool ok = true;
                const bool want = !predicate || !predInvoke ||
                                  Value<bool>(Call(predInvoke, predicate, {type}, "Predicate.Invoke", &ok));
                if (ok && want) c.types.push_back(type);
                if (Faulted()) return nullptr;
            }
            for (auto it = cache.begin(); it != cache.end();)  // forget programs not asking any more
                it = now - it->second.at > 30.0 ? cache.erase(it) : std::next(it);
        }
        const std::vector<Il2CppObject*> types = cache.count(program) ? cache[program].types : std::vector<Il2CppObject*>{};
        Il2CppObject* cloud = nullptr;
        for (Il2CppObject* type : types) {
            int s = AssignSlot(type, now);
            if (s < 0) continue;
            slotUsed[s] = now;
            if (slotType[s] != type) {
                slotType[s] = type;
                if (!cloud) cloud = Cloud();
                SyncSlot(s, cloud);
                if (Faulted()) return nullptr;
            }
            for (auto& [id, rec] : recs)
                if (rec.sms[s] && rec.mirror[s] > 0 && unity::IsAlive(rec.storage)) out.push_back(rec.sms[s]);
        }
    }
    Il2CppObject* arr = arrayNew(storageMetaClass, out.size());
    if (!arr) return nullptr;
    for (size_t i = 0; i < out.size(); ++i) SetElem(arr, i, out[i]);
    return arr;
}

// ---------------------------------------------------------------- tick --

void Impl::Scan(double now) {
    Il2CppObject* gm = SceneField(offScGame);
    Il2CppObject* dict = gm ? unity::Field<Il2CppObject*>(gm, offByIdent) : nullptr;
    if (!dict) return;
    DictMethods& d = Dict(modelDict, dict);
    if (!d.tryGet) return;
    bool ok = false;
    Il2CppObject* list = nullptr;
    const bool found = Value<bool>(Call(d.tryGet, dict, {base, &list}, "identifiablesByIdent.TryGetValue", &ok));
    if (!ok) return;
    std::unordered_map<int64_t, Il2CppObject*> present;  // actor id -> its GameObject (nullptr: not loaded)
    for (size_t i = 0, n = found && list ? unity::ListCount(list) : 0; i < n && i < 1024; ++i) {
        auto* m = static_cast<Il2CppObject*>(unity::ListAt(list, i));
        if (!IsA(m, gadgetModelClass)) continue;
        const int64_t id = unity::Field<int64_t>(m, offActorId);
        Il2CppObject* tr = unity::Field<Il2CppObject*>(m, offGmTransform);
        present[id] = unity::IsAlive(tr) ? unity::GameObjectOf(tr) : nullptr;
    }
    ++stableTicks;
    std::set<int64_t> listed = Actors();
    const std::set<int64_t> before = listed;
    int kits = Kits();
    const int kitsBefore = kits;
    for (auto it = listed.begin(); it != listed.end();) {
        const int64_t id = *it;
        auto p = present.find(id);
        if (p != present.end()) {
            seenIds.insert(id);
            Il2CppObject* go = p->second;
            auto r = recs.find(id);
            if (!go) {  // not loaded: its objects went with the scene
                if (r != recs.end()) DropRec(id, false);
            } else if (r == recs.end() || r->second.root != go || !unity::IsAlive(r->second.holder)) {
                if (r != recs.end()) DropRec(id, false);
                Rec rec;
                if (Build(id, go, rec)) recs[id] = std::move(rec);
                else Undecorate(rec, true);
                if (Faulted()) return;
            }
            ++it;
            continue;
        }
        // Never seen since the load: stale only once the save's Refinery Links are clearly loaded (others
        // are there) for a few ticks, or after a long time (a slow load must not cost the links).
        const bool seen = seenIds.count(id) > 0;
        if (!seen && !(stableTicks >= kStaleTicks && !present.empty()) && stableTicks < kStaleTicksEmpty) {
            ++it;
            continue;
        }
        DropRec(id, false);
        ++kits;
        SR2_INFO("[{}] quantum link: actor {} {} - kit back ({} now)", module, id,
                 seen ? "was removed (picked up / stored)" : "is not in this save any more", kits);
        it = listed.erase(it);
    }
    for (auto it = recs.begin(); it != recs.end();) {  // records nobody lists any more
        if (listed.count(it->first)) {
            ++it;
            continue;
        }
        Undecorate(it->second, true);
        it = recs.erase(it);
    }
    if (listed != before) SetActors(listed);
    if (kits != kitsBefore) SetKits(kits);
    for (auto& [id, rec] : recs)  // the quantum pulse
        if (now >= rec.nextPulse) {
            rec.nextPulse = now + kPulseSeconds + static_cast<double>(id % 5) * 0.3;
            Pulse(rec, nullptr);
        }
}

std::string MethodBySuffixName(Il2CppClass* k, std::string_view suffix, uint32_t argc, const MethodInfo*& out) {
    const auto& a = api();
    out = nullptr;
    if (!k || !a.il2cpp_class_get_methods || !a.il2cpp_method_get_name || !a.il2cpp_method_get_param_count)
        return "method enumeration exports missing";
    void* iter = nullptr;
    while (const MethodInfo* m = a.il2cpp_class_get_methods(k, &iter)) {
        const char* n = a.il2cpp_method_get_name(m);
        if (!n || a.il2cpp_method_get_param_count(m) != argc) continue;
        const std::string_view name(n);
        if (name == suffix.substr(1) || (name.size() > suffix.size() && name.ends_with(suffix))) {
            out = m;
            return {};
        }
    }
    return std::format("method *{} not found", suffix);
}

}  // namespace

// ---------------------------------------------------------------- setup --

bool QuantumLink::Setup(sr2::core::ModuleContext& ctx, Host host, std::string& why, std::string& report) {
    Resolver& R = ctx.resolver();
    auto* p = new Impl();
    p->host = std::move(host);
    auto parentOf = [](Il2CppClass* c) { return api().il2cpp_class_get_parent ? api().il2cpp_class_get_parent(c) : nullptr; };
    auto cls = [&](const char* image, const char* ns, const char* name) -> Il2CppClass* {
        std::string err;
        Il2CppClass* k = R.Class(image, ns, name, &err);
        if (!k && why.empty()) why = err.empty() ? std::format("class {}.{} not found", ns, name) : err;
        return k;
    };
    auto off = [&](Il2CppClass* k, const char* field, size_t& out) {
        std::string err;
        for (Il2CppClass* c = k; c; c = parentOf(c))
            if (auto o = R.FieldOffset(c, field, &err)) {
                out = *o;
                return;
            }
        if (why.empty()) why = std::format("field {} not found", field);
    };
    auto method = [&](Il2CppClass* k, const char* name, int argc) -> const MethodInfo* {
        std::string err;
        for (Il2CppClass* c = k; c; c = parentOf(c))
            if (const MethodInfo* m = R.Method(c, name, argc, &err)) return m;
        if (why.empty()) why = std::format("method {}({}) not found", name, argc);
        return nullptr;
    };
    auto byParams = [&](Il2CppClass* k, const char* name, std::initializer_list<std::string_view> params) {
        std::string err;
        const MethodInfo* m = k ? R.MethodByParams(k, name, params, &err) : nullptr;
        if (!m && why.empty()) why = std::format("{}: {}", name, err);
        return m;
    };
    auto suffix = [&](Il2CppClass* k, std::string_view sfx, uint32_t argc) -> const MethodInfo* {
        const MethodInfo* m = nullptr;
        std::string err = MethodBySuffixName(k, sfx, argc, m);
        if (!m && why.empty()) why = err;
        return m;
    };
    auto fail = [&] {
        delete p;
        return false;
    };
    HMODULE ga = api().module;
    auto exp = [&](const char* n) { return ga ? GetProcAddress(ga, n) : nullptr; };
    p->objectNew = reinterpret_cast<Impl::ObjectNewFn>(exp("il2cpp_object_new"));
    p->arrayNew = reinterpret_cast<Impl::ArrayNewFn>(exp("il2cpp_array_new"));
    p->wbarrier = reinterpret_cast<Impl::WBarrierFn>(exp("il2cpp_gc_wbarrier_set_field"));
    p->fieldGetType = reinterpret_cast<Impl::FieldTypeFn>(exp("il2cpp_field_get_type"));
    p->elemClass = reinterpret_cast<Impl::ElemClassFn>(exp("il2cpp_class_get_element_class"));
    p->elemSize = reinterpret_cast<Impl::ElemSizeFn>(exp("il2cpp_class_array_element_size"));
    if (!p->objectNew || !p->arrayNew) why = "il2cpp_object_new / il2cpp_array_new export missing";

    const char* kFab = "MonomiPark.SlimeRancher.UI.Fabricator";
    p->gadgetDefClass = cls(kImage, "", "GadgetDefinition");
    p->identTypeClass = cls(kImage, "", "IdentifiableType");
    p->groupClass = cls(kImage, "", "IdentifiableTypeGroup");
    p->lsClass = cls("Unity.Localization", "UnityEngine.Localization", "LocalizedString");
    p->gadgetClass = cls(kImage, "MonomiPark.SlimeRancher.World", "Gadget");
    p->gadgetModelClass = cls(kImage, "MonomiPark.SlimeRancher.DataModel", "GadgetModel");
    p->siloStorageClass = cls(kImage, "", "SiloStorage");
    p->siloCatcherClass = cls(kImage, "", "SiloCatcher");
    p->slotDefClass = cls(kImage, "", "AmmoSlotDefinition");
    p->ammoModelClass = cls(kImage, "MonomiPark.SlimeRancher.DataModel", "AmmoModel");
    p->rendererClass = cls(kCore, "UnityEngine", "Renderer");
    p->gameObjectClass = cls(kCore, "UnityEngine", "GameObject");
    p->monoClass = cls(kCore, "UnityEngine", "MonoBehaviour");
    p->colliderClass = cls(kPhysics, "UnityEngine", "Collider");
    p->rigidbodyClass = cls(kPhysics, "UnityEngine", "Rigidbody");
    p->jointClass = cls(kPhysics, "UnityEngine", "Joint");
    p->fabItemClass = cls(kImage, kFab, "GadgetFabricatableItem");
    p->cloudCtrlClass = cls(kImage, "MonomiPark.SlimeRancher.Drone", "DroneStationCloudExtractionController");
    p->teleNodeClass = cls(kImage, "MonomiPark.SlimeRancher.World.Teleportation", "TeleporterNode");
    p->gadgetItemClass = cls(kImage, "MonomiPark.SlimeRancher.Player.PlayerItems", "GadgetItem");
    p->interactableClass = cls(kImage, "MonomiPark.SlimeRancher.UI", "BaseUIInteractable");
    Il2CppClass* uiActivator = cls(kImage, "MonomiPark.SlimeRancher.UI", "UIActivator");
    Il2CppClass* uiDisplay = cls(kImage, "MonomiPark.SlimeRancher.UI", "UIDisplayInteractable");
    p->uiDisplayIntClass = uiDisplay;
    Il2CppClass* fabCategory = cls(kImage, kFab, "GadgetFabricateCategory");
    Il2CppClass* fabRuntime = cls(kImage, kFab, "GadgetFabricateCategoryRuntime");
    Il2CppClass* helper = cls(kImage, kFab, "GadgetDirectorHelper");
    Il2CppClass* gadgetDir = cls(kImage, "", "GadgetDirector");
    Il2CppClass* gadgetsModel = cls(kImage, "MonomiPark.SlimeRancher.DataModel", "GadgetsModel");
    Il2CppClass* identModel = cls(kImage, "MonomiPark.SlimeRancher.DataModel", "IdentifiableModel");
    Il2CppClass* ammoMgr = cls(kImage, "MonomiPark.SlimeRancher.Player", "AmmoSlotManager");
    Il2CppClass* ranchMeta = cls(kImage, "", "RanchMetadata");
    Il2CppClass* cloudModel = cls(kImage, "MonomiPark.SlimeRancher.Drone.Cloud", "DroneCloudModel");
    Il2CppClass* droneDir = cls(kImage, "MonomiPark.SlimeRancher.Drone", "DroneDirector");
    Il2CppClass* sceneCtx = cls(kImage, "", "SceneContext");
    Il2CppClass* gameCtx = cls(kImage, "", "GameContext");
    Il2CppClass* autoSave = cls(kImage, "MonomiPark.SlimeRancher", "AutoSaveDirector");
    Il2CppClass* saveRef = cls(kImage, "MonomiPark.SlimeRancher", "SaveReferenceTranslation");
    Il2CppClass* gameModel = cls(kImage, "MonomiPark.SlimeRancher.DataModel", "GameModel");
    Il2CppClass* lookup = cls(kImage, "", "LookupDirector");
    Il2CppClass* spendable = cls(kImage, "MonomiPark.SlimeRancher.Player", "SpendableDirector");
    Il2CppClass* purchaseCost = cls(kImage, "MonomiPark.SlimeRancher.Economy", "PurchaseCost");
    Il2CppClass* costEntry = cls(kImage, "MonomiPark.SlimeRancher.Economy", "CurrencyCostEntry");
    Il2CppClass* fxHelpers = cls(kImage, "", "FXHelpers");
    Il2CppClass* material = cls(kCore, "UnityEngine", "Material");
    Il2CppClass* component = cls(kCore, "UnityEngine", "Component");
    Il2CppClass* transform = cls(kCore, "UnityEngine", "Transform");
    Il2CppClass* uobject = cls(kCore, "UnityEngine", "Object");
    if (!why.empty()) return fail();
    using NestedFn = Il2CppClass* (*)(Il2CppClass*, void**);
    auto nested = reinterpret_cast<NestedFn>(exp("il2cpp_class_get_nested_types"));
    void* iter = nullptr;
    for (Il2CppClass* k = nested ? nested(ranchMeta, &iter) : nullptr; k && api().il2cpp_class_get_name; k = nested(ranchMeta, &iter))
        if (std::string_view(api().il2cpp_class_get_name(k) ? api().il2cpp_class_get_name(k) : "") == "StorageMetadata")
            p->storageMetaClass = k;
    if (!p->storageMetaClass) {
        why = "RanchMetadata.StorageMetadata not found";
        return fail();
    }
    off(p->identTypeClass, "localizedName", p->offLocName);
    off(p->identTypeClass, "prefab", p->offPrefab);
    off(p->identTypeClass, "icon", p->offIcon);
    off(p->identTypeClass, "referenceId", p->offRefId);
    off(p->identTypeClass, "initializedHashId", p->offHashInit);
    off(p->gadgetDefClass, "_localizedDescription", p->offDesc);
    off(p->gadgetDefClass, "CraftingCosts", p->offCosts);
    off(p->gadgetDefClass, "VariantOf", p->offVariantOf);
    off(p->gadgetDefClass, "VariantGradiantFirstColor", p->offGrad1);
    off(p->gadgetDefClass, "VariantGradiantSecondColor", p->offGrad2);
    off(p->gadgetDefClass, "Type", p->offGType);
    off(p->gadgetDefClass, "LinkedDefinition", p->offLinked);
    off(purchaseCost, "_currencyCosts", p->offPcCurrency);
    off(purchaseCost, "IdentCosts", p->offPcIdent);
    off(costEntry, "_amount", p->offCostAmount);
    off(gadgetDir, "_gadgetVariantMap", p->offVariantMap);
    off(gadgetDir, "_model", p->offGdModel);
    off(gadgetsModel, "_favorites", p->offFavorites);
    off(p->gadgetItemClass, "_heldGadget", p->offHeld);
    off(p->gadgetItemClass, "_lastPlacedGadget", p->offLastPlaced);
    off(p->gadgetClass, "_model", p->offGadgetModel);
    off(p->gadgetModelClass, "transform", p->offGmTransform);
    off(identModel, "actorId", p->offActorId);
    off(p->siloStorageClass, "AmmoSlotDefinitions", p->offStorageDefs);
    off(p->slotDefClass, "_maxAmount", p->offSdMax);
    off(p->slotDefClass, "_slotTypeGroup", p->offSdGroup);
    off(p->slotDefClass, "_maxAmountProperty", p->offSdMaxProp);
    off(p->slotDefClass, "_isUnlockedProperty", p->offSdUnlockProp);
    off(p->storageMetaClass, "storage", p->offSmStorage);
    off(p->storageMetaClass, "index", p->offSmIndex);
    off(p->fabItemClass, "<Definition>k__BackingField", p->offFabDef);
    off(p->fabItemClass, "<Recipe>k__BackingField", p->offFabRecipe);
    off(fabCategory, "_gadgetPurchaseEvent", p->offCatEvent);
    off(fabRuntime, "_itemList", p->offRtItems);      // FabricateCategoryRuntime`1 fields (found through the parent)
    off(fabRuntime, "_categoryCount", p->offRtCount);
    off(cloudModel, "_amounts", p->offAmounts);
    off(p->cloudCtrlClass, "_spawnFX", p->offSpawnFx);
    off(p->teleNodeClass, "_activeFX", p->offActiveFx);
    off(p->teleNodeClass, "_arriveFX", p->offArriveFx);
    off(droneDir, "_droneStationType", p->offStationType);
    off(sceneCtx, "<GameModel>k__BackingField", p->offScGame);
    off(sceneCtx, "<GadgetDirector>k__BackingField", p->offScGadgetDir);
    off(sceneCtx, "<SpendableDirector>k__BackingField", p->offScSpendable);
    off(sceneCtx, "<DroneDirector>k__BackingField", p->offScDrones);
    off(gameCtx, "<AutoSaveDirector>k__BackingField", p->offGcAutoSave);
    off(autoSave, "_saveReferenceTranslation", p->offAsdTranslation);
    off(saveRef, "_identifiableTypeToPersistenceId", p->offSrtIdentTable);
    off(gameModel, "identifiablesByIdent", p->offByIdent);
    off(lookup, "_gadgetDefinitions", p->offGadgets);
    off(p->groupClass, "_memberTypes", p->offMembers);
    off(p->groupClass, "_memberGroups", p->offSubGroups);
    if (!why.empty()) return fail();
    for (size_t* o : {&p->offPcCurrency, &p->offPcIdent})  // value-type field offsets include the object header
        *o = *o >= 2 * sizeof(void*) ? *o - 2 * sizeof(void*) : *o;
    // offActorId: ActorId is a struct wrapping one long at the field's start (read as int64 like drone_source).

    p->getRefinery = method(lookup, "get_RefineryLink", 0);
    p->sceneInst = method(parentOf(sceneCtx), "get_Instance", 0);
    p->gameCtxInst = method(parentOf(gameCtx), "get_Instance", 0);
    p->getRefId = method(p->identTypeClass, "get_ReferenceId", 0);
    p->setHideFlags = method(uobject, "set_hideFlags", 1);
    p->objDestroy = byParams(uobject, "Destroy", {"UnityEngine.Object"});
    p->objDestroyImmediate = byParams(uobject, "DestroyImmediate", {"UnityEngine.Object"});
    p->lsCtor = R.Method(p->lsClass, ".ctor", 2, nullptr);
    p->lsGetTable = method(p->lsClass, "get_TableReference", 0);
    p->lsGetEntry = method(p->lsClass, "get_TableEntryReference", 0);
    p->goCtor = byParams(p->gameObjectClass, ".ctor", {"System.String"});
    p->goAddComponent = byParams(p->gameObjectClass, "AddComponent", {"System.Type"});
    p->compInChildren = byParams(component, "GetComponentInChildren", {"System.Type", "System.Boolean"});
    p->compsInChildren = byParams(component, "GetComponentsInChildren", {"System.Type", "System.Boolean"});
    p->trSetParent = byParams(transform, "SetParent", {"UnityEngine.Transform", "System.Boolean"});
    p->trSetLocalPos = method(transform, "set_localPosition", 1);
    p->trSetLocalRot = method(transform, "set_localRotation", 1);
    p->trSetLocalScale = method(transform, "set_localScale", 1);
    p->trGetPosition = method(transform, "get_position", 0);
    p->trChildCount = method(transform, "get_childCount", 0);
    p->trGetChild = method(transform, "GetChild", 1);
    p->initAmmo = method(p->siloStorageClass, "InitAmmo", 0);
    p->localAmmo = method(p->siloStorageClass, "get_LocalAmmo", 0);
    p->ammoModelCtor = R.Method(p->ammoModelClass, ".ctor", 1, nullptr);
    p->ammoSetModel = method(ammoMgr, "SetModel", 1);
    p->slotName = method(ammoMgr, "GetSlotName", 1);
    p->slotCount = method(ammoMgr, "GetSlotCount", 1);
    p->addResource = method(ammoMgr, "MaybeAddResource", 4);
    p->ammoDecrement = byParams(ammoMgr, "Decrement", {"System.Int32", "System.Int32"});
    p->smCtor = byParams(p->storageMetaClass, ".ctor", {"System.Int32", "SiloStorage", "SiloCatcher"});
    p->cloudGet = method(cloudModel, "GetAmount", 1);
    p->cloudSet = method(cloudModel, "SetAmount", 2);
    p->getCloudModel = method(droneDir, "GetCloudModel", 0);
    p->spawnFx = byParams(fxHelpers, "SpawnAndPlayFX", {"UnityEngine.GameObject", "UnityEngine.Vector3", "UnityEngine.Quaternion"});
    p->rendGetMaterials = method(p->rendererClass, "get_materials", 0);
    p->rendGetShared = method(p->rendererClass, "get_sharedMaterials", 0);
    p->rendSetShared = method(p->rendererClass, "set_sharedMaterials", 1);
    p->matPropNames = byParams(material, "GetPropertyNames", {"UnityEngine.MaterialPropertyType"});
    p->matGetColor = byParams(material, "GetColor", {"System.String"});
    p->matSetColor = byParams(material, "SetColor", {"System.String", "UnityEngine.Color"});
    p->matGetShader = method(material, "get_shader", 0);
    p->fabItemCtor = R.Method(p->fabItemClass, ".ctor", 4, nullptr);
    p->fabCheck = suffix(p->fabItemClass, ".CheckCanFabricate", 1);
    p->spend = byParams(spendable, "Spend", {"MonomiPark.SlimeRancher.Economy.PurchaseCost", "System.Int32"});
    p->addItem = byParams(gadgetDir, "AddItem", {"IdentifiableType", "System.Int32"});
    p->setFavorite = method(gadgetsModel, "SetFavorite", 2);
    p->setHeld = method(p->gadgetItemClass, "SetHeldGadget", 1);
    p->lsGetString = R.Method(p->lsClass, "GetLocalizedString", 0, nullptr);  // optional (type names in Use notices)
    {  // optional: Use opens the Quantum Cloud menu (else HUD notices)
        constexpr const char* kDisp = "MonomiPark.SlimeRancher.UI.Framework.Displays";
        constexpr const char* kDroneUi = "MonomiPark.SlimeRancher.UI.Drone";
        Il2CppClass* udm = R.Class(kImage, kDisp, "UIDisplayManager", nullptr);
        p->droneUiClass = R.Class(kImage, kDroneUi, "DroneUIDisplay", nullptr);
        Il2CppClass* menu = R.Class(kImage, kDroneUi, "DroneMenuUI", nullptr);
        Il2CppClass* tabSetup = R.Class(kImage, kDroneUi, "DroneTabSetup", nullptr);
        Il2CppClass* tabbed = R.Class(kImage, "MonomiPark.SlimeRancher.UI.Framework.CommonControls", "TabbedControl", nullptr);
        std::string sufErr;
        const MethodInfo* show = nullptr;
        if (p->droneUiClass && api().il2cpp_class_get_methods && api().il2cpp_method_get_name && api().il2cpp_method_get_param_count) {  // IDisplayShowDataHandler<IDroneTaskAssignable>.UpdateShowData (explicit)
            void* it = nullptr;
            while (const MethodInfo* m = api().il2cpp_class_get_methods(p->droneUiClass, &it)) {
                const std::string_view n = api().il2cpp_method_get_name(m) ? api().il2cpp_method_get_name(m) : "";
                if (api().il2cpp_method_get_param_count(m) == 1 && n.ends_with(".UpdateShowData") &&
                    n.find("IDroneTaskAssignable") != std::string_view::npos)
                    show = m;
            }
        }
        auto fo = [&](Il2CppClass* k, const char* f, size_t& out) {
            auto o = k ? R.FieldOffset(k, f) : std::nullopt;
            out = o ? *o : 0;
            return out != 0;
        };
        const bool fields = fo(uiDisplay, "_display", p->offUdiDisplay) && fo(p->droneUiClass, "_menuRoot", p->offMenuRoot) &&
                            fo(menu, "_tabs", p->offMenuTabs) && fo(menu, "_tabbedControl", p->offMenuTabbed) &&
                            fo(menu, "_cloudInventory", p->offMenuCloud) && fo(tabSetup, "Target", p->offTabTarget);
        p->udmInstance = udm ? R.Method(udm, "get_Instance", 0, nullptr) : nullptr;
        p->udmOpen = udm ? R.MethodByParams(udm, "OpenDisplay", {"MonomiPark.SlimeRancher.UI.Framework.Displays.UIDisplayHandle"}) : nullptr;
        p->tabGoTo = tabbed ? R.Method(tabbed, "GoToTab", 1, nullptr) : nullptr;
        p->droneShowData = fields && p->udmInstance && p->udmOpen ? show : nullptr;
        if (!p->droneShowData)
            SR2_WARN("[{}] quantum link: the Quantum Cloud menu can't be opened (DroneUIDisplay / UIDisplayManager members missing) - "
                     "Use shows HUD notices", p->module);
        // optional: the glow's local-scaled particle systems (ScaleParticles)
        p->particleClass = R.Class("UnityEngine.ParticleSystemModule", "UnityEngine", "ParticleSystem", nullptr);
        using ResolveIcallFn = void* (*)(const char*);
        auto resolveIcall = reinterpret_cast<ResolveIcallFn>(exp("il2cpp_resolve_icall"));
        p->psScalingMode = resolveIcall ? reinterpret_cast<Impl::ScalingModeFn>(
                                              resolveIcall("UnityEngine.ParticleSystem/MainModule::get_scalingMode()"))
                                        : nullptr;
        p->trGetLocalScale = R.Method(transform, "get_localScale", 0, nullptr);
    }
    if (!p->lsCtor || !p->ammoModelCtor || !p->fabItemCtor) why = "constructors not found";
    // hooks
    const MethodInfo* persistMi = byParams(saveRef, "GetPersistenceId", {"IdentifiableType"});
    const MethodInfo* hasBpMi = method(gadgetDir, "HasBlueprint", 1);
    const MethodInfo* helperBpMi = suffix(helper, ".HasBlueprint", 1);
    const MethodInfo* countMi = method(gadgetDir, "GetItemCount", 1);
    const MethodInfo* runtimeMi = R.Method(fabRuntime, ".ctor", 3, nullptr);
    const MethodInfo* placeholderMi = method(p->fabItemClass, "get_ShouldShowPlaceholder", 0);
    const MethodInfo* fabMi = suffix(p->fabItemClass, ".FabricateAndSpendCost", 1);
    const MethodInfo* placeMi = method(p->gadgetItemClass, "PlaceGadgetEvent", 1);
    const MethodInfo* pickupMi = method(p->gadgetItemClass, "PickupGadget", 1);
    const MethodInfo* variantMi = method(p->gadgetClass, "ChangeVariant", 2);
    const MethodInfo* decMi = method(p->storageMetaClass, "Decrement", 1);
    const MethodInfo* activatorMi = method(uiActivator, "OnInteract", 0);
    const MethodInfo* displayMi = method(uiDisplay, "OnInteract", 0);
    if (!runtimeMi && why.empty()) why = "GadgetFabricateCategoryRuntime..ctor(3) not found";
    p->hasBp = hasBpMi;
    if (!why.empty()) return fail();

    struct H {
        const char* name;
        const MethodInfo* mi;
        void* detour;
        void** orig;
    };
    const H hooks[] = {
        {"SaveReferenceTranslation.GetPersistenceId(IdentifiableType)", persistMi, reinterpret_cast<void*>(&PersistDetour),
         reinterpret_cast<void**>(&g_origPersist)},
        {"GadgetDirector.HasBlueprint", hasBpMi, reinterpret_cast<void*>(&HasBlueprintDetour), reinterpret_cast<void**>(&g_origHasBp)},
        {"GadgetDirectorHelper.HasBlueprint", helperBpMi, reinterpret_cast<void*>(&HelperBlueprintDetour),
         reinterpret_cast<void**>(&g_origHelperBp)},
        {"GadgetDirector.GetItemCount", countMi, reinterpret_cast<void*>(&ItemCountDetour), reinterpret_cast<void**>(&g_origCount)},
        {"GadgetFabricateCategoryRuntime..ctor", runtimeMi, reinterpret_cast<void*>(&RuntimeCtorDetour),
         reinterpret_cast<void**>(&g_origRuntime)},
        {"GadgetFabricatableItem.get_ShouldShowPlaceholder", placeholderMi, reinterpret_cast<void*>(&PlaceholderDetour),
         reinterpret_cast<void**>(&g_origPlaceholder)},
        {"GadgetFabricatableItem.FabricateAndSpendCost", fabMi, reinterpret_cast<void*>(&FabricateDetour),
         reinterpret_cast<void**>(&g_origFab)},
        {"GadgetItem.PlaceGadgetEvent", placeMi, reinterpret_cast<void*>(&PlaceDetour), reinterpret_cast<void**>(&g_origPlace)},
        {"GadgetItem.PickupGadget", pickupMi, reinterpret_cast<void*>(&PickupDetour), reinterpret_cast<void**>(&g_origPickup)},
        {"Gadget.ChangeVariant", variantMi, reinterpret_cast<void*>(&ChangeVariantDetour), reinterpret_cast<void**>(&g_origVariant)},
        {"StorageMetadata.Decrement", decMi, reinterpret_cast<void*>(&DecrementDetour), reinterpret_cast<void**>(&g_origDec)},
        {"UIActivator.OnInteract", activatorMi, reinterpret_cast<void*>(&InteractDetour<&g_origActivator>),
         reinterpret_cast<void**>(&g_origActivator)},
        {"UIDisplayInteractable.OnInteract", displayMi, reinterpret_cast<void*>(&InteractDetour<&g_origDisplay>),
         reinterpret_cast<void**>(&g_origDisplay)},
    };
    constexpr size_t kHooks = std::size(hooks);
    void* ptrs[kHooks] = {};
    for (size_t i = 0; i < kHooks; ++i) {
        std::string err;
        ptrs[i] = R.MethodPointer(hooks[i].mi, &err);
        if (!ptrs[i]) {
            why = std::format("{}: no native body ({})", hooks[i].name, err);
            return fail();
        }
    }
    g = p;  // the detours may run as soon as a hook exists; they act only once armed
    for (size_t i = 0; i < kHooks; ++i) {
        if (!ctx.HookAddress(hooks[i].name, ptrs[i], hooks[i].detour, hooks[i].orig)) {
            why = std::format("hook {} failed", hooks[i].name);
            return false;  // hooks made so far only pass through (not armed, no definition is ever built)
        }
        report += std::format(" {}@0x{:X}", hooks[i].name, Resolver::Rva(ptrs[i]));
    }
    p->armed = true;
    ready_ = true;
    // The gadget-mode label of an aimed-at placed link (a vanilla Refinery Link to the game).
    gadget_look::AddPlacedLabel([](Il2CppObject* gadget, Il2CppObject* def) -> Il2CppObject* {
        if (!g || !g->armed || !g->ours || def != g->base || g->Faulted() || !g->Listed(g->ActorOfGadget(gadget))) return nullptr;
        Il2CppObject* name = unity::Field<Il2CppObject*>(g->ours, g->offLocName);
        return name != unity::Field<Il2CppObject*>(g->base, g->offLocName) ? name : nullptr;
    });
    return true;
}

void QuantumLink::Frame() {
    if (!ready_ || !g || !main_thread::IsMainThread() || g->Faulted()) return;
    if (g->cloudReq) g_guard.Run([] { g->CloudFrame(); });
    if (g->recs.empty() || g->Faulted()) return;
    const double now = Now();
    for (auto& [id, rec] : g->recs) {
        if (!rec.glow || (!rec.glitching && now < rec.glitchAt)) continue;
        if (!unity::IsAlive(rec.glow)) {
            rec.glow = nullptr;
            continue;
        }
        g_guard.Run([&] { g->Glitch(rec, now); });
        if (g->Faulted()) return;
    }
}

// ---------------------------------------------------------------- api --

void QuantumLink::Tick() {
    if (!ready_ || !g || !main_thread::IsMainThread() || g->Faulted()) return;
    Impl& p = *g;
    if (!p.host.saveReady || !p.host.saveReady()) return;
    if (!p.EnsureDefinition() || p.Faulted()) return;
    p.Register();
    if (p.Faulted()) return;
    const double now = Now();
    p.Scan(now);
    if (p.Faulted()) return;
    p.RefreshMirrors(now);
}

void QuantumLink::Cleanup(const char* why) {
    if (!ready_ || !g || !main_thread::IsMainThread()) return;
    Impl& p = *g;
    const bool calls = !p.Faulted();
    int links = 0;
    for (auto& [id, rec] : p.recs) {
        p.Undecorate(rec, calls);
        ++links;
    }
    p.recs.clear();
    p.storages.clear();
    p.cache.clear();
    for (int i = 0; i < kSlots; ++i) p.slotType[i] = nullptr;
    if (!p.ours) return;
    int favs = 0;
    bool unregistered = false;
    if (Il2CppObject* gd = p.SceneField(p.offScGadgetDir); unity::IsAlive(gd)) {
        // Favourite slots holding our variant get the Refinery Link (they are saved; without our hooks
        // our definition has no persistence id - the save alias covers it, this is the second guard).
        Il2CppObject* model = unity::Field<Il2CppObject*>(gd, p.offGdModel);
        Il2CppObject* fav = model ? unity::Field<Il2CppObject*>(model, p.offFavorites) : nullptr;
        auto valuesOff = fav ? Resolver::Get().FieldOffset(api().il2cpp_object_get_class(fav), "_values") : std::nullopt;
        void* values = valuesOff ? unity::Field<void*>(fav, *valuesOff) : nullptr;
        for (size_t i = 0, n = values ? unity::ArrayLength(values) : 0; i < n && i < 16; ++i) {
            if (unity::ArrayAt(values, i) != p.ours) continue;
            int32_t idx = static_cast<int32_t>(i);
            bool ok = false;
            if (calls) p.Call(p.setFavorite, model, {&idx, p.base}, "GadgetsModel.SetFavorite", &ok);
            if (!ok) p.SetElem(static_cast<Il2CppObject*>(values), i, p.base);  // plain write (fault: no game calls)
            ++favs;
        }
        // Out of the variant map (nothing offers it any more; HasBlueprint is vanilla again anyway).
        Il2CppObject* map = unity::Field<Il2CppObject*>(gd, p.offVariantMap);
        if (map && calls) {
            Impl::DictMethods& d = p.Dict(p.variantDict, map);
            bool ok = false;
            Il2CppObject* list = nullptr;
            if (d.tryGet && Impl::Value<bool>(p.Call(d.tryGet, map, {p.base, &list}, "variant map TryGetValue", &ok)) && list) {
                Impl::ListMethods& l = p.List(p.variantList, list);
                if (l.remove) p.Call(l.remove, list, {p.ours}, "variant list Remove", &ok);
            }
            if (d.remove) p.Call(d.remove, map, {p.ours}, "variant map Remove", &ok);
            p.registeredDir = nullptr;
            unregistered = true;
        }
    }
    SR2_INFO("[{}] quantum link: {}: {} link(s) undecorated, {} favourite slot(s) back to the Refinery Link{}", p.module, why,
             links, favs, unregistered ? ", variant unregistered" : "");
}

void QuantumLink::Reset() {
    if (!g) return;
    Impl& p = *g;
    for (auto& [id, rec] : p.recs) Impl::FreeLater(std::move(rec.handles));  // the old game's objects go with its scene
    p.recs.clear();
    p.storages.clear();
    p.seenIds.clear();
    p.cache.clear();
    p.stableTicks = 0;
    ++p.generation;
    for (int i = 0; i < kSlots; ++i) {
        p.slotType[i] = nullptr;
        p.slotAmount[i] = 0;
    }
    p.registeredDir = nullptr;
}

bool Available() {
    if (!g || !g->armed || !main_thread::IsMainThread()) return false;
    for (const auto& [id, rec] : g->recs)
        if (rec.catcher && rec.storage && unity::IsAlive(rec.storage)) return true;
    return false;
}

bool Placed() {
    if (!g || !g->armed || !g->ours || !main_thread::IsMainThread() || g->Faulted()) return false;
    bool any = false;
    g_guard.Run([&] { any = !g->Actors().empty(); });
    return any;
}

Il2CppObject* Sources(Il2CppObject* predicate, Il2CppObject* program) {
    if (!g || !g->armed || !main_thread::IsMainThread() || g->Faulted()) return nullptr;
    return g->Sources(predicate, program);
}

bool IsOurs(Il2CppObject* def) { return def && def == g_ours.load(std::memory_order_acquire); }

Il2CppObject* SourceIcon() {
    if (!g || !g->base) return nullptr;
    if (g->squareIcon && unity::IsAlive(g->squareIcon)) return g->squareIcon;  // our purple copy, square
    if (g->icon && unity::IsAlive(g->icon)) return g->icon;  // our purple copy
    Il2CppObject* icon = unity::Field<Il2CppObject*>(g->base, g->offIcon);
    return unity::IsAlive(icon) ? icon : nullptr;
}

bool Describe(buildables::Info& out) {
    if (!g || !g->armed || !g->ours || !main_thread::IsMainThread() || g->Faulted()) return false;
    Impl& p = *g;
    out = {};
    out.id = buildables::kQuantumLink;
    out.name = kTitle;
    out.base = "Refinery Link";
    out.baseAsset = unity::GetName(p.base);
    out.hint = kUnlockHint;
    out.def = p.ours;
    out.baseDef = p.base;
    out.icon = SourceIcon();
    g_guard.Run([&] {
        out.unlock = p.Override();
        out.baseUnlocked = p.BaseBlueprint();
        out.unlocked = out.unlock == buildables::Unlock::kUnlocked ||
                       (out.unlock == buildables::Unlock::kFollowBase && out.baseUnlocked);
        out.owned = p.Kits();
    });
    return true;
}

bool Grant(int count, std::string& result) {
    if (!g || !g->armed || !main_thread::IsMainThread() || g->Faulted()) {
        result = "the Quantum Storage Link is not available";
        return false;
    }
    Impl& p = *g;
    if (!p.ours || !p.host.saveReady || !p.host.saveReady()) {
        result = "load a save first (the Quantum Storage Link is set up at the first tick of a loaded save)";
        return false;
    }
    count = std::clamp(count, 1, 999);
    bool ok = false;
    g_guard.Run([&] {
        Il2CppObject* gd = p.SceneField(p.offScGadgetDir);
        if (!unity::IsAlive(gd)) {
            result = "no GadgetDirector";
            return;
        }
        bool called = false;
        const int32_t added = Impl::Value<int32_t>(p.Call(p.addItem, gd, {p.base, &count}, "GadgetDirector.AddItem", &called));
        if (!called) {
            result = "GadgetDirector.AddItem failed (see the log)";
            return;
        }
        const int kits = p.Kits() + count;
        p.SetKits(kits);
        ok = true;
        result = std::format("{} Quantum Storage Link(s): {} Refinery Link(s) added, {} kit(s) now", count, added, kits);
    });
    return ok;
}

}  // namespace sr2::modules::custom_upgrades::quantum

namespace sr2::modules::custom_upgrades::variant_ext {
void Set(Extension* ext) { quantum::g_ext.store(ext, std::memory_order_release); }
Extension* Get() { return quantum::Ext(); }
}  // namespace sr2::modules::custom_upgrades::variant_ext
