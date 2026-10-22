#include "core.h"

#include <chrono>
#include <cwchar>
#include <exception>
#include <string>
#include <vector>

#include "../proxy/proxy.h"
#include "config.h"
#include "hook_manager.h"
#include "il2cpp_api.h"
#include "guard.h"
#include "log.h"
#include "main_thread.h"
#include "perf.h"
#include "module_manager.h"
#include "paths.h"
#include "resolver.h"
#include "settings_internal.h"
#include "unity.h"
#include "version_guard.h"

#ifndef SR2MODS_VERSION
#define SR2MODS_VERSION "0.0.0-dev"
#endif
#ifdef _DEBUG
#define SR2MODS_BUILD "Debug"
#else
#define SR2MODS_BUILD "Release"
#endif

// Provided by the UI owners (ui/options_tab.cpp, ui/overlay.cpp).
namespace sr2::ui {
bool StartOptionsTab();
bool StartOverlay();
}  // namespace sr2::ui

namespace sr2::core {
namespace {

using Clock = std::chrono::steady_clock;
constexpr wchar_t kGameExe[] = L"SlimeRancher2.exe";

// The proxy may also be loaded by other executables in the game folder
// (e.g. UnityCrashHandler64.exe). Only act inside the game, unless forced
// with SR2MODS_ANY_HOST=1 (for testing the proxy elsewhere).
bool IsGameHost() {
    std::wstring exe = paths::HostExePath().filename().wstring();
    if (_wcsicmp(exe.c_str(), kGameExe) == 0) return true;
    wchar_t buf[8] = {};
    return GetEnvironmentVariableW(L"SR2MODS_ANY_HOST", buf, 8) > 0 && buf[0] == L'1';
}

long long MsSince(Clock::time_point t0) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count();
}

HMODULE WaitForGameAssembly(const Config& cfg) {
    auto t0 = Clock::now();
    for (;;) {
        if (HMODULE h = GetModuleHandleW(L"GameAssembly.dll")) {
            SR2_INFO("GameAssembly.dll loaded at {} (waited {} ms)", static_cast<void*>(h), MsSince(t0));
            return h;
        }
        if (MsSince(t0) > cfg.startupTimeoutMs) return nullptr;
        Sleep(50);
    }
}

// NOTE: il2cpp_domain_get() is NOT a readiness signal - in this build it
// lazily callocs the domain if it does not exist yet (verified by
// disassembly), and calling it before il2cpp_init could even race the
// runtime. So we first poll il2cpp_get_corlib(), which is a plain read of
// il2cpp_defaults.corlib (set during il2cpp_init after the domain was
// created), and only then call il2cpp_domain_get() and wait until the
// assembly list contains Assembly-CSharp.
Il2CppDomain* WaitForIl2Cpp(const Config& cfg) {
    const il2cpp::Api& a = il2cpp::api();
    auto t0 = Clock::now();
    bool sawCorlib = false;
    for (;;) {
        if (a.il2cpp_get_corlib()) {
            if (!sawCorlib) {
                SR2_INFO("il2cpp corlib ready after {} ms", MsSince(t0));
                sawCorlib = true;
            }
            if (Il2CppDomain* d = a.il2cpp_domain_get()) {
                size_t n = 0;
                const Il2CppAssembly** as = a.il2cpp_domain_get_assemblies(d, &n);
                for (size_t i = 0; as && i < n; ++i) {
                    const Il2CppImage* img = as[i] ? a.il2cpp_assembly_get_image(as[i]) : nullptr;
                    const char* name = img ? a.il2cpp_image_get_name(img) : nullptr;
                    if (name && _stricmp(name, "Assembly-CSharp.dll") == 0) {
                        SR2_INFO("il2cpp domain {} ready: {} assemblies (waited {} ms)", static_cast<void*>(d), n,
                                 MsSince(t0));
                        return d;
                    }
                }
            }
        }
        if (MsSince(t0) > cfg.startupTimeoutMs) return nullptr;
        Sleep(100);
    }
}

void DumpImages() {
    const il2cpp::Api& a = il2cpp::api();
    auto& images = il2cpp::Resolver::Get().Images();
    size_t total = 0;
    SR2_INFO("---- {} loaded images ----", images.size());
    for (size_t i = 0; i < images.size(); ++i) {
        const auto& img = images[i];
        if (a.il2cpp_image_get_class_count) {
            size_t c = a.il2cpp_image_get_class_count(img.image);
            total += c;
            SR2_INFO("  [{:3}] {:<60} classes={}", i, img.name, c);
        } else {
            SR2_INFO("  [{:3}] {}", i, img.name);
        }
    }
    if (a.il2cpp_image_get_class_count)
        SR2_INFO("---- total classes: {} ----", total);
    else
        SR2_WARN("il2cpp_image_get_class_count not exported; class counts unavailable");
}

// Each UI entry point fails independently (false / exception / crash -> logged).
void StartUi(const char* what, bool (*start)()) {
    bool ok = false;
    std::string err;
    if (!Try([&] { ok = start(); }, &err))
        SR2_ERROR("ui: {} crashed during start: {}", what, err);
    else if (ok)
        SR2_INFO("ui: {} started", what);
    else
        SR2_WARN("ui: {} not started (returned false)", what);
}

void Run() {
    auto t0 = Clock::now();
    const auto dir = paths::SelfDir();

    Config cfg;
    std::string cfgErr;
    ConfigStatus cfgStatus = LoadConfig(dir / L"sr2mods.json", cfg, cfgErr);
    log::Init(dir / L"sr2mods.log", cfg.logLevel, cfg.debugConsole);

    SR2_INFO("sr2mods {} ({} build, {} {}) - native loader, no managed code", SR2MODS_VERSION, SR2MODS_BUILD, __DATE__,
             __TIME__);
    SR2_INFO("host: {} (pid {})", log::Narrow(paths::HostExePath().wstring()), GetCurrentProcessId());
    SR2_INFO("self: {}", log::Narrow(paths::SelfPath().wstring()));
    version_guard::LogSelfIdentity();  // PE timestamp + sha256 + PDB GUID/age -> match to dist\archive
    switch (cfgStatus) {
        case ConfigStatus::Loaded: SR2_INFO("config: sr2mods.json loaded"); break;
        case ConfigStatus::Missing: SR2_WARN("config: sr2mods.json {}", cfgErr); break;
        default: SR2_ERROR("config: sr2mods.json invalid ({}), using defaults", cfgErr); break;
    }
    // Runtime settings read saved values from the loaded document and write
    // changes back. If the file failed to parse, saving refuses to overwrite it.
    settings::Init(cfg.root, dir / L"sr2mods.json");

    if (proxy::EnsureLoaded())
        SR2_INFO("proxy: real version.dll = {} ({} stubs)", log::Narrow(proxy::RealDllPath()), proxy::StubCount());
    else
        SR2_ERROR("proxy: FAILED to load real version.dll from '{}' (error {}); version APIs will fail",
                  log::Narrow(proxy::RealDllPath()), proxy::LoadError());

    HMODULE ga = WaitForGameAssembly(cfg);
    if (!ga) {
        SR2_ERROR("GameAssembly.dll not loaded within {} ms - giving up (mods inactive)", cfg.startupTimeoutMs);
        return;
    }

    std::vector<std::string> missingReq, missingOpt;
    bool apiOk = il2cpp::Load(ga, missingReq, missingOpt);
    for (auto& n : missingOpt) SR2_WARN("optional export missing: {}", n);
    if (!apiOk) {
        for (auto& n : missingReq) SR2_ERROR("required export missing: {}", n);
        SR2_ERROR("IL2CPP API incomplete - mods inactive");
        return;
    }
    SR2_INFO("IL2CPP API resolved by name ({} optional missing)", missingOpt.size());

    // Hash while IL2CPP is still initialising (reads ~100 MB once).
    version_guard::CheckGameAssemblyHash(ga);

    Il2CppDomain* domain = WaitForIl2Cpp(cfg);
    if (!domain) {
        SR2_ERROR("IL2CPP not initialised within {} ms - giving up (mods inactive)", cfg.startupTimeoutMs);
        return;
    }
    if (cfg.settleDelayMs) Sleep(cfg.settleDelayMs);

    // Must precede every other IL2CPP call that touches managed state.
    Il2CppThread* thread = il2cpp::api().il2cpp_thread_attach(domain);
    if (!thread) {
        SR2_ERROR("il2cpp_thread_attach failed - mods inactive");
        return;
    }
    SR2_INFO("worker attached to IL2CPP domain (thread {})", static_cast<void*>(thread));

    il2cpp::Resolver::Get().Refresh();
    if (cfg.dumpAssemblies) DumpImages();

    RunVersionGuard(cfg);

    // Il2CppGCHandle width / round trip. Modules cache objects through GC
    // handles; if that is broken, no module may run.
    std::string gcReport;
    bool gcOk = unity::GCHandleSelfTest(&gcReport);
    if (gcOk) SR2_INFO("{}", gcReport);
    else SR2_ERROR("{} - all modules disabled", gcReport);

    // Timing of the mod's own per-frame work + frame hitches + GC ("perf_log", default on for now).
    perf::Init(cfg.root["perf_log"].asBool(true));

    if (HookManager::Get().Initialize()) {
        std::string err;
        if (!main_thread::Install(&err))
            SR2_ERROR("main thread dispatcher unavailable: {} (main_thread::Post / frame callbacks will never run)", err);

        auto& mm = ModuleManager::Get();
        RegisterBuiltinModules(mm);
        if (gcOk)
            mm.SetupAll(cfg);
        else
            mm.FailAll(cfg, "core GC handle self-test failed (see log)");
        version_guard::ScheduleMainThreadCheck(cfg);

        StartUi("options tab", &sr2::ui::StartOptionsTab);
        StartUi("overlay", &sr2::ui::StartOverlay);

        size_t hooks = HookManager::Get().List().size();
        SR2_INFO("startup complete in {} ms: {} module(s), {} hook(s)", MsSince(t0), mm.List().size(), hooks);
    } else {
        SR2_ERROR("hook manager unavailable - modules not loaded");
    }

    il2cpp::api().il2cpp_thread_detach(thread);
    SR2_DEBUG("worker detached and exiting");
}

void RunCatching() {
    try {
        Run();
    } catch (const std::exception& e) {
        SR2_ERROR("worker: unhandled exception: {}", e.what());
    } catch (...) {
        SR2_ERROR("worker: unhandled unknown exception");
    }
}

void LogSeh(DWORD code) { SR2_ERROR("worker: native exception 0x{:08X} - mods may be partially inactive", code); }

}  // namespace

DWORD WINAPI WorkerThreadProc(LPVOID /*param*/) {
    if (!IsGameHost()) return 0;  // pure proxy in any other process

    // Pin ourselves: hooks and this thread must never outlive the image.
    HMODULE pinned = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                       reinterpret_cast<LPCWSTR>(&WorkerThreadProc), &pinned);

    __try {
        RunCatching();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LogSeh(GetExceptionCode());
    }
    return 0;
}

}  // namespace sr2::core
