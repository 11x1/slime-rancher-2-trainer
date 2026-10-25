#pragma once
// sandbox: noclip (see NOTES.md, "Noclip"). SR2 1.3.0.
//
// Fly through walls while Sandbox is on: the movement keys (the game's own
// movement input) move you along your view, two bindable keys straight up and
// down, at a speed set on the Sandbox page. Toggled only by its bindable key
// (unbound by default; the page has no switch). Collision and gravity come back
// when it is switched off, when Sandbox goes off, when the player object goes
// away, and on a scene change.
//
// The player's KinematicCharacterMotor keeps running (mouse look, abilities,
// interpolation) with its collision and ground solving switched off and the
// controller's gravity bypassed, and the noclip moves it once per frame with
// KinematicCharacterMotor.SetPosition. hl2_movement steps aside while it is on
// (modules/common/noclip_state.h). Two hooks (DeathHandler.Kill,
// KillOnTrigger.OnTriggerEnter) keep kill volumes such as the slime sea from
// knocking the noclipping player out.

#include <string>

namespace sr2::core {
class ModuleContext;
}

namespace sr2::modules::sandbox {

// Worker thread, from Sandbox::Setup: registers the settings, resolves the game symbols. Never fails the module.
void SetupNoclip(sr2::core::ModuleContext& ctx);
// Main thread, every frame (also with Sandbox off or faulted: then it only restores). `live`: Sandbox on and not faulted.
void NoclipTick(bool live);
// Main thread: for the page's status line. On, and why it can't be switched on ("" = it can).
bool NoclipOn();
std::string NoclipWhy(bool live);

}  // namespace sr2::modules::sandbox
