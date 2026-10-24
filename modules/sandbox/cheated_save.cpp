// sandbox: the "cheated" mark of a save game (see cheated_save.h). SR2 1.3.0.

#include "modules/sandbox/cheated_save.h"

#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <string_view>

#include "core/detour_guard.h"
#include "core/hook_manager.h"
#include "core/il2cpp_api.h"
#include "core/json.h"
#include "core/log.h"
#include "core/main_thread.h"
#include "core/module.h"
#include "core/paths.h"
#include "core/settings.h"
#include "core/text_override.h"
#include "core/unity.h"
#include "modules/sandbox/achievements.h"

namespace sr2::modules::sandbox {
namespace {

namespace unity = sr2::core::unity;
namespace main_thread = sr2::core::main_thread;
using sr2::il2cpp::api;
using sr2::il2cpp::Resolver;

constexpr const char* kModule = "sandbox";
constexpr const char* kSaves = "sandbox_saves";  // HookManager module + DetourGuard fault name
constexpr const char* kImage = "Assembly-CSharp";
constexpr const char* kSidecarSuffix = ".sav.json";  // same naming as custom_upgrades
constexpr const char* kRowText = "Sandbox: Used";  // the save details row of a marked save
constexpr unsigned long long kPollMs = 1000;
constexpr unsigned long long kPendingMs = 180000;  // a BeginLoad that never reached the game is dropped after this
constexpr unsigned long long kMenuCacheMs = 2000;  // save list: a sidecar is stat'ed at most this often

// ---- resolved names (worker thread at setup, read-only afterwards) ----
const MethodInfo* g_gameCtxInstance = nullptr;   // GameContext.Instance (SRSingleton)
size_t g_offGcAutoSave = 0;                       // GameContext.<AutoSaveDirector>k__BackingField
const MethodInfo* g_sceneCtxInstance = nullptr;  // SceneContext.Instance (SRSingleton)
size_t g_offScGameModel = 0;                      // SceneContext.<GameModel>k__BackingField
Il2CppClass* g_autoSaveClass = nullptr;           // AutoSaveDirector
size_t g_offAsdMetadata = 0;                      // AutoSaveDirector._currentGameMetadata (Nullable<GameMetadata>)
size_t g_offAsdProvider = 0;                      // AutoSaveDirector._storageProvider
size_t g_offMetaGameName = 0, g_offMetaSaveNumber = 0;  // GameMetadata (value-type offsets)
Il2CppClass* g_fileProviderClass = nullptr;       // FileStorageProvider
size_t g_offProviderSavePath = 0;                 // FileStorageProvider.savePath
// "Sandbox: Used" row in the save list's details panel (optional)
bool g_menuOk = false;
Il2CppClass* g_rootClass = nullptr;        // MainMenu.SaveGamesRootUI
size_t g_offRootItems = 0;                 // SaveGamesRootUI._gameSummaryItems (GameSummaryItemCollection)
Il2CppClass* g_summaryModelClass = nullptr;  // MainMenu.Model.GameSummaryModel
size_t g_offModelSummary = 0;              // GameSummaryModel._gameSummary
Il2CppClass* g_summaryClass = nullptr;     // Persist.Summary
size_t g_offSumSaveName = 0, g_offSumSaveNumber = 0;
Il2CppClass* g_tarrDefClass = nullptr;     // TarrEnabledGameSummaryItemDefinition (the template of our row)
size_t g_offDefIcon = 0;                   // GameSummaryItemDefinition._icon
size_t g_offTarrOn = 0, g_offTarrOff = 0;  // TarrEnabledGameSummaryItemDefinition._enabledString / _disabledString
Il2CppClass* g_tarrVmClass = nullptr;      // TarrEnabledGameSummaryItemViewModel
const MethodInfo* g_tarrVmCtor = nullptr;  // .ctor(TarrEnabledGameSummaryItemDefinition, GameSummaryModel)
Il2CppClass* g_spriteClass = nullptr;      // UnityEngine.Sprite (the row's icon, by name)
using ObjectNewFn = Il2CppObject* (*)(Il2CppClass*);
ObjectNewFn g_objectNew = nullptr;         // il2cpp_object_new
Il2CppGCHandle g_rowDef = nullptr;         // our clone of the Tarr row definition (kept for good)
bool g_rowFailed = false;                  // building it failed once: no row (logged)

using BeginLoadFn = void (*)(Il2CppObject*, const void*, Il2CppObject*, const MethodInfo*);
using CleanupFn = void (*)(Il2CppObject*, Il2CppString*, const MethodInfo*);
using FetchFn = Il2CppObject* (*)(Il2CppObject*, Il2CppObject*, const MethodInfo*);
BeginLoadFn g_origBeginLoad = nullptr;
CleanupFn g_origCleanup = nullptr;
FetchFn g_origFetch = nullptr;

sr2::core::DetourGuard g_beginLoadGuard{kSaves, "AutoSaveDirector.BeginLoad"};
sr2::core::DetourGuard g_cleanupGuard{kSaves, "AutoSaveDirector.CleanupAutosaves"};
sr2::core::DetourGuard g_fetchGuard{kSaves, "SaveGamesRootUI.FetchGameSummaryItemData"};
sr2::core::DetourGuard g_pollGuard{kSaves, "poll"};

// ---- session (main thread only) ----
struct Session {
    std::string game;                    // GameName of the loaded save ("" = none)
    std::string file;                    // its .sav file name without extension ("<GameName>_<i>"), "" = unknown
    std::string since;                   // when Sandbox was first used in it (marked sessions)
    unsigned long long number = 0;       // GameMetadata.SaveNumber of `file`
    unsigned long long markNumber = 0;   // the mark's save_number, checked once the load finished
    bool known = false;                  // number checked against the loaded game
};
Session g_s;
bool g_pending = false;  // BeginLoad seen, the game is not running yet
unsigned long long g_pendingSince = 0, g_lastPoll = 0;
// Sandbox on / off per save (main thread). The module's `enabled` setting is global, so this keeps it per
// save: off at startup and whenever a save is left; a marked save gets back its own last state on load
// (the mark's "enabled"); a save without the mark loads with Sandbox off, unless it was switched on (after its
// confirmation) on the title screen since: then the next save loaded is the one it is meant for.
bool g_startupDone = false;
bool g_lastOn = false;  // Sandbox on at the last poll
bool g_armed = false;   // switched on while no save was running: applies to the next save loaded
// The loaded save's Sandbox toggles (SaveToggle); all off for a save without the mark.
constexpr const char* kToggleKeys[] = {kToggleInfiniteEnergy, kToggleInfiniteHealth};
std::map<std::string, bool> g_toggles;
unsigned g_togglesVersion = 1;

struct MenuEntry {
    std::filesystem::file_time_type time{};
    uintmax_t size = 0;
    bool exists = false;
    bool cheated = false;
    unsigned long long checked = 0;
};
std::map<std::wstring, MenuEntry> g_menuCache;  // sidecar path -> mark (main thread)
std::set<std::string> g_menuLogged;             // one log line per save shown with the row
std::map<Il2CppClass*, const MethodInfo*> g_listAdd;  // List<IListItemViewModel>.Add by list class

bool IsA(Il2CppObject* obj, Il2CppClass* k) {
    const auto& a = api();
    if (!obj || !k || !a.il2cpp_object_get_class) return false;
    Il2CppClass* c = a.il2cpp_object_get_class(obj);
    if (c == k) return true;
    return c && a.il2cpp_class_is_assignable_from && a.il2cpp_class_is_assignable_from(k, c);
}

std::wstring Widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return {};
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string Narrow(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

// Same as custom_upgrades' SanitizeFileName: both modules must name the sidecar of a .sav alike.
std::string SanitizeFileName(const std::string& s) {
    std::string out;
    for (char c : s) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' ||
                        c == '_' || c == '.';
        out += ok ? c : '_';
        if (out.size() >= 120) break;
    }
    if (out.empty() || out == "." || out == "..") out = "_";
    return out;
}

std::filesystem::path SidecarDir() { return sr2::paths::SelfDir() / "sr2mods_saves"; }
std::filesystem::path SidecarPath(const std::string& save) {
    return SidecarDir() / Widen(SanitizeFileName(save) + kSidecarSuffix);
}

std::string Now() {
    SYSTEMTIME t{};
    GetLocalTime(&t);
    return std::format("{:04}-{:02}-{:02} {:02}:{:02}:{:02}", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
}

// ---- the mark in a sidecar ----
struct Mark {
    bool cheated = false;
    std::string since, save;
    unsigned long long number = 0;
    int enabled = -1;  // Sandbox on (1) / off (0) when that file was last written; -1 = not recorded (older mark)
    std::map<std::string, bool> toggles;  // "toggles" (missing keys: off)
};

bool ReadJson(const std::filesystem::path& p, sr2::json::Value& root, std::string& err) {
    std::ifstream f(p, std::ios::binary);
    if (!f) {
        err = "cannot open";
        return false;
    }
    std::stringstream ss;
    ss << f.rdbuf();
    if (!sr2::json::Parse(ss.str(), root, err) || !root.isObject()) {
        if (err.empty()) err = "not a JSON object";
        return false;
    }
    return true;
}

bool ReadMark(const std::filesystem::path& p, Mark& m) {
    std::error_code ec;
    if (!std::filesystem::exists(p, ec)) return false;
    sr2::json::Value root;
    std::string err;
    if (!ReadJson(p, root, err)) {
        SR2_WARN("[{}] sidecar {} unreadable ({}) - no Sandbox mark read from it", kModule, Narrow(p.filename().wstring()),
                 err);
        return false;
    }
    const sr2::json::Value& s = root["sandbox"];
    m.cheated = s["cheated"].asBool(false);
    m.since = s["since"].asString();
    m.save = s["save"].asString();
    const double n = s["save_number"].asNumber(0);
    m.number = n > 0 ? static_cast<unsigned long long>(n) : 0;
    if (s["enabled"].isBool()) m.enabled = s["enabled"].asBool() ? 1 : 0;
    for (const char* k : kToggleKeys) m.toggles[k] = s["toggles"][k].asBool(false);
    return m.cheated;
}

// The mark of .sav file `save` with save number `number` (0 = unknown: not checked).
bool Marked(const std::string& save, unsigned long long number, Mark* out = nullptr) {
    Mark m;
    if (save.empty() || !ReadMark(SidecarPath(save), m)) return false;
    if (!m.save.empty() && m.save != save) return false;  // a copied / renamed sidecar
    if (m.number && number && m.number != number) return false;  // written for an older file of that name
    if (out) *out = m;
    return true;
}

// Sets "sandbox" in the sidecar of `save`, keeping whatever else is in it (custom_upgrades' data).
// `enabled`: whether Sandbox is on in that save now (restored when it is loaded again).
bool WriteMark(const std::string& save, const std::string& game, unsigned long long number, const std::string& since,
               bool enabled) {
    const auto path = SidecarPath(save);
    std::error_code ec;
    sr2::json::Value root = sr2::json::Value::Object();
    if (std::filesystem::exists(path, ec)) {
        std::string err;
        if (!ReadJson(path, root, err)) {  // never replace a file we can't read (it may hold custom upgrades)
            SR2_WARN("[{}] sidecar {} unreadable ({}) - Sandbox mark not written", kModule,
                     Narrow(path.filename().wstring()), err);
            return false;
        }
    } else {
        root.set("note", sr2::json::Value(std::string(
                             "sr2mods data for this save file. Not read by the game. \"sandbox\": Sandbox was used in "
                             "this save, so Steam achievements stay disabled in it (delete that entry to clear it).")));
    }
    sr2::json::Value mark = sr2::json::Value::Object();
    mark.set("cheated", sr2::json::Value(true));
    mark.set("since", sr2::json::Value(since));
    mark.set("game", sr2::json::Value(game));
    mark.set("save", sr2::json::Value(save));
    mark.set("save_number", sr2::json::Value(static_cast<double>(number)));
    mark.set("enabled", sr2::json::Value(enabled));
    sr2::json::Value toggles = sr2::json::Value::Object();
    for (const char* k : kToggleKeys) {
        const auto it = g_toggles.find(k);
        toggles.set(k, sr2::json::Value(it != g_toggles.end() && it->second));
    }
    mark.set("toggles", std::move(toggles));
    root.set("sandbox", std::move(mark));
    std::filesystem::create_directories(path.parent_path(), ec);
    const std::string text = sr2::json::Serialize(root);
    auto tmp = path;
    tmp += L".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) {
            SR2_WARN("[{}] cannot write {}", kModule, Narrow(tmp.wstring()));
            return false;
        }
        f.write(text.data(), static_cast<std::streamsize>(text.size()));
        if (!f) return false;
    }
    if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        SR2_WARN("[{}] cannot replace {} (error {})", kModule, Narrow(path.wstring()), GetLastError());
        return false;
    }
    g_menuCache.erase(path.wstring());
    return true;
}

// ---- game state ----
Il2CppObject* Invoke(const MethodInfo* m, void* obj, void** params, const char* what, bool* ok) {
    std::string err;
    Il2CppObject* r = Resolver::Get().Invoke(m, obj, params, &err);
    *ok = err.empty();
    if (!*ok) {
        if (err.rfind("native exception", 0) == 0) sr2::core::MarkModuleFaulted(kSaves, std::format("{}: {}", what, err));
        else SR2_WARN("[{}] {} failed: {}", kModule, what, err);
    }
    return r;
}

Il2CppObject* AutoSave() {
    bool ok = false;
    Il2CppObject* gc = Invoke(g_gameCtxInstance, nullptr, nullptr, "GameContext.Instance", &ok);
    if (!ok || !unity::IsAlive(gc)) return nullptr;
    Il2CppObject* asd = unity::Field<Il2CppObject*>(gc, g_offGcAutoSave);
    return unity::IsAlive(asd) && IsA(asd, g_autoSaveClass) ? asd : nullptr;
}

// A save game is running (on the title screen _currentGameMetadata still holds the last game).
bool InGame() {
    bool ok = false;
    Il2CppObject* sc = Invoke(g_sceneCtxInstance, nullptr, nullptr, "SceneContext.Instance", &ok);
    return ok && unity::IsAlive(sc) && unity::Field<Il2CppObject*>(sc, g_offScGameModel);
}

// AutoSaveDirector._currentGameMetadata: GameName and SaveNumber (after a save: the new file's).
bool Metadata(Il2CppObject* asd, std::string& game, unsigned long long& number) {
    if (!asd || !IsA(asd, g_autoSaveClass)) return false;
    // Nullable<GameMetadata>: bool hasValue at +0, the struct at +8 (as custom_upgrades reads it).
    char* nullable = reinterpret_cast<char*>(asd) + g_offAsdMetadata;
    if (!*reinterpret_cast<bool*>(nullable)) return false;
    char* meta = nullable + 8;
    game = unity::ToUtf8(*reinterpret_cast<Il2CppString**>(meta + g_offMetaGameName));
    number = *reinterpret_cast<unsigned long long*>(meta + g_offMetaSaveNumber);
    return true;
}

// The newest "<game>_<i>.sav" in the game's save folder (right after a save: the file just written).
std::string NewestSave(Il2CppObject* asd, const std::string& game) {
    if (!asd || game.empty()) return {};
    Il2CppObject* provider = unity::Field<Il2CppObject*>(asd, g_offAsdProvider);
    if (!provider || !IsA(provider, g_fileProviderClass)) return {};
    const std::wstring dir = Widen(unity::ToUtf8(unity::Field<Il2CppString*>(provider, g_offProviderSavePath)));
    if (dir.empty()) return {};
    const std::wstring prefix = Widen(game) + L"_";
    std::error_code ec;
    std::filesystem::file_time_type best{};
    std::wstring found;
    std::filesystem::directory_iterator it(dir, ec), end;
    for (int guard = 0; !ec && it != end && guard < 4096; it.increment(ec), ++guard) {
        const std::filesystem::path& p = it->path();
        if (p.extension() != L".sav") continue;
        const std::wstring stem = p.stem().wstring();
        if (stem.size() <= prefix.size() || stem.compare(0, prefix.size(), prefix) != 0) continue;
        if (!std::all_of(stem.begin() + static_cast<std::ptrdiff_t>(prefix.size()), stem.end(),
                         [](wchar_t c) { return c >= L'0' && c <= L'9'; }))
            continue;
        std::error_code ec2;
        const auto t = std::filesystem::last_write_time(p, ec2);
        if (ec2) continue;
        if (found.empty() || t > best) {
            best = t;
            found = stem;
        }
    }
    return Narrow(found);
}

// ---- session logic (main thread) ----
// The toggles of the save being loaded (its mark), or all off (no mark, title screen).
void LoadToggles(const Mark* m, const std::string& what) {
    g_toggles.clear();
    std::string on;
    if (m)
        for (const auto& [k, v] : m->toggles) {
            g_toggles[k] = v;
            if (v) on += (on.empty() ? "" : ", ") + k;
        }
    ++g_togglesVersion;
    SR2_INFO("[{}] Sandbox toggles for {}: {}", kModule, what, on.empty() ? std::string("all off") : on + " on");
}

// Switches Sandbox (the module's persisted `enabled` setting, as the overlay's switch does). `now`: from the
// poll (a frame callback), applied at once (the module manager switches the module on this thread); from a
// detour it is posted, so no hook is switched inside another hook.
void SetSandbox(bool on, const std::string& why, bool now) {
    if (AchievementsPaused() == on) return;
    SR2_INFO("[{}] Sandbox {}: {}", kModule, on ? "on" : "off", why);
    auto apply = [on] {
        if (AchievementsPaused() != on) sr2::core::settings::Set(kModule, "enabled", on ? 1.0 : 0.0);
        g_lastOn = AchievementsPaused();
    };
    if (now) apply();
    else main_thread::Post(apply);
}

// A save is being loaded or found running: Sandbox as that save had it. A marked save gets its last state
// back (older marks without it keep the current state); a save without the mark starts with Sandbox off,
// unless Sandbox was switched on on the title screen for it (g_armed).
void ApplySaveState(bool cheated, int markEnabled, const std::string& file, bool now) {
    if (cheated) {
        if (markEnabled >= 0)
            SetSandbox(markEnabled == 1, std::format("save '{}' had it {} when it was last saved", file,
                                                     markEnabled == 1 ? "on" : "off"),
                       now);
    } else if (!g_armed) {
        SetSandbox(false, std::format("save '{}' has no Sandbox mark, so it starts with Sandbox off (each save keeps "
                                      "its own Sandbox switch)",
                                      file),
                   now);
    }
    g_armed = false;
}

void MarkNow(Il2CppObject* asd, const char* why) {
    if (!g_s.known || g_s.game.empty()) return;
    if (g_s.file.empty()) g_s.file = NewestSave(asd, g_s.game);
    if (g_s.since.empty()) g_s.since = Now();
    g_s.markNumber = g_s.number;
    SetSaveCheated(true);
    if (g_s.file.empty()) {
        SR2_WARN("[{}] {}: save game '{}' (save #{}) is marked as cheated for this session, but its save file is "
                 "unknown - the mark goes into the next file the game saves",
                 kModule, why, g_s.game, g_s.number);
        return;
    }
    const bool ok = WriteMark(g_s.file, g_s.game, g_s.number, g_s.since, AchievementsPaused());
    SR2_INFO("[{}] {}: save '{}' (game '{}', save #{}) marked as cheated{} - Steam achievements stay disabled in it",
             kModule, why, g_s.file, g_s.game, g_s.number, ok ? "" : " (sidecar not written, this session only)");
}

// A save is running that we did not see being loaded (or whose number changed unexpectedly):
// look for this game's sidecar with that save number.
void Evaluate(Il2CppObject* asd, const std::string& game, unsigned long long number) {
    const bool fresh = !g_s.known || g_s.game != game;  // not just a save number that moved within the game
    g_s = {};
    g_s.game = game;
    g_s.number = number;
    g_s.known = true;
    Mark found;
    bool cheated = false;
    if (number) {
        const std::wstring prefix = Widen(SanitizeFileName(game) + "_");
        const std::wstring suffix = Widen(kSidecarSuffix);
        std::error_code ec;
        std::filesystem::directory_iterator it(SidecarDir(), ec), end;
        for (int guard = 0; !ec && it != end && guard < 4096 && !cheated; it.increment(ec), ++guard) {
            const std::wstring name = it->path().filename().wstring();
            if (name.size() <= suffix.size() || name.rfind(prefix, 0) != 0 ||
                name.compare(name.size() - suffix.size(), suffix.size(), suffix) != 0)
                continue;
            Mark m;
            if (!ReadMark(it->path(), m) || m.number != number || m.save.empty()) continue;
            if (it->path().filename() != SidecarPath(m.save).filename()) continue;  // copied / renamed file
            found = m;
            cheated = true;
        }
    }
    if (cheated) {
        g_s.file = found.save;
        g_s.since = found.since;
        g_s.markNumber = found.number;
    } else {
        g_s.file = NewestSave(asd, game);
    }
    SetSaveCheated(cheated);
    SR2_INFO("[{}] save game '{}' (save #{}, file '{}') is running: {}", kModule, game, number, g_s.file,
             cheated ? std::format("marked as cheated (Sandbox used since {})", found.since)
                     : std::string("no Sandbox mark"));
    if (fresh) {
        ApplySaveState(cheated, found.enabled, g_s.file.empty() ? game : g_s.file, /*now=*/true);
        LoadToggles(cheated ? &found : nullptr, std::format("save '{}'", g_s.file.empty() ? game : g_s.file));
    }
}

void OnBeginLoad(const void* identifier) {
    if (!identifier) return;
    // GameSaveIdentifier { string GameName; string SaveName; } by reference (x64, 16 bytes);
    // SaveName is the .sav file name without extension.
    auto* const* s = static_cast<Il2CppString* const*>(identifier);
    const std::string game = unity::ToUtf8(s[0]), file = unity::ToUtf8(s[1]);
    Mark m;
    const bool cheated = Marked(file, 0, &m);  // the save number is checked once the load finished
    g_s = {};
    g_s.game = game;
    g_s.file = file;
    if (cheated) {
        g_s.since = m.since;
        g_s.markNumber = m.number;
    }
    g_pending = true;
    g_pendingSince = GetTickCount64();
    SetSaveCheated(cheated);
    SR2_INFO("[{}] loading save '{}' (game '{}'): {}", kModule, file, game,
             cheated ? std::format("marked as cheated (Sandbox used since {}) - achievements stay blocked", m.since)
                     : std::string("no Sandbox mark"));
    ApplySaveState(cheated, cheated ? m.enabled : -1, file, /*now=*/false);  // posted: we are inside the hook
    LoadToggles(cheated ? &m : nullptr, std::format("save '{}'", file));
}

void OnSaved(Il2CppObject* asd, Il2CppString* gameName) {
    const std::string game = unity::ToUtf8(gameName);
    std::string metaGame;
    unsigned long long number = 0;
    if (game.empty() || !Metadata(asd, metaGame, number) || metaGame != game) return;
    if (game != g_s.game) {  // e.g. the first save of a new game
        g_s = {};
        g_s.game = game;
        SetSaveCheated(false);
    }
    g_s.file = NewestSave(asd, game);
    g_s.number = number;
    g_s.known = true;
    if (AchievementsPaused() && !SaveCheated()) {  // switched on less than a poll ago
        g_s.since = Now();
        SetSaveCheated(true);
    }
    if (!SaveCheated()) return;
    if (g_s.since.empty()) g_s.since = Now();
    g_s.markNumber = number;
    if (g_s.file.empty()) {
        SR2_WARN("[{}] game '{}' saved (save #{}), but the new save file was not found - Sandbox mark not written",
                 kModule, game, number);
        return;
    }
    if (WriteMark(g_s.file, game, number, g_s.since, AchievementsPaused()))
        SR2_INFO("[{}] new save file '{}' (save #{}) marked as cheated, like the save it was made from", kModule,
                 g_s.file, number);
}

void Poll() {
    const unsigned long long now = GetTickCount64();
    if (now - g_lastPoll < kPollMs) return;
    g_lastPoll = now;
    const bool on = AchievementsPaused();  // Sandbox is on
    const bool inGame = InGame();
    if (!g_startupDone) {
        g_startupDone = true;
        // sr2mods.json keeps `enabled` from the last session's save: no save is running, so it does not apply.
        if (!inGame && !g_pending && on) {
            SetSandbox(false, "no save is loaded yet (each save keeps its own Sandbox switch)", true);
            return;
        }
    }
    if (!inGame) {
        if (g_pending && now - g_pendingSince < kPendingMs) return;  // loading
        g_pending = false;
        if (g_s.known || !g_s.game.empty() || SaveCheated()) {
            if (SaveCheated())
                SR2_INFO("[{}] left save game '{}' (title screen) - its Sandbox mark no longer applies", kModule,
                         g_s.game);
            const std::string left = g_s.game;
            g_s = {};
            SetSaveCheated(false);
            g_armed = false;
            SetSandbox(false, std::format("left save game '{}' (each save keeps its own Sandbox switch)", left), true);
            LoadToggles(nullptr, "the title screen");
        } else if (on && !g_lastOn) {
            g_armed = true;  // switched on (confirmed) on the title screen: for the save loaded next
            SR2_INFO("[{}] Sandbox switched on with no save loaded: it stays on for the save you load next", kModule);
        } else if (!on) {
            g_armed = false;
        }
        g_lastOn = AchievementsPaused();
        return;
    }
    Il2CppObject* asd = AutoSave();
    std::string game;
    unsigned long long number = 0;
    if (!asd || !Metadata(asd, game, number) || game.empty()) return;
    if (g_pending) {
        g_pending = false;
        if (game == g_s.game) {
            g_s.number = number;
            g_s.known = true;
            if (SaveCheated() && g_s.markNumber && number && g_s.markNumber != number) {
                SR2_INFO("[{}] the Sandbox mark of '{}' was written for save #{}, the loaded file is save #{} - "
                         "ignored (an older file of that name)",
                         kModule, g_s.file, g_s.markNumber, number);
                g_s.since.clear();
                g_s.markNumber = 0;
                SetSaveCheated(false);
                SetSandbox(false, std::format("save '{}' has no valid Sandbox mark", g_s.file), true);
                LoadToggles(nullptr, std::format("save '{}'", g_s.file));
            }
        } else {
            Evaluate(asd, game, number);
        }
    } else if (!g_s.known || game != g_s.game || number != g_s.number) {
        Evaluate(asd, game, number);
    }
    const bool nowOn = AchievementsPaused();
    if (nowOn && !SaveCheated()) {
        MarkNow(asd, "Sandbox is on");
    } else if (nowOn != g_lastOn && SaveCheated() && g_s.known && !g_s.file.empty()) {
        // Switched in a marked save: its sidecar records the new state, restored when it is loaded again.
        if (WriteMark(g_s.file, g_s.game, g_s.number, g_s.since, nowOn))
            SR2_INFO("[{}] save '{}': Sandbox {} - kept for this save", kModule, g_s.file, nowOn ? "on" : "off");
    }
    g_lastOn = nowOn;
}

// ---- save list tag ----
bool MenuMarked(const std::string& save, unsigned long long number) {
    const auto path = SidecarPath(save);
    const std::wstring key = path.wstring();
    const unsigned long long now = GetTickCount64();
    auto it = g_menuCache.find(key);
    if (it != g_menuCache.end() && now - it->second.checked < kMenuCacheMs) return it->second.cheated;
    MenuEntry e;
    e.checked = now;
    std::error_code ec;
    e.exists = std::filesystem::exists(path, ec);
    if (e.exists) {
        e.time = std::filesystem::last_write_time(path, ec);
        e.size = std::filesystem::file_size(path, ec);
    }
    if (it != g_menuCache.end() && it->second.exists == e.exists && it->second.time == e.time &&
        it->second.size == e.size) {
        it->second.checked = now;  // unchanged
        return it->second.cheated;
    }
    e.cheated = e.exists && Marked(save, number);
    if (g_menuCache.size() > 256) g_menuCache.clear();
    g_menuCache[key] = e;
    return e.cheated;
}

// Our row's definition: a clone of the save list's Tarr row definition (TarrEnabledGameSummaryItemDefinition,
// from SaveGamesRootUI._gameSummaryItems) whose two texts (_enabledString / _disabledString, fresh
// LocalizedString instances in the clone) are registered with core/text_override as "Sandbox: Used", and whose
// icon is the Fabricator's tools sprite when loaded (else none: the row shows the panel's unknown icon). Built
// once; kept with a GC handle.
Il2CppObject* RowDefinition(Il2CppObject* root) {
    if (g_rowDef) {
        Il2CppObject* def = unity::GCHandleTarget(g_rowDef);
        if (def && unity::IsAlive(def)) return def;
        Il2CppGCHandle h = g_rowDef;  // unloaded: build it again; the handle is freed outside the hook
        main_thread::Post([h] { unity::FreeGCHandle(h); });
        g_rowDef = nullptr;
    }
    if (g_rowFailed) return nullptr;
    auto fail = [](const std::string& why) -> Il2CppObject* {
        g_rowFailed = true;
        SR2_WARN("[{}] save list: no \"{}\" row ({})", kModule, kRowText, why);
        return nullptr;
    };
    Il2CppObject* items = unity::Field<Il2CppObject*>(root, g_offRootItems);
    if (!items || !unity::IsAlive(items)) return nullptr;  // not set up yet: try again next time
    auto listOff = Resolver::Get().FieldOffset(api().il2cpp_object_get_class(items), "items");  // ListAsset<T>.items
    void* list = listOff ? unity::Field<void*>(items, *listOff) : nullptr;
    Il2CppObject* tmpl = nullptr;
    for (size_t i = 0, n = unity::ListCount(list); i < n && !tmpl; ++i) {
        auto* d = static_cast<Il2CppObject*>(unity::ListAt(list, i));
        if (IsA(d, g_tarrDefClass) && unity::IsAlive(d)) tmpl = d;
    }
    if (!tmpl) return fail("the panel has no Tarr row to copy");
    Il2CppObject* def = unity::Instantiate(tmpl, nullptr);
    if (!def || !IsA(def, g_tarrDefClass)) return fail("Object.Instantiate failed");
    Il2CppObject* on = unity::Field<Il2CppObject*>(def, g_offTarrOn);
    Il2CppObject* off = unity::Field<Il2CppObject*>(def, g_offTarrOff);
    // The texts must be the clone's own instances, or the override would rename the real Tarr row too.
    if (!on || !off || on == unity::Field<Il2CppObject*>(tmpl, g_offTarrOn) ||
        off == unity::Field<Il2CppObject*>(tmpl, g_offTarrOff))
        return fail("the copy shares its texts with the Tarr row");
    sr2::core::text_override::Set(on, kRowText);
    sr2::core::text_override::Set(off, kRowText);
    Il2CppObject* icon = nullptr;
    if (g_spriteClass)
        for (Il2CppObject* s : unity::FindObjectsOfTypeAll(g_spriteClass))  // once
            if (unity::IsAlive(s) && unity::GetName(s) == "iconCategoryUtilities") {
                icon = s;
                break;
            }
    unity::Field<Il2CppObject*>(def, g_offDefIcon) = icon;
    g_rowDef = unity::NewGCHandle(def);
    SR2_INFO("[{}] save list: \"{}\" row ready (a copy of the Tarr row{})", kModule, kRowText,
             icon ? ", Fabricator tools icon" : ", no icon");
    return def;
}

// SaveGamesRootUI.FetchGameSummaryItemData(model) (1.3.0 RVA 0x7351A0, decompiled) builds the details panel's
// rows (Newbucks, zone, day, discoveries, Tarrs, slime aggression) from _gameSummaryItems, one
// GameSummaryItemViewModel each, when a save is selected. For a marked save one more row is appended: a
// TarrEnabledGameSummaryItemViewModel (its .ctor(definition, model)) of our definition, so it is drawn by the
// same view holder in the same style. Nothing while the text sink is down (the row would read "Tarrs ...").
void AddSummaryRow(Il2CppObject* root, Il2CppObject* model, Il2CppObject* list) {
    if (!list || !IsA(model, g_summaryModelClass) || !sr2::core::text_override::SinkActive()) return;
    Il2CppObject* summary = unity::Field<Il2CppObject*>(model, g_offModelSummary);
    if (!summary || !IsA(summary, g_summaryClass)) return;
    const std::string save = unity::ToUtf8(unity::Field<Il2CppString*>(summary, g_offSumSaveName));
    const unsigned long long number = unity::Field<unsigned long long>(summary, g_offSumSaveNumber);
    if (save.empty() || !MenuMarked(save, number)) return;
    Il2CppObject* def = RowDefinition(root);
    if (!def) return;
    Il2CppObject* vm = g_objectNew(g_tarrVmClass);
    if (!vm) return;
    bool ok = false;
    void* ctorArgs[] = {def, model};
    Invoke(g_tarrVmCtor, vm, ctorArgs, "TarrEnabledGameSummaryItemViewModel..ctor", &ok);
    if (!ok) return;
    Il2CppClass* k = api().il2cpp_object_get_class(list);
    auto it = g_listAdd.find(k);
    if (it == g_listAdd.end()) {
        const MethodInfo* m = api().il2cpp_class_get_method_from_name ? api().il2cpp_class_get_method_from_name(k, "Add", 1)
                                                                      : nullptr;
        it = g_listAdd.emplace(k, m).first;
        if (!m) SR2_WARN("[{}] save list: {}.Add not found - no \"{}\" row", kModule, unity::ClassName(list), kRowText);
    }
    if (!it->second) return;
    void* addArgs[] = {vm};
    Invoke(it->second, list, addArgs, "save details row", &ok);
    if (ok && g_menuLogged.insert(save).second)
        SR2_INFO("[{}] save list: '{}' shows \"{}\" (marked as cheated)", kModule, save, kRowText);
}

// ---- detours ----
void BeginLoadDetour(Il2CppObject* self, const void* identifier, Il2CppObject* stream, const MethodInfo* mi) {
    g_beginLoadGuard.Run([&] {
        if (main_thread::IsMainThread()) OnBeginLoad(identifier);
    });
    g_origBeginLoad(self, identifier, stream, mi);
}

void CleanupDetour(Il2CppObject* self, Il2CppString* gameName, const MethodInfo* mi) {
    g_origCleanup(self, gameName, mi);  // deletes the files past the 5 newest; the new one stays
    g_cleanupGuard.Run([&] {
        if (main_thread::IsMainThread()) OnSaved(self, gameName);
    });
}

Il2CppObject* FetchDetour(Il2CppObject* self, Il2CppObject* model, const MethodInfo* mi) {
    Il2CppObject* r = g_origFetch(self, model, mi);
    if (g_menuOk) g_fetchGuard.Run([&] {
        if (main_thread::IsMainThread() && IsA(self, g_rootClass)) AddSummaryRow(self, model, r);
    });
    return r;
}

const MethodInfo* MethodInParents(Resolver& R, Il2CppClass* k, const char* name, int argc, std::string& err) {
    for (Il2CppClass* c = k; c; c = api().il2cpp_class_get_parent ? api().il2cpp_class_get_parent(c) : nullptr)
        if (const MethodInfo* m = R.Method(c, name, argc, &err)) return m;
    return nullptr;
}

bool Hook(sr2::core::ModuleContext& ctx, const char* name, const MethodInfo* m, void* detour, void** original,
          std::string& rvas) {
    std::string err;
    void* target = m ? Resolver::Get().MethodPointer(m, &err) : nullptr;
    auto& hm = sr2::core::HookManager::Get();
    const std::string full = std::format("{}.{}", kSaves, name);
    if (!target || !hm.Create(kSaves, full, target, detour, original, &err) || !hm.Enable(full, &err))
        return ctx.Fail(std::format("cheated saves: hook {}: {}", name, err.empty() ? "method not found" : err));
    rvas += std::format("{}{} +0x{:X}", rvas.empty() ? "" : ", ", name, Resolver::Rva(target));
    return true;
}

}  // namespace

bool SaveToggle(const char* key) {
    const auto it = g_toggles.find(key);
    return it != g_toggles.end() && it->second;
}

void SetSaveToggle(const char* key, bool on) {
    if (SaveToggle(key) == on && g_toggles.count(key)) return;
    g_toggles[key] = on;
    // A marked save records it now; otherwise the mark written when Sandbox marks the save (or the next
    // game save of a marked one) takes it along.
    if (SaveCheated() && g_s.known && !g_s.file.empty() &&
        WriteMark(g_s.file, g_s.game, g_s.number, g_s.since, AchievementsPaused()))
        SR2_INFO("[{}] save '{}': {} {} - kept for this save", kModule, g_s.file, key, on ? "on" : "off");
}

unsigned SaveTogglesVersion() { return g_togglesVersion; }

bool SetupCheatedSave(sr2::core::ModuleContext& ctx) {
    auto& R = ctx.resolver();
    std::string why;
    auto cls = [&](const char* image, const char* ns, const char* name, std::string& w) -> Il2CppClass* {
        std::string err;
        Il2CppClass* k = R.Class(image, ns, name, &err);
        if (!k && w.empty()) w = err.empty() ? std::format("class {}.{} not found", ns, name) : err;
        return k;
    };
    auto off = [&](Il2CppClass* k, const char* field, size_t& out, std::string& w) {
        if (!k) return;
        std::string err;
        if (auto o = R.FieldOffset(k, field, &err)) out = *o;
        else if (w.empty()) w = err.empty() ? std::format("field {} not found", field) : err;
    };

    Il2CppClass* gameCtx = cls(kImage, "", "GameContext", why);
    Il2CppClass* sceneCtx = cls(kImage, "", "SceneContext", why);
    g_autoSaveClass = cls(kImage, "MonomiPark.SlimeRancher", "AutoSaveDirector", why);
    Il2CppClass* meta = cls(kImage, "MonomiPark.SlimeRancher", "GameMetadata", why);
    g_fileProviderClass = cls(kImage, "", "FileStorageProvider", why);
    if (!why.empty()) return ctx.Fail("cheated saves: " + why);
    std::string err;
    g_gameCtxInstance = MethodInParents(R, gameCtx, "get_Instance", 0, err);
    g_sceneCtxInstance = MethodInParents(R, sceneCtx, "get_Instance", 0, err);
    if (!g_gameCtxInstance || !g_sceneCtxInstance) return ctx.Fail("cheated saves: GameContext / SceneContext.Instance not found");
    off(gameCtx, "<AutoSaveDirector>k__BackingField", g_offGcAutoSave, why);
    off(sceneCtx, "<GameModel>k__BackingField", g_offScGameModel, why);
    off(g_autoSaveClass, "_currentGameMetadata", g_offAsdMetadata, why);
    off(g_autoSaveClass, "_storageProvider", g_offAsdProvider, why);
    off(meta, "GameName", g_offMetaGameName, why);
    off(meta, "SaveNumber", g_offMetaSaveNumber, why);
    off(g_fileProviderClass, "savePath", g_offProviderSavePath, why);
    if (!why.empty()) return ctx.Fail("cheated saves: " + why);
    if (g_offMetaGameName < 2 * sizeof(void*) || g_offMetaSaveNumber < 2 * sizeof(void*))
        return ctx.Fail("cheated saves: unexpected GameMetadata field offsets");
    g_offMetaGameName -= 2 * sizeof(void*);  // value-type field offsets include the object header
    g_offMetaSaveNumber -= 2 * sizeof(void*);

    std::string rvas;
    if (!Hook(ctx, "AutoSaveDirector.BeginLoad", R.Method(g_autoSaveClass, "BeginLoad", 2, &err),
              reinterpret_cast<void*>(&BeginLoadDetour), reinterpret_cast<void**>(&g_origBeginLoad), rvas) ||
        !Hook(ctx, "AutoSaveDirector.CleanupAutosaves", R.Method(g_autoSaveClass, "CleanupAutosaves", 1, &err),
              reinterpret_cast<void*>(&CleanupDetour), reinterpret_cast<void**>(&g_origCleanup), rvas))
        return false;

    {
        // Optional: the "Sandbox: Used" row in the save list's details panel. Missing names only turn it off.
        std::string mw;
        constexpr const char* kMenu = "MonomiPark.SlimeRancher.UI.MainMenu";
        g_rootClass = cls(kImage, kMenu, "SaveGamesRootUI", mw);
        g_summaryModelClass = cls(kImage, "MonomiPark.SlimeRancher.UI.MainMenu.Model", "GameSummaryModel", mw);
        g_summaryClass = cls(kImage, "MonomiPark.SlimeRancher.Persist", "Summary", mw);
        Il2CppClass* itemDef = cls(kImage, "MonomiPark.SlimeRancher.UI.MainMenu.Definition.GameSummary",
                                   "GameSummaryItemDefinition", mw);
        g_tarrDefClass = cls(kImage, "MonomiPark.SlimeRancher.UI.MainMenu.Definition.GameSummary",
                             "TarrEnabledGameSummaryItemDefinition", mw);
        g_tarrVmClass = cls(kImage, "MonomiPark.SlimeRancher.UI.MainMenu.ViewModel.GameSummaryItem",
                            "TarrEnabledGameSummaryItemViewModel", mw);
        off(g_rootClass, "_gameSummaryItems", g_offRootItems, mw);
        off(g_summaryModelClass, "_gameSummary", g_offModelSummary, mw);
        off(g_summaryClass, "SaveName", g_offSumSaveName, mw);
        off(g_summaryClass, "SaveNumber", g_offSumSaveNumber, mw);
        off(itemDef, "_icon", g_offDefIcon, mw);
        off(g_tarrDefClass, "_enabledString", g_offTarrOn, mw);
        off(g_tarrDefClass, "_disabledString", g_offTarrOff, mw);
        if (g_tarrVmClass) g_tarrVmCtor = R.Method(g_tarrVmClass, ".ctor", 2, &mw);
        g_objectNew = api().module ? reinterpret_cast<ObjectNewFn>(GetProcAddress(api().module, "il2cpp_object_new"))
                                   : nullptr;
        if (!g_objectNew && mw.empty()) mw = "il2cpp_object_new not found";
        std::string sw;
        g_spriteClass = cls("UnityEngine.CoreModule", "UnityEngine", "Sprite", sw);  // the icon only
        const MethodInfo* fetch = g_rootClass ? R.Method(g_rootClass, "FetchGameSummaryItemData", 1, &mw) : nullptr;
        if (mw.empty() && fetch && g_tarrVmCtor) {
            std::string hookErr;
            void* target = Resolver::Get().MethodPointer(fetch, &hookErr);
            auto& hm = sr2::core::HookManager::Get();
            const std::string full = std::format("{}.SaveGamesRootUI.FetchGameSummaryItemData", kSaves);
            if (target && hm.Create(kSaves, full, target, reinterpret_cast<void*>(&FetchDetour),
                                    reinterpret_cast<void**>(&g_origFetch), &hookErr) &&
                hm.Enable(full, &hookErr)) {
                g_menuOk = true;
                rvas += std::format(", SaveGamesRootUI.FetchGameSummaryItemData +0x{:X}", Resolver::Rva(target));
            } else {
                mw = hookErr;
            }
        }
        if (!g_menuOk) SR2_WARN("[{}] cheated saves: no \"{}\" row in the save list ({})", kModule, kRowText, mw);
    }

    if (!main_thread::AddFrameCallback(kSaves, [](float) { g_pollGuard.Run([] { Poll(); }); }))
        return ctx.Fail("cheated saves: could not add the main-thread frame callback");
    SR2_INFO("[{}] cheated-save hooks ready (always on): {}; marks in {}", kModule, rvas,
             Narrow(SidecarDir().wstring()));
    return true;
}

}  // namespace sr2::modules::sandbox
