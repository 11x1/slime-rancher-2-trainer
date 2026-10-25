# Player: character controller, movement, abilities, energy

`MP.SR` = `MonomiPark.SlimeRancher`. Tags: **IDA** = decompiled; **dump** = signature/offset only. Mod use: `modules/hl2_movement/` (Source-style movement, scroll-jump, autobhop), `modules/quick_travel/` (teleport, knockout), `modules/sandbox/player_extras.cpp`. Hooks: re/hooks.md.

Stack: `SRCharacterController : KFCCharacterController` implements the KCC `ICharacterController`. `KinematicCharacterSystem.FixedUpdate` 0x5C0600 -> `Simulate(float, motors, movers)` 0x5C11E0 -> `KinematicCharacterMotor.UpdatePhase1/2` -> callbacks into `UpdateVelocity`. All per-tick velocity shaping is in the virtual `Handle*` methods.

### KFCCharacterController (MonomiPark.KFC) : MonoBehaviour, ICharacterController
- methods (dump): `UpdateVelocity(ref Vector3, float)` 0x5BEF50 (per physics tick, unique: primary hook) dispatching to `UpdateVelocityForActiveAbilities` 0x5BEB70 and `UpdateVelocityWithExternalForces` 0x5BECC0; `HandleStablyGroundedVelocity(ref Vector3, float)` 0x5BCCF0; `HandleStablyGroundedDrag` 0x5BCCA0 (0x50 bytes); `HandleHorizontalAirborneMovement(ref Vector3, float, Vector3)` 0x5BC840; `ClampAirborneHorizontalVelocity(ref Vector3, float)` 0x5BBD40 (air speed cap: neuter for uncapped bhop); `HandleAirborneDrag(ref Vector3, float, Vector3)` 0x5BC310; `HandleGravity` 0x5BC3D0; `HandleGroundOnAirborneMovement` base 0x5BC500; `UpdateMovementAndLookVectors(Vector2, Quaternion)` 0x5BE720 (writes MovementVector, LookVector, InputVector); `get_IsGrounded` 0x5BF6D0; `PostGroundingUpdate` 0x5BDC40; `AddForce` 0x5BB9D0.
- fields: `_parameters` CharacterControllerParameters +0x20, `_motor` KinematicCharacterMotor +0x48, `AbilityBehaviors` IAbilityBehavior[] +0x50, `MaxHeight` +0x58, `LookVector` Vector3 +0x88, `MovementVector` Vector3 +0x94 (camera-planar wish dir), `_gravityMagnitude` Nullable<float> +0xA0, `GravityAmount` +0xA8, `BypassGravity` +0xAC, `InputVector` Vector2 +0xB0.

### CharacterControllerParameters (MonomiPark.KFC) : ScriptableObject — patch data (getters folded)
`_maxGroundedMoveSpeed` +0x18, `_stableMovementSharpness` +0x1C, `_orientationSharpness` +0x20, `_groundedDrag` +0x24, `_maxAirMoveSpeed` +0x28, `_airAccelerationSpeed` +0x2C, `_airMovementSharpness` +0x30, `_airDecelerationSharpness` +0x34, `_airborneDrag` +0x38, `_gravityMagnitude` +0x3C, `_maxExternalForceFrames` int +0x40, `_gravityAccumulationSharpness` +0x44, `_defaultMovementAndLookType` +0x48, `_abilityData` AbilityData[] +0x50.

### SRCharacterController (MP.SR.Player.CharacterController) : KFCCharacterController
- methods: `Update` 0xA7FEE0, `LateUpdate` 0xA7EB90, `HandleGroundOnAirborneMovement(ref Vector3, ref Vector3)` 0xA7E9F0 (override), `get_IsSprinting` 0xA80160, `get_CurrentEnergy` 0xA80050, `StartEnergyRegen` 0xA7F090, `StopEnergyRegen` 0xA7F230, `TryStartEnergyRegen` 0xA7F270.
- fields: `_energyResource` +0xE8, `_energyShouldRegen` +0x108, `_currentEnergy` UnitResource +0x110, `_propertyTracker` +0x118, `_characterInput` ICharacterInput +0x148.

### UnitResource (MonomiPark.UnitPropertySystem)
`_unitResourceValue` float +0x10, `OnValueChanged` +0x18. `set_Value(float)` 0x5CAB50 (unique; use it so the HUD updates); `get_Value` 0x516B10 (folded).

### KinematicCharacterMotor (KinematicCharacterController, stock KCC)
- fields: `MaxStableSlopeAngle` +0x44, `StepHandling` +0x50, `GroundingStatus` CharacterGroundingReport +0x98 (`FoundAnyGround` +0, `IsStableOnGround` +1 => **motor+0x99**, `SnappingPrevented` +2, `GroundNormal` +4), `LastGroundingStatus` +0xD8, `_attachedRigidbodyVelocity` +0x1A4, `CharacterController` +0x1C0, `BaseVelocity` Vector3 +0x1F8, `_mustUnground` +0x269.
- methods: `UpdatePhase1` 0x59D490, `UpdatePhase2` 0x59EA60 (don't hook), `ProbeGround` 0x59BD20, `ForceUnground(float time=0.1)` 0x598FD0 (call when jumping), `MustUnground` 0x59BC50, `get_Velocity` 0x5A0100. Grounding resolves in Phase1 before `UpdateVelocity`.

### SRCharacterInput (MP.SR.Player.CharacterController) : MonoBehaviour
- `PollInput()` 0xA80FF0: polls `_moveHorizontal` +0x20 / `_moveVertical` +0x28 / `_lookHorizontal` +0x30 / `_lookVertical` +0x38 InputEvents (`InputEvent.PollValue`), applies deadzone/sensitivity, writes `_movementVector` Vector2 +0x60, `_lookVector` +0x68; gated by `_inputEnabled` +0x98. `_characterController` +0x50. Jump and sprint are NOT read here (ability InputEvents).
- `get_MovementInput` 0xA812E0, `get_LookInput` 0xA81270.

### Abilities (MonomiPark.KFC / MP.SR.Player.CharacterController.Abilities)
- `AbilityData` (SO): `_startInputEvent` InputEvent +0x18, `_stopInputEvent` +0x20, `_useAlternativeInput` ScriptedBool +0x28, `_alternativeStartInputEvent` +0x30, `_alternativeStopInputEvent` +0x38, `_movementAndLookTypeOverride` +0x40, `_isConcurrent` +0x48.
- `AbilityBehavior<T>` (shared generic: call only): `PerformStartInputManually()` 0x1EC9B50, `RegisterInputEvents` 0x1EC9B90, virtual `StartInputEventPerformed`. Ability start is edge-triggered by the InputEvent `Performed` callback.
- **JumpAbilityBehavior** : SRAbilityBehavior<JumpAbilityData>: `StartInputEventPerformed(InputEventData)` 0xA78000 (sets the generic `StartAbilityEventReceived` on press; `Start()` sets `_jumpRequested`: see the hl2_movement section below), `CanStart()` 0xA77AF0 (jumpRequested, grounded or within `_coyoteTime`, ceiling clearance, `_recurrenceDelay` since landing), `UpdateVelocity` 0xA782E0, `OnGroundedStateChanged` 0xA77E90 (records `_landingTime`), `Start` 0xA78070, `Stop` 0xA78290, `ShouldStop` 0xA77FB0. Fields `_landingTime` Nullable<double> +0x68, `_inAirTime` +0x78, `_holdForce` +0x88, `_jumpRequested` bool +0x8C.
- **JumpAbilityData** : SRAbilityData: `_jumpUpSpeed` +0x60, `_sidewaysSpeedMultiplier` +0x64, `_backwardsSpeedMultiplier` +0x68, `_airborneForceDamping` +0x6C, `_minCeilingJumpHeight` +0x70, `_coyoteTime` +0x74, `_recurrenceDelay` +0x78, `_initialHoldVelocity` +0x7C, `_holdVelocityDamping` +0x80, `_verticalVelocityStopThreshold` +0x84, `_jumpStartCue` +0x88, `_jumpEvent` +0x90. The jump InputEvent = its `_startInputEvent` (+0x18); find the asset in `CharacterControllerParameters._abilityData`.
- **SprintAbilityBehavior**: `Update(float)` 0xA84A90 (energy drain after `EnergyDecreaseStartDelay`, inferred), `CanStart` 0xA844A0, `ShouldStop` 0xA84890, `Start` 0xA849B0, `CanApplyMultiplier` 0xA84480, `get_EnergyDecreasePerSecond` 0xA84FA0, `get_IsGroundedAndSprinting` 0xA85150. Fields `_energyDecreaseStartTime` double +0x68, `_freeEnergyActiveLastFrame` +0x70, `_player` PlayerModel +0x78, `_fullStopTimer` +0x80. Data: `_sprintMultiplier` +0x60, `_minSprintValue` +0x64, `_maxSprintValue` +0x68, `_fullStopTimer` +0x6C, `_energyDecreaseStartDelayDefinition` +0x70, `_energyDecreasePerSecondDefinition` +0x78, `_sprintEfficiency` +0x80.
- **JetpackAbilityBehavior**: `UpdateVelocity` 0xA76F00, `Update` 0xA77010, `CanStart` 0xA75860, `Start` 0xA76260, `OnJumpStart(object, EventArgs)` 0xA75FC0, `OnJumpStop` 0xA76050, `StartInputEventCanceled` 0xA76180; listens to the jump ability's events (`_firstJumpInput` +0xD0); engages on held jump after `_jumpToJetpackPause`. Data: `_jumpToJetpackPause` +0x64, `_hoverHeight` +0x68, `_maxUpwardThrustForce` +0x6C, `_energyPerSecond` +0x8C, `_initEnergyUsed` +0x90.
- `DebugFlyAbilityBehavior.CanStart` = folded `return false` (0x510250): the debug fly is dead code (re/debug-mode.md).

### Recipes (hl2_movement)
- Requesting a jump from code: the press flag is the generic `StartAbilityEventReceived` (hl2_movement reads it by name); `PerformStartInputManually()` triggers the start-input path. Find the behaviour in `KFCCharacterController.AbilityBehaviors` by class. Avoid `jumpInputEvent.Perform()`: it also reaches the jetpack listeners. (Older notes said to set `_jumpRequested`; `Start()` sets that itself.)
- Autobhop: jump `PollValue() > 0.5`, motor stable (+0x99), jump not active; `_recurrenceDelay` = 0. Current implementation: modules/hl2_movement.
- Scroll-jump: the scroll wheel cycles slots via `VacuumItem.NextSlot/PreviousSlot` (re/vacpack.md); suppress when `InputEventData.Source == POINTER`.
- Sprint energy: `UnitResource.set_Value` on `_currentEnergy` (+0x110).

## From the minimap, map_tools, quick_travel, wall_markers, collectibles notes

### TeleportablePlayer
- methods: `TeleportTo(Vector3 pos, SceneGroup group, Nullable<Vector3> rotation, bool overlayEnabled)` (4) - also calls `PlayerModel.SetCurrentSceneGroup(group)` (0x54EC90); without a rotation the controller keeps its velocity, with one it zeroes `BaseVelocity` (IDA)
- methods: `get_SceneGroup`
- gotchas: `Nullable<Vector3>` arg: call directly, not via runtime_invoke helper
- gotchas: calling TeleportTo during a respawn load resets the scene group back and broke the load (crash 2026-10, `Error while populating Land Plot Model`)
- mod: GT, QT (in-scene teleport reference)

### Layers
- methods: `Layers.GetLayersCollidedWith(layer)` - collision matrix mask; player layer 8
- mod: GT

### PlayerDeathHandler (global ns)  -- knockout flow
- methods: `OnDeath(source, go, stack)` (3) - returns if `_isPlayerDead`; sets it; boss-fight reset (ammo kept then); analytics; music suppressor; `_screenFadedCue`; `unlockWorldTime = max(HoursTilDawnOnDeath ? GetNextDawnAfterNextDusk() : HoursFromNow(HoursLostOnDeath), now + MinDeathTime)`, `MinDeathTime = (_delayTime + 2*_fadeTime)*60` game s; `LockOnDeath.Lock(unlockWorldTime)` (locks controller/items, mutes SFX); `ResetPlayer(clearOutAmmo)` (`VacuumItem.DropAllVacced`, `AmbianceDirector.ExitAllLiquid`, `SpendCurrency(PctCurrencyLostOnDeath*currency)`, health max, rad 0, energy max, ammo cleared/reduced + radiant slimes lost if clearOutAmmo); `TutorialDirector.SuppressTutorials`, `_deathEvent`, `_damageDeathEvent`; `DeathObscurer.StartObscuring(true, _fadeTime, b__0)` (IDA)
- chains: `b__0` -> `TeleportNetwork.Teleport_Death(player, _delayTime, b__1)` -> target `GetDefaultWakeUpDestination()` (else default group spawn). In ranch (`SceneLoader.IsCurrentSceneGroupDefault()`): queue `EnqueueLoadDeathLoadingScene` ("Setup For Death Transition", `_deathLoadingScene`, `LoadingScreenView.Init(DEATH)`) -> "Teleport Player" (TeleportTo wake-up) -> `EnqueueDelaySeconds("Wait For Min Delay On Death", _delayTime)` -> "Before Death Screen Close" (b__1) -> `EnqueueUnloadDeathLoadingScene` ("Notify Exit Death Loading Animation", "Wait for Death Loading Outro" 1.0 s, unload). Elsewhere: `LoadSceneGroup(defaultGameplaySceneGroup, {TeleportPlayer, PauseGame, DEATH, OnSceneGroupLoadedPhase2 = b__1})` (real load, `_delayTime` unused)
- chains: `b__1` opens `RanchHouseUI` with `RanchHouseOptions{SleepType = UNTIL_WORLD_TIME, TimeValue = unlockWorldTime, AfterSleepAction = b__2}` -> `Sleep` -> `SleepUntil`: `TimeDirector.Unpause`, fade, `LockOnDeath.LockUntil(unlockWorldTime, 0, cb)` -> `TimeDirector.FastForwardTo` (the time skip) -> `LockOnDeath.Update` at `HasReached`: unlock, `PlayerItemController.ResetAfterDeath`, `AutoSaveDirector.SaveGameAndFlush`, cb -> `SleepUntil.b__0`: `ForceFastForwardEnd`, fade-in, `TimeDirector.Pause`, `_sleeping = false`, music, `b__2` -> deregister suppressor, `_isPlayerDead = false`, `UnsuppressTutorials`, `StartObscuring(false, _fadeTime)`
- chains: house screen stays open: `Close()`/Esc -> `OnCloseImpl` (door cue, `VacuumItem.ClearVac`, `ResetPlayerLocation`) -> `ResetPlayer(false)` + `Teleport_ResetPlayer` (in ranch: instant TeleportTo, closes display)
- fields: `_isPlayerDead` (ScriptedBool, `get_Value`), `_fadeTime`, `_delayTime`
- gotchas: unique bodies for OnDeath/StartObscuring/Teleport_* (checked against Cpp2IL RVA index)
- gotchas: real-time time skip speed = serialized `FfSecsPerGameDay`
- mod: QT (hooks OnDeath, StartObscuring), GT/MT (knock-out check)

### DeathObscurer (global ns)
- methods: `StartObscuring(bool, float fadeTime, Action)` (3) - fade to black / back; keep fade > 0 (1/fade)
- mod: QT

## From the slime_dj, spawn_scaling, hl2_movement, quick_start, free_placement, misc_tweaks, plot_upgrades, drone_price_limit notes

### KFCCharacterController.UpdateVelocity 0x5BEF50 - disassembled structure (dumpbin; adds vtable slots)
```
if (_motor == null) <init>
if (_motor.GroundingStatus.IsStableOnGround)   // byte [motor+0x99]
    HandleStablyGroundedVelocity(ref v, dt)     // vtable slot 0x308
    HandleStablyGroundedDrag(ref v, dt)         // 0x318
else
    HandleHorizontalAirborneMovement(ref v, dt, MovementVector)  // 0x328
    HandleAirborneDrag(ref v, dt, MovementVector)                // 0x338
    HandleGravity(ref v, dt)                                     // 0x348
    ClampAirborneHorizontalVelocity(ref v, dt)                   // 0x358
foreach (a in AbilityBehaviors) if (a.IsActive) a.UpdateVelocity(ref v, dt)
tail-call UpdateVelocityWithExternalForces(ref v, dt)
```
- gotchas: vtable slots map onto the dump's method tokens in order. `SRCharacterController` overrides none of these except `HandleGroundOnAirborneMovement`. Handle* args: MovementVector passed by pointer to a copy, dt in xmm2. Player filter = exact class `SRCharacterController`.
- chain: `KinematicCharacterSystem.FixedUpdate -> KinematicCharacterMotor.UpdatePhase2 -> ICharacterController.UpdateVelocity(ref BaseVelocity, dt)`

### Tick rate / jump timing (disasm + perf log)
- KCC runs the player at a fixed **40 Hz** (dt 0.025): `hook:hl2.UpdateVelocity` 400 calls per 10 s (log)
- `KFCCharacterController.BeforeCharacterUpdate` 0x5BBD00 calls `UpdateAbilities`: the jump starts in the same KCC tick before UpdateVelocity, but before ground probing, so `JumpAbilityBehavior.CanStart` sees the previous tick's grounding
- `CanStart` 0xA77AF0: base `!IsActive && StartAbilityEventReceived`; ceiling check; grounded: `_landingTime + _recurrenceDelay <= now`; airborne: coyote via `_inAirTime` (only if `_landingTime` set, i.e. never right after a jump)
- **(corrects the older notes)** `StartAbilityEventReceived` (generic `AbilityBehavior<T>` field) is set on press (`StartInputEventPerformed`, ignored while the jump is active), cleared on release (`StartInputEventCanceled`) and by `Start()`; `Start()` 0xA78070 sets `_jumpRequested` and writes no velocity. A press during the falling half of a jump is buffered while held.
- ability stops when `velocity.y <= _verticalVelocityStopThreshold` (`ShouldStop` 0xA77FB0) or on landing
- vanilla consequence: landing tick N with friction, CanStart fails N+1, N+2 (`_recurrenceDelay` 0.07 s), passes N+3 -> 3 friction ticks per hop (~27 % speed loss per hop at 40 Hz)
- `JetpackAbilityBehavior.UpdateVelocity` 0xA76F00 writes only `velocity.y`; `Update` only `AddForce(Vector3.up/down)`; `Start` sets `BypassGravity`; no horizontal component in 1.3.0
- `HandleAirborneDrag` 0x5BC310: horizontal-only (x and z divided by `1 + _airborneDrag*dt` when that axis of the movement input is 0)
- `UpdateVelocityWithExternalForces` 0x5BECC0: adds queued forces * dt, no clamp

### KCC collisions / slopes (disasm)
- `KinematicCharacterMotor.HandleVelocityProjection(ref v, normal, stableOnHit)` 0x599AB0 (virtual, called via vtable from `InternalHandleVelocityProjection`; rcx this, rdx &v, r8 &normal, r9b stable), stock KCC 3.x: grounded and not MustUnground: stable hit -> tangent * |v|; unstable -> projection following the ground plane. Airborne: stable hit -> `ProjectOnPlane(v, up)` then tangent * |that| (**vertical speed dropped on landing**); unstable -> `ProjectOnPlane(v, n)` (= ClipVelocity overbounce 1)
- fields: `_mustUnground` 0x269, `_mustUngroundTimeCounter` 0x26C
- `IsStableOnNormal` 0x59B9B0: `Vector3.Angle(up, n) <= MaxStableSlopeAngle` (motor+0x44); steeper ground = FoundAnyGround without IsStableOnGround -> airborne branch
- `ForceUnground(time)` 0x598FD0 (16 bytes): `_mustUnground = true; _mustUngroundTimeCounter = time`; time 0 = next tick only
- `SRCharacterController.HandleGroundOnAirborneMovement` 0xA7E9F0: calls the KFC version (KCC anti-air-climb projection of added air velocity while FoundAnyGround), then zeroes `v.y` for one ability state (an ability's IsActive at +0x30 and a threshold at +0x68; kept with the jetpack on)
- `KFC.HandleGravity` 0x5BC3D0: `GravityAmount = min(GravityAmount + g * _gravityAccumulationSharpness, g); v += down * GravityAmount * dt`; g = `_gravityMagnitude` = 17 m/s^2 in 1.3.0; reset point of GravityAmount not traced
- `JumpAbilityBehavior.UpdateVelocity` 0xA782E0: take-off tick (`_jumpRequested`): `ForceUnground()`, vertical replaced by `_jumpUpSpeed` (x `_sidewaysSpeedMultiplier` / `_backwardsSpeedMultiplier` blended by input direction; 1 forward / no input), `_holdForce = _initialHoldVelocity`. Later ticks while active: `v.y /= 1 + dt * _airborneForceDamping`; unless released (bool at behaviour+0x19, assumed), `_holdForce /= 1 + dt * _holdVelocityDamping; v.y += _holdForce` (per-tick add, not * dt)
- data: `_jumpUpSpeed` 15 m/s; ballistic 15^2/(2*17) = 6.6 m is only an upper bound; JumpAbilityData values live in the asset (hl2_movement logs an estimate + measurement)
- vanilla `_maxGroundedMoveSpeed` = 10.00 m/s (log)

### Input (additions)
- `InputEvent.Cancel(InputEventData)` 0x9DF990 (1 arg) - hl2_movement hook with `StartInput` 0x9DFDE0 (press/release fallback)
- `InputEvent.PollValue` Func<float> of an unbound event = `InputEvent.DefaultPoll` (constant 0); invoke natively via the delegate class's own `Invoke` MethodInfo
- `GadgetFavoritesBar` (MonomiPark.SlimeRancher.UI.Gadget) `NextSlot` 0x75E180 / `PreviousSlot` 0x75E500 (1 arg) - gadget-mode wheel; hl2_movement hooks them like VacuumItem's
- PulseWave ability never moves the player
- energy max: real max is a PropertyDefinition via `UnitPropertyTracker` (not resolved)

## From the sandbox, sell_upgrades, collectibles, instant_build notes

### PlayerState / PlayerModel / teleport
- see re/vacpack.md PlayerState (health/energy/Damage); `SceneContextUtility.TryGetPlayerState`; `TeleportablePlayer.TeleportTo(pos, rot)` (zeroes velocity); `PlayerDeathHandler` (a source killing directly through it bypasses PlayerState.Damage - unverified)
- placement used by sb spawner: main camera position + forward flattened; `Physics.Raycast(origin, dir, out hit, 3.1, DefaultRaycastLayers, QueryTriggerInteraction.Ignore)`
