// sandbox: the weather changer (see weather.h and NOTES.md, "Weather changer"). SR2 1.3.0.
//
// - Catalog: WeatherRegistry.DEBUG_GetWeatherZones() (the registry's zones), per zone
//   DEBUG_GetZoneWeathers(zone) (its ZoneWeatherConfig.Patterns, the live list), per pattern its states
//   from its transitions (StartingTransitions[].ToState, RunningTransitions[].FromState / .Transitions[].ToState).
//   Names: the pattern's WeatherTypeMetadata.PediaEntry title (the Slimepedia's weather entry), the state's
//   StateName, the zone's _localizedName; icons: WeatherTypeMetadata.Icon (the map's weather icon).
//   Built once per WeatherRegistry (a new one comes with every load); the objects are held with GC handles.
// - Now: DEBUG_GetRunningWeather(zone) (ZoneWeatherData.CurrentWeatherPatterns, live): WeatherPattern
//   _definition / _activeState. Forecast: DEBUG_GetForecast(zone) (WeatherModel.ZoneData.Forecast, live).
// - Changes: DEBUG_SetZoneWeather(zone, pattern, state) only (state null = end the pattern). The forecast
//   is never written, so nothing of ours reaches the save; a lock is kept in the sidecar (SaveData).
//
// Every game call is Resolver::Invoke on the main thread; a native fault marks the module faulted.

#include "modules/sandbox/weather.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <format>
#include <initializer_list>
#include <map>
#include <string>
#include <vector>

#include "core/detour_guard.h"
#include "core/il2cpp_api.h"
#include "core/json.h"
#include "core/log.h"
#include "core/main_thread.h"
#include "core/module.h"
#include "core/unity.h"
#include "modules/sandbox/cheated_save.h"
#include "ui/item_icons.h"
#include "ui/sandbox_bridge.h"
#include "ui/sandbox_weather_bridge.h"

namespace sr2::modules::sandbox {
namespace {

namespace unity = sr2::core::unity;
namespace wb = sr2::ui::sandbox::weather;
using sr2::il2cpp::api;
using sr2::il2cpp::Resolver;

constexpr const char* kModule = "sandbox";
constexpr const char* kImage = "Assembly-CSharp";
constexpr const char* kWeatherNs = "MonomiPark.SlimeRancher.Weather";
constexpr unsigned long long kSnapshotMs = 500;   // card state while the page is open
constexpr unsigned long long kEnforceMs = 1000;   // locks / timed starts
constexpr unsigned long long kRebuildMs = 2000;   // catalog retry while the registry has no zones yet
constexpr double kSecsPerHour = 3600.0;           // TimeDirector.SECS_PER_HOUR (world time is in game seconds)
constexpr float kDefaultHours = 4.0f, kMinHours = 1.0f, kMaxHours = 24.0f;  // a start without lock
constexpr int kMaxReapplyLogs = 30;

std::atomic<bool> g_live{false};
std::string g_why;  // "" = available

// ---- resolved in SetupWeather ----
const MethodInfo* g_tryGetRegistry = nullptr;  // SceneContextUtility.TryGetWeatherRegistry(out WeatherRegistry)
const MethodInfo* g_tryGetTime = nullptr;      // SceneContextUtility.TryGetTimeDirector(out TimeDirector)
const MethodInfo* g_tryGetScene = nullptr;     // SceneContextUtility.TryGetSceneContext(out SceneContext)
const MethodInfo* g_scZoneTracker = nullptr;   // SceneContext.get_PlayerZoneTracker
const MethodInfo* g_currentZone = nullptr;     // PlayerZoneTracker.GetCurrentZone
const MethodInfo* g_worldTime = nullptr;       // TimeDirector.WorldTime
const MethodInfo* g_setWeather = nullptr;      // WeatherRegistry.DEBUG_SetZoneWeather(zone, pattern, state)
const MethodInfo* g_getZones = nullptr;        // WeatherRegistry.DEBUG_GetWeatherZones
const MethodInfo* g_getPatterns = nullptr;     // WeatherRegistry.DEBUG_GetZoneWeathers(zone)
const MethodInfo* g_getRunning = nullptr;      // WeatherRegistry.DEBUG_GetRunningWeather(zone)
const MethodInfo* g_getForecast = nullptr;     // WeatherRegistry.DEBUG_GetForecast(zone)
const MethodInfo* g_pediaTitle = nullptr;      // PediaEntry.get_Title (optional)
const MethodInfo* g_locGet = nullptr;          // LocalizedString.GetLocalizedString (optional)
Il2CppClass* g_registryClass = nullptr;
Il2CppClass* g_patternClass = nullptr;         // WeatherPattern (the running instance)
Il2CppClass* g_patternDefClass = nullptr;      // WeatherPatternDefinition
Il2CppClass* g_stateDefClass = nullptr;        // WeatherStateDefinition
size_t g_offRegModel = 0, g_offRegFirstLoad = 0;          // WeatherRegistry._model, _firstLoad
size_t g_offPatDef = 0, g_offPatState = 0;                // WeatherPattern._definition, _activeState
size_t g_offDefMeta = 0, g_offDefStart = 0, g_offDefRun = 0;  // WeatherPatternDefinition.Metadata, StartingTransitions, RunningTransitions
size_t g_offStateName = 0, g_offStateTier = 0, g_offStateHours = 0;  // WeatherStateDefinition.StateName, MapTier, MinDurationHours
size_t g_offMetaPedia = 0, g_offMetaIcon = 0;             // WeatherTypeMetadata.PediaEntry, Icon
size_t g_offZoneName = 0;                                 // ZoneDefinition._localizedName
// Nested classes, offsets read from the first live object.
size_t g_offTrTo = 0, g_offTlFrom = 0, g_offTlList = 0;   // Transition.ToState, TransitionList.FromState / Transitions
// ---- the clock (optional: the card's "Time of day" part; see SetTime below) ----
std::string g_timeWhy;                         // "" = available
const MethodInfo* g_fastForwardTo = nullptr;   // TimeDirector.FastForwardTo(double)
const MethodInfo* g_ffEnd = nullptr;           // TimeDirector.ForceFastForwardEnd()
size_t g_offTdModel = 0, g_offTdFfSecs = 0, g_offTdSecs = 0;  // TimeDirector._worldModel, FfSecsPerGameDay, SecsPerGameDay
size_t g_offTdLastDay = 0, g_offTdLastHour = 0;               // TimeDirector._lastDay / _lastHour
size_t g_offWmTime = 0, g_offWmStart = 0, g_offWmFf = 0;      // WorldModel.worldTime, worldTimeStart, fastForwardUntil
size_t g_offWmPause = 0, g_offWmLast = 0;                     // WorldModel.pauseWorldTime, lastWorldTime
bool g_timeLogged = false;
bool g_pausedByUs = false;                     // the clock was paused from the card (unpaused when Sandbox goes off)
// Nullable<double> as il2cpp lays it out (hasValue first; see TimeDirector.LateUpdate in IDA).
struct NullableDouble {
    bool hasValue;
    double value;
};
static_assert(sizeof(NullableDouble) == 16);

struct ForecastOffsets {
    Il2CppClass* klass = nullptr;
    bool ok = false;
    size_t pattern = 0, state = 0, start = 0, endKnown = 0, end = 0, started = 0;
} g_fc;

// ---- catalog (main thread) ----
struct StateC {
    Il2CppObject* obj = nullptr;
    std::string asset, name;
    int tier = 0;
    float hours = 0;
};
struct PatternC {
    Il2CppObject* obj = nullptr;
    Il2CppObject* meta = nullptr;  // WeatherTypeMetadata (the same weather type in other zones)
    std::string asset, name;
    uint32_t icon = 0;
    std::vector<StateC> states;
};
struct ZoneC {
    Il2CppObject* obj = nullptr;
    std::string asset, name;
    std::vector<PatternC> patterns;
};
std::vector<ZoneC> g_zones;
std::vector<Il2CppGCHandle> g_handles;  // every catalog object
Il2CppGCHandle g_regHandle = nullptr;   // the registry the catalog belongs to
unsigned long long g_lastBuild = 0;

// ---- overrides (main thread), by zone asset name ----
struct Override {
    std::string pattern, state;  // asset names; pattern "" = clear skies
    bool locked = false;
    double until = 0;            // !locked: world time (game seconds) when the natural weather comes back
    std::string label;           // "Thunderstorm - Heavy"
};
std::map<std::string, Override> g_over;
unsigned g_dataSeen = 0;      // SaveTogglesVersion() of the locks in g_over
bool g_locksFresh = false;    // locks just loaded from the save: the first apply is logged
std::string g_show;           // zone asset name the card shows ("" = where the player is)
int g_reapplyLogs = 0;
unsigned long long g_lastSnap = 0, g_lastEnforce = 0;

bool IsA(Il2CppObject* obj, Il2CppClass* k) {
    const auto& a = api();
    if (!obj || !k || !a.il2cpp_object_get_class) return false;
    Il2CppClass* c = a.il2cpp_object_get_class(obj);
    if (c == k) return true;
    return c && a.il2cpp_class_is_assignable_from && a.il2cpp_class_is_assignable_from(k, c);
}

bool Live() { return g_live.load() && !sr2::core::ModuleFaulted(kModule); }

Il2CppObject* Call(const MethodInfo* m, void* obj, std::initializer_list<void*> args, const char* what, bool* ok = nullptr) {
    if (ok) *ok = false;
    if (!m || sr2::core::ModuleFaulted(kModule)) return nullptr;
    std::vector<void*> params(args);
    std::string err;
    Il2CppObject* r = Resolver::Get().Invoke(m, obj, params.empty() ? nullptr : params.data(), &err);
    if (!err.empty()) {
        if (err.rfind("native exception", 0) == 0) sr2::core::MarkModuleFaulted(kModule, std::format("{}: {}", what, err));
        else SR2_WARN("[{}] {} failed: {}", kModule, what, err);
        return nullptr;
    }
    if (ok) *ok = true;
    return r;
}

template <class T>
bool CallValue(const MethodInfo* m, void* obj, std::initializer_list<void*> args, const char* what, T& out) {
    bool ok = false;
    Il2CppObject* r = Call(m, obj, args, what, &ok);
    if (!ok || !r) return false;
    out = *reinterpret_cast<T*>(reinterpret_cast<char*>(r) + 2 * sizeof(void*));
    return true;
}

void Done(bool ok, const std::string& text) {
    if (ok) SR2_INFO("[{}] {}", kModule, text);
    else SR2_WARN("[{}] {}", kModule, text);
    sr2::ui::sandbox::SetStatus(text, !ok);
}

// Rich-text tags removed; "!! key !!" (missing translation) is no name.
std::string Localized(Il2CppObject* ls) {
    if (!ls || !g_locGet) return {};
    auto* s = reinterpret_cast<Il2CppString*>(Call(g_locGet, ls, {}, "LocalizedString.GetLocalizedString"));
    std::string t, raw = Resolver::Get().ToUtf8(s);
    bool tag = false;
    for (char c : raw) {
        if (c == '<') tag = true;
        else if (c == '>' && tag) tag = false;
        else if (!tag) t.push_back(c);
    }
    return t.rfind("!!", 0) == 0 ? std::string() : t;
}

Il2CppObject* Hold(Il2CppObject* obj) {
    if (Il2CppGCHandle h = obj ? unity::NewGCHandle(obj) : nullptr) g_handles.push_back(h);
    return obj;
}

void DropCatalog() {
    g_zones.clear();
    for (Il2CppGCHandle h : g_handles) unity::FreeGCHandle(h);  // frame callback, not a hook
    g_handles.clear();
    if (g_regHandle) unity::FreeGCHandle(g_regHandle);
    g_regHandle = nullptr;
}

// ------------------------------------------------------------- game ----
Il2CppObject* Registry() {
    Il2CppObject* reg = nullptr;
    bool found = false;
    if (!CallValue(g_tryGetRegistry, nullptr, {&reg}, "SceneContextUtility.TryGetWeatherRegistry", found) || !found ||
        !IsA(reg, g_registryClass) || !unity::IsAlive(reg))
        return nullptr;
    return reg;
}

// The registry has its model and has run its first Update after the load (the save's running forecast
// entries are started then, all at once): only then are our changes not undone by the load itself.
bool Ready(Il2CppObject* reg) {
    return reg && unity::Field<void*>(reg, g_offRegModel) && !unity::Field<bool>(reg, g_offRegFirstLoad);
}

Il2CppObject* TimeDir() {
    Il2CppObject* td = nullptr;
    bool found = false;
    if (!CallValue(g_tryGetTime, nullptr, {&td}, "SceneContextUtility.TryGetTimeDirector", found) || !found || !td ||
        !unity::IsAlive(td))
        return nullptr;
    return td;
}

bool WorldTime(double& t) {
    Il2CppObject* td = TimeDir();
    return td && CallValue(g_worldTime, td, {}, "TimeDirector.WorldTime", t) && std::isfinite(t);
}

Il2CppObject* PlayerZone() {
    Il2CppObject* sc = nullptr;
    bool found = false;
    if (!CallValue(g_tryGetScene, nullptr, {&sc}, "SceneContextUtility.TryGetSceneContext", found) || !found || !sc ||
        !unity::IsAlive(sc))
        return nullptr;
    Il2CppObject* tracker = Call(g_scZoneTracker, sc, {}, "SceneContext.PlayerZoneTracker");
    if (!tracker || !unity::IsAlive(tracker)) return nullptr;
    return Call(g_currentZone, tracker, {}, "PlayerZoneTracker.GetCurrentZone");
}

size_t LiveOffset(Il2CppObject* obj, const char* field) {
    if (!obj) return 0;
    auto o = Resolver::Get().FieldOffset(api().il2cpp_object_get_class(obj), field);
    return o ? *o : 0;
}

void AddState(PatternC& p, Il2CppObject* st) {
    if (!IsA(st, g_stateDefClass) || !unity::IsAlive(st)) return;
    for (const auto& s : p.states)
        if (s.obj == st) return;
    StateC s;
    s.obj = Hold(st);
    s.asset = unity::GetName(st);
    s.name = Resolver::Get().ToUtf8(unity::Field<Il2CppString*>(st, g_offStateName));
    if (s.name.empty()) s.name = s.asset;
    s.tier = std::clamp(unity::Field<int>(st, g_offStateTier), 0, 3);
    s.hours = unity::Field<float>(st, g_offStateHours);
    if (!std::isfinite(s.hours) || s.hours < 0) s.hours = 0;
    p.states.push_back(std::move(s));
}

// A pattern's states in transition order: where it starts, then every state it runs through.
void ReadStates(PatternC& p) {
    void* starting = unity::Field<void*>(p.obj, g_offDefStart);
    for (size_t i = 0, n = unity::ListCount(starting); i < n; ++i) {
        auto* tr = static_cast<Il2CppObject*>(unity::ListAt(starting, i));
        if (!tr) continue;
        if (!g_offTrTo) g_offTrTo = LiveOffset(tr, "ToState");
        if (g_offTrTo) AddState(p, unity::Field<Il2CppObject*>(tr, g_offTrTo));
    }
    void* running = unity::Field<void*>(p.obj, g_offDefRun);
    for (size_t i = 0, n = unity::ListCount(running); i < n; ++i) {
        auto* tl = static_cast<Il2CppObject*>(unity::ListAt(running, i));
        if (!tl) continue;
        if (!g_offTlFrom) g_offTlFrom = LiveOffset(tl, "FromState");
        if (!g_offTlList) g_offTlList = LiveOffset(tl, "Transitions");
        if (g_offTlFrom) AddState(p, unity::Field<Il2CppObject*>(tl, g_offTlFrom));
        void* list = g_offTlList ? unity::Field<void*>(tl, g_offTlList) : nullptr;
        for (size_t j = 0, m = unity::ListCount(list); j < m; ++j) {
            auto* tr = static_cast<Il2CppObject*>(unity::ListAt(list, j));
            if (!tr) continue;
            if (!g_offTrTo) g_offTrTo = LiveOffset(tr, "ToState");
            if (g_offTrTo) AddState(p, unity::Field<Il2CppObject*>(tr, g_offTrTo));
        }
    }
}

bool EnsureCatalog(Il2CppObject* reg) {
    if (g_regHandle && unity::GCHandleTarget(g_regHandle) == reg && !g_zones.empty()) return true;
    const unsigned long long now = GetTickCount64();
    if (g_regHandle && unity::GCHandleTarget(g_regHandle) == reg && now - g_lastBuild < kRebuildMs) return false;
    g_lastBuild = now;
    DropCatalog();
    g_regHandle = unity::NewGCHandle(reg);
    void* zones = Call(g_getZones, reg, {}, "WeatherRegistry.DEBUG_GetWeatherZones");
    std::string summary;
    int patterns = 0, states = 0;
    for (size_t i = 0, n = unity::ListCount(zones); i < n; ++i) {
        auto* z = static_cast<Il2CppObject*>(unity::ListAt(zones, i));
        if (!z || !unity::IsAlive(z)) continue;
        ZoneC zc;
        zc.obj = Hold(z);
        zc.asset = unity::GetName(z);
        zc.name = Localized(unity::Field<Il2CppObject*>(z, g_offZoneName));
        if (zc.name.empty()) zc.name = zc.asset;
        void* list = Call(g_getPatterns, reg, {z}, "WeatherRegistry.DEBUG_GetZoneWeathers");
        for (size_t j = 0, m = unity::ListCount(list); j < m; ++j) {
            auto* def = static_cast<Il2CppObject*>(unity::ListAt(list, j));
            if (!IsA(def, g_patternDefClass) || !unity::IsAlive(def)) continue;
            PatternC pc;
            pc.obj = Hold(def);
            pc.asset = unity::GetName(def);
            if (auto* meta = unity::Field<Il2CppObject*>(def, g_offDefMeta); meta && unity::IsAlive(meta)) {
                pc.meta = Hold(meta);
                if (auto* pedia = unity::Field<Il2CppObject*>(meta, g_offMetaPedia); pedia && unity::IsAlive(pedia))
                    pc.name = Localized(Call(g_pediaTitle, pedia, {}, "PediaEntry.Title"));
                if (pc.name.empty()) pc.name = unity::GetName(meta);
                if (auto* icon = unity::Field<Il2CppObject*>(meta, g_offMetaIcon); icon && unity::IsAlive(icon))
                    pc.icon = sr2::ui::item_icons::Register(icon);
            }
            if (pc.name.empty()) pc.name = pc.asset;
            ReadStates(pc);
            std::string st;
            for (const auto& s : pc.states)
                st += std::format("{}{} '{}' (tier {}, {:g} h)", st.empty() ? "" : ", ", s.asset, s.name, s.tier, s.hours);
            summary += std::format("{}{} / {}: {} '{}' [{}]", summary.empty() ? "" : "; ", zc.name, zc.asset, pc.asset,
                                   pc.name, st);
            ++patterns;
            states += static_cast<int>(pc.states.size());
            zc.patterns.push_back(std::move(pc));
        }
        g_zones.push_back(std::move(zc));
    }
    if (g_zones.empty()) return false;
    SR2_INFO("[{}] weather: {} zone(s), {} pattern(s), {} state(s): {}", kModule, g_zones.size(), patterns, states, summary);
    return true;
}

ZoneC* ZoneByAsset(const std::string& asset) {
    for (auto& z : g_zones)
        if (z.asset == asset) return &z;
    return nullptr;
}
ZoneC* ZoneByObj(const void* obj) {
    for (auto& z : g_zones)
        if (z.obj == obj) return &z;
    return nullptr;
}
PatternC* PatternByAsset(ZoneC& z, const std::string& asset) {
    for (auto& p : z.patterns)
        if (p.asset == asset) return &p;
    return nullptr;
}
const StateC* StateByAsset(const PatternC& p, const std::string& asset) {
    for (const auto& s : p.states)
        if (s.asset == asset) return &s;
    return p.states.empty() ? nullptr : &p.states.front();
}
std::string PatternName(const ZoneC& z, const Il2CppObject* def) {
    for (const auto& p : z.patterns)
        if (p.obj == def) return p.name;
    return def ? unity::GetName(const_cast<Il2CppObject*>(def)) : std::string("?");
}

struct RunningC {
    Il2CppObject* def = nullptr;
    Il2CppObject* state = nullptr;
};

std::vector<RunningC> Running(Il2CppObject* reg, const ZoneC& z) {
    std::vector<RunningC> out;
    void* list = Call(g_getRunning, reg, {z.obj}, "WeatherRegistry.DEBUG_GetRunningWeather");
    for (size_t i = 0, n = unity::ListCount(list); i < n; ++i) {
        auto* inst = static_cast<Il2CppObject*>(unity::ListAt(list, i));
        if (!IsA(inst, g_patternClass)) continue;
        RunningC r{unity::Field<Il2CppObject*>(inst, g_offPatDef), unity::Field<Il2CppObject*>(inst, g_offPatState)};
        if (r.def) out.push_back(r);
    }
    return out;
}

bool SetWeather(Il2CppObject* reg, const ZoneC& z, Il2CppObject* def, Il2CppObject* state) {
    bool ok = false;
    Call(g_setWeather, reg, {z.obj, def, state}, "WeatherRegistry.DEBUG_SetZoneWeather", &ok);
    return ok;
}

struct ForecastC {
    Il2CppObject* pattern = nullptr;
    Il2CppObject* state = nullptr;
    double start = 0, end = 0;
    bool endKnown = false, started = false;
};

std::vector<ForecastC> ReadForecast(Il2CppObject* reg, const ZoneC& z) {
    std::vector<ForecastC> out;
    void* list = Call(g_getForecast, reg, {z.obj}, "WeatherRegistry.DEBUG_GetForecast");
    for (size_t i = 0, n = unity::ListCount(list); i < n; ++i) {
        auto* e = static_cast<Il2CppObject*>(unity::ListAt(list, i));
        if (!e) continue;
        Il2CppClass* k = api().il2cpp_object_get_class(e);
        if (k != g_fc.klass) {
            g_fc = {};
            g_fc.klass = k;
            auto& R = Resolver::Get();
            auto o = [&](const char* f, size_t& out) {
                auto v = R.FieldOffset(k, f);
                if (v) out = *v;
                return v.has_value();
            };
            g_fc.ok = o("Pattern", g_fc.pattern) && o("State", g_fc.state) && o("StartTime", g_fc.start) &&
                      o("IsEndTimeKnown", g_fc.endKnown) && o("EndTime", g_fc.end) && o("Started", g_fc.started);
            if (!g_fc.ok) SR2_WARN("[{}] weather: forecast entry fields not found - no forecast shown", kModule);
        }
        if (!g_fc.ok) break;
        ForecastC f;
        f.pattern = unity::Field<Il2CppObject*>(e, g_fc.pattern);
        f.state = unity::Field<Il2CppObject*>(e, g_fc.state);
        f.start = unity::Field<double>(e, g_fc.start);
        f.endKnown = unity::Field<bool>(e, g_fc.endKnown);
        f.end = unity::Field<double>(e, g_fc.end);
        f.started = unity::Field<bool>(e, g_fc.started);
        if (f.pattern) out.push_back(f);
    }
    return out;
}

// What the zone runs now matches `ov`: other patterns ended, the chosen one in its state. `what` gets a
// short account ("ended Rain, started Thunderstorm - Heavy"); true if anything was changed.
bool Apply(Il2CppObject* reg, ZoneC& z, const Override& ov, std::string& what) {
    PatternC* p = ov.pattern.empty() ? nullptr : PatternByAsset(z, ov.pattern);
    if (!ov.pattern.empty() && !p) {
        what = std::format("{} has no weather '{}'", z.name, ov.pattern);
        return false;
    }
    const StateC* s = p ? StateByAsset(*p, ov.state) : nullptr;
    if (p && !s) {
        what = std::format("{} has no states", p->name);
        return false;
    }
    bool have = false, changed = false;
    std::string ended;
    for (const RunningC& r : Running(reg, z)) {
        if (p && r.def == p->obj) {
            have |= r.state == s->obj;
            continue;
        }
        if (SetWeather(reg, z, r.def, nullptr)) {
            ended += (ended.empty() ? "" : ", ") + PatternName(z, r.def);
            changed = true;
        }
    }
    what = ended.empty() ? std::string() : "ended " + ended;
    if (p && !have && SetWeather(reg, z, p->obj, s->obj)) {
        what += std::format("{}started {} - {}", what.empty() ? "" : ", ", p->name, s->name);
        changed = true;
    }
    return changed;
}

// Ends whatever runs in the zone, then starts what its forecast has running now (the forecast entries the
// game started that are not over), as the game had it.
std::string RestoreNatural(Il2CppObject* reg, ZoneC& z) {
    std::string ended, started;
    for (const RunningC& r : Running(reg, z))
        if (SetWeather(reg, z, r.def, nullptr)) ended += (ended.empty() ? "" : ", ") + PatternName(z, r.def);
    double now = 0;
    if (WorldTime(now)) {
        std::vector<Il2CppObject*> done;
        for (const ForecastC& f : ReadForecast(reg, z)) {
            if (!f.started || f.start > now || (f.endKnown && f.end <= now) || !f.state) continue;
            if (std::find(done.begin(), done.end(), f.pattern) != done.end()) continue;
            if (!IsA(f.pattern, g_patternDefClass) || !IsA(f.state, g_stateDefClass)) continue;
            done.push_back(f.pattern);
            if (SetWeather(reg, z, f.pattern, f.state))
                started += (started.empty() ? "" : ", ") + PatternName(z, f.pattern);
        }
    }
    return std::format("ended {}; the forecast's weather back: {}", ended.empty() ? "nothing" : ended,
                       started.empty() ? "none (clear)" : started);
}

// ------------------------------------------------------------ locks ----
void SaveLocks() {
    sr2::json::Value locks = sr2::json::Value::Array();
    for (const auto& [zone, ov] : g_over) {
        if (!ov.locked) continue;
        sr2::json::Value e = sr2::json::Value::Object();
        e.set("zone", sr2::json::Value(zone));
        e.set("pattern", sr2::json::Value(ov.pattern));
        e.set("state", sr2::json::Value(ov.state));
        e.set("label", sr2::json::Value(ov.label));
        locks.push(std::move(e));
    }
    if (locks.size() == 0) {
        SetSaveData(kDataWeather, sr2::json::Value());
        return;
    }
    sr2::json::Value v = sr2::json::Value::Object();
    v.set("note", sr2::json::Value(std::string("Weather locked from the Sandbox page (asset names; pattern \"\" = clear "
                                               "skies). The save itself keeps the game's own forecast.")));
    v.set("locks", std::move(locks));
    SetSaveData(kDataWeather, std::move(v));
}

void LoadLocks() {
    g_over.clear();
    g_show.clear();
    g_reapplyLogs = 0;
    const sr2::json::Value& v = SaveData(kDataWeather);
    std::string list;
    for (const auto& e : v["locks"].items()) {
        const std::string zone = e["zone"].asString();
        if (zone.empty()) continue;
        Override ov;
        ov.pattern = e["pattern"].asString();
        ov.state = e["state"].asString();
        ov.label = e["label"].asString(ov.pattern.empty() ? "Clear skies" : ov.pattern);
        ov.locked = true;
        list += std::format("{}{}: {}", list.empty() ? "" : ", ", zone, ov.label);
        g_over[zone] = std::move(ov);
    }
    g_locksFresh = !g_over.empty();
    if (!g_over.empty()) SR2_INFO("[{}] weather locks of this save: {} (applied once the weather is up)", kModule, list);
}

void Enforce(Il2CppObject* reg) {
    double now = 0;
    const bool haveTime = WorldTime(now);
    bool locksChanged = false;
    for (auto it = g_over.begin(); it != g_over.end();) {
        ZoneC* z = ZoneByAsset(it->first);
        if (!z) {
            ++it;
            continue;
        }
        Override& ov = it->second;
        if (!ov.locked && haveTime && now >= ov.until) {
            SR2_INFO("[{}] weather: {} back to natural weather ({} ran its time): {}", kModule, z->name, ov.label,
                     RestoreNatural(reg, *z));
            it = g_over.erase(it);
            continue;
        }
        std::string what;
        const bool changed = Apply(reg, *z, ov, what);
        if (!changed && !what.empty() && what.find("has no") != std::string::npos) {
            SR2_WARN("[{}] weather: {} dropped: {}", kModule, ov.label, what);
            locksChanged |= ov.locked;
            it = g_over.erase(it);
            continue;
        }
        if (changed && g_reapplyLogs < kMaxReapplyLogs) {
            ++g_reapplyLogs;
            SR2_INFO("[{}] weather: {} in {} {}: {}", kModule, ov.label, z->name,
                     g_locksFresh ? "applied from the save's lock" : "re-applied (the game changed the weather)", what);
        }
        ++it;
    }
    g_locksFresh = false;
    if (locksChanged) SaveLocks();
}

// ------------------------------------------------------------- time ----
// The game's clock is WorldModel.worldTime (game seconds, saved), advanced by TimeDirector.Update: by
// deltaTime * 86400 / SecsPerGameDay, or while WorldModel.fastForwardUntil is set by 86400 / FfSecsPerGameDay
// until it is reached (TimeDirector.FastForwardTo: the ranch house's sleep and the death screen use it;
// RanchCellFastForwarder then simulates the ranch's corrals, gardens, feeders and drones for the skipped
// time). WorldModel.pauseWorldTime makes Update return before touching the clock (set nowhere in the game
// but InitModel; not saved). Everything timed (TimeDirector.HasReached / OnPassedTime delegates, garden
// regrowth, slime hunger, the weather forecast, drones, the plort market day) compares against worldTime.
//
// - Forward (TimeSkip): only through FastForwardTo, so the world runs through the skipped time as when
//   sleeping (nothing is jumped over). Takes FfSecsPerGameDay * hours / 24 real seconds.
// - Back (TimeRewind, after a confirmation in the card): worldTime and lastWorldTime are set together (so
//   DeltaWorldTime is 0 on that frame, not a negative step), and TimeDirector._lastDay / _lastHour to the
//   new day and hour (no "day passed" / "hour passed" event for going back). Nothing is undone: whatever the
//   game scheduled after the old time (regrowth, feeding, forecast entries, timers) simply comes that much
//   later. At most 24 h back and never before WorldModel.worldTimeStart.
// - Pause (TimePause): pauseWorldTime. Lasts until a reload (not saved) or Sandbox is turned off.
constexpr double kSecsPerDay = 86400.0;

std::string Clock(double t) {
    const double day = std::floor(t / kSecsPerDay);
    const int mins = static_cast<int>(std::floor((t - day * kSecsPerDay) / 60.0));
    return std::format("day {}, {:02}:{:02}", static_cast<long long>(day) + 1, mins / 60 % 24, mins % 60);
}

Il2CppObject* WorldModel(Il2CppObject* td) {
    auto* wm = td ? unity::Field<Il2CppObject*>(td, g_offTdModel) : nullptr;
    return wm && std::isfinite(unity::Field<double>(wm, g_offWmTime)) ? wm : nullptr;
}

void ReadTime(wb::Time& out) {
    out = {};
    if (!g_timeWhy.empty()) {
        out.why = "Time of day unavailable: " + g_timeWhy;
        return;
    }
    Il2CppObject* td = TimeDir();
    Il2CppObject* wm = WorldModel(td);
    if (!wm) {
        out.why = "Load a save to change the time.";
        return;
    }
    out.ok = true;
    out.now = unity::Field<double>(wm, g_offWmTime);
    out.start = unity::Field<double>(wm, g_offWmStart);
    out.paused = unity::Field<bool>(wm, g_offWmPause);
    const NullableDouble ff = unity::Field<NullableDouble>(wm, g_offWmFf);
    out.skipping = ff.hasValue;
    out.skipUntil = ff.hasValue ? ff.value : 0;
    out.ffSecsPerDay = unity::Field<float>(td, g_offTdFfSecs);
    out.secsPerDay = unity::Field<float>(td, g_offTdSecs);
    if (!g_timeLogged) {
        g_timeLogged = true;
        SR2_INFO("[{}] clock: {} (world time {:.0f} s, started at {:.0f} s); a game day takes {:.0f} real s, {:.0f} s "
                 "while fast-forwarding{}", kModule, Clock(out.now), out.now, out.start, out.secsPerDay, out.ffSecsPerDay,
                 out.paused ? "; paused" : "");
    }
}

void EndSkip(Il2CppObject* td, Il2CppObject* wm) {
    if (unity::Field<NullableDouble>(wm, g_offWmFf).hasValue) Call(g_ffEnd, td, {}, "TimeDirector.ForceFastForwardEnd");
}

void DoTime(const wb::Action& a) {
    Il2CppObject* td = TimeDir();
    Il2CppObject* wm = WorldModel(td);
    if (!wm) return Done(false, "Time not changed: load a save first.");
    const double now = unity::Field<double>(wm, g_offWmTime);
    const double hour = std::clamp(a.hour, 0.0, 24.0 - 1.0 / 60.0);
    const double dayStart = std::floor(now / kSecsPerDay) * kSecsPerDay;
    switch (a.op) {
        case wb::Op::TimeSkip: {
            double target = dayStart + hour * 3600.0;
            if (target <= now + 30.0) target += kSecsPerDay;  // its next occurrence, at least half a minute away
            if (unity::Field<bool>(wm, g_offWmPause)) {
                unity::Field<bool>(wm, g_offWmPause) = false;  // a paused clock would never get there
                g_pausedByUs = false;
            }
            EndSkip(td, wm);
            bool ok = false;
            Call(g_fastForwardTo, td, {&target}, "TimeDirector.FastForwardTo", &ok);
            const double ffDay = unity::Field<float>(td, g_offTdFfSecs);
            const double hours = (target - now) / 3600.0;
            return Done(ok, ok ? std::format("Time: skipping ahead from {} to {} ({:.1f} game hours, about {:.0f} s), as "
                                             "sleeping does",
                                             Clock(now), Clock(target), hours, ffDay > 0 ? hours / 24.0 * ffDay : 0.0)
                               : std::string("Time not changed: TimeDirector.FastForwardTo failed."));
        }
        case wb::Op::TimeRewind: {
            double target = dayStart + hour * 3600.0;
            if (target >= now) target -= kSecsPerDay;  // its last occurrence (at most 24 h back)
            const double start = unity::Field<double>(wm, g_offWmStart);
            if (target < start)
                return Done(false, std::format("Time not changed: {} is before this game started ({}).", Clock(target),
                                               Clock(start)));
            EndSkip(td, wm);
            unity::Field<double>(wm, g_offWmTime) = target;
            unity::Field<NullableDouble>(wm, g_offWmLast) = {true, target};  // no negative step this frame
            unity::Field<int32_t>(td, g_offTdLastDay) = static_cast<int32_t>(std::floor(target / kSecsPerDay)) + 1;
            unity::Field<int32_t>(td, g_offTdLastHour) =
                static_cast<int32_t>(std::floor((target - std::floor(target / kSecsPerDay) * kSecsPerDay) / 3600.0));
            return Done(true, std::format("Time: set back from {} to {} ({:.1f} game hours); what the game had scheduled "
                                          "comes that much later",
                                          Clock(now), Clock(target), (now - target) / 3600.0));
        }
        case wb::Op::TimePause:
            if (a.lock) EndSkip(td, wm);
            unity::Field<bool>(wm, g_offWmPause) = a.lock;
            g_pausedByUs = a.lock;
            return Done(true, a.lock ? std::format("Time paused at {} (until you let it run, reload or turn Sandbox off)",
                                                   Clock(now))
                                     : std::format("Time runs again from {}", Clock(now)));
        case wb::Op::TimeStop: {
            const bool was = unity::Field<NullableDouble>(wm, g_offWmFf).hasValue;
            EndSkip(td, wm);
            return Done(true, was ? std::format("Time: skip stopped at {}", Clock(now)) : std::string("Time: no skip running"));
        }
        default: break;
    }
}

// ------------------------------------------------------------ state ----
wb::State BaseState(const std::string& why) {
    wb::State st;
    st.tick = GetTickCount64();
    st.why = why;
    return st;
}

void Send(wb::State st) {
    ReadTime(st.time);
    wb::Publish(std::move(st));
}

void Publish(Il2CppObject* reg) {
    if (!reg) {
        Send(BaseState("Load a save to change the weather."));
        return;
    }
    if (!Ready(reg) || !EnsureCatalog(reg)) {
        Send(BaseState("The weather is not running yet."));
        return;
    }
    wb::State st = BaseState("");
    st.inGame = true;
    Il2CppObject* here = PlayerZone();
    for (size_t i = 0; i < g_zones.size(); ++i) {
        const ZoneC& z = g_zones[i];
        wb::Zone wz;
        wz.id = reinterpret_cast<uintptr_t>(z.obj);
        wz.name = z.name;
        for (const auto& p : z.patterns) {
            wb::Pattern wp;
            wp.id = reinterpret_cast<uintptr_t>(p.obj);
            wp.name = p.name;
            wp.icon = p.icon;
            for (const auto& s : p.states) wp.states.push_back({reinterpret_cast<uintptr_t>(s.obj), s.name, s.tier, s.hours});
            wz.patterns.push_back(std::move(wp));
        }
        st.zones.push_back(std::move(wz));
        if (z.obj == here) st.here = static_cast<int>(i);
        if (!g_show.empty() && z.asset == g_show) st.shown = static_cast<int>(i);
    }
    if (st.here < 0 && here && unity::IsAlive(here)) {  // a zone without weather (the ranch): its name, cached
        static const Il2CppObject* s_named = nullptr;
        static std::string s_name;
        if (here != s_named) {
            s_named = here;
            s_name = Localized(unity::Field<Il2CppObject*>(here, g_offZoneName));
            if (s_name.empty()) s_name = unity::GetName(here);
        }
        st.hereName = s_name;
    }
    if (st.shown < 0) st.shown = st.here;
    for (const auto& [zone, ov] : g_over) {
        ++st.overriddenZones;
        st.lockedZones += ov.locked;
    }
    if (st.shown >= 0) {
        ZoneC& z = g_zones[static_cast<size_t>(st.shown)];
        auto findPattern = [&](const Il2CppObject* def) -> const PatternC* {
            for (const auto& p : z.patterns)
                if (p.obj == def) return &p;
            return nullptr;
        };
        auto fill = [&](const Il2CppObject* def, Il2CppObject* state, std::string& name, std::string& sname,
                        uint32_t& icon, int& tier) {
            const PatternC* p = findPattern(def);
            name = p ? p->name : PatternName(z, def);
            icon = p ? p->icon : 0;
            if (p)
                for (const auto& s : p->states)
                    if (s.obj == state) sname = s.name, tier = s.tier;
            if (sname.empty() && IsA(state, g_stateDefClass)) {
                sname = Resolver::Get().ToUtf8(unity::Field<Il2CppString*>(state, g_offStateName));
                tier = std::clamp(unity::Field<int>(state, g_offStateTier), 0, 3);
            }
        };
        for (const RunningC& r : Running(reg, z)) {
            wb::Running wr;
            fill(r.def, r.state, wr.name, wr.state, wr.icon, wr.tier);
            st.running.push_back(std::move(wr));
        }
        double now = 0;
        if (WorldTime(now)) {
            for (const ForecastC& f : ReadForecast(reg, z)) {
                if (f.endKnown && f.end <= now) continue;
                wb::Forecast wf;
                fill(f.pattern, f.state, wf.name, wf.state, wf.icon, wf.tier);
                wf.startIn = (f.start - now) / kSecsPerHour;
                wf.endIn = f.endKnown ? (f.end - now) / kSecsPerHour : -1;
                wf.running = f.started && f.start <= now;
                st.forecast.push_back(std::move(wf));
            }
            std::sort(st.forecast.begin(), st.forecast.end(),
                      [](const wb::Forecast& a, const wb::Forecast& b) { return a.startIn < b.startIn; });
            if (st.forecast.size() > 8) st.forecast.resize(8);
        }
        if (auto it = g_over.find(z.asset); it != g_over.end()) {
            st.mode = it->second.locked ? 2 : 1;
            st.overrideName = it->second.label;
            if (!it->second.locked && now > 0) st.endsIn = std::max(0.0, (it->second.until - now) / kSecsPerHour);
        }
    }
    Send(std::move(st));
}

// The same weather in another zone: the pattern itself, else one of the same weather type
// (WeatherTypeMetadata), else one with the same name; its state by asset, name or position.
const PatternC* Match(const ZoneC& z, const PatternC& want) {
    for (const auto& p : z.patterns)
        if (p.obj == want.obj) return &p;
    if (want.meta)
        for (const auto& p : z.patterns)
            if (p.meta == want.meta) return &p;
    for (const auto& p : z.patterns)
        if (p.name == want.name) return &p;
    return nullptr;
}
const StateC* MatchState(const PatternC& p, const PatternC& from, const StateC& want) {
    for (const auto& s : p.states)
        if (s.obj == want.obj || s.asset == want.asset) return &s;
    for (const auto& s : p.states)
        if (s.name == want.name) return &s;
    if (p.states.empty()) return nullptr;
    size_t idx = 0;
    for (size_t i = 0; i < from.states.size(); ++i)
        if (from.states[i].obj == want.obj) idx = i;
    return &p.states[std::min(idx, p.states.size() - 1)];
}

void DoStart(Il2CppObject* reg, const wb::Action& a) {
    // The chosen pattern / state, wherever in the catalog they are (the ids are checked here).
    const PatternC* want = nullptr;
    const StateC* wantState = nullptr;
    ZoneC* base = a.zone ? ZoneByObj(reinterpret_cast<void*>(a.zone)) : ZoneByObj(PlayerZone());
    if (a.pattern) {
        for (auto& z : g_zones)
            for (auto& p : z.patterns)
                if (reinterpret_cast<uintptr_t>(p.obj) == a.pattern) {
                    want = &p;
                    if (!base && !a.all) base = &z;
                }
        if (!want) return Done(false, "Weather not changed: that weather is no longer known (the save changed?).");
        for (const auto& s : want->states)
            if (reinterpret_cast<uintptr_t>(s.obj) == a.state) wantState = &s;
        if (!wantState && !want->states.empty()) wantState = &want->states.front();
        if (!wantState) return Done(false, std::format("Weather not changed: {} has no states.", want->name));
    }
    if (!a.all && !base) return Done(false, "Weather not changed: there is no weather where you are.");
    double now = 0;
    if (!a.lock && !WorldTime(now)) return Done(false, "Weather not changed: the game time is not available.");
    const float hours = a.pattern && wantState && wantState->hours > 0 ? std::clamp(wantState->hours, kMinHours, kMaxHours)
                                                                      : kDefaultHours;
    std::vector<ZoneC*> targets;
    if (a.all)
        for (auto& z : g_zones) targets.push_back(&z);
    else
        targets.push_back(base);
    int applied = 0;
    bool locksChanged = false;
    std::string zones, skipped;
    for (ZoneC* z : targets) {
        Override ov;
        ov.locked = a.lock;
        ov.until = now + hours * kSecsPerHour;
        if (want) {
            const PatternC* p = Match(*z, *want);
            const StateC* s = p ? MatchState(*p, *want, *wantState) : nullptr;
            if (!p || !s) {
                skipped += (skipped.empty() ? "" : ", ") + z->name;
                continue;
            }
            ov.pattern = p->asset;
            ov.state = s->asset;
            ov.label = p->states.size() > 1 ? std::format("{} - {}", p->name, s->name) : p->name;
        } else {
            ov.label = "Clear skies";
        }
        const auto old = g_over.find(z->asset);
        locksChanged |= a.lock || (old != g_over.end() && old->second.locked);
        g_over[z->asset] = ov;
        std::string what;
        Apply(reg, *z, ov, what);
        SR2_INFO("[{}] weather: {} requested in {} ({}): {}", kModule, ov.label, z->name,
                 a.lock ? "locked" : std::format("for {:g} game hours", hours), what.empty() ? "already so" : what);
        zones += (zones.empty() ? "" : ", ") + z->name;
        ++applied;
    }
    if (locksChanged) SaveLocks();
    const std::string label = want ? (want->states.size() > 1 ? std::format("{} - {}", want->name, wantState->name) : want->name)
                                   : std::string("Clear skies");
    if (applied == 0)
        return Done(false, std::format("Weather not changed: no zone has {}.", label));
    Done(true, std::format("Weather: {} in {}{}{}", label, a.all ? std::format("{} zone(s)", applied) : zones,
                           a.lock ? ", locked (kept in this save)" : std::format(" for {:g} game hours", hours),
                           skipped.empty() ? "" : std::format(" (not in {}: no such weather there)", skipped)));
}

void DoNatural(Il2CppObject* reg, const wb::Action& a) {
    std::vector<ZoneC*> targets;
    if (a.all) {
        for (const auto& [zone, ov] : g_over)
            if (ZoneC* z = ZoneByAsset(zone)) targets.push_back(z);
    } else if (ZoneC* z = a.zone ? ZoneByObj(reinterpret_cast<void*>(a.zone)) : ZoneByObj(PlayerZone())) {
        targets.push_back(z);
    }
    if (targets.empty()) return Done(false, a.all ? "No zone has changed weather." : "There is no weather where you are.");
    bool locksChanged = false;
    for (ZoneC* z : targets) {
        if (auto it = g_over.find(z->asset); it != g_over.end()) {
            locksChanged |= it->second.locked;
            g_over.erase(it);
        }
        SR2_INFO("[{}] weather: {} back to natural weather: {}", kModule, z->name, RestoreNatural(reg, *z));
    }
    if (a.all) g_over.clear();
    if (locksChanged) SaveLocks();
    Done(true, targets.size() == 1 ? std::format("Weather: {} back to natural weather", targets.front()->name)
                                   : std::format("Weather: {} zones back to natural weather", targets.size()));
}

void Handle(const wb::Action& a) {
    if (a.op == wb::Op::TimeSkip || a.op == wb::Op::TimeRewind || a.op == wb::Op::TimePause || a.op == wb::Op::TimeStop) {
        if (!Live()) return sr2::ui::sandbox::SetStatus("Sandbox is off.", true);
        if (!g_timeWhy.empty()) return Done(false, "Time of day unavailable: " + g_timeWhy);
        DoTime(a);
        if (g_why.empty()) Publish(Registry());
        else Send(BaseState("Weather changer unavailable: " + g_why));
        return;
    }
    if (!Live() || !g_why.empty()) {
        sr2::ui::sandbox::SetStatus(g_why.empty() ? "Sandbox is off." : "Weather changer unavailable: " + g_why, true);
        return;
    }
    Il2CppObject* reg = Registry();
    if (!reg || !Ready(reg) || !EnsureCatalog(reg)) {
        Done(false, "Weather not changed: the weather is not running (load a save first).");
        return;
    }
    switch (a.op) {
        case wb::Op::Show: {
            const ZoneC* z = a.zone ? ZoneByObj(reinterpret_cast<void*>(a.zone)) : nullptr;
            g_show = z ? z->asset : std::string();
            break;
        }
        case wb::Op::Start: DoStart(reg, a); break;
        case wb::Op::Natural: DoNatural(reg, a); break;
    }
    Publish(reg);
}

void HandleThunk(const wb::Action& a) { Handle(a); }

}  // namespace

void SetupWeather(sr2::core::ModuleContext& ctx) {
    auto& R = ctx.resolver();
    std::string& w = g_why;
    auto cls = [&](const char* image, const char* ns, const char* name) -> Il2CppClass* {
        std::string err;
        Il2CppClass* k = R.Class(image, ns, name, &err);
        if (!k && w.empty()) w = err.empty() ? std::format("class {}.{} not found", ns, name) : err;
        return k;
    };
    auto off = [&](Il2CppClass* k, const char* field, size_t& out) {
        if (!k) return;
        std::string err;
        if (auto o = R.FieldOffset(k, field, &err)) out = *o;
        else if (w.empty()) w = err.empty() ? std::format("field {} not found", field) : err;
    };
    auto method = [&](Il2CppClass* k, const char* name, int argc) -> const MethodInfo* {
        if (!k) return nullptr;
        std::string err;
        const MethodInfo* m = R.Method(k, name, argc, &err);
        if (!m && w.empty()) w = std::format("method {}({}) not found", name, argc);
        return m;
    };
    Il2CppClass* scu = cls(kImage, "MonomiPark.SlimeRancher.Util", "SceneContextUtility");
    Il2CppClass* sceneCtx = cls(kImage, "", "SceneContext");
    Il2CppClass* tracker = cls(kImage, "", "PlayerZoneTracker");
    Il2CppClass* timeDir = cls(kImage, "", "TimeDirector");
    g_registryClass = cls(kImage, kWeatherNs, "WeatherRegistry");
    g_patternClass = cls(kImage, kWeatherNs, "WeatherPattern");
    g_patternDefClass = cls(kImage, kWeatherNs, "WeatherPatternDefinition");
    g_stateDefClass = cls(kImage, kWeatherNs, "WeatherStateDefinition");
    Il2CppClass* meta = cls(kImage, kWeatherNs, "WeatherTypeMetadata");
    Il2CppClass* zone = cls(kImage, "MonomiPark.SlimeRancher.World", "ZoneDefinition");
    g_tryGetRegistry = method(scu, "TryGetWeatherRegistry", 1);
    g_tryGetTime = method(scu, "TryGetTimeDirector", 1);
    g_tryGetScene = method(scu, "TryGetSceneContext", 1);
    g_scZoneTracker = method(sceneCtx, "get_PlayerZoneTracker", 0);
    g_currentZone = method(tracker, "GetCurrentZone", 0);
    g_worldTime = method(timeDir, "WorldTime", 0);
    g_setWeather = method(g_registryClass, "DEBUG_SetZoneWeather", 3);
    g_getZones = method(g_registryClass, "DEBUG_GetWeatherZones", 0);
    g_getPatterns = method(g_registryClass, "DEBUG_GetZoneWeathers", 1);
    g_getRunning = method(g_registryClass, "DEBUG_GetRunningWeather", 1);
    g_getForecast = method(g_registryClass, "DEBUG_GetForecast", 1);
    off(g_registryClass, "_model", g_offRegModel);
    off(g_registryClass, "_firstLoad", g_offRegFirstLoad);
    off(g_patternClass, "_definition", g_offPatDef);
    off(g_patternClass, "_activeState", g_offPatState);
    off(g_patternDefClass, "Metadata", g_offDefMeta);
    off(g_patternDefClass, "StartingTransitions", g_offDefStart);
    off(g_patternDefClass, "RunningTransitions", g_offDefRun);
    off(g_stateDefClass, "StateName", g_offStateName);
    off(g_stateDefClass, "MapTier", g_offStateTier);
    off(g_stateDefClass, "MinDurationHours", g_offStateHours);
    off(meta, "PediaEntry", g_offMetaPedia);
    off(meta, "Icon", g_offMetaIcon);
    off(zone, "_localizedName", g_offZoneName);
    {
        // Names only: without them the asset names are shown.
        std::string err;
        if (Il2CppClass* pedia = R.Class(kImage, "MonomiPark.SlimeRancher.Pedia", "PediaEntry", &err))
            g_pediaTitle = R.Method(pedia, "get_Title", 0, &err);
        if (Il2CppClass* loc = R.Class("Unity.Localization", "UnityEngine.Localization", "LocalizedString", &err))
            g_locGet = R.Method(loc, "GetLocalizedString", 0, &err);
        if (!g_pediaTitle || !g_locGet) SR2_INFO("[{}] weather: localized names unavailable ({}); asset names shown", kModule, err);
    }
    if (w.empty() && g_setWeather)
        SR2_INFO("[{}] weather changer ready: WeatherRegistry.DEBUG_SetZoneWeather +0x{:X}", kModule,
                 Resolver::Rva(R.MethodPointer(g_setWeather)));
    else
        SR2_WARN("[{}] weather changer unavailable: {}", kModule, w);
    {
        // The clock (optional; its own reason, so the weather works without it and the other way round).
        std::string& tw = g_timeWhy;
        auto toff = [&](Il2CppClass* k, const char* field, size_t& out) {
            std::string err;
            if (!k) {
                if (tw.empty()) tw = std::format("no class for {}", field);
            } else if (auto o = R.FieldOffset(k, field, &err)) {
                out = *o;
            } else if (tw.empty()) {
                tw = err.empty() ? std::format("field {} not found", field) : err;
            }
        };
        std::string err;
        Il2CppClass* td = R.Class(kImage, "", "TimeDirector", &err);
        Il2CppClass* wm = R.Class(kImage, "MonomiPark.SlimeRancher.DataModel", "WorldModel", &err);
        toff(td, "_worldModel", g_offTdModel);
        toff(td, "FfSecsPerGameDay", g_offTdFfSecs);
        toff(td, "SecsPerGameDay", g_offTdSecs);
        toff(td, "_lastDay", g_offTdLastDay);
        toff(td, "_lastHour", g_offTdLastHour);
        toff(wm, "worldTime", g_offWmTime);
        toff(wm, "worldTimeStart", g_offWmStart);
        toff(wm, "fastForwardUntil", g_offWmFf);
        toff(wm, "pauseWorldTime", g_offWmPause);
        toff(wm, "lastWorldTime", g_offWmLast);
        g_fastForwardTo = td ? R.Method(td, "FastForwardTo", 1, &err) : nullptr;
        g_ffEnd = td ? R.Method(td, "ForceFastForwardEnd", 0, &err) : nullptr;
        if (tw.empty() && (!g_fastForwardTo || !g_ffEnd)) tw = "TimeDirector.FastForwardTo / ForceFastForwardEnd not found";
        if (tw.empty() && (!g_tryGetTime || !g_worldTime)) tw = "SceneContextUtility.TryGetTimeDirector not found";
        if (tw.empty())
            SR2_INFO("[{}] time of day ready: TimeDirector.FastForwardTo +0x{:X}, WorldModel.worldTime 0x{:X} "
                     "pauseWorldTime 0x{:X} lastWorldTime 0x{:X}",
                     kModule, Resolver::Rva(R.MethodPointer(g_fastForwardTo)), g_offWmTime, g_offWmPause, g_offWmLast);
        else
            SR2_WARN("[{}] time of day unavailable: {}", kModule, tw);
    }
}

void WeatherEnabled(bool on) {
    g_live.store(on);
    wb::SetHandler(on ? &HandleThunk : nullptr);
    if (!on) wb::Publish(BaseState("Sandbox is off."));
    if (!on && g_pausedByUs && sr2::core::main_thread::Ready())  // a clock paused from the card runs again
        sr2::core::main_thread::Post([] {
            if (!g_pausedByUs || sr2::core::ModuleFaulted(kModule) || g_live.load()) return;
            g_pausedByUs = false;
            if (Il2CppObject* wm = WorldModel(TimeDir())) unity::Field<bool>(wm, g_offWmPause) = false;
            SR2_INFO("[{}] time runs again (Sandbox off)", kModule);
        });
}

void WeatherTick(bool snapshot) {
    if (!Live()) return;
    const unsigned long long now = GetTickCount64();
    if (!g_why.empty()) {
        if (snapshot && now - g_lastSnap >= kSnapshotMs) {
            g_lastSnap = now;
            Send(BaseState("Weather changer unavailable: " + g_why));
        }
        return;
    }
    if (const unsigned v = SaveTogglesVersion(); v != g_dataSeen) {  // another save (or the title screen)
        g_dataSeen = v;
        g_pausedByUs = false;  // a loaded save's clock runs (pauseWorldTime is not saved)
        g_timeLogged = false;
        LoadLocks();
    }
    const bool snap = snapshot && now - g_lastSnap >= kSnapshotMs;
    const bool enforce = !g_over.empty() && now - g_lastEnforce >= kEnforceMs;
    if (!snap && !enforce) return;
    Il2CppObject* reg = Registry();
    if (enforce) {
        g_lastEnforce = now;
        if (reg && Ready(reg) && EnsureCatalog(reg)) Enforce(reg);
    }
    if (snap) {
        g_lastSnap = now;
        Publish(reg);
    }
}

}  // namespace sr2::modules::sandbox
