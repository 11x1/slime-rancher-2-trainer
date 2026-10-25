#pragma once
// slime_dj: "Plorts glow to the music" (round 16: "Plorts glow to music aswell pleaseeeeeee"). Loose plorts
// near a speaker that plays a song brighten their body colours with that song's beat, read from the same
// sources and channels as the lamps (LampPulser::Source / Channel, built by the module) and smoothed the
// same way. Strength grows with the number and loudness of the speakers in range, like the lamps.
//
// Main thread only, inside the module's guarded tick (no hooks). Plorts come from
// GameModel.identifiablesByIdent[plort type] (one dictionary lookup per type of GameModel.plortGroup, no
// scene search) at most every kFindSeconds while a speaker plays; up to kMaxPlorts nearest to a speaker
// pulse, through a property block on each plort's body renderer(s). The renderer's own block is put back
// when the song stops or pauses, the plort leaves range, the setting goes off, or the module is disabled;
// a plort that was vacuumed, sold or eaten is simply dropped. See NOTES.md "Plorts glow to the music".

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "modules/slime_dj/lamps.h"

namespace sr2::modules::slime_dj {

class PlortPulser {
public:
    using Source = LampPulser::Source;
    using Channel = LampPulser::Channel;

    PlortPulser();
    ~PlortPulser();
    // Once, in the module's Setup. False (with `why`) = the feature is unavailable this session.
    bool Resolve(std::string& why);
    bool Ready() const;

    // Every box scan (2 s) and when the set of playing speakers changed: the playing speakers and the
    // plort range (m). Finds the loaded plorts at most every kFindSeconds (sooner when a speaker joined),
    // otherwise re-reads the positions of the plorts it holds (plorts roll and get carried).
    void Sync(const std::vector<Source>& sources, float range);
    // Every frame. Channels not listed (or not playing) let their plorts settle and restore.
    void Tick(float dt, const std::vector<Channel>& channels);
    // Restores everything and forgets the plorts (setting off, no song, module disabled).
    void Clear();
    // Every frame, music or not: `want` = debug_tools' highlighter is on. Publishes boxes of the glowing
    // plorts and of loaded plorts that do not glow (`slime_dj.plorts.*`); off: removes its categories.
    void Debug(bool want);
    size_t Plorts() const;

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

// Registers `dance_plorts` and `plort_range` (module RegisterSettings, after the lamp settings).
void RegisterPlortSettings();

}  // namespace sr2::modules::slime_dj
