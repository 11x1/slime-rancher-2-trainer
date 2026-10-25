// quick_start: shorter way from launch to playing.
//
// Boot flow of SR2 1.3.0 (from the dump + disassembly, see NOTES.md):
//   level0 (bootstrap)  -> SceneLoader loads scene group "CompanyLogo"         (~0.75 s, Player.log)
//   CompanyLogoScene.StartUpCoroutine: Monomi Park logo fade in / hold (blink) /
//     fade out, black hold, warning screen (fade in, hold until timeout or a key
//     press -> _skipWarningScreen), fade out, black hold, game logo background +
//     game logo fade in. Every wait is a WaitForSeconds(<serialized float>) or a
//     Time.time loop - pure presentation, nothing loads inside it.
//   -> LoadSceneGroup(PlatformInfoProvider.standaloneEngagementPrompt) = "StandaloneStart"
//     (~0.9 s) with LoadStep CompanyLogoScene.OnMainMenuLoaded -> it finds the
//     PlatformEngagementPrompt ("press any key") and calls Onloaded() (registers
//     the submit action). The game now waits for the player.
//   key press -> StandaloneEngagementPrompt.OnInteract: establish platform user,
//     InitializePlatformForCurrentUser, Animate(AnimationDuration), then
//     LoadSceneGroup(SceneGroupToLoad) = "MainMenuFromBoot" (~9.7 s, real asset
//     loading: ~88k objects) -> OnSceneGroupLoad -> CompanyLogoScene.AnimateOut.
//   Main menu "Continue" = ContinueGameBehaviorModel (: LoadGameBehaviorModel)
//     .InvokeBehavior() -> AutoSaveDirector.BeginLoad(GetSaveToContinue()).
//
// What this module does (all by name, no RVAs):
//   skip_intro (default on)
//     * CompanyLogoScene.Start (pre-hook, plus a FindObjectOfType fallback when
//       the hook came too late): zero the 11 serialized timing floats and set
//       _skipWarningScreen. The game's own coroutine still runs every step in
//       order (music, bootstrap camera, scene group load); each wait just ends
//       after one frame.
//     * PlatformEngagementPrompt.Onloaded (post-hook): once the prompt is active
//       and no scene load is in progress, do exactly what a key press does -
//       call StandaloneEngagementPrompt.OnInteract (the context argument is not
//       read by it). AnimationDuration / EaMessageFadeTime are zeroed first.
//       StandaloneEngagementPrompt.OnInteract is hooked (pass-through) only to
//       see whether the player already pressed a key, so it never runs twice.
//     * Nothing that loads is skipped: both scene group loads, the user/platform
//       init and the save system init run unchanged.
//   auto_continue (default off), auto_continue_delay
//     * MainMenuLandingRootUI.OnEnable (post-hook owned by ui/modded_label.cpp, which
//       calls our listener) marks "menu seen". A frame
//       callback waits until SystemContext.SceneLoader.IsSceneLoadInProgress is
//       false and (for at most kMenuSoftWait more) the landing UI is active and the
//       logo overlay and the prompt are gone, waits auto_continue_delay, then calls
//       InvokeBehavior() on the menu's ContinueGameBehaviorModel (what the Continue
//       button does) if ButtonBehaviorConditionUtility.HasContinueGame() and the
//       model has a save summary. Once per process; holding Shift while the game
//       window is in the foreground and the mod menu is closed, at any point before
//       it fires, cancels it for this launch, and so does a scene load the player
//       starts from the menu.
//   skip_new_game_intro (default on)
//     * New game: AutoSaveDirector.StartNewGame's load step calls
//       CutsceneDirector.SpawnIntroSequence, which pushes IntroSequenceUIRoot (the
//       story text frames before control). Its StartUpCoroutine queues the intro
//       music, waits _fadeInWaitTime, fades the black screen out over
//       _fadeInTime and calls StartFrame(0). After the last frame the game runs
//       the same code as EndSequence(): input off, music dequeued, EndCoroutine
//       (fade to black, wait, Close). Closing destroys the UI and its OnDestroy
//       applies the intro's state: SFX unpaused, cutscene camera destroyed,
//       _cameraEnabled = true, MetaGameDirector.StartMainQuest(true),
//       FadeIntoGameplay (tutorials suppressed + LockOnDeath.Freeze until the
//       DeathObscurer fade ends, then Unfreeze + UnsuppressTutorials) and
//       CutsceneDirector.ClearIntroSequenceUIRoot.
//     * IntroSequenceUIRoot.Start (pre-hook): fade-in wait 0, fade-in 0.1 s,
//       fade-out <= 0.5 s, fade-out wait 0.
//     * IntroSequenceUIRoot.StartFrame (replace, first call per instance): calls
//       the game's own EndSequence() instead of showing frame 0, so the end path
//       above runs unchanged. If that call fails, the original StartFrame runs
//       (vanilla intro).
//
//   The [modded] title-screen label is not part of this module: ui/modded_label.cpp
//   (always on). It owns the MainMenuLandingRootUI.OnEnable hook; we register a listener.
//
// Safety: objects are validated (class check, IsAlive, activeInHierarchy) before
// use. All mod logic in hooks and in the frame callback runs inside a
// DetourGuard: a native fault marks the module faulted for the session (hooks
// disabled, detours pass straight through); runtime_invoke native faults do the
// same. Managed objects are re-found with FindObjectOfType when needed; cached
// Il2CppObject* values are only compared, never dereferenced, except the main
// menu, held by a GC handle and checked with IsAlive before every use.

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <format>
#include <memory>
#include <string>
#include <string_view>

#include "core/detour_guard.h"
#include "core/il2cpp_api.h"
#include "core/log.h"
#include "core/main_thread.h"
#include "core/module.h"
#include "core/settings.h"
#include "core/unity.h"
#include "ui/modded_label.h"
#include "ui/overlay.h"
#include "ui/quick_start_hint.h"

namespace sr2::modules {
namespace {

namespace settings = sr2::core::settings;
namespace unity = sr2::core::unity;
namespace main_thread = sr2::core::main_thread;
using sr2::il2cpp::api;

constexpr const char* kModule = "quick_start";
constexpr const char* kImage = "Assembly-CSharp";
constexpr const char* kUiNs = "MonomiPark.SlimeRancher.UI";
constexpr const char* kMenuNs = "MonomiPark.SlimeRancher.UI.MainMenu";
constexpr const char* kMenuModelNs = "MonomiPark.SlimeRancher.UI.MainMenu.Model";
constexpr const char* kButtonNs = "MonomiPark.SlimeRancher.UI.ButtonBehavior";
constexpr const char* kSceneNs = "MonomiPark.SlimeRancher.SceneManagement";
constexpr const char* kIntroNs = "MonomiPark.SlimeRancher.UI.IntroSequence";

// Setting keys.
constexpr const char* kSkipIntro = "skip_intro";
constexpr const char* kAutoContinue = "auto_continue";
constexpr const char* kAutoContinueDelay = "auto_continue_delay";
constexpr const char* kSkipNewGameIntro = "skip_new_game_intro";

// IntroSequenceUIRoot fades while skipping (seconds). Waits go to 0; the fades
// keep a short non-zero length (tween / particle durations, DeathObscurer).
constexpr float kIntroFadeIn = 0.1f;
constexpr float kIntroFadeOutMax = 0.5f;

// Il2CppObject header (klass + monitor): a boxed value starts right after it.
// Runtime ABI constant, same footing as unity::kArrayDataOffset.
constexpr size_t kObjectHeader = 2 * sizeof(void*);

constexpr double kLogoPollWindow = 60.0;     // s after setup: look for an unpatched logo scene
constexpr double kLogoPollInterval = 0.1;    // s between FindObjectOfType polls
constexpr int kPromptMinFrames = 2;          // frames between Onloaded and our "key press"
constexpr double kPromptGiveUp = 20.0;       // s: prompt never became ready -> leave it to the player
constexpr double kMenuPollInterval = 0.1;    // s between main-menu readiness checks
constexpr double kMenuGiveUp = 120.0;        // s after "menu seen" without ever being ready
constexpr double kMenuSoftWait = 3.0;        // s the soft readiness checks may hold it up (see MenuState)
constexpr double kMenuIdleMin = 0.3;         // s without a scene load before a new load counts as the player's
constexpr double kMenuGoneWait = 2.0;        // s the menu may be gone (no load running) before we stop
constexpr int kMaxWaitLogs = 8;              // "waiting for the main menu" lines per launch

// CompanyLogoScene timing floats, with how often StartUpCoroutine waits for
// each (logoBackgroundHoldTime twice; blink happens inside the hold; the
// background fade-out runs concurrently and in AnimateOut).
struct LogoField {
    const char* name;
    int waits;
};
constexpr LogoField kLogoFields[] = {
    {"logoFadeInTime", 1},       {"logoHoldTime", 1},          {"logoBlinkTime", 0},
    {"logoFadeOutTime", 1},      {"logoBackgroundHoldTime", 2}, {"logoBackgroundFadeOutTime", 0},
    {"warningFadeInTime", 1},    {"warningHoldTime", 1},       {"warningFadeOutTime", 1},
    {"splashFadeInTime", 1},     {"splashFadeInWaitTime", 1},
};
constexpr size_t kLogoFieldCount = sizeof(kLogoFields) / sizeof(kLogoFields[0]);

using Clock = std::chrono::steady_clock;
double SecondsSince(Clock::time_point t) { return std::chrono::duration<double>(Clock::now() - t).count(); }

// Seconds since this process was created (for "T+x s" log lines); -1 if unknown.
double SecondsSinceProcessStart() {
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) return -1.0;
    FILETIME now{};
    GetSystemTimePreciseAsFileTime(&now);
    ULARGE_INTEGER c{}, n{};
    c.LowPart = created.dwLowDateTime;
    c.HighPart = created.dwHighDateTime;
    n.LowPart = now.dwLowDateTime;
    n.HighPart = now.dwHighDateTime;
    if (n.QuadPart < c.QuadPart) return -1.0;
    return static_cast<double>(n.QuadPart - c.QuadPart) / 1e7;
}

std::string ProcessTime() {
    const double t = SecondsSinceProcessStart();
    return t < 0 ? std::string("T+?") : std::format("T+{:.1f} s after process start", t);
}

// Exact class or subclass. Only called with objects handed to us by the game
// (hook `this`, FindObjectOfType results, list elements of a live object).
bool IsA(Il2CppObject* obj, Il2CppClass* k) {
    const auto& a = api();
    if (!obj || !k || !a.il2cpp_object_get_class) return false;
    Il2CppClass* c = a.il2cpp_object_get_class(obj);
    if (c == k) return true;
    return c && a.il2cpp_class_is_assignable_from && a.il2cpp_class_is_assignable_from(k, c);
}

bool IsGameWindowForeground() {
    HWND fg = GetForegroundWindow();
    if (!fg) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}

bool IsNativeFault(const std::string& invokeErr) { return invokeErr.starts_with("native exception"); }

class QuickStart final : public sr2::core::Module {
public:
    const char* Name() const override { return kModule; }
    bool Setup(sr2::core::ModuleContext& ctx) override;
    void OnEnable() override { enabled_.store(true); }
    void OnDisable() override {
        enabled_.store(false);
        sr2::ui::quick_start_hint::Clear();
    }

    // ---- called from detours (main thread, inside a DetourGuard) ----
    void OnLogoStart(Il2CppObject* self) { PatchLogoScene(self, /*fromStart=*/true); }
    void OnPromptLoaded(Il2CppObject* self);
    void OnPromptInteract(Il2CppObject* self);
    void OnMenuEnable(Il2CppObject* self);
    void OnIntroStart(Il2CppObject* self);
    bool OnIntroStartFrame(Il2CppObject* self, int index);  // true = replaced (original must not run)

    bool Enabled() const { return enabled_.load(std::memory_order_relaxed); }

private:
    bool ResolveIntro(sr2::core::ModuleContext& ctx, std::string& why);
    bool ResolveContinue(sr2::core::ModuleContext& ctx, std::string& why);
    bool ResolveNewGameIntro(sr2::core::ModuleContext& ctx, std::string& why);
    void Frame();
    void PatchLogoScene(Il2CppObject* scene, bool fromStart);
    void TryAutoInteract();
    void CheckShift();
    void TryAutoContinue();
    Il2CppObject* GameSceneLoader();
    Il2CppObject* Landing();
    void TakePendingLanding();
    void ReleaseLanding();
    // Ready: Continue may fire. Soft: only cosmetic checks fail. Hard: wait (a Continue would do
    // nothing or the menu is not shown). Gone: the menu instance was destroyed.
    enum class Menu { Ready, Soft, Hard, Gone };
    Menu MenuState(Il2CppObject*& landing, bool& loading, std::string& why);
    void LogMenuWait(const std::string& why);
    Il2CppObject* FindContinueModel(Il2CppObject* landing, size_t listOffset);
    bool DirectContinue(std::string& err);
    void ContinueDone(std::string_view why, bool info);
    void PublishHint();

    std::atomic<bool> enabled_{false};
    bool introOk_ = false;
    bool continueOk_ = false;
    bool newGameIntroOk_ = false;
    int frameId_ = 0;
    Clock::time_point setupAt_{};

    // -- resolved by name --
    Il2CppClass* logoClass_ = nullptr;
    size_t offLogo_[kLogoFieldCount] = {};
    size_t offSkipWarning_ = 0;
    Il2CppClass* promptClass_ = nullptr;      // PlatformEngagementPrompt (abstract)
    Il2CppClass* standaloneClass_ = nullptr;  // StandaloneEngagementPrompt
    const MethodInfo* onInteractMi_ = nullptr;
    bool hasAnimDur_ = false, hasEaFade_ = false;
    size_t offAnimDur_ = 0, offEaFade_ = 0;
    Il2CppClass* loaderClass_ = nullptr;      // SceneLoader
    size_t offLoading_ = 0;                   // <IsSceneLoadInProgress>k__BackingField
    Il2CppClass* sysCtxClass_ = nullptr;      // SystemContext (optional): the SceneLoader the menu buttons use
    size_t offSysLoader_ = 0;                 // SystemContext.<SceneLoader>k__BackingField
    Il2CppClass* landingClass_ = nullptr;     // MainMenuLandingRootUI
    size_t offModels_ = 0;
    bool hasSubModels_ = false;
    size_t offSubModels_ = 0;
    // Fallback when the Continue model does not exist (submenu never opened).
    bool directOk_ = false;
    Il2CppClass* autoSaveClass_ = nullptr;
    const MethodInfo* getSaveToContinueMi_ = nullptr;
    const MethodInfo* beginLoadMi_ = nullptr;
    const MethodInfo* saveIdentifierMi_ = nullptr;
    Il2CppClass* continueModelClass_ = nullptr;
    size_t offSummary_ = 0;                   // LoadGameBehaviorModel.<GameDataSummary>k__BackingField
    const MethodInfo* invokeBehaviorMi_ = nullptr;
    const MethodInfo* hasContinueMi_ = nullptr;
    // skip_new_game_intro
    Il2CppClass* introSeqClass_ = nullptr;    // IntroSequenceUIRoot
    const MethodInfo* endSequenceMi_ = nullptr;
    size_t offFadeInWait_ = 0, offFadeIn_ = 0, offFadeOut_ = 0, offFadeOutWait_ = 0;

    // -- state (main thread only) --
    // skip_intro: logo sequence
    const void* logoPatched_ = nullptr;  // identity only, never dereferenced
    bool logoPatchedAtStart_ = false;
    double logoVanillaSeconds_ = 0;
    Clock::time_point logoPatchedAt_{};
    bool logoPhaseOver_ = false;
    double nextLogoPoll_ = 0;
    // skip_intro: "press any key" prompt
    bool promptShown_ = false;
    bool promptDone_ = false;
    bool promptInteracted_ = false;
    bool autoInteracting_ = false;
    unsigned long long promptShownTick_ = 0;
    Clock::time_point promptShownAt_{};
    // auto_continue
    bool continueDone_ = false;
    bool shiftCanceled_ = false;
    bool menuSeen_ = false;
    Clock::time_point menuSeenAt_{};
    bool menuReady_ = false;
    bool menuEverReady_ = false;
    bool menuIdleSeen_ = false;  // no scene load ran for kMenuIdleMin after the menu was enabled
    bool idleTiming_ = false;
    Clock::time_point idleSince_{};
    bool goneTiming_ = false;
    Clock::time_point goneSince_{};
    bool findDiagDone_ = false;  // logged whether FindObjectOfType sees the active menu
    // The MainMenuLandingRootUI handed to the OnEnable listener. The listener only stores the raw
    // pointer (no GC handle work inside a hook); the next Frame() class-checks it and takes a handle.
    Il2CppObject* pendingLanding_ = nullptr;
    Il2CppGCHandle landingHandle_ = nullptr;
    Clock::time_point menuReadyAt_{};
    bool softWaiting_ = false;  // only soft checks fail, since softSince_
    Clock::time_point softSince_{};
    std::string lastWait_;  // last "waiting for the main menu" reason logged
    int waitLogs_ = 0;
    double nextMenuPoll_ = 0;
    bool hintDone_ = false;  // overlay note finished for this launch
    // skip_new_game_intro (identity only, never dereferenced)
    const void* introPatched_ = nullptr;
    const void* introSkipped_ = nullptr;
};

QuickStart* g_self = nullptr;

sr2::core::DetourGuard g_logoStartGuard{kModule, "CompanyLogoScene.Start"};
sr2::core::DetourGuard g_promptLoadedGuard{kModule, "PlatformEngagementPrompt.Onloaded"};
sr2::core::DetourGuard g_promptInteractGuard{kModule, "StandaloneEngagementPrompt.OnInteract"};
sr2::core::DetourGuard g_menuEnableGuard{kModule, "MainMenuLandingRootUI.OnEnable"};
sr2::core::DetourGuard g_frameGuard{kModule, "frame"};
sr2::core::DetourGuard g_introStartGuard{kModule, "IntroSequenceUIRoot.Start"};
sr2::core::DetourGuard g_introFrameGuard{kModule, "IntroSequenceUIRoot.StartFrame"};

using VoidFn = void (*)(Il2CppObject*, const MethodInfo*);
// OnInteract(InputAction.CallbackContext): the 16-byte struct is passed by
// reference on x64 (MSVC ABI), so forwarding the pointer unchanged is exact.
using InteractFn = void (*)(Il2CppObject*, void*, const MethodInfo*);
VoidFn g_origLogoStart = nullptr;
VoidFn g_origPromptLoaded = nullptr;
InteractFn g_origPromptInteract = nullptr;
VoidFn g_origIntroStart = nullptr;
using StartFrameFn = void (*)(Il2CppObject*, int32_t, const MethodInfo*);
StartFrameFn g_origIntroStartFrame = nullptr;

bool HookReady(Il2CppObject* self) { return g_self && self && g_self->Enabled() && main_thread::IsMainThread(); }

void LogoStartDetour(Il2CppObject* self, const MethodInfo* mi) {
    if (HookReady(self)) g_logoStartGuard.Run([&] { g_self->OnLogoStart(self); });
    g_origLogoStart(self, mi);
}

void PromptLoadedDetour(Il2CppObject* self, const MethodInfo* mi) {
    g_origPromptLoaded(self, mi);
    if (HookReady(self)) g_promptLoadedGuard.Run([&] { g_self->OnPromptLoaded(self); });
}

void PromptInteractDetour(Il2CppObject* self, void* context, const MethodInfo* mi) {
    if (HookReady(self)) g_promptInteractGuard.Run([&] { g_self->OnPromptInteract(self); });
    g_origPromptInteract(self, context, mi);
}

// Listener on ui/modded_label's MainMenuLandingRootUI.OnEnable post-hook (called after the original).
void MenuEnableListener(Il2CppObject* self) {
    if (HookReady(self)) g_menuEnableGuard.Run([&] { g_self->OnMenuEnable(self); });
}

void IntroStartDetour(Il2CppObject* self, const MethodInfo* mi) {
    if (HookReady(self)) g_introStartGuard.Run([&] { g_self->OnIntroStart(self); });
    g_origIntroStart(self, mi);
}

// Replace pattern: the original runs unless our logic ran and replaced it.
void IntroStartFrameDetour(Il2CppObject* self, int32_t index, const MethodInfo* mi) {
    bool replaced = false;
    if (HookReady(self)) g_introFrameGuard.Run([&] { replaced = g_self->OnIntroStartFrame(self, index); });
    if (!replaced) g_origIntroStartFrame(self, index, mi);
}

// ------------------------------------------------------------------ setup --

// Collects the first resolve error of an optional feature group.
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
};

bool QuickStart::ResolveIntro(sr2::core::ModuleContext& ctx, std::string& why) {
    Need n{ctx.resolver(), why};
    logoClass_ = n.Class(kUiNs, "CompanyLogoScene");
    for (size_t i = 0; i < kLogoFieldCount; ++i)
        if (!n.Offset(logoClass_, kLogoFields[i].name, offLogo_[i])) return false;
    if (!n.Offset(logoClass_, "_skipWarningScreen", offSkipWarning_)) return false;
    promptClass_ = n.Class(kMenuNs, "PlatformEngagementPrompt");
    standaloneClass_ = n.Class(kMenuNs, "StandaloneEngagementPrompt");
    onInteractMi_ = n.Method(standaloneClass_, "OnInteract", 1);
    if (!promptClass_ || !standaloneClass_ || !onInteractMi_) return false;
    // Optional: minimum display times of the prompt.
    std::string ignore;
    Need opt{ctx.resolver(), ignore};
    hasAnimDur_ = opt.Offset(promptClass_, "AnimationDuration", offAnimDur_);
    hasEaFade_ = opt.Offset(promptClass_, "EaMessageFadeTime", offEaFade_);
    if (!ignore.empty()) SR2_WARN("[{}] prompt animation fields: {} (prompt animation keeps its vanilla length)", kModule, ignore);
    return true;
}

bool QuickStart::ResolveContinue(sr2::core::ModuleContext& ctx, std::string& why) {
    Need n{ctx.resolver(), why};
    landingClass_ = n.Class(kMenuNs, "MainMenuLandingRootUI");
    if (!n.Offset(landingClass_, "_models", offModels_)) return false;
    Il2CppClass* loadModel = n.Class(kMenuModelNs, "LoadGameBehaviorModel");
    continueModelClass_ = n.Class(kMenuModelNs, "ContinueGameBehaviorModel");
    if (!n.Offset(loadModel, "<GameDataSummary>k__BackingField", offSummary_)) return false;
    invokeBehaviorMi_ = n.Method(loadModel, "InvokeBehavior", 0);
    Il2CppClass* cond = n.Class(kButtonNs, "ButtonBehaviorConditionUtility");
    hasContinueMi_ = n.Method(cond, "HasContinueGame", 0);
    if (!continueModelClass_ || !invokeBehaviorMi_ || !hasContinueMi_) return false;

    // Optional pieces.
    std::string optWhy;
    Need opt{ctx.resolver(), optWhy};
    hasSubModels_ = opt.Offset(landingClass_, "_subListModels", offSubModels_);
    autoSaveClass_ = opt.Class("MonomiPark.SlimeRancher", "AutoSaveDirector");
    getSaveToContinueMi_ = opt.Method(autoSaveClass_, "GetSaveToContinue", 0);
    beginLoadMi_ = opt.Method(autoSaveClass_, "BeginLoad", 2);
    Il2CppClass* summary = opt.Class("MonomiPark.SlimeRancher.Persist", "Summary");
    saveIdentifierMi_ = opt.Method(summary, "get_SaveIdentifier", 0);
    directOk_ = autoSaveClass_ && getSaveToContinueMi_ && beginLoadMi_ && saveIdentifierMi_;
    if (!optWhy.empty()) SR2_WARN("[{}] auto_continue: optional pieces missing: {}", kModule, optWhy);
    return true;
}

bool QuickStart::ResolveNewGameIntro(sr2::core::ModuleContext& ctx, std::string& why) {
    Need n{ctx.resolver(), why};
    introSeqClass_ = n.Class(kIntroNs, "IntroSequenceUIRoot");
    if (!n.Offset(introSeqClass_, "_fadeInWaitTime", offFadeInWait_)) return false;
    if (!n.Offset(introSeqClass_, "_fadeInTime", offFadeIn_)) return false;
    if (!n.Offset(introSeqClass_, "_fadeOutTime", offFadeOut_)) return false;
    if (!n.Offset(introSeqClass_, "_fadeOutWaitTime", offFadeOutWait_)) return false;
    endSequenceMi_ = n.Method(introSeqClass_, "EndSequence", 0);
    return endSequenceMi_ != nullptr;
}

bool QuickStart::Setup(sr2::core::ModuleContext& ctx) {
    setupAt_ = Clock::now();

    // Shared: SceneLoader.IsSceneLoadInProgress (both features wait for loads to finish).
    {
        std::string why;
        Need n{ctx.resolver(), why};
        loaderClass_ = n.Class(kSceneNs, "SceneLoader");
        if (!n.Offset(loaderClass_, "<IsSceneLoadInProgress>k__BackingField", offLoading_))
            return ctx.Fail("SceneLoader.IsSceneLoadInProgress not resolved: " + why);
        // The instance the menu's Continue checks (LoadGameBehaviorModel.InvokeBehavior reads
        // SystemContext.Instance.SceneLoader); optional, FindObjectOfType(SceneLoader) otherwise.
        std::string optWhy;
        Need opt{ctx.resolver(), optWhy};
        sysCtxClass_ = opt.Class("", "SystemContext");
        if (!opt.Offset(sysCtxClass_, "<SceneLoader>k__BackingField", offSysLoader_)) sysCtxClass_ = nullptr;
        if (!sysCtxClass_)
            SR2_INFO("[{}] SystemContext.SceneLoader not resolved ({}); using the first SceneLoader found", kModule, optWhy);
    }

    std::string introWhy, contWhy, newGameWhy;
    introOk_ = ResolveIntro(ctx, introWhy);
    continueOk_ = ResolveContinue(ctx, contWhy);
    newGameIntroOk_ = ResolveNewGameIntro(ctx, newGameWhy);
    if (!introOk_) SR2_WARN("[{}] skip_intro unavailable: {}", kModule, introWhy);
    if (!continueOk_) SR2_WARN("[{}] auto_continue unavailable: {}", kModule, contWhy);
    if (!newGameIntroOk_) SR2_WARN("[{}] skip_new_game_intro unavailable: {}", kModule, newGameWhy);
    if (!introOk_ && !continueOk_ && !newGameIntroOk_)
        return ctx.Fail("no feature resolved: " + introWhy + " / " + contWhy + " / " + newGameWhy);

    if (introOk_) {
        bool ok = ctx.HookMethod("CompanyLogoScene.Start", kImage, kUiNs, "CompanyLogoScene", "Start", 0,
                                 reinterpret_cast<void*>(&LogoStartDetour), reinterpret_cast<void**>(&g_origLogoStart));
        ok = ok && ctx.HookMethod("PlatformEngagementPrompt.Onloaded", kImage, kMenuNs, "PlatformEngagementPrompt",
                                  "Onloaded", 0, reinterpret_cast<void*>(&PromptLoadedDetour),
                                  reinterpret_cast<void**>(&g_origPromptLoaded));
        ok = ok && ctx.HookMethod("StandaloneEngagementPrompt.OnInteract", kImage, kMenuNs,
                                  "StandaloneEngagementPrompt", "OnInteract", 1,
                                  reinterpret_cast<void*>(&PromptInteractDetour),
                                  reinterpret_cast<void**>(&g_origPromptInteract));
        if (!ok) return false;  // ctx recorded the reason; all hooks of this module are removed
    }
    // MinHook allows one hook per target: ui/modded_label owns MainMenuLandingRootUI.OnEnable.
    if (continueOk_ && !sr2::ui::modded_label::SetMenuEnableListener(&MenuEnableListener)) {
        continueOk_ = false;
        SR2_WARN("[{}] auto_continue unavailable: the shared MainMenuLandingRootUI.OnEnable hook is not installed "
                 "(see the 'ui: modded label' lines)",
                 kModule);
        if (!introOk_ && !newGameIntroOk_) return ctx.Fail("no feature available (MainMenuLandingRootUI.OnEnable hook missing)");
    }
    if (newGameIntroOk_) {
        bool ok = ctx.HookMethod("IntroSequenceUIRoot.Start", kImage, kIntroNs, "IntroSequenceUIRoot", "Start", 0,
                                 reinterpret_cast<void*>(&IntroStartDetour), reinterpret_cast<void**>(&g_origIntroStart));
        ok = ok && ctx.HookMethod("IntroSequenceUIRoot.StartFrame", kImage, kIntroNs, "IntroSequenceUIRoot",
                                  "StartFrame", 1, reinterpret_cast<void*>(&IntroStartFrameDetour),
                                  reinterpret_cast<void**>(&g_origIntroStartFrame));
        if (!ok) return false;
    }

    using settings::Def;
    using settings::Kind;
    auto reg = [](const char* key, const char* label, Kind kind, double def, double mn, double mx, double step,
                  const char* tip) {
        Def d;
        d.module = kModule;
        d.key = key;
        d.label = label;
        d.kind = kind;
        d.def = def;
        d.min = mn;
        d.max = mx;
        d.step = step;
        d.tooltip = tip;
        settings::Register(d);
    };
    if (introOk_)
        reg(kSkipIntro, "Skip intro screens", Kind::Bool, 1.0, 0, 1, 1,
            "Skips the logos, the warning screen and 'press any key' at launch. Takes effect at the next launch.");
    if (continueOk_) {
        reg(kAutoContinue, "Auto-continue last save", Kind::Bool, 0.0, 0, 1, 1,
            "Loads your latest save when the main menu is ready, like Continue. Hold Shift at launch to cancel.");
        reg(kAutoContinueDelay, "Auto-continue delay", Kind::Float, 0.5, 0.0, 5.0, 0.1,
            "How long the main menu stays interactive before auto-continue fires.");
        settings::SetDisplay(kModule, kAutoContinueDelay, " s", "instant");
        settings::SetParent(kModule, kAutoContinueDelay, kAutoContinue);  // shown while auto-continue is on
    }
    if (newGameIntroOk_)
        reg(kSkipNewGameIntro, "Skip new-game intro", Kind::Bool, 1.0, 0, 1, 1,
            "Skips the story intro of a new save. The main quest and tutorials start as usual.");

    // A fault in Frame() marks the module faulted (logged by the guard); exceptions are logged and skipped.
    frameId_ = main_thread::AddFrameCallback(kModule, [this](float) { g_frameGuard.Run([this] { Frame(); }); });
    if (frameId_ == 0) return ctx.Fail("main_thread::AddFrameCallback failed");

    g_self = this;
    SR2_INFO("[{}] setup ok: skip_intro {} (CompanyLogoScene timings, PlatformEngagementPrompt), auto_continue {} "
             "(MainMenuLandingRootUI / ContinueGameBehaviorModel.InvokeBehavior), skip_new_game_intro {} "
             "(IntroSequenceUIRoot.EndSequence) | {}",
             kModule, introOk_ ? "available" : "UNAVAILABLE", continueOk_ ? "available" : "UNAVAILABLE",
             newGameIntroOk_ ? "available" : "UNAVAILABLE", ProcessTime());
    return true;
}

// --------------------------------------------------------- skip_intro: logos --

void QuickStart::PatchLogoScene(Il2CppObject* scene, bool fromStart) {
    if (!introOk_ || logoPhaseOver_ || !settings::GetBool(kModule, kSkipIntro)) return;
    if (scene == logoPatched_) return;
    if (!IsA(scene, logoClass_) || !unity::IsAlive(scene)) return;

    double vanilla = 0;
    std::string detail;
    for (size_t i = 0; i < kLogoFieldCount; ++i) {
        float& f = unity::Field<float>(scene, offLogo_[i]);
        const float v = f;
        if (v > 0 && v < 1000.0f) vanilla += static_cast<double>(v) * kLogoFields[i].waits;
        detail += std::format("{}{}={:.2f}", detail.empty() ? "" : ", ", kLogoFields[i].name, v);
        f = 0.0f;
    }
    unity::Field<bool>(scene, offSkipWarning_) = true;

    logoPatched_ = scene;
    logoPatchedAtStart_ = fromStart;
    logoVanillaSeconds_ = vanilla;
    logoPatchedAt_ = Clock::now();
    if (fromStart) {
        SR2_INFO("[{}] skip_intro: logo/warning/splash sequence shortened to ~1 frame per step (vanilla waits "
                 "{:.1f} s, warning screen until timeout or key press) | {} | vanilla timings: {}",
                 kModule, vanilla, ProcessTime(), detail);
    } else {
        SR2_INFO("[{}] skip_intro: logo sequence was already running when found ({:.1f} s after setup); remaining "
                 "waits shortened (vanilla total {:.1f} s, a wait already in progress still finishes) | {} | vanilla "
                 "timings: {}",
                 kModule, SecondsSince(setupAt_), vanilla, ProcessTime(), detail);
    }
}

// --------------------------------------------------------- skip_intro: prompt --

void QuickStart::OnPromptLoaded(Il2CppObject* self) {
    if (!introOk_ || !IsA(self, promptClass_)) return;
    // The logo coroutine has finished (it loads the prompt's scene group last).
    if (!logoPhaseOver_) {
        logoPhaseOver_ = true;
        if (logoPatched_) {
            const double took = SecondsSince(logoPatchedAt_);
            if (logoPatchedAtStart_) {
                // `took` includes the 'StandaloneStart' scene group load, which vanilla has on top of its waits.
                SR2_INFO("[{}] skip_intro: logo sequence + 'StandaloneStart' scene group load took {:.2f} s (vanilla: "
                         "{:.1f} s of waits + the same load) -> saved at least {:.1f} s",
                         kModule, took, logoVanillaSeconds_,
                         logoVanillaSeconds_ > took ? logoVanillaSeconds_ - took : 0.0);
            } else {
                SR2_INFO("[{}] skip_intro: logo sequence finished {:.2f} s after the late patch", kModule, took);
            }
        }
    }
    if (promptShown_) return;  // answer the boot prompt only (once per process)
    promptShown_ = true;
    promptDone_ = false;
    promptInteracted_ = false;
    promptShownTick_ = main_thread::TickCount();
    promptShownAt_ = Clock::now();
    SR2_INFO("[{}] 'press any key' prompt shown | {}", kModule, ProcessTime());
}

void QuickStart::OnPromptInteract(Il2CppObject* self) {
    if (!promptShown_ || !IsA(self, standaloneClass_)) return;
    if (!promptInteracted_ && !autoInteracting_)
        SR2_INFO("[{}] 'press any key' prompt answered by the player after {:.2f} s", kModule,
                 SecondsSince(promptShownAt_));
    promptInteracted_ = true;
}

void QuickStart::TryAutoInteract() {
    if (!settings::GetBool(kModule, kSkipIntro)) {
        promptDone_ = true;  // vanilla: wait for the player
        return;
    }
    if (promptInteracted_) {
        promptDone_ = true;
        return;
    }
    const double waited = SecondsSince(promptShownAt_);
    if (waited > kPromptGiveUp) {
        promptDone_ = true;
        SR2_WARN("[{}] skip_intro: prompt did not become ready within {:.0f} s; leaving it to the player", kModule,
                 kPromptGiveUp);
        return;
    }
    if (main_thread::TickCount() < promptShownTick_ + kPromptMinFrames) return;

    Il2CppObject* prompt = unity::FindObjectOfType(promptClass_);  // active instances only
    if (!prompt || !unity::IsAlive(prompt)) return;
    if (!IsA(prompt, standaloneClass_)) {
        promptDone_ = true;
        SR2_WARN("[{}] skip_intro: engagement prompt is a {}, not a StandaloneEngagementPrompt; not answering it",
                 kModule, unity::ClassName(prompt));
        return;
    }
    if (!unity::ActiveInHierarchy(prompt)) return;
    Il2CppObject* loader = unity::FindObjectOfType(loaderClass_);
    if (!loader || !unity::IsAlive(loader) || !IsA(loader, loaderClass_)) return;
    if (unity::Field<bool>(loader, offLoading_)) return;  // as in vanilla: the prompt is answerable after the load

    float animDur = 0, eaFade = 0;
    if (hasAnimDur_) {
        animDur = unity::Field<float>(prompt, offAnimDur_);
        unity::Field<float>(prompt, offAnimDur_) = 0.0f;  // Animate(): clamps t/0 to 1 -> one frame
    }
    if (hasEaFade_) {
        eaFade = unity::Field<float>(prompt, offEaFade_);
        unity::Field<float>(prompt, offEaFade_) = 0.0f;
    }

    // InputAction.CallbackContext (16 bytes) - not read by StandaloneEngagementPrompt.OnInteract.
    alignas(16) unsigned char callbackContext[32] = {};
    void* args[1] = {callbackContext};
    std::string err;
    autoInteracting_ = true;
    sr2::il2cpp::Resolver::Get().Invoke(onInteractMi_, prompt, args, &err);
    autoInteracting_ = false;
    promptDone_ = true;
    if (!err.empty()) {
        if (IsNativeFault(err)) {
            sr2::core::MarkModuleFaulted(kModule, "StandaloneEngagementPrompt.OnInteract: " + err);
        } else {
            SR2_WARN("[{}] skip_intro: answering the prompt failed ({}); press a key to continue", kModule, err);
        }
        return;
    }
    SR2_INFO("[{}] skip_intro: 'press any key' prompt answered automatically {:.2f} s after it appeared (saved: the "
             "wait for a key press + prompt animation {:.2f} s{}) | {}",
             kModule, waited, animDur, hasEaFade_ && eaFade > 0 ? std::format(" + EA message fade {:.2f} s", eaFade) : "",
             ProcessTime());
}

// ------------------------------------------------------------ auto_continue --

void QuickStart::OnMenuEnable(Il2CppObject* self) {
    if (!continueOk_ || continueDone_ || !IsA(self, landingClass_)) return;
    pendingLanding_ = self;  // handle taken by the next Frame() (also on a re-enable after a submenu)
    if (menuSeen_) return;
    menuSeen_ = true;
    menuSeenAt_ = Clock::now();
    SR2_INFO("[{}] main menu UI enabled | {}", kModule, ProcessTime());
}

// Main thread, from Frame(): the menu handed to the OnEnable listener, now held by a GC handle.
// A scene object is kept alive by its native side, so the raw pointer is still valid one frame later.
void QuickStart::TakePendingLanding() {
    Il2CppObject* p = pendingLanding_;
    pendingLanding_ = nullptr;
    if (!p || !IsA(p, landingClass_) || !unity::IsAlive(p)) return;
    if (Landing() == p) return;
    ReleaseLanding();
    landingHandle_ = unity::NewGCHandle(p);
    if (!landingHandle_) SR2_WARN("[{}] auto_continue: no GC handle for the main menu - it cannot be checked", kModule);
}

void QuickStart::ReleaseLanding() {
    if (landingHandle_) unity::FreeGCHandle(landingHandle_);
    landingHandle_ = nullptr;
}

// The held main menu while its native object lives, else nullptr.
Il2CppObject* QuickStart::Landing() {
    Il2CppObject* o = landingHandle_ ? unity::GCHandleTarget(landingHandle_) : nullptr;
    return o && unity::IsAlive(o) && IsA(o, landingClass_) ? o : nullptr;
}

void QuickStart::ContinueDone(std::string_view why, bool info) {
    continueDone_ = true;
    if (why.empty()) return;
    if (info)
        SR2_INFO("[{}] auto_continue: {}", kModule, why);
    else
        SR2_WARN("[{}] auto_continue: {}", kModule, why);
}

void QuickStart::CheckShift() {
    if (shiftCanceled_ || !continueOk_) return;
    // Shift typed in the mod menu (or Shift+Tab tried there) is not "hold Shift at launch".
    if (sr2::ui::overlay::IsOpen()) return;
    if ((GetAsyncKeyState(VK_SHIFT) & 0x8000) == 0 || !IsGameWindowForeground()) return;
    shiftCanceled_ = true;
    if (settings::GetBool(kModule, kAutoContinue))
        SR2_INFO("[{}] auto_continue: Shift held at startup - cancelled for this launch", kModule);
}

// The SceneLoader the menu's Continue checks: SystemContext.Instance.SceneLoader (the first
// SceneLoader found when SystemContext is not resolved or not there yet).
Il2CppObject* QuickStart::GameSceneLoader() {
    if (sysCtxClass_) {
        Il2CppObject* ctx = unity::FindObjectOfType(sysCtxClass_);
        if (ctx && unity::IsAlive(ctx) && IsA(ctx, sysCtxClass_)) {
            auto* loader = unity::Field<Il2CppObject*>(ctx, offSysLoader_);
            if (loader && unity::IsAlive(loader) && IsA(loader, loaderClass_)) return loader;
        }
    }
    Il2CppObject* loader = unity::FindObjectOfType(loaderClass_);
    return loader && unity::IsAlive(loader) && IsA(loader, loaderClass_) ? loader : nullptr;
}

// Hard checks (no timeout): the game's SceneLoader is there and not loading (a Continue does nothing
// otherwise), and the main menu the OnEnable listener handed us is alive and active in the hierarchy
// (round 11: Continue was invoked on a menu that was not up and the next step dereferenced it).
// Gone: that menu was destroyed (the scene changed). Soft checks (cosmetic, wait for them at most
// kMenuSoftWait): the boot overlays (logo scene, engagement prompt) are gone. why = the first check
// that fails. `landing` is only set when the menu is alive and active.
QuickStart::Menu QuickStart::MenuState(Il2CppObject*& landing, bool& loading, std::string& why) {
    landing = nullptr;
    loading = false;
    Il2CppObject* loader = GameSceneLoader();
    if (!loader) {
        why = "no SceneLoader";
        return Menu::Hard;
    }
    loading = unity::Field<bool>(loader, offLoading_);
    if (loading) {
        why = "a scene load is in progress";
        return Menu::Hard;
    }
    Il2CppObject* l = Landing();
    if (!l) {
        why = landingHandle_ ? "the main menu (MainMenuLandingRootUI) was destroyed"
                             : "no main menu (MainMenuLandingRootUI) instance yet";
        return landingHandle_ ? Menu::Gone : Menu::Hard;
    }
    if (!unity::ActiveInHierarchy(l)) {
        why = "the main menu (MainMenuLandingRootUI) is not active";
        return Menu::Hard;
    }
    landing = l;
    if (!findDiagDone_) {
        // Rounds 10/11: FindObjectOfType(MainMenuLandingRootUI) never found the menu the player saw.
        findDiagDone_ = true;
        Il2CppObject* found = unity::FindObjectOfType(landingClass_);
        SR2_INFO("[{}] auto_continue: main menu active; FindObjectOfType(MainMenuLandingRootUI) {}", kModule,
                 found == l ? "finds the same instance" : found ? "finds another instance" : "finds nothing");
    }
    // Boot overlays still up? (both deactivate themselves when the menu is revealed)
    if (logoClass_ && unity::FindObjectOfType(logoClass_)) {
        why = "the logo scene (CompanyLogoScene) is still active";
        return Menu::Soft;
    }
    if (promptClass_ && unity::FindObjectOfType(promptClass_)) {
        why = "the 'press any key' prompt is still active";
        return Menu::Soft;
    }
    why.clear();
    return Menu::Ready;
}

void QuickStart::LogMenuWait(const std::string& why) {
    if (why == lastWait_ || waitLogs_ >= kMaxWaitLogs) return;
    lastWait_ = why;
    ++waitLogs_;
    SR2_INFO("[{}] auto_continue: waiting for the main menu: {} ({:.1f} s after it was enabled)", kModule, why,
             SecondsSince(menuSeenAt_));
}

Il2CppObject* QuickStart::FindContinueModel(Il2CppObject* landing, size_t listOffset) {
    void* list = unity::Field<void*>(landing, listOffset);
    const size_t n = list ? unity::ListCount(list) : 0;
    for (size_t i = 0; i < n; ++i) {
        auto* m = static_cast<Il2CppObject*>(unity::ListAt(list, i));
        if (m && IsA(m, continueModelClass_)) return m;
    }
    return nullptr;
}

// Same calls as LoadGameBehaviorModel.InvokeBehavior on the Continue model:
// AutoSaveDirector.BeginLoad(GetSaveToContinue().SaveIdentifier, null).
// true = load started; false + empty err = no save.
bool QuickStart::DirectContinue(std::string& err) {
    auto& R = sr2::il2cpp::Resolver::Get();
    Il2CppObject* asd = unity::FindObjectOfType(autoSaveClass_);
    if (!asd || !unity::IsAlive(asd) || !IsA(asd, autoSaveClass_)) {
        err = "AutoSaveDirector not found";
        return false;
    }
    Il2CppObject* summary = R.Invoke(getSaveToContinueMi_, asd, nullptr, &err);
    if (!err.empty() || !summary) return false;
    Il2CppObject* id = R.Invoke(saveIdentifierMi_, summary, nullptr, &err);
    if (!err.empty()) return false;
    if (!id || unity::ClassName(id) != "MonomiPark.SlimeRancher.GameSaveIdentifier") {
        err = "Summary.SaveIdentifier returned no GameSaveIdentifier";
        return false;
    }
    void* args[2] = {reinterpret_cast<char*>(id) + kObjectHeader, nullptr};  // boxed struct data, Stream = null
    R.Invoke(beginLoadMi_, asd, args, &err);
    return err.empty();
}

void QuickStart::TryAutoContinue() {
    if (!settings::GetBool(kModule, kAutoContinue)) {
        ContinueDone("off for this launch (it only ever fires at the first main menu after start)", true);
        return;
    }
    if (shiftCanceled_) {
        ContinueDone("", true);  // already logged
        return;
    }
    const double clock = SecondsSince(setupAt_);
    if (clock < nextMenuPoll_) return;
    nextMenuPoll_ = clock + kMenuPollInterval;

    Il2CppObject* landing = nullptr;
    bool loading = false;
    std::string why;
    const Menu state = MenuState(landing, loading, why);
    if (!loading) {
        // No load ran for a moment after the menu was enabled: any load from now on was started
        // by the player (Continue, Load, New Game), whether or not the menu counted as ready.
        if (!idleTiming_) {
            idleTiming_ = true;
            idleSince_ = Clock::now();
        }
        if (SecondsSince(idleSince_) >= kMenuIdleMin) menuIdleSeen_ = true;
    } else {
        idleTiming_ = false;
        if (menuIdleSeen_) {
            ContinueDone("a scene load started from the main menu before auto-continue fired - cancelled", true);
            return;
        }
    }
    if (state == Menu::Gone) {
        // The menu was destroyed with no load running: wait briefly for a new one, then stop.
        if (!goneTiming_) {
            goneTiming_ = true;
            goneSince_ = Clock::now();
        }
        if (SecondsSince(goneSince_) >= kMenuGoneWait) {
            ContinueDone("the main menu closed before auto-continue fired - cancelled", true);
            return;
        }
    } else {
        goneTiming_ = false;
    }
    bool go = state == Menu::Ready;
    if (state == Menu::Soft) {
        // Only cosmetic checks fail: if the menu never was ready, wait for them a little, then go on (a
        // Continue works as soon as no scene load runs). Once it was ready, a soft failure is the menu
        // being hidden (a submenu): wait for it to come back.
        if (!softWaiting_ && !menuEverReady_) {
            softWaiting_ = true;
            softSince_ = Clock::now();
        }
        if (softWaiting_ && SecondsSince(softSince_) >= kMenuSoftWait) {
            if (!menuReady_)
                SR2_WARN("[{}] auto_continue: {} {:.0f} s after no scene load was running - continuing anyway",
                         kModule, why, kMenuSoftWait);
            go = true;
        }
    } else {
        softWaiting_ = false;
    }
    if (!go) {
        LogMenuWait(why);
        if (menuReady_)
            menuReady_ = false;  // menu hidden again (e.g. a submenu); wait for it to come back
        else if (!menuEverReady_ && SecondsSince(menuSeenAt_) > kMenuGiveUp)
            ContinueDone(std::format("main menu never became ready within {:.0f} s ({}) - giving up", kMenuGiveUp, why),
                         false);
        return;
    }
    if (!menuReady_) {
        menuReady_ = true;
        menuEverReady_ = true;
        menuReadyAt_ = Clock::now();
        SR2_INFO("[{}] main menu interactive{} | {}", kModule, state == Menu::Ready ? "" : " (soft checks skipped)",
                 ProcessTime());
    }
    const double delay = settings::GetFloat(kModule, kAutoContinueDelay);
    if (SecondsSince(menuReadyAt_) < delay) return;
    if (!landing) return;  // go implies an active menu (Ready / Soft); never act without one

    std::string err;
    Il2CppObject* boxed = sr2::il2cpp::Resolver::Get().Invoke(hasContinueMi_, nullptr, nullptr, &err);
    if (!err.empty()) {
        if (IsNativeFault(err)) sr2::core::MarkModuleFaulted(kModule, "HasContinueGame: " + err);
        ContinueDone(std::format("HasContinueGame failed ({})", err), false);
        return;
    }
    if (!boxed || unity::ClassName(boxed) != "System.Boolean") {
        ContinueDone("HasContinueGame returned no bool - not continuing", false);
        return;
    }
    if (!unity::Field<bool>(boxed, kObjectHeader)) {
        ContinueDone("HasContinueGame() is false (no save yet) - nothing to do", true);
        return;
    }

    // The Continue entry of the landing menu (main list, or the open sub list).
    Il2CppObject* cont = FindContinueModel(landing, offModels_);
    if (!cont && hasSubModels_) cont = FindContinueModel(landing, offSubModels_);

    continueDone_ = true;  // once per process, whatever happens next
    const double ready = SecondsSince(menuReadyAt_);
    const char* how = "ContinueGameBehaviorModel.InvokeBehavior";
    bool started = false;
    if (cont) {
        if (!unity::Field<void*>(cont, offSummary_)) {
            ContinueDone("the Continue entry has no save summary - nothing to do", true);
            return;
        }
        sr2::il2cpp::Resolver::Get().Invoke(invokeBehaviorMi_, cont, nullptr, &err);
        started = err.empty();
    } else if (directOk_) {
        // Continue sits in a submenu that was never opened (its model does not exist yet):
        // make the same calls its InvokeBehavior makes.
        how = "AutoSaveDirector.BeginLoad(GetSaveToContinue().SaveIdentifier)";
        started = DirectContinue(err);
    } else {
        ContinueDone("no Continue entry found in the main menu - use the menu", false);
        return;
    }
    if (!err.empty()) {
        if (IsNativeFault(err)) sr2::core::MarkModuleFaulted(kModule, std::string(how) + ": " + err);
        ContinueDone(std::format("Continue failed ({}) - use the menu", err), false);
        return;
    }
    if (!started) {
        ContinueDone("no save to continue - nothing to do", true);
        return;
    }
    SR2_INFO("[{}] auto_continue: loading the most recent save via {} after the menu was interactive for {:.2f} s "
             "| {}",
             kModule, how, ready, ProcessTime());
}

// ------------------------------------------------------ skip_new_game_intro --

void QuickStart::OnIntroStart(Il2CppObject* self) {
    if (!newGameIntroOk_ || !settings::GetBool(kModule, kSkipNewGameIntro)) return;
    if (self == introPatched_ || !IsA(self, introSeqClass_) || !unity::IsAlive(self)) return;
    float& fadeInWait = unity::Field<float>(self, offFadeInWait_);
    float& fadeIn = unity::Field<float>(self, offFadeIn_);
    float& fadeOut = unity::Field<float>(self, offFadeOut_);
    float& fadeOutWait = unity::Field<float>(self, offFadeOutWait_);
    SR2_INFO("[{}] skip_new_game_intro: new-game intro starting (vanilla fades: in-wait {:.2f}, in {:.2f}, out {:.2f}, "
             "out-wait {:.2f} s) - shortening them",
             kModule, fadeInWait, fadeIn, fadeOut, fadeOutWait);
    fadeInWait = 0.0f;
    if (!(fadeIn > 0.0f && fadeIn <= kIntroFadeIn)) fadeIn = kIntroFadeIn;
    if (!(fadeOut > 0.0f && fadeOut <= kIntroFadeOutMax)) fadeOut = kIntroFadeOutMax;
    fadeOutWait = 0.0f;
    introPatched_ = self;
}

bool QuickStart::OnIntroStartFrame(Il2CppObject* self, int index) {
    if (!newGameIntroOk_ || !settings::GetBool(kModule, kSkipNewGameIntro)) return false;
    if (self == introSkipped_ || !IsA(self, introSeqClass_) || !unity::IsAlive(self)) return false;
    introSkipped_ = self;  // once per instance; any later StartFrame call is vanilla
    std::string err;
    sr2::il2cpp::Resolver::Get().Invoke(endSequenceMi_, self, nullptr, &err);
    if (!err.empty()) {
        if (IsNativeFault(err))
            sr2::core::MarkModuleFaulted(kModule, "IntroSequenceUIRoot.EndSequence: " + err);
        else
            SR2_WARN("[{}] skip_new_game_intro: EndSequence failed ({}); showing the intro as usual", kModule, err);
        return false;
    }
    SR2_INFO("[{}] skip_new_game_intro: intro ended before frame {} via IntroSequenceUIRoot.EndSequence (the game's "
             "end-of-intro path: StartMainQuest, FadeIntoGameplay, camera re-enabled on close)",
             kModule, index);
    return true;
}

// ----------------------------------------------------------------- per frame --

void QuickStart::Frame() {
    // GC handle bookkeeping on the main-thread tick, never inside a hook.
    if (pendingLanding_) TakePendingLanding();
    if (landingHandle_ && (continueDone_ || !Enabled())) ReleaseLanding();
    if (!Enabled()) return;
    if (introOk_) {
        // Fallback for a late hook install: patch the logo scene found by polling.
        if (!logoPhaseOver_ && !logoPatched_) {
            const double clock = SecondsSince(setupAt_);
            if (clock > kLogoPollWindow) {
                logoPhaseOver_ = true;
            } else if (clock >= nextLogoPoll_) {
                nextLogoPoll_ = clock + kLogoPollInterval;
                if (settings::GetBool(kModule, kSkipIntro))
                    if (Il2CppObject* s = unity::FindObjectOfType(logoClass_)) PatchLogoScene(s, /*fromStart=*/false);
            }
        }
        if (promptShown_ && !promptDone_) TryAutoInteract();
    }
    if (continueOk_ && !continueDone_) {
        CheckShift();
        if (menuSeen_) TryAutoContinue();
    }
    if (continueOk_) PublishHint();
}

// The overlay's "Hold Shift to stay in the main menu" note (ui/quick_start_hint.h).
void QuickStart::PublishHint() {
    namespace hint = sr2::ui::quick_start_hint;
    if (hintDone_) return;
    const bool on = settings::GetBool(kModule, kAutoContinue);
    if (shiftCanceled_ && on) {
        hint::SetStaying();  // shown briefly by the overlay, then hidden
        hintDone_ = true;
        return;
    }
    if (continueDone_ || shiftCanceled_) {
        hint::Clear();
        hintDone_ = true;
        return;
    }
    if (!on) {
        hint::Clear();  // may still be turned on before the menu is ready
        return;
    }
    double left = -1;  // unknown until the menu is interactive
    if (menuReady_)
        left = std::max(0.0, settings::GetFloat(kModule, kAutoContinueDelay) - SecondsSince(menuReadyAt_));
    hint::SetPending(left);
}

}  // namespace

std::unique_ptr<sr2::core::Module> CreateQuickStart() { return std::make_unique<QuickStart>(); }

}  // namespace sr2::modules
