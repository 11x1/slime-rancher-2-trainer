#pragma once
// custom_upgrades: the mod's buildables for other modules (the Sandbox crafter and blueprint manager).
//
// The mod's Fabricator buildables are runtime GadgetDefinition clones that the vanilla save must never
// see (quantum_link.h, dj_buildables.h): the Quantum Storage Link (a Refinery Link variant), the DJ
// Booth (a Scientist Desk variant), the DJ Speaker (a Boombox variant, three tilts) and the Slime Radio
// (a Night Light toy). Never AddItem / AddBlueprint their definitions (`def` below): GadgetsModel.Pull
// would write an unknown or duplicate persistence id and the save fails. Grant() below is the save-safe
// way to give them; SetUnlock() the save-safe way to lock / unlock their Fabricator card.
//
// Unlock rule: a card is unlocked while the vanilla base gadget's blueprint is (Refinery Link,
// Scientist Desk, Boombox; the radio follows the Boombox), unless the save's sidecar has an override
// (key "buildables.unlock.<id>": "1" unlocked, "0" locked; set by SetUnlock, e.g. from the Sandbox's
// Upgrades & Blueprints card). Nothing of it is in the vanilla save.
//
// All functions: Unity main thread. They return empty / false (with `result` saying why) while custom
// upgrades are off or faulted, no save is loaded, or the definitions are not built yet (they are built
// at the first tick after a save is loaded).

#include <string>
#include <vector>

struct Il2CppObject;

namespace sr2::modules::custom_upgrades::buildables {

// Ids, stable across builds (sidecar keys and requests use them).
inline constexpr const char* kQuantumLink = "quantum_link";
inline constexpr const char* kDjBooth = "dj_booth";
inline constexpr const char* kDjSpeaker = "dj_speaker";
inline constexpr const char* kSlimeRadio = "slime_radio";

enum class Unlock : int {
    kFollowBase = 0,  // no override: unlocked with the base gadget's blueprint
    kUnlocked = 1,    // override: always unlocked
    kLocked = 2,      // override: always locked
};

struct Info {
    std::string id;            // one of the ids above
    std::string name;          // "DJ Booth"
    std::string base;          // the vanilla base gadget's display name ("Scientist Desk")
    std::string baseAsset;     // its definition's asset name ("ScientistDesk")
    std::string hint;          // what unlocks it ("Unlocked with the Scientist Desk blueprint.")
    Il2CppObject* def = nullptr;      // our Fabricator card definition (do not put it into the save, see above)
    Il2CppObject* baseDef = nullptr;  // the vanilla definition it is saved as (radio: the Boombox)
    Il2CppObject* icon = nullptr;     // Sprite for lists (the base gadget's icon), may be nullptr
    bool unlocked = false;     // the Fabricator card is unlocked right now (override applied)
    bool baseUnlocked = false; // the base gadget's blueprint is owned
    Unlock unlock = Unlock::kFollowBase;
    int owned = 0;             // unplaced kits (Quantum Storage Link, DJ Booth, DJ Speaker); Slime Radio: radios in the save
};

// Every buildable whose definition is built (in Fabricator order). Empty while not available.
std::vector<Info> List();

// Gives `count` (1..999; the radio 1..5 per call) for free, exactly as fabricating does without the cost:
// Quantum Storage Link / DJ Booth / DJ Speaker = `count` vanilla base gadgets into the inventory
// (GadgetDirector.AddItem) plus `count` kits in the sidecar, so the variant can be placed; Slime Radio =
// `count` radios spawned in front of the player (listed in the sidecar, posted to the next frame).
// Works with the card locked too. `result` gets a one-line summary or the reason it failed.
bool Grant(const std::string& id, int count, std::string& result);

// Sets the sidecar override (kFollowBase clears it). Takes effect at once (an open Fabricator shows it
// after a rebuild / reopen). `result` as above.
bool SetUnlock(const std::string& id, Unlock unlock, std::string& result);

// ---- for quantum_link.cpp / dj_buildables.cpp ----
// The sidecar key of `id`'s override and its parsed value.
std::string UnlockKey(const std::string& id);
Unlock ParseUnlock(const std::string& value);

}  // namespace sr2::modules::custom_upgrades::buildables
