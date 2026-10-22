// plot_upgrades: stronger ranch land-plot upgrades (SR2 1.3.0).
//
// Game side (Cpp2IL dump + disassembly of GameAssembly.dll 1.3.0):
//   * The LandPlot upgrade components (PlortCollectorUpgrader, FeederUpgrader,
//     MusicBoxUpgrader, SprinklerUpgrader, WallUpgrader, AirNetUpgrader, SolarShieldUpgrader,
//     ...) only switch GameObjects on/off. The numbers live in the activated components:
//
//   Corral
//     PlortCollector.DoCollection(): model.collectorNextTime += CollectPeriod * 3600
//       (CollectPeriod = hours between collections, a prefab field). Called from
//       FixedUpdate when collectorNextTime is reached, and from StartCollection.
//       Pond / incinerator collectors use the same component.
//     SlimeFeeder.FixedUpdate() / UpdateToTime(worldTime): every feeding cycle
//       (HoursByFeedSpeed[model.feederCycleSpeed] hours) adds ItemsPerFeeding to
//       model.remainingFeedOperations; each operation ejects one food item from the
//       feeder's storage (0.5 s apart). UpdateToTime is the fast-forward (sleep) path.
//     SlimeEmotions.ComputeAgitationChangePerHour(): factor = _agitationRecoveryFactor
//       (+ daylight/darkness extra) + (_musicBoxCount > 0 ? MusicBoxRegion.EXTRA_CALMING_FACTOR
//       (0.5, inlined const) : 0) + _toyAgitationFactor;  m = factor >= 0 ? 1 + factor
//       : 1 / (1 - factor); rate = recovery rate (minus starving / pollen adjustments);
//       change = (rate < 0 ? rate / m : rate * m) * _agitationDirection -> _changeRate.y.
//       Recomputed on many events and from UpdateToTime.
//   Coop
//     Reproduce.ReproducePeriod() = Randoms.GetInRange(MinReproduceGameHours,
//       MaxReproduceGameHours); used by InitModel / RegistryUpdate to set
//       model.nextReproduceTime. Hens outside a coop lay with 50 % probability.
//   Garden
//     SpawnResource.UpdateToTime(worldTime, deltaWorldTime, timeOfDay): within the
//       spawning hours and without spawn blockers, model.nextSpawnTime -= (IsWatered ?
//       def._wateredTimeFactor : 0) * deltaWorldTime (that is how the sprinkler speeds up
//       growth), then crops spawn once worldTime reaches nextSpawnTime.
//   Incinerator
//     Incinerate.ProcessIncinerateResults(id, amount[, pos, rot]): for food,
//       AshTrough.AddAsh(amount * AshPerIncineration); FillableAshSource.AddAsh clamps
//       plotModel.ashUnits to MaxUnits (vanilla cap).
//
// What this module does (all runtime-only, nothing new is written into the save):
//   * PlortCollector.DoCollection detour: CollectPeriod / collector_speed only while the
//     original runs (restored right after, RAII). Saved value = the next collection
//     timestamp, which vanilla reads as a plain time.
//   * SlimeFeeder.FixedUpdate / UpdateToTime detours: ItemsPerFeeding x feeder_amount
//     while the original runs. Saved value = remainingFeedOperations (an int counter that
//     vanilla simply works off).
//   * SlimeEmotions.ComputeAgitationChangePerHour detour: for slimes inside a music box
//     corral (_musicBoxCount > 0) adds 0.5 * (musicbox_calm - 1) to _toyAgitationFactor
//     while the original runs -> the music box bonus becomes 0.5 * musicbox_calm.
//     Emotions themselves stay in [0,1] (vanilla clamps).
//   * Reproduce.ReproducePeriod detour: result / coop_speed when Reproduce.WithinCoop().
//   * SpawnResource.UpdateToTime detour (garden plots only: _landPlot.TypeId == GARDEN):
//     before the original, within spawning hours and without blockers, advances
//     model.nextSpawnTime by (garden_growth - 1) * (1 + watered factor) * deltaWorldTime -
//     the same mechanism the sprinkler uses. Wild trees/nodes (no LandPlot) untouched.
//   * Incinerate.ProcessIncinerateResults detours (both overloads): AshPerIncineration x
//     ash_amount while the original runs; the vanilla MaxUnits clamp still applies.
//
// Skipped on purpose (see report / NOTES): silo & feeder storage capacity (stored counts
// are saved; vanilla's AmmoSlotManager.ValidateAndAdjustSlots does not clamp counts on
// load, so a removed mod would leave over-full slots), plort collector vac radius (the
// collection trigger already spans the plot; enlarging it only reaches outside the plot),
// garden yield, walls / air net / solar shield / scareslime / vitamizer / soil (no
// numeric tunables or inlined constants).
//
// Fault policy: every detour runs its logic in a DetourGuard (native fault -> module
// faulted, detours pass-through). Objects are class- and IsAlive-checked. No scans, no
// GC handles, no per-frame allocations; per call a few field reads/writes.

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <format>
#include <memory>
#include <string>

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

constexpr const char* kModule = "plot_upgrades";
constexpr const char* kImage = "Assembly-CSharp";
constexpr const char* kDataNs = "MonomiPark.SlimeRancher.DataModel";
constexpr const char* kSlimeNs = "MonomiPark.SlimeRancher.Slime";
constexpr int kPlotGarden = 4;  // LandPlot.Id.GARDEN

// settings keys
constexpr const char* kCollector = "collector_speed";
constexpr const char* kFeeder = "feeder_amount";
constexpr const char* kMusic = "musicbox_calm";
constexpr const char* kCoop = "coop_speed";
constexpr const char* kGarden = "garden_growth";
constexpr const char* kAsh = "ash_amount";

constexpr float kMusicBoxCalming = 0.5f;  // MusicBoxRegion.EXTRA_CALMING_FACTOR (inlined const)

struct Vec3 { float x, y, z; };
struct Quat { float x, y, z, w; };

bool IsA(Il2CppObject* obj, Il2CppClass* k) {
    const auto& a = api();
    if (!obj || !k || !a.il2cpp_object_get_class) return false;
    Il2CppClass* c = a.il2cpp_object_get_class(obj);
    if (c == k) return true;
    return c && a.il2cpp_class_is_assignable_from && a.il2cpp_class_is_assignable_from(k, c);
}

// Restores a field to its saved value when it goes out of scope (also on a managed
// exception unwinding through the detour).
template <class T>
struct FieldRestore {
    T* where = nullptr;
    T saved{};
    ~FieldRestore() {
        if (where) *where = saved;
    }
};

enum Feature { kFCollector, kFFeeder, kFMusic, kFCoop, kFGarden, kFAsh, kFeatureCount };
const char* const kFeatureNames[kFeatureCount] = {"plort collector", "auto-feeder", "music box", "coop",
                                                  "garden", "incinerator ash"};

class PlotUpgrades final : public sr2::core::Module {
public:
    const char* Name() const override { return kModule; }
    bool DefaultEnabled() const override { return true; }
    bool Setup(sr2::core::ModuleContext& ctx) override;
    void OnEnable() override {
        ResetLogOnce();
        enabled_.store(true);
    }
    void OnDisable() override { enabled_.store(false); }

    bool On() const { return enabled_.load(std::memory_order_relaxed); }
    float Mult(Feature f) const { return mult_[f].load(std::memory_order_relaxed); }
    // true once per feature per settings change (for the one-shot apply log)
    bool LogOnce(Feature f) { return !logged_[f].exchange(true, std::memory_order_relaxed); }

    // ---- detour bodies (main thread, inside DetourGuard) ----
    void CollectorBefore(Il2CppObject* self, FieldRestore<float>& r);
    void FeederBefore(Il2CppObject* self, FieldRestore<int32_t>& r, const char* path);
    void MusicBefore(Il2CppObject* self, FieldRestore<float>& r);
    float CoopAfter(Il2CppObject* self, float period);
    void GardenBefore(Il2CppObject* self, double deltaWorldTime, float timeOfDay);
    void AshBefore(Il2CppObject* self, FieldRestore<float>& r);

private:
    void RegisterSettings();
    void LoadSettings();
    void ResetLogOnce() {
        for (auto& l : logged_) l.store(false);
    }

    std::atomic<bool> enabled_{false};
    std::atomic<float> mult_[kFeatureCount]{};
    std::atomic<bool> logged_[kFeatureCount]{};
    int onChange_ = 0;

public:
    // classes
    Il2CppClass* collectorClass_ = nullptr;
    Il2CppClass* feederClass_ = nullptr;
    Il2CppClass* emotionsClass_ = nullptr;
    Il2CppClass* reproduceClass_ = nullptr;
    Il2CppClass* spawnResClass_ = nullptr;
    Il2CppClass* spawnModelClass_ = nullptr;
    Il2CppClass* landPlotClass_ = nullptr;
    Il2CppClass* incinerateClass_ = nullptr;
    // offsets
    size_t offCollectPeriod_ = 0;     // PlortCollector.CollectPeriod (float, hours)
    size_t offItemsPerFeeding_ = 0;   // SlimeFeeder.ItemsPerFeeding (int)
    size_t offToyFactor_ = 0;         // SlimeEmotions._toyAgitationFactor (float)
    size_t offMusicBoxCount_ = 0;     // SlimeEmotions._musicBoxCount (int)
    size_t offSrDef_ = 0;             // SpawnResource._resourceGrowerDefinition
    size_t offSrLandPlot_ = 0;        // SpawnResource._landPlot
    size_t offSrBlockers_ = 0;        // SpawnResource._spawnBlockers (int)
    size_t offSrModel_ = 0;           // SpawnResource._model (SpawnResourceModel)
    size_t offNextSpawn_ = 0;         // SpawnResourceModel.nextSpawnTime (double, world s)
    size_t offPlotType_ = 0;          // LandPlot.TypeId (int enum)
    size_t offDefWaterFactor_ = 0;    // ResourceGrowerDefinition._wateredTimeFactor (float)
    size_t offAshPer_ = 0;            // Incinerate.AshPerIncineration (float)
    // private game methods called from detours
    using BoolFn = bool (*)(Il2CppObject*, const MethodInfo*);
    using BoolFloatFn = bool (*)(Il2CppObject*, float, const MethodInfo*);
    BoolFn withinCoop_ = nullptr;
    const MethodInfo* withinCoopMi_ = nullptr;
    BoolFn isWatered_ = nullptr;
    const MethodInfo* isWateredMi_ = nullptr;
    BoolFloatFn withinHours_ = nullptr;
    const MethodInfo* withinHoursMi_ = nullptr;
};

PlotUpgrades* g_self = nullptr;

sr2::core::DetourGuard g_collectGuard{kModule, "PlortCollector.DoCollection"};
sr2::core::DetourGuard g_feedFixedGuard{kModule, "SlimeFeeder.FixedUpdate"};
sr2::core::DetourGuard g_feedFfGuard{kModule, "SlimeFeeder.UpdateToTime"};
sr2::core::DetourGuard g_musicGuard{kModule, "SlimeEmotions.ComputeAgitationChangePerHour"};
sr2::core::DetourGuard g_coopGuard{kModule, "Reproduce.ReproducePeriod"};
sr2::core::DetourGuard g_gardenGuard{kModule, "SpawnResource.UpdateToTime"};
sr2::core::DetourGuard g_ash2Guard{kModule, "Incinerate.ProcessIncinerateResults(2)"};
sr2::core::DetourGuard g_ash4Guard{kModule, "Incinerate.ProcessIncinerateResults(4)"};

using VoidFn = void (*)(Il2CppObject*, const MethodInfo*);
using FeedFfFn = void (*)(Il2CppObject*, double, const MethodInfo*);
using FloatFn = float (*)(Il2CppObject*, const MethodInfo*);
using SrUpdateFn = void (*)(Il2CppObject*, double, double, float, const MethodInfo*);
using Ash2Fn = bool (*)(Il2CppObject*, Il2CppObject*, int32_t, const MethodInfo*);
using Ash4Fn = void (*)(Il2CppObject*, Il2CppObject*, int32_t, Vec3, Quat, const MethodInfo*);

VoidFn g_origCollect = nullptr;
VoidFn g_origFeedFixed = nullptr;
FeedFfFn g_origFeedFf = nullptr;
VoidFn g_origMusic = nullptr;
FloatFn g_origCoop = nullptr;
SrUpdateFn g_origGarden = nullptr;
Ash2Fn g_origAsh2 = nullptr;
Ash4Fn g_origAsh4 = nullptr;

bool Active(Feature f) { return g_self && g_self->On() && g_self->Mult(f) != 1.0f; }

void CollectDetour(Il2CppObject* self, const MethodInfo* mi) {
    FieldRestore<float> r;
    if (self && Active(kFCollector)) g_collectGuard.Run([&] { g_self->CollectorBefore(self, r); });
    g_origCollect(self, mi);
}

void FeedFixedDetour(Il2CppObject* self, const MethodInfo* mi) {
    FieldRestore<int32_t> r;
    if (self && Active(kFFeeder)) g_feedFixedGuard.Run([&] { g_self->FeederBefore(self, r, "FixedUpdate"); });
    g_origFeedFixed(self, mi);
}

void FeedFfDetour(Il2CppObject* self, double worldTime, const MethodInfo* mi) {
    FieldRestore<int32_t> r;
    if (self && Active(kFFeeder)) g_feedFfGuard.Run([&] { g_self->FeederBefore(self, r, "UpdateToTime"); });
    g_origFeedFf(self, worldTime, mi);
}

void MusicDetour(Il2CppObject* self, const MethodInfo* mi) {
    FieldRestore<float> r;
    if (self && Active(kFMusic)) g_musicGuard.Run([&] { g_self->MusicBefore(self, r); });
    g_origMusic(self, mi);
}

float CoopDetour(Il2CppObject* self, const MethodInfo* mi) {
    float period = g_origCoop(self, mi);
    if (self && Active(kFCoop)) {
        float out = period;
        if (g_coopGuard.Run([&] { out = g_self->CoopAfter(self, period); })) return out;
    }
    return period;
}

void GardenDetour(Il2CppObject* self, double worldTime, double delta, float timeOfDay, const MethodInfo* mi) {
    if (self && Active(kFGarden)) g_gardenGuard.Run([&] { g_self->GardenBefore(self, delta, timeOfDay); });
    g_origGarden(self, worldTime, delta, timeOfDay, mi);
}

bool Ash2Detour(Il2CppObject* self, Il2CppObject* id, int32_t amount, const MethodInfo* mi) {
    FieldRestore<float> r;
    if (self && Active(kFAsh)) g_ash2Guard.Run([&] { g_self->AshBefore(self, r); });
    return g_origAsh2(self, id, amount, mi);
}

void Ash4Detour(Il2CppObject* self, Il2CppObject* id, int32_t amount, Vec3 pos, Quat rot, const MethodInfo* mi) {
    FieldRestore<float> r;
    if (self && Active(kFAsh)) g_ash4Guard.Run([&] { g_self->AshBefore(self, r); });
    g_origAsh4(self, id, amount, pos, rot, mi);
}

std::string NameOf(Il2CppObject* obj, const char* fallback) {
    std::string n = unity::GetName(obj);
    return n.empty() ? std::string(fallback) : n;
}

// ------------------------------------------------------------------ bodies --

void PlotUpgrades::CollectorBefore(Il2CppObject* self, FieldRestore<float>& r) {
    if (!main_thread::IsMainThread() || !IsA(self, collectorClass_) || !unity::IsAlive(self)) return;
    float& period = unity::Field<float>(self, offCollectPeriod_);
    const float orig = period;
    if (!std::isfinite(orig) || !(orig > 0.0f)) return;
    const float m = Mult(kFCollector);
    const float scaled = std::max(orig / m, 0.01f);  // >= 36 world seconds
    r.saved = orig;
    r.where = &period;
    period = scaled;
    if (LogOnce(kFCollector))
        SR2_INFO("[{}] plort collector '{}': CollectPeriod {:.3f} h -> {:.3f} h (x{:.2f} faster) for this collection",
                 kModule, NameOf(self, "PlortCollector"), orig, scaled, m);
}

void PlotUpgrades::FeederBefore(Il2CppObject* self, FieldRestore<int32_t>& r, const char* path) {
    if (!main_thread::IsMainThread() || !IsA(self, feederClass_) || !unity::IsAlive(self)) return;
    int32_t& items = unity::Field<int32_t>(self, offItemsPerFeeding_);
    const int32_t orig = items;
    if (orig <= 0 || orig > 1000) return;
    const float m = Mult(kFFeeder);
    const int32_t scaled = std::clamp(static_cast<int32_t>(std::lround(orig * m)), 1, 100);
    if (scaled == orig) return;
    r.saved = orig;
    r.where = &items;
    items = scaled;
    if (LogOnce(kFFeeder))
        SR2_INFO("[{}] auto-feeder '{}' ({}): ItemsPerFeeding {} -> {} per feeding cycle", kModule,
                 NameOf(self, "SlimeFeeder"), path, orig, scaled);
}

void PlotUpgrades::MusicBefore(Il2CppObject* self, FieldRestore<float>& r) {
    if (!main_thread::IsMainThread()) return;
    // Cheap pre-filter first: only slimes inside a music box corral.
    if (unity::Field<int32_t>(self, offMusicBoxCount_) <= 0) return;
    if (!IsA(self, emotionsClass_) || !unity::IsAlive(self)) return;
    float& toy = unity::Field<float>(self, offToyFactor_);
    const float orig = toy;
    if (!std::isfinite(orig)) return;
    const float m = Mult(kFMusic);
    const float extra = kMusicBoxCalming * (m - 1.0f);
    r.saved = orig;
    r.where = &toy;
    toy = orig + extra;
    if (LogOnce(kFMusic))
        SR2_INFO("[{}] music box: slime '{}' calming bonus {:.2f} -> {:.2f} (_toyAgitationFactor {:.2f} -> {:.2f} "
                 "during ComputeAgitationChangePerHour)",
                 kModule, NameOf(self, "slime"), kMusicBoxCalming, kMusicBoxCalming * m, orig, orig + extra);
}

float PlotUpgrades::CoopAfter(Il2CppObject* self, float period) {
    if (!main_thread::IsMainThread() || !withinCoop_ || !IsA(self, reproduceClass_) || !unity::IsAlive(self))
        return period;
    if (!std::isfinite(period) || !(period > 0.0f)) return period;
    if (!withinCoop_(self, withinCoopMi_)) return period;
    const float m = Mult(kFCoop);
    const float out = std::max(period / m, 0.05f);
    if (LogOnce(kFCoop))
        SR2_INFO("[{}] coop: '{}' Reproduce.ReproducePeriod {:.2f} h -> {:.2f} h (x{:.2f} faster, inside a coop)",
                 kModule, NameOf(self, "animal"), period, out, m);
    return out;
}

void PlotUpgrades::GardenBefore(Il2CppObject* self, double delta, float timeOfDay) {
    if (!main_thread::IsMainThread()) return;
    // Cheap pre-filter: only spawners that belong to a land plot.
    auto* plot = unity::Field<Il2CppObject*>(self, offSrLandPlot_);
    if (!plot) return;
    if (!(delta > 0.0) || delta > 3600.0 || !std::isfinite(delta)) return;  // normal frames only
    if (!IsA(self, spawnResClass_) || !IsA(plot, landPlotClass_)) return;
    if (unity::Field<int32_t>(plot, offPlotType_) != kPlotGarden) return;
    if (!unity::IsAlive(self) || !unity::IsAlive(plot)) return;
    if (unity::Field<int32_t>(self, offSrBlockers_) > 0) return;
    auto* def = unity::Field<Il2CppObject*>(self, offSrDef_);
    auto* model = unity::Field<Il2CppObject*>(self, offSrModel_);
    if (!def || !IsA(model, spawnModelClass_)) return;
    if (!withinHours_(self, timeOfDay, withinHoursMi_)) return;
    float rate = 1.0f;  // vanilla: 1 + watered factor
    if (isWatered_(self, isWateredMi_)) {
        const float wf = unity::Field<float>(def, offDefWaterFactor_);
        if (std::isfinite(wf) && wf > 0.0f && wf < 100.0f) rate += wf;
    }
    const float m = Mult(kFGarden);
    double& next = unity::Field<double>(model, offNextSpawn_);
    if (!std::isfinite(next)) return;
    const double advance = static_cast<double>(m - 1.0f) * rate * delta;
    if (!(advance > 0.0)) return;
    next -= advance;
    if (LogOnce(kFGarden))
        SR2_INFO("[{}] garden '{}': growth rate {:.2f} -> {:.2f} (x{:.2f}; nextSpawnTime advanced {:.2f} s this "
                 "frame)",
                 kModule, NameOf(self, "SpawnResource"), rate, rate * m, m, advance);
}

void PlotUpgrades::AshBefore(Il2CppObject* self, FieldRestore<float>& r) {
    if (!main_thread::IsMainThread() || !IsA(self, incinerateClass_) || !unity::IsAlive(self)) return;
    float& per = unity::Field<float>(self, offAshPer_);
    const float orig = per;
    if (!std::isfinite(orig) || !(orig > 0.0f) || orig > 1000.0f) return;
    const float m = Mult(kFAsh);
    r.saved = orig;
    r.where = &per;
    per = orig * m;
    if (LogOnce(kFAsh))
        SR2_INFO("[{}] incinerator '{}': AshPerIncineration {:.2f} -> {:.2f} (trough still capped at MaxUnits)",
                 kModule, NameOf(self, "Incinerate"), orig, orig * m);
}

// ------------------------------------------------------------------ setup --

void PlotUpgrades::RegisterSettings() {
    using settings::Kind;
    auto reg = [](const char* key, const char* label, const char* group, double def, double mn, double mx,
                  double step, const char* tip) {
        settings::Def d;
        d.module = kModule;
        d.key = key;
        d.label = label;
        d.group = group;
        d.kind = Kind::Float;
        d.def = def;
        d.min = mn;
        d.max = mx;
        d.step = step;
        d.tooltip = tip;
        settings::Register(d);
    };
    reg(kCollector, "Plort Collector speed", "Corral", 2.0, 1.0, 10.0, 0.25,
        "Divides the hours between Plort Collector collections (corral, pond and incinerator collectors). "
        "1.0 = vanilla. Applies from the next collection.");
    reg(kFeeder, "Auto-Feeder food per cycle", "Corral", 2.0, 1.0, 5.0, 0.5,
        "Multiplies how many food items the Auto-Feeder hands out per feeding cycle (rounded). Uses the "
        "feeder's own stored food; storage capacity is unchanged. 1.0 = vanilla. Live.");
    reg(kMusic, "Music Box calming strength", "Corral", 3.0, 1.0, 10.0, 0.5,
        "Multiplies the Music Box agitation-recovery bonus (vanilla +0.5). 1.0 = vanilla. Applies the next time "
        "a slime's agitation rate is recomputed (within moments).");
    reg(kCoop, "Coop: hen laying speed", "Coop", 2.0, 1.0, 5.0, 0.25,
        "Divides the hours between eggs/chicks for hens inside a coop. 1.0 = vanilla. Applies from each hen's "
        "next laying.");
    reg(kGarden, "Garden: crop growth speed", "Garden", 2.0, 1.0, 10.0, 0.25,
        "Crops on garden plots grow this many times faster (stacks with the Sprinkler). Wild trees and nodes "
        "are not affected. 1.0 = vanilla. Live.");
    reg(kAsh, "Incinerator: ash per food", "Incinerator", 2.0, 1.0, 5.0, 0.5,
        "Multiplies the ash added to the Ash Trough per incinerated food. The trough's vanilla capacity "
        "still applies. 1.0 = vanilla. Live.");
}

void PlotUpgrades::LoadSettings() {
    const char* keys[kFeatureCount] = {kCollector, kFeeder, kMusic, kCoop, kGarden, kAsh};
    const float maxv[kFeatureCount] = {10.0f, 5.0f, 10.0f, 5.0f, 10.0f, 5.0f};
    for (int i = 0; i < kFeatureCount; ++i) {
        float v = settings::GetFloat(kModule, keys[i]);
        if (!std::isfinite(v)) v = 1.0f;
        v = std::clamp(v, 1.0f, maxv[i]);
        if (mult_[i].exchange(v) != v) logged_[i].store(false);
    }
}

bool PlotUpgrades::Setup(sr2::core::ModuleContext& ctx) {
    Resolver& R = ctx.resolver();
    auto cls = [&](const char* ns, const char* name, std::string& why) -> Il2CppClass* {
        std::string err;
        Il2CppClass* k = R.Class(kImage, ns, name, &err);
        if (!k && why.empty()) why = err.empty() ? std::format("class {} not found", name) : err;
        return k;
    };
    auto off = [&](Il2CppClass* k, const char* field, size_t& out, std::string& why) -> bool {
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
    auto method = [&](Il2CppClass* k, const char* name, int argc, const MethodInfo*& mi, std::string& why) -> void* {
        if (!k) return nullptr;
        std::string err;
        mi = R.Method(k, name, argc, &err);
        void* p = mi ? R.MethodPointer(mi, &err) : nullptr;
        if (!p && why.empty()) why = std::format("{}({}): {}", name, argc, err.empty() ? "no native body" : err);
        return p;
    };
    auto hook = [&](const char* hookName, void* target, void* detour, void** orig, std::string& why) -> bool {
        if (!target) return false;
        if (!ctx.HookAddress(hookName, target, detour, orig)) {
            if (why.empty()) why = std::format("hook {} failed", hookName);
            return false;
        }
        return true;
    };

    int okCount = 0;
    std::string report;
    auto done = [&](Feature f, bool ok, const std::string& detail) {
        if (ok) {
            ++okCount;
            report += std::format("\n    {}: OK - {}", kFeatureNames[f], detail);
        } else {
            report += std::format("\n    {}: NOT AVAILABLE - {}", kFeatureNames[f], detail);
        }
    };

    // --- plort collector ---
    {
        std::string why;
        const MethodInfo* mi = nullptr;
        collectorClass_ = cls("", "PlortCollector", why);
        bool ok = off(collectorClass_, "CollectPeriod", offCollectPeriod_, why);
        void* p = ok ? method(collectorClass_, "DoCollection", 0, mi, why) : nullptr;
        ok = ok && hook("PlortCollector.DoCollection", p, reinterpret_cast<void*>(&CollectDetour),
                        reinterpret_cast<void**>(&g_origCollect), why);
        done(kFCollector, ok,
             ok ? std::format("DoCollection RVA 0x{:X}, CollectPeriod +0x{:X}", Resolver::Rva(p), offCollectPeriod_)
                : why);
    }
    // --- auto-feeder ---
    {
        std::string why;
        const MethodInfo* mi1 = nullptr;
        const MethodInfo* mi2 = nullptr;
        feederClass_ = cls("", "SlimeFeeder", why);
        bool ok = off(feederClass_, "ItemsPerFeeding", offItemsPerFeeding_, why);
        void* p1 = ok ? method(feederClass_, "FixedUpdate", 0, mi1, why) : nullptr;
        void* p2 = ok ? method(feederClass_, "UpdateToTime", 1, mi2, why) : nullptr;
        ok = ok && p1 && p2 &&
             hook("SlimeFeeder.FixedUpdate", p1, reinterpret_cast<void*>(&FeedFixedDetour),
                  reinterpret_cast<void**>(&g_origFeedFixed), why) &&
             hook("SlimeFeeder.UpdateToTime", p2, reinterpret_cast<void*>(&FeedFfDetour),
                  reinterpret_cast<void**>(&g_origFeedFf), why);
        done(kFFeeder, ok,
             ok ? std::format("FixedUpdate RVA 0x{:X}, UpdateToTime RVA 0x{:X}, ItemsPerFeeding +0x{:X}",
                              Resolver::Rva(p1), Resolver::Rva(p2), offItemsPerFeeding_)
                : why);
    }
    // --- music box ---
    {
        std::string why;
        const MethodInfo* mi = nullptr;
        emotionsClass_ = cls(kSlimeNs, "SlimeEmotions", why);
        bool ok = off(emotionsClass_, "_toyAgitationFactor", offToyFactor_, why) &&
                  off(emotionsClass_, "_musicBoxCount", offMusicBoxCount_, why);
        void* p = ok ? method(emotionsClass_, "ComputeAgitationChangePerHour", 0, mi, why) : nullptr;
        ok = ok && hook("SlimeEmotions.ComputeAgitationChangePerHour", p, reinterpret_cast<void*>(&MusicDetour),
                        reinterpret_cast<void**>(&g_origMusic), why);
        done(kFMusic, ok,
             ok ? std::format("ComputeAgitationChangePerHour RVA 0x{:X}, _toyAgitationFactor +0x{:X}, "
                              "_musicBoxCount +0x{:X}",
                              Resolver::Rva(p), offToyFactor_, offMusicBoxCount_)
                : why);
    }
    // --- coop ---
    {
        std::string why;
        const MethodInfo* mi = nullptr;
        reproduceClass_ = cls("", "Reproduce", why);
        void* within = method(reproduceClass_, "WithinCoop", 0, withinCoopMi_, why);
        withinCoop_ = reinterpret_cast<BoolFn>(within);
        void* p = within ? method(reproduceClass_, "ReproducePeriod", 0, mi, why) : nullptr;
        bool ok = hook("Reproduce.ReproducePeriod", p, reinterpret_cast<void*>(&CoopDetour),
                       reinterpret_cast<void**>(&g_origCoop), why);
        done(kFCoop, ok,
             ok ? std::format("ReproducePeriod RVA 0x{:X}, WithinCoop RVA 0x{:X}", Resolver::Rva(p),
                              Resolver::Rva(within))
                : why);
    }
    // --- garden ---
    {
        std::string why;
        const MethodInfo* mi = nullptr;
        spawnResClass_ = cls("", "SpawnResource", why);
        spawnModelClass_ = cls(kDataNs, "SpawnResourceModel", why);
        landPlotClass_ = cls("", "LandPlot", why);
        Il2CppClass* defClass = cls("", "ResourceGrowerDefinition", why);
        bool ok = off(spawnResClass_, "_resourceGrowerDefinition", offSrDef_, why) &&
                  off(spawnResClass_, "_landPlot", offSrLandPlot_, why) &&
                  off(spawnResClass_, "_spawnBlockers", offSrBlockers_, why) &&
                  off(spawnResClass_, "_model", offSrModel_, why) &&
                  off(spawnModelClass_, "nextSpawnTime", offNextSpawn_, why) &&
                  off(landPlotClass_, "TypeId", offPlotType_, why) &&
                  off(defClass, "_wateredTimeFactor", offDefWaterFactor_, why);
        void* watered = ok ? method(spawnResClass_, "IsWatered", 0, isWateredMi_, why) : nullptr;
        void* hours = watered ? method(spawnResClass_, "WithinSpawningHours", 1, withinHoursMi_, why) : nullptr;
        isWatered_ = reinterpret_cast<BoolFn>(watered);
        withinHours_ = reinterpret_cast<BoolFloatFn>(hours);
        void* p = hours ? method(spawnResClass_, "UpdateToTime", 3, mi, why) : nullptr;
        ok = ok && hook("SpawnResource.UpdateToTime", p, reinterpret_cast<void*>(&GardenDetour),
                        reinterpret_cast<void**>(&g_origGarden), why);
        done(kFGarden, ok,
             ok ? std::format("UpdateToTime RVA 0x{:X}; offsets _landPlot +0x{:X}, _model +0x{:X}, "
                              "nextSpawnTime +0x{:X}, LandPlot.TypeId +0x{:X}, _wateredTimeFactor +0x{:X}",
                              Resolver::Rva(p), offSrLandPlot_, offSrModel_, offNextSpawn_, offPlotType_,
                              offDefWaterFactor_)
                : why);
    }
    // --- incinerator ---
    {
        std::string why;
        const MethodInfo* mi2 = nullptr;
        const MethodInfo* mi4 = nullptr;
        incinerateClass_ = cls("", "Incinerate", why);
        bool ok = off(incinerateClass_, "AshPerIncineration", offAshPer_, why);
        void* p2 = ok ? method(incinerateClass_, "ProcessIncinerateResults", 2, mi2, why) : nullptr;
        void* p4 = ok ? method(incinerateClass_, "ProcessIncinerateResults", 4, mi4, why) : nullptr;
        ok = ok && p2 && p4 &&
             hook("Incinerate.ProcessIncinerateResults2", p2, reinterpret_cast<void*>(&Ash2Detour),
                  reinterpret_cast<void**>(&g_origAsh2), why) &&
             hook("Incinerate.ProcessIncinerateResults4", p4, reinterpret_cast<void*>(&Ash4Detour),
                  reinterpret_cast<void**>(&g_origAsh4), why);
        done(kFAsh, ok,
             ok ? std::format("ProcessIncinerateResults RVA 0x{:X} / 0x{:X}, AshPerIncineration +0x{:X}",
                              Resolver::Rva(p2), Resolver::Rva(p4), offAshPer_)
                : why);
    }

    if (okCount == 0) return ctx.Fail("no plot upgrade could be resolved:" + report);

    RegisterSettings();
    LoadSettings();
    if (!onChange_) onChange_ = settings::OnChange(kModule, [this](const std::string&) { LoadSettings(); });

    g_self = this;
    SR2_INFO("[{}] setup ok ({}/{} features): multipliers collector x{:.2f}, feeder x{:.2f}, music box x{:.2f}, "
             "coop x{:.2f}, garden x{:.2f}, ash x{:.2f}{}",
             kModule, okCount, static_cast<int>(kFeatureCount), Mult(kFCollector), Mult(kFFeeder), Mult(kFMusic),
             Mult(kFCoop), Mult(kFGarden), Mult(kFAsh), report);
    return true;
}

}  // namespace

std::unique_ptr<sr2::core::Module> CreatePlotUpgrades() { return std::make_unique<PlotUpgrades>(); }

}  // namespace sr2::modules
