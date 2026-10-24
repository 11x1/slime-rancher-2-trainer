// free_placement: lifts the gadget placement restrictions of gadget mode.
//
// The placement path (SR2 1.3.0, GadgetItem in MonomiPark.SlimeRancher.Player.PlayerItems,
// decompiled in IDA; details in NOTES.md):
//   * GadgetItem.Update -> UpdateFootprint() every frame while a footprint exists, and
//     PlaceGadgetEvent() calls UpdateFootprint() once more, then reads the fields it wrote:
//     _isFootprintVisible, IsLinkedGadgetInRange, _isGrounded (the PLAYER is grounded),
//     _isPlacementBlocked (Gadget.IsOverlapping) and _isPlacementValid (IsPlacementValid).
//     Any of them false -> error toast and no placement; all true -> spend the cost,
//     GadgetDirector.InstantiateGadget(prefab, RegionRegistry.CurrentSceneGroup, pos, rot).
//   * IsPlacementValid: ray length <= metadata.MaxValidPlacementDist, no stacking onto a
//     stackable gadget unless the held gadget CanStackOnStackableGadgets, sin(MaxValidPlacement
//     Slope) <= hit normal.y, and placeholder Gadget.IsCompletelyGrounded(...).
//   * The footprint itself only shows when the hit normal.y > sin(FootprintVisibleSlopeThreshold).
//   * Gadget mode can't be entered inside a DisableGadgetModeTrigger volume
//     (PlayerItemController.BlockGadgetMode stores it in _gadgetModeBlocker; ToggleItem refuses).
//
// What this module does (each part behind its own setting):
//   * ignore_slope: inside UpdateFootprint the metadata's two slope thresholds are set to 0
//     (anything up to a vertical wall) and restored right after the call; IsCompletelyGrounded
//     returns true while UpdateFootprint runs.
//   * allow_upside_down (under ignore_slope): the thresholds become -90 (any normal) and a hit
//     normal of exactly (0,-1,0) is nudged by one float step (RaycastHit.get_normal, scoped) so the
//     footprint's strict "normal.y > -1" passes. The game's own footprint rotation,
//     LookRotation(toward the player projected onto the surface, normal), already puts the
//     gadget's up along the normal, and the placed gadget takes the placeholder's rotation.
//   * Teleporter gadgets (GadgetTeleporterNode in the prefab) keep the vanilla slope limits.
//   * ignore_collision: Gadget.IsOverlapping(float, LayerMask, bool) returns false while
//     UpdateFootprint runs and the mask is the metadata's GadgetOverlapLayers (the placement
//     overlap test; the placeholder-vs-player hiding test uses other layers).
//   * ignore_range: MaxValidPlacementDist is raised to the footprint ray length during the call
//     and IsInLinkedGadgetRange returns true (linked pairs may be any distance apart).
//   * allow_airborne: after UpdateFootprint, _isGrounded is forced true and the footprint colour
//     is re-evaluated through SetGadgetPlacementValidity.
//   * ignore_zones: BlockGadgetMode is skipped and ToggleItem forgets a stored blocker, so
//     gadget mode works inside DisableGadgetModeTrigger volumes. The ranch-house check stays.
//   * rotation_step: the rotate keys turn the placeholder by fixed steps (press = one step,
//     hold = repeat) instead of the smooth vanilla turn.
// Kept on purpose: the stacking rule (stacked gadgets are linked to their base), the cost, the
// placement through the game's own InstantiateGadget (scene group from the RegionRegistry, so
// the gadget is saved like any other), the ranch-house block and the "near/far" footprint ray.

#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <format>
#include <memory>
#include <string>
#include <unordered_map>

#include "core/detour_guard.h"
#include "core/il2cpp_api.h"
#include "core/log.h"
#include "core/main_thread.h"
#include "core/module.h"
#include "core/settings.h"
#include "core/unity.h"

namespace sr2::modules {
namespace {

namespace settings = sr2::core::settings;
namespace unity = sr2::core::unity;
namespace main_thread = sr2::core::main_thread;
using sr2::il2cpp::api;
using Clock = std::chrono::steady_clock;

constexpr const char* kModule = "free_placement";
constexpr const char* kImage = "Assembly-CSharp";
constexpr const char* kItemsNs = "MonomiPark.SlimeRancher.Player.PlayerItems";
constexpr const char* kWorldNs = "MonomiPark.SlimeRancher.World";

constexpr const char* kIgnoreCollision = "ignore_collision";
constexpr const char* kIgnoreSlope = "ignore_slope";
constexpr const char* kAllowUpsideDown = "allow_upside_down";
constexpr const char* kIgnoreRange = "ignore_range";
constexpr const char* kAllowAirborne = "allow_airborne";
constexpr const char* kIgnoreZones = "ignore_zones";
constexpr const char* kRotationStep = "rotation_step";

constexpr auto kRepeatDelay = std::chrono::milliseconds(350);  // hold a rotate key: first repeat
constexpr auto kRepeatRate = std::chrono::milliseconds(120);   // then one step per this

struct Cfg {
    std::atomic<bool> collision{false}, slope{false}, upside{false}, range{false}, airborne{false}, zones{false};
    std::atomic<int> step{0};
};
Cfg g_cfg;

class FreePlacement final : public sr2::core::Module {
public:
    const char* Name() const override { return kModule; }
    bool DefaultEnabled() const override { return false; }
    bool Setup(sr2::core::ModuleContext& ctx) override;
    void OnEnable() override { enabled_.store(true); }
    void OnDisable() override { enabled_.store(false); }
    bool Enabled() const { return enabled_.load(std::memory_order_relaxed); }

    // Offsets (resolved by name in Setup).
    size_t offMeta = 0;          // GadgetItem._gadgetItemMetadata
    size_t offHeld = 0;          // GadgetItem._heldGadget
    size_t offPlaceholder = 0;   // GadgetItem._gadgetPlaceholderInstance (GameObject)
    size_t offRotCw = 0;         // GadgetItem._rotatingClockwise
    size_t offRotCcw = 0;        // GadgetItem._rotatingCounterClockwise
    size_t offFootVisible = 0;   // GadgetItem._isFootprintVisible
    size_t offGrounded = 0;      // GadgetItem._isGrounded
    size_t offValid = 0;         // GadgetItem._isPlacementValid
    size_t offBlocked = 0;       // GadgetItem._isPlacementBlocked
    size_t offLinked = 0;        // GadgetItem.<IsLinkedGadgetInRange>k__BackingField
    size_t offVisDist = 0;       // GadgetItemMetadata.PlacementFootprintVisibleDist
    size_t offVisSlope = 0;      // GadgetItemMetadata.FootprintVisibleSlopeThreshold
    size_t offMaxDist = 0;       // GadgetItemMetadata.MaxValidPlacementDist
    size_t offMaxSlope = 0;      // GadgetItemMetadata.MaxValidPlacementSlope
    size_t offOverlapLayers = 0; // GadgetItemMetadata.GadgetOverlapLayers (LayerMask = int)
    size_t offBlocker = 0;       // PlayerItemController._gadgetModeBlocker
    size_t offPrefab = 0;        // IdentifiableType.prefab (GadgetDefinition)
    Il2CppClass* itemClass = nullptr;
    Il2CppClass* metaClass = nullptr;
    Il2CppClass* controllerClass = nullptr;
    Il2CppClass* teleporterClass = nullptr;      // GadgetTeleporterNode
    const MethodInfo* compInChildren = nullptr;  // GameObject.GetComponentInChildren(Type, bool)

    using SetValidityFn = void (*)(Il2CppObject*, bool, const MethodInfo*);
    SetValidityFn setValidity = nullptr;
    const MethodInfo* setValidityMi = nullptr;
    using RotateFn = void (*)(Il2CppObject*, float, float, float, const MethodInfo*);
    RotateFn rotate = nullptr;  // Transform.Rotate(float, float, float), Space.Self
    const MethodInfo* rotateMi = nullptr;

    // UpdateFootprint state (main thread only).
    int depth = 0;                      // > 0 while the original UpdateFootprint runs
    int overlapMask = 0;                // metadata.GadgetOverlapLayers of the running call
    bool slopeNow = false;              // ignore_slope applies to the running call
    bool upsideNow = false;             // allow_upside_down applies to the running call
    std::unordered_map<const void*, bool> teleporters;  // GadgetDefinition (identity only) -> is a teleporter
    bool lastCw = false, lastCcw = false;
    Clock::time_point nextRepeat{};

private:
    std::atomic<bool> enabled_{false};
    int onChange_ = 0;
};

FreePlacement* g_self = nullptr;

bool IsA(Il2CppObject* obj, Il2CppClass* k) {
    const auto& a = api();
    if (!obj || !k || !a.il2cpp_object_get_class) return false;
    Il2CppClass* c = a.il2cpp_object_get_class(obj);
    if (c == k) return true;
    return c && a.il2cpp_class_is_assignable_from && a.il2cpp_class_is_assignable_from(k, c);
}

bool Active() { return g_self && g_self->Enabled(); }

void LoadCfg() {
    g_cfg.collision.store(settings::GetBool(kModule, kIgnoreCollision));
    g_cfg.slope.store(settings::GetBool(kModule, kIgnoreSlope));
    g_cfg.upside.store(settings::GetBool(kModule, kAllowUpsideDown));
    g_cfg.range.store(settings::GetBool(kModule, kIgnoreRange));
    g_cfg.airborne.store(settings::GetBool(kModule, kAllowAirborne));
    g_cfg.zones.store(settings::GetBool(kModule, kIgnoreZones));
    g_cfg.step.store(settings::GetInt(kModule, kRotationStep));
}

// Teleporter gadgets keep the vanilla slope limits: the arriving player gets the node's full
// rotation (TeleportablePlayer.TeleportTo -> IPlayerController.set_Rotation ->
// KinematicCharacterMotor.SetRotation), and KFCCharacterController.UpdateRotation keeps the
// motor's up axis, so a tilted or upside-down teleporter would leave the player tilted.
// One managed lookup per gadget definition, cached (main thread).
bool IsTeleporter(FreePlacement& s, Il2CppObject* def) {
    if (!def || !s.teleporterClass || !s.compInChildren) return false;
    auto it = s.teleporters.find(def);
    if (it != s.teleporters.end()) return it->second;
    bool tp = false;
    auto* prefab = unity::Field<Il2CppObject*>(def, s.offPrefab);
    if (prefab && unity::IsAlive(prefab)) {
        bool inactive = true;
        void* args[2] = {unity::TypeOf(s.teleporterClass), &inactive};
        std::string err;
        Il2CppObject* c = sr2::il2cpp::Resolver::Get().Invoke(s.compInChildren, prefab, args, &err);
        tp = c != nullptr && err.empty();
        if (!err.empty()) SR2_WARN("[{}] teleporter check failed for '{}': {}", kModule, unity::GetName(def), err);
    }
    s.teleporters.emplace(def, tp);
    if (tp)
        SR2_INFO("[{}] '{}' is a teleporter: it keeps the vanilla slope limits (no walls, no upside-down)", kModule,
                 unity::GetName(def));
    return tp;
}

// ---------------------------------------------------------------- hooks --

sr2::core::DetourGuard g_footGuard{kModule, "GadgetItem.UpdateFootprint"};
sr2::core::DetourGuard g_groundGuard{kModule, "Gadget.IsCompletelyGrounded"};
sr2::core::DetourGuard g_overlapGuard{kModule, "Gadget.IsOverlapping"};
sr2::core::DetourGuard g_linkGuard{kModule, "GadgetItem.IsInLinkedGadgetRange"};
sr2::core::DetourGuard g_blockGuard{kModule, "PlayerItemController.BlockGadgetMode"};
sr2::core::DetourGuard g_toggleGuard{kModule, "PlayerItemController.ToggleItem"};
sr2::core::DetourGuard g_normalGuard{kModule, "RaycastHit.get_normal"};

using FootFn = void (*)(Il2CppObject*, const MethodInfo*);
using GroundFn = bool (*)(Il2CppObject*, float, float, const MethodInfo*);
using OverlapFn = bool (*)(Il2CppObject*, float, int32_t, bool, const MethodInfo*);
using LinkFn = bool (*)(Il2CppObject*, void*, const MethodInfo*);  // Vector3 by reference
using BlockFn = void (*)(Il2CppObject*, Il2CppObject*, const MethodInfo*);
using ToggleFn = void (*)(Il2CppObject*, void*, const MethodInfo*);  // InputEventData (12 bytes) by reference
FootFn g_origFoot = nullptr;
GroundFn g_origGround = nullptr;
OverlapFn g_origOverlap = nullptr;
LinkFn g_origLink = nullptr;
BlockFn g_origBlock = nullptr;
ToggleFn g_origToggle = nullptr;
struct Vec3 {
    float x, y, z;
};
using NormalFn = Vec3* (*)(Vec3*, void*, const MethodInfo*);  // RaycastHit.get_normal (struct this by pointer)
NormalFn g_origNormal = nullptr;

// Metadata values swapped for the duration of one UpdateFootprint call. The destructor
// restores them, also when a managed exception unwinds through the detour.
struct MetaSwap {
    char* meta = nullptr;
    bool slope = false, dist = false;
    float visSlope = 0, maxSlope = 0, maxDist = 0;
    ~MetaSwap() {
        if (!meta || !g_self) return;
        if (slope) {
            unity::Field<float>(meta, g_self->offVisSlope) = visSlope;
            unity::Field<float>(meta, g_self->offMaxSlope) = maxSlope;
        }
        if (dist) unity::Field<float>(meta, g_self->offMaxDist) = maxDist;
    }
};

struct RotSave {
    Il2CppObject* item = nullptr;
    bool cw = false, ccw = false;
};

void FootDetour(Il2CppObject* self, const MethodInfo* mi) {
    if (!self || !Active() || !main_thread::IsMainThread()) return g_origFoot(self, mi);
    FreePlacement& s = *g_self;
    MetaSwap swap;
    RotSave rot;
    s.slopeNow = s.upsideNow = false;
    g_footGuard.Run([&] {
        if (!IsA(self, s.itemClass)) return;
        auto* meta = unity::Field<Il2CppObject*>(self, s.offMeta);
        if (!IsA(meta, s.metaClass)) return;
        char* m = reinterpret_cast<char*>(meta);
        s.overlapMask = unity::Field<int32_t>(m, s.offOverlapLayers);
        swap.meta = m;
        if (g_cfg.slope.load(std::memory_order_relaxed) &&
            !IsTeleporter(s, unity::Field<Il2CppObject*>(self, s.offHeld))) {
            s.slopeNow = true;
            s.upsideNow = g_cfg.upside.load(std::memory_order_relaxed);
            swap.visSlope = unity::Field<float>(m, s.offVisSlope);
            swap.maxSlope = unity::Field<float>(m, s.offMaxSlope);
            swap.slope = true;
            // 0: footprint for normal.y > 0, valid for normal.y >= 0 (up to a vertical wall).
            // -90: sin = -1, any normal; a normal of exactly (0,-1,0) is nudged in NormalDetour
            // so the footprint's strict "y > -1" test passes.
            const float t = s.upsideNow ? -90.0f : 0.0f;
            unity::Field<float>(m, s.offVisSlope) = t;
            unity::Field<float>(m, s.offMaxSlope) = t;
        }
        if (g_cfg.range.load(std::memory_order_relaxed)) {
            swap.maxDist = unity::Field<float>(m, s.offMaxDist);
            swap.dist = true;
            const float vis = unity::Field<float>(m, s.offVisDist);
            if (vis > swap.maxDist) unity::Field<float>(m, s.offMaxDist) = vis;
        }
        if (g_cfg.step.load(std::memory_order_relaxed) > 0) {
            // The original turns the placeholder by speed * dt while a flag is set; hide the
            // flags from it and turn by steps ourselves below.
            rot.item = self;
            rot.cw = unity::Field<bool>(self, s.offRotCw);
            rot.ccw = unity::Field<bool>(self, s.offRotCcw);
            unity::Field<bool>(self, s.offRotCw) = false;
            unity::Field<bool>(self, s.offRotCcw) = false;
        }
    });

    ++s.depth;
    struct DepthGuard {
        int& d;
        ~DepthGuard() { --d; }
    } dg{s.depth};
    g_origFoot(self, mi);

    if (rot.item) {
        unity::Field<bool>(self, s.offRotCw) = rot.cw;
        unity::Field<bool>(self, s.offRotCcw) = rot.ccw;
    }
    if (!swap.meta) return;

    g_footGuard.Run([&] {
        // Player in the air (jetpack / jump): the original refuses; re-evaluate without it.
        if (g_cfg.airborne.load(std::memory_order_relaxed) && unity::Field<void*>(self, s.offHeld) &&
            unity::Field<bool>(self, s.offFootVisible) && !unity::Field<bool>(self, s.offGrounded)) {
            unity::Field<bool>(self, s.offGrounded) = true;
            const bool ok = !unity::Field<bool>(self, s.offBlocked) && unity::Field<bool>(self, s.offValid) &&
                            unity::Field<bool>(self, s.offLinked);
            if (ok && s.setValidity) s.setValidity(self, true, s.setValidityMi);
        }

        if (!rot.item) return;
        const int step = g_cfg.step.load(std::memory_order_relaxed);
        const int dir = (rot.cw && !rot.ccw) ? 1 : (rot.ccw && !rot.cw) ? -1 : 0;
        const bool pressed = (rot.cw && !s.lastCw) || (rot.ccw && !s.lastCcw);
        s.lastCw = rot.cw;
        s.lastCcw = rot.ccw;
        if (dir == 0 || step <= 0) return;
        const auto now = Clock::now();
        if (pressed) {
            s.nextRepeat = now + kRepeatDelay;
        } else if (now >= s.nextRepeat) {
            s.nextRepeat = now + kRepeatRate;
        } else {
            return;
        }
        auto* go = unity::Field<Il2CppObject*>(self, s.offPlaceholder);
        if (!go || !unity::IsAlive(go)) return;
        Il2CppObject* tr = unity::TransformOf(go);
        if (!tr || !s.rotate) return;
        s.rotate(tr, 0.0f, static_cast<float>(dir * step), 0.0f, s.rotateMi);
    });
}

bool GroundDetour(Il2CppObject* self, float adj, float dist, const MethodInfo* mi) {
    bool r = false;
    if (g_self && g_self->depth > 0 && g_self->slopeNow && Active() && g_groundGuard.Run([&] { r = true; }))
        return r;
    return g_origGround(self, adj, dist, mi);
}

bool OverlapDetour(Il2CppObject* self, float floorTol, int32_t mask, bool skipIgnored, const MethodInfo* mi) {
    bool skip = false;
    if (g_self && g_self->depth > 0 && Active() && g_cfg.collision.load(std::memory_order_relaxed))
        g_overlapGuard.Run([&] { skip = mask == g_self->overlapMask; });
    if (skip) return false;
    return g_origOverlap(self, floorTol, mask, skipIgnored, mi);
}

// Shared code: RaycastHit.get_normal is folded with ParticleCollisionEvent.get_normal and is
// called all over the game; outside UpdateFootprint with allow_upside_down it only passes through.
Vec3* NormalDetour(Vec3* ret, void* hit, const MethodInfo* mi) {
    Vec3* r = g_origNormal(ret, hit, mi);
    if (r && g_self && g_self->depth > 0 && g_self->upsideNow && r->y <= -1.0f && main_thread::IsMainThread())
        g_normalGuard.Run([&] { r->y = -0.99999994f; });  // the float next to -1 (same direction)
    return r;
}

bool LinkDetour(Il2CppObject* self, void* pos, const MethodInfo* mi) {
    bool r = false;
    if (Active() && g_cfg.range.load(std::memory_order_relaxed) && g_linkGuard.Run([&] { r = true; })) return r;
    return g_origLink(self, pos, mi);
}

void BlockDetour(Il2CppObject* self, Il2CppObject* blocker, const MethodInfo* mi) {
    bool skip = false;
    if (Active() && g_cfg.zones.load(std::memory_order_relaxed))
        g_blockGuard.Run([&] {
            skip = true;
            SR2_INFO("[{}] gadget-mode blocker '{}' ignored", kModule, blocker ? unity::GetName(blocker) : "");
        });
    if (skip) return;
    g_origBlock(self, blocker, mi);
}

void ToggleDetour(Il2CppObject* self, void* data, const MethodInfo* mi) {
    if (self && Active() && g_cfg.zones.load(std::memory_order_relaxed))
        g_toggleGuard.Run([&] {
            if (!IsA(self, g_self->controllerClass)) return;
            // A blocker stored before the setting was turned on (entered its volume earlier).
            // Storing null needs no GC write barrier; leaving the volume clears it anyway.
            auto& blocker = unity::Field<Il2CppObject*>(self, g_self->offBlocker);
            if (blocker) {
                SR2_INFO("[{}] stored gadget-mode blocker cleared", kModule);
                blocker = nullptr;
            }
        });
    g_origToggle(self, data, mi);
}

// ---------------------------------------------------------------- setup --

bool FreePlacement::Setup(sr2::core::ModuleContext& ctx) {
    auto& R = ctx.resolver();
    std::string why;
    auto cls = [&](const char* ns, const char* name) -> Il2CppClass* {
        std::string err;
        Il2CppClass* k = R.Class(kImage, ns, name, &err);
        if (!k && why.empty()) why = err.empty() ? std::format("class {}.{} not found", ns, name) : err;
        return k;
    };
    auto off = [&](Il2CppClass* k, const char* field, size_t& out) -> bool {
        if (!k) return false;
        std::string err;
        auto o = R.FieldOffset(k, field, &err);
        if (!o) {
            if (why.empty()) why = err.empty() ? std::format("field {} not found", field) : err;
            return false;
        }
        out = *o;
        return true;
    };
    auto ptr = [&](const MethodInfo* m, const char* what) -> void* {
        if (!m) {
            if (why.empty()) why = std::format("{} not found", what);
            return nullptr;
        }
        std::string err;
        void* p = R.MethodPointer(m, &err);
        if (!p && why.empty()) why = std::format("{} has no code: {}", what, err);
        return p;
    };

    itemClass = cls(kItemsNs, "GadgetItem");
    metaClass = cls(kItemsNs, "GadgetItemMetadata");
    controllerClass = cls(kItemsNs, "PlayerItemController");
    Il2CppClass* gadget = cls(kWorldNs, "Gadget");
    Il2CppClass* identType = cls("", "IdentifiableType");
    bool ok = off(itemClass, "_gadgetItemMetadata", offMeta) && off(itemClass, "_heldGadget", offHeld) &&
              off(itemClass, "_gadgetPlaceholderInstance", offPlaceholder) &&
              off(itemClass, "_rotatingClockwise", offRotCw) && off(itemClass, "_rotatingCounterClockwise", offRotCcw) &&
              off(itemClass, "_isFootprintVisible", offFootVisible) && off(itemClass, "_isGrounded", offGrounded) &&
              off(itemClass, "_isPlacementValid", offValid) && off(itemClass, "_isPlacementBlocked", offBlocked) &&
              off(itemClass, "<IsLinkedGadgetInRange>k__BackingField", offLinked) &&
              off(metaClass, "PlacementFootprintVisibleDist", offVisDist) &&
              off(metaClass, "FootprintVisibleSlopeThreshold", offVisSlope) &&
              off(metaClass, "MaxValidPlacementDist", offMaxDist) &&
              off(metaClass, "MaxValidPlacementSlope", offMaxSlope) &&
              off(metaClass, "GadgetOverlapLayers", offOverlapLayers) &&
              off(controllerClass, "_gadgetModeBlocker", offBlocker) && off(identType, "prefab", offPrefab) &&
              gadget;
    if (!ok) return ctx.Fail("resolve failed: " + why);

    std::string err;
    void* foot = ptr(R.Method(itemClass, "UpdateFootprint", 0, &err), "GadgetItem.UpdateFootprint");
    void* link = ptr(R.Method(itemClass, "IsInLinkedGadgetRange", 1, &err), "GadgetItem.IsInLinkedGadgetRange");
    setValidityMi = R.Method(itemClass, "SetGadgetPlacementValidity", 1, &err);
    setValidity = reinterpret_cast<SetValidityFn>(ptr(setValidityMi, "GadgetItem.SetGadgetPlacementValidity"));
    void* ground = ptr(R.Method(gadget, "IsCompletelyGrounded", 2, &err), "Gadget.IsCompletelyGrounded");
    void* overlap = ptr(R.MethodByParams(gadget, "IsOverlapping",
                                         {"System.Single", "UnityEngine.LayerMask", "System.Boolean"}, &err),
                        "Gadget.IsOverlapping(float, LayerMask, bool)");
    void* block = ptr(R.Method(controllerClass, "BlockGadgetMode", 1, &err), "PlayerItemController.BlockGadgetMode");
    void* toggle = ptr(R.Method(controllerClass, "ToggleItem", 1, &err), "PlayerItemController.ToggleItem");
    rotateMi = R.MethodByParams(unity::TransformClass(), "Rotate", {"System.Single", "System.Single", "System.Single"},
                                &err);
    rotate = reinterpret_cast<RotateFn>(ptr(rotateMi, "Transform.Rotate(float, float, float)"));
    Il2CppClass* hitClass = R.Class("UnityEngine.PhysicsModule", "UnityEngine", "RaycastHit", &err);
    void* normal = ptr(hitClass ? R.Method(hitClass, "get_normal", 0, &err) : nullptr, "RaycastHit.get_normal");
    if (!foot || !link || !setValidity || !ground || !overlap || !block || !toggle || !rotate || !normal)
        return ctx.Fail("resolve failed: " + why);
    // Teleporter exclusion (optional: without it teleporters follow the slope settings, logged).
    teleporterClass = R.Class(kImage, "MonomiPark.SlimeRancher.World.Teleportation", "GadgetTeleporterNode", &err);
    compInChildren = R.MethodByParams(unity::GameObjectClass(), "GetComponentInChildren",
                                      {"System.Type", "System.Boolean"}, &err);
    if (!teleporterClass || !compInChildren)
        SR2_WARN("[{}] teleporter exclusion unavailable (GadgetTeleporterNode / GetComponentInChildren not found)",
                 kModule);

    auto D = [](auto f) { return reinterpret_cast<void*>(f); };
    auto O = [](auto* p) { return reinterpret_cast<void**>(p); };
    if (!ctx.HookAddress("GadgetItem.UpdateFootprint", foot, D(&FootDetour), O(&g_origFoot)) ||
        !ctx.HookAddress("GadgetItem.IsInLinkedGadgetRange", link, D(&LinkDetour), O(&g_origLink)) ||
        !ctx.HookAddress("Gadget.IsCompletelyGrounded", ground, D(&GroundDetour), O(&g_origGround)) ||
        !ctx.HookAddress("Gadget.IsOverlapping", overlap, D(&OverlapDetour), O(&g_origOverlap)) ||
        !ctx.HookAddress("PlayerItemController.BlockGadgetMode", block, D(&BlockDetour), O(&g_origBlock)) ||
        !ctx.HookAddress("PlayerItemController.ToggleItem", toggle, D(&ToggleDetour), O(&g_origToggle)) ||
        !ctx.HookAddress("RaycastHit.get_normal", normal, D(&NormalDetour), O(&g_origNormal)))
        return false;

    auto reg = [](const char* key, const char* label, const char* tip, bool def) {
        settings::Def d;
        d.module = kModule;
        d.key = key;
        d.label = label;
        d.kind = settings::Kind::Bool;
        d.def = def ? 1.0 : 0.0;
        d.min = 0;
        d.max = 1;
        d.step = 1;
        d.tooltip = tip;
        settings::Register(d);
    };
    reg(kIgnoreCollision, "Ignore collision",
        "Gadgets may overlap other gadgets, walls and props. Placing a gadget into the player can push you away.",
        true);
    reg(kIgnoreSlope, "Ignore slope",
        "Place on any slope up to a vertical wall, and over edges.",
        true);
    reg(kAllowUpsideDown, "Ignore slope: upside-down",
        "Also place on ceilings and overhangs. Teleporters keep the normal limits.",
        true);
    settings::SetParent(kModule, kAllowUpsideDown, kIgnoreSlope);
    reg(kIgnoreRange, "Ignore range limits",
        "Place as far as the outline reaches, and linked pairs at any distance.",
        true);
    reg(kAllowAirborne, "Place while airborne", "Place gadgets while jumping or flying with the jetpack.", true);
    reg(kIgnoreZones, "Ignore no-gadget zones",
        "Gadget mode also works where the game turns it off, except in the ranch house. Those areas were not "
        "made for gadgets.",
        false);
    {
        settings::Def d;
        d.module = kModule;
        d.key = kRotationStep;
        d.label = "Rotation step";
        d.kind = settings::Kind::Int;
        d.def = 0;
        d.min = 0;
        d.max = 90;
        d.step = 1;
        d.tooltip = "The rotate keys turn the gadget by this many degrees per press (hold to repeat). "
                    "0 = the game's smooth turn.";
        d.unit = "\xC2\xB0";  // degree sign
        d.zeroText = "smooth";
        settings::Register(d);
    }
    LoadCfg();
    if (!onChange_) onChange_ = settings::OnChange(kModule, [](const std::string&) { LoadCfg(); });

    g_self = this;
    SR2_INFO("[{}] setup ok: UpdateFootprint RVA 0x{:X}, IsOverlapping RVA 0x{:X}, IsCompletelyGrounded RVA 0x{:X}, "
             "BlockGadgetMode RVA 0x{:X}, RaycastHit.get_normal RVA 0x{:X}; collision {}, slope {}, upside-down {}, "
             "range {}, airborne {}, zones {}, step {}",
             kModule, sr2::il2cpp::Resolver::Rva(foot), sr2::il2cpp::Resolver::Rva(overlap),
             sr2::il2cpp::Resolver::Rva(ground), sr2::il2cpp::Resolver::Rva(block), sr2::il2cpp::Resolver::Rva(normal),
             g_cfg.collision.load(), g_cfg.slope.load(), g_cfg.upside.load(), g_cfg.range.load(), g_cfg.airborne.load(), g_cfg.zones.load(), g_cfg.step.load());
    return true;
}

}  // namespace

std::unique_ptr<sr2::core::Module> CreateFreePlacement() { return std::make_unique<FreePlacement>(); }

}  // namespace sr2::modules
