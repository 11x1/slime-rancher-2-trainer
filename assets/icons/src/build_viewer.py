"""Build a local, offline icon browser for extracted SR2 icons.

Writes assets/icons/game/index.html (gitignored). PNGs are referenced by
relative path; nothing is embedded or uploaded. Rerun after extracting icons:

    python assets/icons/src/build_viewer.py [path-to-sr2-mods-repo]

Every PNG under assets/icons/game/ is picked up, in any subfolder depth
(settings/, slimepedia/, upgrades/plot/, all/<category>/<folder>/ ...).
The first folder level is the category (for all/ the second level, shown as
"all: <name>"); the full relative folder is the folder filter. Files whose
name starts with "_" (contact sheets, metadata) are skipped.

Optional: assets/icons/game/_names.json maps a PNG path relative to game/
to extra text (aliases of deduplicated sprites, source bundle). The text is
searchable and shown in the full-size view.
"""
import json
import struct
import sys
from pathlib import Path

REPO = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).resolve().parents[3]
ICONS = REPO / "assets" / "icons"
GAME = ICONS / "game"
OUT = GAME / "index.html"

# Known in-game uses, keyed by file stem (any folder).
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

# Categories listed first, in this order; the rest follow alphabetically, mod icons last.
CAT_ORDER = ["root", "settings", "slimepedia", "upgrades"]


def png_size(p):
    """Width and height from the PNG header (no decoding; fast for thousands of files)."""
    try:
        with open(p, "rb") as f:
            head = f.read(24)
        if head[:8] == b"\x89PNG\r\n\x1a\n" and head[12:16] == b"IHDR":
            return struct.unpack(">II", head[16:24])
    except OSError:
        pass
    return 0, 0


def item(p, base, prefix, cat, folder, extra):
    w, h = png_size(p)
    rel_game = p.relative_to(base).as_posix()
    return {
        "n": p.name,
        "s": prefix + rel_game,
        "r": p.relative_to(REPO).as_posix(),
        "w": w, "h": h,
        "c": cat, "f": folder,
        "cap": CAPTIONS.get(p.stem, ""),
        "x": extra.get(rel_game, ""),
    }


def collect():
    extra = {}
    names = GAME / "_names.json"
    if names.is_file():
        try:
            extra = json.loads(names.read_text(encoding="utf-8"))
        except ValueError:
            extra = {}
    items = []
    if GAME.is_dir():
        for p in GAME.rglob("*.png"):
            if p.name.startswith("_"):
                continue
            parts = p.relative_to(GAME).parts[:-1]
            if not parts:
                cat, folder = "root", "root"
            elif parts[0] == "all" and len(parts) > 1:
                cat, folder = "all: " + parts[1], "/".join(parts)
            else:
                cat, folder = parts[0], "/".join(parts)
            items.append(item(p, GAME, "", cat, folder, extra))
    for d, folder in ((ICONS, "mod"), (ICONS / "source", "mod/source")):
        if d.is_dir():
            for p in d.glob("*.png"):
                items.append(item(p, ICONS, "../", "mod icons (ours)", folder, {}))

    def cat_key(c):
        if c in CAT_ORDER:
            return (0, CAT_ORDER.index(c), c)
        if c.startswith("mod"):
            return (2, 0, c)
        return (1, 0, c.lower())

    items.sort(key=lambda it: (cat_key(it["c"]), it["f"].lower(), it["n"].lower()))
    cats = []
    for it in items:
        if not cats or cats[-1] != it["c"]:
            cats.append(it["c"])
    return items, cats


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
header { position: sticky; top: 0; z-index: 5; background: color-mix(in srgb, var(--bg) 90%, transparent);
  backdrop-filter: blur(8px); border-bottom: 1px solid var(--border); }
.bar { max-width: 1600px; margin: 0 auto; padding: 10px 16px; display: flex; flex-wrap: wrap; gap: 10px; align-items: center; }
h1 { font-size: 17px; margin: 0 8px 0 0; font-weight: 650; white-space: nowrap; }
h1 small { color: var(--muted); font-weight: 400; margin-left: 6px; }
input[type=search], select { padding: 7px 10px; border-radius: 8px; border: 1px solid var(--border);
  background: var(--panel); color: var(--text); font: inherit; min-width: 0; }
input[type=search] { flex: 1 1 220px; }
select { flex: 0 1 220px; max-width: 100%; }
.seg { display: inline-flex; border: 1px solid var(--border); border-radius: 8px; overflow: hidden; }
.seg button { border: 0; background: var(--panel); color: var(--muted); padding: 6px 10px; font: inherit; cursor: pointer; }
.seg button + button { border-left: 1px solid var(--border); }
.seg button.on { background: var(--accent); color: #fff; }
label.sz { display: inline-flex; align-items: center; gap: 6px; color: var(--muted); }
.chips { max-width: 1600px; margin: 0 auto; padding: 0 16px 10px; display: flex; flex-wrap: wrap; gap: 6px;
  max-height: 30vh; overflow-y: auto; }
.chip { border: 1px solid var(--border); background: var(--panel); color: var(--text); border-radius: 999px;
  padding: 2px 10px; font: inherit; font-size: 12px; cursor: pointer; }
.chip b { font-weight: 600; color: var(--muted); margin-left: 4px; }
.chip.on { border-color: var(--accent); background: var(--accent); color: #fff; }
.chip.on b { color: #fff; }
.chip.zero { opacity: .45; }
main { max-width: 1600px; margin: 0 auto; padding: 4px 16px 48px; }
h2 { font-size: 14px; margin: 22px 0 10px; display: flex; align-items: center; gap: 8px; word-break: break-all; }
h2 .count { font-size: 12px; color: var(--muted); background: var(--panel); border: 1px solid var(--border);
  border-radius: 999px; padding: 1px 8px; font-weight: 500; }
h2.mod { color: var(--mod); }
.grid { display: grid; gap: 10px; grid-template-columns: repeat(auto-fill, minmax(calc(var(--tile) + 18px), 1fr)); }
.card { background: var(--panel); border: 1px solid var(--border); border-radius: 10px; box-shadow: var(--shadow);
  padding: 6px; cursor: pointer; display: flex; flex-direction: column; gap: 4px; min-width: 0;
  transition: transform .08s, border-color .08s; }
.card:hover, .card:focus-visible { border-color: var(--accent); transform: translateY(-1px); outline: none; }
.thumb { height: var(--tile); border-radius: 6px; display: flex; align-items: center; justify-content: center; overflow: hidden; }
.thumb img { max-width: 100%; max-height: 100%; }
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
#more { height: 40px; }
#lb { position: fixed; inset: 0; background: rgba(0,0,0,.72); display: none; align-items: center; justify-content: center;
  z-index: 20; padding: 16px; }
#lb.open { display: flex; }
.lb-box { background: var(--panel); border-radius: 12px; max-width: min(1200px, 100%); max-height: 100%;
  display: flex; flex-direction: column; overflow: hidden; box-shadow: 0 10px 40px rgba(0,0,0,.5); }
.lb-img { flex: 1 1 auto; min-height: 120px; display: flex; align-items: center; justify-content: center; padding: 16px; overflow: auto; }
.lb-img img { max-width: 100%; max-height: calc(100vh - 220px); }
.lb-img img.small { image-rendering: pixelated; min-width: 128px; }
.lb-info { padding: 12px 16px; border-top: 1px solid var(--border); display: flex; flex-wrap: wrap; gap: 6px 16px; align-items: center; }
.lb-info .t { font-weight: 650; word-break: break-all; }
.lb-info code { font-size: 12px; color: var(--muted); word-break: break-all; }
.lb-info .x { font-size: 12px; color: var(--muted); flex-basis: 100%; word-break: break-word; }
.lb-info .x:empty { display: none; }
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
<header>
  <div class="bar">
    <h1>SR2 Icons<small id="total"></small></h1>
    <input type="search" id="q" placeholder="Search by name, alias or path..." autocomplete="off">
    <select id="cat" aria-label="Category"></select>
    <select id="folder" aria-label="Folder"></select>
    <div class="seg" id="bgseg">
      <button data-bg="checker" class="on">Checker</button><button data-bg="dark">Dark</button><button data-bg="light">Light</button>
    </div>
    <label class="sz">Size <input type="range" id="size" min="48" max="320" step="8" value="112"></label>
  </div>
  <div class="chips" id="chips"></div>
</header>
<main id="main"></main>
<div id="more"></div>
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
      <span class="x" id="lbx"></span>
    </div>
  </div>
  <button class="nav" id="next" title="Next (Right)">&#8250;</button>
</div>
<script id="data" type="application/json">__DATA__</script>
<script>
(function () {
  const DATA = JSON.parse(document.getElementById('data').textContent);
  const ITEMS = DATA.items, CATS = DATA.cats;
  const CHUNK = 240;
  const $ = id => document.getElementById(id);
  const url = p => p.split('/').map(encodeURIComponent).join('/');
  const main = $('main'), qEl = $('q'), catEl = $('cat'), folderEl = $('folder');
  const total = {}, folders = {};
  ITEMS.forEach(it => {
    it.k = (it.n + ' ' + it.cap + ' ' + it.r + ' ' + it.x).toLowerCase();
    total[it.c] = (total[it.c] || 0) + 1;
    (folders[it.c] = folders[it.c] || new Map()).set(it.f, (folders[it.c].get(it.f) || 0) + 1);
  });
  $('total').textContent = ITEMS.length + ' files';

  function store(k, v) { try { localStorage.setItem('sr2icons.' + k, v); } catch (e) {} }
  function load(k) { try { return localStorage.getItem('sr2icons.' + k); } catch (e) { return null; } }

  function opt(sel, value, text) { const o = document.createElement('option'); o.value = value; o.textContent = text; sel.appendChild(o); }
  opt(catEl, '', 'All categories (' + ITEMS.length + ')');
  CATS.forEach(c => opt(catEl, c, c + ' (' + total[c] + ')'));
  function fillFolders() {
    folderEl.innerHTML = '';
    const c = catEl.value;
    if (!c) { opt(folderEl, '', 'All folders'); folderEl.disabled = true; return; }
    folderEl.disabled = false;
    opt(folderEl, '', 'All folders in ' + c);
    [...folders[c].entries()].forEach(([f, n]) => opt(folderEl, f, f + ' (' + n + ')'));
  }

  let visible = [], shown = 0, lastFolder = null, grid = null, folderCount = {};
  function card(it) {
    if (it.card) return it.card;
    const cd = document.createElement('div'); cd.className = 'card'; cd.tabIndex = 0;
    const th = document.createElement('div'); th.className = 'thumb';
    const img = document.createElement('img'); img.loading = 'lazy'; img.decoding = 'async'; img.src = url(it.s); img.alt = it.n;
    if (it.w && it.w <= 64 && it.h <= 64) img.className = 'small';
    th.appendChild(img); cd.appendChild(th);
    const n = document.createElement('div'); n.className = 'name'; n.textContent = it.n; n.title = it.n + (it.x ? '\n' + it.x : ''); cd.appendChild(n);
    const m = document.createElement('div'); m.className = 'meta'; m.textContent = it.w + ' × ' + it.h + ' px'; cd.appendChild(m);
    if (it.cap) { const cp = document.createElement('div'); cp.className = 'cap'; cp.textContent = it.cap; cd.appendChild(cp); }
    cd.addEventListener('click', () => openLb(it));
    cd.addEventListener('keydown', e => { if (e.key === 'Enter' || e.key === ' ') { e.preventDefault(); openLb(it); } });
    return (it.card = cd);
  }
  function renderMore() {
    const end = Math.min(shown + CHUNK, visible.length);
    for (; shown < end; shown++) {
      const it = visible[shown];
      if (it.f !== lastFolder) {
        lastFolder = it.f;
        const h = document.createElement('h2');
        if (it.c.startsWith('mod')) h.className = 'mod';
        h.textContent = it.f;
        const c = document.createElement('span'); c.className = 'count'; c.textContent = folderCount[it.f];
        h.appendChild(c); main.appendChild(h);
        grid = document.createElement('div'); grid.className = 'grid'; main.appendChild(grid);
      }
      grid.appendChild(card(it));
    }
  }
  function chips(q) {
    const el = $('chips'); el.innerHTML = '';
    const hit = {};
    ITEMS.forEach(it => { if (!q || it.k.includes(q)) hit[it.c] = (hit[it.c] || 0) + 1; });
    const mk = (c, label, n, of) => {
      const b = document.createElement('button'); b.className = 'chip' + (catEl.value === c ? ' on' : '') + (n ? '' : ' zero');
      b.textContent = label; const s = document.createElement('b'); s.textContent = q ? n + ' / ' + of : of; b.appendChild(s);
      b.addEventListener('click', () => { catEl.value = c; fillFolders(); apply(); });
      el.appendChild(b);
    };
    mk('', 'all', Object.values(hit).reduce((a, b) => a + b, 0), ITEMS.length);
    CATS.forEach(c => mk(c, c, hit[c] || 0, total[c]));
  }
  function apply() {
    const q = qEl.value.trim().toLowerCase(), c = catEl.value, f = folderEl.value;
    visible = ITEMS.filter(it => (!c || it.c === c) && (!f || it.f === f) && (!q || it.k.includes(q)));
    folderCount = {};
    visible.forEach(it => { folderCount[it.f] = (folderCount[it.f] || 0) + 1; });
    main.innerHTML = ''; shown = 0; lastFolder = null; grid = null;
    renderMore();
    $('empty').style.display = visible.length ? 'none' : 'block';
    chips(q);
    store('cat', c); store('folder', f);
  }
  new IntersectionObserver(es => { if (es.some(e => e.isIntersecting) && shown < visible.length) renderMore(); },
    { rootMargin: '1200px' }).observe($('more'));

  let qt = 0;
  qEl.addEventListener('input', () => { clearTimeout(qt); qt = setTimeout(apply, 150); });
  catEl.addEventListener('change', () => { fillFolders(); apply(); });
  folderEl.addEventListener('change', apply);

  function setBg(bg) {
    document.body.classList.remove('bg-checker', 'bg-dark', 'bg-light');
    document.body.classList.add('bg-' + bg);
    document.querySelectorAll('#bgseg button').forEach(b => b.classList.toggle('on', b.dataset.bg === bg));
    store('bg', bg);
  }
  document.querySelectorAll('#bgseg button').forEach(b => b.addEventListener('click', () => setBg(b.dataset.bg)));
  const size = $('size');
  function setSize(v) { document.documentElement.style.setProperty('--tile', v + 'px'); store('size', v); }
  size.addEventListener('input', () => setSize(size.value));
  const sv = load('size'); if (sv) size.value = sv;
  setSize(size.value);
  const bv = load('bg'); if (bv) setBg(bv);

  const lb = $('lb');
  let cur = null;
  function openLb(it) {
    cur = it;
    const im = $('lbimg'); im.src = url(it.s); im.className = (it.w && it.w <= 64 && it.h <= 64) ? 'small' : '';
    $('lbname').textContent = it.n;
    $('lbsize').textContent = it.w + ' × ' + it.h + ' px';
    $('lbcap').textContent = it.cap || '';
    $('lbpath').textContent = it.r;
    $('lbx').textContent = it.x || '';
    $('copy').textContent = 'Copy path';
    lb.classList.add('open');
  }
  function closeLb() { lb.classList.remove('open'); if (cur && cur.card) cur.card.focus(); cur = null; }
  function step(d) {
    if (!cur || !visible.length) return;
    let i = visible.indexOf(cur);
    i = (i + d + visible.length) % visible.length;
    while (i >= shown) renderMore();
    openLb(visible[i]);
  }
  $('prev').addEventListener('click', e => { e.stopPropagation(); step(-1); });
  $('next').addEventListener('click', e => { e.stopPropagation(); step(1); });
  $('close').addEventListener('click', closeLb);
  lb.addEventListener('click', e => { if (e.target === lb) closeLb(); });
  document.addEventListener('keydown', e => {
    if (!lb.classList.contains('open')) return;
    if (e.key === 'Escape') closeLb();
    else if (e.key === 'ArrowLeft') step(-1);
    else if (e.key === 'ArrowRight') step(1);
  });
  $('copy').addEventListener('click', () => {
    const txt = cur ? cur.r : '';
    const btn = $('copy');
    const done = () => { btn.textContent = 'Copied'; };
    const fallback = () => {
      const ta = document.createElement('textarea'); ta.value = txt; document.body.appendChild(ta); ta.select();
      try { document.execCommand('copy'); done(); } catch (e) { btn.textContent = 'Copy failed'; }
      ta.remove();
    };
    if (navigator.clipboard && window.isSecureContext) navigator.clipboard.writeText(txt).then(done, fallback);
    else fallback();
  });

  const lc = load('cat');
  if (lc && total[lc]) catEl.value = lc;
  fillFolders();
  const lf = load('folder');
  if (lf && catEl.value && folders[catEl.value].has(lf)) folderEl.value = lf;
  apply();
})();
</script>
</body>
</html>
"""


def main():
    items, cats = collect()
    blob = json.dumps({"items": items, "cats": cats}, ensure_ascii=False, separators=(",", ":")).replace("</", "<\\/")
    OUT.write_text(PAGE.replace("__DATA__", blob), encoding="utf-8")
    print(f"wrote {OUT} ({len(items)} icons)")
    counts = {}
    for it in items:
        counts[it["c"]] = counts.get(it["c"], 0) + 1
    for c in cats:
        print(f"  {counts[c]:6d}  {c}")


if __name__ == "__main__":
    main()
