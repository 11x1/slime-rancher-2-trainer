// The only file that includes the generated stamp, so a new stamp recompiles just this.
#include "build_info.h"

#include "sr2mods_build_info.h"  // in build/<Config>/generated

namespace sr2::core::build_info {

const char* Time() { return SR2MODS_BUILD_TIME_SHORT; }
const char* TimeIso() { return SR2MODS_BUILD_TIME; }
const char* Commit() { return SR2MODS_BUILD_COMMIT; }

}  // namespace sr2::core::build_info
