#pragma once

#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "module.h"

namespace sr2::core {

enum class ModuleState { Registered, DisabledByConfig, Failed, Enabled, Disabled };
const char* StateName(ModuleState s);

class ModuleManager {
public:
    static ModuleManager& Get();

    void Register(std::unique_ptr<Module> m);

    // Registers each module's "enabled" setting (settings.h), then runs
    // Setup() for every module whose setting is on. Worker thread.
    void SetupAll(const Config& cfg);

    // Runtime enable/disable. Keeps the "<module>.enabled" setting in sync
    // (changing that setting calls this too). A module that was off at startup
    // is set up lazily on first enable. When the main-thread dispatcher is
    // ready the actual switch (Setup/OnEnable/OnDisable + hook batch) runs on
    // the Unity main thread; the call then returns true = "accepted" and
    // List() reflects the new state from the next frame on.
    bool SetEnabled(std::string_view name, bool enabled);

    struct Info {
        std::string name;
        ModuleState state;
        std::string reason;
    };
    std::vector<Info> List() const;
    bool IsEnabled(std::string_view name) const;  // state == Enabled

    // A guarded detour faulted (see detour_guard.h): state -> Failed(reason),
    // hooks disabled on the main thread (next tick), OnDisable NOT called.
    void MarkFaulted(std::string_view name, std::string reason);
    // Registers the "enabled" settings like SetupAll but sets up nothing:
    // every module is marked Failed(reason). Used when a core self-test fails.
    void FailAll(const Config& cfg, const std::string& reason);

private:
    struct Entry {
        std::unique_ptr<Module> module;
        ModuleState state = ModuleState::Registered;
        std::string reason;
    };
    bool Apply(std::string_view name, bool enabled);  // performs the switch on the current thread
    bool SetupLocked(Entry& e, bool enable = true);  // enable = false: hooks stay off (state Disabled)

    mutable std::recursive_mutex mutex_;
    std::vector<Entry> entries_;
    std::optional<Config> cfg_;
};

// Defined in core/module_registry.cpp: adds the built-in modules.
void RegisterBuiltinModules(ModuleManager& mm);

}  // namespace sr2::core
