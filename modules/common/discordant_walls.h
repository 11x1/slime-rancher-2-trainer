#pragma once
// Nullifier walls of the Grey Labyrinth, shared by the minimap (icons) and
// wall_markers (screen-space markers).
//
// Game 1.3.0 (dump + IDA, see modules/wall_markers/NOTES.md):
//   * The "Nullifier" gadget (localisation key m.gadget.name.harmonizer) is the
//     class MonomiPark.SlimeRancher.Labyrinth.Harmonizer.
//   * The walls it opens are MonomiPark.SlimeRancher.Labyrinth.DiscordantWall
//     (MonoBehaviour, IDisruptable). They are not destroyed: a placed Nullifier
//     within 1 m of the wall's _harmonizerDetectionCollider "harmonizes" it
//     (Harmonize(): _targetHarmonizationAmount = 1, the solid
//     _disharmonizedCollisionObject is switched off); picking the gadget up
//     again disharmonizes it (target 0). No per-wall save data: the state is
//     derived from the placed gadgets (which the save keeps).
//   * Registry: static List<DiscordantWall> _activeWalls, Add in Awake, Remove
//     in OnDestroy. Read here instead of hooking or scanning the scene.
//   * Inactive walls (isActiveAndEnabled false) only queue harmonizers and
//     evaluate them in OnEnable, so their state can be stale: they are skipped.
//
// Collect() runs on the Unity main thread only and keeps no managed pointer
// after it returns (positions and flags are copied out). It calls into Unity
// directly (no internal SEH net): the caller runs it inside its own native
// fault guard and disables itself on a fault.

#include <string>
#include <vector>

namespace sr2::modules::walls {

struct Wall {
    float x = 0, y = 0, z = 0;   // world position (collider bounds centre, else the transform)
    bool intact = true;          // not harmonized (_targetHarmonizationAmount < 0.5)
    bool active = true;          // isActiveAndEnabled (false: its region is unloaded / proxied, state may be stale)
};

struct Stats {
    int listed = 0;      // entries in DiscordantWall._activeWalls
    int valid = 0;       // alive DiscordantWall objects
    int active = 0;      // isActiveAndEnabled
    int intact = 0;      // active and not harmonized
    int fromBounds = 0;  // positions taken from the detection collider's bounds
    int inactive = 0;    // includeInactive: inactive walls returned (positions from the cache / transform)
    int fromCache = 0;   // ... of those, positions remembered from when the wall was active
};

// Worker thread (Setup). Idempotent and thread-safe; false + reason if the
// game has no such class / fields (feature unavailable, nothing else breaks).
bool Resolve(std::string& err);
bool Resolved();

// Main thread. Appends every ACTIVE wall (intact and harmonized) to `out`.
// Returns false only if the registry is unusable (layout mismatch).
//
// includeInactive: also the registered walls that are inactive. The Labyrinth's
// walls all wake up (register) when its scene group loads, then every region
// away from the player is proxied (its Root deactivated), so most walls are
// registered but inactive. Their harmonized flag is the last one they had
// (good enough for the full map); their position is the collider-bounds centre
// remembered from when they were last active (a disabled collider has empty
// bounds), else the transform position.
bool Collect(std::vector<Wall>& out, Stats* stats = nullptr, bool includeInactive = false);

}  // namespace sr2::modules::walls
