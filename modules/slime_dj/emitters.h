#pragma once
// slime_dj: the emitter registry behind api.h, module side. See api.h for the public calls.

#include <string>
#include <vector>

#include "core/il2cpp_api.h"
#include "modules/slime_dj/api.h"

namespace sr2::modules::slime_dj::emitters {

struct Entry {
    Il2CppObject* key = nullptr;    // the object as registered (identity only)
    Il2CppGCHandle h = nullptr;     // keeps the managed wrapper alive
    EmitterKind kind = EmitterKind::kSpeaker;
    float range = 1.0f, volume = 1.0f;
};

// Main thread (slime_dj tick). Frees the handles of unregistered entries and drops entries whose
// object Unity destroyed; returns the live ones (handles stay owned by the registry).
std::vector<Entry> Collect();
// Bumped by every Register / Unregister (any change worth an early rescan).
unsigned Generation();
// Published by the module tick for SongPlaying() / SongName().
void SetPlaying(bool playing, const std::string& name);

}  // namespace sr2::modules::slime_dj::emitters
