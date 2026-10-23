#pragma once
// "[modded]" line on the title screen, shown whenever the mod is loaded (see modded_label.cpp).
// Not a module: no setting, no overlay tab, cannot be turned off. Started by the core worker
// (sr2::ui::StartModdedLabel in ui.h) before the modules are set up.
//
// It owns the one MinHook hook on MainMenuLandingRootUI.OnEnable (MinHook allows one hook per
// target), so other code that wants that event registers a listener here instead of hooking it.

#include "core/il2cpp_api.h"

namespace sr2::ui::modded_label {

// Called from the OnEnable post-hook on the main thread, after the game's OnEnable and after the
// label's own logic, OUTSIDE the label's DetourGuard: the listener wraps its logic in its own
// module's guard. `landing` is the MainMenuLandingRootUI (not yet class-checked).
using MenuEnableFn = void (*)(Il2CppObject* landing);

// Sets the (single) listener; nullptr clears it. Returns true when the OnEnable hook is installed,
// false when it is not (then the listener is never called). Any thread.
bool SetMenuEnableListener(MenuEnableFn fn);

}  // namespace sr2::ui::modded_label
