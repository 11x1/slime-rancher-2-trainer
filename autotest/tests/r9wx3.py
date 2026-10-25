"""r9wx3 Sandbox weather: lock and reload.

Checklist step (one paragraph in the html):
  Lock on, Start Thunderstorm: stays across a forecast change (log "re-applied"), map indicator shows
  it. Save, title, reload: lock back (log "applied from the save's lock"). Back to natural: lock gone,
  forecast weather returns.
Semi: the map indicator is judged by the user (screenshot + question).

Needs harness:
  probe sandbox.weather          {zone?} -> {zone, label, locked}     the Sandbox weather override of a zone
  exec  sandbox.weather          {zone, op: "start"|"natural", pattern?, lock?}   as the Sandbox Weather card
  probe weather.zone             {zone?} -> {zone, running: [..], forecast: [..]}  the game's weather state
  exec  weather.force_change     {zone}   make the game's forecast move on now (the director's next pattern)
  exec  ui.open_map / ui.close_map
"""
CASE = "r9wx3"
TITLE = "Weather lock and reload"
WHERE = "ranch, Sandbox on (slot 5)"
NEEDS = ["probe sandbox.weather", "exec sandbox.weather", "probe weather.zone", "exec weather.force_change",
         "exec ui.open_map", "exec ui.close_map"]

PATTERN = "Thunderstorm"


def override(t):
    return t.probe("sandbox.weather", zone="current") or {}


def running(t):
    return [str(x) for x in (t.probe("weather.zone", zone="current") or {}).get("running", [])]


def storm_running(t):
    return any(PATTERN in r for r in running(t))


def run(t):
    t.in_slot(5)
    t.set_setting("sandbox.enabled", True)
    t.teleport(to="ranch")

    t.step(1, "Lock on, start Thunderstorm")
    m = t.mark()
    t.exec("sandbox.weather", zone="current", op="start", pattern=PATTERN, lock=True)
    t.wait_until(lambda: storm_running(t), 15, "Thunderstorm running")
    ov = override(t)
    t.check(ov.get("locked") and PATTERN in str(ov.get("label")), f"override after start: {ov}")

    t.step(2, "forecast change: the lock re-applies the storm")
    t.exec("weather.force_change", zone="current")
    t.wait_log(m, r"weather: .*Thunderstorm.* re-applied", 30)
    t.wait_until(lambda: storm_running(t), 15, "Thunderstorm running after the forecast change")

    t.step(3, "map indicator shows the lock")
    t.exec("ui.open_map")
    try:
        t.confirm("Map: does the Ranch show the locked Thunderstorm weather indicator?")
    finally:
        t.exec("ui.close_map")

    t.step(4, "save, title, reload: lock back")
    t.save()
    m = t.mark()
    t.reload()
    t.wait_log(m, r"weather: .*Thunderstorm.* applied from the save's lock", 60)
    ov = override(t)
    t.check(ov.get("locked"), f"lock not back after the reload: {ov}")
    t.wait_until(lambda: storm_running(t), 15, "Thunderstorm running after the reload")
    t.no_faults(m)

    t.step(5, "back to natural: lock gone, forecast weather returns")
    m = t.mark()
    t.exec("sandbox.weather", zone="current", op="natural")
    t.wait_log(m, r"back to natural weather", 10)
    t.check(not override(t).get("locked"), "still locked after Back to natural")

    def natural():
        z = t.probe("weather.zone", zone="current") or {}
        return sorted(map(str, z.get("running", []))) == sorted(map(str, z.get("forecast", [])))
    t.wait_until(natural, 60, "running weather = forecast")
