#!/usr/bin/env python3
"""Release notes grouped by branch (used by .github/workflows/release.yml; runs locally too).

    python .github/scripts/release_notes.py                     # since the newest dev-* tag
    python .github/scripts/release_notes.py --kind master       # since the newest v0.* tag
    python .github/scripts/release_notes.py --since v0.3        # since any ref
    python .github/scripts/release_notes.py --until develop     # up to another ref (default HEAD)

Every "Merge <branch> into develop" commit in <since>..<until> becomes a "### <branch>" heading
with that branch's own commit subjects under it (<merge>^1..<merge>^2, no merges). Commits that
came through no branch go under "Other". Other merges (develop into master, develop into a
branch) are left out. Only subjects are printed, so trailers like Co-Authored-By never appear.
Only commits that change the DLL count (BUILD_PATHS; notes, docs, the checklist and the test harness
don't), so a branch that only touched docs is left out. Keep BUILD_PATHS in step with the `paths:`
filter in release.yml, which skips the whole run for such pushes.
Newest branch first. Stdlib only; needs git on PATH and the full history (fetch-depth: 0).
"""

import argparse
import re
import subprocess
import sys

MERGE_RE = re.compile(r"^Merge (?:branch |remote-tracking branch )?'?([^' ]+)'? into '?develop'?$")
MAX_GROUPS = 80      # a first release (no previous tag) would otherwise list the whole history
MAX_PER_GROUP = 40
# what goes into version.dll (git pathspecs); the same list as release.yml's `paths:` filter
BUILD_PATHS = [
    "core", "modules", "ui", "proxy", "third_party", "cmake", "CMakeLists.txt", "build.ps1",
    "assets/icons/*.png", "assets/icons/src/gen_inc.py",
    ":(exclude)*.md", ":(exclude)modules/autotest", ":(exclude)ui/autotest_view.*",
]


def git(*args):
    return subprocess.run(["git", *args], check=True, capture_output=True, text=True,
                          encoding="utf-8").stdout


def newest_tag(pattern, until):
    """Newest tag matching pattern that is an ancestor of `until` (tag names sort by version/date)."""
    for tag in git("tag", "--list", pattern, "--sort=-v:refname").split():
        if subprocess.run(["git", "merge-base", "--is-ancestor", tag, until],
                          capture_output=True).returncode == 0:
            return tag
    return None


def log(rev_args):
    """[(sha, parents[], subject)] for `git log <rev_args>`, newest first."""
    out = git("log", "--format=%H%x1f%P%x1f%s", *rev_args)
    rows = []
    for line in out.splitlines():
        sha, parents, subject = line.split("\x1f", 2)
        rows.append((sha, parents.split(), subject))
    return rows


def notes(since, until):
    rng = [f"{since}..{until}"] if since else [until]
    commits = log(rng)
    touching = {c[0] for c in log(rng + ["--no-merges", "--", *BUILD_PATHS])}
    seen = set()
    groups = []          # (branch, [subjects])
    for sha, parents, subject in commits:
        m = MERGE_RE.match(subject)
        if not m or len(parents) < 2:
            continue
        # the branch's own commits: reachable from the branch tip, not from develop before the
        # merge, and not already released
        excl = [f"^{parents[0]}"] + ([f"^{since}"] if since else [])
        own = [c for c in log(["--no-merges", parents[1], *excl]) if c[0] not in seen]
        seen.update(c[0] for c in own)
        seen.add(sha)
        subjects = [c[2] for c in own if c[0] in touching]
        if not subjects:
            continue         # docs / notes / harness only: nothing in the DLL changed
        prev = next((g for g in groups if g[0] == m.group(1)), None)   # merged more than once
        if prev:
            prev[1].extend(subjects)
        else:
            groups.append((m.group(1), subjects))
    other = [s for sha, parents, s in commits if sha not in seen and len(parents) < 2 and sha in touching]

    lines = []
    for branch, subjects in groups[:MAX_GROUPS]:
        lines.append(f"### {branch}")
        lines += [f"- {s}" for s in subjects[:MAX_PER_GROUP]] or ["- (no own commits)"]
        if len(subjects) > MAX_PER_GROUP:
            lines.append(f"- ... and {len(subjects) - MAX_PER_GROUP} more")
        lines.append("")
    if len(groups) > MAX_GROUPS:
        lines += [f"... and {len(groups) - MAX_GROUPS} older branches", ""]
    if other:
        lines.append("### Other")
        lines += [f"- {s}" for s in other[:MAX_PER_GROUP]]
        if len(other) > MAX_PER_GROUP:
            lines.append(f"- ... and {len(other) - MAX_PER_GROUP} more")
        lines.append("")
    if not lines:
        lines = ["No changes since the previous release.", ""]
    return "\n".join(lines)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--kind", choices=["dev", "master"], default="dev",
                    help="which previous tag to start from: dev-* (default) or v0.*")
    ap.add_argument("--since", help="start ref (exclusive); default: newest tag of --kind")
    ap.add_argument("--until", default="HEAD", help="end ref (default HEAD)")
    ap.add_argument("--out", help="write to this file instead of stdout")
    a = ap.parse_args()

    since = a.since or newest_tag("dev-*" if a.kind == "dev" else "v0.*", a.until)
    if not since and a.kind == "dev":
        since = newest_tag("v0.*", a.until)   # first dev release: since the last numbered one
    if not since and a.kind == "master":
        # first numbered release: since the previous master commit when `until` is the
        # "Merge develop into master" commit
        parents = git("log", "-1", "--format=%P", a.until).split()
        since = parents[0] if len(parents) > 1 else None
    body = f"Changes since {since or 'the start'}:\n\n" + notes(since, a.until)
    if a.out:
        with open(a.out, "w", encoding="utf-8", newline="\n") as f:
            f.write(body)
    else:
        sys.stdout.reconfigure(encoding="utf-8")
        sys.stdout.write(body)


if __name__ == "__main__":
    main()
