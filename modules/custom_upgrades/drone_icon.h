#pragma once
// custom_upgrades: the icon of the drone source "Hen Collector": the game's Plort Collector source
// icon (iconPatchCorralCollector, the collector with its round output opening) with the Hen icon
// (iconBirdHen) drawn over the opening. Built once at runtime as a new Texture2D + Sprite.
//
// The game's textures are not readable (m_IsReadable 0), so each sprite's texture is drawn into a
// temporary RenderTexture (Graphics.Blit) and its sprite rect copied back with Texture2D.ReadPixels.
// The hen's opaque box is scaled (bilinear, premultiplied alpha) into the opening and composited
// alpha-over. Both icons are full-colour art (no tint). Placement from the asset (see drone_icon.cpp).
//
// Main thread only; every game call through Resolver::Invoke.

#include <string>

struct Il2CppObject;

namespace sr2::il2cpp {
class Resolver;
}

namespace sr2::modules::custom_upgrades::drones {

class HenSourceIcon {
public:
    // Worker thread. false = unavailable (`why`): the plain Hen icon is used.
    bool Setup(sr2::il2cpp::Resolver& R, const char* module, std::string& why);
    bool Ready() const { return ready_; }
    // Main thread. A new Sprite (kept: DontUnloadUnusedAsset + a GC handle, never destroyed), or
    // nullptr and `why`.
    Il2CppObject* Compose(Il2CppObject* baseSprite, Il2CppObject* henSprite, std::string& why);

private:
    struct Impl;
    Impl* impl_ = nullptr;
    bool ready_ = false;
};

}  // namespace sr2::modules::custom_upgrades::drones
