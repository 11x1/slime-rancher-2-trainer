"""Runs checklist cases against the running game through the test harness.

    python autotest/run.py r15sl5 r9wx3        # these cases
    python autotest/run.py --all-skipped        # every round 16 skipped case (no test file -> skipped)
    python autotest/run.py --list               # the cases that have a test file

Before anything it backs up the saves folder (layout as deploy.ps1, folder name ends in _autotest)
and refuses to run if the backup failed. It refuses to run while a game outside slots 5 and 6 is
loaded. Results go to autotest/results/<date>_<time>.json (gitignored).
"""
from __future__ import annotations

import argparse
import importlib.util
import json
import os
import shutil
import subprocess
import sys
import time
import traceback
from datetime import datetime
from pathlib import Path
from typing import Any

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import sr2test  # noqa: E402
from sr2test import (SAFE_SLOTS, Client, ConnectError, HarnessError, HarnessTimeout, Mark,  # noqa: E402
                     NeedsHarness, NeedsUser, SlotGuardError, TestFail, faults_in)

TESTS_DIR = HERE / "tests"
RESULTS_DIR = HERE / "results"

# The "Skipped / ..." folds of the round 16 checklist (.claude/notes/autotest.md). coverage.md has a row each.
SKIPPED_R16 = (
    "r11qs2 r11qs3 r11qs4 r12pg4 r15sl5 r12nc2 r7sb8 r9wx3 r9wx4 r12lp4 r11cr3 r10cr3 r10cr4 r7bd6 r9rs3 "
    "r10dj6 r12pg1 r10dp3 r9dp4 r9dp5 r7dr2 r4dr3 r10sh2 r10sh3 r10sh4 r10sh5 r9sh4 r9sh5 r9sh7 r8tp3 "
    "r4mm4 r6vu4 hc4 p7 ug2 ug3 sv2 ut6"
).split()

STATUSES = ("pass", "fail", "needs_user", "skipped", "error")


# ---- save backup ----------------------------------------------------------------------------------

def main_checkout_root() -> Path:
    """The main checkout (save-backups\\ lives there, also when this runs from a worktree)."""
    try:
        out = subprocess.run(["git", "rev-parse", "--path-format=absolute", "--git-common-dir"], cwd=HERE,
                             capture_output=True, text=True, timeout=10)
        if out.returncode == 0 and out.stdout.strip():
            return Path(out.stdout.strip()).parent
    except (OSError, subprocess.SubprocessError):
        pass
    return HERE.parent


def default_saves_root() -> Path:
    env = os.environ.get("SR2_SAVES_ROOT")
    if env:
        return Path(env)
    return Path(os.environ.get("USERPROFILE", "~")).expanduser() / "AppData" / "LocalLow" / "MonomiPark" / \
        "SlimeRancher2" / "Steam"


class BackupError(Exception):
    pass


def _files(root: Path) -> dict[str, int]:
    return {str(p.relative_to(root)): p.stat().st_size for p in root.rglob("*") if p.is_file()}


def backup_saves(saves_root: Path | None = None, backup_root: Path | None = None,
                 game_dir: Path | None = None, tag: str = "autotest") -> Path:
    """Copies the saves to <backup_root>\\<yyyy-MM-dd_HHmmss>_<tag>\\ and checks the copy.

    Layout as deploy.ps1: one Steam account folder -> its files flat in the backup folder; several ->
    one subfolder each. The game folder's sr2mods_saves\\ (sidecars) and sr2mods.json go to
    <stamp>_<tag>_gamefolder\\ next to it, so the saves folder copy keeps deploy.ps1's layout.
    Raises BackupError if anything is missing; the caller must not go on then.
    """
    saves_root = Path(saves_root) if saves_root else default_saves_root()
    backup_root = Path(backup_root) if backup_root else main_checkout_root() / "save-backups"
    if not saves_root.is_dir():
        raise BackupError(f"saves folder not found: {saves_root}")
    id_dirs = sorted(p for p in saves_root.iterdir() if p.is_dir())
    if not id_dirs:
        raise BackupError(f"no account folders under {saves_root}")
    stamp = datetime.now().strftime("%Y-%m-%d_%H%M%S")
    dest = backup_root / f"{stamp}_{tag}"
    n = 1
    while dest.exists():  # two runs in the same second
        n += 1
        dest = backup_root / f"{stamp}_{tag}{n}"
    try:
        dest.mkdir(parents=True)
        if len(id_dirs) == 1:
            shutil.copytree(id_dirs[0], dest, dirs_exist_ok=True)
            pairs = [(id_dirs[0], dest)]
        else:
            pairs = []
            for d in id_dirs:
                shutil.copytree(d, dest / d.name)
                pairs.append((d, dest / d.name))
        for src, dst in pairs:
            want, got = _files(src), _files(dst)
            missing = [k for k, size in want.items() if got.get(k) != size]
            if missing:
                raise BackupError(f"backup incomplete, {len(missing)} file(s) differ, e.g. {missing[0]}")
        if sum(len(_files(dst)) for _, dst in pairs) == 0:
            raise BackupError("backup holds no files")
        if game_dir:
            side = backup_root / (dest.name + "_gamefolder")
            sidecars = Path(game_dir) / "sr2mods_saves"
            if sidecars.is_dir():
                shutil.copytree(sidecars, side / "sr2mods_saves")
            cfg = Path(game_dir) / "sr2mods.json"
            if cfg.is_file():
                side.mkdir(parents=True, exist_ok=True)
                shutil.copy2(cfg, side / "sr2mods.json")
    except OSError as e:
        raise BackupError(f"copy failed: {e}") from e
    return dest


# ---- the test context -----------------------------------------------------------------------------

class Test:
    """What a test's run(t) gets. Records steps, answers, screenshots and log excerpts for the report."""

    YES, NO, CANT = "Yes", "No", "Can't tell"

    def __init__(self, client: Client, case: str, module: Any, ask_timeout: float = 600.0):
        self.c = client
        self.case = case
        self.module = module
        self.ask_timeout = ask_timeout
        self.cur_step: int | None = None
        self.steps: list[dict] = []
        self.notes: list[str] = []
        self.uncovered_parts: list[str] = []
        self.answers: list[dict] = []
        self.screenshots: list[str] = []
        self.logs: list[dict] = []
        self._settings_before: dict[str, Any] = {}

    # -- bookkeeping
    def step(self, n: int, text: str = "") -> None:
        self.cur_step = n
        self.steps.append({"step": n, "text": text, "t": round(time.time(), 1)})
        print(f"    step {n}: {text}")

    def note(self, text: str) -> None:
        self.notes.append(text)
        print(f"    note: {text}")

    def uncovered(self, text: str) -> None:
        """A part of the case this test does not cover (e.g. needs a mod-free game); goes in the note."""
        self.uncovered_parts.append(text)

    def check(self, cond: Any, msg: str) -> None:
        if not cond:
            raise TestFail(f"step {self.cur_step}: {msg}")

    def eq(self, got: Any, want: Any, what: str) -> None:
        self.check(got == want, f"{what}: got {got!r}, want {want!r}")

    def needs_harness(self, *what: str) -> None:
        raise NeedsHarness("; ".join(what))

    # -- client passthroughs
    def state(self) -> dict:
        return self.c.state()

    def wait_until(self, pred, timeout: float = 10.0, what: str = "condition") -> Any:
        try:
            return self.c.wait_until(pred, timeout, what=what)
        except HarnessTimeout as e:
            raise TestFail(f"step {self.cur_step}: {e.error}") from None

    def find(self, type: str, **kw) -> list:
        return self.c.find(type, **kw)

    def teleport(self, pos=None, rot=None, to=None) -> Any:
        return self.c.teleport(pos, rot, to)

    def input(self, action: str | None = None, key: str | None = None, mode: str = "tap") -> Any:
        return self._wrap("input", lambda: self.c.input(action, key, mode))

    def _wrap(self, what: str, fn):
        try:
            return fn()
        except HarnessError as e:
            if e.unknown:
                raise NeedsHarness(f"{what}: {e.error}") from None
            raise

    def probe(self, name: str, **args) -> Any:
        return self._wrap(f"probe {name}", lambda: self.c.probe(name, **args))

    def exec(self, name: str, **args) -> Any:
        return self._wrap(f"exec {name}", lambda: self.c.exec(name, **args))

    # -- settings (restored after the case)
    def setting(self, key: str) -> Any:
        r = self.c.settings_get(key)
        return r.get("value") if isinstance(r, dict) and "value" in r else r

    def set_setting(self, key: str, value: Any) -> None:
        if key not in self._settings_before:
            self._settings_before[key] = self.setting(key)
        self.c.settings_set(key, value)

    def restore_settings(self) -> list[str]:
        failed = []
        for key, value in reversed(list(self._settings_before.items())):
            try:
                self.c.settings_set(key, value)
            except Exception as e:  # noqa: BLE001 - report every setting that could not go back
                failed.append(f"{key}={value!r}: {e}")
        self._settings_before.clear()
        return failed

    # -- logs
    def mark(self) -> Mark:
        return self.c.log_mark()

    def log_since(self, mark: Mark) -> list[str]:
        return self.c.log_since(mark)

    def wait_log(self, mark: Mark, pattern: str, timeout: float = 15.0) -> str:
        try:
            line = self.c.wait_log(mark, pattern, timeout)
        except HarnessTimeout:
            tail = self.c.log_since(mark)[-15:]
            self.logs.append({"step": self.cur_step, "expected": pattern, "lines": tail})
            raise TestFail(f"step {self.cur_step}: no log line /{pattern}/ in {timeout:g} s") from None
        self.logs.append({"step": self.cur_step, "matched": pattern, "lines": [line]})
        return line

    def no_log(self, mark: Mark, pattern: str) -> None:
        import re
        hits = [ln for ln in self.c.log_since(mark) if re.search(pattern, ln)]
        if hits:
            self.logs.append({"step": self.cur_step, "unexpected": pattern, "lines": hits[:10]})
        self.check(not hits, f"unexpected log line /{pattern}/: {hits[0] if hits else ''}")

    def no_faults(self, mark: Mark) -> None:
        bad = faults_in(self.c.log_since(mark), self.c.player_log_since(mark))
        if bad:
            self.logs.append({"step": self.cur_step, "faults": bad[:20]})
        self.check(not bad, f"{len(bad)} error / exception line(s) in the logs, first: {bad[0] if bad else ''}")

    # -- the user
    def screenshot(self, name: str | None = None) -> str:
        path = self._wrap("screenshot", lambda: self.c.screenshot(name or f"{self.case}_s{self.cur_step}"))
        if path:
            self.screenshots.append(path)
        return path

    def ask(self, question: str, options: tuple[str, ...] | list[str] = (YES, NO, CANT), shot: bool = True) -> str:
        """Asks the user in the game (screen edges flash). Returns the chosen label or "timeout"."""
        if shot:
            try:
                self.screenshot()
            except (HarnessError, NeedsHarness) as e:
                self.note(f"no screenshot: {e}")
        print(f"    ASK: {question} {list(options)}")
        r = self._wrap("ask", lambda: self.c.ask(question, list(options), self.case, self.cur_step,
                                                 self.ask_timeout))
        self.answers.append({"step": self.cur_step, "question": question, **r})
        print(f"    answer: {r['answer']}" + (f" ({r['note']})" if r.get("note") else ""))
        return r["answer"]

    def confirm(self, question: str) -> None:
        """Yes / No / Can't tell. No fails the case; Can't tell or no answer -> needs_user."""
        a = self.ask(question)
        if a == self.YES:
            return
        note = self.answers[-1].get("note") or ""
        if a == self.NO:
            raise TestFail(f"step {self.cur_step}: user says no: {question}" + (f" ({note})" if note else ""))
        raise NeedsUser(f"step {self.cur_step}: {question} -> {a}" + (f" ({note})" if note else ""))

    # -- games (slots 5 and 6 only; the client refuses the rest)
    def game_in_slot(self, slot: int) -> dict | None:
        for g in self.c.saves_list():
            if g.get("slot") == slot:
                return g
        return None

    def in_slot(self, slot: int, fresh: bool = False, name: str | None = None) -> dict:
        """Makes the game in `slot` (5 or 6) the one in play. fresh: start a new game there.

        A new game is started when the slot is empty. Returns state().
        """
        if slot not in SAFE_SLOTS:
            raise SlotGuardError(f"slot {slot} is not a test slot")
        st = self.state()
        if not fresh and st.get("in_game") and st.get("slot") == slot:
            return st
        if fresh or self.game_in_slot(slot) is None:
            if fresh and self.game_in_slot(slot) is not None:
                self.c.delete_slot(slot)
            self.c.new_game(slot, name or f"autotest {slot}")
        else:
            self.c.load_slot(slot)
        st = self.wait_until(lambda: (lambda s: s if s.get("in_game") and s.get("slot") == slot else None)(
            self.state()), 120, f"in game in slot {slot}")
        return st

    def save(self) -> dict:
        return self.c.save()

    def reload(self) -> dict:
        """Quit to the title without saving and load the same slot's newest save again."""
        slot = self.state().get("slot")
        if slot not in SAFE_SLOTS:
            raise SlotGuardError(f"reload refused: slot {slot!r}")
        self.c.quit_to_title()
        self.c.load_slot(slot)
        return self.wait_until(lambda: (lambda s: s if s.get("in_game") and s.get("slot") == slot else None)(
            self.state()), 120, f"back in game in slot {slot}")

    # -- generic wave-1 probes and actions (names in one place; rename here if the harness differs)
    def give_newbucks(self, amount: int) -> Any:
        return self.exec("give_newbucks", amount=amount)

    def set_time(self, hour: float | None = None, add_hours: float | None = None) -> Any:
        a: dict = {}
        if hour is not None:
            a["hour"] = hour
        if add_hours is not None:
            a["add_hours"] = add_hours
        return self.exec("set_time", **a)

    def spawn(self, ident: str, count: int = 1, pos=None) -> Any:
        a: dict = {"id": ident, "count": count}
        if pos is not None:
            a["pos"] = list(pos)
        return self.exec("spawn", **a)

    def plots(self) -> list[dict]:
        r = self.probe("plots")
        return list(r.get("plots", []) if isinstance(r, dict) else r or [])

    def open_plot_menu(self, plot_id: str) -> Any:
        return self.exec("plot_menu", plot=plot_id)

    def set_upgrade_level(self, upgrade: str, level: int) -> Any:
        return self.exec("sandbox.upgrade_level", id=upgrade, level=level)


# ---- runner ---------------------------------------------------------------------------------------

def load_test(case: str):
    path = TESTS_DIR / f"{case}.py"
    if not path.is_file():
        return None
    spec = importlib.util.spec_from_file_location(f"autotest_case_{case}", path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    if not callable(getattr(mod, "run", None)):
        raise RuntimeError(f"{path.name} has no run(t)")
    return mod


def run_case(client: Client, case: str, ask_timeout: float) -> dict:
    t0 = time.time()
    res: dict = {"status": "error", "note": ""}
    try:
        mod = load_test(case)
    except Exception as e:  # noqa: BLE001
        return {"status": "error", "note": f"test file broken: {e}", "duration_s": 0}
    if mod is None:
        return {"status": "skipped", "note": "no test file yet", "duration_s": 0}
    print(f"\n== {case}: {getattr(mod, 'TITLE', '')}  [{getattr(mod, 'WHERE', '')}]")
    t = Test(client, case, mod, ask_timeout)
    try:
        mod.run(t)
        res = {"status": "pass", "note": "ok"}
    except TestFail as e:
        res = {"status": "fail", "note": str(e)}
    except NeedsHarness as e:
        res = {"status": "skipped", "note": f"needs harness: {e}"}
    except NeedsUser as e:
        res = {"status": "needs_user", "note": str(e)}
    except SlotGuardError as e:
        res = {"status": "error", "note": f"slot guard: {e}"}
    except HarnessError as e:
        res = {"status": "error", "note": f"harness: {e}"}
    except ConnectError as e:
        res = {"status": "error", "note": f"connection: {e}", "connection_lost": True}
    except Exception as e:  # noqa: BLE001 - a broken test must not stop the run
        res = {"status": "error", "note": f"{type(e).__name__}: {e}", "traceback": traceback.format_exc()}
    finally:
        if not res.get("connection_lost"):
            try:
                failed = t.restore_settings()
                if failed:
                    res["settings_not_restored"] = failed
            except Exception as e:  # noqa: BLE001
                res["settings_not_restored"] = [str(e)]
    if t.uncovered_parts:
        res["note"] += " | not covered: " + "; ".join(t.uncovered_parts)
    if t.notes:
        res["notes"] = t.notes
    res.update({"steps": t.steps, "answers": t.answers, "screenshots": t.screenshots, "logs": t.logs,
                "duration_s": round(time.time() - t0, 1)})
    print(f"   -> {res['status']}: {res['note']}")
    return res


def connect(args) -> Client:
    return Client(pipe=args.pipe, game_dir=args.game_dir, player_log=args.player_log,
                  connect_timeout=args.connect_timeout, verbose=args.verbose)


def check_start_state(client: Client) -> str | None:
    """Reason to refuse, or None. A game outside slots 5 and 6 in play: never drive it."""
    st = client.state()
    if st.get("in_game") and st.get("slot") not in SAFE_SLOTS:
        return (f"a game in slot {st.get('slot')} ({st.get('display_name') or st.get('game_name')}) is loaded. "
                "Save it yourself and quit to the title screen (or load slot 5 or 6), then run again.")
    return None


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("cases", nargs="*", help="case ids (tests/<id>.py)")
    ap.add_argument("--all-skipped", action="store_true", help="every round 16 skipped case")
    ap.add_argument("--list", action="store_true", help="list the cases with a test file and exit")
    ap.add_argument("--pipe", default=None, help=f"pipe name (default {sr2test.PIPE_NAME}, env SR2TEST_PIPE)")
    ap.add_argument("--game-dir", default=None, help="game folder (sr2mods.log, sidecars; env SR2_GAME_DIR)")
    ap.add_argument("--player-log", default=None, help="Player.log path (env SR2_PLAYER_LOG)")
    ap.add_argument("--saves-root", default=None, help="the saves' Steam folder (env SR2_SAVES_ROOT)")
    ap.add_argument("--backup-root", default=None, help="where backups go (default <main checkout>\\save-backups)")
    ap.add_argument("--results-dir", default=str(RESULTS_DIR))
    ap.add_argument("--ask-timeout", type=float, default=600.0, help="seconds the user has per question")
    ap.add_argument("--connect-timeout", type=float, default=5.0)
    ap.add_argument("-v", "--verbose", action="store_true", help="print every request and reply")
    args = ap.parse_args(argv)

    if args.list:
        for p in sorted(TESTS_DIR.glob("*.py")):
            mod = load_test(p.stem)
            print(f"{p.stem:8} {getattr(mod, 'TITLE', '')}  [{getattr(mod, 'WHERE', '')}]")
        return 0
    cases = list(dict.fromkeys(args.cases + (SKIPPED_R16 if args.all_skipped else [])))
    if not cases:
        ap.error("name case ids or --all-skipped")

    game_dir = Path(args.game_dir) if args.game_dir else sr2test.default_game_dir()
    try:
        backup = backup_saves(args.saves_root, args.backup_root, game_dir)
    except BackupError as e:
        print(f"REFUSED: save backup failed: {e}", file=sys.stderr)
        return 2
    print(f"Saves backed up to {backup}")

    try:
        client = connect(args)
    except ConnectError as e:
        print(f"Cannot connect: {e}", file=sys.stderr)
        return 3
    started = datetime.now()
    report: dict = {"started": started.isoformat(timespec="seconds"), "backup": str(backup), "cases": {}}
    stopped = "run stopped"
    try:
        report["ping"] = client.ping()
        why = check_start_state(client)
        if why:
            print(f"REFUSED: {why}", file=sys.stderr)
            stopped = f"refused: {why}"
            return 4
        for case in cases:
            res = run_case(client, case, args.ask_timeout)
            report["cases"][case] = res
            if res.get("connection_lost"):
                client.close()
                try:
                    client = connect(args)
                    why = check_start_state(client)
                    if why:
                        print(f"Stopped: {why}", file=sys.stderr)
                        stopped = f"stopped: {why}"
                        break
                except ConnectError as e:
                    print(f"Lost the harness and cannot reconnect: {e}", file=sys.stderr)
                    stopped = f"harness lost: {e}"
                    break
    finally:
        client.close()
        for case in cases:
            report["cases"].setdefault(case, {"status": "error", "note": f"not run ({stopped})"})
        report["finished"] = datetime.now().isoformat(timespec="seconds")
        out_dir = Path(args.results_dir)
        out_dir.mkdir(parents=True, exist_ok=True)
        out = out_dir / f"{started.strftime('%Y-%m-%d_%H%M%S')}.json"
        n = 1
        while out.exists():
            n += 1
            out = out_dir / f"{started.strftime('%Y-%m-%d_%H%M%S')}_{n}.json"
        out.write_text(json.dumps(report, indent=2), encoding="utf-8")
        counts = {s: sum(1 for r in report["cases"].values() if r["status"] == s) for s in STATUSES}
        print("\nSummary: " + ", ".join(f"{n} {s}" for s, n in counts.items() if n))
        for case, r in report["cases"].items():
            print(f"  {case:8} {r['status']:10} {r['note'][:110]}")
        print(f"Results: {out}")
    return 0 if all(r["status"] in ("pass", "skipped") for r in report["cases"].values()) else 1


if __name__ == "__main__":
    sys.exit(main())
