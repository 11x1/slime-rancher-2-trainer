#pragma once
// autotest: game access shared by the harness commands (module.cpp) and the generic actions
// (actions.cpp). Everything here runs on the Unity main thread. Symbols are resolved by name once
// (Resolve, from Setup); a missing one only disables what needs it (Error "... unavailable").
// Calls go through Resolver::Invoke: a managed exception becomes autotest::Error, a native fault
// marks the module faulted (then every command fails) and becomes an Error too.

#include <cstddef>
#include <initializer_list>
#include <string>
#include <vector>

#include "core/il2cpp_api.h"
#include "core/json.h"
#include "modules/autotest/api.h"

namespace sr2::modules::autotest {

inline constexpr const char* kModule = "autotest";

namespace game {

struct Vec3f {
    float x = 0, y = 0, z = 0;
};

// The value-type data of a boxed object.
template <class T>
T Unbox(Il2CppObject* boxed) {
    return *reinterpret_cast<T*>(reinterpret_cast<char*>(boxed) + 2 * sizeof(void*));
}

Il2CppObject* Call(const MethodInfo* m, void* obj, std::initializer_list<void*> args, const char* what);
template <class T>
T CallValue(const MethodInfo* m, void* obj, std::initializer_list<void*> args, const char* what) {
    Il2CppObject* r = Call(m, obj, args, what);
    if (!r) throw Error(std::string(what) + " returned nothing");
    return Unbox<T>(r);
}

const MethodInfo* MethodOn(Il2CppClass* k, const char* name, int argc);  // k, then its parents
Il2CppClass* FindClass(const std::string& name);  // "Ns.Class" or "Class", searched in every image
bool IsA(Il2CppObject* obj, Il2CppClass* k);
size_t FieldOffset(Il2CppClass* k, const char* name);  // k and parents; 0 = not found
std::string Utf8(Il2CppString* s);

// Resolves everything (worker thread, from Setup). `report` lists what is missing ("" = all there).
void Resolve(std::string& report);

// ---- world state (main thread; null / false when not available) ----
Il2CppObject* SceneCtx();        // SceneContext.Instance (alive)
Il2CppObject* GameCtx();         // GameContext.Instance
Il2CppObject* AutoSave();        // GameContext.AutoSaveDirector
Il2CppObject* SceneLoader();     // SystemContext.SceneLoader
Il2CppObject* PlayerObject();    // SceneContext.player (GameObject)
bool Loading();                  // SceneLoader.IsSceneLoadInProgress
bool AtTitle();                  // a main menu scene group (after a quit, or the boot one), not loading
bool InGame();                   // gameplay scene group, a GameModel and a player, not loading
std::string SceneName();         // the current scene group's name
bool Position(Il2CppObject* goOrComponent, Vec3f& out);
bool Euler(Il2CppObject* goOrComponent, Vec3f& out);

struct Metadata {
    bool has = false;
    std::string gameName, displayName;
    unsigned long long saveNumber = 0;
    int slotIndex = -1;  // GameMetadata.SlotIndex; the slot the user sees is slotIndex + 1
};
Metadata CurrentMetadata();                       // AutoSaveDirector._currentGameMetadata
Metadata MetadataOf(Il2CppObject* autoSaveDirector);  // the same from a given director (save guard)

// One save file of a game. The save folder's file names are the source of truth (which games exist, their
// slot, game name and files); AutoSaveDirector.AvailableGamesByGameName's summaries add the display name,
// save number, save time and the invalid flag when they can be read (hasSummary).
struct SaveInfo {
    std::string gameName, displayName, saveName;  // saveName = file name without ".sav"
    int slotIndex = -1;                           // from the game name "<yyyyMMddHHmmss>_<slot>": slot - 1
    unsigned long long saveNumber = 0;            // 0 = unknown (no summary)
    std::string timeLocal, timeUtc;               // "YYYY-MM-DD HH:MM:SS": the summary's save time, else the file's
    long long unixUtc = 0;
    bool invalid = false, gameOver = false;
    std::string fileName;                         // "<saveName>.sav" ("" = only in the summaries: folder unknown)
    long long fileUnixUtc = 0;                    // the file's last write time
    std::string fileTimeLocal;                    // the same, "YYYY-MM-DD HH:MM:SS" local
    bool hasSummary = false;
};
struct SaveList {
    std::vector<SaveInfo> saves;  // by slot, then game name, newest save first (NewerThan)
    std::string folder;           // the save folder ("" = not found)
    std::string folderFrom;       // "storage provider" / "LocalLow" / ""
    std::string summaryError;     // why the summaries could not be read ("" = read fine)
};
SaveList ListSaves();
std::vector<SaveInfo> Saves();  // ListSaves().saves
// The newer of two saves of a game: by save number when both have one, else by file (or save) time.
bool NewerThan(const SaveInfo& a, const SaveInfo& b);
// Loads a save: AutoSaveDirector.BeginLoad(GameSaveIdentifier { GameName, SaveName }), the same pair
// Summary.get_SaveIdentifier returns, built from the file name.
void BeginLoad(const SaveInfo& s);
void DeleteGame(const std::string& gameName);
bool SaveNow();                                   // AutoSaveDirector.SaveAllNow(false, null)
void StartNewGame(int slotIndex);                 // AutoSaveDirector.StartNewGame with a fresh GameSettingsModel
void QuitToTitle();                               // SceneContext.OnSessionEnded + SceneLoader.LoadMainMenuSceneGroup (no save)
void QuitGame();                                  // Application.Quit

// Time of day: hours 0-24 and the day number; false when not in game.
bool Clock(double& hour, int& day, double& worldTime);
void FastForwardTo(double worldTime);             // TimeDirector.FastForwardTo
bool Paused();                                    // TimeDirector.HasPauser
bool Newbucks(int& out);
std::string ZoneName();                           // current zone asset name ("" = none)
std::vector<std::string> OpenMenus();             // active UI roots the mod knows (cached ~1 s)

// Screenshots (showcase): the game HUD (the Canvas on HudUI's GameObject, Behaviour.enabled) and the
// minimap (its root GameObject; the collectibles tracker is a child of it) hidden for a few frames.
// GC handles keep what was hidden; RestoreUi puts back only what HideUi changed and frees them.
struct HiddenUi {
    Il2CppGCHandle hudCanvas = nullptr;
    Il2CppGCHandle minimapRoot = nullptr;
    std::string what;  // for the log: "HUD, minimap" ("" = nothing was showing)
};
HiddenUi HideUi(bool hud, bool minimap);  // hud: the HUD and the minimap
void RestoreUi(HiddenUi& h);

}  // namespace game
}  // namespace sr2::modules::autotest
