"""Build a local, offline icon browser for extracted SR2 icons.

Writes assets/icons/game/index.html (gitignored). PNGs are referenced by
relative path; nothing is embedded or uploaded. Rerun after extracting icons:

    python build_viewer.py [path-to-sr2-mods-repo]
"""
import html
import json
import sys
from pathlib import Path

from PIL import Image

REPO = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(r"C:\Users\franz\Documents\sr2-mods")
ICONS = REPO / "assets" / "icons"
GAME = ICONS / "game"
OUT = GAME / "index.html"

# Known in-game uses, keyed by file stem (any section).
CAPTIONS = {
    "slimepedia_logo": "Slimepedia header / open-Slimepedia button",
    "controls": "Options: Input (Mods tab fallback)",
    "gameplay": "Options: Gameplay cog",
    "iconOptions": "White options cog",
    "graphics": "Options: Video",
    "video": "Options: Display",
    "about": "Options: About",
    "keyboard": "Key / gamepad bindings",
    "bindings": "Key / gamepad bindings",
    "iconPediaHomeBlueprint": "Slimepedia Blueprint tile",
}

# (section title, directory, href prefix relative to index.html, is_mod)
SECTIONS = [
    ("Game: root (hen / rooster / elder / plort collector)", GAME, "", False),
    ("Game: settings", GAME / "settings", "settings/", False),
    ("Game: slimepedia", GAME / "slimepedia", "slimepedia/", False),
    ("Mod icons (ours)", ICONS, "../", True),
    ("Mod icons (ours): source", ICONS / "source", "../source/", True),
]


def collect():
    out = []
    for title, d, prefix, is_mod in SECTIONS:
        items = []
        if d.is_dir():
            for p in sorted(d.glob("*.png"), key=lambda p: p.name.lower()):
                if p.name.startswith("_contact_sheet"):
                    continue
                try:
                    with Image.open(p) as im:
                        w, h = im.size
                except Exception:
                    w = h = 0
                items.append({
                    "name": p.name,
                    "src": prefix + p.name,
                    "rel": p.relative_to(REPO).as_posix(),
                    "w": w, "h": h,
                    "cap": CAPTIONS.get(p.stem, ""),
                })
        out.append({"title": title, "mod": is_mod, "items": items})
    return out


PAGE = r"""<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>SR2 Icon Browser</title>
<style>
:root {
  --bg: #f6f7f9; --panel: #ffffff; --text: #1b1f24; --muted: #667085;
  --border: #e3e6ea; --accent: #2f7de1; --mod: #c27c0e;
  --chk-a: #ffffff; --chk-b: #dfe3e8; --shadow: 0 1px 2px rgba(0,0,0,.06), 0 2px 8px rgba(0,0,0,.05);
}
@media (prefers-color-scheme: dark) {
  :root {
    --bg: #111418; --panel: #1a1e24; --text: #e6e8eb; --muted: #8b95a3;
    --border: #2a3039; --accent: #5aa2ff; --mod: #f0b44c;
    --chk-a: #2a2f37; --chk-b: #20242a; --shadow: 0 1px 2px rgba(0,0,0,.4);
  }
}
* { box-sizing: border-box; }
body { margin: 0; background: var(--bg); color: var(--text);
  font: 14px/1.4 system-ui, -apple-system, "Segoe UI", Roboto, sans-serif; }
header { position: sticky; top: 0; z-index: 5; background: color-mix(in srgb, var(--bg) 88%, transparent);
  backdrop-filter: blur(8px); border-bottom: 1px solid var(--border); }
.bar { max-width: 1400px; margin: 0 auto; padding: 12px 16px; display: flex; flex-wrap: wrap; gap: 12px; align-items: center; }
h1 { font-size: 17px; margin: 0 12px 0 0; font-weight: 650; }
h1 small { color: var(--muted); font-weight: 400; margin-left: 6px; }
input[type=search] { flex: 1 1 200px; min-width: 160px; padding: 7px 10px; border-radius: 8px;
  border: 1px solid var(--border); background: var(--panel); color: var(--text); font: inherit; }
.seg { display: inline-flex; border: 1px solid var(--border); border-radius: 8px; overflow: hidden; }
.seg button { border: 0; background: var(--panel); color: var(--muted); padding: 6px 10px; font: inherit; cursor: pointer; }
.seg button + button { border-left: 1px solid var(--border); }
.seg button.on { background: var(--accent); color: #fff; }
label.sz { display: inline-flex; align-items: center; gap: 6px; color: var(--muted); }
main { max-width: 1400px; margin: 0 auto; padding: 8px 16px 48px; }
section { margin-top: 22px; }
section h2 { font-size: 15px; margin: 0 0 10px; display: flex; align-items: center; gap: 8px; }
section h2 .count { font-size: 12px; color: var(--muted); background: var(--panel); border: 1px solid var(--border);
  border-radius: 999px; padding: 1px 8px; font-weight: 500; }
section.mod h2 { color: var(--mod); }
section.mod { border-top: 2px dashed var(--border); padding-top: 14px; }
.grid { display: grid; gap: 12px; grid-template-columns: repeat(auto-fill, minmax(calc(var(--tile) + 24px), 1fr)); }
.card { background: var(--panel); border: 1px solid var(--border); border-radius: 10px; box-shadow: var(--shadow);
  padding: 8px; cursor: pointer; display: flex; flex-direction: column; gap: 6px; min-width: 0;
  transition: transform .08s, border-color .08s; }
.card:hover, .card:focus-visible { border-color: var(--accent); transform: translateY(-1px); outline: none; }
.thumb { height: var(--tile); border-radius: 6px; display: flex; align-items: center; justify-content: center; overflow: hidden; }
.thumb img { max-width: 100%; max-height: 100%; image-rendering: auto; }
.thumb img.small { image-rendering: pixelated; }
.name { font-size: 12px; font-weight: 550; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
.meta { font-size: 11px; color: var(--muted); }
.cap { font-size: 11px; color: var(--accent); line-height: 1.25; }
body.bg-checker .thumb, body.bg-checker .lb-img {
  background-color: var(--chk-a);
  background-image: linear-gradient(45deg, var(--chk-b) 25%, transparent 25%, transparent 75%, var(--chk-b) 75%),
                    linear-gradient(45deg, var(--chk-b) 25%, transparent 25%, transparent 75%, var(--chk-b) 75%);
  background-size: 16px 16px; background-position: 0 0, 8px 8px; }
body.bg-dark .thumb, body.bg-dark .lb-img { background: #15171b; }
body.bg-light .thumb, body.bg-light .lb-img { background: #f4f4f4; }
.empty { color: var(--muted); padding: 30px 0; text-align: center; display: none; }
#lb { position: fixed; inset: 0; background: rgba(0,0,0,.72); display: none; align-items: center; justify-content: center;
  z-index: 20; padding: 16px; }
#lb.open { display: flex; }
.lb-box { background: var(--panel); border-radius: 12px; max-width: min(1100px, 100%); max-height: 100%;
  display: flex; flex-direction: column; overflow: hidden; box-shadow: 0 10px 40px rgba(0,0,0,.5); }
.lb-img { flex: 1 1 auto; min-height: 120px; display: flex; align-items: center; justify-content: center; padding: 16px; overflow: auto; }
.lb-img img { max-width: 100%; max-height: calc(100vh - 200px); }
.lb-info { padding: 12px 16px; border-top: 1px solid var(--border); display: flex; flex-wrap: wrap; gap: 8px 16px; align-items: center; }
.lb-info .t { font-weight: 650; }
.lb-info code { font-size: 12px; color: var(--muted); word-break: break-all; }
.lb-info .sp { flex: 1; }
.btn { border: 1px solid var(--border); background: var(--bg); color: var(--text); border-radius: 8px; padding: 5px 10px;
  font: inherit; cursor: pointer; }
.btn:hover { border-color: var(--accent); }
.nav { position: fixed; top: 50%; transform: translateY(-50%); background: rgba(255,255,255,.12); color: #fff; border: 0;
  width: 44px; height: 64px; border-radius: 10px; font-size: 26px; cursor: pointer; }
.nav:hover { background: rgba(255,255,255,.25); }
#prev { left: 8px; } #next { right: 8px; }
</style>
</head>
<body class="bg-checker">
<header><div class="bar">
  <h1>SR2 Icons<small id="total"></small></h1>
  <input type="search" id="q" placeholder="Filter by name or caption..." autocomplete="off">
  <div class="seg" id="bgseg">
    <button data-bg="checker" class="on">Checker</button><button data-bg="dark">Dark</button><button data-bg="light">Light</button>
  </div>
  <label class="sz">Size <input type="range" id="size" min="48" max="320" step="8" value="112"></label>
</div></header>
<main id="main"></main>
<div class="empty" id="empty">No icons match.</div>
<div id="lb" role="dialog" aria-modal="true">
  <button class="nav" id="prev" title="Previous (Left)">&#8249;</button>
  <div class="lb-box">
    <div class="lb-img"><img id="lbimg" alt=""></div>
    <div class="lb-info">
      <span class="t" id="lbname"></span><span class="meta" id="lbsize"></span>
      <span class="cap" id="lbcap"></span>
      <code id="lbpath"></code><span class="sp"></span>
      <button class="btn" id="copy">Copy path</button>
      <button class="btn" id="close">Close (Esc)</button>
    </div>
  </div>
  <button class="nav" id="next" title="Next (Right)">&#8250;</button>
</div>
<script id="data" type="application/json">__DATA__</script>
<script>
(function () {
  const DATA = JSON.parse(document.getElementById('data').textContent);
  const main = document.getElementById('main');
  const all = [];
  let total = 0;
  DATA.forEach(sec => {
    const s = document.createElement('section');
    if (sec.mod) s.className = 'mod';
    const h = document.createElement('h2');
    h.textContent = sec.title;
    const c = document.createElement('span'); c.className = 'count';
    h.appendChild(c); s.appendChild(h);
    const g = document.createElement('div'); g.className = 'grid'; s.appendChild(g);
    sec.cards = [];
    sec.items.forEach(it => {
      const card = document.createElement('div');
      card.className = 'card'; card.tabIndex = 0;
      const th = document.createElement('div'); th.className = 'thumb';
      const img = document.createElement('img'); img.loading = 'lazy'; img.src = it.src; img.alt = it.name;
      if (it.w && it.w <= 64) img.className = 'small';
      th.appendChild(img); card.appendChild(th);
      const n = document.createElement('div'); n.className = 'name'; n.textContent = it.name; n.title = it.name; card.appendChild(n);
      const m = document.createElement('div'); m.className = 'meta'; m.textContent = it.w + ' \u00d7 ' + it.h + ' px'; card.appendChild(m);
      if (it.cap) { const cp = document.createElement('div'); cp.className = 'cap'; cp.textContent = it.cap; card.appendChild(cp); }
      it.card = card; it.key = (it.name + ' ' + it.cap + ' ' + it.rel).toLowerCase();
      card.addEventListener('click', () => openLb(it));
      card.addEventListener('keydown', e => { if (e.key === 'Enter' || e.key === ' ') { e.preventDefault(); openLb(it); } });
      g.appendChild(card); all.push(it);
    });
    sec.el = s; sec.countEl = c;
    total += sec.items.length;
    main.appendChild(s);
  });
  document.getElementById('total').textContent = total + ' files';

  let visible = all.slice();
  function filter() {
    const q = document.getElementById('q').value.trim().toLowerCase();
    visible = [];
    DATA.forEach(sec => {
      let n = 0;
      sec.items.forEach(it => {
        const ok = !q || it.key.includes(q);
        it.card.style.display = ok ? '' : 'none';
        if (ok) { n++; visible.push(it); }
      });
      sec.countEl.textContent = q ? (n + ' / ' + sec.items.length) : String(sec.items.length);
      sec.el.style.display = n ? '' : 'none';
    });
    document.getElementById('empty').style.display = visible.length ? 'none' : 'block';
  }
  document.getElementById('q').addEventListener('input', filter);

  function store(k, v) { try { localStorage.setItem('sr2icons.' + k, v); } catch (e) {} }
  function load(k) { try { return localStorage.getItem('sr2icons.' + k); } catch (e) { return null; } }

  function setBg(bg) {
    document.body.classList.remove('bg-checker', 'bg-dark', 'bg-light');
    document.body.classList.add('bg-' + bg);
    document.querySelectorAll('#bgseg button').forEach(b => b.classList.toggle('on', b.dataset.bg === bg));
    store('bg', bg);
  }
  document.querySelectorAll('#bgseg button').forEach(b => b.addEventListener('click', () => setBg(b.dataset.bg)));
  const size = document.getElementById('size');
  function setSize(v) { document.documentElement.style.setProperty('--tile', v + 'px'); store('size', v); }
  size.addEventListener('input', () => setSize(size.value));
  const sv = load('size'); if (sv) size.value = sv;
  setSize(size.value);
  const bv = load('bg'); if (bv) setBg(bv);

  const lb = document.getElementById('lb');
  let cur = null;
  function openLb(it) {
    cur = it;
    document.getElementById('lbimg').src = it.src;
    document.getElementById('lbname').textContent = it.name;
    document.getElementById('lbsize').textContent = it.w + ' \u00d7 ' + it.h + ' px';
    document.getElementById('lbcap').textContent = it.cap || '';
    document.getElementById('lbpath').textContent = it.rel;
    document.getElementById('copy').textContent = 'Copy path';
    lb.classList.add('open');
  }
  function closeLb() { lb.classList.remove('open'); if (cur) cur.card.focus(); cur = null; }
  function step(d) {
    if (!cur || !visible.length) return;
    let i = visible.indexOf(cur);
    i = (i + d + visible.length) % visible.length;
    openLb(visible[i]);
  }
  document.getElementById('prev').addEventListener('click', e => { e.stopPropagation(); step(-1); });
  document.getElementById('next').addEventListener('click', e => { e.stopPropagation(); step(1); });
  document.getElementById('close').addEventListener('click', closeLb);
  lb.addEventListener('click', e => { if (e.target === lb) closeLb(); });
  document.addEventListener('keydown', e => {
    if (!lb.classList.contains('open')) return;
    if (e.key === 'Escape') closeLb();
    else if (e.key === 'ArrowLeft') step(-1);
    else if (e.key === 'ArrowRight') step(1);
  });
  document.getElementById('copy').addEventListener('click', () => {
    const txt = cur ? cur.rel : '';
    const btn = document.getElementById('copy');
    const done = () => { btn.textContent = 'Copied'; };
    const fallback = () => {
      const ta = document.createElement('textarea'); ta.value = txt; document.body.appendChild(ta); ta.select();
      try { document.execCommand('copy'); done(); } catch (e) { btn.textContent = 'Copy failed'; }
      ta.remove();
    };
    if (navigator.clipboard && window.isSecureContext) navigator.clipboard.writeText(txt).then(done, fallback);
    else fallback();
  });
  filter();
})();
</script>
</body>
</html>
"""


def main():
    data = collect()
    blob = json.dumps(data, ensure_ascii=False).replace("</", "<\\/")
    OUT.write_text(PAGE.replace("__DATA__", blob), encoding="utf-8")
    n = sum(len(s["items"]) for s in data)
    print(f"wrote {OUT} ({n} icons)")
    for s in data:
        print(f"  {len(s['items']):3d}  {s['title']}")


if __name__ == "__main__":
    main()
