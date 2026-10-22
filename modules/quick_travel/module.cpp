// quick_travel: faster respawn after a knockout and less padding around teleports.
//
// Flows of SR2 1.3.0 (from the dump + a capstone disassembly of GameAssembly.dll,
// see NOTES.md for the full call chains and evidence):
//
//   Knockout: PlayerDeathHandler.OnDeath
//     - sets _isPlayerDead, resets a boss fight, analytics, music suppressor
//     - unlockWorldTime = max(HoursFromNow(GameMode.HoursLostOnDeath) or next dawn,
//       now + MinDeathTime)                                   (time penalty)
//     - LockOnDeath.Lock(unlockWorldTime), ResetPlayer(clearAmmo): drop vac, leave
//       liquids, currency penalty, health/energy/rad reset, ammo/radiant loss
//     - death events, then DeathObscurer.StartObscuring(true, _fadeTime, b__0)
//   b__0 (screen black) -> TeleportNetwork.Teleport_Death(player, _delayTime, b__1)
//     - player in the default (ranch) scene group: in-scene path on the SceneLoader
//       queue: load the death loading scene, TeleportTo(wake-up point),
//       EnqueueDelaySeconds("Wait For Min Delay On Death", _delayTime),
//       b__1, unload the death loading scene ("Wait for Death Loading Outro" 1 s)
//     - anywhere else: full LoadSceneGroup(default group, DEATH transition) with
//       b__1 as OnSceneGroupLoadedPhase2 (a real zone load)
//   b__1 opens the RanchHouseUI with SleepType UNTIL_WORLD_TIME: the house UI
//     fast-forwards time until unlockWorldTime (LockOnDeath.LockUntil), saves, then
//     calls b__2 (player alive again, obscurer fades in) and stays open until the
//     player leaves it (Close -> ResetPlayerLocation -> ResetPlayer(false) + teleport).
//
//   Teleporter: TeleporterNode.OnTriggerEnter -> TeleportNetwork.Teleport ->
//     TeleportToDestinationImpl: destination node registered (same loaded zone) and
//     not ForceSceneTransition -> TeleportTo immediately (no fade, no wait);
//     otherwise LoadSceneGroup(dest group) with a loading screen. The loading
//     queue pads every load with EnqueueDelaySeconds("Wait For Loading Scene Fade
//     In", 1 s) and LoadingScreenView.FadeToBlack (_fadeOutTime) at the end.
//
// What this module does (all resolved by name, no RVAs):
//   fast_respawn (default on), while a knockout is being handled:
//     * DeathObscurer.StartObscuring: fade time -> 0.02 s (fade out and fade in)
//     * EnqueueDelaySeconds("Wait For Min Delay On Death" / "Wait for Death
//       Loading Outro"): 0 s
//     * EnqueueLoad/UnloadDeathLoadingScene during Teleport_Death: skipped as a
//       pair (the screen is already black from the obscurer)
//     * cross-zone respawn load: same padding removal as fast_teleport
//     * the house UI still runs the time skip (the game's own fast-forward and
//       save), then the module closes it the way Esc does (RanchHouseUI.Close).
//     Everything PlayerDeathHandler does to the player is kept unchanged.
//   fast_teleport (default on): for scene loads started by a teleporter,
//     "Wait For Loading Scene Fade In" -> 0 s and the loading screen's fade to
//     black -> one frame. Same-zone teleports are already instant in the game.
//   Every scene group load is timed (LoadSceneGroup -> IsSceneLoadInProgress
//   false) and logged with what was skipped.
//
// Safety: all mod logic in detours runs inside DetourGuards (original calls stay
// outside); a native fault marks the module faulted (hooks off, detours pass
// through). runtime_invoke native faults do the same. Objects are validated
// (class + IsAlive). The one GC handle (SceneLoader) is only freed in the frame
// callback, never in a hook.

#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <format>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "core/detour_guard.h"
#include "core/il2cpp_api.h"
#include "core/log.h"
#include "core/main_thread.h"
#include "core/module.h"
#include "core/settings.h"
#include "core/unity.h"

namespace sr2::modules {
namespace {

namespace settings = sr2::core::settings;
namespace unity = sr2::core::unity;
namespace main_thread = sr2::core::main_thread;
using sr2::il2cpp::api;

constexpr const char* kModule = "quick_travel";
constexpr const char* kImage = "Assembly-CSharp";
constexpr const char* kSceneNs = "MonomiPark.SlimeRancher.SceneManagement";
constexpr const char* kTeleportNs = "MonomiPark.SlimeRancher.World.Teleportation";
constexpr const char* kUiNs = "MonomiPark.SlimeRancher.UI";
constexpr const char* kLoadingNs = "MonomiPark.SlimeRancher.UI.Loading";

constexpr const char* kFastRespawn = "fast_respawn";
constexpr const char* kFastTeleport = "fast_teleport";

// SceneLoader queue step names (string literals of the game, 1.3.0).
constexpr std::string_view kStepMinDeathDelay = "Wait For Min Delay On Death";
constexpr std::string_view kStepDeathOutro = "Wait for Death Loading Outro";
constexpr std::string_view kStepLoadFadeIn = "Wait For Loading Scene Fade In";

constexpr float kFastFade = 0.02f;          // DeathObscurer fade (s); > 0 so 1/fade stays finite
constexpr float kFastLoadFade = 0.0001f;    // LoadingScreenView._fadeOutTime; > 0 avoids 0/0
constexpr int RANCH_UNTIL_WORLD_TIME = 3;   // RanchHouseSleepType.UNTIL_WORLD_TIME
constexpr double kPollInterval = 0.1;       // s between house polls
constexpr int kCloseDelayFrames = 2;        // frames between wake and auto-close
constexpr double kDeathTimeout = 900.0;     // s: give up on a death session
constexpr double kLoadTimeout = 300.0;      // s: give up timing a load
constexpr double kLoadStartGrace = 5.0;     // s: load never flagged in progress -> done

// Il2CppObject header (klass + monitor); a boxed value starts right after it.
constexpr size_t kObjectHeader = 2 * sizeof(void*);

using Clock = std::chrono::steady_clock;
double SecondsSince(Clock::time_point t) { return std::chrono::duration<double>(Clock::now() - t).count(); }
double Between(Clock::time_point a, Clock::time_point b) { return std::chrono::duration<double>(b - a).count(); }

bool IsA(Il2CppObject* obj, Il2CppClass* k) {
    const auto& a = api();
    if (!obj || !k || !a.il2cpp_object_get_class) return false;
    Il2CppClass* c = a.il2cpp_object_get_class(obj);
    if (c == k) return true;
    return c && a.il2cpp_class_is_assignable_from && a.il2cpp_class_is_assignable_from(k, c);
}
bool Valid(Il2CppObject* obj, Il2CppClass* k) { return obj && IsA(obj, k) && unity::IsAlive(obj); }
bool IsNativeFault(const std::string& invokeErr) { return invokeErr.starts_with("native exception"); }
std::string NameOr(Il2CppObject* obj, const char* fallback) {
    std::string n = obj ? unity::GetName(obj) : std::string();
    return n.empty() ? std::string(fallback) : n;
}

// Who started the current call chain (set around the original call of the
// TeleportNetwork entry points; read by the SceneLoader hooks it calls into).
enum class Ctx { None, Teleport, Death };
const char* CtxName(Ctx c) { return c == Ctx::Teleport ? "teleport" : c == Ctx::Death ? "respawn" : "other"; }

class QuickTravel final : public sr2::core::Module {
public:
    const char* Name() const override { return kModule; }
    bool DefaultEnabled() const override { return true; }
    bool Setup(sr2::core::ModuleContext& ctx) override;
    void OnEnable() override { enabled_.store(true); }
    void OnDisable() override {
        enabled_.store(false);
        death_ = {};
        load_ = {};
    }
    bool Enabled() const { return enabled_.load(std::memory_order_relaxed); }

    // ---- detour logic (main thread, inside DetourGuards) ----
    void OnDeath(Il2CppObject* self, Il2CppObject* source);
    float ObscureFade(Il2CppObject* self, bool obscure, float fadeTime);
    void BeforeTeleportDeath(Il2CppObject* self, float minDelay);
    void AfterTeleportDeath();
    void BeforeTeleportImpl(Il2CppString* nodeId, Il2CppObject* group, bool force, bool boat);
    void AfterTeleportImpl();
    void OnLoadSceneGroup(Il2CppObject* loader, Il2CppObject* group);
    float DelaySeconds(Il2CppString* name, float secs);
    bool SkipDeathLoadingScene(bool load);
    void BeforeFadeToBlack(Il2CppObject* view);
    void AfterSleep(Il2CppObject* house, int type, double value);

    Ctx ctx_ = Ctx::None;  // main thread only

private:
    bool ResolveCore(sr2::core::ModuleContext& ctx, std::string& why);
    bool ResolveRespawn(sr2::core::ModuleContext& ctx, std::string& why);
    bool ResolveTeleport(sr2::core::ModuleContext& ctx, std::string& why);
    void Frame();
    void PollLoad();
    void PollDeath();
    void EndDeath(std::string_view why, bool warn);
    Il2CppObject* Loader();
    bool FastFor(Ctx c) const {
        if (c == Ctx::Death) return respawnOk_ && settings::GetBool(kModule, kFastRespawn);
        if (c == Ctx::Teleport) return teleportOk_ && settings::GetBool(kModule, kFastTeleport);
        return false;
    }
    // Loads started under a respawn context are only sped up while a knockout is tracked.
    bool FastLoad(Ctx c) const { return FastFor(c) && (c != Ctx::Death || death_.active); }

    std::atomic<bool> enabled_{false};
    bool respawnOk_ = false, teleportOk_ = false, fadeOk_ = false;
    int frameId_ = 0;

    // -- resolved by name --
    Il2CppClass* loaderClass_ = nullptr;     // SceneLoader
    size_t offLoading_ = 0;                  // <IsSceneLoadInProgress>k__BackingField
    bool hasCurrentGroup_ = false;
    size_t offCurrentGroup_ = 0;             // _currentSceneGroup
    Il2CppClass* viewClass_ = nullptr;       // LoadingScreenView
    size_t offViewFade_ = 0;                 // _fadeOutTime
    Il2CppClass* deathClass_ = nullptr;      // PlayerDeathHandler
    size_t offDeathFade_ = 0, offDeathDelay_ = 0;
    Il2CppClass* obscurerClass_ = nullptr;   // DeathObscurer
    Il2CppClass* houseClass_ = nullptr;      // RanchHouseUI
    size_t offSleeping_ = 0, offClosing_ = 0;
    bool hasTimeDir_ = false;
    size_t offTimeDir_ = 0;                  // RanchHouseUI._timeDir
    Il2CppClass* timeDirClass_ = nullptr;
    const MethodInfo* hoursUntilMi_ = nullptr;  // TimeDirector.HoursUntil(double), optional
    const MethodInfo* closeMi_ = nullptr;       // RanchHouseUI.Close()

    // -- SceneLoader handle (created in a hook, freed only in the frame callback) --
    Il2CppGCHandle loaderHandle_ = nullptr;
    std::vector<Il2CppGCHandle> staleHandles_;
    double nextLoaderFind_ = 0;
    Clock::time_point setupAt_{};

    // -- one knockout being handled (main thread only) --
    struct Death {
        bool active = false;
        Clock::time_point at{}, teleportAt{}, houseAt{}, wakeAt{};
        float fadeVanilla = 0, delayVanilla = 0;
        bool sawTeleport = false, reload = false, skippedDeathScene = false;
        int fadesShortened = 0;
        double fadeSavedS = 0, delaySavedS = 0, loadPaddingS = 0;
        bool houseOpened = false, sawSleeping = false, woke = false;
        const void* house = nullptr;  // identity only, never dereferenced later
        double skipHours = -1;
        unsigned long long wakeTick = 0;
        double nextPoll = 0;
        int misses = 0;  // consecutive polls without the house screen
    } death_;

    // -- one scene group load being timed (main thread only) --
    struct Load {
        bool active = false;
        Ctx kind = Ctx::None;
        bool fast = false;
        Clock::time_point at{};
        std::string from, to;
        bool sawInProgress = false;
        double paddingS = 0;   // zeroed waits (s)
        std::string skipped;   // what was zeroed
    } load_;
    bool loadStartedInCall_ = false;  // LoadSceneGroup seen during the current TeleportNetwork call
    std::string tpDesc_;              // teleport being handled (for the log)
};

QuickTravel* g_self = nullptr;

sr2::core::DetourGuard g_deathGuard{kModule, "PlayerDeathHandler.OnDeath"};
sr2::core::DetourGuard g_obscureGuard{kModule, "DeathObscurer.StartObscuring"};
sr2::core::DetourGuard g_tpDeathGuard{kModule, "TeleportNetwork.Teleport_Death"};
sr2::core::DetourGuard g_tpResetGuard{kModule, "TeleportNetwork.Teleport_ResetPlayer"};
sr2::core::DetourGuard g_tpImplGuard{kModule, "TeleportNetwork.TeleportToDestinationImpl"};
sr2::core::DetourGuard g_loadGuard{kModule, "SceneLoader.LoadSceneGroup"};
sr2::core::DetourGuard g_delayGuard{kModule, "SceneLoader.EnqueueDelaySeconds"};
sr2::core::DetourGuard g_deathSceneGuard{kModule, "SceneLoader.Enqueue*DeathLoadingScene"};
sr2::core::DetourGuard g_fadeBlackGuard{kModule, "LoadingScreenView.FadeToBlack"};
sr2::core::DetourGuard g_sleepGuard{kModule, "RanchHouseUI.Sleep"};
sr2::core::DetourGuard g_frameGuard{kModule, "frame"};

using OnDeathFn = void (*)(Il2CppObject*, Il2CppObject*, Il2CppObject*, Il2CppString*, const MethodInfo*);
using ObscureFn = void (*)(Il2CppObject*, bool, float, Il2CppObject*, const MethodInfo*);
using TpDeathFn = void (*)(Il2CppObject*, Il2CppObject*, float, Il2CppObject*, const MethodInfo*);
using TpResetFn = void (*)(Il2CppObject*, Il2CppObject*, Il2CppObject*, const MethodInfo*);
using TpImplFn = void (*)(Il2CppObject*, Il2CppObject*, Il2CppString*, Il2CppObject*, bool, bool, const MethodInfo*);
using LoadGroupFn = void (*)(Il2CppObject*, Il2CppObject*, Il2CppObject*, const MethodInfo*);
using DelayFn = uint64_t (*)(Il2CppObject*, Il2CppString*, float, const MethodInfo*);  // returns SceneLoaderQueueHandle (ulong)
using VoidFn = void (*)(Il2CppObject*, const MethodInfo*);
using FadeBlackFn = void* (*)(void*, Il2CppObject*, const MethodInfo*);  // Continuation (16 B) via hidden return buffer
using SleepFn = void (*)(Il2CppObject*, int32_t, double, Il2CppObject*, const MethodInfo*);

OnDeathFn g_origOnDeath = nullptr;
ObscureFn g_origObscure = nullptr;
TpDeathFn g_origTpDeath = nullptr;
TpResetFn g_origTpReset = nullptr;
TpImplFn g_origTpImpl = nullptr;
LoadGroupFn g_origLoadGroup = nullptr;
DelayFn g_origDelay = nullptr;
VoidFn g_origLoadDeathScene = nullptr;
VoidFn g_origUnloadDeathScene = nullptr;
FadeBlackFn g_origFadeBlack = nullptr;
SleepFn g_origSleep = nullptr;

bool HookReady(Il2CppObject* self) { return g_self && self && g_self->Enabled() && main_thread::IsMainThread(); }

// Sets QuickTravel::ctx_ for the duration of an original call (restored on unwind).
struct CtxScope {
    Ctx saved;
    bool on;
    CtxScope(bool enable, Ctx c) : saved(g_self ? g_self->ctx_ : Ctx::None), on(enable && g_self) {
        if (on) g_self->ctx_ = c;
    }
    ~CtxScope() {
        if (on) g_self->ctx_ = saved;
    }
    CtxScope(const CtxScope&) = delete;
    CtxScope& operator=(const CtxScope&) = delete;
};

void OnDeathDetour(Il2CppObject* self, Il2CppObject* source, Il2CppObject* go, Il2CppString* st, const MethodInfo* mi) {
    if (HookReady(self)) g_deathGuard.Run([&] { g_self->OnDeath(self, source); });
    g_origOnDeath(self, source, go, st, mi);
}

void ObscureDetour(Il2CppObject* self, bool obscure, float fadeTime, Il2CppObject* onComplete, const MethodInfo* mi) {
    float t = fadeTime;
    if (HookReady(self)) g_obscureGuard.Run([&] { t = g_self->ObscureFade(self, obscure, fadeTime); });
    g_origObscure(self, obscure, t, onComplete, mi);
}

void TpDeathDetour(Il2CppObject* self, Il2CppObject* player, float minDelay, Il2CppObject* onBefore,
                   const MethodInfo* mi) {
    const bool ready = HookReady(self);
    if (ready) g_tpDeathGuard.Run([&] { g_self->BeforeTeleportDeath(self, minDelay); });
    {
        CtxScope scope(ready, Ctx::Death);
        g_origTpDeath(self, player, minDelay, onBefore, mi);
    }
    if (ready) g_tpDeathGuard.Run([&] { g_self->AfterTeleportDeath(); });
}

void TpResetDetour(Il2CppObject* self, Il2CppObject* player, Il2CppObject* onComplete, const MethodInfo* mi) {
    CtxScope scope(HookReady(self), Ctx::Death);
    g_origTpReset(self, player, onComplete, mi);
}

void TpImplDetour(Il2CppObject* self, Il2CppObject* player, Il2CppString* nodeId, Il2CppObject* group, bool force,
                  bool boat, const MethodInfo* mi) {
    const bool ready = HookReady(self);
    if (ready) g_tpImplGuard.Run([&] { g_self->BeforeTeleportImpl(nodeId, group, force, boat); });
    {
        CtxScope scope(ready, Ctx::Teleport);
        g_origTpImpl(self, player, nodeId, group, force, boat, mi);
    }
    if (ready) g_tpImplGuard.Run([&] { g_self->AfterTeleportImpl(); });
}

void LoadGroupDetour(Il2CppObject* self, Il2CppObject* group, Il2CppObject* params, const MethodInfo* mi) {
    if (HookReady(self)) g_loadGuard.Run([&] { g_self->OnLoadSceneGroup(self, group); });
    g_origLoadGroup(self, group, params, mi);
}

uint64_t DelayDetour(Il2CppObject* self, Il2CppString* name, float secs, const MethodInfo* mi) {
    float s = secs;
    if (HookReady(self)) g_delayGuard.Run([&] { s = g_self->DelaySeconds(name, secs); });
    return g_origDelay(self, name, s, mi);
}

void LoadDeathSceneDetour(Il2CppObject* self, const MethodInfo* mi) {
    bool skip = false;
    if (HookReady(self)) g_deathSceneGuard.Run([&] { skip = g_self->SkipDeathLoadingScene(true); });
    if (!skip) g_origLoadDeathScene(self, mi);
}

void UnloadDeathSceneDetour(Il2CppObject* self, const MethodInfo* mi) {
    bool skip = false;
    if (HookReady(self)) g_deathSceneGuard.Run([&] { skip = g_self->SkipDeathLoadingScene(false); });
    if (!skip) g_origUnloadDeathScene(self, mi);
}

void* FadeBlackDetour(void* ret, Il2CppObject* self, const MethodInfo* mi) {
    if (HookReady(self)) g_fadeBlackGuard.Run([&] { g_self->BeforeFadeToBlack(self); });
    return g_origFadeBlack(ret, self, mi);
}

void SleepDetour(Il2CppObject* self, int32_t type, double value, Il2CppObject* cb, const MethodInfo* mi) {
    g_origSleep(self, type, value, cb, mi);
    if (HookReady(self)) g_sleepGuard.Run([&] { g_self->AfterSleep(self, type, value); });
}

// ------------------------------------------------------------------ setup --

struct Need {
    sr2::il2cpp::Resolver& R;
    std::string& why;
    Il2CppClass* Class(const char* ns, const char* name) {
        std::string err;
        Il2CppClass* k = R.Class(kImage, ns, name, &err);
        if (!k && why.empty()) why = err.empty() ? std::format("class {}.{} not found", ns, name) : err;
        return k;
    }
    bool Offset(Il2CppClass* k, const char* field, size_t& out) {
        if (!k) return false;
        std::string err;
        auto o = R.FieldOffset(k, field, &err);
        if (!o) {
            if (why.empty()) why = err.empty() ? std::format("field {} not found", field) : err;
            return false;
        }
        out = *o;
        return true;
    }
    const MethodInfo* Method(Il2CppClass* k, const char* name, int argc) {
        if (!k) return nullptr;
        std::string err;
        const MethodInfo* m = R.Method(k, name, argc, &err);
        if (!m && why.empty()) why = err.empty() ? std::format("method {} not found", name) : err;
        return m;
    }
    void* Pointer(Il2CppClass* k, const char* name, int argc) {
        const MethodInfo* m = Method(k, name, argc);
        if (!m) return nullptr;
        std::string err;
        void* p = R.MethodPointer(m, &err);
        if (!p && why.empty()) why = err.empty() ? std::format("method {} has no code", name) : err;
        return p;
    }
};

// Resolved method pointers waiting to be hooked once the whole group resolved.
struct PendingHook {
    const char* name;
    void* target;
    void* detour;
    void** original;
};

bool CreateHooks(sr2::core::ModuleContext& ctx, const std::vector<PendingHook>& hooks) {
    for (const auto& h : hooks)
        if (!ctx.HookAddress(h.name, h.target, h.detour, h.original)) return false;
    return true;
}

template <class F>
void* Detour(F f) {
    return reinterpret_cast<void*>(f);
}
template <class F>
void** Orig(F* f) {
    return reinterpret_cast<void**>(f);
}

bool QuickTravel::ResolveCore(sr2::core::ModuleContext& ctx, std::string& why) {
    Need n{ctx.resolver(), why};
    loaderClass_ = n.Class(kSceneNs, "SceneLoader");
    if (!n.Offset(loaderClass_, "<IsSceneLoadInProgress>k__BackingField", offLoading_)) return false;
    void* loadGroup = n.Pointer(loaderClass_, "LoadSceneGroup", 2);
    void* delay = n.Pointer(loaderClass_, "EnqueueDelaySeconds", 2);
    if (!loadGroup || !delay) return false;
    std::vector<PendingHook> hooks = {
        {"SceneLoader.LoadSceneGroup", loadGroup, Detour(&LoadGroupDetour), Orig(&g_origLoadGroup)},
        {"SceneLoader.EnqueueDelaySeconds", delay, Detour(&DelayDetour), Orig(&g_origDelay)},
    };

    std::string optWhy;
    Need opt{ctx.resolver(), optWhy};
    hasCurrentGroup_ = opt.Offset(loaderClass_, "_currentSceneGroup", offCurrentGroup_);
    viewClass_ = opt.Class(kLoadingNs, "LoadingScreenView");
    void* fadeBlack = nullptr;
    if (opt.Offset(viewClass_, "_fadeOutTime", offViewFade_)) fadeBlack = opt.Pointer(viewClass_, "FadeToBlack", 0);
    if (fadeBlack) {
        hooks.push_back({"LoadingScreenView.FadeToBlack", fadeBlack, Detour(&FadeBlackDetour), Orig(&g_origFadeBlack)});
        fadeOk_ = true;
    }
    if (!optWhy.empty())
        SR2_WARN("[{}] optional: {} (scene-group names in the log / loading-screen fade shortening may be missing)",
                 kModule, optWhy);
    if (!CreateHooks(ctx, hooks)) {
        why = ctx.failure();
        return false;
    }
    return true;
}

bool QuickTravel::ResolveRespawn(sr2::core::ModuleContext& ctx, std::string& why) {
    Need n{ctx.resolver(), why};
    deathClass_ = n.Class("", "PlayerDeathHandler");
    if (!n.Offset(deathClass_, "_fadeTime", offDeathFade_) || !n.Offset(deathClass_, "_delayTime", offDeathDelay_))
        return false;
    void* onDeath = n.Pointer(deathClass_, "OnDeath", 3);
    obscurerClass_ = n.Class("", "DeathObscurer");
    void* obscure = n.Pointer(obscurerClass_, "StartObscuring", 3);
    Il2CppClass* net = n.Class(kTeleportNs, "TeleportNetwork");
    void* tpDeath = n.Pointer(net, "Teleport_Death", 3);
    void* tpReset = n.Pointer(net, "Teleport_ResetPlayer", 2);
    void* loadDeath = n.Pointer(loaderClass_, "EnqueueLoadDeathLoadingScene", 0);
    void* unloadDeath = n.Pointer(loaderClass_, "EnqueueUnloadDeathLoadingScene", 0);
    houseClass_ = n.Class(kUiNs, "RanchHouseUI");
    if (!n.Offset(houseClass_, "_sleeping", offSleeping_) || !n.Offset(houseClass_, "_isClosing", offClosing_))
        return false;
    closeMi_ = n.Method(houseClass_, "Close", 0);
    void* sleep = n.Pointer(houseClass_, "Sleep", 3);
    if (!onDeath || !obscure || !tpDeath || !tpReset || !loadDeath || !unloadDeath || !closeMi_ || !sleep) return false;

    std::string optWhy;
    Need opt{ctx.resolver(), optWhy};
    timeDirClass_ = opt.Class("", "TimeDirector");
    hoursUntilMi_ = opt.Method(timeDirClass_, "HoursUntil", 1);
    hasTimeDir_ = opt.Offset(houseClass_, "_timeDir", offTimeDir_) && hoursUntilMi_;
    if (!optWhy.empty()) SR2_WARN("[{}] optional: {} (time-skip hours not logged)", kModule, optWhy);

    std::vector<PendingHook> hooks = {
        {"PlayerDeathHandler.OnDeath", onDeath, Detour(&OnDeathDetour), Orig(&g_origOnDeath)},
        {"DeathObscurer.StartObscuring", obscure, Detour(&ObscureDetour), Orig(&g_origObscure)},
        {"TeleportNetwork.Teleport_Death", tpDeath, Detour(&TpDeathDetour), Orig(&g_origTpDeath)},
        {"TeleportNetwork.Teleport_ResetPlayer", tpReset, Detour(&TpResetDetour), Orig(&g_origTpReset)},
        {"SceneLoader.EnqueueLoadDeathLoadingScene", loadDeath, Detour(&LoadDeathSceneDetour),
         Orig(&g_origLoadDeathScene)},
        {"SceneLoader.EnqueueUnloadDeathLoadingScene", unloadDeath, Detour(&UnloadDeathSceneDetour),
         Orig(&g_origUnloadDeathScene)},
        {"RanchHouseUI.Sleep", sleep, Detour(&SleepDetour), Orig(&g_origSleep)},
    };
    if (!CreateHooks(ctx, hooks)) {
        why = ctx.failure();
        return false;
    }
    return true;
}

bool QuickTravel::ResolveTeleport(sr2::core::ModuleContext& ctx, std::string& why) {
    Need n{ctx.resolver(), why};
    Il2CppClass* net = n.Class(kTeleportNs, "TeleportNetwork");
    void* impl = n.Pointer(net, "TeleportToDestinationImpl", 5);
    if (!impl) return false;
    if (!ctx.HookAddress("TeleportNetwork.TeleportToDestinationImpl", impl, Detour(&TpImplDetour), Orig(&g_origTpImpl))) {
        why = ctx.failure();
        return false;
    }
    return true;
}

bool QuickTravel::Setup(sr2::core::ModuleContext& ctx) {
    setupAt_ = Clock::now();
    std::string coreWhy;
    if (!ResolveCore(ctx, coreWhy)) return ctx.Fail("SceneLoader hooks not resolved: " + coreWhy);

    std::string respWhy, tpWhy;
    respawnOk_ = ResolveRespawn(ctx, respWhy);
    if (!respawnOk_ && !ctx.failure().empty()) return false;  // a hook could not be created: module fails
    teleportOk_ = ResolveTeleport(ctx, tpWhy);
    if (!teleportOk_ && !ctx.failure().empty()) return false;
    if (!respawnOk_) SR2_WARN("[{}] fast_respawn unavailable: {}", kModule, respWhy);
    if (!teleportOk_) SR2_WARN("[{}] fast_teleport unavailable: {}", kModule, tpWhy);
    if (!respawnOk_ && !teleportOk_) return ctx.Fail("neither feature resolved: " + respWhy + " / " + tpWhy);

    auto reg = [](const char* key, const char* label, const char* tip) {
        settings::Def d;
        d.module = kModule;
        d.key = key;
        d.label = label;
        d.kind = settings::Kind::Bool;
        d.def = 1.0;
        d.min = 0;
        d.max = 1;
        d.step = 1;
        d.tooltip = tip;
        settings::Register(d);
    };
    if (respawnOk_)
        reg(kFastRespawn, "Fast respawn",
            "After a knockout: no fade to black / fade in, no minimum wait and no death loading screen when you are "
            "in the ranch zone; elsewhere the ranch zone is loaded without loading-screen padding. The time skip, "
            "item loss and the save still happen (the house screen runs the time skip), then the house screen "
            "closes by itself.");
    if (teleportOk_)
        reg(kFastTeleport, "Fast teleport loads",
            "Teleporters into another zone: the loading screen's 1 s fade-in wait and its fade to black are skipped. "
            "The zone load itself is not changed. Teleports inside the same zone are already instant.");

    frameId_ = main_thread::AddFrameCallback(kModule, [this](float) { g_frameGuard.Run([this] { Frame(); }); });
    if (frameId_ == 0) return ctx.Fail("main_thread::AddFrameCallback failed");

    g_self = this;
    SR2_INFO("[{}] setup ok: fast_respawn {}, fast_teleport {}, loading-screen fade {}", kModule,
             respawnOk_ ? "available" : "UNAVAILABLE", teleportOk_ ? "available" : "UNAVAILABLE",
             fadeOk_ ? "hooked" : "not hooked");
    return true;
}

// ------------------------------------------------------------ fast_respawn --

void QuickTravel::OnDeath(Il2CppObject* self, Il2CppObject* source) {
    if (!respawnOk_ || !Valid(self, deathClass_)) return;
    if (death_.active) {
        // The game ignores OnDeath while the player is dead. A new knockout after the
        // tracked one woke up (or never reached the house screen) starts over.
        const bool stale = death_.woke || (!death_.houseOpened && SecondsSince(death_.at) > 60.0);
        if (!stale) return;
        SR2_INFO("[{}] respawn: previous knockout no longer tracked (new knockout)", kModule);
        death_ = {};
    }
    if (!settings::GetBool(kModule, kFastRespawn)) return;
    death_ = {};
    death_.active = true;
    death_.at = Clock::now();
    death_.fadeVanilla = unity::Field<float>(self, offDeathFade_);
    death_.delayVanilla = unity::Field<float>(self, offDeathDelay_);
    SR2_INFO("[{}] knockout ({}): fast respawn - vanilla fade {:.2f} s out + {:.2f} s in, minimum delay {:.2f} s; "
             "penalties (items, currency share, health/energy reset, time skip, save) are left to the game",
             kModule, NameOr(source, "unknown source"), death_.fadeVanilla, death_.fadeVanilla, death_.delayVanilla);
}

float QuickTravel::ObscureFade(Il2CppObject* self, bool obscure, float fadeTime) {
    if (!death_.active || !settings::GetBool(kModule, kFastRespawn) || !Valid(self, obscurerClass_)) return fadeTime;
    if (!(fadeTime > kFastFade)) return fadeTime;
    death_.fadesShortened++;
    if (obscure) death_.fadeSavedS += fadeTime - kFastFade;  // the fade-in runs behind the house screen
    SR2_DEBUG("[{}] death screen fade-{} {:.2f} s -> {:.2f} s", kModule, obscure ? "out" : "in", fadeTime, kFastFade);
    return kFastFade;
}

void QuickTravel::BeforeTeleportDeath(Il2CppObject* self, float minDelay) {
    (void)self;
    loadStartedInCall_ = false;
    if (!death_.active) return;
    death_.sawTeleport = true;
    death_.teleportAt = Clock::now();
    if (minDelay != death_.delayVanilla) death_.delayVanilla = minDelay;
}

void QuickTravel::AfterTeleportDeath() {
    if (!death_.active) return;
    death_.reload = loadStartedInCall_;
    SR2_INFO("[{}] respawn: screen black after {:.2f} s; {}", kModule, Between(death_.at, death_.teleportAt),
             death_.reload ? "not in the ranch zone -> the game loads the ranch zone (real load, padding removed)"
                           : std::format("in the ranch zone -> teleport to the wake-up point{}",
                                         death_.skippedDeathScene ? " without the death loading screen" : ""));
    loadStartedInCall_ = false;
}

bool QuickTravel::SkipDeathLoadingScene(bool load) {
    // Both calls come from the same Teleport_Death call (in-scene branch), so they
    // are skipped as a pair: never an unload without the load.
    if (ctx_ != Ctx::Death || !FastLoad(Ctx::Death)) return false;
    if (load) {
        death_.skippedDeathScene = true;
        return true;
    }
    return death_.skippedDeathScene;
}

void QuickTravel::AfterSleep(Il2CppObject* house, int type, double value) {
    if (!death_.active || type != RANCH_UNTIL_WORLD_TIME || !Valid(house, houseClass_)) return;
    death_.houseOpened = true;
    death_.house = house;
    death_.houseAt = Clock::now();
    death_.sawSleeping = unity::Field<bool>(house, offSleeping_);
    if (hasTimeDir_) {
        auto* td = unity::Field<Il2CppObject*>(house, offTimeDir_);
        if (Valid(td, timeDirClass_)) {
            double v = value;
            void* args[1] = {&v};
            std::string err;
            Il2CppObject* boxed = sr2::il2cpp::Resolver::Get().Invoke(hoursUntilMi_, td, args, &err);
            if (!err.empty()) {
                if (IsNativeFault(err)) sr2::core::MarkModuleFaulted(kModule, "TimeDirector.HoursUntil: " + err);
            } else if (boxed && unity::ClassName(boxed) == "System.Double") {
                death_.skipHours = unity::Field<double>(boxed, kObjectHeader);
            }
        }
    }
    SR2_INFO("[{}] respawn: house screen open {:.2f} s after the knockout; time skip {} (kept - the game "
             "fast-forwards it, then saves)",
             kModule, Between(death_.at, death_.houseAt),
             death_.skipHours >= 0 ? std::format("{:.2f} game hours", death_.skipHours) : std::string("running"));
}

void QuickTravel::EndDeath(std::string_view why, bool warn) {
    if (warn)
        SR2_WARN("[{}] respawn: {}", kModule, why);
    else if (!why.empty())
        SR2_INFO("[{}] respawn: {}", kModule, why);
    death_ = {};
}

void QuickTravel::PollDeath() {
    if (!death_.active) return;
    const double since = SecondsSince(death_.at);
    if (since > kDeathTimeout) {
        EndDeath(std::format("still not awake after {:.0f} s - stopped tracking (nothing else changed)", kDeathTimeout),
                 true);
        return;
    }
    if (!death_.houseOpened) return;
    const double clock = SecondsSince(setupAt_);
    if (!death_.woke && clock < death_.nextPoll) return;
    death_.nextPoll = clock + kPollInterval;

    Il2CppObject* house = unity::FindObjectOfType(houseClass_);
    if (!house || house != death_.house || !Valid(house, houseClass_)) {
        // Tolerate a few misses (inactive for a moment); then it was closed some other way.
        if (++death_.misses >= (death_.woke ? 3 : 20))
            EndDeath("house screen closed before the module closed it", false);
        return;
    }
    death_.misses = 0;
    if (unity::Field<bool>(house, offSleeping_)) {
        death_.sawSleeping = true;
        return;
    }
    if (!death_.sawSleeping) return;
    if (!death_.woke) {
        death_.woke = true;
        death_.wakeAt = Clock::now();
        death_.wakeTick = main_thread::TickCount();
        SR2_INFO("[{}] respawn: time skip done after {:.2f} s of fast-forward", kModule,
                 Between(death_.houseAt, death_.wakeAt));
        return;
    }
    if (main_thread::TickCount() < death_.wakeTick + kCloseDelayFrames) return;
    if (unity::Field<bool>(house, offClosing_)) {
        EndDeath("house screen closed by the player", false);
        return;
    }
    if (!settings::GetBool(kModule, kFastRespawn)) {
        EndDeath("fast_respawn switched off - house screen left open", false);
        return;
    }
    std::string err;
    sr2::il2cpp::Resolver::Get().Invoke(closeMi_, house, nullptr, &err);
    if (!err.empty()) {
        if (IsNativeFault(err)) sr2::core::MarkModuleFaulted(kModule, "RanchHouseUI.Close: " + err);
        EndDeath(std::format("closing the house screen failed ({}) - press Esc", err), true);
        return;
    }
    const double total = SecondsSince(death_.at);
    const double ff = Between(death_.houseAt, death_.wakeAt);
    const double saved = death_.fadeSavedS + death_.delaySavedS + death_.loadPaddingS +
                         (death_.skippedDeathScene ? 1.0 : 0.0);
    SR2_INFO("[{}] respawn done: player back in control {:.2f} s after the knockout ({:.2f} s of it the time "
             "skip, {:.2f} s before the house screen). Skipped: fade-out {:.2f} s, minimum delay {:.2f} s, {}{} "
             "fade-in {:.2f} s (was hidden behind the house screen), waiting for Esc. Saved >= {:.2f} s + your Esc "
             "press.",
             kModule, total, ff, Between(death_.at, death_.houseAt), death_.fadeSavedS, death_.delaySavedS,
             death_.skippedDeathScene ? "death loading screen (load + 1 s outro + unload), " : "",
             death_.reload ? std::format("ranch-zone load padding {:.2f} s, ", death_.loadPaddingS) : std::string(),
             death_.fadeVanilla, saved);
    death_ = {};
}

// ----------------------------------------------------------- fast_teleport --

void QuickTravel::BeforeTeleportImpl(Il2CppString* nodeId, Il2CppObject* group, bool force, bool boat) {
    loadStartedInCall_ = false;
    tpDesc_ = std::format("-> {} ({}{}{})", unity::ToUtf8(nodeId), NameOr(group, "?"), force ? ", forced load" : "",
                          boat ? ", boat" : "");
}

void QuickTravel::AfterTeleportImpl() {
    if (!loadStartedInCall_)
        SR2_INFO("[{}] teleport {}: same zone - the game teleports instantly (no fade or wait to skip)", kModule,
                 tpDesc_);
    loadStartedInCall_ = false;
}

// ---------------------------------------------------- shared: scene loads --

Il2CppObject* QuickTravel::Loader() {
    if (loaderHandle_) {
        Il2CppObject* o = unity::GCHandleTarget(loaderHandle_);
        if (Valid(o, loaderClass_)) return o;
        staleHandles_.push_back(loaderHandle_);
        loaderHandle_ = nullptr;
    }
    const double clock = SecondsSince(setupAt_);
    if (clock < nextLoaderFind_) return nullptr;
    nextLoaderFind_ = clock + kPollInterval;
    Il2CppObject* o = unity::FindObjectOfType(loaderClass_);
    return Valid(o, loaderClass_) ? o : nullptr;
}

void QuickTravel::OnLoadSceneGroup(Il2CppObject* loader, Il2CppObject* group) {
    if (!Valid(loader, loaderClass_)) return;
    if (!loaderHandle_ || unity::GCHandleTarget(loaderHandle_) != loader) {
        if (loaderHandle_) staleHandles_.push_back(loaderHandle_);  // freed in the frame callback
        loaderHandle_ = unity::NewGCHandle(loader);
    }
    if (load_.active)
        SR2_INFO("[{}] scene load {} -> {} replaced by a new load after {:.2f} s", kModule, load_.from, load_.to,
                 SecondsSince(load_.at));
    load_ = {};
    load_.active = true;
    load_.kind = ctx_;
    load_.fast = FastLoad(ctx_);
    load_.at = Clock::now();
    load_.from = hasCurrentGroup_ ? NameOr(unity::Field<Il2CppObject*>(loader, offCurrentGroup_), "?") : "?";
    load_.to = NameOr(group, "?");
    load_.sawInProgress = unity::Field<bool>(loader, offLoading_);
    loadStartedInCall_ = ctx_ != Ctx::None;
    if (ctx_ == Ctx::Teleport)
        SR2_INFO("[{}] teleport {}: other zone - scene load {} -> {}{}", kModule, tpDesc_, load_.from, load_.to,
                 load_.fast ? " (loading-screen padding removed)" : "");
}

float QuickTravel::DelaySeconds(Il2CppString* name, float secs) {
    if (ctx_ == Ctx::None || !(secs > 0.0f)) return secs;
    const std::string n = unity::ToUtf8(name);
    if (ctx_ == Ctx::Death && FastLoad(Ctx::Death)) {
        if (n == kStepMinDeathDelay) {
            death_.delaySavedS += secs;
            return 0.0f;
        }
        if (n == kStepDeathOutro) {  // only reached if the death loading screen was not skipped
            death_.delaySavedS += secs;
            return 0.0f;
        }
    }
    if (n == kStepLoadFadeIn && FastLoad(ctx_)) {
        if (load_.active) {
            load_.paddingS += secs;
            load_.skipped += std::format("{}fade-in wait {:.2f} s", load_.skipped.empty() ? "" : ", ", secs);
        }
        if (ctx_ == Ctx::Death && death_.active) death_.loadPaddingS += secs;
        return 0.0f;
    }
    return secs;
}

void QuickTravel::BeforeFadeToBlack(Il2CppObject* view) {
    if (!fadeOk_ || !load_.active || !load_.fast || !Valid(view, viewClass_)) return;
    float& f = unity::Field<float>(view, offViewFade_);
    const float vanilla = f;
    if (!(vanilla > kFastLoadFade)) return;
    f = kFastLoadFade;  // FadeToBlack: alpha += dt / _fadeOutTime (clamped to 1) -> done in one frame
    load_.paddingS += vanilla;
    load_.skipped += std::format("{}fade to black {:.2f} s", load_.skipped.empty() ? "" : ", ", vanilla);
    if (load_.kind == Ctx::Death && death_.active) death_.loadPaddingS += vanilla;
}

void QuickTravel::PollLoad() {
    if (!load_.active) return;
    const double took = SecondsSince(load_.at);
    if (took > kLoadTimeout) {
        SR2_WARN("[{}] scene load {} -> {} still running after {:.0f} s - stopped timing it", kModule, load_.from,
                 load_.to, kLoadTimeout);
        load_ = {};
        return;
    }
    Il2CppObject* loader = Loader();
    if (!loader) return;
    const bool inProgress = unity::Field<bool>(loader, offLoading_);
    if (inProgress) {
        load_.sawInProgress = true;
        return;
    }
    if (!load_.sawInProgress && took < kLoadStartGrace) return;
    if (load_.fast) {
        SR2_INFO("[{}] {} load {} -> {}: {:.2f} s (real loading). Skipped padding: {} -> vanilla would take about "
                 "{:.2f} s",
                 kModule, CtxName(load_.kind), load_.from, load_.to, took,
                 load_.skipped.empty() ? std::string("none found") : load_.skipped, took + load_.paddingS);
    } else {
        SR2_INFO("[{}] {} load {} -> {}: {:.2f} s (unchanged; includes the game's loading-screen waits)", kModule,
                 CtxName(load_.kind), load_.from, load_.to, took);
    }
    load_ = {};
}

// ----------------------------------------------------------------- per frame --

void QuickTravel::Frame() {
    for (Il2CppGCHandle h : staleHandles_) unity::FreeGCHandle(h);
    staleHandles_.clear();
    if (!Enabled()) return;
    PollLoad();
    if (respawnOk_) PollDeath();
}

}  // namespace

std::unique_ptr<sr2::core::Module> CreateQuickTravel() { return std::make_unique<QuickTravel>(); }

}  // namespace sr2::modules
