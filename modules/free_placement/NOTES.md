# free_placement

Lifts gadget-mode placement limits. Off by default; card on Misc > General.

## Placement path

RE: `re/fabricator.md` (GadgetItem placement path, IsPlacementValid, footprint rules, gadget-mode blockers,
GadgetModel rotation save, teleporter rotation chain), `re/unity.md` (folded `RaycastHit.get_normal`).

In short: `GadgetItem.UpdateFootprint` (every frame, and once more in `PlaceGadgetEvent`) writes
`_isFootprintVisible`, `_isGrounded` (the player), `_isPlacementBlocked` (`Gadget.IsOverlapping`) and
`_isPlacementValid` (`IsPlacementValid`: distance, stacking, slope, `IsCompletelyGrounded`);
`PlaceGadgetEvent` reads them plus `IsLinkedGadgetInRange`, then spends and calls the game's
`InstantiateGadget`. Gadget mode itself is refused in the ranch house and in `DisableGadgetModeTrigger`
volumes (`PlayerItemController.BlockGadgetMode` / `ToggleItem`). No zone ownership on placed gadgets.

## What the module does

- `UpdateFootprint` detour: on the main thread, swaps metadata values for the duration of the original call
  (restored by an RAII guard, also on exceptions): ignore_slope sets both slope thresholds to 0 (up to vertical,
  never overhangs); ignore_range raises `MaxValidPlacementDist` to the footprint ray length. A depth counter marks
  "inside UpdateFootprint" for the scoped hooks below.
- `Gadget.IsCompletelyGrounded` -> true while inside UpdateFootprint (ignore_slope).
- allow_upside_down (child of ignore_slope, default on): both thresholds become -90 (sin = -1, any normal).
  The footprint test is strict (`normal.y > sin(threshold)`), so an exact (0,-1,0) normal (flat underside of a box
  collider) would still fail; `RaycastHit.get_normal` is hooked and, only inside UpdateFootprint on the main thread
  with this option, returns y = -0.99999994 instead of -1 (same direction, x = z = 0). That getter is folded with
  `ParticleCollisionEvent.get_normal` (RVA 0x339E700, 66 direct callers), so the detour is a pass-through
  everywhere else. Nothing else filters downward faces: the footprint ray (`RaycastNonAlloc`) hits from any side,
  and the game's footprint rotation `LookRotation(player direction projected onto the surface, normal)` already
  puts the gadget's up along the normal; `PlaceGadgetEvent` instantiates with the placeholder's rotation, so the
  preview and the placed gadget match.
- Save round trip: `GadgetModel.PullBase` saves the transform's full euler angles (`PlacedGadgetV07.Rot`, a
  Vector3) and `GadgetModel.GetRot` rebuilds the rotation from all three, so upside-down gadgets load as placed.
- Teleporters (`GadgetTeleporterNode` anywhere in the held definition's prefab, one `GetComponentInChildren`
  per definition, cached, logged once) keep the vanilla slope limits for both ignore_slope and upside-down:
  `TeleportNetwork` passes the destination node's `eulerAngles` to `TeleportablePlayer.TeleportTo` ->
  `IPlayerController.set_Rotation` -> `KinematicCharacterMotor.SetRotation`, and `KFCCharacterController.
  UpdateRotation` keeps the motor's `_characterUp`, so the player would arrive tilted / upside down. (The first
  free_placement version let teleporters onto walls; this fixes that too.)
- Other gadget kinds checked by reasoning only: items they spawn (feeders, water taps, chicken cloner) fall with
  gravity from below the gadget; launch pads launch along the pad's up (downwards); gordo snares and drone
  stations use their transform as is. None of them read the grounded check after placement.
- `Gadget.IsOverlapping(float, LayerMask, bool)` -> false while inside UpdateFootprint and the mask equals the
  metadata's `GadgetOverlapLayers` (ignore_collision). The spawner callers are outside the scope.
- `IsInLinkedGadgetRange` -> true (ignore_range).
- allow_airborne: after the original, `_isGrounded` is forced true and `SetGadgetPlacementValidity(true)` is called
  again if the other conditions hold (PlaceGadgetEvent reads the fields after the call, so placement works).
- ignore_zones: `BlockGadgetMode` is skipped; `ToggleItem` clears an already stored blocker (null store, no write
  barrier needed). The ranch-house check is kept.
- rotation_step (0 = vanilla): the rotate flags are hidden from the original and restored after it; a press turns
  the placeholder by the step (`Transform.Rotate(0, step, 0)`, Self space = around its up), holding repeats after
  350 ms every 120 ms. The vanilla rotate keys and their HUD hints are reused, so no new hint is needed.

Kept: the stacking rule (stacked gadgets are linked to their base and picked up with it), the cost, the game's
own `InstantiateGadget`, the footprint ray length (reach), the layer-0 rule for surfaces, the ranch house.

## Not done

- Height offset: the placeholder sits under the footprint, whose position is set through `Rigidbody.set_position`
  each frame (applied at the physics step), so a transform offset would fight it; floating gadgets were left out.
- Straight overhead: the footprint hides while the hit is within `MinPlacementFootprintVisibleDist` measured
  horizontally (vanilla rule, also for the floor at your feet); step back a little to place on a ceiling.
- With ignore_collision off, the overlap test (with its floor tolerance) may report a ceiling gadget as blocked.
