# Mod icons

Icons for the mod's own features, drawn for this project (no game art is used or shipped).
256 x 256 RGBA PNGs, straight alpha, in the flat two-tone "sticker" style of the game's
plot-upgrade icons (white outline, darker shadow band). They are embedded into the DLL
(`ui/mod_icons_data.inc`) and turned into Unity sprites at run time by `ui/mod_icons.cpp`.

| file | used for |
|---|---|
| `collector_2.png`, `collector_3.png` | Plort Collector II / III (corral) |
| `feeder_2.png` | Auto-Feeder II (corral) |
| `music_2.png` | Music Box II (corral) |
| `hens_1.png`, `hens_2.png` | Faster Hens I / II (coop) |
| `growth_1.png`, `growth_2.png` | Fast Growth I / II (garden) |
| `ash_boost.png` | Ash Booster (incinerator) |
| `sell_all.png` | Sell-All Button (silo) |
| `sell_all_now.png` | Sell All Plorts Now (silo action) |
| `coop_air_net.png` | Coop Air Net |
| `hen_collector.png` | Hen Collector |
| `mods_tab.png` | the "Mods" tab of the in-game Options menu |
| `map_node.png` | collectible tracker: map data nodes (the game has no sprite for them) |
| `research_drone.png` | collectible tracker: research drones (no map / Slimepedia / HUD sprite in the game) |
| `ghost_drone.png` | collectible tracker: ghostly drones (their IdentifiableType has no icon) |

Tier icons share the object and carry a badge with the tier's Roman numeral (I green,
II blue, III gold); the Ash Booster has an orange "boost" badge.

## Regenerating

Offline only (Python 3 with Pillow and numpy; nothing here goes into the game folder):

```sh
python assets/icons/src/make_icons.py --sheet contact_sheet.png   # writes assets/icons/*.png
python assets/icons/src/gen_inc.py                                 # writes ui/mod_icons_data.inc
```

`src/iconkit.py` holds the drawing helpers (masks on a 1024 px canvas, shadow crescents,
the sticker outline, tier badges); `src/make_icons.py` draws each icon.
