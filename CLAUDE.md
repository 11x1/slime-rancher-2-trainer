# CLAUDE.md

## Planned work

`.claude/notes/` holds notes on planned work, one file per task. Read the ones that touch what you are about to change. When you finish a planned task, delete its note or mark it done, and move whatever still matters into the relevant `NOTES.md`.

| note | about |
|---|---|
| `ideas.md` | unplanned ideas |
| `round-18.md` | round 17 test feedback, split into agent tasks |
| `autotest.md` | automated in-game tests of skipped cases (harness in the DLL, Python client) |
| `custom-icons.md` | replacing script-drawn icons with the user's drawings; vanilla icons meanwhile |

## Where the documentation lives

- `re/INDEX.md`: start here. Map of game systems (`re/<system>.md`: classes, RVAs, fields, gotchas), `re/hooks.md` (every hook; check before hooking), core helpers and UI components, and `re/re_lookup.py` (offline dump lookup).
- `.claude/agent-brief.md`: rules for every agent (context, fault policy, git, build, report format).
- `.claude/checklist/`: the user's in-game test checklist (artifact source, round builder, conventions).
- `README.md`: what the mod is, build, install, settings and profiles.
- `ui/NOTES.md`: the in-game Options tab and the ImGui overlay (layout, theme, fonts, Slimepedia slime logo).
- `modules/<name>/NOTES.md`: each module's design and findings.
- The IL2CPP dumps in `re/dump/` and the IDA database in `re/ida/` exist only in the main checkout, not in git.
