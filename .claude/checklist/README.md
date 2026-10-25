# Test checklist (the user's in-game test run)

The user tests each round from a private Artifact page: https://claude.ai/artifact/LVUtmDa1e6KDWD2WpQfY3V. `sr2-test-run.html` is its source. The page has the `db` capability. It saves the user's marks as they go, and you read them with the ArtifactData tool.

## Data
- `results/<case id>`: `{status, note, lastStatus, lastNote, updatedAt}`. `status` is `pass` / `fail` / `skip` / `""`. The page shows `last*` as the previous round's result.
- `misc/notes`: `{text, lastText}`. Free-text feedback; the user often writes the most important notes here.
- Case ids: `r<round><area><n>`, e.g. `r10dj3`. Keep old ids when a case carries over.

## Conventions (the user's rules)
- Group sections by WHERE the case is tested ("Menu only", "Ranch: corral", "Shop", "Grey Labyrinth", ...), in walking order, so the user does everything in one place in a row.
- Steps go in a numbered list, one action per step, then the expected result. In the source, `how` is an array of steps.
- Cases the user skipped, and older cases still untested, go under the Skipped fold, in sub-sections ("Skipped / Sandbox"). They keep their tabs.
- Never write a step that asks the user to send or open a log. Read `sr2mods.log` (game folder) and `Player.log` (`%USERPROFILE%\AppData\LocalLow\MonomiPark\SlimeRancher2\`) yourself.

## Building a new round
1. Collect the new cases from the agents' reports as `[location, id, title, [steps]]` (see `r10-cases.example.json`).
2. Copy `build_round.example.py` to the scratchpad. Set `ORDER` (locations in walking order) and the `Skipped / ...` lists (ids of the carried-over cases), then run it. It writes `rN-new.json`.
3. Copy `sr2-test-run.html` and `splice_round.py` next to it, then run `python splice_round.py rN-new.json <round> <commit> r<N-1>`. This backs up the html, replaces `SECTIONS` (carried-over ids are copied from the old html) and updates the header's round and commit. It prints any missing ids.
4. Reset the carried-over rows with one ArtifactData `batch`, so they show as untested with the last result kept:
   - for each id: `set results/<id>` to `{status:"", note:"", lastStatus:<old status>, lastNote:<old note>, updatedAt:<old>}`
   - `set misc/notes` to `{text:"", lastText:<old text>}`
5. Read the artifact (Artifact `read`), then publish the new html to the same `url`. Keep the capabilities (omit `capabilities`).
6. Copy the updated `sr2-test-run.html` back here and commit it.

## Reading results
ArtifactData `list` on `results` (filter ids by the round prefix plus the carried-over ids) and `get misc/notes`. A `fail` with a note is a task for the next round. A `skip` carries over.
