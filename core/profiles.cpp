#include "profiles.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <exception>
#include <format>
#include <fstream>
#include <mutex>
#include <set>
#include <sstream>

#include "guard.h"
#include "json.h"
#include "log.h"
#include "main_thread.h"
#include "paths.h"
#include "settings.h"
#include "settings_internal.h"

namespace sr2::core::profiles {
namespace {

constexpr const char* kActiveKey = "active_profile";
constexpr const char* kSkipModule = "ui";  // never part of a profile
// Also never part of a profile: a profile load must not turn on the sandbox,
// which the overlay only enables after a confirmation dialog.
constexpr const char* kSandboxModule = "sandbox";
bool Skipped(std::string_view module) { return module == kSkipModule || module == kSandboxModule; }

// A settings::Snapshot without the modules profiles skip.
json::Value WithoutSkipped(const json::Value& mods) {
    json::Value out = json::Value::Object();
    for (size_t i = 0; i < mods.keys().size() && i < mods.items().size(); ++i)
        if (!Skipped(mods.keys()[i])) out.set(mods.keys()[i], mods.items()[i]);
    return out;
}
constexpr size_t kMaxNameChars = 40;
constexpr size_t kMaxDescChars = 200;
constexpr uintmax_t kMaxFileBytes = 1u << 20;

std::mutex g_mutex;  // guards everything below
std::filesystem::path g_folder;
std::vector<Info> g_list;
std::string g_status;
unsigned long long g_statusTick = 0;

// Cache of the active profile's "modules" object for IsModified().
std::string g_cacheName;  // profile the cache belongs to ("" = invalid)
bool g_cacheOk = false;
json::Value g_cacheMods;
unsigned long long g_modTick = 0;
bool g_modValue = false;

std::once_flag g_initOnce;

// ------------------------------------------------------------- helpers ----
std::wstring Widen(std::string_view s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return {};
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

char Lower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

bool EqualsNoCase(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (Lower(a[i]) != Lower(b[i])) return false;
    return true;
}

bool LessNoCase(const std::string& a, const std::string& b) {
    return std::lexicographical_compare(a.begin(), a.end(), b.begin(), b.end(),
                                        [](char x, char y) { return Lower(x) < Lower(y); });
}

bool ContainsNoCase(std::string_view hay, std::string_view needle) {
    if (needle.size() > hay.size()) return false;
    for (size_t i = 0; i + needle.size() <= hay.size(); ++i)
        if (EqualsNoCase(hay.substr(i, needle.size()), needle)) return true;
    return false;
}

std::filesystem::path FolderLocked() {
    if (g_folder.empty()) g_folder = paths::SelfDir() / L"profiles";
    return g_folder;
}

void SetStatus(const std::string& s) {
    std::lock_guard lock(g_mutex);
    g_status = s;
    g_statusTick = GetTickCount64();
}

Outcome Make(Result r, std::string msg, std::string name = {}) {
    SetStatus(msg);
    if (r == Result::Error)
        SR2_WARN("profiles: {}", msg);
    else
        SR2_INFO("profiles: {}", msg);
    return Outcome{r, std::move(msg), std::move(name)};
}

// A name that is safe to use as a file stem inside the folder. Existing files
// may have names SanitizeName() would not produce (shared files), so this is
// more permissive: anything but path / reserved characters.
bool SafeExistingName(std::string_view name) {
    if (name.empty() || name.size() > 200) return false;
    if (name.front() == ' ' || name.back() == ' ' || name.back() == '.' || name.front() == '.') return false;
    for (unsigned char c : name) {
        if (c < 0x20) return false;
        if (std::string_view("\\/:*?\"<>|").find(static_cast<char>(c)) != std::string_view::npos) return false;
    }
    return !Widen(name).empty();
}

bool PathFor(std::string_view name, std::filesystem::path& out) {
    if (!SafeExistingName(name)) return false;
    std::lock_guard lock(g_mutex);
    out = FolderLocked() / (Widen(name) + L".json");
    return true;
}

bool FileExists(const std::filesystem::path& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

bool ReadText(const std::filesystem::path& p, std::string& out, std::string& err) {
    std::error_code ec;
    uintmax_t size = std::filesystem::file_size(p, ec);
    if (ec) {
        err = "cannot read file";
        return false;
    }
    if (size > kMaxFileBytes) {
        err = "file too large";
        return false;
    }
    std::ifstream in(p, std::ios::binary);
    if (!in) {
        err = "cannot open file";
        return false;
    }
    std::stringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

// Parses a profile file: root object, optional "modules" object (missing =
// all defaults), optional "description" string.
bool ReadProfile(const std::filesystem::path& p, json::Value& mods, std::string& desc, std::string& err) {
    std::string text, perr;
    if (!ReadText(p, text, err)) return false;
    json::Value root;
    if (!json::Parse(text, root, perr)) {
        err = "invalid JSON (" + perr + ")";
        return false;
    }
    if (!root.isObject()) {
        err = "top level is not an object";
        return false;
    }
    const json::Value* m = root.find("modules");
    if (m && !m->isObject()) {
        err = "\"modules\" is not an object";
        return false;
    }
    mods = m ? *m : json::Value::Object();
    desc = root["description"].asString();
    if (desc.size() > 1000) desc.resize(1000);
    return true;
}

bool WriteAtomic(const std::filesystem::path& file, const std::string& text, std::string& err) {
    std::filesystem::path tmp = file;
    tmp += L".tmp";
    {
        std::ofstream o(tmp, std::ios::binary | std::ios::trunc);
        if (!o || !o.write(text.data(), static_cast<std::streamsize>(text.size()))) {
            err = "cannot write " + log::Narrow(tmp.wstring());
            o.close();
            DeleteFileW(tmp.c_str());
            return false;
        }
    }
    if (!MoveFileExW(tmp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        err = std::format("cannot replace {} (error {})", log::Narrow(file.wstring()), GetLastError());
        DeleteFileW(tmp.c_str());
        return false;
    }
    return true;
}

std::string CleanDescription(std::string_view d) {
    std::string out;
    for (char c : d) {
        if (static_cast<unsigned char>(c) < 0x20) c = ' ';
        out.push_back(c);
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    size_t b = out.find_first_not_of(' ');
    out = b == std::string::npos ? std::string() : out.substr(b);
    if (out.size() > kMaxDescChars) {
        out.resize(kMaxDescChars);
        // don't cut a UTF-8 sequence in half
        while (!out.empty() && (static_cast<unsigned char>(out.back()) & 0xC0) == 0x80) out.pop_back();
        if (!out.empty() && (static_cast<unsigned char>(out.back()) & 0x80)) out.pop_back();
    }
    return out;
}

json::Value ValueJson(const settings::Def& d, double v) {
    switch (d.kind) {
        case settings::Kind::Bool: return json::Value(v != 0);
        case settings::Kind::Float: {
            double r = v;
            std::istringstream(std::format("{:.6g}", v)) >> r;
            return json::Value(r);
        }
        default: return json::Value(std::round(v));
    }
}

bool NumberOf(const json::Value& v, double& out) {
    if (v.isBool()) {
        out = v.asBool() ? 1 : 0;
        return true;
    }
    if (v.isNumber() && std::isfinite(v.asNumber())) {
        out = v.asNumber();
        return true;
    }
    return false;
}

bool Nearly(double a, double b) { return std::fabs(a - b) <= 1e-5 * std::max(1.0, std::fabs(a)); }

std::string ProfileText(const json::Value& mods, const std::string& desc) {
    json::Value doc = json::Value::Object();
    if (!desc.empty()) doc.set("description", json::Value(desc));
    doc.set("modules", mods);
    return json::Serialize(doc);
}

// Module -> sorted keys -> value, as a json object (modules sorted too).
json::Value BuildModules(const std::vector<settings::Def>& defs, double (*valueOf)(const settings::Def&)) {
    std::vector<settings::Def> sorted;
    for (const auto& d : defs)
        if (!Skipped(d.module)) sorted.push_back(d);
    std::sort(sorted.begin(), sorted.end(), [](const settings::Def& a, const settings::Def& b) {
        return a.module != b.module ? a.module < b.module : a.key < b.key;
    });
    json::Value mods = json::Value::Object();
    for (const auto& d : sorted) mods.child(d.module).set(d.key, ValueJson(d, valueOf(d)));
    return mods;
}

double DefaultValue(const settings::Def& d) { return d.def; }

double VanillaValue(const settings::Def& d) {
    if (d.key == "enabled" && d.kind == settings::Kind::Bool) return 0;
    const bool multiplier = ContainsNoCase(d.key, "mult") || ContainsNoCase(d.label, "multiplier");
    if (d.kind == settings::Kind::Float && multiplier && d.min <= 1.0 && d.max >= 1.0) return 1.0;
    return d.def;
}

void WriteBuiltins(const std::filesystem::path& folder) {
    const auto defs = settings::All();
    struct B {
        const wchar_t* file;
        const char* desc;
        double (*fn)(const settings::Def&);
    };
    const B builtins[] = {
        {L"Vanilla.json", "All mods off - the unmodified game (multipliers at 1).", &VanillaValue},
        {L"Default.json", "Every mod setting at its default value.", &DefaultValue},
    };
    for (const B& b : builtins) {
        std::filesystem::path p = folder / b.file;
        if (FileExists(p)) continue;  // never overwrite
        std::string err;
        if (WriteAtomic(p, ProfileText(BuildModules(defs, b.fn), b.desc), err))
            SR2_INFO("profiles: created starter profile {}", log::Narrow(p.filename().wstring()));
        else
            SR2_WARN("profiles: {}", err);
    }
}

void RefreshImpl() {
    std::filesystem::path folder;
    {
        std::lock_guard lock(g_mutex);
        folder = FolderLocked();
    }
    std::vector<Info> list;
    std::error_code ec;
    for (std::filesystem::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec)) {
        const auto& p = it->path();
        if (!it->is_regular_file(ec) || ec) {
            ec.clear();
            continue;
        }
        if (_wcsicmp(p.extension().c_str(), L".json") != 0) continue;
        Info info;
        info.name = log::Narrow(p.stem().wstring());
        if (!SafeExistingName(info.name)) continue;
        json::Value mods;
        info.valid = ReadProfile(p, mods, info.description, info.error);
        list.push_back(std::move(info));
        if (list.size() >= 500) break;
    }
    std::sort(list.begin(), list.end(), [](const Info& a, const Info& b) { return LessNoCase(a.name, b.name); });
    std::lock_guard lock(g_mutex);
    g_list = std::move(list);
    g_cacheName.clear();  // files may have changed
    g_modTick = 0;
}

void InitImpl() {
    std::filesystem::path folder;
    {
        std::lock_guard lock(g_mutex);
        folder = FolderLocked();
    }
    std::error_code ec;
    if (!std::filesystem::exists(folder, ec)) {
        if (std::filesystem::create_directories(folder, ec))
            WriteBuiltins(folder);
        else
            SR2_WARN("profiles: cannot create {} ({})", log::Narrow(folder.wstring()), ec.message());
    }
    RefreshImpl();
    std::string active = settings::RootString(kActiveKey);
    size_t count = 0;
    {
        std::lock_guard lock(g_mutex);
        count = g_list.size();
    }
    SR2_INFO("profiles: {} profile(s) in {}; active: {}", count, log::Narrow(folder.wstring()),
             active.empty() ? std::string("(none)") : active);
}

void SetActive(const std::string& name) {
    settings::SetRootString(kActiveKey, name);
    std::lock_guard lock(g_mutex);
    g_cacheName.clear();
    g_modTick = 0;
}

// Main thread: the actual Load.
void Apply(const std::string& name, const json::Value& mods) {
    const auto defs = settings::All();
    std::set<std::string> known;
    for (const auto& d : defs) known.insert(d.module);

    // 1. Settings a module has not registered yet: store them, so the
    //    module's Register() (e.g. in Setup() when step 3 enables it) picks
    //    them up. Unknown modules are ignored.
    for (size_t i = 0; i < mods.keys().size() && i < mods.items().size(); ++i) {
        const std::string& m = mods.keys()[i];
        const json::Value& sect = mods.items()[i];
        if (Skipped(m) || !known.contains(m) || !sect.isObject()) continue;
        for (size_t k = 0; k < sect.keys().size() && k < sect.items().size(); ++k)
            if (!settings::Has(m, sect.keys()[k])) settings::StashSaved(m, sect.keys()[k], sect.items()[k]);
    }

    auto valueFor = [&](const settings::Def& d) {
        double v = d.def;
        if (const json::Value* x = mods[d.module].find(d.key)) NumberOf(*x, v);
        return v;
    };
    // 2. Every registered setting (missing -> default), "enabled" flags last
    //    so modules start/stop with their final values in place.
    int changed = 0;
    for (int pass = 0; pass < 2; ++pass) {
        for (const auto& d : settings::All()) {  // re-read: enabling may register more
            if (Skipped(d.module)) continue;
            const bool isEnabled = d.key == "enabled" && d.kind == settings::Kind::Bool;
            if (isEnabled != (pass == 1)) continue;
            double before = settings::Get(d.module, d.key);
            settings::Set(d.module, d.key, valueFor(d));
            if (!Nearly(before, settings::Get(d.module, d.key))) ++changed;
        }
    }
    SetActive(name);
    Make(Result::Ok, std::format("Loaded profile '{}' ({} value(s) changed)", name, changed), name);
}

}  // namespace

// ------------------------------------------------------------------- API ----
void Init() {
    std::call_once(g_initOnce, [] {
        std::string err;
        if (!Try([] { InitImpl(); }, &err)) SR2_ERROR("profiles: init failed: {}", err);
    });
}

std::filesystem::path Folder() {
    std::lock_guard lock(g_mutex);
    return FolderLocked();
}

std::vector<Info> List() {
    Init();
    std::lock_guard lock(g_mutex);
    return g_list;
}

void Refresh() {
    Init();
    std::string err;
    if (!Try([] { RefreshImpl(); }, &err)) SR2_ERROR("profiles: refresh failed: {}", err);
}

std::string ActiveProfile() { return settings::RootString(kActiveKey); }

bool IsModified() {
    Init();
    const std::string active = ActiveProfile();
    if (active.empty()) return false;
    const unsigned long long now = GetTickCount64();
    json::Value mods;
    {
        std::lock_guard lock(g_mutex);
        if (g_cacheName == active && g_modTick != 0 && now - g_modTick < 250) return g_modValue;
        if (g_cacheName == active) {
            if (!g_cacheOk) return false;
            mods = g_cacheMods;
        }
    }
    if (mods.isNull()) {  // (re)load the file
        std::filesystem::path p;
        std::string desc, err;
        bool ok = PathFor(active, p) && FileExists(p) && ReadProfile(p, mods, desc, err);
        std::lock_guard lock(g_mutex);
        g_cacheName = active;
        g_cacheOk = ok;
        g_cacheMods = ok ? mods : json::Value();
        g_modTick = now;
        g_modValue = false;
        if (!ok) return false;
    }
    bool modified = false;
    for (const auto& d : settings::All()) {
        if (Skipped(d.module)) continue;
        double want = d.def;
        if (const json::Value* x = mods[d.module].find(d.key)) {
            double v = 0;
            if (NumberOf(*x, v)) want = settings::ClampFor(d, v);
        }
        if (!Nearly(want, settings::Get(d.module, d.key))) {
            modified = true;
            break;
        }
    }
    std::lock_guard lock(g_mutex);
    g_modTick = now;
    g_modValue = modified;
    return modified;
}

std::string SanitizeName(std::string_view name) {
    std::string out;
    for (char c : name) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == ' ' ||
                        c == '-' || c == '_';
        if (!ok) continue;
        if (c == ' ' && (out.empty() || out.back() == ' ')) continue;
        out.push_back(c);
    }
    if (out.size() > kMaxNameChars) out.resize(kMaxNameChars);
    while (!out.empty() && out.back() == ' ') out.pop_back();
    // Reserved DOS device names cannot be file names.
    static const char* kReserved[] = {"CON", "PRN", "AUX", "NUL", "COM1", "COM2", "COM3", "COM4", "COM5", "COM6",
                                      "COM7", "COM8", "COM9", "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7",
                                      "LPT8", "LPT9"};
    for (const char* r : kReserved)
        if (EqualsNoCase(out, r)) {
            out += "_";
            break;
        }
    return out;
}

Outcome Load(std::string_view nameIn) {
    Init();
    std::string name(nameIn);
    std::filesystem::path p;
    if (!PathFor(name, p)) return Make(Result::Invalid, std::format("Invalid profile name '{}'", name));
    if (!FileExists(p)) return Make(Result::NotFound, std::format("Profile '{}' not found", name), name);
    json::Value mods;
    std::string desc, err;
    if (!ReadProfile(p, mods, desc, err))
        return Make(Result::Invalid, std::format("Profile '{}' is broken: {}", name, err), name);

    auto job = [name, mods] {
        std::string e;
        if (!Try([&] { Apply(name, mods); }, &e)) Make(Result::Error, std::format("Loading '{}' failed: {}", name, e), name);
    };
    if (main_thread::Ready() && !main_thread::IsMainThread()) {
        main_thread::Post(job);
        Outcome o{Result::Queued, std::format("Loading profile '{}'...", name), name};
        SetStatus(o.message);
        return o;
    }
    job();
    std::string msg = Status();
    return Outcome{Result::Ok, msg, name};
}

Outcome SaveCurrentAs(std::string_view nameIn, std::string_view description, bool overwrite) {
    Init();
    std::string name = SanitizeName(nameIn);
    std::filesystem::path p;
    if (name.empty() || !PathFor(name, p))
        return Make(Result::Invalid, "Invalid name: use letters, digits, space, - and _ (max 40)");
    if (FileExists(p) && !overwrite)
        return Make(Result::Exists, std::format("Profile '{}' already exists", name), name);
    std::error_code ec;
    std::filesystem::create_directories(p.parent_path(), ec);

    std::string text;
    std::string err;
    bool ok = Try(
        [&] { text = ProfileText(WithoutSkipped(settings::Snapshot(kSkipModule)), CleanDescription(description)); }, &err);
    if (!ok) return Make(Result::Error, std::format("Saving '{}' failed: {}", name, err), name);
    if (!WriteAtomic(p, text, err)) return Make(Result::Error, std::format("Saving '{}' failed: {}", name, err), name);
    SetActive(name);
    Refresh();
    return Make(Result::Ok, std::format("Saved profile '{}'", name), name);
}

Outcome Delete(std::string_view nameIn) {
    Init();
    std::string name(nameIn);
    std::filesystem::path p;
    if (!PathFor(name, p)) return Make(Result::Invalid, std::format("Invalid profile name '{}'", name));
    if (!FileExists(p)) {
        Refresh();
        return Make(Result::NotFound, std::format("Profile '{}' not found", name), name);
    }
    if (!DeleteFileW(p.c_str()))
        return Make(Result::Error, std::format("Cannot delete '{}' (error {})", name, GetLastError()), name);
    if (EqualsNoCase(ActiveProfile(), name)) SetActive("");
    Refresh();
    return Make(Result::Ok, std::format("Deleted profile '{}'", name), name);
}

Outcome Rename(std::string_view oldIn, std::string_view newIn, bool overwrite) {
    Init();
    std::string oldName(oldIn);
    std::string newName = SanitizeName(newIn);
    std::filesystem::path from, to;
    if (!PathFor(oldName, from)) return Make(Result::Invalid, std::format("Invalid profile name '{}'", oldName));
    if (newName.empty() || !PathFor(newName, to))
        return Make(Result::Invalid, "Invalid name: use letters, digits, space, - and _ (max 40)");
    if (!FileExists(from)) {
        Refresh();
        return Make(Result::NotFound, std::format("Profile '{}' not found", oldName), oldName);
    }
    if (newName == oldName) return Make(Result::Ok, "Name unchanged", newName);
    const bool caseOnly = EqualsNoCase(newName, oldName);
    if (!caseOnly && FileExists(to) && !overwrite)
        return Make(Result::Exists, std::format("Profile '{}' already exists", newName), newName);
    DWORD flags = MOVEFILE_WRITE_THROUGH | ((overwrite && !caseOnly) ? MOVEFILE_REPLACE_EXISTING : 0);
    if (!MoveFileExW(from.c_str(), to.c_str(), flags))
        return Make(Result::Error, std::format("Cannot rename '{}' (error {})", oldName, GetLastError()), oldName);
    if (EqualsNoCase(ActiveProfile(), oldName) || EqualsNoCase(ActiveProfile(), newName)) SetActive(newName);
    Refresh();
    return Make(Result::Ok, std::format("Renamed '{}' to '{}'", oldName, newName), newName);
}

std::string Status(unsigned long long* tick) {
    std::lock_guard lock(g_mutex);
    if (tick) *tick = g_statusTick;
    return g_status;
}

}  // namespace sr2::core::profiles
