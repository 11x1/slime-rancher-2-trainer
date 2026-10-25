// autotest: game access for the harness (game.h). Main thread only, except Resolve.

#include "modules/autotest/game.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <ctime>
#include <format>
#include <map>
#include <mutex>

#include "core/detour_guard.h"
#include "core/log.h"
#include "core/resolver.h"
#include "core/unity.h"
#include "modules/minimap/api.h"

namespace sr2::modules::autotest::game {
namespace {

namespace unity = sr2::core::unity;
using sr2::il2cpp::api;
using sr2::il2cpp::Resolver;

constexpr const char* kImage = "Assembly-CSharp";
constexpr size_t kHeader = 2 * sizeof(void*);  // object header before an instance's fields

struct Syms {
    // contexts
    Il2CppClass* gameCtx = nullptr;
    Il2CppClass* sceneCtx = nullptr;
    Il2CppClass* sysCtx = nullptr;
    const MethodInfo* gameCtxInstance = nullptr;
    const MethodInfo* sceneCtxInstance = nullptr;
    const MethodInfo* sysCtxInstance = nullptr;
    size_t gcAutoSave = 0, gcSettingsDirector = 0;
    size_t scGameModel = 0, scPlayerState = 0, scTimeDirector = 0, scPlayer = 0, scZoneTracker = 0;
    size_t sysLoader = 0;
    const MethodInfo* sessionEnded = nullptr;  // SceneContext.OnSessionEnded
    // scene loader
    Il2CppClass* loader = nullptr;
    size_t ldCurrentGroup = 0, ldLoading = 0, ldMainMenuGroup = 0;
    size_t sgSceneRefs = 0;  // SceneGroup._sceneReferences (AssetReference[])
    size_t arGuid = 0;       // AssetReference.m_AssetGUID
    const MethodInfo* ldIsMainMenu = nullptr;
    const MethodInfo* ldIsGameplay = nullptr;
    const MethodInfo* ldLoadMainMenu = nullptr;
    // saves
    Il2CppClass* autoSave = nullptr;
    size_t asMeta = 0;  // Nullable<GameMetadata>: hasValue at +0, the struct at +8
    size_t asStorage = 0;  // _storageProvider (FileStorageProvider on Steam: savePath)
    size_t metaName = 0, metaDisplay = 0, metaNumber = 0, metaSlot = 0;  // struct-relative
    const MethodInfo* availableGames = nullptr;
    const MethodInfo* beginLoad = nullptr;
    const MethodInfo* deleteGame = nullptr;
    const MethodInfo* saveAllNow = nullptr;
    const MethodInfo* startNewGame = nullptr;
    Il2CppClass* summary = nullptr;
    size_t smName = 0, smDisplay = 0, smSlot = 0, smTime = 0, smSaveName = 0, smNumber = 0, smInvalid = 0, smGameOver = 0;
    const MethodInfo* smIdentifier = nullptr;
    // new game settings
    Il2CppClass* newGameRoot = nullptr;
    size_t ngModel = 0;
    Il2CppClass* optionsList = nullptr;
    size_t olIdentifier = 0;
    const MethodInfo* olRuntime = nullptr;
    size_t rtGameOptions = 0;
    Il2CppClass* settingsModel = nullptr;
    const MethodInfo* settingsModelCtor = nullptr;
    const MethodInfo* settingsModelIcon = nullptr;  // SetGameIconForNewGame
    const MethodInfo* gameIcon = nullptr;           // GameSettingsDirector.get_GameIcon
    // misc
    const MethodInfo* appQuit = nullptr;
    const MethodInfo* trPosition = nullptr;
    const MethodInfo* trEuler = nullptr;
    const MethodInfo* currHour = nullptr;
    const MethodInfo* currDay = nullptr;
    const MethodInfo* worldTime = nullptr;
    const MethodInfo* fastForward = nullptr;
    const MethodInfo* hasPauser = nullptr;
    const MethodInfo* defaultCurrency = nullptr;
    const MethodInfo* getCurrency = nullptr;
    const MethodInfo* currentZone = nullptr;
    std::vector<std::pair<std::string, Il2CppClass*>> menus;  // UI roots for OpenMenus
    // screenshots: hiding the HUD
    Il2CppClass* hudUI = nullptr;
    Il2CppClass* canvas = nullptr;
    const MethodInfo* behGetEnabled = nullptr;
    const MethodInfo* behSetEnabled = nullptr;
};
Syms g_s;

std::string g_missing;
void Miss(const std::string& what) {
    if (!g_missing.empty()) g_missing += ", ";
    g_missing += what;
}

Il2CppClass* Cls(const char* image, const char* ns, const char* name) {
    std::string err;
    Il2CppClass* k = Resolver::Get().Class(image, ns, name, &err);
    if (!k) Miss(std::format("class {}.{}", ns, name));
    return k;
}

const MethodInfo* Meth(Il2CppClass* k, const char* name, int argc) {
    if (!k) return nullptr;
    const MethodInfo* m = MethodOn(k, name, argc);
    if (!m) Miss(std::format("{}({})", name, argc));
    return m;
}

size_t Off(Il2CppClass* k, const char* name) {
    if (!k) return 0;
    const size_t o = FieldOffset(k, name);
    if (!o) Miss(std::format("field {}", name));
    return o;
}

void Need(const void* p, const char* what) {
    if (!p) throw Error(std::string(what) + " is unavailable in this game version (see the setup line in sr2mods.log)");
}

Il2CppObject* Alive(Il2CppObject* o) { return o && unity::IsAlive(o) ? o : nullptr; }

// Every game's summaries: AvailableGamesByGameName() is a Dictionary<string, List<Summary>> built fresh
// per call. Its corefx layout (il2cpp-types.h of the dump): _buckets, _entries (Entry { int hashCode;
// int next; string key; List value } = 24 bytes) and _count; a free entry has next < -1 (none in a fresh
// dictionary, checked anyway). The stride check asks il2cpp_class_array_element_size for the ELEMENT class
// (il2cpp_class_get_element_class of the array's class): given the array class itself it answers
// sizeof(void*) for any SZARRAY, which was the "unexpected Dictionary entry size 8" of the first live run.
template <class F>
void ForEachSummary(F&& fn) {
    Il2CppObject* asd = AutoSave();
    if (!asd) throw Error("no AutoSaveDirector");
    Need(g_s.availableGames, "AutoSaveDirector.AvailableGamesByGameName");
    Il2CppObject* dict = Call(g_s.availableGames, asd, {}, "AutoSaveDirector.AvailableGamesByGameName");
    if (!dict) return;
    Il2CppClass* dk = api().il2cpp_object_get_class(dict);
    const size_t offEntries = FieldOffset(dk, "_entries"), offCount = FieldOffset(dk, "_count");
    if (!offEntries || !offCount) throw Error("unexpected Dictionary layout (no _entries / _count)");
    void* entries = unity::Field<void*>(dict, offEntries);
    const int count = unity::Field<int>(dict, offCount);
    const size_t len = unity::ArrayLength(entries);
    constexpr size_t kEntry = 24;
    using SizeFn = int (*)(Il2CppClass*);
    using ElemFn = Il2CppClass* (*)(Il2CppClass*);
    static auto elemSize = reinterpret_cast<SizeFn>(GetProcAddress(api().module, "il2cpp_class_array_element_size"));
    static auto elemClass = reinterpret_cast<ElemFn>(GetProcAddress(api().module, "il2cpp_class_get_element_class"));
    if (entries && elemSize && elemClass) {
        Il2CppClass* entry = elemClass(api().il2cpp_object_get_class(static_cast<Il2CppObject*>(entries)));
        const int es = entry ? elemSize(entry) : 0;
        if (es != static_cast<int>(kEntry)) throw Error(std::format("unexpected Dictionary entry size {}", es));
    }
    auto* base = unity::ArrayData<unsigned char>(entries);
    for (int i = 0; base && i < count && static_cast<size_t>(i) < len; ++i) {
        unsigned char* e = base + static_cast<size_t>(i) * kEntry;
        const int next = *reinterpret_cast<int*>(e + 4);
        auto* list = *reinterpret_cast<Il2CppObject**>(e + 16);
        if (next < -1 || !list) continue;
        for (size_t j = 0, n = unity::ListCount(list); j < n; ++j)
            if (auto* sm = static_cast<Il2CppObject*>(unity::ListAt(list, j))) fn(sm);
    }
}

std::string FormatTime(long long unix) {
    std::tm tm{};
    const std::time_t t = static_cast<std::time_t>(unix);
    if (gmtime_s(&tm, &t)) return {};
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
    return buf;
}

SaveInfo Info(Il2CppObject* sm) {
    SaveInfo s;
    s.gameName = Utf8(unity::Field<Il2CppString*>(sm, g_s.smName));
    s.displayName = Utf8(unity::Field<Il2CppString*>(sm, g_s.smDisplay));
    s.saveName = Utf8(unity::Field<Il2CppString*>(sm, g_s.smSaveName));
    s.slotIndex = unity::Field<int>(sm, g_s.smSlot);
    s.saveNumber = unity::Field<unsigned long long>(sm, g_s.smNumber);
    s.invalid = g_s.smInvalid && unity::Field<bool>(sm, g_s.smInvalid);
    s.gameOver = g_s.smGameOver && unity::Field<bool>(sm, g_s.smGameOver);
    if (g_s.smTime) {
        // DateTimeOffset { DateTime _dateTime (UTC ticks in the low 62 bits); short _offsetMinutes }
        const unsigned long long data = unity::Field<unsigned long long>(sm, g_s.smTime);
        const short offMin = unity::Field<short>(sm, g_s.smTime + 8);
        const long long ticks = static_cast<long long>(data & 0x3FFFFFFFFFFFFFFFull);
        constexpr long long kUnixEpochTicks = 621355968000000000LL;
        if (ticks > kUnixEpochTicks) {
            s.unixUtc = (ticks - kUnixEpochTicks) / 10000000LL;
            s.timeUtc = FormatTime(s.unixUtc);
            s.timeLocal = FormatTime(s.unixUtc + offMin * 60LL);  // the save's own UTC offset
        }
    }
    s.hasSummary = true;
    return s;
}

// ---- the save folder (the source of truth for which games and saves exist) ----
// FileStorageProvider.savePath = persistentDataPath\Steam\<steam id> (re/save.md); a save file is
// "<GameName>_<i>.sav" with GameName = "<yyyyMMddHHmmss>_<slot>" (StartNewGame adds "_<k>" only when that
// name already has data), so the rotation index is after the last '_' and the slot after the first.
std::wstring g_saveFolder;      // main thread only; found once
std::string g_saveFolderFrom;

bool AllDigits(std::string_view s) {
    return !s.empty() && std::all_of(s.begin(), s.end(), [](char c) { return c >= '0' && c <= '9'; });
}

// "<yyyyMMddHHmmss>_<slot>[_<k>...]_<i>" -> game name and slot (1-based); false for any other file.
bool ParseSaveName(const std::string& stem, std::string& gameName, int& slot) {
    const size_t last = stem.rfind('_');
    if (last == std::string::npos || !AllDigits(std::string_view(stem).substr(last + 1))) return false;
    gameName = stem.substr(0, last);
    if (gameName.size() < 16 || gameName[14] != '_' || !AllDigits(std::string_view(gameName).substr(0, 14))) return false;
    std::string_view rest = std::string_view(gameName).substr(15);
    const size_t us = rest.find('_');
    const std::string_view slotText = rest.substr(0, us);
    if (!AllDigits(slotText) || slotText.size() > 3) return false;
    for (std::string_view more = us == std::string_view::npos ? std::string_view() : rest.substr(us + 1); !more.empty();) {
        const size_t n = more.find('_');
        if (!AllDigits(more.substr(0, n))) return false;
        more = n == std::string_view::npos ? std::string_view() : more.substr(n + 1);
    }
    slot = std::stoi(std::string(slotText));
    return slot >= 1;
}

std::wstring Wide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string Narrow(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

bool IsDir(const std::wstring& p) {
    const DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

// The folder: AutoSaveDirector._storageProvider.savePath when it is a FileStorageProvider, else the one
// account folder under %USERPROFILE%\AppData\LocalLow\MonomiPark\SlimeRancher2\Steam.
const std::wstring& SaveFolder() {
    if (!g_saveFolder.empty()) return g_saveFolder;
    if (Il2CppObject* asd = AutoSave(); asd && g_s.asStorage) {
        Il2CppObject* sp = unity::Field<Il2CppObject*>(asd, g_s.asStorage);
        const size_t off = sp ? FieldOffset(api().il2cpp_object_get_class(sp), "savePath") : 0;
        const std::wstring p = off ? Wide(Utf8(unity::Field<Il2CppString*>(sp, off))) : std::wstring();
        if (!p.empty() && IsDir(p)) {
            g_saveFolder = p;
            g_saveFolderFrom = "storage provider";
        }
    }
    if (g_saveFolder.empty()) {
        wchar_t profile[MAX_PATH] = {};
        if (GetEnvironmentVariableW(L"USERPROFILE", profile, MAX_PATH)) {
            const std::wstring steam = std::wstring(profile) + L"\\AppData\\LocalLow\\MonomiPark\\SlimeRancher2\\Steam";
            std::vector<std::wstring> accounts;
            WIN32_FIND_DATAW fd;
            HANDLE h = FindFirstFileW((steam + L"\\*").c_str(), &fd);
            if (h != INVALID_HANDLE_VALUE) {
                do {
                    if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && fd.cFileName[0] != L'.') accounts.push_back(fd.cFileName);
                } while (FindNextFileW(h, &fd));
                FindClose(h);
            }
            if (accounts.size() == 1) {
                g_saveFolder = steam + L"\\" + accounts.front();
                g_saveFolderFrom = "LocalLow";
            } else if (accounts.size() > 1) {
                SR2_WARN("[{}] save folder: {} Steam account folders under LocalLow and no FileStorageProvider: not guessing",
                         kModule, accounts.size());
            }
        }
    }
    if (!g_saveFolder.empty()) SR2_INFO("[{}] save folder ({}): {}", kModule, g_saveFolderFrom, Narrow(g_saveFolder));
    return g_saveFolder;
}

long long UnixOf(const FILETIME& ft) {
    const unsigned long long t = (static_cast<unsigned long long>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    constexpr unsigned long long kUnixEpochFileTime = 116444736000000000ULL;  // 1970-01-01 in 100 ns since 1601
    return t > kUnixEpochFileTime ? static_cast<long long>((t - kUnixEpochFileTime) / 10000000ULL) : 0;
}

std::string FormatLocal(long long unix) {
    std::tm tm{};
    const std::time_t t = static_cast<std::time_t>(unix);
    if (localtime_s(&tm, &t)) return {};
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
    return buf;
}

std::vector<SaveInfo> SaveFiles(const std::wstring& folder) {
    std::vector<SaveInfo> out;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((folder + L"\\*.sav").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        const std::string file = Narrow(fd.cFileName);
        if (file.size() <= 4 || file.compare(file.size() - 4, 4, ".sav") != 0) continue;  // not "*.save" etc.
        SaveInfo s;
        int slot = 0;
        s.saveName = file.substr(0, file.size() - 4);
        if (!ParseSaveName(s.saveName, s.gameName, slot)) continue;  // profile, settings, anything else
        s.slotIndex = slot - 1;
        s.fileName = file;
        s.fileUnixUtc = UnixOf(fd.ftLastWriteTime);
        s.unixUtc = s.fileUnixUtc;
        s.timeUtc = FormatTime(s.unixUtc);
        s.timeLocal = s.fileTimeLocal = FormatLocal(s.unixUtc);
        s.displayName = std::to_string(slot);  // what StartNewGame names it; a summary may say otherwise
        out.push_back(std::move(s));
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return out;
}

}  // namespace

// ------------------------------------------------------------- helpers ------
Il2CppObject* Call(const MethodInfo* m, void* obj, std::initializer_list<void*> args, const char* what) {
    if (!m) throw Error(std::string(what) + " is unavailable");
    if (sr2::core::ModuleFaulted(kModule)) throw Error("the harness is faulted (see sr2mods.log); restart the game");
    std::vector<void*> params(args);
    std::string err;
    Il2CppObject* r = Resolver::Get().Invoke(m, obj, params.empty() ? nullptr : params.data(), &err);
    if (!err.empty()) {
        if (err.rfind("native exception", 0) == 0) {
            sr2::core::MarkModuleFaulted(kModule, std::format("{}: {}", what, err));
            throw Error(std::format("{}: native fault ({}); the harness is off until the game restarts", what, err));
        }
        throw Error(std::format("{} threw: {}", what, err));
    }
    return r;
}

const MethodInfo* MethodOn(Il2CppClass* k, const char* name, int argc) {
    for (Il2CppClass* c = k; c; c = api().il2cpp_class_get_parent ? api().il2cpp_class_get_parent(c) : nullptr)
        if (const MethodInfo* m = api().il2cpp_class_get_method_from_name(c, name, argc)) return m;
    return nullptr;
}

size_t FieldOffset(Il2CppClass* k, const char* name) {
    for (Il2CppClass* c = k; c; c = api().il2cpp_class_get_parent ? api().il2cpp_class_get_parent(c) : nullptr)
        if (FieldInfo* f = api().il2cpp_class_get_field_from_name(c, name)) return api().il2cpp_field_get_offset(f);
    return 0;
}

bool IsA(Il2CppObject* obj, Il2CppClass* k) {
    if (!obj || !k || !api().il2cpp_object_get_class) return false;
    Il2CppClass* c = api().il2cpp_object_get_class(obj);
    return c == k || (c && api().il2cpp_class_is_assignable_from && api().il2cpp_class_is_assignable_from(k, c));
}

std::string Utf8(Il2CppString* s) { return unity::ToUtf8(s); }

Il2CppClass* FindClass(const std::string& name) {
    static std::mutex m;
    static std::map<std::string, Il2CppClass*> cache;
    {
        std::lock_guard lk(m);
        if (auto it = cache.find(name); it != cache.end()) return it->second;
    }
    std::string ns, cls = name;
    if (const size_t dot = name.rfind('.'); dot != std::string::npos) {
        ns = name.substr(0, dot);
        cls = name.substr(dot + 1);
    }
    Il2CppClass* found = nullptr;
    const auto& a = api();
    for (const auto& img : Resolver::Get().Images()) {
        if (Il2CppClass* k = a.il2cpp_class_from_name(img.image, ns.c_str(), cls.c_str())) {
            found = k;
            break;
        }
    }
    // A bare name in some namespace: scan the class lists (once per name).
    if (!found && ns.empty() && a.il2cpp_image_get_class_count && a.il2cpp_image_get_class && a.il2cpp_class_get_name) {
        for (const auto& img : Resolver::Get().Images()) {
            const size_t n = a.il2cpp_image_get_class_count(img.image);
            for (size_t i = 0; i < n && !found; ++i) {
                auto* k = const_cast<Il2CppClass*>(a.il2cpp_image_get_class(img.image, i));
                if (const char* kn = k ? a.il2cpp_class_get_name(k) : nullptr; kn && cls == kn) found = k;
            }
            if (found) break;
        }
    }
    std::lock_guard lk(m);
    cache[name] = found;
    return found;
}

// --------------------------------------------------------------- resolve ----
void Resolve(std::string& report) {
    g_missing.clear();
    Syms& s = g_s;
    s.gameCtx = Cls(kImage, "", "GameContext");
    s.sceneCtx = Cls(kImage, "", "SceneContext");
    s.sysCtx = Cls(kImage, "", "SystemContext");
    s.gameCtxInstance = Meth(s.gameCtx, "get_Instance", 0);
    s.sceneCtxInstance = Meth(s.sceneCtx, "get_Instance", 0);
    s.sysCtxInstance = Meth(s.sysCtx, "get_Instance", 0);
    s.gcAutoSave = Off(s.gameCtx, "<AutoSaveDirector>k__BackingField");
    s.gcSettingsDirector = Off(s.gameCtx, "<GameSettingsDirector>k__BackingField");
    s.scGameModel = Off(s.sceneCtx, "<GameModel>k__BackingField");
    s.scPlayerState = Off(s.sceneCtx, "<PlayerState>k__BackingField");
    s.scTimeDirector = Off(s.sceneCtx, "<TimeDirector>k__BackingField");
    s.scPlayer = Off(s.sceneCtx, "player");
    s.scZoneTracker = Off(s.sceneCtx, "<PlayerZoneTracker>k__BackingField");
    s.sysLoader = Off(s.sysCtx, "<SceneLoader>k__BackingField");
    s.sessionEnded = Meth(s.sceneCtx, "OnSessionEnded", 0);

    s.loader = Cls(kImage, "MonomiPark.SlimeRancher.SceneManagement", "SceneLoader");
    s.ldCurrentGroup = Off(s.loader, "_currentSceneGroup");
    s.ldLoading = Off(s.loader, "<IsSceneLoadInProgress>k__BackingField");
    s.ldMainMenuGroup = Off(s.loader, "_mainMenuSceneGroup");
    s.sgSceneRefs = Off(Cls(kImage, "MonomiPark.SlimeRancher.SceneManagement", "SceneGroup"), "_sceneReferences");
    s.arGuid = Off(Cls("Unity.Addressables", "UnityEngine.AddressableAssets", "AssetReference"), "m_AssetGUID");
    s.ldIsMainMenu = Meth(s.loader, "IsCurrentSceneGroupMainMenu", 0);
    s.ldIsGameplay = Meth(s.loader, "IsCurrentSceneGroupGameplay", 0);
    s.ldLoadMainMenu = Meth(s.loader, "LoadMainMenuSceneGroup", 1);

    s.autoSave = Cls(kImage, "MonomiPark.SlimeRancher", "AutoSaveDirector");
    s.asMeta = Off(s.autoSave, "_currentGameMetadata");
    s.asStorage = Off(s.autoSave, "_storageProvider");
    if (Il2CppClass* meta = Cls(kImage, "MonomiPark.SlimeRancher", "GameMetadata")) {
        auto rel = [&](const char* f) {
            const size_t o = Off(meta, f);
            return o >= kHeader ? o - kHeader : 0;  // value-type field offsets include the object header
        };
        s.metaName = rel("GameName");
        s.metaDisplay = rel("DisplayName");
        s.metaNumber = rel("SaveNumber");
        s.metaSlot = rel("SlotIndex");
    }
    s.availableGames = Meth(s.autoSave, "AvailableGamesByGameName", 0);
    s.beginLoad = Meth(s.autoSave, "BeginLoad", 2);
    s.deleteGame = Meth(s.autoSave, "DeleteGame", 1);
    s.saveAllNow = Meth(s.autoSave, "SaveAllNow", 2);
    s.startNewGame = Meth(s.autoSave, "StartNewGame", 2);
    s.summary = Cls(kImage, "MonomiPark.SlimeRancher.Persist", "Summary");
    s.smName = Off(s.summary, "Name");
    s.smDisplay = Off(s.summary, "DisplayName");
    s.smSlot = Off(s.summary, "SaveSlotIndex");
    s.smTime = Off(s.summary, "SaveTimestamp");
    s.smSaveName = Off(s.summary, "SaveName");
    s.smNumber = Off(s.summary, "SaveNumber");
    s.smInvalid = Off(s.summary, "IsInvalid");
    s.smGameOver = Off(s.summary, "GameOver");
    s.smIdentifier = Meth(s.summary, "get_SaveIdentifier", 0);

    s.newGameRoot = Cls(kImage, "MonomiPark.SlimeRancher.UI.MainMenu", "NewGameOptionsUIRoot");
    s.ngModel = Off(s.newGameRoot, "_gameSettingsModel");
    s.optionsList = Cls(kImage, "MonomiPark.SlimeRancher.Options", "OptionsItemDefinitionList");
    s.olIdentifier = Off(s.optionsList, "_identifier");
    s.olRuntime = Meth(s.optionsList, "GetRuntime", 0);
    s.rtGameOptions = Off(Cls(kImage, "MonomiPark.SlimeRancher.Options", "RuntimeOptionsItemList"), "_gameOptions");
    s.settingsModel = Cls(kImage, "MonomiPark.SlimeRancher.DataModel", "GameSettingsModel");
    s.settingsModelCtor = Meth(s.settingsModel, ".ctor", 1);
    s.settingsModelIcon = Meth(s.settingsModel, "SetGameIconForNewGame", 1);
    s.gameIcon = Meth(Cls(kImage, "MonomiPark.SlimeRancher", "GameSettingsDirector"), "get_GameIcon", 0);

    s.appQuit = Meth(Cls("UnityEngine.CoreModule", "UnityEngine", "Application"), "Quit", 0);
    Il2CppClass* tr = Cls("UnityEngine.CoreModule", "UnityEngine", "Transform");
    s.trPosition = Meth(tr, "get_position", 0);
    s.trEuler = Meth(tr, "get_eulerAngles", 0);
    Il2CppClass* td = Cls(kImage, "", "TimeDirector");
    s.currHour = Meth(td, "CurrHour", 0);
    s.currDay = Meth(td, "CurrDay", 0);
    s.worldTime = Meth(td, "WorldTime", 0);
    s.fastForward = Meth(td, "FastForwardTo", 1);
    s.hasPauser = Meth(td, "HasPauser", 0);
    s.defaultCurrency = Meth(Cls(kImage, "MonomiPark.SlimeRancher.Economy", "CurrencyUtility"), "get_DefaultCurrency", 0);
    s.getCurrency = Meth(Cls(kImage, "", "PlayerState"), "GetCurrency", 1);
    s.currentZone = Meth(Cls(kImage, "", "PlayerZoneTracker"), "GetCurrentZone", 0);
    s.hudUI = Cls(kImage, "MonomiPark.SlimeRancher.UI", "HudUI");
    s.canvas = Cls("UnityEngine.UIModule", "UnityEngine", "Canvas");
    Il2CppClass* beh = Cls("UnityEngine.CoreModule", "UnityEngine", "Behaviour");
    s.behGetEnabled = Meth(beh, "get_enabled", 0);
    s.behSetEnabled = Meth(beh, "set_enabled", 1);

    // UI roots whose active instance means "that menu is open" (state.menus).
    const std::pair<const char*, const char*> menus[] = {
        {"MonomiPark.SlimeRancher.UI.MainMenu", "MainMenuLandingRootUI"},
        {"MonomiPark.SlimeRancher.UI.MainMenu", "SaveGamesRootUI"},
        {"MonomiPark.SlimeRancher.UI.MainMenu", "NewGameOptionsUIRoot"},
        {"MonomiPark.SlimeRancher.UI", "PauseMenuRoot"},
        {"MonomiPark.SlimeRancher.UI.Plot", "LandPlotUIRoot"},
        {"MonomiPark.SlimeRancher.UI.Fabricator", "FabricatorUI"},
        {"MonomiPark.SlimeRancher.UI.Refinery", "RefineryUI"},
        {"MonomiPark.SlimeRancher.UI.Map", "MapUI"},
        {"MonomiPark.SlimeRancher.UI.Pedia", "PediaRoot"},
        {"MonomiPark.SlimeRancher.UI.Drone", "DroneProgramMenu"},
        {"MonomiPark.SlimeRancher.UI", "RanchHouseUI"},
        {"MonomiPark.SlimeRancher.UI.Popup", "ConfirmPopup"},
    };
    s.menus.clear();
    for (const auto& [ns, name] : menus) {
        std::string err;
        if (Il2CppClass* k = Resolver::Get().Class(kImage, ns, name, &err)) s.menus.emplace_back(name, k);
    }
    report = g_missing;
}

// ----------------------------------------------------------- world state ----
Il2CppObject* GameCtx() { return g_s.gameCtxInstance ? Alive(Call(g_s.gameCtxInstance, nullptr, {}, "GameContext.Instance")) : nullptr; }

Il2CppObject* SceneCtx() {
    return g_s.sceneCtxInstance ? Alive(Call(g_s.sceneCtxInstance, nullptr, {}, "SceneContext.Instance")) : nullptr;
}

Il2CppObject* AutoSave() {
    Il2CppObject* gc = GameCtx();
    Il2CppObject* asd = gc && g_s.gcAutoSave ? Alive(unity::Field<Il2CppObject*>(gc, g_s.gcAutoSave)) : nullptr;
    return asd && IsA(asd, g_s.autoSave) ? asd : nullptr;
}

Il2CppObject* SceneLoader() {
    if (!g_s.sysCtxInstance || !g_s.sysLoader) return nullptr;
    Il2CppObject* sys = Alive(Call(g_s.sysCtxInstance, nullptr, {}, "SystemContext.Instance"));
    Il2CppObject* l = sys ? Alive(unity::Field<Il2CppObject*>(sys, g_s.sysLoader)) : nullptr;
    return l && IsA(l, g_s.loader) ? l : nullptr;
}

Il2CppObject* PlayerObject() {
    Il2CppObject* sc = SceneCtx();
    return sc && g_s.scPlayer ? Alive(unity::Field<Il2CppObject*>(sc, g_s.scPlayer)) : nullptr;
}

bool Loading() {
    Il2CppObject* l = SceneLoader();
    return !l || (g_s.ldLoading && unity::Field<bool>(l, g_s.ldLoading));
}

namespace {
// The scene GUIDs of a SceneGroup (its _sceneReferences' m_AssetGUID), empty ones left out.
std::vector<std::string> SceneGuids(Il2CppObject* group) {
    std::vector<std::string> out;
    void* arr = group && g_s.sgSceneRefs && g_s.arGuid ? unity::Field<void*>(group, g_s.sgSceneRefs) : nullptr;
    const size_t n = arr ? unity::ArrayLength(arr) : 0;
    for (size_t i = 0; i < n; ++i) {
        auto* ref = static_cast<Il2CppObject*>(unity::ArrayAt(arr, i));
        std::string g = ref ? Utf8(unity::Field<Il2CppString*>(ref, g_s.arGuid)) : std::string();
        if (!g.empty()) out.push_back(std::move(g));
    }
    return out;
}

// Whether a non-gameplay scene group shows the main menu. SceneLoader.IsCurrentSceneGroupMainMenu only
// compares with _mainMenuSceneGroup ("MainMenu", loaded by a quit to the title); the menu after launch is
// another group ("MainMenuFromBoot", PlatformEngagementPrompt.SceneGroupToLoad). A group counts as the menu
// when it loads every scene the main menu group loads (by Addressables GUID); a name starting with
// "MainMenu" is the last resort when that check fails. Decided once per group and logged.
bool IsMenuGroup(Il2CppObject* loader, Il2CppObject* group) {
    static Il2CppObject* lastGroup = nullptr;
    static bool lastResult = false;
    if (group == lastGroup) return lastResult;
    Il2CppObject* menu = g_s.ldMainMenuGroup ? Alive(unity::Field<Il2CppObject*>(loader, g_s.ldMainMenuGroup)) : nullptr;
    const std::vector<std::string> want = SceneGuids(menu), have = SceneGuids(group);
    const std::string name = unity::GetName(group);
    const bool byName = name.rfind("MainMenu", 0) == 0;
    bool result = false;
    std::string how;
    if (!want.empty() && !have.empty()) {
        result = std::all_of(want.begin(), want.end(),
                             [&](const std::string& g) { return std::find(have.begin(), have.end(), g) != have.end(); });
        how = std::format("{} the main menu group's {} scene(s) ({} in this group)", result ? "has all of" : "lacks some of",
                          want.size(), have.size());
    } else {
        how = std::format("no scene GUIDs (main menu group {}, this group {})", want.size(), have.size());
    }
    if (!result && byName) {
        result = true;
        how += "; accepted by its name";
    }
    SR2_INFO("[{}] scene group '{}': {} the title screen; {}", kModule, name, result ? "is" : "is not", how);
    lastGroup = group;
    lastResult = result;
    return result;
}
}  // namespace

bool AtTitle() {
    Il2CppObject* l = SceneLoader();
    if (!l || Loading() || !g_s.ldIsMainMenu) return false;
    if (CallValue<bool>(g_s.ldIsMainMenu, l, {}, "SceneLoader.IsCurrentSceneGroupMainMenu")) return true;
    if (g_s.ldIsGameplay && CallValue<bool>(g_s.ldIsGameplay, l, {}, "SceneLoader.IsCurrentSceneGroupGameplay")) return false;
    Il2CppObject* g = g_s.ldCurrentGroup ? Alive(unity::Field<Il2CppObject*>(l, g_s.ldCurrentGroup)) : nullptr;
    return g && IsMenuGroup(l, g);
}

bool InGame() {
    Il2CppObject* l = SceneLoader();
    if (!l || Loading() || !g_s.ldIsGameplay) return false;
    if (!CallValue<bool>(g_s.ldIsGameplay, l, {}, "SceneLoader.IsCurrentSceneGroupGameplay")) return false;
    Il2CppObject* sc = SceneCtx();
    return sc && g_s.scGameModel && unity::Field<Il2CppObject*>(sc, g_s.scGameModel) && PlayerObject();
}

std::string SceneName() {
    Il2CppObject* l = SceneLoader();
    Il2CppObject* g = l && g_s.ldCurrentGroup ? unity::Field<Il2CppObject*>(l, g_s.ldCurrentGroup) : nullptr;
    return g && unity::IsAlive(g) ? unity::GetName(g) : std::string();
}

bool Position(Il2CppObject* goOrComponent, Vec3f& out) {
    Il2CppObject* t = unity::TransformOf(goOrComponent);
    if (!t || !g_s.trPosition) return false;
    out = CallValue<Vec3f>(g_s.trPosition, t, {}, "Transform.get_position");
    return true;
}

bool Euler(Il2CppObject* goOrComponent, Vec3f& out) {
    Il2CppObject* t = unity::TransformOf(goOrComponent);
    if (!t || !g_s.trEuler) return false;
    out = CallValue<Vec3f>(g_s.trEuler, t, {}, "Transform.get_eulerAngles");
    return true;
}

Metadata MetadataOf(Il2CppObject* asd) {
    Metadata m;
    if (!asd || !g_s.asMeta) return m;
    auto* base = reinterpret_cast<unsigned char*>(asd) + g_s.asMeta;
    m.has = *reinterpret_cast<bool*>(base);
    if (!m.has) return m;
    unsigned char* v = base + 8;
    m.gameName = Utf8(*reinterpret_cast<Il2CppString**>(v + g_s.metaName));
    m.displayName = Utf8(*reinterpret_cast<Il2CppString**>(v + g_s.metaDisplay));
    m.saveNumber = *reinterpret_cast<unsigned long long*>(v + g_s.metaNumber);
    m.slotIndex = *reinterpret_cast<int*>(v + g_s.metaSlot);
    return m;
}

Metadata CurrentMetadata() { return MetadataOf(AutoSave()); }

bool NewerThan(const SaveInfo& a, const SaveInfo& b) {
    if (a.saveNumber && b.saveNumber && a.saveNumber != b.saveNumber) return a.saveNumber > b.saveNumber;
    const long long ta = a.fileUnixUtc ? a.fileUnixUtc : a.unixUtc, tb = b.fileUnixUtc ? b.fileUnixUtc : b.unixUtc;
    return ta > tb;
}

SaveList ListSaves() {
    SaveList r;
    const std::wstring& folder = SaveFolder();
    r.folder = Narrow(folder);
    r.folderFrom = g_saveFolderFrom;
    if (!folder.empty()) r.saves = SaveFiles(folder);
    // The summaries: extra details for the files (matched by save name). Without the folder they are the list.
    std::vector<SaveInfo> summaries;
    try {
        ForEachSummary([&](Il2CppObject* sm) { summaries.push_back(Info(sm)); });
    } catch (const Error& e) {
        r.summaryError = e.what();
    }
    for (SaveInfo& sm : summaries) {
        auto it = std::find_if(r.saves.begin(), r.saves.end(), [&](const SaveInfo& f) { return f.saveName == sm.saveName; });
        if (it == r.saves.end()) {
            if (folder.empty()) r.saves.push_back(std::move(sm));
            continue;  // a summary without a file: the file is the truth
        }
        if (sm.gameName != it->gameName || sm.slotIndex != it->slotIndex)
            SR2_WARN("[{}] save '{}': its summary says game '{}' slot {}, the file name game '{}' slot {} (file name used)",
                     kModule, it->saveName, sm.gameName, sm.slotIndex + 1, it->gameName, it->slotIndex + 1);
        it->displayName = sm.displayName;
        it->saveNumber = sm.saveNumber;
        it->invalid = sm.invalid;
        it->gameOver = sm.gameOver;
        if (sm.unixUtc) {
            it->unixUtc = sm.unixUtc;
            it->timeUtc = sm.timeUtc;
            it->timeLocal = sm.timeLocal;
        }
        it->hasSummary = true;
    }
    std::stable_sort(r.saves.begin(), r.saves.end(), [](const SaveInfo& a, const SaveInfo& b) {
        if (a.slotIndex != b.slotIndex) return a.slotIndex < b.slotIndex;
        if (a.gameName != b.gameName) return a.gameName < b.gameName;
        return NewerThan(a, b);
    });
    if (folder.empty() && !r.summaryError.empty())
        throw Error("no save list: the save folder was not found and the summaries failed: " + r.summaryError);
    return r;
}

std::vector<SaveInfo> Saves() { return ListSaves().saves; }

void BeginLoad(const SaveInfo& want) {
    Need(g_s.beginLoad, "AutoSaveDirector.BeginLoad");
    Il2CppObject* asd = AutoSave();
    if (!asd) throw Error("no AutoSaveDirector");
    if (want.gameName.empty() || want.saveName.empty()) throw Error("load: no game / save name");
    if (!g_saveFolder.empty() && !want.fileName.empty() &&
        GetFileAttributesW((g_saveFolder + L"\\" + Wide(want.fileName)).c_str()) == INVALID_FILE_ATTRIBUTES)
        throw Error("that save is gone: " + want.fileName);
    // GameSaveIdentifier { string GameName; string SaveName } (16 bytes, passed by reference). BeginLoad copies
    // it into its closure / coroutine at once; the strings stay reachable from this stack frame until then.
    struct Identifier {
        Il2CppString* gameName;
        Il2CppString* saveName;
    } id{unity::NewString(want.gameName), unity::NewString(want.saveName)};
    if (!id.gameName || !id.saveName) throw Error("il2cpp_string_new failed");
    Call(g_s.beginLoad, asd, {&id, nullptr}, "AutoSaveDirector.BeginLoad");
}

void DeleteGame(const std::string& gameName) {
    Need(g_s.deleteGame, "AutoSaveDirector.DeleteGame");
    Il2CppObject* asd = AutoSave();
    if (!asd) throw Error("no AutoSaveDirector");
    Call(g_s.deleteGame, asd, {unity::NewString(gameName)}, "AutoSaveDirector.DeleteGame");
}

bool SaveNow() {
    Need(g_s.saveAllNow, "AutoSaveDirector.SaveAllNow");
    Il2CppObject* asd = AutoSave();
    if (!asd) throw Error("no AutoSaveDirector");
    bool ignore = false;
    return CallValue<bool>(g_s.saveAllNow, asd, {&ignore, nullptr}, "AutoSaveDirector.SaveAllNow");
}

void StartNewGame(int slotIndex) {
    Need(g_s.startNewGame, "AutoSaveDirector.StartNewGame");
    Il2CppObject* asd = AutoSave();
    if (!asd) throw Error("no AutoSaveDirector");
    // The settings model the New Game screen would hand over: its own when it was ever opened, else one
    // built the way its Awake builds it (new GameSettingsModel(OptionsItemDefinitionList.GetRuntime()._gameOptions)).
    Il2CppObject* model = nullptr;
    std::string how;
    if (g_s.newGameRoot && g_s.ngModel) {
        for (Il2CppObject* root : unity::FindObjectsOfTypeAll(g_s.newGameRoot)) {
            if (Il2CppObject* m = unity::Field<Il2CppObject*>(root, g_s.ngModel)) {
                model = m;
                how = "the New Game screen's own settings";
                break;
            }
        }
    }
    if (!model) {
        Need(g_s.olRuntime, "OptionsItemDefinitionList.GetRuntime");
        Need(g_s.settingsModelCtor, "GameSettingsModel(IEnumerable)");
        Il2CppObject* best = nullptr;
        size_t bestCount = 0;
        bool bestNamed = false;
        std::string bestId;
        for (Il2CppObject* list : unity::FindObjectsOfTypeAll(g_s.optionsList)) {
            Il2CppObject* rt = Call(g_s.olRuntime, list, {}, "OptionsItemDefinitionList.GetRuntime");
            Il2CppObject* opts = rt && g_s.rtGameOptions ? unity::Field<Il2CppObject*>(rt, g_s.rtGameOptions) : nullptr;
            const size_t n = opts ? unity::ListCount(opts) : 0;
            if (!n) continue;
            std::string id = g_s.olIdentifier ? Utf8(unity::Field<Il2CppString*>(list, g_s.olIdentifier)) : std::string();
            std::string lower = id;
            std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            const bool named = lower.find("game") != std::string::npos;
            if (!best || (named && !bestNamed) || (named == bestNamed && n > bestCount)) {
                best = opts;
                bestCount = n;
                bestNamed = named;
                bestId = id;
            }
        }
        if (!best) throw Error("no game options list found to build the new game's settings");
        using NewFn = Il2CppObject* (*)(Il2CppClass*);
        static auto objectNew = reinterpret_cast<NewFn>(GetProcAddress(api().module, "il2cpp_object_new"));
        Need(reinterpret_cast<void*>(objectNew), "il2cpp_object_new");
        model = objectNew(g_s.settingsModel);
        if (!model) throw Error("il2cpp_object_new(GameSettingsModel) failed");
        Call(g_s.settingsModelCtor, model, {best}, "GameSettingsModel..ctor");
        how = std::format("a new GameSettingsModel from options list '{}' ({} game options)", bestId, bestCount);
    }
    // NewGameOptionsUIRoot.OnSubmit sets the icon chosen in the game settings first.
    if (g_s.gameIcon && g_s.settingsModelIcon && g_s.gcSettingsDirector) {
        Il2CppObject* gc = GameCtx();
        Il2CppObject* dir = gc ? Alive(unity::Field<Il2CppObject*>(gc, g_s.gcSettingsDirector)) : nullptr;
        if (Il2CppObject* icon = dir ? Call(g_s.gameIcon, dir, {}, "GameSettingsDirector.get_GameIcon") : nullptr)
            Call(g_s.settingsModelIcon, model, {icon}, "GameSettingsModel.SetGameIconForNewGame");
    }
    SR2_INFO("[{}] new game in save slot index {} (slot {}) with {}", kModule, slotIndex, slotIndex + 1, how);
    Call(g_s.startNewGame, asd, {&slotIndex, model}, "AutoSaveDirector.StartNewGame");
}

void QuitToTitle() {
    Need(g_s.sessionEnded, "SceneContext.OnSessionEnded");
    Need(g_s.ldLoadMainMenu, "SceneLoader.LoadMainMenuSceneGroup");
    Il2CppObject* l = SceneLoader();
    if (!l) throw Error("no SceneLoader");
    if (Il2CppObject* sc = SceneCtx()) Call(g_s.sessionEnded, sc, {}, "SceneContext.OnSessionEnded");
    Call(g_s.ldLoadMainMenu, l, {nullptr}, "SceneLoader.LoadMainMenuSceneGroup");
}

void QuitGame() {
    Need(g_s.appQuit, "Application.Quit");
    Call(g_s.appQuit, nullptr, {}, "Application.Quit");
}

namespace {
Il2CppObject* TimeDir() {
    Il2CppObject* sc = SceneCtx();
    return sc && g_s.scTimeDirector ? Alive(unity::Field<Il2CppObject*>(sc, g_s.scTimeDirector)) : nullptr;
}
}  // namespace

bool Clock(double& hour, int& day, double& worldTime) {
    Il2CppObject* td = TimeDir();
    if (!td || !g_s.currHour || !g_s.currDay || !g_s.worldTime) return false;
    hour = CallValue<float>(g_s.currHour, td, {}, "TimeDirector.CurrHour");
    day = CallValue<int>(g_s.currDay, td, {}, "TimeDirector.CurrDay");
    worldTime = CallValue<double>(g_s.worldTime, td, {}, "TimeDirector.WorldTime");
    return true;
}

void FastForwardTo(double worldTime) {
    Il2CppObject* td = TimeDir();
    if (!td) throw Error("no TimeDirector (not in game)");
    Call(g_s.fastForward, td, {&worldTime}, "TimeDirector.FastForwardTo");
}

bool Paused() {
    Il2CppObject* td = TimeDir();
    return td && g_s.hasPauser && CallValue<bool>(g_s.hasPauser, td, {}, "TimeDirector.HasPauser");
}

bool Newbucks(int& out) {
    Il2CppObject* sc = SceneCtx();
    Il2CppObject* ps = sc && g_s.scPlayerState ? Alive(unity::Field<Il2CppObject*>(sc, g_s.scPlayerState)) : nullptr;
    if (!ps || !g_s.defaultCurrency || !g_s.getCurrency) return false;
    Il2CppObject* cur = Call(g_s.defaultCurrency, nullptr, {}, "CurrencyUtility.get_DefaultCurrency");
    if (!cur) return false;
    out = CallValue<int>(g_s.getCurrency, ps, {cur}, "PlayerState.GetCurrency");
    return true;
}

std::string ZoneName() {
    Il2CppObject* sc = SceneCtx();
    Il2CppObject* tr = sc && g_s.scZoneTracker ? Alive(unity::Field<Il2CppObject*>(sc, g_s.scZoneTracker)) : nullptr;
    if (!tr || !g_s.currentZone) return {};
    Il2CppObject* z = Call(g_s.currentZone, tr, {}, "PlayerZoneTracker.GetCurrentZone");
    return z && unity::IsAlive(z) ? unity::GetName(z) : std::string();
}

std::vector<std::string> OpenMenus() {
    static unsigned long long at = 0;
    static std::vector<std::string> cached;
    const unsigned long long now = GetTickCount64();
    if (at && now - at < 1000) return cached;  // FindObjectOfType per class: not more often than this
    at = now;
    cached.clear();
    for (const auto& [name, k] : g_s.menus) {
        Il2CppObject* o = unity::FindObjectOfType(k);  // active instances only
        if (o && unity::IsAlive(o)) cached.push_back(name);
    }
    return cached;
}

namespace {
void Append(std::string& list, const char* what) {
    if (!list.empty()) list += ", ";
    list += what;
}
}  // namespace

HiddenUi HideUi(bool hud, bool minimap) {
    HiddenUi h;
    if (hud) {
        if (!g_s.hudUI || !g_s.canvas || !g_s.behGetEnabled || !g_s.behSetEnabled)
            throw Error("hiding the HUD is unavailable (HudUI, Canvas or Behaviour.enabled not resolved; see sr2mods.log)");
        // HudUI deactivates its own GameObject while the game hides the HUD: then there is nothing to hide.
        Il2CppObject* ui = Alive(unity::FindObjectOfType(g_s.hudUI));
        Il2CppObject* cv = ui ? Alive(unity::GetComponent(ui, g_s.canvas)) : nullptr;
        if (cv && CallValue<bool>(g_s.behGetEnabled, cv, {}, "Canvas.get_enabled")) {
            bool off = false;
            Call(g_s.behSetEnabled, cv, {&off}, "Canvas.set_enabled(false)");
            h.hudCanvas = unity::NewGCHandle(cv);
            Append(h.what, "HUD");
        }
    }
    if (hud || minimap) {
        // The minimap has a nested Canvas of its own under the HUD's: hidden by its root as well.
        sr2::modules::minimap::api::Snapshot snap;
        if (sr2::modules::minimap::api::Get(snap) && snap.visible && Alive(snap.root) && unity::ActiveInHierarchy(snap.root)) {
            unity::SetActive(snap.root, false);
            h.minimapRoot = unity::NewGCHandle(snap.root);
            Append(h.what, "minimap");
        }
    }
    return h;
}

void RestoreUi(HiddenUi& h) {
    if (h.hudCanvas) {
        Il2CppObject* cv = Alive(unity::GCHandleTarget(h.hudCanvas));
        unity::FreeGCHandle(h.hudCanvas);
        h.hudCanvas = nullptr;
        bool on = true;
        if (cv) Call(g_s.behSetEnabled, cv, {&on}, "Canvas.set_enabled(true)");
    }
    if (h.minimapRoot) {
        Il2CppObject* root = Alive(unity::GCHandleTarget(h.minimapRoot));
        unity::FreeGCHandle(h.minimapRoot);
        h.minimapRoot = nullptr;
        // Back on only while the minimap still wants to show (it may have hidden itself meanwhile: a map
        // opened, a scene change); it sets its root's active state itself only when that state changes.
        sr2::modules::minimap::api::Snapshot snap;
        if (root && sr2::modules::minimap::api::Get(snap) && snap.root == root && snap.visible) unity::SetActive(root, true);
    }
}

}  // namespace sr2::modules::autotest::game
