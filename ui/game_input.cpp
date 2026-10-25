#include "game_input.h"

#include <windows.h>

#include <atomic>
#include <cstddef>
#include <format>
#include <string>
#include <vector>

#include "core/log.h"
#include "core/unity.h"
#include "il2cpp_util.h"

namespace sr2::ui::game_input {
namespace {

// UnityEngine.CursorLockMode
constexpr int kCursorNone = 0;
constexpr int kCursorLocked = 1;

struct Api {
    bool resolved = false;
    // Input System maps
    Il2CppClass* inputDirector = nullptr;
    size_t mainGameOffset = 0;
    size_t pausedOffset = 0;                  // InputDirector._paused (menus); 0 = not found
    const MethodInfo* mapRefToMap = nullptr;  // InputActionMapReference.op_Implicit(InputActionMapReference)
    const MethodInfo* mapGetEnabled = nullptr;
    const MethodInfo* mapEnable = nullptr;
    const MethodInfo* mapDisable = nullptr;
    bool mapOk = false;
    // The menu map is held off action by action (InputActionMap.m_Actions, InputAction.enabled / Enable /
    // Disable): in gameplay only the EventSystem module's actions of that map are on, and a whole-map Enable()
    // afterwards also turned on CloseMap (M) and the rest of the menu map in gameplay (round 14).
    size_t mapActionsOffset = 0;
    const MethodInfo* actGetEnabled = nullptr;
    const MethodInfo* actEnable = nullptr;
    const MethodInfo* actDisable = nullptr;
    // EventSystem input module (the game's UI pointer)
    const MethodInfo* esCurrent = nullptr;      // EventSystem.get_current (static)
    const MethodInfo* esModule = nullptr;       // EventSystem.get_currentInputModule
    const MethodInfo* behGetEnabled = nullptr;  // Behaviour.get_enabled
    const MethodInfo* behSetEnabled = nullptr;  // Behaviour.set_enabled(bool)
    bool moduleOk = false;
    // Cursor
    const MethodInfo* getLockState = nullptr;
    const MethodInfo* setLockState = nullptr;
    const MethodInfo* setVisible = nullptr;   // Cursor.set_visible (there is no getter in this build)
    bool cursorOk = false;
    // The game's own cursor state (MonomiPark...Interaction.CursorManager, static)
    const MethodInfo* cmGetLock = nullptr;     // get_LockState: what the game wants (we only set Cursor.lockState)
    const MethodInfo* cmShouldShow = nullptr;  // ShouldCursorBeVisible (private)
    const MethodInfo* cmApplyLock = nullptr;   // UpdateCursorLockState (private): Cursor.lockState = _lockState
    const MethodInfo* cmApplyShow = nullptr;   // UpdateCursorVisibility (private): Cursor.visible = ShouldCursorBeVisible
};

Api g_api;
bool g_active = false;          // lock applied
bool g_weDisabledMap = false;
Il2CppObject* g_map = nullptr;  // InputActionMap we disabled (kept alive by InputDirector)
// Menu map actions we disabled (pointer over the overlay / typing); the InputDirector's asset keeps them alive.
// `inMenu`: the game had a menu open when we took the action (its CursorManager wanted the cursor free).
struct HeldAction {
    Il2CppObject* action;
    bool inMenu;
};
std::vector<HeldAction> g_heldActions;
bool g_menuBlocked = false;
bool g_heldLogged = false;
Il2CppObject* g_module = nullptr;     // the EventSystem input module we disabled
int g_prevLock = kCursorNone;
bool g_prevVisible = true;
bool g_cursorTouched = false;
std::atomic<bool> g_softCursor{false};
std::atomic<bool> g_cursorUnavailable{false};
std::atomic<bool> g_gameMenu{true};
Il2CppObject* g_director = nullptr;
int g_mapWarned = 0;

void Resolve() {
    if (g_api.resolved) return;
    g_api.resolved = true;
    std::string err;

    g_api.inputDirector = il::Class("Assembly-CSharp", "MonomiPark.SlimeRancher.Input", "InputDirector", &err);
    Il2CppClass* mapRef =
        il::Class("Assembly-CSharp", "MonomiPark.SlimeRancher.Input", "InputActionMapReference", &err);
    Il2CppClass* map = il::Class("Unity.InputSystem", "UnityEngine.InputSystem", "InputActionMap", &err);
    if (g_api.inputDirector && mapRef && map && il::FieldOffset(g_api.inputDirector, "_mainGame", g_api.mainGameOffset, &err)) {
        g_api.mapRefToMap = il::Method(mapRef, "op_Implicit", 1, &err);
        g_api.mapGetEnabled = il::Method(map, "get_enabled", 0, &err);
        g_api.mapEnable = il::Method(map, "Enable", 0, &err);
        g_api.mapDisable = il::Method(map, "Disable", 0, &err);
        g_api.mapOk = g_api.mapRefToMap && g_api.mapGetEnabled && g_api.mapEnable && g_api.mapDisable;
        std::string perr;
        if (g_api.mapOk && !il::FieldOffset(g_api.inputDirector, "_paused", g_api.pausedOffset, &perr)) {
            g_api.pausedOffset = 0;
            SR2_WARN("ui: menu input map unavailable ({}); game menus behind the overlay may react to its input", perr);
        }
        if (g_api.mapOk && g_api.pausedOffset) {
            Il2CppClass* act = il::Class("Unity.InputSystem", "UnityEngine.InputSystem", "InputAction", &perr);
            if (act && il::FieldOffset(map, "m_Actions", g_api.mapActionsOffset, &perr)) {
                g_api.actGetEnabled = il::Method(act, "get_enabled", 0, &perr);
                g_api.actEnable = il::Method(act, "Enable", 0, &perr);
                g_api.actDisable = il::Method(act, "Disable", 0, &perr);
            }
            if (!g_api.actGetEnabled || !g_api.actEnable || !g_api.actDisable) {
                g_api.pausedOffset = 0;  // a whole-map Disable / Enable would leave menu actions on in gameplay
                SR2_WARN("ui: menu input actions unavailable ({}); game menus behind the overlay may react to its "
                         "input",
                         perr);
            }
        }
    }
    if (!g_api.mapOk)
        SR2_WARN("ui: gameplay input lock unavailable ({}); the overlay only filters window messages", err);

    {
        std::string merr;
        Il2CppClass* es = il::Class("UnityEngine.UI", "UnityEngine.EventSystems", "EventSystem", &merr);
        Il2CppClass* beh = il::Class("UnityEngine.CoreModule", "UnityEngine", "Behaviour", &merr);
        g_api.esCurrent = il::Method(es, "get_current", 0, &merr);
        g_api.esModule = il::Method(es, "get_currentInputModule", 0, &merr);
        g_api.behGetEnabled = il::Method(beh, "get_enabled", 0, &merr);
        g_api.behSetEnabled = il::Method(beh, "set_enabled", 1, &merr);
        g_api.moduleOk = g_api.esCurrent && g_api.esModule && g_api.behGetEnabled && g_api.behSetEnabled;
        if (!g_api.moduleOk) SR2_WARN("ui: UI input module lock unavailable ({})", merr);
    }

    err.clear();
    Il2CppClass* cursor = il::Class("UnityEngine.CoreModule", "UnityEngine", "Cursor", &err);
    g_api.getLockState = il::Method(cursor, "get_lockState", 0, &err);
    g_api.setLockState = il::Method(cursor, "set_lockState", 1, &err);
    g_api.cursorOk = g_api.getLockState && g_api.setLockState;
    if (!g_api.cursorOk) {
        g_cursorUnavailable = true;
        SR2_WARN("ui: cursor unlock unavailable ({})", err);
    }
    std::string verr;
    g_api.setVisible = il::Method(cursor, "set_visible", 1, &verr);
    if (!g_api.setVisible) SR2_WARN("ui: cursor show unavailable ({}); a software cursor is drawn instead", verr);

    std::string cerr;
    if (Il2CppClass* cm = il::Class("Assembly-CSharp", "MonomiPark.SlimeRancher.UI.Framework.Interaction", "CursorManager", &cerr)) {
        g_api.cmGetLock = il::Method(cm, "get_LockState", 0, &cerr);
        g_api.cmShouldShow = il::Method(cm, "ShouldCursorBeVisible", 0, &cerr);
        g_api.cmApplyLock = il::Method(cm, "UpdateCursorLockState", 0, &cerr);
        g_api.cmApplyShow = il::Method(cm, "UpdateCursorVisibility", 0, &cerr);
    }
    if (!g_api.cmGetLock || !g_api.cmShouldShow || !g_api.cmApplyLock || !g_api.cmApplyShow)
        SR2_WARN("ui: CursorManager incomplete ({}); the cursor is restored from what the overlay saw on opening", cerr);
    SR2_DEBUG("ui: input lock resolved (map {}, menu map {}, UI module {}, cursor {}, show {}, CursorManager {})",
              g_api.mapOk, g_api.pausedOffset != 0, g_api.moduleOk, g_api.cursorOk, g_api.setVisible != nullptr,
              g_api.cmGetLock != nullptr);
}

// While there is no InputDirector (title screen, loading) the search is retried at
// most every kDirectorRetryMs: a FindObjectOfType on every tick cost 1-2 ms per
// frame while the overlay was open there (perf: frame:ui.overlay, 570 frames > 2 ms).
constexpr unsigned long long kDirectorRetryMs = 1000;
unsigned long long g_directorTry = 0;

Il2CppObject* FindDirector() {
    if (g_director && core::unity::IsAlive(g_director)) return g_director;
    g_director = nullptr;
    const unsigned long long now = GetTickCount64();
    if (g_directorTry && now - g_directorTry < kDirectorRetryMs) return nullptr;
    g_directorTry = now;
    g_director = core::unity::FindObjectOfType(g_api.inputDirector);
    if (g_director) g_directorTry = 0;  // found: the next loss searches at once
    return g_director;
}

Il2CppObject* DirectorMap(size_t offset, const char* what) {
    Il2CppObject* dir = FindDirector();
    if (!dir || !offset) return nullptr;
    void* refPtr = reinterpret_cast<char*>(dir) + offset;  // struct InputActionMapReference (by value)
    std::string err;
    Il2CppObject* m = il::Invoke(g_api.mapRefToMap, nullptr, {refPtr}, &err);
    if (!m && g_mapWarned++ == 0) SR2_WARN("ui: InputDirector.{} has no map ({})", what, err);
    return m;
}

bool MapEnabled(Il2CppObject* m) {
    bool en = false;
    Il2CppObject* r = il::Invoke(g_api.mapGetEnabled, m, {});
    return il::UnboxTo(r, en) && en;
}

bool ActionEnabled(Il2CppObject* a) {
    bool en = false;
    return il::UnboxTo(il::Invoke(g_api.actGetEnabled, a, {}), en) && en;
}

// The game itself wants a menu (CursorManager.LockState not Locked: pause menu, full map, ...). Without
// CursorManager: assumed true, so a held menu action is always handed back.
bool GameWantsMenu() {
    int want = kCursorNone;
    if (!g_api.cmGetLock || !il::UnboxTo(il::Invoke(g_api.cmGetLock, nullptr, {}), want)) return true;
    return want != kCursorLocked;
}

// Disables every action of the menu map that is on and remembers each one. Runs every tick while blocking, so
// actions the game turns on meanwhile (it opened a menu) are taken too.
void HoldMenuActions(Il2CppObject* m) {
    void* arr = core::unity::Field<void*>(m, g_api.mapActionsOffset);
    size_t n = core::unity::ArrayLength(arr);
    bool menu = GameWantsMenu();
    for (size_t i = 0; i < n; ++i) {
        auto* a = static_cast<Il2CppObject*>(core::unity::ArrayAt(arr, i));
        if (!a || !ActionEnabled(a)) continue;
        il::Invoke(g_api.actDisable, a, {});
        bool known = false;
        for (const HeldAction& h : g_heldActions) known |= h.action == a;
        if (!known) g_heldActions.push_back({a, menu});
    }
}

// Hands back exactly the actions we took. If the game left its menu meanwhile (its own
// InputDirector.UpdateInputMaps(false) -> _paused.Disable() found them already off), the ones taken during that
// menu stay off: on in gameplay they arm CloseMap on M, and the next M opened the map and its release closed it
// again (round 14 "map closed instantly after 1-5 frames").
void ReleaseMenuActions() {
    if (g_heldActions.empty()) return;
    bool menu = GameWantsMenu();
    int back = 0, kept = 0;
    if (core::unity::IsAlive(g_director)) {
        for (const HeldAction& h : g_heldActions) {
            if (h.inMenu && !menu) {
                ++kept;
                continue;
            }
            il::Invoke(g_api.actEnable, h.action, {});
            ++back;
        }
    }
    if (!g_heldLogged || kept) {
        SR2_INFO("ui: menu input map: {} action(s) handed back after the overlay held them off{}", back,
                 kept ? std::format(", {} left off (the game closed its menu meanwhile)", kept) : std::string());
        g_heldLogged = true;
    }
    g_heldActions.clear();
}

int GetLock() {
    int v = kCursorNone;
    il::UnboxTo(il::Invoke(g_api.getLockState, nullptr, {}), v);
    return v;
}

void SetLock(int v) { il::Invoke(g_api.setLockState, nullptr, {&v}); }
void SetVisible(bool v) { il::Invoke(g_api.setVisible, nullptr, {&v}); }

// The menu map and the EventSystem input module: off while the overlay has the
// pointer (or the keyboard, for the map), back on as soon as it does not.
void BlockMenus(bool pointer, bool typing) {
    if (g_api.mapOk && g_api.pausedOffset) {
        if (pointer || typing) {
            if (Il2CppObject* m = DirectorMap(g_api.pausedOffset, "_paused"); m && MapEnabled(m)) {
                HoldMenuActions(m);
                if (!g_menuBlocked) SR2_DEBUG("ui: menu input map held off (pointer on the overlay)");
                g_menuBlocked = true;
            }
        } else if (g_menuBlocked) {
            ReleaseMenuActions();
            g_menuBlocked = false;
        }
    }
    if (!g_api.moduleOk) return;
    if (pointer) {
        if (g_module) return;  // already off (the EventSystem may no longer report it as current)
        Il2CppObject* es = il::Invoke(g_api.esCurrent, nullptr, {});
        if (!es || !core::unity::IsAlive(es)) return;
        Il2CppObject* mod = il::Invoke(g_api.esModule, es, {});
        bool en = false;
        if (!mod || !core::unity::IsAlive(mod) || !il::UnboxTo(il::Invoke(g_api.behGetEnabled, mod, {}), en) || !en) return;
        bool off = false;
        il::Invoke(g_api.behSetEnabled, mod, {&off});
        g_module = mod;
    } else if (g_module) {
        if (core::unity::IsAlive(g_module)) {
            bool on = true;
            il::Invoke(g_api.behSetEnabled, g_module, {&on});
        }
        g_module = nullptr;
    }
}

void Apply(bool pointer, bool typing) {
    if (g_api.mapOk) {
        if (Il2CppObject* m = DirectorMap(g_api.mainGameOffset, "_mainGame")) {
            // The game may re-enable the map on its own (scene/menu changes);
            // re-assert every tick while open.
            if (MapEnabled(m)) {
                il::Invoke(g_api.mapDisable, m, {});
                if (!g_weDisabledMap) SR2_DEBUG("ui: gameplay input map disabled");
                g_weDisabledMap = true;
                g_map = m;
            }
        }
    }
    BlockMenus(pointer, typing);
    if (g_api.cursorOk) {
        int cur = GetLock();
        if (!g_cursorTouched) {
            g_prevLock = cur;
            bool show = cur != kCursorLocked;
            if (g_api.cmShouldShow) il::UnboxTo(il::Invoke(g_api.cmShouldShow, nullptr, {}), show);
            g_prevVisible = show;
            g_cursorTouched = true;
            g_softCursor = (cur == kCursorLocked) && !g_api.setVisible;
            SR2_DEBUG("ui: cursor on open: lock {}, visible {}", cur, show);
        }
        if (cur != kCursorNone) SetLock(kCursorNone);
        // The game hides the cursor in some menus (the full map: CursorManager.HideCursorFor) and may do it again
        // at any time (pointer enter / exit events), so it is shown on every tick.
        if (g_api.setVisible) SetVisible(true);
    }
    // A game menu is open when the game itself wants the cursor free (we only change Cursor.lockState).
    int want = g_prevLock;
    if (g_api.cmGetLock) il::UnboxTo(il::Invoke(g_api.cmGetLock, nullptr, {}), want);
    g_gameMenu = want != kCursorLocked;
}

void Release() {
    BlockMenus(false, false);
    if (g_weDisabledMap && g_map && core::unity::IsAlive(g_director)) {
        il::Invoke(g_api.mapEnable, g_map, {});
        SR2_DEBUG("ui: gameplay input map re-enabled");
    }
    g_weDisabledMap = false;
    g_map = nullptr;
    if (g_cursorTouched && g_api.cursorOk) {
        // Back to what the game wants now (it may have opened or closed a menu meanwhile), else to what we saw.
        if (g_api.cmApplyLock) il::Invoke(g_api.cmApplyLock, nullptr, {});
        else SetLock(g_prevLock);
        if (g_api.setVisible) {
            if (g_api.cmApplyShow) il::Invoke(g_api.cmApplyShow, nullptr, {});
            else SetVisible(g_prevVisible);
        }
    }
    g_cursorTouched = false;
    g_softCursor = false;
    g_gameMenu = true;
}

}  // namespace

void Tick(bool overlayOpen, bool pointerOnOverlay, bool typing) {
    // Fault policy: after a native exception inside game code, never call into
    // the game again (the lock stays as it is; the overlay draws its own cursor).
    if (il::AnyFault()) return;
    if (!overlayOpen && !g_active) return;
    Resolve();
    if (overlayOpen) {
        Apply(pointerOnOverlay, typing);
        g_active = true;
    } else {
        Release();
        g_active = false;
    }
}

bool WantSoftwareCursor() { return g_softCursor.load() || g_cursorUnavailable.load() || il::AnyFault(); }

bool GameMenuOpen() { return g_gameMenu.load(); }

}  // namespace sr2::ui::game_input
