#pragma once
// sandbox: the "cheated" mark of a save game. Design: modules/sandbox/NOTES.md, "Cheated saves".
//
// Turning Sandbox on with a save loaded, or loading a save while Sandbox is on, marks that save:
// "sandbox": {"cheated": true, "since": ..., "save": ..., "save_number": ...} in its per-save
// sidecar sr2mods_saves/<GameName>_<i>.sav.json (the file custom_upgrades uses; they share it).
// While a marked save is loaded the achievement hooks keep blocking with Sandbox off
// (achievements.h, SetSaveCheated). Every game save of a marked session marks the new .sav file
// too, so autosaves and later saves of that game stay marked. The title screen and loading a save
// without the mark lift the block. The game's save list tags marked saves "[Sandbox]".
//
// Three hooks, created in the module's Setup and always on (HookManager module "sandbox_saves"):
//   - AutoSaveDirector.BeginLoad(GameSaveIdentifier, Stream): the save being loaded, before its
//     scenes load, so the block is on before the game re-checks achievements on load.
//   - AutoSaveDirector.CleanupAutosaves(string): runs at the end of every successful SaveGameImpl
//     (its only caller), right after the new .sav was written: marks that file.
//   - LoadGameBehaviorModel.get_Label() (optional): the save list / Continue button label; the
//     display name argument gets " [Sandbox]" for a marked save (read from its sidecar, cached).
// Plus a 1 Hz main-thread poll (title screen, saves loaded some other way, Sandbox switched on).
// Everything runs on the main thread inside DetourGuards (fault name "sandbox_saves").

namespace sr2::core {
class ModuleContext;
}

namespace sr2::modules::sandbox {

// Worker thread (module Setup, after SetupAchievementPause). false = ctx.Fail(...) was called.
bool SetupCheatedSave(sr2::core::ModuleContext& ctx);

}  // namespace sr2::modules::sandbox
