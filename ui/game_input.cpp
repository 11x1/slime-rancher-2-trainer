#include "game_input.h"

#include <atomic>
#include <cstddef>
#include <string>

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
    // Input System gameplay map
    Il2CppClass* inputDirector = nullptr;
    size_t mainGameOffset = 0;
    const MethodInfo* mapRefToMap = nullptr;  // InputActionMapReference.op_Implicit(InputActionMapReference)
    const MethodInfo* mapGetEnabled = nullptr;
    const MethodInfo* mapEnable = nullptr;
    const MethodInfo* mapDisable = nullptr;
    bool mapOk = false;
    // Cursor
    const MethodInfo* getLockState = nullptr;
    const MethodInfo* setLockState = nullptr;
    bool cursorOk = false;
};

Api g_api;
bool g_active = false;          // lock applied
bool g_weDisabledMap = false;
Il2CppObject* g_map = nullptr;  // InputActionMap we disabled (kept alive by InputDirector)
int g_prevLock = kCursorNone;
bool g_cursorTouched = false;
std::atomic<bool> g_softCursor{false};
std::atomic<bool> g_cursorUnavailable{false};
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
    }
    if (!g_api.mapOk)
        SR2_WARN("ui: gameplay input lock unavailable ({}); the overlay only filters window messages", err);

    err.clear();
    Il2CppClass* cursor = il::Class("UnityEngine.CoreModule", "UnityEngine", "Cursor", &err);
    g_api.getLockState = il::Method(cursor, "get_lockState", 0, &err);
    g_api.setLockState = il::Method(cursor, "set_lockState", 1, &err);
    g_api.cursorOk = g_api.getLockState && g_api.setLockState;
    if (!g_api.cursorOk) {
        g_cursorUnavailable = true;
        SR2_WARN("ui: cursor unlock unavailable ({})", err);
    }
    SR2_DEBUG("ui: input lock resolved (map {}, cursor {})", g_api.mapOk, g_api.cursorOk);
}

Il2CppObject* FindDirector() {
    if (g_director && core::unity::IsAlive(g_director)) return g_director;
    g_director = core::unity::FindObjectOfType(g_api.inputDirector);
    return g_director;
}

Il2CppObject* MainGameMap() {
    Il2CppObject* dir = FindDirector();
    if (!dir) return nullptr;
    void* refPtr = reinterpret_cast<char*>(dir) + g_api.mainGameOffset;  // struct InputActionMapReference (by value)
    std::string err;
    Il2CppObject* m = il::Invoke(g_api.mapRefToMap, nullptr, {refPtr}, &err);
    if (!m && g_mapWarned++ == 0) SR2_WARN("ui: InputDirector._mainGame has no map ({})", err);
    return m;
}

bool MapEnabled(Il2CppObject* m) {
    bool en = false;
    Il2CppObject* r = il::Invoke(g_api.mapGetEnabled, m, {});
    return il::UnboxTo(r, en) && en;
}

int GetLock() {
    int v = kCursorNone;
    il::UnboxTo(il::Invoke(g_api.getLockState, nullptr, {}), v);
    return v;
}

void SetLock(int v) { il::Invoke(g_api.setLockState, nullptr, {&v}); }

void Apply() {
    if (g_api.mapOk) {
        if (Il2CppObject* m = MainGameMap()) {
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
    if (g_api.cursorOk) {
        int cur = GetLock();
        if (!g_cursorTouched) {
            g_prevLock = cur;
            g_cursorTouched = true;
            g_softCursor = (cur == kCursorLocked);
        }
        if (cur != kCursorNone) SetLock(kCursorNone);
    }
}

void Release() {
    if (g_weDisabledMap && g_map && core::unity::IsAlive(g_director)) {
        il::Invoke(g_api.mapEnable, g_map, {});
        SR2_DEBUG("ui: gameplay input map re-enabled");
    }
    g_weDisabledMap = false;
    g_map = nullptr;
    if (g_cursorTouched && g_api.cursorOk) SetLock(g_prevLock);
    g_cursorTouched = false;
    g_softCursor = false;
}

}  // namespace

void Tick(bool overlayOpen) {
    // Fault policy: after a native exception inside game code, never call into
    // the game again (the lock stays as it is; the overlay draws its own cursor).
    if (il::AnyFault()) return;
    if (!overlayOpen && !g_active) return;
    Resolve();
    if (overlayOpen) {
        Apply();
        g_active = true;
    } else {
        Release();
        g_active = false;
    }
}

bool WantSoftwareCursor() { return g_softCursor.load() || g_cursorUnavailable.load() || il::AnyFault(); }

}  // namespace sr2::ui::game_input
