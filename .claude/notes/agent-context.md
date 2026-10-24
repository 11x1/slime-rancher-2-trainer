# Agent context library (planned, from 2026-10-25)

Agents spend most of their tokens rediscovering context: reading 1,500-line NOTES files, grepping the IL2CPP dumps and re-decompiling the same game functions in IDA. Build a library so an agent finds what is already known in a few reads.

## Goals
- One entry point an agent reads first, pointing to everything else in a sentence each.
- Everything decompiled or reverse-engineered so far written down once, by game system, with class names, method names, RVAs, field offsets and what the code does.
- Module NOTES that describe the current design, not the history of every round.

## Ideas for the shape
- `re/INDEX.md`: a map of game systems (vacpack and ammo slots, upgrades and modifiers, Fabricator and gadgets, drones, plots and plot upgrades, shop, weather and time, map and fog, slimes: diet, emotions, hunger, Ringtail / Phosphor / shadow rules, music and audio, UI: HUD, popups, prompt pills, Options, Slimepedia, save system and sidecar). Each line points to a `re/<system>.md`.
- `re/<system>.md`: classes, methods (RVA, signature), fields (offset, type), call chains already traced (e.g. the ApplyUpgrade -> BatchModifiers -> AmmoSlot.OnSlotChanged NRE chain), gotchas (inlined getters such as GadgetFabricateCategory.GetItems, interface-named methods such as IFabricatableItem.CheckCanFabricate, namespaces such as MonomiPark.SlimeRancher.Slime), and which mod hooks sit where.
- `re/hooks.md`: every hook the mod installs (target, RVA, module, why), so agents check for a second hook on the same address in one place.
- Harvest from the existing notes: re/symbols.md, re/candidates.md, re/custom-upgrades.md, the RE parts of each modules/*/NOTES.md and ui/NOTES.md, and the agents' round reports.
- Split or trim the big module NOTES (custom_upgrades 1,480 lines, minimap, sandbox, slime_dj): keep the current design, move round-by-round history into a short changelog or drop it (git has it).
- Small helper scripts for agents (offline only): find a class or method in the dumps by name, print its RVA and fields; list asset names from the bundles. Document them in the index.
- Update CLAUDE.md and the agent brief to point at re/INDEX.md first.

## Done when
An agent given a typical task (e.g. "change how a corral upgrade card sells") can find the relevant classes, RVAs and existing hooks from the index and one or two system files, without grepping the dumps.
