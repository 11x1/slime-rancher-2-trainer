# Coverage: round 16 skipped cases

Every case under the "Skipped / ..." folds of round 16 (list in `.claude/notes/autotest.md`). Case texts:
`.claude/checklist/sr2-test-run.html` (these ids have no `r*-cases.json` entry).

Kinds:
- **auto**: probes, actions and the logs decide; no person needed.
- **semi**: mostly automatic; the test asks the user in the game (screen edges flash) for what a person
  must judge (look, sound), with a screenshot in the report.
- **manual only**: nothing the harness can do.

"Manual part" = a part of the case the test leaves out (in the result note as "not covered"). Most are
"load without the mod": the harness lives in the DLL, so a mod-free game has no harness. A close stand-in
would be a profile with every module off (the DLL loaded, all hooks passing through); not the same thing,
so these parts stay manual.

Pilots (wave 1, have a test file): **r15sl5, r12pg4, r9wx3, r10cr3, r6vu4**.

Totals: **32 auto, 6 semi, 0 manual only** (38 cases); 7 of them have a manual part.

Names: probes and actions are `<module>.<thing>`. The wave-1 generic ones are called through
`run.py`'s `Test` wrappers (`give_newbucks`, `set_time`, `spawn`, `plots`, `plot_menu`,
`sandbox.upgrade_level`), so a different name in the harness is one edit in `run.py`.

| case | title | where | kind | probes / actions it needs | manual part |
|---|---|---|---|---|---|
| r11qs2 | Manual Continue click | game launch | auto | runner restart (see below); exec `ui.main_menu_click {button:"Continue"}`; probe `quick_start.state -> {countdown, loads}` (or count `BeginLoad` lines in the log); the newest save must be slot 5/6 first (saves_list) | |
| r11qs3 | Hold Shift to stay | game launch | auto | runner restart; `input key=LeftShift press` as soon as the pipe is up, release at the menu; log "Staying in the main menu"; state stays on the title for 10 s | |
| r11qs4 | Submenu pauses the countdown | game launch | auto | runner restart; settings_set `quick_start.<delay key>` = 5; exec `ui.main_menu_click {button:"Load Game"}` / `ui.back`; probe `quick_start.state`; state not in game while the submenu is open, in game after back + delay (slot 5/6 newest) | |
| r12pg4 | Drone programs: [P] pill per save | ranch: drone station | auto | **pilot**: probe `drones.stations`, `ui.prompt_pills`, `custom_upgrades.upgrade`; exec `drones.place_station`, `drones.open_menu`, `ui.close_menu`, `custom_upgrades.set_on`, `custom_upgrades.buy` | |
| r15sl5 | Slime Launcher saved per save | slots 5 and 6 | auto | **pilot**: probe `custom_upgrades.upgrade`, `vacpack.last_shot`; exec `custom_upgrades.set_on`, `custom_upgrades.buy`, `vacpack.set_slot`, `vacpack.shoot` | load without the mod |
| r12nc2 | Noclip with HL2 movement | anywhere outdoors | auto | exec `sandbox.noclip {on}`; `input` press/release of the descend action for ~5 s (fly 150 m down, not teleport: the bug was in the flight path); state pos y before / 10 s after noclip off; then `input` jump taps while moving, probe `hl2_movement.state -> {speed, bhop_chain}` | |
| r7sb8 | Sandbox toggles per save | slots 5 and 6 | auto | settings_set the Sandbox infinite energy / health toggles; save, reload, settings_get; load slot 6 (unmarked) -> both off; settings_get must return the tooltip (needs harness below) to check "Kept per save." | |
| r9wx3 | Weather lock and reload | ranch, Sandbox on | semi | **pilot**: probe `sandbox.weather`, `weather.zone`; exec `sandbox.weather`, `weather.force_change`, `ui.open_map` / `ui.close_map`; asks about the map indicator | |
| r9wx4 | Weather: all zones and other zones | several zones | auto | exec `sandbox.weather {zone:"all", pattern:"Clear", lock}`; teleport `to` each zone, probe `weather.zone`; probe `sandbox.weather_status -> {text}` for "skipped zones"; set another zone, teleport there, probe on arrival | |
| r12lp4 | Lamp follows the corral radio | ranch: corral + DJ lamp | semi | r10cr3's radio probes; exec `slime_dj.queue_play`; probe `slime_dj.lamps -> [{id, pos, channel}]` (channel = that radio's plot / global); asks "does the lamp pulse with the radio's song?"; radio off -> channel global | |
| r11cr3 | Radio playlist survives a reload | ranch: corral | auto | probe `slime_dj.radios`; exec `slime_dj.radio {op:add}`; save, reload; exec `slime_dj.open_radio_menu {plot}` (the blue button) and probe the playlist again | |
| r10cr3 | Corral radio playlist | ranch: corral | semi | **pilot**: probe `plots`, `slime_dj.library`, `slime_dj.radios`, `slime_dj.queue`, `slime_dj.corral_audio`; exec `plots.build`, `plots.buy_upgrade`, `custom_upgrades.buy_plot`, `slime_dj.radio`, `slime_dj.queue_play`; asks whether the song plays from that corral only | |
| r10cr4 | Corral radio save and limits | ranch: 5 corrals | auto | as r10cr3, 5 radio corrals (`plots.build` + `custom_upgrades.buy_plot`); probe `slime_dj.radios` `status` = "Waiting..." on the farthest, playing within 2 s after one goes off; custom upgrades off -> probe `slime_dj.card -> {text}` explains it needs Custom Upgrades, `slime_dj.queue` still plays | |
| r7bd6 | Buildables: saving | ranch: Fabricator buildables | semi | exec `gadgets.place {id}` (desk / boombox variants); probe `custom_upgrades.variants -> [{actor, variant, dressed}]`; save, reload, probe again; custom upgrades off -> plain; probe `sandbox.crafter_items` has none of them; asks "everything dressed again?" with a screenshot | |
| r9rs3 | Radio shop edge cases | shop + ranch | auto | exec `shop.buy {entry}`; probe `shop.pending -> [{id, left}]`; save before all are dispensed, reload, `find` the spawned gadgets (type names); slot 6 without the Boombox blueprint: probe `shop.entries`; `sandbox.unlock_blueprint`; custom upgrades off -> entry gone; exec `shop.set_daily_toy {id:"NightLight"}` | load without the mod (Night Lights, no errors) |
| r10dj6 | Radio in the daily toy rotation | shop | auto | exec `shop.restock` ~40 times (or `set_time` over days); probe `shop.daily_toy -> {id, price, icon}`: radio share 10-45 %, price 1000, icon = Boombox sprite; buy: newbucks -1000 once, `shop.entries` sold out, a radio arrives (`find`); probe `shop.exchange_offers` has no Slime Radio | |
| r12pg1 | Old save without the upgrade | ranch: drone station | auto | stand-in for an old save: slot 6 with 2 programs, then exec `sandbox.remove_upgrade {id:"drone_programs"}`, save, reload; probe `drones.station {id} -> {programs, running}`; pills (r12pg4); `input key=P` changes nothing; `set_time add_hours` x several, running stays 1 | a real save from a build before the upgrade |
| r10dp3 | No switching while the menu is open | ranch: drone station | auto | exec `drones.open_menu {page:"Drone List"}`; wait 30 s real time; probe `drones.station` running unchanged; exec `ui.close_menu`; state `menus` has no discard popup; change the target without Start (exec `drones.set_target`) then close: the popup is there | |
| r9dp4 | Remove, save and reload | ranch: drone station | auto | exec `drones.programs {op:add/remove}`; probe `drones.station`; pills; save while program 2 runs, reload: log "2 programs restored from the sidecar ... program 1 runs"; custom upgrades off -> program 1; Hen Collector / Quantum source as program 1 restored | without the mod |
| r9dp5 | Station gone, upgrades off | ranch: drone station | auto | exec `gadgets.pickup {actor}`: log "extra programs dropped"; custom upgrades off / on while program 2 runs: probe `drones.station`; price-limit pill in `ui.prompt_pills`, `input key=C` opens the dialog (state `menus`) | |
| r7dr2 | Elder Collector | ranch: deluxe coop + drone station | semi | `plots.build` coop + `plots.buy_upgrade` deluxe; exec `drones.set_program {source:"ElderCollector", ...}`; `set_time add_hours`; probe `plots.contents {plot}` (only the elder collector emptied); save, reload, probe sources; probe `ui.list_rows` of the Drone List for names; asks about the icons (screenshot); exec `plots.sell_upgrade` deluxe -> source Coop | |
| r4dr3 | Hen source and saves | ranch: coop + drone station | auto | save, reload: log "restored from the sidecar", probe `drones.station {source}`; exec `plots.sell_upgrade` (all collectors) / custom upgrades off -> Coop + a log line, no faults; back on -> restored; `gadgets.pickup` + `gadgets.place` -> Coop | optional: without the mod |
| r10sh2 | Shaded shadow slime | ranch: shaded corral | auto | exec `vacpack.set_slot` Shadow + `vacpack.shoot` aimed into the corral (or `spawn` there; shooting is the real path); probe `slimes.state {near, radius} -> [{actor, id, face, hunger}]`: no scared face, still there after 2 min; `spawn` 10 fruits onto it: plort count (`find` Plort) +0 from hits, at most +1 from eating, face mischievous | |
| r10sh3 | Shadow slime in the sun | ranch: open + shielded corral | auto | `set_time hour=10`; shoot one in: face scared, gone (`find`) within 15-30 s; again: exec `vacpack.vac {actor}` within 10 s, shoot into the shielded corral: normal face, stays; `set_time hour=19`: stays; `set_time` to daybreak: scared, fades unless shaded | |
| r10sh4 | Shadow hunger rhythm | ranch: corral | auto | `slimes.state` hunger; feed one (`spawn` food): +1 Shadow plort, face mischievous; feed more: eaten, +0; `set_time add_hours=4`; feed: +1 again | |
| r10sh5 | Wild shadow slimes unchanged | the zone with wild shadow slimes | semi | teleport `to` their area; `find` wild Shadow slimes; exec `vacpack.shoot` plorts at one; plort count near it rises (`find`); asks "did they scatter as in vanilla?" | |
| r9sh4 | Shadow slimes in the vacpack save | slot 5 | auto | Shadow Catcher (`custom_upgrades.buy`); `vacpack.set_slot` Shadow; save: log "kept in the sidecar"; reload: log "back in the vacpack from the sidecar"; probe `vacpack.slots`; corral one still there (`slimes.state`) | |
| r9sh5 | Shadow slimes: off and mod-free | ranch | auto | custom upgrades off; teleport near the corral: `find` Shadow on the ranch -> 0; with some in the vacpack: a WARN in the log | load without the mod |
| r9sh7 | Shadow feeding over time | ranch: corral with garden / feeder | auto | `plots.build` garden or feeder; `set_time add_hours` in steps; count plorts (`find`), about one per N game hours; teleport off the ranch / custom upgrades off: face normal, no eating when full; save, reload a full one: `slimes.state` hunger kept | |
| r8tp3 | Teleport after a map node | a zone with a map data node | auto | exec `map.activate_node {id}` (or teleport to it + `input` interact); exec `ui.open_map`; exec `map_tools.teleport_at {map_pos}` in the newly cleared area; state pos changed | |
| r4mm4 | Minimap performance | anywhere, minimap on | auto | stand still 35 s; log "minimap: perf (30 s)" with "page transform set in 0 frames"; the frame:minimap avg / max go into the note | |
| r6vu4 | Vacpack upgrades saving | slot 5 | auto | **pilot**: `sandbox.upgrade_level`; probe `vacpack.slots`; exec `vacpack.set_slot`; log lines "spread into the extended layout", "refilled from the sidecar", "level 8 -> 10" | old save (Extra Tank IV carry-over, old IX "not applied yet") |
| hc4 | Selling the Hen Collector mid-flight | ranch: coop | auto | coop with Hen Collector; `spawn` chickens; exec `plots.sell_upgrade {upgrade:"hen_collector"}` while they fly; after 3 s `find` Chicken with `fields` (Rigidbody isKinematic / velocity, position over 2 s): none frozen in the air | |
| p7 | Per-save upgrades | slot 5 | auto | save, buy a tier (`custom_upgrades.buy`), save; **needs harness `load_save`** to load the older save; probe the level: not there | |
| ug2 | Upgrades lock per save | slot 5 | auto | new game with custom upgrades off, save; on: probe `custom_upgrades.state` locked, Fabricator items appear (probe `fabricator.items`); title: unlocked; reload: on and locked. **needs harness: settings_get returns the lock** | |
| ug3 | Existing save turns the switch on | slot 6 | auto | stand-in for "slot 1/3": a slot 6 game that has custom upgrade data; switch off at the title; load slot 6; within 1 s settings_get `custom_upgrades.enabled` = on and locked. The user's slots 1 and 3 are never loaded | |
| sv2 | Per-save sidecar files | files only | auto | read only: list `<saves>\*.sav` and `<game folder>\sr2mods_saves\*.sav.json` and match names (`<game>_<n>`) for every game with a sidecar; no harness command needed beyond saves_list (slot 5/6 games get a save first) | |
| ut6 | Keyboard selling | Fabricator: Upgrades | auto | exec `ui.open_fabricator {category:"Upgrades"}`, `ui.select_item {id}` on an owned sellable card; `input` tap of the Sell key (settings_get the keybind); probe the level -1 exactly once, newbucks up once; state `menus` has no popup / Pedia | |

## Needs harness (beyond the section A contract)

The client works with the contract as written; these would let more tests run, or make them exact.

1. **`timeout_s` arg.** The client sends `args.timeout_s` on `load_slot` (120), `new_game` (180),
   `quit_to_title` (60), `save` (30), `delete_slot` (30), `screenshot` (15) and `ask` (the question's
   time + 10). Loading a save takes longer than the default 10 s; the harness should use it as that
   command's timeout.
2. **`load_save {slot, save_number}`**: load a given (older) save of slot 5 or 6, not the newest (p7).
   Same slot guard as load_slot.
3. **Game restart** for r11qs2-4: `quit_game` (Application.Quit, refused unless on the title screen),
   then the runner starts the game through Steam (`steam://rungameid/1657630`) and reconnects. The pipe
   must be up before the main menu shows (setting on in sr2mods.json at startup).
4. **Unknown names.** For an unknown probe / action / command the error text should contain
   "unknown" (e.g. `unknown probe 'drones.stations'`). The client turns that into "skipped: needs
   harness" instead of an error.
5. **Field names** the client and tests read (rename in one place if the harness differs):
   - `state`: `scene, in_game, paused, game_name, display_name, slot, save_number, pos, rot, zone,
     time, newbucks, menus, overlay_open`
   - `saves_list`: a list (or `{games: [...]}`) of `{game_name, display_name, slot, last_save}`
   - `settings_get` / `settings_set`: `key` = `"<module>.<key>"` (the sr2mods.json path under
     `modules`), result `{key, value}` (a bare value works too)
   - `log_mark`: `{mark}` = the exact text written to sr2mods.log
   - `screenshot`: `{path}`
   - `ask`: `{answer, note}` (a bare label works too; `{pending: true}` + an `answer` event also works)
   - `find`: a list (or `{objects: [...]}`)
6. **`settings_get` with the lock and tooltip**: `{value, locked, lock_reason, tooltip}` (ug2, ug3, r7sb8).
7. **`settings_set` of `sandbox.enabled`** applies without the overlay's confirmation dialog (r9wx3,
   r9wx4, r7sb8 and every Sandbox action need the Sandbox on).
8. **`list_names`** (optional): every registered probe and action, so the runner can skip a case
   whose `NEEDS` are missing before it starts changing the game.

## Probes and actions the tests call (for wave 2)

Generic (wave 1): `give_newbucks {amount}`, `set_time {hour | add_hours}`, `spawn {id, count, pos?}`,
probe `plots`, `plot_menu {plot}`, `sandbox.upgrade_level {id, level}`.

Module ones (by owner): custom_upgrades (`state`, `set_on`, `buy`, `buy_plot`, `upgrade`, `variants`),
vacpack (`slots`, `set_slot`, `shoot`, `vac`, `last_shot`), drones (`stations`, `station`,
`place_station`, `open_menu`, `programs`, `set_program`, `set_target`), ui (`close_menu`,
`prompt_pills`, `open_map`, `close_map`, `main_menu_click`, `back`, `open_fabricator`, `select_item`,
`list_rows`), weather / sandbox (`weather.zone`, `weather.force_change`, `sandbox.weather`,
`sandbox.weather_status`, `sandbox.noclip`, `sandbox.remove_upgrade`, `sandbox.unlock_blueprint`,
`sandbox.crafter_items`), slime_dj (`library`, `radios`, `queue`, `radio`, `queue_play`,
`corral_audio`, `open_radio_menu`, `card`, `lamps`), plots (`build`, `buy_upgrade`, `sell_upgrade`,
`contents`), shop (`buy`, `pending`, `entries`, `restock`, `daily_toy`, `set_daily_toy`,
`exchange_offers`), gadgets (`place`, `pickup`), slimes (`state`), map (`activate_node`),
map_tools (`teleport_at`), quick_start (`state`), hl2_movement (`state`), fabricator (`items`).
