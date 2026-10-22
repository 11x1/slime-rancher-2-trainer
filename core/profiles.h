#pragma once
// Named settings profiles: JSON files in "profiles\" next to the DLL.
//
//   profiles\<name>.json = { "description": "...",            (optional)
//                            "modules": { "<module>": { "<key>": value } } }
//
// Same shape as the "modules" section of sr2mods.json. sr2mods.json keeps the
// live values and remembers "active_profile" (empty = none). Startup never
// re-applies a profile: the live values are authoritative.
//
// Settings of module "ui" (overlay hotkey, hint) are never part of a profile,
// so loading someone's shared profile cannot move your overlay hotkey.
//
// Thread-safe. File operations run on the calling thread (small files);
// Load() applies the values on the Unity main thread (posted when called from
// another thread while the dispatcher runs, like ui::Write).

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace sr2::core::profiles {

struct Info {
    std::string name;         // file stem (UTF-8), the profile's name
    std::string description;  // "" if none
    bool valid = true;        // file parsed and has the expected shape
    std::string error;        // why not valid
};

enum class Result {
    Ok,        // done
    Queued,    // Load: posted to the main thread, see Status() for the outcome
    Exists,    // target exists and overwrite was not confirmed
    Invalid,   // bad name / bad file
    NotFound,  // no such profile
    Error,     // I/O failure (logged)
};

struct Outcome {
    Result result = Result::Ok;
    std::string message;  // one line for the UI (also stored as Status())
    std::string name;     // the (sanitized) profile name the operation used
    bool ok() const { return result == Result::Ok || result == Result::Queued; }
};

// Creates the folder and, the first time (folder did not exist), the built-in
// starter profiles "Vanilla" and "Default". Call after the modules have
// registered their settings. Idempotent; every other function calls it.
void Init();

std::filesystem::path Folder();

// Cached list (sorted by name, case-insensitive). Refresh() rescans the folder.
std::vector<Info> List();
void Refresh();

// Name of the active profile ("" = none). The file may be missing.
std::string ActiveProfile();
// True if the current values differ from the active profile's file (keys the
// profile lacks count as their default). Cheap; cached for ~250 ms.
bool IsModified();

// Letters, digits, space, '-' and '_' only (others dropped), spaces collapsed,
// trimmed, at most 40 characters; "" if nothing usable is left.
std::string SanitizeName(std::string_view name);

// Applies every value of the profile via settings::Set on the main thread:
// keys missing in the profile reset to their defaults, unknown modules/keys
// are ignored, values for settings a module has not registered yet (module
// never set up) are stored for when it does. Module "enabled" flags are
// applied last, so the normal ModuleManager enable/disable logic runs.
Outcome Load(std::string_view name);
// Writes every setting's current value (sorted, pretty-printed). Refuses to
// replace an existing file unless `overwrite`. Makes it the active profile.
Outcome SaveCurrentAs(std::string_view name, std::string_view description, bool overwrite);
Outcome Delete(std::string_view name);
Outcome Rename(std::string_view oldName, std::string_view newName, bool overwrite);

// Last operation's message ("" if none) and its GetTickCount64() time.
std::string Status(unsigned long long* tick = nullptr);

}  // namespace sr2::core::profiles
