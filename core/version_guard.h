#pragma once

#include <windows.h>

#include <string>

#include "config.h"

namespace sr2::core {

// Startup part of the version guard (worker thread, attached): resolves the
// UnityEngine.Application getters by name and reads Application.unityVersion
// (thread-safe icall). Application.version is read on the main thread via
// version_guard::ScheduleMainThreadCheck. Never blocks loading; mismatches are
// warnings only (log + Mismatch()/Status() for the UI).
void RunVersionGuard(const Config& cfg);

namespace version_guard {

inline constexpr const char* kKnownGameAssemblySha256 =
    "786006ede0d198f6e15a2c01f90ef9108afe694745fa0ad894e1c3ed342385b7";

// True if the build is not the known one. The GameAssembly.dll SHA-256 is
// authoritative: if it matches, this is always false. If it differs, true.
// Only if the hash is unavailable are the version tokens compared
// (thread-safe; false until something is known).
bool Mismatch();
// One-line human readable status for the UI, e.g.
// "Game 1.3.0 / Unity 6000.0.73f1 - GameAssembly.dll matches known build".
std::string Status();

// ---- additions ----
std::string GameVersion();          // leading token of Application.version, e.g. "1.3.0" ("" until read on the main thread)
std::string GameBuildStamp();       // build stamp of Application.version, e.g. "20260704030000-113492" ("" if none)
std::string UnityVersion();         // leading token of Application.unityVersion ("" until read)
std::string GameAssemblySha256();   // lower-case hex ("" if hashing failed)
// Posts the Application getters to the main thread (they run at the first
// dispatcher tick). Idempotent. Needs RunVersionGuard() first.
void ScheduleMainThreadCheck(const Config& cfg);
// SHA-256 of the loaded GameAssembly.dll vs. kKnownGameAssemblySha256 (worker thread).
void CheckGameAssemblyHash(HMODULE gameAssembly);
// Logs our own DLL's PE timestamp, short SHA-256 and PDB GUID/age so a log
// can be matched to dist\archive\<stamp>-<sha8>\version.pdb.
void LogSelfIdentity();
// SHA-256 (lower-case hex) of a file via BCrypt.
bool Sha256OfFile(const std::wstring& path, std::string& hex, std::string& err);

}  // namespace version_guard
}  // namespace sr2::core
