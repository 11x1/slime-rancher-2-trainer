// sandbox: achievements paused while the module is enabled (see achievements.h). SR2 1.3.0.

#include "modules/sandbox/achievements.h"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>

#include "core/detour_guard.h"
#include "core/il2cpp_api.h"
#include "core/log.h"
#include "core/module.h"
#include "core/unity.h"

namespace sr2::modules::sandbox {
namespace {

namespace unity = sr2::core::unity;
using sr2::il2cpp::api;
using sr2::il2cpp::Resolver;

constexpr const char* kModule = "sandbox";
constexpr const char* kImage = "Assembly-CSharp";
constexpr const char* kNs = "MonomiPark.SlimeRancher";
constexpr const char* kPlatformNs = "MonomiPark.SlimeRancher.Platform";

std::atomic<bool> g_paused{false};
std::atomic<bool> g_logged{false};  // one "blocked" line per enable

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

sr2::core::DetourGuard g_incrementGuard{kModule, "AchievementData.IncrementProgress"};
sr2::core::DetourGuard g_setProgressGuard{kModule, "MetaGameDirector.SetAchievementProgress"};
sr2::core::DetourGuard g_soldPlortGuard{kModule, "MetaGameDirector.HandleSoldPlort"};
sr2::core::DetourGuard g_platformGrantGuard{kModule, "SteamMetaGamePlatform.Grant"};

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
    if (!g_paused.load(std::memory_order_relaxed) || !IsA(self, k)) return false;
    if (!g_logged.exchange(true)) {
        const std::string which = achievement >= 0 ? std::format(" (achievement #{})", achievement) : std::string{};
        SR2_INFO("[{}] achievements paused: blocked {}{} while Sandbox is on; further blocks are not logged", kModule,
                 what, which);
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
        if (!ctx.HookAddress(h.name, target, h.detour, h.original)) return false;
        rvas += std::format("{}{} +0x{:X}", rvas.empty() ? "" : ", ", h.name, Resolver::Rva(target));
    }
    SR2_INFO("[{}] achievement pause hooks ready: {}", kModule, rvas);
    return true;
}

void SetAchievementsPaused(bool paused) {
    if (paused) g_logged.store(false);
    if (g_paused.exchange(paused) != paused)
        SR2_INFO("[{}] achievements {} (Sandbox {})", kModule, paused ? "paused" : "resumed", paused ? "on" : "off");
}

}  // namespace sr2::modules::sandbox
