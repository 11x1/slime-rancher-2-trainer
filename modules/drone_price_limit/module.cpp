// drone_price_limit: quantum (ranch) drones only sell plorts to a Plort Market / Market
// Link while that plort's current market price is at or above a limit.
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
// What this module does:
//   1. Detour ScorePlort.CanDroneDeposit: after the original said yes, for a real market
//      (not the caretaker shop, not the sprinkle type) and a type with a market price,
//      return false while price < limit. limit = max(min_price,
//      ceil(min_price_fraction_of_base * base value)); the base value is the plort's
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
// Per call cost: CanDroneDeposit runs whenever a drone evaluates market sinks. With both
// limits at 0 and nothing held it is one atomic load after the original; otherwise a few
// field reads, one native GetCurrValue call and a hash lookup. No per-frame work.
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
#include <unordered_map>

#include "core/detour_guard.h"
#include "core/il2cpp_api.h"
#include "core/log.h"
#include "core/main_thread.h"
#include "core/module.h"
#include "core/settings.h"
#include "core/unity.h"

namespace sr2::modules {
namespace {

namespace unity = sr2::core::unity;
namespace settings = sr2::core::settings;
namespace main_thread = sr2::core::main_thread;
using sr2::il2cpp::api;
using sr2::il2cpp::Resolver;

constexpr const char* kModule = "drone_price_limit";
constexpr const char* kImage = "Assembly-CSharp";
constexpr const char* kDroneNs = "MonomiPark.SlimeRancher.Drone";
constexpr const char* kEconNs = "MonomiPark.SlimeRancher.Economy";
constexpr const char* kKeyMinPrice = "min_price";
constexpr const char* kKeyFraction = "min_price_fraction_of_base";

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
    bool DefaultEnabled() const override { return true; }
    bool Setup(sr2::core::ModuleContext& ctx) override;
    void OnEnable() override {
        enabled_.store(true);
        LogSettings("enabled");
    }
    void OnDisable() override;
    bool Enabled() const { return enabled_.load(std::memory_order_relaxed); }

    // Main thread, inside DetourGuards. true = report "cannot deposit".
    bool FilterDeposit(Il2CppObject* scorePlort, Il2CppObject* id);
    // true = suppress the dump.
    bool SuppressDump(Il2CppObject* dumpBehaviour);

private:
    struct HeldState {
        bool held = false;
        bool dumpNoted = false;
        int price = 0;
        int limit = 0;
    };

    Verdict Evaluate(Il2CppObject* econ, Il2CppObject* id);
    int LimitFor(Il2CppObject* econ, Il2CppObject* id, int minPrice, float fraction);
    float BaseValue(Il2CppObject* econ, Il2CppObject* id);
    void Record(Il2CppObject* id, const Verdict& v);
    bool HasMarketProgram(Il2CppObject* drone);
    void ReadSettings();
    void LogSettings(const char* why);
    bool LimitsActive() const {
        return minPrice_.load(std::memory_order_relaxed) > 0 || fraction_.load(std::memory_order_relaxed) > 0.0f;
    }

    std::atomic<bool> enabled_{false};
    std::atomic<int> minPrice_{0};
    std::atomic<float> fraction_{0.0f};
    std::atomic<bool> anyHeld_{false};

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

    Bound<uint64_t(Il2CppObject*, Il2CppObject*)> getCurrValue_;  // PlortEconomyDirector.GetCurrValue -> int?
    Bound<Il2CppObject*(Il2CppObject*)> getSelectedId_;          // AmmoSlotManager.GetSelectedId

    // main thread only (mutex for OnDisable from another thread)
    std::mutex mu_;
    std::unordered_map<const void*, HeldState> held_;          // key: IdentifiableType (identity only)
    const void* baseEcon_ = nullptr;                            // identity of the econ the table belongs to
    const void* baseArray_ = nullptr;                           // identity of the PlortsTable array
    std::unordered_map<const void*, float> baseValues_;         // IdentifiableType -> InitialValue
};

DronePriceLimit* g_self = nullptr;
sr2::core::DetourGuard g_canDepositGuard{kModule, "ScorePlort.CanDroneDeposit"};
sr2::core::DetourGuard g_dumpGuard{kModule, "RanchDroneBehaviourDumpAmmo.Relevancy"};

using CanDepositFn = bool (*)(Il2CppObject*, Il2CppObject*, bool, const MethodInfo*);
using RelevancyFn = bool (*)(Il2CppObject*, bool, const MethodInfo*);
CanDepositFn g_origCanDeposit = nullptr;
RelevancyFn g_origDumpRelevancy = nullptr;

bool CanDroneDepositDetour(Il2CppObject* self, Il2CppObject* id, bool ignoreShutdown, const MethodInfo* mi) {
    const bool r = g_origCanDeposit(self, id, ignoreShutdown, mi);
    if (!r || !g_self || !g_self->Enabled()) return r;
    bool block = false;
    g_canDepositGuard.Run([&] { block = g_self->FilterDeposit(self, id); });
    return block ? false : r;
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

int DronePriceLimit::LimitFor(Il2CppObject* econ, Il2CppObject* id, int minPrice, float fraction) {
    int limit = std::max(minPrice, 0);
    if (fraction > 0.0f) {
        const float base = BaseValue(econ, id);
        if (base > 0.0f) {
            const double rel = std::ceil(static_cast<double>(base) * fraction - 1e-4);
            if (rel > limit) limit = static_cast<int>(std::min(rel, 1.0e6));
        }
    }
    return limit;
}

Verdict DronePriceLimit::Evaluate(Il2CppObject* econ, Il2CppObject* id) {
    Verdict v;
    if (!IsA(econ, econClass_) || !unity::IsAlive(econ) || !IsA(id, identClass_)) return v;
    const uint64_t raw = getCurrValue_(econ, id);  // Nullable<int>: hasValue byte, value at +4
    if ((raw & 0xFF) == 0) return v;               // no market price for this type: not ours to judge
    v.known = true;
    v.price = static_cast<int32_t>(raw >> 32);
    v.limit = LimitFor(econ, id, minPrice_.load(std::memory_order_relaxed), fraction_.load(std::memory_order_relaxed));
    v.hold = v.limit > 0 && v.price < v.limit;
    return v;
}

void DronePriceLimit::Record(Il2CppObject* id, const Verdict& v) {
    std::lock_guard lk(mu_);
    auto it = held_.find(id);
    if (v.hold) {
        if (it == held_.end()) it = held_.emplace(id, HeldState{}).first;
        HeldState& s = it->second;
        if (!s.held || s.limit != v.limit) {
            SR2_INFO("[{}] holding {}: price {} < limit {} (drones keep it until the price recovers)", kModule,
                     TypeName(id), v.price, v.limit);
            s.dumpNoted = false;
        }
        s.held = true;
        s.price = v.price;
        s.limit = v.limit;
        anyHeld_.store(true, std::memory_order_relaxed);
    } else if (it != held_.end()) {
        if (it->second.held) {
            if (v.limit > 0)
                SR2_INFO("[{}] resuming {}: price {} >= limit {}", kModule, TypeName(id), v.price, v.limit);
            else
                SR2_INFO("[{}] resuming {}: price {}, no limit set", kModule, TypeName(id), v.price);
        }
        held_.erase(it);
        anyHeld_.store(!held_.empty(), std::memory_order_relaxed);
    }
}

bool DronePriceLimit::FilterDeposit(Il2CppObject* sp, Il2CppObject* id) {
    if (!LimitsActive() && !anyHeld_.load(std::memory_order_relaxed)) return false;  // fast path: no effect
    if (!main_thread::IsMainThread()) return false;
    if (!IsA(sp, scorePlortClass_) || !unity::IsAlive(sp) || !id) return false;
    if (unity::Field<bool>(sp, offSpIsCaretaker_)) return false;                    // caretaker shop: not a market
    if (unity::Field<Il2CppObject*>(sp, offSpSprinkle_) == id) return false;        // sprinkles: fixed value 1
    const Verdict v = Evaluate(unity::Field<Il2CppObject*>(sp, offSpEcon_), id);
    if (!v.known) return false;
    Record(id, v);
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
    if (!anyHeld_.load(std::memory_order_relaxed) && !LimitsActive()) return false;
    if (!main_thread::IsMainThread() || !IsA(beh, dumpClass_)) return false;
    auto* drone = unity::Field<Il2CppObject*>(beh, offDumpDrone_);
    if (!IsA(drone, droneClass_) || !unity::IsAlive(drone)) return false;
    auto* ammo = unity::Field<Il2CppObject*>(drone, offDroneAmmo_);
    if (!IsA(ammo, ammoClass_)) return false;
    Il2CppObject* id = getSelectedId_(ammo);
    if (!IsA(id, identClass_)) return false;
    const Verdict v = Evaluate(unity::Field<Il2CppObject*>(drone, offDroneEcon_), id);
    if (!v.known) return false;
    Record(id, v);
    if (!v.hold || !HasMarketProgram(drone)) return false;
    std::lock_guard lk(mu_);
    auto it = held_.find(id);
    if (it != held_.end() && !it->second.dumpNoted) {
        it->second.dumpNoted = true;
        SR2_INFO("[{}] a drone carrying {} waits at its station instead of dumping it (price {} < limit {})", kModule,
                 TypeName(id), v.price, v.limit);
    }
    return true;
}

// ---- settings / lifecycle ---------------------------------------------------------------

void DronePriceLimit::ReadSettings() {
    minPrice_.store(std::max(0, settings::GetInt(kModule, kKeyMinPrice)));
    float f = settings::GetFloat(kModule, kKeyFraction);
    fraction_.store(std::isfinite(f) && f > 0.0f ? f : 0.0f);
}

void DronePriceLimit::LogSettings(const char* why) {
    const int mp = minPrice_.load();
    const float fr = fraction_.load();
    if (mp <= 0 && fr <= 0.0f)
        SR2_INFO("[{}] {}: no limit set (min_price 0, fraction 0) - drones sell at any price", kModule, why);
    else
        SR2_INFO("[{}] {}: drones sell a plort only at price >= max({}, ceil({:.2f} x base value))", kModule, why, mp,
                 fr);
}

void DronePriceLimit::OnDisable() {
    enabled_.store(false);
    std::lock_guard lk(mu_);
    if (!held_.empty())
        SR2_INFO("[{}] disabled: {} held plort type(s) released, drones sell at any price again", kModule,
                 held_.size());
    held_.clear();
    anyHeld_.store(false);
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
    if (!why.empty()) return ctx.Fail("resolve failed: " + why);
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
    };
    std::string report;
    for (const H& h : hooks) {
        std::string err;
        void* p = R.MethodPointer(h.mi, &err);
        if (!p) return ctx.Fail(std::format("{}: no native body ({})", h.name, err));
        if (!ctx.HookAddress(h.name, p, h.detour, h.orig)) return ctx.Fail(std::format("hook {} failed", h.name));
        report += std::format(" {}@0x{:X}", h.name, Resolver::Rva(p));
    }

    settings::Def d;
    d.module = kModule;
    d.key = kKeyMinPrice;
    d.label = "Minimum sell price";
    d.kind = settings::Kind::Int;
    d.def = 0;
    d.min = 0;
    d.max = 1000;
    d.step = 1;
    d.tooltip =
        "Quantum drones only sell a plort to the Plort Market / Market Link while its current market price is at "
        "least this many Newbucks. Below it they keep the plort (they don't pick it up, or wait at their station "
        "with it) and sell once the price recovers. 0 = no limit. Also applies while you sleep or are away.";
    settings::Register(d);

    settings::Def f;
    f.module = kModule;
    f.key = kKeyFraction;
    f.label = "Minimum price (fraction of base value)";
    f.kind = settings::Kind::Float;
    f.def = 0.0;
    f.min = 0.0;
    f.max = 3.0;
    f.step = 0.05;
    f.tooltip =
        "Relative limit: sell a plort only when its price is at least this fraction of that plort's base value "
        "(1.0 = base value, 0.8 = 80 %). 0 = off. If both limits are set, the higher one counts.";
    settings::Register(f);

    ReadSettings();
    settings::OnChange(kModule, [this](const std::string& key) {
        if (key != kKeyMinPrice && key != kKeyFraction) return;
        ReadSettings();
        LogSettings("limit changed");
    });

    g_self = this;
    SR2_INFO("[{}] setup ok:{}; ScorePlort _isCaretakerShop +0x{:X} _sprinkleType +0x{:X} _econDir +0x{:X}; "
             "RanchDrone _plexer +0x{:X} _econDir +0x{:X} _ammo +0x{:X}; base table +0x{:X}+0x{:X} stride {}",
             kModule, report, offSpIsCaretaker_, offSpSprinkle_, offSpEcon_, offDronePlexer_, offDroneEcon_,
             offDroneAmmo_, offSetTable_, offTablePlorts_, cfgStride_);
    return true;
}

}  // namespace

std::unique_ptr<sr2::core::Module> CreateDronePriceLimit() { return std::make_unique<DronePriceLimit>(); }

}  // namespace sr2::modules
