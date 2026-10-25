# debug_tools module notes

Developer tools Monomi Park left in the release build, switched back on. Module key `debug_tools`, on by default, but every tool is off by default, so it changes nothing until one is turned on. Options tab: Misc > General, its own card (group "Developer tools"). The research behind it, including everything that did not survive and why it is not here, is in `re/debug-mode.md`.

## Font debug overlay (`font_overlay`, default off)

The game's own `MonomiPark.SlimeRancher.UI.Debug.FontsUsedDebugUI` (1.3.0: `StartDebugUI` RVA 0x83C210, `OnEnable` 0x83B500, `OnGUI` 0x83B690). Its code is complete in 1.3.0; nothing calls it.

- `static FontsUsedDebugUI StartDebugUI()`: `new GameObject("FontsUsedDebugUI", typeof(RectTransform), typeof(Canvas), typeof(FontsUsedDebugUI))`, returns the component.
- `Awake`: an IMGUI `GUIStyle` (font size 12). `OnEnable`: subscribes to `TMPro_EventManager.TEXT_CHANGED_EVENT` and tracks every `TMP_Text` from `Object.FindObjectsOfType<TMP_Text>(true)` (once). Texts that change later are picked up through the event. `OnDisable` unsubscribes.
- `OnGUI` (Repaint only): for each tracked text whose fonts need updating, the distinct `TMP_CharacterInfo.fontAsset`s of its characters, joined with newlines; then for each live, active text a box (black, alpha up to 0.6) with that list at the text's screen position, brighter the closer the mouse (within ~20 px). Destroyed texts are removed by the component (`Object.op_Inequality` check before drawing).

What the module does: a main-thread frame callback, polled every 0.5 s. Option on and no live overlay: `StartDebugUI()`, then `Object.DontDestroyOnLoad(go)` so it survives scene-group changes, held by a GC handle. Option or module off: `Object.Destroy(go)`, handle freed. If the object dies anyway it is started again at the next poll (at most 3 failed starts in a row, then it waits until the option is toggled). No hooks. Log: `font overlay on (FontsUsedDebugUI started, kept across scene loads, start #N)` / `font overlay off (option off | module off)`.

Cost: the component's `OnGUI` loops over all tracked texts every repaint; fine for a developer view, not something to leave on.

Not seen in game: the look of the labels (IMGUI default font, drawn over the game UI), whether `FindObjectsOfType(true)` on enable causes a visible hitch in a big scene, and texts created after the overlay was turned on that never change their text (only the change event registers them).

## Entity hitboxes (`hitboxes`, default off; `hitbox_range` 2-40 m, default 12; `hitbox_triggers`, default off)

`hitboxes.cpp` (round 11: "a debug option to show entity hitboxes"). Wireframes of the colliders of entities near the camera, drawn by the overlay through `ui/hud.h` line segments (clipped at the near plane), hidden while the settings window is open.

- Every 0.5 s: `Physics.OverlapSphere(camera, range, all layers, triggers per option)`; kept: enabled colliders with an `Identifiable` in their parents (slimes, toys, food, plorts, gadgets, the mod's buildables) other than the player's, nearest 160 (by AABB distance), GC handles; shape read once.
- Every frame: `Transform.localToWorldMatrix` per collider; boxes (12 edges), spheres (three circles, radius x the largest axis scale like Unity), capsules (rings, sides, cap arcs), anything else (mesh colliders) its world AABB. Green = solid, yellow = trigger, blue = AABB.
- No hooks. Log `hitboxes: showing N entity collider(s) within R m` once per switch-on.
- Round 12 ("No hitboxes visible"): setup failed (`entity hitbox view unavailable (GetComponentInParent: method
  'GetComponentInParent(System.Type)' not found on class Component)`): 1.3.0 strips the one-argument
  `Component.GetComponentInParent(Type)`; it uses `(Type, bool includeInactive)` (false) like the rest of the
  mod.
- Round 13 ("They sometimes disappear when on screen and close enough"; log `showing 12 ... within 17 m (285
  found)`, another `23 ... (1184 found)`): the query's array is in no distance order and only its first 1024 were
  looked at, so near colliders past that came and went between refreshes; and any collider whose world AABB held
  the camera was dropped as "the player", which hid big or rotated ones and every trigger (toys' 5 m Influence
  spheres with Show triggers) once the camera came near. Now: one `Collider.get_bounds` per collider (up to 8192),
  sorted by the distance from the camera to the AABB (0 inside), then enabled / Identifiable checks on the nearest
  until 160 are kept; the player is skipped by its GameObject (`SceneContextUtility.TryGetSceneContext` ->
  `SceneContext.get_Player`, compared with the Identifiable's GameObject), the AABB test only as the fallback. The
  first log line says which (`player skipped by its GameObject | the camera inside its bounds`). Not seen in game:
  whether the player's Identifiable sits on the `SceneContext.Player` object (if not, its capsule shows round the
  camera).
- Not seen in game: frame cost with many slimes near (about 2 game calls per collider per frame), capsule directions on scaled objects.

## Highlight what the mod touches (`highlight`, default off)

`highlight.cpp` (round 14: "maybe add a debug menu to highlight what you want me to check, which
entities/buildables/map parts"). A registry, `modules/common/debug_marks.h`: modules publish categories of
world boxes (key, label, colour, boxes with an optional ground ring) while `Wanted()` is true; a category not
republished for 6 s is dropped. debug_tools sets `Wanted` from the option, and every frame draws each box as 12
edges (rings as 48 segments) through `ui/hud.h` lines, and the category label above each box as a text-only hud
frame per category (owner `debug_tools.highlight.<key>`). Boxes within 250 m, labels within 80 m, 600 boxes at
most. Camera matrices from `hitboxes::CameraFrame` (shared with the hitbox view). Hidden while the settings
window is open; publishers keep running. Log: `highlight on|off`, `highlight: N <key>, ...` on a change (at most
every 10 s).

Publishers now: slime_dj (`slime_dj.lamps.pulsing|idle|ice`, `slime_dj.speakers.playing|silent`; see
modules/slime_dj/NOTES.md "Debug highlighter"). Another module adds a category by calling
`debug_marks::Publish` on the main thread while `Wanted()`, and `Remove` when it stops.
Not seen in game: label clutter with many crystals, box sizes from renderer bounds (the pillar's glow spheres
are big).
