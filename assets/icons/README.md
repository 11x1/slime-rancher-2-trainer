# Mod icons

Icons for the mod's own features. Most are drawn for this project by `src/make_icons.py`;
the exception is the Hen Collector icon, which the user drew over the game's Plort Collector
icon (so it contains game art). 256 x 256 RGBA PNGs, straight alpha, in the flat two-tone
"sticker" style of the game's plot-upgrade icons (white outline, darker shadow band). They are
embedded into the DLL (`ui/mod_icons_data.inc`) and turned into Unity sprites at run time by
`ui/mod_icons.cpp`.

| file | used for |
|---|---|
| `collector_2.png`, `collector_3.png` | Plort Collector II / III (corral) |
| `feeder_2.png` | Auto-Feeder II (corral) |
| `music_2.png` | Music Box II (corral) |
| `hens_1.png`, `hens_2.png` | Faster Hens I / II (coop) |
| `growth_1.png`, `growth_2.png` | Sprinkler II / III (garden; ids garden_growth_1 / 2) |
| `ash_boost.png` | Ash Booster (incinerator) |
| `sell_all.png` | Sell-All Button (silo) |
| `sell_all_now.png` | Sell All Plorts Now (silo action) |
| `coop_air_net.png` | unused: the Coop Air Net card shows the corral's own Air Net icon (user request) |
| `hen_collector_source.png` | Hen Collector: the coop upgrade card and the drone program source (user's drawing, `source/roostercollector_custom.png`) |
| `elder_collector_source.png` | Elder Collector drone source (user's drawing, `source/eldercollector_custom.png`); without it the source shows the elder hen |
| `mods_tab.png` | the "Mods" tab of the in-game Options menu (user's drawing, `source/modstabicon_custom.png`, kept at its 128 x 128: the tab scales it to the stock category icon, which is 128 x 128 too) |
| `map_node.png` | collectible tracker: map data nodes (the game has no sprite for them) |
| `research_drone.png` | collectible tracker: research drones (no map / Slimepedia / HUD sprite in the game) |
| `ghost_drone.png` | collectible tracker: ghostly drones (their IdentifiableType has no icon) |
| `gordo.png` | collectible tracker: gordos (the map marker sprite is not recognisable at tracker size) |
| `plort_door.png`, `shadow_door.png` | collectible tracker: plort doors / shadow plort doors (same reason) |

Tier icons share the object and carry a badge with the tier's Roman numeral (I green,
II blue, III gold); the Ash Booster has an orange "boost" badge.

`source/` keeps the user's hand-drawn originals untouched; they are not embedded (only
`assets/icons/*.png` is).

## Adding a hand-drawn icon (e.g. the Elder Collector)

1. Put the drawing (any size, transparent background) into `assets/icons/source/`.
2. Make the embeddable 256 x 256 copy:
   `python assets/icons/src/pad_icon.py assets/icons/source/<drawing>.png elder_collector_source`
   It crops to the visible pixels, centres them on a square transparent canvas and reduces
   by a whole factor only (a ~490 px drawing becomes 1/2 scale), so the art is not resampled
   at an odd ratio.
3. `.\build.ps1` (it runs `src/gen_inc.py`, which embeds every `assets/icons/*.png`).

For the Elder Collector that is all: `modules/custom_upgrades/drone_source.cpp` looks for
`elder_collector_source` (`mod_icons::Has`) and uses it for the drone source when it is
embedded. The log says `drone source: Elder Collector icon = the mod icon
'elder_collector_source'`. Commit the PNG and the regenerated `ui/mod_icons_data.inc`.

## Regenerating

Offline only (Python 3 with Pillow and numpy; nothing here goes into the game folder):

```sh
python assets/icons/src/make_icons.py --sheet contact_sheet.png   # writes the drawn assets/icons/*.png
python assets/icons/src/gen_inc.py                                 # writes ui/mod_icons_data.inc
```

`build.ps1` runs `gen_inc.py` before every build (stdlib only; the .inc is rewritten only when
an icon changed; without Python the committed .inc is used). `src/iconkit.py` holds the drawing
helpers (masks on a 1024 px canvas, shadow crescents, the sticker outline, tier badges);
`src/make_icons.py` draws each icon; `src/pad_icon.py` prepares hand-drawn ones.
