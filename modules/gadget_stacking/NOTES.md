# gadget_stacking

Gadget mode places decorations and gadgets on top of other placed gadgets (a lamp on a pillar). Default on;
card on Misc > General (the card's switch is the setting). Round 16 task I.

RE: `re/fabricator.md` "Gadget stacking" (ray loop, IsPlacementValid, overlap helpers, linking, save, pickup).

## Design

The game has a complete stacking system that almost no prefab uses: a placed gadget with
`AllowGadgetStackingOnTop` is a valid surface for the footprint ray, the placed gadget is linked to it
(`AddStackedGadget`, saved as `StackedGadgetIds`, relinked after a load), and picking the base up takes the
stack along. The module switches that system on for every gadget and removes two obstacles:

- `Gadget.SetModel` (every placed or loaded gadget, never the placeholder) sets `AllowGadgetStackingOnTop`.
  Switching the module on marks the gadgets already in the scene (one `FindObjectsOfTypeAll`, gadgets with a
  model only); switching it off restores the ones the module marked (pointer set, identity only).
- `GadgetItem.IsPlacementValid` (runs inside `UpdateFootprint` after the ray loop): sets the placeholder's
  `CanStackOnStackableGadgets`; sets `_baseGadget` to the gadget of the chosen hit, or null. The game assigns it
  from any stackable gadget the ray passes, in ray order, so with every gadget stackable a gadget placed on the
  ground in front of another, or on top of one with another behind it, would be linked to the wrong one.
  On a gadget, a refusal is re-checked with distance and slope only: the grounded test (down rays from the
  bounds' corners) fails whenever the new gadget is wider than the top it stands on. A gadget trigger volume as
  the hit surface is refused (logged once per gadget).
- `GadgetOverlapHelpers.IsGadgetOverlapping` (static, below Free Placement's `Gadget.IsOverlapping`): armed by
  IsPlacementValid for the placeholder's transform and the metadata's `GadgetOverlapLayers` in the same
  dispatcher tick, one use. After a "blocked" result, `GadgetOverlapInfo` holds the blockers; when every one is
  part of a placed gadget (`GetComponentInParent<Gadget>` with a model), the result becomes "not blocked".
  Plot walls, terrain, slimes, the player and anything else still block, so plots, corrals, water and the
  ranch-house rules stay vanilla.
- Pickup: the vanilla rule stays (least surprising, and it keeps the saved links consistent): picking up or
  storing a gadget takes everything stacked on it back into the inventory. `PickupOrStoreStackedGadgets` is
  hooked only to log it (`'X' picked up or stored with N gadget(s) stacked on it`).

Works with Free Placement: it owns `UpdateFootprint`, `Gadget.IsOverlapping` and `IsCompletelyGrounded`; this
module hooks only below them. With Free Placement's ignore_collision on, its detour answers first and this
module's overlap detour is not reached (nothing to relax).

## Notes and limits

- The footprint shows on a gadget only where the gadget has a non-trigger collider not on layer 26 (its
  `Bounds` capsule is layer 26 and is skipped by the game's ray). Gadgets without such colliders on top
  can't be stacked on.
- Right after placing a gadget the game hides the outline while you still aim at it
  (`_hasLookedOffOfLastPlacedGadget`): look away once, then aim at it again to stack on it.
- Changing the variant of a base gadget (`Gadget.ChangeVariant` destroys and re-places it) drops the links to
  the gadgets on it (the game's `HandleStackedGadgetDestroyed`); they stay where they are.
- Turning the module off mid-session also clears the flag on DJ Speaker Stacks that dj_buildables marked
  stackable after this module (both set the same field); a reload restores them.
- Not checked in game (no runtime here): the trigger-volume refusal only matters if the game lets queries hit
  triggers (`Physics.queriesHitTriggers`), and which vanilla gadgets have usable top colliders.
