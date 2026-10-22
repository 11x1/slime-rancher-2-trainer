#pragma once
// sr2mods.json next to the DLL. Missing file / keys -> defaults below.

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

#include "json.h"
#include "log.h"

namespace sr2::core {

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

    json::Value root;  // whole document, for module-specific sections

    // "modules": { "<name>": { "enabled": true, ... } }
    const json::Value& ModuleSection(std::string_view module) const;
    bool ModuleEnabled(std::string_view module, bool def = true) const;
};

enum class ConfigStatus { Loaded, Missing, ParseError, ReadError };

// Always leaves `cfg` usable (defaults on any error). `error` describes failures.
ConfigStatus LoadConfig(const std::filesystem::path& file, Config& cfg, std::string& error);

}  // namespace sr2::core
