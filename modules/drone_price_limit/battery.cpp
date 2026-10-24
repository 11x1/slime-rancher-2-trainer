// drone_price_limit: Drone Battery Saver tiers (see battery.h). SR2 1.3.0, everything by name.

#include "modules/drone_price_limit/battery.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <format>
#include <string_view>

#include "core/detour_guard.h"
#include "core/il2cpp_api.h"
#include "core/log.h"
#include "core/main_thread.h"
#include "core/module.h"
#include "core/resolver.h"
#include "core/unity.h"
#include "modules/custom_upgrades/api.h"

namespace sr2::modules::drone_price_limit {
namespace {

namespace unity = sr2::core::unity;
namespace main_thread = sr2::core::main_thread;
using sr2::il2cpp::api;
using sr2::il2cpp::Resolver;

constexpr const char* kImage = "Assembly-CSharp";
constexpr const char* kKeyPrefix = "drone_battery.station.";
constexpr float kTickSeconds = 2.0f;                  // refund interval (real time)
constexpr unsigned long long kTierCacheFrames = 120;  // re-read a station's tier after ~2 s
constexpr double kSecondsPerHour = 3600.0;
constexpr float kMinRefund = 0.01f;                   // energy units (max 1000)

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

std::string BatteryTierName(int tier) {
    static const char* kRoman[] = {"", "I", "II", "III", "IV"};
    if (tier <= 0 || tier > kBatteryTiers) return "Battery Saver";
    return std::string("Battery Saver ") + kRoman[tier];
}

bool BatterySaver::Setup(Resolver& R, const char* module, std::string& why) {
    module_ = module;
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
    Il2CppClass* gameCtx = cls("", "GameContext");
    Il2CppClass* gameModel = cls("MonomiPark.SlimeRancher.DataModel", "GameModel");
    Il2CppClass* droneDir = cls("MonomiPark.SlimeRancher.Drone", "DroneDirector");
    Il2CppClass* timeDir = cls("", "TimeDirector");
    stationModelClass_ = cls("MonomiPark.SlimeRancher.DataModel", "DroneStationGadgetModel");
    Il2CppClass* identModel = cls("MonomiPark.SlimeRancher.DataModel", "IdentifiableModel");
    Il2CppClass* player = cls("", "PlayerState");
    Il2CppClass* scu = cls("MonomiPark.SlimeRancher.Util", "SceneContextUtility");
    Il2CppClass* lookup = cls("", "LookupDirector");
    Il2CppClass* curList = cls("MonomiPark.SlimeRancher.Economy", "CurrencyList");
    if (!why.empty()) return false;
    off(sceneCtx, "<GameModel>k__BackingField", offScGame_);
    off(sceneCtx, "<DroneDirector>k__BackingField", offScDrones_);
    off(sceneCtx, "<TimeDirector>k__BackingField", offScTime_);
    off(gameModel, "identifiablesByIdent", offByIdent_);
    off(droneDir, "_droneStationType", offStationType_);
    off(droneDir, "EnergyDepletedPerHour", offRate_);
    off(stationModelClass_, "_energyDepleteTime", offDeplete_);
    off(identModel, "actorId", offActorId_);
    off(gameCtx, "<LookupDirector>k__BackingField", offGcLookup_);
    off(lookup, "_currencyList", offLookupCurrencies_);
    off(curList, "_currencies", offCurrencyArr_);
    sceneCtxInst_ = method(parentOf(sceneCtx), "get_Instance", 0);
    gcInst_ = method(parentOf(gameCtx), "get_Instance", 0);
    worldTime_ = method(timeDir, "WorldTime", 0);
    addEnergy_ = method(stationModelClass_, "AddEnergy", 3);
    tryGetPlayer_ = method(scu, "TryGetPlayerState", 1);
    getCurrency_ = method(player, "GetCurrency", 1);
    spendCurrency_ = method(player, "SpendCurrency", 3);
    addCurrency_ = method(player, "AddCurrency", 4);
    if (!why.empty()) return false;
    {
        std::string err;  // optional: else LookupDirector's first currency
        if (Il2CppClass* cu = R.Class(kImage, "MonomiPark.SlimeRancher.Economy", "CurrencyUtility", &err))
            defaultCurrency_ = R.Method(cu, "get_DefaultCurrency", 0, &err);
    }
    ready_ = true;
    SR2_INFO("[{}] battery saver ready: tiers x{} / x{} / x{} / x{} battery life, {} / {} / {} / {} Newbucks per station",
             module_, kBatteryFactor[1], kBatteryFactor[2], kBatteryFactor[3], kBatteryFactor[4], kBatteryPrice[1],
             kBatteryPrice[2], kBatteryPrice[3], kBatteryPrice[4]);
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

bool BatterySaver::Available() const { return ready_ && custom_upgrades::Active(); }

int BatterySaver::Tier(int64_t station) {
    if (!Available() || station == 0) return 0;
    const unsigned long long tick = main_thread::TickCount();
    auto it = tiers_.find(station);
    if (it != tiers_.end() && tick - it->second.tick < kTierCacheFrames) return it->second.tier;
    int tier = 0;
    const std::string raw = custom_upgrades::SaveValue(kKeyPrefix + std::to_string(station));
    if (!raw.empty()) {
        try {
            tier = std::clamp(std::stoi(raw), 0, kBatteryTiers);
        } catch (...) {
            SR2_WARN("[{}] battery saver: station {}: stored tier '{}' not understood - none", module_, station, raw);
        }
    }
    if (tiers_.size() > 512) tiers_.clear();
    tiers_[station] = {tier, tick};
    return tier;
}

bool BatterySaver::SetTier(int64_t station, int tier) {
    const std::string value = tier > 0 ? std::to_string(tier) : std::string();
    if (!custom_upgrades::SetSaveValue(kKeyPrefix + std::to_string(station), value)) return false;
    tiers_[station] = {tier, main_thread::TickCount()};
    return true;
}

Il2CppObject* BatterySaver::SceneContext() {
    bool ok = false;
    Il2CppObject* sc = Call(sceneCtxInst_, nullptr, nullptr, "SceneContext.Instance", &ok);
    return ok && unity::IsAlive(sc) ? sc : nullptr;
}

Il2CppObject* BatterySaver::Currency() {
    bool ok = false;
    // The game's own default (PlayerState.SpendCurrency uses it for a null currency).
    if (defaultCurrency_) {
        Il2CppObject* c = Call(defaultCurrency_, nullptr, nullptr, "CurrencyUtility.DefaultCurrency", &ok);
        if (ok && unity::IsAlive(c)) return c;
    }
    Il2CppObject* gc = Call(gcInst_, nullptr, nullptr, "GameContext.Instance", &ok);
    if (!ok || !unity::IsAlive(gc)) return nullptr;
    Il2CppObject* lookup = unity::Field<Il2CppObject*>(gc, offGcLookup_);
    Il2CppObject* list = unity::IsAlive(lookup) ? unity::Field<Il2CppObject*>(lookup, offLookupCurrencies_) : nullptr;
    void* arr = unity::IsAlive(list) ? unity::Field<void*>(list, offCurrencyArr_) : nullptr;
    // [0] = the default currency, Newbucks (the plot menu prices and the coin icon use it).
    auto* first = arr && unity::ArrayLength(arr) > 0 ? static_cast<Il2CppObject*>(unity::ArrayAt(arr, 0)) : nullptr;
    return unity::IsAlive(first) ? first : nullptr;
}

Il2CppObject* BatterySaver::Player() {
    Il2CppObject* player = nullptr;
    void* args[] = {&player};
    bool ok = false;
    const bool found = Unbox<bool>(Call(tryGetPlayer_, nullptr, args, "SceneContextUtility.TryGetPlayerState", &ok));
    return ok && found && unity::IsAlive(player) ? player : nullptr;
}

int BatterySaver::Newbucks() {
    if (!ready_) return -1;
    Il2CppObject* player = Player();
    Il2CppObject* cur = Currency();
    if (!player || !cur) return -1;
    void* args[] = {cur};
    bool ok = false;
    const int n = Unbox<int32_t>(Call(getCurrency_, player, args, "PlayerState.GetCurrency", &ok));
    return ok ? n : -1;
}

std::string BatterySaver::BuyNext(int64_t station) {
    if (!Available() || station == 0) return "Needs Custom Upgrades on for this save";
    const int tier = Tier(station);
    if (tier >= kBatteryTiers) return "All tiers owned";
    const int next = tier + 1, price = kBatteryPrice[next];
    Il2CppObject* player = Player();
    Il2CppObject* cur = Currency();
    const int have = Newbucks();
    if (!player || !cur || have < 0) return "Can't read your Newbucks right now";
    if (have < price) return std::format("Not enough Newbucks ({} more needed)", price - have);
    // Ownership first (nothing is paid if it can't be stored), then the payment.
    if (!SetTier(station, next)) return "Can't store it for this save right now";
    int32_t amount = price;
    void* args[] = {cur, &amount, nullptr};
    bool ok = false;
    Call(spendCurrency_, player, args, "PlayerState.SpendCurrency", &ok);
    if (!ok) {
        SetTier(station, tier);
        return "The purchase failed";
    }
    looks_.erase(station);  // the new factor counts from the next look on
    SR2_INFO("[{}] battery saver: station {}: bought {} for {} Newbucks (drain x{:.2f}, saved with the next game save)",
             module_, station, BatteryTierName(next), price, 1.0f / kBatteryFactor[next]);
    return {};
}

std::string BatterySaver::SellTop(int64_t station) {
    if (!Available() || station == 0) return "Needs Custom Upgrades on for this save";
    const int tier = Tier(station);
    if (tier <= 0) return "Nothing to sell";
    Il2CppObject* player = Player();
    Il2CppObject* cur = Currency();
    if (!player || !cur) return "Can't reach your Newbucks right now";
    if (!SetTier(station, tier - 1)) return "Can't store it for this save right now";
    int32_t refund = kBatteryPrice[tier] / 2;
    bool notify = true;
    void* args[] = {cur, &refund, &notify, nullptr};
    bool ok = false;
    Call(addCurrency_, player, args, "PlayerState.AddCurrency", &ok);
    if (!ok) {
        SetTier(station, tier);
        return "The sale failed";
    }
    looks_.erase(station);
    SR2_INFO("[{}] battery saver: station {}: sold {} for {} Newbucks", module_, station, BatteryTierName(tier), refund);
    return {};
}

void BatterySaver::Reset() {
    looks_.clear();
    tiers_.clear();
    gameModel_ = nullptr;
}

void BatterySaver::Tick(float unscaledDt) {
    if (!ready_ || sr2::core::ModuleFaulted(module_)) return;
    timer_ += unscaledDt;
    if (timer_ < kTickSeconds) return;
    timer_ = 0.0f;
    if (!custom_upgrades::Active()) {
        if (!looks_.empty()) looks_.clear();
        return;
    }
    Il2CppObject* sc = SceneContext();
    if (!sc) return;
    Il2CppObject* gm = unity::Field<Il2CppObject*>(sc, offScGame_);
    Il2CppObject* dd = unity::Field<Il2CppObject*>(sc, offScDrones_);
    Il2CppObject* td = unity::Field<Il2CppObject*>(sc, offScTime_);
    if (!gm || !unity::IsAlive(dd) || !unity::IsAlive(td)) return;
    if (gm != gameModel_) {  // another game: the last looks belong to the old one
        looks_.clear();
        tiers_.clear();
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
    for (size_t i = 0, n = unity::ListCount(list); i < n && i < 256; ++i) {
        auto* model = static_cast<Il2CppObject*>(unity::ListAt(list, i));
        if (!IsA(model, stationModelClass_)) continue;
        const int64_t station = unity::Field<int64_t>(model, offActorId_);
        if (station == 0) continue;
        auto [it, fresh] = looks_.try_emplace(station);
        Look& look = it->second;
        const double t0 = look.time;
        look.time = now;
        if (fresh || now < t0) continue;  // first look (or time went back): nothing to compare yet
        const int tier = Tier(station);
        if (tier != look.tier) {
            if (tier > 0)
                SR2_INFO("[{}] battery saver: station {}: {} active (battery drain x{:.2f})", module_, station,
                         BatteryTierName(tier), 1.0f / kBatteryFactor[tier]);
            look.tier = tier;
        }
        const double dt = now - t0;
        if (tier <= 0 || dt <= 0.0) continue;
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
        if (!std::isfinite(deplete) || left0 <= 0.0) continue;  // was already empty
        const double left1 = std::max(0.0, left0 - dt / kBatteryFactor[tier]);
        const double vanilla = std::max(0.0, deplete - now);
        float refund = static_cast<float>((left1 - vanilla) / kSecondsPerHour * rate);
        if (!(refund > kMinRefund)) continue;
        void* addArgs[] = {td, &rate, &refund};
        Call(addEnergy_, model, addArgs, "DroneStationGadgetModel.AddEnergy", &ok);
        if (!ok || sr2::core::ModuleFaulted(module_)) return;
    }
}

}  // namespace sr2::modules::drone_price_limit
