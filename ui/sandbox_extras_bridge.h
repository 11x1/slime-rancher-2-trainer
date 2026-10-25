#pragma once
// Data exchange between the sandbox module's player extras
// (modules/sandbox/player_extras.cpp) and the "Extras" card on the Sandbox page
// (menu.cpp). Kept apart from sandbox_bridge.h so the extras don't touch the
// main snapshot.
//
// Threading is the same as sandbox_bridge.h: the module publishes a State on
// the Unity main thread (with the main snapshot, ~4 Hz while the page is
// open); the render thread only reads it. Requests are posted to the main
// thread and handled there. Results go to the shared status line
// (sandbox::SetStatus).

#include <memory>
#include <string>

namespace sr2::ui::sandbox::extras {

struct State {
    unsigned long long tick = 0;    // GetTickCount64 when it was taken
    bool infiniteHealth = false;    // toggle, kept per save (modules/sandbox/cheated_save.h, SaveToggle)
    bool healthHook = false;        // PlayerState.Damage is blocked too (else only the top-up)
    bool canTeleport = false;       // a map marker is set in this zone and the teleport path is available
    std::string teleportWhy;        // !canTeleport: why (tooltip)
    int lockedExpansions = -1;      // locked ranch expansions found (-1: not known / not on the ranch)
    int totalExpansions = 0;        // ranch expansions found (locked or not)
    std::string expansionWhy;       // lockedExpansions < 1: why the button is disabled
    bool noclip = false;            // noclip is on (modules/sandbox/noclip.cpp)
    std::string noclipWhy;          // "" = noclip can be switched on (its key); else why not
};

enum class Op {
    InfiniteHealth,    // value = 0 / 1
    TeleportToMarker,  // the player's map marker (navigation waypoint)
    UnlockExpansions,  // every locked ranch expansion, as a purchase would
};

// ---- module side (Unity main thread) ----
void Publish(State s);
void SetHandler(void (*fn)(Op, long long));  // nullptr detaches (module off). Any thread.

// ---- render thread ----
std::shared_ptr<const State> Get();  // nullptr until the first publish
bool Request(Op op, long long value = 0);

}  // namespace sr2::ui::sandbox::extras
