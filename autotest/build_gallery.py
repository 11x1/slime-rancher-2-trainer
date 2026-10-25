"""Builds one HTML page from a showcase manifest: a card per shot (image, title, caption, feature).

    python autotest/build_gallery.py                       # the newest autotest/showcase_out/<run>/
    python autotest/build_gallery.py autotest/showcase_out/2026-10-25_201500
    python autotest/build_gallery.py <run>/manifest.json --embed   # images inside the page (one file)
    python autotest/build_gallery.py <run> --only sandbox_menu,slime_dj --title "sr2mods 1.4"

Writes <run>/index.html (or --out). The page is the base for the release artifact. It stays on this
machine: game imagery is published only when the user says so.
"""
from __future__ import annotations

import argparse
import base64
import html
import json
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
OUT_DIR = HERE / "showcase_out"

FEATURE_NAMES = {
    "sandbox": "Sandbox", "slime_dj": "Slime DJ", "custom_upgrades": "Custom Upgrades",
    "gadget_stacking": "Gadget Stacking", "minimap": "Minimap", "collectibles": "Collectibles",
    "map_tools": "Full Map", "hl2_movement": "Movement", "quick_travel": "Quick Travel",
}

CSS = """
:root {
  --bg: #f6f4f8; --surface: #ffffff; --text: #1f1a24; --muted: #6b6373; --line: #e4dfe9;
  --accent: #d6457f; --accent-soft: #fbe3ed; --shadow: 0 1px 2px rgb(0 0 0 / .06), 0 8px 24px rgb(0 0 0 / .06);
}
@media (prefers-color-scheme: dark) {
  :root:not([data-theme="light"]) {
    --bg: #141118; --surface: #1e1a23; --text: #f1edf5; --muted: #a59dae; --line: #2e2836;
    --accent: #f06d9f; --accent-soft: #3a1f2c; --shadow: 0 1px 2px rgb(0 0 0 / .4), 0 8px 24px rgb(0 0 0 / .3);
  }
}
:root[data-theme="dark"] {
  --bg: #141118; --surface: #1e1a23; --text: #f1edf5; --muted: #a59dae; --line: #2e2836;
  --accent: #f06d9f; --accent-soft: #3a1f2c; --shadow: 0 1px 2px rgb(0 0 0 / .4), 0 8px 24px rgb(0 0 0 / .3);
}
* { box-sizing: border-box; }
body {
  margin: 0; background: var(--bg); color: var(--text);
  font: 16px/1.5 system-ui, -apple-system, "Segoe UI", Roboto, sans-serif;
}
header, main, footer { max-width: 1180px; margin: 0 auto; padding: 0 16px; }
header { padding-top: 48px; padding-bottom: 24px; }
h1 { margin: 0 0 6px; font-size: clamp(28px, 5vw, 40px); letter-spacing: -0.02em; }
header p { margin: 0; color: var(--muted); max-width: 60ch; }
.grid { display: grid; grid-template-columns: repeat(auto-fill, minmax(min(100%, 340px), 1fr)); gap: 20px; }
.card {
  margin: 0; background: var(--surface); border: 1px solid var(--line); border-radius: 14px;
  overflow: hidden; box-shadow: var(--shadow); display: flex; flex-direction: column;
}
.card a { display: block; background: #000; }
.card img { display: block; width: 100%; height: auto; aspect-ratio: 16 / 9; object-fit: cover; }
.card figcaption { padding: 14px 16px 18px; }
.tag {
  display: inline-block; font-size: 12px; font-weight: 600; letter-spacing: .02em; color: var(--accent);
  background: var(--accent-soft); border-radius: 999px; padding: 2px 10px; margin-bottom: 8px;
}
.card h2 { margin: 0 0 4px; font-size: 19px; }
.card p { margin: 0; color: var(--muted); }
footer { padding-top: 32px; padding-bottom: 40px; color: var(--muted); font-size: 13px; }
"""


def find_manifest(arg: str | None) -> Path:
    if arg:
        p = Path(arg)
        return p / "manifest.json" if p.is_dir() else p
    runs = sorted((d for d in OUT_DIR.iterdir() if (d / "manifest.json").is_file()), key=lambda d: d.name) \
        if OUT_DIR.is_dir() else []
    if not runs:
        raise SystemExit(f"no showcase runs in {OUT_DIR} (run autotest/showcase.py first)")
    return runs[-1] / "manifest.json"


def image_src(folder: Path, name: str, embed: bool) -> str:
    if not embed:
        return name
    data = (folder / name).read_bytes()
    return "data:image/png;base64," + base64.b64encode(data).decode("ascii")


def build(manifest_path: Path, out: Path | None = None, embed: bool = False, title: str = "sr2mods",
          only: list[str] | None = None) -> Path:
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    folder = manifest_path.parent
    shots = [s for s in manifest.get("shots", []) if s.get("status") == "shot" and s.get("file")]
    if only:
        order = {k: i for i, k in enumerate(only)}
        shots = sorted((s for s in shots if s.get("id") in order), key=lambda s: order[s["id"]])
    seen: set[str] = set()
    cards = []
    for s in shots:
        if s["id"] in seen:  # the same shot again (a test loop): the first one
            continue
        seen.add(s["id"])
        if not (folder / s["file"]).is_file():
            print(f"missing image {s['file']}, card left out", file=sys.stderr)
            continue
        src = image_src(folder, s["file"], embed)
        feature = FEATURE_NAMES.get(s.get("feature", ""), s.get("feature", ""))
        alt = s.get("title", "")
        link = html.escape(s["file"] if not embed else "#", quote=True)
        cards.append(
            f'<figure class="card">'
            + (f'<a href="{link}">' if not embed else "<div>")
            + f'<img src="{html.escape(src, quote=True)}" alt="{html.escape(alt, quote=True)}" loading="lazy"'
            + (f' width="{int(s["width"])}" height="{int(s["height"])}"' if s.get("width") and s.get("height") else "")
            + ">"
            + ("</a>" if not embed else "</div>")
            + "<figcaption>"
            + (f'<span class="tag">{html.escape(feature)}</span>' if feature else "")
            + f"<h2>{html.escape(s.get('title', ''))}</h2>"
            + f"<p>{html.escape(s.get('caption', ''))}</p>"
            + "</figcaption></figure>")
    build_info = manifest.get("build") or {}
    sha = str(build_info.get("sha", ""))[:8]
    version = build_info.get("version", "")
    foot = ", ".join(x for x in (f"version {version}" if version else "", f"build {sha}" if sha else "",
                                 f"shots taken {manifest.get('created', '')[:10]}") if x)
    page = f"""<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>{html.escape(title)} showcase</title>
<style>{CSS}</style>
</head>
<body>
<header>
<h1>{html.escape(title)}</h1>
<p>A native mod suite for Slime Rancher 2: new upgrades, your own music, a sandbox, a minimap and more.</p>
</header>
<main class="grid">
{chr(10).join(cards)}
</main>
<footer>{html.escape(foot)}</footer>
</body>
</html>
"""
    out = Path(out) if out else folder / "index.html"
    out.write_text(page, encoding="utf-8")
    return out


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("manifest", nargs="?", help="a showcase run folder or its manifest.json (default: the newest)")
    ap.add_argument("--out", default=None, help="the HTML file (default <run>/index.html)")
    ap.add_argument("--embed", action="store_true", help="put the images inside the page (one self-contained file)")
    ap.add_argument("--title", default="sr2mods")
    ap.add_argument("--only", default=None, help="comma-separated shot ids, in this order")
    args = ap.parse_args(argv)
    mp = find_manifest(args.manifest)
    if not mp.is_file():
        print(f"no manifest: {mp}", file=sys.stderr)
        return 2
    only = [x.strip() for x in args.only.split(",") if x.strip()] if args.only else None
    out = build(mp, args.out, args.embed, args.title, only)
    print(f"Gallery: {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
