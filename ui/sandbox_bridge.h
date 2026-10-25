#pragma once
// Data exchange between the sandbox module (modules/sandbox) and its overlay
// tab (menu.cpp, the Sandbox category).
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
// Largos and containers only appear in the spawner (the vacpack can't hold them). Other: whatever
// fits none of the others (toys, unsorted types); its tab is drawn (no game icon).
enum Group : int {
    kGroupAll = 0,
    kGroupPlorts,
    kGroupSlimes,
    kGroupFood,
    kGroupResources,
    kGroupLargos,
    kGroupContainers,
    kGroupOther,
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
    uintptr_t base = 0, base2 = 0;  // largo: its two base slimes (SlimeDefinition.BaseSlimes, ids as above)
    std::string area;       // crate: the zone it spawns in (ZoneDefinition._crates; its localized name)
    bool mod = false;       // added by the mod (custom_upgrades' Slime Radio): spawned through buildables::Grant; [M] badge
    // Spawn list: the radiant forms the game has for it (Op::SpawnRadiant), bits of kRadiant*. A slime: its own
    // (SlimeDefinition.RadiantBase); a largo: radiant from either base slime (RadiantLargo0 / RadiantLargo1, the
    // first / second of SlimeDefinition.BaseSlimes, ids in radiantBase / radiantBase2). 0 = none.
    uint8_t radiant = 0;
    uintptr_t radiantBase = 0, radiantBase2 = 0;
    // The radiant forms' icons (the Slimepedia's radiant icons: the radiant SlimeAppearance's _icon, which
    // RadiantSlimePediaEntry.get_Icon returns), indexed by Op::SpawnRadiant's slot: 0 the slime's own, 1 / 2 a
    // largo radiant from its first / second base (its RadiantLargo0 / 1 look, else that base's radiant icon).
    // 0 = none (the context menu falls back to the item's icon).
    uint32_t radiantIcon[3] = {};
};
inline constexpr uint8_t kRadiantSelf = 1, kRadiantBase0 = 2, kRadiantBase1 = 4;

// How items relate through slime diets (SlimeDefinition.Diet), by item id. Ids
// may name types that are in neither list; the tab skips those.
struct Relation {
    std::vector<uintptr_t> favourites;   // slime: its favourite foods (SlimeDiet.FavoriteIdents)
    std::vector<uintptr_t> plorts;       // slime: what it produces (SlimeDiet.ProduceIdents)
    std::vector<uintptr_t> favouriteOf;  // food: slimes that have it as a favourite (base slimes first)
    std::vector<uintptr_t> plortOf;      // plort: slimes that produce it (base slimes first)
    std::vector<uintptr_t> largos;       // slime: the largos it is a base slime of (Item::base / base2)
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
    uint32_t icon = 0;       // ui/item_icons.h id (0: none; Newbucks: the game's iconNewBuck once found)
    Kind kind = Kind::Other;
    bool currency = false;
};

// A variant of a gadget recipe, for the crafter's right-click menu: one of the game's variants
// (GadgetDefinition.VariantOf is the recipe's gadget; crafted like a recipe, Op::Craft with its id) or one of
// custom_upgrades' buildables on that base gadget (`mod`: Op::Craft with its id goes to buildables::Grant,
// the vanilla base gadget plus a kit in the mod's sidecar; the clone never enters the save).
struct Variant {
    uintptr_t id = 0;       // opaque, like Recipe::id
    std::string name;
    uint32_t icon = 0;
    bool unlocked = false;  // its blueprint (vanilla: GadgetDirector.HasBlueprint; mod: its Fabricator card)
    bool mod = false;       // a mod buildable (custom_upgrades)
};

// A fabricator gadget recipe (a GadgetDefinition with a crafting cost), or a
// crafting material the refinery stores (Snapshot::materials).
struct Recipe {
    uintptr_t id = 0;       // opaque, like Item::id
    std::string name;
    uint32_t icon = 0;
    int category = -1;      // index into Snapshot::recipeCategories (-1: none)
    int order = -1;         // position in its category as the game's Fabricator lists it (-1: unknown, after the others)
    std::string cost;       // what the fabricator would charge, e.g. "20 x Pink Plort, 150 Newbucks"
    std::vector<CostLine> costs;  // the same, one entry per ident / currency cost
    bool unlocked = false;  // GadgetDirector.HasBlueprint (materials: always true)
    int owned = 0;          // GadgetDirector.GetItemCount (gadget inventory; the refinery for materials)
    int perCraft = 1;       // 2 for gadgets the fabricator makes in pairs (GadgetDefinition.BuyInPairs)
    Kind kind = Kind::Gadget;  // materials: their item kind; the mod's Slime Radio: Toy
    std::vector<Variant> variants;  // the gadget's variants (game's first, then the mod's), for the right-click menu
    // A mod buildable's own tile (custom_upgrades: Quantum Storage Link, DJ Booth, DJ Speaker, Slime Radio): in its
    // base gadget's category right after it (`order` = the base's, `modRank` 1.. in the Fabricator's order), unlocked =
    // its Fabricator card, owned = its kits (radio: radios in the save). Op::Craft goes to buildables::Grant; it is
    // in no blueprint tab (the Upgrades & Blueprints card has its own Mod buildables tab).
    bool mod = false;
    int modRank = 0;
};

struct Snapshot {
    unsigned long long tick = 0;  // GetTickCount64 when it was taken
    bool inGame = false;          // a save is loaded and the player exists
    std::string why;              // !inGame: what is missing
    bool haveNewbucks = false;    // the Newbucks currency was found
    int newbucks = 0, maxNewbucks = 0;
    uint32_t newbucksIcon = 0;    // ui/item_icons.h id of the game's iconNewBuck sprite (0: not found, a drawn coin)
    int health = 0, maxHealth = 0;
    int energy = 0, maxEnergy = 0;
    int rads = 0;
    bool infiniteEnergy = false;  // toggle, kept per save (modules/sandbox/cheated_save.h, SaveToggle)
    std::vector<Slot> slots;
    std::shared_ptr<const std::vector<Item>> catalog;  // items the vacpack can hold, sorted by name
    std::shared_ptr<const std::vector<Item>> spawnables;  // items the spawner can place, sorted by name
    std::string spawnWhy;                                 // spawner unavailable: why ("" = available)
    bool tarrsOff = false;                                // the save's game settings have Tarrs disabled
    std::shared_ptr<const std::vector<Recipe>> recipes;   // fabricator gadget recipes, sorted by name
    std::shared_ptr<const std::vector<std::string>> recipeCategories;  // the fabricator's category titles
    std::shared_ptr<const std::vector<uint32_t>> recipeCategoryIcons;  // their icons (FabricateCategory._icon), same order
    std::shared_ptr<const std::vector<Recipe>> materials; // crafting materials the refinery stores, sorted by name
    uintptr_t pinnedRecipe = 0;                           // the recipe pinned in the game's Fabricator (a Recipe::id; 0: none)
    uint32_t pinnedIcon = 0;                              // the Fabricator's "Pinned" category icon (0: not loaded yet)
    std::string craftWhy;                                 // crafter unavailable: why ("" = available)
    std::shared_ptr<const Relations> relations;           // slime diets (favourites, plorts); nullptr until built
    uint32_t groupIcons[kGroupCount] = {};                // the game's icons for the group tabs (0 = none; All has none)
    int upgradeCount = 0;                                 // player upgrades (UpgradeModel.upgradeDefinitions)
    int upgradeLevelsLeft = -1;                           // levels until all are at their last one (-1: unknown)
    std::string upgradeWhy;                               // why "Max all upgrades" is unavailable ("" = it is not)
    int upgradesHidden = -1;                              // upgrades the Fabricator hides until found (-1: unknown)
    unsigned view = 0;                                    // ViewSession() when it was read (Current)
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
    Spawn,           // item (from spawnables), value = how many (at most 60; the mod's Slime Radio at most 5); placed in front of the player
    Craft,           // item (a recipe id), value = how many crafts; added to the gadget inventory at no cost
    MoveSlot,        // slot = from, value = to: moves the contents (merges into the same item, else swaps)
    UnlockSlot,      // slot: applies the vacpack upgrade levels that unlock it
    AddMaterial,     // item (from materials), value = how many; added to the refinery
    StowItem,        // slot = selected slot, item (from catalog), value = amount (<= 0: one slot's maximum), replace:
                     // fills the selected slot (replacing its contents only if `replace`), then the slots that hold
                     // the item or are empty; what does not fit is dropped in front of the player
    UnlockUpgrades,  // "Max all upgrades": every player upgrade to its last level (IncrementUpgradeLevel), free
    DiscoverUpgrades,  // every upgrade the Fabricator hides until found gets its upgrade component (as if found)
    SpawnRadiant,    // item (a slime or largo from spawnables), value = how many, slot = which radiant form: 0 the slime's
                     // own, 1 / 2 a largo radiant from its first / second base slime (Item::radiant); in front of the player
};

struct Action {
    Op op = Op::RefillHealth;
    int slot = -1;
    int64_t value = 0;
    uintptr_t item = 0;
    bool replace = false;  // StowItem: may empty the selected slot if it holds something else
    // Part of a multi-item request (Craft N, Spawn N, Put in slots): 1-based index and the request's size.
    // Spawns share one grid in front of the player; the items' HUD notices are collected and shown
    // after the last one, one per item type, in sequence. 0 / 0: a single request.
    int batch = 0, batchSize = 0;
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
// Counts the times the tab was shown again after a gap (overlay reopened, switched back to it). The
// module stamps it into Snapshot::view and refreshes at once when it changes, not on its next 250 ms
// tick. Any thread.
unsigned ViewSession();
// The loaded save is marked as cheated (Sandbox was used in it): achievements stay blocked in it,
// also with Sandbox off (modules/sandbox/cheated_save.cpp). Any thread.
void SetSaveCheated(bool cheated);
bool SaveCheated();

// ---- render thread ----
std::shared_ptr<const Snapshot> Get();  // nullptr until the first publish
Status GetStatus();
void MarkViewed();  // call every frame the tab is drawn (before Get)
// The snapshot was read since the tab was shown again, or the module has not answered within
// ~0.4 s (then the old one is all there is). False on the first frame(s) after reopening: the
// snapshot is from before the overlay closed (round 17: emptied slots showed full for a frame).
bool Current(const Snapshot& s);
// Posts the action to the main thread. False (and a status line) if the
// main-thread dispatcher is not running.
bool Request(const Action& a);

}  // namespace sr2::ui::sandbox
