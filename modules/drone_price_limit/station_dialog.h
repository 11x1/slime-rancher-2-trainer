#pragma once
// drone_price_limit: the game-UI part. In the Quantum Drone station menu (DroneProgramMenu, the
// "Programming" page) a "Price limit" action is shown next to the destination's "Change" action while
// the selected destination is the Plort Market; it (or the C key / right stick click) opens a
// game-styled dialog: limit mode (minimum price / minimum fraction of the base price), a slider for
// the value (0 = no limit) and a base-price cheat sheet of every plort in the market terminal's order.
// Built at runtime from uGUI parts and clones of the menu's own prompt; no managed code of ours.
// Also the UI of custom_upgrades' several programs per station (drone_programs.h): a "Programs" action
// next to the target's "Change" action (P key / left stick click) opens a dialog listing the station's
// programs (Edit = run it now and show it on the page, Remove, Add program, when to take the next one).
// It lives here because this file owns the menu hooks (one hook per address); it shows only while
// custom_upgrades is on.
// Findings and layout: see station_dialog.cpp and modules/drone_price_limit/NOTES.md.
//
// Threading: Setup on the worker thread (resolves by name, installs the UI hooks through the module
// context); Tick / Shutdown on the Unity main thread, called by the module inside its DetourGuard.

#include <cstdint>
#include <functional>
#include <string>

struct Il2CppObject;

namespace sr2::core {
class ModuleContext;
}

namespace sr2::modules::drone_price_limit {

enum class LimitMode : int { kPrice = 0, kFraction = 1 };

// A station's limit: drones sell a plort only while its market price is >= the limit.
struct StationLimit {
    LimitMode mode = LimitMode::kPrice;
    int value = 0;  // Newbucks (kPrice) or percent of the plort's base price (kFraction); 0 = no limit
    bool Active() const { return value > 0; }
    bool operator==(const StationLimit&) const = default;
};

// "120 newbucks", "80% of the base price", "no limit".
std::string Describe(const StationLimit& l);
// Persisted form ("price:120", "fraction:80", "" for no limit) and back (false = not parsable).
std::string Serialize(const StationLimit& l);
bool Parse(const std::string& s, StationLimit& out);

// What the dialog needs from the module (all called on the main thread).
struct DialogHost {
    std::function<StationLimit(int64_t station)> get;
    std::function<void(int64_t station, const StationLimit&)> set;
    std::function<bool()> persistent;                     // limits go into the per-save sidecar
    std::function<float(Il2CppObject* ident)> baseValue;  // plort base price, 0 = unknown
};

class StationUi {
public:
    // Worker thread. false = the UI is unavailable (`why`); the price limit itself keeps working.
    bool Setup(sr2::core::ModuleContext& ctx, const char* module, DialogHost host, std::string& why);
    bool Ready() const { return ready_; }
    // Main thread, every frame (one pointer check while no drone menu was seen).
    void Tick(float unscaledDt);
    // Main thread: remove the action and the dialog (module disabled / faulted).
    void Shutdown();

    struct Impl;  // station_dialog.cpp (its detours reach it)

private:
    Impl* impl_ = nullptr;
    bool ready_ = false;
};

}  // namespace sr2::modules::drone_price_limit
