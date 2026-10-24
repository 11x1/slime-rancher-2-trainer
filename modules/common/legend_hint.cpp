#include "modules/common/legend_hint.h"

#include <algorithm>
#include <atomic>
#include <format>
#include <mutex>

#include "core/resolver.h"
#include "core/unity.h"

namespace sr2::modules::legend_hint {
namespace {

namespace unity = sr2::core::unity;
namespace ugui = sr2::modules::minimap::ugui;
using il2cpp::Resolver;

constexpr size_t kNoOff = static_cast<size_t>(-1);
constexpr int kReapplyFrames = 4;  // re-assert the clone's key / label for a few frames
inline bool Has(size_t off) { return off != kNoOff; }

template <class T>
T& F(void* obj, size_t off) {
    return unity::Field<T>(obj, off);
}

struct Sym {
    Il2CppClass *legend = nullptr, *hint = nullptr, *inputIcon = nullptr, *registrar = nullptr;
    Il2CppClass *eventDisplay = nullptr, *invokeOnClick = nullptr, *lse = nullptr, *selectable = nullptr;
    size_t ciObject = kNoOff, legendHints = kNoOff, regPassThrough = kNoOff;
    size_t hintConfig = kNoOff, cfgEvent = kNoOff, cfgEvent2 = kNoOff;  // InputHint._configuration, its events
    size_t hintEnabled = kNoOff;                                         // InputHint._inputEnabled
    size_t iconButton = kNoOff, iconKey = kNoOff, iconKeyText = kNoOff, iconKeyImage = kNoOff, iconKeyLse = kNoOff,
           iconDouble = kNoOff, iconButtonImage = kNoOff;
    ugui::Fn behSetEnabled, setSiblingIndex, getSiblingIndex;
};
Sym s_;
std::mutex g_mutex;
std::atomic<int> g_state{0};  // 0 not tried, 1 ok, -1 failed
std::string g_err;

bool ResolveImpl(std::string& err) {
    Resolver& r = Resolver::Get();
    std::vector<std::string> missing;
    auto cls = [&](const char* img, const char* ns, const char* name) {
        std::string e;
        Il2CppClass* k = r.Class(img, ns, name, &e);
        if (!k) missing.push_back(e.empty() ? std::format("class {}.{}", ns, name) : e);
        return k;
    };
    auto fld = [&](Il2CppClass* k, const char* name) {
        if (!k) return kNoOff;
        std::string e;
        auto o = r.FieldOffset(k, name, &e);
        if (!o) {
            missing.push_back(e.empty() ? std::format("field {}", name) : e);
            return kNoOff;
        }
        return *o;
    };
    auto bind = [&](Il2CppClass* k, const char* name, int argc) {
        if (!k) return ugui::Fn{};
        std::string e;
        const MethodInfo* m = r.Method(k, name, argc, &e);
        ugui::Fn f = m ? ugui::BindFn(m) : ugui::Fn{};
        if (!f) missing.push_back(std::format("{}: {}", name, e.empty() ? "no native body" : e));
        return f;
    };
    const char* kAsm = "Assembly-CSharp";
    const char* nsCtl = "MonomiPark.SlimeRancher.UI.Framework.CommonControls";
    Sym s;
    s.ciObject = fld(cls(kAsm, "MonomiPark.SlimeRancher.UI.Framework", "ComponentInterfaceBase"), "_object");
    s.legend = cls(kAsm, nsCtl, "InputLegend");
    s.legendHints = fld(s.legend, "_hints");
    s.registrar = cls(kAsm, nsCtl, "InputLegendFocusRegistrar");
    s.regPassThrough = fld(s.registrar, "_passThrough");
    s.hint = cls(kAsm, nsCtl, "InputHint");
    s.hintConfig = fld(s.hint, "_configuration");
    s.hintEnabled = fld(s.hint, "_inputEnabled");
    Il2CppClass* cfg = cls(kAsm, nsCtl, "InputHintConfiguration");
    s.cfgEvent = fld(cfg, "InputEvent");
    s.cfgEvent2 = fld(cfg, "AdditionalInputEvent");
    s.inputIcon = cls(kAsm, nsCtl, "InputIcon");
    s.eventDisplay = cls(kAsm, nsCtl, "InputEventDisplay");
    s.invokeOnClick = cls(kAsm, nsCtl, "InvokeInputEventOnPointerClick");
    s.iconButton = fld(s.inputIcon, "_buttonRoot");
    s.iconKey = fld(s.inputIcon, "_keyRoot");
    s.iconKeyText = fld(s.inputIcon, "_keyText");
    s.iconKeyImage = fld(s.inputIcon, "_keyImage");
    s.iconKeyLse = fld(s.inputIcon, "_keyLocalizeStringEvent");
    s.iconDouble = fld(s.inputIcon, "_doubleTapRoot");
    s.iconButtonImage = fld(s.inputIcon, "_buttonImage");
    s.lse = cls("Unity.Localization", "UnityEngine.Localization.Components", "LocalizeStringEvent");
    s.selectable = cls("UnityEngine.UI", "UnityEngine.UI", "Selectable");
    s.behSetEnabled = bind(cls("UnityEngine.CoreModule", "UnityEngine", "Behaviour"), "set_enabled", 1);
    Il2CppClass* transform = cls("UnityEngine.CoreModule", "UnityEngine", "Transform");
    s.setSiblingIndex = bind(transform, "SetSiblingIndex", 1);
    s.getSiblingIndex = bind(transform, "GetSiblingIndex", 0);
    // Required: the bar, its hints, the key cap and Behaviour.enabled. The rest degrades (no registrar walk,
    // no lookup by input event, no ordering, the template's extra behaviours left as they are).
    if (!s.legend || !Has(s.legendHints) || !s.hint || !s.inputIcon || !Has(s.iconKey) || !Has(s.iconKeyText) ||
        !s.behSetEnabled) {
        std::string list;
        for (const auto& m : missing) list += (list.empty() ? "" : ", ") + m;
        err = "key bar symbols missing: " + list;
        return false;
    }
    s_ = s;
    return true;
}

void SetEnabled(Il2CppObject* behaviour, bool on) {
    if (s_.behSetEnabled && behaviour && unity::IsAlive(behaviour)) ugui::Call<void>(s_.behSetEnabled, behaviour, on);
}

bool IsUnder(Il2CppObject* t, Il2CppObject* ancestor) {
    for (int guard = 0; t && guard < 64; ++guard, t = ugui::GetParent(t))
        if (t == ancestor) return true;
    return false;
}

int SiblingIndex(Il2CppObject* t) { return s_.getSiblingIndex && t ? ugui::Call<int>(s_.getSiblingIndex, t) : -1; }

}  // namespace

bool Resolve(std::string& err) {
    std::lock_guard lock(g_mutex);
    if (g_state.load() == 0) g_state.store(ResolveImpl(g_err) ? 1 : -1);
    if (g_state.load() < 0) err = g_err;
    return g_state.load() > 0;
}

bool Resolved() { return g_state.load() > 0; }

Il2CppObject* FollowLegend(Il2CppObject* obj, Il2CppObject* root, std::string_view start, std::string& chain) {
    chain = start;
    if (!Resolved()) return nullptr;
    auto under = [&](Il2CppObject* comp) -> Il2CppObject* {
        if (!comp || !unity::IsAlive(comp)) return nullptr;
        Il2CppObject* found = nullptr;
        ugui::ForEachInChildren(unity::GameObjectOf(comp), s_.legend, false, [&](Il2CppObject* l) {
            found = l;
            return false;
        });
        return found;
    };
    Il2CppObject* last = nullptr;
    for (int hop = 0; hop < 8; ++hop) {
        if (!ugui::PlausiblePointer(obj)) {
            chain += " -> null";
            break;
        }
        chain += " -> " + unity::ClassName(obj);
        if (ugui::IsA(obj, s_.legend)) return unity::IsAlive(obj) ? obj : nullptr;
        if (ugui::IsA(obj, unity::ComponentClass())) {  // a registrar (or another IInputLegend component)
            if (!unity::IsAlive(obj)) break;
            last = obj;
            if (!s_.registrar || !Has(s_.regPassThrough) || !ugui::IsA(obj, s_.registrar)) break;
            obj = F<Il2CppObject*>(obj, s_.regPassThrough);  // ComponentInterface<IInputLegend>
            continue;
        }
        if (!Has(s_.ciObject)) break;  // a ComponentInterface<IInputLegend>: its _object
        obj = F<Il2CppObject*>(obj, s_.ciObject);
    }
    if (Il2CppObject* l = under(last)) {
        chain += " (InputLegend found under '" + unity::GetName(unity::GameObjectOf(last)) + "')";
        return l;
    }
    if (Il2CppObject* l = under(root)) {
        chain += " (InputLegend found under '" + unity::GetName(unity::GameObjectOf(root)) + "')";
        return l;
    }
    return nullptr;
}

std::vector<Il2CppObject*> Hints(Il2CppObject* legend, bool active) {
    std::vector<Il2CppObject*> out;
    if (!Resolved() || !legend || !unity::IsAlive(legend)) return out;
    auto* hints = F<Il2CppArray*>(legend, s_.legendHints);
    if (!ugui::PlausiblePointer(hints)) return out;
    for (size_t i = 0, n = std::min<size_t>(ugui::ArrayCount(hints), 32); i < n; ++i) {
        Il2CppObject* h = ugui::ArrayItem(hints, i);
        if (!ugui::IsA(h, s_.hint) || !unity::IsAlive(h)) continue;
        if (active && !unity::ActiveInHierarchy(unity::GameObjectOf(h))) continue;
        out.push_back(h);
    }
    return out;
}

Il2CppObject* Template(Il2CppObject* legend) {
    std::vector<Il2CppObject*> hints = Hints(legend, true);
    if (hints.empty()) return nullptr;
    if (Has(s_.hintEnabled))
        for (Il2CppObject* h : hints)
            if (F<bool>(h, s_.hintEnabled)) return h;
    return hints.front();
}

Il2CppObject* HintFor(Il2CppObject* legend, Il2CppObject* inputEvent) {
    if (!inputEvent || !Has(s_.hintConfig) || !Has(s_.cfgEvent)) return nullptr;
    for (Il2CppObject* h : Hints(legend, true)) {
        Il2CppObject* cfg = F<Il2CppObject*>(h, s_.hintConfig);
        if (!ugui::PlausiblePointer(cfg)) continue;
        if (F<Il2CppObject*>(cfg, s_.cfgEvent) == inputEvent ||
            (Has(s_.cfgEvent2) && F<Il2CppObject*>(cfg, s_.cfgEvent2) == inputEvent))
            return h;
    }
    return nullptr;
}

// ------------------------------------------------------------------ Clone ----
bool Clone::Create(Il2CppObject* tmpl, std::string_view name, std::string& why) {
    Destroy();
    if (!Resolved() || !ugui::IsA(tmpl, s_.hint) || !unity::IsAlive(tmpl)) {
        why = "no template hint";
        return false;
    }
    Il2CppObject* tmplGo = unity::GameObjectOf(tmpl);
    Il2CppObject* parent = ugui::GetParent(unity::TransformOf(tmplGo));
    Il2CppObject* clone = unity::Instantiate(tmplGo, parent);
    if (!unity::IsAlive(clone)) {
        why = "Object.Instantiate failed";
        return false;
    }
    Il2CppObject* go = ugui::IsA(clone, unity::GameObjectClass()) ? clone : unity::GameObjectOf(clone);
    go_.Reset(go);
    unity::SetName(go, name);
    ugui::SetAsLastSibling(unity::TransformOf(go));
    // Behaviour off: the hint would configure itself from the template's input event and fire it on click.
    if (Il2CppObject* h = unity::GetComponent(go, s_.hint)) ugui::Destroy(h);
    if (s_.invokeOnClick)
        ugui::ForEachInChildren(go, s_.invokeOnClick, true, [&](Il2CppObject* c) {
            ugui::Destroy(c);
            return true;
        });
    for (Il2CppClass* k : {s_.eventDisplay, s_.lse, s_.selectable}) {
        if (!k) continue;
        ugui::ForEachInChildren(go, k, true, [&](Il2CppObject* c) {
            SetEnabled(c, false);
            return true;
        });
    }
    ugui::ForEachInChildren(go, ugui::GraphicClass(), true, [&](Il2CppObject* g) {
        ugui::GraphicSetRaycastTarget(g, false);
        return true;
    });
    // InputHint fades its CanvasGroup with a spring: the clone keeps the template's alpha of this frame
    if (Il2CppClass* cgk = ugui::CanvasGroupClass())
        if (Il2CppObject* cg = unity::GetComponent(go, cgk)) ugui::CanvasGroupSetAlpha(cg, 1.0f);
    // key icons and the label (the TMP text outside every icon)
    std::vector<Il2CppObject*> iconTs;
    ugui::ForEachInChildren(go, s_.inputIcon, true, [&](Il2CppObject* ic) {
        icons_.emplace_back(ic);
        iconTs.push_back(unity::TransformOf(ic));
        return true;
    });
    if (Il2CppClass* tmp = ugui::TmpTextClass())
        ugui::ForEachInChildren(go, tmp, true, [&](Il2CppObject* t) {
            Il2CppObject* tt = unity::TransformOf(t);
            for (Il2CppObject* it : iconTs)
                if (IsUnder(tt, it)) return true;
            label_.Reset(t);
            return false;
        });
    reapply_ = kReapplyFrames;
    Apply();
    return true;
}

void Clone::SetLook(std::string_view key, std::string_view label, Il2CppObject* glyph, bool mirrored) {
    key_ = key;
    text_ = label;
    glyph_.Reset(glyph);
    mirrored_ = mirrored;
    reapply_ = kReapplyFrames;
    Apply();
}

void Clone::Tick() {
    if (reapply_ <= 0 || !go_.Alive()) return;
    --reapply_;
    Apply();
}

void Clone::PlaceBefore(Il2CppObject* hint) {
    if (!go_.Alive() || !s_.setSiblingIndex || !s_.getSiblingIndex) return;
    Il2CppObject* t = unity::TransformOf(go_.get());
    Il2CppObject* target = hint && unity::IsAlive(hint) ? unity::TransformOf(hint) : nullptr;
    if (!target || ugui::GetParent(target) != ugui::GetParent(t)) {
        ugui::SetAsLastSibling(t);
        return;
    }
    const int mine = SiblingIndex(t), theirs = SiblingIndex(target);
    if (mine < 0 || theirs < 0 || mine == theirs - 1) return;
    ugui::Call<void>(s_.setSiblingIndex, t, mine < theirs ? theirs - 1 : theirs);
}

// InputIcon.SetKey / SetButtonImage by hand (see modules/drone_price_limit/station_dialog.cpp): the first icon
// shows the key cap with our text (or the sprite), the others are hidden.
void Clone::Apply() {
    for (size_t i = 0; i < icons_.size(); ++i) {
        Il2CppObject* icon = icons_[i].get();
        if (!icon || !unity::IsAlive(icon)) continue;
        Il2CppObject* go = unity::GameObjectOf(icon);
        if (i > 0) {
            unity::SetActive(go, false);
            continue;
        }
        unity::SetActive(go, true);
        auto obj = [&](size_t off) -> Il2CppObject* {
            if (!Has(off)) return nullptr;
            Il2CppObject* o = F<Il2CppObject*>(icon, off);
            return o && unity::IsAlive(o) ? o : nullptr;
        };
        if (Il2CppObject* d = obj(s_.iconDouble)) unity::SetActive(d, false);
        Il2CppObject* glyph = glyph_.Alive() ? glyph_.get() : nullptr;
        Il2CppObject *button = obj(s_.iconButton), *image = obj(s_.iconButtonImage);
        if (glyph && button && image) {  // as InputIcon.SetButtonImage: _buttonRoot on, _keyRoot off
            unity::SetActive(button, true);
            if (Il2CppObject* k = obj(s_.iconKey)) unity::SetActive(k, false);
            SetEnabled(image, true);
            ugui::ImageSetSprite(image, glyph);
            ugui::SetLocalScale(unity::TransformOf(image), {mirrored_ ? -1.0f : 1.0f, 1, 1});
            continue;
        }
        if (button) unity::SetActive(button, false);
        if (Il2CppObject* k = obj(s_.iconKey)) unity::SetActive(k, true);
        SetEnabled(obj(s_.iconKeyImage), false);
        SetEnabled(obj(s_.iconKeyLse), false);
        if (Il2CppObject* text = obj(s_.iconKeyText)) {
            SetEnabled(text, true);
            ugui::TmpSetText(text, key_);
        }
    }
    if (label_.Alive()) ugui::TmpSetText(label_.get(), text_);
}

void Clone::Destroy() {
    if (go_.Alive()) ugui::Destroy(go_.get());
    go_.Reset(nullptr);
    label_.Reset(nullptr);
    icons_.clear();
    reapply_ = 0;
}

void Clone::Abandon() {
    go_.Abandon();
    label_.Abandon();
    glyph_.Abandon();
    for (auto& r : icons_) r.Abandon();
    icons_.clear();
}

}  // namespace sr2::modules::legend_hint
