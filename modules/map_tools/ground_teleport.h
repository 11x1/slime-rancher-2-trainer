#pragma once
// Teleport onto the ground straight below a world (x, z) in the player's current scene group, shared by the
// full map's right-click teleport (map_tools) and the Sandbox page's "Teleport to waypoint". SR2 1.3.0. See
// modules/map_tools/NOTES.md, "Finding the ground".
//
// Why it is not a single raycast: the world is streamed by regions (RegionLoader / Region). Every region away
// from the player is "proxied": its Root (terrain, rocks, their colliders) is deactivated and only a collider-less
// proxy mesh is drawn. A Physics.Raycast over a far target therefore finds nothing (or something below the real
// ground). So:
//   1. Target near the player (kNearDist): one raycast, teleport at once.
//   2. Otherwise the player is parked a few metres beside the target column at the current height
//      (TeleportablePlayer.TeleportTo, which also runs RegionLoader.InitializeRegionProxies, so the regions
//      around the target load), held there (re-parked every 0.5 s), and the ground is raycast every frame until a
//      hit is stable for two checks; then the player lands 1 m above it. Nothing after kWaitMax s: back to where
//      the player was, refused, every raycast parameter logged.
// Ground ray: from y = kRayTop straight down kRayLength m, every layer but Ignore Raycast and the player's own,
// triggers ignored; a hit on the player's own collider (a child of the player) is skipped by casting again from
// just below it.

#include <functional>
#include <string>

namespace sr2::modules::ground_teleport {

struct Vec3f {
    float x = 0, y = 0, z = 0;
};

struct Result {
    bool ok = false;
    Vec3f from, to;       // to: the landing point (ok)
    float seconds = 0;    // Start -> landing / refusal
    bool parked = false;  // the target area had to be loaded first
    std::string why;      // refusal reason, short (for a pop-up / status line)
};
using Done = std::function<void(const Result&)>;

// Any thread (module Setup). Idempotent; false + reason when a required symbol is missing.
bool Resolve(std::string& err);
bool Resolved();

// Main thread. Starts a teleport for `owner` (a module key: logs, and the module marked faulted on a native
// fault). `done` runs on the main thread exactly once when this returns true, possibly before Start returns.
// False (+ why) when it cannot start: not resolved, another teleport still running, no save loaded.
bool Start(const char* owner, float x, float z, Done done, std::string* why);
bool Busy();

}  // namespace sr2::modules::ground_teleport
