#include "hook_manager.h"

#include <MinHook.h>

#include <format>

#include "log.h"

namespace sr2::core {
namespace {

void SetErr(std::string* err, std::string msg) {
    if (err) *err = std::move(msg);
}

std::string MhError(MH_STATUS s) { return MH_StatusToString(s); }

}  // namespace

HookManager& HookManager::Get() {
    static HookManager m;
    return m;
}

bool HookManager::Initialize() {
    std::lock_guard lock(mutex_);
    if (initialized_) return true;
    MH_STATUS s = MH_Initialize();
    if (s != MH_OK && s != MH_ERROR_ALREADY_INITIALIZED) {
        SR2_ERROR("MinHook init failed: {}", MhError(s));
        return false;
    }
    initialized_ = true;
    return true;
}

void HookManager::Shutdown() {
    std::lock_guard lock(mutex_);
    if (!initialized_) return;
    MH_DisableHook(MH_ALL_HOOKS);
    for (auto& [name, h] : hooks_) MH_RemoveHook(h.target);
    hooks_.clear();
    MH_Uninitialize();
    initialized_ = false;
}

bool HookManager::Create(std::string_view module, std::string_view name, void* target, void* detour, void** original,
                         std::string* err) {
    std::lock_guard lock(mutex_);
    if (!initialized_) {
        SetErr(err, "hook manager not initialized");
        return false;
    }
    if (!target || !detour) {
        SetErr(err, std::format("hook '{}': null target or detour", name));
        return false;
    }
    std::string key(name);
    if (hooks_.contains(key)) {
        SetErr(err, std::format("hook '{}' already exists", name));
        return false;
    }
    for (auto& [n, h] : hooks_) {
        if (h.target == target) {
            SetErr(err, std::format("hook '{}': target {} already hooked by '{}'", name, target, n));
            return false;
        }
    }
    MH_STATUS s = MH_CreateHook(target, detour, original);
    if (s != MH_OK) {
        SetErr(err, std::format("hook '{}': MH_CreateHook({}) failed: {}", name, target, MhError(s)));
        return false;
    }
    hooks_.emplace(std::move(key), Hook{std::string(module), target, detour, original, false});
    SR2_DEBUG("hook created: {} [{}] target={}", name, module, target);
    return true;
}

bool HookManager::Enable(std::string_view name, std::string* err) {
    std::lock_guard lock(mutex_);
    auto it = hooks_.find(std::string(name));
    if (it == hooks_.end()) {
        SetErr(err, std::format("hook '{}' not found", name));
        return false;
    }
    if (it->second.enabled) return true;
    MH_STATUS s = MH_EnableHook(it->second.target);
    if (s != MH_OK && s != MH_ERROR_ENABLED) {
        SetErr(err, std::format("enable '{}' failed: {}", name, MhError(s)));
        return false;
    }
    it->second.enabled = true;
    return true;
}

bool HookManager::Disable(std::string_view name, std::string* err) {
    std::lock_guard lock(mutex_);
    auto it = hooks_.find(std::string(name));
    if (it == hooks_.end()) {
        SetErr(err, std::format("hook '{}' not found", name));
        return false;
    }
    if (!it->second.enabled) return true;
    MH_STATUS s = MH_DisableHook(it->second.target);
    if (s != MH_OK && s != MH_ERROR_DISABLED) {
        SetErr(err, std::format("disable '{}' failed: {}", name, MhError(s)));
        return false;
    }
    it->second.enabled = false;
    return true;
}

bool HookManager::Remove(std::string_view name, std::string* err) {
    std::lock_guard lock(mutex_);
    auto it = hooks_.find(std::string(name));
    if (it == hooks_.end()) {
        SetErr(err, std::format("hook '{}' not found", name));
        return false;
    }
    MH_STATUS s = MH_RemoveHook(it->second.target);  // also disables
    if (s != MH_OK) {
        SetErr(err, std::format("remove '{}' failed: {}", name, MhError(s)));
        return false;
    }
    hooks_.erase(it);
    return true;
}

bool HookManager::SetModule(std::string_view module, bool enable, std::string* err) {
    std::lock_guard lock(mutex_);
    int queued = 0;
    for (auto& [name, h] : hooks_) {
        if (h.module != module || h.enabled == enable) continue;
        MH_STATUS s = enable ? MH_QueueEnableHook(h.target) : MH_QueueDisableHook(h.target);
        if (s != MH_OK) {
            SetErr(err, std::format("queue {} '{}' failed: {}", enable ? "enable" : "disable", name, MhError(s)));
            return false;
        }
        ++queued;
    }
    if (queued == 0) return true;
    MH_STATUS s = MH_ApplyQueued();
    if (s != MH_OK) {
        SetErr(err, std::format("apply {} for module '{}' failed: {}", enable ? "enable" : "disable", module, MhError(s)));
        return false;
    }
    for (auto& [name, h] : hooks_)
        if (h.module == module) h.enabled = enable;
    return true;
}

bool HookManager::EnableModule(std::string_view module, std::string* err) { return SetModule(module, true, err); }
bool HookManager::DisableModule(std::string_view module, std::string* err) { return SetModule(module, false, err); }

void HookManager::RemoveModule(std::string_view module) {
    std::lock_guard lock(mutex_);
    for (auto it = hooks_.begin(); it != hooks_.end();) {
        if (it->second.module == module) {
            MH_RemoveHook(it->second.target);
            it = hooks_.erase(it);
        } else {
            ++it;
        }
    }
}

std::vector<HookStatus> HookManager::List() const {
    std::lock_guard lock(mutex_);
    std::vector<HookStatus> out;
    out.reserve(hooks_.size());
    for (auto& [name, h] : hooks_) out.push_back({h.module, name, h.target, h.enabled});
    return out;
}

}  // namespace sr2::core
