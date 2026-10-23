# Hen Collector fix 2 (branch feature/hen-collector-fix2)

Checklist k2-k5, u8. Build 4d16e38c still fails in-game.
- Collector model never appears at the plot corner (sell button says chickens are inside). Read the "hen collector placement" lines in sr2mods.log first.
- Chickens vanish instead of getting sucked in. Reuse the elder collector's vacuum effect.
- Two limit sliders side by side: one for hens, one for roosters (chicks never count).
