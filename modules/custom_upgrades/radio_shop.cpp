// custom_upgrades: the Slime Radio in the shop (see radio_shop.h). SR2 1.3.0, all lookups by name; RVAs for
// reference only.
//
// Shop data (scriptableobjectlookupdata / shop bundles read with UnityPy, 1.3.0):
//   * The ranch shop's toys are in the "DailyDeals" category (ShopItemCategoryDescription, analytics name
//     "DailyDeals"): rule set DailyDeals_BaseToysRuleSet (count limit 1 per restock) over the table
//     DailyDeals_BaseToysTable (22 toys, 500..2000 Newbucks, each limited to 1). The Night Light's entry:
//     asset GUID 6b6134bfd2a4907479a87c894d40324c, reference / unique id "ToyDefinition.NightLight",
//     filter flags 12 (TOY | SINGLE_ITEM), 500 Newbucks. The Boombox fabricates for 1000 Newbucks + items.
//
// Disassembly (1.3.0):
//   * ShopCategoryRuntime.GetRuntimeItems (0x8B37C0): when _cachedItemList is empty it builds one
//     ShopRuntimeItem per provider entry (ctor 0x8C1960: _owningCategory, _sourceProvider, _transactor from
//     the ShopCategoryContext, then UpdateWithImpl: _itemEntry, _assetFilterFlags = the asset reference's
//     flags | SINGLE_ITEM for individual items, _runtimeHelper = the category's CreateRuntimeHelper(entry)),
//     sorts it and returns the list itself. Called by the shop UI and ShopRuntime.GetInStockItemCount.
//   * Everything the UI shows comes through ShopRuntimeItem.get_Asset (0x8C1E20, the loaded handle's
//     result): get_Title / get_Icon / get_SmallIcon / get_FullArtReference / get_RequiresFullArt (the asset
//     as IUIDisplayData / IFullArtAndIconSource; icon and art only while _loadHandle is valid and done),
//     get_ItemType / GetAcquirableCount / CanAcquireIgnoringCosts (the runtime helper with the asset),
//     get_Description (0x8C1EC0: the helper's GetDescription(asset) = the asset's pedia entry text).
//     ShopItemButton.RepopulateDisplay, ShopItemDescriptionBlurb, ShopPurchasePopup read those getters.
//   * ToyShopItemHelper: GetTypeInfo = TOY_INDIVIDUAL, CanAcquireIgnoringCosts = true, GetAcquirableCount =
//     999, GetOwnedCount / GetMaxCanOwnCount = -1, GetPediaEntry casts the asset to ToyDefinition (throws
//     for anything else), so the displayed asset must be a ToyDefinition.
//   * ShopRuntimeItem.TryPurchase (0x8C1040; only caller ShopContentPage's purchase popup, with the page's
//     _spawnHandlerId): CanAcquireIgnoringCosts, transactor.CanAfford, AcquisitionUtility.Acquire_SpawnOrStore
//     (asset) whose spawn callback queues the IdentifiableType at ShopDataModel.QueueToSpawnItem(spawnHandlerId)
//     and raises PendingSpawnItemsChanged (= ShopDirector.QueueToSpawnItem 0x8B9530), then
//     ShopRuntimeTransactor.Purchase (0x8DE1C0: SpendableDirector.Spend(PurchaseCost, count) and
//     ShopDirector.TrackItemPurchased = ShopDataModel.TrackItemPurchase(category, unique id), saved),
//     NotifyPurchaseComplete (analytics), the category updater.
//   * ShopDataModel saves _shopPurchasesByCategory and _itemsPendingSpawn (Dictionary<string,
//     Queue<IdentifiableType>>). ShopItemSpawner.DispenseNextItem (0x8BB210) calls ShopDirector.SpawnNextItem
//     (0x8B9590: dequeue, InstantiateActor at the spawner, out identSpawned) every few seconds.
//   * AssetInterface<T>.LoadAsset returns the AssetReference's own operation when it is valid and
//     ShopRuntimeItem.ReleaseResources releases the AssetReference, so two items must not share one
//     AssetReference object: ours gets its own (same GUID).
//
// What we do:
//   * GetRuntimeItems detour (after the original): for the Daily Deals category (analytics name, or any toy
//     in it), while the radio card is unlocked (dj_buildables' rule: the Boombox blueprint or the sidecar
//     override), our item is inserted after the last toy (at the end without one). It is a MemberwiseClone
//     of a toy item of that list (owning category, transactor, provider = the toy section) with our own
//     ShopCategoryItemEntry: the Night Light's asset reference (own AssetReference of its GUID; reference /
//     unique id "ToyDefinition.NightLight"), our price (kPrice Newbucks), no count limit; runtime helper
//     ToyShopItemHelper.Instance, no category updater, load handle cleared (the UI loads it like any item).
//   * get_Asset detour: our item shows a runtime ToyDefinition clone of the Night Light ("Slime Radio", the
//     Boombox's icon and full-art picture). get_Description: our text. Both pointer compares only.
//   * TryPurchase detour for our item: affordability, SpendableDirector.Spend, `count` Night Lights queued
//     at the page's spawner (ShopDirector.QueueToSpawnItem), the pending count raised. Nothing tracked in
//     the shop's purchase records. No spawner id: the radios spawn in front of the player (dj Grant).
//   * SpawnNextItem detour (after the original): a Night Light while radios are pending is listed as a
//     Slime Radio (dj::ListRadio) and the count lowered. The tick lowers the count to the Night Lights
//     still queued (a queue lost without the sidecar, Night Lights dispensed while custom upgrades were off).
//   If anything is missing the radio is simply not offered; with the module faulted the detours pass
//   through, and our item (still in the list until the next restock) is a plain Night Light at our price.

#include "modules/custom_upgrades/radio_shop.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <format>
#include <initializer_list>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "core/detour_guard.h"
#include "core/il2cpp_api.h"
#include "core/log.h"
#include "core/main_thread.h"
#include "core/module.h"
#include "core/resolver.h"
#include "core/text_override.h"
#include "core/unity.h"
#include "modules/custom_upgrades/buildables.h"
#include "modules/custom_upgrades/dj_buildables.h"

namespace sr2::modules::custom_upgrades::radio_shop {
namespace {

namespace unity = sr2::core::unity;
namespace main_thread = sr2::core::main_thread;
using sr2::il2cpp::api;
using sr2::il2cpp::Resolver;

constexpr const char* kImage = "Assembly-CSharp";
constexpr const char* kShop = "MonomiPark.SlimeRancher.Shop";
constexpr const char* kShopRuntime = "MonomiPark.SlimeRancher.Shop.Runtime";
constexpr const char* kPendingKey = "dj.shop_radios_pending";
constexpr const char* kCategory = "DailyDeals";                          // analytics name of the toy category
constexpr const char* kNightLightGuid = "6b6134bfd2a4907479a87c894d40324c";  // DailyDeals_BaseToysTable (1.3.0)
constexpr int32_t kPrice = 1000;      // Newbucks (vanilla toys 500..2000; the Boombox fabricates for 1000 + items)
constexpr int32_t kFilterToy = 8 | 4;  // ShopItemFilterFlags TOY | SINGLE_ITEM (as the Night Light's entry)
constexpr int kMaxItems = 4;           // shop categories we serve at once (one Daily Deals per shop)
constexpr const char* kTitle = "Slime Radio";
constexpr const char* kDesc =
    "A little radio toy that plays your Slime DJ song wherever it goes; slimes love to play with it. It comes out "
    "of the shop's dispenser like any toy.";

bool IsA(Il2CppObject* obj, Il2CppClass* k) {
    const auto& a = api();
    if (!obj || !k || !a.il2cpp_object_get_class) return false;
    Il2CppClass* c = a.il2cpp_object_get_class(obj);
    if (c == k) return true;
    return c && a.il2cpp_class_is_assignable_from && a.il2cpp_class_is_assignable_from(k, c);
}

// Our items (any thread reads, main thread writes): the hot getters only compare pointers.
std::atomic<Il2CppObject*> g_items[kMaxItems];
std::atomic<Il2CppObject*> g_display{nullptr};  // the ToyDefinition clone shown for our items
std::atomic<Il2CppObject*> g_desc{nullptr};     // our description LocalizedString

bool IsOurs(Il2CppObject* item) {
    if (!item) return false;
    for (auto& a : g_items)
        if (a.load(std::memory_order_relaxed) == item) return true;
    return false;
}

struct Impl {
    Host host;
    const char* module = "custom_upgrades";

    // classes
    Il2CppClass* itemClass = nullptr, *categoryClass = nullptr, *entryClass = nullptr, *assetRefClass = nullptr;
    Il2CppClass* toyHelperClass = nullptr, *identTypeClass = nullptr, *lsClass = nullptr, *queueBase = nullptr;
    // offsets
    size_t offItemChanged = 0, offOwning = 0, offUpdater = 0, offFilter = 0, offEntry = 0, offHelper = 0, offSoldOut = 0;
    size_t offLoadHandle = 0, offLoadCallbacks = 0, offRefCount = 0, offCached = 0;
    size_t offSarInterface = 0, offSarFilter = 0, offSarRefId = 0, offSarUnique = 0, offAiRef = 0, offGuid = 0;
    size_t offSicItem = 0, offSicUnlock = 0, offSicCost = 0, offPcCur = 0, offPcIdent = 0, offPcComp = 0;
    size_t offLocName = 0, offIcon = 0, offFullArt = 0, offNeedsArt = 0, offRefId = 0, offHashInit = 0;
    size_t offScShop = 0, offScSpendable = 0, offSdModel = 0, offDmPending = 0;
    // methods
    const MethodInfo* clone = nullptr, *toyHelperInstance = nullptr, *newbucks = nullptr, *entryCtor = nullptr;
    const MethodInfo* canAfford = nullptr, *getCost = nullptr, *getSortGroup = nullptr, *getSortIndex = nullptr;
    const MethodInfo* analyticsName = nullptr, *spend = nullptr, *queueSpawn = nullptr, *actorIdOf = nullptr;
    const MethodInfo* sceneInst = nullptr, *setHideFlags = nullptr, *lsCtor = nullptr, *lsGetTable = nullptr, *lsGetEntry = nullptr;
    using ObjectNewFn = Il2CppObject* (*)(Il2CppClass*);
    using WBarrierFn = void (*)(Il2CppObject*, void**, void*);
    ObjectNewFn objectNew = nullptr;
    WBarrierFn wbarrier = nullptr;

    // state (main thread)
    dj::RadioShopInfo info;      // refreshed by the tick
    bool active = false;         // offer the radio now (tick: on, save, unlocked, text sink live, display built)
    bool live = false;           // dispensed Night Lights may become radios (tick: on, save, radio definition built)
    bool displayFailed = false;
    Il2CppObject* toyHelper = nullptr;  // ToyShopItemHelper.Instance
    struct Item {
        Il2CppObject* category = nullptr;
        Il2CppObject* item = nullptr;
        void* cost[3] = {};      // our PurchaseCost {_currencyCosts, IdentCosts, ComponentCosts} (held by the entry)
        Il2CppGCHandle handle = nullptr;
        bool logged = false;
    };
    std::vector<Item> items;
    std::unordered_map<Il2CppObject*, bool> decided;  // category -> is the Daily Deals (decided once it has items)
    std::vector<std::string> seenNames;
    bool loggedCreateFail = false;
    struct Live {
        const void* klass = nullptr;
        const MethodInfo* insert = nullptr, *remove = nullptr, *add = nullptr, *ctor = nullptr, *refCtor = nullptr;
    };
    Live listM, curListM, refM;
    size_t offDictEntries = 0, offDictCount = 0, offQArray = 0, offQHead = 0, offQSize = 0;
    const void* dictClass = nullptr, *queueClass = nullptr;

    // ---- helpers ----
    Il2CppObject* Call(const MethodInfo* m, void* obj, std::initializer_list<void*> args, const char* what, bool* ok = nullptr) {
        if (!m) {
            if (ok) *ok = false;
            return nullptr;
        }
        std::vector<void*> params(args);
        std::string err;
        Il2CppObject* r = Resolver::Get().Invoke(m, obj, params.empty() ? nullptr : params.data(), &err);
        if (ok) *ok = err.empty();
        if (!err.empty()) {
            if (err.rfind("native exception", 0) == 0)
                sr2::core::MarkModuleFaulted(module, std::format("radio shop: {}: {}", what, err));
            else SR2_WARN("[{}] radio shop: {} failed: {}", module, what, err);
        }
        return r;
    }
    template <class T>
    static T Value(Il2CppObject* boxed, T def = T{}) {
        return boxed ? *reinterpret_cast<T*>(reinterpret_cast<char*>(boxed) + 2 * sizeof(void*)) : def;
    }
    static char* Unbox(Il2CppObject* boxed) { return boxed ? reinterpret_cast<char*>(boxed) + 2 * sizeof(void*) : nullptr; }
    void SetRef(Il2CppObject* obj, size_t offset, void* value) {
        void** slot = reinterpret_cast<void**>(reinterpret_cast<char*>(obj) + offset);
        if (wbarrier) wbarrier(obj, slot, value);
        else *slot = value;
    }
    bool Faulted() const { return sr2::core::ModuleFaulted(module); }
    Il2CppObject* SceneField(size_t off) {
        bool ok = false;
        Il2CppObject* sc = Call(sceneInst, nullptr, {}, "SceneContext.Instance", &ok);
        return ok && unity::IsAlive(sc) ? unity::Field<Il2CppObject*>(sc, off) : nullptr;
    }
    static std::optional<size_t> OffsetUp(Il2CppClass* k, const char* field) {
        for (Il2CppClass* c = k; c; c = api().il2cpp_class_get_parent ? api().il2cpp_class_get_parent(c) : nullptr)
            if (auto o = Resolver::Get().FieldOffset(c, field, nullptr)) return o;
        return std::nullopt;
    }
    Live& LiveOf(Live& l, Il2CppObject* obj) {
        Il2CppClass* k = api().il2cpp_object_get_class(obj);
        if (k != l.klass) {
            Resolver& R = Resolver::Get();
            l = {};
            l.klass = k;
            l.insert = R.Method(k, "Insert", 2, nullptr);
            l.remove = R.Method(k, "Remove", 1, nullptr);
            l.add = R.Method(k, "Add", 1, nullptr);
            l.ctor = R.Method(k, ".ctor", 0, nullptr);
            l.refCtor = R.Method(k, ".ctor", 1, nullptr);
        }
        return l;
    }
    static int IndexIn(void* list, Il2CppObject* item) {
        for (size_t i = 0, n = unity::ListCount(list); i < n && i < 512; ++i)
            if (unity::ListAt(list, i) == item) return static_cast<int>(i);
        return -1;
    }
    Il2CppObject* NewObject(Il2CppClass* k, Live& l, Il2CppObject* likeObj, const char* what) {  // new k() via its .ctor()
        if (!k || !objectNew) return nullptr;
        Il2CppObject* o = objectNew(k);
        LiveOf(l, likeObj);
        bool ok = false;
        if (o && l.ctor) Call(l.ctor, o, {}, what, &ok);
        return o && ok ? o : nullptr;
    }
    // A new AssetReference of `like`'s class for `guid` (AssetReferenceT<T>(string)).
    Il2CppObject* NewAssetRef(Il2CppObject* like, const char* guid) {
        if (!like || !objectNew) return nullptr;
        LiveOf(refM, like);
        Il2CppObject* r = refM.refCtor ? objectNew(api().il2cpp_object_get_class(like)) : nullptr;
        bool ok = false;
        if (r) Call(refM.refCtor, r, {unity::NewString(guid)}, "AssetReference..ctor(guid)", &ok);
        return r && ok ? r : nullptr;
    }
    Il2CppObject* NewLocalized(Il2CppObject* tmpl, const char* text) {
        if (!IsA(tmpl, lsClass)) return nullptr;
        bool ok = false;
        Il2CppObject* table = Call(lsGetTable, tmpl, {}, "LocalizedString.get_TableReference", &ok);
        Il2CppObject* entry = ok ? Call(lsGetEntry, tmpl, {}, "LocalizedString.get_TableEntryReference", &ok) : nullptr;
        Il2CppObject* ls = ok && table && entry ? objectNew(lsClass) : nullptr;
        if (ls) Call(lsCtor, ls, {Unbox(table), Unbox(entry)}, "LocalizedString..ctor", &ok);
        if (!ls || !ok || !unity::NewGCHandle(ls)) return nullptr;
        sr2::core::text_override::Set(ls, text);
        return ls;
    }
    // A toy entry (its runtime helper is a ToyShopItemHelper).
    bool Toy(Il2CppObject* item) const { return item && IsA(unity::Field<Il2CppObject*>(item, offHelper), toyHelperClass); }
    int Pending() {
        try {
            const std::string v = host.value(kPendingKey);
            return v.empty() ? 0 : std::max(0, std::stoi(v));
        } catch (...) {
            return 0;
        }
    }
    void SetPending(int n) { host.setValue(kPendingKey, n > 0 ? std::to_string(n) : std::string()); }

    bool EnsureDisplay();
    bool IsToyCategory(Il2CppObject* category, void* list);
    Il2CppObject* MakeItem(Il2CppObject* category, void* list, Item& rec, std::string& what);
    void Serve(Il2CppObject* category, Il2CppObject* list);
    void RemoveAll(const char* why);
    bool Purchase(Il2CppObject* item, int32_t count, Il2CppString* spawnHandlerId);
    void Dispensed(Il2CppObject* go);
    int QueuedNightLights();
};

Impl* g = nullptr;
sr2::core::DetourGuard g_guard{"custom_upgrades", "radio shop"};
using GetItemsFn = Il2CppObject* (*)(Il2CppObject*, Il2CppObject*, const MethodInfo*);
using GetObjFn = Il2CppObject* (*)(Il2CppObject*, const MethodInfo*);
using TryPurchaseFn = bool (*)(Il2CppObject*, int32_t, Il2CppString*, void*, const MethodInfo*);
using SpawnNextFn = Il2CppObject* (*)(Il2CppObject*, Il2CppString*, Il2CppObject*, void*, void*, Il2CppObject**, const MethodInfo*);
GetItemsFn g_origGetItems = nullptr;     // ShopCategoryRuntime.GetRuntimeItems
GetObjFn g_origAsset = nullptr;          // ShopRuntimeItem.get_Asset
GetObjFn g_origDesc = nullptr;           // ShopRuntimeItem.get_Description
TryPurchaseFn g_origPurchase = nullptr;  // ShopRuntimeItem.TryPurchase
SpawnNextFn g_origSpawnNext = nullptr;   // ShopDirector.SpawnNextItem

// ---------------------------------------------------------------- display --

// The ToyDefinition our items show: a clone of the Night Light (never saved, never queued: purchases queue the
// vanilla Night Light) named "Slime Radio", with the Boombox's icon and full-art picture.
bool Impl::EnsureDisplay() {
    Il2CppObject* have = g_display.load();
    if (have && unity::IsAlive(have) && g_desc.load()) return true;
    if (displayFailed || !info.toyDef || !info.boombox) return false;
    Il2CppObject* d = unity::Instantiate(info.toyDef, nullptr);
    if (!IsA(d, identTypeClass) || !unity::IsAlive(d) || d == info.toyDef || !unity::NewGCHandle(d)) {
        displayFailed = true;
        SR2_WARN("[{}] radio shop: the Night Light could not be cloned for the shop entry - the Slime Radio is not sold",
                 module);
        return false;
    }
    int32_t flags = 32;  // HideFlags.DontUnloadUnusedAsset
    Call(setHideFlags, d, {&flags}, "Object.set_hideFlags");
    unity::SetName(d, "SlimeDJRadioShop");
    unity::Field<Il2CppObject*>(d, offRefId) = nullptr;  // ReferenceId: from the new name (never the Night Light's)
    unity::Field<bool>(d, offHashInit) = false;
    Il2CppObject* name = NewLocalized(unity::Field<Il2CppObject*>(info.toyDef, offLocName), kTitle);
    Il2CppObject* desc = NewLocalized(unity::Field<Il2CppObject*>(info.toyDef, offLocName), kDesc);
    if (!name || !desc) {
        displayFailed = true;
        SR2_WARN("[{}] radio shop: no LocalizedString for the shop entry - the Slime Radio is not sold", module);
        return false;
    }
    SetRef(d, offLocName, name);
    SetRef(d, offIcon, unity::Field<Il2CppObject*>(info.boombox, offIcon));
    // The Boombox's picture (its own AssetReference: the purchase view loads and releases it).
    Il2CppObject* art = unity::Field<Il2CppObject*>(info.boombox, offFullArt);
    Il2CppString* guid = art && offGuid ? unity::Field<Il2CppString*>(art, offGuid) : nullptr;
    const std::string g8 = unity::ToUtf8(guid);
    Il2CppObject* mine = g8.empty() ? nullptr : NewAssetRef(art, g8.c_str());
    SetRef(d, offFullArt, mine);
    unity::Field<bool>(d, offNeedsArt) = mine != nullptr;
    g_display.store(d);
    g_desc.store(desc);
    SR2_INFO("[{}] radio shop: shop entry look built (Night Light clone '{}', Boombox icon, {})", module, unity::GetName(d),
             mine ? "Boombox picture" : "no picture (icon only)");
    return true;
}

// ---------------------------------------------------------------- injection --

bool Impl::IsToyCategory(Il2CppObject* category, void* list) {
    if (auto it = decided.find(category); it != decided.end()) return it->second;
    const size_t n = unity::ListCount(list);
    if (n == 0) return false;  // decided once it has items
    bool ok = false;
    const std::string name = unity::ToUtf8(reinterpret_cast<Il2CppString*>(Call(analyticsName, category, {}, "ShopCategoryRuntime.get_AnalyticsName", &ok)));
    bool toy = ok && name == kCategory;
    for (size_t i = 0; !toy && i < n && i < 512; ++i)
        if (auto* it = static_cast<Il2CppObject*>(unity::ListAt(list, i)); it && Toy(it))
            toy = true;
    decided[category] = toy;
    if (std::find(seenNames.begin(), seenNames.end(), name) == seenNames.end() && seenNames.size() < 16) {
        seenNames.push_back(name);
        SR2_INFO("[{}] radio shop: shop category '{}' ({} item(s)): {}", module, name.empty() ? "?" : name, n,
                 toy ? "the toy category, the Slime Radio is sold here" : "not the toy category");
    }
    return toy;
}

// Our ShopRuntimeItem for `category` (see the header). nullptr and `what` on failure.
Il2CppObject* Impl::MakeItem(Il2CppObject* category, void* list, Item& rec, std::string& what) {
    // The template: the last toy of the list (its provider = the toy section), else the last item.
    Il2CppObject* src = nullptr;
    for (size_t i = 0, n = unity::ListCount(list); i < n && i < 512; ++i) {
        auto* it = static_cast<Il2CppObject*>(unity::ListAt(list, i));
        if (!IsA(it, itemClass) || IsOurs(it) || !unity::Field<Il2CppObject*>(it, offEntry)) continue;
        if (!src || Toy(it) ||
            !Toy(src))
            src = it;
    }
    if (!src) return what = "no item to copy", nullptr;
    bool ok = false;
    Il2CppObject* costBox = Call(getCost, src, {}, "ShopRuntimeItem.get_ShopItemCost", &ok);
    char* sc = ok ? Unbox(costBox) : nullptr;
    Il2CppObject* srcRef = sc ? *reinterpret_cast<Il2CppObject**>(sc + offSicItem) : nullptr;
    void* srcCur = sc ? *reinterpret_cast<void**>(sc + offSicCost + offPcCur) : nullptr;
    void* srcIdent = sc ? *reinterpret_cast<void**>(sc + offSicCost + offPcIdent) : nullptr;
    void* srcComp = sc ? *reinterpret_cast<void**>(sc + offSicCost + offPcComp) : nullptr;
    Il2CppObject* srcAi = IsA(srcRef, assetRefClass) ? unity::Field<Il2CppObject*>(srcRef, offSarInterface) : nullptr;
    Il2CppObject* srcAr = srcAi ? unity::Field<Il2CppObject*>(srcAi, offAiRef) : nullptr;
    if (!srcAr || !srcCur) return what = "the template item has no asset reference / Newbucks cost", nullptr;
    // Asset reference: the Night Light's (own AssetInterface and AssetReference objects).
    Il2CppObject* ar = NewAssetRef(srcAr, kNightLightGuid);
    Il2CppObject* ai = ar ? Call(clone, srcAi, {}, "AssetInterface.MemberwiseClone", &ok) : nullptr;
    Il2CppObject* sar = ai && ok ? Call(clone, srcRef, {}, "ShopItemAssetReference.MemberwiseClone", &ok) : nullptr;
    if (!sar || !ok) return what = "asset reference", nullptr;
    SetRef(ai, offAiRef, ar);
    SetRef(sar, offSarInterface, ai);
    unity::Field<int32_t>(sar, offSarFilter) = kFilterToy;
    SetRef(sar, offSarRefId, unity::NewString(info.toyRefId));
    SetRef(sar, offSarUnique, unity::NewString(info.toyRefId));  // ShopItemAssetReference unique id = the reference id
    // Price: kPrice Newbucks; empty item / component costs (new lists of the template's classes).
    int32_t price = kPrice;
    Il2CppObject* entryCost = Call(newbucks, nullptr, {&price}, "CurrencyCostEntry.CreateNewbucks", &ok);
    Il2CppObject* cur = entryCost && ok ? NewObject(api().il2cpp_object_get_class(static_cast<Il2CppObject*>(srcCur)), curListM,
                                                    static_cast<Il2CppObject*>(srcCur), "List<CurrencyCostEntry>..ctor")
                                        : nullptr;
    if (cur && curListM.add) Call(curListM.add, cur, {entryCost}, "List<CurrencyCostEntry>.Add", &ok);
    Live tmp;
    Il2CppObject* ident = srcIdent ? NewObject(api().il2cpp_object_get_class(static_cast<Il2CppObject*>(srcIdent)), tmp,
                                               static_cast<Il2CppObject*>(srcIdent), "List<IdentCostEntry>..ctor")
                                   : nullptr;
    Live tmp2;
    Il2CppObject* comp = srcComp ? NewObject(api().il2cpp_object_get_class(static_cast<Il2CppObject*>(srcComp)), tmp2,
                                             static_cast<Il2CppObject*>(srcComp), "List<UpgradeComponent>..ctor")
                                 : nullptr;
    if (!cur || unity::ListCount(cur) != 1 || (srcIdent && !ident) || (srcComp && !comp)) return what = "price lists", nullptr;
    // Entry: ShopCategoryItemEntry(in ShopItemCost, Optional<int> none, sortIndex, sortGroup).
    alignas(16) unsigned char cost[96] = {};
    *reinterpret_cast<Il2CppObject**>(cost + offSicItem) = sar;
    *reinterpret_cast<int32_t*>(cost + offSicUnlock) = 0;  // AcquireGadgetType.INDIVIDUAL_ITEM
    *reinterpret_cast<Il2CppObject**>(cost + offSicCost + offPcCur) = cur;
    *reinterpret_cast<Il2CppObject**>(cost + offSicCost + offPcIdent) = ident;
    *reinterpret_cast<Il2CppObject**>(cost + offSicCost + offPcComp) = comp;
    alignas(8) unsigned char noLimit[16] = {};
    int32_t sortGroup = Value<int32_t>(Call(getSortGroup, src, {}, "ShopRuntimeItem.get_SortGroup", &ok));
    int32_t sortIndex = Value<int32_t>(Call(getSortIndex, src, {}, "ShopRuntimeItem.get_SortIndex", &ok)) + 1000;
    Il2CppObject* entry = objectNew(entryClass);
    if (entry) Call(entryCtor, entry, {cost, noLimit, &sortIndex, &sortGroup}, "ShopCategoryItemEntry..ctor", &ok);
    if (!entry || !ok) return what = "item entry", nullptr;
    // The item: a copy of the template with our entry, the toy helper, no updater / listeners / load state.
    Il2CppObject* item = Call(clone, src, {}, "ShopRuntimeItem.MemberwiseClone", &ok);
    if (!item || !ok || unity::Field<Il2CppObject*>(item, offOwning) != category) return what = "item copy", nullptr;
    SetRef(item, offItemChanged, nullptr);
    SetRef(item, offUpdater, nullptr);
    SetRef(item, offEntry, entry);
    SetRef(item, offHelper, toyHelper);
    SetRef(item, offLoadCallbacks, nullptr);
    std::memset(reinterpret_cast<char*>(item) + offLoadHandle, 0, offLoadCallbacks - offLoadHandle);
    unity::Field<int32_t>(item, offRefCount) = 0;
    unity::Field<bool>(item, offSoldOut) = false;
    unity::Field<int32_t>(item, offFilter) = kFilterToy;
    rec.cost[0] = cur, rec.cost[1] = ident, rec.cost[2] = comp;
    what = Toy(src) ? "copied from a toy entry"
                                                                      : "no toy in stock: copied from another entry";
    return item;
}

// After GetRuntimeItems: our item in `list` (the category's own item list) while wanted, out of it otherwise.
void Impl::Serve(Il2CppObject* category, Il2CppObject* list) {
    if (!list || !category || !IsA(category, categoryClass)) return;
    if (unity::Field<Il2CppObject*>(category, offCached) != list) return;  // only the category's own list
    Item* rec = nullptr;
    for (Item& r : items)
        if (r.category == category) rec = &r;
    if (!active) {
        if (rec && rec->item) {
            LiveOf(listM, list);
            const int at = IndexIn(list, rec->item);
            bool ok = false;
            if (at >= 0 && listM.remove) Call(listM.remove, list, {rec->item}, "List<ShopRuntimeItem>.Remove", &ok);
        }
        return;
    }
    if (!rec) {
        if (!IsToyCategory(category, list)) return;
        if (items.size() >= static_cast<size_t>(kMaxItems)) return;
        Item fresh;
        fresh.category = category;
        std::string what;
        fresh.item = MakeItem(category, list, fresh, what);
        if (!fresh.item) {
            if (!loggedCreateFail) SR2_WARN("[{}] radio shop: shop entry not made ({}) - the Slime Radio is not sold", module, what);
            loggedCreateFail = true;
            return;
        }
        fresh.handle = unity::NewGCHandle(fresh.item);
        g_items[items.size()].store(fresh.item);
        items.push_back(fresh);
        rec = &items.back();
        SR2_INFO("[{}] radio shop: Slime Radio shop entry made ({}; {} Newbucks, a Night Light dressed as a radio when it "
                 "is dispensed)", module, what, kPrice);
    }
    if (IndexIn(list, rec->item) >= 0) return;
    // After the last toy, else at the end.
    int at = -1;
    const size_t n = unity::ListCount(list);
    for (size_t i = 0; i < n && i < 512; ++i)
        if (auto* it = static_cast<Il2CppObject*>(unity::ListAt(list, i)); it && Toy(it))
            at = static_cast<int>(i) + 1;
    if (at < 0) at = static_cast<int>(n);
    LiveOf(listM, list);
    bool ok = false;
    int32_t index = at;
    if (listM.insert) Call(listM.insert, list, {&index, rec->item}, "List<ShopRuntimeItem>.Insert", &ok);
    if (ok && !rec->logged) {
        rec->logged = true;
        SR2_INFO("[{}] radio shop: Slime Radio added to the shop's toy category (position {} of {})", module, at + 1, n + 1);
    }
}

void Impl::RemoveAll(const char* why) {
    int removed = 0;
    for (Item& r : items) {
        Il2CppObject* list = r.category ? unity::Field<Il2CppObject*>(r.category, offCached) : nullptr;
        if (!list || IndexIn(list, r.item) < 0) continue;
        LiveOf(listM, list);
        bool ok = false;
        if (listM.remove) Call(listM.remove, list, {r.item}, "List<ShopRuntimeItem>.Remove", &ok);
        removed += ok;
    }
    if (removed) SR2_INFO("[{}] radio shop: {}: Slime Radio taken out of {} shop list(s)", module, why, removed);
}

// ---------------------------------------------------------------- purchase --

// Our TryPurchase: Newbucks spent, `count` vanilla Night Lights queued at the page's spawner, pending raised.
bool Impl::Purchase(Il2CppObject* item, int32_t count, Il2CppString* spawnHandlerId) {
    if (count <= 0 || count > 999 || !info.toyDef || !unity::IsAlive(info.toyDef)) return false;
    const Item* rec = nullptr;
    for (const Item& r : items)
        if (r.item == item) rec = &r;
    if (!rec) return false;
    bool ok = false;
    const bool afford = Value<bool>(Call(canAfford, item, {&count}, "ShopRuntimeItem.CanAfford", &ok)) && ok;
    if (!afford) {
        SR2_INFO("[{}] radio shop: {} Slime Radio(s) not bought (not enough Newbucks for {})", module, count, kPrice * count);
        return false;
    }
    Il2CppObject* spendable = SceneField(offScSpendable);
    Il2CppObject* shop = SceneField(offScShop);
    if (!unity::IsAlive(spendable)) return false;
    void* cost[3] = {rec->cost[0], rec->cost[1], rec->cost[2]};  // PurchaseCost (3 references)
    const bool spent = Value<bool>(Call(spend, spendable, {cost, &count}, "SpendableDirector.Spend", &ok)) && ok;
    if (!spent) {
        SR2_INFO("[{}] radio shop: {} Slime Radio(s) not bought (SpendableDirector.Spend refused)", module, count);
        return false;
    }
    const std::string source = unity::ToUtf8(spawnHandlerId);
    bool queued = false;
    if (!source.empty() && unity::IsAlive(shop)) Call(queueSpawn, shop, {spawnHandlerId, info.toyDef, &count}, "ShopDirector.QueueToSpawnItem", &queued);
    if (queued) {
        const int pending = Pending() + count;
        SetPending(pending);
        SR2_INFO("[{}] radio shop: {} Slime Radio(s) bought for {} Newbucks, queued at the shop's dispenser '{}' as Night "
                 "Light(s) ({} radio(s) pending)", module, count, kPrice * count, source, pending);
    } else {
        // No dispenser: in front of the player, as the Fabricator does (at most 5 per call).
        std::string result;
        for (int left = count; left > 0; left -= 5) dj::Grant(buildables::kSlimeRadio, std::min(left, 5), result);
        SR2_INFO("[{}] radio shop: {} Slime Radio(s) bought for {} Newbucks; no dispenser ({}) - spawned in front of you",
                 module, count, kPrice * count, source.empty() ? "no spawner id" : "no ShopDirector");
    }
    return true;
}

// After SpawnNextItem: a Night Light while radios are pending becomes a Slime Radio.
void Impl::Dispensed(Il2CppObject* go) {
    const int pending = Pending();
    if (pending <= 0) return;
    bool ok = false;
    const int64_t id = Value<int64_t>(Call(actorIdOf, nullptr, {go}, "Identifiable.GetActorId", &ok));
    if (!ok || !id) return;
    dj::ListRadio(id);
    SetPending(pending - 1);
    SR2_INFO("[{}] radio shop: the shop dispensed a Slime Radio (actor {}; {} still pending)", module, id, pending - 1);
}

// Night Lights in the shop's pending-spawn queues (ShopDataModel._itemsPendingSpawn, every spawner).
// Plain reads: Dictionary<string, Queue<IdentifiableType>> entries {int hashCode; int next; string key; Queue
// value} (24 bytes, hashCode < 0 = free), Queue {_array, _head, _size}. -1 = unknown.
int Impl::QueuedNightLights() {
    Il2CppObject* shop = SceneField(offScShop);
    Il2CppObject* model = unity::IsAlive(shop) ? unity::Field<Il2CppObject*>(shop, offSdModel) : nullptr;
    Il2CppObject* dict = model ? unity::Field<Il2CppObject*>(model, offDmPending) : nullptr;
    if (!dict) return -1;
    if (api().il2cpp_object_get_class(dict) != dictClass) {
        dictClass = api().il2cpp_object_get_class(dict);
        offDictEntries = OffsetUp(api().il2cpp_object_get_class(dict), "_entries").value_or(0);
        offDictCount = OffsetUp(api().il2cpp_object_get_class(dict), "_count").value_or(0);
    }
    if (!offDictEntries || !offDictCount) return -1;
    void* entries = unity::Field<void*>(dict, offDictEntries);
    const int32_t count = unity::Field<int32_t>(dict, offDictCount);
    if (!entries) return count == 0 ? 0 : -1;
    const size_t len = unity::ArrayLength(entries);
    int found = 0;
    for (int32_t i = 0; i < count && static_cast<size_t>(i) < len && i < 64; ++i) {
        char* e = unity::ArrayData<char>(entries) + static_cast<size_t>(i) * 24;
        if (*reinterpret_cast<int32_t*>(e) < 0) continue;
        auto* q = *reinterpret_cast<Il2CppObject**>(e + 16);
        if (!q) continue;
        if (api().il2cpp_object_get_class(q) != queueClass) {
            queueClass = api().il2cpp_object_get_class(q);
            offQArray = OffsetUp(api().il2cpp_object_get_class(q), "_array").value_or(0);
            offQHead = OffsetUp(api().il2cpp_object_get_class(q), "_head").value_or(0);
            offQSize = OffsetUp(api().il2cpp_object_get_class(q), "_size").value_or(0);
        }
        if (!offQArray || !offQSize) return -1;
        void* arr = unity::Field<void*>(q, offQArray);
        const int32_t head = unity::Field<int32_t>(q, offQHead), size = unity::Field<int32_t>(q, offQSize);
        const size_t alen = arr ? unity::ArrayLength(arr) : 0;
        for (int32_t k = 0; alen && k < size && k < 4096; ++k)
            if (unity::ArrayAt(arr, (static_cast<size_t>(head) + k) % alen) == info.toyDef) ++found;
    }
    return found;
}

// ---------------------------------------------------------------- hooks --

Il2CppObject* GetRuntimeItemsDetour(Il2CppObject* self, Il2CppObject* eventDirector, const MethodInfo* mi) {
    Il2CppObject* list = g_origGetItems(self, eventDirector, mi);
    if (g && list && main_thread::IsMainThread() && !g->Faulted()) g_guard.Run([&] { g->Serve(self, list); });
    return list;
}

Il2CppObject* GetAssetDetour(Il2CppObject* self, const MethodInfo* mi) {
    if (IsOurs(self) && main_thread::IsMainThread()) {
        Il2CppObject* r = nullptr;
        if (g_guard.Run([&] { r = g_display.load(); }) && r) return r;
    }
    return g_origAsset(self, mi);
}

Il2CppObject* GetDescriptionDetour(Il2CppObject* self, const MethodInfo* mi) {
    if (IsOurs(self) && main_thread::IsMainThread()) {
        Il2CppObject* r = nullptr;
        if (g_guard.Run([&] { r = g_desc.load(); }) && r) return r;
    }
    return g_origDesc(self, mi);
}

bool TryPurchaseDetour(Il2CppObject* self, int32_t count, Il2CppString* spawnHandlerId, void* uiSessionId, const MethodInfo* mi) {
    if (g && IsOurs(self) && main_thread::IsMainThread() && !g->Faulted()) {
        bool r = false;
        // Never the original for our item once we tried (it would spend a second time): a failure = no purchase.
        g_guard.Run([&] { r = g->Purchase(self, count, spawnHandlerId); });
        return r;
    }
    return g_origPurchase(self, count, spawnHandlerId, uiSessionId, mi);
}

Il2CppObject* SpawnNextItemDetour(Il2CppObject* self, Il2CppString* sourceId, Il2CppObject* sceneGroup, void* position,
                                  void* rotation, Il2CppObject** identSpawned, const MethodInfo* mi) {
    Il2CppObject* go = g_origSpawnNext(self, sourceId, sceneGroup, position, rotation, identSpawned, mi);
    if (g && go && identSpawned && *identSpawned && *identSpawned == g->info.toyDef && g->live && main_thread::IsMainThread() &&
        !g->Faulted())
        g_guard.Run([&] { g->Dispensed(go); });
    return go;
}

}  // namespace

// ---------------------------------------------------------------- setup --

bool RadioShop::Setup(sr2::core::ModuleContext& ctx, Host host, std::string& why) {
    Resolver& R = ctx.resolver();
    auto* p = new Impl();
    p->host = std::move(host);
    auto parentOf = [](Il2CppClass* c) { return api().il2cpp_class_get_parent ? api().il2cpp_class_get_parent(c) : nullptr; };
    auto cls = [&](const char* image, const char* ns, const char* name) -> Il2CppClass* {
        std::string err;
        Il2CppClass* k = R.Class(image, ns, name, &err);
        if (!k && why.empty()) why = err.empty() ? std::format("class {}.{} not found", ns, name) : err;
        return k;
    };
    auto off = [&](Il2CppClass* k, const char* field, size_t& out) {
        if (auto o = k ? Impl::OffsetUp(k, field) : std::nullopt) out = *o;
        else if (why.empty()) why = std::format("field {} not found", field);
    };
    auto method = [&](Il2CppClass* k, const char* name, int argc) -> const MethodInfo* {
        for (Il2CppClass* c = k; c; c = parentOf(c))
            if (const MethodInfo* m = R.Method(c, name, argc, nullptr)) return m;
        if (why.empty()) why = std::format("method {}({}) not found", name, argc);
        return nullptr;
    };
    auto byParams = [&](Il2CppClass* k, const char* name, std::initializer_list<std::string_view> params) {
        std::string err;
        const MethodInfo* m = k ? R.MethodByParams(k, name, params, &err) : nullptr;
        if (!m && why.empty()) why = std::format("{}: {}", name, err);
        return m;
    };
    auto fail = [&] {
        delete p;
        return false;
    };
    HMODULE ga = api().module;
    p->objectNew = reinterpret_cast<Impl::ObjectNewFn>(ga ? GetProcAddress(ga, "il2cpp_object_new") : nullptr);
    p->wbarrier = reinterpret_cast<Impl::WBarrierFn>(ga ? GetProcAddress(ga, "il2cpp_gc_wbarrier_set_field") : nullptr);
    if (!p->objectNew) why = "il2cpp_object_new export missing";

    p->itemClass = cls(kImage, kShopRuntime, "ShopRuntimeItem");
    p->categoryClass = cls(kImage, kShopRuntime, "ShopCategoryRuntime");
    p->entryClass = cls(kImage, kShop, "ShopCategoryItemEntry");
    p->assetRefClass = cls(kImage, kShop, "ShopItemAssetReference");
    p->toyHelperClass = cls(kImage, kShop, "ToyShopItemHelper");
    p->identTypeClass = cls(kImage, "", "IdentifiableType");
    p->lsClass = cls("Unity.Localization", "UnityEngine.Localization", "LocalizedString");
    Il2CppClass* shopDir = cls(kImage, kShop, "ShopDirector");
    Il2CppClass* itemCost = cls(kImage, kShop, "ShopItemCost");
    Il2CppClass* purchaseCost = cls(kImage, "MonomiPark.SlimeRancher.Economy", "PurchaseCost");
    Il2CppClass* costEntry = cls(kImage, "MonomiPark.SlimeRancher.Economy", "CurrencyCostEntry");
    Il2CppClass* dataModel = cls(kImage, "MonomiPark.SlimeRancher.DataModel", "ShopDataModel");
    Il2CppClass* aiBase = cls(kImage, "MonomiPark.SlimeRancher.UI.Framework", "AssetInterfaceBase");
    Il2CppClass* assetRef = cls("Unity.Addressables", "UnityEngine.AddressableAssets", "AssetReference");
    Il2CppClass* spendable = cls(kImage, "MonomiPark.SlimeRancher.Player", "SpendableDirector");
    Il2CppClass* sceneCtx = cls(kImage, "", "SceneContext");
    Il2CppClass* identifiable = cls(kImage, "", "Identifiable");
    Il2CppClass* sysObject = cls("mscorlib", "System", "Object");
    Il2CppClass* uobject = cls("UnityEngine.CoreModule", "UnityEngine", "Object");
    if (!why.empty()) return fail();
    off(p->itemClass, "ItemChanged", p->offItemChanged);
    off(p->itemClass, "_owningCategory", p->offOwning);
    off(p->itemClass, "_categoryUpdater", p->offUpdater);
    off(p->itemClass, "_assetFilterFlags", p->offFilter);
    off(p->itemClass, "_itemEntry", p->offEntry);
    off(p->itemClass, "_runtimeHelper", p->offHelper);
    off(p->itemClass, "_isSoldOut", p->offSoldOut);
    off(p->itemClass, "_loadHandle", p->offLoadHandle);
    off(p->itemClass, "_loadCallbacks", p->offLoadCallbacks);
    off(p->itemClass, "_refCount", p->offRefCount);
    off(p->categoryClass, "_cachedItemList", p->offCached);
    off(p->assetRefClass, "_assetInterface", p->offSarInterface);
    off(p->assetRefClass, "_filterFlags", p->offSarFilter);
    off(p->assetRefClass, "_identifiableReferenceId", p->offSarRefId);
    off(p->assetRefClass, "_uniqueId", p->offSarUnique);
    off(aiBase, "_assetReference", p->offAiRef);
    off(assetRef, "m_AssetGUID", p->offGuid);
    off(itemCost, "ShopItem", p->offSicItem);
    off(itemCost, "UnlockType", p->offSicUnlock);
    off(itemCost, "PurchaseCost", p->offSicCost);
    off(purchaseCost, "_currencyCosts", p->offPcCur);
    off(purchaseCost, "IdentCosts", p->offPcIdent);
    off(purchaseCost, "ComponentCosts", p->offPcComp);
    off(p->identTypeClass, "localizedName", p->offLocName);
    off(p->identTypeClass, "icon", p->offIcon);
    off(p->identTypeClass, "_fullArt", p->offFullArt);
    off(p->identTypeClass, "_requiresFullArt", p->offNeedsArt);
    off(p->identTypeClass, "referenceId", p->offRefId);
    off(p->identTypeClass, "initializedHashId", p->offHashInit);
    off(sceneCtx, "<ShopDirector>k__BackingField", p->offScShop);
    off(sceneCtx, "<SpendableDirector>k__BackingField", p->offScSpendable);
    off(shopDir, "_dataModel", p->offSdModel);
    off(dataModel, "_itemsPendingSpawn", p->offDmPending);
    if (!why.empty()) return fail();
    // Value-type field offsets include the object header.
    for (size_t* o : {&p->offSicItem, &p->offSicUnlock, &p->offSicCost, &p->offPcCur, &p->offPcIdent, &p->offPcComp})
        *o = *o >= 2 * sizeof(void*) ? *o - 2 * sizeof(void*) : *o;
    if (p->offLoadCallbacks <= p->offLoadHandle || p->offLoadCallbacks - p->offLoadHandle > 0x40 ||
        p->offSicCost + p->offPcComp + sizeof(void*) > 96)
        why = "unexpected ShopRuntimeItem / ShopItemCost layout";

    p->clone = method(sysObject, "MemberwiseClone", 0);
    p->toyHelperInstance = method(p->toyHelperClass, "get_Instance", 0);
    p->newbucks = method(costEntry, "CreateNewbucks", 1);
    p->entryCtor = R.Method(p->entryClass, ".ctor", 4, nullptr);
    p->canAfford = method(p->itemClass, "CanAfford", 1);
    p->getCost = method(p->itemClass, "get_ShopItemCost", 0);
    p->getSortGroup = method(p->itemClass, "get_SortGroup", 0);
    p->getSortIndex = method(p->itemClass, "get_SortIndex", 0);
    p->analyticsName = method(p->categoryClass, "get_AnalyticsName", 0);
    p->spend = byParams(spendable, "Spend", {"MonomiPark.SlimeRancher.Economy.PurchaseCost", "System.Int32"});
    p->queueSpawn = method(shopDir, "QueueToSpawnItem", 3);
    p->actorIdOf = byParams(identifiable, "GetActorId", {"UnityEngine.GameObject"});
    p->sceneInst = method(parentOf(sceneCtx), "get_Instance", 0);
    p->setHideFlags = method(uobject, "set_hideFlags", 1);
    p->lsCtor = R.Method(p->lsClass, ".ctor", 2, nullptr);
    p->lsGetTable = method(p->lsClass, "get_TableReference", 0);
    p->lsGetEntry = method(p->lsClass, "get_TableEntryReference", 0);
    if (!p->entryCtor || !p->lsCtor) why = "constructors not found";
    if (!why.empty()) return fail();
    const MethodInfo* getItems = method(p->categoryClass, "GetRuntimeItems", 1);
    const MethodInfo* getAsset = method(p->itemClass, "get_Asset", 0);
    const MethodInfo* getDesc = method(p->itemClass, "get_Description", 0);
    const MethodInfo* tryPurchase = method(p->itemClass, "TryPurchase", 3);
    const MethodInfo* spawnNext = method(shopDir, "SpawnNextItem", 5);
    if (!why.empty()) return fail();
    void* ptrs[5] = {};
    const MethodInfo* mis[5] = {getItems, getAsset, getDesc, tryPurchase, spawnNext};
    for (int i = 0; i < 5; ++i)
        if (!(ptrs[i] = R.MethodPointer(mis[i], &why))) return fail();
    // None of these is hooked anywhere else (MinHook: one hook per address).
    if (!ctx.HookAddress("radio_shop.ShopCategoryRuntime.GetRuntimeItems", ptrs[0], reinterpret_cast<void*>(&GetRuntimeItemsDetour),
                         reinterpret_cast<void**>(&g_origGetItems)) ||
        !ctx.HookAddress("radio_shop.ShopRuntimeItem.get_Asset", ptrs[1], reinterpret_cast<void*>(&GetAssetDetour),
                         reinterpret_cast<void**>(&g_origAsset)) ||
        !ctx.HookAddress("radio_shop.ShopRuntimeItem.get_Description", ptrs[2], reinterpret_cast<void*>(&GetDescriptionDetour),
                         reinterpret_cast<void**>(&g_origDesc)) ||
        !ctx.HookAddress("radio_shop.ShopRuntimeItem.TryPurchase", ptrs[3], reinterpret_cast<void*>(&TryPurchaseDetour),
                         reinterpret_cast<void**>(&g_origPurchase)) ||
        !ctx.HookAddress("radio_shop.ShopDirector.SpawnNextItem", ptrs[4], reinterpret_cast<void*>(&SpawnNextItemDetour),
                         reinterpret_cast<void**>(&g_origSpawnNext))) {
        why = ctx.failure();
        return fail();
    }
    g = p;
    ready_ = true;
    return true;
}

// ---------------------------------------------------------------- api --

void RadioShop::Tick() {
    if (!ready_ || !g || !main_thread::IsMainThread() || g->Faulted()) return;
    Impl& p = *g;
    g_guard.Run([&] {
        const bool was = p.active;
        bool unlocked = false;
        const bool on = p.host.on && p.host.on();
        p.live = on && p.host.saveReady && p.host.saveReady() && dj::RadioShop(p.info);
        if (p.live) unlocked = p.info.unlocked;
        if (!p.toyHelper) {
            bool ok = false;
            Il2CppObject* h = p.Call(p.toyHelperInstance, nullptr, {}, "ToyShopItemHelper.get_Instance", &ok);
            if (ok && IsA(h, p.toyHelperClass) && unity::NewGCHandle(h)) p.toyHelper = h;
        }
        const bool sink = sr2::core::text_override::SinkActive();
        p.active = unlocked && sink && p.toyHelper && p.EnsureDisplay();
        if (p.active != was)
            SR2_INFO("[{}] radio shop: Slime Radio {} the shop{}", p.module, p.active ? "offered in" : "no longer offered in",
                     p.active  ? ""
                     : !p.live ? " (custom upgrades off, no save or no radio definition)"
                     : !unlocked ? " (radio card locked: needs the Boombox blueprint)"
                     : !sink     ? " (text sink not live)"
                                 : "");
        if (!p.active && was) p.RemoveAll("not offered");
        // Pending radios: at most the Night Lights still queued at the shop's spawners.
        const int pending = p.live ? p.Pending() : 0;
        if (pending > 0) {
            const int queued = p.QueuedNightLights();
            if (queued >= 0 && queued < pending) {
                p.SetPending(queued);
                SR2_INFO("[{}] radio shop: {} pending Slime Radio(s) but {} Night Light(s) queued at the shop - pending set to {}",
                         p.module, pending, queued, queued);
            }
        }
    });
}

void RadioShop::Cleanup(const char* why) {
    if (!ready_ || !g || !main_thread::IsMainThread()) return;
    Impl& p = *g;
    p.active = p.live = false;
    if (!p.Faulted()) g_guard.Run([&] { p.RemoveAll(why); });
}

void RadioShop::Reset() {
    if (!g) return;
    Impl& p = *g;
    std::vector<Il2CppGCHandle> handles;
    for (Impl::Item& r : p.items)
        if (r.handle) handles.push_back(r.handle);
    if (!handles.empty()) main_thread::Post([handles] {
        for (Il2CppGCHandle h : handles) unity::FreeGCHandle(h);
    });
    for (auto& a : g_items) a.store(nullptr);
    p.items.clear();
    p.decided.clear();
    p.active = p.live = false;  // the tick decides again for the loaded save
}

}  // namespace sr2::modules::custom_upgrades::radio_shop
