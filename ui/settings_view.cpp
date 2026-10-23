#include "settings_view.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <exception>
#include <format>
#include <mutex>
#include <unordered_map>

#include "core/log.h"
#include "core/main_thread.h"
#include "core/module_manager.h"

namespace sr2::ui {
namespace {

std::mutex g_mutex;
struct Pending {
    double value;
    unsigned long long tick;
};
std::unordered_map<std::string, Pending> g_pending;  // "module\x1fkey"
unsigned long long g_lastWrite = 0;
std::string g_optionsTabStatus = "not started";
std::string g_overlayStatus = "not started";

std::string PendingKey(std::string_view module, std::string_view key) {
    std::string k;
    k.reserve(module.size() + key.size() + 1);
    k.append(module);
    k.push_back('\x1f');
    k.append(key);
    return k;
}

bool NearlyEqual(double a, double b) { return std::fabs(a - b) <= 1e-6 * std::max(1.0, std::fabs(a)); }

}  // namespace

void RegisterUiSettings() {
    static std::once_flag once;
    std::call_once(once, [] {
        settings::Def hk;
        hk.module = kUiModule;
        hk.key = kKeyOverlayHotkey;
        hk.label = "Overlay hotkey";
        hk.kind = settings::Kind::Key;
        hk.def = VK_F8;
        hk.min = 1;
        hk.max = 254;
        hk.step = 1;
        hk.tooltip = "Opens and closes the sr2mods settings overlay.";
        settings::Register(hk);

        settings::Def hint;
        hint.module = kUiModule;
        hint.key = kKeyOverlayHint;
        hint.label = "Show overlay hint at startup";
        hint.kind = settings::Kind::Bool;
        hint.def = 1;
        hint.min = 0;
        hint.max = 1;
        hint.step = 1;
        hint.tooltip = "Shows a short \"press <hotkey> for mod settings\" note when the game starts.";
        settings::Register(hint);

        settings::Def theme;
        theme.module = kUiModule;
        theme.key = kKeyOverlayTheme;
        theme.label = "Overlay theme";
        theme.kind = settings::Kind::Enum;
        theme.def = 0;
        theme.min = 0;
        theme.max = 1;
        theme.step = 1;
        theme.options = {"Dark", "Light"};
        theme.tooltip = "Colours of the sr2mods overlay. Also switchable at the bottom of the overlay's sidebar.";
        settings::Register(theme);
    });
}

const settings::Def* FindDef(const ModuleView& mv, std::string_view key) {
    if (mv.hasEnabled && mv.enabled.key == key) return &mv.enabled;
    for (const auto& g : mv.groups)
        for (const auto& d : g.defs)
            if (d.key == key) return &d;
    return nullptr;
}

bool ParentChainAllows(const ModuleView& mv, const settings::Def& d) {
    const settings::Def* cur = &d;
    for (int depth = 0; depth < 8 && !cur->parent.empty(); ++depth) {  // depth cap: cycles
        const settings::Def* p = FindDef(mv, cur->parent);
        if (!p) return true;  // parent not registered: nothing to hide behind
        if (!settings::ParentAllows(*cur, Read(*p))) return false;
        cur = p;
    }
    return true;
}

bool Visible(const ModuleView& mv, const settings::Def& d) {
    if (d.key != kKeyEnabled && mv.hasEnabled && Read(mv.enabled) < 0.5) return false;
    return ParentChainAllows(mv, d);
}

std::string PrettyModuleName(std::string_view module) {
    if (module == kUiModule) return "UI / Overlay";
    std::string out;
    bool up = true;
    for (char c : module) {
        if (c == '_' || c == '-' || c == '.') {
            out.push_back(' ');
            up = true;
            continue;
        }
        if (up && c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
        out.push_back(c);
        up = false;
    }
    // A few well-known acronyms.
    for (const char* acr : {"Hl2", "Ui", "Hud"}) {
        std::string a = acr;
        size_t p = out.find(a);
        if (p != std::string::npos && (p + a.size() == out.size() || out[p + a.size()] == ' ') &&
            (p == 0 || out[p - 1] == ' ')) {
            for (size_t i = p; i < p + a.size(); ++i)
                if (out[i] >= 'a' && out[i] <= 'z') out[i] = static_cast<char>(out[i] - 'a' + 'A');
        }
    }
    return out;
}

std::vector<ModuleView> BuildModuleViews() {
    std::vector<settings::Def> all = settings::All();
    std::vector<std::string> order = settings::Modules();
    // Modules() should already be in registration order; make sure every def's
    // module appears even if Modules() lags behind.
    for (const auto& d : all)
        if (std::find(order.begin(), order.end(), d.module) == order.end()) order.push_back(d.module);
    // The UI's own section goes last.
    auto ui = std::find(order.begin(), order.end(), std::string(kUiModule));
    if (ui != order.end()) {
        order.erase(ui);
        order.push_back(kUiModule);
    }

    std::vector<ModuleView> views;
    views.reserve(order.size());
    for (const auto& m : order) {
        ModuleView v;
        v.module = m;
        v.title = PrettyModuleName(m);
        for (const auto& d : all) {
            if (d.module != m) continue;
            if (d.key == kKeyEnabled && d.kind == settings::Kind::Bool) {
                v.hasEnabled = true;
                v.enabled = d;
                continue;
            }
            auto g = std::find_if(v.groups.begin(), v.groups.end(), [&](const GroupView& gv) { return gv.name == d.group; });
            if (g == v.groups.end()) {
                v.groups.push_back(GroupView{d.group, {}});
                g = std::prev(v.groups.end());
            }
            g->defs.push_back(d);
        }
        // Ungrouped settings first.
        std::stable_partition(v.groups.begin(), v.groups.end(), [](const GroupView& g) { return g.name.empty(); });
        views.push_back(std::move(v));
    }
    return views;
}

std::string KeyName(int vk) {
    if (vk <= 0 || vk > 254) return "(none)";
    // Fixed names first: mouse buttons, and keys whose GetKeyNameTextW result
    // is missing, layout-dependent in an unhelpful way, or ambiguous.
    switch (vk) {
        case VK_LBUTTON: return "Mouse 1";
        case VK_RBUTTON: return "Mouse 2";
        case VK_MBUTTON: return "Mouse 3";
        case VK_XBUTTON1: return "Mouse 4";
        case VK_XBUTTON2: return "Mouse 5";
        case VK_CANCEL: return "Break";
        case VK_PAUSE: return "Pause";
        case VK_SNAPSHOT: return "Print Screen";
        case VK_LSHIFT: return "Left Shift";
        case VK_RSHIFT: return "Right Shift";
        case VK_LCONTROL: return "Left Ctrl";
        case VK_RCONTROL: return "Right Ctrl";
        case VK_LMENU: return "Left Alt";
        case VK_RMENU: return "Right Alt";
        case VK_LWIN: return "Left Win";
        case VK_RWIN: return "Right Win";
        case VK_APPS: return "Menu";
        case VK_BROWSER_BACK: return "Browser Back";
        case VK_BROWSER_FORWARD: return "Browser Forward";
        case VK_VOLUME_MUTE: return "Volume Mute";
        case VK_VOLUME_DOWN: return "Volume Down";
        case VK_VOLUME_UP: return "Volume Up";
        case VK_MEDIA_NEXT_TRACK: return "Next Track";
        case VK_MEDIA_PREV_TRACK: return "Previous Track";
        case VK_MEDIA_STOP: return "Media Stop";
        case VK_MEDIA_PLAY_PAUSE: return "Play/Pause";
        default: break;
    }
    if (vk >= VK_F1 && vk <= VK_F24) return std::format("F{}", vk - VK_F1 + 1);

    // Keyboard keys: localized name from the scan code. MAPVK_VK_TO_VSC_EX
    // reports the 0xE0 prefix for most extended keys; the explicit list covers
    // the ones it misses, so e.g. Insert is not named "Num 0".
    UINT sc = MapVirtualKeyW(static_cast<UINT>(vk), MAPVK_VK_TO_VSC_EX);
    bool extended = (sc & 0xFF00) == 0xE000 || (sc & 0xFF00) == 0xE100;
    sc &= 0xFF;
    switch (vk) {
        case VK_INSERT: case VK_DELETE: case VK_HOME: case VK_END: case VK_PRIOR: case VK_NEXT:
        case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN: case VK_DIVIDE: case VK_NUMLOCK:
            extended = true;
            break;
        default: break;
    }
    if (sc != 0) {
        LONG lp = static_cast<LONG>(sc << 16);
        if (extended) lp |= 1 << 24;
        wchar_t buf[64] = {};
        if (GetKeyNameTextW(lp, buf, 64) > 0) {
            std::string name = log::Narrow(buf);
            if (!name.empty()) return name;
        }
    }
    return std::format("VK 0x{:02X}", vk);
}

double Snap(const settings::Def& d, double v) {
    if (std::isnan(v)) v = d.def;
    switch (d.kind) {
        case settings::Kind::Bool: return v >= 0.5 ? 1.0 : 0.0;
        case settings::Kind::Key: return std::clamp(std::round(v), 0.0, 254.0);
        case settings::Kind::Enum: {
            double hi = d.options.empty() ? d.max : static_cast<double>(d.options.size() - 1);
            return std::clamp(std::round(v), 0.0, std::max(0.0, hi));
        }
        case settings::Kind::Int:
        case settings::Kind::Float: {
            double lo = std::min(d.min, d.max), hi = std::max(d.min, d.max);
            v = std::clamp(v, lo, hi);
            double step = d.step;
            if (d.kind == settings::Kind::Int) step = std::max(1.0, std::round(step));
            if (step > 0) {
                double n = std::round((v - lo) / step);
                v = lo + n * step;
                if (v > hi + 1e-9) v -= step;
                v = std::clamp(v, lo, hi);
            }
            if (d.kind == settings::Kind::Int) v = std::round(v);
            return v;
        }
    }
    return v;
}

int StepDecimals(double step) {
    if (step <= 0) return 2;
    for (int dec = 0; dec <= 4; ++dec) {
        double scaled = step * std::pow(10.0, dec);
        if (std::fabs(scaled - std::round(scaled)) < 1e-6) return dec;
    }
    return 4;
}

std::string FormatValue(const settings::Def& d, double v) {
    switch (d.kind) {
        case settings::Kind::Bool: return v >= 0.5 ? "On" : "Off";
        case settings::Kind::Int: return std::format("{}", static_cast<long long>(std::llround(v)));
        case settings::Kind::Float: return std::format("{:.{}f}", v, StepDecimals(d.step));
        case settings::Kind::Enum: {
            long long i = std::llround(v);
            if (i >= 0 && static_cast<size_t>(i) < d.options.size()) return d.options[static_cast<size_t>(i)];
            return std::format("#{}", i);
        }
        case settings::Kind::Key: return KeyName(static_cast<int>(std::lround(v)));
    }
    return std::format("{}", v);
}

void Write(const settings::Def& d, double v) {
    v = Snap(d, v);
    {
        std::lock_guard lk(g_mutex);
        g_lastWrite = GetTickCount64();
    }
    namespace mt = ::sr2::core::main_thread;
    if (mt::Ready() && !mt::IsMainThread()) {
        std::string module = d.module, key = d.key;
        {
            std::lock_guard lk(g_mutex);
            g_pending[PendingKey(module, key)] = Pending{v, GetTickCount64()};
        }
        mt::Post([module, key, v] {
            settings::Set(module, key, v);
            std::lock_guard lk(g_mutex);
            auto it = g_pending.find(PendingKey(module, key));
            if (it != g_pending.end() && NearlyEqual(it->second.value, v)) g_pending.erase(it);
        });
        return;
    }
    settings::Set(d.module, d.key, v);
}

double Read(const settings::Def& d) {
    {
        std::lock_guard lk(g_mutex);
        auto it = g_pending.find(PendingKey(d.module, d.key));
        if (it != g_pending.end()) {
            // A posted write that never ran (dispatcher stalled, e.g. during a
            // scene load) must not pin the UI forever.
            if (GetTickCount64() - it->second.tick < 3000) return it->second.value;
            g_pending.erase(it);
        }
    }
    return settings::Get(d.module, d.key);
}

std::string ModuleStatus(std::string_view module, bool* failed) {
    if (failed) *failed = false;
    for (const auto& info : ::sr2::core::ModuleManager::Get().List()) {
        if (info.name != module) continue;
        if (failed) *failed = info.state == ::sr2::core::ModuleState::Failed;
        std::string s = ::sr2::core::StateName(info.state);
        if (!info.reason.empty()) s += ": " + info.reason;
        return s;
    }
    return {};
}

unsigned long long LastWriteTick() {
    std::lock_guard lk(g_mutex);
    return g_lastWrite;
}

void FlushNow() {
    try {
        settings::Flush();
    } catch (const std::exception& e) {
        SR2_WARN("ui: settings flush failed: {}", e.what());
    } catch (...) {
        SR2_WARN("ui: settings flush failed");
    }
}

void SetOptionsTabStatus(std::string s) {
    std::lock_guard lk(g_mutex);
    g_optionsTabStatus = std::move(s);
}
std::string OptionsTabStatus() {
    std::lock_guard lk(g_mutex);
    return g_optionsTabStatus;
}
void SetOverlayStatus(std::string s) {
    std::lock_guard lk(g_mutex);
    g_overlayStatus = std::move(s);
}
std::string OverlayStatus() {
    std::lock_guard lk(g_mutex);
    return g_overlayStatus;
}

}  // namespace sr2::ui
