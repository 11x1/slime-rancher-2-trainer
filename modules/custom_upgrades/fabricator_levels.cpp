// custom_upgrades: more level circles in the Fabricator (see fabricator_levels.h). SR2 1.3.0, all
// lookups by name; RVAs for reference only.

#include "modules/custom_upgrades/fabricator_levels.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <format>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "core/detour_guard.h"
#include "core/il2cpp_api.h"
#include "core/log.h"
#include "core/main_thread.h"
#include "core/module.h"
#include "core/resolver.h"
#include "core/text_override.h"
#include "core/unity.h"

namespace sr2::modules::custom_upgrades::fabricator {
namespace {

namespace unity = sr2::core::unity;
namespace main_thread = sr2::core::main_thread;
using sr2::il2cpp::api;
using sr2::il2cpp::Resolver;

constexpr const char* kImage = "Assembly-CSharp";
constexpr const char* kFabNs = "MonomiPark.SlimeRancher.UI.Fabricator";
constexpr int kMaxCircles = 32;  // sanity cap for a level count

struct Vec2 {
    float x, y;
};

using ShowFn = void (*)(Il2CppObject*, int32_t, int32_t, const MethodInfo*);
using ArrayNewFn = Il2CppArray* (*)(Il2CppClass*, uintptr_t);
using ElementClassFn = Il2CppClass* (*)(Il2CppClass*);
using WBarrierFn = void (*)(Il2CppObject*, void**, void*);
using DetailsShowFn = void (*)(Il2CppObject*, Il2CppObject*, const MethodInfo*);
using ObjectNewFn = Il2CppObject* (*)(Il2CppClass*);

// One row of circles / grid of pips as the prefab laid it out (read from the live objects once).
struct Layout {
    Il2CppArray* array = nullptr;  // the array we installed (identity: a new instance has its own)
    int prefabCount = 0;           // circles / pips the prefab had
    std::vector<Vec2> pos, size;   // their anchored positions and sizes
    Vec2 highlight{};              // chain only: the highlight's size
    int mode = -1;                 // count last laid out for (prefab layout = 0)
};

struct State {
    const char* module = "custom_upgrades";
    ArrayNewFn arrayNew = nullptr;
    ElementClassFn elementClass = nullptr;
    WBarrierFn wbarrier = nullptr;
    size_t offChainLevels = 0, offChainHighlight = 0;  // UpgradeChain._levels / _highlightTransform
    size_t offButtonPips = 0, offPipsArray = 0;        // FabricatorItemButton._pips / FabricatorItemButtonPips._pips
    const MethodInfo* getPos = nullptr;   // RectTransform.get_anchoredPosition
    const MethodInfo* setPos = nullptr;
    const MethodInfo* getSize = nullptr;  // RectTransform.get_sizeDelta
    const MethodInfo* setSize = nullptr;
    const MethodInfo* getParent = nullptr;  // Transform.get_parent
    ShowFn origShow = nullptr;
    ShowFn origPips = nullptr;
    // PlayerUpgradeDetails.Show: the "Next:" label of a level whose title is ours (DetailsShowDetour).
    DetailsShowFn origDetails = nullptr;
    ObjectNewFn objectNew = nullptr;
    size_t offDetNext = 0, offDetNextString = 0;  // PlayerUpgradeDetails._nextUpgrade / _nextUpgradeString
    size_t offLevelTitle = 0;                     // UpgradeDefinition.UpgradeLevelDefinition.Title (lazy)
    Il2CppClass* stringVarClass = nullptr;        // SmartFormat.PersistentVariables.StringVariable
    const MethodInfo* nextExists = nullptr;       // PlayerUpgradeFabricatableItem.get_NextUpgradeLevelExists
    const MethodInfo* nextLevel = nullptr;        // PlayerUpgradeFabricatableItem.get_NextUpgradeLevelDefinition
    const MethodInfo* svCtor = nullptr;           // StringVariable..ctor
    const MethodInfo* svSetValue = nullptr;       // Variable<string>.set_Value
    const MethodInfo* lsSetItem = nullptr;        // LocalizedString.set_Item(string, IVariable)
    const MethodInfo* lseRefresh = nullptr;       // LocalizeStringEvent.RefreshString
    std::unordered_set<std::string> loggedNext;   // titles logged once
    std::unordered_map<Il2CppObject*, Layout> layouts;  // per UpgradeChain / FabricatorItemButtonPips
    bool loggedChain = false, loggedPips = false, failed = false;
};

State g;
sr2::core::DetourGuard g_chainGuard{"custom_upgrades", "UpgradeChain.Show"};
sr2::core::DetourGuard g_pipsGuard{"custom_upgrades", "FabricatorItemButton.ConfigurePips"};
sr2::core::DetourGuard g_detailsGuard{"custom_upgrades", "PlayerUpgradeDetails.Show"};

template <class T>
T Unbox(Il2CppObject* boxed, T def = T{}) {
    return boxed ? *reinterpret_cast<T*>(reinterpret_cast<char*>(boxed) + 2 * sizeof(void*)) : def;
}

Il2CppObject* Call(const MethodInfo* m, void* obj, void** args, const char* what, bool* ok = nullptr) {
    if (ok) *ok = false;
    if (!m) return nullptr;
    std::string err;
    Il2CppObject* r = Resolver::Get().Invoke(m, obj, args, &err);
    if (!err.empty()) {
        if (err.rfind("native exception", 0) == 0) sr2::core::MarkModuleFaulted(g.module, std::format("{}: {}", what, err));
        else SR2_WARN("[{}] fabricator levels: {} failed: {}", g.module, what, err);
        return nullptr;
    }
    if (ok) *ok = true;
    return r;
}

Vec2 GetVec(const MethodInfo* m, Il2CppObject* rt, const char* what) {
    return Unbox<Vec2>(Call(m, rt, nullptr, what));
}
void SetVec(const MethodInfo* m, Il2CppObject* rt, Vec2 v, const char* what) {
    void* args[] = {&v};
    Call(m, rt, args, what);
}

// The circle / pip graphic's RectTransform.
Il2CppObject* RectOf(Il2CppObject* graphic) { return unity::IsAlive(graphic) ? unity::TransformOf(graphic) : nullptr; }

// An array with `count` entries: the old ones, then clones of the last one (same parent). nullptr =
// failed (logged once); the caller then leaves the instance alone.
Il2CppArray* Extend(Il2CppObject* owner, size_t offArray, int count, const char* what) {
    auto* arr = unity::Field<Il2CppArray*>(owner, offArray);
    const int have = static_cast<int>(unity::ArrayLength(arr));
    if (!arr || have < 2 || !g.arrayNew || !g.elementClass) return nullptr;
    auto* last = static_cast<Il2CppObject*>(unity::ArrayAt(arr, have - 1));
    Il2CppObject* lastGo = unity::IsAlive(last) ? unity::GameObjectOf(last) : nullptr;
    Il2CppObject* parent = lastGo ? Call(g.getParent, unity::TransformOf(lastGo), nullptr, "Transform.get_parent") : nullptr;
    Il2CppClass* elem = g.elementClass(unity::ClassOf(reinterpret_cast<Il2CppObject*>(arr)));
    if (!lastGo || !unity::IsAlive(parent) || !elem) return nullptr;
    Il2CppArray* ext = g.arrayNew(elem, static_cast<uintptr_t>(count));
    if (!ext) return nullptr;
    auto put = [&](int i, Il2CppObject* v) {
        void** slot = unity::ArrayData<void*>(ext) + i;
        if (g.wbarrier) g.wbarrier(reinterpret_cast<Il2CppObject*>(ext), slot, v);
        else *slot = v;
    };
    for (int i = 0; i < have; ++i) put(i, static_cast<Il2CppObject*>(unity::ArrayAt(arr, i)));
    std::vector<Il2CppObject*> made;
    for (int i = have; i < count; ++i) {
        Il2CppObject* go = unity::Instantiate(lastGo, parent);
        Il2CppObject* c = unity::IsAlive(go) ? unity::GetComponent(go, unity::ClassOf(last)) : nullptr;
        if (unity::IsAlive(go)) made.push_back(go);
        if (!c) {
            SR2_WARN("[{}] fabricator levels: {} {} could not be cloned", g.module, what, i);
            for (Il2CppObject* m : made) unity::SetActive(m, false);  // not in any array: keep them out of sight
            g.failed = true;
            return nullptr;
        }
        unity::SetName(go, std::format("Pip{}", i));
        put(i, c);
    }
    void** field = reinterpret_cast<void**>(reinterpret_cast<char*>(owner) + offArray);
    if (g.wbarrier) g.wbarrier(owner, field, ext);
    else *field = ext;
    return ext;
}

// The layout record of an owner, (re)built when the owner's array is not the one we installed.
Layout* LayoutOf(Il2CppObject* owner, size_t offArray, int count, const char* what) {
    auto* arr = unity::Field<Il2CppArray*>(owner, offArray);
    auto it = g.layouts.find(owner);
    if (it != g.layouts.end() && it->second.array == arr) {
        Layout& l = it->second;
        if (count > static_cast<int>(unity::ArrayLength(arr))) {  // an upgrade with even more levels
            Il2CppArray* more = Extend(owner, offArray, count, what);
            if (!more) return nullptr;
            l.array = more;
            l.mode = -1;
        }
        return &l;
    }
    if (it != g.layouts.end()) g.layouts.erase(it);  // a new object at a reused address
    const int have = static_cast<int>(unity::ArrayLength(arr));
    if (count <= have) return nullptr;  // enough circles: vanilla does it all
    Layout l;
    l.prefabCount = have;
    for (int i = 0; i < have; ++i) {
        Il2CppObject* rt = RectOf(static_cast<Il2CppObject*>(unity::ArrayAt(arr, i)));
        if (!rt) return nullptr;
        l.pos.push_back(GetVec(g.getPos, rt, "RectTransform.get_anchoredPosition"));
        l.size.push_back(GetVec(g.getSize, rt, "RectTransform.get_sizeDelta"));
    }
    l.array = Extend(owner, offArray, count, what);
    if (!l.array) return nullptr;
    return &(g.layouts[owner] = std::move(l));
}

// Chain: one row in the prefab's width; more circles -> narrower, closer (same gaps ratio).
void LayoutChain(Il2CppObject* chain, Layout& l, int count) {
    const int mode = count > l.prefabCount ? count : 0;
    if (l.mode == mode) return;
    l.mode = mode;
    const int n = static_cast<int>(unity::ArrayLength(l.array));
    const float w = l.size[0].x, pitch = l.pos[1].x - l.pos[0].x;
    const float left = l.pos[0].x - w * 0.5f;
    const float span = (l.prefabCount - 1) * pitch + w;
    const float s = mode ? span / ((count - 1) * pitch + w) : 1.0f;
    for (int i = 0; i < n; ++i) {
        Il2CppObject* rt = RectOf(static_cast<Il2CppObject*>(unity::ArrayAt(l.array, i)));
        if (!rt) continue;
        Vec2 p{left + w * s * 0.5f + i * pitch * s, l.pos[0].y};
        Vec2 sz{w * s, l.size[0].y};
        if (!mode && i < l.prefabCount) {
            p = l.pos[i];
            sz = l.size[i];
        }
        SetVec(g.setSize, rt, sz, "RectTransform.set_sizeDelta");
        SetVec(g.setPos, rt, p, "RectTransform.set_anchoredPosition");
    }
    Il2CppObject* hl = unity::Field<Il2CppObject*>(chain, g.offChainHighlight);
    if (unity::IsAlive(hl)) {
        if (l.highlight.x <= 0.0f) l.highlight = GetVec(g.getSize, hl, "RectTransform.get_sizeDelta");
        Vec2 sz = l.highlight;
        if (mode) sz.x = w * s + (l.highlight.x - w);
        SetVec(g.setSize, hl, sz, "RectTransform.set_sizeDelta");
    }
    if (mode && !g.loggedChain) {
        g.loggedChain = true;
        SR2_INFO("[{}] fabricator levels: details panel shows {} level circles ({} in the prefab, now {:.1f} px wide every "
                 "{:.1f} px)", g.module, count, l.prefabCount, w * s, pitch * s);
    }
}

// Pips: the prefab's rows, more columns in the prefab's row width.
void LayoutPips(Layout& l, int count) {
    const int mode = count > l.prefabCount ? count : 0;
    if (l.mode == mode) return;
    l.mode = mode;
    int cols = 1;
    while (cols < l.prefabCount && std::fabs(l.pos[cols].y - l.pos[0].y) < 0.5f) ++cols;
    const int rows = std::max(1, (l.prefabCount + cols - 1) / cols);
    const float w = l.size[0].x;
    const float pitchX = cols > 1 ? l.pos[1].x - l.pos[0].x : w + 2.0f;
    const float rowStep = rows > 1 ? l.pos[cols].y - l.pos[0].y : -(l.size[0].y + 3.0f);
    const float gap = pitchX - w;
    const float rowWidth = cols * pitchX - gap;
    const int newCols = mode ? (count + rows - 1) / rows : cols;
    const float newPitch = (rowWidth + gap) / newCols;
    const int n = static_cast<int>(unity::ArrayLength(l.array));
    for (int i = 0; i < n; ++i) {
        Il2CppObject* rt = RectOf(static_cast<Il2CppObject*>(unity::ArrayAt(l.array, i)));
        if (!rt) continue;
        Vec2 p{l.pos[0].x + (i % newCols) * newPitch, l.pos[0].y + (i / newCols) * rowStep};
        Vec2 sz{newPitch - gap, l.size[0].y};
        if (!mode && i < l.prefabCount) {
            p = l.pos[i];
            sz = l.size[i];
        }
        SetVec(g.setSize, rt, sz, "RectTransform.set_sizeDelta");
        SetVec(g.setPos, rt, p, "RectTransform.set_anchoredPosition");
    }
    if (mode && !g.loggedPips) {
        g.loggedPips = true;
        SR2_INFO("[{}] fabricator levels: grid buttons show {} pips ({} rows of {}, {} in the prefab)", g.module, count, rows,
                 newCols, l.prefabCount);
    }
}

// UpgradeChain.Show(currentLevel, numberOfLevels): enough circles before the original runs.
void ShowDetour(Il2CppObject* self, int32_t current, int32_t count, const MethodInfo* mi) {
    if (self && main_thread::IsMainThread() && !g.failed && count > 0 && count <= kMaxCircles)
        g_chainGuard.Run([&] {
            Layout* l = LayoutOf(self, g.offChainLevels, count, "level circle");
            if (l) LayoutChain(self, *l, count);
        });
    g.origShow(self, current, count, mi);
}

// FabricatorItemButton.ConfigurePips(unlockedCount, totalCount).
void PipsDetour(Il2CppObject* self, int32_t unlocked, int32_t total, const MethodInfo* mi) {
    if (self && main_thread::IsMainThread() && !g.failed && total > 0 && total <= kMaxCircles)
        g_pipsGuard.Run([&] {
            Il2CppObject* pips = unity::Field<Il2CppObject*>(self, g.offButtonPips);
            if (!unity::IsAlive(pips)) return;
            Layout* l = LayoutOf(pips, g.offPipsArray, total, "pip");
            if (l) LayoutPips(*l, total);
        });
    g.origPips(self, unlocked, total, mi);
}

// PlayerUpgradeDetails.Show(item) (0x82AAF0) sets "Next: {next_upgrade_name}" with the next level's
// Title LocalizedString as a nested variable. Smart format resolves a nested LocalizedString from its
// table directly, never through LocalizeStringEvent.UpdateString, so the text_override registered on
// our cloned levels' titles does not apply there: the label showed the template's entry (Tank Booster
// IV on Slime Launcher, V on Tank Booster IX; round 15). After the original: when the next level's
// title is one of ours, the variable becomes a StringVariable with our text and the label refreshes.
// The "Next:" around it stays the game's; the next Show puts the level's own title back.
void DetailsShowDetour(Il2CppObject* self, Il2CppObject* item, const MethodInfo* mi) {
    g.origDetails(self, item, mi);
    if (!self || !item || !main_thread::IsMainThread() || !sr2::core::text_override::Any()) return;
    g_detailsGuard.Run([&] {
        bool ok = false;
        const bool exists =
            Unbox<bool>(Call(g.nextExists, item, nullptr, "PlayerUpgradeFabricatableItem.get_NextUpgradeLevelExists", &ok));
        if (!ok || !exists) return;
        Il2CppObject* level =
            Call(g.nextLevel, item, nullptr, "PlayerUpgradeFabricatableItem.get_NextUpgradeLevelDefinition");
        if (!level) return;
        if (!g.offLevelTitle) {
            auto o = Resolver::Get().FieldOffset(unity::ClassOf(level), "Title");
            if (!o) return;
            g.offLevelTitle = *o;
        }
        std::string text;
        if (!sr2::core::text_override::Lookup(unity::Field<Il2CppObject*>(level, g.offLevelTitle), text)) return;
        Il2CppObject* ls = unity::Field<Il2CppObject*>(self, g.offDetNextString);
        Il2CppObject* lse = unity::Field<Il2CppObject*>(self, g.offDetNext);
        if (!ls || !unity::IsAlive(lse)) return;
        Il2CppObject* var = g.objectNew(g.stringVarClass);
        if (!var) return;
        Call(g.svCtor, var, nullptr, "StringVariable..ctor", &ok);
        if (!ok) return;
        void* valueArgs[] = {unity::NewString(text)};
        Call(g.svSetValue, var, valueArgs, "StringVariable.set_Value", &ok);
        if (!ok) return;
        void* itemArgs[] = {unity::NewString("next_upgrade_name"), var};
        Call(g.lsSetItem, ls, itemArgs, "LocalizedString.set_Item", &ok);
        if (!ok) return;
        Call(g.lseRefresh, lse, nullptr, "LocalizeStringEvent.RefreshString");
        if (g.loggedNext.insert(text).second)
            SR2_INFO("[{}] fabricator levels: details panel 'Next:' label shows '{}'", g.module, text);
    });
}

// Optional: without it the "Next:" label of our levels shows the template's title (logged).
void SetupNextLabel(sr2::core::ModuleContext& ctx, Resolver& R, std::string& report) {
    std::string err;
    constexpr const char* kLoc = "Unity.Localization";
    Il2CppClass* details = R.Class(kImage, kFabNs, "PlayerUpgradeDetails", &err);
    Il2CppClass* item = R.Class(kImage, kFabNs, "PlayerUpgradeFabricatableItem", &err);
    Il2CppClass* ls = R.Class(kLoc, "UnityEngine.Localization", "LocalizedString", &err);
    Il2CppClass* lse = R.Class(kLoc, "UnityEngine.Localization.Components", "LocalizeStringEvent", &err);
    g.stringVarClass = R.Class(kLoc, "UnityEngine.Localization.SmartFormat.PersistentVariables", "StringVariable", &err);
    g.objectNew = reinterpret_cast<ObjectNewFn>(GetProcAddress(api().module, "il2cpp_object_new"));
    auto off = [&](const char* name, size_t& out) {
        if (!details) return;
        if (auto o = R.FieldOffset(details, name, &err)) out = *o;
    };
    off("_nextUpgrade", g.offDetNext);
    off("_nextUpgradeString", g.offDetNextString);
    if (item) {
        g.nextExists = R.Method(item, "get_NextUpgradeLevelExists", 0, &err);
        g.nextLevel = R.Method(item, "get_NextUpgradeLevelDefinition", 0, &err);
    }
    if (g.stringVarClass) {
        g.svCtor = R.Method(g.stringVarClass, ".ctor", 0, &err);
        g.svSetValue = R.Method(g.stringVarClass, "set_Value", 1, &err);  // inherited from Variable<string>
    }
    if (ls) g.lsSetItem = R.Method(ls, "set_Item", 2, &err);
    if (lse) g.lseRefresh = R.Method(lse, "RefreshString", 0, &err);
    const MethodInfo* showMi = details ? R.Method(details, "Show", 1, &err) : nullptr;
    void* showP = showMi ? R.MethodPointer(showMi, &err) : nullptr;
    if (!g.objectNew || !g.offDetNext || !g.offDetNextString || !g.nextExists || !g.nextLevel || !g.svCtor ||
        !g.svSetValue || !g.lsSetItem || !g.lseRefresh || !showP ||
        !ctx.HookAddress("PlayerUpgradeDetails.Show", showP, reinterpret_cast<void*>(&DetailsShowDetour),
                         reinterpret_cast<void**>(&g.origDetails))) {
        SR2_WARN("[{}] fabricator levels: PlayerUpgradeDetails.Show not hooked ({}) - the details panel's 'Next:' label "
                 "shows the template's title for mod levels", g.module, err.empty() ? "lookups failed" : err);
        return;
    }
    report += std::format(" PlayerUpgradeDetails.Show@0x{:X}", Resolver::Rva(showP));
}

}  // namespace

bool FabricatorLevels::Setup(sr2::core::ModuleContext& ctx, const char* module, bool& chainOk, std::string& why,
                             std::string& report) {
    chainOk = false;
    g.module = module;
    Resolver& R = ctx.resolver();
    std::string err;
    g.arrayNew = reinterpret_cast<ArrayNewFn>(GetProcAddress(api().module, "il2cpp_array_new"));
    g.elementClass = reinterpret_cast<ElementClassFn>(GetProcAddress(api().module, "il2cpp_class_get_element_class"));
    g.wbarrier = reinterpret_cast<WBarrierFn>(GetProcAddress(api().module, "il2cpp_gc_wbarrier_set_field"));
    if (!g.arrayNew || !g.elementClass) {
        why = "il2cpp_array_new / il2cpp_class_get_element_class export missing";
        return false;
    }
    Il2CppClass* chain = R.Class(kImage, kFabNs, "UpgradeChain", &err);
    Il2CppClass* button = R.Class(kImage, kFabNs, "FabricatorItemButton", &err);
    Il2CppClass* pips = R.Class(kImage, kFabNs, "FabricatorItemButtonPips", &err);
    Il2CppClass* rect = R.Class("UnityEngine.CoreModule", "UnityEngine", "RectTransform", &err);
    Il2CppClass* tr = unity::TransformClass();
    if (!chain || !button || !pips || !rect || !tr) {
        why = err.empty() ? "classes not found" : err;
        return false;
    }
    auto off = [&](Il2CppClass* k, const char* name, size_t& out) {
        if (auto o = R.FieldOffset(k, name, &err)) out = *o;
        else if (why.empty()) why = std::format("field {} not found", name);
    };
    off(chain, "_levels", g.offChainLevels);
    off(chain, "_highlightTransform", g.offChainHighlight);
    off(button, "_pips", g.offButtonPips);
    off(pips, "_pips", g.offPipsArray);
    g.getPos = R.Method(rect, "get_anchoredPosition", 0, &err);
    g.setPos = R.Method(rect, "set_anchoredPosition", 1, &err);
    g.getSize = R.Method(rect, "get_sizeDelta", 0, &err);
    g.setSize = R.Method(rect, "set_sizeDelta", 1, &err);
    g.getParent = R.Method(tr, "get_parent", 0, &err);
    if (!why.empty()) return false;
    if (!g.getPos || !g.setPos || !g.getSize || !g.setSize || !g.getParent) {
        why = "RectTransform / Transform methods not found";
        return false;
    }
    const MethodInfo* showMi = R.Method(chain, "Show", 2, &err);
    const MethodInfo* pipsMi = R.Method(button, "ConfigurePips", 2, &err);
    void* showP = showMi ? R.MethodPointer(showMi, &err) : nullptr;
    void* pipsP = pipsMi ? R.MethodPointer(pipsMi, &err) : nullptr;
    if (showP && ctx.HookAddress("UpgradeChain.Show", showP, reinterpret_cast<void*>(&ShowDetour),
                                 reinterpret_cast<void**>(&g.origShow))) {
        chainOk = true;
        report += std::format(" UpgradeChain.Show@0x{:X}", Resolver::Rva(showP));
    } else {
        why = std::format("UpgradeChain.Show not hooked ({})", err);
    }
    if (pipsP && ctx.HookAddress("FabricatorItemButton.ConfigurePips", pipsP, reinterpret_cast<void*>(&PipsDetour),
                                 reinterpret_cast<void**>(&g.origPips)))
        report += std::format(" FabricatorItemButton.ConfigurePips@0x{:X}", Resolver::Rva(pipsP));
    else
        SR2_WARN("[{}] fabricator levels: FabricatorItemButton.ConfigurePips not hooked ({}) - grid buttons show at most "
                 "their prefab's pips", module, err);
    SetupNextLabel(ctx, R, report);
    return chainOk;
}

}  // namespace sr2::modules::custom_upgrades::fabricator
