// sandbox: the upgrades manager (see upgrades.h and NOTES.md, "Upgrades manager"). SR2 1.3.0.
//
// - Levels: UpgradeModel.SetUpgradeLevel(def, level) (0xAD9C20, decompiled): accepts -1 .. LevelCount - 1,
//   writes the level, raises the TinkerTailorSlimes progress (blocked while Sandbox is on) and
//   OnUpgradeChanged -> ActorUpgradeHandler.OnUpgradeChanged -> ApplyUpgrade(def, level) (0x8646B0), which
//   replaces the upgrade's modifiers with _levels[level].Effects, or with none at -1. Levels replace each
//   other, so a lower level really takes the effect back. custom_upgrades' tiers past the vanilla max are
//   levels of the same definition and its SetUpgradeLevel detour keeps its sidecar in step (also when
//   lowering), so they need nothing here.
// - Discover: one of each level-0 upgrade component the player lacks, AcquisitionUtility.
//   AcquireUpgradeComponent(component, 1) (the treasure pod path; see module.cpp DoDiscoverUpgrades).
//   Undiscover: back to level -1, then UpgradeComponentsModel.LoseComponent(component, held) for level 0's
//   components (SpendableDirector._upgradeComponentsModel; LoseComponent 0xA55D40 removes the entry at 0
//   and raises OwnedComponentsChanged). Resetting a discoverable upgrade to "not bought" gives level 0's
//   components back when they are missing, so it stays discovered (Undiscover is the way to hide it).
// - Blueprints: GadgetDirector.AddBlueprint(def, showPediaUnlockNotif: false) (0x867910: GadgetsModel.
//   blueprints.Add, pedia unlock, BlueprintObtained event; the game's pickup path). The game has no
//   RemoveBlueprint: locking removes the gadget from GadgetsModel.blueprints (the HashSet HasBlueprint reads
//   and the save stores, GadgetsModel.Pull) with HashSet.Remove. Its pedia entry stays unlocked.
// - The open Fabricator is rebuilt after each change (RebuildFabricator below).
//
// Every game call is Resolver::Invoke on the main thread; a native fault marks the module faulted, as in
// module.cpp.

#include "modules/sandbox/upgrades.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <format>
#include <initializer_list>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/detour_guard.h"
#include "core/il2cpp_api.h"
#include "core/log.h"
#include "core/module.h"
#include "core/text_override.h"
#include "core/unity.h"
#include "modules/custom_upgrades/api.h"
#include "modules/custom_upgrades/buildables.h"
#include "modules/sandbox/prefab_icons.h"
#include "ui/item_icons.h"
#include "ui/sandbox_bridge.h"
#include "ui/sandbox_upgrades_bridge.h"

namespace sr2::modules::sandbox {
namespace {

namespace unity = sr2::core::unity;
namespace ub = sr2::ui::sandbox::upgrades;
using sr2::il2cpp::api;
using sr2::il2cpp::Resolver;

constexpr const char* kModule = "sandbox";
constexpr const char* kImage = "Assembly-CSharp";
constexpr unsigned long long kPublishMs = 500;  // while the page is open
constexpr int32_t kSlotUpgradeId = 40;          // "AmmoSlots" (Extra Tank): the vacpack slot count
constexpr int32_t kCauseProgrammatic = 0;       // InteractionCause.PROGRAMMATIC

// Vanilla level counts of the upgrades custom_upgrades extends with more tiers (modules/custom_upgrades/
// NOTES.md, "Vacpack upgrades"): Tank Booster I-VIII (+ IX-XII), Extra Tank I-II (+ III-IV).
int VanillaLevels(int32_t uniqueId) {
    switch (uniqueId) {
        case 20: return 8;  // AmmoCapacity
        case 40: return 2;  // AmmoSlots
        default: return 0;
    }
}

std::atomic<bool> g_live{false};
UpgradesHost g_host;
unsigned long long g_lastPublish = 0;

// ---- resolved in SetupUpgrades ----
std::string g_why;  // "" = the upgrade list and levels are available
Il2CppClass* g_playerStateClass = nullptr;
Il2CppClass* g_playerModelClass = nullptr;
Il2CppClass* g_upgradeDefClass = nullptr;
const MethodInfo* g_tryGetPlayerState = nullptr;  // SceneContextUtility.TryGetPlayerState(out PlayerState)
const MethodInfo* g_getUpgradeModel = nullptr;    // PlayerModel.GetUpgradeModel
const MethodInfo* g_getLevel = nullptr;           // UpgradeModel.GetUpgradeLevel(UpgradeDefinition)
const MethodInfo* g_setLevel = nullptr;           // UpgradeModel.SetUpgradeLevel(UpgradeDefinition, int)
const MethodInfo* g_locGet = nullptr;             // LocalizedString.GetLocalizedString
Il2CppClass* g_handlerClass = nullptr;            // ActorUpgradeHandler (optional: the effects applied again)
const MethodInfo* g_applyUpgrade = nullptr;       // ActorUpgradeHandler.ApplyUpgrade(UpgradeDefinition, int) (private)
size_t g_offUmParticipants = 0;                   // UpgradeModel.participants (List<UpgradeModel.Participant>)
size_t g_offPlayerModel = 0;                      // PlayerState._model
size_t g_offUmDefs = 0;                           // UpgradeModel.upgradeDefinitions (UpgradeDefinitionList)
size_t g_offUdIcon = 0, g_offUdRequire = 0, g_offUdId = 0, g_offUdLevels = 0;  // UpgradeDefinition fields
size_t g_offCostComponents = 0;                   // PurchaseCost.ComponentCosts, inside the struct (no header)
size_t g_offUlCosts = 0, g_offUlTitle = 0;        // UpgradeLevelDefinition._costs / Title (nested class: from a live object)

std::string g_discoverWhy;                         // "" = discover / undiscover available
const MethodInfo* g_tryGetSpendable = nullptr;     // SceneContextUtility.TryGetSpendableDirector(out SpendableDirector)
const MethodInfo* g_componentAmount = nullptr;     // SpendableDirector.GetUpgradeComponentAmount(UpgradeComponent)
const MethodInfo* g_acquireComponent = nullptr;    // static AcquisitionUtility.AcquireUpgradeComponent(component, int)
const MethodInfo* g_loseComponent = nullptr;       // UpgradeComponentsModel.LoseComponent(component, int)
size_t g_offSpendComponents = 0;                   // SpendableDirector._upgradeComponentsModel

std::string g_blueprintWhy;                        // "" = blueprints available
Il2CppClass* g_gadgetDefClass = nullptr;
const MethodInfo* g_tryGetGadgetDir = nullptr;     // SceneContextUtility.TryGetGadgetDirector(out GadgetDirector)
const MethodInfo* g_hasBlueprint = nullptr;        // GadgetDirector.HasBlueprint(GadgetDefinition)
const MethodInfo* g_addBlueprint = nullptr;        // GadgetDirector.AddBlueprint(GadgetDefinition, bool)
size_t g_offGdModel = 0;                           // GadgetDirector._model (GadgetsModel)
size_t g_offGmBlueprints = 0;                      // GadgetsModel.blueprints (HashSet<GadgetDefinition>)
const MethodInfo* g_setRemove = nullptr;           // HashSet<GadgetDefinition>.Remove (from the live set's class)

// The open Fabricator's parts (RebuildFabricator).
std::string g_fabWhy;  // "" = rebuild available
Il2CppClass* g_fabUiClass = nullptr;
size_t g_offUiContent = 0;                                            // FabricatorUI._content
size_t g_offContentList = 0, g_offContentGrid = 0, g_offContentTitle = 0;  // FabricatorContent._categoryList / _itemGrid / _categoryTitle
size_t g_offListShown = 0, g_offListButtons = 0;                      // FabricatorCategoryList._categoriesShown / _buttons
size_t g_offGridCategory = 0, g_offGridNav = 0, g_offGridPages = 0;   // FabricatorItemGrid._category / _gridNavigation / _paginatedView
size_t g_offPagesCurrent = 0;                                         // PaginatedView._currentPage
size_t g_offNavSelected = 0;                                          // GridNavigation._selectedIndex
const MethodInfo* g_findCategories = nullptr;  // FabricatorCategoryList.FindCategoriesToShow (private; returns early unless _categoriesShown is null)
const MethodInfo* g_buttonSetData = nullptr;   // FabricatorCategoryButton.SetData(IFabricateCategoryRuntime)
const MethodInfo* g_titleSetCategory = nullptr;  // FabricatorCategoryTitle.SetCategory(IFabricateCategoryRuntime)
const MethodInfo* g_gridSetCategory = nullptr;   // FabricatorItemGrid.SetCategory(IFabricateCategoryRuntime)
const MethodInfo* g_gridSelection = nullptr;     // FabricatorItemGrid.OnSelectionChanged(InteractionCause) (private)
const MethodInfo* g_pagesSetPage = nullptr;      // PaginatedView.SetPage(int, InteractionCause)
const MethodInfo* g_navSetSelected = nullptr;    // GridNavigation.SetSelectedIndex(int)

// Per definition: base name and icon (read once; the definitions are game assets).
struct DefInfo {
    std::string name;
    uint32_t icon = 0;
};
std::unordered_map<uintptr_t, DefInfo> g_defInfo;

bool IsA(Il2CppObject* obj, Il2CppClass* k) {
    const auto& a = api();
    if (!obj || !k || !a.il2cpp_object_get_class) return false;
    Il2CppClass* c = a.il2cpp_object_get_class(obj);
    if (c == k) return true;
    return c && a.il2cpp_class_is_assignable_from && a.il2cpp_class_is_assignable_from(k, c);
}

bool Live() { return g_live.load() && !sr2::core::ModuleFaulted(kModule); }

Il2CppObject* Call(const MethodInfo* m, void* obj, std::initializer_list<void*> args, const char* what, bool* ok = nullptr) {
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

template <class T>
bool CallValue(const MethodInfo* m, void* obj, std::initializer_list<void*> args, const char* what, T& out) {
    bool ok = false;
    Il2CppObject* r = Call(m, obj, args, what, &ok);
    if (!ok || !r) return false;
    out = *reinterpret_cast<T*>(reinterpret_cast<char*>(r) + 2 * sizeof(void*));
    return true;
}

void Done(bool ok, const std::string& text) {
    if (ok) SR2_INFO("[{}] {}", kModule, text);
    else SR2_WARN("[{}] {}", kModule, text);
    sr2::ui::sandbox::SetStatus(text, !ok);
}

std::string Roman(int n) {  // 1..39 is all an upgrade needs
    static const char* const kTens[] = {"", "X", "XX", "XXX"};
    static const char* const kOnes[] = {"", "I", "II", "III", "IV", "V", "VI", "VII", "VIII", "IX"};
    if (n <= 0 || n >= 40) return std::to_string(n);
    return std::string(kTens[n / 10]) + kOnes[n % 10];
}

// "Tank Booster III" for level 2; "not bought" for -1.
std::string Tier(const std::string& name, int level) {
    return level < 0 ? name + " (not bought)" : std::format("{} {}", name, Roman(level + 1));
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

// --------------------------------------------------------------- player ----
Il2CppObject* PlayerState() {
    Il2CppObject* player = nullptr;
    bool found = false;
    if (!CallValue(g_tryGetPlayerState, nullptr, {&player}, "SceneContextUtility.TryGetPlayerState", found) || !found ||
        !IsA(player, g_playerStateClass) || !unity::IsAlive(player))
        return nullptr;
    return player;
}

Il2CppObject* UpgradeModel() {
    Il2CppObject* player = PlayerState();
    if (!player) return nullptr;
    Il2CppObject* model = unity::Field<Il2CppObject*>(player, g_offPlayerModel);
    if (!IsA(model, g_playerModelClass)) return nullptr;
    Il2CppObject* um = Call(g_getUpgradeModel, model, {}, "PlayerModel.GetUpgradeModel");
    return um && unity::IsAlive(um) ? um : nullptr;
}

// UpgradeModel.upgradeDefinitions (UpgradeDefinitionList; its List<UpgradeDefinition> "items"), the list the
// Fabricator's upgrade page and the save use.
std::vector<Il2CppObject*> Definitions(Il2CppObject* um) {
    std::vector<Il2CppObject*> out;
    Il2CppObject* defList = um ? unity::Field<Il2CppObject*>(um, g_offUmDefs) : nullptr;
    if (!defList || !unity::IsAlive(defList)) return out;
    auto itemsOff = Resolver::Get().FieldOffset(api().il2cpp_object_get_class(defList), "items");
    void* defs = itemsOff ? unity::Field<void*>(defList, *itemsOff) : nullptr;
    for (size_t i = 0, n = unity::ListCount(defs); i < n; ++i) {
        auto* up = static_cast<Il2CppObject*>(unity::ListAt(defs, i));
        if (IsA(up, g_upgradeDefClass) && unity::IsAlive(up)) out.push_back(up);
    }
    return out;
}

// The definition behind a UI id, only if it is in the player's list right now.
Il2CppObject* FindDefinition(Il2CppObject* um, uintptr_t id) {
    for (Il2CppObject* d : Definitions(um))
        if (reinterpret_cast<uintptr_t>(d) == id) return d;
    return nullptr;
}

int LevelCount(Il2CppObject* def) { return static_cast<int>(unity::ListCount(unity::Field<void*>(def, g_offUdLevels))); }

int Level(Il2CppObject* um, Il2CppObject* def) {
    int cur = -1;
    CallValue(g_getLevel, um, {def}, "UpgradeModel.GetUpgradeLevel", cur);
    return cur;
}

Il2CppObject* LevelDef(Il2CppObject* def, int level) {
    void* levels = unity::Field<void*>(def, g_offUdLevels);
    if (level < 0 || static_cast<size_t>(level) >= unity::ListCount(levels)) return nullptr;
    auto* l = static_cast<Il2CppObject*>(unity::ListAt(levels, static_cast<size_t>(level)));
    if (l && (!g_offUlCosts || !g_offUlTitle)) {  // nested class: offsets from the live object, once
        Il2CppClass* k = api().il2cpp_object_get_class(l);
        if (auto o = Resolver::Get().FieldOffset(k, "_costs")) g_offUlCosts = *o;
        if (auto o = Resolver::Get().FieldOffset(k, "Title")) g_offUlTitle = *o;
    }
    return l;
}

std::string Localized(Il2CppObject* ls) {
    if (!ls) return {};
    // The mod's cloned upgrades (custom_upgrades: Drone Battery Saver) keep the template's table entry
    // ("Tank Booster I") and register their own text for the instance; the game shows that one.
    if (std::string own; sr2::core::text_override::Lookup(ls, own)) return CleanText(own);
    if (!g_locGet) return {};
    auto* s = reinterpret_cast<Il2CppString*>(Call(g_locGet, ls, {}, "LocalizedString.GetLocalizedString"));
    std::string t = CleanText(Resolver::Get().ToUtf8(s));
    return t.rfind("!!", 0) == 0 ? std::string() : t;
}

const DefInfo& Info(Il2CppObject* def) {
    auto it = g_defInfo.find(reinterpret_cast<uintptr_t>(def));
    if (it != g_defInfo.end()) return it->second;
    DefInfo info;
    // Level 0's title without its tier numeral ("Tank Booster I" -> "Tank Booster").
    if (Il2CppObject* l0 = LevelDef(def, 0); l0 && g_offUlTitle)
        info.name = Localized(unity::Field<Il2CppObject*>(l0, g_offUlTitle));
    if (const size_t sp = info.name.find_last_of(' '); sp != std::string::npos) {
        const std::string last = info.name.substr(sp + 1);
        if (!last.empty() && last.find_first_not_of("IVX") == std::string::npos) info.name.erase(sp);
    }
    if (info.name.empty()) info.name = unity::GetName(def);
    if (auto* sprite = unity::Field<Il2CppObject*>(def, g_offUdIcon); sprite && unity::IsAlive(sprite))
        info.icon = sr2::ui::item_icons::Register(sprite);
    return g_defInfo.emplace(reinterpret_cast<uintptr_t>(def), std::move(info)).first->second;
}

// ------------------------------------------------------------ components ----
Il2CppObject* SpendableDirector() {
    Il2CppObject* spend = nullptr;
    bool found = false;
    if (!g_tryGetSpendable ||
        !CallValue(g_tryGetSpendable, nullptr, {&spend}, "SceneContextUtility.TryGetSpendableDirector", found) || !found ||
        !spend || !unity::IsAlive(spend))
        return nullptr;
    return spend;
}

// Level 0's upgrade components (PurchaseCost.ComponentCosts), each once. Empty for upgrades the Fabricator
// never hides (no _requireComponentToBeVisibleToPlayer).
std::vector<Il2CppObject*> GateComponents(Il2CppObject* def) {
    std::vector<Il2CppObject*> out;
    if (!g_discoverWhy.empty() || !unity::Field<bool>(def, g_offUdRequire)) return out;
    Il2CppObject* l0 = LevelDef(def, 0);
    if (!l0 || !g_offUlCosts) return out;
    void* comps = *reinterpret_cast<void* const*>(reinterpret_cast<const char*>(l0) + g_offUlCosts + g_offCostComponents);
    for (size_t i = 0, n = unity::ListCount(comps); i < n; ++i) {
        auto* c = static_cast<Il2CppObject*>(unity::ListAt(comps, i));
        if (c && unity::IsAlive(c) && std::find(out.begin(), out.end(), c) == out.end()) out.push_back(c);
    }
    return out;
}

int Held(Il2CppObject* spend, Il2CppObject* comp) {
    int have = 0;
    CallValue(g_componentAmount, spend, {comp}, "SpendableDirector.GetUpgradeComponentAmount", have);
    return have;
}

// As PlayerUpgradeFabricatableItem.get_ShouldShowPlaceholder (0x82B940): not bought and a level-0 component missing.
bool Hidden(Il2CppObject* spend, const std::vector<Il2CppObject*>& gate, int level) {
    if (level >= 0 || gate.empty() || !spend) return false;
    for (Il2CppObject* c : gate)
        if (Held(spend, c) < 1) return true;
    return false;
}

// One of each missing component, as finding the upgrade does. False if the game refused one.
bool GiveComponents(Il2CppObject* spend, const std::vector<Il2CppObject*>& gate, int* given = nullptr) {
    bool ok = true;
    for (Il2CppObject* c : gate) {
        if (Held(spend, c) >= 1) continue;
        int one = 1;
        bool got = false;
        if (CallValue(g_acquireComponent, nullptr, {c, &one}, "AcquisitionUtility.AcquireUpgradeComponent", got) && got) {
            if (given) ++*given;
        } else {
            ok = false;
        }
    }
    return ok;
}

// ------------------------------------------------------------- publish ----
// The mod's buildables (custom_upgrades/buildables.h) for the "Mod buildables" tab.
void PublishMods(ub::State& st) {
    namespace bld = sr2::modules::custom_upgrades::buildables;
    if (!sr2::modules::custom_upgrades::Active()) {
        st.modWhy = "Custom upgrades are off (Options: Custom upgrades).";
        return;
    }
    for (const bld::Info& i : bld::List()) {
        ub::ModBlueprint m;
        m.id = i.id;
        m.name = i.name;
        m.base = i.base;
        m.hint = i.hint;
        m.icon = ModIcon(i.id);  // rendered from the dressed prefab (module.cpp RenderNextModIcon), else the base icon
        if (!m.icon && i.icon && unity::IsAlive(i.icon)) m.icon = sr2::ui::item_icons::Register(i.icon);
        m.unlocked = i.unlocked;
        m.baseUnlocked = i.baseUnlocked;
        m.mode = static_cast<int>(i.unlock);
        m.owned = i.owned;
        st.mods.push_back(std::move(m));
    }
    if (st.mods.empty()) st.modWhy = "Load a save: the mod's buildables are set up a moment after it is loaded.";
}

void Publish() {
    ub::State st;
    st.tick = GetTickCount64();
    st.blueprintWhy = g_blueprintWhy;
    if (Live()) PublishMods(st);
    if (!Live()) {
        st.why = "Sandbox is off.";
    } else if (!g_why.empty()) {
        st.why = "Unavailable: " + g_why;
    } else if (Il2CppObject* um = UpgradeModel()) {
        st.inGame = true;
        Il2CppObject* spend = SpendableDirector();
        for (Il2CppObject* def : Definitions(um)) {
            const int levels = LevelCount(def);
            if (levels <= 0) continue;
            const DefInfo& info = Info(def);
            ub::Upgrade u;
            u.id = reinterpret_cast<uintptr_t>(def);
            u.name = info.name;
            u.icon = info.icon;
            u.level = Level(um, def);
            u.levels = levels;
            const int32_t uid = unity::Field<int32_t>(def, g_offUdId);
            const int vanilla = VanillaLevels(uid);
            u.vanillaLevels = vanilla > 0 && levels > vanilla ? vanilla : 0;
            u.slotTier = uid == kSlotUpgradeId;
            const auto gate = spend ? GateComponents(def) : std::vector<Il2CppObject*>();
            u.discoverable = !gate.empty();
            u.discovered = !Hidden(spend, gate, u.level);
            st.upgrades.push_back(std::move(u));
        }
        if (st.upgrades.empty()) st.why = "The player's upgrade list is empty.";
    } else {
        st.why = "Load a save to use the sandbox.";
    }
    g_lastPublish = st.tick;
    ub::Publish(std::move(st));
}

void AfterChange() {
    if (g_host.changed) g_host.changed();
    if (g_host.refreshFabricator) g_host.refreshFabricator(true);
    Publish();
}

// --------------------------------------------------------------- levels ----
// Upgrade effects, checked and repaired (round 8: "max tank booster gives me x1880 space").
//
// ActorUpgradeHandler.ApplyUpgrade(def, level) (0x8646B0, decompiled) collects _levels[level].Effects per
// property, takes the nodes it applied before from _appliedModifiers[def._uniqueId] (List<AppliedModifier>
// {ModifiableFloat Property @0, LinkedListNode<FloatModifier> Node @8}), calls ModifiableFloat.
// BatchModifiers(add, remove) per property and only at the very end stores the new nodes in
// _appliedModifiers. When anything in between throws (the round-8 log: a NullReferenceException on every
// Tank Booster / Extra Tank change in that session, from inside the game's property update; the cause, a
// destroyed HUD slot view still subscribed to the slot, is removed by PruneSlotListeners), the new modifiers stay on the property but are not remembered: the next level change adds the
// next level's modifier on top (Tank Booster XII is ADD 180: 20 + 180 + 180 + ... = 1880), and nodes that
// were removed but are still remembered make every later ApplyUpgrade of that upgrade throw "The
// LinkedList node does not belong to current LinkedList" (the Drone Battery Saver's errors). The old
// backstop here called ApplyUpgrade a second time after a throwing SetUpgradeLevel, which doubled every
// leak (Extra Tank I from "not bought" unlocked two slots in that log).
//
// Heal, per handler of the model (UpgradeModel.participants; the player's is one), using only what the
// upgrade definitions say: every FloatModifier in any level's Effects belongs to (upgrade, level). On every
// property those modifiers target (UnitPropertyTracker.TryGetProperty), each node of an upgrade modifier
// stays only if its level is the upgrade's current level and it is the only node of that modifier (a
// remembered node wins); the rest go with ModifiableFloat.RemoveModifier. Then remembered nodes that are no
// longer in a list are dropped from _appliedModifiers (List.RemoveAt), and an upgrade whose current level
// has a modifier on no property gets ApplyUpgrade once (safe now: nothing stale to remove). Modifiers of
// anything else (not in an upgrade level) are never touched. So a property always ends at its default plus
// the current levels' values, whatever threw before. Runs after each level change and every few seconds.
// The values are runtime only: a load rebuilds them from the saved levels, so a save never keeps them.
constexpr unsigned long long kHealMs = 3000;
unsigned long long g_lastHeal = 0;
std::string g_healWhy;                         // "" = heal available
size_t g_offHandlerTracker = 0;                // ActorUpgradeHandler._unitPropertyTracker
size_t g_offHandlerApplied = 0;                // ActorUpgradeHandler._appliedModifiers
const MethodInfo* g_tryGetProperty = nullptr;  // UnitPropertyTracker.TryGetProperty(PropertyDefinition, out ModifiableFloat)
size_t g_offUlEffects = 0;                     // UpgradeLevelDefinition.Effects (nested class: from a live object)
size_t g_offModTarget = 0;                     // Modifier<float>.TargetProperty (from a live modifier)
constexpr size_t kAppliedStride = 16;          // AppliedModifier {Property @0, Node @8}
constexpr size_t kAppliedNode = 8;

// Field offset / method on a live object's class (generic BCL types), cached per class.
size_t LiveOffset(Il2CppObject* obj, const char* name) {
    static std::unordered_map<std::string, size_t> cache;
    Il2CppClass* k = obj ? api().il2cpp_object_get_class(obj) : nullptr;
    if (!k) return 0;
    const std::string key = std::format("{}|{}", static_cast<const void*>(k), name);
    if (auto it = cache.find(key); it != cache.end()) return it->second;
    const auto o = Resolver::Get().FieldOffset(k, name);
    return cache[key] = o ? *o : 0;
}
const MethodInfo* LiveMethod(Il2CppObject* obj, const char* name, int argc) {
    static std::unordered_map<std::string, const MethodInfo*> cache;
    Il2CppClass* k = obj ? api().il2cpp_object_get_class(obj) : nullptr;
    if (!k) return nullptr;
    const std::string key = std::format("{}|{}|{}", static_cast<const void*>(k), name, argc);
    if (auto it = cache.find(key); it != cache.end()) return it->second;
    const MethodInfo* m = nullptr;
    for (Il2CppClass* c = k; c && !m; c = api().il2cpp_class_get_parent ? api().il2cpp_class_get_parent(c) : nullptr)
        m = Resolver::Get().Method(c, name, argc);
    return cache[key] = m;
}

void* LevelEffects(Il2CppObject* lvl) {
    if (!lvl) return nullptr;
    if (!g_offUlEffects) g_offUlEffects = LiveOffset(lvl, "Effects");
    return g_offUlEffects ? unity::Field<void*>(lvl, g_offUlEffects) : nullptr;
}

// _appliedModifiers[uid] (List<AppliedModifier>) or nullptr.
Il2CppObject* AppliedList(Il2CppObject* applied, int32_t uid) {
    const MethodInfo* tryGet = LiveMethod(applied, "TryGetValue", 2);
    Il2CppObject* list = nullptr;
    bool found = false;
    if (!tryGet || !CallValue(tryGet, applied, {&uid, &list}, "Dictionary.TryGetValue", found) || !found) return nullptr;
    return list;
}
// The remembered nodes of a List<AppliedModifier>, by index (a null node stays in).
std::vector<Il2CppObject*> AppliedNodes(Il2CppObject* list) {
    std::vector<Il2CppObject*> out;
    const size_t itemsOff = LiveOffset(list, "_items"), sizeOff = LiveOffset(list, "_size");
    if (!list || !itemsOff || !sizeOff) return out;
    void* items = unity::Field<void*>(list, itemsOff);
    const int n = unity::Field<int32_t>(list, sizeOff);
    if (!items || n <= 0 || static_cast<size_t>(n) > unity::ArrayLength(items)) return out;
    char* data = unity::ArrayData<char>(items);
    for (int i = 0; i < n && i < 256; ++i)
        out.push_back(*reinterpret_cast<Il2CppObject**>(data + static_cast<size_t>(i) * kAppliedStride + kAppliedNode));
    return out;
}
bool Listed(Il2CppObject* node) {  // LinkedListNode.list: null once removed
    const size_t off = node ? LiveOffset(node, "list") : 0;
    return off && unity::Field<void*>(node, off);
}

// Every node of a ModifiableFloat: Modifiers (SortedList<int, SortedList<int, LinkedList<FloatModifier>>>).
std::vector<Il2CppObject*> PropertyNodes(Il2CppObject* prop) {
    std::vector<Il2CppObject*> out;
    auto sortedValues = [](Il2CppObject* sl, std::vector<Il2CppObject*>& vals) {
        const size_t vOff = LiveOffset(sl, "values"), nOff = LiveOffset(sl, "_size");
        void* arr = sl && vOff && nOff ? unity::Field<void*>(sl, vOff) : nullptr;
        const int n = arr ? unity::Field<int32_t>(sl, nOff) : 0;
        for (int i = 0; i < n && static_cast<size_t>(i) < unity::ArrayLength(arr) && i < 64; ++i)
            if (auto* v = static_cast<Il2CppObject*>(unity::ArrayAt(arr, static_cast<size_t>(i)))) vals.push_back(v);
    };
    const size_t modsOff = LiveOffset(prop, "Modifiers");
    Il2CppObject* outer = modsOff ? unity::Field<Il2CppObject*>(prop, modsOff) : nullptr;
    std::vector<Il2CppObject*> inner, lists;
    if (outer) sortedValues(outer, inner);
    for (Il2CppObject* sl : inner) sortedValues(sl, lists);
    for (Il2CppObject* ll : lists) {
        const size_t headOff = LiveOffset(ll, "head"), countOff = LiveOffset(ll, "count");
        Il2CppObject* node = headOff ? unity::Field<Il2CppObject*>(ll, headOff) : nullptr;
        const int count = countOff ? unity::Field<int32_t>(ll, countOff) : 0;
        const size_t nextOff = node ? LiveOffset(node, "next") : 0;
        for (int i = 0; i < count && i < 512 && node && nextOff; ++i) {  // circular: count nodes from head
            out.push_back(node);
            node = unity::Field<Il2CppObject*>(node, nextOff);
        }
    }
    return out;
}

struct HealResult {
    int stray = 0, stale = 0, reapplied = 0;
    std::string detail;
    void Add(const std::string& s) { detail += (detail.empty() ? "" : "; ") + s; }
};

void HealHandler(Il2CppObject* um, Il2CppObject* h, const std::vector<Il2CppObject*>& defs, HealResult& r) {
    Il2CppObject* tracker = unity::Field<Il2CppObject*>(h, g_offHandlerTracker);
    Il2CppObject* applied = unity::Field<Il2CppObject*>(h, g_offHandlerApplied);
    if (!tracker || !applied) return;
    struct Owner {
        int level, current;
    };
    std::unordered_map<Il2CppObject*, Owner> owner;  // upgrade modifier -> its level and the upgrade's level
    std::vector<Il2CppObject*> propDefs;
    std::vector<int> current(defs.size(), -1);
    for (size_t d = 0; d < defs.size(); ++d) {
        current[d] = Level(um, defs[d]);
        for (int l = 0, n = LevelCount(defs[d]); l < n; ++l) {
            void* fx = LevelEffects(LevelDef(defs[d], l));
            for (size_t e = 0, ne = unity::ListCount(fx); e < ne; ++e) {
                auto* mod = static_cast<Il2CppObject*>(unity::ListAt(fx, e));
                if (!mod) continue;
                if (!g_offModTarget) g_offModTarget = LiveOffset(mod, "TargetProperty");
                if (!g_offModTarget) return;
                owner[mod] = {l, current[d]};
                Il2CppObject* pd = unity::Field<Il2CppObject*>(mod, g_offModTarget);
                if (pd && std::find(propDefs.begin(), propDefs.end(), pd) == propDefs.end()) propDefs.push_back(pd);
            }
        }
    }
    std::vector<Il2CppObject*> remembered;
    for (Il2CppObject* def : defs)
        if (Il2CppObject* list = AppliedList(applied, unity::Field<int32_t>(def, g_offUdId)))
            for (Il2CppObject* n : AppliedNodes(list)) remembered.push_back(n);
    auto isRemembered = [&](Il2CppObject* n) { return std::find(remembered.begin(), remembered.end(), n) != remembered.end(); };

    std::vector<Il2CppObject*> present;  // upgrade modifiers that stay on a property
    for (Il2CppObject* pd : propDefs) {
        Il2CppObject* prop = nullptr;
        bool has = false;
        if (!CallValue(g_tryGetProperty, tracker, {pd, &prop}, "UnitPropertyTracker.TryGetProperty", has) || !has || !prop)
            continue;
        const auto nodes = PropertyNodes(prop);
        const size_t itemOff = nodes.empty() ? 0 : LiveOffset(nodes.front(), "item");
        if (!itemOff) continue;
        std::vector<Il2CppObject*> drop;
        std::unordered_map<Il2CppObject*, Il2CppObject*> keep;  // modifier -> the node kept
        for (Il2CppObject* node : nodes) {
            Il2CppObject* mod = unity::Field<Il2CppObject*>(node, itemOff);
            auto it = owner.find(mod);
            if (it == owner.end()) continue;  // not an upgrade's modifier: never touched
            if (it->second.level != it->second.current) {
                drop.push_back(node);
                continue;
            }
            auto k = keep.find(mod);
            if (k == keep.end()) {
                keep[mod] = node;
            } else if (isRemembered(node) && !isRemembered(k->second)) {
                drop.push_back(k->second);
                k->second = node;
            } else {
                drop.push_back(node);
            }
        }
        for (const auto& kv : keep) present.push_back(kv.first);
        if (drop.empty()) continue;
        const MethodInfo* remove = LiveMethod(prop, "RemoveModifier", 1);
        int removed = 0;
        for (Il2CppObject* node : drop) {
            Call(remove, prop, {node}, "ModifiableFloat.RemoveModifier");
            removed += Listed(node) ? 0 : 1;  // the removal comes before the re-evaluation that may throw
        }
        r.stray += removed;
        r.Add(std::format("{}: {} of {} stray modifier(s) removed", unity::GetName(pd), removed, drop.size()));
    }

    for (size_t d = 0; d < defs.size(); ++d) {
        Il2CppObject* list = AppliedList(applied, unity::Field<int32_t>(defs[d], g_offUdId));
        const MethodInfo* removeAt = list ? LiveMethod(list, "RemoveAt", 1) : nullptr;
        const auto nodes = list ? AppliedNodes(list) : std::vector<Il2CppObject*>();
        int dropped = 0;
        for (int i = static_cast<int>(nodes.size()) - 1; i >= 0 && removeAt; --i) {
            if (Listed(nodes[static_cast<size_t>(i)])) continue;
            int32_t idx = i;
            bool ok = false;
            Call(removeAt, list, {&idx}, "List.RemoveAt", &ok);
            dropped += ok ? 1 : 0;
        }
        if (dropped) {
            r.stale += dropped;
            r.Add(std::format("'{}': {} stale entr{} dropped", unity::GetName(defs[d]), dropped, dropped == 1 ? "y" : "ies"));
        }
        if (current[d] < 0 || !g_applyUpgrade) continue;
        void* fx = LevelEffects(LevelDef(defs[d], current[d]));
        bool missing = false;
        for (size_t e = 0, ne = unity::ListCount(fx); e < ne && !missing; ++e) {
            auto* mod = static_cast<Il2CppObject*>(unity::ListAt(fx, e));
            missing = mod && std::find(present.begin(), present.end(), mod) == present.end();
        }
        if (!missing) continue;
        int32_t v = current[d];
        Call(g_applyUpgrade, h, {defs[d], &v}, "ActorUpgradeHandler.ApplyUpgrade");
        ++r.reapplied;
        r.Add(std::format("'{}' level {} applied again", unity::GetName(defs[d]), Roman(current[d] + 1)));
    }
}

// Every handler of the model. `why`: for the log ("after a level change", "check").
HealResult Heal(Il2CppObject* um, const char* why) {
    HealResult r;
    g_lastHeal = GetTickCount64();
    if (!g_healWhy.empty() || !um || sr2::core::ModuleFaulted(kModule)) return r;
    const auto defs = Definitions(um);
    void* list = unity::Field<void*>(um, g_offUmParticipants);
    for (size_t i = 0, c = unity::ListCount(list); i < c && i < 64; ++i) {
        auto* h = static_cast<Il2CppObject*>(unity::ListAt(list, i));
        if (IsA(h, g_handlerClass)) HealHandler(um, h, defs, r);
    }
    if (r.stray || r.stale || r.reapplied) {
        SR2_WARN("[{}] upgrade effects repaired ({}): {}", kModule, why, r.detail);
        if (g_host.vacpackState) SR2_INFO("[{}] vacpack after the repair: {}", kModule, g_host.vacpackState(true));
    }
    return r;
}

// ------------------------------------------------- stale slot listeners ----
// The source of the NullReferenceException that every Tank Booster / Extra Tank change threw in the round-9
// log (IDA on 1.3.0, round 10). The chain:
//   UpgradeModel.SetUpgradeLevel -> OnUpgradeChanged -> ActorUpgradeHandler.ApplyUpgrade (0x8646B0) ->
//   ModifiableValue<float>.BatchModifiers (0x1814D3730: adds, removes, then 0x1800AC530 recomputes the value
//   and invokes OnValueChanged unconditionally) -> AmmoSlot.<Setup>b__32_2 / _3 (0xA3B960, the slot's
//   subscription to its AmmoLimit / unlock property) -> AmmoSlot.OnSlotChanged(slot) -> every subscriber,
//   among them AmmoSlotViewHolder.UpdateAmmoDisplay (0x67FE80, the HUD slot view) -> Image.set_sprite on a
//   destroyed view -> ThrowHelper.ThrowNullReferenceException (0x345DA50: a plain NullReferenceException in
//   player builds, the message in the log).
// The destroyed views: the HUD row (AmmoSlotAdapter, an AdapterView) rebinds its list on every
// OnSlotChanged (AmmoSlotAdapter.OnSlotChanged -> Refresh -> BindDataList(List, int), 0x1F281F0) and
// destroys the view holders past the new count when fewer slots are unlocked (Destroyer.Destroy). Their
// AmmoSlotViewHolder.BindData (0x67EFE0) subscriptions to the slot's OnSlotChanged / OnAmmoChanged are only
// removed when the holder is bound to another slot; nothing removes them on destroy (no OnDestroy), and
// AmmoSlotAdapter.Start never unsubscribes either. Vanilla never has fewer unlocked slots, so this needs a
// lowered Extra Tank (Sandbox; the first throw in that log came 5 s after "Extra Tank: IV -> not bought"):
// from then on any change of a slot's properties (every Tank Booster change: AmmoLimit is every slot's;
// raising Extra Tank: the slots unlocked) reaches a destroyed holder and throws, which leaves the new
// modifiers on the property unremembered (the 1880 per slot). The same holders would throw from game code
// as well (AmmoSlot.OnAmmoChanged when the slot fills).
// Fix: remove every subscriber of the player's AmmoSlot.OnSlotChanged / OnAmmoChanged whose target is a
// destroyed UnityEngine.Object (Delegate.GetInvocationList, Delegate.get_Target, AmmoSlot.remove_*). Run
// before each level change here (the views destroyed by an earlier change are gone by then: Destroy runs at
// the end of that frame) and with the effects check every few seconds while Sandbox is on.
std::string g_listenWhy;  // "" = the prune is available
Il2CppClass* g_unityObjectClass = nullptr;
size_t g_offPsAmmo = 0, g_offAmModel = 0, g_offAmSlots = 0;  // PlayerState.Ammo / AmmoSlotManager._ammoModel / AmmoModel.Slots
size_t g_offSlotChanged = 0, g_offAmmoChanged = 0;            // AmmoSlot.OnSlotChanged / OnAmmoChanged (event fields)
const MethodInfo* g_removeSlotChanged = nullptr;              // AmmoSlot.remove_OnSlotChanged
const MethodInfo* g_removeAmmoChanged = nullptr;              // AmmoSlot.remove_OnAmmoChanged

// Listeners of destroyed objects removed from the player's slots; logged when there were any. `why` for the log.
int PruneSlotListeners(const char* why) {
    if (!g_listenWhy.empty() || sr2::core::ModuleFaulted(kModule)) return 0;
    Il2CppObject* ps = PlayerState();
    Il2CppObject* mgr = ps ? unity::Field<Il2CppObject*>(ps, g_offPsAmmo) : nullptr;
    Il2CppObject* model = mgr ? unity::Field<Il2CppObject*>(mgr, g_offAmModel) : nullptr;
    void* slots = model ? unity::Field<void*>(model, g_offAmSlots) : nullptr;
    struct Event {
        size_t off;
        const MethodInfo* remove;
        const char* name;
    };
    const Event events[] = {{g_offSlotChanged, g_removeSlotChanged, "OnSlotChanged"},
                            {g_offAmmoChanged, g_removeAmmoChanged, "OnAmmoChanged"}};
    int removed = 0, failed = 0;
    std::unordered_map<std::string, int> kinds;  // "AmmoSlotViewHolder.OnSlotChanged" -> count
    for (size_t i = 0, n = unity::ArrayLength(slots); i < n && i < 64; ++i) {
        auto* slot = static_cast<Il2CppObject*>(unity::ArrayAt(slots, i));
        if (!slot) continue;
        for (const Event& ev : events) {
            Il2CppObject* d = unity::Field<Il2CppObject*>(slot, ev.off);
            if (!d) continue;
            auto* list = reinterpret_cast<Il2CppArray*>(Call(LiveMethod(d, "GetInvocationList", 0), d, {}, "Delegate.GetInvocationList"));
            for (size_t j = 0, m = unity::ArrayLength(list); j < m && j < 256; ++j) {
                auto* e = static_cast<Il2CppObject*>(unity::ArrayAt(list, j));
                Il2CppObject* target = e ? Call(LiveMethod(e, "get_Target", 0), e, {}, "Delegate.get_Target") : nullptr;
                if (!target || !IsA(target, g_unityObjectClass) || unity::IsAlive(target)) continue;
                bool ok = false;
                Call(ev.remove, slot, {e}, ev.name, &ok);
                if (!ok) {
                    ++failed;
                    continue;
                }
                ++removed;
                const char* cls = api().il2cpp_class_get_name ? api().il2cpp_class_get_name(api().il2cpp_object_get_class(target)) : nullptr;
                ++kinds[std::format("{}.{}", cls ? cls : "?", ev.name)];
            }
        }
    }
    if (removed || failed) {
        std::string list;
        for (const auto& [k, c] : kinds) list += std::format("{}{} x{}", list.empty() ? "" : ", ", k, c);
        SR2_WARN("[{}] vacpack slots ({}): {} listener(s) of destroyed HUD objects removed ({}){}", kModule, why, removed,
                 list.empty() ? "none" : list, failed ? std::format(", {} could not be removed", failed) : std::string());
    }
    return removed;
}

// SetUpgradeLevel(def, to), then the effects checked (Heal; heal = false when the caller heals once after
// several changes); for a discoverable upgrade reset to -1, level 0's components come back if missing (it
// stays discovered). `note` gets what happened besides the level. True once the level is stored: a
// subscriber of OnUpgradeChanged that threw (logged) no longer counts as refused, the heal puts the effects
// right. The slots' stale listeners go first (PruneSlotListeners: the cause of those throws).
bool SetLevel(Il2CppObject* um, Il2CppObject* spend, Il2CppObject* def, int to, std::string& note, bool heal = true) {
    PruneSlotListeners("before a level change");
    int32_t v = to;
    bool ok = false;
    Call(g_setLevel, um, {def, &v}, "UpgradeModel.SetUpgradeLevel", &ok);
    if (Level(um, def) != to) return false;
    if (!ok)
        SR2_WARN("[{}] '{}': SetUpgradeLevel stored level {} but threw (logged above); its effects are checked", kModule,
                 unity::GetName(def), to + 1);
    if (heal) Heal(um, "after a level change");
    if (to < 0 && spend) {
        const auto gate = GateComponents(def);
        int given = 0;
        if (Hidden(spend, gate, to) && GiveComponents(spend, gate, &given) && given)
            note = std::format("; its upgrade component{} given back, so it stays discovered", given > 1 ? "s" : "");
    }
    return true;
}

void DoSetLevel(uintptr_t id, long long value) {
    Il2CppObject* um = UpgradeModel();
    Il2CppObject* def = um ? FindDefinition(um, id) : nullptr;
    if (!def) return Done(false, "Upgrade not changed: it is not in the player's upgrade list any more");
    const int levels = LevelCount(def);
    const int from = Level(um, def);
    const int to = static_cast<int>(std::clamp<long long>(value, -1, levels - 1));
    const std::string name = Info(def).name;
    if (to == from) return Done(true, std::format("{} is {} already", name, to < 0 ? "not bought" : "at " + Roman(to + 1)));
    std::string note;
    const int32_t uid = unity::Field<int32_t>(def, g_offUdId);
    const bool vacpack = VanillaLevels(uid) > 0;  // Tank Booster / Extra Tank: the slots are logged around it
    const std::string before = vacpack && g_host.vacpackState ? g_host.vacpackState(false) : std::string();
    if (!SetLevel(um, SpendableDirector(), def, to, note))
        return Done(false, std::format("{} not changed: the game refused level {} (see the log)", name, to + 1));
    Done(true, std::format("{}: {} -> {} (of {}), free{}", name, from < 0 ? "not bought" : Roman(from + 1),
                           to < 0 ? "not bought" : Roman(to + 1), Roman(levels), note));
    if (vacpack && g_host.vacpackState)
        SR2_INFO("[{}] vacpack after {} {}: {} (before: {})", kModule, name, to < 0 ? "-" : Roman(to + 1),
                 g_host.vacpackState(true), before);
    AfterChange();
}

void DoMaxAll() {
    Il2CppObject* um = UpgradeModel();
    if (!um) return Done(false, "Upgrades not raised: load a save first");
    std::string list, note;
    int raised = 0, failed = 0;
    for (Il2CppObject* def : Definitions(um)) {
        const int last = LevelCount(def) - 1, from = Level(um, def);
        if (last < 0 || from >= last) continue;
        if (SetLevel(um, nullptr, def, last, note, false)) {
            ++raised;
            list += std::format("{}{} {} -> {}", list.empty() ? "" : ", ", Info(def).name, from < 0 ? "-" : Roman(from + 1),
                                Roman(last + 1));
        } else {
            ++failed;
        }
    }
    Heal(um, "after Max");
    if (!raised) return Done(failed == 0, failed ? "Upgrades not raised: the game refused them (see the log)"
                                                 : "Every upgrade is at its last level already");
    Done(failed == 0, std::format("Maxed {} upgrade(s), free: {}{}", raised, list,
                                  failed ? std::format(" ({} refused)", failed) : std::string()));
    if (g_host.vacpackState) SR2_INFO("[{}] vacpack after Max: {}", kModule, g_host.vacpackState(true));
    AfterChange();
}

// ------------------------------------------------------------ discovery ----
void DoDiscover(uintptr_t id, bool all) {
    if (!g_discoverWhy.empty()) return Done(false, "Not discovered: " + g_discoverWhy);
    Il2CppObject* um = UpgradeModel();
    Il2CppObject* spend = SpendableDirector();
    if (!um || !spend) return Done(false, "Not discovered: load a save first");
    std::string list;
    int found = 0, failed = 0;
    for (Il2CppObject* def : Definitions(um)) {
        if (!all && reinterpret_cast<uintptr_t>(def) != id) continue;
        const auto gate = GateComponents(def);
        const int level = Level(um, def);
        if (!Hidden(spend, gate, level)) {
            if (!all) return Done(true, gate.empty() ? Info(def).name + " is never hidden: nothing to discover"
                                                     : Info(def).name + " is discovered already");
            continue;
        }
        if (GiveComponents(spend, gate) && !Hidden(spend, gate, level)) {
            ++found;
            list += (list.empty() ? "" : ", ") + Info(def).name;
        } else {
            ++failed;
        }
    }
    if (!found) return Done(failed == 0, failed ? std::format("Not discovered: the game refused {} upgrade(s)", failed)
                                                : std::string("Every upgrade is discovered already"));
    Done(failed == 0, std::format("Discovered {} (one upgrade component each, as when found; no level bought){}", list,
                                  failed ? std::format("; {} refused", failed) : std::string()));
    AfterChange();
}

void DoUndiscover(uintptr_t id) {
    if (!g_discoverWhy.empty()) return Done(false, "Not undiscovered: " + g_discoverWhy);
    Il2CppObject* um = UpgradeModel();
    Il2CppObject* spend = SpendableDirector();
    Il2CppObject* def = um ? FindDefinition(um, id) : nullptr;
    if (!def || !spend) return Done(false, "Not undiscovered: the upgrade or the player's components were not found");
    const std::string name = Info(def).name;
    const auto gate = GateComponents(def);
    if (gate.empty()) return Done(false, name + " is never hidden: it can't be undiscovered");
    const int from = Level(um, def);
    if (from >= 0) {
        PruneSlotListeners("before a level change");
        int32_t none = -1;
        bool ok = false;
        Call(g_setLevel, um, {def, &none}, "UpgradeModel.SetUpgradeLevel", &ok);
        if (!ok || Level(um, def) != -1) return Done(false, name + " not undiscovered: the game refused to reset its level");
    }
    Il2CppObject* comps = unity::Field<Il2CppObject*>(spend, g_offSpendComponents);
    int taken = 0;
    for (Il2CppObject* c : gate) {
        int32_t have = Held(spend, c);
        if (have < 1 || !comps) continue;
        bool ok = false;
        Call(g_loseComponent, comps, {c, &have}, "UpgradeComponentsModel.LoseComponent", &ok);
        if (ok) taken += have;
    }
    const bool hidden = Hidden(spend, gate, Level(um, def));
    Done(hidden, std::format("{} undiscovered{}: {} upgrade component(s) taken back{}", name,
                             from >= 0 ? std::format(" (was {}, now not bought)", Roman(from + 1)) : std::string(), taken,
                             hidden ? ", the Fabricator hides it again" : " - still not hidden (see the log)"));
    AfterChange();
}

// ------------------------------------------------------------ blueprints ----
Il2CppObject* GadgetDirector() {
    Il2CppObject* dir = nullptr;
    bool found = false;
    if (!CallValue(g_tryGetGadgetDir, nullptr, {&dir}, "SceneContextUtility.TryGetGadgetDirector", found) || !found ||
        !dir || !unity::IsAlive(dir))
        return nullptr;
    return dir;
}

void DoBlueprints(const std::vector<uintptr_t>& ids, bool unlock) {
    const char* verb = unlock ? "unlocked" : "locked";
    if (!g_blueprintWhy.empty()) return Done(false, std::format("Blueprints not {}: {}", verb, g_blueprintWhy));
    Il2CppObject* dir = GadgetDirector();
    if (!dir || !g_host.recipe) return Done(false, std::format("Blueprints not {}: load a save first", verb));
    Il2CppObject* set = nullptr;
    if (Il2CppObject* model = unity::Field<Il2CppObject*>(dir, g_offGdModel))
        set = unity::Field<Il2CppObject*>(model, g_offGmBlueprints);
    if (!set) return Done(false, std::format("Blueprints not {}: the gadget model has no blueprint list", verb));
    if (!unlock && !g_setRemove) {
        std::string err;
        g_setRemove = Resolver::Get().Method(api().il2cpp_object_get_class(set), "Remove", 1, &err);
        if (!g_setRemove) return Done(false, "Blueprints not locked: HashSet.Remove not found (" + err + ")");
    }
    std::vector<std::string> names;
    int failed = 0;
    for (uintptr_t id : ids) {
        Il2CppObject* def = g_host.recipe(id);
        if (!IsA(def, g_gadgetDefClass) || !unity::IsAlive(def)) {
            ++failed;
            continue;
        }
        bool has = false;
        if (!CallValue(g_hasBlueprint, dir, {def}, "GadgetDirector.HasBlueprint", has) || has == unlock) continue;
        bool ok = false;
        if (unlock) {
            bool notify = false;
            Call(g_addBlueprint, dir, {def, &notify}, "GadgetDirector.AddBlueprint", &ok);
        } else {
            Call(g_setRemove, set, {def}, "HashSet<GadgetDefinition>.Remove", &ok);
        }
        bool now = !unlock;
        if (ok && CallValue(g_hasBlueprint, dir, {def}, "GadgetDirector.HasBlueprint", now) && now == unlock)
            names.push_back(unity::GetName(def));
        else
            ++failed;
    }
    if (names.empty())
        return Done(failed == 0, failed ? std::format("Blueprints not {}: the game refused {} (see the log)", verb, failed)
                                        : std::format("Nothing to do: every selected blueprint is {} already", verb));
    std::string list;
    for (size_t i = 0; i < names.size(); ++i) list += (i ? ", " : "") + names[i];
    Done(failed == 0, std::format("{} {} blueprint(s){}: {}", unlock ? "Unlocked" : "Locked", names.size(),
                                  failed ? std::format(" ({} refused)", failed) : std::string(), list));
    AfterChange();
}

// The sidecar override of a mod buildable's Fabricator card (custom_upgrades/buildables.h).
void DoModBlueprint(const std::string& id, long long mode) {
    namespace bld = sr2::modules::custom_upgrades::buildables;
    const bld::Unlock u = mode == 1 ? bld::Unlock::kUnlocked : mode == 2 ? bld::Unlock::kLocked : bld::Unlock::kFollowBase;
    std::string result;
    if (!bld::SetUnlock(id, u, result)) return Done(false, "Mod blueprint not changed: " + result);
    std::string name = id, base;
    for (const bld::Info& i : bld::List())
        if (i.id == id) name = i.name, base = i.base;
    Done(true, u == bld::Unlock::kUnlocked ? std::format("{} unlocked in this save (kept in the mod's sidecar)", name)
               : u == bld::Unlock::kLocked ? std::format("{} locked in this save (kept in the mod's sidecar)", name)
                                           : std::format("{} follows the {} blueprint again", name, base.empty() ? "base" : base));
    AfterChange();
}

// --------------------------------------------------------------- handler ----
void Handle(const ub::Action& a) {
    if (!Live()) return sr2::ui::sandbox::SetStatus("Sandbox is off.", true);
    if (!g_why.empty() && a.op != ub::Op::SetBlueprints && a.op != ub::Op::SetModBlueprint)
        return Done(false, "Upgrades unavailable: " + g_why);
    switch (a.op) {
        case ub::Op::SetLevel: DoSetLevel(a.id, a.value); break;
        case ub::Op::Discover: DoDiscover(a.id, false); break;
        case ub::Op::Undiscover: DoUndiscover(a.id); break;
        case ub::Op::DiscoverAll: DoDiscover(0, true); break;
        case ub::Op::MaxAll: DoMaxAll(); break;
        case ub::Op::SetBlueprints: DoBlueprints(a.ids, a.value != 0); break;
        case ub::Op::SetModBlueprint: DoModBlueprint(a.key, a.value); break;
    }
    Publish();
}

void HandleThunk(const ub::Action& a) { Handle(a); }

// Searches the class and its parents.
const MethodInfo* FindMethod(Resolver& R, Il2CppClass* k, const char* name, int argc, std::string& why) {
    std::string err;
    for (Il2CppClass* c = k; c; c = api().il2cpp_class_get_parent ? api().il2cpp_class_get_parent(c) : nullptr)
        if (const MethodInfo* m = R.Method(c, name, argc, &err)) return m;
    if (k && why.empty()) why = std::format("method {}({}) not found", name, argc);
    return nullptr;
}

}  // namespace

// ------------------------------------------------------- Fabricator rebuild ----
// What the Fabricator does when it opens (decompiled, 1.3.0): FabricatorCategoryList.Start (0x7FF0A0) calls
// FindCategoriesToShow (0x7FE4E0), which returns at once if _categoriesShown is set, else builds a new
// runtime per category from the menu configuration (GadgetFabricateCategoryRuntime /
// PlayerUpgradeFabricateCategoryRuntime: new fabricatable items, so new placeholders, tiers, icons and the
// gadgets whose blueprint is owned), then binds each category button to its runtime (Start's lambda is
// FabricatorCategoryButton.SetData, inlined) and selects the category. The grid (FabricatorItemGrid.
// SetCategory, 0x80A170) only rebuilds for a different runtime object: grid size, PaginatedView.
// SetNumberOfPages -> SetPage(0, forced) -> OnPageChanged (0x809420: _currentPageOfItems from the runtime's
// Items, every button configured: icon, pips, placeholder), clears the selection and fires OnSelectionChanged
// (ItemSelected -> details panel and key legend). So: _categoriesShown = null, FindCategoriesToShow, SetData
// per button, then SetCategory with the new runtime of the category the grid showed, back to its page
// (SetPage) and selection (SetSelectedIndex + OnSelectionChanged). Start itself is not called again: it also
// subscribes events. FabricatorContent.OnCategorySelected is not used either: it fades the grid out until
// the category is activated again. The old runtimes are left to the GC.
bool RebuildFabricator(Il2CppObject* ui) {
    if (!g_fabWhy.empty() || !ui || sr2::core::ModuleFaulted(kModule)) return false;
    if (!IsA(ui, g_fabUiClass) || !unity::IsAlive(ui)) return false;
    Il2CppObject* content = unity::Field<Il2CppObject*>(ui, g_offUiContent);
    if (!content || !unity::IsAlive(content)) return false;
    Il2CppObject* list = unity::Field<Il2CppObject*>(content, g_offContentList);
    Il2CppObject* grid = unity::Field<Il2CppObject*>(content, g_offContentGrid);
    Il2CppObject* title = unity::Field<Il2CppObject*>(content, g_offContentTitle);
    if (!list || !unity::IsAlive(list) || !grid || !unity::IsAlive(grid)) return false;
    void* oldShown = unity::Field<void*>(list, g_offListShown);
    if (!oldShown) return false;  // never built (not started yet): it builds itself when it starts
    Il2CppObject* oldCat = unity::Field<Il2CppObject*>(grid, g_offGridCategory);
    int catIndex = -1;
    for (size_t i = 0, n = unity::ListCount(oldShown); i < n; ++i)
        if (unity::ListAt(oldShown, i) == oldCat) catIndex = static_cast<int>(i);
    Il2CppObject* pages = unity::Field<Il2CppObject*>(grid, g_offGridPages);
    Il2CppObject* nav = unity::Field<Il2CppObject*>(grid, g_offGridNav);
    const int page = pages ? unity::Field<int32_t>(pages, g_offPagesCurrent) : 0;
    const int sel = nav ? unity::Field<int32_t>(nav, g_offNavSelected) : -1;

    unity::Field<void*>(list, g_offListShown) = nullptr;  // FindCategoriesToShow builds anew only then
    bool ok = false;
    Call(g_findCategories, list, {}, "FabricatorCategoryList.FindCategoriesToShow", &ok);
    void* shown = unity::Field<void*>(list, g_offListShown);
    if (!ok || !shown) {
        SR2_WARN("[{}] Fabricator rebuild failed: no new category list (reopen the Fabricator)", kModule);
        return false;
    }
    void* buttons = unity::Field<void*>(list, g_offListButtons);
    const size_t nCats = unity::ListCount(shown);
    const size_t nButtons = std::min(unity::ArrayLength(buttons), nCats);
    for (size_t i = 0; i < nButtons; ++i) {
        auto* b = static_cast<Il2CppObject*>(unity::ArrayAt(buttons, i));
        if (b && unity::IsAlive(b)) Call(g_buttonSetData, b, {unity::ListAt(shown, i)}, "FabricatorCategoryButton.SetData");
    }
    std::string gridNote = "grid not on a category (pinned recipe): left as it is";
    if (catIndex >= 0 && static_cast<size_t>(catIndex) < nCats) {
        void* cat = unity::ListAt(shown, static_cast<size_t>(catIndex));
        Call(g_gridSetCategory, grid, {cat}, "FabricatorItemGrid.SetCategory");
        if (title && unity::IsAlive(title)) Call(g_titleSetCategory, title, {cat}, "FabricatorCategoryTitle.SetCategory");
        int32_t p = page, cause = kCauseProgrammatic;
        if (pages && p > 0) Call(g_pagesSetPage, pages, {&p, &cause}, "PaginatedView.SetPage");
        if (nav && sel >= 0) {
            int32_t s = sel;
            Call(g_navSetSelected, nav, {&s}, "GridNavigation.SetSelectedIndex");
            Call(g_gridSelection, grid, {&cause}, "FabricatorItemGrid.OnSelectionChanged");
        }
        gridNote = std::format("grid on category {}, page {}, selection {}", catIndex + 1, page + 1, sel);
    }
    SR2_INFO("[{}] Fabricator rebuilt as on opening: {} categories, {} buttons rebound, {}", kModule, nCats, nButtons, gridNote);
    return true;
}

void SetupUpgrades(sr2::core::ModuleContext& ctx, UpgradesHost host) {
    g_host = std::move(host);
    auto& R = ctx.resolver();
    auto cls = [&](const char* image, const char* ns, const char* name, std::string& why) -> Il2CppClass* {
        std::string err;
        Il2CppClass* k = R.Class(image, ns, name, &err);
        if (!k && why.empty()) why = err.empty() ? std::format("class {}.{} not found", ns, name) : err;
        return k;
    };
    auto off = [&](Il2CppClass* k, const char* field, size_t& out, std::string& why) {
        if (!k) return;
        std::string err;
        if (auto o = R.FieldOffset(k, field, &err)) out = *o;
        else if (why.empty()) why = err.empty() ? std::format("field {} not found", field) : err;
    };
    auto method = [&](Il2CppClass* k, const char* name, int argc, std::string& why) {
        return FindMethod(R, k, name, argc, why);
    };
    Il2CppClass* scu = nullptr;
    {
        std::string& w = g_why;
        scu = cls(kImage, "MonomiPark.SlimeRancher.Util", "SceneContextUtility", w);
        g_playerStateClass = cls(kImage, "", "PlayerState", w);
        g_playerModelClass = cls(kImage, "MonomiPark.SlimeRancher.DataModel", "PlayerModel", w);
        Il2CppClass* upModel = cls(kImage, "MonomiPark.SlimeRancher.DataModel", "UpgradeModel", w);
        g_upgradeDefClass = cls(kImage, "", "UpgradeDefinition", w);
        Il2CppClass* loc = cls("Unity.Localization", "UnityEngine.Localization", "LocalizedString", w);
        g_tryGetPlayerState = method(scu, "TryGetPlayerState", 1, w);
        g_getUpgradeModel = method(g_playerModelClass, "GetUpgradeModel", 0, w);
        g_getLevel = method(upModel, "GetUpgradeLevel", 1, w);
        g_setLevel = method(upModel, "SetUpgradeLevel", 2, w);
        g_locGet = method(loc, "GetLocalizedString", 0, w);
        {
            std::string& h = g_healWhy;  // the effects repair is optional (see Heal)
            g_handlerClass = cls(kImage, "", "ActorUpgradeHandler", h);
            Il2CppClass* tracker = cls(kImage, "MonomiPark.UnitPropertySystem", "UnitPropertyTracker", h);
            g_applyUpgrade = method(g_handlerClass, "ApplyUpgrade", 2, h);
            g_tryGetProperty = method(tracker, "TryGetProperty", 2, h);
            off(upModel, "participants", g_offUmParticipants, h);
            off(g_handlerClass, "_unitPropertyTracker", g_offHandlerTracker, h);
            off(g_handlerClass, "_appliedModifiers", g_offHandlerApplied, h);
            if (!h.empty()) {
                g_applyUpgrade = nullptr;
                SR2_WARN("[{}] upgrades manager: upgrade effects are not checked or repaired ({})", kModule, h);
            }
        }
        {
            std::string& l = g_listenWhy;  // optional (see PruneSlotListeners)
            g_unityObjectClass = cls("UnityEngine.CoreModule", "UnityEngine", "Object", l);
            Il2CppClass* mgr = cls(kImage, "MonomiPark.SlimeRancher.Player", "AmmoSlotManager", l);
            Il2CppClass* ammoModel = cls(kImage, "MonomiPark.SlimeRancher.DataModel", "AmmoModel", l);
            Il2CppClass* slot = cls(kImage, "MonomiPark.SlimeRancher.Player", "AmmoSlot", l);
            off(g_playerStateClass, "<Ammo>k__BackingField", g_offPsAmmo, l);
            off(mgr, "_ammoModel", g_offAmModel, l);
            off(ammoModel, "Slots", g_offAmSlots, l);
            off(slot, "OnSlotChanged", g_offSlotChanged, l);
            off(slot, "OnAmmoChanged", g_offAmmoChanged, l);
            g_removeSlotChanged = method(slot, "remove_OnSlotChanged", 1, l);
            g_removeAmmoChanged = method(slot, "remove_OnAmmoChanged", 1, l);
            if (!l.empty())
                SR2_WARN("[{}] upgrades manager: stale vacpack HUD listeners are not removed ({}); lowering Extra Tank can "
                         "make later vacpack upgrade changes throw", kModule, l);
        }
        off(g_playerStateClass, "_model", g_offPlayerModel, w);
        off(upModel, "upgradeDefinitions", g_offUmDefs, w);
        off(g_upgradeDefClass, "_icon", g_offUdIcon, w);
        off(g_upgradeDefClass, "_requireComponentToBeVisibleToPlayer", g_offUdRequire, w);
        off(g_upgradeDefClass, "_uniqueId", g_offUdId, w);
        off(g_upgradeDefClass, "_levels", g_offUdLevels, w);
        if (!w.empty()) SR2_WARN("[{}] upgrades manager unavailable: {}", kModule, w);
    }
    {
        std::string& w = g_discoverWhy;
        if (!g_why.empty()) w = "the upgrade list is unavailable";
        Il2CppClass* spend = cls(kImage, "MonomiPark.SlimeRancher.Player", "SpendableDirector", w);
        Il2CppClass* comps = cls(kImage, "MonomiPark.SlimeRancher.Player", "UpgradeComponentsModel", w);
        Il2CppClass* acq = cls(kImage, "MonomiPark.SlimeRancher.Economy", "AcquisitionUtility", w);
        Il2CppClass* cost = cls(kImage, "MonomiPark.SlimeRancher.Economy", "PurchaseCost", w);
        g_tryGetSpendable = method(scu, "TryGetSpendableDirector", 1, w);
        g_componentAmount = method(spend, "GetUpgradeComponentAmount", 1, w);
        g_acquireComponent = method(acq, "AcquireUpgradeComponent", 2, w);
        g_loseComponent = method(comps, "LoseComponent", 2, w);
        off(spend, "_upgradeComponentsModel", g_offSpendComponents, w);
        size_t compOff = 0;
        off(cost, "ComponentCosts", compOff, w);
        if (compOff >= 2 * sizeof(void*)) g_offCostComponents = compOff - 2 * sizeof(void*);  // value type: no header inline
        else if (w.empty()) w = "PurchaseCost.ComponentCosts has an unexpected offset";
        if (!w.empty()) SR2_WARN("[{}] upgrades manager: discover / undiscover unavailable ({})", kModule, w);
    }
    {
        std::string& w = g_blueprintWhy;
        g_gadgetDefClass = cls(kImage, "", "GadgetDefinition", w);
        Il2CppClass* dir = cls(kImage, "", "GadgetDirector", w);
        Il2CppClass* model = cls(kImage, "MonomiPark.SlimeRancher.DataModel", "GadgetsModel", w);
        g_tryGetGadgetDir = method(scu, "TryGetGadgetDirector", 1, w);
        g_hasBlueprint = method(dir, "HasBlueprint", 1, w);
        g_addBlueprint = method(dir, "AddBlueprint", 2, w);
        off(dir, "_model", g_offGdModel, w);
        off(model, "blueprints", g_offGmBlueprints, w);
        if (!w.empty()) SR2_WARN("[{}] upgrades manager: blueprints unavailable ({})", kModule, w);
    }
    {
        std::string& w = g_fabWhy;
        constexpr const char* kFab = "MonomiPark.SlimeRancher.UI.Fabricator";
        g_fabUiClass = cls(kImage, kFab, "FabricatorUI", w);
        Il2CppClass* content = cls(kImage, kFab, "FabricatorContent", w);
        Il2CppClass* list = cls(kImage, kFab, "FabricatorCategoryList", w);
        Il2CppClass* button = cls(kImage, kFab, "FabricatorCategoryButton", w);
        Il2CppClass* title = cls(kImage, kFab, "FabricatorCategoryTitle", w);
        Il2CppClass* grid = cls(kImage, kFab, "FabricatorItemGrid", w);
        Il2CppClass* pages = cls(kImage, "MonomiPark.SlimeRancher.UI.Framework.CommonControls", "PaginatedView", w);
        Il2CppClass* nav = cls(kImage, "MonomiPark.SlimeRancher.UI.Framework.Interaction", "GridNavigation", w);
        off(g_fabUiClass, "_content", g_offUiContent, w);
        off(content, "_categoryList", g_offContentList, w);
        off(content, "_itemGrid", g_offContentGrid, w);
        off(content, "_categoryTitle", g_offContentTitle, w);
        off(list, "_categoriesShown", g_offListShown, w);
        off(list, "_buttons", g_offListButtons, w);
        off(grid, "_category", g_offGridCategory, w);
        off(grid, "_gridNavigation", g_offGridNav, w);
        off(grid, "_paginatedView", g_offGridPages, w);
        off(pages, "_currentPage", g_offPagesCurrent, w);
        off(nav, "_selectedIndex", g_offNavSelected, w);
        g_findCategories = method(list, "FindCategoriesToShow", 0, w);
        g_buttonSetData = method(button, "SetData", 1, w);
        g_titleSetCategory = method(title, "SetCategory", 1, w);
        g_gridSetCategory = method(grid, "SetCategory", 1, w);
        g_gridSelection = method(grid, "OnSelectionChanged", 1, w);
        g_pagesSetPage = method(pages, "SetPage", 2, w);
        g_navSetSelected = method(nav, "SetSelectedIndex", 1, w);
        if (!w.empty()) SR2_WARN("[{}] upgrades manager: the open Fabricator can't be rebuilt ({}); reopen it to see changes", kModule, w);
    }
    SR2_INFO("[{}] upgrades manager: levels {}, discover {}, blueprints {}, Fabricator rebuild {}", kModule,
             g_why.empty() ? "ok" : "off", g_discoverWhy.empty() ? "ok" : "off", g_blueprintWhy.empty() ? "ok" : "off",
             g_fabWhy.empty() ? "ok" : "off");
}

void UpgradesEnabled(bool on) {
    g_live.store(on);
    ub::SetHandler(on ? &HandleThunk : nullptr);
    if (!on) {
        ub::State st;
        st.tick = GetTickCount64();
        st.why = "Sandbox is off.";
        ub::Publish(std::move(st));
    }
}

void UpgradesTick(bool snapshot) {
    if (!Live()) return;
    // The effects check every few seconds while a save is loaded (also right after a load, and after a
    // Fabricator purchase or the custom upgrades' sidecar levels, which change levels without this page).
    // The slots' listeners of destroyed HUD views with it (also a source of throws from game code).
    if (g_why.empty() && GetTickCount64() - g_lastHeal >= kHealMs) {
        g_lastHeal = GetTickCount64();
        PruneSlotListeners("check");
        if (g_healWhy.empty())
            if (Il2CppObject* um = UpgradeModel()) Heal(um, "check");
    }
    if (!snapshot) return;
    if (GetTickCount64() - g_lastPublish >= kPublishMs) Publish();
}

}  // namespace sr2::modules::sandbox
