# Round 16 plan (feedback from the round 15 test, 2026-10-25)

Build tested: d1f076b (develop). Results are in the work-account checklist https://claude.ai/artifact/12HrkuKyWbJs1PtsufwVvE (collection `results`, ids `r15*`; free text in `misc/notes`). User quotes are verbatim.

## How to run this round
Same as round 15 (auto-memory: orchestrator mode, Opus subagents at medium effort in worktrees, git workflow, no session link, plain commands, checklist conventions, read logs yourself, deploy once per round; agent builds take turns via build.ps1 and must `Set-Location` into their own worktree):
1. One agent per task below, up to 9 at a time. Prompt: "Read `.claude/agent-brief.md` and follow it", the task's section, the branch name.
2. Merge with `git merge --no-ff <branch> -m "Merge <branch> into develop"`.
3. When all are merged: one `.\deploy.ps1 -Build` (with `-WaitForExit` in the background if the game runs).
4. Build the round 16 checklist (`.claude/checklist/README.md`) and publish it to the same artifact.

The user: "lowk one of the best passes"; "OH MY GOD THIS TESTING PASS WAS INSANE ITS SO GOOD".

Passed:
- Fabricator pictures, finally sharp
- Slime Radio physics
- drone dialogs: "it is so clean and perfect"
- map after the overlay, all four cases
- Powderfall ice pulse; Ice Lamp
- Hen Collector button
- Slime Launcher shots and the held largo
- Ranch > Upgrades layout: "after 3 passes the layout is correct"
- upward context menu and the food menu
- Quantum Link name and look
- drones respecting the feeder lock

Shared rule for key hints (from round 15): use the game's own key icons through `game_button` `Spec.control`.

On hold: the release workflow (`chore/ci-release-research`).

## A. Corral Radio key grid alignment (`fix/corral-radio-r16`)
User, on r15ra1 ("Very NICE"):
- "when hovering on right side, the buttons are as (L->r, T->B) [Queue, <empty>, <empty>, Clear], should be [<empty>, Queue, <empty>, Clear]". With the song list focused, Queue sits in the top-right cell of the right grid.
- "Right bottom icons should be aligned right, Clear is aligned left with Play Now in queue hints". The right grid's buttons are right-aligned in their cells.

## B. Auto-Feeder: "Leave last food item in" toggle (`fix/feeder-lock-r16`)
- User, on r15fl1: "No no no no no, it should be just a toggle, 'Leave last food item in'".
- Replace the food slider with one on/off toggle on the Auto-Feeder card. While it is on, the feeder never hands out its last food item. One item of that food always stays in, so the feeder keeps that food type, and (with the round 15 refusal hooks) players and drones can only add more of that same food. When the toggle goes off, the last item can be fed out as normal and the feeder accepts anything once empty.
- Reuse what round 15 built: the per-plot sidecar value, the `SiloStorage` hooks, and the drone `GetSinks` path. Change the stored value to an on/off flag and derive the locked food from the feeder's current contents.
- Find where the feeder takes items out to feed (re/plots.md SlimeFeeder; the feed or eject path), and stop it at 1 remaining while the toggle is on.
- Use the game's own toggle style if there is one in the plot menu (the existing sliders and game buttons); keyboard and pad should work.

## C. Upgrade cards and text (`fix/upgrade-cards-r16`)
- r15sl1: "'Next: Tank Booster IV', same issue on Tank Booster upgrade 'Next: Tank Booster V' on VIII, uses wrong next label". The Fabricator's "Next:" label shows the wrong upgrade name and tier: on Slime Launcher it shows Tank Booster's, and on Tank Booster at VIII it shows "V". Find where the next-tier label is computed for custom and extended upgrades and fix both.
- Note: "Remove the 'Vacs up the hens and roosters...' from Hen Collector description, should be generic upgrade text, nothing to do with the configuration of it". Rewrite the Hen Collector card description as a plain upgrade description.
- Log: `[custom_upgrades] coop air net: fit offset (0.00, -190.33, 0.00) too large - left unfitted` appeared 108 times in one session. Log it once per plot, and find why the offset is -190, e.g. a wrong reference transform while the plot is loading.

## D. Quantum Storage Link: row hint and effects (`fix/quantum-link-r16`)
- r15ql1: "Go to hint is gone from the bottom, nice / On drone rows i still see '(Magnifying icon) [SPACE]'". Each drone row in the link's Drones tab has its own magnifier + [SPACE] hint (on the DroneListButton or its row prefab). Hide it in the link's menu only.
- r15ql2: "The blue effect could be doubled, very nice effect". Double the blue effect on top (size 0.30 -> about 0.60, or whatever doubles it visibly).
- r15ql2: "One nitpick is that the 'glitch' effect has a 'chams' look white, not very transparent, just feels like a model copy glitching". Make the phase-through moment subtler and more transparent: less white, more see-through scanlines. If the drone pass can't be made transparent on a copy, try a lower alpha or tint on the copy, or a shorter or partial effect.

## E. Lamps start at once; music pillars and tiles (`feature/lamps-r16`)
- r15lp3: "on initial place it still takes time, otherwise toggling music is rly fast w it". A newly placed lamp near a playing speaker should start pulsing within about half a second. Hook or poll gadget placement cheaply. The lamp list currently refreshes on its own timer.
- Note: "Make music pillars and tiles also pulse their color to music if possible". Find the music-themed decorations (pillars and floor tiles; search the gadget and decor definitions for music, note or dance names). Make them pulse their colour or emission with the beat like the lamps, through the same lamp pulser and highlighter categories.

## F. Plorts glow to the music (`feature/plort-glow-r16`)
- Note: "Plorts glow to music aswell pleaseeeeeee". Plorts near a playing speaker pulse their glow or emission with the beat. Plorts can be numerous: keep the cost bounded with a range around speakers, a cap on count, cached lists refreshed every few seconds, and no per-frame Find. Use property blocks; check the plort material's emissive properties. Add the plorts as a highlighter category. Share the beat source with the lamps (module slime_dj), but keep to its own file.

## G. Minimap: ranch icons everywhere, ghost icon (`fix/minimap-r16`)
- Note: "The ranch's mini icons only show at the laboratory, should show on other ranch islands aswell please". The minimap's ranch icons (plots, buildings and so on) only appear in the Lab area. Show them on the other ranch expansions too: Archway, Den, Gully, Tidepools and so on. Find the filter (zone or area check).
- Note: "Use a blue version of the drone icon for ghosts indicator under minimap for now, need to make one for you". The tracker row under the minimap: the ghost drone icon becomes a blue-tinted copy of the drone icon. Tint it at runtime; add no new PNG of game art. Keep the custom-icon slot so the user's drawing can replace it later (custom-icons.md).

## H. Overlay context menu look, Sandbox +/- tooltips (`fix/overlay-r16`)
- Note: "the context menu hovered item could be rounded and the arrow aswell plus the arrow a bit smaller". In the overlay's context menus, round the hover highlight of an item and the submenu arrow, and make the arrow a bit smaller.
- r15sl4: "remove the tooltips from upgrade +/-". In Sandbox > Upgrades & Blueprints, the - and + buttons have no tooltip.

## Carry-over (untested, re-add under Skipped)
- Skipped this round: r15fl2 (replaced by B's toggle cases), r15sl5 (Slime Launcher per save).
- Still untested: r12pg4, r12nc2, r12pg1, r12lp4, r11qs2-4, r11cr3.
- Earlier: r7sb8, r9wx3, r9wx4, r10cr3, r10cr4, r7bd6, r9rs3, r10dj6, r10dp3, r9dp4, r9dp5, r7dr2, r4dr3, r10sh2-5, r9sh4, r9sh5, r9sh7, r8tp3, r4mm4, r6vu4, hc4, p7, ug2, ug3, sv2, ut6.
