#pragma once
// sandbox: icons rendered from an item's prefab, for spawn-list items the game gives no icon
// (crates, giant eggs, the vine bulb, the Mysterious Statue, ...). SR2 1.3.0, all lookups by name.
//
// No prefab is instantiated (its scripts would run: actor registration, physics, ...). Only its
// meshes are copied: for every enabled MeshRenderer / SkinnedMeshRenderer in the prefab whose
// GameObject is active (LOD1+ children skipped), a new GameObject with a MeshFilter (the shared
// mesh) and a MeshRenderer (the shared materials) at the same place relative to the prefab root,
// on layer 31, 10 km above the world origin. A disabled orthographic Camera (culling only layer
// 31, solid magenta clear, no HDR / MSAA) frames their bounds from the front right and above, three
// directional Lights (key, fill, rim) light them, and Camera.Render draws them into a 128 x 128 sRGB
// RenderTexture; while the drawn pixels are too dark the lights are made stronger and it renders again
// (at most 3 renders), then levels / a gamma lift on the CPU (round 9: "rendered icons are very dim"). Texture2D.ReadPixels copies the result to the CPU, where the background
// is made transparent (the clear alpha if the pipeline kept it, else the magenta key), then
// Apply, Sprite.Create. Every created object is destroyed with DestroyImmediate in the same call,
// so nothing is left in the scene for the game's own cameras (or the save) to see.
//
// Composite looks (RenderPieces): several prefabs, or one child of a prefab, each placed like the
// mod's runtime dressing places its clones (position, yaw, lean-back tilt, scale), parts left out by
// GameObject name, and an optional colour shift of the finished image. The Sandbox uses it for the
// mod buildables' icons (the DJ Booth with its mixer, speakers and pillow, the tilted DJ Speaker, the
// purple Quantum Storage Link, the Slime Radio).
//
// Main thread only; every game call through Resolver::Invoke (managed exceptions caught, a
// native fault marks the module faulted). Cost: one Render plus one GPU readback per icon.

#include <cstdint>
#include <string>
#include <vector>

struct Il2CppObject;

namespace sr2::il2cpp {
class Resolver;
}

namespace sr2::modules::sandbox {

class PrefabIcons {
public:
    PrefabIcons();
    ~PrefabIcons();
    PrefabIcons(const PrefabIcons&) = delete;
    PrefabIcons& operator=(const PrefabIcons&) = delete;

    // Worker thread: resolve by name. false = unavailable (`why`): items keep their name tiles.
    bool Setup(sr2::il2cpp::Resolver& R, const char* module, std::string& why);
    bool Ready() const { return ready_; }

    // Main thread: a Sprite showing `prefab` (a GameObject asset), kept from unloading
    // (HideFlags.DontUnloadUnusedAsset). nullptr and `why` on failure.
    Il2CppObject* Render(Il2CppObject* prefab, std::string& why);

    // One prefab (or one child of it) in a composite look. Local units of the composition, which faces +Z.
    struct Piece {
        Il2CppObject* prefab = nullptr;  // GameObject asset
        const char* sub = nullptr;       // only this child (Transform.Find path, e.g. "model_boombox"); nullptr: all
        bool keepPlace = false;          // sub: starts at its place in the prefab (else at the origin, unrotated)
        std::vector<std::string> skip;   // GameObjects (exact names) left out with everything under them
        float pos[3] = {0, 0, 0};        // added to the start position
        float yaw = 0;                   // degrees around +Y
        float tilt = 0;                  // degrees leaning back (the front, +Z, turns upwards)
        float scale = 1;
    };
    // Main thread: like Render, for several pieces in one image. `tint` (r, g, b in 0..1, or nullptr)
    // shifts the finished image's colours towards it, keeping the shading. `parts` (optional) gets the
    // number of meshes drawn.
    Il2CppObject* RenderPieces(const std::vector<Piece>& pieces, const float* tint, std::string& why, int* parts = nullptr);
    // How the last successful render was lit and levelled (renders, light gain, levels), for log lines.
    std::string LastLook() const;

private:
    struct Impl;
    Impl* impl_ = nullptr;
    bool ready_ = false;
};

// Main thread: icons rendered for the mod's buildables (custom_upgrades/buildables.h ids), shared by the
// crafter / spawner (module.cpp) and the Upgrades & Blueprints card (upgrades.cpp). 0 = none (yet).
uint32_t ModIcon(const std::string& id);
void SetModIcon(const std::string& id, uint32_t icon);

}  // namespace sr2::modules::sandbox
