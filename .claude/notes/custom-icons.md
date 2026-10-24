# Custom icons by the user (planned, from 2026-10-25)

The user is replacing AI-drawn / script-drawn art with their own drawings ("trying to move away from AI art"). Until a drawing exists, use the game's own (vanilla) icon, never a generated one.

## How a drawing goes in
1. The user puts the drawing in `assets/icons/source/<name>_custom.png` (any size, transparent background).
2. `python assets/icons/src/pad_icon.py assets/icons/source/<name>_custom.png <icon name>` makes the 256 x 256 copy in `assets/icons/` (a 128 px drawing that should match a 128 px stock icon can be copied as is, like `mods_tab.png`).
3. `.\build.ps1` embeds it (`gen_inc.py`); the code picks it up by name through `mod_icons::Get` / `mod_icons::Has`, falling back to the vanilla icon.
4. Remove its drawing function from `assets/icons/src/make_icons.py` so a regeneration can't overwrite it.
The extracted game icons for reference are in `assets/icons/game/` (gitignored); browse them with `python assets/icons/src/build_viewer.py` and open `assets/icons/game/index.html`.

## Done by the user
- `mods_tab` (Options menu Mods tab), `hen_collector_source`, `elder_collector_source`.

## Still script-drawn: switch to the vanilla icon now, the user draws later
| icon name | used for | vanilla fallback to use meanwhile |
|---|---|---|
| `music_2` | Music Box II card | the Music Box upgrade icon |
| `collector_2`, `collector_3` | Plort Collector II / III | the Plort Collector icon |
| `feeder_2` | Auto-Feeder II | the Auto-Feeder icon |
| `hens_1`, `hens_2` | Faster Hens I / II | a coop upgrade icon (e.g. the coop's vanilla tier icon) |
| `ash_boost` | Ash Booster | the Ash Trough icon |
| `sell_all`, `sell_all_now` | Sell-All Button, Sell All Plorts Now | a silo upgrade icon / Newbucks icon |
| `map_node`, `research_drone`, `ghost_drone`, `gordo`, `plort_door`, `shadow_door` | collectible tracker | the closest game sprite (map marker, drone, gordo, door), or keep until drawn (the game has no sprite for some) |
| `corral_radio` (missing) | Corral Radio card | the Music Box icon (already) |
| Sandbox sidebar icon | the overlay's Sandbox entry | an icon drawn in the overlay's own style (it's ImGui-drawn, not a game sprite) |

`growth_1`, `growth_2` and `coop_air_net` are already unused (vanilla icons shown).
