// custom_upgrades: Auto-Feeder "Leave last food item in" (see feeder_lock.h). SR2 1.3.0, all lookups by name.

#include "modules/custom_upgrades/feeder_lock.h"

#include <windows.h>

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
#include "core/main_thread.h"
#include "core/module.h"
#include "core/resolver.h"
#include "core/unity.h"

namespace sr2::modules::custom_upgrades::feeder {
namespace {

namespace unity = sr2::core::unity;
namespace main_thread = sr2::core::main_thread;
using sr2::il2cpp::api;
using sr2::il2cpp::Resolver;

constexpr const char* kModule = "custom_upgrades";
constexpr const char* kImage = "Assembly-CSharp";
constexpr const char* kCore = "UnityEngine.CoreModule";

// ---- feeders with the toggle on (read by the detours) ----
struct Entry {
    Il2CppObject* storage = nullptr;  // SiloStorage (SlimeFeeder._storage)
    Il2CppObject* feeder = nullptr;   // SlimeFeeder
    Il2CppObject* type = nullptr;     // kept food (IdentifiableType); nullptr while empty: takes any food
};
SRWLOCK g_lock = SRWLOCK_INIT;
std::vector<Entry> g_keep;
std::atomic<bool> g_any{false};

// true = `type` may go into `storage` (no kept feeder, an empty one, or its kept food). Pointer compares only.
bool Allowed(Il2CppObject* storage, Il2CppObject* type) {
    AcquireSRWLockShared(&g_lock);
    bool allowed = true;
    for (const Entry& e : g_keep)
        if (e.storage == storage) {
            allowed = !e.type || type == e.type;
            break;
        }
    ReleaseSRWLockShared(&g_lock);
    return allowed;
}

bool Keeps(Il2CppObject* feeder) {
    AcquireSRWLockShared(&g_lock);
    bool keeps = false;
    for (const Entry& e : g_keep) keeps |= e.feeder == feeder;
    ReleaseSRWLockShared(&g_lock);
    return keeps;
}

void Publish(std::vector<Entry> keep) {
    AcquireSRWLockExclusive(&g_lock);
    g_keep = std::move(keep);
    g_any.store(!g_keep.empty(), std::memory_order_release);
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
sr2::core::DetourGuard g_feedGuard{kModule, "custom_upgrades.FeederLock.Feed"};

std::set<std::string> g_refusedLogged;  // food names whose first refusal was logged (main thread only)

// AmmoSlot.AmmoMetadata (a 32-byte struct, passed by reference): Id (IdentifiableType) at +0x0.
Il2CppObject* MetadataType(const void* md) { return md ? *static_cast<Il2CppObject* const*>(md) : nullptr; }

void LogRefusal(Il2CppObject* type, const char* via) {
    const std::string name = type ? unity::GetName(type) : std::string("(none)");
    if (g_refusedLogged.size() < 64 && g_refusedLogged.insert(name).second)
        SR2_INFO("[{}] auto-feeder keeping its last food refused '{}' ({}; logged once per food)", kModule, name, via);
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
    size_t offFeederStorage = 0;          // SlimeFeeder._storage
    size_t offFeederModel = 0;            // SlimeFeeder._model (LandPlotModel)
    size_t offRemainingOps = 0;           // LandPlotModel.remainingFeedOperations
    const MethodInfo* compInChildren = nullptr;  // Component.GetComponentInChildren(Type, bool)
    const MethodInfo* foodCount = nullptr;       // SlimeFeeder.GetFoodCount()
    const MethodInfo* foodId = nullptr;          // SlimeFeeder.GetFoodId()
    std::map<std::string, std::string> applied;  // plot id -> kept food ("" = empty feeder; log on change)
    std::set<Il2CppObject*> keptLogged;          // feeders whose first held-back item was logged
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

// The corral's active feeder (Awake has set _storage) and its storage; false without one.
bool FeederOf(Il2CppObject* plot, Il2CppObject*& feeder, Il2CppObject*& storage) {
    feeder = storage = nullptr;
    if (!g_l || !unity::IsAlive(plot)) return false;
    bool inactive = false, ok = false;
    Il2CppObject* f = Call(g_l->compInChildren, plot, {unity::TypeOf(g_l->feederClass), &inactive},
                           "GetComponentInChildren(SlimeFeeder)", &ok);
    if (!ok || !unity::IsAlive(f) || !IsA(f, g_l->feederClass)) return false;
    Il2CppObject* s = unity::Field<Il2CppObject*>(f, g_l->offFeederStorage);
    if (!unity::IsAlive(s) || !IsA(s, g_l->storageClass)) return false;
    feeder = f;
    storage = s;
    return true;
}

// Food items in the feeder (slot 0, the one it feeds from); -1 = unknown.
int FoodCount(Il2CppObject* feeder) {
    bool ok = false;
    Il2CppObject* boxed = Call(g_l->foodCount, feeder, {}, "SlimeFeeder.GetFoodCount", &ok);
    return ok && boxed ? *reinterpret_cast<int32_t*>(reinterpret_cast<char*>(boxed) + 2 * sizeof(void*)) : -1;
}

// The kept food: the feeder's current food while it holds any, else nullptr (takes any food).
Il2CppObject* CurrentFood(Il2CppObject* feeder) {
    if (FoodCount(feeder) <= 0) return nullptr;
    bool ok = false;
    Il2CppObject* t = Call(g_l->foodId, feeder, {}, "SlimeFeeder.GetFoodId", &ok);
    return ok && unity::IsAlive(t) ? t : nullptr;
}

// ---- feeding out (main thread) ----
using FeedFn = void (*)(Il2CppObject*, bool, const MethodInfo*);
using RemainingFn = int32_t (*)(Il2CppObject*, const MethodInfo*);
FeedFn g_origFeed = nullptr;
RemainingFn g_origRemaining = nullptr;

// SlimeFeeder.ProcessFeedOperation(ejectFood): with one item (or none) left in a kept feeder, vanilla's empty
// path only (one pending operation used up, nothing fed). FixedUpdate keeps calling it while operations are
// pending, as for an empty feeder.
void FeedDetour(Il2CppObject* self, bool eject, const MethodInfo* mi) {
    if (g_any.load(std::memory_order_acquire) && self && g_l && main_thread::IsMainThread()) {
        bool held = false;
        g_feedGuard.Run([&] {
            if (!Keeps(self)) return;
            const int count = FoodCount(self);
            if (count < 0 || count > 1) return;
            Il2CppObject* model = unity::Field<Il2CppObject*>(self, g_l->offFeederModel);
            if (!model) return;  // the original NREs as well; leave it to the game
            int32_t& ops = *reinterpret_cast<int32_t*>(reinterpret_cast<char*>(model) + g_l->offRemainingOps);
            ops = ops > 0 ? ops - 1 : 0;
            held = true;
            if (count == 1 && g_l->keptLogged.size() < 64 && g_l->keptLogged.insert(self).second)
                SR2_INFO("[{}] auto-feeder kept its last food item in ({}; logged once per feeder)", kModule,
                         eject ? "feeding" : "fast-forward");
        });
        if (held) return;
    }
    g_origFeed(self, eject, mi);
}

// SlimeFeeder.RemainingFeedOperationsFastForward(): the fast-forward's food budget, min(pending, count).
// A kept feeder offers one item less, so the fast-forward never counts the last one as eaten.
int32_t RemainingDetour(Il2CppObject* self, const MethodInfo* mi) {
    int32_t r = g_origRemaining(self, mi);
    if (g_any.load(std::memory_order_acquire) && self && g_l && r > 0 && main_thread::IsMainThread()) {
        g_feedGuard.Run([&] {
            if (!Keeps(self)) return;
            const int count = FoodCount(self);
            if (count >= 0 && r > count - 1) r = count > 1 ? count - 1 : 0;
        });
    }
    return r;
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
    Il2CppClass* plotModel = cls(kImage, "MonomiPark.SlimeRancher.DataModel", "LandPlotModel");
    Il2CppClass* ident = cls(kImage, "", "Identifiable");
    Il2CppClass* component = cls(kCore, "UnityEngine", "Component");
    if (!why.empty()) {
        delete l;
        return false;
    }
    off(l->feederClass, "_storage", l->offFeederStorage);
    off(l->feederClass, "_model", l->offFeederModel);
    off(plotModel, "remainingFeedOperations", l->offRemainingOps);
    off(ident, "identType", g_offIdentType);
    l->compInChildren = byParams(component, "GetComponentInChildren", {"System.Type", "System.Boolean"});
    // No overloads: by argument count (each is the class's own body, re/hooks.md).
    std::string err;
    l->foodCount = R.Method(l->feederClass, "GetFoodCount", 0, &err);
    l->foodId = R.Method(l->feederClass, "GetFoodId", 0, &err);
    const MethodInfo* feed = R.Method(l->feederClass, "ProcessFeedOperation", 1, &err);
    const MethodInfo* remaining = R.Method(l->feederClass, "RemainingFeedOperationsFastForward", 0, &err);
    const MethodInfo* canSpecific = R.Method(l->storageClass, "CanSpecificSlotAccept", 3, &err);
    const MethodInfo* canAny = R.Method(l->storageClass, "CanAnySlotAccept", 1, &err);
    const MethodInfo* addSpecific = R.Method(l->storageClass, "MaybeAddToSpecificSlot", 3, &err);
    const MethodInfo* addAny = R.Method(l->storageClass, "MaybeAddToAnySlot", 1, &err);
    const MethodInfo* addResource = R.Method(l->storageClass, "MaybeAddAsResource", 4, &err);
    if (why.empty() && (!l->foodCount || !l->foodId || !feed || !remaining))
        why = "SlimeFeeder GetFoodCount / GetFoodId / ProcessFeedOperation / RemainingFeedOperationsFastForward not found";
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
        {"SlimeFeeder.ProcessFeedOperation", feed, reinterpret_cast<void*>(&FeedDetour),
         reinterpret_cast<void**>(&g_origFeed)},
        {"SlimeFeeder.RemainingFeedOperationsFastForward", remaining, reinterpret_cast<void*>(&RemainingDetour),
         reinterpret_cast<void**>(&g_origRemaining)},
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
            // Hooks made so far pass through: no feeder is ever kept without Ready().
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

bool FeederLock::HasFeeder(Il2CppObject* plot) {
    Il2CppObject *feeder = nullptr, *storage = nullptr;
    return ready_ && g_l && FeederOf(plot, feeder, storage);
}

void FeederLock::Tick(const std::vector<KeepRequest>& keep) {
    if (!ready_ || !g_l) return;
    if (keep.empty() && !g_any.load(std::memory_order_acquire) && g_l->applied.empty()) return;
    std::vector<Entry> table;
    std::map<std::string, std::string> applied;
    for (const KeepRequest& r : keep) {
        Il2CppObject *feeder = nullptr, *storage = nullptr;
        if (!FeederOf(r.plot, feeder, storage)) continue;  // no feeder bought (yet): nothing to keep
        Il2CppObject* food = CurrentFood(feeder);
        table.push_back({storage, feeder, food});
        applied[r.id] = food ? unity::GetName(food) : std::string();
        if (sr2::core::ModuleFaulted(kModule)) return;
    }
    for (const auto& [id, food] : applied) {
        auto old = g_l->applied.find(id);
        if (old == g_l->applied.end() || old->second != food)
            SR2_INFO("[{}] auto-feeder on plot '{}': leave last food item in ON, {}", kModule, id,
                     food.empty() ? std::string("empty (takes any food; the first one is kept)")
                                  : std::format("keeps '{}' (drones and shots add only that food)", food));
    }
    for (const auto& [id, food] : g_l->applied)
        if (!applied.count(id)) SR2_INFO("[{}] auto-feeder on plot '{}': leave last food item in OFF", kModule, id);
    g_l->applied = std::move(applied);
    Publish(std::move(table));
}

void FeederLock::SetNow(Il2CppObject* plot, bool on, const std::string& id) {
    if (!ready_ || !g_l) return;
    Il2CppObject *feeder = nullptr, *storage = nullptr;
    if (!FeederOf(plot, feeder, storage)) return;
    std::vector<Entry> table;
    AcquireSRWLockShared(&g_lock);
    for (const Entry& e : g_keep)
        if (e.feeder != feeder) table.push_back(e);
    ReleaseSRWLockShared(&g_lock);
    Il2CppObject* food = on ? CurrentFood(feeder) : nullptr;
    if (on) table.push_back({storage, feeder, food});
    Publish(std::move(table));
    SR2_DEBUG("[{}] auto-feeder on plot '{}': leave last food item in {} (plot menu{})", kModule, id, on ? "on" : "off",
              food ? std::format(", keeps '{}'", unity::GetName(food)) : std::string());
}

void FeederLock::Cleanup(const char* why) {
    const bool had = g_any.load(std::memory_order_acquire);
    Publish({});
    if (g_l) {
        g_l->applied.clear();
        g_l->keptLogged.clear();
    }
    if (had)
        SR2_INFO("[{}] {}: auto-feeders feed out their last food item again (leave last food item in released)",
                 kModule, why);
}

}  // namespace sr2::modules::custom_upgrades::feeder
