#pragma once
// Small by-name IL2CPP helpers used by the UI code (thin layer over
// core::il2cpp::Resolver). Nothing here uses RVAs or hardcoded offsets.

#include <cstddef>
#include <initializer_list>
#include <string>
#include <string_view>

#include "core/il2cpp_api.h"

namespace sr2::ui::il {

// Export of GameAssembly.dll by name (for il2cpp_* functions the core table
// does not carry, e.g. il2cpp_object_new / il2cpp_object_unbox).
void* Export(const char* name);

Il2CppClass* Class(std::string_view image, std::string_view ns, std::string_view name, std::string* err = nullptr);
const MethodInfo* Method(Il2CppClass* k, std::string_view name, int argc, std::string* err = nullptr);
// Searches `k` and its parents.
const MethodInfo* MethodInHierarchy(Il2CppClass* k, std::string_view name, int argc, std::string* err = nullptr);
bool FieldOffset(Il2CppClass* k, std::string_view name, size_t& out, std::string* err = nullptr);
// Searches `k` and its parents.
bool FieldOffsetInHierarchy(Il2CppClass* k, std::string_view name, size_t& out, std::string* err = nullptr);
void* MethodPointer(const MethodInfo* m, std::string* err = nullptr);

// il2cpp_runtime_invoke with managed-exception + SEH capture. `args` are
// pointers to the arguments (pointer to the value for value types, the
// object pointer itself for reference types).
Il2CppObject* Invoke(const MethodInfo* m, void* obj, std::initializer_list<void*> args, std::string* err = nullptr);

// Unboxes a boxed value-type return value (il2cpp_object_unbox by name).
void* Unbox(Il2CppObject* boxed);
template <class T>
bool UnboxTo(Il2CppObject* boxed, T& out) {
    void* p = Unbox(boxed);
    if (!p) return false;
    out = *static_cast<T*>(p);
    return true;
}

Il2CppObject* NewObject(Il2CppClass* k);  // il2cpp_object_new (no constructor run)
Il2CppClass* ClassOf(Il2CppObject* o);
bool IsA(Il2CppObject* o, Il2CppClass* k);  // o's class is k or derives from it

// Strong GC handle that keeps a managed object alive for the rest of the
// process (never freed). Il2CppGCHandle is pointer-sized in this IL2CPP
// version, so this resolves il2cpp_gchandle_new itself with that signature.
void* Keep(Il2CppObject* o);
// Frees a handle returned by Keep (pointer-sized; no-op for nullptr).
void Release(void* handle);

// Fault policy: true once a native exception was caught inside game/IL2CPP
// code (in Invoke). From then on the UI must not call into the game again;
// hooks only pass through to the original.
bool AnyFault();
void MarkFault(const char* where, unsigned long code);

}  // namespace sr2::ui::il
