# plot_upgrades

Applies the effects of the purchasable per-plot upgrade tiers. The tiers are custom upgrades
(`modules/custom_upgrades`): bought in the plot's own purchase menu, stored in the sidecar
`sr2mods_saves\<game>.json` (never in the vanilla save), sold back with sell_upgrades. They only show once Custom Upgrades is switched on (off by default; it
then stays locked on for that save, see `re/custom-upgrades.md` 1.2b).

## Tiers

| tier | plot | needs | price | effect |
|---|---|---|---|---|
| Music Box II | corral | Music Box | 2000 | 2x calming bonus (0.5 -> 1.0) |
| Faster Hens I | coop | - | 2000 | hens lay 1.5x as often |
| Faster Hens II | coop | Faster Hens I | 4000 | hens lay 2x as often |
| Sprinkler II | garden | - | 2000 | crops grow 1.5x as fast (stacks with the Sprinkler) |
| Sprinkler III | garden | Sprinkler II | 4000 | crops grow 2x as fast |

Sprinkler II / III were called Fast Growth I / II until round 8 (user: rename them, with descriptions in
the voice of Slime Rancher 1's garden upgrades). Only the display texts changed: the save ids stay
`garden_growth_1` / `garden_growth_2`, so owned tiers carry over.
| Ash Booster | incinerator | Ash Trough | 1500 | 2x ash per incinerated food (capacity unchanged) |

Only the highest owned tier of a family counts. Prices are fixed in `kDefs`
(custom_upgrades/module.cpp).

Each chain has one menu card: the first tier not owned yet, or the last one once all are owned
(greyed, checkmark). After Faster Hens I is bought the same card reads Faster Hens II, with Buy
(next tier) and Sell (the owned tier) side by side. Music Box II and Ash Booster take over the
vanilla Music Box / Ash Trough card once that is owned; Sell on that card sells the vanilla
upgrade. Sprinkler II / III is listed right after the Sprinkler. Selling is refused while a higher
tier (or, for a vanilla prerequisite, any tier built on it) is still owned. Details:
`re/custom-upgrades.md` (1.3, 2b) and the custom_upgrades / sell_upgrades headers.

Retired: Plort Collector II / III and Auto-Feeder II (checklist u1-u3, "doesn't make sense").
Saves that own one load cleanly: the sidecar key is ignored (logged, no refund) and dropped at
the next save; the collector / feeder hooks are gone.

## Per-plot lookup

`custom_upgrades::Multiplier(obj, family)`: `obj` is the LandPlot or a component below it.
The result is cached per object, invalidated on buy / sell / load / new game / sidecar read and
expired after ~900 frames, so the hooks do one hash-map read per call. Plots without a tier run
plain vanilla code.

* incinerator: the component itself (`GetComponentInParent(LandPlot)` on a miss)
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
plort collector speed and auto-feeder amount (retired tiers), collector vac radius, garden yield,
walls / air net / solar shield / scareslime / vitamizer / soil (no numeric tunables).
