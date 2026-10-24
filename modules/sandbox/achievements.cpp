// sandbox: achievements blocked while the module is enabled or the loaded save is marked as
// cheated (see achievements.h). SR2 1.3.0.

#include "modules/sandbox/achievements.h"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>

#include "core/detour_guard.h"
#include "core/hook_manager.h"
#include "core/il2cpp_api.h"
#include "core/log.h"
#include "core/module.h"
#include "core/unity.h"
#include "ui/sandbox_bridge.h"

namespace sr2::modules::sandbox {
namespace {

namespace unity = sr2::core::unity;
using sr2::il2cpp::api;
using sr2::il2cpp::Resolver;

constexpr const char* kModule = "sandbox";
constexpr const char* kImage = "Assembly-CSharp";
constexpr const char* kNs = "MonomiPark.SlimeRancher";
constexpr const char* kPlatformNs = "MonomiPark.SlimeRancher.Platform";
// HookManager module and DetourGuard fault name of the four hooks. Not "sandbox": the hooks stay on
// while the module is off (a cheated save keeps achievements blocked), and a fault elsewhere in the
// module must not switch them off.
constexpr const char* kHooks = "sandbox_achievements";

std::atomic<bool> g_paused{false};   // Sandbox is on
std::atomic<bool> g_cheated{false};  // the loaded save is marked as cheated (cheated_save.cpp)
std::atomic<bool> g_logged{false};   // one "blocked" line per enable / per marked save

Il2CppClass* g_directorClass = nullptr;  // MetaGameDirector
Il2CppClass* g_dataClass = nullptr;      // MetaGameDirector.AchievementData
Il2CppClass* g_steamClass = nullptr;     // SteamMetaGamePlatform
size_t g_offIdentifier = 0;              // AchievementData.AchievementIdentifier (int enum)

using IncrementFn = bool (*)(Il2CppObject*, int32_t, const MethodInfo*);
using SetProgressFn = void (*)(Il2CppObject*, int32_t, int32_t, const MethodInfo*);
using SoldPlortFn = void (*)(Il2CppObject*, int32_t, const MethodInfo*);
using PlatformGrantFn = void (*)(Il2CppObject*, Il2CppObject*, const MethodInfo*);
IncrementFn g_origIncrement = nullptr;
SetProgressFn g_origSetProgress = nullptr;
SoldPlortFn g_origSoldPlort = nullptr;
PlatformGrantFn g_origPlatformGrant = nullptr;

sr2::core::DetourGuard g_incrementGuard{kHooks, "AchievementData.IncrementProgress"};
sr2::core::DetourGuard g_setProgressGuard{kHooks, "MetaGameDirector.SetAchievementProgress"};
sr2::core::DetourGuard g_soldPlortGuard{kHooks, "MetaGameDirector.HandleSoldPlort"};
sr2::core::DetourGuard g_platformGrantGuard{kHooks, "SteamMetaGamePlatform.Grant"};

// Exact class or a subclass (plain C# objects: no UnityEngine.Object, so no IsAlive).
bool IsA(Il2CppObject* obj, Il2CppClass* k) {
    const auto& a = api();
    if (!obj || !k || !a.il2cpp_object_get_class) return false;
    Il2CppClass* c = a.il2cpp_object_get_class(obj);
    if (c == k) return true;
    return c && a.il2cpp_class_is_assignable_from && a.il2cpp_class_is_assignable_from(k, c);
}

// Inside a guard: true = block this call. `self` must be a `k` (else the call passes through).
bool Block(Il2CppObject* self, Il2CppClass* k, const char* what, int achievement) {
    const bool paused = g_paused.load(std::memory_order_relaxed);
    if ((!paused && !g_cheated.load(std::memory_order_relaxed)) || !IsA(self, k)) return false;
    if (!g_logged.exchange(true)) {
        const std::string which = achievement >= 0 ? std::format(" (achievement #{})", achievement) : std::string{};
        SR2_INFO("[{}] achievements blocked: {}{} ({}); further blocks are not logged", kModule, what, which,
                 paused ? "Sandbox is on" : "this save is marked as cheated");
    }
    return true;
}

// AchievementData.AchievementIdentifier for the log line (-1 if not an AchievementData).
int Identifier(Il2CppObject* data) {
    return g_offIdentifier && IsA(data, g_dataClass) ? unity::Field<int32_t>(data, g_offIdentifier) : -1;
}

bool IncrementDetour(Il2CppObject* self, int32_t amount, const MethodInfo* mi) {
    bool block = false;
    if (g_incrementGuard.Run([&] {
            block = Block(self, g_dataClass, "AchievementData.IncrementProgress", Identifier(self));
        }) &&
        block) {
        return false;  // no progress -> MetaGameDirector.Grant does not report to the platform
    }
    return g_origIncrement(self, amount, mi);
}

void SetProgressDetour(Il2CppObject* self, int32_t achievement, int32_t value, const MethodInfo* mi) {
    bool block = false;
    if (g_setProgressGuard.Run([&] {
            block = Block(self, g_directorClass, "MetaGameDirector.SetAchievementProgress", achievement);
        }) &&
        block) {
        return;
    }
    g_origSetProgress(self, achievement, value, mi);
}

void SoldPlortDetour(Il2CppObject* self, int32_t price, const MethodInfo* mi) {
    bool block = false;
    if (g_soldPlortGuard.Run([&] { block = Block(self, g_directorClass, "MetaGameDirector.HandleSoldPlort", -1); }) &&
        block) {
        return;  // Beallionaire: nothing granted, nothing added to the pending sale total
    }
    g_origSoldPlort(self, price, mi);
}

void PlatformGrantDetour(Il2CppObject* self, Il2CppObject* data, const MethodInfo* mi) {
    bool block = false;
    if (g_platformGrantGuard.Run([&] {
            block = Block(self, g_steamClass, "SteamMetaGamePlatform.Grant", Identifier(data));
        }) &&
        block) {
        return;
    }
    g_origPlatformGrant(self, data, mi);
}

Il2CppClass* Nested(Il2CppClass* outer, std::string_view name) {
    using NestedFn = Il2CppClass* (*)(Il2CppClass*, void**);
    HMODULE ga = api().module;
    auto nested = reinterpret_cast<NestedFn>(ga ? GetProcAddress(ga, "il2cpp_class_get_nested_types") : nullptr);
    if (!nested || !outer || !api().il2cpp_class_get_name) return nullptr;
    void* iter = nullptr;
    while (Il2CppClass* n = nested(outer, &iter)) {
        const char* nn = api().il2cpp_class_get_name(n);
        if (nn && name == nn) return n;
    }
    return nullptr;
}

// The 2-argument overload of `name` on `k` whose first parameter's class is `first`.
const MethodInfo* OverloadByFirstParam(Il2CppClass* k, const char* name, Il2CppClass* first) {
    const auto& a = api();
    if (!k || !first || !a.il2cpp_class_get_methods || !a.il2cpp_method_get_name || !a.il2cpp_method_get_param_count ||
        !a.il2cpp_method_get_param || !a.il2cpp_class_from_type)
        return nullptr;
    void* iter = nullptr;
    while (const MethodInfo* m = a.il2cpp_class_get_methods(k, &iter)) {
        const char* mn = a.il2cpp_method_get_name(m);
        if (!mn || std::string_view(mn) != name || a.il2cpp_method_get_param_count(m) != 2) continue;
        const Il2CppType* t = a.il2cpp_method_get_param(m, 0);
        if (t && a.il2cpp_class_from_type(t) == first) return m;
    }
    return nullptr;
}

}  // namespace

bool SetupAchievementPause(sr2::core::ModuleContext& ctx) {
    auto& R = ctx.resolver();
    std::string err;
    g_directorClass = R.Class(kImage, kNs, "MetaGameDirector", &err);
    if (!g_directorClass) return ctx.Fail("achievement pause: " + err);
    g_dataClass = Nested(g_directorClass, "AchievementData");
    Il2CppClass* achievementEnum = Nested(g_directorClass, "Achievement");
    if (!g_dataClass || !achievementEnum)
        return ctx.Fail("achievement pause: nested class MetaGameDirector.AchievementData / .Achievement not found");
    g_steamClass = R.Class(kImage, kPlatformNs, "SteamMetaGamePlatform", &err);
    if (!g_steamClass) return ctx.Fail("achievement pause: " + err);
    if (auto o = R.FieldOffset(g_dataClass, "AchievementIdentifier", &err)) g_offIdentifier = *o;  // log text only

    const MethodInfo* increment = R.Method(g_dataClass, "IncrementProgress", 1, &err);
    const MethodInfo* setProgress = OverloadByFirstParam(g_directorClass, "SetAchievementProgress", achievementEnum);
    const MethodInfo* soldPlort = R.Method(g_directorClass, "HandleSoldPlort", 1, &err);
    const MethodInfo* platformGrant = R.Method(g_steamClass, "Grant", 1, &err);
    if (!increment || !setProgress || !soldPlort || !platformGrant)
        return ctx.Fail(std::format("achievement pause: method not found ({}{}{}{})",
                                    increment ? "" : "AchievementData.IncrementProgress(int) ",
                                    setProgress ? "" : "MetaGameDirector.SetAchievementProgress(Achievement, int) ",
                                    soldPlort ? "" : "MetaGameDirector.HandleSoldPlort(int) ",
                                    platformGrant ? "" : "SteamMetaGamePlatform.Grant(AchievementData)"));

    struct Hook {
        const char* name;
        const MethodInfo* method;
        void* detour;
        void** original;
    };
    const Hook hooks[] = {
        {"AchievementData.IncrementProgress", increment, reinterpret_cast<void*>(&IncrementDetour),
         reinterpret_cast<void**>(&g_origIncrement)},
        {"MetaGameDirector.SetAchievementProgress", setProgress, reinterpret_cast<void*>(&SetProgressDetour),
         reinterpret_cast<void**>(&g_origSetProgress)},
        {"MetaGameDirector.HandleSoldPlort", soldPlort, reinterpret_cast<void*>(&SoldPlortDetour),
         reinterpret_cast<void**>(&g_origSoldPlort)},
        {"SteamMetaGamePlatform.Grant", platformGrant, reinterpret_cast<void*>(&PlatformGrantDetour),
         reinterpret_cast<void**>(&g_origPlatformGrant)},
    };
    std::string rvas;
    for (const Hook& h : hooks) {
        void* target = R.MethodPointer(h.method, &err);
        if (!target) return ctx.Fail(std::format("achievement pause: {}: {}", h.name, err));
        // Created and enabled here, outside the module's hook set: they stay on while Sandbox is off.
        auto& hm = sr2::core::HookManager::Get();
        const std::string name = std::format("{}.{}", kHooks, h.name);
        if (!hm.Create(kHooks, name, target, h.detour, h.original, &err) || !hm.Enable(name, &err))
            return ctx.Fail(std::format("achievement pause: hook {}: {}", h.name, err));
        rvas += std::format("{}{} +0x{:X}", rvas.empty() ? "" : ", ", h.name, Resolver::Rva(target));
    }
    SR2_INFO("[{}] achievement pause hooks ready (always on; they block while Sandbox is on or the loaded save is "
             "marked as cheated): {}", kModule, rvas);
    return true;
}

void SetAchievementsPaused(bool paused) {
    if (paused) g_logged.store(false);
    if (g_paused.exchange(paused) != paused)
        SR2_INFO("[{}] Sandbox {}: achievements {}", kModule, paused ? "on" : "off",
                 paused ? "paused" : g_cheated.load() ? "stay blocked (this save is marked as cheated)" : "resumed");
}

bool AchievementsPaused() { return g_paused.load(); }

void SetSaveCheated(bool cheated) {
    sr2::ui::sandbox::SetSaveCheated(cheated);
    if (cheated) g_logged.store(false);
    if (g_cheated.exchange(cheated) != cheated && !g_paused.load())
        SR2_INFO("[{}] achievements {}", kModule,
                 cheated ? "blocked: the loaded save is marked as cheated"
                         : "resumed: no save marked as cheated is loaded");
}

bool SaveCheated() { return g_cheated.load(); }

}  // namespace sr2::modules::sandbox
