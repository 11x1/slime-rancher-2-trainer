#include "settings.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <format>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>

#include "guard.h"
#include "json.h"
#include "log.h"
#include "settings_internal.h"

namespace sr2::core::settings {
namespace {

using Clock = std::chrono::steady_clock;
constexpr auto kSaveDelay = std::chrono::milliseconds(1000);

struct Entry {
    Def def;
    double value = 0;
};

struct Listener {
    int id = 0;
    std::string module;
    std::function<void(const std::string&)> fn;
};

std::mutex g_mutex;  // guards everything below
std::vector<Entry> g_entries;
std::vector<std::string> g_modules;
std::vector<Listener> g_listeners;
int g_nextListener = 1;

json::Value g_doc;                  // last known document (config at Init, then what we wrote)
std::filesystem::path g_file;       // sr2mods.json ("" = persistence off)
std::set<std::string> g_persist;    // "module\nkey" entries to write (present in file or changed)
bool g_dirty = false;
Clock::time_point g_lastChange{};
HANDLE g_wake = nullptr;            // auto-reset event for the saver thread
bool g_saverStarted = false;
bool g_parseErrorLogged = false;
std::string g_saveError;            // why the last save failed ("" = it succeeded)
std::map<std::string, std::string> g_rootStrings;  // top-level strings to write ("active_profile")
struct Stashed {
    std::string module, key;
    json::Value value;
};
std::map<std::string, Stashed> g_stashed;  // PKey -> saved value of an unregistered setting
struct Pin {
    double value = 0;
    std::string reason;
};
std::map<std::string, Pin> g_locks;  // PKey -> pinned value (settings::Lock)

std::string PKey(std::string_view m, std::string_view k) { return std::format("{}\n{}", m, k); }

void SetSaveError(std::string e) {
    std::lock_guard lock(g_mutex);
    g_saveError = std::move(e);
}

// A failed save keeps the changes pending: retry a few seconds later (and at Flush).
constexpr auto kRetryDelay = std::chrono::seconds(4);  // on top of kSaveDelay
void RetryLaterLocked() {
    g_dirty = true;
    g_lastChange = Clock::now() + kRetryDelay;
}
void RetryLater() {
    std::lock_guard lock(g_mutex);
    RetryLaterLocked();
}

Entry* FindLocked(std::string_view module, std::string_view key) {
    for (Entry& e : g_entries)
        if (e.def.module == module && e.def.key == key) return &e;
    return nullptr;
}

double Clamp(const Def& d, double v) {
    if (!std::isfinite(v)) v = d.def;
    switch (d.kind) {
        case Kind::Bool: return v != 0 ? 1.0 : 0.0;
        case Kind::Enum: {
            v = std::round(v);
            double hi = d.options.empty() ? d.max : static_cast<double>(d.options.size() - 1);
            return std::clamp(v, 0.0, std::max(0.0, hi));
        }
        case Kind::Key: return std::clamp(std::round(v), 0.0, 255.0);
        case Kind::Int:
            v = std::round(v);
            return d.max > d.min ? std::clamp(v, std::round(d.min), std::round(d.max)) : v;
        case Kind::Float: return d.max > d.min ? std::clamp(v, d.min, d.max) : v;
    }
    return v;
}

const json::Value* SavedLocked(std::string_view module, std::string_view key) {
    const json::Value* v = g_doc["modules"][module].find(key);
    return (v && (v->isBool() || v->isNumber())) ? v : nullptr;
}

json::Value ToJson(const Def& d, double v) {
    switch (d.kind) {
        case Kind::Bool: return json::Value(v != 0);
        case Kind::Float: {
            // 6 significant digits keeps float noise (0.30000001192...) out of the file.
            double r = v;
            std::string s = std::format("{:.6g}", v);
            std::istringstream(s) >> r;
            return json::Value(r);
        }
        default: return json::Value(std::round(v));
    }
}

bool ReadFile(const std::filesystem::path& p, std::string& out) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return false;
    std::stringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

// Builds the document (fresh read of the file, so edits made by hand while the
// game runs are kept) and writes it atomically. Called WITHOUT g_mutex held.
void SaveNow() {
    std::filesystem::path file;
    json::Value doc;
    std::vector<std::pair<std::string, std::pair<std::string, json::Value>>> values;  // module, key, value
    std::map<std::string, std::string> rootStrings;
    {
        std::lock_guard lock(g_mutex);
        if (!g_dirty || g_file.empty()) return;
        g_dirty = false;
        file = g_file;
        doc = g_doc;
        for (const Entry& e : g_entries)
            if (g_persist.contains(PKey(e.def.module, e.def.key)))
                values.push_back({e.def.module, {e.def.key, ToJson(e.def, e.value)}});
        for (const auto& [pk, st] : g_stashed) values.push_back({st.module, {st.key, st.value}});
        rootStrings = g_rootStrings;
    }

    std::string text, perr;
    std::error_code ec;
    if (std::filesystem::exists(file, ec)) {
        json::Value fresh;
        if (ReadFile(file, text) && json::Parse(text, fresh, perr) && fresh.isObject()) {
            doc = std::move(fresh);
        } else {
            std::lock_guard lock(g_mutex);
            g_saveError = "sr2mods.json is not valid JSON; not overwriting it";
            RetryLaterLocked();
            if (!g_parseErrorLogged) {
                SR2_ERROR("settings: {} is not valid JSON ({}); not overwriting it - fix or delete the file",
                          log::Narrow(file.wstring()), perr.empty() ? "unreadable" : perr);
                g_parseErrorLogged = true;
            }
            return;
        }
    }
    if (!doc.isObject()) doc = json::Value::Object();

    for (auto& [module, kv] : values) doc.child("modules").child(module).set(kv.first, kv.second);
    for (auto& [k, v] : rootStrings) doc.set(k, json::Value(v));

    std::string out = json::Serialize(doc);
    std::filesystem::path tmp = file;
    tmp += L".tmp";
    {
        std::ofstream o(tmp, std::ios::binary | std::ios::trunc);
        if (!o || !o.write(out.data(), static_cast<std::streamsize>(out.size()))) {
            SR2_ERROR("settings: cannot write {}", log::Narrow(tmp.wstring()));
            SetSaveError("cannot write " + log::Narrow(tmp.filename().wstring()));
            RetryLater();
            return;
        }
    }
    if (!MoveFileExW(tmp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const DWORD gle = GetLastError();
        SR2_ERROR("settings: cannot replace {} (error {})", log::Narrow(file.wstring()), gle);
        SetSaveError(std::format("cannot replace sr2mods.json (error {})", gle));
        DeleteFileW(tmp.c_str());
        RetryLater();
        return;
    }
    {
        std::lock_guard lock(g_mutex);
        g_doc = std::move(doc);
        g_parseErrorLogged = false;
        g_saveError.clear();
    }
    SR2_DEBUG("settings: saved {} value(s) to sr2mods.json", values.size());
}

DWORD WINAPI SaverThread(LPVOID) {
    for (;;) {
        DWORD wait = INFINITE;
        {
            std::lock_guard lock(g_mutex);
            if (g_dirty) {
                auto due = g_lastChange + kSaveDelay;
                auto now = Clock::now();
                wait = due <= now ? 0
                                  : static_cast<DWORD>(
                                        std::chrono::duration_cast<std::chrono::milliseconds>(due - now).count() + 1);
            }
        }
        if (wait) WaitForSingleObject(g_wake, wait);
        bool due = false;
        {
            std::lock_guard lock(g_mutex);
            due = g_dirty && Clock::now() - g_lastChange >= kSaveDelay;
        }
        if (due) {
            std::string err;
            if (!Try([] { SaveNow(); }, &err)) {
                SR2_ERROR("settings: save failed: {}", err);
                SetSaveError(err);
                RetryLater();
            }
        }
    }
}

void StartSaverLocked() {
    if (g_saverStarted || g_file.empty()) return;
    g_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!g_wake) return;
    if (HANDLE t = CreateThread(nullptr, 0, &SaverThread, nullptr, 0, nullptr)) {
        CloseHandle(t);
        g_saverStarted = true;
    }
}

}  // namespace

void Init(const json::Value& document, const std::filesystem::path& file) {
    std::lock_guard lock(g_mutex);
    g_doc = document.isObject() ? document : json::Value::Object();
    g_file = file;
    // Values that already exist in the file keep being written back.
    const json::Value& mods = g_doc["modules"];
    for (size_t i = 0; i < mods.keys().size(); ++i) {
        const json::Value& sect = mods.items()[i];
        for (const std::string& k : sect.keys()) g_persist.insert(PKey(mods.keys()[i], k));
    }
    StartSaverLocked();
}

void Register(const Def& d) {
    if (d.module.empty() || d.key.empty()) {
        SR2_WARN("settings: Register with empty module/key ignored");
        return;
    }
    std::lock_guard lock(g_mutex);
    if (Entry* e = FindLocked(d.module, d.key)) {
        const std::string parent = e->def.parent;
        const int parentValue = e->def.parentValue;
        const Def old = e->def;
        e->def = d;  // refresh metadata, keep the current value
        if (d.parent.empty()) {  // e.g. the module manager re-registering "enabled": keep a parent set earlier
            e->def.parent = parent;
            e->def.parentValue = parentValue;
        }
        if (d.unit.empty() && d.zeroText.empty() && d.displayScale == 1) {  // keep a display set with SetDisplay
            e->def.unit = old.unit;
            e->def.zeroText = old.zeroText;
            e->def.displayScale = old.displayScale;
        }
        e->value = Clamp(e->def, e->value);
        return;
    }
    Entry e{d, 0};
    e.def.def = Clamp(d, d.def);
    if (e.def.label.empty()) e.def.label = d.key;
    double v = e.def.def;
    if (const json::Value* saved = SavedLocked(d.module, d.key)) v = saved->isBool() ? (saved->asBool() ? 1 : 0) : saved->asNumber();
    e.value = Clamp(e.def, v);
    g_stashed.erase(PKey(d.module, d.key));  // the entry (persisted via g_persist) takes over
    g_entries.push_back(std::move(e));
    if (std::find(g_modules.begin(), g_modules.end(), d.module) == g_modules.end()) g_modules.push_back(d.module);
}

double Get(std::string_view module, std::string_view key) {
    std::lock_guard lock(g_mutex);
    if (const Entry* e = FindLocked(module, key)) return e->value;
    if (const json::Value* saved = SavedLocked(module, key)) return saved->isBool() ? (saved->asBool() ? 1 : 0) : saved->asNumber();
    return 0;
}

bool GetBool(std::string_view module, std::string_view key) { return Get(module, key) != 0; }
float GetFloat(std::string_view module, std::string_view key) { return static_cast<float>(Get(module, key)); }
int GetInt(std::string_view module, std::string_view key) { return static_cast<int>(std::lround(Get(module, key))); }

bool Has(std::string_view module, std::string_view key) {
    std::lock_guard lock(g_mutex);
    return FindLocked(module, key) != nullptr;
}

bool Saved(std::string_view module, std::string_view key) {
    std::lock_guard lock(g_mutex);
    return SavedLocked(module, key) != nullptr;
}

void SetParent(std::string_view module, std::string_view key, std::string_view parent, int parentValue) {
    if (parent == key) return;
    std::lock_guard lock(g_mutex);
    if (Entry* e = FindLocked(module, key)) {
        e->def.parent = std::string(parent);
        e->def.parentValue = parentValue;
    }
}

void SetDisplay(std::string_view module, std::string_view key, std::string_view unit, std::string_view zeroText,
                double displayScale) {
    std::lock_guard lock(g_mutex);
    if (Entry* e = FindLocked(module, key)) {
        e->def.unit = std::string(unit);
        e->def.zeroText = std::string(zeroText);
        e->def.displayScale = displayScale > 0 ? displayScale : 1;
    }
}

bool ParentAllows(const Def& d, double parentValue) {
    if (d.parent.empty()) return true;
    if (d.parentValue < 0) return std::fabs(parentValue) >= 0.5;
    return std::lround(parentValue) == d.parentValue;
}

bool Visible(std::string_view module, std::string_view key) {
    std::lock_guard lock(g_mutex);
    const Entry* e = FindLocked(module, key);
    if (!e) return true;
    if (key != "enabled")
        if (const Entry* en = FindLocked(module, "enabled"); en && en->value < 0.5) return false;
    for (int depth = 0; depth < 8 && !e->def.parent.empty(); ++depth) {  // depth cap: cycles in a module's table
        const Entry* p = FindLocked(module, e->def.parent);
        if (!p) return true;  // parent not registered (feature unavailable): nothing to hide behind
        if (!ParentAllows(e->def, p->value)) return false;
        e = p;
    }
    return true;
}

void Set(std::string_view module, std::string_view key, double v) {
    std::vector<std::function<void(const std::string&)>> notify;
    std::string skey(key);
    {
        std::lock_guard lock(g_mutex);
        Entry* e = FindLocked(module, key);
        if (!e) {
            SR2_WARN("settings: Set({}.{}) on an unregistered setting ignored", module, key);
            return;
        }
        double nv = Clamp(e->def, v);
        if (auto pin = g_locks.find(PKey(module, key)); pin != g_locks.end() && nv != pin->second.value) {
            SR2_DEBUG("settings: {}.{} is locked ({}) - change ignored", module, key, pin->second.reason);
            return;
        }
        if (nv == e->value) return;
        e->value = nv;
        g_persist.insert(PKey(module, key));
        g_dirty = true;
        g_lastChange = Clock::now();
        if (g_wake) SetEvent(g_wake);
        for (const Listener& l : g_listeners)
            if (l.module == module) notify.push_back(l.fn);
    }
    SR2_DEBUG("settings: {}.{} = {}", module, key, Get(module, key));
    for (auto& fn : notify) {
        std::string err;
        if (!Try([&] { fn(skey); }, &err)) SR2_ERROR("settings: OnChange({}) callback failed for '{}': {}", module, skey, err);
    }
}

void Reset(std::string_view module, std::string_view key) {
    double def = 0;
    {
        std::lock_guard lock(g_mutex);
        const Entry* e = FindLocked(module, key);
        if (!e) return;
        def = e->def.def;
    }
    Set(module, key, def);
}

void Lock(std::string_view module, std::string_view key, double value, std::string reason) {
    {
        std::lock_guard lock(g_mutex);
        const Entry* e = FindLocked(module, key);
        if (!e) return;
        g_locks[PKey(module, key)] = Pin{Clamp(e->def, value), std::move(reason)};
    }
    Set(module, key, value);
}

void Unlock(std::string_view module, std::string_view key) {
    std::lock_guard lock(g_mutex);
    g_locks.erase(PKey(module, key));
}

std::string LockReason(std::string_view module, std::string_view key) {
    std::lock_guard lock(g_mutex);
    auto it = g_locks.find(PKey(module, key));
    return it != g_locks.end() ? it->second.reason : std::string();
}

int OnChange(std::string_view module, std::function<void(const std::string& key)> fn) {
    if (!fn) return 0;
    std::lock_guard lock(g_mutex);
    int id = g_nextListener++;
    g_listeners.push_back({id, std::string(module), std::move(fn)});
    return id;
}

void RemoveOnChange(int id) {
    std::lock_guard lock(g_mutex);
    std::erase_if(g_listeners, [id](const Listener& l) { return l.id == id; });
}

std::vector<Def> All() {
    std::lock_guard lock(g_mutex);
    std::vector<Def> out;
    out.reserve(g_entries.size());
    for (const Entry& e : g_entries) out.push_back(e.def);
    return out;
}

std::vector<std::string> Modules() {
    std::lock_guard lock(g_mutex);
    return g_modules;
}

// ---- core-internal (settings_internal.h) ----

std::string RootString(std::string_view key) {
    std::lock_guard lock(g_mutex);
    auto it = g_rootStrings.find(std::string(key));
    if (it != g_rootStrings.end()) return it->second;
    return g_doc[key].asString();
}

void SetRootString(std::string_view key, std::string value) {
    std::lock_guard lock(g_mutex);
    std::string k(key);
    auto it = g_rootStrings.find(k);
    const bool present = it != g_rootStrings.end() || g_doc.find(key) != nullptr;
    const std::string cur = it != g_rootStrings.end() ? it->second : g_doc[key].asString();
    if (present && cur == value) return;
    g_rootStrings[k] = std::move(value);
    g_dirty = true;
    g_lastChange = Clock::now();
    if (g_wake) SetEvent(g_wake);
}

void StashSaved(std::string_view module, std::string_view key, const json::Value& v) {
    if (module.empty() || key.empty() || !(v.isBool() || v.isNumber())) return;
    if (v.isNumber() && !std::isfinite(v.asNumber())) return;
    std::lock_guard lock(g_mutex);
    if (FindLocked(module, key)) return;
    // Register() looks saved values up in g_doc: put it there right away.
    g_doc.child("modules").child(module).set(key, v);
    std::string pk = PKey(module, key);
    g_persist.insert(pk);
    g_stashed[pk] = Stashed{std::string(module), std::string(key), v};
    g_dirty = true;
    g_lastChange = Clock::now();
    if (g_wake) SetEvent(g_wake);
}

json::Value Snapshot(std::string_view skipModule) {
    std::map<std::string, std::map<std::string, json::Value>> out;
    {
        std::lock_guard lock(g_mutex);
        const json::Value& mods = g_doc["modules"];
        for (const std::string& m : g_modules) {
            if (m == skipModule) continue;
            auto& sect = out[m];
            const json::Value& saved = mods[m];
            for (size_t i = 0; i < saved.keys().size() && i < saved.items().size(); ++i) {
                const json::Value& v = saved.items()[i];
                if (!(v.isBool() || v.isNumber())) continue;
                if (FindLocked(m, saved.keys()[i])) continue;
                sect[saved.keys()[i]] = v;
            }
        }
        for (const Entry& e : g_entries)
            if (e.def.module != skipModule) out[e.def.module][e.def.key] = ToJson(e.def, e.value);
    }
    json::Value doc = json::Value::Object();
    for (auto& [m, keys] : out) {
        json::Value& sect = doc.child(m);
        for (auto& [k, v] : keys) sect.set(k, v);
    }
    return doc;
}

bool SavedValue(std::string_view module, std::string_view key, json::Value& out) {
    std::lock_guard lock(g_mutex);
    if (FindLocked(module, key)) return false;
    if (const json::Value* v = SavedLocked(module, key)) {
        out = *v;
        return true;
    }
    return false;
}

double ClampFor(const Def& d, double v) { return Clamp(d, v); }

void Flush() {
    std::string err;
    if (!Try([] { SaveNow(); }, &err)) {
        SR2_ERROR("settings: flush failed: {}", err);
        SetSaveError(err);
        RetryLater();
    }
}

std::string LastSaveError() {
    std::lock_guard lock(g_mutex);
    return g_saveError;
}

}  // namespace sr2::core::settings
