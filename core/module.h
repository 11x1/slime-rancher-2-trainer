#pragma once
// Module interface. A module resolves everything it needs in Setup() through
// the ModuleContext; any failed resolve/hook disables ONLY that module (its
// hooks are removed) and the reason is logged. Other modules keep running.

#include <string>
#include <string_view>

#include "config.h"
#include "hook_manager.h"
#include "resolver.h"

namespace sr2::core {

class ModuleContext {
public:
    ModuleContext(std::string_view module, const Config& cfg) : module_(module), cfg_(cfg) {}

    const std::string& module() const { return module_; }
    il2cpp::Resolver& resolver() const { return il2cpp::Resolver::Get(); }
    HookManager& hooks() const { return HookManager::Get(); }
    const Config& config() const { return cfg_; }
    // This module's "modules.<name>" section of sr2mods.json (Null if absent).
    const json::Value& settings() const { return cfg_.ModuleSection(module_); }

    // Resolve-by-name helpers. On failure they record the reason (see failure())
    // and return nullptr / false, so Setup() can simply `return false`.
    Il2CppClass* Class(std::string_view image, std::string_view ns, std::string_view klass);
    const MethodInfo* Method(Il2CppClass* klass, std::string_view method, int argc);
    void* MethodPointer(std::string_view image, std::string_view ns, std::string_view klass, std::string_view method,
                        int argc);
    bool FieldOffset(Il2CppClass* klass, std::string_view field, size_t& out);

    // Resolve a managed method and create a (disabled) hook named
    // "<module>.<hookName>". The module manager enables all of a module's
    // hooks together after Setup() succeeds.
    bool HookMethod(std::string_view hookName, std::string_view image, std::string_view ns, std::string_view klass,
                    std::string_view method, int argc, void* detour, void** original);
    // Same for an already-known native address.
    bool HookAddress(std::string_view hookName, void* target, void* detour, void** original);

    bool Fail(std::string reason);  // records reason, returns false
    const std::string& failure() const { return failure_; }

private:
    std::string module_;
    const Config& cfg_;
    std::string failure_;
};

class Module {
public:
    virtual ~Module() = default;
    virtual const char* Name() const = 0;  // also the key under "modules" in sr2mods.json
    // Default of modules.<name>.enabled. Off: a new install starts with every feature off and
    // the player turns on what they want (config.h kDefaultsRev; configs from before keep the
    // old defaults, module_manager.cpp kLegacyDefaultOn). Override only for a module that does
    // nothing by itself.
    virtual bool DefaultEnabled() const { return false; }
    // True: Setup() also runs at startup while the module is switched off; its hooks stay
    // disabled (state Disabled) until it is enabled. For a module that must watch something
    // (from a frame callback) while off, e.g. custom_upgrades turning itself on for a save
    // that has custom upgrades.
    virtual bool SetupWhileDisabled() const { return false; }

    // Resolve symbols and create hooks. Runs on the worker thread (attached to
    // the IL2CPP domain). Return false (ideally via ctx.Fail(...)) to disable.
    virtual bool Setup(ModuleContext& ctx) = 0;

    // Runtime toggles (called after hooks were enabled / before disabling).
    virtual void OnEnable() {}
    virtual void OnDisable() {}
};

}  // namespace sr2::core
