#pragma once
// debug_tools: entity hitbox view (round 11: "a debug option to show entity hitboxes"). Wireframes of the
// colliders of entities near the camera (anything with an Identifiable in its parents: slimes, toys, food,
// plorts, gadgets, the mod's buildables), drawn by the ImGui overlay through ui/hud.h line segments.
//
//   * Every kRefreshSeconds: Physics.OverlapSphere(camera, range, all layers, triggers per option) - a physics
//     query, no scene search; each enabled collider whose GetComponentInParent<Identifiable> is set and whose
//     bounds do not hold the camera (the player) is kept (GC handle, at most kMaxColliders, nearest first) with
//     its shape read once (box centre / size, sphere centre / radius, capsule centre / radius / height /
//     direction; anything else: its world AABB).
//   * Every frame: Transform.localToWorldMatrix per kept collider (bounds for the AABB ones), the lines in world
//     space, Camera.main's projection * worldToCamera, hud::Publish. Hidden while the settings window is open.
//
// Colours: solid colliders green, triggers yellow, AABB-only shapes (mesh colliders etc.) blue.
// Main thread only. All game calls are guarded Resolver invokes; a native fault marks the module faulted.

#include <string>

namespace sr2::core {
class ModuleContext;
}

namespace sr2::modules::debug_tools::hitboxes {

// Worker thread (module Setup). false = unavailable (`why`); the rest of debug_tools keeps working.
bool Setup(sr2::core::ModuleContext& ctx, std::string& why);
// Main thread, every frame. `want` = option on and module on.
void Tick(float dt, bool want, float range, bool triggers);

}  // namespace sr2::modules::debug_tools::hitboxes
