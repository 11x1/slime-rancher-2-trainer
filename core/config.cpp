#include "config.h"

#include <algorithm>
#include <fstream>
#include <sstream>

namespace sr2::core {

const json::Value& Config::ModuleSection(std::string_view module) const { return root["modules"][module]; }

bool Config::ModuleEnabled(std::string_view module, bool def) const {
    return ModuleSection(module)["enabled"].asBool(def);
}

ConfigStatus LoadConfig(const std::filesystem::path& file, Config& cfg, std::string& error) {
    cfg = Config{};
    std::error_code ec;
    if (!std::filesystem::exists(file, ec)) {
        error = "not found, using defaults";
        cfg.stampDefaultsRev = true;  // brand-new config: stripped defaults
        return ConfigStatus::Missing;
    }
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        error = "cannot open file, using defaults";
        cfg.legacyDefaults = true;  // an existing config: never strip its modules for the session
        return ConfigStatus::ReadError;
    }
    std::stringstream ss;
    ss << in.rdbuf();
    std::string text = ss.str();

    json::Value root;
    cfg.legacyDefaults = true;  // until "defaults_rev" says otherwise (also on a parse error)
    if (!json::Parse(text, root, error)) return ConfigStatus::ParseError;
    if (!root.isObject()) {
        error = "top level must be an object, using defaults";
        return ConfigStatus::ParseError;
    }

    auto u32 = [](const json::Value& v, uint32_t def) {
        return v.isNumber() && v.asNumber() >= 0 ? static_cast<uint32_t>(v.asNumber()) : def;
    };
    cfg.logLevel = log::ParseLevel(root["log_level"].asString(), cfg.logLevel);
    cfg.debugConsole = root["debug_console"].asBool(cfg.debugConsole);
    cfg.startupTimeoutMs = u32(root["startup_timeout_ms"], cfg.startupTimeoutMs);
    cfg.settleDelayMs = u32(root["settle_delay_ms"], cfg.settleDelayMs);
    cfg.dumpAssemblies = root["dump_assemblies"].asBool(cfg.dumpAssemblies);
    cfg.versionGuardInvoke = root["version_guard_invoke"].asBool(cfg.versionGuardInvoke);
    cfg.expectedGameVersion = root["expected_game_version"].asString(cfg.expectedGameVersion);
    cfg.expectedUnityVersion = root["expected_unity_version"].asString(cfg.expectedUnityVersion);
    const json::Value& rev = root["defaults_rev"];
    cfg.defaultsRev = rev.isNumber() && rev.asNumber() >= 0 ? static_cast<int>(std::min(rev.asNumber(), 1000.0)) : 0;
    cfg.legacyDefaults = cfg.defaultsRev < kDefaultsRev;
    cfg.stampDefaultsRev = cfg.defaultsRev < kDefaultsRev;
    cfg.root = std::move(root);
    return ConfigStatus::Loaded;
}

}  // namespace sr2::core
