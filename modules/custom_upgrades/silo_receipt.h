#pragma once
// custom_upgrades: the silo Sell-All receipt. Pressing the Sell-All world button opens this
// confirmation dialog instead of selling at once: one row per sellable plort type (icon, name,
// count, current unit price, line total), the grand total, Cancel and Sell. module.cpp quotes the
// silo, opens the dialog, and on Sell quotes again: only an identical quote is sold (exactly the
// shown amounts); a different one refreshes the receipt instead.
//
// Built at runtime from uGUI parts in the game's menu style (SDFRoundedRectangle shapes, the drone /
// plot menu palette, a TMP font taken from the HUD): a root GameObject with its own overlay Canvas
// (CanvasScaler 1920 x 1080, GraphicRaycaster) and the game's own UIInputLocker component. Cancel /
// Sell are clones of the game's confirm-popup buttons (own buttons as a fallback); the panel is never
// taller than the screen (the plort rows scroll when they do not fit).
//
// Input lock, the way the game's menus do it (disassembly, 1.3.0): UIInputLocker.OnEnable (0x6B0F00)
// calls TimeDirector.Pause (0x550570: timeScale 0, ref-counted, raises OnPausedChanged) and pauses the
// non-UI sound; InputDirector.OnPausedChanged / UpdateInputMaps (0x9DBB00) then disables the player
// action map and enables the UI one (no movement, no camera look, no interact), and
// CursorLockHandler.LateUpdate frees the cursor while paused (mouse, not on gamepad).
// UIInputLocker.OnDisable (0x6B0DD0) undoes it (TimeDirector.Unpause). We deactivate the root
// before destroying it, so the unlock happens at once.
//
// Input while open: polled from the InputSystem devices (Keyboard.current / Gamepad.current
// ButtonControl.wasPressedThisFrame; nothing hooked): Left / Right, A / D, Tab, d-pad, left stick
// move between Cancel and Sell; Enter / Space / pad A press the focused one; Esc / pad B cancel;
// Up / Down, W / S, d-pad / left stick up / down and the mouse wheel scroll the rows (when they scroll).
// Mouse: both buttons and the dimmed backdrop (= Cancel) carry a UnityEngine.UI.Toggle that we poll
// and reset (no managed delegates). Input is ignored for the first 0.3 s, so the press that opened
// the dialog can never confirm it.
//
// Main thread only; every game call through Resolver::Invoke. Objects we keep across frames are
// GC-handled and freed only from posted main-thread jobs; everything is IsAlive / class checked
// again before use. No scans except one FindObjectOfType(TextMeshProUGUI) per open (font).

#include <string>
#include <vector>

struct Il2CppObject;

namespace sr2::il2cpp {
class Resolver;
}

namespace sr2::modules::custom_upgrades::ui {

// One receipt row: a plort type (IdentifiableType, a persistent asset), how many, at what price.
struct ReceiptLine {
    Il2CppObject* type = nullptr;
    int count = 0;
    int price = 0;  // current market value (rounded, as TrySell pays it)
    long long Total() const { return static_cast<long long>(count) * price; }
    bool operator==(const ReceiptLine&) const = default;
};

struct Receipt {
    std::vector<ReceiptLine> lines;  // sorted by type name (stable for comparison)
    bool marketAvailable = false;
    bool marketClosed = false;
    long long Total() const;
    int Count() const;
    bool Same(const Receipt& o) const {
        return lines == o.lines && marketAvailable == o.marketAvailable && marketClosed == o.marketClosed;
    }
};

class SiloReceipt {
public:
    enum class Result { kNone, kConfirm, kCancel };

    SiloReceipt();
    ~SiloReceipt();
    SiloReceipt(const SiloReceipt&) = delete;
    SiloReceipt& operator=(const SiloReceipt&) = delete;

    // Worker thread: resolve by name. false = unavailable (`why`): the button then sells at once.
    bool Setup(sr2::il2cpp::Resolver& R, const char* module, std::string& why);
    bool Ready() const { return ready_; }

    // Main thread. Builds and shows the dialog (input locked), `note` above the buttons (e.g. an empty
    // silo; Sell is disabled when nothing can be sold). false = could not be built.
    bool Open(const Receipt& r, const std::string& note = {});
    // Main thread: replace the rows (receipt changed) and show `note` above the buttons;
    // `canSell` false disables Sell (market closed / nothing left).
    void Refresh(const Receipt& r, const std::string& note, bool canSell);
    bool IsOpen() const;
    // Main thread, every frame (one branch while closed): input; a decision once per press.
    // kCancel also when the dialog died (scene change).
    Result Tick(float unscaledDt);
    // Main thread: remove the dialog (input unlocked at once). Safe when closed.
    void Close(const char* why);

private:
    struct Impl;
    Impl* impl_ = nullptr;
    bool ready_ = false;
};

}  // namespace sr2::modules::custom_upgrades::ui
