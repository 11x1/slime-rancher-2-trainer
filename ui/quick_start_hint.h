#pragma once
// The "Hold Shift to stay in the main menu" note drawn by the overlay while
// quick_start's auto-continue is pending (from launch until it fires or is
// cancelled), stacked under the "press <hotkey>" startup hint.
//
// Threading: quick_start publishes from the Unity main thread (its frame
// callback); every field is a relaxed / release-acquire atomic. The render
// thread only reads them, no game calls. A pending state that is not
// refreshed for kStaleMs (module stopped or faulted) is not drawn.

namespace sr2::ui::quick_start_hint {

inline constexpr unsigned long long kStaleMs = 20000;
inline constexpr unsigned long long kStayingMs = 2500;  // "Staying in the main menu" shown this long

// ---- any thread (normally the Unity main thread) ----
// Auto-continue is pending. secondsLeft < 0: not counting down yet (menu not ready).
void SetPending(double secondsLeft);
// Shift cancelled it: show "Staying in the main menu" for kStayingMs, then hide.
void SetStaying();
// Fired, finished or turned off: hide.
void Clear();

// ---- render thread ----
enum class Phase { Hidden = 0, Pending = 1, Staying = 2 };  // values match menu::Host::quickHint
struct View {
    Phase phase = Phase::Hidden;
    float secondsLeft = -1;  // Pending only; < 0 = unknown
};
View Get();

}  // namespace sr2::ui::quick_start_hint
