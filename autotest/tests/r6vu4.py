"""r6vu4 Vacpack upgrades: saving.

Checklist step (one paragraph in the html):
  Fill extra slots and water, save, reload: water still in the water slot, extra slots refilled (log
  "spread into the extended layout", "refilled from the sidecar"); Tank Booster X comes back as 150
  (log "level 8 -> 10"). Old save: Extra Tank IV carries over; old IX logs "not applied yet" until
  VIII is owned.
Not covered: the old-save part (it needs a save written by an older build; manual only).

Needs harness:
  exec  sandbox.upgrade_level    {id, level}     (wave-1 generic) "tank_booster" / "extra_tank", 1-based
  probe vacpack.slots            -> [{index, id, count, max, water}]   the player's ammo slots
  exec  vacpack.set_slot         {index, id, count}
"""
CASE = "r6vu4"
TITLE = "Vacpack upgrades saving"
WHERE = "menu only (slot 5)"
NEEDS = ["exec sandbox.upgrade_level", "probe vacpack.slots", "exec vacpack.set_slot"]

FILL = ["PinkPlort", "RockPlort", "TabbyPlort", "PhosphorPlort", "HoneyPlort", "BoomPlort", "CrystalPlort",
        "RadPlort"]


def slots(t):
    r = t.probe("vacpack.slots")
    return list(r.get("slots", []) if isinstance(r, dict) else r or [])


def contents(ss):
    return [(s.get("index"), s.get("id"), s.get("count")) for s in ss]


def run(t):
    t.set_setting("custom_upgrades.enabled", True)
    t.in_slot(5)
    t.exec("custom_upgrades.set_on", on=True)

    t.step(1, "Tank Booster X and Extra Tank IV")
    t.set_upgrade_level("tank_booster", 10)
    t.set_upgrade_level("extra_tank", 4)
    ss = t.wait_until(lambda: (lambda x: x if len(x) >= 9 else None)(slots(t)), 10, "9 vacpack slots (8 + water)")
    normal = [s for s in ss if not s.get("water")]
    water = [s for s in ss if s.get("water")]
    t.eq(len(normal), 8, "item slots with Extra Tank IV")
    t.eq(len(water), 1, "water slots")
    t.check(all(s.get("max") == 150 for s in normal), f"slot max with Tank Booster X: {[s.get('max') for s in normal]}")

    t.step(2, "fill every slot and the water slot")
    for s, ident in zip(normal, FILL):
        t.exec("vacpack.set_slot", index=s["index"], id=ident, count=120)
    t.exec("vacpack.set_slot", index=water[0]["index"], id="Water", count=30)
    before = contents(slots(t))
    t.check(all(c for _, _, c in before), f"a slot stayed empty: {before}")

    t.step(3, "save and reload")
    t.save()
    m = t.mark()
    t.reload()
    t.wait_log(m, r"vacpack: loaded vacpack spread into the extended layout", 30)
    t.wait_log(m, r"vacpack: extra slots refilled from the sidecar", 30)
    t.wait_log(m, r"vacpack: .*Tank Booster.* level 8 -> 10", 30)

    t.step(4, "water in the water slot, extra slots refilled, max 150")
    t.wait_until(lambda: contents(slots(t)) == before, 15, "the vacpack as before the save")
    after = slots(t)
    w = [s for s in after if s.get("water")]
    t.check(w and w[0].get("id") == "Water", f"water slot after the reload: {w}")
    t.check(all(s.get("max") == 150 for s in after if not s.get("water")), "slot max not 150 after the reload")
    t.no_faults(m)
    t.uncovered("old save (Extra Tank IV carry-over, old IX 'not applied yet'): needs a save from an older build")
