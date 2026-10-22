// sr2mods injection test - a minimal version.dll proxy.
//
// Purpose: confirm that Slime Rancher 2 loads a version.dll placed next to
// SlimeRancher2.exe, before installing the full mod. It does NOT touch IL2CPP,
// installs no hooks and never calls into GameAssembly.dll.
//
// DllMain only spawns a thread. The thread writes sr2mods_injecttest.log next
// to this DLL (falls back to %TEMP% if that folder is not writable), then polls
// for GameAssembly.dll / UnityPlayer.dll and logs when each appears.
//
// Only active in SlimeRancher2.exe; set SR2MODS_ANY_HOST=1 to test elsewhere.
// Export forwarding is the shared code in ../proxy (proxy.cpp + stubs + .def).

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <cwchar>

#include "proxy.h"

namespace {

constexpr wchar_t kLogName[] = L"sr2mods_injecttest.log";
constexpr wchar_t kGameExe[] = L"SlimeRancher2.exe";
constexpr DWORD kPollMs = 250;
constexpr ULONGLONG kPollTimeoutMs = 5ull * 60ull * 1000ull;

HANDLE g_log = INVALID_HANDLE_VALUE;

// ---------------------------------------------------------------- logging ---

void WriteRaw(const char* s, size_t n) {
    if (g_log == INVALID_HANDLE_VALUE || n == 0) return;
    DWORD written = 0;
    WriteFile(g_log, s, static_cast<DWORD>(n), &written, nullptr);
    FlushFileBuffers(g_log);  // survive a game crash right after this line
}

void Log(const char* fmt, ...) {
    char line[2048];
    SYSTEMTIME st;
    GetLocalTime(&st);
    int n = snprintf(line, sizeof(line), "%04u-%02u-%02u %02u:%02u:%02u.%03u  ", st.wYear, st.wMonth, st.wDay,
                     st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    if (n < 0) return;
    va_list ap;
    va_start(ap, fmt);
    int m = vsnprintf(line + n, sizeof(line) - static_cast<size_t>(n) - 3, fmt, ap);
    va_end(ap);
    if (m < 0) return;
    size_t len = static_cast<size_t>(n) + static_cast<size_t>(m);
    if (len > sizeof(line) - 3) len = sizeof(line) - 3;
    line[len++] = '\r';
    line[len++] = '\n';
    WriteRaw(line, len);
}

// Wide -> UTF-8 into a fixed buffer (truncates silently).
struct U8 {
    char buf[MAX_PATH * 4 + 64];
    explicit U8(const wchar_t* w) {
        buf[0] = '\0';
        if (w && !WideCharToMultiByte(CP_UTF8, 0, w, -1, buf, static_cast<int>(sizeof(buf)), nullptr, nullptr))
            buf[0] = '\0';
    }
    const char* c() const { return buf; }
};

// ---------------------------------------------------------------- helpers ---

const wchar_t* FileNamePart(const wchar_t* path) {
    const wchar_t* p = path;
    for (const wchar_t* q = path; *q; ++q)
        if (*q == L'\\' || *q == L'/') p = q + 1;
    return p;
}

bool AnyHostOverride() {
    wchar_t v[8] = {};
    DWORD n = GetEnvironmentVariableW(L"SR2MODS_ANY_HOST", v, 8);
    return n > 0 && n < 8 && wcscmp(v, L"1") == 0;
}

bool OpenLog(const wchar_t* dllPath, wchar_t* outPath, size_t outCap) {
    // 1) next to the DLL
    wcscpy_s(outPath, outCap, dllPath);
    wchar_t* name = const_cast<wchar_t*>(FileNamePart(outPath));
    *name = L'\0';
    wcscat_s(outPath, outCap, kLogName);
    g_log = CreateFileW(outPath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (g_log != INVALID_HANDLE_VALUE) return true;

    // 2) fallback: %TEMP%
    DWORD n = GetTempPathW(static_cast<DWORD>(outCap), outPath);
    if (n == 0 || n >= outCap) return false;
    wcscat_s(outPath, outCap, kLogName);
    g_log = CreateFileW(outPath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                        FILE_ATTRIBUTE_NORMAL, nullptr);
    return g_log != INVALID_HANDLE_VALUE;
}

// Self-test of the forwarding: call our OWN exports (resolved by name from this
// module, so they go through the asm stubs) and read the host's file version.
void LogHostFileVersion(HMODULE self, const wchar_t* exePath) {
    using SizeFn = DWORD(WINAPI*)(LPCWSTR, LPDWORD);
    using InfoFn = BOOL(WINAPI*)(LPCWSTR, DWORD, DWORD, LPVOID);
    using QueryFn = BOOL(WINAPI*)(LPCVOID, LPCWSTR, LPVOID*, PUINT);
    auto pSize = reinterpret_cast<SizeFn>(reinterpret_cast<void*>(GetProcAddress(self, "GetFileVersionInfoSizeW")));
    auto pInfo = reinterpret_cast<InfoFn>(reinterpret_cast<void*>(GetProcAddress(self, "GetFileVersionInfoW")));
    auto pQuery = reinterpret_cast<QueryFn>(reinterpret_cast<void*>(GetProcAddress(self, "VerQueryValueW")));
    if (!pSize || !pInfo || !pQuery) {
        Log("forwarding self-test: own exports not found (GetProcAddress on self failed)");
        return;
    }
    DWORD dummy = 0;
    DWORD size = pSize(exePath, &dummy);
    if (size == 0) {
        Log("forwarding self-test: GetFileVersionInfoSizeW(host) = 0, error %lu", GetLastError());
        return;
    }
    void* buf = HeapAlloc(GetProcessHeap(), 0, size);
    if (!buf) return;
    VS_FIXEDFILEINFO* ffi = nullptr;
    UINT ffiLen = 0;
    if (pInfo(exePath, 0, size, buf) && pQuery(buf, L"\\", reinterpret_cast<void**>(&ffi), &ffiLen) && ffi &&
        ffiLen >= sizeof(VS_FIXEDFILEINFO)) {
        Log("forwarding self-test: OK (GetFileVersionInfoSizeW=%lu) host file version %u.%u.%u.%u", size,
            HIWORD(ffi->dwFileVersionMS), LOWORD(ffi->dwFileVersionMS), HIWORD(ffi->dwFileVersionLS),
            LOWORD(ffi->dwFileVersionLS));
    } else {
        Log("forwarding self-test: GetFileVersionInfoW/VerQueryValueW failed, error %lu", GetLastError());
    }
    HeapFree(GetProcessHeap(), 0, buf);
}

// ----------------------------------------------------------------- thread ---

DWORD WINAPI TestThread(LPVOID param) {
    const ULONGLONG t0 = GetTickCount64();
    HMODULE self = static_cast<HMODULE>(param);

    // Pin ourselves for the lifetime of the thread (so a FreeLibrary by a test
    // host cannot unload code we are running). Released on exit below.
    HMODULE pinned = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(&TestThread), &pinned);
    auto finish = [pinned](DWORD code) -> DWORD {
        if (g_log != INVALID_HANDLE_VALUE) {
            CloseHandle(g_log);
            g_log = INVALID_HANDLE_VALUE;
        }
        if (pinned) FreeLibraryAndExitThread(pinned, code);
        return code;
    };

    wchar_t exePath[MAX_PATH * 2] = {};
    GetModuleFileNameW(nullptr, exePath, static_cast<DWORD>(sizeof(exePath) / sizeof(exePath[0])));
    const bool isGame = _wcsicmp(FileNamePart(exePath), kGameExe) == 0;
    const bool anyHost = AnyHostOverride();
    if (!isGame && !anyHost) return finish(0);  // silent in other hosts

    wchar_t dllPath[MAX_PATH * 2] = {};
    GetModuleFileNameW(self, dllPath, static_cast<DWORD>(sizeof(dllPath) / sizeof(dllPath[0])));

    wchar_t logPath[MAX_PATH * 2] = {};
    if (!OpenLog(dllPath, logPath, sizeof(logPath) / sizeof(logPath[0]))) return finish(1);

    const bool real = sr2::proxy::EnsureLoaded();
    HMODULE realMod = GetModuleHandleW(sr2::proxy::RealDllPath()[0] ? sr2::proxy::RealDllPath() : L"?");

    WriteRaw("\r\n", 2);
    Log("==== sr2mods injection test (build " __DATE__ " " __TIME__ ") ====");
    Log("host exe   : %s", U8(exePath).c());
    Log("host PID   : %lu", GetCurrentProcessId());
    Log("host match : %s", isGame ? "SlimeRancher2.exe" : "NO (SR2MODS_ANY_HOST=1 override)");
    Log("proxy dll  : %s (base %p)", U8(dllPath).c(), static_cast<void*>(self));
    Log("log file   : %s", U8(logPath).c());
    if (real) {
        Log("real dll   : %s (base %p) - loaded OK", U8(sr2::proxy::RealDllPath()).c(), static_cast<void*>(realMod));
    } else {
        Log("real dll   : %s - LOAD FAILED, error %lu", U8(sr2::proxy::RealDllPath()).c(), sr2::proxy::LoadError());
    }
    LogHostFileVersion(self, exePath);
    Log("waiting for GameAssembly.dll and UnityPlayer.dll (poll %lu ms, timeout %llu s)...", kPollMs,
        kPollTimeoutMs / 1000ull);

    struct Watch {
        const wchar_t* name;
        HMODULE base;
    } watch[] = {{L"UnityPlayer.dll", nullptr}, {L"GameAssembly.dll", nullptr}};

    unsigned found = 0;
    const unsigned total = static_cast<unsigned>(sizeof(watch) / sizeof(watch[0]));
    for (;;) {
        const ULONGLONG now = GetTickCount64() - t0;
        for (auto& w : watch) {
            if (w.base) continue;
            if (HMODULE h = GetModuleHandleW(w.name)) {
                w.base = h;
                ++found;
                wchar_t p[MAX_PATH * 2] = {};
                GetModuleFileNameW(h, p, static_cast<DWORD>(sizeof(p) / sizeof(p[0])));
                Log("%-16s loaded at +%llu ms, base %p, %s", U8(w.name).c(), now, static_cast<void*>(h), U8(p).c());
            }
        }
        if (found == total) break;
        if (now >= kPollTimeoutMs) break;
        Sleep(kPollMs);
    }

    if (found == total) {
        Log("INJECTION OK");
    } else {
        for (auto& w : watch)
            if (!w.base) Log("%-16s NOT seen within %llu s", U8(w.name).c(), kPollTimeoutMs / 1000ull);
        Log("INJECTION OK (proxy loaded and forwarding), but not all Unity modules were seen");
    }
    return finish(0);
}

}  // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID /*reserved*/) {
    if (reason == DLL_PROCESS_ATTACH) {
        HANDLE t = CreateThread(nullptr, 0, &TestThread, module, 0, nullptr);
        if (t) CloseHandle(t);
    }
    return TRUE;
}
