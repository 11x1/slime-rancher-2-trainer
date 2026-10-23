# plot_upgrades

Applies the effects of the purchasable per-plot upgrade tiers. The tiers are custom upgrades
(`modules/custom_upgrades`): bought in the plot's own purchase menu, stored in the sidecar
`sr2mods_saves\<game>.json` (never in the vanilla save), sold back with sell_upgrades.

## Tiers

| tier | plot | needs | price | effect |
|---|---|---|---|---|
| Plort Collector II | corral | Plort Collector | 2500 | collects 1.5x as often |
| Plort Collector III | corral | Plort Collector II | 5000 | collects 2x as often |
| Auto-Feeder II | corral | Auto-Feeder | 2500 | 2x food per feeding cycle |
| Music Box II | corral | Music Box | 2000 | 2x calming bonus (0.5 -> 1.0) |
| Faster Hens I | coop | - | 2000 | hens lay 1.5x as often |
| Faster Hens II | coop | Faster Hens I | 4000 | hens lay 2x as often |
| Fast Growth I | garden | - | 2000 | crops grow 1.5x as fast (stacks with the Sprinkler) |
| Fast Growth II | garden | Fast Growth I | 4000 | crops grow 2x as fast |
| Ash Booster | incinerator | Ash Trough | 1500 | 2x ash per incinerated food (capacity unchanged) |

Only the highest owned tier of a family counts. A tier appears in the menu once its
prerequisite is owned and reuses the vanilla upgrade's icon. Selling is refused while a higher
tier (or, for a vanilla prerequisite, any tier built on it) is still owned. Prices are fixed
in `kDefs` (custom_upgrades/module.cpp).

## Per-plot lookup

`custom_upgrades::Multiplier(obj, family)`: `obj` is the LandPlot or a component below it.
The result is cached per object, invalidated on buy / sell / load / new game / sidecar read and
expired after ~900 frames, so the hooks do one hash-map read per call. Plots without a tier run
plain vanilla code.

* collector / feeder / incinerator: the component itself (`GetComponentInParent(LandPlot)` on a miss)
* garden: `SpawnResource._landPlot`
* music box: the slime must be in `MusicBoxRegion._currentEmotions` of a region whose corral owns
  Music Box II; regions are registered by a `MusicBoxRegion.OnTriggerEnter` detour
* coop: the `CoopRegion` (static `_allCoops`) whose collider bounds contain the hen

## Global sliders (removed)

The former global multipliers (`collector_speed`, `feeder_amount`, `musicbox_calm`,
`coop_speed`, `garden_growth`, `ash_amount`) are gone from the Mods tab; old values left in
`sr2mods.json` are ignored. No "global override" was kept: the settings system has no hidden /
advanced flag, so it would have shown up as a switch again.

## Not tiered (on purpose)

Silo / feeder storage capacity (stored counts are saved and not clamped on a mod-free load),
collector vac radius, garden yield, walls / air net / solar shield / scareslime / vitamizer / soil
(no numeric tunables). Pond and incinerator plort collectors are not tiered (corral only).
