# Hen Collector fix 2 (branch feature/hen-collector-fix2)

Checklist k2-k5, u8. Build 4d16e38c still failed in-game. Code done on the branch; open = the in-game check.
Findings moved to `modules/custom_upgrades/NOTES.md`.

- [x] Collector model never appears (code). Cause: the prefab's `Deluxe` container is turned -90
  degrees itself, so the clone's -90 degree turn put it exactly inside the vanilla elder collector.
  Now placed in the free (+x, -z) corner, picked from the live plot's parts.
  - [ ] In game: model at the corner diagonal to the vitamizer corner, next to the plot menu side;
    log `hen collector corner on plot ...: collector turned 0 deg into the (+x, -z) corner`.
- [x] Chickens vanish instead of getting sucked in (code). The clone's collector now runs the
  vanilla FixedUpdate on joints we create (the elder collector's own suction).
  - [ ] In game: chickens fly to the collector, puff, cyclone animation and vac sound; log
    `vacuum: the elder collector's own suction` and `vacuuming N`.
- [x] Two limit sliders side by side, hens and roosters (code).
  - [ ] In game: both change with the mouse; d-pad / arrows step hens, right stick / Shift +
    arrows step roosters, the menu grid selection does not move; values survive save + load.
