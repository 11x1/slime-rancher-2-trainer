"""Showcase screenshots of the mod's features, for a release page (game imagery: never committed or uploaded).

    python autotest/showcase.py --list                  # the shot definitions (autotest/showcase/*.py)
    python autotest/showcase.py                         # every shot
    python autotest/showcase.py sandbox_menu slime_dj   # these shots
    python autotest/showcase.py --frame                 # ask the user to frame every shot by hand
    python autotest/showcase.py --no-ask                # never ask (a shot without a saved place is
                                                        # taken from where the player stands)
    python autotest/showcase.py --save-place showcase_corral   # remember where the player stands now

Each shot loads its slot (5 or 6 only), sets its settings (put back after), goes to its place, sets the
time of day, opens a menu or a mod menu page, runs its own setup, and takes the screenshot with the
harness bits hidden (and the game HUD / minimap if the shot says so). Out:
autotest/showcase_out/<date>_<time>/<id>.png and manifest.json (gitignored). build_gallery.py turns a
manifest into one HTML page. Nothing leaves the machine.

Test runs take showcase shots too: t.showcase(id, title, caption) in a test, on with run.py --showcase.

Before anything it backs up the saves (as run.py) and refuses to run while a game outside slots 5 and 6
is loaded.
"""
from __future__ import annotations

import argparse
import importlib.util
import json
import shutil
import sys
import time
import traceback
from datetime import datetime
from pathlib import Path
from typing import Any

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import run  # noqa: E402  (the test context: in_slot, settings restore, generic actions; backups)
import sr2test  # noqa: E402
from sr2test import SAFE_SLOTS, Client, ConnectError, HarnessError, NeedsHarness, SlotGuardError  # noqa: E402

DEFS_DIR = HERE / "showcase"
OUT_DIR = HERE / "showcase_out"
SHOOT, SKIP = "Shoot", "Skip"


# ---- the session: takes shots, copies them, keeps the manifest ------------------------------------

class Showcase:
    """One showcase run's output folder and manifest. Used by showcase.py and by run.py --showcase."""

    def __init__(self, client: Client, out_root: Path | str = OUT_DIR, frame_all: bool = False,
                 ask: bool = True, ask_timeout: float = 600.0):
        self.c = client
        self.out_root = Path(out_root)
        self.frame_all = frame_all
        self.ask_ok = ask
        self.ask_timeout = ask_timeout
        self.started = datetime.now()
        self.dir: Path | None = None  # made with the first shot
        self.manifest: dict = {"created": self.started.isoformat(timespec="seconds"), "build": {}, "shots": []}
        try:
            self.manifest["build"] = client.ping() or {}
        except HarnessError as e:
            self.manifest["build"] = {"error": str(e)}

    def _out(self) -> Path:
        if self.dir is None:
            stamp = self.started.strftime("%Y-%m-%d_%H%M%S")
            d = self.out_root / stamp
            n = 1
            while d.exists():
                n += 1
                d = self.out_root / f"{stamp}_{n}"
            d.mkdir(parents=True)
            self.dir = d
        return self.dir

    def write_manifest(self) -> Path | None:
        if self.dir is None:
            return None
        p = self.dir / "manifest.json"
        p.write_text(json.dumps(self.manifest, indent=2), encoding="utf-8")
        return p

    def frame(self, title: str, hint: str = "") -> bool:
        """Asks the user to frame the shot by hand. True: shoot now."""
        if not self.ask_ok:
            return True
        q = f"Showcase: {title}. " + (f"{hint} " if hint else "") + \
            "Frame the shot (move, aim the camera), then press 1 to shoot."
        r = self.c.ask(q, [SHOOT, SKIP], case="showcase", timeout_s=self.ask_timeout)
        return r.get("answer") == SHOOT

    def take(self, shot_id: str, title: str, caption: str, feature: str = "", *, source: str = "showcase",
             settings: dict | None = None, frame: bool = False, frame_hint: str = "", overlay: bool = False,
             hide_hud: bool = False, hide_minimap: bool = False, menu: dict | None = None,
             extra: dict | None = None) -> dict:
        """Takes one shot (asking to frame it first if `frame` or --frame) and records it.

        Returns its manifest entry ({"status": "shot" | "skipped", ...}).
        """
        entry: dict = {"id": shot_id, "title": title, "caption": caption, "feature": feature, "source": source,
                       "settings": dict(settings or {}), "options": {"overlay": overlay or bool(menu),
                       "hide_hud": hide_hud, "hide_minimap": hide_minimap, "menu": menu}}
        if extra:
            entry.update(extra)
        if (frame or self.frame_all) and not self.frame(title, frame_hint):
            entry.update({"status": "skipped", "note": "the user skipped framing"})
            self._record(entry)
            return entry
        r = self.c.shot(f"showcase_{shot_id}", overlay=overlay, hide_hud=hide_hud, hide_minimap=hide_minimap,
                        hide_harness=True, menu=menu)
        src = Path(str(r.get("path", "")))
        out = self._out()
        dest = out / f"{shot_id}.png"
        n = 1
        while dest.exists():  # the same id twice in one run (a test calling showcase in a loop)
            n += 1
            dest = out / f"{shot_id}_{n}.png"
        try:
            shutil.copy2(src, dest)
        except OSError as e:
            entry.update({"status": "error", "note": f"screenshot not copied from {src}: {e}"})
            self._record(entry)
            return entry
        try:
            st = self.c.state()
        except HarnessError:
            st = {}
        entry.update({"status": "shot", "file": dest.name, "width": r.get("width"), "height": r.get("height"),
                      "taken": datetime.now().isoformat(timespec="seconds"),
                      "game": {k: st.get(k) for k in ("slot", "zone", "time", "pos", "rot")}})
        self._record(entry)
        print(f"    showcase: {dest}")
        return entry

    def _record(self, entry: dict) -> None:
        self._out()
        self.manifest["shots"].append(entry)
        self.write_manifest()


# ---- shot definitions --------------------------------------------------------------------------------

DEFAULTS: dict[str, Any] = {
    "TITLE": "", "CAPTION": "", "FEATURE": "", "SLOT": 5, "SETTINGS": {}, "PLACE": None, "PLACE_HINT": "",
    "ROT": None, "TIME": None, "MENU": None, "PLOT_MENU": None, "HIDE_HUD": False, "HIDE_MINIMAP": False,
    "FRAME": False, "FRAME_HINT": "", "OVERLAY": False,
}


class ShotDef:
    """A file in autotest/showcase/: module constants (DEFAULTS, ID = the file name) and an optional setup(s)."""

    def __init__(self, path: Path):
        if str(path.parent) not in sys.path:  # the definitions' shared helpers (_common.py)
            sys.path.insert(0, str(path.parent))
        spec = importlib.util.spec_from_file_location(f"showcase_def_{path.stem}", path)
        mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)
        self.path = path
        self.module = mod
        self.id = getattr(mod, "ID", path.stem)
        for k, v in DEFAULTS.items():
            setattr(self, k.lower(), getattr(mod, k, v))
        self.setup = getattr(mod, "setup", None)
        if self.slot not in SAFE_SLOTS:
            raise SlotGuardError(f"{path.name}: SLOT {self.slot!r} (only slots 5 and 6)")


def load_defs(defs_dir: Path = DEFS_DIR) -> dict[str, ShotDef]:
    out: dict[str, ShotDef] = {}
    for p in sorted(Path(defs_dir).glob("*.py")):
        if p.name.startswith("_"):
            continue
        out[p.stem] = ShotDef(p)
    return out


def _place(t, d: ShotDef) -> str | None:
    """Goes to the shot's place. Returns why it could not ("" = there, None = no place asked for)."""
    if d.place is None:
        return None
    if isinstance(d.place, (list, tuple)):
        t.teleport(pos=list(d.place), rot=d.rot)
        return ""
    places = t.probe("places") or {}
    p = places.get(d.place) if isinstance(places, dict) else None
    if not p:
        return f"place '{d.place}' is not saved yet"
    pos = p.get("pos") if isinstance(p, dict) else p
    rot = d.rot if d.rot is not None else (p.get("rot") if isinstance(p, dict) else None)
    t.teleport(pos=pos, rot=rot)
    return ""


def run_def(client: Client, sc: Showcase, d: ShotDef, ask_timeout: float) -> dict:
    """Sets the shot up through the harness and takes it. Settings are put back after."""
    t = run.Test(client, f"showcase:{d.id}", d.module, ask_timeout)
    t.sc = sc
    t.shot_def = d
    print(f"\n== {d.id}: {d.title}  [slot {d.slot}]")
    entry: dict
    try:
        for k, v in (d.settings or {}).items():
            t.set_setting(k, v)
        t.in_slot(d.slot)
        missing = _place(t, d)
        if d.time is not None:
            t.set_time(hour=float(d.time))
        if d.setup:
            d.setup(t)
        if d.plot_menu:
            t.open_plot_menu(_plot_for(t, d.plot_menu))
        frame = bool(d.frame)
        hint = d.frame_hint
        extra = {}
        if missing:
            # No saved place: the user frames it (and the place is saved for next time), or with --no-ask
            # the shot is taken from where the player stands.
            extra["note"] = missing + (" (framed by hand, saved for next time)" if sc.ask_ok else
                                       " (taken from where the player stood)")
            frame = sc.ask_ok
            hint = (d.place_hint or f"Go to the spot for '{d.place}'.") + (" " + hint if hint else "")
        entry = sc.take(d.id, d.title, d.caption, d.feature, settings=dict(t._settings_set), frame=frame,
                        frame_hint=hint, overlay=bool(d.overlay), hide_hud=bool(d.hide_hud),
                        hide_minimap=bool(d.hide_minimap), menu=d.menu, extra=extra)
        if missing and entry.get("status") == "shot" and sc.ask_ok and isinstance(d.place, str):
            try:
                save_place(client, d.place)
                print(f"    place '{d.place}' saved: the next run goes there by itself")
            except HarnessError as e:
                print(f"    place '{d.place}' not saved: {e}")
    except NeedsHarness as e:
        entry = {"id": d.id, "title": d.title, "status": "skipped", "note": f"needs harness: {e}"}
        sc._record(entry)
    except (HarnessError, SlotGuardError, run.TestFail) as e:
        entry = {"id": d.id, "title": d.title, "status": "error", "note": str(e)}
        sc._record(entry)
    except ConnectError:
        raise
    except Exception as e:  # noqa: BLE001 - a broken definition must not stop the run
        entry = {"id": d.id, "title": d.title, "status": "error", "note": f"{type(e).__name__}: {e}",
                 "traceback": traceback.format_exc()}
        sc._record(entry)
    finally:
        try:
            if client.state().get("overlay_open"):
                client.overlay(open=False)
        except (HarnessError, ConnectError):
            pass
        if d.plot_menu:  # the next shot needs the plot menu closed
            try:
                if "LandPlotUIRoot" in (client.state().get("menus") or []):
                    try:
                        client.exec("ui.close_menu")
                    except HarnessError:
                        client.input(key="escape")  # needs the game window in front
            except HarnessError as e:
                print(f"    the plot menu may still be open: {e}")
        failed = t.restore_settings()
        if failed:
            print(f"    settings not restored: {failed}")
    print(f"   -> {entry['status']}" + (f": {entry.get('note')}" if entry.get("note") else ""))
    return entry


def save_place(client: Client, name: str) -> Any:
    """exec place_save {name} (Client.exec's own first parameter is called name)."""
    return client.call("exec", {"name": "place_save", "args": {"name": name}})


def _plot_for(t, what: str) -> str:
    """PLOT_MENU: a plot id, or a plot type (the first plot of that type)."""
    want = str(what).lower()
    plots = t.plots()
    for p in plots:
        if str(p.get("id", "")).lower() == want:
            return p["id"]
    for p in plots:
        if str(p.get("type", "")).lower() == want:
            return p["id"]
    raise run.TestFail(f"no plot '{what}' (id or type) for the plot menu")


# ---- command line -----------------------------------------------------------------------------------

def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("shots", nargs="*", help="shot ids (autotest/showcase/<id>.py); none = all")
    ap.add_argument("--list", action="store_true", help="list the shot definitions and exit")
    ap.add_argument("--frame", action="store_true", help="ask the user to frame every shot by hand")
    ap.add_argument("--no-ask", action="store_true", help="never ask the user (no framing)")
    ap.add_argument("--save-place", metavar="NAME", help="store where the player stands as a place and exit")
    ap.add_argument("--defs-dir", default=str(DEFS_DIR), help=argparse.SUPPRESS)
    ap.add_argument("--out-dir", default=str(OUT_DIR), help="where the <date>_<time> folders go")
    ap.add_argument("--pipe", default=None)
    ap.add_argument("--game-dir", default=None)
    ap.add_argument("--player-log", default=None)
    ap.add_argument("--saves-root", default=None)
    ap.add_argument("--backup-root", default=None)
    ap.add_argument("--ask-timeout", type=float, default=600.0)
    ap.add_argument("--connect-timeout", type=float, default=5.0)
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args(argv)

    try:
        defs = load_defs(Path(args.defs_dir))
    except SlotGuardError as e:
        print(f"REFUSED: {e}", file=sys.stderr)
        return 4
    if args.list:
        for i, d in defs.items():
            print(f"{i:18} {d.title}  [{d.feature}]" + ("  (framed by hand)" if d.frame else ""))
        return 0
    if args.frame and args.no_ask:
        ap.error("--frame and --no-ask exclude each other")
    unknown = [s for s in args.shots if s not in defs]
    if unknown:
        ap.error(f"no shot definition: {', '.join(unknown)} (see --list)")
    chosen = [defs[s] for s in (args.shots or list(defs))]

    game_dir = Path(args.game_dir) if args.game_dir else sr2test.default_game_dir()
    if not args.save_place:
        try:
            backup = run.backup_saves(args.saves_root, args.backup_root, game_dir, tag="autotest-showcase")
        except run.BackupError as e:
            print(f"REFUSED: save backup failed: {e}", file=sys.stderr)
            return 2
        print(f"Saves backed up to {backup}")
    try:
        client = run.connect(args)
    except ConnectError as e:
        print(f"Cannot connect: {e}", file=sys.stderr)
        return 3
    try:
        if args.save_place:
            st = client.state()
            if not st.get("in_game") or st.get("slot") not in SAFE_SLOTS:
                print("REFUSED: save places in a slot 5 or 6 game (load it first)", file=sys.stderr)
                return 4
            print(json.dumps(save_place(client, args.save_place)))
            return 0
        why = run.check_start_state(client)
        if why:
            print(f"REFUSED: {why}", file=sys.stderr)
            return 4
        sc = Showcase(client, args.out_dir, frame_all=args.frame, ask=not args.no_ask, ask_timeout=args.ask_timeout)
        for d in chosen:
            try:
                run_def(client, sc, d, args.ask_timeout)
            except ConnectError as e:
                print(f"Lost the harness: {e}", file=sys.stderr)
                sc._record({"id": d.id, "title": d.title, "status": "error", "note": f"connection: {e}"})
                break
    finally:
        client.close()
    shots = sc.manifest["shots"]
    counts: dict[str, int] = {}
    for s in shots:
        counts[s["status"]] = counts.get(s["status"], 0) + 1
    print("\nShowcase: " + ", ".join(f"{n} {s}" for s, n in counts.items()))
    for s in shots:
        print(f"  {s['id']:18} {s['status']:8} {s.get('note', '')[:100]}")
    if sc.dir:
        print(f"Out: {sc.dir}  (python autotest/build_gallery.py {sc.dir})")
    return 0 if all(s["status"] in ("shot", "skipped") for s in shots) else 1


if __name__ == "__main__":
    sys.exit(main())
