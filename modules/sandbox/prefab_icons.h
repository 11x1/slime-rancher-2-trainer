#pragma once
// sandbox: icons rendered from an item's prefab, for spawn-list items the game gives no icon
// (crates, giant eggs, the vine bulb, the Mysterious Statue, ...). SR2 1.3.0, all lookups by name.
//
// No prefab is instantiated (its scripts would run: actor registration, physics, ...). Only its
// meshes are copied: for every enabled MeshRenderer / SkinnedMeshRenderer in the prefab whose
// GameObject is active (LOD1+ children skipped), a new GameObject with a MeshFilter (the shared
// mesh) and a MeshRenderer (the shared materials) at the same place relative to the prefab root,
// on layer 31, 10 km above the world origin. A disabled orthographic Camera (culling only layer
// 31, solid magenta clear, no HDR / MSAA) frames their bounds from the front right and above, a
// directional Light (culling only layer 31) lights them, and Camera.Render draws them into a
// 128 x 128 RenderTexture. Texture2D.ReadPixels copies the result to the CPU, where the background
// is made transparent (the clear alpha if the pipeline kept it, else the magenta key), then
// Apply, Sprite.Create. Every created object is destroyed with DestroyImmediate in the same call,
// so nothing is left in the scene for the game's own cameras (or the save) to see.
//
// Main thread only; every game call through Resolver::Invoke (managed exceptions caught, a
// native fault marks the module faulted). Cost: one Render plus one GPU readback per icon.

#include <string>

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

private:
    struct Impl;
    Impl* impl_ = nullptr;
    bool ready_ = false;
};

}  // namespace sr2::modules::sandbox
