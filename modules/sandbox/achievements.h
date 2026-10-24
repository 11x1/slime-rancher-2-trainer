#pragma once
// sandbox: achievements are blocked while the module is enabled, and in a save marked as cheated
// (Sandbox was used in it, cheated_save.h) also while the module is off: no Steam unlock or stat,
// no change to the save's achievement progress. SR2 1.3.0, all lookups by name; details and the
// disassembly behind the choice of methods: modules/sandbox/NOTES.md, "Achievements paused".
//
// Four hooks, created in the module's Setup (which also runs while the module is off) and always
// on (HookManager module "sandbox_achievements"); the detours block only while a flag is set:
//   - MetaGameDirector.AchievementData.IncrementProgress(int): every Grant path (both Grant
//     overloads and CheckForBehindTheWaterfall) goes through it; blocked = returns false and
//     leaves CurrentProgress alone, so the director never reports to the platform.
//   - MetaGameDirector.SetAchievementProgress(Achievement, int): every gameplay "set progress"
//     call. The AchievementData overload is left alone: SetModel uses it to restore the save's
//     progress on load (it never gains anything a save did not already have).
//   - MetaGameDirector.HandleSoldPlort(int): skipped, so Beallionaire's pending sale total
//     (_tempBeallionaireProgress, flushed 30 s later) never collects Sandbox-time sales.
//   - SteamMetaGamePlatform.Grant(AchievementData): the only caller of SteamUserStats.SetStat /
//     SetAchievement / StoreStats; backstop for anything that still reaches the platform.
// Detours run on the main thread inside DetourGuards; they only compare two flags and a class.

#include <string>

namespace sr2::core {
class ModuleContext;
}

namespace sr2::modules::sandbox {

// Worker thread (module Setup): resolve, create and enable the hooks. false = ctx.Fail(...) was called.
bool SetupAchievementPause(sr2::core::ModuleContext& ctx);
// Module OnEnable / OnDisable. Turning it on also re-arms the one-time "blocked" log line.
void SetAchievementsPaused(bool paused);
bool AchievementsPaused();
// cheated_save.cpp (main thread): the loaded save is marked as cheated, which blocks while Sandbox
// is off too. Mirrored to the overlay (ui/sandbox_bridge.h, SaveCheated).
void SetSaveCheated(bool cheated);
bool SaveCheated();

}  // namespace sr2::modules::sandbox
