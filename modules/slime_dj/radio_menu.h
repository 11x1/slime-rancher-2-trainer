#pragma once
// slime_dj: the Corral Radio playlist menu (round 11). Opened by the blue Corral Radio button on a
// corral (custom_upgrades silo_world.h RadioButtonLook -> api.h OpenRadioMenu): every song of the
// music folder on the left, the corral's playlist on the right, the radio's switches below.
//
// Built at runtime from uGUI parts in the game's menu style, like custom_upgrades' Sell-All receipt
// (silo_receipt.h: same palette, SDFRoundedRectangle shapes, a TMP font taken from the HUD, an
// overlay Canvas with CanvasScaler 1920 x 1080 and the game's own UIInputLocker component, which
// pauses the game and frees the cursor while the menu is open). The lists are virtual: a fixed number
// of row slots whose texts change as the list scrolls, so 2000 songs cost no more than 10.
//
// Round 12: every list row is a button in the game's button colours, the header buttons (Radio, Shuffle,
// Clear) are clones of the game's popup button; no key hint bar, no action button row.
//
// Input (polled from the InputSystem devices, nothing hooked; ignored for the first 0.3 s):
//   Mouse (the main way): hovering a row lights it (the cursor follows the mouse); a click on a song adds
//   it, a click on a playlist entry plays it, the entry's small buttons move it up / down or remove it;
//   the wheel scrolls the list under the pointer; a click on the backdrop closes.
//   Keys (not printed): three focus areas - Songs, Playlist, the header buttons. Up / Down (W / S, d-pad,
//   left stick) move in a list (up past the top: the header), Left / Right (A / D) switch list or button,
//   Tab cycles the areas, Page Up / Down (pad LT / RT), Home / End. Enter / Space / pad A: add the song,
//   play the entry, press the button. Delete / Backspace / pad X remove the entry, Shift + Up / Down
//   (pad LB / RB) move it. R / pad Y radio on / off, F shuffle. Esc / pad B close.
//   Clicks are polled from UnityEngine.UI.Toggle components (no managed delegates); the cloned game
//   buttons from their Button (IsPressed last frame, released with the pointer inside).
//
// Main thread only; every game call through Resolver::Invoke. Objects kept across frames are GC-handled
// and freed only from posted main-thread jobs. No scans except one FindObjectOfType(TextMeshProUGUI) per
// open (the font).

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace sr2::il2cpp {
class Resolver;
}

namespace sr2::modules::slime_dj {

struct MenuSong {
    uint64_t id = 0;
    std::string name;
    double seconds = 0;
    bool playable = true;
};

struct MenuEntry {
    std::string name;
    double seconds = 0;
    bool missing = false;
    bool operator==(const MenuEntry&) const = default;
};

// What the menu shows of the radio (refreshed by the module; only changes are redrawn).
struct MenuState {
    std::string title;               // "Corral Radio - Corral 2419"
    std::vector<MenuEntry> playlist;
    int playing = -1;                // entry playing / played last (-1 = none)
    bool on = false, shuffle = false;
    bool available = true;           // the radio channel exists (false: "tuning in", edits refused)
    std::string status;              // one line: now playing / waiting / why not available
    bool operator==(const MenuState&) const = default;
};

enum class MenuOp { kAdd, kRemove, kUp, kDown, kClear, kToggle, kShuffle, kPlayAt, kClose };

struct MenuAction {
    MenuOp op = MenuOp::kClose;
    uint64_t song = 0;  // kAdd
    int index = -1;     // kRemove / kUp / kDown / kPlayAt
};

class RadioMenu {
public:
    RadioMenu();
    ~RadioMenu();
    RadioMenu(const RadioMenu&) = delete;
    RadioMenu& operator=(const RadioMenu&) = delete;

    // Worker thread: resolve by name. false = unavailable (`why`).
    bool Setup(sr2::il2cpp::Resolver& R, std::string& why);
    bool Ready() const { return ready_; }

    // Main thread. Builds and shows the menu (input locked). false = could not be built.
    bool Open(const MenuState& st, std::shared_ptr<const std::vector<MenuSong>> songs);
    // Main thread: new radio state / song list (redrawn only where they changed).
    void Update(const MenuState& st);
    void SetSongs(std::shared_ptr<const std::vector<MenuSong>> songs);
    // Main thread: a short feedback line under the lists ("Added ...") for a few seconds.
    void Note(const std::string& text, bool good);
    bool IsOpen() const;
    // Main thread, every frame (one branch while closed): input -> actions for the module. kClose also
    // when the menu died (scene change); the menu is then already closed.
    std::vector<MenuAction> Tick(float unscaledDt);
    // Main thread: remove the menu (input unlocked at once). Safe when closed.
    void Close(const char* why);

private:
    struct Impl;
    Impl* impl_ = nullptr;
    bool ready_ = false;
};

}  // namespace sr2::modules::slime_dj
