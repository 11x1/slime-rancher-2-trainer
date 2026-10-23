#pragma once
// Data exchange between the sandbox module (modules/sandbox) and its overlay
// tab (menu.cpp, Misc > Sandbox).
//
// Threading:
//   * The module copies what the tab shows into a Snapshot on the Unity main
//     thread (about 4 Hz, only while the tab was drawn in the last ~0.6 s) and
//     publishes it here. The render thread only reads that copy: no game calls.
//   * The tab asks for changes with Request(). That posts a job to the main
//     thread (core::main_thread), which hands the Action to the module's
//     handler. The module validates and applies it with the game's own methods,
//     logs one line and sets the status text.
//   * Item ids are opaque identities chosen by the module (the address of an
//     IdentifiableType it keeps alive with a GC handle). The UI never
//     dereferences them; the module looks them up in its own catalog.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace sr2::ui::sandbox {

inline constexpr const char* kModule = "sandbox";

struct Item {
    uintptr_t id = 0;       // opaque (see above)
    std::string name;       // localized display name
    uint32_t slotMask = 0;  // bit i: vacpack slot i accepts it (AmmoSlotDefinition.IsAllowed)
};

struct Slot {
    bool unlocked = false;
    uintptr_t item = 0;     // 0 = empty
    std::string name;       // item name ("" when empty)
    int count = 0;
    int max = 0;            // AmmoSlotManager.GetSlotMaxCount
};

struct Snapshot {
    unsigned long long tick = 0;  // GetTickCount64 when it was taken
    bool inGame = false;          // a save is loaded and the player exists
    std::string why;              // !inGame: what is missing
    bool haveNewbucks = false;    // the Newbucks currency was found
    int newbucks = 0, maxNewbucks = 0;
    int health = 0, maxHealth = 0;
    int energy = 0, maxEnergy = 0;
    int rads = 0;
    bool infiniteEnergy = false;  // session toggle (not saved)
    std::vector<Slot> slots;
    std::shared_ptr<const std::vector<Item>> catalog;  // items the vacpack can hold, sorted by name
};

enum class Op {
    SetNewbucks,     // value = new amount
    AddNewbucks,     // value = signed delta
    RefillHealth,
    RefillEnergy,
    ClearRads,
    InfiniteEnergy,  // value = 0 / 1
    SetSlotCount,    // slot, value = new count (0 empties the slot)
    PutItem,         // slot, item, value = count (replaces what the slot holds)
};

struct Action {
    Op op = Op::RefillHealth;
    int slot = -1;
    int64_t value = 0;
    uintptr_t item = 0;
};

struct Status {
    std::string text;
    bool error = false;
    unsigned long long tick = 0;  // GetTickCount64; 0 = none yet
};

// ---- module side (Unity main thread) ----
void Publish(Snapshot s);
void SetStatus(std::string text, bool error);
// Handler for posted actions; nullptr detaches (module off). Any thread.
void SetHandler(void (*fn)(const Action&));
// The tab was drawn within the last ~0.6 s.
bool WantsSnapshot();

// ---- render thread ----
std::shared_ptr<const Snapshot> Get();  // nullptr until the first publish
Status GetStatus();
void MarkViewed();  // call every frame the tab is drawn
// Posts the action to the main thread. False (and a status line) if the
// main-thread dispatcher is not running.
bool Request(const Action& a);

}  // namespace sr2::ui::sandbox
