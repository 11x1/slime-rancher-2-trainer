#include "version_guard.h"

#include <windows.h>
#include <bcrypt.h>

#include <atomic>
#include <format>
#include <mutex>
#include <string_view>
#include <vector>

#include "il2cpp_api.h"
#include "log.h"
#include "main_thread.h"
#include "paths.h"
#include "resolver.h"

namespace sr2::core {
namespace {

using il2cpp::Resolver;

struct State {
    std::mutex mutex;
    bool hashDone = false;
    std::string hash;  // lower-case hex, "" on failure
    std::string hashError;
    std::string gameVersion, unityVersion;  // leading version tokens ("1.3.0", "6000.0.73f1")
    std::string gameBuildStamp;             // "20260704030000-113492" from "1.3.0 [20260704030000-113492]"
    std::string expectedGame = "1.3.0", expectedUnity = "6000.0.73f1";
};

State& S() {
    static State s;
    return s;
}

const MethodInfo* g_getVersion = nullptr;
const MethodInfo* g_getUnityVersion = nullptr;
std::atomic<bool> g_scheduled{false};

const MethodInfo* TestResolve(Il2CppClass* app, const char* method) {
    std::string err;
    Resolver& r = Resolver::Get();
    const MethodInfo* m = r.Method(app, method, 0, &err);
    if (!m) {
        SR2_ERROR("test resolve FAILED: UnityEngine.Application::{} - {}", method, err);
        return nullptr;
    }
    void* code = r.MethodPointer(m, &err);
    if (code)
        SR2_INFO("test resolve OK: UnityEngine.Application::{} MethodInfo={} code=GameAssembly+0x{:X}", method,
                 static_cast<const void*>(m), Resolver::Rva(code));
    else
        SR2_WARN("test resolve: UnityEngine.Application::{} MethodInfo={} but no code pointer: {}", method,
                 static_cast<const void*>(m), err);
    return m;
}

bool InvokeString(const MethodInfo* m, std::string& out, std::string& err) {
    Resolver& r = Resolver::Get();
    Il2CppObject* o = r.Invoke(m, nullptr, nullptr, &err);
    if (!o) {
        if (err.empty()) err = "returned null";
        return false;
    }
    out = r.ToUtf8(reinterpret_cast<Il2CppString*>(o));  // System.String is returned unboxed
    return true;
}

bool Sha256File(const std::wstring& path, std::string& hex, std::string& err) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        err = std::format("cannot open (error {})", GetLastError());
        return false;
    }
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE h = nullptr;
    bool ok = false;
    NTSTATUS st = BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
    if (BCRYPT_SUCCESS(st)) st = BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0);
    if (BCRYPT_SUCCESS(st)) {
        std::vector<unsigned char> buf(1 << 20);
        ok = true;
        for (;;) {
            DWORD got = 0;
            if (!ReadFile(f, buf.data(), static_cast<DWORD>(buf.size()), &got, nullptr)) {
                err = std::format("read failed (error {})", GetLastError());
                ok = false;
                break;
            }
            if (got == 0) break;
            st = BCryptHashData(h, buf.data(), got, 0);
            if (!BCRYPT_SUCCESS(st)) {
                ok = false;
                break;
            }
        }
        unsigned char digest[32] = {};
        if (ok && BCRYPT_SUCCESS(st = BCryptFinishHash(h, digest, sizeof(digest), 0))) {
            hex.clear();
            for (unsigned char b : digest) hex += std::format("{:02x}", b);
        } else {
            ok = false;
        }
    }
    if (!ok && err.empty()) err = std::format("BCrypt error 0x{:08X}", static_cast<unsigned long>(st));
    if (h) BCryptDestroyHash(h);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    CloseHandle(f);
    return ok;
}

bool HashMatchesLocked(const State& s) {
    return s.hashDone && !s.hash.empty() && s.hash == version_guard::kKnownGameAssemblySha256;
}

std::string Trim(std::string_view v) {
    size_t b = v.find_first_not_of(" \t\r\n");
    if (b == std::string_view::npos) return {};
    size_t e = v.find_last_not_of(" \t\r\n");
    return std::string(v.substr(b, e - b + 1));
}

// Leading version token: everything up to the first space or '['.
std::string VersionToken(std::string_view v) {
    std::string t = Trim(v);
    size_t cut = t.find_first_of(" [");
    return cut == std::string::npos ? t : Trim(std::string_view(t).substr(0, cut));
}

// Build stamp: the text inside the first [...] (or after the first space), "" if none.
std::string BuildStamp(std::string_view v) {
    std::string t = Trim(v);
    size_t open = t.find('[');
    if (open != std::string::npos) {
        size_t close = t.find(']', open + 1);
        return Trim(std::string_view(t).substr(open + 1, close == std::string::npos ? std::string::npos
                                                                                      : close - open - 1));
    }
    size_t sp = t.find(' ');
    return sp == std::string::npos ? std::string() : Trim(std::string_view(t).substr(sp + 1));
}

// hashOk: the GameAssembly.dll hash matched the known build, which is
// authoritative - a differing version string is then only informational.
void LogCompare(const char* what, const std::string& value, const std::string& expected, bool hashOk = false) {
    bool match = expected.empty() || value == expected;
    if (match)
        SR2_INFO("{}: {} (expected {})", what, value, expected);
    else if (hashOk)
        SR2_INFO("{}: {} (expected {}) - differs, but GameAssembly.dll matches the known build; ignored", what, value,
                 expected);
    else
        SR2_WARN("{}: {} (expected {})  <-- MISMATCH, modules may fail to resolve", what, value, expected);
}

}  // namespace

namespace version_guard {

bool Sha256OfFile(const std::wstring& path, std::string& hex, std::string& err) { return Sha256File(path, hex, err); }

void LogSelfIdentity() {
    HMODULE self = paths::SelfModule();
    if (!self) return;
    auto base = reinterpret_cast<const unsigned char*>(self);
    auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    DWORD stamp = nt->FileHeader.TimeDateStamp;

    // CodeView (RSDS) record -> PDB GUID + age, as symbol servers / build.ps1 print it.
    std::string pdb = "no CodeView record";
    const IMAGE_DATA_DIRECTORY& dd = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG];
    if (dd.VirtualAddress && dd.Size) {
        auto dbg = reinterpret_cast<const IMAGE_DEBUG_DIRECTORY*>(base + dd.VirtualAddress);
        for (size_t i = 0; i < dd.Size / sizeof(IMAGE_DEBUG_DIRECTORY); ++i) {
            if (dbg[i].Type != IMAGE_DEBUG_TYPE_CODEVIEW || !dbg[i].AddressOfRawData) continue;
            struct Rsds {
                DWORD sig;
                GUID guid;
                DWORD age;
                char path[1];
            };
            auto r = reinterpret_cast<const Rsds*>(base + dbg[i].AddressOfRawData);
            if (r->sig != 0x53445352) continue;  // 'RSDS'
            const GUID& g = r->guid;
            pdb = std::format("{:08X}-{:04X}-{:04X}-{:02X}{:02X}-{:02X}{:02X}{:02X}{:02X}{:02X}{:02X} age {} ({})",
                              g.Data1, g.Data2, g.Data3, g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3], g.Data4[4],
                              g.Data4[5], g.Data4[6], g.Data4[7], r->age, r->path);
            break;
        }
    }
    std::string hex, err;
    bool ok = Sha256File(paths::SelfPath().wstring(), hex, err);
    SR2_INFO("self: PE timestamp 0x{:08X}, sha256 {}, pdb {}", stamp, ok ? hex.substr(0, 16) : "? (" + err + ")",
             pdb);
}

void CheckGameAssemblyHash(HMODULE gameAssembly) {
    std::wstring path = paths::ModulePath(gameAssembly).wstring();
    std::string hex, err;
    DWORD t0 = GetTickCount();
    bool ok = Sha256File(path, hex, err);
    {
        std::lock_guard lock(S().mutex);
        S().hashDone = true;
        S().hash = ok ? hex : std::string();
        S().hashError = err;
    }
    if (!ok) {
        SR2_WARN("version guard: cannot hash GameAssembly.dll: {}", err);
        return;
    }
    bool match = hex == kKnownGameAssemblySha256;
    log::Logf(match ? log::Level::Info : log::Level::Warn, "version guard: GameAssembly.dll SHA-256 {} ({} ms) - {}",
              hex, GetTickCount() - t0,
              match ? "matches known 1.3.0 build"
                    : "UNKNOWN BUILD (not 1.3.0 as dumped); name-based resolves may still work, check module status");
}

bool Mismatch() {
    State& s = S();
    std::lock_guard lock(s.mutex);
    // The GameAssembly.dll hash is authoritative: a known build never mismatches.
    if (HashMatchesLocked(s)) return false;
    if (s.hashDone && !s.hash.empty()) return true;  // hashed, unknown build
    // Hash unavailable: fall back to the version tokens.
    if (!s.gameVersion.empty() && !s.expectedGame.empty() && s.gameVersion != s.expectedGame) return true;
    if (!s.unityVersion.empty() && !s.expectedUnity.empty() && s.unityVersion != s.expectedUnity) return true;
    return false;
}

std::string Status() {
    State& s = S();
    std::lock_guard lock(s.mutex);
    std::string out = std::format("Game {} / Unity {}", s.gameVersion.empty() ? "?" : s.gameVersion,
                                  s.unityVersion.empty() ? "?" : s.unityVersion);
    if (!s.gameBuildStamp.empty()) out += std::format(" (build {})", s.gameBuildStamp);
    if (HashMatchesLocked(s)) return out + " - GameAssembly.dll matches known build";
    if (!s.hashDone)
        out += " - GameAssembly.dll not checked yet";
    else if (s.hash.empty())
        out += " - GameAssembly.dll hash unavailable (" + s.hashError + ")";
    else
        out += " - UNKNOWN GameAssembly.dll build (expected " + s.expectedGame + ")";
    if (!s.gameVersion.empty() && !s.expectedGame.empty() && s.gameVersion != s.expectedGame)
        out += std::format(" - game version mismatch (expected {})", s.expectedGame);
    if (!s.unityVersion.empty() && !s.expectedUnity.empty() && s.unityVersion != s.expectedUnity)
        out += std::format(" - Unity version mismatch (expected {})", s.expectedUnity);
    return out;
}

std::string GameVersion() {
    std::lock_guard lock(S().mutex);
    return S().gameVersion;
}

std::string GameBuildStamp() {
    std::lock_guard lock(S().mutex);
    return S().gameBuildStamp;
}

std::string UnityVersion() {
    std::lock_guard lock(S().mutex);
    return S().unityVersion;
}

std::string GameAssemblySha256() {
    std::lock_guard lock(S().mutex);
    return S().hash;
}

void ScheduleMainThreadCheck(const Config& cfg) {
    if (!cfg.versionGuardInvoke) return;
    if (g_scheduled.exchange(true)) return;
    if (!g_getVersion && !g_getUnityVersion) return;
    // Application.version is not thread-safe -> read it on the Unity main thread.
    main_thread::Post([] {
        std::string v, err;
        if (g_getVersion) {
            if (InvokeString(g_getVersion, v, err)) {
                // SR2 returns "<version> [<build stamp>]", e.g. "1.3.0 [20260704030000-113492]".
                std::string token = VersionToken(v), stamp = BuildStamp(v);
                std::string expected;
                bool hashOk = false;
                {
                    std::lock_guard lock(S().mutex);
                    S().gameVersion = token;
                    S().gameBuildStamp = stamp;
                    expected = S().expectedGame;
                    hashOk = HashMatchesLocked(S());
                }
                SR2_INFO("Game version: Application.version = \"{}\" -> version \"{}\", build stamp \"{}\"", v, token,
                         stamp);
                LogCompare("Game version (main thread)", token, expected, hashOk);
            } else {
                SR2_WARN("Game version: invoke on main thread failed: {}", err);
            }
        }
        if (g_getUnityVersion) {
            err.clear();
            if (InvokeString(g_getUnityVersion, v, err)) {
                std::lock_guard lock(S().mutex);
                S().unityVersion = VersionToken(v);
            }
        }
        SR2_INFO("version guard: {}{}", Status(), Mismatch() ? "  <-- MISMATCH" : "");
    });
}

}  // namespace version_guard

void RunVersionGuard(const Config& cfg) {
    {
        std::lock_guard lock(S().mutex);
        S().expectedGame = cfg.expectedGameVersion;
        S().expectedUnity = cfg.expectedUnityVersion;
    }
    std::string err;
    Il2CppClass* app = Resolver::Get().Class("UnityEngine.CoreModule", "UnityEngine", "Application", &err);
    if (!app) {
        SR2_WARN("version guard: cannot resolve UnityEngine.Application: {}", err);
        return;
    }
    g_getVersion = TestResolve(app, "get_version");
    g_getUnityVersion = TestResolve(app, "get_unityVersion");

    if (!cfg.versionGuardInvoke) {
        SR2_INFO("version guard: invocation disabled in config (version_guard_invoke=false)");
        return;
    }
    // get_unityVersion is bound with IsThreadSafe=true -> safe from this worker
    // thread. get_version is read later on the main thread (ScheduleMainThreadCheck).
    std::string value;
    if (g_getUnityVersion) {
        if (InvokeString(g_getUnityVersion, value, err)) {
            bool hashOk = false;
            value = VersionToken(value);
            {
                std::lock_guard lock(S().mutex);
                S().unityVersion = value;
                hashOk = HashMatchesLocked(S());
            }
            LogCompare("Unity version", value, cfg.expectedUnityVersion, hashOk);
        } else {
            SR2_WARN("Unity version: invoke failed: {}", err);
        }
    }
}

}  // namespace sr2::core
