#pragma once
// Whether the Sandbox noclip (modules/sandbox/noclip.cpp) is moving the player
// right now, for modules that drive the player's movement themselves.
// hl2_movement checks it in its KFCCharacterController.UpdateVelocity detour and
// lets the vanilla tick run untouched while it is set (no Source movement, no
// fall-through recovery, which would teleport a noclipping player back up).
//
// Set and cleared by the noclip on the Unity main thread (on and off only by its
// key, or off when Sandbox goes off / the scene changes); read from any thread.
// The noclip's own kill-volume hooks use its internal state, not this flag.

#include <atomic>

namespace sr2::modules::common {

inline std::atomic<bool>& NoclipFlag() {
    static std::atomic<bool> flag{false};
    return flag;
}

inline bool NoclipActive() { return NoclipFlag().load(std::memory_order_relaxed); }

}  // namespace sr2::modules::common
