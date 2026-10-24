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
