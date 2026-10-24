#pragma once
// Raw IL2CPP C API, resolved by export NAME from GameAssembly.dll at runtime.
// Opaque handle types only: no struct layouts are assumed here (the single
// layout assumption in the project lives in Resolver::MethodPointer).

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct Il2CppDomain;
struct Il2CppAssembly;
struct Il2CppImage;
struct Il2CppClass;
struct MethodInfo;
struct FieldInfo;
struct Il2CppObject;
struct Il2CppString;
struct Il2CppException;
struct Il2CppThread;
struct Il2CppType;
struct Il2CppArray;
using Il2CppChar = char16_t;
// Pointer-sized in this IL2CPP version (Unity 6, metadata v31): `typedef void* Il2CppGCHandle`
// (verified against the il2cpp-api-functions.h / il2cpp-types.h of the 1.3.0 dump).
using Il2CppGCHandle = void*;
static_assert(sizeof(Il2CppGCHandle) == sizeof(void*), "Il2CppGCHandle must be pointer-sized");

// X(required, return type, name, parameter list)
#define SR2_IL2CPP_API(X)                                                                                         \
    X(true, Il2CppDomain*, il2cpp_domain_get, ())                                                                 \
    X(true, const Il2CppAssembly**, il2cpp_domain_get_assemblies, (const Il2CppDomain*, size_t*))                 \
    X(true, Il2CppThread*, il2cpp_thread_attach, (Il2CppDomain*))                                                 \
    X(true, void, il2cpp_thread_detach, (Il2CppThread*))                                                          \
    X(true, const Il2CppImage*, il2cpp_get_corlib, ())                                                            \
    X(true, const Il2CppImage*, il2cpp_assembly_get_image, (const Il2CppAssembly*))                               \
    X(true, const char*, il2cpp_image_get_name, (const Il2CppImage*))                                             \
    X(true, Il2CppClass*, il2cpp_class_from_name, (const Il2CppImage*, const char*, const char*))                 \
    X(true, const MethodInfo*, il2cpp_class_get_method_from_name, (Il2CppClass*, const char*, int))               \
    X(true, FieldInfo*, il2cpp_class_get_field_from_name, (Il2CppClass*, const char*))                            \
    X(true, size_t, il2cpp_field_get_offset, (FieldInfo*))                                                        \
    X(true, Il2CppObject*, il2cpp_runtime_invoke, (const MethodInfo*, void*, void**, Il2CppException**))         \
    X(false, size_t, il2cpp_image_get_class_count, (const Il2CppImage*))                                          \
    X(false, const Il2CppClass*, il2cpp_image_get_class, (const Il2CppImage*, size_t))                            \
    X(false, const char*, il2cpp_class_get_name, (Il2CppClass*))                                                  \
    X(false, const char*, il2cpp_class_get_namespace, (Il2CppClass*))                                             \
    X(false, const char*, il2cpp_method_get_name, (const MethodInfo*))                                            \
    X(false, uint32_t, il2cpp_method_get_param_count, (const MethodInfo*))                                        \
    X(false, Il2CppClass*, il2cpp_object_get_class, (Il2CppObject*))                                              \
    X(false, Il2CppChar*, il2cpp_string_chars, (Il2CppString*))                                                   \
    X(false, int32_t, il2cpp_string_length, (Il2CppString*))                                                      \
    X(false, void, il2cpp_format_exception, (const Il2CppException*, char*, int))                                 \
    X(false, void, il2cpp_format_stack_trace, (const Il2CppException*, char*, int))                               \
    X(false, bool, il2cpp_is_vm_thread, (Il2CppThread*))                                                          \
    X(false, Il2CppThread*, il2cpp_thread_current, ())                                                            \
    X(false, Il2CppString*, il2cpp_string_new, (const char*))                                                     \
    X(false, const Il2CppType*, il2cpp_class_get_type, (Il2CppClass*))                                            \
    X(false, Il2CppObject*, il2cpp_type_get_object, (const Il2CppType*))                                          \
    X(false, char*, il2cpp_type_get_name, (const Il2CppType*))                                                    \
    X(false, Il2CppClass*, il2cpp_class_from_type, (const Il2CppType*))                                           \
    X(false, Il2CppClass*, il2cpp_class_get_parent, (Il2CppClass*))                                               \
    X(false, bool, il2cpp_class_is_assignable_from, (Il2CppClass*, Il2CppClass*))                                 \
    X(false, const MethodInfo*, il2cpp_class_get_methods, (Il2CppClass*, void**))                                 \
    X(false, const Il2CppType*, il2cpp_method_get_param, (const MethodInfo*, uint32_t))                           \
    X(false, bool, il2cpp_method_is_generic, (const MethodInfo*))                                                 \
    X(false, uint32_t, il2cpp_array_length, (Il2CppArray*))                                                       \
    X(false, Il2CppGCHandle, il2cpp_gchandle_new, (Il2CppObject*, bool))                                          \
    X(false, Il2CppObject*, il2cpp_gchandle_get_target, (Il2CppGCHandle))                                         \
    X(false, void, il2cpp_gchandle_free, (Il2CppGCHandle))                                                        \
    X(false, void, il2cpp_free, (void*))

namespace sr2::il2cpp {

struct Api {
#define SR2_DECLARE_FN(req, ret, name, params) ret(*name) params = nullptr;
    SR2_IL2CPP_API(SR2_DECLARE_FN)
#undef SR2_DECLARE_FN

    HMODULE module = nullptr;    // GameAssembly.dll
    uintptr_t moduleBase = 0;
    size_t moduleSize = 0;       // SizeOfImage from the PE header
};

// The process-wide table (valid after Load succeeded).
Api& api();

// Resolves every export by name. Returns false if a *required* export is
// missing; `missingRequired` / `missingOptional` list the names not found.
bool Load(HMODULE gameAssembly, std::vector<std::string>& missingRequired, std::vector<std::string>& missingOptional);

// True if `p` points inside GameAssembly.dll.
bool InGameAssembly(const void* p);

}  // namespace sr2::il2cpp
