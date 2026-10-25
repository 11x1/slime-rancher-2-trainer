# History rewrite: only the user's own icon drawings (planned, prepared 2026-10-26)

The user: "would be nice if we can rewrite the AI generated ones and only have custom ones in the
repo". `11x1/slime-rancher-2-trainer` is public. Goal: the repo, including its whole history, holds
only the user's own icon drawings: no AI- / script-drawn icons, no game art.

The current tree was cleaned on branch `chore/icons-custom-only` (assets/icons/README.md,
custom-icons.md). This note is the history part: the inventory, the script
(`tools/rewrite_icon_history.py`), the dry run and the steps to apply it. **Not applied yet**: the
orchestrator runs it after every open branch is merged, then the user force-pushes.

## Inventory: every image-carrying path that ever existed

Found with `git log --all --no-renames --name-only --diff-filter=A` (every added path, all refs) and
a scan of every blob in the object store for PNG / JPEG / GIF / PDF signatures, base64 PNG
(`iVBORw0KGgo`), `data:image` and PNG bytes written as C arrays (`137,80,78,71`, `0x89, 0x50`).
Image data turned up only in `assets/icons/**`, `ui/mod_icons_data.inc` and, as code, in
`third_party/stb/stb_image*.h` (the PNG signature constant) and `autotest/*.py` (build data URIs at
run time, nothing embedded). `ui/fonts/*.inc` are fonts (OFL), `proxy/proxy_exports.inc` is code.

Evidence for "user's drawing": the commit says so and adds the drawing in `assets/icons/source/`,
custom-icons.md lists it, and every embedded version was re-made pixel for pixel from its
`source/` drawing with pad_icon.py's algorithm (checked 2026-10-26 for all current files and every
older kept version). Evidence for "script-drawn": added by `make_icons.py` in 5683616 "Add custom
mod icons and their generator scripts", c321aa0 and 6c8422c (tracker icons), README "script-drawn".

### Keep: the user's own drawings

| path | versions kept | evidence |
|---|---|---|
| `assets/icons/source/modstabicon_custom.png` | 1 | 46b2bca "Mods tab: the user's hand-drawn icon" |
| `assets/icons/mods_tab.png` | 2nd only (828adc2c = the drawing, byte-identical) | 46b2bca; the 1st version is script-drawn (below) |
| `assets/icons/source/ash_boost_custom.png` | 2 | 6649a67, 6540032 "the user's Ash Booster drawing (with embers)" |
| `assets/icons/ash_boost.png` | 2nd and 3rd (22a3d06b, 0d9740c8) | made from the drawing (pixel check); the 1st is script-drawn |
| `assets/icons/source/sprinkler_2_custom.png` | 1 | 1b96b97 "the user's Sprinkler drawings" |
| `assets/icons/growth_1.png` | 2nd and 3rd (fc8ed37e, df0128d6) | from sprinkler_2_custom (crop, then `--keep-place` in a907b77); the 1st is script-drawn |
| `assets/icons/source/sprinkler_1_custom.png`, `assets/icons/sprinkler_1.png` | all (2 of sprinkler_1) | 1b96b97, a907b77 |
| `assets/icons/source/music_1_custom.png`, `assets/icons/music_1.png` | all (2 of music_1) | febdf94, a907b77 |
| `assets/icons/source/plort_door_custom.png` | 1 | aa0ae72 "the user's plort door drawing" |
| `assets/icons/plort_door.png` | 2nd only (bd188871) | aa0ae72; the 1st (c321aa0) is script-drawn |
| `assets/icons/source/dj_booth_custom.png`, `dj_booth.png`, `dj_booth_large.png` | all | 36641bd "the user's DJ Booth drawing" |
| `assets/icons/source/dj_speaker_custom.png`, `dj_speaker.png`, `dj_speaker_large.png` | all | f10b5f4, 6c85581 |
| `assets/icons/source/slime_radio_custom.png`, `slime_radio.png`, `slime_radio_large.png` | all | d46175d (same blobs as the DJ Speaker drawing, the user's choice) |

Note: `sprinkler_1`, `growth_1` and `music_1` are the user's redraws of game card icons on the
vanilla canvas; the user counts them as their own drawings (custom-icons.md), so they stay.

### Remove: AI / script-drawn (every version unless noted)

| path | added in | note |
|---|---|---|
| `collector_2.png`, `collector_3.png`, `feeder_2.png`, `sell_all_now.png` | 5683616 | retired upgrades, deleted in 9d5f497 |
| `coop_air_net.png`, `growth_2.png`, `hens_1.png`, `hens_2.png`, `music_2.png`, `sell_all.png` | 5683616 | deleted in 9d5f497 (vanilla icons since) |
| `hen_collector.png` | 5683616 | deleted in 5e9318a |
| `ash_boost.png`, `growth_1.png`, `mods_tab.png` | 5683616 | **1st version only**, by blob id (28f6149e, 457b1f1d, cef6a7c2); later versions are the user's |
| `plort_door.png` | c321aa0 | **1st version only** (a95781f6) |
| `gordo.png`, `shadow_door.png` | c321aa0 | deleted in 9d5f497 |
| `ghost_drone.png`, `map_node.png`, `research_drone.png` | 6c8422c | renamed to `*_placeholder.png` in 9d5f497 |
| `ghost_drone_placeholder.png`, `map_node_placeholder.png`, `research_drone_placeholder.png` | 9d5f497 | deleted on chore/icons-custom-only |
| `assets/icons/src/make_icons.py`, `assets/icons/src/iconkit.py` | 5683616 | the drawing code of all of the above (no image, but it regenerates them); deleted on chore/icons-custom-only |

(all PNG paths under `assets/icons/`)

### Remove: game-derived (the user's decision 2026-10-26)

| path | added in | note |
|---|---|---|
| `assets/icons/source/hencollector_custom.png` | 2267a1d | the user's edit of the game's collector body; replaced by roostercollector in 6f163bc |
| `assets/icons/source/roostercollector_custom.png` | 6f163bc | same |
| `assets/icons/source/eldercollector_custom.png` | 098a951 | same |
| `assets/icons/hen_collector_source.png` | 2267a1d (2 versions) | made from the two above |
| `assets/icons/elder_collector_source.png` | 098a951 | made from eldercollector |

These stay on the user's disk in the gitignored `assets/icons/local/` (originals in
`assets/icons/local/source/`), embedded only in local builds.

### Remove: generated

| path | note |
|---|---|
| `ui/mod_icons_data.inc` | 16 versions (32a8786 .. 9d5f497 and later); embeds the bytes of the removed icons. Generated at build time since chore/icons-custom-only |

Totals: 27 paths removed wholesale + 4 blobs by id = 49 blobs; 22 image blobs stay.

## The script

`tools/rewrite_icon_history.py` (Python 3 + `python -m pip install --user git-filter-repo`, offline,
never in the game folder). It clones the given repo with `git clone --mirror --no-local` into a new
scratch folder (all branches, tags, stash), runs `git filter-repo --sensitive-data-removal
--no-fetch --invert-paths --paths-from-file ... --strip-blobs-with-ids ...` there, expires reflogs,
`gc --prune=now`, and checks:
- (a) none of the removed paths or blobs is left: `git rev-list --objects --all` (paths and blob
  ids) and every object in the store (`cat-file --batch-all-objects`); every image blob left is on
  the keep list;
- (b) develop's tree is byte-identical (same tree id) to before the rewrite, and so is every
  branch tip whose tree held nothing removed; the branches whose tips still hold removed files are
  listed in `<work>/tips-with-removed-files.txt`;
- (c) `--build`: checks develop out of the rewritten clone (a linked worktree, so build.ps1 takes
  its turn with 2 jobs) and builds it: a clean checkout without the .inc and without local icons.
`--simulate-merge <branch>` merges a branch into develop inside the clone first (dry runs before
that branch is merged).

### Dry run (2026-10-26, clone of the main repo, `--simulate-merge chore/icons-custom-only --build`)

DRYRUN_RESULTS

## Applying it for real (orchestrator, then the user)

Preconditions:
1. Every open branch is merged into develop or dropped, including chore/icons-custom-only and the
   running agents' worktree branches (`worktree-agent-*` and their feature branches). Branches
   made from the old history and not rewritten would bring the old commits (and the icons) back
   on the next merge or push. `git worktree list` shows only the main checkout (remove the others
   with `git worktree remove`), no agents are running.
2. Before the merge of chore/icons-custom-only: copy the local-only icons, because the merge
   deletes them from the main working tree (they are tracked files there until then):
   ```
   New-Item -ItemType Directory -Force assets\icons\local\source
   Copy-Item assets\icons\hen_collector_source.png, assets\icons\elder_collector_source.png assets\icons\local\
   Copy-Item assets\icons\source\roostercollector_custom.png, assets\icons\source\eldercollector_custom.png assets\icons\local\source\
   ```
   (If that is missed: `git show <old develop>:assets/icons/hen_collector_source.png > ...` before
   the rewrite.)
3. `git fetch origin` and `git ls-remote origin`: GitHub had 21 branches (develop, master, 19 old
   `feature/*`), no tags, no `refs/pull/*` on 2026-10-26. Local develop must contain origin/develop
   (`git merge-base --is-ancestor origin/develop develop`); every other origin branch equals its
   local branch.
4. A backup: `git clone --mirror --no-local C:\Users\franz\Documents\sr2-mods D:\backup\sr2-mods-before-rewrite.git`
   (keep it private, never push it).

Run:
5. `python -P tools\rewrite_icon_history.py --work C:\scratch\rewrite` (no `--simulate-merge`; add
   `--build` for check (c)). It must end with `ALL CHECKS PASSED` and "0 branch tips still hold
   removed files".
6. Push from the rewritten clone (this is the user's force push):
   ```
   cd C:\scratch\rewrite\mirror.git
   git push --force https://github.com/11x1/slime-rancher-2-trainer.git develop master feature/collectibles feature/core feature/custom-upgrades feature/hl2-movement feature/inject-test feature/instant-build feature/minimap feature/overlay-sr2-theme feature/overlay-ui feature/plot-upgrades feature/profiles feature/proxy feature/quick-start feature/quick-travel feature/re-notes feature/sell-upgrades feature/spawn-scaling feature/third-party feature/wall-markers
   ```
   (exactly the branches `git ls-remote origin` lists). In the dry run only develop and
   `feature/overlay-sr2-theme` got new SHAs; the other 18 old branches and master are unchanged. Then `git ls-remote origin` again: the
   develop SHA is the rewritten one.
7. Switch the main checkout to the new history (its ignored files, `re/dump`, `re/ida`,
   `assets/icons/game`, `assets/icons/local`, saves, stay where they are):
   ```
   cd C:\Users\franz\Documents\sr2-mods
   git checkout develop
   git fetch --force --update-head-ok C:\scratch\rewrite\mirror.git "+refs/heads/*:refs/heads/*"
   git reset --hard develop          # same tree as before: no file changes
   git fetch origin --prune
   git for-each-ref --format="%(refname:short)" refs/heads   # delete any branch not in the rewritten clone
   git stash list                    # old stash entries point at old commits: drop them (git stash clear)
   git reflog expire --expire=now --all
   git gc --prune=now
   ```
   Then run step 5's check (a) against the main repo itself: `git rev-list --objects --all` must not
   list `ui/mod_icons_data.inc` or any removed path.
8. `.\build.ps1` once in the main checkout (the log says `icons: ... 2 local` with the local folder).

## What GitHub (and others) may still keep

- Old commits stay reachable by SHA on github.com (commit pages, `/blob/<sha>/...`, raw URLs,
  cached views) until GitHub garbage-collects them, which a force push alone does not trigger.
  To purge: GitHub Support, "remove cached views / sensitive data", with the repo and the first
  changed commits (filter-repo's `--sensitive-data-removal` writes them in
  `mirror.git/filter-repo/first-changed-commits` and prints a summary). Optional: the icons are not
  secrets, but it is the only way to make the old blobs unreachable on GitHub.
- Forks keep their own copy of the old history (GitHub does not rewrite forks). Check the repo's
  Forks / Network page; nothing can be done there except asking the fork owners.
- Anyone's existing clone keeps the old objects; on a pull they would get a merge of old and new
  history: they should re-clone.
- GitHub Actions artifacts / caches and release assets built from the old tree: a `version.dll`
  built before this branch embeds the removed icons (hen / elder collector, placeholders). Delete
  such artifacts and release assets (none existed as tags on 2026-10-26; check Releases and
  Actions anyway).
- PR refs (`refs/pull/*`) cannot be force-pushed; there were none.
- Commit messages still name the removed icons (text only, no image data): that is fine.
