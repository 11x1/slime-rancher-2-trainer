# misc_tweaks module notes

Small speed-ups. Module key `misc_tweaks`, on by default, but every option is off (or 1x) by default, so it changes nothing until one is turned on. Options tab: Misc > General, its own card (groups Fabricator and Vacpack). Game 1.3.0; everything is resolved by name, the RVAs are for re-checking after an update (Cpp2IL dump + IDA). Each feature degrades on its own: a missing symbol drops that feature's settings and hook (log: `fast Fabricator purchases NOT available: ...` / `vacpack shooting options NOT available: ...`); the module fails only when both are missing.

## Fast Fabricator purchases (`fast_fabricator`, default off)

"It takes a bit right now, like completely remove the game's popup maybe?" What the game does (IDA):

- `FabricatorUI.OnItemActivated(IFabricatableItem)` (0x826560), the item grid's activate (click / Fabricate / Upgrade input): returns unless `SpendableDirector.HasSpendables(cost, 1)`; then `_confirming = true`, disables `_content`, hides the key bar, `FabricatorConfirmPopup.Show(item)`, focus to the popup, a cue.
- `FabricatorConfirmPopup.Show` (0x801150): title / name / icon, `_countToMake = 1`, `_maxCountCanOwn` (99, or the refinery space for a gadget), resets the success animator, `CheckCanFabricate(_countToMake)` for the count display. No hold-to-confirm anywhere (`SRButton` has no hold; `ShouldSkipAnimations` compiles to `return false`).
- Confirm = `ISubmitHandler.OnSubmit` (0x8016B0): `IFabricatableItem.CheckCanFabricate(_countToMake)`; not NONE -> `PlayFabcricationDenied`; else a start cue, the `Confirmed` event, `HideForConfirm` (0x800A50): `_isClosing`, the success `Animator` (trigger `SUCCESS`, `IS_UPGRADE`), whose end calls `NotifyAnimatorFadeOutComplete` -> `FadeOutComplete` -> `FabricatorUI.HandleConfirmationFadeOutComplete` -> `ReturnToContent` (0x826CB0: `_confirming = false`, content and key bar back). That animation is the wait.
- `Confirmed` -> `FabricatorUI.OnConfirmationConfirmed(item, count)` (0x826220): pinned-recipe update, `IFabricatableItem.FabricateAndSpendCost(count)`, `UpdateInputLegendBasedOnItemUpgrade`, `FabricatorContent.ItemCrafted(item)`.

What the module does: a detour on `OnItemActivated`. With the option on, for a `GadgetFabricatableItem` / `PlayerUpgradeFabricatableItem` and `_confirming` false: `CheckCanFabricate(1)` (the popup's own check: blueprint, costs, max count, fully upgraded); NONE (0) -> `OnConfirmationConfirmed(item, 1)` (the game's purchase, normal cost) and the popup's success cue (`FabricatorSoundBank.PlayConfirmationConfirmedForUpgrade` / `ForGadget`, as `PlaySuccessAudioCue`); the original is skipped. Anything else (blocked, another item type, a fault in the guard) runs the original: the game's popup with its reason, or nothing when it cannot be afforded (as before). Gadgets are made one per press (the popup's amount picker is skipped). Log: `fast purchase: <class> (upgrade | gadget x1)` for the first 20.

Not seen in game: the content / key bar state after a purchase without the popup (the popup path disables and re-enables them; this path never touches them), the grid's refresh after `ItemCrafted`, and the cue.

## Vacpack shooting (`vac_always_fast`, default off; `vac_shoot_mult`, 1x-5x, default 1x)

"Check whether the game already speeds up the vac when aimed at an input." It does (IDA):

- `VacuumItem.Update` (0xA6B750), shoot mode: when `Time.fixedTimeAsDouble >= _nextShot`: `f = GetShootSpeedFactor(inVac)`, `Expel(1 / f)` (the argument is the shot's audio volume), `_nextShot = now + ShootCooldown / f`. At most one shot per frame.
- `GetShootSpeedFactor(HashSet<GameObject>)` (0xA67940, private): the largest `IVacShootAccelerator.GetVacShootSpeedFactor()` among the objects in the vac cone, else 1. Implementations: `SiloCatcher` (silo, refinery and other storage ports), `ScorePlort` (market), `DroneFuelDepositor`, `ResourceDepositor`; each returns its `VacAccelerationHelper.Factor`.
- `VacAccelerationHelper`: `CreateInput()` (0x99A530) MinFactor 1, MaxFactor 3, Speed 0.5, Duration 1 s; `CreateOutput()` (0x99A590) MaxFactor 1.75. `Factor` = `min(Min + (now - timeBegin) * Speed, Max)` while `now < timeEnd`, else Min; `OnTriggered` (each item fed) keeps `timeBegin` and pushes `timeEnd` 1 s ahead. So feeding an input ramps the shoot rate from 1x to 3x over 4 s and drops back 1 s after the last item.

What the module does: a detour on `GetShootSpeedFactor`: `f = original`; "always fast" -> `f = max(f, 3)` (the input ports' top factor, a constant from the IDA read of `CreateInput`); then `f *= multiplier`. Both stack with the game's ramp (at an input with always fast and 5x: 15x). A second detour on the private `Expel(float shootAudioVolume)` (0xA66C00, found by its parameter type) keeps the volume at least 1/3 while either option is active (the game's own volume at 3x; at 15x the shots would be almost silent). The real rate is capped by the frame rate (one shot per `Update`).

Not seen in game: the feel at high multipliers (items pile up in front of the player), and whether anything else (achievements, the vac's animation) reads the factor.
