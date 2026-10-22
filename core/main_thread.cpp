#include "main_thread.h"

#include <windows.h>

#include <atomic>
#include <format>
#include <memory>
#include <mutex>
#include <vector>

#include "guard.h"
#include "hook_manager.h"
#include "log.h"
#include "perf.h"
#include "resolver.h"

// Which method drives the dispatcher
// ----------------------------------
// Primary: UnityEngine.EventSystems.EventSystem::Update (UnityEngine.UI).
//   A real MonoBehaviour.Update with its own 0x260-byte body (not a folded
//   stub; its RVA is unique in the 1.3.0 dump). SR2 keeps an EventSystem alive
//   in the main menu and in-game for its uGUI menus/HUD, so it ticks in both.
//   It is invoked once per EventSystem component per frame (Unity only lets
//   the "current" one do work, but every instance's Update runs).
// Secondary: UnityEngine.Canvas::SendWillRenderCanvases (UnityEngine.UIModule).
//   [RequiredByNativeCode] static, called by the engine once per rendered
//   frame while any canvas exists - covers loading screens or moments with no
//   enabled EventSystem. Unique RVA, own body.
// Both feed one Tick(), de-duplicated with UnityEngine.Time.frameCount so the
// dispatcher runs exactly once per frame, on whichever fires first. If
// Time.frameCount cannot be resolved only the primary hook is installed.

namespace sr2::core::main_thread {
namespace {

using il2cpp::Resolver;

using InstanceVoidFn = void (*)(Il2CppObject*, const MethodInfo*);
using StaticVoidFn = void (*)(const MethodInfo*);
using StaticIntFn = int (*)(const MethodInfo*);
using StaticFloatFn = float (*)(const MethodInfo*);

constexpr const char* kHookModule = "core";

InstanceVoidFn g_origEventSystemUpdate = nullptr;
StaticVoidFn g_origSendWillRender = nullptr;

StaticIntFn g_frameCount = nullptr;
const MethodInfo* g_frameCountMi = nullptr;
StaticFloatFn g_unscaledDt = nullptr;
const MethodInfo* g_unscaledDtMi = nullptr;

std::atomic<bool> g_installed{false};
std::atomic<bool> g_ready{false};
std::atomic<DWORD> g_mainTid{0};
std::atomic<unsigned long long> g_ticks{0};

// Main-thread-only state.
int g_lastFrame = -1;
bool g_inTick = false;

struct Callback {
    int id = 0;
    std::string owner;
    std::function<void(float)> fn;
    std::atomic<bool> alive{true};
    int perfSlot = -1;  // perf::Slot("frame:<owner>")
};

std::mutex g_mutex;  // guards everything below
std::vector<std::function<void()>> g_queue;
std::vector<std::shared_ptr<Callback>> g_callbacks;
int g_nextId = 1;
std::string g_hookDesc;

void RemoveLocked(int id) {
    for (auto it = g_callbacks.begin(); it != g_callbacks.end(); ++it) {
        if ((*it)->id == id) {
            (*it)->alive = false;
            g_callbacks.erase(it);
            return;
        }
    }
}

void RunTick() {
    const unsigned long long tick = ++g_ticks;
    if (!g_ready.load(std::memory_order_acquire)) {
        g_mainTid = GetCurrentThreadId();
        g_ready.store(true, std::memory_order_release);
        SR2_INFO("main thread: dispatcher live on thread {} via {}", GetCurrentThreadId(), HookDescription());
    }

    perf::OnTick();  // frame time / hitches / GC / summaries (no-op when perf_log is off)

    float dt = 0.0f;
    if (g_unscaledDt) Try([&] { dt = g_unscaledDt(g_unscaledDtMi); });

    // 1) posted jobs (FIFO; jobs posted while draining run next tick)
    std::vector<std::function<void()>> jobs;
    {
        std::lock_guard lock(g_mutex);
        jobs.swap(g_queue);
    }
    static const int postedSlot = perf::Slot("main_thread.posted");
    for (auto& job : jobs) {
        perf::Scope ps(postedSlot);
        std::string err;
        if (!Try([&] { job(); }, &err)) SR2_ERROR("main thread: posted job failed at tick {}: {}", tick, err);
    }

    // 2) frame callbacks (snapshot so callbacks may add/remove freely)
    std::vector<std::shared_ptr<Callback>> cbs;
    {
        std::lock_guard lock(g_mutex);
        cbs = g_callbacks;
    }
    for (auto& cb : cbs) {
        if (!cb->alive) continue;
        std::string err;
        bool ok;
        {
            perf::Scope ps(cb->perfSlot);
            ok = Try([&] { cb->fn(dt); }, &err);
        }
        if (!ok) {
            SR2_ERROR("main thread: frame callback #{} ({}) failed: {} - removed", cb->id, cb->owner, err);
            std::lock_guard lock(g_mutex);
            RemoveLocked(cb->id);
        }
    }
}

// Called from the hooks (main thread). Never lets anything escape into game code.
void Tick() {
    if (g_inTick) return;  // re-entrancy (e.g. a callback triggering a canvas rebuild)
    int frame = -1;
    if (g_frameCount) {
        if (!Try([&] { frame = g_frameCount(g_frameCountMi); })) frame = -1;
        if (frame >= 0 && frame == g_lastFrame) return;
        g_lastFrame = frame;
    }
    g_inTick = true;
    std::string err;
    if (!Try([] { RunTick(); }, &err)) SR2_ERROR("main thread: tick failed: {}", err);
    g_inTick = false;
}

void EventSystemUpdateDetour(Il2CppObject* self, const MethodInfo* mi) {
    Tick();
    g_origEventSystemUpdate(self, mi);  // unguarded on purpose: game exceptions keep their normal path
}

void SendWillRenderCanvasesDetour(const MethodInfo* mi) {
    Tick();
    g_origSendWillRender(mi);
}

bool HookOne(const char* image, const char* ns, const char* klass, const char* method, const char* hookName,
             void* detour, void** original, std::string& desc, std::string& err) {
    void* target = Resolver::Get().MethodPointer(image, ns, klass, method, 0, &err);
    if (!target) return false;
    HookManager& hm = HookManager::Get();
    if (!hm.Create(kHookModule, hookName, target, detour, original, &err)) return false;
    if (!hm.Enable(hookName, &err)) {
        hm.Remove(hookName);
        return false;
    }
    desc += std::format("{}{}:{}.{}::{} (GameAssembly+0x{:X})", desc.empty() ? "" : " + ", image, ns, klass, method,
                        Resolver::Rva(target));
    return true;
}

}  // namespace

bool Ready() { return g_ready.load(std::memory_order_acquire); }

bool IsMainThread() {
    DWORD tid = g_mainTid.load();
    return tid != 0 && tid == GetCurrentThreadId();
}

void Post(std::function<void()> fn) {
    if (!fn) return;
    std::lock_guard lock(g_mutex);
    g_queue.push_back(std::move(fn));
}

int AddFrameCallback(std::string_view owner, std::function<void(float unscaledDt)> fn) {
    if (!fn) return 0;
    auto cb = std::make_shared<Callback>();
    cb->owner = std::string(owner);
    cb->fn = std::move(fn);
    cb->perfSlot = perf::Slot(std::format("frame:{}", owner));
    std::lock_guard lock(g_mutex);
    cb->id = g_nextId++;
    g_callbacks.push_back(cb);
    return cb->id;
}

void RemoveFrameCallback(int id) {
    std::lock_guard lock(g_mutex);
    RemoveLocked(id);
}

unsigned long long TickCount() { return g_ticks.load(); }

std::string HookDescription() {
    std::lock_guard lock(g_mutex);
    return g_hookDesc;
}

bool Install(std::string* errOut) {
    if (g_installed) return true;
    Resolver& r = Resolver::Get();
    std::string err;

    // Time getters (direct calls through the method pointer: static, MethodInfo* last).
    if (Il2CppClass* time = r.Class("UnityEngine.CoreModule", "UnityEngine", "Time", &err)) {
        if ((g_frameCountMi = r.Method(time, "get_frameCount", 0, &err)))
            g_frameCount = reinterpret_cast<StaticIntFn>(r.MethodPointer(g_frameCountMi, &err));
        if (!g_frameCount) SR2_WARN("main thread: Time.frameCount unavailable ({}); single-hook mode", err);
        if ((g_unscaledDtMi = r.Method(time, "get_unscaledDeltaTime", 0, &err)))
            g_unscaledDt = reinterpret_cast<StaticFloatFn>(r.MethodPointer(g_unscaledDtMi, &err));
        if (!g_unscaledDt) SR2_WARN("main thread: Time.unscaledDeltaTime unavailable ({}); dt will be 0", err);
    } else {
        SR2_WARN("main thread: UnityEngine.Time not found: {}", err);
    }

    std::string desc, errPrimary, errSecondary;
    bool primary = HookOne("UnityEngine.UI", "UnityEngine.EventSystems", "EventSystem", "Update",
                           "core.main_thread.EventSystem.Update", reinterpret_cast<void*>(&EventSystemUpdateDetour),
                           reinterpret_cast<void**>(&g_origEventSystemUpdate), desc, errPrimary);
    if (!primary) SR2_WARN("main thread: EventSystem.Update hook failed: {}", errPrimary);

    bool secondary = false;
    if (g_frameCount || !primary) {
        secondary = HookOne("UnityEngine.UIModule", "UnityEngine", "Canvas", "SendWillRenderCanvases",
                            "core.main_thread.Canvas.SendWillRenderCanvases",
                            reinterpret_cast<void*>(&SendWillRenderCanvasesDetour),
                            reinterpret_cast<void**>(&g_origSendWillRender), desc, errSecondary);
        if (!secondary) SR2_WARN("main thread: Canvas.SendWillRenderCanvases hook failed: {}", errSecondary);
    }

    if (!primary && !secondary) {
        if (errOut) *errOut = std::format("no dispatcher hook: {}; {}", errPrimary, errSecondary);
        return false;
    }
    {
        std::lock_guard lock(g_mutex);
        g_hookDesc = desc;
    }
    g_installed = true;
    SR2_INFO("main thread: dispatcher hooked: {}", desc);
    return true;
}

}  // namespace sr2::core::main_thread
