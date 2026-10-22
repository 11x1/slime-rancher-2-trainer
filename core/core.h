#pragma once

#include <windows.h>

namespace sr2::core {

// Worker thread started from DllMain. Waits for IL2CPP, attaches, runs the
// milestone diagnostics and sets up all modules. `param` = our HMODULE.
DWORD WINAPI WorkerThreadProc(LPVOID param);

}  // namespace sr2::core
