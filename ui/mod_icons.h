#pragma once
// The mod's own icons (assets/icons/*.png, embedded in the DLL as ui/mod_icons_data.inc)
// as UnityEngine.Sprite objects, for game UI that shows a sprite for a mod feature
// (custom plot upgrades, the Options "Mods" tab).
//
// Get() decodes the PNG natively (stb_image), creates a Texture2D (RGBA32, sRGB, mip chain,
// trilinear, mip bias -1 like the game's own full art, clamp, outside the texture-quality
// mip limit) and a Sprite.Create over it, the first time an icon is asked for. The texture
// and sprite are flagged DontUnloadUnusedAsset and held by never-freed GC handles, so they
// live for the session; the result is cached per icon.
//
// Main thread only. Returns nullptr when the icon cannot be made (unknown name, not on
// the main thread, a lookup missing, Unity threw): the caller keeps its fallback sprite.
// Each failure is logged once; after a native fault no further icon is created.

#include <string_view>

#include "core/il2cpp_api.h"

namespace sr2::ui::mod_icons {

// `name` = file name without ".png" (e.g. "hen_collector_source", "mods_tab").
// `sizeLike`: optional game sprite the icon replaces. The new sprite's pixelsPerUnit is
// scaled so its native size (rect / pixelsPerUnit) equals that sprite's longer side,
// which keeps layouts that use native sizes unchanged. Only the first call per icon
// decides this (the sprite is cached).
// greyed: a desaturated, lighter copy (for greyed-out / owned menu entries), cached
// separately from the normal icon.
Il2CppObject* Get(std::string_view name, Il2CppObject* sizeLike = nullptr, bool greyed = false);

// Is an icon of that name embedded? (No Unity calls, any thread.) For optional icons that
// may or may not have been dropped into assets/icons at build time; Get() logs a warning
// for an unknown name, this does not.
bool Has(std::string_view name);

}  // namespace sr2::ui::mod_icons
