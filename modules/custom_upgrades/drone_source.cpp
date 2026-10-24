// custom_upgrades: drone program sources "Hen Collector" and "Elder Collector" (see drone_source.h).
// SR2 1.3.0, all by name; RVAs for reference only.
//
// Findings (disassembly):
//   * Sources of a program: DroneTaskSourceType (int enum NONE..SPRINKLE_CANISTER = 10). The station
//     menu lists RanchDrone.GatherSourceTasks(target) (0xA1AA90): GatherSourceTask(type) (0xA19B40)
//     for ten fixed types, kept when every source program accepts the target. GatherSourceTask builds
//     a DroneTaskSource(type, IRanchDroneSourceProgram[]); COOP = [RanchDroneSourceProgramCoop,
//     RanchDroneSourceProgramElderCollector]; an unknown type throws ArgumentOutOfRangeException.
//     RanchDrone.Init calls GatherSourceTask(model._taskData.SourceType) for the saved program.
//   * The elder-collector part accepts every target, so Coop is offered for any target the coop's
//     live chickens or its collectors could hold (meats, elders, single hens / roosters / elders).
//   * UI: DroneProgramViewModel.GatherSourceOptions (0x821F80) turns every DroneTaskSource into a
//     DroneSourceOptionEntry with IDroneUIDefinitionSource.FindSourceDefinition(type) (0x83ABF0,
//     DroneUIDefinitionSource; null = the source is skipped), in list order (no sort there). The
//     definition is a plain DroneUIProgramDefinition<DroneTaskSourceType> {_type, _displayName
//     (LocalizedString), _icon, _sortIndex}; the drone list / read-only views use the same lookup.
//     Applying calls TryFindSourceProgram(type) (GatherSourceTasks again, match by SourceType) and
//     DroneStation.AssignRanchTask(target, source, sink, analytics) -> SetTaskData(new DroneTaskData
//     {TargetType, TargetIdentType, SourceType, SinkType}) + RanchDrone.AssignTask. The sink list
//     (GatherSinkTasks) only compares the source with SILO; enum ToString of an unknown value is a
//     number (analytics), nothing else switches on it.
//   * Save: GameModelPullHelpers.PullGadget calls DroneStationGadgetModel.Pull (0xA8D550), which hands
//     out the LIVE _taskData through an out parameter (a local of PullGadget), and converts it right
//     away into a new DroneTaskV01 {TargetType, TargetIdentTypeId, SourceType, SinkType}.
//   * RanchMetadata's LandPlotMetadata.elderCollectors of a coop: the deluxe coop's vanilla elder
//     collector (one StorageMetadata per slot, ElderGroup slots) plus the Hen Collector's slots the
//     coop tick appends (coop_world.cpp "Ranch drones"). A storage in the tick's Hen Collector list
//     is ours, every other one is the vanilla elder collector.
//
// What we do (two kinds, Hen Collector and Elder Collector, the same way):
//   * GatherSourceTask(our type): DroneTaskSource(our type, [a new RanchDroneSourceProgramElderCollector
//     of that drone]). Its GetSources (0xA31830, virtual, detoured) returns, for a drone whose station
//     program has our type, only that kind's slots (Hen Collector: the Hen Collector storages; Elder
//     Collector: every other elderCollectors storage) as a StorageMetadata[] (arrays are
//     IEnumerable<T>), filtered by the target predicate and ordered by count like vanilla. Live
//     chickens are not touched by either.
//   * GatherSourceTasks(target): our sources are inserted right after Coop (Hen, then Elder) when Coop
//     is offered, at least one storage of that kind exists and the target wants something the kind
//     holds: Hen Collector = the task config's MeatTypeGroup minus ElderGroup minus UnstableMeat (the
//     Hen Collector's slot rule), Elder Collector = ElderGroup (found once by name). So an elder target
//     never offers Hen Collector, and a hen target never offers Elder Collector.
//   * FindSourceDefinition(our type): a runtime copy of the Coop definition, type ours, name through
//     a new LocalizedString with Coop's table entry (text via core/text_override), icon = the Hen
//     (task config HenType.icon) / the elder hen (ElderGroup's "...Hen" member), sort index Coop + 1
//     for both (ties keep the list order: Coop, Hen Collector, Elder Collector).
//   * Pull: a program with our type goes to the save as a copy saying COOP (the live data keeps ours).
//     If the copy can't be made (no il2cpp_object_new, ...) the live data is set to COOP instead.
//   * Tick (1 Hz): stations from GameModel.identifiablesByIdent[DroneDirector._droneStationType]; the
//     sidecar values list the station actor ids per kind. First sight of a station after a load /
//     enable: listed + COOP -> back to that kind (DroneStation.AssignRanchTask when its drone exists,
//     else the data only; RanchDrone.Init then builds our source). Later: our type -> listed; listed
//     but no longer ours -> the player changed it, dropped; listed ids no station has any more (picked
//     up, a load of another save) -> dropped. No storage of the kind left -> Coop (logged). A Hen
//     Collector program whose target is elders (ELDERS, or SINGLE with an elder; possible before the
//     Elder Collector existed) becomes Elder Collector (or Coop without an elder collector).
//   * Module off / faulted: Revert writes COOP into every program that had ours (plain field writes,
//     any thread) and, on the main thread without a fault, gives the running drones the Coop program.

#include "modules/custom_upgrades/drone_source.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <format>
#include <initializer_list>
#include <mutex>
#include <set>
#include <sstream>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

#include "core/detour_guard.h"
#include "core/il2cpp_api.h"
#include "core/log.h"
#include "core/main_thread.h"
#include "core/module.h"
#include "core/resolver.h"
#include "core/text_override.h"
#include "core/unity.h"
#include "modules/custom_upgrades/api.h"

namespace sr2::modules::custom_upgrades::drones {
namespace {

namespace unity = sr2::core::unity;
namespace main_thread = sr2::core::main_thread;
using sr2::il2cpp::api;
using sr2::il2cpp::Resolver;

constexpr const char* kImage = "Assembly-CSharp";
constexpr const char* kDroneNs = "MonomiPark.SlimeRancher.Drone";
constexpr const char* kDataNs = "MonomiPark.SlimeRancher.DataModel";
constexpr int32_t kSourceCoop = 5;    // DroneTaskSourceType.COOP
constexpr int32_t kTargetSingle = 1;  // DroneTaskTargetType.SINGLE
constexpr int32_t kTargetElders = 6;  // DroneTaskTargetType.ELDERS

// Our sources. Index = kind; -1 = none (Coop / anything vanilla).
constexpr int kHen = 0, kElder = 1, kKinds = 2;
struct KindInfo {
    int32_t type;
    const char* key;   // sidecar value: station actor ids, comma separated
    const char* name;  // shown name
};
constexpr KindInfo kKind[kKinds] = {
    {kHenSourceType, "drone_hen_source.stations", "Hen Collector"},
    {kElderSourceType, "drone_elder_source.stations", "Elder Collector"},
};
int KindOf(int32_t type) { return type == kHenSourceType ? kHen : type == kElderSourceType ? kElder : -1; }
const char* NameOf(int kind) { return kind >= 0 && kind < kKinds ? kKind[kind].name : "Coop"; }

template <class T>
T Unbox(Il2CppObject* boxed, T def = T{}) {
    return boxed ? *reinterpret_cast<T*>(reinterpret_cast<char*>(boxed) + 2 * sizeof(void*)) : def;
}
void* UnboxPtr(Il2CppObject* boxed) { return boxed ? reinterpret_cast<char*>(boxed) + 2 * sizeof(void*) : nullptr; }

bool IsA(Il2CppObject* obj, Il2CppClass* k) {
    const auto& a = api();
    if (!obj || !k || !a.il2cpp_object_get_class) return false;
    Il2CppClass* c = a.il2cpp_object_get_class(obj);
    if (c == k) return true;
    return c && a.il2cpp_class_is_assignable_from && a.il2cpp_class_is_assignable_from(k, c);
}

using GatherSourceFn = Il2CppObject* (*)(Il2CppObject*, int32_t, const MethodInfo*);
using GatherSourcesFn = Il2CppObject* (*)(Il2CppObject*, Il2CppObject*, const MethodInfo*);
using GetSourcesFn = Il2CppObject* (*)(Il2CppObject*, Il2CppObject*, const MethodInfo*);
using FindDefFn = Il2CppObject* (*)(Il2CppObject*, int32_t, const MethodInfo*);
using PullFn = void (*)(Il2CppObject*, double*, bool*, int8_t*, void**, void**, Il2CppObject**, double*, int32_t*,
                        Il2CppObject*, const MethodInfo*);
GatherSourceFn g_origGatherSource = nullptr;
GatherSourcesFn g_origGatherSources = nullptr;
GetSourcesFn g_origGetSources = nullptr;
FindDefFn g_origFindDef = nullptr;
PullFn g_origPull = nullptr;

struct Impl {
    const char* module = "custom_upgrades";

    // classes
    Il2CppClass* droneClass = nullptr, *stationModelClass = nullptr, *taskDataClass = nullptr, *taskSourceClass = nullptr;
    Il2CppClass* programIface = nullptr, *elderClass = nullptr, *plotMetaClass = nullptr, *storageMetaClass = nullptr;
    Il2CppClass* taskTargetClass = nullptr, *lsClass = nullptr, *typeGroupClass = nullptr, *identClass = nullptr;
    // offsets
    size_t offDroneModel = 0, offDroneTime = 0, offDroneRanch = 0, offDroneMove = 0, offDroneCfg = 0, offDroneStation = 0;
    size_t offModelTask = 0, offActorId = 0;
    size_t offTdTarget = 0, offTdIdent = 0, offTdSource = 0, offTdSink = 0;
    size_t offSrcType = 0, offProgDrone = 0, offProgRanch = 0;
    size_t offRmPlots = 0, offLpmElder = 0, offSmStorage = 0;
    size_t offCfgHen = 0, offCfgMeat = 0, offIdentIcon = 0, offGroupMembers = 0, offTargetPred = 0;
    size_t offScGame = 0, offScDrones = 0, offByIdent = 0, offStationType = 0;
    // methods
    const MethodInfo* elderCtor = nullptr, *srcCtor = nullptr, *smGetId = nullptr, *smGetCount = nullptr;
    const MethodInfo* targetCtor = nullptr, *gatherSourceMi = nullptr, *gatherSinkMi = nullptr, *assignMi = nullptr;
    const MethodInfo* sceneInst = nullptr, *lsCtor = nullptr, *lsGetTable = nullptr, *lsGetEntry = nullptr;
    using ObjectNewFn = Il2CppObject* (*)(Il2CppClass*);
    using ArrayNewFn = Il2CppObject* (*)(Il2CppClass*, uintptr_t);
    using WBarrierFn = void (*)(Il2CppObject*, void**, void*);
    ObjectNewFn objectNew = nullptr;
    ArrayNewFn arrayNew = nullptr;
    WBarrierFn wbarrier = nullptr;

    // state (main thread unless noted)
    std::vector<Il2CppObject*> henStorages;  // identities of this tick
    bool hooked = false;                     // every hook made (Setup)
    bool available[kKinds] = {};             // a storage of that kind exists (elder: as of the last look)
    Il2CppObject* ranch = nullptr;           // RanchMetadata of the last drone seen (Unity object, IsAlive first)
    std::unordered_map<int64_t, Il2CppObject*> drones;  // station id -> drone (identity, verified before use)
    struct Known {
        Il2CppObject* model = nullptr;  // pinned by h
        Il2CppGCHandle h = nullptr;
    };
    std::mutex mu;                               // known / pending (Revert from any thread)
    std::unordered_map<int64_t, Known> known;    // station models that carried one of our types
    std::unordered_set<int64_t> pending;         // reverted without game calls: drone still on our program
    std::unordered_set<int64_t> seen;            // stations looked at since the load / enable
    const void* seenGame = nullptr;
    std::atomic<bool> resetSeen{false};
    Il2CppObject* definition[kKinds] = {};       // our source definitions (pinned, session)
    size_t offDefIcon = 0;
    Il2CppObject* icon[kKinds] = {};             // Hen / elder hen sprite (pinned, session)
    bool iconSet[kKinds] = {};                   // the definition has its own icon
    // Chicken types (pinned, session): elders = ElderGroup; hens = MeatTypeGroup - elders - UnstableMeat.
    std::vector<Il2CppObject*> elderTypes, henTypes;
    bool elderTried = false, henTried = false;
    const MethodInfo* listInsert = nullptr;
    const void* listInsertClass = nullptr;
    const MethodInfo* predInvoke = nullptr;
    const void* predInvokeClass = nullptr;
    const MethodInfo* tryGetValue = nullptr;
    const void* tryGetValueClass = nullptr;
    bool loggedBuildFail = false, loggedDefFail[kKinds] = {}, loggedPullFallback = false;

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
                sr2::core::MarkModuleFaulted(module, std::format("{}: {}", what, err));
            else SR2_WARN("[{}] drone source: {} failed: {}", module, what, err);
        }
        return r;
    }
    void SetRef(Il2CppObject* obj, void** slot, Il2CppObject* value) {
        if (wbarrier) wbarrier(obj, slot, value);
        else *slot = value;
    }
    static bool Faulted(const char* m) { return sr2::core::ModuleFaulted(m); }
    int64_t StationId(Il2CppObject* model) const {
        return IsA(model, stationModelClass) ? unity::Field<int64_t>(model, offActorId) : 0;
    }
    Il2CppObject* TaskOf(Il2CppObject* model) const {
        Il2CppObject* t = IsA(model, stationModelClass) ? unity::Field<Il2CppObject*>(model, offModelTask) : nullptr;
        return IsA(t, taskDataClass) ? t : nullptr;
    }
    bool IsHenStorage(Il2CppObject* storage) const {
        return std::find(henStorages.begin(), henStorages.end(), storage) != henStorages.end();
    }

    void NoteKnown(Il2CppObject* model);
    void EnsureElderTypes();
    void EnsureHenTypes(Il2CppObject* cfg);
    void NoteDrone(Il2CppObject* drone);
    bool Wants(Il2CppObject* predicate, const std::vector<Il2CppObject*>& types);
    int ElderStorages();
    bool ElderTarget(Il2CppObject* task);
    Il2CppObject* BuildSource(Il2CppObject* drone, int kind);
    void AddOption(Il2CppObject* drone, Il2CppObject* target, Il2CppObject* list);
    int ProgramKind(Il2CppObject* program);
    Il2CppObject* Sources(Il2CppObject* program, Il2CppObject* predicate, int kind);
    void SetIcon(int kind);
    Il2CppObject* Definition(Il2CppObject* defSource, const MethodInfo* mi, int kind);
    Il2CppObject* CopyAsCoop(Il2CppObject* data);
    bool Switch(Il2CppObject* model, int64_t id, int kind, const char* why);
    bool Stations(std::vector<Il2CppObject*>& out);
};

Impl* g = nullptr;
sr2::core::DetourGuard g_guard{"custom_upgrades", "drone sources Hen / Elder Collector"};

// ---------------------------------------------------------------- detours --

Il2CppObject* GatherSourceDetour(Il2CppObject* self, int32_t type, const MethodInfo* mi) {
    const int kind = KindOf(type);
    if (!g || !main_thread::IsMainThread()) return g_origGatherSource(self, kind >= 0 ? kSourceCoop : type, mi);
    if (kind < 0) {
        Il2CppObject* r = g_origGatherSource(self, type, mi);
        g_guard.Run([&] { g->NoteDrone(self); });  // remember the station's drone (for switching a loaded program)
        return r;
    }
    Il2CppObject* r = nullptr;
    if (!Impl::Faulted(g->module)) g_guard.Run([&] { r = g->BuildSource(self, kind); });
    if (r) return r;
    if (!g->loggedBuildFail)
        SR2_WARN("[{}] drone source {} could not be built - the drone uses Coop", g->module, NameOf(kind));
    g->loggedBuildFail = true;
    return g_origGatherSource(self, kSourceCoop, mi);
}

Il2CppObject* GatherSourcesDetour(Il2CppObject* self, Il2CppObject* target, const MethodInfo* mi) {
    Il2CppObject* list = g_origGatherSources(self, target, mi);
    if (!list || !g || !main_thread::IsMainThread() || Impl::Faulted(g->module)) return list;
    g_guard.Run([&] { g->AddOption(self, target, list); });
    return list;
}

Il2CppObject* GetSourcesDetour(Il2CppObject* self, Il2CppObject* predicate, const MethodInfo* mi) {
    if (!g || !main_thread::IsMainThread() || Impl::Faulted(g->module)) return g_origGetSources(self, predicate, mi);
    Il2CppObject* r = nullptr;
    int kind = -1;
    g_guard.Run([&] {
        kind = g->ProgramKind(self);
        if (kind >= 0) r = g->Sources(self, predicate, kind);
    });
    return kind >= 0 && r ? r : g_origGetSources(self, predicate, mi);
}

Il2CppObject* FindDefDetour(Il2CppObject* self, int32_t type, const MethodInfo* mi) {
    const int kind = KindOf(type);
    if (kind < 0 || !g || !main_thread::IsMainThread()) return g_origFindDef(self, kind >= 0 ? kSourceCoop : type, mi);
    Il2CppObject* r = nullptr;
    if (!Impl::Faulted(g->module)) g_guard.Run([&] { r = g->Definition(self, mi, kind); });
    return r ? r : g_origFindDef(self, kSourceCoop, mi);
}

// Never lets our types into the save: a copy saying COOP, or (no copy possible) the live data set to
// COOP. Plain memory work only besides il2cpp_object_new, so it also runs after a fault.
void PullDetour(Il2CppObject* self, double* a, bool* b, int8_t* c, void** d, void** e, Il2CppObject** taskData,
                double* f, int32_t* h, Il2CppObject* lookups, const MethodInfo* mi) {
    g_origPull(self, a, b, c, d, e, taskData, f, h, lookups, mi);
    if (!g || !taskData || !*taskData || !g->offTdSource) return;
    Il2CppObject* data = *taskData;
    if (KindOf(unity::Field<int32_t>(data, g->offTdSource)) < 0) return;
    if (Il2CppObject* copy = g->CopyAsCoop(data)) {
        *taskData = copy;  // PullGadget's local; converted into a DroneTaskV01 right after
        return;
    }
    unity::Field<int32_t>(data, g->offTdSource) = kSourceCoop;
    if (!g->loggedPullFallback)
        SR2_WARN("[{}] drone source: save copy failed - program saved and kept as Coop", g->module);
    g->loggedPullFallback = true;
}

// ---------------------------------------------------------------- impl --

void Impl::NoteKnown(Il2CppObject* model) {
    const int64_t id = StationId(model);
    if (!id) return;
    std::lock_guard lk(mu);
    auto it = known.find(id);
    if (it != known.end() && it->second.model == model) return;
    Il2CppGCHandle old = it != known.end() ? it->second.h : nullptr;
    Il2CppGCHandle h = unity::NewGCHandle(model);
    if (!h) return;
    known[id] = {model, h};
    if (old) main_thread::Post([old] { unity::FreeGCHandle(old); });
}

// ElderGroup's members, once per session (one FindObjectsOfTypeAll, like coop_world's MeatGroup), and
// the elder hen's icon.
void Impl::EnsureElderTypes() {
    if (elderTried) return;
    elderTried = true;
    Il2CppObject* group = nullptr;
    for (Il2CppObject* gr : unity::FindObjectsOfTypeAll(typeGroupClass))
        if (unity::IsAlive(gr) && unity::GetName(gr) == "ElderGroup") {
            group = gr;
            break;
        }
    void* list = group ? unity::Field<void*>(group, offGroupMembers) : nullptr;
    std::string names;
    for (size_t i = 0, n = unity::ListCount(list); i < n && i < 32; ++i) {
        auto* t = static_cast<Il2CppObject*>(unity::ListAt(list, i));
        if (!unity::IsAlive(t) || !IsA(t, identClass) || !unity::NewGCHandle(t)) continue;
        elderTypes.push_back(t);
        const std::string nm = unity::GetName(t);
        names += (names.empty() ? "" : ", ") + nm;
        Il2CppObject* sprite = unity::Field<Il2CppObject*>(t, offIdentIcon);
        const bool hen = nm.find("Hen") != std::string::npos;
        if (unity::IsAlive(sprite) && (!icon[kElder] || hen) && unity::NewGCHandle(sprite)) icon[kElder] = sprite;
    }
    if (elderTypes.empty()) SR2_WARN("[{}] drone source: ElderGroup not found - Elder Collector is never offered", module);
    else SR2_INFO("[{}] drone source: elder types for Elder Collector: {}", module, names);
}

// The Hen Collector's slot rule (coop_world.cpp): MeatTypeGroup minus elders minus UnstableMeat.
void Impl::EnsureHenTypes(Il2CppObject* cfg) {
    if (henTried || !cfg) return;
    Il2CppObject* meat = unity::Field<Il2CppObject*>(cfg, offCfgMeat);
    if (!unity::IsAlive(meat)) return;
    henTried = true;
    EnsureElderTypes();
    void* list = unity::Field<void*>(meat, offGroupMembers);
    for (size_t i = 0, n = unity::ListCount(list); i < n && i < 64; ++i) {
        auto* t = static_cast<Il2CppObject*>(unity::ListAt(list, i));
        if (!unity::IsAlive(t) || !IsA(t, identClass)) continue;
        if (std::find(elderTypes.begin(), elderTypes.end(), t) != elderTypes.end()) continue;
        if (unity::GetName(t).find("Unstable") != std::string::npos) continue;
        if (unity::NewGCHandle(t)) henTypes.push_back(t);
    }
    if (henTypes.empty()) SR2_WARN("[{}] drone source: no adult chicken types - Hen Collector is offered for every Coop target", module);
}

void Impl::NoteDrone(Il2CppObject* drone) {
    if (!IsA(drone, droneClass)) return;
    const int64_t id = StationId(unity::Field<Il2CppObject*>(drone, offDroneModel));
    if (id) drones[id] = drone;
    Il2CppObject* rm = unity::Field<Il2CppObject*>(drone, offDroneRanch);
    if (unity::IsAlive(rm)) ranch = rm;
}

// Does the target predicate want any of `types`? (No predicate / no types known: yes.)
bool Impl::Wants(Il2CppObject* predicate, const std::vector<Il2CppObject*>& types) {
    if (!predicate || types.empty()) return true;
    Il2CppClass* pk = api().il2cpp_object_get_class(predicate);
    if (pk != predInvokeClass) {
        std::string err;
        predInvoke = Resolver::Get().Method(pk, "Invoke", 1, &err);
        predInvokeClass = pk;
    }
    if (!predInvoke) return true;
    for (Il2CppObject* t : types) {
        bool ok = false;
        if (Unbox<bool>(Call(predInvoke, predicate, {t}, "Predicate.Invoke", &ok)) && ok) return true;
        if (!ok) return true;
    }
    return false;
}

// Vanilla elder collector slots on the ranch: count, or -1 = unknown (no ranch seen yet).
int Impl::ElderStorages() {
    if (!unity::IsAlive(ranch)) return -1;
    void* plots = unity::Field<void*>(ranch, offRmPlots);
    int n = 0;
    for (size_t i = 0, c = unity::ListCount(plots); i < c && i < 256; ++i) {
        auto* meta = static_cast<Il2CppObject*>(unity::ListAt(plots, i));
        if (!IsA(meta, plotMetaClass)) continue;
        void* arr = unity::Field<void*>(meta, offLpmElder);
        for (size_t k = 0, m = arr ? unity::ArrayLength(arr) : 0; k < m && k < 64; ++k) {
            auto* sm = static_cast<Il2CppObject*>(unity::ArrayAt(arr, k));
            Il2CppObject* storage = sm ? unity::Field<Il2CppObject*>(sm, offSmStorage) : nullptr;
            if (storage && !IsHenStorage(storage) && unity::IsAlive(storage)) ++n;
        }
    }
    return n;
}

// The program's target is elders (ELDERS, or SINGLE with an elder type).
bool Impl::ElderTarget(Il2CppObject* task) {
    const int32_t tt = unity::Field<int32_t>(task, offTdTarget);
    if (tt == kTargetElders) return true;
    if (tt != kTargetSingle) return false;
    EnsureElderTypes();
    Il2CppObject* ident = unity::Field<Il2CppObject*>(task, offTdIdent);
    return ident && std::find(elderTypes.begin(), elderTypes.end(), ident) != elderTypes.end();
}

Il2CppObject* Impl::BuildSource(Il2CppObject* drone, int kind) {
    if (!IsA(drone, droneClass) || !unity::IsAlive(drone)) return nullptr;
    Il2CppObject* model = unity::Field<Il2CppObject*>(drone, offDroneModel);
    Il2CppObject* prog = objectNew(elderClass);
    if (!prog) return nullptr;
    bool ok = false;
    Call(elderCtor, prog,
         {drone, unity::Field<Il2CppObject*>(drone, offDroneTime), unity::Field<Il2CppObject*>(drone, offDroneRanch),
          unity::Field<Il2CppObject*>(drone, offDroneMove), model},
         "RanchDroneSourceProgramElderCollector..ctor", &ok);
    if (!ok) return nullptr;
    Il2CppObject* arr = arrayNew(programIface, 1);
    Il2CppObject** data = unity::ArrayData<Il2CppObject*>(arr);
    if (!arr || !data) return nullptr;
    SetRef(arr, reinterpret_cast<void**>(&data[0]), prog);
    Il2CppObject* src = objectNew(taskSourceClass);
    int32_t type = kKind[kind].type;
    if (src) Call(srcCtor, src, {&type, arr}, "DroneTaskSource..ctor", &ok);
    if (!src || !ok) return nullptr;
    // The chicken icon: the task config's Hen; the elder hen comes from ElderGroup.
    if (kind == kHen && !icon[kHen]) {
        Il2CppObject* cfg = unity::Field<Il2CppObject*>(drone, offDroneCfg);
        Il2CppObject* hen = cfg ? unity::Field<Il2CppObject*>(cfg, offCfgHen) : nullptr;
        Il2CppObject* henSprite = unity::IsAlive(hen) ? unity::Field<Il2CppObject*>(hen, offIdentIcon) : nullptr;
        if (unity::IsAlive(henSprite) && unity::NewGCHandle(henSprite)) icon[kHen] = henSprite;
    }
    if (kind == kElder) EnsureElderTypes();
    SetIcon(kind);
    NoteKnown(model);  // it may be applied: Revert must know the model
    NoteDrone(drone);
    return src;
}

void Impl::AddOption(Il2CppObject* drone, Il2CppObject* target, Il2CppObject* list) {
    if (!hooked || !IsA(drone, droneClass)) return;  // all hooks (the Pull one too) or no option
    NoteDrone(drone);
    available[kElder] = ElderStorages() > 0;
    if (!available[kHen] && !available[kElder]) return;
    const size_t n = unity::ListCount(list);
    int32_t coop = -1;
    for (size_t i = 0; i < n && i < 64; ++i) {
        auto* s = static_cast<Il2CppObject*>(unity::ListAt(list, i));
        if (!IsA(s, taskSourceClass)) continue;
        const int32_t t = unity::Field<int32_t>(s, offSrcType);
        if (KindOf(t) >= 0) return;  // already added
        if (t == kSourceCoop) coop = static_cast<int32_t>(i);
    }
    if (coop < 0) return;  // Coop not offered for this target: no chickens wanted
    Il2CppObject* pred = IsA(target, taskTargetClass) ? unity::Field<Il2CppObject*>(target, offTargetPred) : nullptr;
    EnsureHenTypes(unity::Field<Il2CppObject*>(drone, offDroneCfg));
    EnsureElderTypes();
    Il2CppClass* lk = api().il2cpp_object_get_class(list);
    if (lk != listInsertClass) {
        std::string err;
        listInsert = Resolver::Get().Method(lk, "Insert", 2, &err);
        listInsertClass = lk;
    }
    if (!listInsert) return;
    int32_t at = coop + 1;
    for (int kind = 0; kind < kKinds; ++kind) {
        if (!available[kind]) continue;
        // Elder Collector needs known elder types (else it would show for every Coop target).
        if (kind == kElder && elderTypes.empty()) continue;
        if (!Wants(pred, kind == kHen ? henTypes : elderTypes)) continue;
        Il2CppObject* src = BuildSource(drone, kind);
        if (!src) continue;
        bool ok = false;
        Call(listInsert, list, {&at, src}, "List<DroneTaskSource>.Insert", &ok);
        if (ok) ++at;
    }
}

int Impl::ProgramKind(Il2CppObject* program) {
    if (!IsA(program, elderClass)) return -1;
    Il2CppObject* drone = unity::Field<Il2CppObject*>(program, offProgDrone);
    if (!IsA(drone, droneClass)) return -1;
    Il2CppObject* task = TaskOf(unity::Field<Il2CppObject*>(drone, offDroneModel));
    return task ? KindOf(unity::Field<int32_t>(task, offTdSource)) : -1;
}

Il2CppObject* Impl::Sources(Il2CppObject* program, Il2CppObject* predicate, int kind) {
    struct E {
        Il2CppObject* sm;
        int32_t count;
    };
    std::vector<E> found;
    Il2CppObject* rm = unity::Field<Il2CppObject*>(program, offProgRanch);
    void* plots = rm ? unity::Field<void*>(rm, offRmPlots) : nullptr;
    if (predicate) {
        Il2CppClass* pk = api().il2cpp_object_get_class(predicate);
        if (pk != predInvokeClass) {
            std::string err;
            predInvoke = Resolver::Get().Method(pk, "Invoke", 1, &err);
            predInvokeClass = pk;
        }
    }
    for (size_t i = 0, n = unity::ListCount(plots); i < n && i < 256; ++i) {
        auto* meta = static_cast<Il2CppObject*>(unity::ListAt(plots, i));
        if (!IsA(meta, plotMetaClass)) continue;
        void* arr = unity::Field<void*>(meta, offLpmElder);
        for (size_t k = 0, m = arr ? unity::ArrayLength(arr) : 0; k < m && k < 64; ++k) {
            auto* sm = static_cast<Il2CppObject*>(unity::ArrayAt(arr, k));
            Il2CppObject* storage = sm ? unity::Field<Il2CppObject*>(sm, offSmStorage) : nullptr;
            // Hen Collector: only our storages; Elder Collector: only the vanilla ones.
            if (!storage || IsHenStorage(storage) != (kind == kHen) || !unity::IsAlive(storage)) continue;
            bool ok = false;
            Il2CppObject* id = Call(smGetId, sm, {}, "StorageMetadata.get_id", &ok);
            if (!ok) continue;
            if (predicate && predInvoke) {
                const bool want = Unbox<bool>(Call(predInvoke, predicate, {id}, "Predicate.Invoke", &ok));
                if (!ok || !want) continue;
            }
            const int32_t count = Unbox<int32_t>(Call(smGetCount, sm, {}, "StorageMetadata.get_count", &ok));
            found.push_back({sm, ok ? count : 0});
        }
    }
    std::stable_sort(found.begin(), found.end(), [](const E& a, const E& b) { return a.count > b.count; });
    Il2CppObject* out = arrayNew(storageMetaClass, found.size());
    Il2CppObject** data = unity::ArrayData<Il2CppObject*>(out);
    if (!out || (!data && !found.empty())) return nullptr;
    for (size_t i = 0; i < found.size(); ++i) SetRef(out, reinterpret_cast<void**>(&data[i]), found[i].sm);
    return out;
}

// The definition's icon: the kind's chicken sprite as soon as it is known (until then Coop's).
void Impl::SetIcon(int kind) {
    Il2CppObject* d = definition[kind];
    if (iconSet[kind] || !d || !offDefIcon || !icon[kind]) return;
    SetRef(d, reinterpret_cast<void**>(reinterpret_cast<char*>(d) + offDefIcon), icon[kind]);
    iconSet[kind] = true;
    SR2_INFO("[{}] drone source: {} icon = the {} icon", module, kKind[kind].name, kind == kHen ? "Hen" : "Elder Hen");
}

Il2CppObject* Impl::Definition(Il2CppObject* defSource, const MethodInfo* mi, int kind) {
    if (definition[kind]) {
        if (kind == kElder) EnsureElderTypes();
        SetIcon(kind);
        return definition[kind];
    }
    auto fail = [&](const char* what) -> Il2CppObject* {
        if (!loggedDefFail[kind]) SR2_WARN("[{}] drone source: {} definition not built ({})", module, kKind[kind].name, what);
        loggedDefFail[kind] = true;
        return nullptr;
    };
    if (loggedDefFail[kind]) return nullptr;
    Il2CppObject* tmpl = g_origFindDef(defSource, kSourceCoop, mi);
    if (!tmpl) return fail("no Coop definition");
    Il2CppClass* k = api().il2cpp_object_get_class(tmpl);
    Resolver& R = Resolver::Get();
    std::string err;
    auto oType = R.FieldOffset(k, "_type", &err), oName = R.FieldOffset(k, "_displayName", &err),
         oIcon = R.FieldOffset(k, "_icon", &err), oSort = R.FieldOffset(k, "_sortIndex", &err);
    if (!oType || !oName || !oIcon || !oSort) return fail("definition fields");
    Il2CppObject* tmplName = unity::Field<Il2CppObject*>(tmpl, *oName);
    if (!IsA(tmplName, lsClass)) return fail("Coop name is no LocalizedString");
    // A LocalizedString of our own (Coop's table entry); its text comes from text_override.
    bool ok = false;
    Il2CppObject* table = Call(lsGetTable, tmplName, {}, "LocalizedString.get_TableReference", &ok);
    Il2CppObject* entry = ok ? Call(lsGetEntry, tmplName, {}, "LocalizedString.get_TableEntryReference", &ok) : nullptr;
    Il2CppObject* name = ok && table && entry ? objectNew(lsClass) : nullptr;
    if (name) Call(lsCtor, name, {UnboxPtr(table), UnboxPtr(entry)}, "LocalizedString..ctor", &ok);
    if (!name || !ok) return fail("LocalizedString");
    Il2CppObject* d = objectNew(k);
    if (!d || !unity::NewGCHandle(d)) return fail("object");
    char* base = reinterpret_cast<char*>(d);
    *reinterpret_cast<int32_t*>(base + *oType) = kKind[kind].type;
    // Coop + 1 for both: ties keep the list order (Coop, Hen Collector, Elder Collector).
    *reinterpret_cast<int32_t*>(base + *oSort) = unity::Field<int32_t>(tmpl, *oSort) + 1;
    SetRef(d, reinterpret_cast<void**>(base + *oName), name);
    SetRef(d, reinterpret_cast<void**>(base + *oIcon), unity::Field<Il2CppObject*>(tmpl, *oIcon));
    sr2::core::text_override::Set(name, kKind[kind].name);
    offDefIcon = *oIcon;
    definition[kind] = d;
    SR2_INFO("[{}] drone source: \"{}\" definition built from Coop{}", module, kKind[kind].name,
             sr2::core::text_override::SinkActive() ? "" : " (text sink off: shows Coop's name)");
    if (kind == kElder) EnsureElderTypes();
    SetIcon(kind);
    return d;
}

Il2CppObject* Impl::CopyAsCoop(Il2CppObject* data) {
    if (!objectNew || !taskDataClass) return nullptr;
    Il2CppObject* copy = objectNew(taskDataClass);
    if (!copy) return nullptr;
    unity::Field<int32_t>(copy, offTdTarget) = unity::Field<int32_t>(data, offTdTarget);
    SetRef(copy, reinterpret_cast<void**>(reinterpret_cast<char*>(copy) + offTdIdent),
           unity::Field<Il2CppObject*>(data, offTdIdent));
    unity::Field<int32_t>(copy, offTdSource) = kSourceCoop;
    unity::Field<int32_t>(copy, offTdSink) = unity::Field<int32_t>(data, offTdSink);
    return copy;
}

// The station's program to one of our sources (kind >= 0) or to Coop (-1). With its drone: the game's
// own AssignRanchTask (new task data + the drone's programs); without (station not loaded): the data
// only, RanchDrone.Init builds the source from it later.
bool Impl::Switch(Il2CppObject* model, int64_t id, int kind, const char* why) {
    Il2CppObject* task = TaskOf(model);
    if (!task) return false;
    Il2CppObject* drone = nullptr;
    if (auto it = drones.find(id); it != drones.end()) drone = it->second;
    if (drone && (!unity::IsAlive(drone) || !IsA(drone, droneClass) ||
                  unity::Field<Il2CppObject*>(drone, offDroneModel) != model))
        drone = nullptr;
    Il2CppObject* station = drone ? unity::Field<Il2CppObject*>(drone, offDroneStation) : nullptr;
    bool assigned = false;
    if (station && unity::IsAlive(station)) {
        int32_t tt = unity::Field<int32_t>(task, offTdTarget), sk = unity::Field<int32_t>(task, offTdSink);
        Il2CppObject* ident = unity::Field<Il2CppObject*>(task, offTdIdent);
        bool ok = false;
        Il2CppObject* target = objectNew(taskTargetClass);
        if (target) Call(targetCtor, target, {unity::Field<Il2CppObject*>(drone, offDroneCfg), &tt, ident}, "DroneTaskTarget..ctor", &ok);
        int32_t coop = kSourceCoop;
        Il2CppObject* source = !ok ? nullptr
                               : kind >= 0 ? BuildSource(drone, kind)
                                           : Call(gatherSourceMi, drone, {&coop}, "RanchDrone.GatherSourceTask", &ok);
        Il2CppObject* sink = ok && source ? Call(gatherSinkMi, drone, {&sk}, "RanchDrone.GatherSinkTask", &ok) : nullptr;
        bool analytics = false;
        if (ok && source && sink) Call(assignMi, station, {target, source, sink, &analytics}, "DroneStation.AssignRanchTask", &ok);
        assigned = ok && source && sink;
    }
    if (!assigned) {
        Il2CppObject* live = TaskOf(model);  // AssignRanchTask may have replaced it
        if (live) unity::Field<int32_t>(live, offTdSource) = kind >= 0 ? kKind[kind].type : kSourceCoop;
    }
    if (kind >= 0) NoteKnown(model);
    SR2_INFO("[{}] drone source: station {} program -> {} ({}; {})", module, id, NameOf(kind), why,
             assigned ? "drone re-programmed" : "program data only");
    return true;
}

bool Impl::Stations(std::vector<Il2CppObject*>& out) {
    out.clear();
    bool ok = false;
    Il2CppObject* sc = Call(sceneInst, nullptr, {}, "SceneContext.Instance", &ok);
    if (!ok || !unity::IsAlive(sc)) return false;
    Il2CppObject* gm = unity::Field<Il2CppObject*>(sc, offScGame);
    Il2CppObject* dd = unity::Field<Il2CppObject*>(sc, offScDrones);
    if (!gm || !unity::IsAlive(dd)) return false;
    if (gm != seenGame) {  // another game: everything is new
        seen.clear();
        drones.clear();
        seenGame = gm;
    }
    Il2CppObject* type = unity::Field<Il2CppObject*>(dd, offStationType);
    Il2CppObject* dict = unity::Field<Il2CppObject*>(gm, offByIdent);
    if (!type || !dict) return false;
    Il2CppClass* dk = api().il2cpp_object_get_class(dict);
    if (dk != tryGetValueClass) {
        std::string err;
        tryGetValue = Resolver::Get().Method(dk, "TryGetValue", 2, &err);
        tryGetValueClass = dk;
    }
    if (!tryGetValue) return false;
    Il2CppObject* list = nullptr;
    const bool found = Unbox<bool>(Call(tryGetValue, dict, {type, &list}, "identifiablesByIdent.TryGetValue", &ok));
    if (!ok) return false;
    for (size_t i = 0, n = found && list ? unity::ListCount(list) : 0; i < n && i < 256; ++i) {
        auto* m = static_cast<Il2CppObject*>(unity::ListAt(list, i));
        if (IsA(m, stationModelClass)) out.push_back(m);
    }
    return true;  // also with no station at all
}

std::set<int64_t> ParseIds(const std::string& s) {
    std::set<int64_t> out;
    std::stringstream ss(s);
    std::string part;
    while (std::getline(ss, part, ',')) {
        try {
            if (!part.empty()) out.insert(std::stoll(part));
        } catch (...) {
        }
    }
    return out;
}

std::string JoinIds(const std::set<int64_t>& ids) {
    std::string s;
    for (int64_t id : ids) s += (s.empty() ? "" : ",") + std::to_string(id);
    return s;
}

}  // namespace

// ---------------------------------------------------------------- setup --

bool HenSource::Setup(sr2::core::ModuleContext& ctx, const char* module, std::string& why, std::string& report) {
    Resolver& R = ctx.resolver();
    auto* p = new Impl();
    p->module = module;
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
    auto fail = [&] {
        delete p;
        return false;
    };
    HMODULE ga = api().module;
    p->objectNew = reinterpret_cast<Impl::ObjectNewFn>(ga ? GetProcAddress(ga, "il2cpp_object_new") : nullptr);
    p->arrayNew = reinterpret_cast<Impl::ArrayNewFn>(ga ? GetProcAddress(ga, "il2cpp_array_new") : nullptr);
    p->wbarrier = reinterpret_cast<Impl::WBarrierFn>(ga ? GetProcAddress(ga, "il2cpp_gc_wbarrier_set_field") : nullptr);
    if (!p->objectNew || !p->arrayNew) why = "il2cpp_object_new / il2cpp_array_new export missing";

    p->droneClass = cls(kImage, kDroneNs, "RanchDrone");
    p->stationModelClass = cls(kImage, kDataNs, "DroneStationGadgetModel");
    p->taskDataClass = cls(kImage, kDroneNs, "DroneTaskData");
    p->taskSourceClass = cls(kImage, kDroneNs, "DroneTaskSource");
    p->programIface = cls(kImage, kDroneNs, "IRanchDroneSourceProgram");
    p->elderClass = cls(kImage, kDroneNs, "RanchDroneSourceProgramElderCollector");
    p->taskTargetClass = cls(kImage, kDroneNs, "DroneTaskTarget");
    p->lsClass = cls("Unity.Localization", "UnityEngine.Localization", "LocalizedString");
    Il2CppClass* ranchMeta = cls(kImage, "", "RanchMetadata");
    Il2CppClass* station = cls(kImage, kDroneNs, "DroneStation");
    Il2CppClass* taskCfg = cls(kImage, kDroneNs, "DroneTaskConfig");
    Il2CppClass* ident = cls(kImage, "", "IdentifiableType");
    p->identClass = ident;
    p->typeGroupClass = cls(kImage, "", "IdentifiableTypeGroup");
    Il2CppClass* identModel = cls(kImage, kDataNs, "IdentifiableModel");
    Il2CppClass* defSource = cls(kImage, "MonomiPark.SlimeRancher.UI.Drone.Data", "DroneUIDefinitionSource");
    Il2CppClass* sceneCtx = cls(kImage, "", "SceneContext");
    Il2CppClass* gameModel = cls(kImage, kDataNs, "GameModel");
    Il2CppClass* droneDir = cls(kImage, kDroneNs, "DroneDirector");
    if (!why.empty()) return fail();
    // RanchMetadata.LandPlotMetadata / StorageMetadata: nested classes.
    using NestedFn = Il2CppClass* (*)(Il2CppClass*, void**);
    auto nested = reinterpret_cast<NestedFn>(ga ? GetProcAddress(ga, "il2cpp_class_get_nested_types") : nullptr);
    void* iter = nullptr;
    for (Il2CppClass* k = nested ? nested(ranchMeta, &iter) : nullptr; k && api().il2cpp_class_get_name; k = nested(ranchMeta, &iter)) {
        const std::string_view n = api().il2cpp_class_get_name(k) ? api().il2cpp_class_get_name(k) : "";
        if (n == "LandPlotMetadata") p->plotMetaClass = k;
        else if (n == "StorageMetadata") p->storageMetaClass = k;
    }
    if (!p->plotMetaClass || !p->storageMetaClass) {
        why = "RanchMetadata.LandPlotMetadata / StorageMetadata not found";
        return fail();
    }
    off(p->droneClass, "_stationModel", p->offDroneModel);
    off(p->droneClass, "_timeDir", p->offDroneTime);
    off(p->droneClass, "_ranchMetadata", p->offDroneRanch);
    off(p->droneClass, "_moveController", p->offDroneMove);
    off(p->droneClass, "_taskConfig", p->offDroneCfg);
    off(p->droneClass, "_droneStation", p->offDroneStation);
    off(p->stationModelClass, "_taskData", p->offModelTask);
    off(identModel, "actorId", p->offActorId);
    off(p->taskDataClass, "TargetType", p->offTdTarget);
    off(p->taskDataClass, "TargetIdentType", p->offTdIdent);
    off(p->taskDataClass, "SourceType", p->offTdSource);
    off(p->taskDataClass, "SinkType", p->offTdSink);
    off(p->taskSourceClass, "SourceType", p->offSrcType);
    off(p->elderClass, "_drone", p->offProgDrone);  // RanchDroneSourceProgramBase<StorageMetadata>
    off(p->elderClass, "_ranchMetadata", p->offProgRanch);
    off(ranchMeta, "plots", p->offRmPlots);
    off(p->plotMetaClass, "elderCollectors", p->offLpmElder);
    off(p->storageMetaClass, "storage", p->offSmStorage);
    off(taskCfg, "HenType", p->offCfgHen);
    off(taskCfg, "MeatTypeGroup", p->offCfgMeat);
    off(ident, "icon", p->offIdentIcon);
    off(p->typeGroupClass, "_memberTypes", p->offGroupMembers);
    off(p->taskTargetClass, "<Predicate>k__BackingField", p->offTargetPred);
    off(sceneCtx, "<GameModel>k__BackingField", p->offScGame);
    off(sceneCtx, "<DroneDirector>k__BackingField", p->offScDrones);
    off(gameModel, "identifiablesByIdent", p->offByIdent);
    off(droneDir, "_droneStationType", p->offStationType);
    p->elderCtor = R.Method(p->elderClass, ".ctor", 5, nullptr);
    p->srcCtor = R.Method(p->taskSourceClass, ".ctor", 2, nullptr);
    p->targetCtor = R.Method(p->taskTargetClass, ".ctor", 3, nullptr);
    p->lsCtor = R.Method(p->lsClass, ".ctor", 2, nullptr);
    if (!p->elderCtor || !p->srcCtor || !p->targetCtor || !p->lsCtor) why = "constructors not found";
    p->smGetId = method(p->storageMetaClass, "get_id", 0);
    p->smGetCount = method(p->storageMetaClass, "get_count", 0);
    p->gatherSourceMi = method(p->droneClass, "GatherSourceTask", 1);
    p->gatherSinkMi = method(p->droneClass, "GatherSinkTask", 1);
    p->assignMi = method(station, "AssignRanchTask", 4);
    p->sceneInst = method(parentOf(sceneCtx), "get_Instance", 0);
    p->lsGetTable = method(p->lsClass, "get_TableReference", 0);
    p->lsGetEntry = method(p->lsClass, "get_TableEntryReference", 0);
    const MethodInfo* sourcesMi = method(p->droneClass, "GatherSourceTasks", 1);
    const MethodInfo* getSourcesMi = R.Method(p->elderClass, "GetSources", 1, nullptr);
    const MethodInfo* findDefMi = method(defSource, "FindSourceDefinition", 1);
    const MethodInfo* pullMi = method(p->stationModelClass, "Pull", 9);
    if (!getSourcesMi && why.empty()) why = "RanchDroneSourceProgramElderCollector.GetSources not found";
    if (!why.empty()) return fail();

    struct H {
        const char* name;
        const MethodInfo* mi;
        void* detour;
        void** orig;
    };
    const H hooks[] = {
        {"RanchDrone.GatherSourceTask", p->gatherSourceMi, reinterpret_cast<void*>(&GatherSourceDetour),
         reinterpret_cast<void**>(&g_origGatherSource)},
        {"RanchDrone.GatherSourceTasks", sourcesMi, reinterpret_cast<void*>(&GatherSourcesDetour),
         reinterpret_cast<void**>(&g_origGatherSources)},
        {"RanchDroneSourceProgramElderCollector.GetSources", getSourcesMi, reinterpret_cast<void*>(&GetSourcesDetour),
         reinterpret_cast<void**>(&g_origGetSources)},
        {"DroneUIDefinitionSource.FindSourceDefinition", findDefMi, reinterpret_cast<void*>(&FindDefDetour),
         reinterpret_cast<void**>(&g_origFindDef)},
        {"DroneStationGadgetModel.Pull", pullMi, reinterpret_cast<void*>(&PullDetour), reinterpret_cast<void**>(&g_origPull)},
    };
    // All or nothing: without the Pull detour our type could reach the save.
    void* ptrs[5] = {};
    for (size_t i = 0; i < 5; ++i) {
        std::string err;
        ptrs[i] = R.MethodPointer(hooks[i].mi, &err);
        if (!ptrs[i]) {
            why = std::format("{}: no native body ({})", hooks[i].name, err);
            return fail();
        }
    }
    g = p;  // the detours may run as soon as a hook exists (they check their own state)
    for (size_t i = 0; i < 5; ++i) {
        if (!ctx.HookAddress(hooks[i].name, ptrs[i], hooks[i].detour, hooks[i].orig)) {
            why = std::format("hook {} failed", hooks[i].name);
            // Hooks already made stay (the module manager owns them); without the source option and
            // its definition nobody can pick our type, so they only pass through.
            p->available[kHen] = p->available[kElder] = false;
            return false;
        }
        report += std::format(" {}@0x{:X}", hooks[i].name, Resolver::Rva(ptrs[i]));
    }
    p->hooked = true;
    ready_ = true;
    return true;
}

// ---------------------------------------------------------------- tick --

void HenSource::Tick(const std::vector<Il2CppObject*>& henStorages) {
    if (!ready_ || !g || !main_thread::IsMainThread() || Impl::Faulted(g->module)) return;
    Impl& p = *g;
    p.henStorages = henStorages;
    p.available[kHen] = !henStorages.empty();
    const int elders = p.ElderStorages();  // -1 = unknown (no drone seen yet)
    if (elders >= 0) p.available[kElder] = elders > 0;
    if (p.resetSeen.exchange(false)) p.seen.clear();
    std::vector<Il2CppObject*> stations;
    if (!p.Stations(stations)) return;
    if (!custom_upgrades::Active()) return;
    // Programs reverted without game calls (module was off): their drones still run our program.
    std::unordered_set<int64_t> pending;
    {
        std::lock_guard lk(p.mu);
        pending.swap(p.pending);
    }
    std::set<int64_t> listed[kKinds], before[kKinds];
    for (int k = 0; k < kKinds; ++k) before[k] = listed[k] = ParseIds(custom_upgrades::SaveValue(kKind[k].key));
    auto listedKind = [&](int64_t id) { return listed[kHen].count(id) ? kHen : listed[kElder].count(id) ? kElder : -1; };
    auto setListed = [&](int64_t id, int kind) {
        for (int k = 0; k < kKinds; ++k)
            if (k == kind) listed[k].insert(id);
            else listed[k].erase(id);
    };
    // A Hen Collector program whose target is elders belongs to the Elder Collector (or Coop).
    auto fitKind = [&](int kind, Il2CppObject* task) {
        if (kind == kHen && p.ElderTarget(task)) return kElder;
        return kind;
    };
    std::set<int64_t> present;
    for (Il2CppObject* model : stations) {
        const int64_t id = p.StationId(model);
        Il2CppObject* task = p.TaskOf(model);
        if (!id || !task) continue;
        present.insert(id);
        const int32_t src = unity::Field<int32_t>(task, p.offTdSource);
        const int cur = KindOf(src);
        const int was = listedKind(id);
        const bool first = !p.seen.count(id);
        if (first && was == kElder && src == kSourceCoop && elders < 0) continue;  // ranch unknown yet: next tick
        p.seen.insert(id);
        const bool restore = first && was >= 0 && src == kSourceCoop;
        if (pending.count(id) && cur < 0 && !restore) p.Switch(model, id, -1, "custom upgrades were off");
        if (first) {
            if (restore) {
                const int want = fitKind(was, task);
                if (p.available[want]) {
                    p.Switch(model, id, want,
                             want == was ? "saved as Coop, restored from the sidecar"
                                         : "saved as Hen Collector with an elder target, now Elder Collector");
                    setListed(id, want);
                } else {
                    SR2_INFO("[{}] drone source: station {} used {}, but no coop has one - it stays Coop", p.module, id,
                             NameOf(want));
                    setListed(id, -1);
                }
            } else if (was >= 0 && cur < 0) {
                SR2_INFO("[{}] drone source: station {}'s program is no Coop program any more - {} entry dropped",
                         p.module, id, NameOf(was));
                setListed(id, -1);
            } else if (cur >= 0) {
                setListed(id, cur);
                p.NoteKnown(model);
            }
            continue;
        }
        if (cur >= 0) {
            p.NoteKnown(model);
            const int want = fitKind(cur, task);
            if (want != cur) {
                if (p.available[want]) {
                    p.Switch(model, id, want, "elder target: Elder Collector holds the elders");
                    setListed(id, want);
                } else {
                    p.Switch(model, id, -1, "elder target and no Elder Collector");
                    setListed(id, -1);
                }
            } else if (!p.available[cur] && (cur == kHen || elders >= 0)) {
                p.Switch(model, id, -1, cur == kHen ? "no Hen Collector left" : "no Elder Collector left");
                setListed(id, -1);
            } else if (was != cur) {
                setListed(id, cur);
                SR2_INFO("[{}] drone source: station {} uses {} (saved as Coop + sidecar entry)", p.module, id,
                         NameOf(cur));
            }
        } else if (was >= 0) {
            setListed(id, -1);
            SR2_INFO("[{}] drone source: station {} no longer uses {} - sidecar entry dropped", p.module, id, NameOf(was));
        }
    }
    for (int k = 0; k < kKinds; ++k) {
        for (auto it = listed[k].begin(); it != listed[k].end();) {
            if (present.count(*it)) {
                ++it;
                continue;
            }
            SR2_INFO("[{}] drone source: station {} is gone (picked up?) - its {} entry is dropped; placed again it has "
                     "a new id and a Coop program",
                     p.module, *it, kKind[k].name);
            it = listed[k].erase(it);
        }
        if (listed[k] != before[k]) custom_upgrades::SetSaveValue(kKind[k].key, JoinIds(listed[k]));
    }
    // Models that no longer carry one of our types need no pin.
    std::vector<Il2CppGCHandle> doomed;
    {
        std::lock_guard lk(p.mu);
        for (auto it = p.known.begin(); it != p.known.end();) {
            Il2CppObject* task = p.TaskOf(it->second.model);
            if (task && KindOf(unity::Field<int32_t>(task, p.offTdSource)) >= 0) {
                ++it;
                continue;
            }
            doomed.push_back(it->second.h);
            it = p.known.erase(it);
        }
    }
    if (!doomed.empty()) main_thread::Post([doomed] {
        for (Il2CppGCHandle h : doomed) unity::FreeGCHandle(h);
    });
}

void HenSource::Revert(bool gameCalls, const char* why) {
    if (!ready_ || !g) return;
    Impl& p = *g;
    const bool calls = gameCalls && main_thread::IsMainThread() && !Impl::Faulted(p.module);
    std::vector<std::pair<int64_t, Il2CppObject*>> models;
    {
        std::lock_guard lk(p.mu);
        for (const auto& [id, k] : p.known) models.push_back({id, k.model});
    }
    int n = 0;
    for (const auto& [id, model] : models) {
        Il2CppObject* task = p.TaskOf(model);
        const bool ours = task && KindOf(unity::Field<int32_t>(task, p.offTdSource)) >= 0;
        bool wasPending = false;
        {
            std::lock_guard lk(p.mu);
            wasPending = p.pending.count(id) > 0;
        }
        if (!ours && !(calls && wasPending)) continue;
        if (calls) {
            p.Switch(model, id, -1, why);
            std::lock_guard lk(p.mu);
            p.pending.erase(id);
        } else {
            unity::Field<int32_t>(task, p.offTdSource) = kSourceCoop;
            std::lock_guard lk(p.mu);
            p.pending.insert(id);
        }
        ++n;
    }
    if (calls) {  // also programs picked in the menu since the last tick
        std::vector<Il2CppObject*> stations;
        if (p.Stations(stations))
            for (Il2CppObject* model : stations) {
                Il2CppObject* task = p.TaskOf(model);
                if (task && KindOf(unity::Field<int32_t>(task, p.offTdSource)) >= 0) {
                    p.Switch(model, p.StationId(model), -1, why);
                    ++n;
                }
            }
    }
    p.resetSeen.store(true);  // enabled again: the sidecar's stations are mapped back
    if (n) SR2_INFO("[{}] drone source: {} program(s) set to Coop ({})", p.module, n, why);
}

void HenSource::ResetSeen() {
    if (g) g->resetSeen.store(true);
}

}  // namespace sr2::modules::custom_upgrades::drones
