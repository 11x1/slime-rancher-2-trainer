#pragma once
// Small Unity / IL2CPP helpers, all resolved lazily by name. Unless noted,
// call them on the Unity main thread (see main_thread.h). All of them are
// null-safe: bad input -> nullptr / 0 / empty result.

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "il2cpp_api.h"

namespace sr2::core::unity {

Il2CppString* NewString(std::string_view utf8);  // il2cpp_string_new (any thread)
std::string ToUtf8(Il2CppString* s);             // any thread
Il2CppObject* TypeOf(Il2CppClass* k);            // System.Type for k
std::vector<Il2CppObject*> FindObjectsOfTypeAll(Il2CppClass* k);  // Resources.FindObjectsOfTypeAll(Type), includes assets
Il2CppObject* FindObjectOfType(Il2CppClass* k);  // first active scene instance or nullptr
bool IsAlive(Il2CppObject* unityObj);            // UnityEngine.Object m_CachedPtr != 0
Il2CppObject* GetComponent(Il2CppObject* goOrComponent, Il2CppClass* k);
Il2CppObject* GameObjectOf(Il2CppObject* component);
Il2CppObject* TransformOf(Il2CppObject* goOrComponent);
Il2CppObject* Instantiate(Il2CppObject* original, Il2CppObject* parentTransform);  // Object.Instantiate(Object, Transform)
void SetActive(Il2CppObject* go, bool active);
void SetName(Il2CppObject* obj, std::string_view name);
float DeltaTime();
float UnscaledDeltaTime();
float FixedDeltaTime();

template <class T>
T& Field(void* obj, size_t offset) {
    return *reinterpret_cast<T*>(reinterpret_cast<char*>(obj) + offset);
}

// Managed arrays (Il2CppArray: klass, monitor, bounds, max_length, data...).
// The data offset is the IL2CPP runtime ABI constant kIl2CppSizeOfArray on x64.
inline constexpr size_t kArrayDataOffset = 4 * sizeof(void*);
size_t ArrayLength(void* il2cppArray);
void* ArrayAt(void* il2cppArray, size_t i);  // element i (reference element); nullptr if out of range
template <class T>
T* ArrayData(void* il2cppArray) {
    return il2cppArray ? reinterpret_cast<T*>(reinterpret_cast<char*>(il2cppArray) + kArrayDataOffset) : nullptr;
}
// System.Collections.Generic.List<T> of reference types (_items/_size by name).
size_t ListCount(void* list);
void* ListAt(void* list, size_t i);

// ---- additions ----
Il2CppClass* ClassOf(Il2CppObject* obj);                    // il2cpp_object_get_class (any thread)
std::string ClassName(Il2CppObject* obj);                   // "Ns.Class" (any thread)
std::string GetName(Il2CppObject* obj);                     // UnityEngine.Object.name
bool ActiveInHierarchy(Il2CppObject* go);
Il2CppObject* Camera_main();                                // UnityEngine.Camera.main or nullptr
// Lazily resolved common classes (nullptr if not found).
Il2CppClass* GameObjectClass();
Il2CppClass* TransformClass();
Il2CppClass* ComponentClass();
// GC handles: keep a managed object alive / findable across frames (any thread).
// Raw Il2CppObject* kept in native memory are invisible to the GC - hold a
// handle for anything you cache longer than the current call.
Il2CppGCHandle NewGCHandle(Il2CppObject* obj, bool pinned = false);  // nullptr on failure
Il2CppObject* GCHandleTarget(Il2CppGCHandle handle);
void FreeGCHandle(Il2CppGCHandle handle);
// Startup self-test (core, worker thread attached): new string -> handle ->
// get_target round trip -> free, all guarded. On failure NewGCHandle returns
// nullptr from then on. `report` gets a one-line result for the log.
bool GCHandleSelfTest(std::string* report);
bool GCHandlesUsable();  // false only after a failed self-test

}  // namespace sr2::core::unity
