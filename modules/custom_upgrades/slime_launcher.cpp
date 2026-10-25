// custom_upgrades: Slime Launcher effect (see slime_launcher.h). SR2 1.3.0, all lookups by name; RVAs
// for reference only.

#include "modules/custom_upgrades/slime_launcher.h"

#include <algorithm>
#include <atomic>
#include <format>
#include <string>

#include "core/detour_guard.h"
#include "core/il2cpp_api.h"
#include "core/log.h"
#include "core/main_thread.h"
#include "core/module.h"
#include "core/resolver.h"
#include "core/unity.h"

namespace sr2::modules::custom_upgrades::launcher {
namespace {

namespace unity = sr2::core::unity;
namespace main_thread = sr2::core::main_thread;
using sr2::il2cpp::api;
using sr2::il2cpp::Resolver;

constexpr const char* kModule = "custom_upgrades";
constexpr const char* kImage = "Assembly-CSharp";
constexpr int kFindEvery = 10;  // CardSlime: FindObjectsOfTypeAll(SlimeDefinition) at most every 10 calls

// Expel(AmmoMetadata metadata, bool ignoreEmotions, float velocityOverride, bool forceUncollected): the
// 32-byte struct goes by pointer (x64 IL2CPP ABI); its IdentifiableType Id is at +0.
using ExpelFn = Il2CppObject* (*)(Il2CppObject*, void*, bool, float, bool, const MethodInfo*);
using ExpelHeldFn = void (*)(Il2CppObject*, float, const MethodInfo*);

struct State {
    Il2CppClass* slimeDefClass = nullptr;
    Il2CppClass* identifiableClass = nullptr;
    size_t offEjectSpeed = 0;  // VacuumItem.EjectSpeed
    size_t offHeld = 0;        // VacuumItem._held (GameObject)
    size_t offIdentType = 0;   // Identifiable.identType
    ExpelFn origExpel = nullptr;
    ExpelHeldFn origHeld = nullptr;

    // main thread
    Il2CppObject* vac = nullptr;  // VacuumItem whose vanilla EjectSpeed is remembered (identity only)
    float baseSpeed = 0.0f;
    int loggedLevel[2] = {};      // level last logged for an ammo / held shot
    Il2CppObject* cardSlime = nullptr;  // "Pink" SlimeDefinition (GC handle held)
    int findTimer = kFindEvery;
    bool loggedNoSlime = false;
};

State g;
std::atomic<int> g_level{0};

sr2::core::DetourGuard g_expelGuard{kModule, "VacuumItem.Expel(AmmoMetadata)"};
sr2::core::DetourGuard g_heldGuard{kModule, "VacuumItem.ExpelHeld"};

bool IsSlime(Il2CppObject* type) {
    const auto& a = api();
    if (!type || !g.slimeDefClass || !a.il2cpp_object_get_class) return false;
    Il2CppClass* c = a.il2cpp_object_get_class(type);
    return c == g.slimeDefClass ||
           (c && a.il2cpp_class_is_assignable_from && a.il2cpp_class_is_assignable_from(g.slimeDefClass, c));
}

// The factor for this shot (1 = vanilla) and the VacuumItem's vanilla speed, remembered per VacuumItem.
float Factor(Il2CppObject* vac) {
    const int level = std::clamp(g_level.load(std::memory_order_relaxed), 0, kLevels);
    if (level == 0 || !vac || !main_thread::IsMainThread()) return 1.0f;
    if (vac != g.vac) {
        g.vac = vac;
        g.baseSpeed = unity::Field<float>(vac, g.offEjectSpeed);
    }
    return kSpeedFactor[level];
}

void LogShot(int path, Il2CppObject* type, float factor) {
    const int level = g_level.load(std::memory_order_relaxed);
    if (g.loggedLevel[path] == level) return;
    g.loggedLevel[path] = level;
    SR2_INFO("[{}] slime launcher: {} '{}' shot at {:g}x speed (EjectSpeed {:g} -> {:g}, Slime Launcher {}; logged once "
             "per level)", kModule, path == 0 ? "vacpack slime" : "held slime", unity::GetName(type), factor, g.baseSpeed,
             g.baseSpeed * factor, level);
}

Il2CppObject* ExpelDetour(Il2CppObject* self, void* metadata, bool ignoreEmotions, float velocityOverride,
                          bool forceUncollected, const MethodInfo* mi) {
    float factor = 1.0f;
    g_expelGuard.Run([&] {
        Il2CppObject* type = metadata ? *static_cast<Il2CppObject**>(metadata) : nullptr;
        if (velocityOverride > 0.0f || !IsSlime(type)) return;
        factor = Factor(self);
        if (factor == 1.0f) return;
        unity::Field<float>(self, g.offEjectSpeed) = g.baseSpeed * factor;
        LogShot(0, type, factor);
    });
    Il2CppObject* r = g.origExpel(self, metadata, ignoreEmotions, velocityOverride, forceUncollected, mi);
    if (factor != 1.0f) unity::Field<float>(self, g.offEjectSpeed) = g.baseSpeed;
    return r;
}

void ExpelHeldDetour(Il2CppObject* self, float volume, const MethodInfo* mi) {
    float factor = 1.0f;
    g_heldGuard.Run([&] {
        if (g_level.load(std::memory_order_relaxed) <= 0) return;
        Il2CppObject* held = unity::Field<Il2CppObject*>(self, g.offHeld);
        Il2CppObject* ident = unity::IsAlive(held) ? unity::GetComponent(held, g.identifiableClass) : nullptr;
        Il2CppObject* type = ident ? unity::Field<Il2CppObject*>(ident, g.offIdentType) : nullptr;
        if (!IsSlime(type)) return;
        factor = Factor(self);
        if (factor == 1.0f) return;
        unity::Field<float>(self, g.offEjectSpeed) = g.baseSpeed * factor;
        LogShot(1, type, factor);
    });
    g.origHeld(self, volume, mi);
    if (factor != 1.0f) unity::Field<float>(self, g.offEjectSpeed) = g.baseSpeed;
}

}  // namespace

void SetLevel(int level) {
    level = std::clamp(level, 0, kLevels);
    const int old = g_level.exchange(level);
    if (old != level) SR2_INFO("[{}] slime launcher: level {} -> {} (slime shot speed x{:g})", kModule, old, level,
                               kSpeedFactor[level]);
}
int Level() { return g_level.load(); }

Il2CppObject* CardSlime() {
    if (g.cardSlime || !g.slimeDefClass || !main_thread::IsMainThread()) return g.cardSlime;
    if (++g.findTimer < kFindEvery) return nullptr;
    g.findTimer = 0;
    for (Il2CppObject* o : unity::FindObjectsOfTypeAll(g.slimeDefClass)) {
        if (!unity::IsAlive(o) || unity::GetName(o) != "Pink") continue;
        unity::NewGCHandle(o);  // session-long
        g.cardSlime = o;
        break;
    }
    if (!g.cardSlime && !g.loggedNoSlime) {
        g.loggedNoSlime = true;
        SR2_INFO("[{}] slime launcher: the 'Pink' SlimeDefinition is not loaded yet (card keeps Tank Booster's art)",
                 kModule);
    }
    return g.cardSlime;
}

bool SlimeLauncher::Setup(sr2::core::ModuleContext& ctx, std::string& why, std::string& report) {
    Resolver& R = ctx.resolver();
    std::string err;
    auto cls = [&](const char* ns, const char* name) -> Il2CppClass* {
        Il2CppClass* k = R.Class(kImage, ns, name, &err);
        if (!k && why.empty()) why = err.empty() ? std::format("class {}.{} not found", ns, name) : err;
        return k;
    };
    auto off = [&](Il2CppClass* k, const char* field, size_t& out) {
        auto o = k ? R.FieldOffset(k, field, &err) : std::nullopt;
        if (o) out = *o;
        else if (why.empty()) why = std::format("field {} not found", field);
    };
    Il2CppClass* vac = cls("MonomiPark.SlimeRancher.Player.PlayerItems", "VacuumItem");
    g.slimeDefClass = cls("", "SlimeDefinition");
    g.identifiableClass = cls("", "Identifiable");
    if (!why.empty()) return false;
    off(vac, "EjectSpeed", g.offEjectSpeed);
    off(vac, "_held", g.offHeld);
    off(g.identifiableClass, "identType", g.offIdentType);
    if (!why.empty()) return false;
    // Expel has a 1-argument private overload (Expel(float), misc_tweaks' hook) and the 4-argument public one.
    const MethodInfo* expelMi = R.Method(vac, "Expel", 4, &err);
    const MethodInfo* heldMi = R.Method(vac, "ExpelHeld", 1, &err);
    void* expelP = expelMi ? R.MethodPointer(expelMi, &err) : nullptr;
    void* heldP = heldMi ? R.MethodPointer(heldMi, &err) : nullptr;
    if (!expelP || !heldP) {
        why = std::format("VacuumItem.{}: no native body ({})", expelP ? "ExpelHeld" : "Expel(AmmoMetadata, ...)", err);
        return false;
    }
    if (!ctx.HookAddress("VacuumItem.Expel(AmmoMetadata)", expelP, reinterpret_cast<void*>(&ExpelDetour),
                         reinterpret_cast<void**>(&g.origExpel))) {
        why = "hook VacuumItem.Expel(AmmoMetadata) failed";
        return false;
    }
    if (!ctx.HookAddress("VacuumItem.ExpelHeld", heldP, reinterpret_cast<void*>(&ExpelHeldDetour),
                         reinterpret_cast<void**>(&g.origHeld))) {
        why = "hook VacuumItem.ExpelHeld failed";
        return false;  // the ammo hook passes through while the level is 0 and is still correct otherwise
    }
    report += std::format(" VacuumItem.Expel(AmmoMetadata)@0x{:X} VacuumItem.ExpelHeld@0x{:X}", Resolver::Rva(expelP),
                          Resolver::Rva(heldP));
    ready_ = true;
    return true;
}

}  // namespace sr2::modules::custom_upgrades::launcher
