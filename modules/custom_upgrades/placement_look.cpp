// custom_upgrades: gadget-mode label and placeholder look of the mod's buildables (see placement_look.h).
// SR2 1.3.0, all lookups by name; RVAs for reference only.

#include "modules/custom_upgrades/placement_look.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <format>
#include <initializer_list>
#include <vector>

#include "core/detour_guard.h"
#include "core/il2cpp_api.h"
#include "core/log.h"
#include "core/main_thread.h"
#include "core/module.h"
#include "core/resolver.h"
#include "core/unity.h"

namespace sr2::modules::custom_upgrades::placement_look {
namespace {

namespace unity = sr2::core::unity;
namespace main_thread = sr2::core::main_thread;
using sr2::il2cpp::api;
using sr2::il2cpp::Resolver;

constexpr const char* kModule = "custom_upgrades";
constexpr size_t kObsValue = 2 * sizeof(void*);  // ObservableValue<T>._value

std::array<Provider*, 8> g_providers{};
std::atomic<int> g_count{0};

struct State {
    size_t offHeld = 0, offPlaceholder = 0;                                 // GadgetItem
    size_t offLdName = 0, offLdDirector = 0, offLdPlayer = 0;               // GadgetLabelDisplay
    size_t offGdSelected = 0, offGdTargeted = 0, offGdOnPlaceholder = 0;    // GadgetDirector
    size_t offPsModeActive = 0;                                             // PlayerState
    const MethodInfo* setStringRef = nullptr, *trFind = nullptr, *goSetLayer = nullptr;
    const MethodInfo* trChildCount = nullptr, *trGetChild = nullptr;
    Il2CppObject* item = nullptr;  // the player's GadgetItem (raw: a player component, checked alive on use)
    unsigned loggedDress = 0, loggedLabel = 0;
};
State* g = nullptr;
sr2::core::DetourGuard g_guard{kModule, "placement look"};

using SetHeldFn = void (*)(Il2CppObject*, Il2CppObject*, const MethodInfo*);
using UpdateLabelsFn = void (*)(Il2CppObject*, const MethodInfo*);
SetHeldFn g_origSetHeld = nullptr;            // GadgetItem.SetHeldGadget
UpdateLabelsFn g_origUpdateLabels = nullptr;  // GadgetLabelDisplay.UpdateLabels

Provider* ProviderFor(Il2CppObject* def) {
    if (!def) return nullptr;
    const int n = g_count.load(std::memory_order_acquire);
    for (int i = 0; i < n; ++i)
        if (g_providers[i] && g_providers[i]->Wants(def)) return g_providers[i];
    return nullptr;
}

Il2CppObject* Invoke(const MethodInfo* m, void* obj, std::initializer_list<void*> args, const char* what, bool* ok = nullptr) {
    if (ok) *ok = false;
    if (!m) return nullptr;
    std::vector<void*> params(args);
    std::string err;
    Il2CppObject* r = Resolver::Get().Invoke(m, obj, params.empty() ? nullptr : params.data(), &err);
    if (!err.empty()) {
        if (err.rfind("native exception", 0) == 0) sr2::core::MarkModuleFaulted(kModule, std::format("placement look: {}: {}", what, err));
        else SR2_WARN("[{}] placement look: {} failed: {}", kModule, what, err);
        return nullptr;
    }
    if (ok) *ok = true;
    return r;
}

// Next frame after SetHeldGadget: the placeholder of a provider's held definition, dressed once.
void DressHeld(Il2CppObject* item) {
    if (!g || !unity::IsAlive(item) || sr2::core::ModuleFaulted(kModule)) return;
    Il2CppObject* def = unity::Field<Il2CppObject*>(item, g->offHeld);
    Il2CppObject* ph = unity::Field<Il2CppObject*>(item, g->offPlaceholder);
    Provider* p = ProviderFor(def);
    if (!p || !unity::IsAlive(ph)) return;
    bool ok = false;
    Il2CppObject* have = Invoke(g->trFind, unity::TransformOf(ph), {unity::NewString(kPreviewChild)}, "Transform.Find", &ok);
    if (!ok || unity::IsAlive(have)) return;  // dressed already
    p->DressPreview(def, ph);
    if (g->loggedDress++ < 3)
        SR2_INFO("[{}] placement look: preview of '{}' dressed (placeholder '{}')", kModule, unity::GetName(def), unity::GetName(ph));
}

void SetHeldDetour(Il2CppObject* self, Il2CppObject* def, const MethodInfo* mi) {
    g_origSetHeld(self, def, mi);
    if (!g || !self || !main_thread::IsMainThread()) return;
    g_guard.Run([&] {
        g->item = self;
        if (!ProviderFor(def)) return;
        main_thread::Post([self] { g_guard.Run([&] { DressHeld(self); }); });
    });
}

// After UpdateLabels: the name label of a provider's definition (selected / held while placing) or of a placed gadget
// it lists (aimed at).
void Relabel(Il2CppObject* display) {
    Il2CppObject* director = unity::Field<Il2CppObject*>(display, g->offLdDirector);
    Il2CppObject* player = unity::Field<Il2CppObject*>(display, g->offLdPlayer);
    Il2CppObject* name = unity::Field<Il2CppObject*>(display, g->offLdName);
    if (!director || !name || !unity::IsAlive(name)) return;
    Il2CppObject* modeObs = player ? unity::Field<Il2CppObject*>(player, g->offPsModeActive) : nullptr;
    const bool mode = modeObs && unity::Field<bool>(modeObs, kObsValue);
    Il2CppObject* selObs = unity::Field<Il2CppObject*>(director, g->offGdSelected);
    Il2CppObject* selected = selObs ? unity::Field<Il2CppObject*>(selObs, kObsValue) : nullptr;
    Il2CppObject* label = nullptr;
    if (mode && selected && unity::Field<bool>(director, g->offGdOnPlaceholder)) {
        Il2CppObject* held = unity::IsAlive(g->item) ? unity::Field<Il2CppObject*>(g->item, g->offHeld) : nullptr;
        for (Il2CppObject* def : {selected, held})
            if (Provider* p = ProviderFor(def); p && !label) label = p->Label(def);
    } else {
        Il2CppObject* tgtObs = unity::Field<Il2CppObject*>(director, g->offGdTargeted);
        Il2CppObject* gadget = tgtObs ? unity::Field<Il2CppObject*>(tgtObs, kObsValue) : nullptr;
        if (gadget && unity::IsAlive(gadget)) {
            const int n = g_count.load(std::memory_order_acquire);
            for (int i = 0; i < n && !label; ++i) label = g_providers[i]->TargetLabel(gadget);
        }
    }
    if (!label) return;
    bool ok = false;
    Invoke(g->setStringRef, name, {label}, "LocalizeStringEvent.set_StringReference", &ok);
    if (ok && g->loggedLabel++ < 3) SR2_INFO("[{}] placement look: gadget label set to a mod buildable's name", kModule);
}

void UpdateLabelsDetour(Il2CppObject* self, const MethodInfo* mi) {
    g_origUpdateLabels(self, mi);
    if (!g || !self || !main_thread::IsMainThread() || sr2::core::ModuleFaulted(kModule)) return;
    g_guard.Run([&] { Relabel(self); });
}

}  // namespace

void Add(Provider* provider) {
    const int n = g_count.load();
    if (!provider || n >= static_cast<int>(g_providers.size())) return;
    g_providers[n] = provider;
    g_count.store(n + 1, std::memory_order_release);
}

void SetLayerRecursive(Il2CppObject* goOrComp, int layer) {
    if (!g) return;
    std::vector<Il2CppObject*> todo{unity::TransformOf(goOrComp)};
    for (size_t i = 0; i < todo.size() && i < 1024; ++i) {
        Il2CppObject* tr = todo[i];
        if (!tr || !unity::IsAlive(tr)) continue;
        Invoke(g->goSetLayer, unity::GameObjectOf(tr), {&layer}, "GameObject.set_layer");
        bool ok = false;
        Il2CppObject* boxed = Invoke(g->trChildCount, tr, {}, "Transform.get_childCount", &ok);
        const int32_t n = ok && boxed ? *reinterpret_cast<int32_t*>(reinterpret_cast<char*>(boxed) + 2 * sizeof(void*)) : 0;
        for (int32_t c = 0; c < n && c < 256; ++c)
            if (Il2CppObject* child = Invoke(g->trGetChild, tr, {&c}, "Transform.GetChild", &ok); ok && child) todo.push_back(child);
    }
}

bool Setup(sr2::core::ModuleContext& ctx, std::string& why) {
    Resolver& R = ctx.resolver();
    auto* s = new State();
    auto parentOf = [](Il2CppClass* c) { return api().il2cpp_class_get_parent ? api().il2cpp_class_get_parent(c) : nullptr; };
    auto cls = [&](const char* image, const char* ns, const char* name) -> Il2CppClass* {
        std::string err;
        Il2CppClass* k = R.Class(image, ns, name, &err);
        if (!k && why.empty()) why = err.empty() ? std::format("class {}.{} not found", ns, name) : err;
        return k;
    };
    auto off = [&](Il2CppClass* k, const char* field, size_t& out) {
        for (Il2CppClass* c = k; c; c = parentOf(c))
            if (auto o = R.FieldOffset(c, field, nullptr)) {
                out = *o;
                return;
            }
        if (why.empty()) why = std::format("field {} not found", field);
    };
    auto method = [&](Il2CppClass* k, const char* name, int argc) -> const MethodInfo* {
        for (Il2CppClass* c = k; c; c = parentOf(c))
            if (const MethodInfo* m = R.Method(c, name, argc, nullptr)) return m;
        if (why.empty()) why = std::format("method {}({}) not found", name, argc);
        return nullptr;
    };
    Il2CppClass* item = cls("Assembly-CSharp", "MonomiPark.SlimeRancher.Player.PlayerItems", "GadgetItem");
    Il2CppClass* display = cls("Assembly-CSharp", "MonomiPark.SlimeRancher.UI.Gadget", "GadgetLabelDisplay");
    Il2CppClass* director = cls("Assembly-CSharp", "", "GadgetDirector");
    Il2CppClass* playerState = cls("Assembly-CSharp", "", "PlayerState");
    Il2CppClass* lse = cls("Unity.Localization", "UnityEngine.Localization.Components", "LocalizeStringEvent");
    Il2CppClass* transform = cls("UnityEngine.CoreModule", "UnityEngine", "Transform");
    Il2CppClass* gameObject = cls("UnityEngine.CoreModule", "UnityEngine", "GameObject");
    if (!why.empty()) {
        delete s;
        return false;
    }
    off(item, "_heldGadget", s->offHeld);
    off(item, "_gadgetPlaceholderInstance", s->offPlaceholder);
    off(display, "_nameLabel", s->offLdName);
    off(display, "_director", s->offLdDirector);
    off(display, "_player", s->offLdPlayer);
    off(director, "<SelectedSlottedGadget>k__BackingField", s->offGdSelected);
    off(director, "<TargetedGadget>k__BackingField", s->offGdTargeted);
    off(director, "_isTargetingPlaceholder", s->offGdOnPlaceholder);
    off(playerState, "<GadgetModeActive>k__BackingField", s->offPsModeActive);
    s->setStringRef = method(lse, "set_StringReference", 1);
    s->goSetLayer = method(gameObject, "set_layer", 1);
    s->trChildCount = method(transform, "get_childCount", 0);
    s->trGetChild = method(transform, "GetChild", 1);
    s->trFind = R.MethodByParams(transform, "Find", {"System.String"}, nullptr);
    const MethodInfo* setHeld = method(item, "SetHeldGadget", 1);
    const MethodInfo* update = method(display, "UpdateLabels", 0);
    if (!s->trFind && why.empty()) why = "Transform.Find not found";
    void* pSetHeld = why.empty() ? R.MethodPointer(setHeld, &why) : nullptr;
    void* pUpdate = pSetHeld ? R.MethodPointer(update, &why) : nullptr;
    if (!pSetHeld || !pUpdate) {
        delete s;
        return false;
    }
    g = s;  // before the hooks go live
    if (!ctx.HookAddress("placement_look.GadgetItem.SetHeldGadget", pSetHeld, reinterpret_cast<void*>(&SetHeldDetour),
                         reinterpret_cast<void**>(&g_origSetHeld)) ||
        !ctx.HookAddress("placement_look.GadgetLabelDisplay.UpdateLabels", pUpdate, reinterpret_cast<void*>(&UpdateLabelsDetour),
                         reinterpret_cast<void**>(&g_origUpdateLabels))) {
        why = ctx.failure();
        return false;  // g stays (a hook that went live may run; it only reads)
    }
    return true;
}

}  // namespace sr2::modules::custom_upgrades::placement_look
