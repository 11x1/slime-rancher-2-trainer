#pragma once
// sandbox: player extras on the Sandbox page's "Extras" card (see NOTES.md,
// "Player extras"): infinite health, teleport to the map marker, unlock the
// ranch expansions. SR2 1.3.0.
//
// Nothing here is required: a missing symbol only turns that tool off (with the
// reason in the card's tooltip and the log). The one hook (PlayerState.Damage,
// for infinite health) is created in Setup and switched on and off with the
// module like the achievement-pause hooks.

#include <string>

namespace sr2::core {
class ModuleContext;
}

namespace sr2::modules::sandbox {

// Worker thread, from Sandbox::Setup. Never fails the module.
void SetupPlayerExtras(sr2::core::ModuleContext& ctx);
// Module on / off (any thread): attaches the request handler, clears the session toggles.
void PlayerExtrasEnabled(bool on);
// Main thread, every frame while the module is live. `snapshot`: the page wants a fresh state now.
void PlayerExtrasTick(bool snapshot);
// Main thread: a HUD pop-up in the game's notification list (teleporter icon); only logged when the HUD can't.
void PlayerExtrasNotice(const std::string& text);

}  // namespace sr2::modules::sandbox
