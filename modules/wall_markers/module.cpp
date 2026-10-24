// wall_markers: screen-space markers at intact Nullifier walls (Grey Labyrinth).
//
// The Nullifier gadget (class Harmonizer) opens DiscordantWall objects; see
// modules/common/discordant_walls.h and NOTES.md for the game side. This module
// draws a small diamond (+ optional distance) over every intact wall within
// `range` metres of the camera, through the ImGui overlay's foreground draw
// list (ui/hud.h). Harmonized ("opened") walls are not marked.
//
// Data flow:
//   main thread (frame callback)
//     * every 0.25 s: walls::Collect() reads DiscordantWall._activeWalls (the
//       game's own static registry; no hooks, no scene scans) and copies out
//       positions + intact flags of the active walls;
//     * every frame while intact walls exist: Camera.main, its
//       worldToCameraMatrix and projectionMatrix -> vp = P * V, camera position,
//       range filter, ui::hud::Publish(frame) (a copy under a mutex);
//     * hidden (hud::Clear) while the settings overlay / a game menu / the map
//       is open (cursor not locked or time scale 0) or there is no camera.
//   render thread (overlay Present hook): projects the published world points
//     with that frame's vp; frames older than 250 ms are dropped.
//
// Fault policy: no hooks. Every game call runs inside one guarded tick; a
// native fault marks the module faulted and stops it for the session (no
// further game calls); managed exceptions are logged (rate limited) and the
// module stops after 20 of them. No managed pointer is kept between frames.

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <format>
#include <memory>
#include <string>
#include <vector>

#include "core/detour_guard.h"
#include "core/guard.h"
#include "core/il2cpp_api.h"
#include "core/log.h"
#include "core/main_thread.h"
#include "core/module.h"
#include "core/resolver.h"
#include "core/settings.h"
#include "core/unity.h"
#include "modules/common/discordant_walls.h"
#include "ui/hud.h"
#include "ui/overlay.h"

namespace sr2::modules {
namespace {

namespace settings = sr2::core::settings;
namespace unity = sr2::core::unity;
namespace main_thread = sr2::core::main_thread;
using il2cpp::Resolver;

constexpr const char* kModule = "wall_markers";
constexpr float kScanInterval = 0.25f;
constexpr size_t kMaxMarkers = 64;
constexpr int kMaxExceptions = 20;

struct Mat4 { float m[16]; };  // UnityEngine.Matrix4x4, column-major (m00, m10, m20, m30, m01, ...)

template <class Fn>
struct M {
    Fn fn = nullptr;
    const MethodInfo* mi = nullptr;
    explicit operator bool() const { return fn != nullptr; }
};

struct Cfg {
    float range = 60;
    float size = 1;
    bool showDistance = true;
};

class WallMarkers {
public:
    bool Setup(core::ModuleContext& ctx) {
        std::string err;
        if (!walls::Resolve(err)) return ctx.Fail("Nullifier walls not resolvable: " + err);
        Resolver& r = Resolver::Get();
        const char* core = "UnityEngine.CoreModule";
        Il2CppClass* camera = r.Class(core, "UnityEngine", "Camera", &err);
        if (!camera) return ctx.Fail("UnityEngine.Camera: " + err);
        if (!Bind(worldToCamera_, camera, "get_worldToCameraMatrix", err) ||
            !Bind(projection_, camera, "get_projectionMatrix", err))
            return ctx.Fail(err);
        // optional "menu open" signals (the minimap uses the same ones)
        std::string optErr;
        if (Il2CppClass* cursor = r.Class(core, "UnityEngine", "Cursor", &optErr)) Bind(lockState_, cursor, "get_lockState", optErr);
        if (Il2CppClass* time = r.Class(core, "UnityEngine", "Time", &optErr)) Bind(timeScale_, time, "get_timeScale", optErr);
        cameraClass_ = camera;

        RegisterSettings();
        if (!onChange_) onChange_ = settings::OnChange(kModule, [this](const std::string&) { cfgDirty_.store(true); });
        SR2_INFO("wall_markers: resolved (DiscordantWall registry, Camera matrices, menu check: cursor {}, time scale {})",
                 lockState_ ? "yes" : "no", timeScale_ ? "yes" : "no");
        return true;
    }

    void OnEnable() {
        if (dead_.load()) {
            SR2_WARN("wall_markers: stays off for this session after a native fault");
            return;
        }
        cfgDirty_.store(true);
        resetPending_.store(true);  // applied by the next tick (main thread owns the tick state)
        if (!frameCb_) frameCb_ = main_thread::AddFrameCallback(kModule, [this](float dt) { Tick(dt); });
    }

    void OnDisable() {
        if (frameCb_) main_thread::RemoveFrameCallback(frameCb_);
        frameCb_ = 0;
        ui::hud::Clear(kModule);  // a tick still running publishes at most once more; stale after 250 ms
    }

private:
    template <class Fn>
    static bool Bind(M<Fn>& m, Il2CppClass* k, const char* name, std::string& err) {
        std::string e;
        const MethodInfo* mi = Resolver::Get().Method(k, name, 0, &e);
        void* p = mi ? Resolver::Get().MethodPointer(mi, &e) : nullptr;
        if (!p) {
            err = std::format("{}: {}", name, e.empty() ? "no native body" : e);
            return false;
        }
        m.fn = reinterpret_cast<Fn>(p);
        m.mi = mi;
        return true;
    }

    static void RegisterSettings() {
        using settings::Kind;
        auto def = [](const char* key, const char* label, Kind kind, double d, double mn, double mx, double step,
                      const char* tip) {
            settings::Def x;
            x.module = kModule;
            x.key = key;
            x.label = label;
            x.kind = kind;
            x.def = d;
            x.min = mn;
            x.max = mx;
            x.step = step;
            x.tooltip = tip;
            settings::Register(x);
        };
        def("range", "Marker range", Kind::Float, 60, 10, 250, 5,
            "Intact Nullifier walls closer than this get a marker, even behind other walls.");
        def("show_distance", "Show distance", Kind::Bool, 1, 0, 1, 1, "Distance label under each marker");
        def("size", "Marker size", Kind::Float, 1, 0.5, 3, 0.1, "");
        settings::SetDisplay(kModule, "range", " m");
        settings::SetDisplay(kModule, "size", "x");
    }

    static Cfg LoadCfg() {
        Cfg c;
        c.range = std::clamp(settings::GetFloat(kModule, "range"), 1.0f, 1000.0f);
        c.size = std::clamp(settings::GetFloat(kModule, "size"), 0.2f, 5.0f);
        c.showDistance = settings::GetBool(kModule, "show_distance");
        return c;
    }

    void Tick(float dt) {
        if (dead_.load()) return;
        if (core::ModuleFaulted(kModule)) {
            Die("module marked faulted");
            return;
        }
        std::string err;
        core::FaultInfo fault;
        auto body = [&] { TickImpl(dt); };
        using Body = decltype(body);
        bool ok = core::GuardedCallEx([](void* p) { (*static_cast<Body*>(p))(); }, &body, &err, &fault);
        if (ok) return;
        if (fault.code != 0) {
            Die(std::format("native exception {}", err));
            return;
        }
        ui::hud::Clear(kModule);
        published_ = false;
        if (++exceptions_ <= 3) SR2_WARN("wall_markers: tick failed: {}", err);
        if (exceptions_ >= kMaxExceptions) Die(std::format("{} managed exceptions, last: {}", exceptions_, err));
    }

    // Permanent stop: no further game calls this session.
    void Die(const std::string& why) {
        dead_.store(true);
        if (frameCb_) main_thread::RemoveFrameCallback(frameCb_);
        frameCb_ = 0;
        ui::hud::Clear(kModule);
        walls_.clear();
        SR2_ERROR("wall_markers: {} - Nullifier wall markers disabled for the rest of this session", why);
        core::MarkModuleFaulted(kModule, why);
    }

    void Hide() {
        if (published_) ui::hud::Clear(kModule);
        published_ = false;
    }

    bool MenusOpen() {
        if (ui::overlay::IsOpen()) return true;
        if (lockState_ && lockState_.fn(lockState_.mi) != 1 /* CursorLockMode.Locked */) return true;
        if (timeScale_ && timeScale_.fn(timeScale_.mi) == 0.0f) return true;
        return false;
    }

    void Scan() {
        std::vector<walls::Wall> all;
        walls::Stats st;
        if (!walls::Collect(all, &st)) {
            if (!layoutWarned_) {
                layoutWarned_ = true;
                SR2_WARN("wall_markers: DiscordantWall._activeWalls has an unexpected type - no markers");
            }
            walls_.clear();
            return;
        }
        walls_.clear();
        for (const walls::Wall& w : all)
            if (w.intact) walls_.push_back(w);
        // One line whenever the registry changes (zone load / unload, a wall opened or closed again).
        if (st.listed != lastStats_.listed || st.active != lastStats_.active || st.intact != lastStats_.intact) {
            if (st.listed || lastStats_.listed)
                SR2_INFO("wall_markers: Nullifier walls: {} registered, {} alive, {} active, {} intact ({} harmonized), "
                         "{} positioned by collider bounds",
                         st.listed, st.valid, st.active, st.intact, st.active - st.intact, st.fromBounds);
            lastStats_ = st;
        }
    }

    void TickImpl(float dt) {
        if (resetPending_.exchange(false)) {
            walls_.clear();
            published_ = false;
            scanAcc_ = kScanInterval;
        }
        if (cfgDirty_.exchange(false)) cfg_ = LoadCfg();
        scanAcc_ += dt;
        if (scanAcc_ >= kScanInterval) {
            scanAcc_ = 0;
            Scan();
        }
        if (walls_.empty() || MenusOpen()) {
            Hide();
            return;
        }
        Il2CppObject* cam = unity::Camera_main();
        Il2CppClass* camClass = unity::ClassOf(cam);
        if (!cam || !camClass ||
            (camClass != cameraClass_ && (!il2cpp::api().il2cpp_class_is_assignable_from ||
                                          !il2cpp::api().il2cpp_class_is_assignable_from(cameraClass_, camClass)))) {
            Hide();
            return;
        }
        Mat4 v = worldToCamera_.fn(cam, worldToCamera_.mi);
        Mat4 p = projection_.fn(cam, projection_.mi);
        ui::hud::Frame f;
        // vp = P * V (column-major: element (r, c) at [c * 4 + r])
        for (int c = 0; c < 4; ++c)
            for (int r = 0; r < 4; ++r) {
                float s = 0;
                for (int k = 0; k < 4; ++k) s += p.m[k * 4 + r] * v.m[c * 4 + k];
                f.vp[c * 4 + r] = s;
            }
        for (float x : f.vp)
            if (!std::isfinite(x)) {
                Hide();
                return;
            }
        // camera position from V = [R t; 0 1]: pos = -R^T t
        float tx = v.m[12], ty = v.m[13], tz = v.m[14];
        float cx = -(v.m[0] * tx + v.m[1] * ty + v.m[2] * tz);
        float cy = -(v.m[4] * tx + v.m[5] * ty + v.m[6] * tz);
        float cz = -(v.m[8] * tx + v.m[9] * ty + v.m[10] * tz);
        float r2 = cfg_.range * cfg_.range;
        for (const walls::Wall& w : walls_) {
            float dx = w.x - cx, dy = w.y - cy, dz = w.z - cz;
            float d2 = dx * dx + dy * dy + dz * dz;
            if (d2 > r2) continue;
            f.markers.push_back({w.x, w.y, w.z, std::sqrt(d2)});
        }
        if (f.markers.empty()) {
            Hide();
            return;
        }
        if (f.markers.size() > kMaxMarkers) {
            std::partial_sort(f.markers.begin(), f.markers.begin() + kMaxMarkers, f.markers.end(),
                              [](const auto& a, const auto& b) { return a.distance < b.distance; });
            f.markers.resize(kMaxMarkers);
        }
        f.range = cfg_.range;
        f.size = cfg_.size;
        f.showDistance = cfg_.showDistance;
        f.color = IM_COL32_RGBA(224, 102, 255, 235);
        if (!firstShownLogged_) {
            firstShownLogged_ = true;
            SR2_INFO("wall_markers: first markers drawn: {} intact wall(s) within {:.0f} m (camera at {:.1f}, {:.1f}, "
                     "{:.1f}; nearest at {:.1f} m)",
                     f.markers.size(), cfg_.range, cx, cy, cz,
                     std::min_element(f.markers.begin(), f.markers.end(),
                                      [](const auto& a, const auto& b) { return a.distance < b.distance; })
                         ->distance);
        }
        ui::hud::Publish(kModule, std::move(f));
        published_ = true;
    }

    // IM_COL32 layout without including imgui here: A<<24 | B<<16 | G<<8 | R
    static constexpr uint32_t IM_COL32_RGBA(uint32_t r, uint32_t g, uint32_t b, uint32_t a) {
        return (a << 24) | (b << 16) | (g << 8) | r;
    }

    M<Mat4 (*)(Il2CppObject*, const MethodInfo*)> worldToCamera_, projection_;
    M<int (*)(const MethodInfo*)> lockState_;
    M<float (*)(const MethodInfo*)> timeScale_;
    Il2CppClass* cameraClass_ = nullptr;

    Cfg cfg_;
    std::atomic<bool> cfgDirty_{true};
    std::atomic<bool> dead_{false};
    std::atomic<bool> resetPending_{true};
    int frameCb_ = 0, onChange_ = 0, exceptions_ = 0;
    float scanAcc_ = 0;
    std::vector<walls::Wall> walls_;  // intact, active walls (copied positions only)
    walls::Stats lastStats_;
    bool published_ = false, layoutWarned_ = false, firstShownLogged_ = false;
};

// Never deleted (process exit may run after the IL2CPP runtime is gone).
class WallMarkersModule final : public core::Module {
public:
    const char* Name() const override { return kModule; }
    bool DefaultEnabled() const override { return true; }
    bool Setup(core::ModuleContext& ctx) override { return impl_->Setup(ctx); }
    void OnEnable() override { impl_->OnEnable(); }
    void OnDisable() override { impl_->OnDisable(); }

private:
    WallMarkers* impl_ = new WallMarkers();
};

}  // namespace

std::unique_ptr<sr2::core::Module> CreateWallMarkers() { return std::make_unique<WallMarkersModule>(); }

}  // namespace sr2::modules
