"""r12pg4 Drone programs: the [P] pill per save.

Checklist steps:
  1. Load a save that doesn't own the upgrade: no [P] pill on any station.
  2. Load the save that owns it: the pill is back.
Plan: slot 6 has a drone station and no Drone Programs upgrade, slot 5 has a station and buys it.

Needs harness:
  probe drones.stations          -> [{id, pos, programs}]   placed drone stations (id = PlacedGadget ActorId)
  exec  drones.place_station     {pos?}                     place a drone station on the ranch near the player
  exec  drones.open_menu         {station, page}            open a station's menu on a page ("Programming")
  exec  ui.close_menu                                       close the open game menu (as Esc)
  probe ui.prompt_pills          -> [{key, label}]          prompt pills / input hints of the open menu
  exec  custom_upgrades.set_on / custom_upgrades.buy, probe custom_upgrades.upgrade (see r15sl5)
"""
import re

CASE = "r12pg4"
TITLE = "Drone programs [P] pill per save"
WHERE = "ranch: drone station (slots 5 and 6)"
NEEDS = ["probe drones.stations", "exec drones.place_station", "exec drones.open_menu", "exec ui.close_menu",
         "probe ui.prompt_pills", "exec custom_upgrades.buy", "probe custom_upgrades.upgrade"]

UPGRADE = "drone_programs"
PILL = re.compile(r"\bPrograms?\b")


def stations(t):
    r = t.probe("drones.stations")
    return list(r.get("stations", []) if isinstance(r, dict) else r or [])


def ensure_station(t):
    s = stations(t)
    if not s:
        t.teleport(to="ranch")
        t.exec("drones.place_station")
        s = t.wait_until(lambda: stations(t), 10, "a drone station")
    return s


def pills_on(t, station):
    t.exec("drones.open_menu", station=station["id"], page="Programming")
    try:
        r = t.probe("ui.prompt_pills")
        pills = list(r.get("pills", []) if isinstance(r, dict) else r or [])
        t.screenshot(f"{CASE}_station_{station['id']}")
        return [p for p in pills if PILL.search(str(p.get("label", ""))) or str(p.get("key", "")).upper() == "P"]
    finally:
        t.exec("ui.close_menu")


def run(t):
    t.set_setting("custom_upgrades.enabled", True)

    t.step(1, "slot 6: a station, no Drone Programs upgrade")
    t.in_slot(6)
    t.exec("custom_upgrades.set_on", on=True)
    if t.probe("custom_upgrades.upgrade", id=UPGRADE).get("level"):
        t.note("slot 6 owns Drone Programs from an earlier run: new game in slot 6")
        t.in_slot(6, fresh=True)
        t.exec("custom_upgrades.set_on", on=True)
    ensure_station(t)
    t.save()

    t.step(2, "slot 5: a station, buy Drone Programs, save")
    t.in_slot(5)
    t.exec("custom_upgrades.set_on", on=True)
    ensure_station(t)
    if not t.probe("custom_upgrades.upgrade", id=UPGRADE).get("level"):
        t.give_newbucks(50000)
        t.exec("custom_upgrades.buy", id=UPGRADE)
    t.check(t.probe("custom_upgrades.upgrade", id=UPGRADE).get("level"), "Drone Programs not owned after buying")
    t.save()

    t.step(3, "load slot 6: no [P] pill on any station")
    t.in_slot(6)
    for s in stations(t):
        hits = pills_on(t, s)
        t.check(not hits, f"station {s['id']} in slot 6 shows {hits}")

    t.step(4, "load slot 5: the pill is back")
    t.in_slot(5)
    st = stations(t)
    t.check(st, "slot 5 has no drone station after the reload")
    for s in st:
        t.check(pills_on(t, s), f"station {s['id']} in slot 5 has no [P] Programs pill")
