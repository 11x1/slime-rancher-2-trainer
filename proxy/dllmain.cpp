// Entry point of the proxy DLL. Kept deliberately minimal: no LoadLibrary, no
// IL2CPP, no file I/O under the loader lock - only spawn the worker thread.
// (DisableThreadLibraryCalls is intentionally NOT used: the static CRT wants
// thread attach/detach notifications.)

#include <windows.h>

#include "../core/core.h"

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID /*reserved*/) {
    if (reason == DLL_PROCESS_ATTACH) {
        HANDLE t = CreateThread(nullptr, 0, &sr2::core::WorkerThreadProc, module, 0, nullptr);
        if (t) CloseHandle(t);
    }
    return TRUE;
}
