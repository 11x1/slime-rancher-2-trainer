#!/usr/bin/env python3
"""Offline lookup in the SR2 1.3.0 IL2CPP dump (Cpp2IL DiffableCs). Stdlib only.

Usage:
  python re/re_lookup.py NAME [NAME ...]    class, Class.Member, or Namespace.Class[.Member]
  python re/re_lookup.py -s TEXT            search member names (substring, case-insensitive)
  python re/re_lookup.py -c TEXT            search class names (substring, case-insensitive)
  python re/re_lookup.py --rva 0x4ED3F0     which methods sit at an RVA (folded bodies)
Options:
  -f / -m      class view: fields only / methods only
  -g TEXT      class view: only members whose line contains TEXT
  -a           class view: include compiler-generated nested types (<>c, d__N, DisplayClass)
  -n N         max lines per class / search (default 60; 0 = no limit)
  --rebuild    rebuild the index (otherwise built once, ~30 s, kept next to the dump)

The dump lives only in the main checkout (re/dump/, not in git); found via
`git rev-parse --git-common-dir`, so it works from a worktree. Override with SR2_DUMP.
RVAs: IDA VA = 0x180000000 + RVA. "[shared xN]" = folded body used by N methods: never hook it.
"""
import argparse
import os
import re
import sqlite3
import subprocess
import sys

SUB = os.path.join("cpp2il_diffable-cs_rva", "DiffableCs")
INDEX_NAME = "re_lookup.sqlite"
INDEX_VERSION = 3


def find_dump():
    env = os.environ.get("SR2_DUMP")
    if env:
        return env
    here = os.path.dirname(os.path.abspath(__file__))
    cands = []
    try:
        common = subprocess.run(["git", "rev-parse", "--git-common-dir"], cwd=here,
                                capture_output=True, text=True, timeout=10).stdout.strip()
        if common:
            if not os.path.isabs(common):
                common = os.path.join(here, common)
            cands.append(os.path.join(os.path.dirname(os.path.abspath(common)), "re", "dump"))
    except Exception:
        pass
    cands.append(os.path.join(here, "dump"))
    for c in cands:
        if os.path.isdir(os.path.join(c, SUB)):
            return c
    sys.exit("re_lookup: dump not found (looked in: %s). Set SR2_DUMP." % ", ".join(cands))


TYPE_RE = re.compile(r"^(?:\[.*\]\s*)?((?:public|private|internal|protected|static|sealed|abstract|readonly|"
                     r"unsafe|new|partial|ref|file|override|virtual)\s+)*(class|struct|interface|enum|record)\s+"
                     r"([^\s:]+)(?:\s*:\s*(.+))?$")
ADDR_RE = re.compile(r'\[Address\(RVA = "0x([0-9A-Fa-f]+)".*?Length = "0x([0-9A-Fa-f]+)"')
FOFF_RE = re.compile(r"//Field offset: 0x([0-9A-Fa-f]+)")
NAME_PAREN_RE = re.compile(r"([^\s(]+)\s*\(")
GEN_RE = re.compile(r"^<|__|DisplayClass|<>")


def build(dump, db_path):
    root = os.path.join(dump, SUB)
    tmp = db_path + ".tmp"
    if os.path.exists(tmp):
        os.remove(tmp)
    db = sqlite3.connect(tmp)
    db.executescript("""
        CREATE TABLE meta(k TEXT PRIMARY KEY, v TEXT);
        CREATE TABLE cls(id INTEGER PRIMARY KEY, name TEXT, lname TEXT, full TEXT, ns TEXT, asm TEXT,
                         kind TEXT, bases TEXT, file TEXT, gen INTEGER);
        CREATE TABLE mem(cid INTEGER, kind TEXT, name TEXT, lname TEXT, rva INTEGER, len INTEGER,
                         off INTEGER, sig TEXT, static INTEGER);
    """)
    cid = 0
    crow, mrow = [], []
    for asm in sorted(os.listdir(root)):
        adir = os.path.join(root, asm)
        if not os.path.isdir(adir):
            continue
        for dp, _, files in os.walk(adir):
            for fn in files:
                if not fn.endswith(".cs"):
                    continue
                path = os.path.join(dp, fn)
                rel = os.path.relpath(path, root)
                ns = ""
                stack = []          # (cid, depth, fullname)
                depth = 0
                pending_type = None
                pending_prop = None
                prop = None         # (name, depth)
                rva = ln = None
                with open(path, encoding="utf-8", errors="replace") as f:
                    for raw in f:
                        s = raw.strip()
                        if not s:
                            continue
                        if s.startswith("namespace ") and s.endswith(";"):
                            ns = s[10:-1].strip()
                            continue
                        if s == "{":
                            depth += 1
                            if pending_type is not None:
                                stack.append((pending_type[0], depth, pending_type[1], pending_type[2]))
                                pending_type = None
                            elif pending_prop is not None:
                                prop = (pending_prop, depth)
                                pending_prop = None
                            continue
                        if s.startswith("}"):
                            if prop and prop[1] == depth:
                                prop = None
                            while stack and stack[-1][1] >= depth:
                                stack.pop()
                            depth -= 1
                            continue
                        if s.startswith("["):
                            m = ADDR_RE.search(s)
                            if m:
                                rva, ln = int(m.group(1), 16), int(m.group(2), 16)
                            continue
                        if s.startswith("//"):
                            continue
                        tm = TYPE_RE.match(s)
                        if tm and "(" not in s.split(":")[0]:
                            kind, name, bases = tm.group(2), tm.group(3), (tm.group(4) or "").strip()
                            cid += 1
                            outer = stack[-1][2] if stack else ""
                            full = (outer + "/" + name) if outer else name
                            gen = 1 if (outer and GEN_RE.search(name)) else 0
                            if stack:
                                # nested in a generated type -> generated too
                                pass
                            crow.append((cid, name, name.lower(), full, ns, asm, kind, bases, rel, gen))
                            pending_type = (cid, full, kind)
                            rva = ln = None
                            continue
                        if not stack:
                            continue
                        cur = stack[-1][0]
                        fo = FOFF_RE.search(s)
                        if fo and "(" not in s.split("//")[0]:
                            decl = s.split("//")[0].strip().rstrip(";").strip()
                            decl = decl.split(" = ")[0] if "const " in decl else decl
                            toks = decl.split()
                            nm = toks[-1] if toks else "?"
                            static = 1 if (" static " in " " + decl + " " or "const " in decl) else 0
                            mrow.append((cur, "field", nm, nm.lower(), None, None, int(fo.group(1), 16),
                                         decl, static))
                            rva = ln = None
                            continue
                        body = s.split("//")[0].strip()
                        if prop is not None:
                            # property accessors: Cpp2IL also lists them as get_X / set_X methods
                            rva = ln = None
                            continue
                        if stack[-1][3] == "enum":
                            v = body.rstrip(",").strip()
                            if v:
                                nm = v.split("=")[0].strip()
                                mrow.append((cur, "value", nm, nm.lower(), None, None, None, v, 1))
                            continue
                        if body.endswith("{ }") or body.endswith(";") and "(" in body:
                            decl = body[:-3].strip() if body.endswith("{ }") else body[:-1].strip()
                            pm = NAME_PAREN_RE.search(decl)
                            nm = pm.group(1) if pm else decl
                            nm = nm.split(".")[-1] if "." in nm and not nm.startswith("<") else nm
                            nm = re.sub(r"<.*$", "", nm) or nm
                            static = 1 if re.search(r"\bstatic\b", decl.split("(")[0]) else 0
                            mrow.append((cur, "method", nm, nm.lower(), rva, ln, None, decl, static))
                            rva = ln = None
                            continue
                        if "(" not in body and not body.endswith(";"):
                            toks = body.split()
                            if toks:
                                pending_prop = "Item" if "this[" in body else toks[-1]
                            continue
                if len(mrow) > 200000:
                    db.executemany("INSERT INTO cls VALUES(?,?,?,?,?,?,?,?,?,?)", crow)
                    db.executemany("INSERT INTO mem VALUES(?,?,?,?,?,?,?,?,?)", mrow)
                    crow, mrow = [], []
    db.executemany("INSERT INTO cls VALUES(?,?,?,?,?,?,?,?,?,?)", crow)
    db.executemany("INSERT INTO mem VALUES(?,?,?,?,?,?,?,?,?)", mrow)
    db.executescript("""
        CREATE INDEX i_cls_lname ON cls(lname);
        CREATE INDEX i_mem_cid ON mem(cid);
        CREATE INDEX i_mem_lname ON mem(lname);
        CREATE INDEX i_mem_rva ON mem(rva);
    """)
    db.execute("INSERT INTO meta VALUES('version', ?)", (str(INDEX_VERSION),))
    db.commit()
    db.close()
    os.replace(tmp, db_path)


def open_db(dump, rebuild):
    db_path = os.path.join(dump, INDEX_NAME)
    ok = os.path.exists(db_path) and not rebuild
    if ok:
        try:
            db = sqlite3.connect(db_path)
            if db.execute("SELECT v FROM meta WHERE k='version'").fetchone()[0] == str(INDEX_VERSION):
                return db
            db.close()
        except Exception:
            pass
    print("re_lookup: building index (once) ...", file=sys.stderr)
    build(dump, db_path)
    return sqlite3.connect(db_path)


_shared_cache = {}


def shared(db, rva):
    if rva is None:
        return ""
    if rva not in _shared_cache:
        _shared_cache[rva] = db.execute("SELECT COUNT(*) FROM mem WHERE rva=?", (rva,)).fetchone()[0]
    n = _shared_cache[rva]
    return " [shared x%d]" % n if n > 1 else ""


def fmt_member(db, kind, rva, ln, off, sig, static):
    if kind == "value":
        return "  %-9s %s" % ("=", sig)
    if kind == "field":
        tag = "static" if static else "+0x%X" % off
        return "  %-9s %s" % (tag, sig)
    r = "0x%X" % rva if rva is not None else "-"
    return "  %-9s %s%s" % (r, sig, shared(db, rva))


def cls_title(row):
    _, name, full, ns, asm, kind, bases = row
    t = "%s %s" % (kind, full.replace("/", "."))
    if ns:
        t += "  (%s)" % ns
    if asm != "Assembly-CSharp":
        t += "  [%s]" % asm
    if bases:
        t += "  : " + bases
    return t


def resolve_classes(db, name):
    parts = name.split(".")
    rows = db.execute("SELECT id,name,full,ns,asm,kind,bases FROM cls WHERE lname=?", (parts[-1].lower(),)).fetchall()
    if len(parts) > 1:
        want_ns = ".".join(parts[:-1])
        exact = [r for r in rows if r[3] == want_ns or r[2].replace("/", ".").endswith(name)]
        if exact:
            rows = exact
    exact_case = [r for r in rows if r[1] == parts[-1]]
    rows = exact_case or rows
    rows.sort(key=lambda r: (r[4] != "Assembly-CSharp", "/" in r[2], r[3]))
    return rows


def show_class(db, row, a):
    print(cls_title(row))
    q = "SELECT kind,name,rva,len,off,sig,static FROM mem WHERE cid=? ORDER BY rowid"
    mems = db.execute(q, (row[0],)).fetchall()
    out = []
    for kind, nm, rva, ln, off, sig, static in mems:
        if a.f and kind not in ("field", "value") or a.m and kind != "method":
            continue
        if kind == "field" and sig.startswith(("public const", "private const", "internal const")) and not a.g:
            continue
        line = fmt_member(db, kind, rva, ln, off, sig, static)
        if a.g and a.g.lower() not in line.lower():
            continue
        out.append(line)
    nested = db.execute("SELECT name,gen FROM cls WHERE full LIKE ? AND full NOT LIKE ?",
                        (row[2] + "/%", row[2] + "/%/%")).fetchall()
    limit = a.n or len(out)
    for l in out[:limit]:
        print(l)
    if len(out) > limit:
        print("  ... %d more (use -n 0, -f, -m or -g)" % (len(out) - limit))
    shown = [n for n, g in nested if a.a or not g]
    if shown:
        print("  nested: " + ", ".join(shown))


def lookup(db, name, a):
    rows = resolve_classes(db, name)
    exact = [r for r in rows if r[1] == name.split(".")[-1]]
    if rows and (exact or "." not in name):
        rows = exact or rows
        for r in rows[:3]:
            show_class(db, r, a)
        if len(rows) > 3:
            print("  (+%d more classes named %s)" % (len(rows) - 3, name))
        return True
    if "." in name:
        cname, mname = name.rsplit(".", 1)
        crows = resolve_classes(db, cname)
        hit = False
        for c in crows:
            mems = db.execute("SELECT kind,rva,len,off,sig,static FROM mem WHERE cid=? AND lname IN (?,?,?)",
                              (c[0], mname.lower(), "get_" + mname.lower(), "set_" + mname.lower())).fetchall()
            if mems:
                print(cls_title(c))
                for k, rva, ln, off, sig, st in mems:
                    print(fmt_member(db, k, rva, ln, off, sig, st))
                hit = True
        if hit:
            return True
        if crows:
            print("%s: no member '%s' in %s (try -s %s)" % (name, mname, cname, mname))
            return False
    print("%s: not found (try -c / -s)" % name)
    return False


def search_members(db, text, a):
    q = ("SELECT c.full,c.ns,m.kind,m.rva,m.len,m.off,m.sig,m.static,c.gen,c.asm FROM mem m JOIN cls c ON c.id=m.cid "
         "WHERE m.lname LIKE ? ORDER BY c.asm!='Assembly-CSharp', c.gen, c.full")
    rows = db.execute(q, ("%" + text.lower() + "%",)).fetchall()
    rows = [r for r in rows if a.a or not r[8]]
    limit = a.n or len(rows)
    for full, ns, k, rva, ln, off, sig, st, gen, asm in rows[:limit]:
        where = full.replace("/", ".") + ("  (%s)" % ns if ns else "")
        print("%s\n%s" % (where, fmt_member(db, k, rva, ln, off, sig, st)))
    if len(rows) > limit:
        print("... %d more (-n 0)" % (len(rows) - limit))


def search_classes(db, text, a):
    rows = db.execute("SELECT id,name,full,ns,asm,kind,bases,gen FROM cls WHERE lname LIKE ? "
                      "ORDER BY asm!='Assembly-CSharp', full", ("%" + text.lower() + "%",)).fetchall()
    rows = [r for r in rows if a.a or not r[7]]
    limit = a.n or len(rows)
    for r in rows[:limit]:
        print(cls_title(r[:7]))
    if len(rows) > limit:
        print("... %d more (-n 0)" % (len(rows) - limit))


def by_rva(db, text):
    rva = int(text, 16)
    if rva >= 0x180000000:
        rva -= 0x180000000
    rows = db.execute("SELECT c.full,c.ns,m.sig FROM mem m JOIN cls c ON c.id=m.cid WHERE m.rva=? ORDER BY c.gen,c.full",
                      (rva,)).fetchall()
    print("0x%X: %d method(s)%s" % (rva, len(rows), " - FOLDED, do not hook" if len(rows) > 1 else ""))
    for full, ns, sig in rows[:40]:
        print("  %s: %s" % (full.replace("/", "."), sig))
    if len(rows) > 40:
        print("  ... %d more" % (len(rows) - 40))


def main():
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0],
                                usage="re_lookup.py [NAME ...] [-s TEXT] [-c TEXT] [--rva HEX] [-f|-m] [-g TEXT] [-a] [-n N]")
    p.add_argument("names", nargs="*")
    p.add_argument("-s", metavar="TEXT")
    p.add_argument("-c", metavar="TEXT")
    p.add_argument("--rva")
    p.add_argument("-f", action="store_true")
    p.add_argument("-m", action="store_true")
    p.add_argument("-g", metavar="TEXT")
    p.add_argument("-a", action="store_true")
    p.add_argument("-n", type=int, default=60)
    p.add_argument("--rebuild", action="store_true")
    a = p.parse_args()
    if not (a.names or a.s or a.c or a.rva or a.rebuild):
        print(__doc__)
        return 0
    dump = find_dump()
    db = open_db(dump, a.rebuild)
    ok = True
    for n in a.names:
        ok = lookup(db, n, a) and ok
    if a.s:
        search_members(db, a.s, a)
    if a.c:
        search_classes(db, a.c, a)
    if a.rva:
        by_rva(db, a.rva)
    return 0 if ok else 1


if __name__ == "__main__":
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    except Exception:
        pass
    sys.exit(main())
