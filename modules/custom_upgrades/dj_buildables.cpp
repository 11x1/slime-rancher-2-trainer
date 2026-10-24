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
//   * Dressing a listed, loaded actor: booth = clutter hidden, a small boombox on the desk (mixer), two
//     boombox speakers on the floor beside it (emitters), a flower pillow with the game's slime attachment
//     point behind it; speaker = its boombox tilted / scaled, stacking allowed on top, an emitter; radio =
//     the night light hidden, a small boombox instead, an emitter. Every clone is script-free except the
//     pillow's attachment point. While a song plays the boomboxes dance and show their notes.

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
#include "modules/custom_upgrades/variant_ext.h"
#include "modules/slime_dj/api.h"

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
constexpr int kStaleTicks = 5;
constexpr int kStaleTicksEmpty = 60;
constexpr int kMaxRadioSpawn = 5;

struct Vec3 {
    float x, y, z;
};
struct Quat {
    float x, y, z, w;
};
struct Color {
    float r, g, b, a;
};

Quat Mul(const Quat& a, const Quat& b) {
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}
Quat AxisX(float deg) {
    const float h = deg * 3.14159265f / 360.0f;
    return {std::sin(h), 0.0f, 0.0f, std::cos(h)};
}
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
    float tilt;  // speakers: degrees the boombox leans back (0 = upright)
};
constexpr VariantSpec kBoothSpec[] = {
    {"SlimeDJBooth", "Slime DJ Booth",
     "A DJ table with a mixer and two speakers that play your Slime DJ song. A slime can sit on the flower pillow "
     "behind the table. Place it as the Scientist Desk's variant (each one uses a DJ Booth you fabricated); it is "
     "saved as a Scientist Desk.",
     0.0f},
};
constexpr VariantSpec kSpeakerSpec[] = {
    {"SlimeDJSpeaker", "DJ Speaker",
     "A speaker that plays your Slime DJ song. Other decorations and speakers stack on top of it. Toggle the variant "
     "to tilt it; place it as the Boombox's variant (each one uses a DJ Speaker you fabricated). Saved as a Boombox.",
     0.0f},
    {"SlimeDJSpeakerTilted", "DJ Speaker (tilted)",
     "A DJ Speaker leaning back, for stacks and stages. Plays your Slime DJ song. Saved as a Boombox.", 30.0f},
    {"SlimeDJSpeakerUp", "DJ Speaker (facing up)",
     "A DJ Speaker facing the sky. Plays your Slime DJ song. Saved as a Boombox.", 80.0f},
};
constexpr VariantSpec kRadioSpec = {
    "SlimeDJRadio", "Slime Radio",
    "A radio toy that plays your Slime DJ song wherever it is - slimes love to play with it. Fabricating one drops "
    "it in front of you. It is a Night Light toy for the save.",
    0.0f};

struct Family {
    const char* label;
    const char* kitsKey;
    const char* actorsKey;
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
    // offsets
    size_t offLocName = 0, offPrefab = 0, offRefId = 0, offHashInit = 0, offDesc = 0, offCosts = 0, offVariantOf = 0;
    size_t offGrad1 = 0, offGrad2 = 0, offLinked = 0, offPcCurrency = 0, offPcIdent = 0, offCostAmount = 0;
    size_t offVariantMap = 0, offGdModel = 0, offFavorites = 0, offTargeted = 0;
    size_t offHeld = 0, offLastPlaced = 0, offGadgetModel = 0, offGmTransform = 0, offAmTransform = 0, offActorId = 0;
    size_t offStackTop = 0, offFabDef = 0, offFabRecipe = 0, offCatEvent = 0, offMusicInstance = 0;
    size_t offScGame = 0, offScGadgetDir = 0, offScSpendable = 0, offScRegions = 0, offGcAutoSave = 0;
    size_t offAsdTranslation = 0, offSrtIdentTable = 0, offByIdent = 0, offGadgets = 0, offToys = 0;
    size_t offMembers = 0, offSubGroups = 0;
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
    const MethodInfo* actorIdOf = nullptr;
    FieldInfo* animHashField = nullptr;
    using ObjectNewFn = Il2CppObject* (*)(Il2CppClass*);
    using WBarrierFn = void (*)(Il2CppObject*, void**, void*);
    using StaticGetFn = void (*)(FieldInfo*, void*);
    ObjectNewFn objectNew = nullptr;
    WBarrierFn wbarrier = nullptr;
    StaticGetFn staticGet = nullptr;

    // session objects (pinned, never freed)
    Family fams[kFamCount] = {
        {"DJ Booth", "dj.booth_kits", "dj.booths"},
        {"DJ Speaker", "dj.speaker_kits", "dj.speakers"},
        {"Slime Radio", nullptr, kRadiosKey},
    };
    Il2CppObject* boomboxPrefab = nullptr, *pillowPrefab = nullptr, *toyDef = nullptr, *toyPrefab = nullptr;
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
    bool loggedNoVariantEntry = false, loggedCards = false;

    // dressed actors
    struct Rec {
        int fam = 0, kind = 0;
        Il2CppObject* root = nullptr;
        Il2CppObject* holder = nullptr;
        std::vector<Il2CppObject*> hidden;     // vanilla GameObjects we switched off
        Il2CppObject* model = nullptr;         // speaker: the vanilla boombox model we turned / scaled
        Quat modelRot{0, 0, 0, 1};
        Vec3 modelScale{1, 1, 1};
        Il2CppObject* gadget = nullptr;        // speaker: its Gadget (stacking flag)
        bool stackWas = false;
        Il2CppObject* player = nullptr;        // speaker: its GadgetMusicPlayer (its own track wins)
        std::vector<Il2CppObject*> emitters;
        std::vector<Il2CppObject*> animators;
        std::vector<Il2CppObject*> fx;
        std::vector<bool> fxWas;
        int anim = -1;                         // dance state applied (-1 = none yet)
        std::vector<Il2CppGCHandle> handles;
    };
    std::map<int64_t, Rec> recs;
    std::unordered_set<int64_t> seenIds;
    int stableTicks = 0;
    unsigned generation = 1;

    // pickup hand-over (BeforePickup -> AfterPickup)
    int pickupFam = -1, pickupKind = 0;

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
    void Register();
    void ReadAnimHash();
    void Strip(Il2CppObject* root);
    Il2CppObject* NewHolder(Il2CppObject* root);
    Il2CppObject* CloneBoombox(Rec& rec, Il2CppObject* parent, Vec3 pos, float yaw, float scale);
    void HideVanilla(Rec& rec, std::initializer_list<const char*> paths);
    bool Build(int64_t id, Il2CppObject* root, Rec& rec);
    void Undress(Rec& rec, bool restore);
    void DropRec(int64_t id, bool restore);
    void Animate();
    void Scan();
    void SpawnRadios(int count);

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
};

Impl* g = nullptr;
sr2::core::DetourGuard g_guard{"custom_upgrades", "dj buildables"};

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
    auto rev = Resolver::Get().FieldOffset(api().il2cpp_object_get_class(table), "_reverseIndex");
    Il2CppObject* dict = rev ? unity::Field<Il2CppObject*>(table, *rev) : nullptr;
    if (!dict) return;
    DictMethods& d = Dict(aliasDict, dict);
    if (!d.containsKey || !d.tryGet || !d.setItem) return;
    for (int f = 0; f < kFamCount; ++f)
        for (Il2CppObject* def : fams[f].defs) {
            const std::string mine = std::format("GadgetDefinition.{}", unity::GetName(def));
            Il2CppString* key = unity::NewString(mine);
            if (Value<bool>(Call(d.containsKey, dict, {key}, "save index ContainsKey", &ok)) || !ok) continue;
            int32_t id = -1;
            const bool found = Value<bool>(Call(d.tryGet, dict, {unity::NewString(RefId(fams[f].base)), &id}, "save index TryGetValue", &ok));
            if (!ok || !found || id < 0) continue;
            Call(d.setItem, dict, {key, &id}, "save index set_Item", &ok);
            if (ok && aliased.insert(mine).second)
                SR2_INFO("[{}] dj buildables: save alias {} -> persistence id {}", module, mine, id);
        }
}

// ---------------------------------------------------------------- extension --

bool Impl::HasBlueprint(Il2CppObject* def, const std::function<bool(Il2CppObject*)>& vanilla) {
    const int f = FamOf(def);
    if (f < 0 || f == kRadio) return f == kRadio && vanilla(fams[kRadio].base);
    if (!vanilla(fams[f].base)) return false;
    return Kits(f) > 0 || TargetListed(f);
}

bool Impl::HelperBlueprint(Il2CppObject* def, const std::function<bool(Il2CppObject*)>& vanilla) {
    const int f = FamOf(def);
    return f >= 0 && vanilla(fams[f].base);
}

int32_t Impl::ItemCount(Il2CppObject* def, int32_t vanilla) {
    const int f = FamOf(def);
    if (f < 0) return vanilla;
    if (f == kRadio) return static_cast<int32_t>(Actors(kRadio).size());
    return std::min(vanilla, Kits(f));
}

void Impl::InsertCards(Il2CppObject* category, Il2CppObject* list, Il2CppObject* helper, Il2CppObject* factory) {
    if (!defsBuilt) return;
    // (card definition, the vanilla definition it follows)
    std::vector<std::pair<Il2CppObject*, Il2CppObject*>> cards;
    if (!fams[kBooth].defs.empty()) cards.emplace_back(fams[kBooth].defs[0], fams[kBooth].base);
    if (!fams[kSpeaker].defs.empty()) cards.emplace_back(fams[kSpeaker].defs[0], fams[kSpeaker].base);
    if (!fams[kRadio].defs.empty() && !fams[kSpeaker].defs.empty()) cards.emplace_back(fams[kRadio].defs[0], fams[kSpeaker].defs[0]);
    int added = 0;
    for (const auto& [def, after] : cards) {
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
        if (ok) ++added;
    }
    if (added && !loggedCards) {
        loggedCards = true;
        SR2_INFO("[{}] dj buildables: {} Fabricator card(s) added (Decorations)", module, added);
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
}

bool Impl::BeforePickup(Il2CppObject*, Il2CppObject* gadget) {
    pickupFam = -1;
    const int64_t id = ActorOfGadget(gadget);
    int kind = 0;
    const int f = ListedFam(id, &kind);
    if (f < 0 || f == kRadio) return false;
    Unlist(f, id);
    SetKits(f, Kits(f) + 1);
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

// A script-free copy of the Boombox's "model_boombox" (Animator kept: it dances while a song plays).
Il2CppObject* Impl::CloneBoombox(Rec& rec, Il2CppObject* parent, Vec3 pos, float yaw, float scale) {
    Il2CppObject* src = boomboxPrefab ? Find(boomboxPrefab, "model_boombox") : nullptr;
    if (!src) return nullptr;
    Il2CppObject* c = unity::Instantiate(src, unity::TransformOf(parent));
    if (!unity::IsAlive(c)) return nullptr;
    Strip(c);
    Place(c, pos, AxisY(yaw), {scale, scale, scale});
    unity::SetActive(c, true);
    if (Il2CppObject* a = InChildren(c, animatorClass)) {
        rec.animators.push_back(a);
        Keep(rec, a);
    }
    if (Il2CppObject* fx = Find(c, "bbox_base_jnt/bbox_head_jnt/FX_MusicalNotes")) {
        unity::SetActive(fx, false);
        rec.fx.push_back(fx);
        rec.fxWas.push_back(false);
        Keep(rec, fx);
    }
    Keep(rec, c);
    return c;
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
    rec.root = root;
    Keep(rec, root);
    Il2CppObject* holder = NewHolder(root);
    if (!holder) return false;
    rec.holder = holder;
    Keep(rec, holder);
    std::string what;
    if (fam == kBooth) {
        HideVanilla(rec, {"model/scientistDesk_bubbleScroll", "model/scientistDesk_glass", "model/scientistDesk_glass_LOD01",
                          "model/vials", "model/vials_LOD01", "model/decoLabBeaker01", "model/decoLabClipboard",
                          "model/decoLabWrenchMonkey03_0", "model/pens", "SoundScientistDesk"});
        CloneBoombox(rec, holder, {0.0f, 1.22f, 0.05f}, 180.0f, 0.5f);  // the mixer, facing the DJ (-z)
        for (float side : {-1.0f, 1.0f}) {
            Il2CppObject* spk = CloneBoombox(rec, holder, {side * 1.75f, 0.0f, 0.1f}, -side * 12.0f, 1.35f);
            if (spk) rec.emitters.push_back(spk);
        }
        // The DJ's spot: the flower pillow with the game's own slime attachment point, behind the desk.
        int spot = 0;
        if (pillowPrefab) {
            const Vec3 at{0.0f, 0.0f, -1.55f};
            for (const char* part : {"mesh", "COL", "SlimeAttachmentPoint"}) {
                Il2CppObject* src = Find(pillowPrefab, part);
                Il2CppObject* c = src ? unity::Instantiate(src, unity::TransformOf(holder)) : nullptr;
                if (!unity::IsAlive(c)) continue;
                if (std::string_view(part) == "mesh") Strip(c);  // the colliders and the attachment point keep theirs
                const Vec3 off = std::string_view(part) == "SlimeAttachmentPoint" ? Vec3{0.0f, 1.30f, 0.0f}
                                 : std::string_view(part) == "COL"                ? Vec3{0.0f, 0.05f, 0.0f}
                                                                                   : Vec3{0.0f, 0.0f, 0.0f};
                Place(c, {at.x + off.x, at.y + off.y, at.z + off.z}, {0, 0, 0, 1}, {1, 1, 1});
                unity::SetActive(c, true);
                Keep(rec, c);
                ++spot;
            }
        }
        what = std::format("{} part(s) hidden, mixer + 2 speakers, slime spot {}", rec.hidden.size(), spot == 3 ? "ready" : "missing");
    } else if (fam == kSpeaker) {
        static constexpr float kTilts[] = {kSpeakerSpec[0].tilt, kSpeakerSpec[1].tilt, kSpeakerSpec[2].tilt};
        const float tilt = kTilts[std::clamp(kind, 0, 2)];
        rec.model = Find(root, "model_boombox");
        if (rec.model) {
            Keep(rec, rec.model);
            Il2CppObject* tr = unity::TransformOf(rec.model);
            bool ok = false, ok2 = false;
            rec.modelRot = Value<Quat>(Call(trGetLocalRot, tr, {}, "Transform.get_localRotation", &ok), Quat{0, 0, 0, 1});
            rec.modelScale = Value<Vec3>(Call(trGetLocalScale, tr, {}, "Transform.get_localScale", &ok2), Vec3{1, 1, 1});
            if (ok && ok2) {
                Quat r = Mul(rec.modelRot, AxisX(-tilt));  // lean back: the front (+z) turns upwards
                Vec3 s{rec.modelScale.x * 1.25f, rec.modelScale.y * 1.25f, rec.modelScale.z * 1.25f};
                Call(trSetLocalRot, tr, {&r}, "Transform.set_localRotation");
                Call(trSetLocalScale, tr, {&s}, "Transform.set_localScale");
            } else {
                rec.model = nullptr;
            }
            if (Il2CppObject* a = InChildren(root, animatorClass)) {
                rec.animators.push_back(a);
                Keep(rec, a);
            }
            if (Il2CppObject* fx = Find(root, "model_boombox/bbox_base_jnt/bbox_head_jnt/FX_MusicalNotes")) {
                rec.fx.push_back(fx);
                rec.fxWas.push_back(ActiveSelf(fx));
                Keep(rec, fx);
            }
        }
        rec.gadget = unity::GetComponent(root, gadgetClass);
        if (rec.gadget) {
            Keep(rec, rec.gadget);
            rec.stackWas = unity::Field<bool>(rec.gadget, offStackTop);
            unity::Field<bool>(rec.gadget, offStackTop) = true;  // speakers (and decorations) stack on it
        }
        rec.player = unity::GetComponent(root, musicPlayerClass);
        if (rec.player) Keep(rec, rec.player);
        // The emitter sits on a child at the boombox's head.
        Il2CppObject* em = objectNew(gameObjectClass);
        bool ok = false;
        if (em) Call(goCtor, em, {unity::NewString("sr2dj_emitter")}, "GameObject..ctor", &ok);
        if (em && ok && unity::IsAlive(em)) {
            bool stays = false;
            Call(trSetParent, unity::TransformOf(em), {unity::TransformOf(holder), &stays}, "Transform.SetParent", &ok);
            Place(em, {0.0f, 1.3f, 0.0f}, {0, 0, 0, 1}, {1, 1, 1});
            rec.emitters.push_back(em);
            Keep(rec, em);
        }
        what = std::format("tilt {} deg, stacking on top {}", tilt, rec.gadget ? "on" : "unknown (no Gadget)");
    } else {
        HideVanilla(rec, {"model_nightlight"});
        Il2CppObject* radio = CloneBoombox(rec, holder, {0.0f, -0.5f, 0.0f}, 0.0f, 0.6f);
        if (radio) rec.emitters.push_back(radio);
        what = std::format("night light {}, radio {}", rec.hidden.empty() ? "not found" : "hidden", radio ? "built" : "missing");
    }
    if (Faulted()) return false;
    unity::SetActive(holder, true);
    const sdj::EmitterKind ek = fam == kBooth ? sdj::EmitterKind::kBooth : fam == kSpeaker ? sdj::EmitterKind::kSpeaker
                                                                                         : sdj::EmitterKind::kRadio;
    int registered = 0;
    for (Il2CppObject* e : rec.emitters)
        registered += sdj::RegisterEmitter(e, ek, fam == kRadio ? 0.6f : 1.0f, fam == kRadio ? 0.8f : 1.0f) ? 1 : 0;
    SR2_INFO("[{}] dj buildables: {} actor {} dressed ({}; {} emitter(s))", module, fams[fam].label, id, what, registered);
    return true;
}

// Vanilla parts back (restore) and our objects away. The GameObject may be gone already.
void Impl::Undress(Rec& rec, bool restore) {
    for (Il2CppObject* e : rec.emitters) sdj::UnregisterEmitter(e);
    if (restore && unity::IsAlive(rec.root)) {
        bool ok = false;
        for (Il2CppObject* go : rec.hidden)
            if (unity::IsAlive(go)) unity::SetActive(go, true);
        if (unity::IsAlive(rec.model)) {
            Il2CppObject* tr = unity::TransformOf(rec.model);
            Call(trSetLocalRot, tr, {&rec.modelRot}, "Transform.set_localRotation", &ok);
            Call(trSetLocalScale, tr, {&rec.modelScale}, "Transform.set_localScale", &ok);
        }
        if (unity::IsAlive(rec.gadget)) unity::Field<bool>(rec.gadget, offStackTop) = rec.stackWas;
        if (rec.fam == kSpeaker && rec.anim == 1) {  // the vanilla boombox stops dancing (unless its own track plays)
            const bool own = rec.player && unity::IsAlive(rec.player) && unity::Field<Il2CppObject*>(rec.player, offMusicInstance);
            if (!own) {
                int32_t h = animHash;
                bool off = false;
                for (Il2CppObject* a : rec.animators)
                    if (unity::IsAlive(a) && h) Call(animSetBool, a, {&h, &off}, "Animator.SetBool", &ok);
                for (size_t i = 0; i < rec.fx.size(); ++i)
                    if (unity::IsAlive(rec.fx[i])) unity::SetActive(rec.fx[i], rec.fxWas[i]);
            }
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

// Boomboxes dance and show their notes while a Slime DJ song plays.
void Impl::Animate() {
    const int want = sdj::SongPlaying() ? 1 : 0;
    for (auto& [id, rec] : recs) {
        if (rec.anim == want) continue;
        rec.anim = want;
        const bool own = rec.player && unity::IsAlive(rec.player) && unity::Field<Il2CppObject*>(rec.player, offMusicInstance);
        if (own) continue;  // the Boombox plays its own track: leave its animation alone
        bool on = want != 0, ok = false;
        int32_t h = animHash;
        for (Il2CppObject* a : rec.animators)
            if (unity::IsAlive(a) && h) Call(animSetBool, a, {&h, &on}, "Animator.SetBool", &ok);
        for (size_t i = 0; i < rec.fx.size(); ++i)
            if (unity::IsAlive(rec.fx[i])) unity::SetActive(rec.fx[i], on || rec.fxWas[i]);
        if (Faulted()) return;
    }
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
            if (f != kRadio) ++kits;
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
        if (listed != before) SetActors(f, listed);
        if (kits != kitsBefore) SetKits(f, kits);
    }
    Animate();
}

// `count` Night Light toys in front of the camera (InstantiationHelpers.InstantiateActor, as the sandbox
// spawner and the vacpack do), each one listed as a Slime Radio.
void Impl::SpawnRadios(int count) {
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
        SR2_WARN("[{}] dj buildables: {} Slime Radio(s) paid but not spawned (no scene group / camera / toy prefab)", module, count);
        return;
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
    SR2_INFO("[{}] dj buildables: {} Slime Radio(s) spawned in front of you ({} in this save)", module, spawned, listed.size());
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
    if (!why.empty()) return fail();
    for (size_t* o : {&p->offPcCurrency, &p->offPcIdent})  // value-type field offsets include the object header
        *o = *o >= 2 * sizeof(void*) ? *o - 2 * sizeof(void*) : *o;
    p->animHashField = R.Field(p->musicPlayerClass, "_gadgetActive", nullptr);  // optional (no dance without it)

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
    p->setFavorite = method(gadgetsModel, "SetFavorite", 2);
    p->setHeld = method(p->gadgetItemClass, "SetHeldGadget", 1);
    p->instantiateActor = method(helpers, "InstantiateActor", 10);
    p->currentGroup = method(regions, "get_CurrentSceneGroup", 0);
    p->actorIdOf = byParams(identifiable, "GetActorId", {"UnityEngine.GameObject"});
    if (!p->lsCtor || !p->fabItemCtor) why = "constructors not found";
    if (!why.empty()) return fail();
    g = p;
    variant_ext::Set(p);  // the Quantum Storage Link's detours ask us from now on
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
    p.seenIds.clear();
    p.stableTicks = 0;
    ++p.generation;
    p.registeredDir = nullptr;
}

bool Owns(Il2CppObject* def) { return TableIndex(def) >= 0; }

}  // namespace sr2::modules::custom_upgrades::dj
