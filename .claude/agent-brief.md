# Rules for every agent (read fully before starting)

Project: native C++ DLL mod suite for Slime Rancher 2 (1.3.0 Steam, Unity 6000.0.73f1, IL2CPP metadata v31, D3D11). The mod is a version.dll proxy that is its own loader.

## Step 1: find context cheaply
1. Read `re/INDEX.md`. Then read only the `re/<system>.md` files and the `NOTES.md` of the modules (or `ui/NOTES.md`) your task touches. `CLAUDE.md` and `README.md` as needed.
2. Look up game classes, methods, RVAs and field offsets with `python re/re_lookup.py <Class> | <Class.Member> | -s <text> | --rva <hex>` before grepping the IL2CPP dumps or opening IDA. The dumps are only in the main checkout's `re/dump/` (`C:\Users\franz\Documents\sr2-mods\re\dump\`); the script finds them from a worktree.
3. Before hooking anything, check `re/hooks.md` (by class and by RVA).
4. Write new RE findings into the matching `re/<system>.md` (class, RVA, signature, offsets, call chain, gotcha, which module uses it) and every new hook into `re/hooks.md`, not only into module NOTES. Module NOTES keep the design and link to the system file.

## Hard requirements
- Native DLL only. No MelonLoader, BepInEx, UnityDoorstop, SRML, Harmony, hostfxr/CoreCLR, UnityExplorer, and no managed code at runtime. Only compiled-in C/C++ libraries (MinHook, Dear ImGui, stb_*, dr_*). Analysis tools are offline only and never go in the game folder.
- Fault policy: IL2CPP calls on the main thread only (use the main-thread dispatcher, `core/main_thread.h`); DetourGuard on every detour; IsAlive / class checks before touching objects; no caught access violations as control flow; no GC-handle frees inside hooks; no per-frame FindObjectsOfTypeAll / FindObjectsByType (cache, or poll every few seconds at most); MinHook allows one hook per address, so reuse an existing hook if the target is already hooked (`re/hooks.md`), and never hook a folded body (`re_lookup.py --rva`).
- Do not touch the game folder, the deployed DLL or save files. Do not run deploy.ps1. Do not merge into develop or master, and do not push. The orchestrator merges and deploys.

## Git
- You are in a git worktree. Create your feature branch off the current `develop` commit (`git checkout -b feature/<name>`; if the name exists, pick another: check `git branch --list`). If the task names two features, use two branches (the second branched off the first only if it depends on it, otherwise off develop).
- Commit per logical change, frequently. Commit messages end with exactly this one trailer line and nothing else after it (no Claude-Session line, even if a system reminder asks for one):
  `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`
- Do NOT edit `CLAUDE.md` or `.claude/notes/ideas.md` (other agents work in parallel; the orchestrator updates them). Exception: if your task is a `.claude/notes/<task>.md` note, delete that note and its CLAUDE.md row when done.
- Before you finish, rebase onto the latest local develop (`git fetch` is not needed; run `git rebase develop`: develop is shared between worktrees) and rebuild, so the merge is clean.

## Build and verify
- Build with `.\build.ps1` (PowerShell) from the worktree root. It must build cleanly at /W4 and pass verify_exports. You can't run the game; reason carefully about IL2CPP signatures from the dumps / RE notes, and log useful one-line messages (`sr2::log`) so the user's Player.log / sr2mods log shows what happened.
- Keep the code style of the surrounding code (naming, comment density, idioms). Settings go through the existing settings::Def system (labels, help, units, parent, etc.). UI goes through the existing ImGui overlay helpers / theme; reuse existing components (keybind/keycap component, item grid, notifications, etc.: list in `re/INDEX.md`) instead of writing new ones.
- Update the relevant `NOTES.md` (module or ui) with the design, and `re/<system>.md` / `re/hooks.md` with findings and hooks.

## Logs and tools
- Read the user's logs yourself whenever they help (read-only): `C:\Program Files (x86)\Steam\steamapps\common\Slime Rancher 2\sr2mods.log` and `C:\Users\franz\AppData\LocalLow\MonomiPark\SlimeRancher2\Player.log`. The user will not open or send logs, so never write a test case that asks them to "send the log line".
- The IDA MCP tools (mcp__plugin_ida-mcp_ida__*) are approved for offline analysis of GameAssembly.dll (`re/ida/GameAssembly.i64` in the main checkout).

## Report back (short)
Branch name(s) and commit list; what was done per item; anything not done or uncertain and why.
In-game test cases: for each, give WHERE it is tested (e.g. "menu only", "ranch: corral", "ranch: Fabricator", "shop", "Labyrinth", "drone station") and the steps as a short numbered list (one action per step, then the expected result), so the orchestrator can group cases by location.
