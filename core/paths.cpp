#include "paths.h"

#include <string>

namespace sr2::paths {

HMODULE SelfModule() {
    HMODULE h = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&SelfModule), &h);
    return h;
}

std::filesystem::path ModulePath(HMODULE m) {
    std::wstring buf(MAX_PATH, L'\0');
    for (;;) {
        DWORD n = GetModuleFileNameW(m, buf.data(), static_cast<DWORD>(buf.size()));
        if (n == 0) return {};
        if (n < buf.size()) {
            buf.resize(n);
            return buf;
        }
        buf.resize(buf.size() * 2);  // truncated: grow (long paths)
    }
}

std::filesystem::path SelfPath() { return ModulePath(SelfModule()); }
std::filesystem::path SelfDir() { return SelfPath().parent_path(); }
std::filesystem::path HostExePath() { return ModulePath(nullptr); }

}  // namespace sr2::paths
