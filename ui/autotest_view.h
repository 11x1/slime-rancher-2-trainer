#pragma once
// What the test harness (modules/autotest) shows and asks in the overlay: the "Test harness connected"
// tag, the "Test question" panel with flashing screen edges, and back-buffer screenshots.
//
// Threading:
//   * The module (its pipe threads, never the Unity main thread for any of this) sets the armed flag,
//     opens / cancels a question and waits for its answer, and asks for screenshots and waits for them.
//   * The render thread (overlay.cpp, inside Present) draws the tag and the panel, takes the answer
//     (mouse, keys 1-4, the note field), and copies the back buffer when a screenshot is pending.
//   * Everything here is guarded by one mutex plus condition variables; no game calls.
//   * The main thread only reads WantsGameInputLock / PointerOnPanel / Typing (atomics) for the
//     per-action input lock (game_input.h) while the pointer is over the panel or the note field types.

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace sr2::ui::autotest {

// ---- connection tag (any thread) ----
void SetArmed(bool armed);
bool Armed();

// ---- questions (module side, any thread but the render thread) ----
struct Question {
    uint64_t id = 0;
    std::string text;
    std::vector<std::string> options;  // 2-4 labels
    std::string caseId;                // optional: shown small under the question
    int step = 0;                      // optional (0 = none)
};
struct Answer {
    uint64_t id = 0;
    std::string choice;   // the chosen label; "" when cancelled / timed out
    std::string note;     // the typed note ("" = none)
    bool cancelled = false;
};
// Opens `q` (replaces an open question, which is cancelled).
void Ask(const Question& q);
// Closes the open question (if `id` is 0 or matches): its waiter gets cancelled = true.
void CancelAsk(uint64_t id = 0);
// Waits until question `id` is answered or cancelled, at most `timeoutMs` (< 0: no limit), checking
// `*stop` (may be null) every 50 ms. False on timeout / stop (the question stays open; cancel it).
bool WaitAnswer(uint64_t id, long long timeoutMs, Answer& out, const std::atomic<bool>* stop);
bool QuestionOpen();

// ---- screenshots (module side) ----
struct Shot {
    bool ok = false;
    std::string error;
    unsigned width = 0, height = 0;
    std::vector<unsigned char> rgba;  // width * height * 4, top row first, alpha 255
};
// Asks the render thread for the next frame's back buffer (`withOverlay`: after the overlay drew, else
// the game's frame only) and waits at most `timeoutMs`. One at a time (a second caller waits its turn).
Shot TakeShot(bool withOverlay, long long timeoutMs);

// ---- render thread (overlay.cpp) ----
// Anything to draw (tag or question): the overlay renders a frame for it.
bool HasContent();
// A question is open: the window's input messages are queued for ImGui also while the overlay is closed.
bool WantsInput();
// Last frame: the pointer was over the panel / its note field had the keyboard (atomics, any thread).
bool PointerOnPanel();
bool Typing();
// The panel wants the game's input locked (pointer on it or typing). Any thread (main thread polls it).
bool WantsGameInputLock();
// Draws the tag and the question (between NewFrame and Render). `foreground`: the game window is in
// front (keys 1-4 are polled only then); `overlayTyping`: an overlay text field had the keyboard last
// frame (keys go there, not to the question).
void Draw(float scale, bool foreground, bool overlayTyping);
// A screenshot is pending for this phase (before / after the overlay drew).
bool ShotWanted(bool afterOverlay);
// The captured frame (or an error) for the pending screenshot.
void DeliverShot(Shot shot);

}  // namespace sr2::ui::autotest
