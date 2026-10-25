# Mod icons

Icons for the mod's own features. This folder holds only the user's own drawings: nothing
AI- or script-drawn and no game art ("trying to move away from AI art"; since 2026-10-26 the
repo and its history carry nothing else). A feature without a drawing shows the game's own
(vanilla) icon or a game sprite found by name, or no icon at all.

The PNGs (256 x 256 RGBA, straight alpha; `*_large` 512 px) are embedded into the DLL at build
time: a CMake step runs `src/gen_inc.py`, which writes `build/generated/sr2mods_icons_data.inc`
(never committed), and `ui/mod_icons.cpp` turns them into Unity sprites at run time.

| file | used for | drawing |
|---|---|---|
| `mods_tab.png` | the "Mods" tab of the in-game Options menu (kept at 128 x 128, the stock category icon's size) | `source/modstabicon_custom.png` |
| `sprinkler_1.png` | the vanilla Sprinkler card (garden T1), `--keep-place` | `source/sprinkler_1_custom.png` |
| `growth_1.png` | Sprinkler II card, `--keep-place` | `source/sprinkler_2_custom.png` |
| `music_1.png` | the vanilla Music Box card (corral T1), `--keep-place` | `source/music_1_custom.png` |
| `ash_boost.png` | Ash Booster card | `source/ash_boost_custom.png` |
| `dj_booth.png`, `dj_booth_large.png` | DJ Booth Fabricator card (tile, details) | `source/dj_booth_custom.png` |
| `dj_speaker.png`, `dj_speaker_large.png` | DJ Speaker Stack card | `source/dj_speaker_custom.png` |
| `slime_radio.png`, `slime_radio_large.png` | Slime Radio card (the same stacked-radio drawing) | `source/slime_radio_custom.png` |
| `plort_door.png` | collectible tracker row | `source/plort_door_custom.png` |

Names a drawing can take over (nothing embedded under them now, the vanilla icon shows):
`sell_all`, `music_2`, `corral_radio`, `hens_1`, `hens_2`, `growth_2`, `coop_air_net`,
`slime_launcher` (cards, `modules/custom_upgrades` `ModIconName`) and `gordo`, `shadow_door`,
`map_node`, `research_drone`, `ghost_drone` (collectible tracker, `modules/collectibles`
`IconFor`; without a drawing or a loaded game sprite the row shows no icon, just its count).
See `.claude/notes/custom-icons.md`.

`source/` keeps the user's hand-drawn originals untouched; they are not embedded (only
`assets/icons/*.png` is).

## Local-only icons (`local/`, gitignored)

`assets/icons/local/` holds icons that are the user's edits of game art and so stay out of the
public repo: `hen_collector_source.png` (Hen Collector card + drone source) and
`elder_collector_source.png` (Elder Collector drone source), with their originals in
`local/source/` (`roostercollector_custom.png`, `eldercollector_custom.png`). `gen_inc.py` embeds
`local/*.png` when the folder exists (the build log says `icons: ... N local`), so the user's
builds have them; a clean checkout and CI build without them and show the game's Hen / elder hen
and Deluxe Coop icons instead. A name may not be in both folders.

## Adding a hand-drawn icon

1. Put the drawing (any size, transparent background) into `assets/icons/source/<name>_custom.png`.
2. Make the embeddable 256 x 256 copy:
   `python assets/icons/src/pad_icon.py assets/icons/source/<name>_custom.png <icon name>`
   It crops to the visible pixels, centres them on a square transparent canvas and reduces
   by a whole factor only (a ~490 px drawing becomes 1/2 scale), so the art is not resampled
   at an odd ratio.
   A drawing made on the vanilla icon's canvas (a redraw of a game card icon, e.g. the 512 px
   Sprinkler / Music Box ones) takes `--keep-place`: the whole canvas is kept and only reduced,
   so the art stays where the vanilla icon has it on the card instead of being centred.
3. `.\build.ps1` embeds it. Commit the drawing and the PNG (there is no generated file to commit).

Only the user's own drawings go in here. Anything based on game art goes into `local/`.

## Tools

Offline only (nothing here goes into the game folder):

- `src/gen_inc.py <out.inc>` (stdlib only): run by the build; rewrites the .inc only when an
  icon changed.
- `src/pad_icon.py` (Pillow): prepares a hand-drawn icon (above).
- `src/build_viewer.py`: the game icon browser (below).

## Browsing the extracted game icons

`python assets/icons/src/build_viewer.py` writes `assets/icons/game/index.html`, an offline page that shows the game icons extracted into `assets/icons/game/` (and ours, for comparison). Open it from disk in a browser. It and the icons it shows are gitignored and stay local: don't publish or upload them.
