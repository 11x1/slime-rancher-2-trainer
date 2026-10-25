# misc_tweaks module notes

Small speed-ups. Module key `misc_tweaks`, on by default, but every option is off (or 1x) by default, so it changes nothing until one is turned on. Options tab: Misc > General, its own card, with the small sub-headers Fabricator and Vacpack (the card's groups). Help texts are one short sentence each (round 9 request: "keep them 1 sentence and lean"). Game 1.3.0; everything is resolved by name, the RVAs are for re-checking after an update (Cpp2IL dump + IDA). Each feature degrades on its own: a missing symbol drops that feature's hook and its combo entry or settings (log: `fast Fabricator purchases NOT available: ...` / `skip upgrade animation NOT available: ...` / `vacpack shooting options NOT available: ...`); the module fails only when all three are missing.

## Fabricator purchases (`fab_purchases`, combo, default Normal)

Round 9 request: "fast fabricator purchases should override skip upgrade animation, make it a combo instead". The two switches below became one Enum: **Normal** (the game's way), **Skip animation** (the confirm window stays, the success animation goes: the HideForConfirm detour), **Instant** (no window, bought at once: the OnItemActivated detour; the HideForConfirm skip is on too, for what still opens the window). Without the OnItemActivated hook the combo has only Normal / Skip animation; without the HideForConfirm hook, Instant still works and Skip animation does nothing (logged at setup).

**Migration** (`MigrateFabMode`, once, at setup, only when `fab_purchases` was never saved and an old key was): `fast_fabricator` on -> Instant; else `skip_fab_anim` on -> Skip animation; else Normal. Logged as `migrated Fabricator settings (...) -> Fabricator purchases = N`. The old keys stay in `sr2mods.json` but are no longer registered or read. Profiles saved before round 9 still carry the old keys, which do nothing now.

## Instant purchases (was `fast_fabricator` until round 9)

"It takes a bit right now, like completely remove the game's popup maybe?" The game's flow
(`FabricatorUI.OnItemActivated` -> `FabricatorConfirmPopup.Show` -> `OnSubmit` -> `Confirmed` ->
`FabricatorUI.OnConfirmationConfirmed`, RVAs and slots): `re/fabricator.md`.

What the module does: a detour on `OnItemActivated`. With the option on, for a `GadgetFabricatableItem` / `PlayerUpgradeFabricatableItem` and `_confirming` false: `CheckCanFabricate(1)` (the popup's own check: blueprint, costs, max count, fully upgraded); NONE (0) -> `OnConfirmationConfirmed(item, 1)` (the game's purchase, normal cost) and the popup's success cue (`FabricatorSoundBank.PlayConfirmationConfirmedForUpgrade` / `ForGadget`, as `PlaySuccessAudioCue`); the original is skipped. Anything else (blocked, another item type, a fault in the guard) runs the original: the game's popup with its reason, or nothing when it cannot be afforded (as before). Gadgets are made one per press (the popup's amount picker is skipped). The content and key bar are never disabled on this path, so the Fabricator keeps taking input.

**Round 7 failure (2026-10-24), fixed:** the option never appeared. `sr2mods.log`: `fast Fabricator purchases NOT available: CheckCanFabricate: method 'CheckCanFabricate' (argc=1) not found on class GadgetFabricatableItem`. Both item classes implement `CheckCanFabricate` (and `CheckCanAffordFabricateCosts`, `CheckCanBeFabricatedIgnoringCosts`, `FabricateAndSpendCost`) as **explicit interface implementations**, so the metadata name is `MonomiPark.SlimeRancher.UI.Fabricator.IFabricatableItem.CheckCanFabricate` (Gadget 0x827D50, Upgrade 0x82AE30). The module now looks that name up first, the plain name second. The hook target (`OnItemActivated`) and the call flow above were re-checked in IDA and are right.

Log: at setup the RVAs of the hook, both `CheckCanFabricate` bodies and `OnConfirmationConfirmed`; per press `fast purchase: <class> (upgrade | gadget x1)` (first 20), or `fast purchase skipped, the game's way: <class> (<why>)` (first 20: popup already up, not an upgrade / gadget, or `CheckCanFabricate(1) = N`, the `FabricationBlockedReason`).

Not seen in game: the grid's refresh after `ItemCrafted`, and the cue.

## Skip animation (was `skip_fab_anim` until round 9)

The softer option: the confirmation window stays (count picker for gadgets, confirm), only the wait after the confirm goes. After the confirm, `HideForConfirm` starts the popup's success Animator, whose end event `NotifyAnimatorFadeOutComplete` fades out and returns to the content (`ReturnToContent`) and whose other event plays `PlaySuccessAudioCue` (details: `re/fabricator.md`).

What the module does: a detour on `HideForConfirm`: the original first, then, with the option on and only when the Animator path was taken (`_isClosing` set, `_successAnimator` alive, `_transitionFader` set; `NotifyAnimatorFadeOutComplete` throws a NullReferenceException without them), `PlaySuccessAudioCue` and `NotifyAnimatorFadeOutComplete`. The Animator is disabled in the same frame it was triggered, so nothing of it is drawn and its events never fire (no double cue, no second `FadeOutComplete`); `Show` resets it next time. Works for upgrades and gadgets. With Instant, buyable items never reach the window, so this only matters for what still goes through it. Log: `skipped the Fabricator's success animation` (first 10).

Not seen in game: whether the fader's own fade-out is short enough to feel instant, and that the Animator starts clean on the next purchase.

## Vacpack shooting (`vac_always_fast`, default off; `vac_shoot_mult`, 1x-5x, default 1x)

"Check whether the game already speeds up the vac when aimed at an input." It does: `VacuumItem.Update` shoots once per `ShootCooldown / GetShootSpeedFactor(inVac)`, and the factor is the largest `IVacShootAccelerator` (silo / market / fuel / resource ports) in the vac cone, whose `VacAccelerationHelper` ramps an input from 1x to 3x over 4 s and drops back 1 s after the last item (details: `re/vacpack.md`).

What the module does: a detour on `GetShootSpeedFactor`: `f = original`; "Fast vacpack shooting everywhere" -> `f = max(f, ramp)`, then `f *= multiplier`. The ramp is the input ports' own curve, `min(1 + 0.5 * t, 3)`, with `t` the seconds since the first shot of the current burst; a burst ends when no shot came for 1 s (the helper's Duration; or `ShootCooldown` + 0.1 s if that is longer, since the detour runs once per shot), so the next shot starts at 1x again. The clock is the real-time `steady_clock` (a pause longer than 1 s ends the burst). At an input the game's ramp and ours run side by side and the larger wins; the multiplier stacks on top (at full ramp and 5x: 15x).

**Round 7 report (2026-10-24), fixed:** "vacpack speed is always enabled, not the slime rancher ramp up". The first version used `max(f, 3)`: full speed from the first shot. Now it ramps.

A second detour on the private `Expel(float shootAudioVolume)` (0xA66C00, found by its parameter type) keeps the volume at least 1/3 while either option is active (the game's own volume at 3x; at 15x the shots would be almost silent). The real rate is capped by the frame rate (one shot per `Update`). Log: `fast shooting: burst of N shots over T s, top factor F` when a burst ends (first 10).

Not seen in game: the feel at high multipliers (items pile up in front of the player), and whether anything else (achievements, the vac's animation) reads the factor.
