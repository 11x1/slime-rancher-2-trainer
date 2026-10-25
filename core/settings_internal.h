#pragma once
// Core-only part of the settings API (not for modules / UI).

#include <filesystem>
#include <string>
#include <string_view>

#include "json.h"
#include "settings.h"

namespace sr2::core::settings {

// Called once by the worker after sr2mods.json was loaded: saved values are
// looked up in `document`; changes are written back to `file` (empty path =
// no persistence). Starts the debounced saver thread.
void Init(const json::Value& document, const std::filesystem::path& file);

// ---- used by core/profiles.cpp ----

// Top-level string in sr2mods.json (e.g. "active_profile"). Set marks the
// document dirty (debounced save like a setting change).
std::string RootString(std::string_view key);
void SetRootString(std::string_view key, std::string value);
// Top-level number (e.g. "defaults_rev"), same rules as SetRootString.
void SetRootNumber(std::string_view key, double value);

// Stores a saved value for a setting that is NOT registered (yet), e.g. a
// setting of a module that has not run Setup(). It is written to
// sr2mods.json and picked up by a later Register(). Ignored if the setting is
// registered (use Set). `v` must be a bool or number.
void StashSaved(std::string_view module, std::string_view key, const json::Value& v);

// {"<module>": {"<key>": value}} with every registered setting's current value
// plus saved bool/number values of not-yet-registered keys of registered
// modules. Modules and keys sorted. `skipModule` is left out.
json::Value Snapshot(std::string_view skipModule);

// Saved bool/number value of a key that is not registered (false if none).
bool SavedValue(std::string_view module, std::string_view key, json::Value& out);

// Converts a json bool/number to the def's clamped value.
double ClampFor(const Def& d, double v);

}  // namespace sr2::core::settings
