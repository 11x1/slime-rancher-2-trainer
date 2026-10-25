// custom_upgrades: Slime DJ buildables (see dj_buildables.h). SR2 1.3.0, all lookups by name; RVAs for
// reference only.
//
// Prefab data (identifiabletypes bundle read with UnityPy, 1.3.0; local units):
//   * decorScientistDesk: "model" (BoxCollider + LODGroup; desk top at y ~1.22, x -1.1..1.1, z -0.5..0.5)
//     with lab clutter children scientistDesk_bubbleScroll, scientistDesk_glass (+_LOD01, bubbles and drip
//     particles under it), vials (+_LOD01), decoLabBeaker01, decoLabClipboard, decoLabWrenchMonkey03_0,
//     pens; a looping lab sound "SoundScientistDesk"; three point lights. AllowGadgetStackingOnTop 0.
//   * decorBoombox: a GadgetMusicPlayer (interact: plays a music track, Animator bool _gadgetActive,
//     _gadgetActiveFX), a bench "ranchHouseBench" and "model_boombox" at y 0.72 (Animator + LODGroup;
//     bbox_base_jnt/bbox_head_jnt/FX_MusicalNotes = note particles; skinned meshes). Faces +z (eyes at
//     z 0.31). AllowGadgetStackingOnTop 0, CanStackOnStackableGadgets 1.
//   * decorFlowerPillowSmall: "mesh" (LODGroup, 3 LODs), "COL" (4 capsule colliders) and
//     "SlimeAttachmentPoint" at y 1.30 (SphereCollider trigger + SlimeAttachPoint: base slimes, no largo /
//     tarr, detach cooldown 5 s; children LockJoint / LargoLockJoint rigidbodies and "Attractor", a
//     15x sphere trigger with the Attractor script that draws slimes in).
//   * toyNightLight (root scale 1.75): "model_nightlight" (mesh, a hinged handle, a point light).
//
// Disassembly (1.3.0) on top of quantum_link.cpp's findings:
//   * GadgetItem.ToggleVariant (0xA47AC0): holding a gadget cycles _heldGadgetVariants; aiming at a placed
//     one calls Gadget.ChangeVariant(_targetedGadgetVariants[(IndexOf(gadget.IdentTypeAsDefinition) + 1) %
//     n]). A placed DJ gadget is a vanilla model (index 0), so the toggle always asks for our FIRST
//     available variant; ChangeVariantDetour cycles our kinds instead.
//   * GadgetMusicPlayer.Update (0x7481F0) only stops its own track; nothing resets the Animator bool, so
//     we can set it while a Slime DJ song plays (static int GadgetMusicPlayer._gadgetActive = its hash).
//   * Stacking: the footprint ray accepts a hit gadget with AllowGadgetStackingOnTop (instance field)
//     when the placeholder has CanStackOnStackableGadgets (free_placement/NOTES.md). Placed speakers get
//     the flag set; the stacked relation is the vanilla GadgetModel._stackedGadgetsIds (vanilla ids).
//
// What we do (tick ~1 Hz, main thread):
//   * Definitions: Object.Instantiate of the vanilla definitions (found by prefab name in
//     LookupDirector._gadgetDefinitions / _toyDefinitions), own names / descriptions (text_override),
//     variant gradients, scaled crafting costs; registered in the live GadgetDirector._gadgetVariantMap
//     (base's list + an empty key of their own) and aliased in the save's _reverseIndex to the base's
//     persistence id, like the Quantum Storage Link.
//   * Fabricator cards after the vanilla card (booth after the Scientist Desk, speaker and radio after the
//     Boombox). Unlocked with the base's blueprint (the radio with the Boombox's).
//   * Dressing a listed, loaded actor: booth = clutter hidden, a boombox upright in the middle of the desk,
//     two small ones on the front corners, two big boomboxes on the floor beside it (emitters), a
//     bench with the flower pillow and the game's slime attachment point behind it (largos allowed), E on
//     the desk root; speaker = bench and boombox hidden, a stack of two boomboxes with one box collider, stacking allowed on top, an emitter; radio = the night light hidden,
//     a small boombox instead, upright and box-shaped, an emitter. Every clone is script-free except the
//     pillow's attachment point. The boomboxes dance and show their notes only while they play.
//   * Plain placed Boomboxes (not DJ Speakers) are left as they are but play the song too when switched
//     on: an emitter on their model_boombox, and the same dance (SyncPlain).
//
// Round 8 (user feedback on the placed look and the music):
//   * Prefab data (UnityPy, 1.3.0): decorBoombox root = Gadget + GadgetMusicPlayer + GadgetUIInteractable
//     (gadgetMusic "BoomboxMusic", timeBetweenActivations 3, _blockInteractInGadgetMode 1); "Collision" =
//     a sphere collider r 0.5 at y 1.08 (layer 27); the bench (layer 27) has a mesh + box collider.
//     decorScientistDesk: "model" box collider on layer 27; scientistDesk_misc01 (+_LOD01) = the tubes
//     between the flasks (were left over). toyNightLight: root sphere collider r 0.47 + a small box at the
//     bottom, Rigidbody 0.5 kg without constraints (the radio rolled like a ball). The pillow's
//     SlimeAttachPoint: _allowLargo 0, trigger sphere r 1.
//   * UIDetector.Update (0x6AD6B0): sphere cast, then GetComponents + GetComponentsInParent<IUIInteractable>
//     on every hit collider. GadgetUIInteractable.Awake (0x763FA0) takes GetComponentInParent<IGadgetInteractor>;
//     its prompt is _deactivateInteractionPrompt when IGadgetInteractor.ShowDeactivatePrompt() is true.
//   * GadgetMusicPlayer.OnInteract (0x747930): when it is GadgetDirector.CurrentGadgetMusicPlayer, it stops
//     (Dequeue, OnStopPlaying); otherwise it queues gadgetMusic on the MusicController (one global track, the
//     previous player stops), sets the animator bool and the notes FX; then the 3 s cooldown.
//     ShowDeactivatePrompt (0x748180) = "Current == this". Awake (0x747680) caches the MusicController,
//     GadgetDirector and GetComponentInParent<Gadget>, nothing else.
//   * SlimeAttachPoint.OnTriggerEnter (0x753480): a largo needs _allowLargo; FixedUpdate keeps _isJointActive /
//     _slime while a slime sits.
//   So: GadgetMusicPlayer.OnInteract / ShowDeactivatePrompt are hooked for the players of our records only
//   (vanilla Boomboxes nobody listed and every other gadget run the original). E switches a record on / off
//   (sidecar "dj.on"); on = the Slime DJ song from its emitters when a song is loaded (booth: and a slime sits
//   behind it), else the Boombox's own track as filler (the original OnInteract). The booth gets a
//   GadgetMusicPlayer + GadgetUIInteractable added to its root (fields copied from the Boombox prefab).

#include "modules/custom_upgrades/dj_buildables.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
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
#include "modules/custom_upgrades/gadget_look.h"
#include "modules/custom_upgrades/variant_ext.h"
#include "modules/slime_dj/api.h"
#include "ui/mod_icons.h"

namespace sr2::modules::custom_upgrades::dj {
namespace {

namespace unity = sr2::core::unity;
namespace main_thread = sr2::core::main_thread;
namespace sdj = sr2::modules::slime_dj;
using sr2::il2cpp::api;
using sr2::il2cpp::Resolver;

constexpr const char* kImage = "Assembly-CSharp";
constexpr const char* kCore = "UnityEngine.CoreModule";
constexpr const char* kPhysics = "UnityEngine.PhysicsModule";
constexpr const char* kHolderPrefix = "sr2dj#";
constexpr const char* kDeskPrefab = "decorScientistDesk";
constexpr const char* kBoomboxPrefab = "decorBoombox";
constexpr const char* kPillowPrefab = "decorFlowerPillowSmall";
constexpr const char* kToyPrefab = "toyNightLight";
constexpr const char* kRadiosKey = "dj.radios";
constexpr const char* kAwayKey = "dj.radios_away";  // radios that left the world (vacuumed up): the next Night Light near you is one
constexpr const char* kOwedKey = "dj.radios_owed";  // radios paid for but not spawned yet (spawned as soon as possible)
constexpr const char* kOnKey = "dj.on";  // actor ids of the booths / speakers / Boomboxes switched on with E
constexpr int kLayerGadgetCollision = 27;  // the Boombox's "Collision" / bench layer (also the desk's)
constexpr int kStaleTicks = 5;
constexpr int kStaleTicksEmpty = 60;
constexpr int kMaxRadioSpawn = 5;
// The speaker stack (BuildStack): base boombox scale, top boombox scale and height (at s = 1), the Boombox's bounds
// centre on its gadget root, the box collider around the stack; the radio is the same stack at kRadioScale (toy units).
constexpr float kStackBase = 1.3f, kStackTop = 0.85f, kStackTopY = 1.0f, kStackX = 0.18f, kStackZ = 0.03f;
struct StackBox {
    float x, y, z;
};
constexpr StackBox kStackBox{1.2f, 1.7f, 0.9f};
constexpr float kRadioScale = 0.26f;
constexpr float kRadioDamping = 0.6f;  // the radio's Rigidbody linear damping (at least)
// The booth (BuildBooth, DressPreview): desk top height, the middle boombox's scale and depth, the seat (bench) and its top.
constexpr float kDeskTop = 1.22f, kDeck = 0.45f, kDeckZ = 0.05f, kSeatTop = 0.72f;
constexpr StackBox kSeat{0.0f, 0.0f, -1.8f};

struct Vec3 {
    float x, y, z;
};
struct Quat {
    float x, y, z, w;
};
struct Color {
    float r, g, b, a;
};

Quat AxisY(float deg) {
    const float h = deg * 3.14159265f / 360.0f;
    return {0.0f, std::sin(h), 0.0f, std::cos(h)};
}

bool IsA(Il2CppObject* obj, Il2CppClass* k) {
    const auto& a = api();
    if (!obj || !k || !a.il2cpp_object_get_class) return false;
    Il2CppClass* c = a.il2cpp_object_get_class(obj);
    if (c == k) return true;
    return c && a.il2cpp_class_is_assignable_from && a.il2cpp_class_is_assignable_from(k, c);
}

// ---- families ----
enum Fam : int { kBooth = 0, kSpeaker = 1, kRadio = 2, kFamCount = 3 };

struct VariantSpec {
    const char* defName;
    const char* title;
    const char* desc;
};
constexpr VariantSpec kBoothSpec[] = {
    {"SlimeDJBooth", "Slime DJ Booth",
     "A DJ table with a deck, monitor speakers and two big speakers. Interact with it to switch it on: it plays your "
     "Slime DJ song while a slime (largos too) sits on the pillow behind it, the corral music box tune otherwise. To place "
     "one: fabricate it, hold a Scientist Desk in gadget mode, press the Variant key until it shows Slime DJ Booth, "
     "then place it. Needs the Scientist Desk blueprint."},
};
// Round 9 ("remove the tilted variants and correct the name"): one variant, the two-boombox stack.
constexpr VariantSpec kSpeakerSpec[] = {
    {"SlimeDJSpeaker", "DJ Speaker Stack",
     "Two boomboxes stacked into a DJ speaker. It plays your Slime DJ song while it is switched on (interact with it, "
     "like a Boombox). Stack decorations or more speaker stacks on top. To place one: fabricate it, hold a Boombox in "
     "gadget mode, press the Variant key until it shows DJ Speaker Stack, then place it. Needs the Boombox blueprint."},
};
constexpr VariantSpec kRadioSpec = {
    "SlimeDJRadio", "Slime Radio",
    "A little radio toy, a tiny speaker stack, that plays your Slime DJ song wherever it goes; slimes love to play "
    "with it. Fabricating one drops it right in front of you. Needs the Boombox blueprint."};

struct Rgb3 {
    float r, g, b;
};
struct Family {
    const char* label;
    const char* kitsKey;
    const char* actorsKey;
    const char* id;        // buildables.h id (sidecar override key)
    const char* baseName;  // the vanilla base's display name (unlock hint)
    const char* iconName;  // assets/icons/<name>.png: the user's drawing when embedded (custom-icons.md)
    Rgb3 tint;             // else the base's icon recoloured towards this
    Il2CppObject* base = nullptr;          // vanilla definition
    std::vector<Il2CppObject*> defs;       // our definitions (variants; radio: the card)
    Color c1{}, c2{};
};

// Pointer table for SaveAs / Owns (any thread): ours -> vanilla.
constexpr int kMaxDefs = 8;
std::atomic<Il2CppObject*> g_defs[kMaxDefs];
std::atomic<Il2CppObject*> g_bases[kMaxDefs];
std::atomic<int> g_defCount{0};

int TableIndex(Il2CppObject* def) {
    if (!def) return -1;
    const int n = g_defCount.load(std::memory_order_acquire);
    for (int i = 0; i < n; ++i)
        if (g_defs[i].load(std::memory_order_relaxed) == def) return i;
    return -1;
}

struct Impl : variant_ext::Extension {
    Host host;
    const char* module = "custom_upgrades";

    // classes
    Il2CppClass* gadgetDefClass = nullptr, *identTypeClass = nullptr, *groupClass = nullptr, *lsClass = nullptr;
    Il2CppClass* gadgetClass = nullptr, *gadgetModelClass = nullptr, *actorModelClass = nullptr, *gadgetItemClass = nullptr;
    Il2CppClass* fabItemClass = nullptr, *gameObjectClass = nullptr, *monoClass = nullptr, *colliderClass = nullptr;
    Il2CppClass* rigidbodyClass = nullptr, *jointClass = nullptr, *animatorClass = nullptr, *musicPlayerClass = nullptr;
    Il2CppClass* uiClass = nullptr, *attachClass = nullptr, *boxColliderClass = nullptr, *sphereColliderClass = nullptr;
    Il2CppClass* sectrSourceClass = nullptr, *vacuumableClass = nullptr, *physMatClass = nullptr;
    // offsets
    size_t offLocName = 0, offPrefab = 0, offIcon = 0, offRefId = 0, offHashInit = 0, offDesc = 0, offCosts = 0, offVariantOf = 0;
    size_t offGrad1 = 0, offGrad2 = 0, offLinked = 0, offPcCurrency = 0, offPcIdent = 0, offCostAmount = 0;
    size_t offVariantMap = 0, offGdModel = 0, offFavorites = 0, offTargeted = 0;
    size_t offHeld = 0, offLastPlaced = 0, offGadgetModel = 0, offGmTransform = 0, offAmTransform = 0, offActorId = 0;
    size_t offStackTop = 0, offFabDef = 0, offFabRecipe = 0, offCatEvent = 0, offMusicInstance = 0;
    size_t offScGame = 0, offScGadgetDir = 0, offScSpendable = 0, offScRegions = 0, offGcAutoSave = 0;
    size_t offAsdTranslation = 0, offSrtIdentTable = 0, offByIdent = 0, offGadgets = 0, offToys = 0;
    size_t offMembers = 0, offSubGroups = 0;
    size_t offGmpMusic = 0, offGmpCue = 0, offGmpCooldown = 0, offGmpNext = 0, offGdCurrentPlayer = 0;
    size_t offUiPrompt = 0, offUiInput = 0, offUiBlock = 0, offUiDeactivate = 0;
    size_t offApAllowLargo = 0, offApSlime = 0, offApJointActive = 0, offApSpin = 0;
    size_t offVacHeld = 0;
    size_t offSrcVolume = 0, offSrcPitch = 0, offSrcCue = 0, offSrcLoop = 0, offSrcPlayOnStart = 0, offSrcRestart = 0;
    // methods
    const MethodInfo* sceneInst = nullptr, *gameCtxInst = nullptr, *getRefId = nullptr, *setHideFlags = nullptr;
    const MethodInfo* objDestroy = nullptr, *objDestroyImmediate = nullptr;
    const MethodInfo* lsCtor = nullptr, *lsGetTable = nullptr, *lsGetEntry = nullptr;
    const MethodInfo* goCtor = nullptr, *compInChildren = nullptr, *compsInChildren = nullptr;
    const MethodInfo* trFind = nullptr, *trSetParent = nullptr, *trSetLocalPos = nullptr, *trSetLocalRot = nullptr;
    const MethodInfo* trSetLocalScale = nullptr, *trGetLocalRot = nullptr, *trGetLocalScale = nullptr;
    const MethodInfo* trGetPosition = nullptr, *trGetForward = nullptr, *trChildCount = nullptr, *trGetChild = nullptr;
    const MethodInfo* goActiveSelf = nullptr, *animSetBool = nullptr;
    const MethodInfo* fabItemCtor = nullptr, *fabCheck = nullptr, *spend = nullptr, *addItem = nullptr;
    const MethodInfo* setFavorite = nullptr, *setHeld = nullptr, *instantiateActor = nullptr, *currentGroup = nullptr;
    const MethodInfo* actorIdOf = nullptr, *hasBp = nullptr;
    const MethodInfo* goAddComponent = nullptr, *goSetLayer = nullptr, *colSetEnabled = nullptr;
    const MethodInfo* boxGetCenter = nullptr, *boxSetCenter = nullptr, *boxGetSize = nullptr, *boxSetSize = nullptr;
    const MethodInfo* sphereGetRadius = nullptr, *sphereSetRadius = nullptr, *rbGetConstraints = nullptr, *rbSetConstraints = nullptr;
    const MethodInfo* trGetEuler = nullptr, *trSetEuler = nullptr;
    const MethodInfo* rbGetDamping = nullptr, *rbSetDamping = nullptr, *colGetMat = nullptr, *colSetMat = nullptr;
    const MethodInfo* pmCtor = nullptr, *pmSetDyn = nullptr, *pmSetStatic = nullptr, *pmSetBounce = nullptr,
                      *pmSetFricCombine = nullptr, *pmSetBounceCombine = nullptr;
    Il2CppObject* radioMat = nullptr;  // our high-friction physics material for the radio's box (session object)
    const MethodInfo* onInteractMi = nullptr, *showDeactMi = nullptr, *getPlotPrefab = nullptr;
    FieldInfo* animHashField = nullptr;
    using ObjectNewFn = Il2CppObject* (*)(Il2CppClass*);
    using WBarrierFn = void (*)(Il2CppObject*, void**, void*);
    using StaticGetFn = void (*)(FieldInfo*, void*);
    ObjectNewFn objectNew = nullptr;
    WBarrierFn wbarrier = nullptr;
    StaticGetFn staticGet = nullptr;

    // session objects (pinned, never freed)
    Family fams[kFamCount] = {
        {"DJ Booth", "dj.booth_kits", "dj.booths", buildables::kDjBooth, "Scientist Desk", "dj_booth", {0.95f, 0.2f, 0.75f}},
        {"DJ Speaker Stack", "dj.speaker_kits", "dj.speakers", buildables::kDjSpeaker, "Boombox", "dj_speaker", {1.0f, 0.55f, 0.1f}},
        {"Slime Radio", nullptr, kRadiosKey, buildables::kSlimeRadio, "Boombox", "slime_radio", {0.15f, 0.8f, 1.0f}},
    };
    Il2CppObject* boomboxPrefab = nullptr, *pillowPrefab = nullptr, *toyDef = nullptr, *toyPrefab = nullptr;
    // The corral Music Box's own tune (round 9: "dj booth isnt playing standby music when no slime & activated (the
    // same one playing in ranch plots)"): the SECTR audio source under the corral prefab's MusicBox, copied onto
    // every booth. Looked up once a LookupDirector exists.
    Il2CppObject* standbySrc = nullptr;
    Il2CppClass* standbyClass = nullptr;
    const MethodInfo* standbyPlayMi = nullptr, *standbyStopMi = nullptr, *standbyIsPlayingMi = nullptr;
    bool standbyLooked = false;
    bool defsBuilt = false, defsFailed = false, loggedWait = false;
    int32_t animHash = 0;
    bool animHashRead = false;

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
    const void* registeredDir = nullptr;
    std::set<std::string> aliased;
    // Per-tick save alias check (Register): the key / base ReferenceId of each definition (constant once
    // built; used to cost a get_name + get_ReferenceId + format per definition every tick) and the
    // _reverseIndex offset of the table's class.
    std::unordered_map<Il2CppObject*, std::pair<std::string, std::string>> aliasKeys;
    const void* revClass = nullptr;
    size_t revOff = 0;
    bool loggedNoVariantEntry = false, loggedNoDefs = false;
    std::string cardState[kFamCount];
    bool loggedPreview[kFamCount] = {};  // last logged Fabricator card state per family (logged when it changes)

    // dressed actors
    // What a record plays (Apply): nothing, the Slime DJ song from its emitters, the Boombox's own track.
    enum Mode : int { kModeOff = 0, kModeSong = 1, kModeFiller = 2, kModeStandby = 3 };
    struct Rec {
        int fam = 0, kind = 0;
        int64_t id = 0;
        Il2CppObject* root = nullptr;
        Il2CppObject* holder = nullptr;
        std::vector<Il2CppObject*> hidden;     // vanilla GameObjects we switched off
        Il2CppObject* gadget = nullptr;        // speaker: its Gadget (stacking flag)
        bool stackWas = false;
        Il2CppObject* player = nullptr;        // the GadgetMusicPlayer E talks to (booth: the one we added)
        Il2CppObject* addedUi = nullptr;       // booth: the GadgetUIInteractable we added to the desk root
        bool addedPlayer = false;              // booth: `player` is ours (destroyed on undress)
        Il2CppObject* attach = nullptr;        // booth: the pillow's SlimeAttachPoint
        bool seated = false;                   // booth: a slime sat there at the last Apply
        float spinWas = 0.0f;                  // booth: the attach point's _rotationSpeed (the seated slime's "dance")
        int spin = -1;                         // booth: spin applied (-1 = none yet)
        Il2CppObject* standby = nullptr;       // booth: our copy of the corral music box's SECTR audio source
        bool standbyOn = false;                // booth: Play() was called on it
        Il2CppObject* rb = nullptr;            // radio: the toy's Rigidbody (rotation frozen while not held)
        int32_t rbWas = 0;
        float dampWas = -1.0f;                 // radio: its linear damping before ours (-1 = untouched)
        Il2CppObject* vac = nullptr;           // radio: the toy's Vacuumable (_held read every frame)
        int held = -1;                         // radio: hold state applied (-1 = none yet)
        Il2CppObject* matWas = nullptr;        // radio: the box collider's shared physics material before ours
        bool matSet = false;
        Il2CppObject* sphere = nullptr;        // radio: the toy's ball collider (off)
        Il2CppObject* box = nullptr;           // radio: the toy's box collider (made the radio's size)
        Vec3 boxCenterWas{}, boxSizeWas{};
        std::vector<Il2CppObject*> emitters;
        float vol = -1.0f;                     // emitter volume scale registered (-1 = none yet)
        std::vector<Il2CppObject*> animators;
        std::vector<Il2CppObject*> fx;
        int anim = -1;                         // dance state applied (-1 = none yet)
        bool on = false;                       // switched on with E (sidecar "dj.on")
        int mode = -1;                         // Mode applied (-1 = none yet)
        std::vector<Il2CppGCHandle> handles;
    };
    std::map<int64_t, Rec> recs;
    // Plain (vanilla) Boomboxes: not dressed, but each plays the Slime DJ song (an emitter on its
    // model_boombox) and dances with it. Kind -1, no holder.
    std::map<int64_t, Rec> plain;
    std::unordered_set<int64_t> seenIds;
    std::unordered_set<int64_t> lastToys;  // Night Light actor ids at the last scan (a new one may be a radio coming back)
    int stableTicks = 0;
    unsigned generation = 1;

    // pickup hand-over (BeforePickup -> AfterPickup)
    int pickupFam = -1, pickupKind = 0;
    ULONGLONG lastToggle = 0;  // E debounce

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
                sr2::core::MarkModuleFaulted(module, std::format("dj buildables: {}: {}", what, err));
            else SR2_WARN("[{}] dj buildables: {} failed: {}", module, what, err);
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
    static void FreeLater(std::vector<Il2CppGCHandle> handles) {
        if (!handles.empty()) main_thread::Post([handles] {
            for (Il2CppGCHandle h : handles) unity::FreeGCHandle(h);
        });
    }
    void Keep(Rec& rec, Il2CppObject* o) {
        if (Il2CppGCHandle h = o ? unity::NewGCHandle(o) : nullptr) rec.handles.push_back(h);
    }
    Il2CppObject* SceneContext() {
        bool ok = false;
        Il2CppObject* sc = Call(sceneInst, nullptr, {}, "SceneContext.Instance", &ok);
        return ok && unity::IsAlive(sc) ? sc : nullptr;
    }
    Il2CppObject* SceneField(size_t off) {
        Il2CppObject* sc = SceneContext();
        return sc ? unity::Field<Il2CppObject*>(sc, off) : nullptr;
    }
    void Destroy(Il2CppObject* obj) {
        if (unity::IsAlive(obj)) Call(objDestroy, nullptr, {obj}, "Object.Destroy");
    }
    void DestroyNow(Il2CppObject* obj) {
        if (unity::IsAlive(obj)) Call(objDestroyImmediate, nullptr, {obj}, "Object.DestroyImmediate");
    }
    Il2CppObject* Find(Il2CppObject* goOrComp, const char* path) {
        Il2CppObject* tr = unity::TransformOf(goOrComp);
        if (!tr) return nullptr;
        bool ok = false;
        Il2CppObject* r = Call(trFind, tr, {unity::NewString(path)}, "Transform.Find", &ok);
        return ok && unity::IsAlive(r) ? unity::GameObjectOf(r) : nullptr;
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
    bool ActiveSelf(Il2CppObject* go) {
        bool ok = false;
        return Value<bool>(Call(goActiveSelf, go, {}, "GameObject.get_activeSelf", &ok)) && ok;
    }
    void Place(Il2CppObject* goOrComp, Vec3 pos, Quat rot, Vec3 scale) {
        Il2CppObject* tr = unity::TransformOf(goOrComp);
        if (!tr) return;
        Call(trSetLocalPos, tr, {&pos}, "Transform.set_localPosition");
        Call(trSetLocalRot, tr, {&rot}, "Transform.set_localRotation");
        Call(trSetLocalScale, tr, {&scale}, "Transform.set_localScale");
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
    std::string RefId(Il2CppObject* t) {
        bool ok = false;
        Il2CppObject* r = Call(getRefId, t, {}, "IdentifiableType.get_ReferenceId", &ok);
        return ok ? unity::ToUtf8(reinterpret_cast<Il2CppString*>(r)) : std::string();
    }
    int64_t ActorOfGadget(Il2CppObject* gadget) {
        if (!IsA(gadget, gadgetClass) || !unity::IsAlive(gadget)) return 0;
        Il2CppObject* model = unity::Field<Il2CppObject*>(gadget, offGadgetModel);
        return IsA(model, gadgetModelClass) ? unity::Field<int64_t>(model, offActorId) : 0;
    }

    // ---- families / sidecar ----
    int FamOf(Il2CppObject* def, int* kind = nullptr) const {
        if (!def) return -1;
        for (int f = 0; f < kFamCount; ++f)
            for (size_t k = 0; k < fams[f].defs.size(); ++k)
                if (fams[f].defs[k] == def) {
                    if (kind) *kind = static_cast<int>(k);
                    return f;
                }
        return -1;
    }
    int Kits(int f) {
        if (!fams[f].kitsKey) return 0;
        try {
            const std::string v = host.value(fams[f].kitsKey);
            return v.empty() ? 0 : std::max(0, std::stoi(v));
        } catch (...) {
            return 0;
        }
    }
    void SetKits(int f, int n) {
        if (fams[f].kitsKey) host.setValue(fams[f].kitsKey, n > 0 ? std::to_string(n) : std::string());
    }
    // Sandbox override of family f's card (buildables.h), else the base's blueprint.
    buildables::Unlock Override(int f) { return buildables::ParseUnlock(host.value(buildables::UnlockKey(fams[f].id))); }
    bool CardUnlocked(int f, const std::function<bool(Il2CppObject*)>& vanilla) {
        switch (Override(f)) {
            case buildables::Unlock::kUnlocked: return true;
            case buildables::Unlock::kLocked: return false;
            default: return fams[f].base && vanilla(fams[f].base);
        }
    }
    // The vanilla GadgetDirector.HasBlueprint (through the quantum detour, which passes vanilla definitions on).
    bool BaseBlueprint(int f) {
        Il2CppObject* gd = SceneField(offScGadgetDir);
        bool ok = false;
        return unity::IsAlive(gd) && fams[f].base &&
               Value<bool>(Call(hasBp, gd, {fams[f].base}, "GadgetDirector.HasBlueprint", &ok)) && ok;
    }
    std::string Hint(int f) const { return std::format("Needs the {} blueprint.", fams[f].baseName); }
    std::map<int64_t, int> Actors(int f) {  // actor id -> kind
        std::map<int64_t, int> out;
        std::stringstream ss(host.value(fams[f].actorsKey));
        std::string part;
        while (std::getline(ss, part, ','))
            try {
                if (part.empty()) continue;
                const size_t c = part.find(':');
                const int kind = c == std::string::npos ? 0 : std::stoi(part.substr(c + 1));
                out[std::stoll(part.substr(0, c))] = std::clamp(kind, 0, std::max(0, static_cast<int>(fams[f].defs.size()) - 1));
            } catch (...) {
            }
        return out;
    }
    void SetActors(int f, const std::map<int64_t, int>& ids) {
        std::string s;
        for (const auto& [id, kind] : ids)
            s += (s.empty() ? "" : ",") + (f == kSpeaker ? std::format("{}:{}", id, kind) : std::to_string(id));
        host.setValue(fams[f].actorsKey, s);
    }
    int ListedFam(int64_t id, int* kind = nullptr) {
        if (!id) return -1;
        for (int f = 0; f < kFamCount; ++f) {
            auto ids = Actors(f);
            if (auto it = ids.find(id); it != ids.end()) {
                if (kind) *kind = it->second;
                return f;
            }
        }
        return -1;
    }
    void List(int f, int64_t id, int kind) {
        auto ids = Actors(f);
        ids[id] = kind;
        SetActors(f, ids);
    }
    void Unlist(int f, int64_t id) {
        auto ids = Actors(f);
        if (ids.erase(id)) SetActors(f, ids);
    }
    // The placed gadget the player aims at is one of family f's (the toggle-variant key cycles its kinds).
    bool TargetListed(int f) {
        Il2CppObject* gd = SceneField(offScGadgetDir);
        Il2CppObject* obs = unity::IsAlive(gd) ? unity::Field<Il2CppObject*>(gd, offTargeted) : nullptr;
        Il2CppObject* gadget = obs ? unity::Field<Il2CppObject*>(obs, 2 * sizeof(void*)) : nullptr;  // ObservableValue._value
        const int64_t id = ActorOfGadget(gadget);
        return id && Actors(f).count(id) > 0;
    }

    // ---- live generics ----
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
    ListMethods& Lst(ListMethods& l, Il2CppObject* list) {
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

    // ---- build ----
    void CollectDefs(Il2CppObject* group, std::unordered_map<std::string, Il2CppObject*>& byPrefab, Il2CppClass* k);
    Il2CppObject* MakeDef(Il2CppObject* base, const VariantSpec& spec, Il2CppObject* variantOf, Color c1, Color c2,
                          float curMul, float itemMul);
    bool EnsureDefinitions();
    bool EnsureStandby();
    void AddStandby(Rec& rec, Vec3 at);
    void StandbyPlay(Rec& rec, bool play);
    bool StandbyPlaying(Rec& rec);
    void Spin(Rec& rec, bool on);
    void Register();
    void ReadAnimHash();
    void Strip(Il2CppObject* root);
    Il2CppObject* NewHolder(Il2CppObject* root);
    Il2CppObject* CloneBoombox(Rec& rec, Il2CppObject* parent, Vec3 pos, Quat rot, float scale);
    Il2CppObject* NewChild(Il2CppObject* parent, const char* name);
    Il2CppObject* AddComponent(Il2CppObject* go, Il2CppClass* k, const char* what);
    void HideVanilla(Rec& rec, std::initializer_list<const char*> paths);
    void BuildBooth(Rec& rec, std::string& what);
    Il2CppObject* BuildStack(Rec& rec, Il2CppObject* parent, Vec3 at, float s, int* built);
    void BuildSpeaker(Rec& rec, std::string& what);
    void BuildRadio(Rec& rec, std::string& what);
    void RadioHeld(Rec& rec, bool held);
    Il2CppObject* RadioMaterial();
    void FrameRadios();
    void AddBoothInteraction(Rec& rec);
    bool Build(int64_t id, Il2CppObject* root, Rec& rec);
    void Undress(Rec& rec, bool restore);
    void DropRec(int64_t id, bool restore);
    void DressNow(int64_t id, Il2CppObject* gadget, int fam, int kind);
    // E / music state
    std::set<int64_t> OnIds();
    void SetOn(int64_t id, bool on);
    const char* Label(const Rec& rec) const { return rec.kind < 0 ? "Boombox" : fams[rec.fam].label; }
    sdj::EmitterKind KindOf(const Rec& rec) const {
        return rec.fam == kBooth ? sdj::EmitterKind::kBooth : rec.fam == kRadio ? sdj::EmitterKind::kRadio : sdj::EmitterKind::kSpeaker;
    }
    Rec* RecOfPlayer(Il2CppObject* player);
    bool Seated(const Rec& rec);
    bool VanillaPlaying(Il2CppObject* player);
    void ToggleVanilla(Il2CppObject* player);
    void SetVolume(Rec& rec, float vol);
    void Dance(Rec& rec, bool on);
    void Apply(Rec& rec);
    bool Interact(Il2CppObject* player);
    void Animate();
    void SyncPlain(const std::unordered_map<int64_t, Il2CppObject*>& present, const std::map<int64_t, int>& listed);
    void Scan();
    int SpawnRadios(int count, bool owed = false);
    int Count(const char* key);
    void SetCount(const char* key, int n) { host.setValue(key, n > 0 ? std::to_string(n) : std::string()); }
    void Adopt(const std::unordered_map<int64_t, Il2CppObject*>& present, std::map<int64_t, int>& listed);

    // ---- variant_ext::Extension ----
    Il2CppObject* SaveAs(Il2CppObject* type) const override {
        const int i = TableIndex(type);
        return i >= 0 ? g_bases[i].load(std::memory_order_relaxed) : nullptr;
    }
    bool Owns(Il2CppObject* def) const override { return TableIndex(def) >= 0; }
    bool HasBlueprint(Il2CppObject* def, const std::function<bool(Il2CppObject*)>& vanilla) override;
    bool HelperBlueprint(Il2CppObject* def, const std::function<bool(Il2CppObject*)>& vanilla) override;
    int32_t ItemCount(Il2CppObject* def, int32_t vanilla) override;
    void InsertCards(Il2CppObject* category, Il2CppObject* list, Il2CppObject* helper, Il2CppObject* factory) override;
    int32_t Fabricate(Il2CppObject* item, Il2CppObject* def, int32_t count) override;
    void BeforePlace(Il2CppObject* gadgetItem, Il2CppObject* held, variant_ext::PlaceCtx& ctx) override;
    void AfterPlace(Il2CppObject* gadgetItem, const variant_ext::PlaceCtx& ctx) override;
    bool BeforePickup(Il2CppObject* gadgetItem, Il2CppObject* gadget) override;
    void AfterPickup(Il2CppObject* gadgetItem) override;
    void ChangeVariant(Il2CppObject* gadget, Il2CppObject* variant) override;

    // ---- gadget mode (gadget_look.h): the ghost and the label ----
    void RegisterLooks();
    Il2CppObject* MakeIcon(int f);
    void DressPreview(Il2CppObject* def, Il2CppObject* placeholder);
    void SetLayerRecursive(Il2CppObject* goOrComp, int layer);
    Il2CppObject* Label(Il2CppObject* def);
    Il2CppObject* TargetLabel(Il2CppObject* gadget);
};

Impl* g = nullptr;
sr2::core::DetourGuard g_guard{"custom_upgrades", "dj buildables"};
using OnInteractFn = void (*)(Il2CppObject*, const MethodInfo*);
using ShowDeactFn = bool (*)(Il2CppObject*, const MethodInfo*);
OnInteractFn g_origInteract = nullptr;  // GadgetMusicPlayer.OnInteract (trampoline: never our detour)
ShowDeactFn g_origShowDeact = nullptr;  // GadgetMusicPlayer.ShowDeactivatePrompt

// ---------------------------------------------------------------- definitions --

void Impl::CollectDefs(Il2CppObject* root, std::unordered_map<std::string, Il2CppObject*>& byPrefab, Il2CppClass* k) {
    std::vector<Il2CppObject*> groups{root};
    for (size_t gi = 0; gi < groups.size() && gi < 64; ++gi) {
        Il2CppObject* grp = groups[gi];
        if (!IsA(grp, groupClass)) continue;
        void* members = unity::Field<void*>(grp, offMembers);
        for (size_t i = 0, n = unity::ListCount(members); i < n && i < 2000; ++i) {
            auto* t = static_cast<Il2CppObject*>(unity::ListAt(members, i));
            if (!IsA(t, k) || !unity::IsAlive(t)) continue;
            Il2CppObject* prefab = unity::Field<Il2CppObject*>(t, offPrefab);
            if (!unity::IsAlive(prefab)) continue;
            const std::string name = unity::GetName(prefab);
            if (!byPrefab.count(name)) byPrefab[name] = t;
        }
        void* subs = unity::Field<void*>(grp, offSubGroups);
        for (size_t i = 0, n = unity::ListCount(subs); i < n && i < 64; ++i)
            groups.push_back(static_cast<Il2CppObject*>(unity::ListAt(subs, i)));
    }
}

// A runtime clone of `base` (same prefab): own name, texts, gradient, cost scaled on its own deep copies.
Il2CppObject* Impl::MakeDef(Il2CppObject* base, const VariantSpec& spec, Il2CppObject* variantOf, Color c1, Color c2,
                            float curMul, float itemMul) {
    Il2CppObject* d = unity::Instantiate(base, nullptr);
    if (!IsA(d, gadgetDefClass) || !unity::IsAlive(d) || d == base || !unity::NewGCHandle(d)) return nullptr;
    int32_t flags = 32;  // HideFlags.DontUnloadUnusedAsset
    Call(setHideFlags, d, {&flags}, "Object.set_hideFlags");
    unity::SetName(d, spec.defName);
    unity::Field<Il2CppObject*>(d, offRefId) = nullptr;  // ReferenceId / StableHashedId: from the new name
    unity::Field<bool>(d, offHashInit) = false;
    SetRef(d, offVariantOf, variantOf);
    SetRef(d, offLinked, nullptr);
    unity::Field<Color>(d, offGrad1) = c1;
    unity::Field<Color>(d, offGrad2) = c2;
    if (Il2CppObject* n = NewLocalized(unity::Field<Il2CppObject*>(base, offLocName), spec.title)) SetRef(d, offLocName, n);
    if (Il2CppObject* n = NewLocalized(unity::Field<Il2CppObject*>(base, offDesc), spec.desc)) SetRef(d, offDesc, n);
    // cost
    char* mine = reinterpret_cast<char*>(d) + offCosts;
    char* theirs = reinterpret_cast<char*>(base) + offCosts;
    void* cur = *reinterpret_cast<void**>(mine + offPcCurrency);
    void* idents = *reinterpret_cast<void**>(mine + offPcIdent);
    std::string paid;
    if ((cur && cur == *reinterpret_cast<void**>(theirs + offPcCurrency)) ||
        (idents && idents == *reinterpret_cast<void**>(theirs + offPcIdent))) {
        paid = "the base's (lists shared)";
    } else {
        for (size_t i = 0, n = unity::ListCount(cur); i < n && i < 8; ++i)
            if (auto* e = static_cast<Il2CppObject*>(unity::ListAt(cur, i))) {
                int32_t& a = unity::Field<int32_t>(e, offCostAmount);
                a = std::max(static_cast<int32_t>(std::lround(a * curMul / 5.0f) * 5), 5);  // round to 5 like vanilla
                paid += std::format("{}{} currency", paid.empty() ? "" : ", ", a);
            }
        auto itemsOff = idents ? Resolver::Get().FieldOffset(unity::ClassOf(static_cast<Il2CppObject*>(idents)), "_items")
                               : std::nullopt;
        void* arr = itemsOff ? unity::Field<void*>(idents, *itemsOff) : nullptr;
        for (size_t i = 0, n = unity::ListCount(idents); arr && i < n && i < 16; ++i) {
            char* e = unity::ArrayData<char>(arr) + i * 16;  // IdentCostEntry {int Amount @0; IdentifiableType IdentType @8}
            int32_t& a = *reinterpret_cast<int32_t*>(e);
            a = std::max(static_cast<int32_t>(std::lround(a * itemMul)), 1);
            paid += std::format("{}{} {}", paid.empty() ? "" : ", ", a, unity::GetName(*reinterpret_cast<Il2CppObject**>(e + 8)));
        }
    }
    const std::string rid = RefId(d);
    if (rid.empty() || rid == RefId(base)) return nullptr;
    SR2_INFO("[{}] dj buildables: definition '{}' from '{}' (prefab '{}'), cost {}", module, rid, unity::GetName(base),
             unity::GetName(unity::Field<Il2CppObject*>(d, offPrefab)), paid.empty() ? "none" : paid);
    return d;
}

bool Impl::EnsureDefinitions() {
    if (defsBuilt) return true;
    if (defsFailed) return false;
    Il2CppObject* lookup = host.lookup ? host.lookup() : nullptr;
    if (!lookup) return false;
    std::unordered_map<std::string, Il2CppObject*> gadgets, toys;
    CollectDefs(unity::Field<Il2CppObject*>(lookup, offGadgets), gadgets, gadgetDefClass);
    CollectDefs(unity::Field<Il2CppObject*>(lookup, offToys), toys, identTypeClass);
    Il2CppObject* desk = gadgets.count(kDeskPrefab) ? gadgets[kDeskPrefab] : nullptr;
    Il2CppObject* boombox = gadgets.count(kBoomboxPrefab) ? gadgets[kBoomboxPrefab] : nullptr;
    Il2CppObject* pillow = gadgets.count(kPillowPrefab) ? gadgets[kPillowPrefab] : nullptr;
    toyDef = toys.count(kToyPrefab) ? toys[kToyPrefab] : nullptr;
    if (!desk || !boombox) {
        if (gadgets.empty()) return false;  // groups not loaded yet
        defsFailed = true;
        SR2_WARN("[{}] dj buildables: {} not found among {} gadget definitions - the DJ buildables are not offered", module,
                 !desk ? kDeskPrefab : kBoomboxPrefab, gadgets.size());
        return false;
    }
    for (Il2CppObject* o : {desk, boombox}) unity::NewGCHandle(o);
    boomboxPrefab = unity::Field<Il2CppObject*>(boombox, offPrefab);
    if (pillow) pillowPrefab = unity::Field<Il2CppObject*>(pillow, offPrefab);
    if (toyDef) toyPrefab = unity::Field<Il2CppObject*>(toyDef, offPrefab);
    for (Il2CppObject* o : {boomboxPrefab, pillowPrefab, toyDef, toyPrefab})
        if (o) unity::NewGCHandle(o);
    fams[kBooth].base = desk;
    fams[kBooth].c1 = {0.95f, 0.15f, 0.75f, 1.0f};
    fams[kBooth].c2 = {0.15f, 0.85f, 1.0f, 1.0f};
    fams[kSpeaker].base = boombox;
    fams[kSpeaker].c1 = {1.0f, 0.55f, 0.1f, 1.0f};
    fams[kSpeaker].c2 = {0.95f, 0.15f, 0.75f, 1.0f};
    fams[kRadio].base = boombox;
    auto add = [&](int f, Il2CppObject* d) {
        const int i = g_defCount.load();
        if (!d || i >= kMaxDefs) return false;
        fams[f].defs.push_back(d);
        g_defs[i].store(d, std::memory_order_relaxed);
        g_bases[i].store(fams[f].base, std::memory_order_relaxed);
        g_defCount.store(i + 1, std::memory_order_release);
        return true;
    };
    bool ok = add(kBooth, MakeDef(desk, kBoothSpec[0], desk, fams[kBooth].c1, fams[kBooth].c2, 2.5f, 2.0f));
    for (const VariantSpec& s : kSpeakerSpec)
        ok = ok && add(kSpeaker, MakeDef(boombox, s, boombox, fams[kSpeaker].c1, fams[kSpeaker].c2, 1.5f, 1.5f));
    if (toyDef && toyPrefab) add(kRadio, MakeDef(boombox, kRadioSpec, nullptr, {}, {}, 0.5f, 0.5f));
    else SR2_WARN("[{}] dj buildables: toy '{}' not found - no Slime Radio", module, kToyPrefab);
    if (!ok) {
        defsFailed = true;
        SR2_WARN("[{}] dj buildables: definitions not built - the DJ buildables are not offered", module);
        return false;
    }
    defsBuilt = true;
    RegisterLooks();
    SR2_INFO("[{}] dj buildables: DJ Booth on '{}', {} DJ Speaker variant(s) on '{}', Slime Radio {} (pillow {})", module,
             unity::GetName(desk), fams[kSpeaker].defs.size(), unity::GetName(boombox),
             fams[kRadio].defs.empty() ? std::string("off") : std::format("on toy '{}'", unity::GetName(toyDef)),
             pillowPrefab ? "found" : "not found");
    return true;
}

// Variants in the live GadgetDirector, ReferenceIds in the save's reverse index (as quantum_link.cpp).
void Impl::Register() {
    Il2CppObject* gd = SceneField(offScGadgetDir);
    Il2CppObject* map = unity::IsAlive(gd) ? unity::Field<Il2CppObject*>(gd, offVariantMap) : nullptr;
    if (map) {
        DictMethods& d = Dict(variantDict, map);
        bool ok = false;
        for (int f = 0; f < kFamCount; ++f) {
            if (fams[f].defs.empty() || !d.tryGet) continue;
            Il2CppObject* list = nullptr;
            const bool found = Value<bool>(Call(d.tryGet, map, {fams[f].base, &list}, "variant map TryGetValue", &ok));
            if (!ok || !found || !list) {
                if (!loggedNoVariantEntry) SR2_WARN("[{}] dj buildables: '{}' has no variant entry", module, unity::GetName(fams[f].base));
                loggedNoVariantEntry = true;
                continue;
            }
            ListMethods& l = Lst(variantList, list);
            for (Il2CppObject* def : fams[f].defs) {
                if (f != kRadio && !ListHas(list, def) && l.add) Call(l.add, list, {def}, "variant list Add", &ok);
                Il2CppObject* mine = nullptr;
                const bool have = Value<bool>(Call(d.tryGet, map, {def, &mine}, "variant map TryGetValue", &ok));
                if (ok && !have && l.ctor && objectNew && d.setItem) {
                    Il2CppObject* empty = objectNew(api().il2cpp_object_get_class(list));
                    if (empty) Call(l.ctor, empty, {}, "List..ctor", &ok);
                    if (empty && ok) Call(d.setItem, map, {def, empty}, "variant map set_Item", &ok);
                }
            }
        }
        if (gd != registeredDir) {
            registeredDir = gd;
            aliased.clear();
            SR2_INFO("[{}] dj buildables: registered as variants of the Scientist Desk / Boombox", module);
        }
    }
    bool ok = false;
    Il2CppObject* gc = Call(gameCtxInst, nullptr, {}, "GameContext.Instance", &ok);
    Il2CppObject* asd = ok && unity::IsAlive(gc) ? unity::Field<Il2CppObject*>(gc, offGcAutoSave) : nullptr;
    Il2CppObject* srt = unity::IsAlive(asd) ? unity::Field<Il2CppObject*>(asd, offAsdTranslation) : nullptr;
    Il2CppObject* table = srt ? unity::Field<Il2CppObject*>(srt, offSrtIdentTable) : nullptr;
    if (!table) return;
    if (Il2CppClass* tk = api().il2cpp_object_get_class(table); tk != revClass) {
        revClass = tk;
        auto rev = Resolver::Get().FieldOffset(tk, "_reverseIndex");
        revOff = rev ? *rev : 0;
    }
    Il2CppObject* dict = revOff ? unity::Field<Il2CppObject*>(table, revOff) : nullptr;
    if (!dict) return;
    DictMethods& d = Dict(aliasDict, dict);
    if (!d.containsKey || !d.tryGet || !d.setItem) return;
    for (int f = 0; f < kFamCount; ++f)
        for (Il2CppObject* def : fams[f].defs) {
            auto k = aliasKeys.find(def);
            if (k == aliasKeys.end()) {
                std::string base = RefId(fams[f].base);
                if (base.empty()) continue;
                k = aliasKeys.emplace(def, std::make_pair(std::format("GadgetDefinition.{}", unity::GetName(def)), std::move(base))).first;
            }
            const std::string& mine = k->second.first;
            Il2CppString* key = unity::NewString(mine);
            if (Value<bool>(Call(d.containsKey, dict, {key}, "save index ContainsKey", &ok)) || !ok) continue;
            int32_t id = -1;
            const bool found = Value<bool>(Call(d.tryGet, dict, {unity::NewString(k->second.second), &id}, "save index TryGetValue", &ok));
            if (!ok || !found || id < 0) continue;
            Call(d.setItem, dict, {key, &id}, "save index set_Item", &ok);
            if (ok && aliased.insert(mine).second)
                SR2_INFO("[{}] dj buildables: save alias {} -> persistence id {}", module, mine, id);
        }
}

// ---------------------------------------------------------------- extension --

bool Impl::HasBlueprint(Il2CppObject* def, const std::function<bool(Il2CppObject*)>& vanilla) {
    const int f = FamOf(def);
    if (f < 0 || !CardUnlocked(f, vanilla)) return false;
    if (f == kRadio) return true;
    return Kits(f) > 0 || TargetListed(f);
}

bool Impl::HelperBlueprint(Il2CppObject* def, const std::function<bool(Il2CppObject*)>& vanilla) {
    const int f = FamOf(def);
    return f >= 0 && CardUnlocked(f, vanilla);
}

int32_t Impl::ItemCount(Il2CppObject* def, int32_t vanilla) {
    const int f = FamOf(def);
    if (f < 0) return vanilla;
    if (f == kRadio) return static_cast<int32_t>(Actors(kRadio).size());
    return std::min(vanilla, Kits(f));
}

void Impl::InsertCards(Il2CppObject* category, Il2CppObject* list, Il2CppObject* helper, Il2CppObject* factory) {
    if (!defsBuilt) {
        if (!loggedNoDefs) SR2_INFO("[{}] dj buildables: Fabricator opened before the definitions were built (built at the "
                                    "first tick with a save loaded{}) - no DJ cards this time", module,
                                    defsFailed ? "; building them FAILED, see above" : "");
        loggedNoDefs = true;
        return;
    }
    // (family, card definition, the vanilla definition it follows)
    struct Card {
        int fam;
        Il2CppObject* def;
        Il2CppObject* after;
    };
    std::vector<Card> cards;
    if (!fams[kBooth].defs.empty()) cards.push_back({kBooth, fams[kBooth].defs[0], fams[kBooth].base});
    if (!fams[kSpeaker].defs.empty()) cards.push_back({kSpeaker, fams[kSpeaker].defs[0], fams[kSpeaker].base});
    if (!fams[kRadio].defs.empty() && !fams[kSpeaker].defs.empty())
        cards.push_back({kRadio, fams[kRadio].defs[0], fams[kSpeaker].defs[0]});
    for (const auto& [fam, def, after] : cards) {
        int32_t at = -1;
        bool present = false;
        for (size_t i = 0, n = unity::ListCount(list); i < n && i < 256; ++i) {
            auto* item = static_cast<Il2CppObject*>(unity::ListAt(list, i));
            if (!IsA(item, fabItemClass)) continue;
            Il2CppObject* d = unity::Field<Il2CppObject*>(item, offFabDef);
            if (d == def) present = true;
            if (d == after) at = static_cast<int32_t>(i) + 1;
        }
        if (present || at < 0) continue;  // not this category
        ListMethods& l = Lst(itemList, list);
        Il2CppObject* item = objectNew(fabItemClass);
        bool ok = false;
        if (item) Call(fabItemCtor, item, {helper, factory, unity::Field<Il2CppObject*>(category, offCatEvent), def},
                       "GadgetFabricatableItem..ctor", &ok);
        if (!item || !ok || !l.insert) continue;
        SetRef(item, offFabRecipe, nullptr);  // no pinned recipe: RecipeFactory would build the base's (quantum_link.cpp)
        Call(l.insert, list, {&at, item}, "Fabricator item list Insert", &ok);
        if (!ok) continue;
        // Why the card is locked / unlocked, logged when it changes.
        const buildables::Unlock o = Override(fam);
        const bool bp = BaseBlueprint(fam);
        const bool unlocked = o == buildables::Unlock::kUnlocked || (o == buildables::Unlock::kFollowBase && bp);
        const std::string why = o == buildables::Unlock::kUnlocked ? std::string("Sandbox override: unlocked")
                                : o == buildables::Unlock::kLocked ? std::string("Sandbox override: locked")
                                                                   : std::format("unlocked with the {} blueprint", fams[fam].baseName);
        const std::string state = std::format("{} ({}; {} blueprint {})", unlocked ? "unlocked" : "LOCKED", why,
                                              fams[fam].baseName, bp ? "owned" : "not owned");
        if (state != cardState[fam]) {
            cardState[fam] = state;
            const std::string owned = fam == kRadio ? std::format("{} radio(s) in the save", Actors(kRadio).size())
                                                    : std::format("{} kit(s)", Kits(fam));
            SR2_INFO("[{}] dj buildables: Fabricator card '{}' added after '{}' (position {}): {}, {}", module,
                     fams[fam].label, unity::GetName(after), at + 1, state, owned);
        }
    }
}

int32_t Impl::Fabricate(Il2CppObject* item, Il2CppObject* def, int32_t count) {
    const int f = FamOf(def);
    if (f < 0) return 7;
    bool ok = false;
    const int32_t reason = Value<int32_t>(Call(fabCheck, item, {&count}, "GadgetFabricatableItem.CheckCanFabricate", &ok), 7);
    if (!ok) return 7;
    if (reason != 0) return reason;
    Il2CppObject* sd = SceneField(offScSpendable);
    Il2CppObject* gd = SceneField(offScGadgetDir);
    if (!unity::IsAlive(sd) || !unity::IsAlive(gd)) return 7;
    if (f == kRadio) count = std::clamp(count, 1, kMaxRadioSpawn);
    void* cost = reinterpret_cast<char*>(def) + offCosts;  // PurchaseCost by pointer to our copy
    if (!Value<bool>(Call(spend, sd, {cost, &count}, "SpendableDirector.Spend", &ok)) || !ok) return 3;  // INSUFFICIENT_COST_ITEMS
    if (f == kRadio) {
        const int n = count;
        main_thread::Post([n] {  // outside the Fabricator's call: spawn and list the toys
            if (g) g_guard.Run([&] { g->SpawnRadios(n); });
        });
        return 0;
    }
    const int32_t added = Value<int32_t>(Call(addItem, gd, {fams[f].base, &count}, "GadgetDirector.AddItem", &ok));
    const int kits = Kits(f) + count;
    SetKits(f, kits);
    SR2_INFO("[{}] dj buildables: fabricated {} {}(s): {} '{}' added, {} kit(s) now", module, count, fams[f].label,
             ok ? added : 0, unity::GetName(fams[f].base), kits);
    return 0;
}

void Impl::BeforePlace(Il2CppObject* gadgetItem, Il2CppObject* held, variant_ext::PlaceCtx& ctx) {
    int kind = 0;
    const int f = FamOf(held, &kind);
    if (f < 0 || f == kRadio) return;
    if (Kits(f) <= 0) {
        SetRef(gadgetItem, offHeld, fams[f].base);  // no kit left: the plain decoration is placed
        SR2_INFO("[{}] dj buildables: no {} left to place - placing a plain '{}'", module, fams[f].label,
                 unity::GetName(fams[f].base));
        return;
    }
    ctx.ours = true;
    ctx.family = f;
    ctx.kind = kind;
    ctx.before = unity::Field<Il2CppObject*>(gadgetItem, offLastPlaced);
}

void Impl::AfterPlace(Il2CppObject* gadgetItem, const variant_ext::PlaceCtx& ctx) {
    Il2CppObject* last = unity::Field<Il2CppObject*>(gadgetItem, offLastPlaced);
    if (!last || last == ctx.before || !unity::IsAlive(last)) return;  // nothing placed (blocked, popup)
    const int64_t id = ActorOfGadget(last);
    if (!id || ctx.family < 0) return;
    const int kits = Kits(ctx.family);
    List(ctx.family, id, ctx.kind);
    SetKits(ctx.family, kits - 1);
    SR2_INFO("[{}] dj buildables: {} placed (actor {}, kind {}, saved as '{}'; {} kit(s) left)", module, fams[ctx.family].label,
             id, ctx.kind, unity::GetName(fams[ctx.family].base), std::max(kits - 1, 0));
    DressNow(id, last, ctx.family, ctx.kind);
}

// Dressed right away (placement / toggle), not at the next ~1 Hz tick: round 11 "on placement have to wait a
// second for it to be the correct model". Scan keeps the record (same root, kind and a live holder).
void Impl::DressNow(int64_t id, Il2CppObject* gadget, int fam, int kind) {
    Il2CppObject* go = unity::IsAlive(gadget) ? unity::GameObjectOf(gadget) : nullptr;
    if (!go || !defsBuilt) return;
    DropRec(id, true);
    Rec rec;
    rec.fam = fam;
    rec.kind = kind;
    if (Build(id, go, rec)) {
        recs[id] = std::move(rec);
        Apply(recs[id]);
    } else {
        Undress(rec, true);
    }
}

bool Impl::BeforePickup(Il2CppObject*, Il2CppObject* gadget) {
    pickupFam = -1;
    const int64_t id = ActorOfGadget(gadget);
    int kind = 0;
    const int f = ListedFam(id, &kind);
    if (f < 0 || f == kRadio) return false;
    Unlist(f, id);
    SetKits(f, Kits(f) + 1);
    SetOn(id, false);
    DropRec(id, false);  // its GameObject goes with the pickup
    pickupFam = f;
    pickupKind = kind;
    SR2_INFO("[{}] dj buildables: {} picked up (actor {}): kit back, held as the variant", module, fams[f].label, id);
    return true;
}

void Impl::AfterPickup(Il2CppObject* gadgetItem) {
    const int f = pickupFam;
    pickupFam = -1;
    if (f < 0 || pickupKind < 0 || pickupKind >= static_cast<int>(fams[f].defs.size())) return;
    if (unity::Field<Il2CppObject*>(gadgetItem, offHeld) == fams[f].base)
        Call(setHeld, gadgetItem, {fams[f].defs[pickupKind]}, "GadgetItem.SetHeldGadget");
}

// Toggle-variant on a placed one: plain -> kind 0 (a kit) -> 1 -> ... -> plain (kit back).
void Impl::ChangeVariant(Il2CppObject* gadget, Il2CppObject* variant) {
    const int f = FamOf(variant);
    const int64_t id = ActorOfGadget(gadget);
    if (f < 0 || f == kRadio || !id) return;
    int kind = 0;
    const int listedFam = ListedFam(id, &kind);
    const int kits = Kits(f);
    if (listedFam == f) {
        DropRec(id, true);  // the tick dresses it again with the new kind
        if (kind + 1 < static_cast<int>(fams[f].defs.size())) {
            List(f, id, kind + 1);
            SR2_INFO("[{}] dj buildables: actor {} is now '{}'", module, id, unity::GetName(fams[f].defs[kind + 1]));
            DressNow(id, gadget, f, kind + 1);
        } else {
            Unlist(f, id);
            SetKits(f, kits + 1);
            SR2_INFO("[{}] dj buildables: actor {} is a plain '{}' again (kit back, {} now)", module, id,
                     unity::GetName(fams[f].base), kits + 1);
        }
    } else if (listedFam < 0 && kits > 0) {
        List(f, id, 0);
        SetKits(f, kits - 1);
        SR2_INFO("[{}] dj buildables: '{}' (actor {}) turned into a {} ({} kit(s) left)", module, unity::GetName(fams[f].base), id,
                 fams[f].label, kits - 1);
        DressNow(id, gadget, f, 0);
    } else {
        SR2_INFO("[{}] dj buildables: no {} kit left - actor {} stays as it is", module, fams[f].label, id);
    }
}

// ---------------------------------------------------------------- dressing --

// GadgetMusicPlayer's static hash is set by its static constructor, i.e. once a Boombox existed: read
// again at every build until it is there.
void Impl::ReadAnimHash() {
    if (animHash || !animHashField || !staticGet) return;
    staticGet(animHashField, &animHash);
    if (animHash || !animHashRead)
        SR2_INFO("[{}] dj buildables: boombox dance {}", module,
                 animHash ? std::format("animator parameter hash {}", animHash) : std::string("not available yet (hash 0)"));
    animHashRead = true;
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

Il2CppObject* Impl::NewHolder(Il2CppObject* root) {
    Il2CppObject* rootTr = unity::TransformOf(root);
    bool ok = false;
    Il2CppObject* holder = objectNew(gameObjectClass);
    if (holder) Call(goCtor, holder, {unity::NewString(std::format("{}{}", kHolderPrefix, generation))}, "GameObject..ctor", &ok);
    if (!ok || !unity::IsAlive(holder)) return nullptr;
    unity::SetActive(holder, false);  // nothing wakes up before it is set up
    bool stays = false;
    Call(trSetParent, unity::TransformOf(holder), {rootTr, &stays}, "Transform.SetParent", &ok);
    Place(holder, {0, 0, 0}, {0, 0, 0, 1}, {1, 1, 1});
    return holder;
}

// A script-free copy of the Boombox's "model_boombox" (Animator kept: it dances while it plays).
Il2CppObject* Impl::CloneBoombox(Rec& rec, Il2CppObject* parent, Vec3 pos, Quat rot, float scale) {
    Il2CppObject* src = boomboxPrefab ? Find(boomboxPrefab, "model_boombox") : nullptr;
    if (!src) return nullptr;
    Il2CppObject* c = unity::Instantiate(src, unity::TransformOf(parent));
    if (!unity::IsAlive(c)) return nullptr;
    Strip(c);
    Place(c, pos, rot, {scale, scale, scale});
    unity::SetActive(c, true);
    if (Il2CppObject* a = InChildren(c, animatorClass)) {
        rec.animators.push_back(a);
        Keep(rec, a);
    }
    if (Il2CppObject* fx = Find(c, "bbox_base_jnt/bbox_head_jnt/FX_MusicalNotes")) {
        unity::SetActive(fx, false);
        rec.fx.push_back(fx);
        Keep(rec, fx);
    }
    Keep(rec, c);
    return c;
}

Il2CppObject* Impl::NewChild(Il2CppObject* parent, const char* name) {
    Il2CppObject* go = objectNew(gameObjectClass);
    bool ok = false;
    if (go) Call(goCtor, go, {unity::NewString(name)}, "GameObject..ctor", &ok);
    if (!ok || !unity::IsAlive(go)) return nullptr;
    bool stays = false;
    Call(trSetParent, unity::TransformOf(go), {unity::TransformOf(parent), &stays}, "Transform.SetParent", &ok);
    Place(go, {0, 0, 0}, {0, 0, 0, 1}, {1, 1, 1});
    return ok ? go : nullptr;
}

Il2CppObject* Impl::AddComponent(Il2CppObject* go, Il2CppClass* k, const char* what) {
    bool ok = false;
    Il2CppObject* c = go && k ? Call(goAddComponent, go, {unity::TypeOf(k)}, what, &ok) : nullptr;
    return ok && unity::IsAlive(c) ? c : nullptr;
}

void Impl::HideVanilla(Rec& rec, std::initializer_list<const char*> paths) {
    for (const char* p : paths) {
        Il2CppObject* go = Find(rec.root, p);
        if (!go || !ActiveSelf(go)) continue;
        unity::SetActive(go, false);
        rec.hidden.push_back(go);
        Keep(rec, go);
    }
}

// Gadget mode (gadget_look.h owns the hooks): our name while held (label), our description on the Fabricator card
// and the dressed placement ghost. Round 9: "custom buildables (mod ones) use the default label and the preview is
// also default". Called once the definitions are built (main thread).
void Impl::RegisterLooks() {
    if (!gadget_look::Ready()) {
        SR2_INFO("[{}] dj buildables: no gadget looks (gadget_look unavailable) - the vanilla label and preview show", module);
        return;
    }
    int n = 0;
    for (int f : {kBooth, kSpeaker, kRadio}) {
        Il2CppObject* icon = fams[f].defs.empty() ? nullptr : MakeIcon(f);
        for (Il2CppObject* def : fams[f].defs) {
            gadget_look::Look look;
            look.name = Label(def);
            Il2CppObject* desc = unity::Field<Il2CppObject*>(def, offDesc);
            look.description = desc != unity::Field<Il2CppObject*>(fams[f].base, offDesc) && IsA(desc, lsClass) ? desc : nullptr;
            if (f != kRadio) look.dressPreview = [this, def](Il2CppObject* placeholder) { DressPreview(def, placeholder); };
            if (icon) {
                SetRef(def, offIcon, icon);  // the grid tile (GadgetDefinition.get_Icon)
                look.icon = icon;            // the details picture
            }
            gadget_look::Register(def, std::move(look));
            ++n;
        }
    }
    SR2_INFO("[{}] dj buildables: gadget looks registered for {} definition(s) (label, card text, icon, dressed preview)", module, n);
}

// The Fabricator card icon (round 11: "fabricator icon is still the default"): the user's drawing
// (assets/icons/<iconName>.png) when embedded, else the base's vanilla icon recoloured in the family's
// colour (never generated art: custom-icons.md). nullptr: the base's icon stays.
Il2CppObject* Impl::MakeIcon(int f) {
    Il2CppObject* baseIcon = offIcon ? unity::Field<Il2CppObject*>(fams[f].base, offIcon) : nullptr;
    if (fams[f].iconName && sr2::ui::mod_icons::Has(fams[f].iconName))
        if (Il2CppObject* own = sr2::ui::mod_icons::Get(fams[f].iconName, unity::IsAlive(baseIcon) ? baseIcon : nullptr)) {
            SR2_INFO("[{}] dj buildables: {} card icon = the mod icon '{}'", module, fams[f].label, fams[f].iconName);
            return own;
        }
    std::string why;
    const Rgb3 t = fams[f].tint;
    Il2CppObject* tinted = unity::IsAlive(baseIcon) ? gadget_look::TintedSprite(baseIcon, {t.r, t.g, t.b}, 0.7f, why) : nullptr;
    if (tinted) SR2_INFO("[{}] dj buildables: {} card icon = the {}'s icon recoloured", module, fams[f].label, fams[f].baseName);
    else SR2_INFO("[{}] dj buildables: {} card icon = the {}'s ({})", module, fams[f].label, fams[f].baseName,
                  unity::IsAlive(baseIcon) ? why : std::string("no base icon"));
    return tinted;
}

// GameObject.layer of `goOrComp` and everything under it.
void Impl::SetLayerRecursive(Il2CppObject* goOrComp, int layer) {
    std::vector<Il2CppObject*> todo{unity::TransformOf(goOrComp)};
    for (size_t i = 0; i < todo.size() && i < 1024; ++i) {
        if (!todo[i] || !unity::IsAlive(todo[i])) continue;
        int32_t l = layer;
        Call(goSetLayer, unity::GameObjectOf(todo[i]), {&l}, "GameObject.set_layer");
        for (Il2CppObject* c : ChildTransforms(todo[i])) todo.push_back(c);
    }
}

// The placement ghost: the dressed look without scripts, colliders, emitters or interaction, on the gadget overlay
// layer (26, GadgetItem.SetGadgetLayerRecursively). "change the preview to match the stacked speakers".
void Impl::DressPreview(Il2CppObject* def, Il2CppObject* placeholder) {
    int kind = 0;
    const int f = FamOf(def, &kind);
    if (f != kBooth && f != kSpeaker) return;
    Rec tmp;
    tmp.root = placeholder;
    Il2CppObject* holder = NewChild(placeholder, "sr2look");
    if (!holder) return;
    int parts = 0;
    if (f == kSpeaker) {
        HideVanilla(tmp, {"ranchHouseBench", "model_boombox", "Collision"});
        BuildStack(tmp, holder, {kStackX, 0.0f, kStackZ}, 1.0f, &parts);
    } else {
        HideVanilla(tmp, {"model/scientistDesk_bubbleScroll", "model/scientistDesk_glass", "model/scientistDesk_glass_LOD01",
                          "model/scientistDesk_misc01", "model/scientistDesk_misc01_LOD01", "model/vials", "model/vials_LOD01",
                          "model/decoLabBeaker01", "model/decoLabClipboard", "model/decoLabWrenchMonkey03_0", "model/pens"});
        for (float side : {-1.0f, 1.0f}) {
            parts += CloneBoombox(tmp, holder, {side * 1.75f, 0.0f, 0.1f}, AxisY(-side * 12.0f), 1.35f) ? 1 : 0;
            parts += CloneBoombox(tmp, holder, {side * 0.8f, kDeskTop, 0.3f}, AxisY(-side * 12.0f), 0.4f) ? 1 : 0;
        }
        parts += CloneBoombox(tmp, holder, {0.0f, kDeskTop, kDeckZ}, {0, 0, 0, 1}, kDeck) ? 1 : 0;
        if (Il2CppObject* src = boomboxPrefab ? Find(boomboxPrefab, "ranchHouseBench") : nullptr) {
            Il2CppObject* bench = unity::Instantiate(src, unity::TransformOf(holder));
            if (unity::IsAlive(bench)) {
                Strip(bench);
                Place(bench, {kSeat.x, kSeat.y, kSeat.z}, {0, 0, 0, 1}, {0.46f, 0.66f, 0.66f});
                unity::SetActive(bench, true);
                ++parts;
            }
        }
        if (Il2CppObject* src = pillowPrefab ? Find(pillowPrefab, "mesh") : nullptr) {
            Il2CppObject* pillow = unity::Instantiate(src, unity::TransformOf(holder));
            if (unity::IsAlive(pillow)) {
                Strip(pillow);
                Place(pillow, {kSeat.x, kSeatTop, kSeat.z}, {0, 0, 0, 1}, {1, 1, 1});
                unity::SetActive(pillow, true);
                ++parts;
            }
        }
    }
    SetLayerRecursive(holder, 26);
    if (!loggedPreview[f]) SR2_INFO("[{}] dj buildables: {} placement preview dressed ({} part(s))", module, fams[f].label, parts);
    loggedPreview[f] = true;
    FreeLater(std::move(tmp.handles));  // the objects live and die with the placeholder
    if (parts == 0) SR2_WARN("[{}] dj buildables: {} preview: nothing could be built", module, fams[f].label);
}

Il2CppObject* Impl::Label(Il2CppObject* def) {
    const int f = FamOf(def);
    Il2CppObject* name = f >= 0 && f != kRadio ? unity::Field<Il2CppObject*>(def, offLocName) : nullptr;
    return IsA(name, lsClass) ? name : nullptr;
}

Il2CppObject* Impl::TargetLabel(Il2CppObject* gadget) {
    int kind = 0;
    const int f = ListedFam(ActorOfGadget(gadget), &kind);
    if (f < 0 || f == kRadio || kind < 0 || kind >= static_cast<int>(fams[f].defs.size())) return nullptr;
    return Label(fams[f].defs[kind]);
}

// The booth faces +z (the big speakers look there); the DJ sits behind it (-z).
void Impl::BuildBooth(Rec& rec, std::string& what) {
    Il2CppObject* holder = rec.holder;
    HideVanilla(rec, {"model/scientistDesk_bubbleScroll", "model/scientistDesk_glass", "model/scientistDesk_glass_LOD01",
                      "model/scientistDesk_misc01", "model/scientistDesk_misc01_LOD01", "model/vials", "model/vials_LOD01",
                      "model/decoLabBeaker01", "model/decoLabClipboard", "model/decoLabWrenchMonkey03_0", "model/pens",
                      "Point Light (1)", "SoundScientistDesk"});
    constexpr float kTop = kDeskTop;
    // Big speakers on the floor beside the desk; they carry the song.
    for (float side : {-1.0f, 1.0f})
        if (Il2CppObject* spk = CloneBoombox(rec, holder, {side * 1.75f, 0.0f, 0.1f}, AxisY(-side * 12.0f), 1.35f))
            rec.emitters.push_back(spk);
    // Small monitor speakers on the front corners of the desk, facing the same way.
    int parts = 0;
    for (float side : {-1.0f, 1.0f})
        parts += CloneBoombox(rec, holder, {side * 0.8f, kTop, 0.3f}, AxisY(-side * 12.0f), 0.4f) ? 1 : 0;
    // The middle speaker: upright on the desk, facing the audience like the others. It used to lie face up as
    // a "deck" (round 11: "the middle small speaker is looking up").
    parts += CloneBoombox(rec, holder, {0.0f, kTop, kDeckZ}, {0, 0, 0, 1}, kDeck) ? 1 : 0;
    // The DJ's seat: the Boombox's bench (its colliders kept) with the Flower Pillow on top - mesh,
    // colliders and the game's slime attachment point with its scripts - so the slime sits high enough to
    // be seen over the desk. Largos allowed (larger trigger), far enough back not to clip into the desk.
    if (Il2CppObject* src = boomboxPrefab ? Find(boomboxPrefab, "ranchHouseBench") : nullptr) {
        Il2CppObject* bench = unity::Instantiate(src, unity::TransformOf(holder));
        if (unity::IsAlive(bench)) {
            Place(bench, {kSeat.x, kSeat.y, kSeat.z}, {0, 0, 0, 1}, {0.46f, 0.66f, 0.66f});
            unity::SetActive(bench, true);
            Keep(rec, bench);
            ++parts;
        }
    }
    int spot = 0;
    if (pillowPrefab) {
        for (const char* part : {"mesh", "COL", "SlimeAttachmentPoint"}) {
            Il2CppObject* src = Find(pillowPrefab, part);
            Il2CppObject* c = src ? unity::Instantiate(src, unity::TransformOf(holder)) : nullptr;
            if (!unity::IsAlive(c)) continue;
            const std::string_view p(part);
            if (p == "mesh") Strip(c);  // the colliders and the attachment point keep theirs
            const float y = p == "SlimeAttachmentPoint" ? 1.30f : p == "COL" ? 0.05f : 0.0f;
            Place(c, {kSeat.x, kSeatTop + y, kSeat.z}, {0, 0, 0, 1}, {1, 1, 1});
            if (p == "SlimeAttachmentPoint") {
                if (Il2CppObject* ap = unity::GetComponent(c, attachClass)) {
                    unity::Field<bool>(ap, offApAllowLargo) = true;  // still inactive: set before its Awake
                    rec.attach = ap;
                    rec.spinWas = offApSpin ? unity::Field<float>(ap, offApSpin) : 0.0f;
                    Keep(rec, ap);
                }
                if (Il2CppObject* sc = unity::GetComponent(c, sphereColliderClass)) {
                    bool ok = false;
                    const float r = Value<float>(Call(sphereGetRadius, sc, {}, "SphereCollider.get_radius", &ok), 1.0f);
                    float big = (ok && r > 0.0f ? r : 1.0f) * 1.5f;
                    Call(sphereSetRadius, sc, {&big}, "SphereCollider.set_radius");
                }
            }
            unity::SetActive(c, true);
            Keep(rec, c);
            ++spot;
        }
    }
    AddBoothInteraction(rec);
    AddStandby(rec, {0.0f, kTop + 0.3f, 0.0f});
    what = std::format("{} part(s) hidden, 2 floor speakers, {} desk / seat part(s), slime seat {}{} (spin {:.0f}), interact {}, "
                       "standby tune {}", rec.hidden.size(), parts, spot == 3 ? "ready" : "missing",
                       rec.attach ? " (largos allowed)" : "", rec.spinWas, rec.addedUi ? "on" : "NOT available",
                       rec.standby ? "ready" : "NOT available (the Boombox's track instead)");
}

// The corral Music Box's tune: LookupDirector.GetPlotPrefab(CORRAL) -> "Music Box/techCorralMusicBox/MusicBox" ->
// its SECTR_AudioSource (slime_dj mutes the same source under live music boxes). Its concrete class's Play / Stop /
// IsPlaying are resolved on that class (the base methods are abstract).
bool Impl::EnsureStandby() {
    if (standbySrc && unity::IsAlive(standbySrc)) return true;
    if (standbyLooked || !sectrSourceClass || !getPlotPrefab) return false;
    Il2CppObject* lookup = host.lookup ? host.lookup() : nullptr;
    if (!lookup) return false;  // again at the next dressing
    standbyLooked = true;
    int32_t corralId = 2;  // LandPlot.Id.CORRAL
    bool ok = false;
    Il2CppObject* corral = Call(getPlotPrefab, lookup, {&corralId}, "LookupDirector.GetPlotPrefab", &ok);
    Il2CppObject* box = ok && unity::IsAlive(corral) ? Find(corral, "Music Box/techCorralMusicBox/MusicBox") : nullptr;
    Il2CppObject* src = box ? InChildren(box, sectrSourceClass) : nullptr;
    Il2CppClass* k = src ? api().il2cpp_object_get_class(src) : nullptr;
    auto up = [&](const char* name, int argc) -> const MethodInfo* {
        for (Il2CppClass* c = k; c; c = api().il2cpp_class_get_parent ? api().il2cpp_class_get_parent(c) : nullptr)
            if (const MethodInfo* m = Resolver::Get().Method(c, name, argc, nullptr)) return m;
        return nullptr;
    };
    standbyPlayMi = k ? up("Play", 0) : nullptr;
    standbyStopMi = k ? up("Stop", 1) : nullptr;
    standbyIsPlayingMi = k ? up("get_IsPlaying", 0) : nullptr;
    if (!src || !standbyPlayMi || !standbyStopMi || !unity::IsAlive(unity::Field<Il2CppObject*>(src, offSrcCue))) {
        SR2_WARN("[{}] dj buildables: the corral music box tune was not found ({}) - a switched-on DJ Booth without a slime "
                 "plays the Boombox's track instead", module,
                 !corral ? "no corral prefab" : !box ? "no MusicBox in the corral prefab" : !src ? "no SECTR audio source under it"
                 : !standbyPlayMi || !standbyStopMi ? "Play / Stop not found" : "the source has no cue");
        return false;
    }
    unity::NewGCHandle(src);
    standbySrc = src;
    standbyClass = k;
    SR2_INFO("[{}] dj buildables: standby tune = the corral music box's '{}' ({} on '{}')", module,
             unity::GetName(unity::Field<Il2CppObject*>(src, offSrcCue)), unity::ClassName(src), unity::GetName(unity::GameObjectOf(src)));
    return true;
}

// The booth's own copy of that source (a new GameObject under the holder, the cue looping, never auto-played).
void Impl::AddStandby(Rec& rec, Vec3 at) {
    if (!EnsureStandby()) return;
    Il2CppObject* go = NewChild(rec.holder, "sr2dj_standby");
    if (!go) return;
    Place(go, at, {0, 0, 0, 1}, {1, 1, 1});
    Keep(rec, go);
    Il2CppObject* c = AddComponent(go, standbyClass, "AddComponent(SECTR audio source)");
    if (!c) return;  // the holder is still inactive: nothing of it ran yet
    SetRef(c, offSrcCue, unity::Field<Il2CppObject*>(standbySrc, offSrcCue));
    unity::Field<float>(c, offSrcVolume) = unity::Field<float>(standbySrc, offSrcVolume);
    unity::Field<float>(c, offSrcPitch) = unity::Field<float>(standbySrc, offSrcPitch);
    unity::Field<bool>(c, offSrcLoop) = true;
    unity::Field<bool>(c, offSrcPlayOnStart) = false;
    unity::Field<bool>(c, offSrcRestart) = true;
    rec.standby = c;
    Keep(rec, c);
}

void Impl::StandbyPlay(Rec& rec, bool play) {
    if (!rec.standby || !unity::IsAlive(rec.standby)) return;
    bool now = true, ok = false;
    if (play) Call(standbyPlayMi, rec.standby, {}, "SECTR audio source Play", &ok);
    else Call(standbyStopMi, rec.standby, {&now}, "SECTR audio source Stop", &ok);
    rec.standbyOn = play && ok;
}

bool Impl::StandbyPlaying(Rec& rec) {
    if (!rec.standbyOn || !rec.standby || !unity::IsAlive(rec.standby)) return false;
    if (!standbyIsPlayingMi) return true;
    bool ok = false;
    const bool playing = Value<bool>(Call(standbyIsPlayingMi, rec.standby, {}, "SECTR audio source IsPlaying", &ok));
    return !ok || playing;
}

// The seated slime turns on the pillow (SlimeAttachPoint._rotationSpeed, FixedUpdate) - it looked like dancing, so it
// only does while music plays (round 9: "slime keeps dancing when music is turned off").
void Impl::Spin(Rec& rec, bool on) {
    if (!rec.attach || !offApSpin || rec.spin == (on ? 1 : 0) || !unity::IsAlive(rec.attach)) return;
    rec.spin = on ? 1 : 0;
    unity::Field<float>(rec.attach, offApSpin) = on ? rec.spinWas : 0.0f;
}

// E on the booth: a GadgetMusicPlayer (the filler track; our hooks decide) and a GadgetUIInteractable (the
// interact prompt) on the desk root, their fields copied from the Boombox prefab. UIDetector finds the
// interactable through the desk's own collider ("model", layer 27 as the Boombox's).
void Impl::AddBoothInteraction(Rec& rec) {
    Il2CppObject* srcPlayer = boomboxPrefab ? unity::GetComponent(boomboxPrefab, musicPlayerClass) : nullptr;
    Il2CppObject* srcUi = boomboxPrefab ? unity::GetComponent(boomboxPrefab, uiClass) : nullptr;
    if (!srcPlayer || !srcUi) {
        SR2_WARN("[{}] dj buildables: the Boombox prefab has no {} - the DJ Booth can't be switched with E", module,
                 !srcPlayer ? "GadgetMusicPlayer" : "GadgetUIInteractable");
        return;
    }
    // The player first: the interactable's Awake looks for it (GetComponentInParent<IGadgetInteractor>).
    Il2CppObject* player = AddComponent(rec.root, musicPlayerClass, "AddComponent(GadgetMusicPlayer)");
    if (!player) return;
    SetRef(player, offGmpMusic, unity::Field<Il2CppObject*>(srcPlayer, offGmpMusic));
    SetRef(player, offGmpCue, unity::Field<Il2CppObject*>(srcPlayer, offGmpCue));
    unity::Field<float>(player, offGmpCooldown) = unity::Field<float>(srcPlayer, offGmpCooldown);
    rec.player = player;
    rec.addedPlayer = true;
    Keep(rec, player);
    Il2CppObject* ui = AddComponent(rec.root, uiClass, "AddComponent(GadgetUIInteractable)");
    if (!ui) return;
    for (size_t off : {offUiPrompt, offUiInput, offUiDeactivate}) SetRef(ui, off, unity::Field<Il2CppObject*>(srcUi, off));
    unity::Field<bool>(ui, offUiBlock) = unity::Field<bool>(srcUi, offUiBlock);
    rec.addedUi = ui;
    Keep(rec, ui);
}

// The speaker stack (the DJ Speaker Stack at s = 1; the Slime Radio is a small one): a big boombox with a
// smaller one on top, both facing +z, their feet at `at`. The base is returned (the emitter).
Il2CppObject* Impl::BuildStack(Rec& rec, Il2CppObject* parent, Vec3 at, float s, int* built) {
    Il2CppObject* base = CloneBoombox(rec, parent, at, {0, 0, 0, 1}, kStackBase * s);
    Il2CppObject* top = CloneBoombox(rec, parent, {at.x, at.y + kStackTopY * s, at.z}, {0, 0, 0, 1}, kStackTop * s);
    if (built) *built = (base ? 1 : 0) + (top ? 1 : 0);
    return base;
}

// A speaker stack instead of the boombox on its bench. The bench, the vanilla boombox and its ball collider are
// switched off and one box collider covers the stack. E is the Boombox's own interaction.
void Impl::BuildSpeaker(Rec& rec, std::string& what) {
    HideVanilla(rec, {"ranchHouseBench", "model_boombox", "Collision"});
    int built = 0;
    if (Il2CppObject* base = BuildStack(rec, rec.holder, {kStackX, 0.0f, kStackZ}, 1.0f, &built)) rec.emitters.push_back(base);
    bool body = false;
    if (Il2CppObject* go = NewChild(rec.holder, "sr2dj_body")) {
        int32_t layer = kLayerGadgetCollision;
        Call(goSetLayer, go, {&layer}, "GameObject.set_layer");
        if (Il2CppObject* bc = AddComponent(go, boxColliderClass, "AddComponent(BoxCollider)")) {
            Vec3 c{kStackX, kStackBox.y * 0.5f, kStackZ - 0.05f}, sz{kStackBox.x, kStackBox.y, kStackBox.z};
            Call(boxSetCenter, bc, {&c}, "BoxCollider.set_center");
            Call(boxSetSize, bc, {&sz}, "BoxCollider.set_size");
            body = true;
        }
        Keep(rec, go);
    }
    rec.gadget = unity::GetComponent(rec.root, gadgetClass);
    if (rec.gadget) {
        Keep(rec, rec.gadget);
        rec.stackWas = unity::Field<bool>(rec.gadget, offStackTop);
        unity::Field<bool>(rec.gadget, offStackTop) = true;  // speakers (and decorations) stack on it
    }
    rec.player = unity::GetComponent(rec.root, musicPlayerClass);
    if (rec.player) Keep(rec, rec.player);
    what = std::format("stack of {} boombox(es), box collider {}, stacking on top {}", built, body ? "on" : "MISSING",
                       rec.gadget ? "on" : "unknown (no Gadget)");
}

// The Night Light toy dressed as a small speaker stack (the DJ Speaker Stack's look), upright and box-shaped (its
// ball collider made it roll). The stack is centred on the toy's root, where the night light was: held in the
// vacuum the root sits at the nozzle (round 9: "slime radio just disappears when vacced" - the old single boombox
// hung below the root, out of view).
void Impl::BuildRadio(Rec& rec, std::string& what) {
    HideVanilla(rec, {"model_nightlight"});
    const float h = kStackBox.y * kRadioScale;  // the stack's height (toy units; the toy is scaled 1.75)
    int built = 0;
    Il2CppObject* radio = BuildStack(rec, rec.holder, {0.0f, -h * 0.5f, 0.0f}, kRadioScale, &built);
    if (radio) rec.emitters.push_back(radio);
    bool ok = false;
    if ((rec.rb = unity::GetComponent(rec.root, rigidbodyClass)) != nullptr) {
        rec.rbWas = Value<int32_t>(Call(rbGetConstraints, rec.rb, {}, "Rigidbody.get_constraints", &ok));
        if (ok) {
            Il2CppObject* tr = unity::TransformOf(rec.root);
            bool got = false;
            Vec3 e = Value<Vec3>(Call(trGetEuler, tr, {}, "Transform.get_eulerAngles", &got));
            if (got) {
                e.x = 0.0f, e.z = 0.0f;  // stand it up, keep its heading
                Call(trSetEuler, tr, {&e}, "Transform.set_eulerAngles");
            }
            int32_t frozen = rec.rbWas | 16 | 64;  // RigidbodyConstraints.FreezeRotationX | FreezeRotationZ
            Call(rbSetConstraints, rec.rb, {&frozen}, "Rigidbody.set_constraints", &ok);
        }
        if (ok) Keep(rec, rec.rb);
        else rec.rb = nullptr;
    }
    if ((rec.box = unity::GetComponent(rec.root, boxColliderClass)) != nullptr) {
        bool a = false, b = false;
        rec.boxCenterWas = Value<Vec3>(Call(boxGetCenter, rec.box, {}, "BoxCollider.get_center", &a));
        rec.boxSizeWas = Value<Vec3>(Call(boxGetSize, rec.box, {}, "BoxCollider.get_size", &b));
        if (a && b) {
            Vec3 c{0.0f, 0.0f, -0.05f * kRadioScale}, sz{kStackBox.x * kRadioScale, h, kStackBox.z * kRadioScale};
            Call(boxSetCenter, rec.box, {&c}, "BoxCollider.set_center");
            Call(boxSetSize, rec.box, {&sz}, "BoxCollider.set_size");
            Keep(rec, rec.box);
        } else {
            rec.box = nullptr;
        }
    }
    if (rec.box && (rec.sphere = unity::GetComponent(rec.root, sphereColliderClass)) != nullptr) {
        bool off = false;
        Call(colSetEnabled, rec.sphere, {&off}, "Collider.set_enabled", &ok);
        if (ok) Keep(rec, rec.sphere);
        else rec.sphere = nullptr;
    }
    // Round 11: "It slides so much when moved". The toy's ball had the toy's slippery material and no
    // damping; the box gets a high-friction, non-bouncy material and some linear damping.
    if (rec.box) {
        bool got = false;
        if (Il2CppObject* mat = RadioMaterial()) {
            rec.matWas = Call(colGetMat, rec.box, {}, "Collider.get_sharedMaterial", &got);
            if (got) Call(colSetMat, rec.box, {mat}, "Collider.set_sharedMaterial", &got);
            rec.matSet = got;
            if (got && rec.matWas) Keep(rec, rec.matWas);
        }
    }
    if (rec.rb && rbGetDamping && rbSetDamping) {
        bool got = false;
        const float was = Value<float>(Call(rbGetDamping, rec.rb, {}, "Rigidbody.get_linearDamping", &got));
        if (got) {
            float d = std::max(was, kRadioDamping);
            Call(rbSetDamping, rec.rb, {&d}, "Rigidbody.set_linearDamping", &got);
            if (got) rec.dampWas = was;
        }
    }
    // Round 11: "it disappears after vaccing for 1-2 secs". Held at the nozzle, the vacuum's lock joint turns
    // the toy to face the player (VacuumItem.UpdateHeld); with rotation X / Z frozen the joint fought the
    // constraints and threw the radio away. FrameRadios frees the rotation while Vacuumable._held is set.
    if ((rec.vac = vacuumableClass ? unity::GetComponent(rec.root, vacuumableClass) : nullptr) != nullptr) Keep(rec, rec.vac);
    what = std::format("night light {}, stack of {} boombox(es), upright {}, box collider {}, grip {}, hold watch {}",
                       rec.hidden.empty() ? "not found" : "hidden", built, rec.rb ? "yes" : "no", rec.sphere ? "yes" : "no",
                       rec.matSet ? "yes" : "no", rec.vac && offVacHeld ? "yes" : "no");
}

// The radio's physics material (made once): grippy, no bounce; of two touching colliders the higher friction
// and the lower bounce win.
Il2CppObject* Impl::RadioMaterial() {
    if (radioMat && unity::IsAlive(radioMat)) return radioMat;
    if (!physMatClass || !pmCtor || !colSetMat || !colGetMat) return nullptr;
    Il2CppObject* m = objectNew(physMatClass);
    bool ok = false;
    if (m) Call(pmCtor, m, {unity::NewString("sr2dj radio grip")}, "PhysicsMaterial..ctor", &ok);
    if (!ok || !unity::IsAlive(m) || !unity::NewGCHandle(m)) return nullptr;
    float dyn = 0.9f, stat = 1.0f, bounce = 0.0f;
    int32_t maxCombine = 3, minCombine = 2;  // PhysicsMaterialCombine.Maximum / Minimum
    Call(pmSetDyn, m, {&dyn}, "PhysicsMaterial.set_dynamicFriction");
    Call(pmSetStatic, m, {&stat}, "PhysicsMaterial.set_staticFriction");
    Call(pmSetBounce, m, {&bounce}, "PhysicsMaterial.set_bounciness");
    Call(pmSetFricCombine, m, {&maxCombine}, "PhysicsMaterial.set_frictionCombine");
    Call(pmSetBounceCombine, m, {&minCombine}, "PhysicsMaterial.set_bounceCombine");
    radioMat = m;
    return m;
}

// Held in the vacuum: rotation free (the lock joint turns it). Let go: upright again, X / Z frozen.
void Impl::RadioHeld(Rec& rec, bool held) {
    rec.held = held ? 1 : 0;
    if (!rec.rb || !unity::IsAlive(rec.rb)) return;
    bool ok = false;
    if (held) {  // only our two bits off: whatever else the vacuum set stays
        int32_t c = Value<int32_t>(Call(rbGetConstraints, rec.rb, {}, "Rigidbody.get_constraints", &ok));
        if (ok && (c & (16 | 64)) != 0) {
            c &= ~(16 | 64);
            Call(rbSetConstraints, rec.rb, {&c}, "Rigidbody.set_constraints", &ok);
        }
    } else {
        Il2CppObject* tr = unity::TransformOf(rec.root);
        bool got = false;
        Vec3 e = Value<Vec3>(Call(trGetEuler, tr, {}, "Transform.get_eulerAngles", &got));
        if (got && (std::fabs(e.x) > 0.01f || std::fabs(e.z) > 0.01f)) {
            e.x = 0.0f, e.z = 0.0f;
            Call(trSetEuler, tr, {&e}, "Transform.set_eulerAngles");
        }
        int32_t c = rec.rbWas | 16 | 64;
        Call(rbSetConstraints, rec.rb, {&c}, "Rigidbody.set_constraints", &ok);
    }
}

void Impl::FrameRadios() {
    if (!offVacHeld) return;
    for (auto& [id, rec] : recs) {
        if (rec.fam != kRadio || !rec.vac || !unity::IsAlive(rec.vac)) continue;
        const bool held = unity::Field<bool>(rec.vac, offVacHeld);
        if (rec.held == (held ? 1 : 0)) continue;
        if (rec.held >= 0 || held)
            SR2_INFO("[{}] dj buildables: Slime Radio actor {} {}", module, id, held ? "held in the vacuum (rotation free)" : "let go (upright)");
        RadioHeld(rec, held);
        if (Faulted()) return;
    }
}

bool Impl::Build(int64_t id, Il2CppObject* root, Rec& rec) {
    ReadAnimHash();
    Il2CppObject* rootTr = unity::TransformOf(root);
    if (!rootTr) return false;
    for (Il2CppObject* child : ChildTransforms(rootTr))  // holders of an older generation / a lost record
        if (Il2CppObject* go = unity::GameObjectOf(child); go && unity::GetName(go).rfind(kHolderPrefix, 0) == 0) DestroyNow(go);
    const int fam = rec.fam, kind = rec.kind;
    rec = Rec{};
    rec.fam = fam;
    rec.kind = kind;
    rec.id = id;
    rec.root = root;
    rec.on = fam != kRadio && OnIds().count(id) > 0;
    Keep(rec, root);
    Il2CppObject* holder = NewHolder(root);
    if (!holder) return false;
    rec.holder = holder;
    Keep(rec, holder);
    std::string what;
    if (fam == kBooth) BuildBooth(rec, what);
    else if (fam == kSpeaker) BuildSpeaker(rec, what);
    else BuildRadio(rec, what);
    if (Faulted()) return false;
    unity::SetActive(holder, true);
    // Booths and speakers stay silent until they are switched on (Apply sets the volume); radios always play.
    rec.vol = fam == kRadio ? 0.8f : 0.0f;
    int registered = 0;
    for (Il2CppObject* e : rec.emitters) registered += sdj::RegisterEmitter(e, KindOf(rec), fam == kRadio ? 0.6f : 1.0f, rec.vol) ? 1 : 0;
    SR2_INFO("[{}] dj buildables: {} actor {} dressed ({}; {} emitter(s){})", module, fams[fam].label, id, what, registered,
             fam == kRadio ? "" : rec.on ? "; switched on" : "; switched off");
    return true;
}

// Vanilla parts back (restore) and our objects away. The GameObject may be gone already.
void Impl::Undress(Rec& rec, bool restore) {
    for (Il2CppObject* e : rec.emitters) sdj::UnregisterEmitter(e);
    if (restore && unity::IsAlive(rec.root)) {
        bool ok = false;
        if (rec.addedPlayer && VanillaPlaying(rec.player)) ToggleVanilla(rec.player);  // the booth's filler stops with it
        if (rec.standbyOn) StandbyPlay(rec, false);
        if (rec.addedUi) DestroyNow(rec.addedUi);  // now, not at the end of the frame: a new dressing may add them again
        if (rec.addedPlayer) DestroyNow(rec.player);
        for (Il2CppObject* go : rec.hidden)
            if (unity::IsAlive(go)) unity::SetActive(go, true);
        if (unity::IsAlive(rec.gadget)) unity::Field<bool>(rec.gadget, offStackTop) = rec.stackWas;
        if (unity::IsAlive(rec.rb)) {
            Call(rbSetConstraints, rec.rb, {&rec.rbWas}, "Rigidbody.set_constraints", &ok);
            if (rec.dampWas >= 0.0f) Call(rbSetDamping, rec.rb, {&rec.dampWas}, "Rigidbody.set_linearDamping", &ok);
        }
        if (rec.matSet && unity::IsAlive(rec.box)) Call(colSetMat, rec.box, {rec.matWas}, "Collider.set_sharedMaterial", &ok);
        if (unity::IsAlive(rec.box)) {
            Call(boxSetCenter, rec.box, {&rec.boxCenterWas}, "BoxCollider.set_center", &ok);
            Call(boxSetSize, rec.box, {&rec.boxSizeWas}, "BoxCollider.set_size", &ok);
        }
        if (unity::IsAlive(rec.sphere)) {
            bool on = true;
            Call(colSetEnabled, rec.sphere, {&on}, "Collider.set_enabled", &ok);
        }
        if (rec.kind < 0 && rec.anim == 1 && !VanillaPlaying(rec.player)) {  // a plain Boombox stops dancing
            int32_t h = animHash;
            bool off = false;
            for (Il2CppObject* a : rec.animators)
                if (unity::IsAlive(a) && h) Call(animSetBool, a, {&h, &off}, "Animator.SetBool", &ok);
            for (Il2CppObject* fx : rec.fx)
                if (unity::IsAlive(fx)) unity::SetActive(fx, false);
        }
        Destroy(rec.holder);
    }
    FreeLater(std::move(rec.handles));
    rec = Rec{};
}

void Impl::DropRec(int64_t id, bool restore) {
    auto it = recs.find(id);
    if (it == recs.end()) return;
    Undress(it->second, restore);
    recs.erase(it);
}

// ---------------------------------------------------------------- E / music --

std::set<int64_t> Impl::OnIds() {
    std::set<int64_t> out;
    std::stringstream ss(host.value(kOnKey));
    std::string part;
    while (std::getline(ss, part, ','))
        try {
            if (!part.empty()) out.insert(std::stoll(part));
        } catch (...) {
        }
    return out;
}

void Impl::SetOn(int64_t id, bool on) {
    if (!id) return;
    std::set<int64_t> ids = OnIds();
    if (on ? !ids.insert(id).second : ids.erase(id) == 0) return;
    std::string s;
    for (int64_t i : ids) s += (s.empty() ? "" : ",") + std::to_string(i);
    host.setValue(kOnKey, s);
}

Impl::Rec* Impl::RecOfPlayer(Il2CppObject* player) {
    if (!player) return nullptr;
    for (auto* m : {&recs, &plain})
        for (auto& [id, rec] : *m)
            if (rec.player == player) return &rec;
    return nullptr;
}

bool Impl::Seated(const Rec& rec) {
    if (!rec.attach || !unity::IsAlive(rec.attach)) return false;
    Il2CppObject* slime = unity::Field<Il2CppObject*>(rec.attach, offApSlime);
    return unity::Field<bool>(rec.attach, offApJointActive) && slime && unity::IsAlive(slime);
}

// The Boombox's own track plays from this player (GadgetDirector.CurrentGadgetMusicPlayer, its instance set).
bool Impl::VanillaPlaying(Il2CppObject* player) {
    if (!player || !unity::IsAlive(player)) return false;
    Il2CppObject* gd = SceneField(offScGadgetDir);
    return unity::IsAlive(gd) && unity::Field<Il2CppObject*>(gd, offGdCurrentPlayer) == player &&
           unity::Field<Il2CppObject*>(player, offMusicInstance) != nullptr;
}

// The original OnInteract: starts the track (stopping any other Boombox's) or stops it. Past the cooldown.
void Impl::ToggleVanilla(Il2CppObject* player) {
    if (!g_origInteract || !player || !unity::IsAlive(player)) return;
    unity::Field<double>(player, offGmpNext) = 0.0;
    g_origInteract(player, onInteractMi);
}

void Impl::SetVolume(Rec& rec, float vol) {
    if (rec.vol == vol) return;
    rec.vol = vol;
    for (Il2CppObject* e : rec.emitters)  // registering again only changes the scales
        sdj::RegisterEmitter(e, KindOf(rec), 1.0f, vol);
}

// The boomboxes dance and show their notes only while something plays from them.
void Impl::Dance(Rec& rec, bool on) {
    if (rec.anim == (on ? 1 : 0)) return;
    rec.anim = on ? 1 : 0;
    int32_t h = animHash;
    bool b = on, ok = false;
    for (Il2CppObject* a : rec.animators)
        if (unity::IsAlive(a) && h) Call(animSetBool, a, {&h, &b}, "Animator.SetBool", &ok);
    for (Il2CppObject* fx : rec.fx)
        if (unity::IsAlive(fx)) unity::SetActive(fx, on);
}

// Switched off: silent. On: the Slime DJ song from the record's emitters while a song is loaded (booth: and
// a slime sits behind the desk), else the Boombox's own track as filler.
void Impl::Apply(Rec& rec) {
    if (rec.fam == kRadio || !rec.player || !unity::IsAlive(rec.player)) {
        if (rec.rb && rec.held != 1 && unity::IsAlive(rec.rb)) {  // the vacuum may have reset the constraints
            bool ok = false;
            int32_t c = Value<int32_t>(Call(rbGetConstraints, rec.rb, {}, "Rigidbody.get_constraints", &ok));
            if (ok && (c & (16 | 64)) != (16 | 64)) {
                c |= 16 | 64;
                Call(rbSetConstraints, rec.rb, {&c}, "Rigidbody.set_constraints");
            }
        }
        Dance(rec, sdj::SongPlaying());  // the radio plays whenever a song does
        return;
    }
    const bool song = !sdj::SongName().empty();
    const bool seated = rec.fam != kBooth || Seated(rec);
    if (rec.fam == kBooth && seated != rec.seated) {
        rec.seated = seated;
        SR2_INFO("[{}] dj buildables: DJ Booth actor {}: {}", module, rec.id,
                 seated ? "a slime sits behind the desk" : "nobody behind the desk any more");
    }
    const bool standby = rec.standby && unity::IsAlive(rec.standby);
    bool vanilla = VanillaPlaying(rec.player);
    int want = !rec.on ? kModeOff : song && seated ? kModeSong : standby ? kModeStandby : kModeFiller;
    const char* why = "";
    if (want == kModeFiller && !vanilla) {
        if (rec.mode == kModeFiller) {  // the game stopped it: another Boombox started, or other music took over
            rec.on = false;
            SetOn(rec.id, false);
            want = kModeOff;
            why = " (its track was stopped by the game: another Boombox or other music)";
        } else {
            ToggleVanilla(rec.player);
            vanilla = VanillaPlaying(rec.player);
        }
    } else if (want != kModeFiller && vanilla) {
        ToggleVanilla(rec.player);
        vanilla = VanillaPlaying(rec.player);
    }
    if (standby) {  // booth: the music box tune while switched on without a slime / song
        const bool playing = StandbyPlaying(rec);
        if (want == kModeStandby && !playing) StandbyPlay(rec, true);
        else if (want != kModeStandby && (playing || rec.standbyOn)) StandbyPlay(rec, false);
    }
    if (want == kModeFiller || want == kModeStandby) why = !song ? " (no Slime DJ song loaded)" : " (no slime behind the desk)";
    SetVolume(rec, want == kModeSong ? 1.0f : 0.0f);
    const bool music = (want == kModeSong && sdj::SongPlaying()) || (want == kModeFiller && vanilla) ||
                       (want == kModeStandby && rec.standbyOn);
    Dance(rec, music);
    Spin(rec, music);
    if (want != rec.mode) {
        static constexpr const char* kNames[] = {"nothing", "the Slime DJ song", "the Boombox's own track",
                                                 "the corral music box tune"};
        if (rec.mode >= 0 || want != kModeOff)
            SR2_INFO("[{}] dj buildables: {} actor {} plays {}{}", module, Label(rec), rec.id, kNames[want], why);
        rec.mode = want;
    }
}

// E on one of our players (GadgetMusicPlayer.OnInteract detour). False: not ours, the original runs.
bool Impl::Interact(Il2CppObject* player) {
    Rec* rec = RecOfPlayer(player);
    if (!rec) return false;
    const ULONGLONG now = GetTickCount64();
    if (now - lastToggle < 400) return true;  // one press, one toggle
    lastToggle = now;
    rec->on = !rec->on;
    SetOn(rec->id, rec->on);
    SR2_INFO("[{}] dj buildables: {} actor {} switched {} (interact)", module, Label(*rec), rec->id, rec->on ? "on" : "off");
    Apply(*rec);
    return true;
}

void Impl::Animate() {
    if (!animHash && animHashRead && (!recs.empty() || !plain.empty())) {
        ReadAnimHash();  // the Boombox's static hash appears once a Boombox existed
        if (animHash)
            for (auto* m : {&recs, &plain})
                for (auto& [id, rec] : *m) rec.anim = -1;
    }
    for (auto* m : {&recs, &plain})
        for (auto& [id, rec] : *m) {
            Apply(rec);
            if (Faulted()) return;
        }
}

// Every loaded Boombox that is not a DJ Speaker gets a plain record: switched on with E it plays the
// Slime DJ song from its head (model_boombox, 0.72 m up) and dances (Apply). Users expected "boomboxes
// play the DJ music"; the round-7 log had a placed plain Boombox, no DJ Speaker, and nothing played there.
void Impl::SyncPlain(const std::unordered_map<int64_t, Il2CppObject*>& present, const std::map<int64_t, int>& listed) {
    constexpr size_t kMaxPlain = 40;  // slime_dj gives 48 emitters a speaker
    int added = 0, dropped = 0, registered = 0;
    for (auto it = plain.begin(); it != plain.end();) {
        auto p = present.find(it->first);
        const bool same = p != present.end() && p->second && p->second == it->second.root && unity::IsAlive(it->second.root);
        if (same && !listed.count(it->first)) {
            ++it;
            continue;
        }
        if (p == present.end()) SetOn(it->first, false);  // gone from the save (picked up)
        Undress(it->second, same);  // still there (turned into a DJ Speaker): stops dancing
        it = plain.erase(it);
        ++dropped;
    }
    std::set<int64_t> on;
    bool onRead = false;
    for (const auto& [id, go] : present) {
        if (!go || listed.count(id) || plain.count(id)) continue;
        if (!onRead) on = OnIds(), onRead = true;
        if (plain.size() >= kMaxPlain) break;
        if (!animHash) ReadAnimHash();
        Rec rec;
        rec.fam = kSpeaker;
        rec.kind = -1;
        rec.id = id;
        rec.root = go;
        rec.on = on.count(id) > 0;
        Keep(rec, go);
        Il2CppObject* model = Find(go, "model_boombox");
        if (Il2CppObject* a = InChildren(go, animatorClass)) {
            rec.animators.push_back(a);
            Keep(rec, a);
        }
        if (Il2CppObject* fx = Find(go, "model_boombox/bbox_base_jnt/bbox_head_jnt/FX_MusicalNotes")) {
            rec.fx.push_back(fx);
            Keep(rec, fx);
        }
        rec.player = unity::GetComponent(go, musicPlayerClass);
        if (rec.player) Keep(rec, rec.player);
        Il2CppObject* em = model ? model : go;
        rec.emitters.push_back(em);
        Keep(rec, em);
        rec.vol = 0.0f;  // silent until switched on (Apply)
        registered += sdj::RegisterEmitter(em, sdj::EmitterKind::kSpeaker, 1.0f, rec.vol) ? 1 : 0;
        plain[id] = std::move(rec);
        ++added;
        if (Faulted()) return;
    }
    if (added || dropped)
        SR2_INFO("[{}] dj buildables: {} plain Boombox(es) play the Slime DJ song when switched on ({} added, {} emitter(s) registered; {} dropped)",
                 module, plain.size(), added, registered, dropped);
}

void Impl::Scan() {
    Il2CppObject* gm = SceneField(offScGame);
    Il2CppObject* dict = gm ? unity::Field<Il2CppObject*>(gm, offByIdent) : nullptr;
    if (!dict) return;
    DictMethods& d = Dict(modelDict, dict);
    if (!d.tryGet) return;
    ++stableTicks;
    for (int f = 0; f < kFamCount; ++f) {
        if (fams[f].defs.empty()) continue;
        Il2CppObject* type = f == kRadio ? toyDef : fams[f].base;
        bool ok = false;
        Il2CppObject* list = nullptr;
        const bool found = Value<bool>(Call(d.tryGet, dict, {type, &list}, "identifiablesByIdent.TryGetValue", &ok));
        if (!ok) return;
        std::unordered_map<int64_t, Il2CppObject*> present;  // actor id -> its GameObject (nullptr: not loaded)
        for (size_t i = 0, n = found && list ? unity::ListCount(list) : 0; i < n && i < 2048; ++i) {
            auto* m = static_cast<Il2CppObject*>(unity::ListAt(list, i));
            Il2CppObject* tr = nullptr;
            if (f == kRadio && IsA(m, actorModelClass)) tr = unity::Field<Il2CppObject*>(m, offAmTransform);
            else if (f != kRadio && IsA(m, gadgetModelClass)) tr = unity::Field<Il2CppObject*>(m, offGmTransform);
            else continue;
            present[unity::Field<int64_t>(m, offActorId)] = unity::IsAlive(tr) ? unity::GameObjectOf(tr) : nullptr;
        }
        std::map<int64_t, int> listed = Actors(f);
        const std::map<int64_t, int> before = listed;
        if (f == kSpeaker) {
            SyncPlain(present, listed);
            if (Faulted()) return;
        }
        int kits = Kits(f);
        const int kitsBefore = kits;
        for (auto it = listed.begin(); it != listed.end();) {
            const int64_t id = it->first;
            auto p = present.find(id);
            if (p != present.end()) {
                seenIds.insert(id);
                Il2CppObject* go = p->second;
                auto r = recs.find(id);
                if (!go) {
                    if (r != recs.end()) DropRec(id, false);  // not loaded: its objects went with the scene
                } else if (r == recs.end() || r->second.root != go || r->second.kind != it->second || !unity::IsAlive(r->second.holder)) {
                    if (r != recs.end()) DropRec(id, r->second.root == go);
                    Rec rec;
                    rec.fam = f;
                    rec.kind = it->second;
                    if (Build(id, go, rec)) recs[id] = std::move(rec);
                    else Undress(rec, true);
                    if (Faulted()) return;
                }
                ++it;
                continue;
            }
            // Never seen since the load: stale only once the save's ones are clearly loaded, or after a long time.
            const bool seen = seenIds.count(id) > 0;
            if (!seen && !(stableTicks >= kStaleTicks && !present.empty()) && stableTicks < kStaleTicksEmpty) {
                ++it;
                continue;
            }
            DropRec(id, false);
            SetOn(id, false);
            if (f != kRadio) ++kits;
            if (f == kRadio && seen) {  // vacuumed up (or eaten, sold, ...): it comes back as the next Night Light near you
                const int away = std::min(Count(kAwayKey) + 1, 20);
                SetCount(kAwayKey, away);
                SR2_INFO("[{}] dj buildables: Slime Radio actor {} left the world (vacuumed up?) - the next Night Light that "
                         "appears near you is a Slime Radio again ({} away)", module, id, away);
            }
            SR2_INFO("[{}] dj buildables: {} actor {} {}{}", module, fams[f].label, id,
                     seen ? "was removed" : "is not in this save any more", f != kRadio ? std::format(" - kit back ({} now)", kits) : "");
            it = listed.erase(it);
        }
        for (auto it = recs.begin(); it != recs.end();) {  // records nobody lists any more
            if (it->second.fam != f || listed.count(it->first)) {
                ++it;
                continue;
            }
            Undress(it->second, true);
            it = recs.erase(it);
        }
        if (f == kRadio) {
            Adopt(present, listed);
            if (Faulted()) return;
        }
        if (listed != before) SetActors(f, listed);
        if (kits != kitsBefore) SetKits(f, kits);
    }
    Animate();
}

int Impl::Count(const char* key) {
    try {
        const std::string v = host.value(key);
        return v.empty() ? 0 : std::max(0, std::stoi(v));
    } catch (...) {
        return 0;
    }
}

// A radio vacuumed up leaves the world as an actor (the vacpack keeps a plain Night Light); shot out again it is a
// new Night Light actor. So while radios are away, a Night Light that was not there at the last scan and is within
// 6 m of the camera is listed as a Slime Radio (dressed at once).
void Impl::Adopt(const std::unordered_map<int64_t, Il2CppObject*>& present, std::map<int64_t, int>& listed) {
    const bool armed = stableTicks > 2 && !lastToys.empty();
    int away = -1;
    bool haveEye = false;
    Vec3 eye{};
    for (const auto& [id, go] : present) {
        if (!armed || !go || listed.count(id) || lastToys.count(id)) continue;
        if (away < 0) away = Count(kAwayKey);
        if (away <= 0) break;
        if (!haveEye) {
            Il2CppObject* cam = unity::Camera_main();
            Il2CppObject* tr = cam ? unity::TransformOf(cam) : nullptr;
            bool ok = false;
            if (tr) eye = Value<Vec3>(Call(trGetPosition, tr, {}, "Transform.get_position", &ok));
            if (!ok) break;
            haveEye = true;
        }
        bool ok = false;
        const Vec3 at = Value<Vec3>(Call(trGetPosition, unity::TransformOf(go), {}, "Transform.get_position", &ok));
        const float dx = at.x - eye.x, dy = at.y - eye.y, dz = at.z - eye.z;
        if (!ok || dx * dx + dy * dy + dz * dz > 36.0f) continue;
        listed[id] = 0;
        SetCount(kAwayKey, --away);
        SR2_INFO("[{}] dj buildables: Night Light actor {} appeared next to you - a Slime Radio again ({} still away)", module, id, away);
    }
    lastToys.clear();
    for (const auto& [id, go] : present) lastToys.insert(id);
}

// `count` Night Light toys in front of the camera (InstantiationHelpers.InstantiateActor, as the sandbox
// spawner and the vacpack do), each one listed as a Slime Radio.
int Impl::SpawnRadios(int count, bool owed) {
    Il2CppObject* sc = SceneContext();
    Il2CppObject* regions = sc ? unity::Field<Il2CppObject*>(sc, offScRegions) : nullptr;
    bool ok = false;
    Il2CppObject* group = regions ? Call(currentGroup, regions, {}, "RegionRegistry.CurrentSceneGroup", &ok) : nullptr;
    Il2CppObject* cam = unity::Camera_main();
    Il2CppObject* tr = cam ? unity::TransformOf(cam) : nullptr;
    Vec3 eye = tr ? Value<Vec3>(Call(trGetPosition, tr, {}, "Transform.get_position", &ok)) : Vec3{};
    Vec3 fwd = tr && ok ? Value<Vec3>(Call(trGetForward, tr, {}, "Transform.get_forward", &ok)) : Vec3{};
    fwd.y = 0.0f;
    const float len = std::sqrt(fwd.x * fwd.x + fwd.z * fwd.z);
    if (!group || !unity::IsAlive(group) || !tr || !ok || len < 1e-3f || !unity::IsAlive(toyPrefab)) {
        if (!owed) {
            const int n = std::min(Count(kOwedKey) + count, 50);
            SetCount(kOwedKey, n);
            SR2_WARN("[{}] dj buildables: {} Slime Radio(s) paid but not spawned yet (no scene group / camera / toy prefab) - "
                     "they spawn in front of you as soon as possible ({} owed)", module, count, n);
        }
        return 0;
    }
    fwd.x /= len, fwd.z /= len;
    const Vec3 right{fwd.z, 0.0f, -fwd.x};
    auto listed = Actors(kRadio);
    int spawned = 0;
    for (int i = 0; i < count; ++i) {
        const float side = (static_cast<float>(i) - static_cast<float>(count - 1) * 0.5f) * 1.0f;
        Vec3 pos{eye.x + fwd.x * 1.8f + right.x * side, eye.y - 0.2f, eye.z + fwd.z * 1.8f + right.z * side};
        Quat rot = AxisY(std::atan2(-fwd.x, -fwd.z) * 180.0f / 3.14159265f);  // facing the player
        bool nonActorOk = false, ignoreEmotions = false, setCollected = false;
        int appearance = 0, secondAppearance = 0;
        alignas(16) unsigned char metadata[64] = {};  // Nullable<AmmoMetadata> without a value
        Il2CppObject* go = Call(instantiateActor, nullptr,
                                {toyPrefab, group, &pos, &rot, &nonActorOk, &appearance, &secondAppearance, metadata,
                                 &ignoreEmotions, &setCollected},
                                "InstantiationHelpers.InstantiateActor", &ok);
        if (!ok || !go) break;
        const int64_t id = Value<int64_t>(Call(actorIdOf, nullptr, {go}, "Identifiable.GetActorId", &ok));
        if (ok && id) listed[id] = 0;
        ++spawned;
    }
    SetActors(kRadio, listed);
    SR2_INFO("[{}] dj buildables: {} Slime Radio(s) spawned in front of you{} ({} in this save)", module, spawned,
             owed ? " (owed from an earlier purchase)" : "", listed.size());
    return spawned;
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

// ---------------------------------------------------------------- hooks --

// GadgetMusicPlayer.OnInteract: E on one of our records switches it (Impl::Interact); everything else runs
// the original.
void OnInteractDetour(Il2CppObject* self, const MethodInfo* mi) {
    if (g && self && main_thread::IsMainThread() && !g->Faulted()) {
        bool handled = false;
        if (g_guard.Run([&] { handled = g->Interact(self); }) && handled) return;
    }
    g_origInteract(self, mi);
}

// GadgetMusicPlayer.ShowDeactivatePrompt: our records show "deactivate" while switched on.
bool ShowDeactivateDetour(Il2CppObject* self, const MethodInfo* mi) {
    if (g && self && main_thread::IsMainThread() && !g->Faulted()) {
        int r = -1;
        if (g_guard.Run([&] {
                if (Impl::Rec* rec = g->RecOfPlayer(self)) r = rec->on ? 1 : 0;
            }) &&
            r >= 0)
            return r == 1;
    }
    return g_origShowDeact(self, mi);
}

}  // namespace

// ---------------------------------------------------------------- setup --

bool DjBuildables::Setup(sr2::core::ModuleContext& ctx, Host host, std::string& why) {
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
    auto fail = [&] {
        delete p;
        return false;
    };
    HMODULE ga = api().module;
    auto exp = [&](const char* n) { return ga ? GetProcAddress(ga, n) : nullptr; };
    p->objectNew = reinterpret_cast<Impl::ObjectNewFn>(exp("il2cpp_object_new"));
    p->wbarrier = reinterpret_cast<Impl::WBarrierFn>(exp("il2cpp_gc_wbarrier_set_field"));
    p->staticGet = reinterpret_cast<Impl::StaticGetFn>(exp("il2cpp_field_static_get_value"));
    if (!p->objectNew) why = "il2cpp_object_new export missing";

    const char* kFab = "MonomiPark.SlimeRancher.UI.Fabricator";
    const char* kModel = "MonomiPark.SlimeRancher.DataModel";
    p->gadgetDefClass = cls(kImage, "", "GadgetDefinition");
    p->identTypeClass = cls(kImage, "", "IdentifiableType");
    p->groupClass = cls(kImage, "", "IdentifiableTypeGroup");
    p->lsClass = cls("Unity.Localization", "UnityEngine.Localization", "LocalizedString");
    p->gadgetClass = cls(kImage, "MonomiPark.SlimeRancher.World", "Gadget");
    p->gadgetModelClass = cls(kImage, kModel, "GadgetModel");
    p->actorModelClass = cls(kImage, kModel, "ActorModel");
    p->gadgetItemClass = cls(kImage, "MonomiPark.SlimeRancher.Player.PlayerItems", "GadgetItem");
    p->fabItemClass = cls(kImage, kFab, "GadgetFabricatableItem");
    p->gameObjectClass = cls(kCore, "UnityEngine", "GameObject");
    p->monoClass = cls(kCore, "UnityEngine", "MonoBehaviour");
    p->colliderClass = cls(kPhysics, "UnityEngine", "Collider");
    p->rigidbodyClass = cls(kPhysics, "UnityEngine", "Rigidbody");
    p->jointClass = cls(kPhysics, "UnityEngine", "Joint");
    p->animatorClass = cls("UnityEngine.AnimationModule", "UnityEngine", "Animator");
    p->musicPlayerClass = cls(kImage, "", "GadgetMusicPlayer");
    p->uiClass = cls(kImage, "MonomiPark.SlimeRancher.UI.Gadget", "GadgetUIInteractable");
    p->attachClass = cls(kImage, "", "SlimeAttachPoint");
    p->boxColliderClass = cls(kPhysics, "UnityEngine", "BoxCollider");
    p->sphereColliderClass = cls(kPhysics, "UnityEngine", "SphereCollider");
    p->sectrSourceClass = cls(kImage, "", "SECTR_AudioSource");
    p->vacuumableClass = R.Class(kImage, "MonomiPark.SlimeRancher", "Vacuumable");  // optional: radio hold watch
    p->physMatClass = R.Class(kPhysics, "UnityEngine", "PhysicsMaterial");         // optional: radio grip
    Il2CppClass* baseUi = cls(kImage, "MonomiPark.SlimeRancher.UI", "BaseUIInteractable");
    Il2CppClass* fabCategory = cls(kImage, kFab, "GadgetFabricateCategory");
    Il2CppClass* gadgetDir = cls(kImage, "", "GadgetDirector");
    Il2CppClass* gadgetsModel = cls(kImage, kModel, "GadgetsModel");
    Il2CppClass* identModel = cls(kImage, kModel, "IdentifiableModel");
    Il2CppClass* sceneCtx = cls(kImage, "", "SceneContext");
    Il2CppClass* gameCtx = cls(kImage, "", "GameContext");
    Il2CppClass* autoSave = cls(kImage, "MonomiPark.SlimeRancher", "AutoSaveDirector");
    Il2CppClass* saveRef = cls(kImage, "MonomiPark.SlimeRancher", "SaveReferenceTranslation");
    Il2CppClass* gameModel = cls(kImage, kModel, "GameModel");
    Il2CppClass* lookup = cls(kImage, "", "LookupDirector");
    Il2CppClass* spendable = cls(kImage, "MonomiPark.SlimeRancher.Player", "SpendableDirector");
    Il2CppClass* purchaseCost = cls(kImage, "MonomiPark.SlimeRancher.Economy", "PurchaseCost");
    Il2CppClass* costEntry = cls(kImage, "MonomiPark.SlimeRancher.Economy", "CurrencyCostEntry");
    Il2CppClass* helpers = cls(kImage, "", "InstantiationHelpers");
    Il2CppClass* regions = cls(kImage, "MonomiPark.SlimeRancher.Regions", "RegionRegistry");
    Il2CppClass* identifiable = cls(kImage, "", "Identifiable");
    Il2CppClass* component = cls(kCore, "UnityEngine", "Component");
    Il2CppClass* transform = cls(kCore, "UnityEngine", "Transform");
    Il2CppClass* uobject = cls(kCore, "UnityEngine", "Object");
    if (!why.empty()) return fail();
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
    off(p->gadgetDefClass, "LinkedDefinition", p->offLinked);
    off(purchaseCost, "_currencyCosts", p->offPcCurrency);
    off(purchaseCost, "IdentCosts", p->offPcIdent);
    off(costEntry, "_amount", p->offCostAmount);
    off(gadgetDir, "_gadgetVariantMap", p->offVariantMap);
    off(gadgetDir, "_model", p->offGdModel);
    off(gadgetDir, "<TargetedGadget>k__BackingField", p->offTargeted);
    off(gadgetsModel, "_favorites", p->offFavorites);
    off(p->gadgetItemClass, "_heldGadget", p->offHeld);
    off(p->gadgetItemClass, "_lastPlacedGadget", p->offLastPlaced);
    off(p->gadgetClass, "_model", p->offGadgetModel);
    off(p->gadgetClass, "AllowGadgetStackingOnTop", p->offStackTop);
    off(p->gadgetModelClass, "transform", p->offGmTransform);
    off(p->actorModelClass, "transform", p->offAmTransform);
    off(identModel, "actorId", p->offActorId);
    off(p->fabItemClass, "<Definition>k__BackingField", p->offFabDef);
    off(p->fabItemClass, "<Recipe>k__BackingField", p->offFabRecipe);
    off(fabCategory, "_gadgetPurchaseEvent", p->offCatEvent);
    off(p->musicPlayerClass, "gadgetMusicInstance", p->offMusicInstance);
    off(sceneCtx, "<GameModel>k__BackingField", p->offScGame);
    off(sceneCtx, "<GadgetDirector>k__BackingField", p->offScGadgetDir);
    off(sceneCtx, "<SpendableDirector>k__BackingField", p->offScSpendable);
    off(sceneCtx, "<RegionRegistry>k__BackingField", p->offScRegions);
    off(gameCtx, "<AutoSaveDirector>k__BackingField", p->offGcAutoSave);
    off(autoSave, "_saveReferenceTranslation", p->offAsdTranslation);
    off(saveRef, "_identifiableTypeToPersistenceId", p->offSrtIdentTable);
    off(gameModel, "identifiablesByIdent", p->offByIdent);
    off(lookup, "_gadgetDefinitions", p->offGadgets);
    off(lookup, "_toyDefinitions", p->offToys);
    off(p->groupClass, "_memberTypes", p->offMembers);
    off(p->groupClass, "_memberGroups", p->offSubGroups);
    off(p->musicPlayerClass, "gadgetMusic", p->offGmpMusic);
    off(p->musicPlayerClass, "onInteractCue", p->offGmpCue);
    off(p->musicPlayerClass, "timeBetweenActivations", p->offGmpCooldown);
    off(p->musicPlayerClass, "nextAllowedActivationTime", p->offGmpNext);
    off(gadgetDir, "<CurrentGadgetMusicPlayer>k__BackingField", p->offGdCurrentPlayer);
    off(baseUi, "defaultInteractionPrompt", p->offUiPrompt);
    off(baseUi, "interactionInput", p->offUiInput);
    off(p->uiClass, "_blockInteractInGadgetMode", p->offUiBlock);
    off(p->uiClass, "_deactivateInteractionPrompt", p->offUiDeactivate);
    off(p->attachClass, "_allowLargo", p->offApAllowLargo);
    off(p->attachClass, "_slime", p->offApSlime);
    off(p->attachClass, "_isJointActive", p->offApJointActive);
    off(p->attachClass, "_rotationSpeed", p->offApSpin);
    off(p->sectrSourceClass, "volume", p->offSrcVolume);
    off(p->sectrSourceClass, "pitch", p->offSrcPitch);
    off(p->sectrSourceClass, "Cue", p->offSrcCue);
    off(p->sectrSourceClass, "Loop", p->offSrcLoop);
    off(p->sectrSourceClass, "PlayOnStart", p->offSrcPlayOnStart);
    off(p->sectrSourceClass, "RestartLoopsOnEnabled", p->offSrcRestart);
    if (!why.empty()) return fail();
    for (size_t* o : {&p->offPcCurrency, &p->offPcIdent})  // value-type field offsets include the object header
        *o = *o >= 2 * sizeof(void*) ? *o - 2 * sizeof(void*) : *o;
    p->animHashField = R.Field(p->musicPlayerClass, "_gadgetActive", nullptr);
    if (p->vacuumableClass)  // optional: without it the radio's rotation stays frozen while held
        if (auto o = R.FieldOffset(p->vacuumableClass, "_held")) p->offVacHeld = *o;  // optional (no dance without it)

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
    p->goActiveSelf = method(p->gameObjectClass, "get_activeSelf", 0);
    p->compInChildren = byParams(component, "GetComponentInChildren", {"System.Type", "System.Boolean"});
    p->compsInChildren = byParams(component, "GetComponentsInChildren", {"System.Type", "System.Boolean"});
    p->trFind = byParams(transform, "Find", {"System.String"});
    p->trSetParent = byParams(transform, "SetParent", {"UnityEngine.Transform", "System.Boolean"});
    p->trSetLocalPos = method(transform, "set_localPosition", 1);
    p->trSetLocalRot = method(transform, "set_localRotation", 1);
    p->trSetLocalScale = method(transform, "set_localScale", 1);
    p->trGetLocalRot = method(transform, "get_localRotation", 0);
    p->trGetLocalScale = method(transform, "get_localScale", 0);
    p->trGetPosition = method(transform, "get_position", 0);
    p->trGetForward = method(transform, "get_forward", 0);
    p->trChildCount = method(transform, "get_childCount", 0);
    p->trGetChild = method(transform, "GetChild", 1);
    p->animSetBool = byParams(p->animatorClass, "SetBool", {"System.Int32", "System.Boolean"});
    p->fabItemCtor = R.Method(p->fabItemClass, ".ctor", 4, nullptr);
    {
        const MethodInfo* m = nullptr;
        std::string err = MethodBySuffixName(p->fabItemClass, ".CheckCanFabricate", 1, m);
        p->fabCheck = m;
        if (!m && why.empty()) why = err;
    }
    p->spend = byParams(spendable, "Spend", {"MonomiPark.SlimeRancher.Economy.PurchaseCost", "System.Int32"});
    p->addItem = byParams(gadgetDir, "AddItem", {"IdentifiableType", "System.Int32"});
    p->hasBp = method(gadgetDir, "HasBlueprint", 1);
    p->setFavorite = method(gadgetsModel, "SetFavorite", 2);
    p->setHeld = method(p->gadgetItemClass, "SetHeldGadget", 1);
    p->instantiateActor = method(helpers, "InstantiateActor", 10);
    p->currentGroup = method(regions, "get_CurrentSceneGroup", 0);
    p->actorIdOf = byParams(identifiable, "GetActorId", {"UnityEngine.GameObject"});
    p->goAddComponent = byParams(p->gameObjectClass, "AddComponent", {"System.Type"});
    p->goSetLayer = method(p->gameObjectClass, "set_layer", 1);
    p->colSetEnabled = method(p->colliderClass, "set_enabled", 1);
    p->boxGetCenter = method(p->boxColliderClass, "get_center", 0);
    p->boxSetCenter = method(p->boxColliderClass, "set_center", 1);
    p->boxGetSize = method(p->boxColliderClass, "get_size", 0);
    p->boxSetSize = method(p->boxColliderClass, "set_size", 1);
    p->sphereGetRadius = method(p->sphereColliderClass, "get_radius", 0);
    p->sphereSetRadius = method(p->sphereColliderClass, "set_radius", 1);
    p->rbGetConstraints = method(p->rigidbodyClass, "get_constraints", 0);
    p->rbSetConstraints = method(p->rigidbodyClass, "set_constraints", 1);
    p->trGetEuler = method(transform, "get_eulerAngles", 0);
    p->trSetEuler = method(transform, "set_eulerAngles", 1);
    // optional (the radio's grip): damping, collider material, PhysicsMaterial
    p->rbGetDamping = R.Method(p->rigidbodyClass, "get_linearDamping", 0);
    p->rbSetDamping = R.Method(p->rigidbodyClass, "set_linearDamping", 1);
    p->colGetMat = R.Method(p->colliderClass, "get_sharedMaterial", 0);
    p->colSetMat = R.Method(p->colliderClass, "set_sharedMaterial", 1);
    if (p->physMatClass) {
        p->pmCtor = R.MethodByParams(p->physMatClass, ".ctor", {"System.String"});
        p->pmSetDyn = R.Method(p->physMatClass, "set_dynamicFriction", 1);
        p->pmSetStatic = R.Method(p->physMatClass, "set_staticFriction", 1);
        p->pmSetBounce = R.Method(p->physMatClass, "set_bounciness", 1);
        p->pmSetFricCombine = R.Method(p->physMatClass, "set_frictionCombine", 1);
        p->pmSetBounceCombine = R.Method(p->physMatClass, "set_bounceCombine", 1);
    }
    p->onInteractMi = method(p->musicPlayerClass, "OnInteract", 0);
    p->showDeactMi = method(p->musicPlayerClass, "ShowDeactivatePrompt", 0);
    p->getPlotPrefab = R.Method(lookup, "GetPlotPrefab", 1, nullptr);  // optional: no standby tune without it
    if (!p->lsCtor || !p->fabItemCtor) why = "constructors not found";
    if (!why.empty()) return fail();
    // E on our booths / speakers / Boomboxes (GadgetMusicPlayer is hooked nowhere else).
    void* interact = R.MethodPointer(p->onInteractMi, &why);
    void* showDeact = interact ? R.MethodPointer(p->showDeactMi, &why) : nullptr;
    if (!interact || !showDeact) return fail();
    if (!ctx.HookAddress("dj.GadgetMusicPlayer.OnInteract", interact, reinterpret_cast<void*>(&OnInteractDetour),
                         reinterpret_cast<void**>(&g_origInteract)) ||
        !ctx.HookAddress("dj.GadgetMusicPlayer.ShowDeactivatePrompt", showDeact, reinterpret_cast<void*>(&ShowDeactivateDetour),
                         reinterpret_cast<void**>(&g_origShowDeact))) {
        why = ctx.failure();
        return fail();
    }
    g = p;
    variant_ext::Set(p);  // the Quantum Storage Link's detours ask us from now on
    // The gadget-mode label of an aimed-at placed booth / speaker stack (a vanilla desk / Boombox to the game).
    gadget_look::AddPlacedLabel([](Il2CppObject* gadget, Il2CppObject*) -> Il2CppObject* {
        if (!g || !g->defsBuilt || g->Faulted()) return nullptr;
        Il2CppObject* r = nullptr;
        g_guard.Run([&] { r = g->TargetLabel(gadget); });
        return r;
    });
    ready_ = true;
    return true;
}

// ---------------------------------------------------------------- api --

void DjBuildables::Tick() {
    if (!ready_ || !g || !main_thread::IsMainThread() || g->Faulted()) return;
    Impl& p = *g;
    if (!p.host.saveReady || !p.host.saveReady()) return;
    if (!p.EnsureDefinitions() || p.Faulted()) return;
    p.Register();
    if (p.Faulted()) return;
    p.Scan();
    if (p.Faulted() || p.fams[kRadio].defs.empty()) return;
    if (const int owed = p.Count(kOwedKey); owed > 0) {  // paid radios that could not be spawned (e.g. in a menu)
        const int n = p.SpawnRadios(std::min(owed, kMaxRadioSpawn), true);
        if (n > 0) p.SetCount(kOwedKey, owed - n);
    }
}

void DjBuildables::Frame() {
    if (!ready_ || !g || !main_thread::IsMainThread() || g->Faulted() || g->recs.empty()) return;
    g_guard.Run([] { g->FrameRadios(); });
}

void DjBuildables::Cleanup(const char* why) {
    if (!ready_ || !g || !main_thread::IsMainThread()) return;
    Impl& p = *g;
    const bool calls = !p.Faulted();
    int dressed = 0;
    for (auto& [id, rec] : p.recs) {
        p.Undress(rec, calls);
        ++dressed;
    }
    p.recs.clear();
    for (auto& [id, rec] : p.plain) p.Undress(rec, calls);  // plain Boomboxes: emitters off, dance off
    p.plain.clear();
    if (!p.defsBuilt) return;
    int favs = 0;
    bool unregistered = false;
    if (Il2CppObject* gd = p.SceneField(p.offScGadgetDir); unity::IsAlive(gd)) {
        // Favourite slots holding our variants get the vanilla gadget (they are saved).
        Il2CppObject* model = unity::Field<Il2CppObject*>(gd, p.offGdModel);
        Il2CppObject* fav = model ? unity::Field<Il2CppObject*>(model, p.offFavorites) : nullptr;
        auto valuesOff = fav ? Resolver::Get().FieldOffset(api().il2cpp_object_get_class(fav), "_values") : std::nullopt;
        void* values = valuesOff ? unity::Field<void*>(fav, *valuesOff) : nullptr;
        for (size_t i = 0, n = values ? unity::ArrayLength(values) : 0; i < n && i < 16; ++i) {
            auto* cur = static_cast<Il2CppObject*>(unity::ArrayAt(values, i));
            const int f = p.FamOf(cur);
            if (f < 0) continue;
            int32_t idx = static_cast<int32_t>(i);
            bool ok = false;
            if (calls) p.Call(p.setFavorite, model, {&idx, p.fams[f].base}, "GadgetsModel.SetFavorite", &ok);
            if (!ok) p.SetElem(static_cast<Il2CppObject*>(values), i, p.fams[f].base);
            ++favs;
        }
        Il2CppObject* map = unity::Field<Il2CppObject*>(gd, p.offVariantMap);
        if (map && calls) {
            Impl::DictMethods& d = p.Dict(p.variantDict, map);
            bool ok = false;
            for (int f = 0; f < kFamCount; ++f) {
                Il2CppObject* list = nullptr;
                const bool have = d.tryGet && Impl::Value<bool>(p.Call(d.tryGet, map, {p.fams[f].base, &list}, "variant map TryGetValue", &ok));
                for (Il2CppObject* def : p.fams[f].defs) {
                    if (have && list) {
                        Impl::ListMethods& l = p.Lst(p.variantList, list);
                        if (l.remove) p.Call(l.remove, list, {def}, "variant list Remove", &ok);
                    }
                    if (d.remove) p.Call(d.remove, map, {def}, "variant map Remove", &ok);
                }
            }
            p.registeredDir = nullptr;
            unregistered = true;
        }
    }
    SR2_INFO("[{}] dj buildables: {}: {} undressed, {} favourite slot(s) back to the vanilla decoration{}", p.module, why, dressed,
             favs, unregistered ? ", variants unregistered" : "");
}

void DjBuildables::Reset() {
    if (!g) return;
    Impl& p = *g;
    for (auto& [id, rec] : p.recs) {
        for (Il2CppObject* e : rec.emitters) sdj::UnregisterEmitter(e);
        Impl::FreeLater(std::move(rec.handles));  // the old game's objects go with its scene
    }
    p.recs.clear();
    for (auto& [id, rec] : p.plain) {
        for (Il2CppObject* e : rec.emitters) sdj::UnregisterEmitter(e);
        Impl::FreeLater(std::move(rec.handles));
    }
    p.plain.clear();
    p.seenIds.clear();
    p.lastToys.clear();
    p.stableTicks = 0;
    ++p.generation;
    p.registeredDir = nullptr;
}

bool Owns(Il2CppObject* def) { return TableIndex(def) >= 0; }

void Describe(std::vector<buildables::Info>& out) {
    if (!g || !g->defsBuilt || !main_thread::IsMainThread() || g->Faulted()) return;
    Impl& p = *g;
    g_guard.Run([&] {
        for (int f = 0; f < kFamCount; ++f) {
            if (p.fams[f].defs.empty()) continue;
            buildables::Info i;
            i.id = p.fams[f].id;
            i.name = p.fams[f].label;
            i.base = p.fams[f].baseName;
            i.baseAsset = unity::GetName(p.fams[f].base);
            i.hint = p.Hint(f);
            i.def = p.fams[f].defs[0];
            i.baseDef = p.fams[f].base;
            Il2CppObject* icon = p.offIcon ? unity::Field<Il2CppObject*>(p.fams[f].defs[0], p.offIcon) : nullptr;
            i.icon = unity::IsAlive(icon) ? icon : nullptr;
            i.unlock = p.Override(f);
            i.baseUnlocked = p.BaseBlueprint(f);
            i.unlocked = i.unlock == buildables::Unlock::kUnlocked ||
                         (i.unlock == buildables::Unlock::kFollowBase && i.baseUnlocked);
            i.owned = f == kRadio ? static_cast<int>(p.Actors(kRadio).size()) : p.Kits(f);
            out.push_back(std::move(i));
        }
    });
}

bool Grant(const std::string& id, int count, std::string& result) {
    if (!g || !main_thread::IsMainThread() || g->Faulted()) {
        result = "the Slime DJ buildables are not available";
        return false;
    }
    Impl& p = *g;
    int f = -1;
    for (int i = 0; i < kFamCount; ++i)
        if (id == p.fams[i].id) f = i;
    if (f < 0 || !p.defsBuilt || p.fams[f].defs.empty() || !p.host.saveReady || !p.host.saveReady()) {
        result = f < 0 ? "unknown buildable"
                       : "load a save first (the DJ buildables are set up at the first tick of a loaded save)";
        return false;
    }
    if (f == kRadio) {
        const int n = std::clamp(count, 1, kMaxRadioSpawn);
        main_thread::Post([n] {  // as the Fabricator does: spawned and listed on the next frame
            if (g) g_guard.Run([&] { g->SpawnRadios(n); });
        });
        result = std::format("{} Slime Radio(s) spawning in front of you", n);
        return true;
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
        const int32_t added =
            Impl::Value<int32_t>(p.Call(p.addItem, gd, {p.fams[f].base, &count}, "GadgetDirector.AddItem", &called));
        if (!called) {
            result = "GadgetDirector.AddItem failed (see the log)";
            return;
        }
        const int kits = p.Kits(f) + count;
        p.SetKits(f, kits);
        ok = true;
        result = std::format("{} {}(s): {} '{}' added, {} kit(s) now", count, p.fams[f].label, added,
                             unity::GetName(p.fams[f].base), kits);
    });
    return ok;
}

bool RadioShop(RadioShopInfo& out) {
    if (!g || !g->defsBuilt || !main_thread::IsMainThread() || g->Faulted()) return false;
    Impl& p = *g;
    if (p.fams[kRadio].defs.empty() || !unity::IsAlive(p.toyDef)) return false;
    bool ok = false;
    g_guard.Run([&] {
        out.toyDef = p.toyDef;
        out.boombox = p.fams[kRadio].base;
        out.toyRefId = p.RefId(p.toyDef);
        out.unlocked = p.CardUnlocked(kRadio, [&](Il2CppObject*) { return p.BaseBlueprint(kRadio); });
        ok = !out.toyRefId.empty();
    });
    return ok;
}

void ListRadio(int64_t actorId) {
    if (!g || !actorId || !main_thread::IsMainThread() || g->Faulted()) return;
    g_guard.Run([&] {
        g->List(kRadio, actorId, 0);
        SR2_INFO("[{}] dj buildables: Slime Radio actor {} listed ({} in this save)", g->module, actorId,
                 g->Actors(kRadio).size());
    });
}

}  // namespace sr2::modules::custom_upgrades::dj
