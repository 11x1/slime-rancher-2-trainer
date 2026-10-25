"""r15sl5 Slime Launcher saved per save.

Checklist steps:
  1. Own Slime Launcher II and save; reload: the card shows III as next and slimes still fly further.
  2. Turn custom upgrades off (or load without the mod): the save loads fine and slimes shoot at
     vanilla speed.
Plan (note): slot 6 = a game without the upgrade, slot 5 = buys it; switch between them and back.
Not covered: loading without the mod at all (needs another DLL; custom upgrades off stands in).

Needs harness (probes / actions this test calls; not in the wave-1 generic set):
  probe custom_upgrades.state     -> {on, locked}        per-save custom upgrades switch
  exec  custom_upgrades.set_on    {on}                   the per-save switch (as the Fabricator card)
  exec  custom_upgrades.buy       {id, level?}           buy the next level (Newbucks spent as in game)
  probe custom_upgrades.upgrade   {id} -> {level, max, next_label}   what the Fabricator card shows
  exec  vacpack.set_slot          {index, id, count}     put slimes in the vacpack
  exec  vacpack.shoot             {index}                shoot one item from that slot
  probe vacpack.last_shot         -> {id, speed, eject_speed}  speed of the last shot object right after
        the shot and the vacpack's vanilla EjectSpeed (owned by the harness, so it also works with
        custom upgrades off; the log line "shot at 1.4x" is written once per level per session only)
"""

import re

CASE = "r15sl5"
TITLE = "Slime Launcher saved per save"
WHERE = "menu only (slots 5 and 6)"
NEEDS = ["probe custom_upgrades.state", "exec custom_upgrades.set_on", "exec custom_upgrades.buy",
         "probe custom_upgrades.upgrade", "exec vacpack.set_slot", "exec vacpack.shoot",
         "probe vacpack.last_shot"]

UPGRADE = "slime_launcher"
NEXT_I = re.compile(r"Slime Launcher I(?!I)")
NEXT_III = re.compile(r"Slime Launcher III")


def launcher(t):
    return t.probe("custom_upgrades.upgrade", id=UPGRADE)


def shot_factor(t):
    """Puts a Pink slime in slot 1, shoots it, returns last shot speed / vanilla speed."""
    t.exec("vacpack.set_slot", index=0, id="Pink", count=1)
    t.exec("vacpack.shoot", index=0)
    p = t.probe("vacpack.last_shot")
    base, last = float(p.get("eject_speed") or 0), float(p.get("speed") or 0)
    t.check(base > 0 and last > 0, f"no shot speed measured: {p}")
    return last / base


def run(t):
    t.set_setting("custom_upgrades.enabled", True)

    t.step(1, "slot 6: a game without the Slime Launcher")
    t.in_slot(6)
    t.exec("custom_upgrades.set_on", on=True)
    if launcher(t).get("level"):
        t.note("slot 6 had a Slime Launcher from an earlier run: new game in slot 6")
        t.in_slot(6, fresh=True)
        t.exec("custom_upgrades.set_on", on=True)
    t.eq(launcher(t).get("level"), 0, "Slime Launcher level in slot 6 before the test")
    t.save()

    t.step(2, "slot 5: buy Slime Launcher I and II, save")
    t.in_slot(5, fresh=True)
    t.exec("custom_upgrades.set_on", on=True)
    t.give_newbucks(50000)
    t.exec("custom_upgrades.buy", id=UPGRADE)
    t.exec("custom_upgrades.buy", id=UPGRADE)
    lv = launcher(t)
    t.eq(lv.get("level"), 2, "level after two purchases")
    t.save()

    t.step(3, "slot 6: no launcher there")
    t.in_slot(6)
    lv6 = launcher(t)
    t.eq(lv6.get("level"), 0, "slot 6 level (must not inherit slot 5's purchase)")
    t.check(NEXT_I.search(str(lv6.get("next_label"))), f"slot 6 card next: {lv6.get('next_label')!r}")

    t.step(4, "back to slot 5: card shows III next, slimes fly 1.4x")
    t.in_slot(5)
    lv = launcher(t)
    t.eq(lv.get("level"), 2, "slot 5 level after the reload")
    t.check(NEXT_III.search(str(lv.get("next_label"))), f"card next: {lv.get('next_label')!r}")
    m = t.mark()
    f = shot_factor(t)
    t.check(abs(f - 1.4) < 0.05, f"slime shot at {f:.2f}x the vanilla speed, want 1.4x")
    t.no_faults(m)

    t.step(5, "custom upgrades off, reload slot 5: loads fine, vanilla speed")
    t.set_setting("custom_upgrades.enabled", False)
    m = t.mark()
    t.reload()
    t.no_faults(m)
    f = shot_factor(t)
    t.check(abs(f - 1.0) < 0.05, f"slime shot at {f:.2f}x with custom upgrades off, want 1.0x")
    t.uncovered("load without the mod")
