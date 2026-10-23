// plot_upgrades: the effects of the purchasable per-plot upgrade tiers (SR2 1.3.0).
//
// The tiers themselves (Music Box II, Faster Hens I/II, Fast Growth I/II, Ash Booster;
// Plort Collector II/III and Auto-Feeder II were retired) are custom upgrades bought in
// each plot's own purchase menu (modules/custom_upgrades: prices, prerequisites, sidecar persistence,
// selling through sell_upgrades). This module only applies the multiplier of the tier a
// plot owns, through custom_upgrades::Multiplier(plot or component, family) - a cached
// O(1) lookup per object. A plot without a tier runs pure vanilla code (multiplier 1).
// The former global multiplier sliders are gone (see modules/plot_upgrades/NOTES.md).
//
// Game side (Cpp2IL dump + disassembly of GameAssembly.dll 1.3.0):
//   * The LandPlot upgrade components (MusicBoxUpgrader, AshTroughUpgrader, ...) only
//     switch GameObjects on/off. The numbers live in the activated components:
//
//   Corral
//     SlimeEmotions.ComputeAgitationChangePerHour(): factor = _agitationRecoveryFactor
//       (+ daylight/darkness extra) + (_musicBoxCount > 0 ? MusicBoxRegion.EXTRA_CALMING_FACTOR
//       (0.5, inlined const) : 0) + _toyAgitationFactor; ... Recomputed on many events.
//     MusicBoxRegion.OnTriggerEnter/Exit: slime's SlimeEmotions.Add/RemoveMusicBox and
//       _currentEmotions (List<SlimeEmotions>) Add/Remove - the slimes inside that box.
//   Coop
//     Reproduce.ReproducePeriod() = Randoms.GetInRange(MinReproduceGameHours,
//       MaxReproduceGameHours); used by InitModel / RegistryUpdate to set
//       model.nextReproduceTime. WithinCoop() = CoopRegion.IsWithin(transform.position):
//       any CoopRegion (static _allCoops) whose Collider.bounds contains the position.
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
//   * SlimeEmotions.ComputeAgitationChangePerHour detour: for a slime inside a music box
//     (_musicBoxCount > 0) that is in the _currentEmotions of a MusicBoxRegion whose
//     corral owns Music Box II, adds 0.5 * (m - 1) to _toyAgitationFactor while the
//     original runs -> the bonus becomes 0.5 * m. MusicBoxRegion.OnTriggerEnter detour
//     (pre) only registers the region (GC handle, a handful per save; dead ones are
//     dropped and their handles freed by a posted job).
//   * Reproduce.ReproducePeriod detour: result / m when the hen is inside a coop whose
//     CoopRegion collider bounds contain it and whose plot owns Faster Hens.
//   * SpawnResource.UpdateToTime detour (garden plots only): before the original, within
//     spawning hours and without blockers, advances model.nextSpawnTime by (m - 1) *
//     (1 + watered factor) * deltaWorldTime - the sprinkler's own mechanism.
//   * Incinerate.ProcessIncinerateResults detours (both overloads): AshPerIncineration x m
//     while the original runs; the vanilla MaxUnits clamp still applies.
//
// Fault policy: every detour runs its logic in a DetourGuard (native fault -> module
// faulted, detours pass-through). Objects are class- and IsAlive-checked. No scans, no
// per-frame work; GC handles only for music box regions (never freed inside a hook).

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <format>
#include <memory>
#include <string>
#include <vector>

#include "core/detour_guard.h"
#include "core/il2cpp_api.h"
#include "core/log.h"
#include "core/main_thread.h"
#include "core/module.h"
#include "core/unity.h"
#include "modules/custom_upgrades/api.h"

namespace sr2::modules {
namespace {

namespace unity = sr2::core::unity;
namespace main_thread = sr2::core::main_thread;
using sr2::il2cpp::api;
using sr2::il2cpp::Resolver;
using custom_upgrades::Family;

constexpr const char* kModule = "plot_upgrades";
constexpr const char* kImage = "Assembly-CSharp";
constexpr const char* kCore = "UnityEngine.CoreModule";
constexpr const char* kPhysics = "UnityEngine.PhysicsModule";
constexpr const char* kDataNs = "MonomiPark.SlimeRancher.DataModel";
constexpr const char* kSlimeNs = "MonomiPark.SlimeRancher.Slime";
constexpr int kPlotGarden = 4;  // LandPlot.Id.GARDEN
constexpr size_t kMaxRegions = 64;

constexpr float kMusicBoxCalming = 0.5f;  // MusicBoxRegion.EXTRA_CALMING_FACTOR (inlined const)

struct Vec3 { float x, y, z; };
struct Quat { float x, y, z, w; };
struct Bounds { Vec3 center, extents; };

bool IsA(Il2CppObject* obj, Il2CppClass* k) {
    const auto& a = api();
    if (!obj || !k || !a.il2cpp_object_get_class) return false;
    Il2CppClass* c = a.il2cpp_object_get_class(obj);
    if (c == k) return true;
    return c && a.il2cpp_class_is_assignable_from && a.il2cpp_class_is_assignable_from(k, c);
}

template <class F>
F Export(const char* name) {
    HMODULE m = api().module;
    return m ? reinterpret_cast<F>(GetProcAddress(m, name)) : nullptr;
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

enum Feature { kFMusic, kFCoop, kFGarden, kFAsh, kFeatureCount };
const char* const kFeatureNames[kFeatureCount] = {"music box", "coop", "garden", "incinerator ash"};

float Mult(Il2CppObject* key, Family f) { return custom_upgrades::Multiplier(key, f); }

class PlotUpgrades final : public sr2::core::Module {
public:
    const char* Name() const override { return kModule; }
    bool DefaultEnabled() const override { return true; }
    bool Setup(sr2::core::ModuleContext& ctx) override;
    void OnEnable() override {
        for (auto& l : logged_) l.store(false);
        enabled_.store(true);
    }
    void OnDisable() override { enabled_.store(false); }

    bool On() const { return enabled_.load(std::memory_order_relaxed); }
    // true once per feature per enable (for the one-shot apply log)
    bool LogOnce(Feature f) { return !logged_[f].exchange(true, std::memory_order_relaxed); }

    // ---- detour bodies (main thread, inside DetourGuard) ----
    void MusicBefore(Il2CppObject* self, FieldRestore<float>& r);
    void RegisterRegion(Il2CppObject* region);
    float CoopAfter(Il2CppObject* self, float period);
    void GardenBefore(Il2CppObject* self, double deltaWorldTime, float timeOfDay);
    void AshBefore(Il2CppObject* self, FieldRestore<float>& r);

private:
    struct Region {
        Il2CppObject* obj = nullptr;
        Il2CppGCHandle handle = nullptr;
    };
    bool ListContains(void* list, Il2CppObject* item);

    std::atomic<bool> enabled_{false};
    std::atomic<bool> logged_[kFeatureCount]{};
    std::vector<Region> regions_;  // main thread only

public:
    // classes
    Il2CppClass* emotionsClass_ = nullptr;
    Il2CppClass* musicRegionClass_ = nullptr;
    Il2CppClass* reproduceClass_ = nullptr;
    Il2CppClass* coopRegionClass_ = nullptr;
    Il2CppClass* colliderClass_ = nullptr;
    Il2CppClass* spawnResClass_ = nullptr;
    Il2CppClass* spawnModelClass_ = nullptr;
    Il2CppClass* landPlotClass_ = nullptr;
    Il2CppClass* incinerateClass_ = nullptr;
    // offsets
    size_t offToyFactor_ = 0;         // SlimeEmotions._toyAgitationFactor (float)
    size_t offMusicBoxCount_ = 0;     // SlimeEmotions._musicBoxCount (int)
    size_t offRegionEmotions_ = 0;    // MusicBoxRegion._currentEmotions (List<SlimeEmotions>)
    size_t offSrDef_ = 0;             // SpawnResource._resourceGrowerDefinition
    size_t offSrLandPlot_ = 0;        // SpawnResource._landPlot
    size_t offSrBlockers_ = 0;        // SpawnResource._spawnBlockers (int)
    size_t offSrModel_ = 0;           // SpawnResource._model (SpawnResourceModel)
    size_t offNextSpawn_ = 0;         // SpawnResourceModel.nextSpawnTime (double, world s)
    size_t offPlotType_ = 0;          // LandPlot.TypeId (int enum)
    size_t offDefWaterFactor_ = 0;    // ResourceGrowerDefinition._wateredTimeFactor (float)
    size_t offAshPer_ = 0;            // Incinerate.AshPerIncineration (float)
    FieldInfo* allCoopsField_ = nullptr;  // CoopRegion._allCoops (static List<CoopRegion>)
    // private game methods called from detours
    using BoolFn = bool (*)(Il2CppObject*, const MethodInfo*);
    using BoolFloatFn = bool (*)(Il2CppObject*, float, const MethodInfo*);
    using ObjFn = Il2CppObject* (*)(Il2CppObject*, const MethodInfo*);
    using PosFn = Vec3 (*)(Il2CppObject*, const MethodInfo*);
    using BoundsFn = Bounds (*)(Il2CppObject*, const MethodInfo*);
    using StaticGetFn = void (*)(FieldInfo*, void*);
    BoolFn withinCoop_ = nullptr;
    const MethodInfo* withinCoopMi_ = nullptr;
    BoolFn isWatered_ = nullptr;
    const MethodInfo* isWateredMi_ = nullptr;
    BoolFloatFn withinHours_ = nullptr;
    const MethodInfo* withinHoursMi_ = nullptr;
    ObjFn getTransform_ = nullptr;
    const MethodInfo* getTransformMi_ = nullptr;
    PosFn getPosition_ = nullptr;
    const MethodInfo* getPositionMi_ = nullptr;
    BoundsFn getBounds_ = nullptr;
    const MethodInfo* getBoundsMi_ = nullptr;
    StaticGetFn staticGet_ = nullptr;
};

PlotUpgrades* g_self = nullptr;

sr2::core::DetourGuard g_musicGuard{kModule, "SlimeEmotions.ComputeAgitationChangePerHour"};
sr2::core::DetourGuard g_regionGuard{kModule, "MusicBoxRegion.OnTriggerEnter"};
sr2::core::DetourGuard g_coopGuard{kModule, "Reproduce.ReproducePeriod"};
sr2::core::DetourGuard g_gardenGuard{kModule, "SpawnResource.UpdateToTime"};
sr2::core::DetourGuard g_ash2Guard{kModule, "Incinerate.ProcessIncinerateResults(2)"};
sr2::core::DetourGuard g_ash4Guard{kModule, "Incinerate.ProcessIncinerateResults(4)"};

using VoidFn = void (*)(Il2CppObject*, const MethodInfo*);
using VoidObjFn = void (*)(Il2CppObject*, Il2CppObject*, const MethodInfo*);
using FloatFn = float (*)(Il2CppObject*, const MethodInfo*);
using SrUpdateFn = void (*)(Il2CppObject*, double, double, float, const MethodInfo*);
using Ash2Fn = bool (*)(Il2CppObject*, Il2CppObject*, int32_t, const MethodInfo*);
using Ash4Fn = void (*)(Il2CppObject*, Il2CppObject*, int32_t, Vec3, Quat, const MethodInfo*);

VoidFn g_origMusic = nullptr;
VoidObjFn g_origRegionEnter = nullptr;
FloatFn g_origCoop = nullptr;
SrUpdateFn g_origGarden = nullptr;
Ash2Fn g_origAsh2 = nullptr;
Ash4Fn g_origAsh4 = nullptr;

// Module on and custom_upgrades (the tiers) available.
bool Live() { return g_self && g_self->On() && custom_upgrades::Active(); }

void MusicDetour(Il2CppObject* self, const MethodInfo* mi) {
    FieldRestore<float> r;
    if (self && Live()) g_musicGuard.Run([&] { g_self->MusicBefore(self, r); });
    g_origMusic(self, mi);
}

void RegionEnterDetour(Il2CppObject* self, Il2CppObject* collider, const MethodInfo* mi) {
    if (self && Live()) g_regionGuard.Run([&] { g_self->RegisterRegion(self); });
    g_origRegionEnter(self, collider, mi);
}

float CoopDetour(Il2CppObject* self, const MethodInfo* mi) {
    float period = g_origCoop(self, mi);
    if (self && Live()) {
        float out = period;
        if (g_coopGuard.Run([&] { out = g_self->CoopAfter(self, period); })) return out;
    }
    return period;
}

void GardenDetour(Il2CppObject* self, double worldTime, double delta, float timeOfDay, const MethodInfo* mi) {
    if (self && Live()) g_gardenGuard.Run([&] { g_self->GardenBefore(self, delta, timeOfDay); });
    g_origGarden(self, worldTime, delta, timeOfDay, mi);
}

bool Ash2Detour(Il2CppObject* self, Il2CppObject* id, int32_t amount, const MethodInfo* mi) {
    FieldRestore<float> r;
    if (self && Live()) g_ash2Guard.Run([&] { g_self->AshBefore(self, r); });
    return g_origAsh2(self, id, amount, mi);
}

void Ash4Detour(Il2CppObject* self, Il2CppObject* id, int32_t amount, Vec3 pos, Quat rot, const MethodInfo* mi) {
    FieldRestore<float> r;
    if (self && Live()) g_ash4Guard.Run([&] { g_self->AshBefore(self, r); });
    g_origAsh4(self, id, amount, pos, rot, mi);
}

std::string NameOf(Il2CppObject* obj, const char* fallback) {
    std::string n = unity::GetName(obj);
    return n.empty() ? std::string(fallback) : n;
}

// ------------------------------------------------------------------ bodies --

bool PlotUpgrades::ListContains(void* list, Il2CppObject* item) {
    for (size_t i = 0, n = unity::ListCount(list); i < n && i < 512; ++i)
        if (unity::ListAt(list, i) == item) return true;
    return false;
}

void PlotUpgrades::RegisterRegion(Il2CppObject* region) {
    if (!main_thread::IsMainThread() || !IsA(region, musicRegionClass_) || !unity::IsAlive(region)) return;
    for (const Region& r : regions_)
        if (r.obj == region) return;
    // Drop dead regions (demolished corrals); their handles are freed by a posted job.
    for (auto it = regions_.begin(); it != regions_.end();) {
        if (!unity::IsAlive(it->obj)) {
            Il2CppGCHandle h = it->handle;
            main_thread::Post([h] { unity::FreeGCHandle(h); });
            it = regions_.erase(it);
        } else {
            ++it;
        }
    }
    if (regions_.size() >= kMaxRegions) return;
    Il2CppGCHandle h = unity::NewGCHandle(region);
    if (!h) return;
    regions_.push_back({region, h});
}

void PlotUpgrades::MusicBefore(Il2CppObject* self, FieldRestore<float>& r) {
    if (!main_thread::IsMainThread()) return;
    // Cheap pre-filter first: only slimes inside a music box.
    if (unity::Field<int32_t>(self, offMusicBoxCount_) <= 0 || regions_.empty()) return;
    if (!IsA(self, emotionsClass_) || !unity::IsAlive(self)) return;
    float m = 1.0f;
    for (const Region& reg : regions_) {
        if (!unity::IsAlive(reg.obj)) continue;
        const float rm = Mult(reg.obj, Family::kMusicBox);  // cached per region
        if (!(rm > m)) continue;
        if (ListContains(unity::Field<void*>(reg.obj, offRegionEmotions_), self)) m = rm;
    }
    if (!(m > 1.0f)) return;
    float& toy = unity::Field<float>(self, offToyFactor_);
    const float orig = toy;
    if (!std::isfinite(orig)) return;
    const float extra = kMusicBoxCalming * (m - 1.0f);
    r.saved = orig;
    r.where = &toy;
    toy = orig + extra;
    if (LogOnce(kFMusic))
        SR2_INFO("[{}] music box II: slime '{}' calming bonus {:.2f} -> {:.2f}", kModule, NameOf(self, "slime"),
                 kMusicBoxCalming, kMusicBoxCalming * m);
}

float PlotUpgrades::CoopAfter(Il2CppObject* self, float period) {
    if (!main_thread::IsMainThread() || !withinCoop_ || !IsA(self, reproduceClass_) || !unity::IsAlive(self))
        return period;
    if (!std::isfinite(period) || !(period > 0.0f)) return period;
    if (!withinCoop_(self, withinCoopMi_)) return period;
    // Which coop: the CoopRegion whose collider bounds contain the hen (as CoopRegion.IsWithin).
    void* coops = nullptr;
    staticGet_(allCoopsField_, &coops);
    if (!coops) return period;
    Il2CppObject* t = getTransform_(self, getTransformMi_);
    if (!unity::IsAlive(t)) return period;
    const Vec3 p = getPosition_(t, getPositionMi_);
    float m = 1.0f;
    for (size_t i = 0, n = unity::ListCount(coops); i < n && i < 64; ++i) {
        auto* region = static_cast<Il2CppObject*>(unity::ListAt(coops, i));
        if (!IsA(region, coopRegionClass_) || !unity::IsAlive(region)) continue;
        const float rm = Mult(region, Family::kCoop);  // cached per region
        if (!(rm > m)) continue;
        Il2CppObject* col = unity::GetComponent(region, colliderClass_);
        if (!col) continue;
        const Bounds b = getBounds_(col, getBoundsMi_);
        if (std::fabs(p.x - b.center.x) <= b.extents.x && std::fabs(p.y - b.center.y) <= b.extents.y &&
            std::fabs(p.z - b.center.z) <= b.extents.z)
            m = rm;
    }
    if (!(m > 1.0f)) return period;
    const float out = std::max(period / m, 0.05f);
    if (LogOnce(kFCoop))
        SR2_INFO("[{}] coop: '{}' Reproduce.ReproducePeriod {:.2f} h -> {:.2f} h (Faster Hens x{:.2f})", kModule,
                 NameOf(self, "animal"), period, out, m);
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
    const float m = Mult(plot, Family::kGarden);
    if (!(m > 1.0f)) return;
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
    double& next = unity::Field<double>(model, offNextSpawn_);
    if (!std::isfinite(next)) return;
    const double advance = static_cast<double>(m - 1.0f) * rate * delta;
    if (!(advance > 0.0)) return;
    next -= advance;
    if (LogOnce(kFGarden))
        SR2_INFO("[{}] garden '{}': growth rate {:.2f} -> {:.2f} (Fast Growth x{:.2f})", kModule,
                 NameOf(self, "SpawnResource"), rate, rate * m, m);
}

void PlotUpgrades::AshBefore(Il2CppObject* self, FieldRestore<float>& r) {
    if (!main_thread::IsMainThread() || !IsA(self, incinerateClass_) || !unity::IsAlive(self)) return;
    const float m = Mult(self, Family::kAsh);
    if (!(m > 1.0f)) return;
    float& per = unity::Field<float>(self, offAshPer_);
    const float orig = per;
    if (!std::isfinite(orig) || !(orig > 0.0f) || orig > 1000.0f) return;
    r.saved = orig;
    r.where = &per;
    per = orig * m;
    if (LogOnce(kFAsh))
        SR2_INFO("[{}] incinerator '{}': AshPerIncineration {:.2f} -> {:.2f} (Ash Booster; trough still capped)",
                 kModule, NameOf(self, "Incinerate"), orig, orig * m);
}

// ------------------------------------------------------------------ setup --

bool PlotUpgrades::Setup(sr2::core::ModuleContext& ctx) {
    Resolver& R = ctx.resolver();
    auto cls = [&](const char* ns, const char* name, std::string& why, const char* image = kImage) -> Il2CppClass* {
        std::string err;
        Il2CppClass* k = R.Class(image, ns, name, &err);
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

    // --- plot lookup helpers (shared) ---
    {
        std::string why;
        landPlotClass_ = cls("", "LandPlot", why);
        if (!landPlotClass_) return ctx.Fail(why);
    }

    // --- music box ---
    {
        std::string why;
        const MethodInfo* mi = nullptr;
        const MethodInfo* miEnter = nullptr;
        emotionsClass_ = cls(kSlimeNs, "SlimeEmotions", why);
        musicRegionClass_ = cls("", "MusicBoxRegion", why);
        bool ok = off(emotionsClass_, "_toyAgitationFactor", offToyFactor_, why) &&
                  off(emotionsClass_, "_musicBoxCount", offMusicBoxCount_, why) &&
                  off(musicRegionClass_, "_currentEmotions", offRegionEmotions_, why);
        void* p = ok ? method(emotionsClass_, "ComputeAgitationChangePerHour", 0, mi, why) : nullptr;
        void* pe = p ? method(musicRegionClass_, "OnTriggerEnter", 1, miEnter, why) : nullptr;
        ok = ok && p && pe &&
             hook("SlimeEmotions.ComputeAgitationChangePerHour", p, reinterpret_cast<void*>(&MusicDetour),
                  reinterpret_cast<void**>(&g_origMusic), why) &&
             hook("MusicBoxRegion.OnTriggerEnter", pe, reinterpret_cast<void*>(&RegionEnterDetour),
                  reinterpret_cast<void**>(&g_origRegionEnter), why);
        done(kFMusic, ok,
             ok ? std::format("ComputeAgitationChangePerHour RVA 0x{:X}, MusicBoxRegion.OnTriggerEnter RVA 0x{:X}",
                              Resolver::Rva(p), Resolver::Rva(pe))
                : why);
    }
    // --- coop ---
    {
        std::string why;
        const MethodInfo* mi = nullptr;
        reproduceClass_ = cls("", "Reproduce", why);
        coopRegionClass_ = cls("", "CoopRegion", why);
        colliderClass_ = cls("UnityEngine", "Collider", why, kPhysics);
        Il2CppClass* component = cls("UnityEngine", "Component", why, kCore);
        Il2CppClass* transform = cls("UnityEngine", "Transform", why, kCore);
        staticGet_ = Export<StaticGetFn>("il2cpp_field_static_get_value");
        if (!staticGet_ && why.empty()) why = "il2cpp_field_static_get_value export missing";
        if (coopRegionClass_ && why.empty()) {
            std::string err;
            allCoopsField_ = R.Field(coopRegionClass_, "_allCoops", &err);
            if (!allCoopsField_) why = "CoopRegion._allCoops: " + err;
        }
        void* within = why.empty() ? method(reproduceClass_, "WithinCoop", 0, withinCoopMi_, why) : nullptr;
        withinCoop_ = reinterpret_cast<BoolFn>(within);
        getTransform_ = reinterpret_cast<ObjFn>(within ? method(component, "get_transform", 0, getTransformMi_, why)
                                                       : nullptr);
        getPosition_ = reinterpret_cast<PosFn>(getTransform_ ? method(transform, "get_position", 0, getPositionMi_, why)
                                                             : nullptr);
        getBounds_ = reinterpret_cast<BoundsFn>(getPosition_ ? method(colliderClass_, "get_bounds", 0, getBoundsMi_, why)
                                                             : nullptr);
        void* p = getBounds_ ? method(reproduceClass_, "ReproducePeriod", 0, mi, why) : nullptr;
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

    g_self = this;
    SR2_INFO("[{}] setup ok ({}/{} tier effects; multipliers come from the per-plot tiers bought in the plot menus, "
             "see custom_upgrades){}",
             kModule, okCount, static_cast<int>(kFeatureCount), report);
    return true;
}

}  // namespace

std::unique_ptr<sr2::core::Module> CreatePlotUpgrades() { return std::make_unique<PlotUpgrades>(); }

}  // namespace sr2::modules
