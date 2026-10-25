"""Deletes the test games in save slots 5 and 6 (after a save backup), and the screenshot folder.

    python autotest/cleanup.py --yes               # delete the slot 5 and 6 games
    python autotest/cleanup.py --yes --shots       # and <game folder>\\sr2mods_autotest\\shots
    python autotest/cleanup.py --yes --slots 6     # only slot 6
    python autotest/cleanup.py --shots-only --yes  # only the screenshots (no game needed)

Only slots 5 and 6 can be named; the client refuses any other slot, and so does the harness.
Without --yes it only prints what it would delete.
"""
from __future__ import annotations

import argparse
import shutil
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import sr2test  # noqa: E402
from run import BackupError, backup_saves  # noqa: E402
from sr2test import SAFE_SLOTS, Client, ConnectError, HarnessError  # noqa: E402


def shots_dir(game_dir: Path) -> Path:
    return game_dir / "sr2mods_autotest" / "shots"


def delete_shots(game_dir: Path, really: bool) -> None:
    d = shots_dir(game_dir)
    # Only ever this exact folder inside the game folder.
    if d.name != "shots" or d.parent.name != "sr2mods_autotest" or d.parent.parent != game_dir:
        raise RuntimeError(f"unexpected screenshot path {d}")
    if not d.is_dir():
        print(f"No screenshot folder ({d}).")
        return
    n = sum(1 for p in d.rglob("*") if p.is_file())
    if not really:
        print(f"Would delete {d} ({n} file(s)).")
        return
    shutil.rmtree(d)
    print(f"Deleted {d} ({n} file(s)).")


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--slots", type=int, nargs="+", default=list(SAFE_SLOTS), choices=SAFE_SLOTS)
    ap.add_argument("--shots", action="store_true", help="also delete the screenshot folder")
    ap.add_argument("--shots-only", action="store_true", help="only delete the screenshot folder")
    ap.add_argument("--yes", action="store_true", help="really delete (otherwise a dry run)")
    ap.add_argument("--pipe", default=None)
    ap.add_argument("--game-dir", default=None)
    ap.add_argument("--saves-root", default=None)
    ap.add_argument("--backup-root", default=None)
    args = ap.parse_args(argv)
    game_dir = Path(args.game_dir) if args.game_dir else sr2test.default_game_dir()

    if args.shots_only:
        delete_shots(game_dir, args.yes)
        return 0

    try:
        client = Client(pipe=args.pipe, game_dir=game_dir)
    except ConnectError as e:
        print(f"Cannot connect: {e}", file=sys.stderr)
        return 3
    with client:
        games = {g.get("slot"): g for g in client.saves_list()}
        targets = [s for s in args.slots if s in games]
        for s in args.slots:
            g = games.get(s)
            print(f"Slot {s}: " + (f"{g.get('display_name') or g.get('game_name')} ({g.get('game_name')})"
                                   if g else "empty"))
        if not args.yes:
            print("Dry run: add --yes to delete.")
            if args.shots:
                delete_shots(game_dir, False)
            return 0
        if targets:
            try:
                backup = backup_saves(args.saves_root, args.backup_root, game_dir, tag="autotest-cleanup")
            except BackupError as e:
                print(f"REFUSED: save backup failed: {e}", file=sys.stderr)
                return 2
            print(f"Saves backed up to {backup}")
            st = client.state()
            if st.get("in_game"):
                if st.get("slot") not in SAFE_SLOTS:
                    print(f"REFUSED: a game in slot {st.get('slot')} is loaded; save it and go to the title "
                          "screen first.", file=sys.stderr)
                    return 4
                client.quit_to_title()
            for s in targets:
                try:
                    client.delete_slot(s)
                    print(f"Deleted the game in slot {s}.")
                except HarnessError as e:
                    print(f"Slot {s}: {e}", file=sys.stderr)
                    return 1
            left = {g.get("slot") for g in client.saves_list()} & set(targets)
            if left:
                print(f"Still there after delete: slot(s) {sorted(left)}", file=sys.stderr)
                return 1
    if args.shots:
        delete_shots(game_dir, True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
