#include "module_manager.h"

#include <exception>
#include <format>

#include "guard.h"
#include "log.h"
#include "main_thread.h"
#include "settings.h"

namespace sr2::core {

// ---------------------------------------------------------------- context --

bool ModuleContext::Fail(std::string reason) {
    if (failure_.empty()) failure_ = std::move(reason);
    return false;
}

Il2CppClass* ModuleContext::Class(std::string_view image, std::string_view ns, std::string_view klass) {
    std::string err;
    Il2CppClass* k = resolver().Class(image, ns, klass, &err);
    if (!k) Fail(err);
    return k;
}

const MethodInfo* ModuleContext::Method(Il2CppClass* klass, std::string_view method, int argc) {
    std::string err;
    const MethodInfo* m = resolver().Method(klass, method, argc, &err);
    if (!m) Fail(err);
    return m;
}

void* ModuleContext::MethodPointer(std::string_view image, std::string_view ns, std::string_view klass,
                                   std::string_view method, int argc) {
    std::string err;
    void* p = resolver().MethodPointer(image, ns, klass, method, argc, &err);
    if (!p) Fail(err);
    return p;
}

bool ModuleContext::FieldOffset(Il2CppClass* klass, std::string_view field, size_t& out) {
    std::string err;
    auto off = resolver().FieldOffset(klass, field, &err);
    if (!off) return Fail(err);
    out = *off;
    return true;
}

bool ModuleContext::HookMethod(std::string_view hookName, std::string_view image, std::string_view ns,
                               std::string_view klass, std::string_view method, int argc, void* detour,
                               void** original) {
    void* target = MethodPointer(image, ns, klass, method, argc);
    if (!target) return false;
    SR2_DEBUG("[{}] {}.{}::{} -> GameAssembly+0x{:X}", module_, ns, klass, method, il2cpp::Resolver::Rva(target));
    return HookAddress(hookName, target, detour, original);
}

bool ModuleContext::HookAddress(std::string_view hookName, void* target, void* detour, void** original) {
    std::string err;
    if (!hooks().Create(module_, std::format("{}.{}", module_, hookName), target, detour, original, &err))
        return Fail(err);
    return true;
}

// ---------------------------------------------------------------- manager --

const char* StateName(ModuleState s) {
    switch (s) {
        case ModuleState::Registered: return "registered";
        case ModuleState::DisabledByConfig: return "disabled (config)";
        case ModuleState::Failed: return "FAILED";
        case ModuleState::Enabled: return "enabled";
        case ModuleState::Disabled: return "disabled";
    }
    return "?";
}

ModuleManager& ModuleManager::Get() {
    static ModuleManager m;
    return m;
}

void ModuleManager::Register(std::unique_ptr<Module> m) {
    if (!m) return;
    std::lock_guard lock(mutex_);
    entries_.push_back({std::move(m)});
}

// Runs Setup() and enables the module's hooks. Caller holds mutex_.
bool ModuleManager::SetupLocked(Entry& e) {
    const char* name = e.module->Name();
    ModuleContext ctx(name, *cfg_);
    bool ok = false;
    try {
        ok = e.module->Setup(ctx);
    } catch (const std::exception& ex) {
        ctx.Fail(std::format("exception in Setup: {}", ex.what()));
    } catch (...) {
        ctx.Fail("unknown exception in Setup");
    }
    std::string err;
    if (ok && !HookManager::Get().EnableModule(name, &err)) {
        ok = false;
        ctx.Fail(err);
    }
    if (!ok) {
        HookManager::Get().RemoveModule(name);
        e.state = ModuleState::Failed;
        e.reason = ctx.failure().empty() ? "Setup returned false" : ctx.failure();
        SR2_ERROR("module {}: DISABLED - {}", name, e.reason);
        return false;
    }
    e.state = ModuleState::Enabled;
    e.reason.clear();
    std::string cbErr;
    if (!Try([&] { e.module->OnEnable(); }, &cbErr)) SR2_ERROR("module {}: OnEnable failed: {}", name, cbErr);
    SR2_INFO("module {}: enabled", name);
    return true;
}

void ModuleManager::SetupAll(const Config& cfg) {
    std::lock_guard lock(mutex_);
    cfg_ = cfg;
    if (entries_.empty()) {
        SR2_INFO("modules: none registered");
        return;
    }
    for (Entry& e : entries_) {
        const std::string name = e.module->Name();

        settings::Def d;
        d.module = name;
        d.key = "enabled";
        d.label = "Enabled";
        d.kind = settings::Kind::Bool;
        d.def = e.module->DefaultEnabled() ? 1 : 0;
        d.tooltip = "Turn this module on or off (applies immediately).";
        settings::Register(d);  // picks up modules.<name>.enabled from sr2mods.json
        settings::OnChange(name, [name](const std::string& key) {
            if (key == "enabled") ModuleManager::Get().SetEnabled(name, settings::GetBool(name, "enabled"));
        });

        if (!settings::GetBool(name, "enabled")) {
            e.state = ModuleState::DisabledByConfig;
            SR2_INFO("module {}: disabled in config", name);
            continue;
        }
        SetupLocked(e);
    }
}

bool ModuleManager::SetEnabled(std::string_view name, bool enabled) {
    // Keep the persisted setting in sync; its OnChange calls back into here
    // with the same value, which then falls through to the dispatch below.
    if (settings::Has(name, "enabled") && settings::GetBool(name, "enabled") != enabled) {
        settings::Set(name, "enabled", enabled ? 1.0 : 0.0);
        return true;
    }
    if (main_thread::Ready() && !main_thread::IsMainThread()) {
        std::string n(name);
        main_thread::Post([n, enabled] { ModuleManager::Get().Apply(n, enabled); });
        return true;
    }
    return Apply(name, enabled);
}

bool ModuleManager::Apply(std::string_view name, bool enabled) {
    std::lock_guard lock(mutex_);
    for (Entry& e : entries_) {
        if (name != e.module->Name()) continue;
        std::string err;
        switch (e.state) {
            case ModuleState::Enabled:
                if (enabled) return true;
                {
                    std::string cbErr;
                    if (!Try([&] { e.module->OnDisable(); }, &cbErr))
                        SR2_ERROR("module {}: OnDisable failed: {}", name, cbErr);
                }
                if (!HookManager::Get().DisableModule(name, &err)) {
                    SR2_ERROR("module {}: disable failed: {}", name, err);
                    return false;
                }
                e.state = ModuleState::Disabled;
                SR2_INFO("module {}: disabled", name);
                return true;
            case ModuleState::Disabled:
                if (!enabled) return true;
                if (!HookManager::Get().EnableModule(name, &err)) {
                    SR2_ERROR("module {}: enable failed: {}", name, err);
                    return false;
                }
                e.state = ModuleState::Enabled;
                {
                    std::string cbErr;
                    if (!Try([&] { e.module->OnEnable(); }, &cbErr))
                        SR2_ERROR("module {}: OnEnable failed: {}", name, cbErr);
                }
                SR2_INFO("module {}: enabled", name);
                return true;
            case ModuleState::DisabledByConfig:
            case ModuleState::Registered:
                if (!enabled) return true;
                if (!cfg_) {
                    SR2_WARN("module {}: cannot set up before SetupAll", name);
                    return false;
                }
                SR2_INFO("module {}: first enable at runtime - running Setup", name);
                return SetupLocked(e);
            case ModuleState::Failed:
                if (enabled) SR2_WARN("module {}: cannot enable, Setup failed earlier: {}", name, e.reason);
                return !enabled;
        }
        return false;
    }
    SR2_WARN("module {}: not registered", name);
    return false;
}

std::vector<ModuleManager::Info> ModuleManager::List() const {
    std::lock_guard lock(mutex_);
    std::vector<Info> out;
    for (const Entry& e : entries_) out.push_back({e.module->Name(), e.state, e.reason});
    return out;
}

void ModuleManager::MarkFaulted(std::string_view name, std::string reason) {
    {
        std::lock_guard lock(mutex_);
        for (Entry& e : entries_) {
            if (name != e.module->Name()) continue;
            e.state = ModuleState::Failed;
            e.reason = "faulted: " + reason;
        }
    }
    std::string n(name);
    auto disable = [n] {
        std::string err;
        if (!HookManager::Get().DisableModule(n, &err)) SR2_WARN("module {}: disabling hooks after fault: {}", n, err);
    };
    // Never switch hooks from inside the faulting detour: defer to the next tick.
    if (main_thread::Ready())
        main_thread::Post(disable);
    else
        disable();
}

void ModuleManager::FailAll(const Config& cfg, const std::string& reason) {
    std::lock_guard lock(mutex_);
    cfg_ = cfg;
    for (Entry& e : entries_) {
        settings::Def d;
        d.module = e.module->Name();
        d.key = "enabled";
        d.label = "Enabled";
        d.kind = settings::Kind::Bool;
        d.def = e.module->DefaultEnabled() ? 1 : 0;
        settings::Register(d);
        e.state = ModuleState::Failed;
        e.reason = reason;
        SR2_ERROR("module {}: DISABLED - {}", d.module, reason);
    }
}

bool ModuleManager::IsEnabled(std::string_view name) const {
    std::lock_guard lock(mutex_);
    for (const Entry& e : entries_)
        if (name == e.module->Name()) return e.state == ModuleState::Enabled;
    return false;
}

}  // namespace sr2::core
