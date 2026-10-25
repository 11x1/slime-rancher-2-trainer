#pragma once
// Locks gameplay input while the overlay is open (Unity main thread only).
//
// SR2 reads gameplay input through the Unity Input System, which does not go
// through the window's WM_KEY*/WM_MOUSE* messages the overlay can swallow.
// So in addition to the WndProc filter, while the overlay is open we:
//  - disable the game's own "main game" InputActionMap
//    (MonomiPark.SlimeRancher.Input.InputDirector._mainGame -> InputActionMap.Disable())
//    and re-enable it on close only if we were the ones who disabled it;
//  - while the pointer is over the overlay (or a text field has focus), also
//    disable the menu map (InputDirector._paused: full map, pause menu, ...)
//    and, for the pointer, the EventSystem's input module, so a game menu
//    behind the overlay does not react to clicks, the wheel or keys meant for
//    the overlay; both are re-enabled as soon as the pointer leaves. The menu
//    map is held action by action and only the actions that were on come
//    back (a whole-map Enable in gameplay armed CloseMap on M);
//  - unlock the cursor (UnityEngine.Cursor.lockState = None) and show it
//    (Cursor.visible = true: the full map hides the hardware cursor through
//    CursorManager.HideCursorFor), then hand both back to the game's
//    CursorManager on close.
// All lookups are by name; any failure just logs once and degrades to the
// WndProc filter.

namespace sr2::ui::game_input {

// Call every main-thread tick with the overlay's desired state. `pointerOnOverlay`:
// the overlay wants the mouse (pointer over one of its windows, an ImGui drag,
// HUD edit mode); `typing`: an overlay text field has focus.
void Tick(bool overlayOpen, bool pointerOnOverlay, bool typing);

// True if the overlay should draw its own software cursor: the game had the
// cursor locked (hidden) when the overlay opened and it cannot be made visible.
bool WantSoftwareCursor();

// Any thread. A game menu (pause menu, full map, Slimepedia, inventory, ...)
// has freed the cursor: the game's CursorManager wants it unlocked. Updated on
// every tick while the overlay is open; true until the first tick after opening.
bool GameMenuOpen();

}  // namespace sr2::ui::game_input
