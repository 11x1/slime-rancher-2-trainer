#pragma once
// slime_dj: public surface for other modules (buildable DJ booths, speakers, radios, the
// Music Box II prop of custom_upgrades).
//
// An emitter is any GameObject that should play the Slime DJ song. Slime DJ gives every
// registered object a child "sr2dj_Speaker" with its own Unity AudioSource that shares the song's
// clip and plays it in sync with the corral music boxes (same position, started together or
// joined at the current position). Emitters play wherever they are, also away from the ranch.
// They follow the player's Volume and Hearing range settings, scaled per emitter.
//
// Lifetime: register when the object is built / loaded. Unregister when it is removed; an
// object Unity destroyed is dropped by itself at the next scan (every 2 s while a song plays),
// so a missed Unregister costs nothing. Registering the same object again updates its kind and
// scales. Registrations are kept while Slime DJ is off and used when it is on again.
//
// Main thread only, outside detours (Register takes a GC handle; handles are freed later on
// Slime DJ's own tick). Safe to call whether or not the module is enabled, set up or faulted.

#include <string>

struct Il2CppObject;

namespace sr2::modules::slime_dj {

enum class EmitterKind : int {
    kSpeaker = 0,  // a plain speaker (Music Box II's second speaker, buildable speakers)
    kBooth,        // a DJ booth
    kRadio,        // a radio
};

// `obj`: a GameObject or any Component on it (the speaker is parented to its transform).
// `rangeScale` multiplies the Hearing range setting (0.1-4), `volumeScale` the song volume
// (0-1; Unity caps an AudioSource at full volume). False if obj is not a live Unity object.
bool RegisterEmitter(Il2CppObject* obj, EmitterKind kind, float rangeScale = 1.0f, float volumeScale = 1.0f);
// Same object (or the same Component) as registered. Unknown objects are ignored.
void UnregisterEmitter(Il2CppObject* obj);

// Any thread. A song is loaded, not paused by the player, and audible somewhere (at least one
// music box or emitter plays it); for "now playing" lights. Updated on Slime DJ's tick.
bool SongPlaying();
// Any thread. Name of the loaded song ("" = none).
std::string SongName();

// Main thread (a detour is fine: it only records the request). Opens the Corral Radio playlist menu
// (radio_menu.h) for this corral LandPlot in Slime DJ's next tick. False with `why` (a short sentence
// for the player) when it can't: Slime DJ off, the menu unavailable, already open, plot unknown.
// Used by custom_upgrades' blue Corral Radio button.
bool OpenRadioMenu(Il2CppObject* landPlot, std::string* why = nullptr);

}  // namespace sr2::modules::slime_dj
