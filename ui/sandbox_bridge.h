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

// What kind of thing an item is (from its IdentifiableType class and flags);
// the tab uses it for the tile accent and the spawner's filter.
enum class Kind : uint8_t { Other, Slime, Largo, Plort, Food, Animal, Toy, Resource, Liquid, Gadget };

struct Item {
    uintptr_t id = 0;       // opaque (see above)
    std::string name;       // localized display name
    uint32_t slotMask = 0;  // bit i: vacpack slot i accepts it (AmmoSlotDefinition.IsAllowed); 0 in the spawn list
    uint32_t icon = 0;      // ui/item_icons.h id (0 = none)
    Kind kind = Kind::Other;
    bool tarr = false;      // spawn list: in GameModel.tarrTypeGroup (grayed out while Tarrs are off)
};

struct Slot {
    bool unlocked = false;
    uintptr_t item = 0;     // 0 = empty
    std::string name;       // item name ("" when empty)
    uint32_t icon = 0;      // the item's icon (0 = none)
    Kind kind = Kind::Other;
    int count = 0;
    int max = 0;            // AmmoSlotManager.GetSlotMaxCount
};

// A fabricator gadget recipe (a GadgetDefinition with a crafting cost).
struct Recipe {
    uintptr_t id = 0;       // opaque, like Item::id
    std::string name;
    uint32_t icon = 0;
    int category = -1;      // index into Snapshot::recipeCategories (-1: none)
    std::string cost;       // what the fabricator would charge, e.g. "20 x Pink Plort, 150 Newbucks"
    bool unlocked = false;  // GadgetDirector.HasBlueprint
    int owned = 0;          // GadgetDirector.GetItemCount (in the gadget inventory)
    int perCraft = 1;       // 2 for gadgets the fabricator makes in pairs (GadgetDefinition.BuyInPairs)
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
    std::shared_ptr<const std::vector<Item>> spawnables;  // items the spawner can place, sorted by name
    std::string spawnWhy;                                 // spawner unavailable: why ("" = available)
    bool tarrsOff = false;                                // the save's game settings have Tarrs disabled
    std::shared_ptr<const std::vector<Recipe>> recipes;   // fabricator gadget recipes, sorted by name
    std::shared_ptr<const std::vector<std::string>> recipeCategories;  // the fabricator's category titles
    std::string craftWhy;                                 // crafter unavailable: why ("" = available)
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
    Spawn,           // item (from spawnables), value = how many; placed in front of the player
    Craft,           // item (a recipe id), value = how many crafts; added to the gadget inventory at no cost
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
