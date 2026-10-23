#pragma once
// Runtime settings: modules register definitions, the UI renders them, the
// values persist to sr2mods.json under "modules.<module>.<key>". Thread-safe.
// Change callbacks run on the thread that called Set() (outside the lock).

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace sr2::core::settings {

enum class Kind { Bool, Float, Int, Enum, Key };

struct Def {
    std::string module, key, label, group;  // group = optional sub-header inside the module
    Kind kind = Kind::Bool;
    double def = 0, min = 0, max = 1, step = 0.1;  // Bool: 0/1; Enum: index; Key: Win32 virtual-key code
    std::vector<std::string> options;              // Enum labels
    std::string tooltip;
    // Optional visibility parent: a key of the same module this setting depends on.
    // The UIs show the setting only while the parent is on (non-zero), or, with
    // parentValue >= 0, while the parent equals parentValue (an Enum index; 0 on a
    // Bool means "while off"). Chains are followed. Independently of this, every
    // setting except "enabled" is hidden while its module's "enabled" is off.
    std::string parent;
    int parentValue = -1;
};

void Register(const Def& d);  // idempotent per module+key; loads saved value if present
double Get(std::string_view module, std::string_view key);
bool GetBool(std::string_view module, std::string_view key);
float GetFloat(std::string_view module, std::string_view key);
int GetInt(std::string_view module, std::string_view key);
// Clamps, notifies, debounced save (~1 s) to sr2mods.json "modules.<module>.<key>".
void Set(std::string_view module, std::string_view key, double v);
int OnChange(std::string_view module, std::function<void(const std::string& key)> fn);
std::vector<Def> All();               // registration order, for UI
std::vector<std::string> Modules();   // modules with registered defs, registration order

// ---- additions ----
void RemoveOnChange(int id);
bool Has(std::string_view module, std::string_view key);
// Restore a def's default value (Set(def)).
void Reset(std::string_view module, std::string_view key);
// Write pending changes now (also done on shutdown).
void Flush();
// Why the last write of sr2mods.json failed; "" once a write succeeds (or none failed yet).
std::string LastSaveError();

// ---- visibility (Def::parent) ----
// Sets a registered def's visibility parent (convenience for modules whose
// registration helpers do not take one; a later Register without a parent
// keeps it). No-op if module.key is not registered.
void SetParent(std::string_view module, std::string_view key, std::string_view parent, int parentValue = -1);
// True if `parentValue` (the parent's current value) lets `d` show. True when d has no parent.
bool ParentAllows(const Def& d, double parentValue);
// The full rule on the stored values: the parent chain allows it and, for any
// key but "enabled", the module's "enabled" (if it has one) is on. Unknown
// settings are visible.
bool Visible(std::string_view module, std::string_view key);

}  // namespace sr2::core::settings
