#pragma once
// slime_dj: "Walls dance to the music". The energy walls of a corral whose Music Box plays the
// song (and its Air Net, if bought) pulse with the beat analysis of the song (beat.h).
//
// Main thread only, inside the module's guarded tick (no hooks). Objects are found once per plot
// (and again when its walls or net change), held by GC handles; nothing is searched per frame.
// Everything changed is put back exactly (scale, property block) when the song stops or pauses,
// the plot leaves the hearing range, the setting is turned off or the module is disabled. See
// NOTES.md "Walls dance to the music".

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

    // Every music box scan: the MusicBoxUpgrader components of the corrals to animate now (their
    // box plays the song and the camera is within hearing range). Others are restored and dropped.
    void Sync(const std::vector<Il2CppObject*>& upgraders);
    // Every frame. `beat`: the analysis at the play position, nullptr while nothing plays (the
    // walls then settle and are restored). `intensity`: the setting, 0..2.
    void Tick(float dt, const BeatTrack::Sample* beat, float intensity);
    // Restores everything and forgets the plots (setting off, song stopped, module disabled).
    void Clear();
    size_t Plots() const;

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

}  // namespace sr2::modules::slime_dj
