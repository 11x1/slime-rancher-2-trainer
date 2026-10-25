#pragma once
// slime_dj: "Walls dance to the music". The energy walls of a corral whose Music Box plays a
// song (and its Air Net, if bought) dance to the beat analysis of that song (beat.h; the global
// queue's, or the corral's own Corral Radio's): a vanilla
// barrier ripple runs round the walls on every beat, glow and colour follow the bass.
//
// Main thread only, inside the module's guarded tick (no hooks). Objects are found once per plot
// (and again when its walls or net change), held by GC handles; nothing is searched per frame.
// Everything changed is put back exactly (property blocks) when the song stops or pauses,
// the plot leaves the hearing range, the setting is turned off or the module is disabled. See
// NOTES.md "Walls dance to the music".

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "core/il2cpp_api.h"
#include "modules/slime_dj/beat.h"

namespace sr2::modules::slime_dj {

class WallDancer {
public:
    WallDancer();
    ~WallDancer();
    // Once, in the module's Setup. False (with `why`) = the feature is unavailable this session.
    bool Resolve(std::string& why);
    bool Ready() const;

    // A corral to animate: its MusicBoxUpgrader, the song it follows (`group`: an opaque id of the
    // module's channel, the global queue or that corral's Corral Radio; never dereferenced) and a
    // factor on the intensity (speakers placed in the corral make its walls move more).
    struct Target {
        Il2CppObject* up = nullptr;
        uintptr_t group = 0;
        float boost = 1.0f;
    };
    // Every music box scan: the corrals to animate now (their box plays a song and the camera is
    // within hearing range). Others are restored and dropped.
    void Sync(const std::vector<Target>& targets);
    // Every frame. `beatOf(group)`: the analysis of that song at its play position, nullptr while it
    // does not play (those walls then settle and are restored). `intensity`: the setting, 0..2.
    void Tick(float dt, const std::function<const BeatTrack::Sample*(uintptr_t)>& beatOf, float intensity);
    // Restores everything and forgets the plots (setting off, song stopped, module disabled).
    void Clear();
    size_t Plots() const;

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

}  // namespace sr2::modules::slime_dj
