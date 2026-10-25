"""A fake harness: speaks the autotest pipe protocol with a small simulated game.

    python autotest/mock_server.py                    # serve \\\\.\\pipe\\sr2mods-autotest-mock
    python autotest/mock_server.py --selftest         # test the client, runner and cleanup against it

Run the runner against a standalone mock:
    python autotest/mock_server.py --game-dir %TEMP%\\sr2mock
    python autotest/run.py --pipe \\\\.\\pipe\\sr2mods-autotest-mock --game-dir %TEMP%\\sr2mock ^
        --saves-root <a folder with a fake account folder> --backup-root %TEMP%\\sr2mock-backups r15sl5

The simulated game knows what the five pilot tests touch (custom upgrades, vacpack, drone stations,
Sandbox weather, corral radios) and writes the log lines the real mod writes. Slots 1-4 hold fake
"user" games; the mock records any request that would touch them in `violations`, and refuses it
like the real harness.
"""
from __future__ import annotations

import argparse
import copy
import ctypes
import json
import os
import shutil
import struct
import sys
import tempfile
import threading
import time
import uuid
import zlib
from ctypes import wintypes
from datetime import datetime
from pathlib import Path

HERE = Path(__file__).resolve().parent
MOCK_PIPE = r"\\.\pipe\sr2mods-autotest-mock"
SAFE = (5, 6)

# ---- Win32 pipe server ------------------------------------------------------------------------------

if sys.platform == "win32":
    _k32 = ctypes.WinDLL("kernel32", use_last_error=True)
    _k32.CreateNamedPipeW.restype = wintypes.HANDLE
    _k32.CreateNamedPipeW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, wintypes.DWORD,
                                      wintypes.DWORD, wintypes.DWORD, wintypes.DWORD, ctypes.c_void_p]
    _k32.ConnectNamedPipe.argtypes = [wintypes.HANDLE, ctypes.c_void_p]
    _k32.ConnectNamedPipe.restype = wintypes.BOOL
    _k32.DisconnectNamedPipe.argtypes = [wintypes.HANDLE]
    _k32.DisconnectNamedPipe.restype = wintypes.BOOL
    _k32.ReadFile.argtypes = [wintypes.HANDLE, ctypes.c_void_p, wintypes.DWORD,
                              ctypes.POINTER(wintypes.DWORD), ctypes.c_void_p]
    _k32.ReadFile.restype = wintypes.BOOL
    _k32.WriteFile.argtypes = [wintypes.HANDLE, ctypes.c_void_p, wintypes.DWORD,
                               ctypes.POINTER(wintypes.DWORD), ctypes.c_void_p]
    _k32.WriteFile.restype = wintypes.BOOL
    _k32.FlushFileBuffers.argtypes = [wintypes.HANDLE]
    _k32.CloseHandle.argtypes = [wintypes.HANDLE]

_PIPE_ACCESS_DUPLEX = 0x3
_PIPE_REJECT_REMOTE_CLIENTS = 0x8
_ERROR_PIPE_CONNECTED = 535
_INVALID = ctypes.c_void_p(-1).value


class PipeServer:
    """One client at a time; each request line is handled by `handler(msg) -> (reply, events)`."""

    def __init__(self, name: str, handler):
        self.name = name
        self.handler = handler
        self.connections = 0
        self._h = _k32.CreateNamedPipeW(name, _PIPE_ACCESS_DUPLEX, _PIPE_REJECT_REMOTE_CLIENTS, 1, 65536, 65536,
                                        0, None)
        if not self._h or self._h == _INVALID:
            raise OSError(f"CreateNamedPipeW failed ({ctypes.get_last_error()})")

    def _write(self, obj) -> bool:
        data = (json.dumps(obj, separators=(",", ":")) + "\n").encode("utf-8")
        done = wintypes.DWORD(0)
        return bool(_k32.WriteFile(self._h, data, len(data), ctypes.byref(done), None))

    def serve_forever(self) -> None:
        while True:
            ok = _k32.ConnectNamedPipe(self._h, None)
            if not ok and ctypes.get_last_error() != _ERROR_PIPE_CONNECTED:
                time.sleep(0.05)
                continue
            self.connections += 1
            buf = b""
            while True:
                chunk = ctypes.create_string_buffer(4096)
                got = wintypes.DWORD(0)
                if not _k32.ReadFile(self._h, chunk, 4096, ctypes.byref(got), None):
                    break  # client gone
                buf += chunk.raw[:got.value]
                alive = True
                while b"\n" in buf:
                    line, buf = buf.split(b"\n", 1)
                    if not line.strip():
                        continue
                    try:
                        msg = json.loads(line.decode("utf-8"))
                    except ValueError:
                        continue
                    reply, events = self.handler(msg)
                    for ev in events:
                        alive &= self._write(ev)
                    alive &= self._write(reply)
                if not alive:
                    break
            _k32.DisconnectNamedPipe(self._h)


# ---- the simulated game -------------------------------------------------------------------------------

LABELS = {
    "slime_launcher": ["Slime Launcher I", "Slime Launcher II", "Slime Launcher III"],
    "drone_programs": ["Drone Program Scheduler"],
    "shadow_catcher": ["Shadow Catcher"],
}
TANK = [20, 30, 40, 50, 60, 70, 80, 90, 100, 125, 150, 175, 200]  # max per slot by Tank Booster level
SONGS = ["Ranch Morning", "Plort Market", "Ember Valley", "Starlight Strand", "Powderfall Bluffs"]
EJECT = 30.0
FACTOR = [1.0, 1.2, 1.4, 1.6]
# The mod menu's categories and tabs (ui/menu_layout.cpp), for screenshot {menu} / overlay {cat, tab}.
PAGES = {"ranch": ("Ranch", ["Upgrades", "Slime DJ"]), "movement": ("Movement", ["Hl2 Movement"]),
         "hud": ("HUD", ["Minimap", "Collectibles", "Full Map", "Speed Panel"]),
         "world": ("World", ["Spawn Scaling", "Wall Markers"]), "misc": ("Misc", ["General"]),
         "sandbox": ("Sandbox", ["General", "Vacpack", "Spawn & Craft", "Upgrades", "Weather"]),
         "profiles": ("Profiles", ["Profiles"]), "settings": ("Settings", ["General"])}


def tiny_png(w: int = 16, h: int = 9) -> bytes:
    """A real (small, plain) PNG, so the gallery and image tools can open the mock's screenshots."""
    raw = b"".join(b"\x00" + b"\xd6\x45\x7f" * w for _ in range(h))

    def chunk(kind: bytes, data: bytes) -> bytes:
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))


def new_world() -> dict:
    plots = [{"id": f"plot{i:02d}", "type": "empty", "upgrades": [], "pos": [100.0 + 20 * i, 15.0, 50.0],
              "num": 1000 + 1111 * i % 9000} for i in range(6)]
    plots[0]["type"] = plots[1]["type"] = "corral"
    return {"newbucks": 250, "cu_on": False, "upgrades": {}, "plots": plots, "stations": [],
            "slots": {}, "weather": {"running": ["Clear"], "forecast": ["Clear"], "override": None},
            "radios": {}, "queue": {"songs": [], "playing": None}, "pos": [0.0, 15.0, 0.0], "time": 8.0}


class MockGame:
    def __init__(self, game_dir: Path, start_in_slot: int | None = None, answer: str | None = None):
        self.game_dir = Path(game_dir)
        self.game_dir.mkdir(parents=True, exist_ok=True)
        self.log_path = self.game_dir / "sr2mods.log"
        self.lock = threading.Lock()
        self.games: dict[int, dict] = {}
        for s in range(1, 5):  # the user's games: must never be touched
            self.games[s] = {"game_name": f"2026102{s}120000", "display_name": f"User game {s}",
                             "saves": [{"save_number": 10 * s, "time": f"2026-10-2{s} 12:00", "world": new_world()}]}
        self.settings = {"custom_upgrades.enabled": True, "sandbox.enabled": False, "slime_dj.enabled": True,
                         "autotest.enabled": True, "minimap.enabled": True, "collectibles.enabled": True,
                         "gadget_stacking.enabled": False}
        self.current: dict | None = None
        self.menus: list[str] = []
        self.answer = answer          # None = the first option
        self.missing: set[str] = set()  # probe / action names to report as unknown
        self.journal: list[dict] = []
        self.violations: list[str] = []
        self.subscribed = False
        self._events: list[dict] = []
        self.save_counter = 100
        self.overlay_open = False
        self.page = ("ranch", "Upgrades")
        self.places: dict[str, dict] = {}  # sr2mods_autotest\places.json
        self.shots: list[dict] = []         # every screenshot's options and the state it was taken in
        if start_in_slot:
            self._load(start_in_slot)

    # -- logging
    def log(self, text: str, level: str = "INFO") -> None:
        line = f"{datetime.now().strftime('%Y-%m-%d %H:%M:%S.%f')[:-3]} [{level:5}] [4242] {text}"
        with open(self.log_path, "a", encoding="utf-8") as f:
            f.write(line + "\n")
        if self.subscribed:
            self._events.append({"event": "log", "line": line})

    # -- protocol
    def handle(self, msg: dict):
        with self.lock:
            self._events = []
            rid = msg.get("id")
            cmd = msg.get("cmd", "")
            args = msg.get("args") or {}
            self.journal.append({"cmd": cmd, "args": args})
            try:
                fn = getattr(self, "cmd_" + cmd, None)
                if fn is None:
                    raise MockError(f"unknown command '{cmd}'")
                reply = {"id": rid, "ok": True, "result": fn(args)}
            except MockError as e:
                reply = {"id": rid, "ok": False, "error": str(e)}
            return reply, self._events

    def _slot_arg(self, cmd: str, args: dict) -> int:
        slot = args.get("slot")
        if slot not in SAFE:
            self.violations.append(f"{cmd} slot {slot}")
            raise MockError(f"refused: slot {slot} is not a test slot")
        return slot

    @property
    def w(self) -> dict:
        if not self.current:
            raise MockError("not in game")
        return self.current["world"]

    def cu(self) -> bool:
        return bool(self.settings.get("custom_upgrades.enabled")) and bool(self.current and self.w["cu_on"])

    # -- contract commands
    def cmd_ping(self, a):
        return {"build": "mock", "sha": "0000000", "version": "mock-1"}

    def cmd_state(self, a):
        c = self.current
        return {"scene": "zoneFields" if c else "MainMenuEnvironment", "in_game": bool(c), "paused": False,
                "game_name": c["game_name"] if c else None, "display_name": c["display_name"] if c else None,
                "slot": c["slot"] if c else None, "save_number": c["save_number"] if c else None,
                "pos": self.w["pos"] if c else None, "rot": [0, 0, 0] if c else None,
                "zone": "Ranch" if c else None, "time": self.w["time"] if c else None,
                "newbucks": self.w["newbucks"] if c else None, "menus": list(self.menus),
                "overlay_open": self.overlay_open}

    def cmd_log_mark(self, a):
        mark = f"autotest mark {uuid.uuid4().hex[:12]}"
        self.log(mark)
        return {"mark": mark}

    def cmd_settings_get(self, a):
        k = a.get("key")
        if k not in self.settings:
            raise MockError(f"unknown setting '{k}'")
        return {"key": k, "value": self.settings[k]}

    def cmd_settings_set(self, a):
        k = a.get("key")
        if k not in self.settings:
            raise MockError(f"unknown setting '{k}'")
        self.settings[k] = a.get("value")
        return {"key": k, "value": self.settings[k]}

    def cmd_saves_list(self, a):
        return [{"game_name": g["game_name"], "display_name": g["display_name"], "slot": s,
                 "last_save": g["saves"][-1]["time"]} for s, g in sorted(self.games.items())]

    def _load(self, slot: int) -> None:
        g = self.games[slot]
        sv = g["saves"][-1]
        self.current = {"slot": slot, "game_name": g["game_name"], "display_name": g["display_name"],
                        "save_number": sv["save_number"], "world": copy.deepcopy(sv["world"])}
        self.menus = []
        self.log(f"load: '{g['display_name']}' ({g['game_name']}_{sv['save_number'] % 5}) slot {slot}")
        self._after_load()

    def _after_load(self) -> None:
        w = self.w
        ov = w["weather"]["override"]
        if ov and ov.get("locked"):
            self.log(f"[sandbox] weather: {ov['label']} in Ranch applied from the save's lock: "
                     f"started {ov['label']}")
        if not self.cu():
            return
        tb = w["upgrades"].get("tank_booster", 0)
        if tb > 8:
            self.log(f"[custom_upgrades] vacpack: Tank Booster level 8 -> {tb} (from the sidecar)")
        slots = w["slots"]
        if any(int(i) >= 6 and not s.get("water") and s.get("count") for i, s in slots.items()):
            self.log("[custom_upgrades] vacpack: loaded vacpack spread into the extended layout "
                     "(saved slot 6 = water -> slot 8)")
            self.log("[custom_upgrades] vacpack: extra slots refilled from the sidecar (2 put back, 0 skipped): ...")

    def cmd_load_slot(self, a):
        slot = self._slot_arg("load_slot", a)
        if slot not in self.games:
            raise MockError(f"slot {slot} is empty")
        self._load(slot)
        return self.cmd_state({})

    def cmd_new_game(self, a):
        slot = self._slot_arg("new_game", a)
        name = time.strftime("%Y%m%d%H%M%S") + str(slot)
        self.games[slot] = {"game_name": name, "display_name": a.get("name") or f"Test {slot}", "saves": [
            {"save_number": 1, "time": time.strftime("%Y-%m-%d %H:%M:%S"), "world": new_world()}]}
        self._load(slot)
        return self.cmd_state({})

    def cmd_save(self, a):
        if not self.current:
            raise MockError("not in game")
        slot = self.current["slot"]
        if slot not in SAFE:
            self.violations.append(f"save in slot {slot}")
            raise MockError(f"refused: the game in play is in slot {slot}")
        self.save_counter += 1
        self.current["save_number"] = self.save_counter
        self.games[slot]["saves"].append({"save_number": self.save_counter, "time": time.strftime("%Y-%m-%d %H:%M:%S"),
                                          "world": copy.deepcopy(self.w)})
        self.games[slot]["saves"] = self.games[slot]["saves"][-5:]
        self.log(f"save: {self.current['game_name']} save #{self.save_counter}")
        return {"save_number": self.save_counter}

    def cmd_quit_to_title(self, a):
        self.current = None
        self.menus = []
        return {}

    def cmd_delete_slot(self, a):
        slot = self._slot_arg("delete_slot", a)
        if self.current and self.current["slot"] == slot:
            raise MockError("that game is loaded")
        self.games.pop(slot, None)
        return {}

    def cmd_teleport(self, a):
        if "pos" in a:
            self.w["pos"] = list(a["pos"])
        elif a.get("to") == "ranch":  # the pilot tests' ranch spot
            self.w["pos"] = [0.0, 15.0, 0.0]
        elif a.get("to"):
            if a["to"] not in self.places:
                raise MockError(f"unknown place '{a['to']}' (save one with exec place_save {{name}})")
            self.w["pos"] = list(self.places[a["to"]]["pos"])
        else:
            raise MockError("teleport needs \"pos\" [x, y, z] (exact) or [x, z] (onto the ground), or \"to\"")
        return {"pos": self.w["pos"]}

    def exec_place_save(self, a):
        if not a.get("name"):
            raise MockError("place_save needs \"name\"")
        self.places[a["name"]] = {"pos": list(self.w["pos"]), "rot": [0.0, 90.0, 0.0], "zone": "Ranch"}
        return self.places[a["name"]]

    def probe_places(self, a):
        return copy.deepcopy(self.places)

    def _page(self, cat, tab) -> tuple[str, str]:
        """As the menu's ApplyPageRequest: category by id or title, tab by title or index."""
        for cid, (ctitle, tabs) in PAGES.items():
            if str(cat).lower() in (cid, ctitle.lower()):
                if tab in (None, ""):
                    return cid, tabs[0]
                for i, t in enumerate(tabs):
                    if str(tab).lower() == t.lower() or str(tab) == str(i):
                        return cid, t
                raise MockError(f"no tab '{tab}' in {cid}")
        raise MockError(f"no category '{cat}'")

    def cmd_overlay(self, a):
        if not a.get("open", True):
            self.overlay_open = False
            return {"open": False}
        self.overlay_open = True
        r = {"open": True}
        if a.get("cat") is not None or a.get("tab") is not None:
            self.page = self._page(a.get("cat", self.page[0]), a.get("tab"))
            r["page"] = " / ".join(self.page)
        return r

    def cmd_find(self, a):
        return []

    def cmd_input(self, a):
        return {}

    def cmd_screenshot(self, a):
        page = None
        if isinstance(a.get("menu"), dict):
            page = self._page(a["menu"].get("cat"), a["menu"].get("tab"))
        d = self.game_dir / "sr2mods_autotest" / "shots"
        d.mkdir(parents=True, exist_ok=True)
        p = d / f"{a.get('name') or 'shot'}_{int(time.time() * 1000)}.png"
        p.write_bytes(tiny_png())
        self.shots.append({"name": a.get("name"), "overlay": bool(a.get("overlay")) or page is not None,
                           "hide_hud": bool(a.get("hide_hud")), "hide_minimap": bool(a.get("hide_minimap")),
                           "hide_harness": bool(a.get("hide_harness")), "page": page,
                           "slot": self.current["slot"] if self.current else None,
                           "pos": list(self.w["pos"]) if self.current else None,
                           "time": self.w["time"] if self.current else None, "menus": list(self.menus)})
        return {"path": str(p), "width": 16, "height": 9}

    def cmd_ask(self, a):
        opts = a.get("options") or []
        if not 2 <= len(opts) <= 4:
            raise MockError("ask takes 2-4 options")
        ans = self.answer if self.answer in opts or self.answer == "timeout" else opts[0]
        return {"answer": ans, "note": "mock"}

    def cmd_cancel_ask(self, a):
        return {}

    def cmd_subscribe_log(self, a):
        self.subscribed = True
        return {}

    def cmd_probe(self, a):
        return self._named("probe", a)

    def cmd_exec(self, a):
        return self._named("exec", a)

    def _named(self, kind: str, a: dict):
        name = a.get("name", "")
        fn = getattr(self, f"{kind}_" + name.replace(".", "__"), None)
        if fn is None or name in self.missing:
            raise MockError(f"unknown {'probe' if kind == 'probe' else 'action'} '{name}'")
        return fn(a.get("args") or {})

    # -- generic actions / probes
    def exec_give_newbucks(self, a):
        self.w["newbucks"] += int(a.get("amount", 0))
        return {"newbucks": self.w["newbucks"]}

    def exec_set_time(self, a):
        if "hour" in a:
            self.w["time"] = float(a["hour"])
        self.w["time"] = (self.w["time"] + float(a.get("add_hours", 0))) % 24
        return {"time": self.w["time"]}

    def exec_spawn(self, a):
        return {"spawned": a.get("count", 1)}

    def probe_plots(self, a):
        return [{k: p[k] for k in ("id", "type", "upgrades", "pos")} for p in self.w["plots"]]

    def exec_plot_menu(self, a):
        self.menus = ["LandPlotUIRoot"]
        return {}

    def exec_sandbox__upgrade_level(self, a):
        self.w["upgrades"][a["id"]] = int(a["level"])
        return {}

    def _plot(self, pid):
        for p in self.w["plots"]:
            if p["id"] == pid:
                return p
        raise MockError(f"no plot {pid}")

    def exec_plots__build(self, a):
        self._plot(a["plot"])["type"] = a["type"]
        return {}

    def exec_plots__buy_upgrade(self, a):
        self._plot(a["plot"])["upgrades"].append(str(a["upgrade"]).lower())
        return {}

    # -- custom upgrades
    def probe_custom_upgrades__state(self, a):
        return {"on": self.w["cu_on"], "locked": self.w["cu_on"]}

    def exec_custom_upgrades__set_on(self, a):
        self.w["cu_on"] = bool(a.get("on"))
        return {}

    def exec_custom_upgrades__buy(self, a):
        if not self.cu():
            raise MockError("custom upgrades are off")
        uid = a["id"]
        lv = self.w["upgrades"].get(uid, 0)
        if lv >= len(LABELS.get(uid, [uid])):
            raise MockError("max level")
        self.w["upgrades"][uid] = lv + 1
        return {"level": lv + 1}

    def probe_custom_upgrades__upgrade(self, a):
        uid = a["id"]
        labels = LABELS.get(uid, [uid])
        lv = self.w["upgrades"].get(uid, 0)
        return {"level": lv, "max": len(labels), "next_label": labels[lv] if lv < len(labels) else None}

    def exec_custom_upgrades__buy_plot(self, a):
        self._plot(a["plot"])["upgrades"].append(a["id"])
        return {}

    # -- vacpack
    def _layout(self) -> list[dict]:
        w = self.w
        cu = self.cu()
        et = w["upgrades"].get("extra_tank", 0)
        tb = w["upgrades"].get("tank_booster", 0)
        if not cu:
            et, tb = min(et, 2), min(tb, 8)
        n = 4 + et
        out = []
        for i in range(n + 1):
            s = w["slots"].get(str(i), {})
            water = i == n
            out.append({"index": i, "id": s.get("id"), "count": s.get("count", 0),
                        "max": 30 if water else TANK[tb], "water": water})
        return out

    def probe_vacpack__slots(self, a):
        return self._layout()

    def exec_vacpack__set_slot(self, a):
        self.w["slots"][str(a["index"])] = {"id": a["id"], "count": int(a["count"]),
                                            "water": a["id"] == "Water"}
        return {}

    def exec_vacpack__shoot(self, a):
        lv = self.w["upgrades"].get("slime_launcher", 0) if self.cu() else 0
        self.w["last_shot"] = {"id": "Pink", "speed": EJECT * FACTOR[lv], "eject_speed": EJECT}
        return {}

    def probe_vacpack__last_shot(self, a):
        return self.w.get("last_shot") or {}

    # -- drones
    def probe_drones__stations(self, a):
        return [{"id": s, "pos": [0, 15, 5], "programs": 1} for s in self.w["stations"]]

    def exec_drones__place_station(self, a):
        sid = 7000 + len(self.w["stations"])
        self.w["stations"].append(sid)
        return {"id": sid}

    def exec_drones__open_menu(self, a):
        self.menus = [f"DroneStation:{a.get('station')}:{a.get('page')}"]
        return {}

    def exec_ui__close_menu(self, a):
        self.menus = []
        return {}

    def probe_ui__prompt_pills(self, a):
        pills = [{"key": "Esc", "label": "Close"}]
        if self.menus and self.menus[0].startswith("DroneStation") and self.cu() and \
                self.w["upgrades"].get("drone_programs"):
            pills.append({"key": "P", "label": "Programs"})
        return pills

    # -- weather
    def probe_sandbox__weather(self, a):
        return self.w["weather"]["override"] or {"zone": "Ranch", "label": None, "locked": False}

    def exec_sandbox__weather(self, a):
        wt = self.w["weather"]
        if a.get("op") == "natural":
            wt["override"] = None
            wt["running"] = list(wt["forecast"])
            self.log("[sandbox] weather: Ranch back to natural weather: started " + ", ".join(wt["forecast"]))
        else:
            label = f"{a['pattern']} - Heavy"
            wt["override"] = {"zone": "Ranch", "label": label, "locked": bool(a.get("lock"))}
            wt["running"] = [label]
        return {}

    def probe_weather__zone(self, a):
        wt = self.w["weather"]
        return {"zone": "Ranch", "running": list(wt["running"]), "forecast": list(wt["forecast"])}

    def exec_weather__force_change(self, a):
        wt = self.w["weather"]
        wt["forecast"] = ["Rain"] if wt["forecast"] != ["Rain"] else ["Clear"]
        ov = wt["override"]
        if ov and ov.get("locked"):
            self.log(f"[sandbox] weather: {ov['label']} in Ranch re-applied (the game changed the weather): "
                     f"ended {wt['forecast'][0]}, started {ov['label']}")
        else:
            wt["running"] = list(wt["forecast"])
        return {}

    def exec_ui__open_map(self, a):
        self.menus = ["MapUI"]
        return {}

    def exec_ui__close_map(self, a):
        self.menus = []
        return {}

    # -- slime dj
    def probe_slime_dj__library(self, a):
        return {"songs": list(SONGS)}

    def _radio(self, pid) -> dict:
        p = self._plot(pid)
        if "corral_radio" not in p["upgrades"] or not self.cu():
            raise MockError(f"no radio on {pid}")
        return self.w["radios"].setdefault(pid, {"on": False, "playlist": [], "playing": None, "shuffle": False})

    def probe_slime_dj__radios(self, a):
        out = []
        for p in self.w["plots"]:
            if "corral_radio" in p["upgrades"] and self.cu():
                r = self._radio(p["id"])
                out.append({"plot": p["id"], "name": f"Corral {p['num']:04d}", "distance": 12.5, **copy.deepcopy(r)})
        return out

    def probe_slime_dj__queue(self, a):
        return copy.deepcopy(self.w["queue"])

    def exec_slime_dj__radio(self, a):
        r = self._radio(a["plot"])
        op = a.get("op")
        pl = r["playlist"]
        if op == "add":
            pl.extend(a.get("songs", []))
        elif op == "on":
            r["on"] = True
            r["playing"] = r["playing"] if r["playing"] in pl else (pl[0] if pl else None)
        elif op == "off":
            r["on"] = False
        elif op == "skip" and pl:
            i = pl.index(r["playing"]) if r["playing"] in pl else -1
            r["playing"] = pl[(i + 1) % len(pl)]
        elif op == "shuffle":
            r["shuffle"] = not r["shuffle"]
        elif op == "play":
            r["playing"] = pl[int(a["index"])]
        elif op == "move":
            i, j = int(a["index"]), int(a["index"]) + int(a["by"])
            pl[i], pl[j] = pl[j], pl[i]
        elif op == "remove":
            pl.pop(int(a["index"]))
        else:
            raise MockError(f"bad op {op}")
        return {}

    def exec_slime_dj__queue_play(self, a):
        self.w["queue"]["playing"] = a["song"]
        if a["song"] not in self.w["queue"]["songs"]:
            self.w["queue"]["songs"].append(a["song"])
        return {}

    def probe_slime_dj__corral_audio(self, a):
        p = self._plot(a["plot"])
        r = self.w["radios"].get(p["id"])
        if r and r["on"] and self.cu():
            return {"source": "radio", "song": r["playing"]}
        if "music_box" in p["upgrades"] and self.w["queue"]["playing"]:
            return {"source": "queue", "song": self.w["queue"]["playing"]}
        return {"source": "vanilla", "song": None}


class MockError(Exception):
    pass


def start(pipe: str, game: MockGame) -> PipeServer:
    srv = PipeServer(pipe, game.handle)
    threading.Thread(target=srv.serve_forever, daemon=True, name="mock-pipe").start()
    return srv


# ---- self-test -------------------------------------------------------------------------------------

def selftest() -> int:
    sys.path.insert(0, str(HERE))
    import cleanup
    import run
    import sr2test
    from sr2test import Client, SlotGuardError

    root = Path(tempfile.mkdtemp(prefix="sr2mock_"))
    game_dir, saves, backups, results = root / "game", root / "saves", root / "backups", root / "results"
    acct = saves / "76561190000000000"
    acct.mkdir(parents=True)
    for i in range(1, 5):
        (acct / f"2026102{i}120000_{i}_0.sav").write_bytes(os.urandom(64))
    (game_dir / "sr2mods_saves").mkdir(parents=True)
    (game_dir / "sr2mods_saves" / "x_1.sav.json").write_text("{}")
    os.environ["SR2_PLAYER_LOG"] = str(root / "Player.log")
    (root / "Player.log").write_text("")

    pipe = rf"\\.\pipe\sr2mods-autotest-mock-{os.getpid()}"
    game = MockGame(game_dir, start_in_slot=2)
    start(pipe, game)
    common = ["--pipe", pipe, "--game-dir", str(game_dir), "--saves-root", str(saves), "--backup-root",
              str(backups), "--results-dir", str(results), "--connect-timeout", "5"]
    failures: list[str] = []

    def check(cond, what):
        print(("  ok   " if cond else "  FAIL ") + what)
        if not cond:
            failures.append(what)

    print("client:")
    with Client(pipe=pipe, game_dir=game_dir) as c:
        check(c.ping().get("build") == "mock", "ping")
        st = c.state()
        check(st.get("in_game") and st.get("slot") == 2, "state: the user's slot 2 game is loaded")
        n = len(game.journal)
        for fn, what in [(lambda: c.load_slot(1), "load_slot(1)"), (lambda: c.new_game(4), "new_game(4)"),
                         (lambda: c.delete_slot(3), "delete_slot(3)"), (lambda: c.call("load_slot", {}), "load_slot()"),
                         (lambda: c.call("delete_slot", {"slot": "5"}), "delete_slot('5')"),
                         (lambda: c.exec("save_game"), "exec save_game"),
                         (lambda: c.exec("plots.reset", slot=2), "exec with slot=2")]:
            try:
                fn()
                check(False, f"{what} refused by the client")
            except SlotGuardError:
                check(True, f"{what} refused by the client")
        check(len(game.journal) == n, "refused requests never reached the harness")
        try:
            c.save()
            check(False, "save() refused while slot 2 is in play")
        except SlotGuardError:
            check(True, "save() refused while slot 2 is in play")
        check(not any(j["cmd"] == "save" for j in game.journal), "no save request sent")
        m = c.log_mark()
        game.log("[custom_upgrades] hello after the mark")
        check(any("hello after the mark" in ln for ln in c.log_since(m)), "log_since sees a line after the mark")
        check(not any(m.text in ln for ln in c.log_since(m)), "log_since leaves out the marker")
        check(c.ask("Q?", ["Yes", "No"], case="x", step=1)["answer"] == "Yes", "ask returns the answer")
        try:
            c.probe("nope.nothing")
            check(False, "unknown probe raises")
        except sr2test.HarnessError as e:
            check(e.unknown, "unknown probe raises HarnessError.unknown")
        c.subscribe_log()
        c.log_mark()
        check(any("autotest mark" in ln for ln in c.log_lines), "subscribe_log delivers log events")

    print("runner refuses:")
    check(run.main(common + ["r15sl5"]) == 4, "run.py refuses while a slot 1-4 game is loaded")
    conns = len(game.journal)
    rc = run.main(common[:4] + ["--saves-root", str(root / "missing"), "--backup-root", str(backups),
                                "--results-dir", str(results), "r15sl5"])
    check(rc == 2 and len(game.journal) == conns, "run.py refuses (and never connects) when the backup fails")
    with Client(pipe=pipe, game_dir=game_dir) as c:
        c.quit_to_title()

    print("backup:")
    dest = run.backup_saves(saves, backups, game_dir)
    check(sorted(p.name for p in dest.iterdir()) == sorted(p.name for p in acct.iterdir()),
          "flat copy of the single account folder (deploy.ps1 layout)")
    check((backups / (dest.name + "_gamefolder") / "sr2mods_saves" / "x_1.sav.json").is_file(),
          "sidecars copied next to it")

    print("runner, pilots:")
    rc = run.main(common + ["r15sl5", "r12pg4", "r9wx3", "r10cr3", "r6vu4", "sv2"])
    rep = json.loads(max(results.glob("*.json"), key=lambda p: p.stat().st_mtime_ns).read_text(encoding="utf-8"))
    for case in ("r15sl5", "r12pg4", "r9wx3", "r10cr3", "r6vu4"):
        r = rep["cases"][case]
        check(r["status"] == "pass", f"{case} passes against the mock ({r['status']}: {r['note'][:150]})")
    check(rep["cases"]["sv2"]["status"] == "skipped", "a case without a test file is skipped")
    check(rc == 0, "run.py exit code 0")
    check(game.settings["custom_upgrades.enabled"] is True and game.settings["sandbox.enabled"] is False,
          "settings restored after the cases")

    print("runner, other outcomes:")
    game.answer = "No"
    run.main(common + ["r9wx3"])
    rep = json.loads(max(results.glob("*.json"), key=lambda p: p.stat().st_mtime_ns).read_text(encoding="utf-8"))
    check(rep["cases"]["r9wx3"]["status"] == "fail", "a No answer fails the case")
    game.answer = "Can't tell"
    run.main(common + ["r9wx3"])
    rep = json.loads(max(results.glob("*.json"), key=lambda p: p.stat().st_mtime_ns).read_text(encoding="utf-8"))
    check(rep["cases"]["r9wx3"]["status"] == "needs_user", "Can't tell -> needs_user")
    game.answer = None
    game.missing = {"drones.stations"}
    run.main(common + ["r12pg4"])
    rep = json.loads(max(results.glob("*.json"), key=lambda p: p.stat().st_mtime_ns).read_text(encoding="utf-8"))
    r = rep["cases"]["r12pg4"]
    check(r["status"] == "skipped" and "needs harness" in r["note"], f"unknown probe -> skipped ({r['note']})")
    game.missing = set()

    selftest_showcase(game, root, pipe, game_dir, saves, backups, common, results, check)

    print("cleanup:")
    shots = game_dir / "sr2mods_autotest" / "shots"
    check(shots.is_dir() and any(shots.iterdir()), "screenshots were taken")
    rc = cleanup.main(["--pipe", pipe, "--game-dir", str(game_dir), "--saves-root", str(saves), "--backup-root",
                       str(backups)])
    check(rc == 0 and 5 in game.games, "cleanup without --yes deletes nothing")
    rc = cleanup.main(["--yes", "--shots", "--pipe", pipe, "--game-dir", str(game_dir), "--saves-root", str(saves),
                       "--backup-root", str(backups)])
    check(rc == 0 and 5 not in game.games and 6 not in game.games, "cleanup --yes deletes slots 5 and 6")
    check(all(s in game.games for s in (1, 2, 3, 4)), "slots 1-4 still there")
    check(not shots.exists(), "cleanup --shots deletes the screenshot folder")
    check(any(p.name.endswith("_autotest-cleanup") for p in backups.iterdir()), "cleanup backed up first")

    print("safety over the whole run:")
    check(not game.violations, f"no request ever touched slots 1-4 ({game.violations})")
    for s in range(1, 5):
        check(len(game.games[s]["saves"]) == 1, f"slot {s} saves unchanged")

    shutil.rmtree(root, ignore_errors=True)
    print(f"\n{'ALL OK' if not failures else f'{len(failures)} FAILED'}")
    return 1 if failures else 0


def selftest_showcase(game: MockGame, root: Path, pipe: str, game_dir: Path, saves: Path, backups: Path,
                      common: list[str], results: Path, check) -> None:
    """showcase.py, run.py --showcase and build_gallery.py against the mock."""
    import build_gallery
    import run
    import showcase

    out = root / "showcase_out"
    sc = ["--pipe", pipe, "--game-dir", str(game_dir), "--saves-root", str(saves), "--backup-root", str(backups),
          "--out-dir", str(out), "--connect-timeout", "5"]

    def latest() -> tuple[Path, dict]:
        m = max(out.glob("*/manifest.json"), key=lambda p: p.stat().st_mtime_ns)
        return m.parent, json.loads(m.read_text(encoding="utf-8"))

    print("showcase:")
    check(showcase.main(["--list"]) == 0, "showcase --list")
    defs = showcase.load_defs()
    check(len(defs) >= 7 and all(d.slot in SAFE for d in defs.values()),
          f"{len(defs)} shot definitions, all in slot 5 or 6")
    n_shots, jn = len(game.shots), len(game.journal)
    rc = showcase.main(sc)  # the mock answers every framing question with the first option, Shoot
    folder, man = latest()
    by_id = {s["id"]: s for s in man["shots"]}
    check(rc == 0 and set(by_id) == set(defs), f"every definition ran (rc {rc}: {sorted(by_id)})")
    for i, s in sorted(by_id.items()):
        check(s["status"] == "shot" and (folder / s.get("file", "-")).is_file(),
              f"{i}: shot, copied to the run folder ({s['status']}: {s.get('note', '')[:120]})")
    check(man["build"].get("sha") == "0000000", "manifest has the build sha")
    check(by_id["sandbox_menu"]["settings"].get("sandbox.enabled") is True and
          by_id["plort_glow"]["settings"].get("sandbox.enabled") is True, "manifest has the settings each shot used")
    check(all(s.get("title") and s.get("caption") and s.get("feature") for s in man["shots"]),
          "every shot has a title, caption and feature")
    new = {s["name"]: s for s in game.shots[n_shots:]}
    check(all(s["hide_harness"] for s in new.values()), "every showcase shot hides the harness tag and panel")
    check(all(s["slot"] in SAFE for s in new.values()), "every showcase shot taken in slot 5 or 6")
    check(new["showcase_sandbox_menu"]["page"] == ("sandbox", "Spawn & Craft") and new["showcase_sandbox_menu"]["overlay"],
          "a MENU shot opens that mod menu page, with the overlay in the frame")
    check(new["showcase_corral_radio"]["hide_hud"] and not new["showcase_minimap_tracker"]["hide_hud"],
          "HIDE_HUD per shot")
    check(new["showcase_upgrade_cards"]["menus"] == ["LandPlotUIRoot"], "PLOT_MENU: the plot menu is open for the shot")
    check(not game.menus, "the plot menu is closed after its shot")
    check(new["showcase_plort_glow"]["time"] == 22, "TIME is set before the shot")
    asks = [j for j in game.journal[jn:] if j["cmd"] == "ask"]
    check("showcase_corral" in game.places and len(asks) >= 4,
          f"no saved place: the user frames it ({len(asks)} questions), then the place is saved")
    check(game.settings["sandbox.enabled"] is False and game.settings["gadget_stacking.enabled"] is False,
          "settings put back after the shots")
    check(not game.overlay_open, "the mod menu is closed after the shots")

    jn = len(game.journal)
    rc = showcase.main(sc + ["--no-ask", "corral_radio", "sandbox_menu"])
    tps = [j["args"] for j in game.journal[jn:] if j["cmd"] == "teleport"]
    check(rc == 0 and any(a.get("pos") == game.places["showcase_corral"]["pos"] for a in tps) and
          not any(j["cmd"] == "ask" for j in game.journal[jn:]), "a saved place: teleported there, nothing asked")

    game.answer = "Skip"
    showcase.main(sc + ["--frame", "sandbox_menu"])
    _, man = latest()
    check(man["shots"][0]["status"] == "skipped", "--frame: Skip leaves the shot out")
    game.answer = None

    game.missing = {"slime_dj.library"}
    showcase.main(sc + ["corral_radio"])
    _, man = latest()
    check(man["shots"][0]["status"] == "skipped" and "needs harness" in man["shots"][0]["note"],
          "a missing action: the shot is skipped (needs harness)")
    game.missing = set()

    bad = root / "bad_defs"
    bad.mkdir()
    (bad / "bad.py").write_text('TITLE = "x"\nSLOT = 2\n', encoding="utf-8")
    jn = len(game.journal)
    rc = showcase.main(sc + ["--defs-dir", str(bad)])
    check(rc == 4 and len(game.journal) == jn, "a definition outside slots 5 and 6 is refused before connecting")

    print("run.py --showcase:")
    before = set(out.iterdir())
    run.main(common + ["r10cr3"])
    check(set(out.iterdir()) == before, "without --showcase a test takes no showcase shots")
    run.main(common + ["--showcase", "--showcase-dir", str(out), "r10cr3"])
    added = set(out.iterdir()) - before
    man = json.loads((next(iter(added)) / "manifest.json").read_text(encoding="utf-8")) if len(added) == 1 else {}
    ids = [s["id"] for s in man.get("shots", [])]
    check(ids == ["corral_radio_test", "slime_dj_radios"] and
          all(s["source"].startswith("test r10cr3") and s["status"] == "shot" for s in man["shots"]),
          f"--showcase: the test's shots in a new run folder ({ids})")
    rep = json.loads(max(results.glob("*.json"), key=lambda p: p.stat().st_mtime_ns).read_text(encoding="utf-8"))
    check(rep["cases"]["r10cr3"]["status"] == "pass" and len(rep["cases"]["r10cr3"].get("showcase", [])) == 2 and
          rep.get("showcase_dir"), "the run report lists the showcase shots and folder")

    print("gallery:")
    page = build_gallery.build(folder / "manifest.json")
    text = page.read_text(encoding="utf-8")
    check(page.name == "index.html" and all(d.title in text for d in defs.values()) and
          'src="sandbox_menu.png"' in text, "index.html has a card per shot, images next to it")
    emb = build_gallery.build(folder / "manifest.json", root / "embedded.html", embed=True)
    text = emb.read_text(encoding="utf-8")
    check("data:image/png;base64," in text and 'src="sandbox_menu.png"' not in text, "--embed: one self-contained file")
    check(build_gallery.main([str(folder), "--only", "slime_dj,sandbox_menu", "--out", str(root / "only.html")]) == 0 and
          (root / "only.html").read_text(encoding="utf-8").count('class="card"') == 2, "--only picks and orders cards")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--pipe", default=MOCK_PIPE)
    ap.add_argument("--game-dir", default=str(Path(tempfile.gettempdir()) / "sr2mock"))
    ap.add_argument("--start-in-slot", type=int, default=None, help="start with this slot's game loaded")
    ap.add_argument("--answer", default=None, help="answer every ask with this label (default: the first option)")
    ap.add_argument("--selftest", action="store_true")
    args = ap.parse_args()
    if sys.platform != "win32":
        print("Windows only (named pipes).", file=sys.stderr)
        return 2
    if args.selftest:
        return selftest()
    game = MockGame(Path(args.game_dir), args.start_in_slot, args.answer)
    srv = PipeServer(args.pipe, game.handle)
    print(f"mock harness on {args.pipe}, game folder {args.game_dir} (Ctrl+C to stop)")
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
