# Overlay logo from the Slimepedia (planned)

Status: not implemented. Requested 2026-10-23 on branch `feature/overlay-sr2-theme`.

## What is wanted

The big slime at the top of the overlay sidebar (`ui/slime_icons.cpp`, drawn by `ui/menu.cpp`):

1. **Source:** the slime icons the Slimepedia itself shows, not every `SlimeDefinition.icon`.
2. **Only discovered slimes:** pick only from the Slimepedia slime entries the player has unlocked in the loaded save. Nothing discovered, or no save loaded (title screen): keep today's fallback, the pink slime drawn by `DrawSlimeFallback` in `ui/menu.cpp`.
3. **A different one every open:** as today (`slime_icons::OnOpen`, never the same twice in a row).
4. **Hover tooltip with the entry:** the slime's name and its Slimepedia details, with the entry's sections (each section's title and text, as the Slimepedia shows them).

## What exists today

- `slime_icons.cpp` collects every `SlimeDefinition` (largos skipped) from `Resources.FindObjectsOfTypeAll(IdentifiableType)` on the main thread. It reads `icon` (Sprite), `Sprite.textureRect` and `Texture.GetNativeTexturePtr`, and on the render thread draws it through a D3D11 SRV, with a gamma shader for sRGB textures. Undiscovered slimes are included and the tooltip shows only the name.
- `ui/menu.cpp` `Tooltip()` draws one wrapped text block on the floating surface (`FloatingSurface`, `SoftShadow`). A multi-section tooltip needs a richer variant: heading in `theme::Display()`, body in `theme::Body()` at `kSmallSize`, a section title per block, max width about 360 px at 1080p.
- `ui/NOTES.md`, "Slime logo", documents the current behaviour and the unverified UV orientation.

## Known from the RE notes

- `re/custom-upgrades.md` §2b.1: list items carry a `PediaEntry` (`item.PediaEntry != null` shows the Info button), and `ListUIRoot.OnAltAction(item)` opens `PediaDirector.ShowPedia(entry)`. So the Pedia data is a `PediaEntry` asset type and `PediaDirector` owns the runtime side.
- Localized text: `spawn_scaling` already turns a `LocalizedString` into text with `UnityEngine.Localization.LocalizedString.GetLocalizedString()` (argc 0, image `Unity.Localization`), resolved by name and called through `Resolver::Invoke` (see `modules/spawn_scaling/NOTES.md`, "Labels"). Reuse that pattern for the entry's name and section texts.

## To confirm in the dump (`re/dump/`, not in git)

- The exact classes and namespaces: `PediaEntry` and its slime subtype (if any), `PediaDirector`, and how a slime entry links to its `SlimeDefinition` (or carries its own icon Sprite).
- Where the unlocked state lives for the loaded save (a director method such as "is unlocked", or a model list of unlocked entry ids), and whether it changes while the overlay is closed (re-read on every open, it is cheap).
- How an entry stores its sections: a list of section objects with a title and a body `LocalizedString`, or fixed fields. Read the titles from the data; do not hardcode section names.
- The entry's icon Sprite (the one the Slimepedia list shows), and whether it is tightly packed in an atlas (then `textureRect` throws; use `Sprite.uv` bounds instead).

Add what you find to `re/symbols.md` like the other entries.

## Implementation notes

- All IL2CPP work stays on the main thread (`core::main_thread::Post`), as `Search()` does now. Copy plain data out: entry id, name, section titles and texts as UTF-8, and the icon's native texture, rect and texture size. Keep the textures alive with `il::Keep`. The render thread only reads that copy under the mutex.
- Discovery depends on the save, so refresh the discovered set on every open (or when a save loads). Keep the texture views of entries seen before and only re-filter.
- Hover: `menu.cpp` already shows `slime_icons::CurrentName()` as a tooltip over the logo. Replace it with something like `slime_icons::CurrentEntry()` (name plus sections) and the richer tooltip above.
- Failure modes stay quiet: any missing class or method means the current behaviour (all slimes, name-only tooltip) or the drawn fallback. Log once.
- Verify in game: icon not upside down, colours right (sRGB path), only discovered slimes appear (try a new save: few slimes; an old one: many), tooltip sections match the Slimepedia page, and the title screen shows the fallback.
