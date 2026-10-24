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
#include <unordered_map>
#include <vector>

namespace sr2::ui::sandbox {

inline constexpr const char* kModule = "sandbox";

// What kind of thing an item is (from its IdentifiableType class and flags);
// the tab uses it for the tile accent, the group tabs and the tooltip tag.
enum class Kind : uint8_t { Other, Slime, Largo, Plort, Food, Animal, Toy, Resource, Liquid, Gadget, Container };

// The icon-only group tabs of the vacpack and spawner grids (index into Snapshot::groupIcons).
// Largos and containers only appear in the spawner (the vacpack can't hold them).
enum Group : int {
    kGroupAll = 0,
    kGroupPlorts,
    kGroupSlimes,
    kGroupFood,
    kGroupResources,
    kGroupLargos,
    kGroupContainers,
    kGroupCount
};

struct Item {
    uintptr_t id = 0;       // opaque (see above)
    std::string name;       // localized display name
    uint32_t slotMask = 0;  // bit i: vacpack slot i accepts it (AmmoSlotDefinition.IsAllowed); 0 in the spawn list
    uint32_t icon = 0;      // ui/item_icons.h id (0 = none)
    uint32_t icon2 = 0;     // second half of a composed icon (a largo without its own: its two base slimes); 0 = none
    Kind kind = Kind::Other;
    bool tarr = false;      // spawn list: in GameModel.tarrTypeGroup (grayed out while Tarrs are off)
    std::string family;     // largo: the base slime it is listed under (the alphabetically first of its two)
    std::string family2;    // largo: its other base slime (sort key within the family)
};

// How items relate through slime diets (SlimeDefinition.Diet), by item id. Ids
// may name types that are in neither list; the tab skips those.
struct Relation {
    std::vector<uintptr_t> favourites;   // slime: its favourite foods (SlimeDiet.FavoriteIdents)
    std::vector<uintptr_t> plorts;       // slime: what it produces (SlimeDiet.ProduceIdents)
    std::vector<uintptr_t> favouriteOf;  // food: slimes that have it as a favourite (base slimes first)
    std::vector<uintptr_t> plortOf;      // plort: slimes that produce it (base slimes first)
};
using Relations = std::unordered_map<uintptr_t, Relation>;

struct Slot {
    bool unlocked = false;
    uintptr_t item = 0;     // 0 = empty
    std::string name;       // item name ("" when empty)
    uint32_t icon = 0;      // the item's icon (0 = none)
    uint32_t icon2 = 0;     // see Item::icon2
    Kind kind = Kind::Other;
    int count = 0;
    int max = 0;            // AmmoSlotManager.GetSlotMaxCount
    bool unlockable = false;  // locked, and a vacpack upgrade unlocks it (Op::UnlockSlot)
};

// One line of a recipe's fabricator cost ("20 x Pink Plort" with its icon, or "150 Newbucks").
struct CostLine {
    std::string text;
    uint32_t icon = 0;       // ui/item_icons.h id (0: none; currencies have none)
    Kind kind = Kind::Other;
    bool currency = false;
};

// A fabricator gadget recipe (a GadgetDefinition with a crafting cost), or a
// crafting material the refinery stores (Snapshot::materials).
struct Recipe {
    uintptr_t id = 0;       // opaque, like Item::id
    std::string name;
    uint32_t icon = 0;
    int category = -1;      // index into Snapshot::recipeCategories (-1: none)
    std::string cost;       // what the fabricator would charge, e.g. "20 x Pink Plort, 150 Newbucks"
    std::vector<CostLine> costs;  // the same, one entry per ident / currency cost
    bool unlocked = false;  // GadgetDirector.HasBlueprint (materials: always true)
    int owned = 0;          // GadgetDirector.GetItemCount (gadget inventory; the refinery for materials)
    int perCraft = 1;       // 2 for gadgets the fabricator makes in pairs (GadgetDefinition.BuyInPairs)
    Kind kind = Kind::Gadget;  // materials: their item kind
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
    std::shared_ptr<const std::vector<uint32_t>> recipeCategoryIcons;  // their icons (FabricateCategory._icon), same order
    std::shared_ptr<const std::vector<Recipe>> materials; // crafting materials the refinery stores, sorted by name
    std::string craftWhy;                                 // crafter unavailable: why ("" = available)
    std::shared_ptr<const Relations> relations;           // slime diets (favourites, plorts); nullptr until built
    uint32_t groupIcons[kGroupCount] = {};                // the game's icons for the group tabs (0 = none; All has none)
    int upgradeCount = 0;                                 // player upgrades (UpgradeModel.upgradeDefinitions)
    int upgradeLevelsLeft = -1;                           // levels until all are at their last one (-1: unknown)
    std::string upgradeWhy;                               // why "Unlock all upgrades" is unavailable ("" = it is not)
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
    MoveSlot,        // slot = from, value = to: moves the contents (merges into the same item, else swaps)
    UnlockSlot,      // slot: applies the vacpack upgrade levels that unlock it
    AddMaterial,     // item (from materials), value = how many; added to the refinery
    StowItem,        // slot = selected slot, item (from catalog), value = amount (<= 0: one slot's maximum), replace:
                     // fills the selected slot (replacing its contents only if `replace`), then the slots that hold
                     // the item or are empty; what does not fit is dropped in front of the player
    UnlockUpgrades,  // every player upgrade to its last level (UpgradeModel.IncrementUpgradeLevel), free
};

struct Action {
    Op op = Op::RefillHealth;
    int slot = -1;
    int64_t value = 0;
    uintptr_t item = 0;
    bool replace = false;  // StowItem: may empty the selected slot if it holds something else
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
// The loaded save is marked as cheated (Sandbox was used in it): achievements stay blocked in it,
// also with Sandbox off (modules/sandbox/cheated_save.cpp). Any thread.
void SetSaveCheated(bool cheated);
bool SaveCheated();
// The game's Fabricator is open while Sandbox is on: the overlay shows a small hint for the
// "add missing materials" key `vk`. Refreshed about once a second; stale after 3 s. Any thread.
void SetFabricatorHint(bool open, int vk);
int FabricatorHintKey();  // the key to show, 0 = no hint

// ---- render thread ----
std::shared_ptr<const Snapshot> Get();  // nullptr until the first publish
Status GetStatus();
void MarkViewed();  // call every frame the tab is drawn
// Posts the action to the main thread. False (and a status line) if the
// main-thread dispatcher is not running.
bool Request(const Action& a);

}  // namespace sr2::ui::sandbox
