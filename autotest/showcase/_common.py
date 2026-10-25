"""Helpers shared by the shot definitions (not a shot: the loader skips files starting with _)."""
import run


def radio_corral(t, songs: int = 3) -> str:
    """A corral with Music Box + Corral Radio playing a playlist of `songs` songs. Returns its plot id.

    Uses the r10cr3 test's set-up (same probes and actions: custom_upgrades.buy_plot, slime_dj.*).
    """
    r10cr3 = run.load_test("r10cr3")
    t.exec("custom_upgrades.set_on", on=True)
    cs = r10cr3.ensure_radio_corrals(t, 1)
    plot = cs[0]["id"]
    lib = list((t.probe("slime_dj.library") or {}).get("songs", []))
    t.check(lib, "no songs in sr2mods_music")
    radio = r10cr3.radio(t, plot) or {}
    if not radio.get("playlist"):
        t.exec("slime_dj.radio", plot=plot, op="add", songs=lib[:songs])
    if not radio.get("on"):
        t.exec("slime_dj.radio", plot=plot, op="on")
    return plot
