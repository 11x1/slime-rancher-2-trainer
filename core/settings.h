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
    // Optional "any of these on" parents (keys of the same module): the setting
    // shows only while at least one of them is on (non-zero) and itself shown by
    // its own parent rules. Combines with `parent` (both must allow). For a group
    // of settings that matters only while any of several switches is on.
    std::vector<std::string> anyOf;
    // Optional display of Float/Int values (UI only; stored values are unchanged):
    // shown = value * displayScale, followed by `unit` ("%", " m", " s", "x", " m/s");
    // `zeroText` replaces the number while the value is 0 (e.g. "no limit", "auto").
    std::string unit;
    std::string zeroText;
    double displayScale = 1;
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
// True if sr2mods.json had (or the mod has since written) a value for module.key,
// registered or not. For one-time migrations of renamed settings.
bool Saved(std::string_view module, std::string_view key);
// Restore a def's default value (Set(def)).
void Reset(std::string_view module, std::string_view key);
// Write pending changes now (also done on shutdown).
void Flush();
// Why the last write of sr2mods.json failed; "" once a write succeeds (or none failed yet).
std::string LastSaveError();

// ---- free-text module values ----
// A string under "modules.<module>.<key>" in sr2mods.json that is not a Def: no
// generic UI row, not in profiles, no change callbacks (poll it). "" if absent.
// SetString marks the document dirty (debounced save like a setting). Any thread.
std::string GetString(std::string_view module, std::string_view key);
void SetString(std::string_view module, std::string_view key, std::string value);

// ---- visibility (Def::parent) ----
// Sets a registered def's visibility parent (convenience for modules whose
// registration helpers do not take one; a later Register without a parent
// keeps it). No-op if module.key is not registered.
void SetParent(std::string_view module, std::string_view key, std::string_view parent, int parentValue = -1);
// Sets a registered def's "any of these on" parents (Def::anyOf); a later
// Register without any keeps them. No-op if module.key is not registered.
void SetAnyParent(std::string_view module, std::string_view key, const std::vector<std::string>& anyOf);
// Sets a registered def's display unit, zero text and scale (see Def::unit) for
// modules whose registration helpers do not take them; a later Register without
// any of them keeps these. No-op if module.key is not registered.
void SetDisplay(std::string_view module, std::string_view key, std::string_view unit, std::string_view zeroText = {},
                double displayScale = 1);
// True if `parentValue` (the parent's current value) lets `d` show. True when d has no parent.
bool ParentAllows(const Def& d, double parentValue);
// The full rule on the stored values: the parent chain allows it and, for any
// key but "enabled", the module's "enabled" (if it has one) is on. Unknown
// settings are visible.
bool Visible(std::string_view module, std::string_view key);

// ---- locks ----
// Pins module.key to `value` (set now, with the usual notification) and records why. While
// locked, Set() / Reset() / profiles with any other value are ignored; the overlay draws the
// control locked with `reason` as its tooltip. A module uses it for a switch that must not be
// turned off right now (custom_upgrades: a save with custom upgrades is loaded).
void Lock(std::string_view module, std::string_view key, double value, std::string reason);
void Unlock(std::string_view module, std::string_view key);
std::string LockReason(std::string_view module, std::string_view key);  // "" = not locked

}  // namespace sr2::core::settings
