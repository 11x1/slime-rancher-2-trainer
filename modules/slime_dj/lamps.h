#pragma once
// slime_dj: "Lamps pulse to the music". Placed lamp decorations (Electric Pillar Lamp, Pink Striped
// Lamp, Conservatory Lamp, Blue Lava Lamp, Ice Lamp and their variants) and the glowing ice crystals of
// Powderfall Bluffs near a speaker that plays a song (music box, DJ Booth, speaker stack, Slime Radio,
// Corral Radio) pulse their glow, emission and Light components with that song's beat analysis
// (beat.h), the same per-hop level / onset / energy the walls use (walls.cpp). Strength grows with the
// number and loudness of the speakers in range (one close speaker = a gentle pulse).
//
// Main thread only, inside the module's guarded tick (no hooks). Lamps come from
// GameModel.identifiablesByIdent (a dictionary lookup, no scene search) at most every
// kFindSeconds; scene ice from one FindObjectsOfTypeAll(LightControl) per scene group. Only lamps
// within range of a playing speaker are cached and touched per frame.
// Everything changed is put back (the renderers' own property blocks, the lights' own values) when the song stops or pauses, the lamp leaves range, the setting goes off,
// the scene changes or the module is disabled. See NOTES.md "Lamps pulse to the music".

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "core/il2cpp_api.h"
#include "modules/slime_dj/beat.h"

namespace sr2::modules::slime_dj {

class LampPulser {
public:
    // A speaker that plays its channel's song now (positions from the last box scan).
    struct Source {
        float x = 0, y = 0, z = 0;
        const void* channel = nullptr;  // identity of the channel it plays (key into Channel below)
        float loud = 1.0f;              // its own loudness share (emitter volume scale x muffle), 0..1+
    };
    // One channel's play position, every frame.
    struct Channel {
        const void* key = nullptr;
        const BeatTrack* track = nullptr;  // the channel's stream analysis (nullptr = no song)
        uint64_t frame = 0;                // song frame heard now (lag already subtracted)
        unsigned gen = 0;                  // clip generation (a new song / seek resets the read position)
        bool playing = false;              // false while paused, buffering, past the end
    };

    LampPulser();
    ~LampPulser();
    // Once, in the module's Setup. False (with `why`) = the feature is unavailable this session.
    bool Resolve(std::string& why);
    bool Ready() const;

    // Every music box scan (2 s), and right away when the set of playing speakers changed (the module
    // checks it every 0.25 s): the playing speakers and the lamp range (m). Finds the lamps at most every
    // kFindSeconds (kNewSourceSeconds when a speaker joined); lamps out of range are restored and dropped.
    void Sync(const std::vector<Source>& sources, float range);
    // Every frame. Channels not listed (or not playing) let their lamps settle and restore.
    void Tick(float dt, const std::vector<Channel>& channels);
    // Restores everything and forgets the lamps (setting off, no song, module disabled).
    void Clear();
    // Every frame, music or not: `want` = debug_tools' highlighter is on (modules/common/debug_marks.h).
    // Publishes boxes of the pulsing lamps, the loaded lamps that do not pulse and the glowing ice crystals;
    // scans the zone's ice without music. Off: removes its categories.
    void Debug(bool want);
    size_t Lamps() const;

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

}  // namespace sr2::modules::slime_dj
