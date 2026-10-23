# free_placement

Lifts gadget-mode placement limits. Off by default; card on Misc > General.

## Placement path (1.3.0, IDA)

All in `MonomiPark.SlimeRancher.Player.PlayerItems.GadgetItem`:

- `Update` (0xA494D0) -> `UpdateFootprint` (0xA48050) each frame while a footprint exists.
  `PlaceGadgetEvent` (0xA45410) calls `UpdateFootprint` once more, then reads the fields it wrote, in this order:
  `_isFootprintVisible` (silently return), `IsLinkedGadgetInRange` (NotInRange toast),
  `_isGrounded` (player grounded, via `IPlayerController`; GroundedCheck toast),
  `_isPlacementBlocked` (BlockedPlacement toast), `_isPlacementValid` (InvalidPlacement toast).
  All pass -> `SpendableDirector.HasSpendables` / spend, `GadgetDirector.InstantiateGadget(prefab,
  RegionRegistry.CurrentSceneGroup, placeholder pos, placeholder rot, true, false)`, `Gadget.OnPlaced`,
  `AddStackedGadget` when `_baseGadget` is set.
- Footprint visible: ray (length `PlacementFootprintVisibleDist`, mask -1610662149) hits something farther than
  `MinPlacementFootprintVisibleDist`, on layer 0 (or a stackable base gadget), with normal.y >
  sin(`FootprintVisibleSlopeThreshold`).
- `IsPlacementValid(Ray, RaycastHit)` (0xA439F0): distance <= `MaxValidPlacementDist`; stacking onto a stackable
  gadget only if the placeholder `CanStackOnStackableGadgets`; sin(`MaxValidPlacementSlope`) <= normal.y;
  placeholder `Gadget.IsCompletelyGrounded(GroundedCheckAdjustment, GroundedRaycastDistance)`.
- `_isPlacementBlocked` = `Gadget.IsOverlapping(OverlapPlacementFloorTolerance, GadgetOverlapLayers, false)`.
  The same overload is also called by `ShouldHideGadgetBasedOnPlayerCollision` (inside UpdateFootprint) and by
  `DefaultDynamicObjectSpawner*.InstantiateGadgetModel`.
- `IsInLinkedGadgetRange(Vector3)` (0xA43770): only for gadgets with `LinkedDefinition` and `LinkedGadgetRange > 0`:
  the first placed partner must be within range.
- Final colour: `SetGadgetPlacementValidity(!blocked && valid && grounded && linked)` (only reached with a held
  gadget and a visible footprint; otherwise `true`).
- Rotation: `_rotatingClockwise/_rotatingCounterClockwise` (set by the rotate input callbacks) ->
  placeholder `Transform.Rotate(up, ±GadgetRotationSpeed * dt, World)`. The placeholder is a child of the
  footprint, so its yaw accumulates; the footprint rigidbody itself is re-aligned to the surface every frame.
- No zone/site ownership: a placed gadget is a `GadgetModel` with scene group + position, saved like any other.
  The only "zone" limits are on entering gadget mode: `PlayerItemController.ToggleItem` refuses inside the ranch
  house (`_isInRanchHouse`), when dead, while fast-forwarding, or when `_gadgetModeBlocker` (a
  `DisableGadgetModeTrigger` volume, set by `BlockGadgetMode`, cleared by `UnblockGadgetMode`) is set.

## What the module does

- `UpdateFootprint` detour: on the main thread, swaps metadata values for the duration of the original call
  (restored by an RAII guard, also on exceptions): ignore_slope sets both slope thresholds to 0 (up to vertical,
  never overhangs); ignore_range raises `MaxValidPlacementDist` to the footprint ray length. A depth counter marks
  "inside UpdateFootprint" for the scoped hooks below.
- `Gadget.IsCompletelyGrounded` -> true while inside UpdateFootprint (ignore_slope).
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
- Placing upside down / on ceilings (normal.y < 0): left out on purpose.
