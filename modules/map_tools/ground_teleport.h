#pragma once
// Teleport onto the ground straight below a world (x, z) in the player's current scene group, shared by the
// full map's right-click teleport (map_tools) and the Sandbox page's "Teleport to waypoint". SR2 1.3.0. See
// modules/map_tools/NOTES.md, "Finding the ground".
//
// Why it is not a single raycast: the world is streamed by regions (RegionLoader / Region). Every region away
// from the player is "proxied": its Root (terrain, rocks, their colliders) is deactivated and only a collider-less
// proxy mesh is drawn. A Physics.Raycast over a far target therefore finds nothing (or something below the real
// ground). So the regions whose bounds hold the target column are loaded on the spot, without moving the player:
// Region.AddNonProxiedReference (the game's own reference count, the one RegionLoader uses; the first reference
// activates the Root). The player stays where he is while they load. When the floor found below the target is the
// same for two checks, the player is teleported there (TeleportablePlayer.TeleportTo, which also lets the
// RegionLoader take its own references), and our references are released.
//
// The landing spot (Pick): every upward-facing surface below the column (rays from y = kRayTop down, layers the
// player collides with, triggers ignored), top to bottom. A surface counts when it is walkable (normal), has
// headroom, is not inside a hazard trigger (KillOnTrigger, KillOnTriggerCloudOcean, LavaVolume,
// DamagePlayerOnEnterTrigger: lava, the slime sea, the fall-through kill volumes) and is not sealed in. The top
// one wins unless it is a thin slab (a roof) with a valid floor below it: then the floor (the Grey Labyrinth's
// roofs). Nothing valid: refused.
//
// Never while the player is knocked out (PlayerDeathHandler._isPlayerDead) or a scene load runs; if that
// happens mid-way, the request is dropped without moving the player.

#include <functional>
#include <string>

namespace sr2::modules::ground_teleport {

struct Vec3f {
    float x = 0, y = 0, z = 0;
};

struct Result {
    bool ok = false;
    Vec3f from, to;           // to: the landing point (ok)
    float seconds = 0;        // Start -> landing / refusal
    bool loadedFirst = false; // the target area had to be loaded first
    std::string why;          // refusal reason, short (for a pop-up / status line)
};
using Done = std::function<void(const Result&)>;

// Any thread (module Setup). Idempotent; false + reason when a required symbol is missing.
bool Resolve(std::string& err);
bool Resolved();

// Main thread. Starts a teleport for `owner` (a module key: logs, and the module marked faulted on a native
// fault). `done` runs on the main thread exactly once when this returns true, possibly before Start returns.
// False (+ why) when it cannot start: not resolved, another teleport still running, no save loaded, the player
// knocked out or a scene load running.
bool Start(const char* owner, float x, float z, Done done, std::string* why);
bool Busy();

// Main thread. True (+ a short reason) while no teleport may run: the player is knocked out / respawning
// (`knockedOut` true), or a scene load is in progress. False also when this cannot be told (not resolved).
bool PlayerUnavailable(std::string* why, bool* knockedOut = nullptr);

}  // namespace sr2::modules::ground_teleport
