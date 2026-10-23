#pragma once
// Build stamp of this DLL, written by cmake/build_info.cmake on every build.

namespace sr2::core::build_info {

const char* Time();       // "dd/MM HH:mm", local time of the build machine
const char* TimeIso();    // "yyyy-MM-ddTHH:mm"
const char* Commit();     // short git commit ("abc1234", "-dirty" suffix), "" without git

}  // namespace sr2::core::build_info
