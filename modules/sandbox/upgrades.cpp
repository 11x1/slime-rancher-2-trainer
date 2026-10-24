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
#include "core/unity.h"
#include "modules/custom_upgrades/api.h"
#include "modules/custom_upgrades/buildables.h"
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
Il2CppObject* UpgradeModel() {
    Il2CppObject* player = nullptr;
    bool found = false;
    if (!CallValue(g_tryGetPlayerState, nullptr, {&player}, "SceneContextUtility.TryGetPlayerState", found) || !found ||
        !IsA(player, g_playerStateClass) || !unity::IsAlive(player))
        return nullptr;
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
    if (!ls || !g_locGet) return {};
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
        if (i.icon && unity::IsAlive(i.icon)) m.icon = sr2::ui::item_icons::Register(i.icon);
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
// The level's effects once more, straight on every ActorUpgradeHandler of the model (UpgradeModel.
// participants; the player's handler is one): ApplyUpgrade (0x8646B0, decompiled) removes what that upgrade
// applied before and adds _levels[level].Effects (none at -1), so a second call changes nothing when the
// event already did it. Needed when SetUpgradeLevel stored the level but an earlier subscriber of its
// multicast OnUpgradeChanged threw, so the handler never heard of it (round 8: "Tank Booster max stayed at
// 60"; the exact cause was not seen). Returns the number of handlers.
int ApplyEffects(Il2CppObject* um, Il2CppObject* def, int level) {
    if (!g_applyUpgrade || !g_handlerClass || !g_offUmParticipants) return 0;
    void* list = unity::Field<void*>(um, g_offUmParticipants);
    int n = 0;
    for (size_t i = 0, c = unity::ListCount(list); i < c && i < 64; ++i) {
        auto* h = static_cast<Il2CppObject*>(unity::ListAt(list, i));
        if (!IsA(h, g_handlerClass)) continue;
        int32_t v = level;
        bool ok = false;
        Call(g_applyUpgrade, h, {def, &v}, "ActorUpgradeHandler.ApplyUpgrade", &ok);
        if (ok) ++n;
    }
    return n;
}

// SetUpgradeLevel(def, to), then the effects applied directly too (ApplyEffects); for a discoverable upgrade
// reset to -1, level 0's components come back if missing (it stays discovered). `note` gets what happened
// besides the level.
bool SetLevel(Il2CppObject* um, Il2CppObject* spend, Il2CppObject* def, int to, std::string& note) {
    int32_t v = to;
    bool ok = false;
    Call(g_setLevel, um, {def, &v}, "UpgradeModel.SetUpgradeLevel", &ok);
    if (Level(um, def) != to) return false;
    const int handlers = ApplyEffects(um, def, to);
    if (!ok) {
        // The level is stored; the exception came from a subscriber of OnUpgradeChanged (logged above).
        SR2_WARN("[{}] '{}': SetUpgradeLevel stored level {} but threw; effects applied directly on {} handler(s)",
                 kModule, unity::GetName(def), to + 1, handlers);
        if (!handlers) return false;
    }
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
        if (SetLevel(um, nullptr, def, last, note)) {
            ++raised;
            list += std::format("{}{} {} -> {}", list.empty() ? "" : ", ", Info(def).name, from < 0 ? "-" : Roman(from + 1),
                                Roman(last + 1));
        } else {
            ++failed;
        }
    }
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
            std::string optional;  // the direct re-apply is a backstop only
            g_handlerClass = cls(kImage, "", "ActorUpgradeHandler", optional);
            g_applyUpgrade = method(g_handlerClass, "ApplyUpgrade", 2, optional);
            off(upModel, "participants", g_offUmParticipants, optional);
            if (!optional.empty()) {
                g_applyUpgrade = nullptr;
                SR2_WARN("[{}] upgrades manager: effects are applied by the game's event only ({})", kModule, optional);
            }
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
    if (!Live() || !snapshot) return;
    if (GetTickCount64() - g_lastPublish >= kPublishMs) Publish();
}

}  // namespace sr2::modules::sandbox
