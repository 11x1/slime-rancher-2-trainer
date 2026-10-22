#pragma once
// Text overrides for game LocalizedString objects (Unity.Localization).
//
// Modules that clone a game ScriptableObject (e.g. a plot purchase item) get
// fresh LocalizedString instances with the template's table/entry. They
// register their own text for those INSTANCES here. The single text sink of
// the game, LocalizeStringEvent.UpdateString(string), is hooked by the UI
// (ui/options_tab.cpp): when the event's m_StringReference is a registered
// instance, the registered text replaces the localized one. Game strings are
// never touched (pointer-identity lookup, no string compare).
//
// Registered objects must be kept alive by the caller (GC handle on the
// owning object) for as long as they are registered: identity = address.
//
// Threading: Set/Remove/Lookup are thread-safe (one mutex; Lookup returns
// immediately while the map is empty).

#include <string>
#include <string_view>

#include "il2cpp_api.h"

namespace sr2::core::text_override {

void Set(Il2CppObject* localizedString, std::string text);
void Remove(Il2CppObject* localizedString);
// True + text if `localizedString` is registered.
bool Lookup(Il2CppObject* localizedString, std::string& out);
bool Any();  // cheap: map non-empty

// The UI marks the sink live once its UpdateString hook is enabled, and dead
// again on a fault. Modules hide their cloned entries while it is dead (they
// would otherwise show the template's text).
void SetSinkActive(bool active);
bool SinkActive();

}  // namespace sr2::core::text_override
