#pragma once
// sr2mods.json next to the DLL. Missing file / keys -> defaults below.

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

#include "json.h"
#include "log.h"

namespace sr2::core {

// Module on/off defaults generation, stored as "defaults_rev" in sr2mods.json.
//   rev 0/1 (no key): most feature modules defaulted to on.
//   rev 2: stripped defaults, every feature module starts off (module.h DefaultEnabled).
// A config file from before rev 2 keeps the old default for each module it does not
// list (module_manager.cpp writes those once, explicitly), then gets stamped rev 2.
inline constexpr int kDefaultsRev = 2;

struct Config {
    log::Level logLevel = log::Level::Info;
#ifdef _DEBUG
    bool debugConsole = true;  // only honoured in Debug builds
#else
    bool debugConsole = false;
#endif
    uint32_t startupTimeoutMs = 300000;  // wait for GameAssembly / IL2CPP init
    uint32_t settleDelayMs = 1500;       // extra wait after IL2CPP looks ready
    bool dumpAssemblies = true;          // milestone: log every image + class count
    bool versionGuardInvoke = true;      // actually call Application getters
    std::string expectedGameVersion = "1.3.0";
    std::string expectedUnityVersion = "6000.0.73f1";

    int defaultsRev = 0;          // "defaults_rev" of the file (0: absent)
    // The file predates kDefaultsRev (or could not be read / parsed): modules it does not
    // list keep their pre-rev-2 default. False for a missing file (brand-new install).
    bool legacyDefaults = false;
    // Write "defaults_rev": kDefaultsRev back (file missing or older; not when it failed to parse).
    bool stampDefaultsRev = false;

    json::Value root;  // whole document, for module-specific sections

    // "modules": { "<name>": { "enabled": true, ... } }
    const json::Value& ModuleSection(std::string_view module) const;
    bool ModuleEnabled(std::string_view module, bool def = true) const;
};

enum class ConfigStatus { Loaded, Missing, ParseError, ReadError };

// Always leaves `cfg` usable (defaults on any error). `error` describes failures.
ConfigStatus LoadConfig(const std::filesystem::path& file, Config& cfg, std::string& error);

}  // namespace sr2::core
