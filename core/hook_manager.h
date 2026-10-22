#pragma once
// Named hooks over MinHook. Each hook belongs to a module; hooks are created
// disabled and switched on/off per hook or per module at runtime. Module-wide
// switches are applied as one MinHook batch (threads suspended once).

#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace sr2::core {

struct HookStatus {
    std::string module;
    std::string name;
    void* target = nullptr;
    bool enabled = false;
};

class HookManager {
public:
    static HookManager& Get();

    bool Initialize();  // MH_Initialize (idempotent)
    void Shutdown();    // disable + remove everything, MH_Uninitialize

    // Creates (does not enable) a hook. `name` must be unique across modules;
    // convention "<module>.<what>". *original receives the trampoline.
    bool Create(std::string_view module, std::string_view name, void* target, void* detour, void** original,
                std::string* err = nullptr);

    bool Enable(std::string_view name, std::string* err = nullptr);
    bool Disable(std::string_view name, std::string* err = nullptr);
    bool Remove(std::string_view name, std::string* err = nullptr);

    bool EnableModule(std::string_view module, std::string* err = nullptr);
    bool DisableModule(std::string_view module, std::string* err = nullptr);
    void RemoveModule(std::string_view module);

    std::vector<HookStatus> List() const;

private:
    struct Hook {
        std::string module;
        void* target = nullptr;
        void* detour = nullptr;
        void** original = nullptr;
        bool enabled = false;
    };
    bool SetModule(std::string_view module, bool enable, std::string* err);

    mutable std::mutex mutex_;
    bool initialized_ = false;
    std::unordered_map<std::string, Hook> hooks_;
};

}  // namespace sr2::core
