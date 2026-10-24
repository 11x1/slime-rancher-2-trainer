#include "resolver.h"

#include <algorithm>
#include <format>
#include <mutex>

#include "log.h"

namespace sr2::il2cpp {
namespace {

std::recursive_mutex g_mutex;

std::string Key(std::string_view name) {
    std::string k(name);
    std::transform(k.begin(), k.end(), k.begin(), [](unsigned char c) { return static_cast<char>(tolower(c)); });
    if (k.size() > 4 && k.ends_with(".dll")) k.resize(k.size() - 4);
    return k;
}

void SetErr(std::string* err, std::string msg) {
    if (err) *err = std::move(msg);
}

// ---- managed exception -> text, fault-proof ----------------------------------
// Everything below runs in frames without C++ objects that need unwinding
// (required for __try). Each step has its own guard, so a fault in one step
// (e.g. a bogus exception object) only loses that part of the message.

void AppendC(char* out, size_t n, const char* s) {
    if (s && *s) strncat_s(out, n, s, _TRUNCATE);
}

// "Namespace.Class" of the exception object.
void ExcClassName(Il2CppException* exc, char* out, size_t n) {
    const Api& a = api();
    if (!a.il2cpp_object_get_class || !a.il2cpp_class_get_name) return;
    __try {
        Il2CppClass* k = a.il2cpp_object_get_class(reinterpret_cast<Il2CppObject*>(exc));
        if (!k) return;
        const char* ns = a.il2cpp_class_get_namespace ? a.il2cpp_class_get_namespace(k) : nullptr;
        const char* cn = a.il2cpp_class_get_name(k);
        if (ns && *ns) {
            AppendC(out, n, ns);
            AppendC(out, n, ".");
        }
        AppendC(out, n, cn);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        AppendC(out, n, "<unreadable exception class>");
    }
}

// System.Exception._message (resolved by name; il2cpp_class_get_field_from_name
// walks the parent chain) converted to UTF-8.
void ExcMessage(Il2CppException* exc, char* out, size_t n) {
    const Api& a = api();
    if (!a.il2cpp_object_get_class || !a.il2cpp_string_chars || !a.il2cpp_string_length) return;
    __try {
        Il2CppClass* k = a.il2cpp_object_get_class(reinterpret_cast<Il2CppObject*>(exc));
        FieldInfo* f = k ? a.il2cpp_class_get_field_from_name(k, "_message") : nullptr;
        if (!f) return;
        size_t off = a.il2cpp_field_get_offset(f);
        if (off == 0 || off > 0x1000) return;
        auto* msg = *reinterpret_cast<Il2CppString**>(reinterpret_cast<char*>(exc) + off);
        if (!msg) return;
        const Il2CppChar* chars = a.il2cpp_string_chars(msg);
        int32_t len = a.il2cpp_string_length(msg);
        if (!chars || len <= 0) return;
        if (len > 2048) len = 2048;
        int w = WideCharToMultiByte(CP_UTF8, 0, reinterpret_cast<const wchar_t*>(chars), len, out,
                                    static_cast<int>(n - 1), nullptr, nullptr);
        out[w > 0 ? w : 0] = '\0';
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out[0] = '\0';
    }
}

// Last resort: il2cpp_format_exception (also guarded).
void ExcFormat(Il2CppException* exc, char* out, size_t n) {
    const Api& a = api();
    if (!a.il2cpp_format_exception) return;
    __try {
        a.il2cpp_format_exception(exc, out, static_cast<int>(n - 1));
        out[n - 1] = '\0';
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out[0] = '\0';
    }
}

// The managed stack's first frames (il2cpp_format_stack_trace: "at Ns.Class.Method () [0x00000] in <...>:0"
// per line) as " (at A.B; at C.D; ...)", so a log line says where inside the game a call threw (round 10:
// the upgrade NullReferenceException took a decompile to place). Empty when the runtime keeps no trace.
void ExcTrace(Il2CppException* exc, char* out, size_t n) {
    const Api& a = api();
    out[0] = '\0';
    if (!a.il2cpp_format_stack_trace) return;
    char raw[2048] = {};
    __try {
        a.il2cpp_format_stack_trace(exc, raw, static_cast<int>(sizeof(raw) - 1));
        raw[sizeof(raw) - 1] = '\0';
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return;
    }
    constexpr int kFrames = 5;
    int frames = 0;
    const char* p = raw;
    while (*p && frames < kFrames) {
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') ++p;
        if (!*p) break;
        const char* end = p;
        while (*end && *end != '\r' && *end != '\n') ++end;
        // "at X.Y (args) [0x00000] in <...>:0" -> "at X.Y (args)"
        const char* cut = end;
        for (const char* q = p; q + 1 < end; ++q)
            if (q[0] == ' ' && q[1] == '[') {
                cut = q;
                break;
            }
        char frame[256] = {};
        const size_t len = static_cast<size_t>(cut - p) < sizeof(frame) - 1 ? static_cast<size_t>(cut - p) : sizeof(frame) - 1;
        memcpy(frame, p, len);
        AppendC(out, n, frames ? "; " : " (");
        AppendC(out, n, frame);
        ++frames;
        p = end;
    }
    if (frames) AppendC(out, n, *p ? "; ...)" : ")");
}

void DescribeException(Il2CppException* exc, char* out, size_t n) {
    out[0] = '\0';
    char cls[256] = {};
    char msg[768] = {};
    char trace[768] = {};
    ExcClassName(exc, cls, sizeof(cls));
    ExcMessage(exc, msg, sizeof(msg));
    if (!msg[0] && !cls[0]) ExcFormat(exc, msg, sizeof(msg));
    ExcTrace(exc, trace, sizeof(trace));
    AppendC(out, n, cls[0] ? cls : "managed exception");
    if (msg[0]) {
        AppendC(out, n, ": ");
        AppendC(out, n, msg);
    }
    AppendC(out, n, trace);
}

// Invoke + exception description, all under SEH. On a managed exception the
// text lands in excText. No C++ objects with destructors in this frame.
Il2CppObject* InvokeGuarded(const MethodInfo* m, void* obj, void** params, bool* threw, char* excText, size_t n,
                            DWORD* sehCode) {
    __try {
        Il2CppException* exc = nullptr;
        Il2CppObject* r = api().il2cpp_runtime_invoke(m, obj, params, &exc);
        if (exc) {
            *threw = true;
            DescribeException(exc, excText, n);
            return nullptr;
        }
        return r;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *sehCode = GetExceptionCode();
        return nullptr;
    }
}

}  // namespace

Resolver& Resolver::Get() {
    static Resolver r;
    return r;
}

void Resolver::Refresh() {
    std::lock_guard lock(g_mutex);
    images_.clear();
    byName_.clear();
    const Api& a = api();
    size_t count = 0;
    Il2CppDomain* domain = a.il2cpp_domain_get();
    const Il2CppAssembly** assemblies = domain ? a.il2cpp_domain_get_assemblies(domain, &count) : nullptr;
    if (!assemblies) return;
    for (size_t i = 0; i < count; ++i) {
        const Il2CppImage* img = assemblies[i] ? a.il2cpp_assembly_get_image(assemblies[i]) : nullptr;
        if (!img) continue;
        const char* n = a.il2cpp_image_get_name(img);
        ImageInfo info{n ? n : "<null>", img};
        byName_.emplace(Key(info.name), img);
        images_.push_back(std::move(info));
    }
}

const std::vector<ImageInfo>& Resolver::Images() {
    std::lock_guard lock(g_mutex);
    if (images_.empty()) Refresh();
    return images_;
}

const Il2CppImage* Resolver::Image(std::string_view name, std::string* err) {
    std::lock_guard lock(g_mutex);
    if (images_.empty()) Refresh();
    auto it = byName_.find(Key(name));
    if (it == byName_.end()) {
        Refresh();  // assemblies loaded later (rare in IL2CPP, but cheap to retry)
        it = byName_.find(Key(name));
    }
    if (it == byName_.end()) {
        SetErr(err, std::format("image '{}' not loaded", name));
        return nullptr;
    }
    return it->second;
}

Il2CppClass* Resolver::Class(const Il2CppImage* image, std::string_view ns, std::string_view name, std::string* err) {
    if (!image) {
        SetErr(err, std::format("class '{}.{}': null image", ns, name));
        return nullptr;
    }
    std::string sns(ns), sname(name);
    Il2CppClass* k = api().il2cpp_class_from_name(image, sns.c_str(), sname.c_str());
    if (!k) {
        const char* in = api().il2cpp_image_get_name(image);
        SetErr(err, std::format("class '{}{}{}' not found in {}", ns, ns.empty() ? "" : ".", name, in ? in : "?"));
    }
    return k;
}

Il2CppClass* Resolver::Class(std::string_view image, std::string_view ns, std::string_view name, std::string* err) {
    const Il2CppImage* img = Image(image, err);
    return img ? Class(img, ns, name, err) : nullptr;
}

const MethodInfo* Resolver::Method(Il2CppClass* klass, std::string_view name, int argc, std::string* err) {
    if (!klass) {
        SetErr(err, std::format("method '{}': null class", name));
        return nullptr;
    }
    std::string sname(name);
    const MethodInfo* m = api().il2cpp_class_get_method_from_name(klass, sname.c_str(), argc);
    if (!m) {
        const char* cn = api().il2cpp_class_get_name ? api().il2cpp_class_get_name(klass) : nullptr;
        SetErr(err, std::format("method '{}' (argc={}) not found on class {}", name, argc, cn ? cn : "?"));
    }
    return m;
}

const MethodInfo* Resolver::MethodByParams(Il2CppClass* klass, std::string_view name,
                                           std::initializer_list<std::string_view> paramTypes, std::string* err) {
    const Api& a = api();
    if (!klass) {
        SetErr(err, std::format("method '{}': null class", name));
        return nullptr;
    }
    if (!a.il2cpp_class_get_methods || !a.il2cpp_method_get_name || !a.il2cpp_method_get_param_count ||
        !a.il2cpp_method_get_param || !a.il2cpp_type_get_name) {
        SetErr(err, std::format("method '{}': il2cpp method/param enumeration exports missing", name));
        return nullptr;
    }
    const uint32_t argc = static_cast<uint32_t>(paramTypes.size());
    for (Il2CppClass* k = klass; k; k = a.il2cpp_class_get_parent ? a.il2cpp_class_get_parent(k) : nullptr) {
        void* iter = nullptr;
        while (const MethodInfo* m = a.il2cpp_class_get_methods(k, &iter)) {
            const char* mn = a.il2cpp_method_get_name(m);
            if (!mn || name != mn || a.il2cpp_method_get_param_count(m) != argc) continue;
            if (a.il2cpp_method_is_generic && a.il2cpp_method_is_generic(m)) continue;
            bool match = true;
            uint32_t i = 0;
            for (std::string_view want : paramTypes) {
                const Il2CppType* t = a.il2cpp_method_get_param(m, i++);
                char* tn = t ? a.il2cpp_type_get_name(t) : nullptr;
                bool eq = tn && want == tn;
                if (tn && a.il2cpp_free) a.il2cpp_free(tn);
                if (!eq) {
                    match = false;
                    break;
                }
            }
            if (match) return m;
        }
    }
    std::string sig;
    for (std::string_view p : paramTypes) sig += std::format("{}{}", sig.empty() ? "" : ", ", p);
    const char* cn = a.il2cpp_class_get_name ? a.il2cpp_class_get_name(klass) : nullptr;
    SetErr(err, std::format("method '{}({})' not found on class {}", name, sig, cn ? cn : "?"));
    return nullptr;
}

FieldInfo* Resolver::Field(Il2CppClass* klass, std::string_view name, std::string* err) {
    if (!klass) {
        SetErr(err, std::format("field '{}': null class", name));
        return nullptr;
    }
    std::string sname(name);
    FieldInfo* f = api().il2cpp_class_get_field_from_name(klass, sname.c_str());
    if (!f) {
        const char* cn = api().il2cpp_class_get_name ? api().il2cpp_class_get_name(klass) : nullptr;
        SetErr(err, std::format("field '{}' not found on class {}", name, cn ? cn : "?"));
    }
    return f;
}

std::optional<size_t> Resolver::FieldOffset(Il2CppClass* klass, std::string_view name, std::string* err) {
    FieldInfo* f = Field(klass, name, err);
    if (!f) return std::nullopt;
    return api().il2cpp_field_get_offset(f);
}

void* Resolver::MethodPointer(const MethodInfo* method, std::string* err) {
    if (!method) {
        SetErr(err, "null MethodInfo");
        return nullptr;
    }
    // LAYOUT ASSUMPTION (the only one): MethodInfo::methodPointer is the first
    // field of MethodInfo in every IL2CPP version from metadata v24 through
    // Unity 6. There is no exported getter for it. Validated below by checking
    // that the pointer lands inside GameAssembly.dll.
    void* p = *reinterpret_cast<void* const*>(method);
    if (!p) {
        SetErr(err, "method has no compiled body (abstract / generic definition / stripped)");
        return nullptr;
    }
    if (!InGameAssembly(p)) {
        SetErr(err, std::format("methodPointer {} is outside GameAssembly.dll (layout changed?)", p));
        return nullptr;
    }
    return p;
}

void* Resolver::MethodPointer(std::string_view image, std::string_view ns, std::string_view klass,
                              std::string_view method, int argc, std::string* err) {
    Il2CppClass* k = Class(image, ns, klass, err);
    const MethodInfo* m = k ? Method(k, method, argc, err) : nullptr;
    return m ? MethodPointer(m, err) : nullptr;
}

Il2CppObject* Resolver::Invoke(const MethodInfo* method, void* obj, void** params, std::string* err) {
    if (!method) {
        SetErr(err, "null MethodInfo");
        return nullptr;
    }
    bool threw = false;
    char excText[1024] = {};
    DWORD seh = 0;
    Il2CppObject* r = InvokeGuarded(method, obj, params, &threw, excText, sizeof(excText), &seh);
    if (seh) {
        SetErr(err, std::format("native exception 0x{:08X} during runtime_invoke{}{}", seh, excText[0] ? " / " : "",
                                excText));
        return nullptr;
    }
    if (threw) {
        SetErr(err, excText[0] ? std::string(excText) : std::string("managed exception"));
        return nullptr;
    }
    return r;
}

namespace {
// Copies up to `cap` UTF-16 units of `s` into dst; returns the string's full
// length, or -1 if reading faulted (bogus / freed string). SEH-only frame.
int32_t CopyStringGuarded(Il2CppString* s, wchar_t* dst, int32_t cap) {
    const Api& a = api();
    __try {
        const Il2CppChar* c = a.il2cpp_string_chars(s);
        int32_t n = a.il2cpp_string_length(s);
        if (!c || n <= 0) return 0;
        int32_t m = n < cap ? n : cap;
        memcpy(dst, c, static_cast<size_t>(m) * sizeof(wchar_t));
        return n;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}
}  // namespace

std::string Resolver::ToUtf8(Il2CppString* s) {
    const Api& a = api();
    if (!s || !a.il2cpp_string_chars || !a.il2cpp_string_length) return {};
    wchar_t small[256];
    int32_t n = CopyStringGuarded(s, small, 256);
    if (n <= 0) return {};
    if (n <= 256) return log::Narrow(std::wstring_view(small, static_cast<size_t>(n)));
    std::wstring big(static_cast<size_t>(n), L'\0');
    int32_t n2 = CopyStringGuarded(s, big.data(), n);
    if (n2 <= 0) return {};
    big.resize(static_cast<size_t>(n2 < n ? n2 : n));
    return log::Narrow(big);
}

uintptr_t Resolver::Rva(const void* p) {
    return InGameAssembly(p) ? reinterpret_cast<uintptr_t>(p) - api().moduleBase : 0;
}

}  // namespace sr2::il2cpp
