#pragma once
// debug_tools: "Highlight what the mod touches" (round 14: "maybe add a debug menu to highlight what you want me
// to check, which entities/buildables/map parts"). Draws the boxes other modules publish to
// modules/common/debug_marks.h (slime_dj: pulsing lamps, lamps that do not pulse, glowing ice crystals,
// playing / silent speakers with the lamp range) as wireframes with a label per box, through ui/hud.h like the
// entity hitboxes. Main thread, every frame while on; no game calls of its own besides the camera matrices
// (hitboxes::CameraFrame). Hidden while the settings window is open (publishers keep running).

namespace sr2::modules::debug_tools::highlight {

// Main thread, every frame. `want` = option on and module on.
void Tick(bool want);

}  // namespace sr2::modules::debug_tools::highlight
