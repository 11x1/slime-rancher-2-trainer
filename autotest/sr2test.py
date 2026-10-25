"""sr2test: client for the sr2mods test harness (modules/autotest in the DLL).

Python 3 standard library only. Windows only (the harness is a Win32 named pipe).

Protocol (contract in .claude/notes/autotest.md, section A): one JSON object per line, UTF-8.
  request  {"id": 7, "cmd": "state", "args": {...}}
  reply    {"id": 7, "ok": true, "result": {...}}  or  {"id": 7, "ok": false, "error": "text"}
  event    {"event": "log", "line": "..."}  /  {"event": "answer", ...}

Save safety: the client refuses, before anything is sent, every load / new game / delete for a slot
other than 5 or 6, and every save unless the game in play is in slot 5 or 6. The DLL has its own
guard; this is the second one.

Usage:
    from sr2test import Client
    with Client() as c:
        print(c.state())
"""
from __future__ import annotations

import ctypes
import json
import os
import re
import sys
import time
from ctypes import wintypes
from pathlib import Path
from typing import Any, Callable, Iterable

PIPE_NAME = r"\\.\pipe\sr2mods-autotest"
SAFE_SLOTS = (5, 6)
DEFAULT_TIMEOUT = 10.0
# Commands that load, start or delete a game: their `slot` arg must be 5 or 6.
SLOT_COMMANDS = ("load_slot", "new_game", "delete_slot")
# exec action names that look like they save, load or delete a game: never sent. Saving goes
# through save() (guarded), loading through load_slot() / new_game().
_BLOCKED_ACTION = re.compile(r"(^|[._])(save|autosave|load|delete_game|delete_slot|delete_save)($|[._])", re.I)
# Harness actions whose names match that but touch no game save.
_SAFE_ACTIONS = ("place_save",)  # writes sr2mods_autotest\places.json (teleport targets)
# Commands that take long in the game; the client waits this long for the reply. The value is also
# sent as args.timeout_s so the harness can use it (needs harness: honour timeout_s).
LONG_COMMANDS = {"load_slot": 120.0, "new_game": 180.0, "quit_to_title": 60.0, "save": 30.0,
                 "delete_slot": 30.0}


def default_game_dir() -> Path:
    env = os.environ.get("SR2_GAME_DIR")
    if env:
        return Path(env)
    pf86 = os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")
    return Path(pf86) / "Steam" / "steamapps" / "common" / "Slime Rancher 2"


def default_player_log() -> Path:
    env = os.environ.get("SR2_PLAYER_LOG")
    if env:
        return Path(env)
    return Path(os.environ.get("USERPROFILE", "~")).expanduser() / "AppData" / "LocalLow" / "MonomiPark" / \
        "SlimeRancher2" / "Player.log"


# ---- errors -------------------------------------------------------------------------------------

class HarnessError(Exception):
    """The harness replied ok:false (the message is its error text)."""

    def __init__(self, cmd: str, error: str):
        super().__init__(f"{cmd}: {error}")
        self.cmd = cmd
        self.error = error

    @property
    def unknown(self) -> bool:
        """True if the error says the command, probe or action does not exist (yet)."""
        return bool(re.search(r"unknown|not registered|no such|not found: (probe|action)", self.error, re.I))


class HarnessTimeout(HarnessError):
    pass


class SlotGuardError(Exception):
    """The client refused a request that could touch save slots 1-4."""


class ConnectError(Exception):
    pass


# Raised by tests (through the runner's Test object) to end a case with that status.
class TestFail(AssertionError):
    """A check failed: the case fails."""


class NeedsHarness(Exception):
    """A step needs a probe, action or command the harness does not have yet: the case is skipped."""


class NeedsUser(Exception):
    """The user could not judge, or did not answer in time: the case needs a person."""


# ---- Win32 pipe transport -----------------------------------------------------------------------

if sys.platform == "win32":
    _k32 = ctypes.WinDLL("kernel32", use_last_error=True)
    _k32.CreateFileW.restype = wintypes.HANDLE
    _k32.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, ctypes.c_void_p,
                                 wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE]
    _k32.WaitNamedPipeW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD]
    _k32.WaitNamedPipeW.restype = wintypes.BOOL
    _k32.PeekNamedPipe.argtypes = [wintypes.HANDLE, ctypes.c_void_p, wintypes.DWORD, ctypes.c_void_p,
                                   ctypes.POINTER(wintypes.DWORD), ctypes.c_void_p]
    _k32.PeekNamedPipe.restype = wintypes.BOOL
    _k32.ReadFile.argtypes = [wintypes.HANDLE, ctypes.c_void_p, wintypes.DWORD,
                              ctypes.POINTER(wintypes.DWORD), ctypes.c_void_p]
    _k32.ReadFile.restype = wintypes.BOOL
    _k32.WriteFile.argtypes = [wintypes.HANDLE, ctypes.c_void_p, wintypes.DWORD,
                               ctypes.POINTER(wintypes.DWORD), ctypes.c_void_p]
    _k32.WriteFile.restype = wintypes.BOOL
    _k32.CloseHandle.argtypes = [wintypes.HANDLE]
    _k32.CloseHandle.restype = wintypes.BOOL

_INVALID_HANDLE = ctypes.c_void_p(-1).value
_GENERIC_RW = 0x80000000 | 0x40000000
_OPEN_EXISTING = 3
_ERROR_FILE_NOT_FOUND = 2
_ERROR_BROKEN_PIPE = 109
_ERROR_PIPE_BUSY = 231


class PipeTransport:
    """Byte stream over a Win32 named pipe (client end).

    One synchronous handle, used from one thread only: a blocked ReadFile on a synchronous pipe
    handle would block WriteFile from another thread, so reads only happen when PeekNamedPipe
    says data is waiting.
    """

    def __init__(self, name: str = PIPE_NAME, connect_timeout: float = 5.0):
        if sys.platform != "win32":
            raise ConnectError("the harness pipe exists on Windows only")
        self.name = name
        deadline = time.monotonic() + connect_timeout
        while True:
            h = _k32.CreateFileW(name, _GENERIC_RW, 0, None, _OPEN_EXISTING, 0, None)
            if h and h != _INVALID_HANDLE:
                self._h = h
                return
            err = ctypes.get_last_error()
            if time.monotonic() >= deadline:
                if err == _ERROR_FILE_NOT_FOUND:
                    raise ConnectError(f"{name} not found: is the game running with Debug > Test harness on?")
                if err == _ERROR_PIPE_BUSY:
                    raise ConnectError(f"{name} is busy: another client is connected")
                raise ConnectError(f"cannot open {name} (Win32 error {err})")
            if err == _ERROR_PIPE_BUSY:
                _k32.WaitNamedPipeW(name, 500)
            else:
                time.sleep(0.2)

    def available(self) -> int:
        n = wintypes.DWORD(0)
        if not _k32.PeekNamedPipe(self._h, None, 0, None, ctypes.byref(n), None):
            err = ctypes.get_last_error()
            raise ConnectError("the harness closed the pipe" if err == _ERROR_BROKEN_PIPE
                               else f"PeekNamedPipe failed (Win32 error {err})")
        return n.value

    def read(self, n: int) -> bytes:
        buf = ctypes.create_string_buffer(n)
        got = wintypes.DWORD(0)
        if not _k32.ReadFile(self._h, buf, n, ctypes.byref(got), None):
            raise ConnectError(f"ReadFile failed (Win32 error {ctypes.get_last_error()})")
        return buf.raw[:got.value]

    def write(self, data: bytes) -> None:
        view = memoryview(data)
        while view:
            done = wintypes.DWORD(0)
            chunk = bytes(view[:65536])
            if not _k32.WriteFile(self._h, chunk, len(chunk), ctypes.byref(done), None):
                raise ConnectError(f"WriteFile failed (Win32 error {ctypes.get_last_error()})")
            view = view[done.value:]

    def close(self) -> None:
        if getattr(self, "_h", None):
            _k32.CloseHandle(self._h)
            self._h = None


# ---- log marks ----------------------------------------------------------------------------------

class Mark:
    """A point in sr2mods.log and Player.log. Lines after it are what a step caused."""

    def __init__(self, text: str, sr2_offset: int, player_offset: int):
        self.text = text
        self.sr2_offset = sr2_offset
        self.player_offset = player_offset

    def __repr__(self) -> str:
        return f"Mark({self.text!r})"


def _file_size(p: Path) -> int:
    try:
        return p.stat().st_size
    except OSError:
        return 0


def _read_from(p: Path, offset: int) -> str:
    try:
        with open(p, "rb") as f:
            size = f.seek(0, 2)
            if offset > size:  # the log was recreated (game restart): read it all
                offset = 0
            f.seek(offset)
            return f.read().decode("utf-8", "replace")
    except OSError:
        return ""


# ---- the client ---------------------------------------------------------------------------------

class Client:
    def __init__(self, pipe: str | None = None, game_dir: Path | str | None = None,
                 player_log: Path | str | None = None, connect_timeout: float = 5.0,
                 transport: Any = None, verbose: bool = False):
        self.pipe = pipe or os.environ.get("SR2TEST_PIPE", PIPE_NAME)
        self.game_dir = Path(game_dir) if game_dir else default_game_dir()
        self.sr2_log = self.game_dir / "sr2mods.log"
        self.player_log = Path(player_log) if player_log else default_player_log()
        self.verbose = verbose
        self._t = transport or PipeTransport(self.pipe, connect_timeout)
        self._buf = b""
        self._next_id = 1
        self._replies: dict[int, dict] = {}
        self.events: list[dict] = []        # pushed events not consumed yet
        self.log_lines: list[str] = []      # lines from {"event":"log"} after subscribe_log()
        self.sent: list[dict] = []          # every request sent (journal, for the runner's report)

    # -- context manager
    def __enter__(self) -> "Client":
        return self

    def __exit__(self, *exc) -> None:
        self.close()

    def close(self) -> None:
        if self._t:
            self._t.close()
            self._t = None

    # -- wire
    def _pump(self, wait: float) -> bool:
        """Reads whatever is waiting (up to `wait` seconds for the first byte). True if a line came."""
        deadline = time.monotonic() + wait
        got_line = False
        while True:
            n = self._t.available()
            if n:
                self._buf += self._t.read(n)
                while b"\n" in self._buf:
                    line, self._buf = self._buf.split(b"\n", 1)
                    line = line.strip()
                    if line:
                        self._dispatch(line)
                        got_line = True
                if got_line:
                    return True
                continue
            if time.monotonic() >= deadline:
                return got_line
            time.sleep(0.01)

    def _dispatch(self, line: bytes) -> None:
        try:
            msg = json.loads(line.decode("utf-8"))
        except (ValueError, UnicodeDecodeError):
            if self.verbose:
                print(f"[sr2test] bad line from harness: {line[:200]!r}", file=sys.stderr)
            return
        if "event" in msg:
            if msg.get("event") == "log" and "line" in msg:
                self.log_lines.append(msg["line"])
            else:
                self.events.append(msg)
        elif "id" in msg:
            self._replies[msg["id"]] = msg

    def _guard(self, cmd: str, args: dict) -> None:
        if cmd in SLOT_COMMANDS:
            slot = args.get("slot")
            if not isinstance(slot, int) or isinstance(slot, bool) or slot not in SAFE_SLOTS:
                raise SlotGuardError(f"{cmd} refused: slot {slot!r} (only slots 5 and 6 are for tests)")
        else:
            inner = args.get("args") if cmd in ("exec", "probe") and isinstance(args.get("args"), dict) else {}
            for d in (args, inner):
                if "slot" in d and d["slot"] not in SAFE_SLOTS:
                    raise SlotGuardError(f"{cmd} refused: slot {d['slot']!r} in the args (only 5 and 6)")
        if cmd == "exec" and args.get("name") not in _SAFE_ACTIONS and _BLOCKED_ACTION.search(str(args.get("name", ""))):
            raise SlotGuardError(f"exec {args.get('name')!r} refused: save / load / delete only through "
                                 "save(), load_slot(), new_game() and delete_slot()")
        if cmd == "save":
            st = self.call("state")
            slot = st.get("slot")
            if not st.get("in_game") or slot not in SAFE_SLOTS:
                raise SlotGuardError(f"save refused: the game in play is in slot {slot!r} "
                                     f"(in game: {st.get('in_game')}); only slots 5 and 6 may be saved")

    def call(self, cmd: str, args: dict | None = None, timeout: float | None = None) -> Any:
        """Sends one command and returns its result; raises HarnessError on ok:false."""
        args = dict(args or {})
        self._guard(cmd, args)
        if timeout is None:
            timeout = LONG_COMMANDS.get(cmd, DEFAULT_TIMEOUT)
        if cmd in LONG_COMMANDS or timeout != DEFAULT_TIMEOUT:
            args.setdefault("timeout_s", timeout)
        rid = self._next_id
        self._next_id += 1
        req = {"id": rid, "cmd": cmd, "args": args}
        self.sent.append(req)
        if self.verbose:
            print(f"[sr2test] -> {json.dumps(req)}", file=sys.stderr)
        self._t.write((json.dumps(req, separators=(",", ":")) + "\n").encode("utf-8"))
        # The harness has its own timeout per command; wait a bit longer for its timeout reply.
        deadline = time.monotonic() + timeout + 5.0
        while rid not in self._replies:
            left = deadline - time.monotonic()
            if left <= 0:
                raise HarnessTimeout(cmd, f"no reply in {timeout + 5.0:.0f} s")
            self._pump(min(left, 0.5))
        rep = self._replies.pop(rid)
        if self.verbose:
            print(f"[sr2test] <- {json.dumps(rep)[:500]}", file=sys.stderr)
        if not rep.get("ok"):
            err = str(rep.get("error", "error"))
            if err == "timeout":
                raise HarnessTimeout(cmd, "timeout (in the harness)")
            raise HarnessError(cmd, err)
        return rep.get("result")

    def poll_events(self, wait: float = 0.0) -> list[dict]:
        """Returns and clears the pushed events, after reading for up to `wait` seconds."""
        self._pump(wait)
        ev, self.events = self.events, []
        return ev

    # -- contract commands
    def ping(self) -> dict:
        return self.call("ping")

    def state(self) -> dict:
        return self.call("state") or {}

    def settings_get(self, key: str) -> Any:
        """key = "<module>.<key>" (the sr2mods.json path under "modules")."""
        return self.call("settings_get", {"key": key})

    def settings_set(self, key: str, value: Any) -> Any:
        return self.call("settings_set", {"key": key, "value": value})

    def saves_list(self) -> list[dict]:
        r = self.call("saves_list")
        if isinstance(r, dict):
            r = r.get("games") or r.get("saves") or []
        return list(r or [])

    def load_slot(self, slot: int) -> dict:
        return self.call("load_slot", {"slot": slot})

    def new_game(self, slot: int, name: str | None = None) -> dict:
        a: dict = {"slot": slot}
        if name:
            a["name"] = name
        return self.call("new_game", a)

    def save(self) -> dict:
        return self.call("save")

    def quit_to_title(self) -> dict:
        return self.call("quit_to_title")

    def delete_slot(self, slot: int) -> dict:
        return self.call("delete_slot", {"slot": slot})

    def teleport(self, pos: Iterable[float] | None = None, rot: Any = None, to: str | None = None) -> Any:
        a: dict = {}
        if pos is not None:
            a["pos"] = list(pos)
        if rot is not None:
            a["rot"] = rot
        if to:
            a["to"] = to
        return self.call("teleport", a)

    def find(self, type: str, name: str | None = None, near: Iterable[float] | None = None,
             radius: float | None = None, limit: int | None = None, fields: list[str] | None = None) -> list:
        a: dict = {"type": type}
        if name:
            a["name"] = name
        if near is not None:
            a["near"] = list(near)
        if radius is not None:
            a["radius"] = radius
        if limit is not None:
            a["limit"] = limit
        if fields:
            a["fields"] = fields
        r = self.call("find", a)
        if isinstance(r, dict):
            r = r.get("objects", [])
        return list(r or [])

    def probe(self, name: str, timeout: float | None = None, **args) -> Any:
        return self.call("probe", {"name": name, "args": args}, timeout)

    def exec(self, name: str, timeout: float | None = None, **args) -> Any:
        return self.call("exec", {"name": name, "args": args}, timeout)

    def input(self, action: str | None = None, key: str | None = None, mode: str = "tap") -> Any:
        if mode not in ("press", "release", "tap"):
            raise ValueError("mode is press, release or tap")
        a: dict = {mode: True}
        if action:
            a["action"] = action
        if key:
            a["key"] = key
        return self.call("input", a)

    def screenshot(self, name: str | None = None) -> str:
        r = self.call("screenshot", {"name": name} if name else {}, timeout=15.0)
        if isinstance(r, dict):
            return str(r.get("path", ""))
        return str(r or "")

    def shot(self, name: str | None = None, *, overlay: bool = False, hide_hud: bool = False,
             hide_minimap: bool = False, hide_harness: bool = True, menu: dict | None = None,
             settle_ms: int | None = None) -> dict:
        """A screenshot with the showcase options; returns {path, width, height}.

        overlay: after the mod's overlay drew (else the game's frame only, which never has the mod's
        ImGui bits). hide_hud: the game HUD (and the minimap under it) off for the shot.
        hide_minimap: only the minimap and its collectibles tracker. hide_harness: no "Test harness
        connected" tag / question panel in an overlay shot. menu {"cat", "tab"}: the mod menu open at
        that page for the shot (closed again after if it was closed). All restored after the frame.
        """
        a: dict = {"overlay": overlay, "hide_hud": hide_hud, "hide_minimap": hide_minimap,
                   "hide_harness": hide_harness}
        if name:
            a["name"] = name
        if menu:
            a["menu"] = dict(menu)
        if settle_ms is not None:
            a["settle_ms"] = settle_ms
        r = self.call("screenshot", a, timeout=20.0)
        return r if isinstance(r, dict) else {"path": str(r or "")}

    def overlay(self, open: bool = True, cat: str | None = None, tab: str | int | None = None) -> dict:
        """Opens (at a page: category id or title, tab title or index) or closes the mod menu."""
        a: dict = {"open": open}
        if cat is not None:
            a["cat"] = cat
        if tab is not None:
            a["tab"] = tab
        return self.call("overlay", a) or {}

    def ask(self, question: str, options: list[str], case: str | None = None, step: int | None = None,
            timeout_s: float = 600.0) -> dict:
        """Asks the user in the game. Returns {"answer": label | "timeout", "note": str}."""
        if not 2 <= len(options) <= 4:
            raise ValueError("ask takes 2-4 options")
        a: dict = {"question": question, "options": list(options), "timeout_s": timeout_s}
        if case:
            a["case"] = case
        if step is not None:
            a["step"] = step
        r = self.call("ask", a, timeout=timeout_s + 10.0)
        ans = _parse_answer(r)
        if ans is None:  # the harness answered "pending": wait for the answer event
            deadline = time.monotonic() + timeout_s + 10.0
            while ans is None and time.monotonic() < deadline:
                self._pump(0.5)
                for i, ev in enumerate(self.events):
                    if ev.get("event") == "answer":
                        ans = _parse_answer(self.events.pop(i))
                        break
            if ans is None:
                ans = {"answer": "timeout", "note": ""}
        return ans

    def cancel_ask(self) -> Any:
        return self.call("cancel_ask")

    def subscribe_log(self) -> Any:
        return self.call("subscribe_log")

    # -- helpers
    def wait_until(self, pred: Callable[[], Any], timeout: float = 10.0, interval: float = 0.25,
                   what: str = "condition") -> Any:
        """Calls pred until it returns something truthy; returns that, raises HarnessTimeout after timeout."""
        deadline = time.monotonic() + timeout
        while True:
            v = pred()
            if v:
                return v
            if time.monotonic() >= deadline:
                raise HarnessTimeout("wait_until", f"{what} not reached in {timeout:g} s")
            self._pump(interval)

    def log_mark(self) -> Mark:
        """Writes a marker to sr2mods.log; log_since(mark) returns what came after it."""
        sr2_off = _file_size(self.sr2_log)
        pl_off = _file_size(self.player_log)
        r = self.call("log_mark")
        text = r.get("mark", "") if isinstance(r, dict) else str(r or "")
        return Mark(text, sr2_off, pl_off)

    def log_since(self, mark: Mark) -> list[str]:
        """sr2mods.log lines after the marker (the marker line itself excluded)."""
        text = _read_from(self.sr2_log, mark.sr2_offset)
        if mark.text:
            i = text.rfind(mark.text)
            if i >= 0:
                nl = text.find("\n", i)
                text = text[nl + 1:] if nl >= 0 else ""
        return [ln for ln in text.splitlines() if ln.strip()]

    def player_log_since(self, mark: Mark) -> list[str]:
        """Player.log lines written since the mark was taken."""
        return [ln for ln in _read_from(self.player_log, mark.player_offset).splitlines() if ln.strip()]

    def wait_log(self, mark: Mark, pattern: str, timeout: float = 10.0) -> str:
        """Waits until a sr2mods.log line after `mark` matches the regex; returns the line."""
        rx = re.compile(pattern)

        def hit():
            for ln in self.log_since(mark):
                if rx.search(ln):
                    return ln
            return None
        return self.wait_until(hit, timeout, 0.25, f"log line /{pattern}/")


def _parse_answer(r: Any) -> dict | None:
    if isinstance(r, str):
        return {"answer": r, "note": ""}
    if isinstance(r, dict):
        if r.get("pending"):
            return None
        for k in ("answer", "choice", "label", "option"):
            if k in r:
                return {"answer": str(r[k]), "note": str(r.get("note") or "")}
    return {"answer": "timeout", "note": ""} if r is None else {"answer": str(r), "note": ""}


# ---- faults in the logs -------------------------------------------------------------------------

_SR2_FAULT = re.compile(r"\[ERROR\s*\]|FAULTED|exception in hook")
_PLAYER_FAULT = re.compile(r"^(NullReferenceException|.*Exception:|.*Exception\b.*at )|Crash!!!")


def faults_in(sr2_lines: list[str], player_lines: list[str] | None = None) -> list[str]:
    """Error-level sr2mods.log lines, module faults, and managed exceptions in Player.log."""
    out = [ln for ln in sr2_lines if _SR2_FAULT.search(ln)]
    for ln in player_lines or []:
        if _PLAYER_FAULT.search(ln):
            out.append("Player.log: " + ln)
    return out
