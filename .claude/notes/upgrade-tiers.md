# Upgrade tiers (branch feature/upgrade-tiers2) - DONE, needs an in-game check

Checklist u1-u4, u9, u10, u12, p1, p2, p8.
- Tiered upgrades chain like vanilla: after "Music Box I" is bought, the same card becomes "Music Box II", with Sell and Buy side by side. Same for ALL tiered custom upgrades.
- Garden upgrade (u9) goes after the sprinkler.
- Buy menu doesn't refresh after purchase (had to reopen, u10).
- Remove u1, u2, u3 ("doesn't make sense") and u12 (an action inside the buy menu).
- No sell popup in the buy menu (p8). No "can't sell" dialog (p2).
- Remove the "Owned" and "Can't sell:" prefixes.
- Owned custom upgrades need the checkmark icon in the buy menu (p1).

## Done

The branch is `feature/upgrade-tiers2`: `feature/upgrade-tiers` is an older, already merged branch that is still
checked out in another worktree.

- One card per tier chain: Faster Hens I -> II, Fast Growth I -> II; Music Box -> Music Box II and
  Ash Trough -> Ash Booster take over the vanilla card. A card that shows a tier not yet owned has
  Buy and Sell (the owned tier below) side by side.
- Fast Growth is listed right after the Sprinkler.
- Menu refresh: `ListUIRoot.RebuildUI` hook (the open menu never re-ran
  `UpdateMenuItemAvailabilityAndHidden`).
- Retired: Plort Collector II / III, Auto-Feeder II (u1-u3) and the silo "Sell All Plorts Now"
  menu action (u12; the Sell-All Button upgrade and its silo button stay, silo-sell builds on
  them). Old sidecar entries are ignored (logged, no refund) and dropped at the next save.
- Sell button: sells at once, no popup; no "Owned -" / "Can't sell:" prefix; passing reasons show
  as a disabled button ("Holds food"), permanent ones as no button.
- Checkmark: already fixed on develop (5d3ad87, clones keep the template's `_unavailableIcon`);
  not changed here.

## To check in game

- Sell button size / position next to PURCHASED and Purchase (split of the vanilla rect; log
  line "sell button layout: ...").
- A click on Sell never opens the purchase prompt of the card's next tier.
- Gamepad can no longer sell (the button is not navigable; confirming an owned card does
  nothing). Decide whether that is acceptable.
- Checkmark on owned (maxed) custom cards.
