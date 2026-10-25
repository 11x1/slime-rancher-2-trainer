# hl2_movement notes

Half-Life 2 (Source SDK 2013) movement for the SR2 player, with uncapped bunnyhopping and HL2's accelerated back hopping (ABH). Game 1.3.0. Everything is resolved by name at runtime. The RVAs below are only for cross-checking against `re/dump`.

## Files

| File | What |
|---|---|
| `source_movement.h/.cpp` | Pure port of the movement math with no game dependencies: Friction, Accelerate, AirAccelerate (30 u/s cap), ClipVelocity, CheckVelocity, the HL2 jump with the four boost modes (`BoostMode`), ramp helpers (`LandingClip`, `IsWalkable` 0.7, `MovingUpRapidly` 140 u/s, `GroundClipZ`), `JumpImpulseForHeight` / `DiscreteJumpApex`, and `SelfTest()`. |
| `module.cpp` | Hooks (all mod logic inside `DetourGuard`s), settings, sprint/energy, scroll-wheel jump, autobhop, jetpack guard, ramp physics, jump height, safety, fast-movement readout, jump measurement. |

`SelfTest()` runs in `Setup()` in Debug builds and logs one line. It was also built and run standalone with `cl /W4 /WX` (120 Hz substeps, as in game at 40 Hz x 3):
`strafe-jump 15 hops: 1762 uncapped / 311 vanilla clamp / 1762 uncapped+ABH / 4951 ABH both ways u/s; back-hop 8 hops from 300: 300 / 3500 / 3500 / 3500 u/s; forward-hop 8 hops from 300: 300 / 285 / 300 / 3500 u/s; 30deg down-ramp landing from 64 u: 300 -> 442 u/s (old model 260, hop 442); 30deg up-ramp at 1000: launched, z 431 u/s; 60deg surf 2 s at 1000: along 1000, total 1001 u/s, 239 contacts, 0 ground ticks (30deg slide: 0 u/s); jump 2.00 m at 600 u/s^2: apex 1.999 m, air 0.73 s; all checks passed` (the strafe sim applies sv_maxvelocity 3500 per axis, hence 4951 on the diagonal).

Ramp/jump checks: falling 64 u (3.4 m) onto a 30 degree down slope at 300 u/s lands with 442 u/s ground speed (the old horizontal-only model gave 260) and a hop on the landing tick keeps all of it; a flat landing keeps 300 exactly; a fast landing on a 30 degree up slope is launched (clipped z 431 > 140); 2 s on a 60 degree ramp (normal.z 0.5) never becomes ground, keeps the 1000 u/s along the ramp exactly and only gains from gravity, while the same slide on a walkable 30 degree slope is ground and friction stops it; the jump apex matches the height setting within 1 % for 1/2/4 m at gravity x0.5/x1/x2 in the 40 Hz tick model.
The back-hop sim travels backwards at 300 u/s with the view facing away and no keys at take-off: uncapped keeps 300, the three ABH modes go 300 -> 315 -> 345 -> 405 -> 525 -> 765 -> 1245 -> 2205 -> 3500 (sv_maxvelocity 3500 in the test). The forward-hop sim is its mirror (travelling forwards at 300 u/s, view along the velocity, no keys): uncapped and Uncapped + ABH keep 300, vanilla clamp drops to 285, ABH both ways matches the back hop on every hop (checked hop by hop).

## Game facts the design relies on

RE: `re/player.md` (UpdateVelocity structure and vtable slots, 40 Hz tick, jump timing, KCC velocity
projection, HandleGravity, JumpAbilityBehavior.UpdateVelocity), `re/player.md` (offsets).

- `KFCCharacterController.UpdateVelocity`: grounded -> HandleStablyGroundedVelocity + Drag; airborne ->
  HandleHorizontalAirborneMovement, HandleAirborneDrag, HandleGravity, ClampAirborneHorizontalVelocity;
  then active abilities' `UpdateVelocity`, then external forces (no clamp). `SRCharacterController`
  overrides only `HandleGroundOnAirborneMovement`. KCC keeps collision, grounding, steps, slope projection.
- KCC ticks the player at a fixed **40 Hz**; each tick is split into `ceil(dt / 0.01)` equal substeps.
- SR2 starts a jump in `BeforeCharacterUpdate` of the same tick, before UpdateVelocity but with the previous
  tick's grounding. Vanilla `_recurrenceDelay` 0.07 s gave 3 friction ticks per Space hop (~27 % speed lost
  per hop): the main "feels capped". Fixed by `_recurrenceDelay` = 0 and the landing-tick friction skip.
- The jetpack is vertical-only in 1.3.0; `HandleAirborneDrag` is horizontal-only.
- KCC drops the vertical speed on an airborne stable hit (landing) and keeps |v| when re-tilting onto a new
  slope; `ForceUnground(0)` = ungrounded for the next tick only.
- Vanilla jump: `_jumpUpSpeed` 15 m/s with damping + hold force, gravity 17 m/s^2 ramped by
  `_gravityAccumulationSharpness`. Data values live in the JumpAbilityData asset, so the vanilla apex is
  computed at runtime (log "estimated vanilla apex", tap and held) and measured in gravity mode "Vanilla
  SR2" (log "measured jump", first jump per mode).

## Resolved names (all in Assembly-CSharp)

**Classes**
- `MonomiPark.KFC.KFCCharacterController`
- `MonomiPark.SlimeRancher.Player.CharacterController.SRCharacterController`. The player filter is an exact class match on `this`.
- `KinematicCharacterController.KinematicCharacterMotor`
- `KinematicCharacterController.CharacterGroundingReport` (struct)
- `...CharacterController.Abilities.{JumpAbilityBehavior, JumpAbilityData, SprintAbilityBehavior, JetpackAbilityBehavior}`
- `MonomiPark.KFC.AbilityData`
- `MonomiPark.SlimeRancher.Input.{InputEvent, InputEventData}`
- `MonomiPark.UnitPropertySystem.UnitResource`
- Optional: `MonomiPark.KFC.CharacterControllerParameters`, `MonomiPark.SlimeRancher.Player.PlayerItems.VacuumItem`, `MonomiPark.SlimeRancher.UI.Gadget.GadgetFavoritesBar`

**Fields**
- KFC: `_motor`, `AbilityBehaviors`, `LookVector`, `MovementVector`, `<InputVector>k__BackingField`, `<BypassGravity>k__BackingField`, and optionally `_parameters`
- SRCC: `_currentEnergy`
- Motor: `GroundingStatus`, `BaseVelocity`, `_transientPosition`; optional `MaxStableSlopeAngle`, `_mustUnground`, `_mustUngroundTimeCounter` (ramp physics)
- JumpAbilityData, optional (vanilla apex estimate): `_airborneForceDamping`, `_initialHoldVelocity`, `_holdVelocityDamping`, `_verticalVelocityStopThreshold`; Params optional `_gravityAccumulationSharpness`
- GroundingReport: `FoundAnyGround`, `IsStableOnGround`, `GroundNormal`. IL2CPP reports value-type field offsets including the 0x10 object header, so the module subtracts `2*sizeof(void*)`. This gives +0x99 for IsStableOnGround, which matches the disassembly.
- `AbilityBehavior<T>` fields, resolved on the concrete classes: `AbilityData`, `<IsActive>k__BackingField`. Unknown abilities are resolved lazily per class.
- AbilityData: `_startInputEvent`, `_alternativeStartInputEvent`
- JumpAbilityData: `_recurrenceDelay`, `_jumpUpSpeed`
- UnitResource: `_unitResourceValue`
- Params (optional): `_maxGroundedMoveSpeed`, `_gravityMagnitude`
- InputEventData: `Source`, `_vecValue`. These are used only to check the layout. The size is also checked through the `il2cpp_class_value_size` export when present.

**Methods called**
- `KinematicCharacterMotor.ForceUnground(float)`
- `UnitResource.set_Value(float)`. It fires `OnValueChanged`, so the HUD follows.
- `KinematicCharacterMotor.SetPosition(Vector3,bool)`, optional (fall recovery)
- `SRCharacterController.HandleGroundOnAirborneMovement(ref,ref)`, optional, with the KFC version as fallback

## Hooks

| Hook (name, argc) | 1.3.0 RVA | Behaviour when HL2 movement applies |
|---|---|---|
| KFC.UpdateVelocity(2) | 0x5BEF50 | Tick wrapper: player filter, grounded state, substeps (`ceil(dt/0.01)` equal steps, max 10), wish dir/speed, sprint state, jump/sprint held poll, `_recurrenceDelay` = 0, landing jump queue. Post-tick: take-off vertical guard, jetpack horizontal guard, safety speed limit, CheckVelocity (sv_maxvelocity), limiter counters, fall-through recovery bookkeeping. |
| KFC.HandleStablyGroundedVelocity(2) | 0x5BCCF0 | Source Friction + Accelerate in substeps. The result is horizontal velocity, with the vertical component chosen so the velocity is tangent to the ground normal (horizontal speed preserved exactly). Ramp physics: the starting horizontal velocity is the landing clip on the first grounded tick, last tick's horizontal velocity when KCC only re-tilted it onto another slope, KCC's velocity otherwise; a clipped vertical above 140 u/s leaves the ground instead (see Ramp physics). On a take-off tick (an SR2 jump pending, scroll latch, or autobhop key held) and on the landing tick of a queued SR2 jump (`StartAbilityEventReceived` set, within the first 2 grounded ticks) it runs AirMove without friction, like FullWalkMove after CheckJumpButton. With the jetpack active: AirAccelerate only, vertical untouched. Synthetic jumps (wheel/autobhop) happen here. |
| KFC.HandleStablyGroundedDrag(2) | 0x5BCCA0 | Skipped. |
| KFC.HandleHorizontalAirborneMovement(3) | 0x5BC840 | Source AirAccelerate in substeps. The added velocity goes through `HandleGroundOnAirborneMovement`, which keeps SR2's no-wall-climb projection, except with ramp physics while touching a surface too steep to stand on (surf). |
| KinematicCharacterMotor.HandleVelocityProjection(3) | 0x599AB0 | Optional. Player motor only (pointer compare), ramp physics on, HL2 tick, no jetpack, and airborne or forced off the ground: `ClipVelocity(v, n, 1)` for every hit instead of KCC's stable-landing projection (which drops the vertical speed). Grounded hits go to KCC unchanged. |
| KFC.HandleAirborneDrag(3) | 0x5BC310 | Skipped (it is horizontal-only drag). |
| KFC.HandleGravity(2) | 0x5BC3D0 | `v.y -= sv_gravity * gravity_multiplier * scale * tick dt`. Vanilla when the jetpack is active, `BypassGravity` is set, or vanilla gravity mode is selected. |
| KFC.ClampAirborneHorizontalVelocity(2) | 0x5BBD40 | Skipped. This is SR2's air speed cap. |
| JumpAbilityBehavior.UpdateVelocity(2) | 0xA782E0 | Always calls the original, so SR2's state machine keeps running. On the first tick after `Start`, the velocity is replaced by HL2's jump: flattened pre-jump velocity, z += the impulse for "Jump height (m)" (or SR2's vertical in vanilla mode), plus the jump boost (0.5, or 0.1 when sprinting) per `jump_boost_mode`. The jump is refused unless grounded at tick entry or within 0.15 s coyote time with no jump since. On other ticks SR2's horizontal change is stripped, and in HL2 mode its vertical hold force is too. |
| JumpAbilityBehavior.Start(0) | 0xA78070 | Calls the original, then bumps a serial so the next tick knows a take-off is coming. A pending start is valid for one tick. |
| SprintAbilityBehavior.Update(1) | 0xA84A90 | When HL2-style sprint is on, undoes any energy decrease that happened inside it (SR2's sprint drain). |
| InputEvent.StartInput(1) / InputEvent.Cancel(1) | 0x9DFDE0 / 0x9DF990 | Optional. Fallback "jump held" / "sprint held" tracking (start event StartInput = down, Cancel = up, jump stop event StartInput = up). Ignored once `InputEvent.PollValue` has reported a press (see below). |
| VacuumItem.NextSlot/PreviousSlot(1) | 0xA67D00 / 0xA68F80 | Optional. With scroll-wheel jump on and `data.Source` POINTER/UNKNOWN: latch a jump (per direction setting) and swallow the call, so the wheel never changes slots. Keyboard and gamepad sources pass through. |
| GadgetFavoritesBar.NextSlot/PreviousSlot(1) | 0x75E180 / 0x75E500 | Optional. Same as above for gadget mode. |

All hooks call the original unchanged when `this` is not the player, during an unknown active ability (debug fly; PulseWave is ignored because it never moves the player), or while the module is inactive. Every detour's mod logic runs in a `DetourGuard` (one per hook); the original is always called outside it. Replacing detours use the documented replace pattern (`if (guard.Run(...)) return; original(...)`). The old `JumpAbilityBehavior.StartInputEventPerformed` hook (it set "held" on every Performed, which could stick) is gone.

**Held state**: each tick the jump (and sprint) start InputEvents' `PollValue` (`Func<float>` field) is invoked natively through the delegate class's own `Invoke` MethodInfo (resolved by name, cached per class, inside `Try`). The poll becomes authoritative once it has returned > 0.5 (an unbound event polls `InputEvent.DefaultPoll`, constant 0); until then the StartInput/Cancel tracking is used.

## Behaviour summary

- **Sandbox noclip** (`modules/common/noclip_state.h`): while it is on, the `UpdateVelocity` detour runs the vanilla tick untouched and clears the fall-through reference, the ramp motor and the HL2 sprint state (modules/sandbox/NOTES.md "Noclip").

- **Units**: "Match vanilla walk speed" is on by default. It sets scale = vanilla `_maxGroundedMoveSpeed / normal_speed`. In 1.3.0 that is 10.00 m/s / 190 = **0.0526 m/u**. The manual `unit scale` (default 0.0254) is used when match is off or before the vanilla value is known. **Moving the unit scale slider turns matching off** (logged), and while matching is on a non-default manual scale is reported as ignored in the scale log line. (The user's log showed the slider dragged to 0.2 while matching stayed on, so the scale snapped back to 0.0526.) The effective scale and what it means in metres is logged at Info on discovery and on every change.
- **Vertical at the default scale**: speeds and gravity are scaled by the same factor (HL2 at SR2 scale). The jump is no longer HL2's 160 u/s (21 u = 1.1 m, which users found too low): it is set in metres by **"Jump height (m)"** (default **2.0 m**, range 0.5-6), with impulse `sqrt(2 g h) - g dt / 2` at the HL2 gravity in effect. The `- g dt/2` term corrects for KCC's per-tick integration (take-off tick moves with the full impulse, later ticks subtract gravity before moving; apex = (v0 + g dt/2)^2 / 2g), so the apex equals the setting exactly (self-test: 1.999 m for 2 m). **Gravity multiplier** (0.25-3, default 1) scales sv_gravity; the jump height is kept, the air time changes. Air time per hop at the defaults: 0.73 s (HL2: 0.53 s), so each hop covers more distance and the per-hop air-strafe gain is larger; the boost modes are unchanged horizontally. The scale log line prints gravity, impulse, apex and air time on every relevant change.

  | | HL2 mode at scale 0.0526 (defaults) | Vanilla SR2 1.3.0 |
  |---|---|---|
  | Normal / sprint speed | 10.0 / 16.8 m/s | 10.0 m/s max ground speed |
  | Walk speed | 7.9 m/s | n/a |
  | Gravity | 600 u/s² x 1 = 31.6 m/s² | 17 m/s², ramped up per tick by `_gravityAccumulationSharpness` |
  | Jump take-off | 206 u/s = 10.8 m/s (for 2.0 m) | `_jumpUpSpeed` 15 m/s, then damping + hold force |
  | Jump height / air time | **2.00 m** / 0.73 s (setting) | logged at runtime: estimate from JumpAbilityData, measurement in Vanilla SR2 mode |

  Gravity mode "Vanilla SR2" stays available (SR2's gravity, jump and hold/damping vertically, HL2 horizontally); users disliked it, so HL2 stays the default.
- **Ramp physics (Source)** (`ramp_physics`, default **on**; off = exactly the previous behaviour):
  - *Landing*: KCC drops the vertical speed when it lands (stable hit while airborne). The module stores the final velocity of every airborne tick and, on the first grounded tick, uses `ClipVelocity(preLanding, groundNormal, 1)` like Source's TryPlayerMove + SetGroundEntity (z then 0, horizontal kept). Falling onto a down slope turns fall speed into ground speed (ramp boost); landing on an up slope costs speed. This happens before friction, and the landing-tick jump / no-friction logic (queued Space jump, wheel, autobhop) uses the clipped speed, so a ramp landing + hop keeps the boost.
  - *Launch*: if the clipped velocity moves up faster than 140 u/s (NON_JUMP_VELOCITY), Source never grounds; the module keeps the clipped 3D velocity and calls `ForceUnground(0)` (one tick). On the ground, Source's StepMove takes z from the clipped slide move (`-(h.n) n.z`); above 140 u/s CategorizePosition drops the ground, which is what makes fast uphill running slide/launch (frictionless, losing speed to gravity) instead of grounding. Counted as "ramp launch" in the fast-movement readout. Not applied on a take-off tick (the jump wins).
  - *Grounded on slopes*: Source zeroes z before Friction/Accelerate and keeps the horizontal velocity on slopes (StepMove), so ground speed does not depend on the slope (downhill keeps its speed, uphill only costs speed through the launch rule and the landing clip). KCC instead re-tilts the velocity onto a new slope keeping its 3D length (horizontal speed changes by cos). When the new KCC velocity has the same length (0.5 %) and horizontal direction (0.9995) as last tick's final velocity, it is treated as that re-tilt and last tick's horizontal velocity is used; anything else (walls, external forces) uses KCC's velocity as before.
  - *Airborne clip / surf*: `HandleVelocityProjection` is hooked; while airborne (or forced off the ground) on HL2 ticks without the jetpack, every hit is `ClipVelocity(v, n, 1)` (Source), also on walkable slopes where KCC would drop the vertical speed. Surfaces steeper than the surf limit are not ground for KCC (airborne branch: no friction, HL2 gravity, air accelerate), so you slide along them. SR2's anti-air-climb projection of the air acceleration is skipped while touching such a surface (`FoundAnyGround && !IsStableOnGround`), because it removes exactly the strafe into the ramp that keeps you on it.
  - *Surf limit* (`surf_slope_limit`): "SR2" (default) = KCC's `MaxStableSlopeAngle` (logged once per motor; the game's walkable slopes stay walkable); "Source" = the player motor's `MaxStableSlopeAngle` lowered to 45.57 degrees (normal.z 0.7) while ramp physics is on, restored when off or on module disable.
- **Speeds**: walk 150 while the walk key is held (`walk_key`, unbound by default since Alt is the autobhop hold key; if both are bound to the same key the autobhop key wins, walk is disabled and a warning is logged once), normal 190, sprint 320. Wish = flattened `LookVector` forward/right × `InputVector`, clamped like CheckParameters.
- **Sprint (HL2-style, default on)**: active while the sprint input is held or SR2's sprint ability is active (toggle mode). It can't start below 10% of max energy and stops at 0. It drains `sprint_drain` % of max per second, only while horizontal speed > 1 u/s (HL2 skips the drain when not moving). SR2's own sprint drain is undone. Regen stays vanilla. Max energy is the highest value observed (floor 100).
- **Jump boost** (`jump_boost_mode`, default **Uncapped + ABH**): exact Source SDK 2013 `CheckJumpButton` HL2 block (confirmed against `src/game/shared/gamemovement.cpp` on GitHub): `flSpeedBoostPerc` 0.5 (0.1 sprinting), `flSpeedAddition = |forwardmove * perc|`, `flMaxSpeed = maxspeed + maxspeed * perc`, `if (flNewSpeed > flMaxSpeed) flSpeedAddition -= flNewSpeed - flMaxSpeed;`, sign flip for `forwardmove < 0`, added along the flattened view forward.
  - *Uncapped forward boost*: no clamp line (old default). Back hops neither gain nor lose from the boost.
  - *Vanilla HL2 (clamp, with ABH)*: the exact code. Forward hops stay near 1.5x max speed; above that the "boost" is negative along the view, so travelling backwards (view facing away, no key or W at take-off) speeds you up by `speed - flMaxSpeed` per hop (ABH, exponential up to sv_maxvelocity); holding S while moving forward does the same forwards (AFH).
  - *Uncapped + ABH*: uncapped while the horizontal velocity points forward relative to the view (dot >= 0), the exact vanilla formula while it points backwards. Same strafe-jump result as uncapped, same back-hop result as vanilla (self-test). No AFH (that needs the clamp while moving forward).
  - *Accelerated forward + back (ABH both ways)* (value 3, not the default): the exact vanilla formula, mirrored while the velocity points forward relative to the view (dot >= 0). Above `flMaxSpeed` the vanilla addition is `-(speed - flMaxSpeed)` along the view (it would slow a forward hop); this mode applies `+(speed - flMaxSpeed)` along the view instead, which is what ABH does along the backwards travel direction. So a forward hop facing where you go gains `speed - flMaxSpeed` per hop exactly like a back hop, exponential up to sv_maxvelocity, with no key or W held (holding S gives the same, it is vanilla AFH there). Below `flMaxSpeed` and while travelling backwards it is the unmodified vanilla formula (normal +95 jump from walk speed, vanilla ABH). Strafe-jumping also accelerates exponentially in this mode.
  - The default stays *Uncapped + ABH* (2).
- **Jetpack**: SR2's vertical thrust (vanilla gravity path, `BypassGravity`) only. Horizontal is Source AirAccelerate (30 u/s cap, `sv_airaccelerate`) on the current velocity, also if KCC reports ground while hovering. Any horizontal change made after our air step while the jetpack runs (abilities, external forces) is discarded and counted as a limiter. SR2 itself adds no horizontal speed (disassembly above); the speed you have when it engages is the take-off speed incl. the jump boost, kept because Source air has no drag.
- **Scroll-wheel jump**: a wheel tick sets a latch. It is consumed at the next KCC tick. On the ground it is a jump; in the air it is discarded (Source +jump semantics). Always single presses.
- **Space jumps**: SR2 decides (press edge). `_recurrenceDelay` is 0 whenever HL2 movement is on (Source has no re-jump delay; restored when the module is turned off). The landing tick of a queued press has no friction, so a press held into the landing hops with zero speed loss. Holding Space after a jump does not re-jump (the press flag is cleared by `Start`), so a release is required, as in Source.
- **Autobhop** (off by default): while the **autobhop hold key** (`autobhop_key`, default Alt / VK_MENU) is held, a synthetic jump happens on the first grounded tick of every landing (same path as the scroll-wheel jump: landing-substep take-off, no friction, jump boost per `jump_boost_mode` incl. ABH). The key is read with `GetAsyncKeyState` once per frame in the frame callback, only while the game window is foreground and the settings overlay is closed (`sr2::ui::overlay::IsOpen()`; all key polls, walk key included, read as released while it is open so typing in its text fields does not leak into movement), and latched for the KCC ticks (like the walk key). It is only read, never swallowed, so Alt+Tab / Alt+F4 are unaffected. Holding Space never auto-hops (Space = SR2 press-edge single jumps, release required, as in Source). With autobhop off, the key does nothing.
- **Fast-movement readout** (`speed_readout`, default on): once per second while the peak horizontal speed of that second is > 2x normal speed, an Info line with the speed in u/s and m/s (current and peak), grounded, jetpack, tick count and the limiters that touched the velocity that second: ground friction above max speed (late hop), safety speed limit, sv_maxvelocity, HL2 boost formula (clamp/ABH), wall projection, jetpack horizontal discarded, vanilla movement ticks, SR2 jump refused.
- **Safety**: optional horizontal speed limit (u/s). Fall-through recovery: airborne for more than 3 s and more than 150 m below the last grounded position while falling teleports to that position (+1 m) via `SetPosition` on the next main-thread tick. Moves of more than 25 m in one tick count as a game teleport and reset the reference.
- **Debug**: "Debug speed log" logs speed in u/s and m/s, grounded state, sprint, jump held (and whether the poll is in use), energy, and ticks/substeps once per second at Debug level.

## Verified vs assumed

**Verified**
- The `UpdateVelocity` structure and call order (disassembly, above).
- The `Handle*` argument shapes: MovementVector is passed by pointer to a copy, and dt is in xmm2.
- The grounding byte offset.
- All names, field names and argument counts exist in the 1.3.0 dump.
- The movement math (self-test).
- The build is /W4 clean (Release and Debug).

**Cross-checked with the old symbols notes (now re/player.md) (written by the IDA agent; mostly at dump level)**
- Same hook targets and offsets.
- Jump is press-edge: `StartInputEventPerformed` sets `_jumpRequested`, and `CanStart` gates on grounded/coyote and `_recurrenceDelay`.
- The jetpack listens to the jump ability's start/stop events.
- Sprint drain is inside `SprintAbilityBehavior.Update`. The older notes infer this from property names.
- `InputEventData` is 12 bytes, passed by hidden pointer, with POINTER = 1.
- re/ui.md advises against hooking `InputEvent.Perform*` because every input passes through it. This module hooks `StartInput`/`Cancel(InputEventData)` instead. Each detour is a pointer compare and then a pass-through.

**Assumed, to verify in game**
1. SR2 applies the jump impulse inside `JumpAbilityBehavior.UpdateVelocity` on the first tick after `Start`, KCC-example style with `_jumpRequested` (`Start` sets `_jumpRequested`; it writes no velocity itself - disassembly).
2. (Verified now) `Start()` runs in `BeforeCharacterUpdate` of the same KCC tick, before `UpdateVelocity`. Assumed: `OnGroundedStateChanged` records `_landingTime` on the landing tick, so `CanStart` passes on the next tick.
3. SR2's sprint drain happens inside `SprintAbilityBehavior.Update`. If it is elsewhere (for example a resource update definition), HL2 sprint would drain twice.
4. `InputEvent.PollValue` of the jump/sprint start event returns the button actuation (1 while held). If it never reports > 0.5 the module falls back to `StartInput`/`Cancel(InputEventData)` (assumed press/release) and the stop event.
5. The mouse wheel arrives in `VacuumItem.NextSlot/PreviousSlot` with `Source == POINTER` (enum values UNKNOWN=0, POINTER=1 from the metadata). NextSlot is taken to be wheel down and PreviousSlot wheel up. If the game reports wheel input as KEYBOARD, the wheel is neither suppressed nor turned into jumps.
6. `LookVector` is the camera look direction in world space, and `<InputVector>k__BackingField` is the raw move input (x = strafe, y = forward). If LookVector is zero, the fallback is `MovementVector`.
7. Energy max: SR2's real max is a PropertyDefinition evaluated through `UnitPropertyTracker`, which is not resolved. The highest observed energy (floor 100) is used instead.
8. Holding jump in the air may still start the jetpack (vanilla). The autobhop key (Alt) is separate and does not start it.
9. Ramp physics: the motor's `GroundingStatus.GroundNormal` on the landing tick is the slope landed on (a probe of the capsule bottom; on edges/creases it can be a blend). KCC's UpdatePhase1 was not disassembled; the landing clip uses the stored airborne velocity, so it does not matter whether Phase1 re-tilts the velocity again.
10. The slope re-tilt heuristic (same length and horizontal direction as last tick) also restores tiny glancing wall contacts; KCC still resolves the collision during the move.
11. Vanilla jump estimate: assumes `GravityAmount` starts at 0 at take-off and that the "released" flag is behaviour+0x19; the measured value from gravity mode Vanilla SR2 is authoritative. Module-off jumps are not measured (the hooks pass through untouched while the module is off).

## History

- **v1**: first port (uncapped boost, 100 Hz accumulator, input-event held tracking, no DetourGuard).
- **v2 (after the first in-game test)**:
  - Autobhop was "always on": the held flag could stick (StartInputEventPerformed hook set it on every Performed). Now polled from `InputEvent.PollValue` each tick; the Performed hook was removed.
  - Jump boost mode setting with ABH (`jump_boost_mode`, replaces the `jump_boost_clamp` bool, which is no longer read).
  - "Feels slow / capped": found 3 friction ticks per Space hop (SR2 jump timing + `_recurrenceDelay` 0.07 s, see above). Fixed with `_recurrenceDelay` = 0 and the landing-tick friction skip. Also: exact substeps per 40 Hz tick, airborne drag always skipped, match/unit-scale UI semantics, fast-movement readout with limiter counters. No other cap found: `ClampAirborneHorizontalVelocity`, grounded/air drag and SR2 ground accel are replaced; external forces and the jetpack have no clamp; safety limit off by default; sv_maxvelocity is the user's 100000.
  - Jetpack: AirAccelerate-only horizontal also on ground-hover ticks, horizontal changes after our step discarded while it runs.
  - All detours moved to `DetourGuard`.

- **v3**: jump boost mode 3 *Accelerated forward + back (ABH both ways)* (mirrored ABH for forward hops; self-test forward-hop sim). Autobhop moved from "hold Space" to its own hold key (`autobhop_key`, default Alt); `walk_key` default changed from Alt to unbound (saved values are kept; a clash is resolved in favour of autobhop with a one-time warning).

- **v4**: ramp physics (Source landing clip / ramp boost, launch above 140 u/s, horizontal speed kept on slopes, airborne ClipVelocity via `HandleVelocityProjection`, surf on unstandable slopes with the anti-air-climb projection skipped there, surf limit SR2/Source), "Jump height (m)" (default 2.0 m, exact apex at 40 Hz) and "Gravity multiplier", vanilla jump analysis (estimate from JumpAbilityData + measurement in Vanilla SR2 mode), self-test extended (ramp landing, launch, surf, jump apex).

**Not handled**: water and swimming, largo carry slowdown, surface friction per material (surfaceFriction = 1), ducking.
