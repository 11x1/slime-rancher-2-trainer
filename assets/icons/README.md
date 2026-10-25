# Mod icons

Icons for the mod's own features: the user's own drawings, plus three script-drawn placeholders
for collectible tracker rows the game has no sprite for. Since round 11 nothing else is
script-drawn ("trying to move away from AI art"): mod features without a drawing show the
game's own (vanilla) icon. 256 x 256 RGBA PNGs, straight alpha. They are embedded into the DLL
(`ui/mod_icons_data.inc`) and turned into Unity sprites at run time by `ui/mod_icons.cpp`.

| file | used for |
|---|---|
| `hen_collector_source.png` | Hen Collector: the coop upgrade card and the drone program source (user's drawing, `source/roostercollector_custom.png`) |
| `elder_collector_source.png` | Elder Collector drone source (user's drawing, `source/eldercollector_custom.png`); without it the source shows the elder hen |
| `mods_tab.png` | the "Mods" tab of the in-game Options menu (user's drawing, `source/modstabicon_custom.png`, kept at its 128 x 128: the tab scales it to the stock category icon, which is 128 x 128 too) |
| `map_node_placeholder.png`, `research_drone_placeholder.png`, `ghost_drone_placeholder.png` | collectible tracker, script-drawn (`src/make_icons.py`): shown only when neither a user drawing (`map_node.png` / `research_drone.png` / `ghost_drone.png`) nor the game sprite found by name (`pediaHomeWorld`, `iconPediaDrone(_small)`, `iconExplorerDronePedia`) is there |

Names a drawing can take over (nothing embedded under them now, the vanilla icon shows):
`sell_all`, `music_2`, `corral_radio`, `hens_1`, `hens_2`, `growth_1`, `growth_2`, `ash_boost`,
`coop_air_net` (plot upgrade cards, `modules/custom_upgrades` `ModIconName`) and `gordo`,
`plort_door`, `shadow_door`, `map_node`, `research_drone`, `ghost_drone` (collectible tracker,
`modules/collectibles` `IconFor`). See `.claude/notes/custom-icons.md`.

`source/` keeps the user's hand-drawn originals untouched; they are not embedded (only
`assets/icons/*.png` is).

## Adding a hand-drawn icon (e.g. the Elder Collector)

1. Put the drawing (any size, transparent background) into `assets/icons/source/`.
2. Make the embeddable 256 x 256 copy:
   `python assets/icons/src/pad_icon.py assets/icons/source/<drawing>.png elder_collector_source`
   It crops to the visible pixels, centres them on a square transparent canvas and reduces
   by a whole factor only (a ~490 px drawing becomes 1/2 scale), so the art is not resampled
   at an odd ratio.
   A drawing made on the vanilla icon's canvas (a redraw of a game card icon, e.g. the 512 px
   Sprinkler / Music Box ones) takes `--keep-place`: the whole canvas is kept and only reduced,
   so the art stays where the vanilla icon has it on the card instead of being centred.
3. `.\build.ps1` (it runs `src/gen_inc.py`, which embeds every `assets/icons/*.png`).

For the Elder Collector that is all: `modules/custom_upgrades/drone_source.cpp` looks for
`elder_collector_source` (`mod_icons::Has`) and uses it for the drone source when it is
embedded. The log says `drone source: Elder Collector icon = the mod icon
'elder_collector_source'`. Commit the PNG and the regenerated `ui/mod_icons_data.inc`.

## Regenerating

Offline only (Python 3 with Pillow and numpy; nothing here goes into the game folder):

```sh
python assets/icons/src/make_icons.py --sheet contact_sheet.png   # writes assets/icons/*_placeholder.png
python assets/icons/src/gen_inc.py                                 # writes ui/mod_icons_data.inc
```

`build.ps1` runs `gen_inc.py` before every build (stdlib only; the .inc is rewritten only when
an icon changed; without Python the committed .inc is used). `src/iconkit.py` holds the drawing
helpers (masks on a 1024 px canvas, shadow crescents, the sticker outline, tier badges);
`src/make_icons.py` draws the placeholders; `src/pad_icon.py` prepares hand-drawn ones.

## Browsing the extracted game icons

`python assets/icons/src/build_viewer.py` writes `assets/icons/game/index.html`, an offline page that shows the game icons extracted into `assets/icons/game/` (and ours, for comparison). Open it from disk in a browser. It and the icons it shows are gitignored and stay local: don't publish or upload them.
