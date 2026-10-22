// instant_build: skips the charge-up wait of newly placed gadgets (Market Link,
// Storage Link, teleporters, ...).
//
// Mechanism (SR2 1.3.0, decompiled in IDA):
//   * Each gadget prefab has Gadget.CooldownTimeGameHrs (formerly WaitTimeGameHrs).
//     Gadget.InitModel: if |model.waitForChargeupTime| < 0.1 (unset) and
//     CooldownTimeGameHrs > 0, model.waitForChargeupTime = TimeDirector.HoursFromNow(
//     CooldownTimeGameHrs) (world seconds). 1 game hour = 1 real minute at normal speed.
//     The value is saved with the gadget (GadgetModel.PushBase/PullBase), so a gadget
//     still charging when the game saves keeps charging after a load.
//   * Gadget.InGadgetCooldown() = !TimeDirector.HasReached(model.waitForChargeupTime);
//     consumers: TeleporterNode.IsLinkActive, ScorePlort.Start/CanDroneDeposit (Market
//     Link), SiloCatcher.Start/IsActive (Storage Link / silo / refinery ports),
//     VacSpawner.Start. ScorePlort.Start / SiloCatcher.Start subscribe to
//     Gadget.OnCooldownFinish when they start during the wait.
//   * Gadget.Update: while charging, the first Update spawns _waitingObj (the
//     countdown indicator, GadgetDirector._waitForChargeupPrefab). Once the time is
//     reached and _waitingObj exists and !_triggeredCooldownOutro, it invokes
//     OnCooldownFinish (activates the link visuals), plays the cooldown-complete cue and
//     sets _triggeredCooldownOutro; the indicator is destroyed after _chargeupTimePadding.
//   * No other usage cooldown exists on these links (SiloCatcher._nextEject is the
//     0.25 s eject rate).
//
// What this module does: a detour on Gadget.Update. When a gadget's countdown
// indicator is up and the finish has not been triggered yet (= it is charging, and
// all its components already ran Start and subscribed to OnCooldownFinish), it sets
// model.waitForChargeupTime to the current world time before the original Update
// runs. The original then completes the charge-up through the game's own path
// (OnCooldownFinish, cue, indicator removal), one frame after the indicator appeared.
// Only that time value is changed - it is the value the game would have reached on
// its own; nothing structural is touched. Gadgets loaded from a save while charging
// and gadgets placed after the module is re-enabled are handled the same way (the
// check runs every Update). Disabled -> vanilla wait for anything charging from then on.
//
// Per-frame cost: per active gadget, two field reads (indicator pointer + flag)
// inside a DetourGuard; the actual work runs once per charging gadget.

#include <windows.h>

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
#include "core/unity.h"

namespace sr2::modules {
namespace {

namespace unity = sr2::core::unity;
namespace main_thread = sr2::core::main_thread;
using sr2::il2cpp::api;

constexpr const char* kModule = "instant_build";
constexpr const char* kImage = "Assembly-CSharp";
constexpr const char* kWorldNs = "MonomiPark.SlimeRancher.World";
constexpr const char* kDataNs = "MonomiPark.SlimeRancher.DataModel";

constexpr int kVerboseShortcuts = 40;  // log every shortcut up to this many, then every 50th

bool IsA(Il2CppObject* obj, Il2CppClass* k) {
    const auto& a = api();
    if (!obj || !k || !a.il2cpp_object_get_class) return false;
    Il2CppClass* c = a.il2cpp_object_get_class(obj);
    if (c == k) return true;
    return c && a.il2cpp_class_is_assignable_from && a.il2cpp_class_is_assignable_from(k, c);
}

class InstantBuild final : public sr2::core::Module {
public:
    const char* Name() const override { return kModule; }
    bool DefaultEnabled() const override { return true; }
    bool Setup(sr2::core::ModuleContext& ctx) override;
    void OnEnable() override { enabled_.store(true); }
    void OnDisable() override { enabled_.store(false); }
    bool Enabled() const { return enabled_.load(std::memory_order_relaxed); }

    // Main thread, inside the DetourGuard.
    void BeforeUpdate(Il2CppObject* gadget);

private:
    std::atomic<bool> enabled_{false};

    Il2CppClass* gadgetClass_ = nullptr;     // MonomiPark.SlimeRancher.World.Gadget
    Il2CppClass* modelClass_ = nullptr;      // MonomiPark.SlimeRancher.DataModel.GadgetModel
    Il2CppClass* timeDirClass_ = nullptr;    // TimeDirector
    Il2CppClass* worldModelClass_ = nullptr; // MonomiPark.SlimeRancher.DataModel.WorldModel
    size_t offCooldownHrs_ = 0;   // Gadget.CooldownTimeGameHrs (float)
    size_t offModel_ = 0;         // Gadget._model
    size_t offTimeDir_ = 0;       // Gadget._timeDir
    size_t offWaitingObj_ = 0;    // Gadget._waitingObj
    size_t offTriggered_ = 0;     // Gadget._triggeredCooldownOutro (bool)
    size_t offWaitTime_ = 0;      // GadgetModel.waitForChargeupTime (double, world seconds)
    size_t offWorldModel_ = 0;    // TimeDirector._worldModel
    size_t offWorldTime_ = 0;     // WorldModel.worldTime (double)

    int shortcuts_ = 0;
    const void* lastSkipped_ = nullptr;  // identity only (log de-dup for skipped gadgets)

public:
    size_t OffWaitingObj() const { return offWaitingObj_; }
    size_t OffTriggered() const { return offTriggered_; }
};

InstantBuild* g_self = nullptr;
sr2::core::DetourGuard g_updateGuard{kModule, "Gadget.Update"};

using UpdateFn = void (*)(Il2CppObject*, const MethodInfo*);
UpdateFn g_origUpdate = nullptr;

void UpdateDetour(Il2CppObject* self, const MethodInfo* mi) {
    if (g_self && self && g_self->Enabled()) {
        g_updateGuard.Run([&] {
            // Cheap pre-filter: only gadgets whose countdown indicator is up and not finished.
            if (!unity::Field<void*>(self, g_self->OffWaitingObj())) return;
            if (unity::Field<bool>(self, g_self->OffTriggered())) return;
            if (!main_thread::IsMainThread()) return;
            g_self->BeforeUpdate(self);
        });
    }
    g_origUpdate(self, mi);
}

bool InstantBuild::Setup(sr2::core::ModuleContext& ctx) {
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

    gadgetClass_ = cls(kWorldNs, "Gadget");
    modelClass_ = cls(kDataNs, "GadgetModel");
    timeDirClass_ = cls("", "TimeDirector");
    worldModelClass_ = cls(kDataNs, "WorldModel");
    bool ok = off(gadgetClass_, "CooldownTimeGameHrs", offCooldownHrs_) && off(gadgetClass_, "_model", offModel_) &&
              off(gadgetClass_, "_timeDir", offTimeDir_) && off(gadgetClass_, "_waitingObj", offWaitingObj_) &&
              off(gadgetClass_, "_triggeredCooldownOutro", offTriggered_) &&
              off(modelClass_, "waitForChargeupTime", offWaitTime_) &&
              off(timeDirClass_, "_worldModel", offWorldModel_) && off(worldModelClass_, "worldTime", offWorldTime_);
    if (!ok) return ctx.Fail("resolve failed: " + why);

    const MethodInfo* update = nullptr;
    {
        std::string err;
        update = R.Method(gadgetClass_, "Update", 0, &err);
        if (!update) return ctx.Fail("Gadget.Update not found: " + err);
    }
    std::string err;
    void* updatePtr = R.MethodPointer(update, &err);
    if (!updatePtr) return ctx.Fail("Gadget.Update has no code: " + err);
    if (!ctx.HookAddress("Gadget.Update", updatePtr, reinterpret_cast<void*>(&UpdateDetour),
                         reinterpret_cast<void**>(&g_origUpdate)))
        return false;

    g_self = this;
    SR2_INFO("[{}] setup ok: Gadget.Update hooked at RVA 0x{:X}; offsets CooldownTimeGameHrs 0x{:X}, _model 0x{:X}, "
             "_timeDir 0x{:X}, _waitingObj 0x{:X}, _triggeredCooldownOutro 0x{:X}, "
             "GadgetModel.waitForChargeupTime 0x{:X}, TimeDirector._worldModel 0x{:X}, WorldModel.worldTime 0x{:X}",
             kModule, sr2::il2cpp::Resolver::Rva(updatePtr), offCooldownHrs_, offModel_, offTimeDir_,
             offWaitingObj_, offTriggered_, offWaitTime_, offWorldModel_, offWorldTime_);
    return true;
}

void InstantBuild::BeforeUpdate(Il2CppObject* gadget) {
    if (!IsA(gadget, gadgetClass_) || !unity::IsAlive(gadget)) return;
    const float hrs = unity::Field<float>(gadget, offCooldownHrs_);
    if (!(hrs > 0.0f)) return;  // the game only runs the countdown for CooldownTimeGameHrs > 0
    auto* model = unity::Field<Il2CppObject*>(gadget, offModel_);
    auto* timeDir = unity::Field<Il2CppObject*>(gadget, offTimeDir_);
    if (!IsA(model, modelClass_) || !IsA(timeDir, timeDirClass_) || !unity::IsAlive(timeDir)) return;
    auto* world = unity::Field<Il2CppObject*>(timeDir, offWorldModel_);
    if (!IsA(world, worldModelClass_)) return;
    const double now = unity::Field<double>(world, offWorldTime_);
    double& until = unity::Field<double>(model, offWaitTime_);
    const double remaining = until - now;
    // now must stay >= 0.1 so Gadget.InitModel never mistakes the value for "unset"
    // after a reload (it would restart the countdown).
    if (!std::isfinite(now) || !std::isfinite(until) || now < 1.0) {
        if (lastSkipped_ != gadget) {
            lastSkipped_ = gadget;
            SR2_INFO("[{}] charging gadget left alone: world time {:.1f}, ready at {:.1f}", kModule, now, until);
        }
        return;
    }
    if (!(remaining > 0.0)) return;  // already reached: the original Update finishes it
    until = now;
    ++shortcuts_;
    if (shortcuts_ <= kVerboseShortcuts || shortcuts_ % 50 == 0) {
        std::string name = unity::GetName(gadget);
        SR2_INFO("[{}] '{}' charge-up finished early: {:.1f} game min of {:.1f} left (CooldownTimeGameHrs {:.2f}){}",
                 kModule, name.empty() ? std::string("gadget") : name, remaining / 60.0, hrs * 60.0, hrs,
                 shortcuts_ == kVerboseShortcuts ? " - further shortcuts are logged every 50th" : "");
    }
}

}  // namespace

std::unique_ptr<sr2::core::Module> CreateInstantBuild() { return std::make_unique<InstantBuild>(); }

}  // namespace sr2::modules
