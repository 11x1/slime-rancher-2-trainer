# Custom icons by the user (planned, from 2026-10-25)

The user is replacing AI-drawn / script-drawn art with their own drawings ("trying to move away from AI art"). Until a drawing exists, use the game's own (vanilla) icon, never a generated one.

## How a drawing goes in
1. The user puts the drawing in `assets/icons/source/<name>_custom.png` (any size, transparent background).
2. `python assets/icons/src/pad_icon.py assets/icons/source/<name>_custom.png <icon name>` makes the 256 x 256 copy in `assets/icons/` (a 128 px drawing that should match a 128 px stock icon can be copied as is, like `mods_tab.png`).
   For a buildable's Fabricator card (DJ Booth / Speaker Stack / Slime Radio) also make the details copy: `python assets/icons/src/pad_icon.py assets/icons/source/<name>_custom.png <name>_large 512` (shown sharp in the details pane; without it the 256 px icon is used there).
3. `.\build.ps1` embeds it (`gen_inc.py`); the code picks it up by name through `mod_icons::Get` / `mod_icons::Has`, falling back to the vanilla icon.
4. Remove its drawing function from `assets/icons/src/make_icons.py` so a regeneration can't overwrite it.
The extracted game icons for reference are in `assets/icons/game/` (gitignored); browse them with `python assets/icons/src/build_viewer.py` and open `assets/icons/game/index.html`.

## Done by the user
- `mods_tab` (Options menu Mods tab), `hen_collector_source`, `elder_collector_source`, `dj_speaker` (DJ Speaker Stack card, from stackedradio.png), `dj_booth` (DJ Booth card, with a 512 px `dj_booth_large`), `slime_radio` (Slime Radio card, the same stacked-radio drawing as `dj_speaker`, the user's choice).

## Vanilla icon now, the user draws later (done 2026-10-25, branch chore/vanilla-card-icons)
The script-drawn PNGs were removed from `assets/icons/`. Each name below has nothing embedded, so the vanilla icon shows; a drawing saved as `assets/icons/<name>.png` takes over at the next build (the code checks `mod_icons::Has(name)` first).

| icon name to draw | used for | vanilla icon shown now |
|---|---|---|
| `music_2` | Music Box II card | the Music Box upgrade icon |
| `corral_radio` | Corral Radio card | the Music Box upgrade icon |
| `hens_1`, `hens_2` | Faster Hens I / II | the Deluxe Coop upgrade icon |
| `ash_boost` | Ash Booster | the Ash Trough upgrade icon |
| `sell_all` | Sell-All Button | the Market Link icon |
| `growth_1`, `growth_2` | Sprinkler II / III | the Sprinkler upgrade icon |
| `slime_launcher` | Slime Launcher I-III (Fabricator) | the Pink slime icon (details: its Slimepedia picture) |
| `coop_air_net` | Coop Air Net | the corral's Air Net upgrade icon, for good (the user's choice, no drawing planned) |
| `gordo` | collectible tracker | sprite `iconGordoPedia_small` / `iconGordoPedia` (Slimepedia), else the map marker |
| `plort_door`, `shadow_door` | collectible tracker | the map marker sprites |
| `ghost_drone` | collectible tracker (under the minimap) | sprite `iconExplorerDronePedia` (user's pick, confirmed round 16; loaded through a drone station UI preload, see re/drones.md), else placeholder |
| `research_drone` | collectible tracker | sprite `iconPediaDrone_small` / `iconPediaDrone`, else placeholder |
| `map_node` | collectible tracker | sprite `pediaHomeWorld` (the user's pick), then `iconPediaHomeWorld` (Slimepedia World tile, may not be loaded in game), else placeholder |

The tracker loads the game sprites by name (one `FindObjectsOfTypeAll(Sprite)` after a map page build, at most 3 per session; log line `collectibles: game sprites by name ...`). If one isn't loaded, `<name>_placeholder.png` (script-drawn, `make_icons.py`) shows for the three drone / node rows: these are the only script-drawn icons left. `collector_2/3`, `feeder_2` and `sell_all_now` belonged to retired upgrades and are gone.

Still open, owned by other agents: the Sandbox sidebar icon (ImGui-drawn, overlay style), the Shadow Catcher card icon.
