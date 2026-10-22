#include "proxy.h"

#include <cwchar>

namespace {

constexpr const char* kExportNames[] = {
#define SR2_PROXY_EXPORT(index, name) name,
#include "proxy_exports.inc"
#undef SR2_PROXY_EXPORT
};
constexpr unsigned kExportCount = static_cast<unsigned>(sizeof(kExportNames) / sizeof(kExportNames[0]));

INIT_ONCE g_once = INIT_ONCE_STATIC_INIT;
HMODULE g_real = nullptr;
DWORD g_loadError = 0;
wchar_t g_realPath[MAX_PATH + 32] = {};
volatile LONG g_missing = 0;

// Fallback for an export the real DLL does not provide (should never happen):
// every proxied API returns BOOL/DWORD where 0 means failure.
extern "C" unsigned long long sr2_proxy_missing() {
    SetLastError(ERROR_PROC_NOT_FOUND);
    return 0;
}

BOOL CALLBACK LoadRealOnce(PINIT_ONCE, PVOID, PVOID*) {
    UINT n = GetSystemDirectoryW(g_realPath, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        g_loadError = n == 0 ? GetLastError() : ERROR_BUFFER_OVERFLOW;
        g_realPath[0] = L'\0';
        return TRUE;
    }
    wcscat_s(g_realPath, L"\\version.dll");

    // Full path only - never the bare name (that would find ourselves).
    HMODULE h = LoadLibraryExW(g_realPath, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!h) {
        g_loadError = GetLastError();
        return TRUE;
    }
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&LoadRealOnce), &self);
    if (h == self) {  // paranoia: loader handed us back our own module
        FreeLibrary(h);
        g_loadError = ERROR_INVALID_HANDLE;
        return TRUE;
    }
    g_real = h;
    return TRUE;
}

}  // namespace

extern "C" {
// Referenced from version_stubs.asm.
void* sr2_proxy_table[kExportCount] = {};

void* sr2_proxy_resolve(unsigned index) {
    if (index >= kExportCount) return reinterpret_cast<void*>(&sr2_proxy_missing);
    if (void* p = sr2_proxy_table[index]) return p;
    sr2::proxy::EnsureLoaded();
    void* p = g_real ? reinterpret_cast<void*>(GetProcAddress(g_real, kExportNames[index])) : nullptr;
    if (!p) {
        InterlockedIncrement(&g_missing);
        return reinterpret_cast<void*>(&sr2_proxy_missing);
    }
    // Aligned pointer store is atomic on x64; racing resolvers write the same value.
    InterlockedExchangePointer(&sr2_proxy_table[index], p);
    return p;
}
}

namespace sr2::proxy {

bool EnsureLoaded() {
    InitOnceExecuteOnce(&g_once, &LoadRealOnce, nullptr, nullptr);
    return g_real != nullptr;
}

const wchar_t* RealDllPath() { return g_realPath; }
DWORD LoadError() { return g_loadError; }
unsigned StubCount() { return kExportCount; }
unsigned MissingCount() { return static_cast<unsigned>(g_missing); }

}  // namespace sr2::proxy
