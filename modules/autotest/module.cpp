// autotest: the in-game test harness (see NOTES.md). Default OFF; while off there is no pipe and nothing
// is hooked or registered with the game.
//
// Threads:
//   * pipe.cpp's server thread (reads requests, quick commands, log tail) and command thread (one
//     request at a time). Neither touches Unity: game work is posted to the main thread (OnMain) and the
//     command thread waits for its result, with the command's timeout (args.timeout_s or a default).
//   * Unity main thread: the posted jobs, a frame callback (save guard hook upkeep, timed key releases,
//     a scheduled quit) and the save veto inside custom_upgrades' SaveGameImpl detour.
//   * Render thread (overlay): the question panel and screenshots, through ui/autotest_view.h.
//
// Save guard: while a client is connected ("armed"), every game save of a game outside slots 5 and 6 is
// skipped. It plugs into the one AutoSaveDirector.SaveGameImpl hook (custom_upgrades owns it:
// custom_upgrades::SetSaveVeto) and keeps that hook enabled while armed, also when custom_upgrades is off.

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <ctime>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

#include "core/build_info.h"
#include "core/detour_guard.h"
#include "core/hook_manager.h"
#include "core/log.h"
#include "core/main_thread.h"
#include "core/module.h"
#include "core/module_manager.h"
#include "core/paths.h"
#include "core/resolver.h"
#include "core/settings.h"
#include "core/unity.h"
#include "modules/autotest/api.h"
#include "modules/autotest/game.h"
#include "modules/autotest/pipe.h"
#include "modules/custom_upgrades/api.h"
#include "modules/map_tools/ground_teleport.h"
#include "ui/autotest_view.h"
#include "ui/overlay.h"

#ifndef SR2MODS_VERSION
#define SR2MODS_VERSION "dev"
#endif

namespace sr2::modules {
namespace autotest {
void RegisterGenericActions();                                                               // actions.cpp
bool WritePng(const std::filesystem::path& file, unsigned w, unsigned h, const unsigned char* rgba, std::string& why);  // png.cpp
}  // namespace autotest

namespace {

namespace at = sr2::modules::autotest;
namespace game = sr2::modules::autotest::game;
namespace view = sr2::ui::autotest;
namespace unity = sr2::core::unity;
namespace settings = sr2::core::settings;
namespace main_thread = sr2::core::main_thread;
namespace gt = sr2::modules::ground_teleport;
using at::Arr;
using at::Bool;
using at::Error;
using at::Num;
using at::Obj;
using at::Str;
using at::Vec3;
using sr2::json::Value;
using Clock = std::chrono::steady_clock;

constexpr const char* kModule = at::kModule;
constexpr double kDefaultTimeout = 10.0;

std::atomic<bool> g_armed{false};
std::atomic<bool> g_guardHookOurs{false};  // we enabled custom_upgrades' SaveGameImpl hook (it was off)
std::atomic<bool> g_guardActive{false};    // that hook exists and is enabled
sr2::core::DetourGuard g_vetoGuard{kModule, "save guard (SaveGameImpl veto)"};

// ------------------------------------------------------------ save guard ----
bool SaveVeto(Il2CppObject* asd) {
    if (!g_armed.load()) return false;
    bool veto = true;  // armed and the slot can't be told: skip, to be safe
    game::Metadata m;
    if (g_vetoGuard.Run([&] { m = game::MetadataOf(asd); })) {
        if (!m.has) return false;  // no game running: SaveGameImpl does nothing anyway
        const int slot = m.slotIndex + 1;
        veto = slot != 5 && slot != 6;
        if (veto)
            SR2_WARN("[{}] save guard: save of '{}' ({}, slot {}) skipped: the test harness is connected and may only save "
                     "slots 5 and 6",
                     kModule, m.displayName, m.gameName, slot);
    } else {
        SR2_WARN("[{}] save guard: could not read the game's slot: save skipped", kModule);
    }
    return veto;
}

bool HookEnabled(bool& exists) {
    exists = false;
    for (const auto& h : sr2::core::HookManager::Get().List())
        if (h.name == sr2::modules::custom_upgrades::kSaveHookName) {
            exists = true;
            return h.enabled;
        }
    return false;
}

// Armed: custom_upgrades' SaveGameImpl hook must be on (its detour asks the veto first, also with the
// module off). Any thread (HookManager locks; MinHook suspends threads itself).
void KeepGuardHook() {
    bool exists = false;
    const bool on = HookEnabled(exists);
    if (!exists) {
        if (g_guardActive.exchange(false) || !g_guardHookOurs.load())
            SR2_WARN("[{}] save guard unavailable: custom_upgrades' SaveGameImpl hook does not exist (custom_upgrades failed "
                     "to set up?); saves of slots 1-4 are not blocked",
                     kModule);
        g_guardHookOurs = true;  // (log once)
        return;
    }
    if (!on) {
        std::string err;
        if (sr2::core::HookManager::Get().Enable(sr2::modules::custom_upgrades::kSaveHookName, &err)) {
            g_guardHookOurs = true;
            SR2_INFO("[{}] save guard: enabled custom_upgrades' SaveGameImpl hook (custom_upgrades is off)", kModule);
        } else {
            SR2_WARN("[{}] save guard: cannot enable the SaveGameImpl hook: {}", kModule, err);
            g_guardActive = false;
            return;
        }
    }
    g_guardActive = true;
}

void ReleaseGuardHook() {
    if (g_guardHookOurs.exchange(false) && !sr2::core::ModuleManager::Get().IsEnabled("custom_upgrades")) {
        bool exists = false;
        if (HookEnabled(exists) && exists) {
            sr2::core::HookManager::Get().Disable(sr2::modules::custom_upgrades::kSaveHookName);
            SR2_INFO("[{}] save guard: SaveGameImpl hook back off (custom_upgrades is off)", kModule);
        }
    }
    g_guardActive = false;
}

// ---------------------------------------------------------- main thread -----
double TimeoutOf(const Value& args, double def) {
    const double t = args["timeout_s"].asNumber(def);
    return t > 0 ? std::min(t, 3600.0) : def;
}

struct Job {
    std::mutex m;
    std::condition_variable cv;
    bool done = false;
    Value result;
    std::string error;
};

// Runs fn on the Unity main thread and waits (command thread). Throws Error on failure / timeout / cancel.
Value OnMain(std::function<Value()> fn, Clock::time_point deadline, const std::atomic<bool>& cancel) {
    if (!main_thread::Ready()) throw Error("the game is not ready (main-thread dispatcher not running)");
    auto job = std::make_shared<Job>();
    main_thread::Post([job, fn = std::move(fn)] {
        Value r;
        std::string err;
        std::string guardErr;
        const bool ran = sr2::core::Try(
            [&] {
                try {
                    r = fn();
                } catch (const Error& e) {
                    err = e.what();
                } catch (const std::exception& e) {
                    err = std::string("internal error: ") + e.what();
                }
            },
            &guardErr);
        if (!ran) {
            err = "native fault in the harness: " + guardErr;
            sr2::core::MarkModuleFaulted(kModule, err);
        }
        {
            std::lock_guard lk(job->m);
            job->result = std::move(r);
            job->error = std::move(err);
            job->done = true;
        }
        job->cv.notify_all();
    });
    std::unique_lock lk(job->m);
    while (!job->done) {
        if (cancel.load()) throw Error("cancelled (the client left or the harness stopped)");
        if (Clock::now() >= deadline) throw Error("timeout");
        job->cv.wait_for(lk, std::chrono::milliseconds(50));
    }
    if (!job->error.empty()) throw Error(job->error);
    return std::move(job->result);
}

bool IsRetry(const Value& v) { return v.isObject() && v["retry"].asBool(false); }

// Calls step(attempt) on the main thread every ~100 ms until it returns a result that is not Retry().
Value Steps(std::function<Value(int)> step, Clock::time_point deadline, const std::atomic<bool>& cancel) {
    for (int attempt = 0;; ++attempt) {
        Value r = OnMain([step, attempt] { return step(attempt); }, deadline, cancel);
        if (!IsRetry(r)) return r;
        if (Clock::now() >= deadline) throw Error("timeout");
        for (int i = 0; i < 2 && !cancel.load(); ++i) Sleep(50);
    }
}

int SlotArg(const Value& args, const char* cmd) {
    const Value& v = args["slot"];
    if (!v.isNumber() || v.asNumber() != std::floor(v.asNumber()))
        throw Error(std::format("{} refused: \"slot\" must be 5 or 6", cmd));
    const int slot = static_cast<int>(v.asInt());
    if (slot != 5 && slot != 6) throw Error(std::format("{} refused: slot {} (only slots 5 and 6 are for tests)", cmd, slot));
    return slot;
}

Value NullOr(bool has, Value v) { return has ? std::move(v) : Value(); }

// The last check before a load or delete reaches the game: the save's slot (from its file / game name)
// must be 5 or 6, whatever picked it.
void GuardTestSlot(const game::SaveInfo& s, const char* what) {
    const int slot = s.slotIndex + 1;
    if (slot != 5 && slot != 6)
        throw Error(std::format("{} refused: '{}' is in slot {} (only slots 5 and 6 are for tests)", what, s.saveName, slot));
}

// ----------------------------------------------------------------- state ----
Value State() {
    Value r = Obj();
    const bool inGame = game::InGame();
    r.set("scene", Str(game::SceneName()));
    r.set("in_game", Bool(inGame));
    r.set("at_title", Bool(game::AtTitle()));
    r.set("loading", Bool(game::Loading()));
    r.set("paused", Bool(inGame && game::Paused()));
    const game::Metadata m = game::CurrentMetadata();
    const bool hasGame = inGame && m.has;
    r.set("game_name", NullOr(hasGame, Str(m.gameName)));
    r.set("display_name", NullOr(hasGame, Str(m.displayName)));
    r.set("slot", NullOr(hasGame, Num(m.slotIndex + 1)));
    r.set("save_number", NullOr(hasGame, Num(static_cast<double>(m.saveNumber))));
    game::Vec3f pos, rot;
    Il2CppObject* player = inGame ? game::PlayerObject() : nullptr;
    const bool hasPos = player && game::Position(player, pos);
    const bool hasRot = player && game::Euler(player, rot);
    r.set("pos", NullOr(hasPos, Vec3(pos.x, pos.y, pos.z)));
    r.set("rot", NullOr(hasRot, Vec3(rot.x, rot.y, rot.z)));
    std::string zone = inGame ? game::ZoneName() : std::string();
    r.set("zone", NullOr(!zone.empty(), Str(zone)));
    double hour = 0, world = 0;
    int day = 0;
    const bool clock = inGame && game::Clock(hour, day, world);
    r.set("time", NullOr(clock, Num(hour)));
    r.set("day", NullOr(clock, Num(day)));
    r.set("world_time", NullOr(clock, Num(world)));
    int nb = 0;
    const bool hasNb = inGame && game::Newbucks(nb);
    r.set("newbucks", NullOr(hasNb, Num(nb)));
    Value menus = Arr();
    for (const std::string& s : game::OpenMenus()) menus.push(Str(s));
    r.set("menus", std::move(menus));
    r.set("overlay_open", Bool(sr2::ui::overlay::IsOpen()));
    r.set("question_open", Bool(view::QuestionOpen()));
    r.set("save_guard", Bool(g_guardActive.load()));
    return r;
}

// ----------------------------------------------------------------- saves ----
// Every game, from the save folder's file names (slot, game name, files, file times), with the summaries'
// display name / save number / save time / invalid flag where they could be read. The result is the
// games array; the folder and a summary error go to the log.
Value SavesList() {
    Value games = Arr();
    const game::SaveList list = game::ListSaves();
    if (!list.summaryError.empty())
        SR2_WARN("[{}] saves_list: summaries unavailable ({}); listing from the files only", kModule, list.summaryError);
    const std::vector<game::SaveInfo>& all = list.saves;
    for (size_t i = 0; i < all.size();) {
        const std::string name = all[i].gameName;
        Value g = Obj();
        g.set("game_name", Str(name));
        g.set("display_name", Str(all[i].displayName));
        g.set("slot", Num(all[i].slotIndex + 1));
        g.set("slot_index", Num(all[i].slotIndex));
        g.set("last_save", Str(all[i].timeLocal));
        g.set("last_save_utc", Str(all[i].timeUtc));
        g.set("save_number", NullOr(all[i].saveNumber != 0, Num(static_cast<double>(all[i].saveNumber))));
        g.set("summaries", Bool(list.summaryError.empty()));
        g.set("folder", Str(list.folder));
        Value saves = Arr();
        for (; i < all.size() && all[i].gameName == name; ++i) {
            Value s = Obj();
            s.set("save_name", Str(all[i].saveName));
            s.set("file", NullOr(!all[i].fileName.empty(), Str(all[i].fileName)));
            s.set("file_time", NullOr(all[i].fileUnixUtc != 0, Str(all[i].fileTimeLocal)));
            s.set("save_number", NullOr(all[i].saveNumber != 0, Num(static_cast<double>(all[i].saveNumber))));
            s.set("time", Str(all[i].timeLocal));
            s.set("invalid", Bool(all[i].invalid));
            s.set("has_summary", Bool(all[i].hasSummary));
            saves.push(std::move(s));
        }
        g.set("saves", std::move(saves));
        games.push(std::move(g));
    }
    return games;
}

// Loads a save / starts a new game in slot 5 or 6, quitting the running game first (without saving).
// A main-thread state machine driven by Steps.
struct LoadJob {
    bool newGame = false;
    int slot = 0;
    unsigned long long saveNumber = 0;  // 0 = newest
    std::string saveName;               // load_save by file name ("" = by number / newest)
    bool replace = false;
    int phase = 0;
    game::SaveInfo target;
    std::vector<std::string> deleteFirst;
    unsigned long long since = 0;
};

Value LoadStep(LoadJob& j, int) {
    const unsigned long long now = GetTickCount64();
    const int index = j.slot - 1;
    switch (j.phase) {
        case 0: {
            std::vector<game::SaveInfo> all = game::Saves();
            if (j.newGame) {
                for (const auto& s : all)
                    if (s.slotIndex == index &&
                        std::find(j.deleteFirst.begin(), j.deleteFirst.end(), s.gameName) == j.deleteFirst.end())
                        j.deleteFirst.push_back(s.gameName);
                if (!j.deleteFirst.empty() && !j.replace)
                    throw Error(std::format("slot {} already has a game ({}): delete_slot first, or pass \"replace\": true",
                                            j.slot, j.deleteFirst.front()));
            } else {
                // The newest save of the slot (save number when the summaries were read, else the file time),
                // or the one asked for by save number / save name.
                const game::SaveInfo* best = nullptr;
                for (const auto& s : all) {
                    if (s.slotIndex != index || s.invalid) continue;
                    if (!j.saveName.empty()) {
                        if (s.saveName == j.saveName) best = &s;
                    } else if (j.saveNumber) {
                        if (s.saveNumber == j.saveNumber) best = &s;
                    } else if (!best || game::NewerThan(s, *best)) {
                        best = &s;
                    }
                }
                if (!best)
                    throw Error(!j.saveName.empty() ? std::format("slot {} has no save '{}'", j.slot, j.saveName)
                                : j.saveNumber      ? std::format("slot {} has no save number {} (save numbers need the "
                                                                  "summaries; pass \"save_name\" instead)",
                                                                  j.slot, j.saveNumber)
                                                    : std::format("slot {} is empty", j.slot));
                j.target = *best;
            }
            if (game::InGame()) {
                SR2_INFO("[{}] {}: quitting the running game to the title first (no save)", kModule,
                         j.newGame ? "new_game" : "load_slot");
                game::QuitToTitle();
            }
            j.phase = 1;
            return at::Retry();
        }
        case 1:  // the title screen, loaded and settled
            if (!game::AtTitle()) {
                j.since = 0;
                return at::Retry();
            }
            if (!j.since) j.since = now;
            if (now - j.since < 1500) return at::Retry();
            j.phase = 2;
            [[fallthrough]];
        case 2:
            if (j.newGame) {
                for (const std::string& g : j.deleteFirst) {
                    SR2_INFO("[{}] new_game: deleting '{}' in slot {} first (replace)", kModule, g, j.slot);
                    game::DeleteGame(g);
                }
                game::StartNewGame(index);
            } else {
                SR2_INFO("[{}] load: '{}' ({}, save {}) from slot {}", kModule, j.target.displayName, j.target.saveName,
                         j.target.saveNumber, j.slot);
                GuardTestSlot(j.target, "load");
                game::BeginLoad(j.target);
            }
            j.phase = 3;
            j.since = now;
            return at::Retry();
        case 3:  // in game, then a second to settle
            if (!game::InGame()) return at::Retry();
            j.phase = 4;
            j.since = now;
            return at::Retry();
        default: {
            if (!game::InGame()) {
                j.phase = 3;
                return at::Retry();
            }
            if (now - j.since < 1000) return at::Retry();
            const game::Metadata m = game::CurrentMetadata();
            if (m.has && m.slotIndex != index)
                throw Error(std::format("the game that loaded is in slot {}, not {}", m.slotIndex + 1, j.slot));
            return State();
        }
    }
}

// --------------------------------------------------------------- teleport ---
std::filesystem::path HarnessDir() { return sr2::paths::SelfDir() / L"sr2mods_autotest"; }

Value LoadPlaces() {
    std::ifstream f(HarnessDir() / L"places.json", std::ios::binary);
    if (!f) return Obj();
    std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    Value v;
    std::string err;
    return sr2::json::Parse(text, v, err) && v.isObject() ? v : Obj();
}

bool ReadVec(const Value& v, game::Vec3f& out, int& n) {
    n = 0;
    if (!v.isArray() || v.size() < 2 || v.size() > 3) return false;
    for (size_t i = 0; i < v.size(); ++i)
        if (!v.at(i).isNumber()) return false;
    n = static_cast<int>(v.size());
    out.x = static_cast<float>(v.at(0).asNumber());
    if (n == 3) {
        out.y = static_cast<float>(v.at(1).asNumber());
        out.z = static_cast<float>(v.at(2).asNumber());
    } else {
        out.z = static_cast<float>(v.at(1).asNumber());
    }
    return true;
}

struct GroundJob {
    std::mutex m;
    bool started = false, done = false;
    gt::Result result;
};

Value Teleport(const Value& args, Clock::time_point deadline, const std::atomic<bool>& cancel) {
    Value posArg = args["pos"];
    if (posArg.isNull() && args["to"].isString()) {
        const std::string to = args["to"].asString();
        Value places = LoadPlaces();
        if (const Value* p = places.find(to)) posArg = (*p)["pos"].isArray() ? (*p)["pos"] : *p;
        else throw Error("unknown place '" + to + "' (save one with exec place_save {name})");
    }
    game::Vec3f pos;
    int n = 0;
    if (!ReadVec(posArg, pos, n)) throw Error("teleport needs \"pos\" [x, y, z] (exact) or [x, z] (onto the ground), or \"to\"");
    game::Vec3f euler;
    bool haveRot = false;
    if (args["rot"].isNumber()) {
        euler.y = static_cast<float>(args["rot"].asNumber());
        haveRot = true;
    } else if (int rn = 0; args["rot"].isArray() && ReadVec(args["rot"], euler, rn) && rn == 3) {
        haveRot = true;
    }
    if (n == 3) {
        return OnMain(
            [pos, euler, haveRot] {
                if (!game::InGame()) throw Error("not in game");
                std::string why;
                gt::Vec3f p{pos.x, pos.y, pos.z}, e{euler.x, euler.y, euler.z};
                if (!gt::TeleportExact(kModule, p, haveRot ? &e : nullptr, &why)) throw Error("teleport refused: " + why);
                Value r = Obj();
                r.set("pos", Vec3(pos.x, pos.y, pos.z));
                return r;
            },
            deadline, cancel);
    }
    auto job = std::make_shared<GroundJob>();
    return Steps(
        [job, pos](int attempt) -> Value {
            if (attempt == 0) {
                if (!game::InGame()) throw Error("not in game");
                std::string why;
                if (!gt::Start(
                        kModule, pos.x, pos.z,
                        [job](const gt::Result& r) {
                            std::lock_guard lk(job->m);
                            job->result = r;
                            job->done = true;
                        },
                        &why))
                    throw Error("teleport refused: " + why);
                return at::Retry();
            }
            std::lock_guard lk(job->m);
            if (!job->done) return at::Retry();
            if (!job->result.ok) throw Error("teleport refused: " + job->result.why);
            Value r = Obj();
            r.set("pos", Vec3(job->result.to.x, job->result.to.y, job->result.to.z));
            r.set("seconds", Num(job->result.seconds));
            return r;
        },
        deadline, cancel);
}

Value PlaceSave(const Value& args) {
    const std::string name = args["name"].asString();
    if (name.empty()) throw Error("place_save needs \"name\"");
    Il2CppObject* p = game::InGame() ? game::PlayerObject() : nullptr;
    game::Vec3f pos, rot;
    if (!p || !game::Position(p, pos)) throw Error("not in game");
    game::Euler(p, rot);
    Value places = LoadPlaces();
    Value e = Obj();
    e.set("pos", Vec3(pos.x, pos.y, pos.z));
    e.set("rot", Vec3(rot.x, rot.y, rot.z));
    e.set("zone", Str(game::ZoneName()));
    places.set(name, e);
    std::error_code ec;
    std::filesystem::create_directories(HarnessDir(), ec);
    std::ofstream f(HarnessDir() / L"places.json", std::ios::binary | std::ios::trunc);
    const std::string text = sr2::json::Serialize(places);
    f.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!f) throw Error("cannot write places.json");
    return e;
}

// ------------------------------------------------------------------ find ----
using FieldGetTypeFn = const Il2CppType* (*)(FieldInfo*);
using TypeGetTypeFn = int (*)(const Il2CppType*);
using ClassIsEnumFn = bool (*)(const Il2CppClass*);
using FieldGetFlagsFn = int (*)(FieldInfo*);
using MethodRetFn = const Il2CppType* (*)(const MethodInfo*);

template <class T>
T Export(const char* name) {
    return reinterpret_cast<T>(GetProcAddress(sr2::il2cpp::api().module, name));
}

// A field / getter value at `data` (the field's storage; for a reference type the slot holding it).
Value Decode(const Il2CppType* t, const void* data) {
    static auto typeGetType = Export<TypeGetTypeFn>("il2cpp_type_get_type");
    static auto classIsEnum = Export<ClassIsEnumFn>("il2cpp_class_is_enum");
    const auto& a = sr2::il2cpp::api();
    if (!t || !typeGetType) return Str("?");
    const int e = typeGetType(t);
    auto rd = [data](auto v) { std::memcpy(&v, data, sizeof(v)); return v; };
    switch (e) {
        case 0x02: return Bool(rd(bool{}));
        case 0x03: return Num(rd(uint16_t{}));
        case 0x04: return Num(rd(int8_t{}));
        case 0x05: return Num(rd(uint8_t{}));
        case 0x06: return Num(rd(int16_t{}));
        case 0x07: return Num(rd(uint16_t{}));
        case 0x08: return Num(rd(int32_t{}));
        case 0x09: return Num(rd(uint32_t{}));
        case 0x0a: return Num(static_cast<double>(rd(int64_t{})));
        case 0x0b: return Num(static_cast<double>(rd(uint64_t{})));
        case 0x0c: return Num(rd(float{}));
        case 0x0d: return Num(rd(double{}));
        case 0x0e: return Str(unity::ToUtf8(rd(static_cast<Il2CppString*>(nullptr))));
        case 0x11: {  // value type
            Il2CppClass* k = a.il2cpp_class_from_type ? a.il2cpp_class_from_type(t) : nullptr;
            if (k && classIsEnum && classIsEnum(k)) return Num(rd(int32_t{}));
            char* tn = a.il2cpp_type_get_name ? a.il2cpp_type_get_name(t) : nullptr;
            const std::string name = tn ? tn : "";
            if (tn && a.il2cpp_free) a.il2cpp_free(tn);
            if (name == "UnityEngine.Vector3") {
                const auto v = rd(game::Vec3f{});
                return Vec3(v.x, v.y, v.z);
            }
            if (name == "UnityEngine.Vector2") {
                struct V2 {
                    float x, y;
                };
                const auto v = rd(V2{});
                Value arr = Arr();
                arr.push(Num(v.x));
                arr.push(Num(v.y));
                return arr;
            }
            if (name == "UnityEngine.Quaternion" || name == "UnityEngine.Color") {
                float q[4];
                std::memcpy(q, data, sizeof(q));
                Value arr = Arr();
                for (float f : q) arr.push(Num(f));
                return arr;
            }
            return Str("<" + name + ">");
        }
        default: {  // reference types
            auto* o = rd(static_cast<Il2CppObject*>(nullptr));
            if (!o) return Value();
            Value r = Obj();
            r.set("class", Str(unity::ClassName(o)));
            r.set("ptr", Str(std::format("0x{:X}", reinterpret_cast<uintptr_t>(o))));
            if (game::IsA(o, unity::GameObjectClass()) || game::IsA(o, unity::ComponentClass())) {
                if (unity::IsAlive(o)) r.set("name", Str(unity::GetName(o)));
            }
            return r;
        }
    }
}

Value ReadMember(Il2CppObject* obj, const std::string& name) {
    static auto fieldGetType = Export<FieldGetTypeFn>("il2cpp_field_get_type");
    static auto fieldFlags = Export<FieldGetFlagsFn>("il2cpp_field_get_flags");
    static auto methodRet = Export<MethodRetFn>("il2cpp_method_get_return_type");
    const auto& a = sr2::il2cpp::api();
    Il2CppClass* k = a.il2cpp_object_get_class(obj);
    for (Il2CppClass* c = k; c; c = a.il2cpp_class_get_parent ? a.il2cpp_class_get_parent(c) : nullptr) {
        FieldInfo* f = a.il2cpp_class_get_field_from_name(c, name.c_str());
        if (!f) continue;
        if (fieldFlags && (fieldFlags(f) & 0x10)) return Str("<static>");  // FIELD_ATTRIBUTE_STATIC
        const size_t off = a.il2cpp_field_get_offset(f);
        return Decode(fieldGetType ? fieldGetType(f) : nullptr, reinterpret_cast<char*>(obj) + off);
    }
    // A property getter instead.
    const std::string getter = "get_" + name;
    if (const MethodInfo* m = game::MethodOn(k, getter.c_str(), 0)) {
        Il2CppObject* r = game::Call(m, obj, {}, getter.c_str());
        const Il2CppType* rt = methodRet ? methodRet(m) : nullptr;
        static auto typeGetType = Export<TypeGetTypeFn>("il2cpp_type_get_type");
        if (!rt || !typeGetType) return Str("?");
        const int e = typeGetType(rt);
        const bool valueType = (e >= 0x02 && e <= 0x0d) || e == 0x11;
        if (valueType) {
            if (!r) return Value();
            return Decode(rt, reinterpret_cast<char*>(r) + 2 * sizeof(void*));
        }
        return Decode(rt, &r);
    }
    return Str("<no field or getter '" + name + "'>");
}

Value Find(const Value& args) {
    const std::string type = args["type"].asString();
    if (type.empty()) throw Error("find needs \"type\" (an IL2CPP class name, e.g. \"SlimeAppearanceApplicator\" or \"Ns.Class\")");
    Il2CppClass* k = game::FindClass(type);
    if (!k) throw Error("unknown class '" + type + "'");
    const bool isGo = k == unity::GameObjectClass();
    if (!isGo && (!unity::ComponentClass() || !sr2::il2cpp::api().il2cpp_class_is_assignable_from(unity::ComponentClass(), k)))
        throw Error("'" + type + "' is not a Component or GameObject type");
    std::string nameSub = args["name"].asString();
    std::transform(nameSub.begin(), nameSub.end(), nameSub.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    game::Vec3f nearPos{};
    int nn = 0;
    const bool hasNear = ReadVec(args["near"], nearPos, nn) && nn == 3;
    const double radius = args["radius"].asNumber(hasNear ? 30.0 : 0.0);
    const size_t limit = static_cast<size_t>(std::clamp<int64_t>(args["limit"].asInt(50), 1, 1000));
    const bool inactive = args["include_inactive"].asBool(false);
    struct Hit {
        Il2CppObject* o;
        game::Vec3f pos;
        bool hasPos;
        double dist;
    };
    std::vector<Hit> hits;
    size_t scanned = 0;
    for (Il2CppObject* o : unity::FindObjectsOfTypeAll(k)) {
        if (!unity::IsAlive(o)) continue;
        Il2CppObject* go = isGo ? o : unity::GameObjectOf(o);
        if (!go || !unity::IsAlive(go)) continue;
        if (!inactive && !unity::ActiveInHierarchy(go)) continue;  // also drops prefab assets
        ++scanned;
        if (!nameSub.empty()) {
            std::string n = unity::GetName(go);
            std::transform(n.begin(), n.end(), n.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (n.find(nameSub) == std::string::npos) continue;
        }
        Hit h{o, {}, false, 0};
        h.hasPos = game::Position(go, h.pos);
        if (hasNear) {
            if (!h.hasPos) continue;
            const double dx = h.pos.x - nearPos.x, dy = h.pos.y - nearPos.y, dz = h.pos.z - nearPos.z;
            h.dist = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (radius > 0 && h.dist > radius) continue;
        }
        hits.push_back(h);
    }
    if (hasNear) std::sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b) { return a.dist < b.dist; });
    Value list = Arr();
    for (size_t i = 0; i < hits.size() && i < limit; ++i) {
        const Hit& h = hits[i];
        Value e = Obj();
        e.set("id", Str(std::format("0x{:X}", reinterpret_cast<uintptr_t>(h.o))));
        Il2CppObject* go = isGo ? h.o : unity::GameObjectOf(h.o);
        e.set("name", Str(unity::GetName(go)));
        e.set("class", Str(unity::ClassName(h.o)));
        if (h.hasPos) e.set("pos", Vec3(h.pos.x, h.pos.y, h.pos.z));
        if (hasNear) e.set("distance", Num(h.dist));
        if (args["fields"].isArray()) {
            Value fields = Obj();
            for (const Value& f : args["fields"].items())
                if (f.isString()) fields.set(f.asString(), ReadMember(h.o, f.asString()));
            e.set("fields", std::move(fields));
        }
        list.push(std::move(e));
    }
    Value r = Obj();
    r.set("objects", std::move(list));
    r.set("matched", Num(static_cast<double>(hits.size())));
    r.set("scanned", Num(static_cast<double>(scanned)));
    return r;
}

// ----------------------------------------------------------------- input ----
// Game input events (MonomiPark.SlimeRancher.Input.InputEvent assets, which gameplay listens to), found
// by their asset name or their Input System action's name through the InputActionController assets.
struct InputSyms {
    bool tried = false;
    Il2CppClass* controller = nullptr;
    size_t bindings = 0, bindInput = 0, bindEvents = 0;
    const MethodInfo* startInput = nullptr;
    const MethodInfo* perform = nullptr;
    const MethodInfo* cancel = nullptr;
};
InputSyms g_in;

struct InputEventData {  // MonomiPark.SlimeRancher.Input.InputEventData (12 bytes)
    int source = 2;      // KEYBOARD
    float x = 1, y = 0;  // _vecValue: Value = x
};

struct TimedRelease {
    Il2CppObject* evt;  // an asset (kept alive by its InputActionController)
    unsigned long long at;
};
std::vector<TimedRelease> g_releases;  // main thread
unsigned long long g_quitAt = 0;       // main thread

void ResolveInput() {
    if (g_in.tried) return;
    g_in.tried = true;
    auto& R = sr2::il2cpp::Resolver::Get();
    std::string err;
    constexpr const char* ns = "MonomiPark.SlimeRancher.Input";
    g_in.controller = R.Class("Assembly-CSharp", ns, "InputActionController", &err);
    Il2CppClass* binding = R.Class("Assembly-CSharp", ns, "InputEventBinding", &err);
    Il2CppClass* evt = R.Class("Assembly-CSharp", ns, "InputEvent", &err);
    g_in.bindings = game::FieldOffset(g_in.controller, "_inputBindings");
    g_in.bindInput = game::FieldOffset(binding, "_input");
    g_in.bindEvents = game::FieldOffset(binding, "_inputEvents");
    g_in.startInput = evt ? R.MethodByParams(evt, "StartInput", {"MonomiPark.SlimeRancher.Input.InputEventData"}, &err) : nullptr;
    g_in.perform = evt ? R.MethodByParams(evt, "Perform", {"MonomiPark.SlimeRancher.Input.InputEventData"}, &err) : nullptr;
    g_in.cancel = evt ? R.MethodByParams(evt, "Cancel", {"MonomiPark.SlimeRancher.Input.InputEventData"}, &err) : nullptr;
}

std::string Lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// name "" = list every event (names into `all`).
Il2CppObject* FindInputEvent(const std::string& name, std::vector<std::string>* all) {
    ResolveInput();
    if (!g_in.controller || !g_in.bindings || !g_in.bindEvents) throw Error("the game's input events are unavailable");
    const std::string want = Lower(name);
    for (Il2CppObject* c : unity::FindObjectsOfTypeAll(g_in.controller)) {
        void* arr = unity::Field<void*>(c, g_in.bindings);
        for (size_t i = 0, n = unity::ArrayLength(arr); i < n; ++i) {
            auto* b = static_cast<Il2CppObject*>(unity::ArrayAt(arr, i));
            if (!b) continue;
            auto* ref = g_in.bindInput ? unity::Field<Il2CppObject*>(b, g_in.bindInput) : nullptr;
            const std::string action = ref && unity::IsAlive(ref) ? unity::GetName(ref) : std::string();
            void* evs = unity::Field<void*>(b, g_in.bindEvents);
            for (size_t j = 0, m = unity::ArrayLength(evs); j < m; ++j) {
                auto* e = static_cast<Il2CppObject*>(unity::ArrayAt(evs, j));
                if (!e || !unity::IsAlive(e)) continue;
                const std::string en = unity::GetName(e);
                if (all) {
                    all->push_back(action.empty() ? en : en + " (" + action + ")");
                    continue;
                }
                if (Lower(en) == want || (!action.empty() && (Lower(action) == want || Lower(action).ends_with("/" + want) ||
                                                              Lower(action).ends_with("." + want))))
                    return e;
            }
        }
    }
    return nullptr;
}

void PressEvent(Il2CppObject* e) {
    InputEventData d;
    game::Call(g_in.startInput, e, {&d}, "InputEvent.StartInput");
    game::Call(g_in.perform, e, {&d}, "InputEvent.Perform");
}

void ReleaseEvent(Il2CppObject* e) {
    InputEventData d;
    d.x = 0;
    game::Call(g_in.cancel, e, {&d}, "InputEvent.Cancel");
}

int VkOf(std::string key) {
    key = Lower(key);
    if (key.size() == 1) {
        const char c = key[0];
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) return std::toupper(static_cast<unsigned char>(c));
    }
    if (key.size() >= 2 && key[0] == 'f' && std::isdigit(static_cast<unsigned char>(key[1]))) {
        const int n = std::atoi(key.c_str() + 1);
        if (n >= 1 && n <= 24) return VK_F1 + n - 1;
    }
    static const std::pair<const char*, int> names[] = {
        {"space", VK_SPACE},       {"enter", VK_RETURN},       {"return", VK_RETURN},     {"escape", VK_ESCAPE},
        {"esc", VK_ESCAPE},        {"tab", VK_TAB},            {"backspace", VK_BACK},    {"shift", VK_LSHIFT},
        {"leftshift", VK_LSHIFT},  {"lshift", VK_LSHIFT},      {"rightshift", VK_RSHIFT}, {"ctrl", VK_LCONTROL},
        {"control", VK_LCONTROL},  {"leftctrl", VK_LCONTROL},  {"alt", VK_LMENU},         {"leftalt", VK_LMENU},
        {"up", VK_UP},             {"down", VK_DOWN},          {"left", VK_LEFT},         {"right", VK_RIGHT},
        {"delete", VK_DELETE},     {"insert", VK_INSERT},      {"home", VK_HOME},         {"end", VK_END},
        {"pageup", VK_PRIOR},      {"pagedown", VK_NEXT},      {"capslock", VK_CAPITAL},  {"mouse3", VK_MBUTTON},
        {"minus", VK_OEM_MINUS},   {"plus", VK_OEM_PLUS},      {"comma", VK_OEM_COMMA},   {"period", VK_OEM_PERIOD},
    };
    for (const auto& [n, vk] : names)
        if (key == n) return vk;
    return 0;
}

HWND GameWindow() {
    struct Ctx {
        DWORD pid;
        HWND found;
    } ctx{GetCurrentProcessId(), nullptr};
    EnumWindows(
        [](HWND h, LPARAM p) -> BOOL {
            auto* c = reinterpret_cast<Ctx*>(p);
            DWORD pid = 0;
            GetWindowThreadProcessId(h, &pid);
            if (pid != c->pid || !IsWindowVisible(h)) return TRUE;
            wchar_t cls[64] = {};
            GetClassNameW(h, cls, 64);
            if (std::wstring(cls) == L"UnityWndClass") {
                c->found = h;
                return FALSE;
            }
            if (!c->found) c->found = h;
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&ctx));
    return ctx.found;
}

void SendKey(int vk, bool down) {
    INPUT in{};
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = static_cast<WORD>(vk);
    in.ki.wScan = static_cast<WORD>(MapVirtualKeyW(static_cast<UINT>(vk), MAPVK_VK_TO_VSC));
    in.ki.dwFlags = down ? 0 : KEYEVENTF_KEYUP;
    if (vk == VK_UP || vk == VK_DOWN || vk == VK_LEFT || vk == VK_RIGHT || vk == VK_DELETE || vk == VK_INSERT ||
        vk == VK_HOME || vk == VK_END || vk == VK_PRIOR || vk == VK_NEXT)
        in.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
    SendInput(1, &in, sizeof(in));
}

Value Input(const Value& args, Clock::time_point deadline, const std::atomic<bool>& cancel) {
    const bool press = args["press"].asBool(false), release = args["release"].asBool(false);
    const bool tap = args["tap"].asBool(!press && !release);
    const int holdMs = static_cast<int>(std::clamp<int64_t>(args["hold_ms"].asInt(80), 10, 5000));
    Value r = Obj();
    r.set("mode", Str(press ? "press" : release ? "release" : "tap"));
    if (args["key"].isString()) {
        const int vk = VkOf(args["key"].asString());
        if (!vk) throw Error("unknown key '" + args["key"].asString() + "'");
        HWND w = GameWindow();
        if (!w || GetForegroundWindow() != w)
            throw Error("the game window is not in front: a simulated key would go to another window");
        if (press || tap) SendKey(vk, true);
        if (tap) Sleep(static_cast<DWORD>(holdMs));
        if (release || tap) SendKey(vk, false);
        r.set("key", Str(args["key"].asString()));
        return r;
    }
    const std::string action = args["action"].asString();
    if (action.empty()) throw Error("input needs \"action\" (a game input event or action name) or \"key\"");
    return OnMain(
        [action, press, release, tap, holdMs, r]() mutable {
            Il2CppObject* e = FindInputEvent(action, nullptr);
            if (!e) throw Error("unknown input action '" + action + "' (probe list_inputs lists them)");
            if (!g_in.startInput || !g_in.perform || !g_in.cancel) throw Error("InputEvent methods unavailable");
            if (press || tap) PressEvent(e);
            if (release) ReleaseEvent(e);
            if (tap) g_releases.push_back({e, GetTickCount64() + static_cast<unsigned long long>(holdMs)});
            r.set("event", Str(unity::GetName(e)));
            return r;
        },
        deadline, cancel);
}

// ------------------------------------------------------------ screenshot ----
std::string SafeName(std::string s) {
    for (char& c : s)
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_') c = '_';
    if (s.size() > 60) s.resize(60);
    return s.empty() ? "shot" : s;
}

// Waits on the command thread for the settings window to show a page (autotest_view.h RequestPage). The
// overlay must be open. Returns "<cat> / <tab>".
std::string ShowPage(const Value& cat, const Value& tab, Clock::time_point deadline, const std::atomic<bool>& cancel) {
    const std::string tabText = tab.isNumber() ? std::to_string(tab.asInt()) : tab.asString();
    const uint64_t id = view::RequestPage(cat.asString(), tabText);
    for (;;) {
        std::string detail;
        const int r = view::PageResult(id, detail);
        if (r > 0) return detail;
        if (r < 0) throw Error(detail);
        if (cancel.load()) throw Error("cancelled (the client left or the harness stopped)");
        if (Clock::now() >= deadline) throw Error("timeout (the mod menu did not draw: overlay unavailable?)");
        Sleep(30);
    }
}

void Settle(long long ms, const std::atomic<bool>& cancel) {
    for (long long t = 0; t < ms && !cancel.load(); t += 25) Sleep(25);
}

// `overlay`+: opens or closes the mod menu, optionally at a page (for screenshots of the menu).
Value OverlayCmd(const Value& args, Clock::time_point deadline, const std::atomic<bool>& cancel) {
    Value r = Obj();
    if (!args["open"].asBool(true)) {
        sr2::ui::overlay::SetOpen(false);
        r.set("open", Bool(false));
        return r;
    }
    sr2::ui::overlay::SetOpen(true);
    if (!args["cat"].isNull() || !args["tab"].isNull()) r.set("page", Str(ShowPage(args["cat"], args["tab"], deadline, cancel)));
    r.set("open", Bool(sr2::ui::overlay::IsOpen()));
    return r;
}

// Puts back what a screenshot changed (scope exit, also on errors). The UI goes back on the main thread
// without waiting (jobs run in order, so before the next command's).
struct ShotRestore {
    std::shared_ptr<game::HiddenUi> ui;
    bool closeMenu = false;
    ShotRestore() = default;
    ShotRestore(const ShotRestore&) = delete;
    ShotRestore& operator=(const ShotRestore&) = delete;
    ~ShotRestore() {
        if (closeMenu) sr2::ui::overlay::SetOpen(false);
        if (!ui || (!ui->hudCanvas && !ui->minimapRoot)) return;
        main_thread::Post([ui = ui] {
            std::string why;
            if (!sr2::core::Try(
                    [&] {
                        try {
                            game::RestoreUi(*ui);
                        } catch (const Error& e) {
                            SR2_WARN("[{}] screenshot: could not show the UI again: {}", kModule, e.what());
                        }
                    },
                    &why))
                sr2::core::MarkModuleFaulted(kModule, "native fault restoring the UI after a screenshot: " + why);
        });
    }
};

Value Screenshot(const Value& args, double timeoutS, Clock::time_point deadline, const std::atomic<bool>& cancel) {
    const bool hideHud = args["hide_hud"].asBool(false);
    const bool hideMinimap = args["hide_minimap"].asBool(false);
    const bool hideHarness = args["hide_harness"].asBool(false);
    const Value& menu = args["menu"];
    const bool withMenu = menu.isObject();
    const bool overlay = args["overlay"].asBool(false) || withMenu;
    ShotRestore restore;
    std::string changed;
    if (withMenu) {
        restore.closeMenu = !sr2::ui::overlay::IsOpen();
        sr2::ui::overlay::SetOpen(true);
        changed = "menu " + ShowPage(menu["cat"], menu["tab"], deadline, cancel);
    }
    if (hideHud || hideMinimap) {
        auto hidden = std::make_shared<game::HiddenUi>();
        restore.ui = hidden;
        OnMain(
            [hidden, hideHud, hideMinimap] {
                *hidden = game::HideUi(hideHud, hideMinimap);
                return Value();
            },
            deadline, cancel);
        if (!hidden->what.empty()) changed += (changed.empty() ? "hid " : ", hid ") + hidden->what;
    }
    // Hidden / opened things show up (or vanish) a frame or two later: wait a little before the frame.
    Settle(static_cast<long long>(args["settle_ms"].asNumber(changed.empty() ? 0 : 300)), cancel);
    view::Shot shot = view::TakeShot(overlay, static_cast<long long>(std::min(timeoutS, 30.0) * 1000), hideHarness);
    if (!shot.ok) throw Error("screenshot failed: " + shot.error);
    if (!changed.empty()) SR2_INFO("[{}] screenshot '{}': {}", kModule, args["name"].asString(), changed);
    SYSTEMTIME st;
    GetLocalTime(&st);
    const std::string file = std::format("{}_{:04}{:02}{:02}-{:02}{:02}{:02}-{:03}.png", SafeName(args["name"].asString()), st.wYear,
                                         st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    const std::filesystem::path path = HarnessDir() / L"shots" / std::filesystem::path(file);
    std::string why;
    if (!at::WritePng(path, shot.width, shot.height, shot.rgba.data(), why)) throw Error("screenshot not written: " + why);
    Value r = Obj();
    r.set("path", Str(sr2::log::Narrow(path.wstring())));
    r.set("width", Num(shot.width));
    r.set("height", Num(shot.height));
    return r;
}

// ------------------------------------------------------------------- ask ----
std::atomic<uint64_t> g_askId{0};

void Flash(bool on) {
    HWND w = GameWindow();
    if (!w) return;
    FLASHWINFO fi{};
    fi.cbSize = sizeof(fi);
    fi.hwnd = w;
    fi.dwFlags = on ? (FLASHW_ALL | FLASHW_TIMERNOFG) : FLASHW_STOP;
    fi.uCount = 0;
    FlashWindowEx(&fi);
}

Value AnswerValue(const view::Answer& a, bool timedOut) {
    Value r = Obj();
    r.set("answer", Str(timedOut ? "timeout" : a.cancelled ? "cancelled" : a.choice));
    r.set("note", Str(a.note));
    return r;
}

Value Ask(const Value& args, const std::atomic<bool>& cancel) {
    view::Question q;
    q.text = args["question"].asString();
    if (q.text.empty()) throw Error("ask needs \"question\"");
    for (const Value& o : args["options"].items())
        if (o.isString() && !o.asString().empty()) q.options.push_back(o.asString());
    if (q.options.size() < 2 || q.options.size() > 4) throw Error("ask needs 2-4 \"options\"");
    q.caseId = args["case"].asString();
    q.step = static_cast<int>(args["step"].asInt(0));
    q.id = ++g_askId;
    const long long timeoutMs = static_cast<long long>(args["timeout_s"].asNumber(600) * 1000);
    view::Ask(q);
    Flash(true);
    SR2_INFO("[{}] ask #{}{}: {}", kModule, q.id, q.caseId.empty() ? "" : " (" + q.caseId + ")", q.text);
    if (!args["wait"].asBool(true)) {  // answer later as an event
        const uint64_t id = q.id;
        std::thread([id, timeoutMs] {
            view::Answer a;
            const bool got = view::WaitAnswer(id, timeoutMs, a, nullptr);
            if (!got) view::CancelAsk(id);
            Flash(false);
            Value ev = AnswerValue(a, !got);
            ev.set("event", Str("answer"));
            ev.set("ask_id", Num(static_cast<double>(id)));
            at::pipe::SendEvent(ev);
        }).detach();
        Value r = Obj();
        r.set("pending", Bool(true));
        r.set("ask_id", Num(static_cast<double>(q.id)));
        return r;
    }
    view::Answer a;
    const bool got = view::WaitAnswer(q.id, timeoutMs, a, &cancel);
    if (!got) view::CancelAsk(q.id);
    Flash(false);
    SR2_INFO("[{}] ask #{}: {}{}", kModule, q.id, got ? (a.cancelled ? "cancelled" : "\"" + a.choice + "\"") : "timeout",
             a.note.empty() ? "" : " (note: " + a.note + ")");
    if (!got && cancel.load()) throw Error("cancelled (the client left or the harness stopped)");
    return AnswerValue(a, !got);
}

// -------------------------------------------------------------- settings ----
bool SplitKey(const std::string& full, std::string& module, std::string& key) {
    const size_t dot = full.find('.');
    if (dot == std::string::npos || dot == 0 || dot + 1 >= full.size()) return false;
    module = full.substr(0, dot);
    key = full.substr(dot + 1);
    return true;
}

Value SettingValue(const settings::Def& d) {
    const double v = settings::Get(d.module, d.key);
    return d.kind == settings::Kind::Bool ? Bool(v != 0) : Num(v);
}

Value SettingsGet(const Value& args) {
    std::string module, key;
    if (!SplitKey(args["key"].asString(), module, key)) throw Error("settings key must be \"<module>.<key>\"");
    for (const settings::Def& d : settings::All()) {
        if (d.module != module || d.key != key) continue;
        Value r = Obj();
        r.set("key", Str(module + "." + key));
        r.set("value", SettingValue(d));
        const std::string lock = settings::LockReason(module, key);
        r.set("locked", Bool(!lock.empty()));
        r.set("lock_reason", Str(lock));
        r.set("tooltip", Str(d.tooltip));
        r.set("label", Str(d.label));
        static const char* kinds[] = {"bool", "float", "int", "enum", "key"};
        r.set("kind", Str(kinds[static_cast<int>(d.kind)]));
        r.set("default", Num(d.def));
        r.set("min", Num(d.min));
        r.set("max", Num(d.max));
        if (d.kind == settings::Kind::Enum) {
            Value opts = Arr();
            for (const auto& o : d.options) opts.push(Str(o));
            r.set("options", std::move(opts));
        }
        return r;
    }
    if (settings::Saved(module, key) || !settings::GetString(module, key).empty()) {
        Value r = Obj();
        r.set("key", Str(module + "." + key));
        r.set("value", Str(settings::GetString(module, key)));
        r.set("locked", Bool(false));
        return r;
    }
    throw Error("unknown setting '" + module + "." + key + "'");
}

Value SettingsSet(const Value& args) {
    std::string module, key;
    if (!SplitKey(args["key"].asString(), module, key)) throw Error("settings key must be \"<module>.<key>\"");
    if (!settings::Has(module, key)) throw Error("unknown setting '" + module + "." + key + "'");
    const Value& v = args["value"];
    double d = 0;
    if (v.isBool()) d = v.asBool() ? 1 : 0;
    else if (v.isNumber()) d = v.asNumber();
    else throw Error("\"value\" must be a number or a bool");
    const std::string lock = settings::LockReason(module, key);
    if (!lock.empty() && settings::Get(module, key) != d) throw Error("locked: " + lock);
    if (module == kModule && key == "enabled" && d == 0)
        SR2_WARN("[{}] the client turned the harness off (the pipe closes)", kModule);
    settings::Set(module, key, d);  // as the Options tab does (module switches apply on the next tick)
    SR2_INFO("[{}] settings_set {}.{} = {}", kModule, module, key, d);
    return SettingsGet(args);
}

// --------------------------------------------------------------- commands ---
const char* const kCommands[] = {"ping",         "state",       "log_mark",  "settings_get", "settings_set", "saves_list",
                                 "load_slot",    "load_save",   "new_game",  "save",         "quit_to_title", "quit_game",
                                 "delete_slot",  "teleport",    "find",      "probe",        "exec",         "input",
                                 "screenshot",   "ask",         "cancel_ask", "subscribe_log", "list_names",   "overlay"};

double DefaultTimeout(const std::string& cmd) {
    if (cmd == "load_slot" || cmd == "load_save") return 120;
    if (cmd == "new_game") return 180;
    if (cmd == "quit_to_title") return 60;
    if (cmd == "save" || cmd == "delete_slot") return 30;
    if (cmd == "screenshot") return 15;
    if (cmd == "teleport") return 20;
    return kDefaultTimeout;
}

Value NamedCall(bool probe, const Value& args, Clock::time_point deadline, const std::atomic<bool>& cancel) {
    const std::string name = args["name"].asString();
    at::Fn fn;
    if (name.empty() || !at::FindFn(probe, name, fn))
        throw Error(std::format("unknown {} '{}'", probe ? "probe" : "action", name));
    Value inner = args["args"].isObject() ? args["args"] : Obj();
    return Steps(
        [fn, inner](int attempt) mutable {
            inner.set("_attempt", Num(attempt));
            return fn(inner);
        },
        deadline, cancel);
}

Value Handle(const std::string& cmd, const Value& args, const std::atomic<bool>& cancel) {
    if (sr2::core::ModuleFaulted(kModule) && cmd != "screenshot" && cmd != "ask")
        throw Error("the harness is faulted (see sr2mods.log); restart the game");
    const double timeoutS = TimeoutOf(args, DefaultTimeout(cmd));
    const auto deadline = Clock::now() + std::chrono::milliseconds(static_cast<long long>(timeoutS * 1000));
    if (cmd == "state") return OnMain([] { return State(); }, deadline, cancel);
    if (cmd == "saves_list") return OnMain([] { return SavesList(); }, deadline, cancel);
    if (cmd == "settings_set") return OnMain([args] { return SettingsSet(args); }, deadline, cancel);
    if (cmd == "load_slot" || cmd == "load_save" || cmd == "new_game") {
        auto job = std::make_shared<LoadJob>();
        job->newGame = cmd == "new_game";
        job->slot = SlotArg(args, cmd.c_str());
        job->replace = args["replace"].asBool(false);
        if (cmd == "load_save") {
            if (args["save_name"].isString() && !args["save_name"].asString().empty()) {
                job->saveName = args["save_name"].asString();
                if (job->saveName.size() > 4 && job->saveName.ends_with(".sav")) job->saveName.resize(job->saveName.size() - 4);
            } else if (args["save_number"].isNumber() && args["save_number"].asInt() > 0) {
                job->saveNumber = static_cast<unsigned long long>(args["save_number"].asInt());
            } else {
                throw Error("load_save needs \"save_name\" or \"save_number\" (from saves_list)");
            }
        }
        if (job->newGame && args["name"].isString())
            SR2_INFO("[{}] new_game: \"name\" is not applied (the game names new games itself)", kModule);
        return Steps([job](int a) { return LoadStep(*job, a); }, deadline, cancel);
    }
    if (cmd == "save") {
        return OnMain(
            [] {
                if (!game::InGame()) throw Error("save refused: not in game");
                const game::Metadata m = game::CurrentMetadata();
                const int slot = m.slotIndex + 1;
                if (!m.has || (slot != 5 && slot != 6))
                    throw Error(std::format("save refused: the game in play is in slot {} (only 5 and 6 may be saved)", slot));
                const bool ok = game::SaveNow();
                const game::Metadata after = game::CurrentMetadata();
                if (!ok) throw Error("AutoSaveDirector.SaveAllNow reported a failure (see Player.log)");
                Value r = Obj();
                r.set("save_number", Num(static_cast<double>(after.saveNumber)));
                r.set("slot", Num(slot));
                return r;
            },
            deadline, cancel);
    }
    if (cmd == "quit_to_title") {
        auto since = std::make_shared<unsigned long long>(0);
        auto started = std::make_shared<bool>(false);
        return Steps(
            [since, started](int) -> Value {
                if (!*started) {
                    *started = true;
                    if (game::InGame()) {
                        SR2_INFO("[{}] quit_to_title (no save)", kModule);
                        game::QuitToTitle();
                        return at::Retry();
                    }
                }
                if (!game::AtTitle()) {
                    *since = 0;
                    return at::Retry();
                }
                if (!*since) *since = GetTickCount64();
                if (GetTickCount64() - *since < 1000) return at::Retry();
                return State();
            },
            deadline, cancel);
    }
    if (cmd == "quit_game") {
        return OnMain(
            [] {
                if (!game::AtTitle()) throw Error("quit_game refused: only on the title screen");
                g_quitAt = GetTickCount64() + 500;  // after this reply went out
                SR2_INFO("[{}] quit_game: Application.Quit in 0.5 s", kModule);
                Value r = Obj();
                r.set("quitting", Bool(true));
                return r;
            },
            deadline, cancel);
    }
    if (cmd == "delete_slot") {
        const int slot = SlotArg(args, "delete_slot");
        return OnMain(
            [slot] {
                const game::Metadata m = game::CurrentMetadata();
                std::vector<std::string> names;
                for (const auto& s : game::Saves())
                    if (s.slotIndex == slot - 1 && std::find(names.begin(), names.end(), s.gameName) == names.end()) {
                        GuardTestSlot(s, "delete_slot");
                        names.push_back(s.gameName);
                    }
                for (const auto& n : names)
                    if (game::InGame() && m.has && m.gameName == n)
                        throw Error(std::format("delete_slot refused: slot {}'s game is the one loaded (quit_to_title first)", slot));
                Value del = Arr();
                for (const auto& n : names) {
                    SR2_INFO("[{}] delete_slot {}: deleting game {}", kModule, slot, n);
                    game::DeleteGame(n);
                    del.push(Str(n));
                }
                // AutoSaveDirector.DeleteGame finds the files through its own summaries: check the folder.
                Value left = Arr();
                for (const auto& s : game::Saves())
                    if (!s.fileName.empty() && std::find(names.begin(), names.end(), s.gameName) != names.end())
                        left.push(Str(s.fileName));
                if (left.size())
                    SR2_WARN("[{}] delete_slot {}: {} save file(s) still there after DeleteGame", kModule, slot, left.size());
                Value r = Obj();
                r.set("deleted", std::move(del));
                r.set("files_left", std::move(left));
                return r;
            },
            deadline, cancel);
    }
    if (cmd == "teleport") return Teleport(args, deadline, cancel);
    if (cmd == "find") return OnMain([args] { return Find(args); }, deadline, cancel);
    if (cmd == "probe") return NamedCall(true, args, deadline, cancel);
    if (cmd == "exec") return NamedCall(false, args, deadline, cancel);
    if (cmd == "input") return Input(args, deadline, cancel);
    if (cmd == "screenshot") return Screenshot(args, timeoutS, deadline, cancel);
    if (cmd == "overlay") return OverlayCmd(args, deadline, cancel);
    if (cmd == "ask") return Ask(args, cancel);
    throw Error("unknown command '" + cmd + "'");
}

// Answered on the server thread at once (thread-safe, no game calls).
bool Quick(const std::string& cmd, const Value& args, Value& result) {
    if (cmd == "ping") {
        result = Obj();
        result.set("build", Str(sr2::core::build_info::TimeIso()));
        result.set("sha", Str(sr2::core::build_info::Commit()));
        result.set("version", Str(SR2MODS_VERSION));
        result.set("save_guard", Bool(g_guardActive.load()));
        result.set("faulted", Bool(sr2::core::ModuleFaulted(kModule)));
        return true;
    }
    if (cmd == "log_mark") {
        static std::atomic<unsigned> n{0};
        const std::string mark = std::format("autotest mark {}-{}", ++n, GetTickCount64());
        SR2_INFO("[{}] {}", kModule, mark);
        result = Obj();
        result.set("mark", Str(mark));
        return true;
    }
    if (cmd == "cancel_ask") {
        const bool open = view::QuestionOpen();
        view::CancelAsk(static_cast<uint64_t>(args["ask_id"].asInt(0)));
        Flash(false);
        result = Obj();
        result.set("cancelled", Bool(open));
        return true;
    }
    if (cmd == "subscribe_log") {
        const bool on = args["on"].asBool(true);
        at::pipe::SubscribeLog(on);
        result = Obj();
        result.set("subscribed", Bool(on));
        return true;
    }
    if (cmd == "settings_get") {
        result = SettingsGet(args);
        return true;
    }
    if (cmd == "list_names") {
        result = Obj();
        Value probes = Arr(), actions = Arr(), cmds = Arr();
        for (const auto& n : at::Names(true)) probes.push(Str(n));
        for (const auto& n : at::Names(false)) actions.push(Str(n));
        for (const char* c : kCommands) cmds.push(Str(c));
        result.set("commands", std::move(cmds));
        result.set("probes", std::move(probes));
        result.set("actions", std::move(actions));
        return true;
    }
    return false;
}

void OnConnect(bool connected) {
    if (connected) {
        g_armed = true;
        view::SetArmed(true);
        KeepGuardHook();
        SR2_INFO("[{}] armed: saves outside slots 5 and 6 are skipped while the client is connected (guard {})", kModule,
                 g_guardActive.load() ? "on" : "UNAVAILABLE");
    } else {
        g_armed = false;
        view::SetArmed(false);
        view::CancelAsk();
        Flash(false);
        ReleaseGuardHook();
        SR2_INFO("[{}] disarmed: the save guard is lifted", kModule);
    }
}

// --------------------------------------------------------------- module -----
class Autotest final : public sr2::core::Module {
public:
    const char* Name() const override { return kModule; }
    bool DefaultEnabled() const override { return false; }

    bool Setup(sr2::core::ModuleContext&) override {
        std::string missing;
        game::Resolve(missing);
        if (!missing.empty())
            SR2_WARN("[{}] some game symbols are missing; the commands that need them fail with a reason: {}", kModule, missing);
        std::string err;
        if (!gt::Resolve(err)) SR2_WARN("[{}] teleport unavailable: {}", kModule, err);
        at::RegisterGenericActions();
        at::RegisterAction("place_save", [](const Value& a) { return PlaceSave(a); });
        at::RegisterProbe("places", [](const Value&) { return LoadPlaces(); });
        at::RegisterProbe("list_inputs", [](const Value&) {
            std::vector<std::string> all;
            FindInputEvent("", &all);
            Value r = Arr();
            for (const auto& n : all) r.push(Str(n));
            return r;
        });
        SR2_INFO("[{}] setup ok: {} probe(s), {} action(s) registered", kModule, at::Names(true).size(), at::Names(false).size());
        return true;
    }

    void OnEnable() override {
        sr2::modules::custom_upgrades::SetSaveVeto(&SaveVeto);
        if (!frameCb_) frameCb_ = main_thread::AddFrameCallback(kModule, [](float) { Frame(); });
        at::pipe::Start(&Handle, &Quick, &OnConnect);
    }

    void OnDisable() override {
        at::pipe::Stop();
        if (g_armed.exchange(false)) {
            view::SetArmed(false);
            view::CancelAsk();
            Flash(false);
            ReleaseGuardHook();
        }
        sr2::modules::custom_upgrades::SetSaveVeto(nullptr);
        if (frameCb_) main_thread::RemoveFrameCallback(frameCb_);
        frameCb_ = 0;
    }

private:
    // Main thread, every tick.
    static void Frame() {
        const unsigned long long now = GetTickCount64();
        static unsigned long long nextGuard = 0;
        if (g_armed.load() && now >= nextGuard) {
            nextGuard = now + 1000;
            KeepGuardHook();  // custom_upgrades switched off meanwhile disables its hooks
        }
        for (size_t i = 0; i < g_releases.size();) {
            if (now < g_releases[i].at) {
                ++i;
                continue;
            }
            Il2CppObject* e = g_releases[i].evt;
            g_releases.erase(g_releases.begin() + static_cast<std::ptrdiff_t>(i));
            try {
                if (unity::IsAlive(e)) ReleaseEvent(e);
            } catch (const Error& ex) {
                SR2_WARN("[{}] input release failed: {}", kModule, ex.what());
            }
        }
        if (g_quitAt && now >= g_quitAt) {
            g_quitAt = 0;
            try {
                game::QuitGame();
            } catch (const Error& ex) {
                SR2_WARN("[{}] quit_game failed: {}", kModule, ex.what());
            }
        }
    }

    int frameCb_ = 0;
};

}  // namespace

std::unique_ptr<sr2::core::Module> CreateAutotest() { return std::make_unique<Autotest>(); }

}  // namespace sr2::modules
