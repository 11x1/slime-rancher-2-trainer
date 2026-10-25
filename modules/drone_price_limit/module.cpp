// drone_price_limit: quantum (ranch) drones only sell plorts to a Plort Market / Market
// Link while that plort's current market price is at or above their station's limit.
// The limit is set per drone station in the station menu (station_dialog.cpp) and stored in the
// per-save sidecar of custom_upgrades (api.h SaveValue, key "drone_price_limit.station.<actor id>").
//
// Findings (SR2 1.3.0, GameAssembly.dll disassembled; RVAs for reference only, every
// lookup below is by name):
//
//   ScorePlort.CanDroneDeposit(IdentifiableType id, bool ignoreMarketShutdown) (0x991E40)
//     returns false for an inactive GameObject or a Market Link still charging, else it
//     tail-calls GetMarketValue(id, ignore).HasValue: id == _sprinkleType -> 1; a
//     caretaker shop (_isCaretakerShop) never; market shut down (unless ignored) -> null;
//     else PlortEconomyDirector.GetCurrValue(id) = RoundToInt(_currValueMap[id].CurrValue)
//     (null for types without a market entry). That rounded value is the price shown on
//     the board and paid by Deposit/TrySell.
//   Only two callers (rel32 xref scan): the GetSinks iterators of
//     RanchDroneSinkProgramPlortMarket and RanchDroneSinkProgramCaretakerShop, both with
//     ignoreMarketShutdown = true. Everything a market sink program does goes through
//     GetSinks:
//       * HasAvailableSpace / GetAvailableSpace = GetSinks(id).Any()
//       * Relevancy(overflow): drone holds `id` and CanTarget(id) -> _sink =
//         Prioritize(GetSinks(id)).FirstOrDefault(); relevant only if non-null
//       * OnAction: GetSinks(heldId).Any(s => s == _sink) re-validated every action tick
//       * the source programs (RanchDroneSourceProgramBase.Relevancy / IsSourceValid) only
//         gather a type while some sink program reports space for it
//       * FastForward (sleep / away catch-up, 0xA2C2C0): Prioritize(GetSinks).First() then
//         Deposit. Its only caller DroneFastForwarder.FastForward_Deposit first filters the
//         sink programs with HasAvailableSpace(group.id) and returns "nothing deposited" when
//         none is left, and the source group is only decremented by what was deposited.
//     So one filter in CanDroneDeposit covers live drones AND the fast-forward path, and
//     Enumerable.First never sees an empty sequence (same frame, same price).
//   What a drone does when no sink program is relevant:
//     DroneBehaviorPlexer.PickNextBehavior: Idle? -> program plexer (gather / deposit) ->
//     RanchDroneBehaviourDumpAmmo if its ammo slot is not empty -> Rest. DumpAmmo
//     (OnActionDumpAmmo: PopFromSelectedSlot + FX) DESTROYS the held items. A drone already
//     carrying plorts when the price drops (daily price update while flying, limit raised
//     at runtime, loaded save) would therefore throw them away.
//
// Which station asks: CanDroneDeposit only gets (ScorePlort, id). Its market caller is the
//   iterator RanchDroneSinkProgramPlortMarket.<GetSinks>d__16 (MoveNext 0xA356A0, field
//   <>4__this = the program). Every use of GetSinks enumerates it inside the calling method
//   (Any / FirstOrDefault / First), so a detour on that MoveNext publishes "the program being
//   enumerated" for the duration of the call (main thread only, nesting-safe), and the
//   CanDroneDeposit detour reads program._drone (RanchDroneSinkProgramBase<ScorePlort>) ->
//   RanchDrone._stationModel (DroneStationGadgetModel) -> IdentifiableModel.actorId. The actor
//   id is saved with the gadget (PlacedGadgetV06.ActorId), so it is stable across save / load.
//   The caretaker shop's iterator is not detoured: no context = no limit (and the shop is
//   skipped anyway).
//
// What this module does:
//   1. Detour ScorePlort.CanDroneDeposit: after the original said yes, for a real market
//      (not the caretaker shop, not the sprinkle type) and a type with a market price,
//      return false while price < the asking station's limit. Limit: a minimum price in
//      Newbucks, or ceil(percent / 100 x base value); the base value is the plort's
//      PlortEconomySettings.PlortsTable InitialValue (the same number the director stores
//      as CurrValueEntry.BaseValue). Drones then neither pick that plort up nor fly to the
//      market with it; gathering resumes by itself once the price is back at the limit.
//   2. Detour RanchDroneBehaviourDumpAmmo.Relevancy: if the drone holds a plort that is
//      being held back by our limit and the drone has a Plort Market sink program, report
//      "not relevant". PickNextBehavior then picks Rest: the drone parks at its station with
//      the plorts and re-checks on its own (Rest.ShouldRethink / MaybeHasNextGatherBehavior);
//      once the price recovers it deposits them. Any other dump (no market program, Market
//      Link removed while the price is fine, station destroyed via ForceDumpAmmoAndDestroy)
//      stays vanilla.
//   Fast forward (sleeping / away): covered by (1). Held plorts stay in the drone (FastForward
//   only deposits through a sink with space) and plorts that were never gathered stay at the
//   source (corral floor / silo), exactly as when a drone has no target.
//
// Per call cost: CanDroneDeposit runs whenever a drone evaluates market sinks: a few field
// reads to the station id and a cached limit lookup (the sidecar value is re-read at most
// every ~2 s per station); with a limit set, one native GetCurrValue call and a hash lookup.
// No per-frame work in the drone logic; the station UI ticks only while a station menu exists.
//
// Settings: only the module's on / off switch in the F8 menu. The limits are a game-UI feature
// (station menu). Without custom_upgrades (off / failed) the limits still work but live only in
// memory for the session.
//
// Fault policy: DetourGuard on both detours (original always called outside the guard,
// faults -> pass-through + module FAILED), main thread only, every object class-checked
// (Unity objects also IsAlive), no managed object cached across frames (raw pointers kept
// only as identities / map keys, never dereferenced later), no GC handles.

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <format>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

#include "core/detour_guard.h"
#include "core/il2cpp_api.h"
#include "core/log.h"
#include "core/main_thread.h"
#include "core/module.h"
#include "core/unity.h"
#include "modules/custom_upgrades/api.h"
#include "modules/drone_price_limit/station_dialog.h"

namespace sr2::modules {
namespace {

namespace unity = sr2::core::unity;
namespace main_thread = sr2::core::main_thread;
using sr2::il2cpp::api;
using sr2::il2cpp::Resolver;

constexpr const char* kModule = "drone_price_limit";
constexpr const char* kImage = "Assembly-CSharp";
constexpr const char* kDroneNs = "MonomiPark.SlimeRancher.Drone";
constexpr const char* kEconNs = "MonomiPark.SlimeRancher.Economy";
constexpr const char* kStationKeyPrefix = "drone_price_limit.station.";
constexpr unsigned long long kLimitCacheFrames = 120;  // re-read a station's sidecar value after ~2 s

using drone_price_limit::LimitMode;
using drone_price_limit::StationLimit;

bool IsA(Il2CppObject* obj, Il2CppClass* k) {
    const auto& a = api();
    if (!obj || !k || !a.il2cpp_object_get_class) return false;
    Il2CppClass* c = a.il2cpp_object_get_class(obj);
    if (c == k) return true;
    return c && a.il2cpp_class_is_assignable_from && a.il2cpp_class_is_assignable_from(k, c);
}

template <class Sig>
struct Bound;
template <class R, class... A>
struct Bound<R(A...)> {
    using Fn = R (*)(A..., const MethodInfo*);
    Fn fn = nullptr;
    const MethodInfo* mi = nullptr;
    explicit operator bool() const { return fn != nullptr; }
    R operator()(A... a) const { return fn(a..., mi); }
};

struct Verdict {
    bool known = false;  // the type has a market price
    bool hold = false;   // price < limit
    int price = 0;
    int limit = 0;
};

class DronePriceLimit final : public sr2::core::Module {
public:
    const char* Name() const override { return kModule; }
    bool Setup(sr2::core::ModuleContext& ctx) override;
    void OnEnable() override {
        enabled_.store(true);
        resetCache_.store(true);
        SR2_INFO("[{}] enabled: per-station limits ({})", kModule,
                 custom_upgrades::Active() ? "stored per save" : "custom_upgrades off: session only");
    }
    void OnDisable() override;
    bool Enabled() const { return enabled_.load(std::memory_order_relaxed); }

    // Main thread, inside DetourGuards. true = report "cannot deposit".
    bool FilterDeposit(Il2CppObject* scorePlort, Il2CppObject* id, Il2CppObject* program);
    // true = suppress the dump.
    bool SuppressDump(Il2CppObject* dumpBehaviour);

    // Per-station limits (main thread).
    StationLimit GetLimit(int64_t station);
    void SetLimit(int64_t station, const StationLimit& l);
    float BaseValueOf(Il2CppObject* id);  // for the dialog: base value via SceneContext's economy director
    void Frame(float dt);

private:
    struct HeldKey {
        int64_t station;
        const void* id;
        bool operator==(const HeldKey&) const = default;
    };
    struct HeldKeyHash {
        size_t operator()(const HeldKey& k) const {
            return std::hash<const void*>()(k.id) ^ (std::hash<int64_t>()(k.station) * 1099511628211ull);
        }
    };
    struct HeldState {
        bool held = false;
        bool dumpNoted = false;
        int price = 0;
        int limit = 0;
    };
    struct CachedLimit {
        StationLimit limit;
        unsigned long long tick = 0;
    };

    Verdict Evaluate(Il2CppObject* econ, Il2CppObject* id, const StationLimit& lim);
    int LimitFor(Il2CppObject* econ, Il2CppObject* id, const StationLimit& lim);
    float BaseValue(Il2CppObject* econ, Il2CppObject* id);
    void Record(int64_t station, Il2CppObject* id, const Verdict& v);
    bool HasMarketProgram(Il2CppObject* drone);
    int64_t StationOfDrone(Il2CppObject* drone);
    Il2CppObject* SceneEcon();

    std::atomic<bool> enabled_{false};
    std::atomic<bool> anyHeld_{false};
    std::atomic<bool> resetCache_{false};  // OnEnable (any thread) -> drop the limit cache on the main thread

    // classes
    Il2CppClass* scorePlortClass_ = nullptr;
    Il2CppClass* econClass_ = nullptr;
    Il2CppClass* econSettingsClass_ = nullptr;
    Il2CppClass* identClass_ = nullptr;
    Il2CppClass* dumpClass_ = nullptr;
    Il2CppClass* droneClass_ = nullptr;
    Il2CppClass* behaviourPlexerClass_ = nullptr;
    Il2CppClass* programPlexerClass_ = nullptr;
    Il2CppClass* marketProgramClass_ = nullptr;
    Il2CppClass* ammoClass_ = nullptr;
    Il2CppClass* stationModelClass_ = nullptr;

    // offsets
    size_t offSpIsCaretaker_ = 0;   // ScorePlort._isCaretakerShop (bool)
    size_t offSpSprinkle_ = 0;      // ScorePlort._sprinkleType
    size_t offSpEcon_ = 0;          // ScorePlort._econDir
    size_t offEconSettings_ = 0;    // PlortEconomyDirector._settings
    size_t offSetTable_ = 0;        // PlortEconomySettings.PlortsTable (struct)
    size_t offTablePlorts_ = 0;     // PlortValueConfigurationTable.Plorts (inside the struct)
    size_t offCfgType_ = 0;         // PlortValueConfiguration.Type (inside the struct)
    size_t offCfgInitial_ = 0;      // PlortValueConfiguration.InitialValue
    size_t cfgStride_ = 0;          // array element size of PlortValueConfiguration
    size_t offDumpDrone_ = 0;       // RanchDroneBehaviourDumpAmmo._drone
    size_t offDronePlexer_ = 0;     // RanchDrone._plexer
    size_t offDroneEcon_ = 0;       // RanchDrone._econDir
    size_t offDroneAmmo_ = 0;       // RanchDrone._ammo
    size_t offPlexerPrograms_ = 0;  // DroneBehaviorPlexer._programPlexer
    size_t offProgramsSinks_ = 0;   // RanchDroneProgramPlexer._sinks (IRanchDroneSinkProgram[])
    size_t offProgramDrone_ = 0;    // RanchDroneSinkProgramBase<ScorePlort>._drone
    size_t offDroneStation_ = 0;    // RanchDrone._stationModel
    size_t offActorId_ = 0;         // IdentifiableModel.actorId (ActorId { long Value })
    size_t offScEcon_ = 0;          // SceneContext.<PlortEconomyDirector>k__BackingField
    const MethodInfo* sceneCtxInstMi_ = nullptr;

    Bound<uint64_t(Il2CppObject*, Il2CppObject*)> getCurrValue_;  // PlortEconomyDirector.GetCurrValue -> int?
    Bound<Il2CppObject*(Il2CppObject*)> getSelectedId_;          // AmmoSlotManager.GetSelectedId

    // main thread only (mutex for OnDisable from another thread)
    std::mutex mu_;
    std::unordered_map<HeldKey, HeldState, HeldKeyHash> held_;  // key: station + IdentifiableType (identity)
    std::unordered_map<int64_t, CachedLimit> limitCache_;       // main thread only
    std::unordered_map<int64_t, StationLimit> sessionLimits_;   // without custom_upgrades
    bool loggedNoStore_ = false;
    drone_price_limit::StationUi ui_;
    const void* baseEcon_ = nullptr;                            // identity of the econ the table belongs to
    const void* baseArray_ = nullptr;                           // identity of the PlortsTable array
    std::unordered_map<const void*, float> baseValues_;         // IdentifiableType -> InitialValue
};

DronePriceLimit* g_self = nullptr;
sr2::core::DetourGuard g_canDepositGuard{kModule, "ScorePlort.CanDroneDeposit"};
sr2::core::DetourGuard g_dumpGuard{kModule, "RanchDroneBehaviourDumpAmmo.Relevancy"};
sr2::core::DetourGuard g_sinksGuard{kModule, "RanchDroneSinkProgramPlortMarket.<GetSinks>.MoveNext"};
sr2::core::DetourGuard g_uiGuard{kModule, "station ui"};

using CanDepositFn = bool (*)(Il2CppObject*, Il2CppObject*, bool, const MethodInfo*);
using RelevancyFn = bool (*)(Il2CppObject*, bool, const MethodInfo*);
using MoveNextFn = bool (*)(Il2CppObject*, const MethodInfo*);
CanDepositFn g_origCanDeposit = nullptr;
RelevancyFn g_origDumpRelevancy = nullptr;
MoveNextFn g_origSinksMoveNext = nullptr;
size_t g_offIterThis = 0;                      // <GetSinks>d__16.<>4__this
Il2CppObject* g_enumeratingProgram = nullptr;  // main thread: market sink program inside GetSinks

bool CanDroneDepositDetour(Il2CppObject* self, Il2CppObject* id, bool ignoreShutdown, const MethodInfo* mi) {
    const bool r = g_origCanDeposit(self, id, ignoreShutdown, mi);
    if (!r || !g_self || !g_self->Enabled()) return r;
    bool block = false;
    g_canDepositGuard.Run([&] { block = g_self->FilterDeposit(self, id, g_enumeratingProgram); });
    return block ? false : r;
}

// Publishes the market sink program being enumerated (see header). No game calls here; only the
// main thread publishes (drone logic runs there), and the previous value is restored (nesting).
bool SinksMoveNextDetour(Il2CppObject* self, const MethodInfo* mi) {
    if (!main_thread::IsMainThread() || !g_sinksGuard.Active()) return g_origSinksMoveNext(self, mi);
    Il2CppObject* prev = g_enumeratingProgram;
    g_enumeratingProgram = self && g_offIterThis ? unity::Field<Il2CppObject*>(self, g_offIterThis) : nullptr;
    const bool r = g_origSinksMoveNext(self, mi);
    g_enumeratingProgram = prev;
    return r;
}

bool DumpRelevancyDetour(Il2CppObject* self, bool readOnly, const MethodInfo* mi) {
    const bool r = g_origDumpRelevancy(self, readOnly, mi);
    if (!r || !g_self || !g_self->Enabled()) return r;
    bool suppress = false;
    g_dumpGuard.Run([&] { suppress = g_self->SuppressDump(self); });
    return suppress ? false : r;
}

std::string TypeName(Il2CppObject* id) {
    std::string n = unity::GetName(id);
    return n.empty() ? std::string("plort") : n;
}

// ---- evaluation ---------------------------------------------------------------------------

float DronePriceLimit::BaseValue(Il2CppObject* econ, Il2CppObject* id) {
    auto* cfg = unity::Field<Il2CppObject*>(econ, offEconSettings_);
    if (!IsA(cfg, econSettingsClass_) || !unity::IsAlive(cfg)) return 0.0f;
    // PlortsTable is a struct field holding one array reference.
    void* arr = unity::Field<void*>(cfg, offSetTable_ + offTablePlorts_);
    if (!arr) return 0.0f;
    if (econ != baseEcon_ || arr != baseArray_) {
        baseValues_.clear();
        baseEcon_ = econ;
        baseArray_ = arr;
        const size_t n = unity::ArrayLength(arr);
        auto* data = reinterpret_cast<char*>(arr) + unity::kArrayDataOffset;
        for (size_t i = 0; i < n && i < 4096; ++i) {
            char* e = data + i * cfgStride_;
            auto* type = *reinterpret_cast<Il2CppObject**>(e + offCfgType_);
            const float v = *reinterpret_cast<float*>(e + offCfgInitial_);
            if (type && std::isfinite(v)) baseValues_[type] = v;
        }
        SR2_INFO("[{}] base values read: {} plort types", kModule, baseValues_.size());
    }
    auto it = baseValues_.find(id);
    return it == baseValues_.end() ? 0.0f : it->second;
}

int DronePriceLimit::LimitFor(Il2CppObject* econ, Il2CppObject* id, const StationLimit& lim) {
    if (!lim.Active()) return 0;
    if (lim.mode == LimitMode::kPrice) return std::min(lim.value, 1000000);
    const float base = BaseValue(econ, id);
    if (base <= 0.0f) return 0;
    const double rel = std::ceil(static_cast<double>(base) * (lim.value / 100.0) - 1e-4);
    return static_cast<int>(std::clamp(rel, 0.0, 1.0e6));
}

Verdict DronePriceLimit::Evaluate(Il2CppObject* econ, Il2CppObject* id, const StationLimit& lim) {
    Verdict v;
    if (!IsA(econ, econClass_) || !unity::IsAlive(econ) || !IsA(id, identClass_)) return v;
    const uint64_t raw = getCurrValue_(econ, id);  // Nullable<int>: hasValue byte, value at +4
    if ((raw & 0xFF) == 0) return v;               // no market price for this type: not ours to judge
    v.known = true;
    v.price = static_cast<int32_t>(raw >> 32);
    v.limit = LimitFor(econ, id, lim);
    v.hold = v.limit > 0 && v.price < v.limit;
    return v;
}

void DronePriceLimit::Record(int64_t station, Il2CppObject* id, const Verdict& v) {
    std::lock_guard lk(mu_);
    const HeldKey key{station, id};
    auto it = held_.find(key);
    if (v.hold) {
        if (it == held_.end()) it = held_.emplace(key, HeldState{}).first;
        HeldState& s = it->second;
        if (!s.held || s.limit != v.limit) {
            SR2_INFO("[{}] station {}: holding {}: price {} < limit {} (drones keep it until the price recovers)",
                     kModule, station, TypeName(id), v.price, v.limit);
            s.dumpNoted = false;
        }
        s.held = true;
        s.price = v.price;
        s.limit = v.limit;
        anyHeld_.store(true, std::memory_order_relaxed);
    } else if (it != held_.end()) {
        if (it->second.held) {
            if (v.limit > 0)
                SR2_INFO("[{}] station {}: resuming {}: price {} >= limit {}", kModule, station, TypeName(id), v.price,
                         v.limit);
            else
                SR2_INFO("[{}] station {}: resuming {}: price {}, no limit set", kModule, station, TypeName(id), v.price);
        }
        held_.erase(it);
        anyHeld_.store(!held_.empty(), std::memory_order_relaxed);
    }
}

int64_t DronePriceLimit::StationOfDrone(Il2CppObject* drone) {
    if (!IsA(drone, droneClass_) || !unity::IsAlive(drone)) return 0;
    Il2CppObject* model = unity::Field<Il2CppObject*>(drone, offDroneStation_);
    if (!IsA(model, stationModelClass_)) return 0;
    return unity::Field<int64_t>(model, offActorId_);
}

bool DronePriceLimit::FilterDeposit(Il2CppObject* sp, Il2CppObject* id, Il2CppObject* program) {
    if (!program || !main_thread::IsMainThread()) return false;  // not asked by a market sink program
    if (!IsA(program, marketProgramClass_)) return false;
    const int64_t station = StationOfDrone(unity::Field<Il2CppObject*>(program, offProgramDrone_));
    if (station == 0) return false;
    const StationLimit lim = GetLimit(station);
    if (!lim.Active() && !anyHeld_.load(std::memory_order_relaxed)) return false;  // fast path: no effect
    if (!IsA(sp, scorePlortClass_) || !unity::IsAlive(sp) || !id) return false;
    if (unity::Field<bool>(sp, offSpIsCaretaker_)) return false;                    // caretaker shop: not a market
    if (unity::Field<Il2CppObject*>(sp, offSpSprinkle_) == id) return false;        // sprinkles: fixed value 1
    const Verdict v = Evaluate(unity::Field<Il2CppObject*>(sp, offSpEcon_), id, lim);
    if (!v.known) return false;
    Record(station, id, v);
    return v.hold;
}

bool DronePriceLimit::HasMarketProgram(Il2CppObject* drone) {
    auto* plexer = unity::Field<Il2CppObject*>(drone, offDronePlexer_);
    if (!IsA(plexer, behaviourPlexerClass_)) return false;
    auto* programs = unity::Field<Il2CppObject*>(plexer, offPlexerPrograms_);
    if (!IsA(programs, programPlexerClass_)) return false;
    void* sinks = unity::Field<void*>(programs, offProgramsSinks_);
    if (!sinks) return false;
    const size_t n = unity::ArrayLength(sinks);
    for (size_t i = 0; i < n && i < 64; ++i)
        if (IsA(static_cast<Il2CppObject*>(unity::ArrayAt(sinks, i)), marketProgramClass_)) return true;
    return false;
}

bool DronePriceLimit::SuppressDump(Il2CppObject* beh) {
    if (!main_thread::IsMainThread() || !IsA(beh, dumpClass_)) return false;
    auto* drone = unity::Field<Il2CppObject*>(beh, offDumpDrone_);
    if (!IsA(drone, droneClass_) || !unity::IsAlive(drone)) return false;
    const int64_t station = StationOfDrone(drone);
    if (station == 0) return false;
    const StationLimit lim = GetLimit(station);
    if (!lim.Active() && !anyHeld_.load(std::memory_order_relaxed)) return false;
    auto* ammo = unity::Field<Il2CppObject*>(drone, offDroneAmmo_);
    if (!IsA(ammo, ammoClass_)) return false;
    Il2CppObject* id = getSelectedId_(ammo);
    if (!IsA(id, identClass_)) return false;
    const Verdict v = Evaluate(unity::Field<Il2CppObject*>(drone, offDroneEcon_), id, lim);
    if (!v.known) return false;
    Record(station, id, v);
    if (!v.hold || !HasMarketProgram(drone)) return false;
    std::lock_guard lk(mu_);
    auto it = held_.find(HeldKey{station, id});
    if (it != held_.end() && !it->second.dumpNoted) {
        it->second.dumpNoted = true;
        SR2_INFO("[{}] station {}: a drone carrying {} waits at its station instead of dumping it (price {} < limit {})",
                 kModule, station, TypeName(id), v.price, v.limit);
    }
    return true;
}

// ---- per-station limits --------------------------------------------------------------------

StationLimit DronePriceLimit::GetLimit(int64_t station) {
    if (station == 0) return {};
    if (resetCache_.exchange(false)) limitCache_.clear();
    const unsigned long long tick = main_thread::TickCount();
    auto it = limitCache_.find(station);
    if (it != limitCache_.end() && tick - it->second.tick < kLimitCacheFrames) return it->second.limit;
    StationLimit l;
    if (custom_upgrades::Active()) {
        const std::string raw = custom_upgrades::SaveValue(kStationKeyPrefix + std::to_string(station));
        if (!drone_price_limit::Parse(raw, l)) {
            SR2_WARN("[{}] station {}: stored limit '{}' not understood - no limit", kModule, station, raw);
            l = {};
        }
    } else if (auto s = sessionLimits_.find(station); s != sessionLimits_.end()) {
        l = s->second;
    }
    if (limitCache_.size() > 512) limitCache_.clear();
    limitCache_[station] = {l, tick};
    return l;
}

void DronePriceLimit::SetLimit(int64_t station, const StationLimit& l) {
    if (station == 0) return;
    const std::string key = kStationKeyPrefix + std::to_string(station);
    bool stored = false;
    if (custom_upgrades::Active()) stored = custom_upgrades::SetSaveValue(key, drone_price_limit::Serialize(l));
    if (!stored) {
        if (!loggedNoStore_)
            SR2_WARN("[{}] station limits can't go into the save's sidecar (custom_upgrades off or no save loaded) - "
                     "kept for this session only", kModule);
        loggedNoStore_ = true;
        if (l.Active()) sessionLimits_[station] = l;
        else sessionLimits_.erase(station);
    }
    limitCache_[station] = {l, main_thread::TickCount()};
    SR2_INFO("[{}] station {}: limit set to {}{}", kModule, station, drone_price_limit::Describe(l),
             stored ? " (saved with the next game save)" : " (session only)");
}

Il2CppObject* DronePriceLimit::SceneEcon() {
    if (!sceneCtxInstMi_ || !offScEcon_) return nullptr;
    std::string err;
    Il2CppObject* sc = Resolver::Get().Invoke(sceneCtxInstMi_, nullptr, nullptr, &err);
    if (!err.empty() || !unity::IsAlive(sc)) return nullptr;
    Il2CppObject* econ = unity::Field<Il2CppObject*>(sc, offScEcon_);
    return IsA(econ, econClass_) && unity::IsAlive(econ) ? econ : nullptr;
}

float DronePriceLimit::BaseValueOf(Il2CppObject* id) {
    Il2CppObject* econ = SceneEcon();
    return econ && IsA(id, identClass_) ? BaseValue(econ, id) : 0.0f;
}

void DronePriceLimit::Frame(float dt) {
    if (!ui_.Ready() || !Enabled() || sr2::core::ModuleFaulted(kModule)) return;
    g_uiGuard.Run([&] { ui_.Tick(dt); });
}

// ---- lifecycle --------------------------------------------------------------------------

void DronePriceLimit::OnDisable() {
    enabled_.store(false);
    {
        std::lock_guard lk(mu_);
        if (!held_.empty())
            SR2_INFO("[{}] disabled: {} held plort type(s) released, drones sell at any price again", kModule,
                     held_.size());
        held_.clear();
        anyHeld_.store(false);
    }
    // The station menu action / dialog go away (main thread).
    if (main_thread::IsMainThread()) g_uiGuard.Run([&] { ui_.Shutdown(); });
    else main_thread::Post([this] { g_uiGuard.Run([&] { ui_.Shutdown(); }); });
}

bool DronePriceLimit::Setup(sr2::core::ModuleContext& ctx) {
    auto& R = ctx.resolver();
    std::string why;
    auto cls = [&](const char* ns, const char* name) -> Il2CppClass* {
        std::string err;
        Il2CppClass* k = R.Class(kImage, ns, name, &err);
        if (!k && why.empty()) why = err.empty() ? std::format("class {}.{} not found", ns, name) : err;
        return k;
    };
    auto off = [&](Il2CppClass* k, const char* field, size_t& out) -> bool {
        if (!k) return false;
        std::string err;
        auto o = R.FieldOffset(k, field, &err);
        if (!o) {
            if (why.empty()) why = err.empty() ? std::format("field {} not found", field) : err;
            return false;
        }
        out = *o;
        return true;
    };
    // Field of a value type: il2cpp offsets include the boxed object header.
    auto structOff = [&](Il2CppClass* k, const char* field, size_t& out) -> bool {
        if (!off(k, field, out)) return false;
        if (out >= 2 * sizeof(void*)) out -= 2 * sizeof(void*);
        return true;
    };
    auto method = [&](Il2CppClass* k, const char* name, int argc) -> const MethodInfo* {
        if (!k) return nullptr;
        std::string err;
        const MethodInfo* m = R.Method(k, name, argc, &err);
        if (!m && why.empty()) why = std::format("method {}({}) not found: {}", name, argc, err);
        return m;
    };
    auto bind = [&](auto& b, const MethodInfo* m) -> bool {
        if (!m) return false;
        std::string err;
        void* p = R.MethodPointer(m, &err);
        if (!p) {
            if (why.empty()) why = "no native body: " + err;
            return false;
        }
        using B = std::remove_reference_t<decltype(b)>;
        b.fn = reinterpret_cast<typename B::Fn>(p);
        b.mi = m;
        return true;
    };

    scorePlortClass_ = cls("", "ScorePlort");
    identClass_ = cls("", "IdentifiableType");
    econClass_ = cls(kEconNs, "PlortEconomyDirector");
    econSettingsClass_ = cls(kEconNs, "PlortEconomySettings");
    Il2CppClass* tableClass = cls(kEconNs, "PlortValueConfigurationTable");
    Il2CppClass* cfgClass = cls(kEconNs, "PlortValueConfiguration");
    dumpClass_ = cls(kDroneNs, "RanchDroneBehaviourDumpAmmo");
    droneClass_ = cls(kDroneNs, "RanchDrone");
    behaviourPlexerClass_ = cls(kDroneNs, "DroneBehaviorPlexer");
    programPlexerClass_ = cls(kDroneNs, "RanchDroneProgramPlexer");
    marketProgramClass_ = cls(kDroneNs, "RanchDroneSinkProgramPlortMarket");
    ammoClass_ = cls("MonomiPark.SlimeRancher.Player", "AmmoSlotManager");
    stationModelClass_ = cls("MonomiPark.SlimeRancher.DataModel", "DroneStationGadgetModel");
    Il2CppClass* identModel = cls("MonomiPark.SlimeRancher.DataModel", "IdentifiableModel");
    Il2CppClass* sceneCtx = cls("", "SceneContext");
    if (!why.empty()) return ctx.Fail("resolve failed: " + why);

    size_t offCfgFullSat = 0;
    off(scorePlortClass_, "_isCaretakerShop", offSpIsCaretaker_);
    off(scorePlortClass_, "_sprinkleType", offSpSprinkle_);
    off(scorePlortClass_, "_econDir", offSpEcon_);
    off(econClass_, "_settings", offEconSettings_);
    off(econSettingsClass_, "PlortsTable", offSetTable_);
    structOff(tableClass, "Plorts", offTablePlorts_);
    structOff(cfgClass, "Type", offCfgType_);
    structOff(cfgClass, "InitialValue", offCfgInitial_);
    structOff(cfgClass, "FullSaturation", offCfgFullSat);
    off(dumpClass_, "_drone", offDumpDrone_);
    off(droneClass_, "_plexer", offDronePlexer_);
    off(droneClass_, "_econDir", offDroneEcon_);
    off(droneClass_, "_ammo", offDroneAmmo_);
    off(behaviourPlexerClass_, "_programPlexer", offPlexerPrograms_);
    off(programPlexerClass_, "_sinks", offProgramsSinks_);
    off(droneClass_, "_stationModel", offDroneStation_);
    off(identModel, "actorId", offActorId_);
    off(sceneCtx, "<PlortEconomyDirector>k__BackingField", offScEcon_);
    // RanchDroneSinkProgramBase<ScorePlort>._drone: a field of the (inflated) generic base class.
    if (api().il2cpp_class_get_parent) {
        for (Il2CppClass* c = api().il2cpp_class_get_parent(marketProgramClass_); c && !offProgramDrone_;
             c = api().il2cpp_class_get_parent(c)) {
            std::string err;
            if (auto o = R.FieldOffset(c, "_drone", &err)) offProgramDrone_ = *o;
        }
    }
    if (!offProgramDrone_ && why.empty()) why = "field RanchDroneSinkProgramBase<ScorePlort>._drone not found";
    // The GetSinks iterator: nested class "<GetSinks>d__N" of the market program.
    Il2CppClass* sinksIter = nullptr;
    {
        using NestedFn = Il2CppClass* (*)(Il2CppClass*, void**);
        HMODULE ga = api().module;
        auto nested = reinterpret_cast<NestedFn>(ga ? GetProcAddress(ga, "il2cpp_class_get_nested_types") : nullptr);
        void* iter = nullptr;
        while (nested && api().il2cpp_class_get_name) {
            Il2CppClass* n = nested(marketProgramClass_, &iter);
            if (!n) break;
            const char* nn = api().il2cpp_class_get_name(n);
            if (nn && std::string_view(nn).starts_with("<GetSinks>d__")) sinksIter = n;
        }
        if (!sinksIter && why.empty()) why = "nested class RanchDroneSinkProgramPlortMarket.<GetSinks>d__* not found";
        if (sinksIter) off(sinksIter, "<>4__this", g_offIterThis);
    }
    if (!why.empty()) return ctx.Fail("resolve failed: " + why);
    if (Il2CppClass* single = api().il2cpp_class_get_parent ? api().il2cpp_class_get_parent(sceneCtx) : nullptr) {
        std::string err;
        sceneCtxInstMi_ = R.Method(single, "get_Instance", 0, &err);  // optional: the dialog's base prices
    }
    // PlortValueConfiguration { IdentifiableType Type; float InitialValue; float FullSaturation; }:
    // element size = last field end rounded up to pointer alignment (16 on x64).
    {
        const size_t end = std::max({offCfgType_ + sizeof(void*), offCfgInitial_ + 4, offCfgFullSat + 4});
        cfgStride_ = (end + sizeof(void*) - 1) & ~(sizeof(void*) - 1);
        if (cfgStride_ != 16 || offCfgType_ != 0)
            return ctx.Fail(std::format("unexpected PlortValueConfiguration layout (Type +0x{:X}, stride {})",
                                        offCfgType_, cfgStride_));
    }

    bind(getCurrValue_, method(econClass_, "GetCurrValue", 1));
    bind(getSelectedId_, method(ammoClass_, "GetSelectedId", 0));
    const MethodInfo* canMi = method(scorePlortClass_, "CanDroneDeposit", 2);
    const MethodInfo* relMi = method(dumpClass_, "Relevancy", 1);
    const MethodInfo* moveNextMi = method(sinksIter, "MoveNext", 0);
    if (!why.empty()) return ctx.Fail(why);

    struct H {
        const char* name;
        const MethodInfo* mi;
        void* detour;
        void** orig;
    };
    const H hooks[] = {
        {"ScorePlort.CanDroneDeposit", canMi, reinterpret_cast<void*>(&CanDroneDepositDetour),
         reinterpret_cast<void**>(&g_origCanDeposit)},
        {"RanchDroneBehaviourDumpAmmo.Relevancy", relMi, reinterpret_cast<void*>(&DumpRelevancyDetour),
         reinterpret_cast<void**>(&g_origDumpRelevancy)},
        {"RanchDroneSinkProgramPlortMarket.<GetSinks>.MoveNext", moveNextMi, reinterpret_cast<void*>(&SinksMoveNextDetour),
         reinterpret_cast<void**>(&g_origSinksMoveNext)},
    };
    std::string report;
    for (const H& h : hooks) {
        std::string err;
        void* p = R.MethodPointer(h.mi, &err);
        if (!p) return ctx.Fail(std::format("{}: no native body ({})", h.name, err));
        if (!ctx.HookAddress(h.name, p, h.detour, h.orig)) return ctx.Fail(std::format("hook {} failed", h.name));
        report += std::format(" {}@0x{:X}", h.name, Resolver::Rva(p));
    }

    // The station menu action + dialog (optional: without it the limits can't be changed, but stored
    // limits keep working).
    {
        drone_price_limit::DialogHost host;
        host.get = [this](int64_t st) { return GetLimit(st); };
        host.set = [this](int64_t st, const StationLimit& l) { SetLimit(st, l); };
        host.persistent = [] { return custom_upgrades::Active(); };
        host.baseValue = [this](Il2CppObject* id) { return BaseValueOf(id); };
        std::string uiWhy;
        if (!ui_.Setup(ctx, kModule, std::move(host), uiWhy))
            SR2_WARN("[{}] station menu action NOT available: {} (stored limits still apply)", kModule, uiWhy);
        else
            main_thread::AddFrameCallback(kModule, [](float dt) {
                if (g_self) g_self->Frame(dt);
            });
    }

    g_self = this;
    SR2_INFO("[{}] setup ok:{}; ScorePlort _isCaretakerShop +0x{:X} _sprinkleType +0x{:X} _econDir +0x{:X}; "
             "RanchDrone _plexer +0x{:X} _econDir +0x{:X} _ammo +0x{:X} _stationModel +0x{:X}; program _drone +0x{:X}; "
             "iterator <>4__this +0x{:X}; actorId +0x{:X}; base table +0x{:X}+0x{:X} stride {}",
             kModule, report, offSpIsCaretaker_, offSpSprinkle_, offSpEcon_, offDronePlexer_, offDroneEcon_,
             offDroneAmmo_, offDroneStation_, offProgramDrone_, g_offIterThis, offActorId_, offSetTable_,
             offTablePlorts_, cfgStride_);
    return true;
}

}  // namespace

std::unique_ptr<sr2::core::Module> CreateDronePriceLimit() { return std::make_unique<DronePriceLimit>(); }

}  // namespace sr2::modules
