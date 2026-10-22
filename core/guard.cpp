#include "guard.h"

#include <windows.h>

#include <exception>
#include <filesystem>
#include <format>

#include "log.h"

namespace sr2::core {
namespace {

enum class CppResult { Ok, StdException, Unknown };

// Inner layer: C++ exceptions. The message is copied into a fixed buffer so
// that no object with a destructor lives in the SEH frame below.
CppResult CallCpp(void (*thunk)(void*), void* ctx, char* msg, size_t msgSize) {
    try {
        thunk(ctx);
        return CppResult::Ok;
    } catch (const std::exception& e) {
        strncpy_s(msg, msgSize, e.what(), _TRUNCATE);
        return CppResult::StdException;
    } catch (...) {
        return CppResult::Unknown;
    }
}

// Captures code + address of an SEH exception. C++ exceptions (0xE06D7363)
// are normally handled by CallCpp before reaching this filter.
int Filter(EXCEPTION_POINTERS* ep, DWORD* code, void** addr) {
    *code = ep->ExceptionRecord->ExceptionCode;
    *addr = ep->ExceptionRecord->ExceptionAddress;
    return EXCEPTION_EXECUTE_HANDLER;
}

// Outer layer: SEH. Must not contain objects that need unwinding.
CppResult CallSeh(void (*thunk)(void*), void* ctx, char* msg, size_t msgSize, DWORD* code, void** addr) {
    __try {
        return CallCpp(thunk, ctx, msg, msgSize);
    } __except (Filter(GetExceptionInformation(), code, addr)) {
        return CppResult::Unknown;
    }
}

}  // namespace

std::string DescribeAddress(const void* p) {
    HMODULE m = nullptr;
    if (p && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                static_cast<LPCWSTR>(p), &m) &&
        m) {
        wchar_t path[MAX_PATH] = {};
        GetModuleFileNameW(m, path, MAX_PATH);
        return std::format("{}+0x{:X}", log::Narrow(std::filesystem::path(path).filename().wstring()),
                           reinterpret_cast<uintptr_t>(p) - reinterpret_cast<uintptr_t>(m));
    }
    return std::format("0x{:X}", reinterpret_cast<uintptr_t>(p));
}

bool GuardedCall(void (*thunk)(void*), void* ctx, std::string* err) { return GuardedCallEx(thunk, ctx, err, nullptr); }

bool GuardedCallEx(void (*thunk)(void*), void* ctx, std::string* err, FaultInfo* fault) {
    char msg[512] = {};
    DWORD code = 0;
    void* addr = nullptr;
    CppResult r = CallSeh(thunk, ctx, msg, sizeof(msg), &code, &addr);
    if (fault) {
        fault->code = code;
        fault->address = addr;
    }
    if (r == CppResult::Ok) return true;
    if (err) {
        if (code)
            *err = std::format("native exception 0x{:08X} at {}", code, DescribeAddress(addr));
        else if (r == CppResult::StdException)
            *err = std::format("exception: {}", msg);
        else
            *err = "unknown C++ / managed exception";
    }
    return false;
}

}  // namespace sr2::core
