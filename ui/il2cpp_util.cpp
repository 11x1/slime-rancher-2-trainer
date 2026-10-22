#include "il2cpp_util.h"

#include <windows.h>

#include <atomic>
#include <format>
#include <vector>

#include "core/log.h"
#include "core/resolver.h"
#include "core/text_override.h"

namespace sr2::ui::il {
namespace {

il2cpp::Resolver& R() { return il2cpp::Resolver::Get(); }

}  // namespace

void* Export(const char* name) {
    HMODULE m = il2cpp::api().module;
    if (!m) m = GetModuleHandleW(L"GameAssembly.dll");
    return m ? reinterpret_cast<void*>(GetProcAddress(m, name)) : nullptr;
}

Il2CppClass* Class(std::string_view image, std::string_view ns, std::string_view name, std::string* err) {
    return R().Class(image, ns, name, err);
}

const MethodInfo* Method(Il2CppClass* k, std::string_view name, int argc, std::string* err) {
    return k ? R().Method(k, name, argc, err) : nullptr;
}

const MethodInfo* MethodInHierarchy(Il2CppClass* k, std::string_view name, int argc, std::string* err) {
    auto parentOf = il2cpp::api().il2cpp_class_get_parent;
    std::string first;
    for (Il2CppClass* c = k; c; c = parentOf ? parentOf(c) : nullptr) {
        std::string e;
        if (const MethodInfo* m = R().Method(c, name, argc, &e)) return m;
        if (first.empty()) first = e;
        if (!parentOf) break;
    }
    if (err) *err = first.empty() ? std::string("method not found: ") + std::string(name) : first;
    return nullptr;
}

bool FieldOffset(Il2CppClass* k, std::string_view name, size_t& out, std::string* err) {
    if (!k) return false;
    auto r = R().FieldOffset(k, name, err);
    if (!r) return false;
    out = *r;
    return true;
}

bool FieldOffsetInHierarchy(Il2CppClass* k, std::string_view name, size_t& out, std::string* err) {
    auto parentOf = il2cpp::api().il2cpp_class_get_parent;
    std::string first;
    for (Il2CppClass* c = k; c; c = parentOf ? parentOf(c) : nullptr) {
        std::string e;
        if (auto r = R().FieldOffset(c, name, &e)) {
            out = *r;
            return true;
        }
        if (first.empty()) first = e;
        if (!parentOf) break;
    }
    if (err) *err = first.empty() ? std::string("field not found: ") + std::string(name) : first;
    return false;
}

void* MethodPointer(const MethodInfo* m, std::string* err) { return m ? R().MethodPointer(m, err) : nullptr; }

namespace {

// No C++ objects with destructors in these frames (required for __try).
Il2CppObject* InvokeSeh(const MethodInfo* m, void* obj, void** params, Il2CppException** exc, DWORD* code) {
    __try {
        return il2cpp::api().il2cpp_runtime_invoke(m, obj, params, exc);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *code = GetExceptionCode();
        return nullptr;
    }
}

const char* SafeClassName(Il2CppObject* o, const char** ns) {
    const auto& a = il2cpp::api();
    *ns = nullptr;
    __try {
        if (!o || !a.il2cpp_object_get_class || !a.il2cpp_class_get_name) return nullptr;
        Il2CppClass* k = a.il2cpp_object_get_class(o);
        if (!k) return nullptr;
        if (a.il2cpp_class_get_namespace) *ns = a.il2cpp_class_get_namespace(k);
        return a.il2cpp_class_get_name(k);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

Il2CppString* SafeReadString(Il2CppObject* o, size_t off) {
    __try {
        return *reinterpret_cast<Il2CppString**>(reinterpret_cast<char*>(o) + off);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

// Exception text without il2cpp_format_exception (which runs managed
// ToString() outside any guard): "Ns.Class: _message".
std::string DescribeException(Il2CppException* exc) {
    auto* o = reinterpret_cast<Il2CppObject*>(exc);
    const char* ns = nullptr;
    const char* name = SafeClassName(o, &ns);
    std::string text = name ? ((ns && *ns) ? std::string(ns) + "." + name : std::string(name)) : "managed exception";
    size_t off = 0;
    if (Il2CppClass* k = ClassOf(o); k && FieldOffsetInHierarchy(k, "_message", off))
        if (Il2CppString* msg = SafeReadString(o, off)) text += ": " + R().ToUtf8(msg);
    return text;
}

}  // namespace

Il2CppObject* Invoke(const MethodInfo* m, void* obj, std::initializer_list<void*> args, std::string* err) {
    if (!m) {
        if (err) *err = "null MethodInfo";
        return nullptr;
    }
    if (AnyFault()) {
        if (err) *err = "UI is in fault mode (earlier native exception)";
        return nullptr;
    }
    std::vector<void*> params(args);
    Il2CppException* exc = nullptr;
    DWORD code = 0;
    Il2CppObject* r = InvokeSeh(m, obj, params.empty() ? nullptr : params.data(), &exc, &code);
    if (code) {
        MarkFault("runtime_invoke", code);
        if (err) *err = std::format("native exception 0x{:08X} in runtime_invoke", code);
        return nullptr;
    }
    if (exc) {
        if (err) *err = DescribeException(exc);
        return nullptr;
    }
    return r;
}

void* Unbox(Il2CppObject* boxed) {
    using Fn = void* (*)(Il2CppObject*);
    static Fn fn = reinterpret_cast<Fn>(Export("il2cpp_object_unbox"));
    if (!boxed || !fn) return nullptr;
    return fn(boxed);
}

Il2CppObject* NewObject(Il2CppClass* k) {
    using Fn = Il2CppObject* (*)(Il2CppClass*);
    static Fn fn = reinterpret_cast<Fn>(Export("il2cpp_object_new"));
    if (!k || !fn) return nullptr;
    return fn(k);
}

Il2CppClass* ClassOf(Il2CppObject* o) {
    auto fn = il2cpp::api().il2cpp_object_get_class;
    return (o && fn) ? fn(o) : nullptr;
}

bool IsA(Il2CppObject* o, Il2CppClass* k) {
    Il2CppClass* c = ClassOf(o);
    if (!c || !k) return false;
    if (c == k) return true;
    if (auto fn = il2cpp::api().il2cpp_class_is_assignable_from) return fn(k, c);
    auto parentOf = il2cpp::api().il2cpp_class_get_parent;
    for (; c && parentOf; c = parentOf(c))
        if (c == k) return true;
    return false;
}

void* Keep(Il2CppObject* o) {
    using Fn = void* (*)(Il2CppObject*, bool);
    static Fn fn = reinterpret_cast<Fn>(Export("il2cpp_gchandle_new"));
    return (o && fn) ? fn(o, false) : nullptr;
}

void Release(void* handle) {
    using Fn = void (*)(void*);
    static Fn fn = reinterpret_cast<Fn>(Export("il2cpp_gchandle_free"));
    if (handle && fn) fn(handle);
}

namespace {
std::atomic<bool> g_fault{false};
}

bool AnyFault() { return g_fault.load(std::memory_order_relaxed); }

void MarkFault(const char* where, unsigned long code) {
    core::text_override::SetSinkActive(false);  // LocalizedString overrides stop with the UI hooks
    if (!g_fault.exchange(true))
        SR2_ERROR("ui: native exception 0x{:08X} in {} - UI stops calling into the game for this session "
                  "(hooks pass through; the overlay window itself keeps working)", code, where ? where : "?");
}

}  // namespace sr2::ui::il
