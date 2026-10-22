#pragma once
// Name-based IL2CPP lookups. No hardcoded RVAs or offsets: everything goes
// through the il2cpp_* exports. Every lookup returns nullptr on failure and
// writes a human-readable reason to `err` (if given) for the module log.

#include <cstddef>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "il2cpp_api.h"

namespace sr2::il2cpp {

struct ImageInfo {
    std::string name;  // e.g. "Assembly-CSharp.dll"
    const Il2CppImage* image = nullptr;
};

class Resolver {
public:
    static Resolver& Get();

    // (Re)builds the image list from il2cpp_domain_get_assemblies.
    void Refresh();
    const std::vector<ImageInfo>& Images();

    // Accepts "Assembly-CSharp" or "Assembly-CSharp.dll" (case-insensitive).
    const Il2CppImage* Image(std::string_view name, std::string* err = nullptr);

    Il2CppClass* Class(const Il2CppImage* image, std::string_view ns, std::string_view name, std::string* err = nullptr);
    Il2CppClass* Class(std::string_view image, std::string_view ns, std::string_view name, std::string* err = nullptr);

    // argc = -1 matches any parameter count.
    const MethodInfo* Method(Il2CppClass* klass, std::string_view name, int argc, std::string* err = nullptr);
    // Overload-exact lookup: the non-generic method `name` whose parameter
    // types' full names (il2cpp_type_get_name, e.g. "System.Type",
    // "UnityEngine.Transform", "System.Boolean") equal `paramTypes`.
    // Searches `klass`, then its parents.
    const MethodInfo* MethodByParams(Il2CppClass* klass, std::string_view name,
                                     std::initializer_list<std::string_view> paramTypes, std::string* err = nullptr);
    FieldInfo* Field(Il2CppClass* klass, std::string_view name, std::string* err = nullptr);
    std::optional<size_t> FieldOffset(Il2CppClass* klass, std::string_view name, std::string* err = nullptr);

    // Native code pointer of a compiled method (the address to hook).
    void* MethodPointer(const MethodInfo* method, std::string* err = nullptr);

    // Convenience: image + namespace + class + method -> native pointer.
    void* MethodPointer(std::string_view image, std::string_view ns, std::string_view klass, std::string_view method,
                        int argc, std::string* err = nullptr);

    // il2cpp_runtime_invoke with managed-exception capture and an SEH guard.
    // For rare calls only (slow, boxes value types). `err` gets the exception text.
    Il2CppObject* Invoke(const MethodInfo* method, void* obj, void** params, std::string* err = nullptr);

    // Il2CppString -> UTF-8 (empty for null).
    std::string ToUtf8(Il2CppString* s);

    // RVA of a pointer inside GameAssembly.dll (for logging / cross-checking dumps).
    static uintptr_t Rva(const void* p);

private:
    std::vector<ImageInfo> images_;
    std::unordered_map<std::string, const Il2CppImage*> byName_;  // lower-case, without ".dll"
};

}  // namespace sr2::il2cpp
