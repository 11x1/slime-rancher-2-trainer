// misc_tweaks: small speed-ups, each off by default (Misc > General card). See NOTES.md for the research.
//
//   * Fast Fabricator purchases: FabricatorUI.OnItemActivated(item) (the item grid's activate / Fabricate /
//     Upgrade) normally opens FabricatorConfirmPopup (count picker, confirm), and after the confirm plays the
//     popup's success animation before the menu takes input again. With the option on, the detour checks the
//     item as the popup's confirm does (IFabricatableItem.CheckCanFabricate(1) == NONE) and, if it can be made,
//     calls the game's own FabricatorUI.OnConfirmationConfirmed(item, 1) (what the popup's Confirmed event
//     runs: FabricateAndSpendCost(1), the pinned-recipe update, the legend, FabricatorContent.ItemCrafted) and
//     plays the popup's success cue (FabricatorSoundBank.PlayConfirmationConfirmedFor{Upgrade,Gadget}). No popup,
//     no animation. Anything that cannot be made goes the game's way (the popup with its reason, or nothing).
//     CheckCanFabricate is an explicit interface implementation, so its metadata name is
//     "MonomiPark.SlimeRancher.UI.Fabricator.IFabricatableItem.CheckCanFabricate" (the plain name is not found).
//   * Skip upgrade animation: FabricatorConfirmPopup.HideForConfirm (after the confirm) starts the popup's success
//     Animator, whose end calls NotifyAnimatorFadeOutComplete (fade out, FadeOutComplete -> FabricatorUI.
//     ReturnToContent) and whose event plays PlaySuccessAudioCue. With the option on, the detour calls both right
//     after the original: the popup closes and the menu takes input in the same frame.
//   * Vacpack shooting: VacuumItem.Update shoots when fixedTime >= _nextShot and then sets
//     _nextShot = now + ShootCooldown / GetShootSpeedFactor(inVac). GetShootSpeedFactor is the largest
//     IVacShootAccelerator factor among the objects in the vac cone (SiloCatcher, ScorePlort, DroneFuelDepositor,
//     ResourceDepositor: VacAccelerationHelper.Factor, ramping from 1 by 0.5 per second of continuous feeding up to
//     3 for inputs, 1.75 for outputs), else 1. The detour on GetShootSpeedFactor applies "fast shooting
//     everywhere" (the same ramp, timed from the first shot of a burst of shots, wherever the vac aims) and then the
//     multiplier. The shot's audio volume is 1 / factor in the game; the detour on Expel(float) keeps it at least
//     1/3 (the quietest the game itself goes).
//
// Fault policy: the detours run on the main thread; their logic is inside DetourGuards (a native fault marks
// the module faulted and its hooks become pass-through). Game calls are direct method pointers.

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
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

constexpr const char* kModule = "misc_tweaks";
constexpr const char* kImage = "Assembly-CSharp";
constexpr const char* kFastFab = "fast_fabricator";
constexpr const char* kSkipAnim = "skip_fab_anim";
constexpr const char* kVacFast = "vac_always_fast";
constexpr const char* kVacMult = "vac_shoot_mult";
// VacAccelerationHelper.CreateInput (1.3.0 RVA 0x99A530): MinFactor 1, MaxFactor 3, Speed 0.5/s, Duration 1 s.
// CreateOutput (0x99A590): MaxFactor 1.75. "Fast shooting everywhere" uses the input ports' ramp.
constexpr float kInputMaxFactor = 3.0f;
constexpr float kInputRampPerSec = 0.5f;  // VacAccelerationHelper.Speed of the input ports
constexpr double kInputHoldSec = 1.0;     // VacAccelerationHelper.Duration: the ramp ends 1 s after the last item
constexpr float kMinShotVolume = 1.0f / kInputMaxFactor;  // the game's own volume at that factor
constexpr int kVerboseBuys = 20;                            // log every fast purchase up to this many
constexpr int kVerboseSkips = 20;                           // log every activation that goes the game's way
constexpr int kVerboseAnims = 10;                           // log every skipped success animation
constexpr int kVerboseBursts = 10;                          // log every fast-shooting burst
constexpr const char* kItemNs = "MonomiPark.SlimeRancher.UI.Fabricator";
// Explicit interface implementations carry the interface's full name in the metadata.
constexpr const char* kCheckExplicit = "MonomiPark.SlimeRancher.UI.Fabricator.IFabricatableItem.CheckCanFabricate";

bool IsA(Il2CppObject* obj, Il2CppClass* k) {
    const auto& a = api();
    if (!obj || !k || !a.il2cpp_object_get_class) return false;
    Il2CppClass* c = a.il2cpp_object_get_class(obj);
    if (c == k) return true;
    return c && a.il2cpp_class_is_assignable_from && a.il2cpp_class_is_assignable_from(k, c);
}

template <class Fn>
struct M {
    Fn fn = nullptr;
    const MethodInfo* mi = nullptr;
    explicit operator bool() const { return fn != nullptr; }
};

std::atomic<bool> g_enabled{false};
std::atomic<bool> g_fastFab{false}, g_skipAnim{false}, g_vacFast{false};
std::atomic<float> g_vacMult{1.0f};

void LoadCfg() {
    g_fastFab.store(settings::GetBool(kModule, kFastFab));
    g_skipAnim.store(settings::GetBool(kModule, kSkipAnim));
    g_vacFast.store(settings::GetBool(kModule, kVacFast));
    float m = settings::GetFloat(kModule, kVacMult);
    g_vacMult.store(std::isfinite(m) ? std::clamp(m, 1.0f, 5.0f) : 1.0f);
}

bool VacBoosted() { return g_vacFast.load(std::memory_order_relaxed) || g_vacMult.load(std::memory_order_relaxed) > 1.0f; }

class MiscTweaks final : public sr2::core::Module {
public:
    const char* Name() const override { return kModule; }
    bool DefaultEnabled() const override { return true; }  // every option is off / 1x by default
    bool Setup(sr2::core::ModuleContext& ctx) override;
    void OnEnable() override { g_enabled.store(true); }
    void OnDisable() override { g_enabled.store(false); }

    // Main thread, inside the DetourGuard. True = bought here (the original must not run).
    bool FastBuy(Il2CppObject* ui, Il2CppObject* item);
    // Main thread, inside the DetourGuard, after the original HideForConfirm.
    void SkipSuccessAnimation(Il2CppObject* popup);
    // Main thread: the "fast shooting everywhere" ramp's factor for this shot.
    float RampFactor(Il2CppObject* vac);

private:
    bool SetupFabricator(sr2::core::ModuleContext& ctx, std::string& why);
    bool SetupSkipAnim(sr2::core::ModuleContext& ctx, std::string& why);
    bool SetupVacpack(sr2::core::ModuleContext& ctx, std::string& why);
    void RegisterSettings(bool fab, bool anim, bool vac);
    void LogSkip(Il2CppObject* item, const std::string& why);

    Il2CppClass* fabUiClass_ = nullptr;      // MonomiPark.SlimeRancher.UI.Fabricator.FabricatorUI
    Il2CppClass* gadgetItemClass_ = nullptr; // GadgetFabricatableItem
    Il2CppClass* upgradeItemClass_ = nullptr;// PlayerUpgradeFabricatableItem
    size_t offConfirming_ = 0, offSoundBank_ = 0;  // FabricatorUI._confirming / _soundBank
    using CheckFn = int32_t (*)(Il2CppObject*, int32_t, const MethodInfo*);
    using ConfirmFn = void (*)(Il2CppObject*, Il2CppObject*, int32_t, const MethodInfo*);
    using CueFn = bool (*)(Il2CppObject*, const MethodInfo*);  // returns whether a cue played
    M<CheckFn> gadgetCheck_, upgradeCheck_;  // <item>.CheckCanFabricate(int)
    M<ConfirmFn> confirmed_;                 // FabricatorUI.OnConfirmationConfirmed(IFabricatableItem, int)
    M<CueFn> cueUpgrade_, cueGadget_;       // FabricatorSoundBank.PlayConfirmationConfirmedFor{Upgrade,Gadget}
    int buys_ = 0, skips_ = 0;
    // FabricatorConfirmPopup: _successAnimator / _transitionFader / _isClosing; NotifyAnimatorFadeOutComplete,
    // PlaySuccessAudioCue.
    Il2CppClass* popupClass_ = nullptr;
    size_t offAnimator_ = 0, offFader_ = 0, offClosing_ = 0;
    using VoidFn = void (*)(Il2CppObject*, const MethodInfo*);
    M<VoidFn> notifyFadeOut_, successCue_;
    int anims_ = 0;
    // The fast-shooting ramp (main thread only). rampVac_ is compared, never dereferenced.
    size_t offCooldown_ = 0;  // VacuumItem.ShootCooldown (0 = unknown)
    const void* rampVac_ = nullptr;
    double rampBegin_ = 0, rampLast_ = -1e9;
    float rampPeak_ = 1.0f;
    int rampShots_ = 0, bursts_ = 0;
    int onChange_ = 0;
};

MiscTweaks* g_self = nullptr;
sr2::core::DetourGuard g_activateGuard{kModule, "FabricatorUI.OnItemActivated"};
sr2::core::DetourGuard g_hideGuard{kModule, "FabricatorConfirmPopup.HideForConfirm"};
sr2::core::DetourGuard g_factorGuard{kModule, "VacuumItem.GetShootSpeedFactor"};
sr2::core::DetourGuard g_expelGuard{kModule, "VacuumItem.Expel"};

using ActivateFn = void (*)(Il2CppObject*, Il2CppObject*, const MethodInfo*);
using HideFn = void (*)(Il2CppObject*, const MethodInfo*);
using FactorFn = float (*)(Il2CppObject*, Il2CppObject*, const MethodInfo*);
using ExpelFn = void (*)(Il2CppObject*, float, const MethodInfo*);
ActivateFn g_origActivate = nullptr;
HideFn g_origHide = nullptr;
FactorFn g_origFactor = nullptr;
ExpelFn g_origExpel = nullptr;

void ActivateDetour(Il2CppObject* self, Il2CppObject* item, const MethodInfo* mi) {
    if (g_self && self && item && g_enabled.load() && g_fastFab.load()) {
        bool bought = false;
        g_activateGuard.Run([&] {
            if (main_thread::IsMainThread()) bought = g_self->FastBuy(self, item);
        });
        if (bought) return;
    }
    g_origActivate(self, item, mi);
}

void HideDetour(Il2CppObject* self, const MethodInfo* mi) {
    g_origHide(self, mi);
    if (g_self && self && g_enabled.load() && g_skipAnim.load())
        g_hideGuard.Run([&] {
            if (main_thread::IsMainThread()) g_self->SkipSuccessAnimation(self);
        });
}

float FactorDetour(Il2CppObject* self, Il2CppObject* inVac, const MethodInfo* mi) {
    float f = g_origFactor(self, inVac, mi);
    if (g_self && g_enabled.load() && VacBoosted())
        g_factorGuard.Run([&] {
            float r = std::isfinite(f) && f > 0.0f ? f : 1.0f;
            if (g_vacFast.load() && main_thread::IsMainThread()) r = std::max(r, g_self->RampFactor(self));
            f = r * g_vacMult.load();
        });
    return f;
}

void ExpelDetour(Il2CppObject* self, float volume, const MethodInfo* mi) {
    if (g_enabled.load() && VacBoosted()) g_expelGuard.Run([&] { volume = std::max(volume, kMinShotVolume); });
    g_origExpel(self, volume, mi);
}

template <class Fn>
bool Bind(sr2::il2cpp::Resolver& R, M<Fn>& m, Il2CppClass* k, const char* name, int argc, std::string& why,
          const char* altName = nullptr) {
    if (!k) return false;
    std::string err;
    const MethodInfo* mi = R.Method(k, name, argc, &err);
    if (!mi && altName) mi = R.Method(k, altName, argc, nullptr);
    void* p = mi ? R.MethodPointer(mi, &err) : nullptr;
    if (!p) {
        if (why.empty()) why = std::format("{}: {}", name, err.empty() ? "no native body" : err);
        return false;
    }
    m.fn = reinterpret_cast<Fn>(p);
    m.mi = mi;
    return true;
}

bool MiscTweaks::SetupFabricator(sr2::core::ModuleContext& ctx, std::string& why) {
    auto& R = ctx.resolver();
    auto cls = [&](const char* ns, const char* name) -> Il2CppClass* {
        std::string err;
        Il2CppClass* k = R.Class(kImage, ns, name, &err);
        if (!k && why.empty()) why = err.empty() ? std::format("class {}.{} not found", ns, name) : err;
        return k;
    };
    auto off = [&](Il2CppClass* k, const char* field, size_t& out) {
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
    constexpr const char* kFabNs = kItemNs;
    fabUiClass_ = cls(kFabNs, "FabricatorUI");
    gadgetItemClass_ = cls(kFabNs, "GadgetFabricatableItem");
    upgradeItemClass_ = cls(kFabNs, "PlayerUpgradeFabricatableItem");
    Il2CppClass* sound = cls(kFabNs, "FabricatorSoundBank");
    bool ok = off(fabUiClass_, "_confirming", offConfirming_) && off(fabUiClass_, "_soundBank", offSoundBank_) &&
              Bind(R, gadgetCheck_, gadgetItemClass_, kCheckExplicit, 1, why, "CheckCanFabricate") &&
              Bind(R, upgradeCheck_, upgradeItemClass_, kCheckExplicit, 1, why, "CheckCanFabricate") &&
              Bind(R, confirmed_, fabUiClass_, "OnConfirmationConfirmed", 2, why);
    if (!ok) return false;
    std::string cw;  // the success cue is optional
    Bind(R, cueUpgrade_, sound, "PlayConfirmationConfirmedForUpgrade", 0, cw);
    Bind(R, cueGadget_, sound, "PlayConfirmationConfirmedForGadget", 0, cw);
    if (!cw.empty()) SR2_INFO("[{}] Fabricator success cue missing ({}): fast purchases are silent", kModule, cw);
    std::string err;
    const MethodInfo* act = R.Method(fabUiClass_, "OnItemActivated", 1, &err);
    void* target = act ? R.MethodPointer(act, &err) : nullptr;
    if (!target) {
        why = "FabricatorUI.OnItemActivated: " + (err.empty() ? std::string("no native body") : err);
        return false;
    }
    if (!ctx.HookAddress("FabricatorUI.OnItemActivated", target, reinterpret_cast<void*>(&ActivateDetour),
                         reinterpret_cast<void**>(&g_origActivate))) {
        why = "hook FabricatorUI.OnItemActivated failed";
        return false;
    }
    SR2_INFO("[{}] fast Fabricator purchases: FabricatorUI.OnItemActivated hooked at RVA 0x{:X}; CheckCanFabricate "
             "at RVA 0x{:X} (upgrade) / 0x{:X} (gadget), OnConfirmationConfirmed at RVA 0x{:X}",
             kModule, sr2::il2cpp::Resolver::Rva(target),
             sr2::il2cpp::Resolver::Rva(reinterpret_cast<void*>(upgradeCheck_.fn)),
             sr2::il2cpp::Resolver::Rva(reinterpret_cast<void*>(gadgetCheck_.fn)),
             sr2::il2cpp::Resolver::Rva(reinterpret_cast<void*>(confirmed_.fn)));
    return true;
}

bool MiscTweaks::SetupSkipAnim(sr2::core::ModuleContext& ctx, std::string& why) {
    auto& R = ctx.resolver();
    std::string err;
    popupClass_ = R.Class(kImage, kItemNs, "FabricatorConfirmPopup", &err);
    if (!popupClass_) {
        why = "FabricatorConfirmPopup: " + err;
        return false;
    }
    for (auto [name, out] : {std::pair{"_successAnimator", &offAnimator_}, std::pair{"_transitionFader", &offFader_},
                             std::pair{"_isClosing", &offClosing_}}) {
        auto o = R.FieldOffset(popupClass_, name, &err);
        if (!o) {
            why = std::format("FabricatorConfirmPopup.{}: {}", name, err);
            return false;
        }
        *out = *o;
    }
    if (!Bind(R, notifyFadeOut_, popupClass_, "NotifyAnimatorFadeOutComplete", 0, why) ||
        !Bind(R, successCue_, popupClass_, "PlaySuccessAudioCue", 0, why))
        return false;
    const MethodInfo* hide = R.Method(popupClass_, "HideForConfirm", 0, &err);
    void* target = hide ? R.MethodPointer(hide, &err) : nullptr;
    if (!target) {
        why = "FabricatorConfirmPopup.HideForConfirm: " + (err.empty() ? std::string("no native body") : err);
        return false;
    }
    if (!ctx.HookAddress("FabricatorConfirmPopup.HideForConfirm", target, reinterpret_cast<void*>(&HideDetour),
                         reinterpret_cast<void**>(&g_origHide))) {
        why = "hook FabricatorConfirmPopup.HideForConfirm failed";
        return false;
    }
    SR2_INFO("[{}] skip upgrade animation: FabricatorConfirmPopup.HideForConfirm hooked at RVA 0x{:X}", kModule,
             sr2::il2cpp::Resolver::Rva(target));
    return true;
}

bool MiscTweaks::SetupVacpack(sr2::core::ModuleContext& ctx, std::string& why) {
    auto& R = ctx.resolver();
    std::string err;
    Il2CppClass* vac = R.Class(kImage, "MonomiPark.SlimeRancher.Player.PlayerItems", "VacuumItem", &err);
    if (!vac) {
        why = "VacuumItem: " + err;
        return false;
    }
    if (auto o = R.FieldOffset(vac, "ShootCooldown", nullptr)) offCooldown_ = *o;  // optional: the ramp's gap
    const MethodInfo* factor = R.Method(vac, "GetShootSpeedFactor", 1, &err);
    void* fp = factor ? R.MethodPointer(factor, &err) : nullptr;
    if (!fp) {
        why = "VacuumItem.GetShootSpeedFactor: " + (err.empty() ? std::string("no native body") : err);
        return false;
    }
    // Expel(float shootAudioVolume) is private; the public overload takes 4 arguments.
    const MethodInfo* expel = R.MethodByParams(vac, "Expel", {"System.Single"}, &err);
    void* ep = expel ? R.MethodPointer(expel, &err) : nullptr;
    if (!ctx.HookAddress("VacuumItem.GetShootSpeedFactor", fp, reinterpret_cast<void*>(&FactorDetour),
                         reinterpret_cast<void**>(&g_origFactor))) {
        why = "hook VacuumItem.GetShootSpeedFactor failed";
        return false;
    }
    const bool volume = ep && ctx.HookAddress("VacuumItem.Expel", ep, reinterpret_cast<void*>(&ExpelDetour),
                                              reinterpret_cast<void**>(&g_origExpel));
    SR2_INFO("[{}] vacpack shooting: VacuumItem.GetShootSpeedFactor hooked at RVA 0x{:X}; shot volume floor {}", kModule,
             sr2::il2cpp::Resolver::Rva(fp),
             volume ? std::format("on (Expel(float) at RVA 0x{:X})", sr2::il2cpp::Resolver::Rva(ep))
                    : std::string("off (Expel(float) not hooked)"));
    return true;
}

void MiscTweaks::RegisterSettings(bool fab, bool anim, bool vac) {
    auto def = [](const char* key, const char* label, const char* group, settings::Kind kind, double d, double mn,
                  double mx, double step, const char* tip) {
        settings::Def x;
        x.module = kModule;
        x.key = key;
        x.label = label;
        x.group = group;
        x.kind = kind;
        x.def = d;
        x.min = mn;
        x.max = mx;
        x.step = step;
        x.tooltip = tip;
        return x;
    };
    if (fab)
        settings::Register(def(kFastFab, "Fast Fabricator purchases", "Fabricator", settings::Kind::Bool, 0, 0, 1, 1,
                               "Fabricate or Upgrade in the Fabricator buys at once: no confirmation window and no "
                               "purchase animation. It costs what it always costs and needs the same materials; "
                               "gadgets are made one per press (the window's amount picker is skipped). Anything "
                               "you cannot buy opens the game's window as usual"));
    if (anim)
        settings::Register(def(kSkipAnim, "Skip upgrade animation", "Fabricator", settings::Kind::Bool, 0, 0, 1, 1,
                               "After you confirm a purchase in the Fabricator's window (upgrades and gadgets), the "
                               "success animation is skipped: the window closes at once and the Fabricator takes "
                               "input again. The success sound still plays"));
    if (vac) {
        settings::Register(def(kVacFast, "Fast vacpack shooting everywhere", "Vacpack", settings::Kind::Bool, 0, 0, 1,
                               1,
                               "Holding the shoot button speeds the vacpack up the way feeding a silo, refinery or "
                               "market input does, wherever you aim: from the normal rate to 3x over 4 seconds of "
                               "shooting, back to normal 1 second after you stop"));
        settings::Def m = def(kVacMult, "Vacpack shooting speed", "Vacpack", settings::Kind::Float, 1, 1, 5, 0.25,
                              "Multiplies how fast the vacpack shoots out items. Stacks with the option above and "
                              "with the game's own speed-up at inputs (the vacpack shoots at most once per frame)");
        m.unit = "x";
        settings::Register(m);
    }
}

bool MiscTweaks::Setup(sr2::core::ModuleContext& ctx) {
    std::string fabWhy, animWhy, vacWhy;
    const bool fab = SetupFabricator(ctx, fabWhy);
    const bool anim = SetupSkipAnim(ctx, animWhy);
    const bool vac = SetupVacpack(ctx, vacWhy);
    if (!fab) SR2_WARN("[{}] fast Fabricator purchases NOT available: {}", kModule, fabWhy);
    if (!anim) SR2_WARN("[{}] skip upgrade animation NOT available: {}", kModule, animWhy);
    if (!vac) SR2_WARN("[{}] vacpack shooting options NOT available: {}", kModule, vacWhy);
    if (!fab && !anim && !vac) return ctx.Fail("nothing to do: " + fabWhy + "; " + animWhy + "; " + vacWhy);
    RegisterSettings(fab, anim, vac);
    LoadCfg();
    if (!onChange_) onChange_ = settings::OnChange(kModule, [](const std::string&) { LoadCfg(); });
    g_self = this;
    SR2_INFO("[{}] setup ok (fast Fabricator {}, skip animation {}, vacpack {}; now: fast purchases {}, skip "
             "animation {}, fast shooting {}, speed {:.2f}x)",
             kModule, fab ? "yes" : "no", anim ? "yes" : "no", vac ? "yes" : "no", g_fastFab.load() ? "on" : "off",
             g_skipAnim.load() ? "on" : "off", g_vacFast.load() ? "on" : "off", g_vacMult.load());
    return true;
}

// The popup's confirm, minus the popup: FabricatorConfirmPopup.OnSubmit checks
// IFabricatableItem.CheckCanFabricate(count) (FabricationBlockedReason, NONE = 0), then its Confirmed event runs
// FabricatorUI.OnConfirmationConfirmed(item, count); the success cue is the popup's PlaySuccessAudioCue.
void MiscTweaks::LogSkip(Il2CppObject* item, const std::string& why) {
    if (++skips_ > kVerboseSkips) return;
    SR2_INFO("[{}] fast purchase skipped, the game's way: {} ({}){}", kModule, unity::ClassName(item), why,
             skips_ == kVerboseSkips ? " - further skips are not logged" : "");
}

bool MiscTweaks::FastBuy(Il2CppObject* ui, Il2CppObject* item) {
    if (!IsA(ui, fabUiClass_) || !unity::IsAlive(ui)) {
        LogSkip(item, "not a live FabricatorUI");
        return false;
    }
    if (unity::Field<bool>(ui, offConfirming_)) {
        LogSkip(item, "the game's window is up");
        return false;
    }
    const bool upgrade = IsA(item, upgradeItemClass_);
    const M<CheckFn>& check = upgrade ? upgradeCheck_ : gadgetCheck_;
    if (!upgrade && !IsA(item, gadgetItemClass_)) {
        LogSkip(item, "neither an upgrade nor a gadget");
        return false;
    }
    const int32_t reason = check.fn(item, 1, check.mi);
    if (reason != 0) {  // cannot be made: the game's popup / refusal
        LogSkip(item, std::format("CheckCanFabricate(1) = {} (FabricationBlockedReason, 0 = NONE)", reason));
        return false;
    }
    confirmed_.fn(ui, item, 1, confirmed_.mi);
    Il2CppObject* bank = unity::Field<Il2CppObject*>(ui, offSoundBank_);
    const M<CueFn>& cue = upgrade ? cueUpgrade_ : cueGadget_;
    if (cue && bank && unity::IsAlive(bank)) cue.fn(bank, cue.mi);
    ++buys_;
    if (buys_ <= kVerboseBuys)
        SR2_INFO("[{}] fast purchase: {} ({}){}", kModule, unity::ClassName(item), upgrade ? "upgrade" : "gadget x1",
                 buys_ == kVerboseBuys ? " - further fast purchases are not logged" : "");
    return true;
}

// The game's own end of the success animation, at once: NotifyAnimatorFadeOutComplete disables the Animator, fades
// the popup out and raises FadeOutComplete (FabricatorUI.ReturnToContent); the cue is the animation's event.
// HideForConfirm without a success Animator already took the no-animation path (fade out + FadeOutComplete), so
// only the Animator path is finished here; NotifyAnimatorFadeOutComplete would throw without one.
void MiscTweaks::SkipSuccessAnimation(Il2CppObject* popup) {
    if (!IsA(popup, popupClass_) || !unity::IsAlive(popup)) return;
    if (!unity::Field<bool>(popup, offClosing_)) return;  // HideForConfirm returned early: nothing to finish
    Il2CppObject* animator = unity::Field<Il2CppObject*>(popup, offAnimator_);
    if (!animator || !unity::IsAlive(animator)) return;   // the game took the no-animation path already
    if (!unity::Field<Il2CppObject*>(popup, offFader_)) return;  // Notify would throw: let the animation run
    successCue_.fn(popup, successCue_.mi);
    notifyFadeOut_.fn(popup, notifyFadeOut_.mi);
    ++anims_;
    if (anims_ <= kVerboseAnims)
        SR2_INFO("[{}] skipped the Fabricator's success animation{}", kModule,
                 anims_ == kVerboseAnims ? " - further skips are not logged" : "");
}

// The input ports' ramp (VacAccelerationHelper.CreateInput): 1 + 0.5 per second, at most 3, timed from the first
// shot of a burst. A burst ends when no shot came for the helper's 1 s hold (or one normal cooldown, if longer:
// GetShootSpeedFactor runs once per shot, so the gap between calls is at most ShootCooldown).
float MiscTweaks::RampFactor(Il2CppObject* vac) {
    const double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    double gap = kInputHoldSec;
    if (offCooldown_) {
        const float cd = unity::Field<float>(vac, offCooldown_);
        if (std::isfinite(cd) && cd > 0.0f && cd < 10.0f) gap = std::max(gap, static_cast<double>(cd) + 0.1);
    }
    if (vac != rampVac_ || now - rampLast_ > gap) {
        if (rampShots_ > 1 && bursts_ < kVerboseBursts) {
            ++bursts_;
            SR2_INFO("[{}] fast shooting: burst of {} shots over {:.1f} s, top factor {:.2f}x{}", kModule, rampShots_,
                     rampLast_ - rampBegin_, rampPeak_,
                     bursts_ == kVerboseBursts ? " - further bursts are not logged" : "");
        }
        rampVac_ = vac;
        rampBegin_ = now;
        rampPeak_ = 1.0f;
        rampShots_ = 0;
    }
    rampLast_ = now;
    ++rampShots_;
    const float f = std::min(1.0f + static_cast<float>(now - rampBegin_) * kInputRampPerSec, kInputMaxFactor);
    rampPeak_ = std::max(rampPeak_, f);
    return f;
}

}  // namespace

std::unique_ptr<sr2::core::Module> CreateMiscTweaks() { return std::make_unique<MiscTweaks>(); }

}  // namespace sr2::modules
