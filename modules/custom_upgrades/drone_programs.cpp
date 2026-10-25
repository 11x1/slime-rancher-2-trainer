// custom_upgrades: several programs per ranch drone station (see drone_programs.h). SR2 1.3.0, all by
// name; RVAs for reference only.
//
// Findings (disassembly / dump):
//   * A station has ONE program: DroneStationGadgetModel._taskData (DroneTaskData {TargetType,
//     TargetIdentType, SourceType, SinkType}). DroneStation.AssignRanchTask(target, source, sink,
//     analytics) (0x9FE4E0) writes a new DroneTaskData (SetTaskData) and calls RanchDrone.AssignTask ->
//     DroneBehaviorPlexer.OnTaskChanged (0x9F5270): the current behaviour is deselected and a new
//     RanchDroneProgramPlexer is built from the task's source / sink programs. That is all the game
//     does when the player presses Start Program, so switching programs at runtime is the same call.
//   * What the drone does: DroneBehaviorPlexer._currBehaviour (IRanchDroneBehaviour). PickNextBehavior:
//     Idle (at-station flourishes) -> the program plexer (gather / deposit) -> RanchDroneBehaviourDumpAmmo
//     when its ammo is not empty (DESTROYS the cargo) -> RanchDroneBehaviourRest. Idle comes FIRST and is
//     picked whenever the drone is at its station and a flourish is due (random), even with work to do,
//     so it is not "idle" (round 9: counting it switched away before the drone started the program).
//     "Nothing to do" = the current behaviour is Rest, RanchDrone._ammo (AmmoSlotManager).IsEmpty() and
//     DroneBehaviorPlexer.MaybeHasNextGatherBehavior() (= any source program's Relevancy(readOnly), the
//     game's own "is there work" query that Rest uses itself) is false. Idle neither counts nor resets.
//     OnTaskChanged deselects the current behaviour, so a new program is tried on the next FixedTick.
//     Without battery the drone rests too (RanchDrone.GetCurrentEnergy() <= 0): no switching then.
//   * Stuck: a dynamic source (Outside Plots / Free Range: RanchDroneSourceProgramDynamic) flies to an
//     item it found in the cells and cone-scans for it there (Physics.SphereCastNonAlloc, layer mask
//     0x8800, the type must match). A scan that finds nothing completes empty and the next pick flies to
//     the same item again: the drone "keeps scanning" and never rests. A program that holds no cargo for
//     kStuckMs while working counts as "nothing it can pick up" (logged with the behaviour class and its
//     GetProgramStateStr, so the log says what the drone was doing).
//   * The page: DroneProgramViewModel.RevertChangesToLastAppliedProgram (0x822E80) compares the target /
//     source / destination sub view models with the station's current task data and resets the ones
//     that differ (ProgramChanged event -> the menu redraws). station_dialog.cpp calls it after Run so
//     the page shows the program that now runs.
//   * Fast-forward (sleeping / away) only runs the program that is current at that moment.
//   * Incinerator destination (RanchDroneSinkProgramIncinerator): GetSinks keeps only incinerators with
//     Incinerate.GetAshSpace() > 0 (0x987DA0: 0 without an active Ash Trough, else ceil(trough space /
//     AshPerIncineration)). A source only gathers while a sink has space, so with the trough full (or no
//     Ash Trough) a "burn X" program has nothing to do: MaybeHasNextGatherBehavior is false and the drone
//     rests. Round 10 "not starting with chickens" was this (the switch lines were in the log, the
//     chicken program -> Incinerator had nothing to do); the first idle switch of a program now logs why.
//
// Save: drone_source.cpp's Pull detour asks SaveFilter for program 1 (any thread; the entries are
// guarded by a mutex). Its source goes to the save as the vanilla type; the sidecar keeps the true one.
// The drone source lists (Hen / Elder Collector, Quantum Storage Link) skip stations this file manages
// (ManagedFilter), so rotating between a Hen Collector program and another one doesn't add and drop
// list entries every few seconds; program 1's true source comes back from this sidecar after a load.

#include "modules/custom_upgrades/drone_programs.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <format>
#include <initializer_list>
#include <mutex>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

#include "core/detour_guard.h"  // ModuleFaulted / MarkModuleFaulted
#include "core/il2cpp_api.h"
#include "core/log.h"
#include "core/main_thread.h"
#include "core/resolver.h"
#include "core/unity.h"
#include "modules/custom_upgrades/api.h"

namespace sr2::modules::custom_upgrades::drone_programs {
namespace {

namespace unity = sr2::core::unity;
namespace main_thread = sr2::core::main_thread;
using drones::ProgramData;
using sr2::il2cpp::api;
using sr2::il2cpp::Resolver;

constexpr const char* kImage = "Assembly-CSharp";
constexpr const char* kDroneNs = "MonomiPark.SlimeRancher.Drone";
constexpr const char* kKeyPrefix = "drone_programs.station.";
constexpr uint64_t kMinRunMs = 5000;       // a program runs at least this long before the next one
constexpr uint64_t kStuckMs = 90000;       // working without ever holding cargo this long = stuck
constexpr uint64_t kAllIdleWaitMs = 20000;  // every program was idle in a row: try the next one this rarely
constexpr int kIdleTicks = 2;              // ~1 s ticks resting with empty hands = nothing to do
constexpr uint64_t kMenuPauseMs = 2000;    // the station menu was open this recently: no switching
constexpr uint64_t kLogEveryMs = 30000;    // one switch line per station per 30 s at most
constexpr uint64_t kIncinRescanMs = 300000;  // FindObjectsOfTypeAll(Incinerate) for the idle line: 5 min at most

template <class T>
T Unbox(Il2CppObject* boxed, T def = T{}) {
    return boxed ? *reinterpret_cast<T*>(reinterpret_cast<char*>(boxed) + 2 * sizeof(void*)) : def;
}

bool IsA(Il2CppObject* obj, Il2CppClass* k) {
    const auto& a = api();
    if (!obj || !k || !a.il2cpp_object_get_class) return false;
    Il2CppClass* c = a.il2cpp_object_get_class(obj);
    if (c == k) return true;
    return c && a.il2cpp_class_is_assignable_from && a.il2cpp_class_is_assignable_from(k, c);
}

uint64_t NowMs() { return GetTickCount64(); }

std::string Key(int64_t id) { return kKeyPrefix + std::to_string(id); }

struct Entry {
    std::vector<ProgramData> progs;  // >= 2 while the entry exists
    int active = 0;
    Mode mode = Mode::kWhenIdle;
    Il2CppObject* model = nullptr;   // DroneStationGadgetModel, pinned by h
    Il2CppGCHandle h = nullptr;
    uint64_t lastSwitch = 0;
    int idleTicks = 0;
    int idleRound = 0;               // idle switches in a row (no cargo seen in between)
    bool cargo = false;              // the drone held something at the last look
    uint64_t lastCargo = 0;          // last time it held something (or the program started)
    uint64_t lastLog = 0;
    int unlogged = 0;
    unsigned explained = 0;          // programs whose "nothing to do" was logged since the last cargo (bits)
};

struct Impl {
    const char* module = "custom_upgrades";
    Il2CppClass* droneClass = nullptr, *restClass = nullptr, *idleClass = nullptr, *identClass = nullptr;
    Il2CppClass* incinerateClass = nullptr;      // optional (idle explanation only)
    const MethodInfo* ashSpace = nullptr;        // Incinerate.GetAshSpace(), optional
    // The scene's incinerators for the idle line, from one scan kept as GC handles (round 13: the scan per
    // idle line cost ~20 ms a frame; rescanned at most every kIncinRescanMs, when one went or none was found).
    std::vector<Il2CppGCHandle> incinerators;
    uint64_t incinScanMs = 0;  // 0: never scanned
    size_t offPlexer = 0, offAmmo = 0, offCurrBehaviour = 0, offProgramPlexer = 0;
    const MethodInfo* ammoIsEmpty = nullptr, *droneEnergy = nullptr, *maybeHasGather = nullptr;
    std::unordered_map<Il2CppClass*, const MethodInfo*> stateStr;  // GetProgramStateStr per behaviour class
    std::function<int()> owned;  // Drone Program Scheduler owned: 1 / 0 / -1 unknown (empty: always)
    int unlocked = -1;           // its state at the last tick since the load / enable (-1: not looked yet)

    std::mutex mu;  // entries (the Pull detour reads them from the saving thread)
    std::unordered_map<int64_t, Entry> entries;
    std::unordered_set<int64_t> seen;        // stations looked at since the load / enable (main thread)
    std::unordered_set<int64_t> reassign;    // reverted without game calls: program 1 again when seen
    std::atomic<int64_t> menuStation{0};
    std::atomic<uint64_t> menuMs{0};
    // Identifiable types by asset name (the sidecar's target), built once when first needed; pinned.
    std::unordered_map<std::string, Il2CppObject*> identsByName;
    bool identsBuilt = false;
    std::unordered_set<Il2CppObject*> pinned;  // idents held by a GC handle (session)
    bool loggedEnergyFail = false;

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
            if (err.rfind("native exception", 0) == 0) sr2::core::MarkModuleFaulted(module, std::format("{}: {}", what, err));
            else SR2_WARN("[{}] drone programs: {} failed: {}", module, what, err);
        }
        return r;
    }
    static bool Faulted(const char* m) { return sr2::core::ModuleFaulted(m); }
    static void FreeLater(Il2CppGCHandle h) {
        if (h) main_thread::Post([h] { unity::FreeGCHandle(h); });
    }
    void Pin(Il2CppObject* ident) {
        if (ident && !pinned.count(ident) && unity::NewGCHandle(ident)) pinned.insert(ident);
    }
    bool MenuOpen(int64_t id) const {
        return menuStation.load() == id && NowMs() - menuMs.load() < kMenuPauseMs;
    }

    std::string Serialize(const Entry& e);
    bool Parse(const std::string& raw, Entry& e, std::string& why);
    std::string Describe(const ProgramData& p);
    Il2CppObject* IdentByName(const std::string& name);
    void Persist(int64_t id, const Entry& e);
    void Drop(int64_t id, const char* why);
    void Load(int64_t id, Il2CppObject* model, const ProgramData& live, bool locked);
    void Lock();
    int OwnedNow() const { return owned ? owned() : 1; }
    void Rotate(int64_t id, Entry& e, uint64_t now);
    int NextValid(const Entry& e) const;
    std::string BehaviourInfo(Il2CppObject* beh);
    std::string IdleReason(const ProgramData& p);
    Entry* Find(int64_t id) {
        auto it = entries.find(id);
        return it != entries.end() ? &it->second : nullptr;
    }
    void SetModel(Entry& e, Il2CppObject* model) {
        if (e.model == model) return;
        Il2CppGCHandle old = e.h;
        e.h = unity::NewGCHandle(model);
        e.model = e.h ? model : nullptr;
        FreeLater(old);
    }
    void ClearAll() {
        std::lock_guard lk(mu);
        for (auto& [id, e] : entries) FreeLater(e.h);
        entries.clear();
    }
};

Impl* g = nullptr;

// ---------------------------------------------------------------- filters (drone_source) --

bool SaveFilter(int64_t id, ProgramData& out) {
    if (!g || !id) return false;
    std::lock_guard lk(g->mu);
    auto it = g->entries.find(id);
    if (it == g->entries.end() || it->second.progs.size() < 2) return false;
    out = it->second.progs[0];
    return true;
}

bool ManagedFilter(int64_t id) {
    if (!g || !id) return false;
    {
        std::lock_guard lk(g->mu);
        if (g->entries.count(id)) return true;
    }
    return !custom_upgrades::SaveValue(Key(id)).empty();
}

// ---------------------------------------------------------------- impl --

constexpr const char* kModeKey[2] = {"idle", "delivery"};

std::string Impl::Serialize(const Entry& e) {
    std::string s = std::format("v1;{}", kModeKey[static_cast<int>(e.mode) & 1]);
    for (size_t i = 0; i < e.progs.size(); ++i) {
        const ProgramData& p = e.progs[i];
        std::string ident = p.ident ? unity::GetName(p.ident) : std::string();
        if (ident.empty() || ident.find_first_of(":|;") != std::string::npos) ident = "-";
        s += std::format("{}{}:{}:{}:{}", i == 0 ? ";" : "|", p.target, ident, p.source, p.sink);
    }
    return s;
}

Il2CppObject* Impl::IdentByName(const std::string& name) {
    if (!identsBuilt) {
        identsBuilt = true;  // one FindObjectsOfTypeAll per session
        for (Il2CppObject* t : unity::FindObjectsOfTypeAll(identClass))
            if (unity::IsAlive(t)) identsByName.emplace(unity::GetName(t), t);
        SR2_INFO("[{}] drone programs: {} identifiable types known by name", module, identsByName.size());
    }
    auto it = identsByName.find(name);
    if (it == identsByName.end()) return nullptr;
    Pin(it->second);
    return it->second;
}

bool Impl::Parse(const std::string& raw, Entry& e, std::string& why) {
    std::vector<std::string> parts;
    std::stringstream ss(raw);
    for (std::string part; std::getline(ss, part, ';');) parts.push_back(part);
    if (parts.size() < 3 || parts[0] != "v1") {
        why = "unknown format";
        return false;
    }
    e.mode = parts[1] == kModeKey[1] ? Mode::kAfterDelivery : Mode::kWhenIdle;
    std::stringstream ps(parts[2]);
    for (std::string prog; std::getline(ps, prog, '|');) {
        std::vector<std::string> f;
        std::stringstream fs(prog);
        for (std::string x; std::getline(fs, x, ':');) f.push_back(x);
        if (f.size() != 4) {
            why = "bad program '" + prog + "'";
            return false;
        }
        ProgramData p;
        try {
            p.target = std::stoi(f[0]);
            p.source = std::stoi(f[2]);
            p.sink = std::stoi(f[3]);
        } catch (...) {
            why = "bad program '" + prog + "'";
            return false;
        }
        if (f[1] != "-") {
            p.ident = IdentByName(f[1]);
            if (!p.ident) {
                why = "unknown target type '" + f[1] + "'";
                return false;
            }
        }
        e.progs.push_back(p);
        if (static_cast<int>(e.progs.size()) >= kMaxPrograms) break;
    }
    if (e.progs.size() < 2) {
        why = "fewer than two programs";
        return false;
    }
    return true;
}

std::string Impl::Describe(const ProgramData& p) {
    std::string ident = p.ident ? unity::GetName(p.ident) : std::string();
    return std::format("target {}{}{} source {} sink {}", p.target, ident.empty() ? "" : " ", ident, p.source, p.sink);
}

void Impl::Persist(int64_t id, const Entry& e) {
    if (e.progs.size() < 2) custom_upgrades::SetSaveValue(Key(id), "");
    else custom_upgrades::SetSaveValue(Key(id), Serialize(e));
}

void Impl::Drop(int64_t id, const char* why) {
    {
        std::lock_guard lk(mu);
        if (auto it = entries.find(id); it != entries.end()) {
            FreeLater(it->second.h);
            entries.erase(it);
        }
    }
    custom_upgrades::SetSaveValue(Key(id), "");
    SR2_INFO("[{}] drone programs: station {}: extra programs dropped ({})", module, id, why);
}

// First sight of a station since the load / enable: its sidecar entry, checked against the save.
// Locked (Drone Program Scheduler not owned): the entry is restored all the same (program 1 with its
// true source runs, page edits keep it in sync with the save), it just does not rotate.
void Impl::Load(int64_t id, Il2CppObject* model, const ProgramData& live, bool locked) {
    const std::string raw = custom_upgrades::SaveValue(Key(id));
    const bool wasReverted = reassign.erase(id) > 0;
    if (raw.empty()) return;
    Entry e;
    std::string why;
    if (!Parse(raw, e, why)) {
        Drop(id, std::format("sidecar entry not readable: {}", why).c_str());
        return;
    }
    const ProgramData& p0 = e.progs[0];
    const bool match = live.target == p0.target && live.ident == p0.ident && live.sink == p0.sink &&
                       drones::VanillaSource(live.source) == drones::VanillaSource(p0.source);
    if (!match) {
        Drop(id, std::format("the save's program ({}) is not the mod's program 1 ({}) - changed without the mod?",
                             Describe(live), Describe(p0))
                     .c_str());
        return;
    }
    for (const ProgramData& p : e.progs) Pin(p.ident);
    e.active = 0;
    e.lastSwitch = NowMs();
    SetModel(e, model);
    {
        std::lock_guard lk(mu);
        entries[id] = e;
    }
    if (live != p0 || wasReverted)
        drones::AssignProgram(model, p0, live != p0 ? "program 1 restored from the sidecar (true source)"
                                                    : "program 1 again after custom upgrades were off");
    if (locked)
        SR2_INFO("[{}] drone programs: station {}: {} programs stored in the sidecar, inactive until the {} is bought "
                 "(program 1 runs, the others are kept)", module, id, e.progs.size(), kUpgradeName);
    else
        SR2_INFO("[{}] drone programs: station {}: {} programs restored from the sidecar ({}), program 1 runs", module, id,
                 e.progs.size(), ModeName(e.mode));
}

// The Drone Program Scheduler is no longer owned (the Sandbox lowered it): every station back on program 1
// (its true source; the rotation never switched with cargo, this does: rare). The entries stay, so the
// sidecar keeps every program.
void Impl::Lock() {
    std::vector<std::pair<Il2CppObject*, ProgramData>> todo;
    {
        std::lock_guard lk(mu);
        for (auto& [id, e] : entries) {
            if (e.active == 0 || e.progs.empty() || !e.model) continue;
            e.active = 0;
            e.lastSwitch = NowMs();
            e.idleTicks = e.idleRound = 0;
            todo.push_back({e.model, e.progs[0]});
        }
    }
    for (const auto& [model, p0] : todo) drones::AssignProgram(model, p0, nullptr);
    size_t n = 0;
    {
        std::lock_guard lk(mu);
        n = entries.size();
    }
    SR2_INFO("[{}] drone programs: {} not owned (any more): {} station(s) with stored programs run program 1 "
             "({} switched back), the other programs stay stored", module, kUpgradeName, n, todo.size());
}

int Impl::NextValid(const Entry& e) const {
    const int n = static_cast<int>(e.progs.size());
    for (int k = 1; k < n; ++k) {
        const int i = (e.active + k) % n;
        if (e.progs[i].Valid()) return i;
    }
    return e.active;
}

// "RanchDroneSourceProgramOutsidePlots (<its GetProgramStateStr>)" for the stuck log line.
std::string Impl::BehaviourInfo(Il2CppObject* beh) {
    const auto& a = api();
    Il2CppClass* k = beh && a.il2cpp_object_get_class ? a.il2cpp_object_get_class(beh) : nullptr;
    if (!k) return "?";
    const char* cname = a.il2cpp_class_get_name ? a.il2cpp_class_get_name(k) : nullptr;
    const std::string name = cname ? cname : "?";
    auto it = stateStr.find(k);
    if (it == stateStr.end()) {
        std::string err;
        const MethodInfo* m = nullptr;
        for (Il2CppClass* c = k; c && !m; c = a.il2cpp_class_get_parent ? a.il2cpp_class_get_parent(c) : nullptr)
            m = Resolver::Get().Method(c, "GetProgramStateStr", 0, &err);
        it = stateStr.emplace(k, m).first;
    }
    bool ok = false;
    Il2CppObject* r = it->second ? Call(it->second, beh, {}, "GetProgramStateStr", &ok) : nullptr;
    const std::string st = ok && r ? unity::ToUtf8(reinterpret_cast<Il2CppString*>(r)) : std::string();
    return st.empty() ? name : std::format("{} ({})", name, st);
}

// Why a program has nothing to do, for the first idle switch of a program (rare: at most once per program
// until the drone holds cargo again; but a resting drone's program logs that again after each cargo, ~every
// 1.5 min in round 12). The incinerators come from a cached scan (Impl::incinerators).
std::string Impl::IdleReason(const ProgramData& p) {
    constexpr int kSinkIncinerator = 4;  // DroneTaskSinkType.INCINERATOR
    if (p.sink != kSinkIncinerator || !incinerateClass || !ashSpace)
        return "the game finds nothing to gather that the destination has room for";
    std::vector<Il2CppObject*> live;
    bool gone = false;
    for (Il2CppGCHandle h : incinerators) {
        Il2CppObject* inc = unity::GCHandleTarget(h);
        if (inc && unity::IsAlive(inc)) live.push_back(inc);
        else gone = true;
    }
    const uint64_t now = NowMs();
    if (!incinScanMs || ((gone || live.empty()) && now - incinScanMs >= kIncinRescanMs)) {
        incinScanMs = now;
        for (Il2CppGCHandle h : incinerators) unity::FreeGCHandle(h);
        incinerators.clear();
        live.clear();
        for (Il2CppObject* inc : unity::FindObjectsOfTypeAll(incinerateClass)) {
            if (!unity::IsAlive(inc) || !unity::ActiveInHierarchy(unity::GameObjectOf(inc))) continue;  // prefabs
            if (Il2CppGCHandle h = unity::NewGCHandle(inc)) incinerators.push_back(h);
            live.push_back(inc);
            if (live.size() >= 64) break;
        }
    }
    int total = 0, room = 0;
    for (Il2CppObject* inc : live) {
        if (!unity::ActiveInHierarchy(unity::GameObjectOf(inc))) continue;
        ++total;
        bool ok = false;
        const int space = Unbox<int>(Call(ashSpace, inc, {}, "Incinerate.GetAshSpace", &ok));
        if (ok && space > 0) ++room;
    }
    return std::format("incinerators with room in their Ash Trough: {} of {} (a drone only burns into an incinerator "
                       "whose Ash Trough is built and not full)",
                       room, total);
}

void Impl::Rotate(int64_t id, Entry& e, uint64_t now) {
    if (e.progs.size() < 2 || !e.model) return;
    Il2CppObject* drone = drones::DroneOf(e.model);
    if (!drone || !IsA(drone, droneClass)) return;  // not loaded / not seen yet
    if (e.lastCargo < e.lastSwitch) e.lastCargo = e.lastSwitch;
    bool ok = false;
    const float energy = Unbox<float>(Call(droneEnergy, drone, {}, "RanchDrone.GetCurrentEnergy", &ok));
    if (!ok) {
        if (!loggedEnergyFail) SR2_WARN("[{}] drone programs: battery level unreadable - no switching", module);
        loggedEnergyFail = true;
        return;
    }
    if (energy <= 0.0f) {  // resting for lack of battery: every program would be "idle"
        e.idleTicks = 0;
        e.lastCargo = now;
        return;
    }
    Il2CppObject* ammo = unity::Field<Il2CppObject*>(drone, offAmmo);
    const bool empty = ammo && Unbox<bool>(Call(ammoIsEmpty, ammo, {}, "AmmoSlotManager.IsEmpty", &ok)) && ok;
    if (!empty) {  // cargo: never switch (the next program's destination may not take it -> dumped)
        e.cargo = true;
        e.idleTicks = 0;
        e.idleRound = 0;
        e.lastCargo = now;
        e.explained = 0;
        return;
    }
    const bool delivered = e.cargo;
    e.cargo = false;
    Il2CppObject* plexer = unity::Field<Il2CppObject*>(drone, offPlexer);
    Il2CppObject* beh = plexer ? unity::Field<Il2CppObject*>(plexer, offCurrBehaviour) : nullptr;
    const bool flourish = beh && IsA(beh, idleClass);  // the at-station animation: picked before any work
    bool resting = beh && IsA(beh, restClass);
    if (resting && unity::Field<Il2CppObject*>(plexer, offProgramPlexer)) {
        // Resting, but does the game see something to gather for this program? (No program plexer: the
        // task is not valid for the drone, nothing to do.)
        const bool work =
            Unbox<bool>(Call(maybeHasGather, plexer, {}, "DroneBehaviorPlexer.MaybeHasNextGatherBehavior", &ok));
        if (!ok) return;
        resting = !work;
    }
    if (resting) ++e.idleTicks;
    else if (!flourish) e.idleTicks = 0;
    const uint64_t since = now - e.lastSwitch;
    const int n = static_cast<int>(e.progs.size());
    const char* why = nullptr;
    std::string stuck;
    if (e.mode == Mode::kAfterDelivery && delivered && since >= kMinRunMs) why = "after a delivery";
    else if (e.idleTicks >= kIdleTicks && since >= (e.idleRound >= n ? kAllIdleWaitMs : kMinRunMs)) why = "nothing to do";
    else if (beh && !resting && !flourish && now - e.lastCargo >= kStuckMs) {
        why = "found nothing it could pick up";
        stuck = BehaviourInfo(beh);
    }
    if (!why) return;
    const int next = NextValid(e);
    if (next == e.active) {
        e.lastCargo = now;  // nothing else to run: look again in kStuckMs
        return;
    }
    const int from = e.active;
    // The first idle switch of a program since the last cargo says why it had nothing to do (the drone
    // rests on it; round 10 read that as "never switches").
    std::string reason;
    if (stuck.empty() && !delivered && !(e.explained & (1u << from))) {
        e.explained |= 1u << from;
        reason = IdleReason(e.progs[from]);
    }
    if (!drones::AssignProgram(e.model, e.progs[next], nullptr)) return;
    {
        std::lock_guard lk(mu);
        e.active = next;
    }
    e.lastSwitch = now;
    e.lastCargo = now;
    e.idleTicks = 0;
    e.idleRound = delivered ? 0 : e.idleRound + 1;
    if (!stuck.empty()) {  // always logged: it explains a program that never gets anything done
        SR2_INFO("[{}] drone programs: station {}: program {} ({}) held no cargo for {} s while working, the drone was "
                 "{} - program {} runs",
                 module, id, from + 1, Describe(e.progs[from]), kStuckMs / 1000, stuck, next + 1);
        return;
    }
    if (!reason.empty()) {  // always logged (once per program per idle streak)
        SR2_INFO("[{}] drone programs: station {}: program {} -> {} (nothing to do: program {} ({}) - {})", module, id,
                 from + 1, next + 1, from + 1, Describe(e.progs[from]), reason);
        return;
    }
    if (now - e.lastLog >= kLogEveryMs || e.lastLog == 0) {
        SR2_INFO("[{}] drone programs: station {}: program {} -> {} ({}){}", module, id, from + 1, next + 1, why,
                 e.unlogged ? std::format(" [+{} switch(es) not logged]", e.unlogged) : std::string());
        e.lastLog = now;
        e.unlogged = 0;
    } else {
        ++e.unlogged;
    }
}

}  // namespace

// ---------------------------------------------------------------- public --

const char* ModeName(Mode m) { return m == Mode::kAfterDelivery ? "after each delivery" : "when idle"; }

bool Available() { return g && custom_upgrades::Active() && !Impl::Faulted(g->module); }

bool Owned() { return Available() && main_thread::IsMainThread() && g->OwnedNow() == 1; }

bool Get(Il2CppObject* stationModel, View& out) {
    out = {};
    if (!Available() || !main_thread::IsMainThread()) return false;
    const int64_t id = drones::StationIdOf(stationModel);
    ProgramData live;
    if (!id || !drones::ReadProgram(stationModel, live)) return false;
    out.explorer = drones::IsExplorer(stationModel);
    std::lock_guard lk(g->mu);
    if (Entry* e = g->Find(id)) {
        out.programs = e->progs;
        out.running = e->active;
        out.mode = e->mode;
        if (out.running >= 0 && out.running < static_cast<int>(out.programs.size()))
            out.programs[out.running] = live;  // edited on the page and not synced yet
    } else {
        out.programs = {live};
    }
    return true;
}

bool Add(Il2CppObject* stationModel) {
    if (!Owned() || !main_thread::IsMainThread()) return false;
    const int64_t id = drones::StationIdOf(stationModel);
    ProgramData live;
    const bool explorer = drones::IsExplorer(stationModel);
    // Explorer mode (round 14: "can't add multiple programs to a explorer mode drone as it has no source and
    // destination"): a program is its target only.
    if (!id || !drones::ReadProgram(stationModel, live) || !live.ValidFor(explorer)) {
        SR2_INFO("[{}] drone programs: station {}: no program added - the running one is incomplete ({})", g->module,
                 id, explorer ? "explorer mode: no target" : "ranch mode: needs target, source and destination");
        return false;
    }
    Impl& p = *g;
    p.Pin(live.ident);
    Entry snapshot;
    {
        std::lock_guard lk(p.mu);
        Entry* e = p.Find(id);
        if (!e) {
            Entry fresh;
            fresh.progs = {live};
            e = &p.entries.emplace(id, fresh).first->second;
        }
        if (static_cast<int>(e->progs.size()) >= kMaxPrograms) return false;
        if (e->active >= 0 && e->active < static_cast<int>(e->progs.size())) e->progs[e->active] = live;
        e->progs.push_back(live);
        e->active = static_cast<int>(e->progs.size()) - 1;
        e->lastSwitch = NowMs();
        e->idleTicks = e->idleRound = 0;
        snapshot = *e;
    }
    {
        std::lock_guard lk(p.mu);
        if (Entry* e = p.Find(id)) p.SetModel(*e, stationModel);
    }
    p.seen.insert(id);
    p.Persist(id, snapshot);
    SR2_INFO("[{}] drone programs: station {}: program {} added (a copy of the running one; it runs now){}", p.module, id,
             snapshot.progs.size(), explorer ? " - explorer mode: programs switch only from the menu" : "");
    return true;
}

bool Remove(Il2CppObject* stationModel, int index) {
    if (!Owned() || !main_thread::IsMainThread()) return false;
    const int64_t id = drones::StationIdOf(stationModel);
    Impl& p = *g;
    Entry snapshot;
    bool runOther = false;
    {
        std::lock_guard lk(p.mu);
        Entry* e = p.Find(id);
        if (!e || index < 0 || index >= static_cast<int>(e->progs.size()) || e->progs.size() < 2) return false;
        e->progs.erase(e->progs.begin() + index);
        if (index == e->active) {
            e->active = std::min(index, static_cast<int>(e->progs.size()) - 1);
            runOther = true;
        } else if (index < e->active) {
            --e->active;
        }
        e->lastSwitch = NowMs();
        e->idleTicks = e->idleRound = 0;
        snapshot = *e;
        if (e->progs.size() < 2) {
            Impl::FreeLater(e->h);
            p.entries.erase(id);
        }
    }
    p.Persist(id, snapshot);
    if (runOther) drones::AssignProgram(stationModel, snapshot.progs[snapshot.active], nullptr);
    SR2_INFO("[{}] drone programs: station {}: program {} removed, {} left, program {} runs", p.module, id, index + 1,
             snapshot.progs.size(), snapshot.active + 1);
    return true;
}

bool Run(Il2CppObject* stationModel, int index) {
    if (!Owned() || !main_thread::IsMainThread()) return false;
    const int64_t id = drones::StationIdOf(stationModel);
    Impl& p = *g;
    ProgramData want, live;
    {
        std::lock_guard lk(p.mu);
        Entry* e = p.Find(id);
        if (!e || index < 0 || index >= static_cast<int>(e->progs.size())) return false;
        if (index == e->active) return true;
        if (drones::ReadProgram(stationModel, live)) e->progs[e->active] = live;  // page edits not synced yet
        want = e->progs[index];
    }
    if (!drones::AssignProgram(stationModel, want, nullptr)) return false;
    Entry snapshot;
    {
        std::lock_guard lk(p.mu);
        Entry* e = p.Find(id);
        if (!e) return false;
        e->active = index;
        e->lastSwitch = NowMs();
        e->idleTicks = e->idleRound = 0;
        e->cargo = false;
        snapshot = *e;
    }
    p.Persist(id, snapshot);
    SR2_INFO("[{}] drone programs: station {}: program {} runs (chosen in the station menu)", p.module, id, index + 1);
    return true;
}

bool SetMode(Il2CppObject* stationModel, Mode m) {
    if (!Owned() || !main_thread::IsMainThread()) return false;
    const int64_t id = drones::StationIdOf(stationModel);
    Impl& p = *g;
    Entry snapshot;
    {
        std::lock_guard lk(p.mu);
        Entry* e = p.Find(id);
        if (!e) return false;
        if (e->mode == m) return true;
        e->mode = m;
        snapshot = *e;
    }
    p.Persist(id, snapshot);
    SR2_INFO("[{}] drone programs: station {}: next program {}", p.module, id, ModeName(m));
    return true;
}

void NoteMenuOpen(int64_t station) {
    if (!g) return;
    g->menuStation.store(station);
    g->menuMs.store(NowMs());
}

// ---------------------------------------------------------------- rotation --

bool Rotation::Setup(Resolver& R, const char* module, std::function<int()> owned, std::string& why) {
    auto* p = new Impl();
    p->module = module;
    p->owned = std::move(owned);
    auto parentOf = [](Il2CppClass* c) { return api().il2cpp_class_get_parent ? api().il2cpp_class_get_parent(c) : nullptr; };
    auto cls = [&](const char* ns, const char* name) -> Il2CppClass* {
        std::string err;
        Il2CppClass* k = R.Class(kImage, ns, name, &err);
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
    p->droneClass = cls(kDroneNs, "RanchDrone");
    Il2CppClass* plexer = cls(kDroneNs, "DroneBehaviorPlexer");
    p->restClass = cls(kDroneNs, "RanchDroneBehaviourRest");
    p->idleClass = cls(kDroneNs, "RanchDroneBehaviourIdle");
    p->identClass = cls("", "IdentifiableType");
    Il2CppClass* ammo = cls("MonomiPark.SlimeRancher.Player", "AmmoSlotManager");
    if (!why.empty()) {
        delete p;
        return false;
    }
    off(p->droneClass, "_plexer", p->offPlexer);
    off(p->droneClass, "_ammo", p->offAmmo);
    off(plexer, "_currBehaviour", p->offCurrBehaviour);
    off(plexer, "_programPlexer", p->offProgramPlexer);
    p->ammoIsEmpty = method(ammo, "IsEmpty", 0);
    p->maybeHasGather = method(plexer, "MaybeHasNextGatherBehavior", 0);
    p->droneEnergy = method(p->droneClass, "GetCurrentEnergy", 0);
    if (!why.empty()) {
        delete p;
        return false;
    }
    {  // optional: only explains an idle "burn" program in the log
        std::string err;
        p->incinerateClass = R.Class(kImage, "", "Incinerate", &err);
        p->ashSpace = p->incinerateClass ? R.Method(p->incinerateClass, "GetAshSpace", 0, &err) : nullptr;
        if (!p->ashSpace) SR2_INFO("[{}] drone programs: Incinerate.GetAshSpace not found (idle log without ash info)", module);
    }
    g = p;
    drones::SetProgramHooks(&SaveFilter, &ManagedFilter);
    ready_ = true;
    return true;
}

void Rotation::Tick() {
    if (!ready_ || !g || !main_thread::IsMainThread() || Impl::Faulted(g->module) || !custom_upgrades::Active()) return;
    Impl& p = *g;
    // The Drone Program Scheduler: not known yet (save still loading) -> wait; not owned -> program 1 only.
    const int own = p.OwnedNow();
    if (own < 0) return;
    const bool locked = own == 0;
    if (p.unlocked != own) {
        if (p.unlocked == 1 && locked) p.Lock();
        else if (p.unlocked == 0 && !locked)
            SR2_INFO("[{}] drone programs: {} owned now: stored programs take turns again", p.module, kUpgradeName);
        else
            SR2_INFO("[{}] drone programs: {} {} in this save{}", p.module, kUpgradeName, locked ? "not owned" : "owned",
                     locked ? " - one program per station (stored extra programs wait for it)" : "");
        p.unlocked = own;
    }
    std::vector<Il2CppObject*> stations;
    if (!drones::StationModels(stations)) return;
    const uint64_t now = NowMs();
    std::unordered_set<int64_t> present;
    for (Il2CppObject* model : stations) {
        const int64_t id = drones::StationIdOf(model);
        ProgramData live;
        if (!id || !drones::ReadProgram(model, live)) continue;
        present.insert(id);
        if (p.seen.insert(id).second) {
            p.Load(id, model, live, locked);
            continue;  // restored programs start at the next tick
        }
        Entry* e = nullptr;
        {
            std::lock_guard lk(p.mu);
            e = p.Find(id);
        }
        if (!e) continue;
        p.SetModel(*e, model);
        // Changed on the page (Start Program) or by the game (a source with nothing left -> Coop):
        // that is the running program now.
        if (e->active >= 0 && e->active < static_cast<int>(e->progs.size()) && live != e->progs[e->active]) {
            p.Pin(live.ident);
            Entry snapshot;
            {
                std::lock_guard lk(p.mu);
                e->progs[e->active] = live;
                snapshot = *e;
            }
            p.Persist(id, snapshot);
            SR2_INFO("[{}] drone programs: station {}: program {} changed ({})", p.module, id, e->active + 1, p.Describe(live));
        }
        if (locked || p.MenuOpen(id)) continue;
        p.Rotate(id, *e, now);
        if (Impl::Faulted(p.module)) return;
    }
    // Stations that are gone (picked up, demolished): their programs go with them.
    std::vector<int64_t> gone;
    {
        std::lock_guard lk(p.mu);
        for (const auto& [id, e] : p.entries)
            if (!present.count(id)) gone.push_back(id);
    }
    for (int64_t id : gone) p.Drop(id, "the station is gone (picked up?); placed again it has a new id and one program");
}

void Rotation::ResetSeen() {
    if (!g) return;
    g->ClearAll();  // now: a save before the next tick must not see the old game's programs
    main_thread::Post([] {
        if (!g) return;
        g->seen.clear();
        g->unlocked = -1;  // the new save's Drone Program Scheduler state is logged again
    });
}

void Rotation::Revert(bool gameCalls, const char* why) {
    if (!ready_ || !g) return;
    Impl& p = *g;
    const bool calls = gameCalls && main_thread::IsMainThread() && !Impl::Faulted(p.module);
    std::vector<std::pair<int64_t, std::pair<Il2CppObject*, ProgramData>>> todo;
    {
        std::lock_guard lk(p.mu);
        for (const auto& [id, e] : p.entries) {
            if (e.progs.empty() || !e.model) continue;
            ProgramData p0 = e.progs[0];
            p0.source = drones::VanillaSource(p0.source);
            todo.push_back({id, {e.model, p0}});
        }
    }
    int n = 0;
    for (const auto& [id, mp] : todo) {
        const auto& [model, p0] = mp;
        if (calls) {
            if (drones::AssignProgram(model, p0, nullptr)) ++n;
        } else if (drones::SetProgramData(model, p0)) {
            if (main_thread::IsMainThread()) p.reassign.insert(id);
            else main_thread::Post([id] {
                if (g) g->reassign.insert(id);
            });
            ++n;
        }
    }
    if (calls) {
        p.ClearAll();
        p.seen.clear();
        p.reassign.clear();
        p.unlocked = -1;
    }
    if (n) SR2_INFO("[{}] drone programs: {} station(s) back on program 1 ({}{})", p.module, n, why,
                    calls ? "" : "; program data only");
}

}  // namespace sr2::modules::custom_upgrades::drone_programs
