#pragma once
// Locks gameplay input while the overlay is open (Unity main thread only).
//
// SR2 reads gameplay input through the Unity Input System, which does not go
// through the window's WM_KEY*/WM_MOUSE* messages the overlay can swallow.
// So in addition to the WndProc filter, while the overlay is open we:
//  - disable the game's own "main game" InputActionMap
//    (MonomiPark.SlimeRancher.Input.InputDirector._mainGame -> InputActionMap.Disable())
//    and re-enable it on close only if we were the ones who disabled it;
//  - unlock the cursor (UnityEngine.Cursor.lockState = None) so it can move,
//    restoring the previous lock mode on close.
// All lookups are by name; any failure just logs once and degrades to the
// WndProc filter.

namespace sr2::ui::game_input {

// Call every main-thread tick with the overlay's desired state.
void Tick(bool overlayOpen);

// True if the cursor was locked (hidden) by the game when the overlay opened,
// i.e. the overlay should draw its own software cursor.
bool WantSoftwareCursor();

}  // namespace sr2::ui::game_input
