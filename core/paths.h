#pragma once

#include <windows.h>

#include <filesystem>

namespace sr2::paths {

// Module handle of this DLL (found from a code address, no refcount change).
HMODULE SelfModule();
// Full path of this DLL / its directory (the game folder once deployed).
std::filesystem::path SelfPath();
std::filesystem::path SelfDir();
// Full path of the host executable.
std::filesystem::path HostExePath();
// Full path of any loaded module.
std::filesystem::path ModulePath(HMODULE m);

}  // namespace sr2::paths
