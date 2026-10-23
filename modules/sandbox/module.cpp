// sandbox: edit the character's resources from the overlay (Misc > Sandbox):
// Newbucks, health / energy / radiation, the vacpack's ammo slots. Default OFF;
// the overlay only turns it on after a confirmation dialog. Design and the
// game methods used: modules/sandbox/NOTES.md.
//
// Save safety: every change goes through the game's own methods (the same ones
// the game uses for pickups, purchases and vacuuming), with their validation,
// and values are clamped to what a vanilla game can hold. Nothing is written
// into the save directly; the game saves the result the next time it saves.
//
// Threading: no hooks. A main-thread frame callback copies a Snapshot for the
// tab (4 Hz while it is open) and tops up energy while "infinite energy" is on;
// the tab's requests arrive as posted main-thread jobs (ui/sandbox_bridge.h).
// Every game call goes through Resolver::Invoke (runtime_invoke, managed
// exceptions captured); a native fault there marks the module faulted and it
// stops calling into the game.

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <format>
#include <initializer_list>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/detour_guard.h"
#include "core/il2cpp_api.h"
#include "core/log.h"
#include "core/main_thread.h"
#include "core/module.h"
#include "core/unity.h"
#include "ui/sandbox_bridge.h"

namespace sr2::modules {
namespace {

namespace unity = sr2::core::unity;
namespace main_thread = sr2::core::main_thread;
namespace bridge = sr2::ui::sandbox;
using sr2::il2cpp::api;
using sr2::il2cpp::Resolver;

constexpr const char* kModule = "sandbox";
constexpr const char* kImage = "Assembly-CSharp";
constexpr int kNewbuckPersistenceId = 1;  // CurrencyUtility.NEWBUCK_PERSISTENCE_ID
constexpr unsigned long long kSnapshotMs = 250;
constexpr unsigned long long kEnergyMs = 100;
constexpr int kMaxSlots = 32;  // slot mask width

bool IsA(Il2CppObject* obj, Il2CppClass* k) {
    const auto& a = api();
    if (!obj || !k || !a.il2cpp_object_get_class) return false;
    Il2CppClass* c = a.il2cpp_object_get_class(obj);
    if (c == k) return true;
    return c && a.il2cpp_class_is_assignable_from && a.il2cpp_class_is_assignable_from(k, c);
}

// Value of a boxed value type returned by runtime_invoke (data after the object header).
template <class T>
bool Unbox(Il2CppObject* boxed, T& out) {
    if (!boxed) return false;
    out = *reinterpret_cast<T*>(reinterpret_cast<char*>(boxed) + 2 * sizeof(void*));
    return true;
}

std::string Thousands(int64_t v) {
    std::string s = std::to_string(v < 0 ? -v : v);
    for (int i = static_cast<int>(s.size()) - 3; i > 0; i -= 3) s.insert(static_cast<size_t>(i), ",");
    return v < 0 ? "-" + s : s;
}

std::string CleanText(const std::string& s) {
    std::string out;
    bool tag = false;
    for (char c : s) {
        if (c == '<') tag = true;
        else if (c == '>' && tag) tag = false;
        else if (!tag && c != '\r' && c != '\n') out += c;
    }
    const size_t a = out.find_first_not_of(" \t");
    if (a == std::string::npos) return {};
    return out.substr(a, out.find_last_not_of(" \t") - a + 1);
}

class Sandbox final : public sr2::core::Module {
public:
    const char* Name() const override { return kModule; }
    bool DefaultEnabled() const override { return false; }
    bool Setup(sr2::core::ModuleContext& ctx) override;
    void OnEnable() override;
    void OnDisable() override;

    void Tick();                         // main thread, every frame
    void Handle(const bridge::Action&);  // main thread, posted job

private:
    struct Ctx {
        Il2CppObject* player = nullptr;    // PlayerState
        Il2CppObject* model = nullptr;     // PlayerModel
        Il2CppObject* currency = nullptr;  // Newbucks CurrencyDefinition (nullptr if not found)
        Il2CppObject* ammo = nullptr;      // AmmoSlotManager
        void* slots = nullptr;             // AmmoSlot[]
    };
    struct CatalogEntry {
        Il2CppGCHandle handle = nullptr;
        Il2CppObject* obj = nullptr;  // identity; re-read through the handle before use
    };

    bool Live() const { return enabled_.load() && !sr2::core::ModuleFaulted(kModule); }
    Il2CppObject* Call(const MethodInfo* m, void* obj, std::initializer_list<void*> args, const char* what,
                       bool* ok = nullptr);
    template <class T>
    bool CallValue(const MethodInfo* m, void* obj, std::initializer_list<void*> args, const char* what, T& out) {
        bool ok = false;
        Il2CppObject* r = Call(m, obj, args, what, &ok);
        return ok && Unbox(r, out);
    }
    bool Context(Ctx& c, std::string& why);
    Il2CppObject* SlotAt(const Ctx& c, int idx);
    std::string Localized(Il2CppObject* ls);
    std::string ItemName(Il2CppObject* ident);
    void EnsureCatalog(const Ctx& c);
    Il2CppObject* CatalogItem(uintptr_t id, uint32_t* mask);
    void Refresh();
    void TopUpEnergy();
    void Done(bool ok, const std::string& text);

    void DoNewbucks(const Ctx& c, int64_t target, const char* how);
    void DoSlotCount(const Ctx& c, int idx, int64_t value);
    void DoPutItem(const Ctx& c, int idx, uintptr_t item, int64_t value);

    std::atomic<bool> enabled_{false};
    std::atomic<bool> infinite_{false};
    int frameCb_ = 0;
    unsigned long long lastSnap_ = 0, lastEnergy_ = 0;

    // catalog (main thread)
    std::shared_ptr<const std::vector<bridge::Item>> catalog_;
    std::unordered_map<uintptr_t, CatalogEntry> catalogById_;
    std::vector<Il2CppObject*> catalogDefs_;  // slot definitions it was built for (identity only)
    std::unordered_map<uintptr_t, std::string> nameCache_;  // identity -> name (never dereferenced)

    // ---- resolved in Setup ----
    Il2CppClass* playerStateClass_ = nullptr;
    Il2CppClass* playerModelClass_ = nullptr;
    Il2CppClass* currencyClass_ = nullptr;
    Il2CppClass* ammoMgrClass_ = nullptr;
    Il2CppClass* ammoSlotClass_ = nullptr;
    Il2CppClass* slotDefClass_ = nullptr;
    Il2CppClass* identClass_ = nullptr;
    Il2CppClass* vacClass_ = nullptr;
    Il2CppClass* locStringClass_ = nullptr;
    size_t offPlayerModel_ = 0;     // PlayerState._model
    size_t offMaxCurrency_ = 0;     // PlayerModel.maxCurrency
    size_t offPersistenceId_ = 0;   // CurrencyDefinition._persistenceId
    size_t offSlotId_ = 0;          // AmmoSlot._id
    size_t offSlotCount_ = 0;       // AmmoSlot._count
    size_t offSlotDef_ = 0;         // AmmoSlot.Definition
    size_t offIdentPrefab_ = 0;     // IdentifiableType.prefab
    size_t offIdentLocName_ = 0;    // IdentifiableType.localizedName
    size_t offIdentIsPlayer_ = 0;   // IdentifiableType.IsPlayer
    size_t offIdentIsScene_ = 0;    // IdentifiableType.IsSceneObject
    size_t offVacSize_ = 0;         // Vacuumable.Size (VacuumableSize: NORMAL 0, LARGE 1, GIANT 2)
    const MethodInfo* tryGetPlayerState_ = nullptr;  // SceneContextUtility.TryGetPlayerState(out PlayerState)
    const MethodInfo* defaultCurrency_ = nullptr;    // CurrencyUtility.get_DefaultCurrency
    const MethodInfo* getCurrency_ = nullptr;        // PlayerState.GetCurrency(ICurrency)
    const MethodInfo* addCurrency_ = nullptr;        // PlayerState.AddCurrency(ICurrency, int, bool, IUIDisplayData)
    const MethodInfo* spendCurrency_ = nullptr;      // PlayerState.SpendCurrency(ICurrency, int, IUIDisplayData)
    const MethodInfo* getHealth_ = nullptr;          // PlayerState.GetCurrHealth
    const MethodInfo* getMaxHealth_ = nullptr;       // PlayerState.GetMaxHealth
    const MethodInfo* setHealth_ = nullptr;          // PlayerState.SetHealth(int)
    const MethodInfo* getEnergy_ = nullptr;          // PlayerState.GetCurrEnergy
    const MethodInfo* getMaxEnergy_ = nullptr;       // PlayerState.GetMaxEnergy
    const MethodInfo* setEnergy_ = nullptr;          // PlayerState.SetEnergy(int)
    const MethodInfo* getRad_ = nullptr;             // PlayerState.GetCurrRad
    const MethodInfo* setRad_ = nullptr;             // PlayerState.SetRad(int)
    const MethodInfo* getAmmo_ = nullptr;            // PlayerState.get_Ammo
    const MethodInfo* getSlots_ = nullptr;           // AmmoSlotManager.get_Slots
    const MethodInfo* slotMax_ = nullptr;            // AmmoSlotManager.GetSlotMaxCount(int)
    const MethodInfo* addResource_ = nullptr;        // AmmoSlotManager.MaybeAddResource(IdentifiableType, int, int, bool)
    const MethodInfo* decrement_ = nullptr;          // AmmoSlotManager.Decrement(int, int)
    const MethodInfo* slotUnlocked_ = nullptr;       // AmmoSlot.get_IsUnlocked
    const MethodInfo* isAllowed_ = nullptr;          // AmmoSlotDefinition.IsAllowed(IdentifiableType)
    const MethodInfo* locGet_ = nullptr;             // LocalizedString.GetLocalizedString()
    const MethodInfo* locIsEmpty_ = nullptr;         // LocalizedReference.get_IsEmpty (optional)
};

Sandbox* g_self = nullptr;

void HandleThunk(const bridge::Action& a) {
    if (g_self) g_self->Handle(a);
}

// ------------------------------------------------------------------ setup ----
bool Sandbox::Setup(sr2::core::ModuleContext& ctx) {
    auto& R = ctx.resolver();
    std::string why;
    auto note = [&](const std::string& s) {
        if (why.empty()) why = s;
    };
    auto cls = [&](const char* image, const char* ns, const char* name) -> Il2CppClass* {
        std::string err;
        Il2CppClass* k = R.Class(image, ns, name, &err);
        if (!k) note(err.empty() ? std::format("class {}.{} not found", ns, name) : err);
        return k;
    };
    auto off = [&](Il2CppClass* k, const char* field, size_t& out) {
        if (!k) return;
        std::string err;
        auto o = R.FieldOffset(k, field, &err);
        if (o) out = *o;
        else note(err.empty() ? std::format("field {} not found", field) : err);
    };
    // Searches the class and its parents (inherited / interface-implementing methods).
    auto method = [&](Il2CppClass* k, const char* name, int argc, bool required = true) -> const MethodInfo* {
        std::string err;
        for (Il2CppClass* c = k; c; c = api().il2cpp_class_get_parent ? api().il2cpp_class_get_parent(c) : nullptr)
            if (const MethodInfo* m = R.Method(c, name, argc, &err)) return m;
        if (required && k) note(std::format("method {}({}) not found", name, argc));
        return nullptr;
    };

    playerStateClass_ = cls(kImage, "", "PlayerState");
    playerModelClass_ = cls(kImage, "MonomiPark.SlimeRancher.DataModel", "PlayerModel");
    currencyClass_ = cls(kImage, "MonomiPark.SlimeRancher.Economy", "CurrencyDefinition");
    Il2CppClass* currencyUtil = cls(kImage, "MonomiPark.SlimeRancher.Economy", "CurrencyUtility");
    Il2CppClass* scu = cls(kImage, "MonomiPark.SlimeRancher.Util", "SceneContextUtility");
    ammoMgrClass_ = cls(kImage, "MonomiPark.SlimeRancher.Player", "AmmoSlotManager");
    ammoSlotClass_ = cls(kImage, "MonomiPark.SlimeRancher.Player", "AmmoSlot");
    slotDefClass_ = cls(kImage, "", "AmmoSlotDefinition");
    identClass_ = cls(kImage, "", "IdentifiableType");
    vacClass_ = cls(kImage, "MonomiPark.SlimeRancher", "Vacuumable");
    locStringClass_ = cls("Unity.Localization", "UnityEngine.Localization", "LocalizedString");
    if (!why.empty()) return ctx.Fail(why);

    off(playerStateClass_, "_model", offPlayerModel_);
    off(playerModelClass_, "maxCurrency", offMaxCurrency_);
    off(currencyClass_, "_persistenceId", offPersistenceId_);
    off(ammoSlotClass_, "_id", offSlotId_);
    off(ammoSlotClass_, "_count", offSlotCount_);
    off(ammoSlotClass_, "Definition", offSlotDef_);
    off(identClass_, "prefab", offIdentPrefab_);
    off(identClass_, "localizedName", offIdentLocName_);
    off(identClass_, "IsPlayer", offIdentIsPlayer_);
    off(identClass_, "IsSceneObject", offIdentIsScene_);
    off(vacClass_, "Size", offVacSize_);
    if (!why.empty()) return ctx.Fail(why);

    tryGetPlayerState_ = method(scu, "TryGetPlayerState", 1);
    defaultCurrency_ = method(currencyUtil, "get_DefaultCurrency", 0);
    getCurrency_ = method(playerStateClass_, "GetCurrency", 1);
    addCurrency_ = method(playerStateClass_, "AddCurrency", 4);
    spendCurrency_ = method(playerStateClass_, "SpendCurrency", 3);
    getHealth_ = method(playerStateClass_, "GetCurrHealth", 0);
    getMaxHealth_ = method(playerStateClass_, "GetMaxHealth", 0);
    setHealth_ = method(playerStateClass_, "SetHealth", 1);
    getEnergy_ = method(playerStateClass_, "GetCurrEnergy", 0);
    getMaxEnergy_ = method(playerStateClass_, "GetMaxEnergy", 0);
    setEnergy_ = method(playerStateClass_, "SetEnergy", 1);
    getRad_ = method(playerStateClass_, "GetCurrRad", 0);
    setRad_ = method(playerStateClass_, "SetRad", 1);
    getAmmo_ = method(playerStateClass_, "get_Ammo", 0);
    getSlots_ = method(ammoMgrClass_, "get_Slots", 0);
    slotMax_ = method(ammoMgrClass_, "GetSlotMaxCount", 1);
    addResource_ = method(ammoMgrClass_, "MaybeAddResource", 4);
    {
        std::string err;  // two Decrement overloads take 2 arguments: (int, int) and (IdentifiableType, int)
        decrement_ = R.MethodByParams(ammoMgrClass_, "Decrement", {"System.Int32", "System.Int32"}, &err);
        if (!decrement_) note("Decrement(int, int): " + err);
    }
    slotUnlocked_ = method(ammoSlotClass_, "get_IsUnlocked", 0);
    isAllowed_ = method(slotDefClass_, "IsAllowed", 1);
    locGet_ = method(locStringClass_, "GetLocalizedString", 0);
    locIsEmpty_ = method(locStringClass_, "get_IsEmpty", 0, false);
    if (!why.empty()) return ctx.Fail(why);

    g_self = this;
    if (!frameCb_) frameCb_ = main_thread::AddFrameCallback(kModule, [this](float) { Tick(); });
    if (!frameCb_) return ctx.Fail("could not add the main-thread frame callback");
    SR2_INFO("[{}] setup ok: PlayerState currency/health/energy methods, AmmoSlotManager.MaybeAddResource / "
             "Decrement, AmmoSlotDefinition.IsAllowed resolved; offsets PlayerState._model 0x{:X}, "
             "PlayerModel.maxCurrency 0x{:X}, AmmoSlot._id 0x{:X} _count 0x{:X} Definition 0x{:X}",
             kModule, offPlayerModel_, offMaxCurrency_, offSlotId_, offSlotCount_, offSlotDef_);
    return true;
}

void Sandbox::OnEnable() {
    enabled_.store(true);
    bridge::SetHandler(&HandleThunk);
    lastSnap_ = 0;
    SR2_INFO("[{}] enabled: edits from the overlay's Sandbox tab now change the loaded game", kModule);
}

void Sandbox::OnDisable() {
    enabled_.store(false);
    if (infinite_.exchange(false)) SR2_INFO("[{}] infinite energy off (module disabled)", kModule);
    bridge::SetHandler(nullptr);
    bridge::Snapshot s;
    s.tick = GetTickCount64();
    s.why = "Sandbox is off.";
    bridge::Publish(std::move(s));
    SR2_INFO("[{}] disabled", kModule);
}

// ---------------------------------------------------------------- helpers ----
Il2CppObject* Sandbox::Call(const MethodInfo* m, void* obj, std::initializer_list<void*> args, const char* what,
                            bool* ok) {
    if (ok) *ok = false;
    if (!m || sr2::core::ModuleFaulted(kModule)) return nullptr;
    std::vector<void*> params(args);
    std::string err;
    Il2CppObject* r = Resolver::Get().Invoke(m, obj, params.empty() ? nullptr : params.data(), &err);
    if (!err.empty()) {
        if (err.rfind("native exception", 0) == 0) sr2::core::MarkModuleFaulted(kModule, std::format("{}: {}", what, err));
        else SR2_WARN("[{}] {} failed: {}", kModule, what, err);
        return nullptr;
    }
    if (ok) *ok = true;
    return r;
}

bool Sandbox::Context(Ctx& c, std::string& why) {
    Il2CppObject* player = nullptr;
    bool found = false;
    if (!CallValue(tryGetPlayerState_, nullptr, {&player}, "SceneContextUtility.TryGetPlayerState", found) || !found ||
        !player) {
        why = "Load a save to use the sandbox.";
        return false;
    }
    if (!IsA(player, playerStateClass_) || !unity::IsAlive(player)) {
        why = "The player is not available right now.";
        return false;
    }
    Il2CppObject* model = unity::Field<Il2CppObject*>(player, offPlayerModel_);
    if (!IsA(model, playerModelClass_)) {
        why = "The player has no data model yet.";
        return false;
    }
    c.player = player;
    c.model = model;

    bool ok = false;
    Il2CppObject* cur = Call(defaultCurrency_, nullptr, {}, "CurrencyUtility.DefaultCurrency", &ok);
    // Newbucks only: a CurrencyDefinition asset with the Newbucks persistence id.
    if (ok && IsA(cur, currencyClass_) && unity::IsAlive(cur) &&
        unity::Field<int32_t>(cur, offPersistenceId_) == kNewbuckPersistenceId)
        c.currency = cur;

    Il2CppObject* ammo = Call(getAmmo_, player, {}, "PlayerState.Ammo", &ok);
    if (ok && IsA(ammo, ammoMgrClass_)) {
        Il2CppObject* arr = Call(getSlots_, ammo, {}, "AmmoSlotManager.Slots", &ok);
        if (ok && arr) {
            c.ammo = ammo;
            c.slots = arr;
        }
    }
    return true;
}

Il2CppObject* Sandbox::SlotAt(const Ctx& c, int idx) {
    if (!c.slots || idx < 0 || static_cast<size_t>(idx) >= unity::ArrayLength(c.slots)) return nullptr;
    auto* s = static_cast<Il2CppObject*>(unity::ArrayAt(c.slots, static_cast<size_t>(idx)));
    return IsA(s, ammoSlotClass_) ? s : nullptr;
}

std::string Sandbox::Localized(Il2CppObject* ls) {
    if (!ls || !IsA(ls, locStringClass_)) return {};
    bool empty = false;
    if (locIsEmpty_ && CallValue(locIsEmpty_, ls, {}, "LocalizedString.IsEmpty", empty) && empty) return {};
    Il2CppObject* r = Call(locGet_, ls, {}, "LocalizedString.GetLocalizedString");
    return r ? CleanText(unity::ToUtf8(reinterpret_cast<Il2CppString*>(r))) : std::string();
}

std::string Sandbox::ItemName(Il2CppObject* ident) {
    if (!ident) return {};
    const auto key = reinterpret_cast<uintptr_t>(ident);
    if (auto it = nameCache_.find(key); it != nameCache_.end()) return it->second;
    if (!IsA(ident, identClass_) || !unity::IsAlive(ident)) return "unknown item";
    std::string n = Localized(unity::Field<Il2CppObject*>(ident, offIdentLocName_));
    if (n.empty()) n = unity::GetName(ident);
    if (n.empty()) n = "unknown item";
    nameCache_[key] = n;
    return n;
}

// Items a vanilla vacpack can hold: IdentifiableType assets whose prefab has a
// normal-size Vacuumable (largos and other LARGE / GIANT ones are only held,
// never stored), excluding the player and scene objects, and that at least
// one slot's AmmoSlotDefinition.IsAllowed accepts. Rebuilt only when the slot
// definitions change; the entries are kept alive with GC handles.
void Sandbox::EnsureCatalog(const Ctx& c) {
    std::vector<Il2CppObject*> defs;
    const size_t n = std::min<size_t>(c.slots ? unity::ArrayLength(c.slots) : 0, kMaxSlots);
    for (size_t i = 0; i < n; ++i) {
        Il2CppObject* s = SlotAt(c, static_cast<int>(i));
        Il2CppObject* d = s ? unity::Field<Il2CppObject*>(s, offSlotDef_) : nullptr;
        defs.push_back(IsA(d, slotDefClass_) && unity::IsAlive(d) ? d : nullptr);
    }
    if (catalog_ && defs == catalogDefs_) return;

    const unsigned long long t0 = GetTickCount64();
    std::vector<Il2CppGCHandle> old;
    for (auto& [id, e] : catalogById_) old.push_back(e.handle);
    catalogById_.clear();
    if (!old.empty())
        main_thread::Post([old] {
            for (Il2CppGCHandle h : old) unity::FreeGCHandle(h);
        });

    auto items = std::make_shared<std::vector<bridge::Item>>();
    size_t scanned = 0;
    for (Il2CppObject* o : unity::FindObjectsOfTypeAll(identClass_)) {
        if (sr2::core::ModuleFaulted(kModule)) break;
        if (!IsA(o, identClass_) || !unity::IsAlive(o)) continue;
        ++scanned;
        if (unity::Field<bool>(o, offIdentIsPlayer_) || unity::Field<bool>(o, offIdentIsScene_)) continue;
        Il2CppObject* prefab = unity::Field<Il2CppObject*>(o, offIdentPrefab_);
        if (!prefab || !unity::IsAlive(prefab)) continue;
        Il2CppObject* vac = unity::GetComponent(prefab, vacClass_);
        if (!vac || !IsA(vac, vacClass_) || unity::Field<int32_t>(vac, offVacSize_) != 0) continue;
        uint32_t mask = 0;
        for (size_t i = 0; i < defs.size(); ++i) {
            bool allowed = false;
            if (defs[i] && CallValue(isAllowed_, defs[i], {o}, "AmmoSlotDefinition.IsAllowed", allowed) && allowed)
                mask |= 1u << i;
        }
        if (!mask) continue;
        Il2CppGCHandle h = unity::NewGCHandle(o);
        if (!h) continue;
        const auto id = reinterpret_cast<uintptr_t>(o);
        catalogById_[id] = CatalogEntry{h, o};
        items->push_back(bridge::Item{id, ItemName(o), mask});
    }
    // Same display name twice (variants): add the asset name.
    std::map<std::string, int> seen;
    for (const auto& it : *items) ++seen[it.name];
    for (auto& it : *items)
        if (seen[it.name] > 1)
            if (Il2CppObject* o = CatalogItem(it.id, nullptr)) it.name += " (" + unity::GetName(o) + ")";
    std::sort(items->begin(), items->end(), [](const bridge::Item& a, const bridge::Item& b) { return a.name < b.name; });
    catalog_ = std::move(items);
    catalogDefs_ = std::move(defs);
    SR2_INFO("[{}] vacpack item list: {} of {} identifiable types fit {} slot(s) ({} ms)", kModule, catalog_->size(),
             scanned, catalogDefs_.size(), GetTickCount64() - t0);
}

Il2CppObject* Sandbox::CatalogItem(uintptr_t id, uint32_t* mask) {
    auto it = catalogById_.find(id);
    if (it == catalogById_.end()) return nullptr;
    Il2CppObject* o = unity::GCHandleTarget(it->second.handle);
    if (o != it->second.obj || !IsA(o, identClass_) || !unity::IsAlive(o)) return nullptr;
    if (mask) {
        *mask = 0;
        if (catalog_)
            for (const auto& i : *catalog_)
                if (i.id == id) *mask = i.slotMask;
    }
    return o;
}

// ---------------------------------------------------------------- refresh ----
void Sandbox::Refresh() {
    bridge::Snapshot s;
    s.tick = GetTickCount64();
    s.infiniteEnergy = infinite_.load();
    Ctx c;
    if (!Live()) {
        s.why = "Sandbox is off.";
    } else if (!Context(c, s.why)) {
        // s.why set
    } else {
        s.inGame = true;
        if (c.currency) {
            s.haveNewbucks = CallValue(getCurrency_, c.player, {c.currency}, "PlayerState.GetCurrency", s.newbucks);
            s.maxNewbucks = unity::Field<int32_t>(c.model, offMaxCurrency_);
        }
        CallValue(getHealth_, c.player, {}, "PlayerState.GetCurrHealth", s.health);
        CallValue(getMaxHealth_, c.player, {}, "PlayerState.GetMaxHealth", s.maxHealth);
        CallValue(getEnergy_, c.player, {}, "PlayerState.GetCurrEnergy", s.energy);
        CallValue(getMaxEnergy_, c.player, {}, "PlayerState.GetMaxEnergy", s.maxEnergy);
        CallValue(getRad_, c.player, {}, "PlayerState.GetCurrRad", s.rads);
        const size_t n = std::min<size_t>(c.slots ? unity::ArrayLength(c.slots) : 0, kMaxSlots);
        for (size_t i = 0; i < n; ++i) {
            bridge::Slot slot;
            int idx = static_cast<int>(i);
            if (Il2CppObject* a = SlotAt(c, idx)) {
                CallValue(slotUnlocked_, a, {}, "AmmoSlot.IsUnlocked", slot.unlocked);
                CallValue(slotMax_, c.ammo, {&idx}, "AmmoSlotManager.GetSlotMaxCount", slot.max);
                slot.count = unity::Field<int32_t>(a, offSlotCount_);
                Il2CppObject* id = unity::Field<Il2CppObject*>(a, offSlotId_);
                if (id && slot.count > 0) {
                    slot.item = reinterpret_cast<uintptr_t>(id);
                    slot.name = ItemName(id);
                } else {
                    slot.count = 0;
                }
            }
            s.slots.push_back(std::move(slot));
        }
        if (c.slots) EnsureCatalog(c);
        s.catalog = catalog_;
    }
    bridge::Publish(std::move(s));
}

void Sandbox::TopUpEnergy() {
    Ctx c;
    std::string why;
    if (!Context(c, why)) return;
    int cur = 0, max = 0;
    if (!CallValue(getEnergy_, c.player, {}, "PlayerState.GetCurrEnergy", cur) ||
        !CallValue(getMaxEnergy_, c.player, {}, "PlayerState.GetMaxEnergy", max))
        return;
    if (max > 0 && cur < max) Call(setEnergy_, c.player, {&max}, "PlayerState.SetEnergy");
}

void Sandbox::Tick() {
    if (!Live()) return;
    const unsigned long long now = GetTickCount64();
    if (infinite_.load() && now - lastEnergy_ >= kEnergyMs) {
        lastEnergy_ = now;
        TopUpEnergy();
    }
    if (bridge::WantsSnapshot() && now - lastSnap_ >= kSnapshotMs) {
        lastSnap_ = now;
        Refresh();
    }
}

void Sandbox::Done(bool ok, const std::string& text) {
    if (ok) SR2_INFO("[{}] {}", kModule, text);
    else SR2_WARN("[{}] {}", kModule, text);
    bridge::SetStatus(text, !ok);
}

// ---------------------------------------------------------------- actions ----
void Sandbox::Handle(const bridge::Action& a) {
    if (!Live()) {
        bridge::SetStatus("Sandbox is off.", true);
        return;
    }
    if (a.op == bridge::Op::InfiniteEnergy) {
        const bool on = a.value != 0;
        if (infinite_.exchange(on) != on)
            Done(true, on ? "Infinite energy on (energy is refilled to its maximum; not saved, off after a restart)"
                          : "Infinite energy off");
        Refresh();
        return;
    }
    Ctx c;
    std::string why;
    if (!Context(c, why)) {
        Done(false, "Not applied: " + why);
        Refresh();
        return;
    }
    switch (a.op) {
        case bridge::Op::SetNewbucks: DoNewbucks(c, a.value, "set"); break;
        case bridge::Op::AddNewbucks: {
            int cur = 0;
            if (c.currency && CallValue(getCurrency_, c.player, {c.currency}, "PlayerState.GetCurrency", cur))
                DoNewbucks(c, static_cast<int64_t>(cur) + a.value, a.value >= 0 ? "add" : "subtract");
            else
                Done(false, "Newbucks not changed: the Newbucks currency was not found");
            break;
        }
        case bridge::Op::RefillHealth:
        case bridge::Op::RefillEnergy: {
            const bool health = a.op == bridge::Op::RefillHealth;
            int cur = 0, max = 0;
            const char* what = health ? "Health" : "Energy";
            if (!CallValue(health ? getHealth_ : getEnergy_, c.player, {}, "PlayerState.GetCurr", cur) ||
                !CallValue(health ? getMaxHealth_ : getMaxEnergy_, c.player, {}, "PlayerState.GetMax", max) || max <= 0) {
                Done(false, std::format("{} not refilled: could not read it", what));
                break;
            }
            bool ok = false;
            Call(health ? setHealth_ : setEnergy_, c.player, {&max}, health ? "PlayerState.SetHealth" : "PlayerState.SetEnergy",
                 &ok);
            Done(ok, ok ? std::format("{} refilled: {} -> {}", what, cur, max) : std::format("{} not refilled", what));
            break;
        }
        case bridge::Op::ClearRads: {
            int cur = 0, zero = 0;
            CallValue(getRad_, c.player, {}, "PlayerState.GetCurrRad", cur);
            bool ok = false;
            Call(setRad_, c.player, {&zero}, "PlayerState.SetRad", &ok);
            Done(ok, ok ? std::format("Radiation cleared: {} -> 0", cur) : "Radiation not cleared");
            break;
        }
        case bridge::Op::SetSlotCount: DoSlotCount(c, a.slot, a.value); break;
        case bridge::Op::PutItem: DoPutItem(c, a.slot, a.item, a.value); break;
        case bridge::Op::InfiniteEnergy: break;  // handled above
    }
    Refresh();
}

// Newbucks to `target`, clamped to [0, PlayerModel.maxCurrency]: more through
// PlayerState.AddCurrency (as a pickup or sale does; it also counts towards
// "Newbucks ever collected"), less through PlayerState.SpendCurrency (as a
// purchase does). Both show the game's own coin popup.
void Sandbox::DoNewbucks(const Ctx& c, int64_t target, const char* how) {
    if (!c.currency) {
        Done(false, "Newbucks not changed: the Newbucks currency was not found");
        return;
    }
    int cur = 0;
    if (!CallValue(getCurrency_, c.player, {c.currency}, "PlayerState.GetCurrency", cur)) {
        Done(false, "Newbucks not changed: could not read the current amount");
        return;
    }
    const int max = unity::Field<int32_t>(c.model, offMaxCurrency_);
    if (max <= 0) {
        Done(false, "Newbucks not changed: the maximum is unknown");
        return;
    }
    const int64_t clamped = std::clamp<int64_t>(target, 0, max);
    const int delta = static_cast<int>(clamped - cur);
    if (delta == 0) {
        Done(true, std::format("Newbucks unchanged ({}{})", Thousands(cur),
                               clamped != target ? std::format(", limit 0..{}", Thousands(max)) : std::string()));
        return;
    }
    bool ok = false;
    if (delta > 0) {
        int adjust = delta;
        bool popup = true;
        Call(addCurrency_, c.player, {c.currency, &adjust, &popup, nullptr}, "PlayerState.AddCurrency", &ok);
    } else {
        int adjust = -delta;
        Call(spendCurrency_, c.player, {c.currency, &adjust, nullptr}, "PlayerState.SpendCurrency", &ok);
    }
    int now = cur;
    CallValue(getCurrency_, c.player, {c.currency}, "PlayerState.GetCurrency", now);
    Done(ok, ok ? std::format("Newbucks {}: {} -> {}{}", how, Thousands(cur), Thousands(now),
                              clamped != target ? std::format(" (clamped to 0..{})", Thousands(max)) : std::string())
                : std::format("Newbucks not changed ({} failed)", delta > 0 ? "AddCurrency" : "SpendCurrency"));
}

// Count of what a slot holds, clamped to [0, GetSlotMaxCount]: up through
// AmmoSlotManager.MaybeAddResource (no overflow: it refuses more than the
// slot's maximum and anything the slot does not accept), down through
// AmmoSlotManager.Decrement (which clears the slot at 0).
void Sandbox::DoSlotCount(const Ctx& c, int idx, int64_t value) {
    Il2CppObject* slot = SlotAt(c, idx);
    if (!slot || !c.ammo) {
        Done(false, std::format("Slot {} not changed: no such slot", idx + 1));
        return;
    }
    bool unlocked = false;
    if (!CallValue(slotUnlocked_, slot, {}, "AmmoSlot.IsUnlocked", unlocked) || !unlocked) {
        Done(false, std::format("Slot {} not changed: the slot is locked", idx + 1));
        return;
    }
    Il2CppObject* id = unity::Field<Il2CppObject*>(slot, offSlotId_);
    const int cur = unity::Field<int32_t>(slot, offSlotCount_);
    if (!id || cur <= 0) {
        Done(false, std::format("Slot {} is empty: pick an item to put in it", idx + 1));
        return;
    }
    int max = 0;
    if (!CallValue(slotMax_, c.ammo, {&idx}, "AmmoSlotManager.GetSlotMaxCount", max) || max <= 0) {
        Done(false, std::format("Slot {} not changed: its maximum is unknown", idx + 1));
        return;
    }
    const int target = static_cast<int>(std::clamp<int64_t>(value, 0, max));
    const std::string name = ItemName(id);
    if (target == cur) {
        Done(true, std::format("Slot {} unchanged: {} x {}", idx + 1, cur, name));
        return;
    }
    bool ok = false;
    if (target > cur) {
        int add = target - cur;
        bool overflow = false, added = false;
        ok = CallValue(addResource_, c.ammo, {id, &idx, &add, &overflow}, "AmmoSlotManager.MaybeAddResource", added) &&
             added;
    } else {
        int sub = cur - target;
        Call(decrement_, c.ammo, {&idx, &sub}, "AmmoSlotManager.Decrement", &ok);
    }
    const int now = unity::Field<int32_t>(slot, offSlotCount_);
    Done(ok, ok ? std::format("Slot {}: {} {} -> {}", idx + 1, name, cur, target == 0 ? 0 : now)
                : std::format("Slot {} not changed: the game refused {} x {}", idx + 1, target, name));
}

// Replaces a slot's contents with `value` of a catalog item: empties it first
// if it holds something else (Decrement), then MaybeAddResource, which checks
// AmmoSlotDefinition.IsAllowed and the slot's maximum itself.
void Sandbox::DoPutItem(const Ctx& c, int idx, uintptr_t item, int64_t value) {
    uint32_t mask = 0;
    Il2CppObject* ident = CatalogItem(item, &mask);
    if (!ident) {
        Done(false, "Item not put: it is not in the vacpack item list (anymore)");
        return;
    }
    const std::string name = ItemName(ident);
    Il2CppObject* slot = SlotAt(c, idx);
    if (!slot || !c.ammo || idx >= kMaxSlots) {
        Done(false, std::format("{} not put: no slot {}", name, idx + 1));
        return;
    }
    if (!(mask & (1u << idx))) {
        Done(false, std::format("{} not put: slot {} does not accept it", name, idx + 1));
        return;
    }
    bool unlocked = false;
    if (!CallValue(slotUnlocked_, slot, {}, "AmmoSlot.IsUnlocked", unlocked) || !unlocked) {
        Done(false, std::format("{} not put: slot {} is locked", name, idx + 1));
        return;
    }
    int max = 0;
    if (!CallValue(slotMax_, c.ammo, {&idx}, "AmmoSlotManager.GetSlotMaxCount", max) || max <= 0) {
        Done(false, std::format("{} not put: the slot's maximum is unknown", name));
        return;
    }
    const int target = static_cast<int>(std::clamp<int64_t>(value, 1, max));
    Il2CppObject* curId = unity::Field<Il2CppObject*>(slot, offSlotId_);
    int cur = unity::Field<int32_t>(slot, offSlotCount_);
    std::string replaced;
    if (cur > 0 && curId != ident) {
        replaced = std::format(" (replaced {} x {})", cur, ItemName(curId));
        bool ok = false;
        int sub = cur;
        Call(decrement_, c.ammo, {&idx, &sub}, "AmmoSlotManager.Decrement", &ok);
        cur = unity::Field<int32_t>(slot, offSlotCount_);
        if (!ok || cur > 0) {
            Done(false, std::format("{} not put: slot {} could not be emptied", name, idx + 1));
            return;
        }
    }
    if (cur > 0) {  // same item already there: just change the count
        DoSlotCount(c, idx, target);
        return;
    }
    int add = target;
    bool overflow = false, added = false;
    const bool ok =
        CallValue(addResource_, c.ammo, {ident, &idx, &add, &overflow}, "AmmoSlotManager.MaybeAddResource", added) && added;
    Done(ok, ok ? std::format("Slot {}: {} x {}{}", idx + 1, target, name, replaced)
                : std::format("{} not put: the game refused it for slot {}{}", name, idx + 1, replaced));
}

}  // namespace

std::unique_ptr<sr2::core::Module> CreateSandbox() { return std::make_unique<Sandbox>(); }

}  // namespace sr2::modules
