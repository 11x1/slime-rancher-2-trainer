// custom_upgrades: Auto-Feeder food lock (see feeder_lock.h). SR2 1.3.0, all lookups by name.

#include "modules/custom_upgrades/feeder_lock.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <format>
#include <initializer_list>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "core/detour_guard.h"
#include "core/il2cpp_api.h"
#include "core/log.h"
#include "core/module.h"
#include "core/resolver.h"
#include "core/unity.h"

namespace sr2::modules::custom_upgrades::feeder {
namespace {

namespace unity = sr2::core::unity;
using sr2::il2cpp::api;
using sr2::il2cpp::Resolver;

constexpr const char* kModule = "custom_upgrades";
constexpr const char* kImage = "Assembly-CSharp";
constexpr const char* kCore = "UnityEngine.CoreModule";

// ---- storage -> locked food (read by the detours on any thread) ----
SRWLOCK g_lock = SRWLOCK_INIT;
std::vector<std::pair<Il2CppObject*, Il2CppObject*>> g_locks;  // SiloStorage -> IdentifiableType
std::atomic<bool> g_any{false};

// true = `type` may go into `storage` (not a locked feeder, or its locked food). Pointer compares only.
bool Allowed(Il2CppObject* storage, Il2CppObject* type) {
    AcquireSRWLockShared(&g_lock);
    bool allowed = true;
    for (const auto& [s, t] : g_locks)
        if (s == storage) {
            allowed = type == t;
            break;
        }
    ReleaseSRWLockShared(&g_lock);
    return allowed;
}

void Publish(std::vector<std::pair<Il2CppObject*, Il2CppObject*>> locks) {
    AcquireSRWLockExclusive(&g_lock);
    g_locks = std::move(locks);
    g_any.store(!g_locks.empty(), std::memory_order_release);
    ReleaseSRWLockExclusive(&g_lock);
}

// ---- detours ----
using CanSpecificFn = bool (*)(Il2CppObject*, const void*, int32_t, bool, const MethodInfo*);
using CanAnyFn = bool (*)(Il2CppObject*, const void*, const MethodInfo*);
using AddSpecificFn = bool (*)(Il2CppObject*, Il2CppObject*, int32_t, bool, const MethodInfo*);
using AddAnyFn = bool (*)(Il2CppObject*, const void*, const MethodInfo*);
using AddResourceFn = bool (*)(Il2CppObject*, Il2CppObject*, int32_t, int32_t, bool, const MethodInfo*);
CanSpecificFn g_origCanSpecific = nullptr;
CanAnyFn g_origCanAny = nullptr;
AddSpecificFn g_origAddSpecific = nullptr;
AddAnyFn g_origAddAny = nullptr;
AddResourceFn g_origAddResource = nullptr;
size_t g_offIdentType = 0;  // Identifiable.identType

sr2::core::DetourGuard g_canGuard{kModule, "custom_upgrades.FeederLock.CanAccept"};
sr2::core::DetourGuard g_addGuard{kModule, "custom_upgrades.FeederLock.MaybeAdd"};

std::set<std::string> g_refusedLogged;  // food names whose first refusal was logged (main thread only)

// AmmoSlot.AmmoMetadata (a 32-byte struct, passed by reference): Id (IdentifiableType) at +0x0.
Il2CppObject* MetadataType(const void* md) { return md ? *static_cast<Il2CppObject* const*>(md) : nullptr; }

void LogRefusal(Il2CppObject* type, const char* via) {
    const std::string name = type ? unity::GetName(type) : std::string("(none)");
    if (g_refusedLogged.size() < 64 && g_refusedLogged.insert(name).second)
        SR2_INFO("[{}] locked auto-feeder refused '{}' ({}; logged once per food)", kModule, name, via);
}

bool CanSpecificDetour(Il2CppObject* self, const void* md, int32_t slot, bool overflow, const MethodInfo* mi) {
    if (g_any.load(std::memory_order_acquire) && self) {
        bool allowed = true;
        g_canGuard.Run([&] { allowed = Allowed(self, MetadataType(md)); });
        if (!allowed) return false;  // no drone destination for this food (GetSinks)
    }
    return g_origCanSpecific(self, md, slot, overflow, mi);
}

bool CanAnyDetour(Il2CppObject* self, const void* md, const MethodInfo* mi) {
    if (g_any.load(std::memory_order_acquire) && self) {
        bool allowed = true;
        g_canGuard.Run([&] { allowed = Allowed(self, MetadataType(md)); });
        if (!allowed) return false;
    }
    return g_origCanAny(self, md, mi);
}

bool AddSpecificDetour(Il2CppObject* self, Il2CppObject* ident, int32_t slot, bool overflow, const MethodInfo* mi) {
    if (g_any.load(std::memory_order_acquire) && self && ident) {
        bool allowed = true;
        g_addGuard.Run([&] {
            Il2CppObject* type = unity::Field<Il2CppObject*>(ident, g_offIdentType);
            allowed = Allowed(self, type);
            if (!allowed) LogRefusal(type, "shot into the feeder");
        });
        if (!allowed) return false;  // SiloCatcher.Insert: the item stays outside, as for a non-food
    }
    return g_origAddSpecific(self, ident, slot, overflow, mi);
}

bool AddAnyDetour(Il2CppObject* self, const void* md, const MethodInfo* mi) {
    if (g_any.load(std::memory_order_acquire) && self) {
        bool allowed = true;
        g_addGuard.Run([&] {
            allowed = Allowed(self, MetadataType(md));
            if (!allowed) LogRefusal(MetadataType(md), "MaybeAddToAnySlot");
        });
        if (!allowed) return false;
    }
    return g_origAddAny(self, md, mi);
}

bool AddResourceDetour(Il2CppObject* self, Il2CppObject* type, int32_t slot, int32_t count, bool overflow,
                       const MethodInfo* mi) {
    if (g_any.load(std::memory_order_acquire) && self) {
        bool allowed = true;
        g_addGuard.Run([&] {
            allowed = Allowed(self, type);
            if (!allowed) LogRefusal(type, "MaybeAddAsResource");
        });
        if (!allowed) return false;
    }
    return g_origAddResource(self, type, slot, count, overflow, mi);
}

// ---- main-thread lookups ----
struct Lookups {
    Il2CppClass* feederClass = nullptr;   // SlimeFeeder
    Il2CppClass* storageClass = nullptr;  // SiloStorage
    Il2CppClass* identTypeClass = nullptr;
    Il2CppClass* groupClass = nullptr;    // IdentifiableTypeGroup
    Il2CppClass* slotDefClass = nullptr;  // AmmoSlotDefinition
    Il2CppClass* lsClass = nullptr;       // UnityEngine.Localization.LocalizedString
    size_t offFeederStorage = 0;          // SlimeFeeder._storage
    size_t offStorageDefs = 0;            // SiloStorage.AmmoSlotDefinitions
    size_t offDefGroup = 0, offDefBlock = 0;  // AmmoSlotDefinition._slotTypeGroup / _slotBlockList
    size_t offMembers = 0, offSubGroups = 0;  // IdentifiableTypeGroup._memberTypes / _memberGroups
    size_t offTypeName = 0;                   // IdentifiableType.localizedName
    const MethodInfo* compInChildren = nullptr;  // Component.GetComponentInChildren(Type, bool)
    const MethodInfo* lsGet = nullptr;           // LocalizedString.GetLocalizedString()
    std::map<Il2CppObject*, std::vector<Food>> foods;  // per slot group (session)
    std::map<std::string, std::string> applied;        // plot id -> locked food (log on change)
};
Lookups* g_l = nullptr;

bool IsA(Il2CppObject* obj, Il2CppClass* k) {
    const auto& a = api();
    if (!obj || !k || !a.il2cpp_object_get_class) return false;
    Il2CppClass* c = a.il2cpp_object_get_class(obj);
    if (c == k) return true;
    return c && a.il2cpp_class_is_assignable_from && a.il2cpp_class_is_assignable_from(k, c);
}

Il2CppObject* Call(const MethodInfo* m, void* obj, std::initializer_list<void*> args, const char* what, bool* ok = nullptr) {
    if (!m) {
        if (ok) *ok = false;
        return nullptr;
    }
    std::vector<void*> params(args);
    std::string err;
    Il2CppObject* r = Resolver::Get().Invoke(m, obj, params.empty() ? nullptr : params.data(), &err);
    const bool good = err.empty();
    if (ok) *ok = good;
    if (!good) {
        if (err.rfind("native exception", 0) == 0) sr2::core::MarkModuleFaulted(kModule, std::format("{}: {}", what, err));
        else SR2_WARN("[{}] {} failed: {}", kModule, what, err);
    }
    return r;
}

// The storage of the corral's active feeder (Awake has set _storage), nullptr without one.
Il2CppObject* FeederStorage(Il2CppObject* plot) {
    if (!g_l || !unity::IsAlive(plot)) return nullptr;
    bool inactive = false, ok = false;
    Il2CppObject* feeder = Call(g_l->compInChildren, plot, {unity::TypeOf(g_l->feederClass), &inactive},
                                "GetComponentInChildren(SlimeFeeder)", &ok);
    if (!ok || !unity::IsAlive(feeder) || !IsA(feeder, g_l->feederClass)) return nullptr;
    Il2CppObject* storage = unity::Field<Il2CppObject*>(feeder, g_l->offFeederStorage);
    return unity::IsAlive(storage) && IsA(storage, g_l->storageClass) ? storage : nullptr;
}

std::string DisplayName(Il2CppObject* type) {
    Il2CppObject* ls = unity::Field<Il2CppObject*>(type, g_l->offTypeName);
    if (ls && g_l->lsGet && IsA(ls, g_l->lsClass)) {
        bool ok = false;
        Il2CppObject* s = Call(g_l->lsGet, ls, {}, "LocalizedString.GetLocalizedString", &ok);
        std::string name = ok && s ? unity::ToUtf8(reinterpret_cast<Il2CppString*>(s)) : std::string();
        if (!name.empty()) return name;
    }
    return unity::GetName(type);
}

void Collect(Il2CppObject* group, void* blockList, int depth, std::vector<Food>& out, std::set<Il2CppObject*>& seen) {
    if (!unity::IsAlive(group) || !IsA(group, g_l->groupClass) || depth > 4 || out.size() >= 256) return;
    void* members = unity::Field<void*>(group, g_l->offMembers);
    for (size_t i = 0, n = unity::ListCount(members); i < n && i < 256; ++i) {
        auto* t = static_cast<Il2CppObject*>(unity::ListAt(members, i));
        if (!unity::IsAlive(t) || !IsA(t, g_l->identTypeClass) || !seen.insert(t).second) continue;
        bool blocked = false;
        for (size_t b = 0, nb = unity::ListCount(blockList); b < nb && b < 256 && !blocked; ++b)
            blocked = unity::ListAt(blockList, b) == t;
        if (!blocked) out.push_back({t, unity::GetName(t), {}});  // names: Foods(), at menu time
    }
    void* groups = unity::Field<void*>(group, g_l->offSubGroups);
    for (size_t i = 0, n = unity::ListCount(groups); i < n && i < 64; ++i)
        Collect(static_cast<Il2CppObject*>(unity::ListAt(groups, i)), blockList, depth + 1, out, seen);
}

const std::vector<Food>* FoodsOf(Il2CppObject* storage) {
    void* defs = unity::Field<void*>(storage, g_l->offStorageDefs);
    auto* def = static_cast<Il2CppObject*>(unity::ArrayAt(defs, 0));
    if (!unity::IsAlive(def) || !IsA(def, g_l->slotDefClass)) return nullptr;
    Il2CppObject* group = unity::Field<Il2CppObject*>(def, g_l->offDefGroup);
    if (!unity::IsAlive(group)) return nullptr;
    auto it = g_l->foods.find(group);
    if (it != g_l->foods.end()) return &it->second;
    std::vector<Food> list;
    std::set<Il2CppObject*> seen;
    Collect(group, unity::Field<void*>(def, g_l->offDefBlock), 0, list, seen);
    std::string names;
    for (const Food& f : list) names += (names.empty() ? "" : ", ") + f.id;
    SR2_INFO("[{}] auto-feeder food lock: {} food(s) in group '{}' ({} slot definition(s)): {}", kModule, list.size(),
             unity::GetName(group), unity::ArrayLength(defs), names);
    return &g_l->foods.emplace(group, std::move(list)).first->second;
}

}  // namespace

bool FeederLock::Setup(sr2::core::ModuleContext& ctx, std::string& why, std::string& report) {
    Resolver& R = ctx.resolver();
    auto parentOf = [](Il2CppClass* c) { return api().il2cpp_class_get_parent ? api().il2cpp_class_get_parent(c) : nullptr; };
    auto cls = [&](const char* image, const char* ns, const char* name) -> Il2CppClass* {
        std::string err;
        Il2CppClass* k = R.Class(image, ns, name, &err);
        if (!k && why.empty()) why = err.empty() ? std::format("class {}.{} not found", ns, name) : err;
        return k;
    };
    auto off = [&](Il2CppClass* k, const char* field, size_t& out) {
        std::string err;
        for (Il2CppClass* c = k; c; c = parentOf(c))
            if (auto o = R.FieldOffset(c, field, &err)) {
                out = *o;
                return;
            }
        if (why.empty()) why = std::format("field {} not found", field);
    };
    auto byParams = [&](Il2CppClass* k, const char* name, std::initializer_list<std::string_view> params) {
        std::string err;
        const MethodInfo* m = k ? R.MethodByParams(k, name, params, &err) : nullptr;
        if (!m && why.empty()) why = std::format("{}: {}", name, err);
        return m;
    };

    auto* l = new Lookups();
    l->feederClass = cls(kImage, "", "SlimeFeeder");
    l->storageClass = cls(kImage, "", "SiloStorage");
    l->identTypeClass = cls(kImage, "", "IdentifiableType");
    l->groupClass = cls(kImage, "", "IdentifiableTypeGroup");
    l->slotDefClass = cls(kImage, "", "AmmoSlotDefinition");
    Il2CppClass* ident = cls(kImage, "", "Identifiable");
    Il2CppClass* component = cls(kCore, "UnityEngine", "Component");
    {
        std::string err;  // optional: names fall back to the asset names
        l->lsClass = R.Class("Unity.Localization", "UnityEngine.Localization", "LocalizedString", &err);
        if (l->lsClass) l->lsGet = R.Method(l->lsClass, "GetLocalizedString", 0, &err);
    }
    if (!why.empty()) {
        delete l;
        return false;
    }
    off(l->feederClass, "_storage", l->offFeederStorage);
    off(l->storageClass, "AmmoSlotDefinitions", l->offStorageDefs);
    off(l->slotDefClass, "_slotTypeGroup", l->offDefGroup);
    off(l->slotDefClass, "_slotBlockList", l->offDefBlock);
    off(l->groupClass, "_memberTypes", l->offMembers);
    off(l->groupClass, "_memberGroups", l->offSubGroups);
    off(l->identTypeClass, "localizedName", l->offTypeName);
    off(ident, "identType", g_offIdentType);
    l->compInChildren = byParams(component, "GetComponentInChildren", {"System.Type", "System.Boolean"});
    // No overloads: by argument count (each is the class's own body, re/hooks.md).
    std::string err;
    const MethodInfo* canSpecific = R.Method(l->storageClass, "CanSpecificSlotAccept", 3, &err);
    const MethodInfo* canAny = R.Method(l->storageClass, "CanAnySlotAccept", 1, &err);
    const MethodInfo* addSpecific = R.Method(l->storageClass, "MaybeAddToSpecificSlot", 3, &err);
    const MethodInfo* addAny = R.Method(l->storageClass, "MaybeAddToAnySlot", 1, &err);
    const MethodInfo* addResource = R.Method(l->storageClass, "MaybeAddAsResource", 4, &err);
    if (why.empty() && (!canSpecific || !canAny || !addSpecific || !addAny || !addResource))
        why = "SiloStorage CanSpecificSlotAccept / CanAnySlotAccept / MaybeAddToSpecificSlot / MaybeAddToAnySlot / "
              "MaybeAddAsResource not found";
    if (!why.empty()) {
        delete l;
        return false;
    }

    struct H {
        const char* name;
        const MethodInfo* mi;
        void* detour;
        void** orig;
    };
    const H hooks[] = {
        {"SiloStorage.CanSpecificSlotAccept", canSpecific, reinterpret_cast<void*>(&CanSpecificDetour),
         reinterpret_cast<void**>(&g_origCanSpecific)},
        {"SiloStorage.CanAnySlotAccept", canAny, reinterpret_cast<void*>(&CanAnyDetour),
         reinterpret_cast<void**>(&g_origCanAny)},
        {"SiloStorage.MaybeAddToSpecificSlot", addSpecific, reinterpret_cast<void*>(&AddSpecificDetour),
         reinterpret_cast<void**>(&g_origAddSpecific)},
        {"SiloStorage.MaybeAddToAnySlot", addAny, reinterpret_cast<void*>(&AddAnyDetour),
         reinterpret_cast<void**>(&g_origAddAny)},
        {"SiloStorage.MaybeAddAsResource", addResource, reinterpret_cast<void*>(&AddResourceDetour),
         reinterpret_cast<void**>(&g_origAddResource)},
    };
    // All or nothing: resolve every body first, so a missing one creates no hook at all.
    void* bodies[sizeof(hooks) / sizeof(hooks[0])] = {};
    for (size_t i = 0; i < sizeof(hooks) / sizeof(hooks[0]); ++i) {
        std::string e;
        bodies[i] = R.MethodPointer(hooks[i].mi, &e);
        if (!bodies[i]) {
            why = std::format("{}: no native body ({})", hooks[i].name, e);
            delete l;
            return false;
        }
    }
    for (size_t i = 0; i < sizeof(hooks) / sizeof(hooks[0]); ++i) {
        if (!ctx.HookAddress(hooks[i].name, bodies[i], hooks[i].detour, hooks[i].orig)) {
            // Hooks made so far pass through: nothing is ever locked without Ready().
            why = std::format("hook {} failed", hooks[i].name);
            delete l;
            return false;
        }
        report += std::format(" {}@0x{:X}", hooks[i].name, Resolver::Rva(bodies[i]));
    }
    g_l = l;
    ready_ = true;
    return true;
}

bool FeederLock::Foods(Il2CppObject* plot, std::vector<Food>& out) {
    out.clear();
    if (!ready_ || !g_l) return false;
    Il2CppObject* storage = FeederStorage(plot);
    const std::vector<Food>* list = storage ? FoodsOf(storage) : nullptr;
    if (!list) return false;
    out = *list;
    for (Food& f : out) f.name = DisplayName(f.type);  // the current language
    return true;
}

void FeederLock::Tick(const std::vector<LockRequest>& locks) {
    if (!ready_ || !g_l) return;
    if (locks.empty() && !g_any.load(std::memory_order_acquire) && g_l->applied.empty()) return;
    std::vector<std::pair<Il2CppObject*, Il2CppObject*>> table;
    std::map<std::string, std::string> applied;
    for (const LockRequest& r : locks) {
        Il2CppObject* storage = FeederStorage(r.plot);
        const std::vector<Food>* foods = storage ? FoodsOf(storage) : nullptr;
        if (!foods) continue;  // no feeder bought (yet): nothing to lock
        auto f = std::find_if(foods->begin(), foods->end(), [&](const Food& x) { return x.id == r.food; });
        if (f == foods->end()) {
            if (g_l->applied[r.id] != "?" + r.food)
                SR2_WARN("[{}] auto-feeder on plot '{}': locked food '{}' is not a feeder food - not locked", kModule,
                         r.id, r.food);
            applied[r.id] = "?" + r.food;
            continue;
        }
        table.emplace_back(storage, f->type);
        applied[r.id] = r.food;
        if (sr2::core::ModuleFaulted(kModule)) return;
    }
    for (const auto& [id, food] : applied)
        if (food[0] != '?' && g_l->applied[id] != food)
            SR2_INFO("[{}] auto-feeder on plot '{}' locked to '{}' (drones bring only that food)", kModule, id, food);
    for (const auto& [id, food] : g_l->applied)
        if (!applied.count(id) && !food.empty() && food[0] != '?')
            SR2_INFO("[{}] auto-feeder on plot '{}' unlocked (takes any food)", kModule, id);
    g_l->applied = std::move(applied);
    Publish(std::move(table));
}

void FeederLock::SetNow(Il2CppObject* plot, Il2CppObject* type, const std::string& id) {
    if (!ready_ || !g_l) return;
    Il2CppObject* storage = FeederStorage(plot);
    if (!storage) return;
    std::vector<std::pair<Il2CppObject*, Il2CppObject*>> table;
    AcquireSRWLockShared(&g_lock);
    for (const auto& e : g_locks)
        if (e.first != storage) table.push_back(e);
    ReleaseSRWLockShared(&g_lock);
    if (type) table.emplace_back(storage, type);
    Publish(std::move(table));
    SR2_DEBUG("[{}] auto-feeder on plot '{}': {} (plot menu)", kModule, id,
              type ? std::format("locked to '{}'", unity::GetName(type)) : std::string("unlocked"));
}

void FeederLock::Cleanup(const char* why) {
    const bool had = g_any.load(std::memory_order_acquire);
    Publish({});
    if (g_l) g_l->applied.clear();
    if (had) SR2_INFO("[{}] {}: auto-feeder locks released (feeders take any food)", kModule, why);
}

}  // namespace sr2::modules::custom_upgrades::feeder
