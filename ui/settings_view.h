#pragma once
// Shared, renderer-independent helpers for the two settings UIs (Options tab
// and ImGui overlay): grouping of the registered settings, value formatting,
// key names, module status lines and thread-safe writes.

#include <string>
#include <string_view>
#include <vector>

#include "core/settings.h"

namespace sr2::ui {

namespace settings = ::sr2::core::settings;

inline constexpr const char* kUiModule = "ui";
inline constexpr const char* kKeyOverlayHotkey = "overlay_hotkey";
inline constexpr const char* kKeyOverlayHint = "overlay_hint";
inline constexpr const char* kKeyOverlayTheme = "overlay_theme";  // Enum: 0 Dark, 1 Light
inline constexpr const char* kKeyEnabled = "enabled";

struct GroupView {
    std::string name;  // "" = ungrouped (rendered first)
    std::vector<settings::Def> defs;
};

struct ModuleView {
    std::string module;
    std::string title;           // pretty name, e.g. "Spawn Scaling"
    bool hasEnabled = false;     // module has the auto "enabled" Bool
    settings::Def enabled;       // valid if hasEnabled
    std::vector<GroupView> groups;  // registration order of first appearance
};

// Registers the "ui" module's own settings (idempotent).
void RegisterUiSettings();

// All registered settings, grouped by module (registration order), then by
// group. The "enabled" def is split out into ModuleView::enabled.
std::vector<ModuleView> BuildModuleViews();

// "spawn_scaling" -> "Spawn Scaling", "ui" -> "UI / Overlay".
std::string PrettyModuleName(std::string_view module);

// Human readable Win32 virtual-key name ("F8", "Right Ctrl", "Mouse 5", ...);
// 0 is "(none)", unknown codes are "VK 0x..".
std::string KeyName(int vk);

// Snap a value to the def's [min,max] range and step grid (Int/Float/Enum/Key).
double Snap(const settings::Def& d, double v);

// Number of decimals that represent `step` exactly enough for display.
int StepDecimals(double step);
// Format a value of a def for display ("0.75", "On", enum label, key name).
std::string FormatValue(const settings::Def& d, double v);

// Writes a value. If the Unity main-thread dispatcher is running and the
// caller is not on the main thread, the write is posted to the main thread
// (module OnChange callbacks may touch Unity objects); the overlay keeps
// showing the pending value until it lands. Always clamps/snaps first.
void Write(const settings::Def& d, double v);
// Current value as the UI should show it (pending write if any, else stored).
double Read(const settings::Def& d);

// One-line status for a module from ModuleManager::List() ("enabled",
// "FAILED: <reason>", ...). Empty if the module is not a core module.
std::string ModuleStatus(std::string_view module, bool* failed = nullptr);

// "Saved" indicator support: time (GetTickCount64) of the last UI write, 0 if none.
unsigned long long LastWriteTick();
// Persist pending changes now (settings::Flush, exception-safe).
void FlushNow();

// Status of each UI path, shown by the other UI.
void SetOptionsTabStatus(std::string s);
std::string OptionsTabStatus();
void SetOverlayStatus(std::string s);
std::string OverlayStatus();

}  // namespace sr2::ui
