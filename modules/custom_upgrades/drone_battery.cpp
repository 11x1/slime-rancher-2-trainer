// custom_upgrades: Drone Battery Saver effect (see drone_battery.h). SR2 1.3.0, everything by name.

#include "modules/custom_upgrades/drone_battery.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <format>

#include "core/detour_guard.h"
#include "core/il2cpp_api.h"
#include "core/log.h"
#include "core/module.h"
#include "core/resolver.h"
#include "core/unity.h"

namespace sr2::modules::custom_upgrades::drones {
namespace {

namespace unity = sr2::core::unity;
using sr2::il2cpp::api;
using sr2::il2cpp::Resolver;

constexpr const char* kImage = "Assembly-CSharp";
constexpr float kTickSeconds = 2.0f;  // refund interval (real time)
constexpr double kSecondsPerHour = 3600.0;
constexpr float kMinRefund = 0.01f;   // energy units (max 1000)
constexpr float kSummarySeconds = 60.0f;  // one summary line a minute (real time)

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

}  // namespace

std::string BatteryLevelName(int level) {
    static const char* kRoman[] = {"", "I", "II", "III", "IV"};
    if (level <= 0 || level > kBatteryLevels) return "Drone Battery Saver";
    return std::string("Drone Battery Saver ") + kRoman[level];
}

bool BatterySaver::Setup(Resolver& R, const char* module, std::function<int()> level, std::string& why) {
    module_ = module;
    level_ = std::move(level);
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
    Il2CppClass* sceneCtx = cls("", "SceneContext");
    Il2CppClass* gameModel = cls("MonomiPark.SlimeRancher.DataModel", "GameModel");
    Il2CppClass* droneDir = cls("MonomiPark.SlimeRancher.Drone", "DroneDirector");
    Il2CppClass* timeDir = cls("", "TimeDirector");
    stationModelClass_ = cls("MonomiPark.SlimeRancher.DataModel", "DroneStationGadgetModel");
    Il2CppClass* identModel = cls("MonomiPark.SlimeRancher.DataModel", "IdentifiableModel");
    if (!why.empty()) return false;
    off(sceneCtx, "<GameModel>k__BackingField", offScGame_);
    off(sceneCtx, "<DroneDirector>k__BackingField", offScDrones_);
    off(sceneCtx, "<TimeDirector>k__BackingField", offScTime_);
    off(gameModel, "identifiablesByIdent", offByIdent_);
    off(droneDir, "_droneStationType", offStationType_);
    off(droneDir, "EnergyDepletedPerHour", offRate_);
    off(stationModelClass_, "_energyDepleteTime", offDeplete_);
    off(identModel, "actorId", offActorId_);
    sceneCtxInst_ = method(parentOf(sceneCtx), "get_Instance", 0);
    worldTime_ = method(timeDir, "WorldTime", 0);
    addEnergy_ = method(stationModelClass_, "AddEnergy", 3);
    if (!why.empty()) return false;
    {
        // Optional: the blueprint check for the Fabricator card (without it the card has no prerequisite).
        std::string err;
        gadgetDefClass_ = R.Class(kImage, "", "GadgetDefinition", &err);
        Il2CppClass* gadgetDir = R.Class(kImage, "", "GadgetDirector", &err);
        if (auto o = R.FieldOffset(sceneCtx, "<GadgetDirector>k__BackingField", &err)) offScGadgets_ = *o;
        hasBlueprint_ = gadgetDir ? R.Method(gadgetDir, "HasBlueprint", 1, &err) : nullptr;
        if (!gadgetDefClass_ || !offScGadgets_ || !hasBlueprint_) {
            hasBlueprint_ = nullptr;
            SR2_WARN("[{}] battery saver: Drone Station blueprint check unavailable ({}) - the Fabricator card has no "
                     "prerequisite", module_, err);
        }
    }
    ready_ = true;
    SR2_INFO("[{}] battery saver ready: levels x{} / x{} / x{} / x{} battery life on every drone station", module_,
             kBatteryFactor[1], kBatteryFactor[2], kBatteryFactor[3], kBatteryFactor[4]);
    return true;
}

Il2CppObject* BatterySaver::Call(const MethodInfo* m, void* obj, void** args, const char* what, bool* ok) {
    std::string err;
    Il2CppObject* r = Resolver::Get().Invoke(m, obj, args, &err);
    if (ok) *ok = err.empty();
    if (!err.empty()) {
        if (err.rfind("native exception", 0) == 0) sr2::core::MarkModuleFaulted(module_, std::format("{}: {}", what, err));
        else SR2_WARN("[{}] battery saver: {} failed: {}", module_, what, err);
    }
    return r;
}

Il2CppObject* BatterySaver::SceneContext() {
    bool ok = false;
    Il2CppObject* sc = Call(sceneCtxInst_, nullptr, nullptr, "SceneContext.Instance", &ok);
    return ok && unity::IsAlive(sc) ? sc : nullptr;
}

Il2CppObject* BatterySaver::StationDefinition() {
    if (!ready_) return nullptr;
    Il2CppObject* sc = SceneContext();
    Il2CppObject* dd = sc ? unity::Field<Il2CppObject*>(sc, offScDrones_) : nullptr;
    if (!unity::IsAlive(dd)) return nullptr;
    Il2CppObject* def = unity::Field<Il2CppObject*>(dd, offStationType_);
    return unity::IsAlive(def) ? def : nullptr;
}

int BatterySaver::HasStationBlueprint() {
    if (!ready_ || !hasBlueprint_) return -1;
    Il2CppObject* def = StationDefinition();
    Il2CppObject* sc = def ? SceneContext() : nullptr;
    Il2CppObject* gd = sc ? unity::Field<Il2CppObject*>(sc, offScGadgets_) : nullptr;
    if (!IsA(def, gadgetDefClass_) || !unity::IsAlive(gd)) return -1;
    void* args[] = {def};
    bool ok = false;
    const bool has = Unbox<bool>(Call(hasBlueprint_, gd, args, "GadgetDirector.HasBlueprint", &ok));
    return ok ? (has ? 1 : 0) : -1;
}

void BatterySaver::Reset() {
    looks_.clear();
    gameModel_ = nullptr;
}

void BatterySaver::Tick(float unscaledDt) {
    if (!ready_ || sr2::core::ModuleFaulted(module_)) return;
    timer_ += unscaledDt;
    if (timer_ < kTickSeconds) return;
    timer_ = 0.0f;
    const int level = level_ ? std::clamp(level_(), 0, kBatteryLevels) : 0;
    Il2CppObject* sc = SceneContext();
    if (!sc) return;
    Il2CppObject* gm = unity::Field<Il2CppObject*>(sc, offScGame_);
    Il2CppObject* dd = unity::Field<Il2CppObject*>(sc, offScDrones_);
    Il2CppObject* td = unity::Field<Il2CppObject*>(sc, offScTime_);
    if (!gm || !unity::IsAlive(dd) || !unity::IsAlive(td)) return;
    if (gm != gameModel_) {  // another game: the last looks belong to the old one
        looks_.clear();
        gameModel_ = gm;
    }
    Il2CppObject* stationType = unity::Field<Il2CppObject*>(dd, offStationType_);
    float rate = unity::Field<float>(dd, offRate_);
    Il2CppObject* dict = unity::Field<Il2CppObject*>(gm, offByIdent_);
    if (!stationType || !dict || !std::isfinite(rate) || rate <= 0.0f) return;
    // GameModel.AllDroneStations: identifiablesByIdent.TryGetValue(DroneDirector._droneStationType).
    Il2CppClass* dk = api().il2cpp_object_get_class(dict);
    if (dk != tryGetValueClass_) {
        std::string err;
        tryGetValue_ = Resolver::Get().Method(dk, "TryGetValue", 2, &err);
        tryGetValueClass_ = dk;
        if (!tryGetValue_ && !loggedFail_) SR2_WARN("[{}] battery saver: Dictionary.TryGetValue not found: {}", module_, err);
        loggedFail_ = loggedFail_ || !tryGetValue_;
    }
    if (!tryGetValue_) return;
    Il2CppObject* list = nullptr;
    void* args[] = {stationType, &list};
    bool ok = false;
    const bool found = Unbox<bool>(Call(tryGetValue_, dict, args, "identifiablesByIdent.TryGetValue", &ok));
    if (!ok || !found || !list) return;
    bool okTime = false;
    const double now = Unbox<double>(Call(worldTime_, td, nullptr, "TimeDirector.WorldTime", &okTime));
    if (!okTime || !std::isfinite(now)) return;
    summaryTimer_ += kTickSeconds;
    const bool summary = summaryTimer_ >= kSummarySeconds;
    if (summary) summaryTimer_ = 0.0f;
    double summaryTotal = 0.0;
    int summaryStations = 0;
    std::string summaryList;
    for (size_t i = 0, n = unity::ListCount(list); i < n && i < 256; ++i) {
        auto* model = static_cast<Il2CppObject*>(unity::ListAt(list, i));
        if (!IsA(model, stationModelClass_)) continue;
        const int64_t station = unity::Field<int64_t>(model, offActorId_);
        if (station == 0) continue;
        auto [it, fresh] = looks_.try_emplace(station);
        Look& look = it->second;
        const double t0 = look.time;
        if (fresh || now < t0) {  // first look (or time went back): nothing to compare yet
            look.time = now;
            continue;
        }
        if (level != look.level) {
            if (level > 0)
                SR2_INFO("[{}] battery saver: station {}: {} active (battery drain x{:.2f}: {:.2f} instead of {:.2f} "
                         "energy per game hour)", module_, station, BatteryLevelName(level),
                         1.0f / kBatteryFactor[level], rate / kBatteryFactor[level], rate);
            look.level = level;
            look.first = true;
            look.time = now;
            continue;  // the new factor counts from this look on
        }
        const double dt = now - t0;
        if (level <= 0 || dt <= 0.0) {
            look.time = now;
            continue;
        }
        Il2CppObject* obs = unity::Field<Il2CppObject*>(model, offDeplete_);
        if (!obs) continue;
        if (!offObsValue_) {  // ObservableValue<double>._value: a field of the inflated class
            std::string err;
            if (auto o = Resolver::Get().FieldOffset(api().il2cpp_object_get_class(obs), "_value", &err)) offObsValue_ = *o;
            if (!offObsValue_) {
                if (!loggedFail_) SR2_WARN("[{}] battery saver: ObservableValue<double>._value not found: {}", module_, err);
                loggedFail_ = true;
                return;
            }
        }
        const double deplete = unity::Field<double>(obs, offObsValue_);
        const double left0 = deplete - t0;  // battery seconds at the last look
        if (!std::isfinite(deplete) || left0 <= 0.0) {  // was already empty
            look.time = now;
            continue;
        }
        const double left1 = std::max(0.0, left0 - dt / kBatteryFactor[level]);
        const double vanilla = std::max(0.0, deplete - now);
        float refund = static_cast<float>((left1 - vanilla) / kSecondsPerHour * rate);
        const double before = vanilla / kSecondsPerHour * rate;
        if (refund > kMinRefund) {  // else t0 stays: the refund grows until it is worth adding
            void* addArgs[] = {td, &rate, &refund};
            Call(addEnergy_, model, addArgs, "DroneStationGadgetModel.AddEnergy", &ok);
            if (!ok || sr2::core::ModuleFaulted(module_)) return;
            look.time = now;
            look.refunded += refund;
            if (look.first) {
                look.first = false;
                const double after = std::max(0.0, unity::Field<double>(obs, offObsValue_) - now) / kSecondsPerHour * rate;
                SR2_INFO("[{}] battery saver: station {}: first refund +{:.2f} energy for {:.0f} world s (battery {:.1f} -> "
                         "{:.1f} of 1000)", module_, station, refund, dt, before, after);
            }
        }
        if (summary) {
            summaryTotal += look.refunded;
            ++summaryStations;
            const double cur = std::max(0.0, unity::Field<double>(obs, offObsValue_) - now) / kSecondsPerHour * rate;
            summaryList += std::format("{}{} +{:.1f} (battery {:.0f})", summaryList.empty() ? "" : ", ", station,
                                       look.refunded, cur);
            look.refunded = 0.0;
        }
    }
    if (summary && level > 0 && summaryStations > 0)
        SR2_INFO("[{}] battery saver: {} gave back {:.1f} energy to {} station(s) in the last {:.0f} s: {}", module_,
                 BatteryLevelName(level), summaryTotal, summaryStations, kSummarySeconds, summaryList);
}

}  // namespace sr2::modules::custom_upgrades::drones
